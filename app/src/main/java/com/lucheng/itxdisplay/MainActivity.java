package com.lucheng.itxdisplay;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.SurfaceTexture;
import android.graphics.Matrix;
import android.graphics.RectF;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.mikimn.droiduvc.StreamFormat;
import com.mikimn.droiduvc.StreamMode;
import com.mikimn.droiduvc.UvcCamera;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends Activity {
    private static final String ACTION_USB_PERMISSION =
            "com.lucheng.itxdisplay.USB_PERMISSION";
    private static final int TARGET_WIDTH = 1920;
    private static final int TARGET_HEIGHT = 1080;
    private static final int TARGET_FPS = 50;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ExecutorService cameraExecutor = Executors.newSingleThreadExecutor();

    private UsbManager usbManager;
    private TextureView previewView;
    private TextView statusView;
    private TextView deviceView;
    private TextView modeView;
    private TextView statsView;
    private int videoWidth = TARGET_WIDTH;
    private int videoHeight = TARGET_HEIGHT;
    private long fpsWindowStart = 0L;
    private int renderedFrames = 0;
    private float displayedFps = 0f;
    private boolean receiverRegistered = false;
    private boolean opening = false;
    private volatile boolean activityResumed = false;
    private volatile int sessionGeneration = 0;
    private UsbDevice selectedDevice;

    // These three session resources are only read or changed on cameraExecutor.
    private UvcCamera camera;
    private UsbDeviceConnection usbConnection;
    private Surface streamSurface;

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (ACTION_USB_PERMISSION.equals(action)) {
                UsbDevice device = getUsbDevice(intent);
                boolean granted = intent.getBooleanExtra(
                        UsbManager.EXTRA_PERMISSION_GRANTED, false);
                if (granted && device != null) {
                    selectedDevice = device;
                    showStatus("USB 已授权，正在准备 UVC 预览…");
                    openCameraIfReady();
                } else {
                    showStatus("USB 授权未通过。请重新扫描并允许访问采集卡。");
                }
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)
                    || UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                scanDevices();
            }
        }
    };

    @SuppressWarnings("deprecation")
    private UsbDevice getUsbDevice(Intent intent) {
        if (Build.VERSION.SDK_INT >= 33) {
            return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        }
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyImmersiveMode();

        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        buildUi();
        registerUsbReceiver();

        if (Build.VERSION.SDK_INT >= 23
                && checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, 100);
        }
    }

    private void applyImmersiveMode() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    private int dp(float value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private void buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);

        previewView = new TextureView(this);
        root.addView(previewView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        previewView.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            @Override
            public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
                applyAspectRatio();
                openCameraIfReady();
            }

            @Override
            public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {
                applyAspectRatio();
            }

            @Override
            public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
                invalidateAndCloseSession();
                return true;
            }

            @Override
            public void onSurfaceTextureUpdated(SurfaceTexture surface) {
                updateDisplayFps();
            }
        });

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(10), dp(14), dp(10));
        panel.setBackgroundColor(0xB5101318);

        TextView title = new TextView(this);
        title.setText("ITX Display  ·  USB 视频输入");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(19);
        panel.addView(title);

        statusView = new TextView(this);
        statusView.setText("正在检查 USB 摄像设备…");
        statusView.setTextColor(0xFFD7E0ED);
        statusView.setTextSize(14);
        statusView.setPadding(0, dp(7), 0, dp(5));
        panel.addView(statusView);

        deviceView = new TextView(this);
        deviceView.setTextColor(0xFFFFFFFF);
        deviceView.setTextSize(13);
        deviceView.setPadding(0, dp(4), 0, dp(4));
        panel.addView(deviceView);

        modeView = new TextView(this);
        modeView.setTextColor(0xFF9FB2C9);
        modeView.setTextSize(12);
        modeView.setPadding(0, dp(4), 0, dp(8));
        panel.addView(modeView);

        Button scanButton = new Button(this);
        scanButton.setText("重新扫描 USB 设备");
        scanButton.setOnClickListener(v -> scanDevices());
        panel.addView(scanButton);

        statsView = new TextView(this);
        statsView.setText("显示 FPS：--");
        statsView.setTextColor(0xFFFFFFFF);
        statsView.setTextSize(13);
        statsView.setPadding(dp(10), dp(7), dp(10), dp(7));
        statsView.setBackgroundColor(0xB5101318);
        FrameLayout.LayoutParams statsParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        statsParams.setMargins(dp(14), dp(14), dp(14), dp(14));
        root.addView(statsView, statsParams);

        TextView target = new TextView(this);
        target.setText("目标：1920×1080 · 50 FPS · MJPEG 优先 · 无音频");
        target.setTextColor(0xFF9AA8BA);
        target.setTextSize(11);
        target.setPadding(0, dp(5), 0, 0);
        panel.addView(target);

        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        panelParams.setMargins(dp(14), dp(14), dp(14), dp(14));
        root.addView(panel, panelParams);

        setContentView(root);
    }

    private void registerUsbReceiver() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(usbReceiver, filter);
        }
        receiverRegistered = true;
    }

    private void scanDevices() {
        if (usbManager == null) return;

        List<UsbDevice> candidates = new ArrayList<>();
        for (UsbDevice device : usbManager.getDeviceList().values()) {
            if (hasVideoInterface(device)) candidates.add(device);
        }

        if (candidates.isEmpty()) {
            selectedDevice = null;
            invalidateAndCloseSession();
            if (deviceView != null) {
                deviceView.setText("未发现 UVC 视频设备。\n请检查 OTG 转接头、采集卡供电和 USB 连接。");
            }
            if (modeView != null) modeView.setText("");
            showStatus("等待 USB 视频设备连接");
            return;
        }

        UsbDevice device = candidates.get(0);
        if (selectedDevice != null && !selectedDevice.equals(device)) {
            invalidateAndCloseSession();
        }
        selectedDevice = device;

        if (deviceView != null) {
            deviceView.setText("设备：" + safeName(device) + "\n"
                    + "VID:PID  " + hex(device.getVendorId()) + ":" + hex(device.getProductId())
                    + "    接口数：" + device.getInterfaceCount()
                    + "\n检测到 " + candidates.size() + " 个 USB 视频设备");
        }

        if (usbManager.hasPermission(device)) {
            showStatus("USB 已授权，正在连接 UVC 视频流…");
            openCameraIfReady();
        } else {
            showStatus("发现视频设备，正在请求 USB 访问权限…");
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
            PendingIntent permissionIntent = PendingIntent.getBroadcast(
                    this, 0, new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName()),
                    flags);
            usbManager.requestPermission(device, permissionIntent);
        }
    }

    private boolean hasVideoInterface(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            if (device.getInterface(i).getInterfaceClass() == 14) return true;
        }
        return false;
    }

    private String safeName(UsbDevice device) {
        String name = device.getProductName();
        return name == null || name.trim().isEmpty() ? "USB Video Device" : name;
    }

    private String hex(int value) {
        return String.format(Locale.US, "%04X", value);
    }

    private void showStatus(String message) {
        if (statusView != null) statusView.setText(message);
    }

    private void openCameraIfReady() {
        if (!activityResumed || selectedDevice == null || previewView == null
                || !previewView.isAvailable() || opening) {
            return;
        }
        if (!usbManager.hasPermission(selectedDevice)) return;

        SurfaceTexture surfaceTexture = previewView.getSurfaceTexture();
        if (surfaceTexture == null) return;

        final int token = sessionGeneration;
        final UsbDevice device = selectedDevice;
        final Surface surface = new Surface(surfaceTexture);
        opening = true;
        showStatus("已获得 USB 权限，正在读取采集卡支持的 UVC 模式…");
        if (modeView != null) modeView.setText("");

        cameraExecutor.execute(() -> {
            UsbDeviceConnection newConnection = null;
            UvcCamera newCamera = null;
            try {
                if (token != sessionGeneration || !activityResumed) {
                    surface.release();
                    return;
                }

                newConnection = usbManager.openDevice(device);
                if (newConnection == null) {
                    throw new IOException("Android 无法打开 USB 设备。请拔插采集卡后重试。");
                }

                // droiduvc accepts the file descriptor from the still-open USB connection.
                newCamera = UvcCamera.Companion.open(newConnection.getFileDescriptor());
                List<StreamMode> modes = newCamera.supportedModes();
                if (modes == null || modes.isEmpty()) {
                    throw new IOException("设备没有报告可显示的 MJPEG/YUYV 视频模式。");
                }

                // Request 1080p50 and prefer MJPEG. The returned mode is the actual negotiated mode.
                StreamMode actual = newCamera.start(
                        surface, TARGET_WIDTH, TARGET_HEIGHT, TARGET_FPS, StreamFormat.MJPEG);

                if (token != sessionGeneration || !activityResumed || !surface.isValid()) {
                    closeLocalResources(newCamera, newConnection, surface);
                    return;
                }

                camera = newCamera;
                usbConnection = newConnection;
                streamSurface = surface;

                videoWidth = actual.getWidth();
                videoHeight = actual.getHeight();
                mainHandler.post(this::applyAspectRatio);
                final String modeText = describeMode(actual);
                final String supportedText = summarizeModes(modes);
                final String cameraStatus = "实时预览已启动";
                mainHandler.post(() -> {
                    if (token != sessionGeneration) return;
                    opening = false;
                    showStatus(cameraStatus + "（实际协商结果）");
                    if (modeView != null) {
                        modeView.setText("当前：" + modeText + "\n设备支持：" + supportedText);
                    }
                    if (statsView != null) {
                        statsView.setText("输入：" + actual.getFps() + " FPS（协商值）\n显示：" + formatFps(displayedFps));
                    }
                });
            } catch (Exception e) {
                closeLocalResources(newCamera, newConnection, surface);
                final String error = e.getMessage() == null
                        ? e.getClass().getSimpleName() : e.getMessage();
                mainHandler.post(() -> {
                    if (token != sessionGeneration) return;
                    opening = false;
                    showStatus("UVC 启动失败：" + error);
                    if (modeView != null) {
                        modeView.setText("请确认采集卡仍连接，并尝试重新扫描。");
                    }
                });
            }
        });
    }

    private void applyAspectRatio() {
        if (previewView == null || previewView.getWidth() <= 0 || previewView.getHeight() <= 0
                || videoWidth <= 0 || videoHeight <= 0) return;
        RectF source = new RectF(0, 0, videoWidth, videoHeight);
        RectF destination = new RectF(0, 0, previewView.getWidth(), previewView.getHeight());
        Matrix transform = new Matrix();
        transform.setRectToRect(source, destination, Matrix.ScaleToFit.CENTER);
        previewView.setTransform(transform);
    }

    private void updateDisplayFps() {
        long now = android.os.SystemClock.elapsedRealtime();
        if (fpsWindowStart == 0L) fpsWindowStart = now;
        renderedFrames++;
        long elapsed = now - fpsWindowStart;
        if (elapsed >= 1000L) {
            displayedFps = renderedFrames * 1000f / elapsed;
            renderedFrames = 0;
            fpsWindowStart = now;
            if (statsView != null) {
                String input = modeView != null ? modeView.getText().toString() : "";
                statsView.setText("显示：" + formatFps(displayedFps) + " FPS");
            }
        }
    }

    private String formatFps(float fps) {
        return fps <= 0f ? "--" : String.format(Locale.US, "%.1f", fps);
    }

    private String describeMode(StreamMode mode) {
        return mode.getWidth() + "×" + mode.getHeight() + " · "
                + mode.getFormat().name() + " · " + mode.getFps() + " FPS";
    }

    private String summarizeModes(List<StreamMode> modes) {
        StringBuilder result = new StringBuilder();
        int limit = Math.min(modes.size(), 8);
        for (int i = 0; i < limit; i++) {
            if (i > 0) result.append("  |  ");
            result.append(describeMode(modes.get(i)));
        }
        if (modes.size() > limit) result.append("  |  …共 ").append(modes.size()).append(" 种");
        return result.toString();
    }

    private void invalidateAndCloseSession() {
        sessionGeneration++;
        opening = false;
        fpsWindowStart = 0L;
        renderedFrames = 0;
        displayedFps = 0f;
        cameraExecutor.execute(this::closeCameraResources);
    }

    private void closeCameraResources() {
        UvcCamera oldCamera = camera;
        UsbDeviceConnection oldConnection = usbConnection;
        Surface oldSurface = streamSurface;
        camera = null;
        usbConnection = null;
        streamSurface = null;
        closeLocalResources(oldCamera, oldConnection, oldSurface);
    }

    private void closeLocalResources(UvcCamera oldCamera,
                                     UsbDeviceConnection oldConnection,
                                     Surface oldSurface) {
        if (oldCamera != null) {
            try {
                oldCamera.stop();
            } catch (Exception ignored) {
            }
            try {
                oldCamera.close();
            } catch (Exception ignored) {
            }
        }
        if (oldSurface != null) {
            try {
                oldSurface.release();
            } catch (Exception ignored) {
            }
        }
        if (oldConnection != null) {
            try {
                oldConnection.close();
            } catch (Exception ignored) {
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        activityResumed = true;
        applyImmersiveMode();
        if (Build.VERSION.SDK_INT < 23
                || checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            scanDevices();
        }
    }

    @Override
    protected void onPause() {
        activityResumed = false;
        invalidateAndCloseSession();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (receiverRegistered) {
            unregisterReceiver(usbReceiver);
            receiverRegistered = false;
        }
        invalidateAndCloseSession();
        cameraExecutor.shutdown();
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 100) {
            if (grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                scanDevices();
            } else {
                showStatus("需要允许相机权限，Android 才能通过 USB 打开 UVC 采集卡。");
            }
        }
    }
}
