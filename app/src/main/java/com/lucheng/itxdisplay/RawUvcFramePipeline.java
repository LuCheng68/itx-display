package com.lucheng.itxdisplay;

import com.mikimn.droiduvc.RawFrameListener;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Process;

import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Low-latency raw UVC payload tap.
 *
 * The native callback only copies the borrowed payload into a three-buffer pool and enqueues it.
 * The worker parses the JPEG header (when the payload is MJPEG) and updates diagnostics. The
 * display path remains the existing native Surface path; this queue is deliberately bounded and
 * drops the oldest queued frame rather than accumulating latency.
 */
public final class RawUvcFramePipeline implements RawFrameListener, AutoCloseable {
    private static final int POOL_SIZE = 3;
    private static final int QUEUE_CAPACITY = 2;
    private static final int MAX_FRAME_BYTES = 32 * 1024 * 1024;

    private static final class FrameSlot {
        byte[] bytes = new byte[0];
        int size;
        int width;
        int height;
        int formatCode;
        long sequence;
        long captureTimeNs;
        long callbackTimeNs;
    }

    private final ArrayBlockingQueue<FrameSlot> freeSlots = new ArrayBlockingQueue<>(POOL_SIZE);
    private final ArrayBlockingQueue<FrameSlot> readySlots = new ArrayBlockingQueue<>(QUEUE_CAPACITY);
    private final AtomicBoolean running = new AtomicBoolean(true);
    private final AtomicBoolean grayProbeRequested = new AtomicBoolean(false);
    private final AtomicLong callbackCount = new AtomicLong();
    private final AtomicLong droppedCount = new AtomicLong();
    private final AtomicLong analyzedCount = new AtomicLong();
    private final AtomicLong sequenceGapCount = new AtomicLong();
    private final Thread worker;

    private volatile String jpegDescription = "等待 UVC 原始帧";
    private volatile int latestFrameBytes;
    private volatile double averageFrameBytes;
    private volatile double callbackFps;
    private volatile double analysisFps;
    private volatile double averageCallbackIntervalMs;
    private volatile double maxCallbackIntervalMs;
    private volatile double averageCaptureIntervalMs;
    private volatile double maxCaptureIntervalMs;
    private volatile long captureTimestampSamples;
    private final AtomicLong callbackIntervalsOver30Ms = new AtomicLong();
    private final AtomicLong callbackIntervalsOver40Ms = new AtomicLong();
    private final AtomicLong callbackIntervalsOver60Ms = new AtomicLong();
    private final AtomicLong captureIntervalsOver30Ms = new AtomicLong();
    private final AtomicLong captureIntervalsOver40Ms = new AtomicLong();
    private final AtomicLong captureIntervalsOver60Ms = new AtomicLong();
    private volatile double averageQueueWaitMs;
    private volatile double maxQueueWaitMs;
    private long previousCallbackNs;
    private long previousCaptureTimeNs;
    private volatile long latestSequence = -1;
    private volatile int latestFormatCode = -1;
    private volatile int callbackTid = -1;
    private volatile int analyzerTid = -1;
    private volatile String grayProbeResult = "未执行";

    public RawUvcFramePipeline() {
        for (int i = 0; i < POOL_SIZE; i++) {
            freeSlots.offer(new FrameSlot());
        }
        worker = new Thread(this::runWorker, "ITX-RawUVC-Analyzer");
        worker.setPriority(Thread.NORM_PRIORITY - 1);
        worker.start();
    }

    /**
     * Called on the native UVC streaming thread. The direct ByteBuffer is borrowed and becomes
     * invalid after this method returns, so copy immediately and do no JPEG decoding here.
     */
    @Override
    public void onFrame(ByteBuffer data,
                        int width,
                        int height,
                        int formatCode,
                        long sequence,
                        long captureTimeNs) {
        if (!running.get() || data == null) return;
        callbackTid = Process.myTid();
        callbackCount.incrementAndGet();
        final long callbackNowNs = System.nanoTime();
        if (previousCallbackNs != 0 && callbackNowNs > previousCallbackNs) {
            long intervalNs = callbackNowNs - previousCallbackNs;
            double intervalMs = intervalNs / 1_000_000.0;
            double instantFps = 1_000_000_000.0 / intervalNs;
            callbackFps = callbackFps == 0 ? instantFps : callbackFps * 0.8 + instantFps * 0.2;
            averageCallbackIntervalMs = averageCallbackIntervalMs == 0
                    ? intervalMs : averageCallbackIntervalMs * 0.8 + intervalMs * 0.2;
            maxCallbackIntervalMs = Math.max(maxCallbackIntervalMs, intervalMs);
            if (intervalMs > 30.0) callbackIntervalsOver30Ms.incrementAndGet();
            if (intervalMs > 40.0) callbackIntervalsOver40Ms.incrementAndGet();
            if (intervalMs > 60.0) callbackIntervalsOver60Ms.incrementAndGet();
        }
        previousCallbackNs = callbackNowNs;
        // libuvc's capture_time_finished is a host-side frame-completion timestamp,
        // not a hardware camera clock. Only use positive, strictly increasing samples.
        if (captureTimeNs > 0) {
            if (previousCaptureTimeNs > 0 && captureTimeNs > previousCaptureTimeNs) {
                double intervalMs = (captureTimeNs - previousCaptureTimeNs) / 1_000_000.0;
                averageCaptureIntervalMs = averageCaptureIntervalMs == 0
                        ? intervalMs : averageCaptureIntervalMs * 0.8 + intervalMs * 0.2;
                maxCaptureIntervalMs = Math.max(maxCaptureIntervalMs, intervalMs);
                if (intervalMs > 30.0) captureIntervalsOver30Ms.incrementAndGet();
                if (intervalMs > 40.0) captureIntervalsOver40Ms.incrementAndGet();
                if (intervalMs > 60.0) captureIntervalsOver60Ms.incrementAndGet();
                captureTimestampSamples++;
            }
            previousCaptureTimeNs = captureTimeNs;
        }
        final int size = data.remaining();
        if (size <= 0 || size > MAX_FRAME_BYTES) {
            droppedCount.incrementAndGet();
            return;
        }

        // If the analysis queue is full, evict its oldest frame before enqueueing
        // the incoming one. This ensures the pipeline is latest-frame-first rather than
        // accidentally dropping the current frame while preserving stale queued frames.
        FrameSlot slot = freeSlots.poll();
        if (slot == null) {
            slot = readySlots.poll();
            if (slot == null) {
                droppedCount.incrementAndGet();
                return;
            }
            droppedCount.incrementAndGet();
        } else if (readySlots.remainingCapacity() == 0) {
            FrameSlot stale = readySlots.poll();
            if (stale != null) {
                droppedCount.incrementAndGet();
                freeSlots.offer(stale);
            }
        }

        try {
            if (slot.bytes.length < size) {
                slot.bytes = new byte[nextCapacity(size)];
            }
            ByteBuffer source = data.duplicate();
            source.get(slot.bytes, 0, size);
            slot.size = size;
            slot.width = width;
            slot.height = height;
            slot.formatCode = formatCode;
            slot.sequence = sequence;
            slot.captureTimeNs = captureTimeNs;
            slot.callbackTimeNs = callbackNowNs;
            latestFormatCode = formatCode;

            if (!readySlots.offer(slot)) {
                droppedCount.incrementAndGet();
                freeSlots.offer(slot);
            }
        } catch (RuntimeException e) {
            droppedCount.incrementAndGet();
            freeSlots.offer(slot);
        }
    }

    private static int nextCapacity(int required) {
        int capacity = 1024;
        while (capacity < required && capacity < MAX_FRAME_BYTES) {
            capacity <<= 1;
        }
        return Math.max(required, Math.min(capacity, MAX_FRAME_BYTES));
    }

    public boolean requestGrayPatchProbe() {
        if (!running.get()) return false;
        grayProbeResult = "已请求，等待下一帧 MJPEG";
        return grayProbeRequested.compareAndSet(false, true);
    }

    private void runWorker() {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        } catch (RuntimeException ignored) {
            // Keep the analyzer alive with the inherited scheduler priority if denied.
        }
        analyzerTid = Process.myTid();
        long windowStartNs = System.nanoTime();
        long previousArrivalNs = 0;
        long previousSequence = -1;
        long windowFrames = 0;
        try {
            while (running.get() || !readySlots.isEmpty()) {
                FrameSlot slot = readySlots.poll(250, TimeUnit.MILLISECONDS);
                if (slot == null) continue;

                try {
                    long queueWaitNs = Math.max(0L, System.nanoTime() - slot.callbackTimeNs);
                    double queueWaitMs = queueWaitNs / 1_000_000.0;
                    averageQueueWaitMs = averageQueueWaitMs == 0
                            ? queueWaitMs : averageQueueWaitMs * 0.8 + queueWaitMs * 0.2;
                    maxQueueWaitMs = Math.max(maxQueueWaitMs, queueWaitMs);
                    analyzedCount.incrementAndGet();
                    latestFrameBytes = slot.size;
                    if (averageFrameBytes == 0) averageFrameBytes = slot.size;
                    else averageFrameBytes = averageFrameBytes * 0.8 + slot.size * 0.2;

                    if (previousSequence >= 0 && slot.sequence > previousSequence + 1) {
                        sequenceGapCount.addAndGet(slot.sequence - previousSequence - 1);
                    }
                    previousSequence = slot.sequence;
                    latestSequence = slot.sequence;

                    jpegDescription = parseJpegHeader(slot.bytes, slot.size);
                    if (grayProbeRequested.compareAndSet(true, false)) {
                        grayProbeResult = analyzeGrayPatches(slot.bytes, slot.size);
                    }
                    if (previousArrivalNs != 0 && slot.callbackTimeNs > previousArrivalNs) {
                        long interval = slot.callbackTimeNs - previousArrivalNs;
                        double instantFps = 1_000_000_000.0 / interval;
                        analysisFps = analysisFps == 0 ? instantFps : analysisFps * 0.8 + instantFps * 0.2;
                    }
                    previousArrivalNs = slot.callbackTimeNs;

                    windowFrames++;
                    long now = System.nanoTime();
                    long elapsed = now - windowStartNs;
                    if (elapsed >= 1_000_000_000L) {
                        if (windowFrames > 0) {
                            analysisFps = windowFrames * 1_000_000_000.0 / elapsed;
                        }
                        windowStartNs = now;
                        windowFrames = 0;
                    }
                } finally {
                    slot.size = 0;
                    freeSlots.offer(slot);
                }
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    public String getSummary() {
        return String.format(Locale.US,
                "UVC回调 %.1f FPS / 分析 %.1f FPS · 最近 %.1f KB · %s · 丢帧 %d",
                callbackFps,
                analysisFps,
                latestFrameBytes / 1024.0,
                jpegDescription,
                droppedCount.get());
    }

    public String getDetailedReport() {
        return getSummary()
                + "\n回调帧数：" + callbackCount.get()
                + "\n分析帧数：" + analyzedCount.get()
                + "\n序号缺口估计：" + sequenceGapCount.get()
                + "\n最近序号：" + latestSequence
                + "\nUVC format code：" + latestFormatCode
                + "\n平均压缩帧大小：" + String.format(Locale.US, "%.1f KB", averageFrameBytes / 1024.0)
                + "\nUVC 回调线程 TID：" + callbackTid
                + "\n分析线程 TID：" + analyzerTid
                + "\n回调帧间隔：" + String.format(Locale.US, "均值 %.2f ms / 最大 %.2f ms", averageCallbackIntervalMs, maxCallbackIntervalMs)
                + String.format(Locale.US, "；超过 30/40/60 ms：%d/%d/%d 次",
                        callbackIntervalsOver30Ms.get(), callbackIntervalsOver40Ms.get(), callbackIntervalsOver60Ms.get())
                + "\nlibuvc 帧完成时间戳：" + (captureTimestampSamples > 0
                        ? String.format(Locale.US, "有效间隔样本 %d / 平均 %.2f ms / 最大 %.2f ms",
                                captureTimestampSamples, averageCaptureIntervalMs, maxCaptureIntervalMs)
                        : "未提供可用的递增时间戳")
                + (captureTimestampSamples > 0
                        ? String.format(Locale.US, "；超过 30/40/60 ms：%d/%d/%d 次",
                                captureIntervalsOver30Ms.get(), captureIntervalsOver40Ms.get(), captureIntervalsOver60Ms.get())
                        : "")
                + "\n说明：libuvc 帧完成时间戳属于主机侧时间，不是摄像头硬件时钟。"
                + "\n回调到分析开始等待：" + String.format(Locale.US, "均值 %.2f ms / 最大 %.2f ms", averageQueueWaitMs, maxQueueWaitMs)
                + "\n灰阶探测：" + grayProbeResult;
    }

    /**
     * Decodes one requested JPEG frame only, at half resolution, and samples the centers
     * of the ten equal-width neutral patches in itx_display_rgb_grayscale_test.png.
     * This diagnostic is user-triggered and is not part of the 50 FPS hot path.
     */
    private static String analyzeGrayPatches(byte[] data, int size) {
        Bitmap bitmap = null;
        try {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = 2;
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            bitmap = BitmapFactory.decodeByteArray(data, 0, size, options);
            if (bitmap == null || bitmap.getWidth() < 100 || bitmap.getHeight() < 100) {
                return "灰阶探测失败：当前负载无法解码为 JPEG";
            }

            final int[] expected = {16, 32, 64, 96, 128, 160, 192, 220, 240, 255};
            final int width = bitmap.getWidth();
            final int height = bitmap.getHeight();
            final int yCenter = height / 2;
            final int xRadius = Math.max(2, width / 120);
            final int yRadius = Math.max(2, height / 80);
            StringBuilder result = new StringBuilder("期望→实测灰阶（每个样本为中心小区域 RGB 均值）：");

            int[] measured = new int[expected.length];
            for (int patch = 0; patch < expected.length; patch++) {
                int xCenter = (int) ((patch + 0.5f) * width / expected.length);
                long sum = 0;
                long count = 0;
                for (int y = Math.max(0, yCenter - yRadius);
                     y < Math.min(height, yCenter + yRadius + 1); y++) {
                    for (int x = Math.max(0, xCenter - xRadius);
                         x < Math.min(width, xCenter + xRadius + 1); x++) {
                        int pixel = bitmap.getPixel(x, y);
                        int red = (pixel >> 16) & 0xFF;
                        int green = (pixel >> 8) & 0xFF;
                        int blue = pixel & 0xFF;
                        sum += (red + green + blue) / 3;
                        count++;
                    }
                }
                measured[patch] = count == 0 ? -1 : (int) Math.round((double) sum / count);
                if (patch > 0) result.append("；");
                result.append(expected[patch]).append("→").append(measured[patch]);
            }

            int delta220To240 = measured[8] - measured[7];
            int delta240To255 = measured[9] - measured[8];
            if (measured[8] >= 250 && measured[9] >= 250 && delta240To255 <= 2) {
                result.append("；检测到高光端平台/疑似剪切：240 与 255 两级已无法区分（Δ=")
                        .append(delta240To255).append("）");
            } else if (delta240To255 < 7) {
                result.append("；高光端明显压缩：240→255 的实测差仅 ")
                        .append(delta240To255);
            } else if (delta220To240 < 8) {
                result.append("；高光端渐进压缩：220→240 的实测差仅 ")
                        .append(delta220To240);
            } else {
                result.append("；220/240/255 高光阶梯仍可区分；需与 YCbCr 基线比较");
            }
            return result.toString();
        } catch (RuntimeException e) {
            return "灰阶探测异常：" + e.getClass().getSimpleName();
        } finally {
            if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
        }
    }

    private static String parseJpegHeader(byte[] data, int size) {
        if (size < 4 || (data[0] & 0xFF) != 0xFF || (data[1] & 0xFF) != 0xD8) {
            return "非 JPEG 负载";
        }

        boolean jfif = false;
        boolean adobe = false;
        int position = 2;

        while (position + 4 <= size) {
            while (position < size && (data[position] & 0xFF) != 0xFF) position++;
            if (position >= size) break;
            while (position < size && (data[position] & 0xFF) == 0xFF) position++;
            if (position >= size) break;

            int marker = data[position++] & 0xFF;
            if (marker == 0xD9 || marker == 0xDA) break; // EOI or start of compressed scan
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
            if (position + 2 > size) break;

            int segmentLength = u16(data, position);
            if (segmentLength < 2 || position + segmentLength > size) break;
            int payload = position + 2;
            int payloadLength = segmentLength - 2;

            if (marker == 0xE0 && payloadLength >= 5 && ascii(data, payload, "JFIF\0")) {
                jfif = true;
            } else if (marker == 0xEE && payloadLength >= 5 && ascii(data, payload, "Adobe")) {
                adobe = true;
            }

            if (isStartOfFrame(marker) && payloadLength >= 6) {
                int componentCount = data[payload + 5] & 0xFF;
                if (componentCount >= 3 && payloadLength >= 6 + componentCount * 3) {
                    int ySampling = data[payload + 7] & 0xFF;
                    int cbSampling = data[payload + 10] & 0xFF;
                    int crSampling = data[payload + 13] & 0xFF;
                    int yH = (ySampling >> 4) & 0x0F;
                    int yV = ySampling & 0x0F;
                    int cbH = (cbSampling >> 4) & 0x0F;
                    int cbV = cbSampling & 0x0F;
                    int crH = (crSampling >> 4) & 0x0F;
                    int crV = crSampling & 0x0F;

                    String sampling;
                    if (yH == 2 && yV == 2 && cbH == 1 && cbV == 1 && crH == 1 && crV == 1) {
                        sampling = "JPEG 4:2:0";
                    } else if (yH == 2 && yV == 1 && cbH == 1 && cbV == 1 && crH == 1 && crV == 1) {
                        sampling = "JPEG 4:2:2";
                    } else if (yH == 1 && yV == 1 && cbH == 1 && cbV == 1 && crH == 1 && crV == 1) {
                        sampling = "JPEG 4:4:4";
                    } else {
                        sampling = "JPEG 色度采样 Y=" + yH + "x" + yV
                                + " Cb=" + cbH + "x" + cbV
                                + " Cr=" + crH + "x" + crV;
                    }
                    return sampling + (jfif ? " / JFIF" : "") + (adobe ? " / Adobe" : "");
                }
            }
            position += segmentLength;
        }
        return "MJPEG 头已收到，采样因子未解析" + (jfif ? " / JFIF" : "") + (adobe ? " / Adobe" : "");
    }

    private static boolean isStartOfFrame(int marker) {
        return marker >= 0xC0 && marker <= 0xCF
                && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    }

    private static int u16(byte[] data, int offset) {
        return ((data[offset] & 0xFF) << 8) | (data[offset + 1] & 0xFF);
    }

    private static boolean ascii(byte[] data, int offset, String value) {
        if (offset + value.length() > data.length) return false;
        for (int i = 0; i < value.length(); i++) {
            if ((data[offset + i] & 0xFF) != (value.charAt(i) & 0xFF)) return false;
        }
        return true;
    }

    @Override
    public void close() {
        if (running.compareAndSet(true, false)) {
            worker.interrupt();
        }
    }
}
