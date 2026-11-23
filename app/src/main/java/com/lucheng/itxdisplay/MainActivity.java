package com.lucheng.itxdisplay;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.SurfaceTexture;
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
import android.widget.ScrollView;
import android.widget.TextView;

import com.mikimn.droiduvc.CameraControls;
import com.mikimn.droiduvc.CameraInfo;
import com.mikimn.droiduvc.Control;
import com.mikimn.droiduvc.ControlId;
import com.mikimn.droiduvc.ControlRange;
import com.mikimn.droiduvc.StreamFormat;
import com.mikimn.droiduvc.StreamMode;
import com.mikimn.droiduvc.UvcCamera;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;

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
    private FrameLayout previewContainer;
    private TextView statusView;
    private TextView deviceView;
    private TextView modeView;
    private TextView statsView;
    private TextView diagnosticsView;
    private Button colorReadButton;
    private Button copyDiagnosticsButton;
    private Button infoToggle;
    private volatile String baseDiagnosticsReport = "尚未连接 UVC 设备。";
    private volatile String diagnosticsReport = "尚未连接 UVC 设备。";
    private volatile boolean readingColorControls = false;
    private boolean videoInfoVisible = false;
    private int videoWidth = TARGET_WIDTH;
    private int videoHeight = TARGET_HEIGHT;
    private long fpsWindowStart = 0L;
    private int renderedFrames = 0;
    private float displayedFps = 0f;
    private boolean receiverRegistered = false;
    private volatile boolean opening = false;
    private volatile boolean previewRunning = false;
    private volatile boolean executorShutdown = false;
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
        previewContainer = root;
        root.setBackgroundColor(0xFF000000);

        // Match the source aspect ratio by sizing the TextureView itself.
        // This keeps the full frame visible and exposes black letterbox bars.
        previewView = new TextureView(this);
        previewView.setOpaque(false);
        FrameLayout.LayoutParams previewParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.CENTER);
        root.addView(previewView, previewParams);

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
        panelView = panel;
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(10), dp(14), dp(10));
        panel.setBackgroundColor(0xE5101318);
        panel.setElevation(dp(8));

        // Long device diagnostics scroll independently; the action buttons remain pinned
        // to the bottom of the panel and are always reachable.
        ScrollView panelScroll = new ScrollView(this);
        panelScroll.setFillViewport(false);
        panelScroll.setVerticalScrollBarEnabled(true);
        panelScroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);

        LinearLayout panelContent = new LinearLayout(this);
        panelContent.setOrientation(LinearLayout.VERTICAL);
        panelScroll.addView(panelContent, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        panel.addView(panelScroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        TextView title = new TextView(this);
        title.setText("ITX Display  ·  USB 视频输入");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(19);
        panelContent.addView(title);

        statusView = new TextView(this);
        statusView.setText("正在检查 USB 摄像设备…");
        statusView.setTextColor(0xFFD7E0ED);
        statusView.setTextSize(14);
        statusView.setPadding(0, dp(7), 0, dp(5));
        panelContent.addView(statusView);

        deviceView = new TextView(this);
        deviceView.setTextColor(0xFFFFFFFF);
        deviceView.setTextSize(13);
        deviceView.setPadding(0, dp(4), 0, dp(4));
        panelContent.addView(deviceView);

        modeView = new TextView(this);
        modeView.setTextColor(0xFF9FB2C9);
        modeView.setTextSize(12);
        modeView.setPadding(0, dp(4), 0, dp(8));
        panelContent.addView(modeView);

        diagnosticsView = new TextView(this);
        diagnosticsView.setText("UVC 诊断：连接设备后读取。\\n此功能只读取信息，不会修改采集卡参数。");
        diagnosticsView.setTextColor(0xFFB8C7D9);
        diagnosticsView.setTextSize(11);
        diagnosticsView.setPadding(0, dp(4), 0, dp(8));
        panelContent.addView(diagnosticsView);

        TextView target = new TextView(this);
        target.setText("目标：1920×1080 · 50 FPS · MJPEG 优先 · 无音频");
        target.setTextColor(0xFF9AA8BA);
        target.setTextSize(11);
        target.setPadding(0, dp(5), 0, dp(2));
        panelContent.addView(target);

        // Sticky actions: unlike the diagnostic text, these never scroll out of view.
        LinearLayout actionRow = new LinearLayout(this);
        actionRow.setOrientation(LinearLayout.HORIZONTAL);

        colorReadButton = new Button(this);
        colorReadButton.setText("读取 UVC 色彩控件");
        colorReadButton.setTextSize(12);
        colorReadButton.setAllCaps(false);
        colorReadButton.setMinWidth(0);
        colorReadButton.setEnabled(false);
        colorReadButton.setOnClickListener(v -> readColorControlValues());
        actionRow.addView(colorReadButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        copyDiagnosticsButton = new Button(this);
        copyDiagnosticsButton.setText("复制设备诊断报告");
        copyDiagnosticsButton.setTextSize(12);
        copyDiagnosticsButton.setAllCaps(false);
        copyDiagnosticsButton.setMinWidth(0);
        copyDiagnosticsButton.setOnClickListener(v -> copyDiagnosticsReport());
        actionRow.addView(copyDiagnosticsButton, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        panel.addView(actionRow);

        Button scanButton = new Button(this);
        scanButton.setText("重新扫描 USB 设备");
        scanButton.setTextSize(12);
        scanButton.setAllCaps(false);
        scanButton.setOnClickListener(v -> scanDevices());
        panel.addView(scanButton, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        statsView = new TextView(this);
        statsView.setText("显示 FPS：--");
        statsView.setTextColor(0xFFFFFFFF);
        statsView.setTextSize(13);
        statsView.setPadding(dp(10), dp(7), dp(10), dp(7));
        statsView.setBackgroundColor(0xB5101318);
        statsView.setVisibility(View.GONE);
        FrameLayout.LayoutParams statsParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        statsParams.setMargins(dp(14), dp(68), dp(14), dp(14));
        root.addView(statsView, statsParams);

        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        panelParams.setMargins(dp(14), dp(14), dp(14), dp(14));
        panel.setVisibility(View.GONE);
        root.addView(panel, panelParams);

        infoToggle = new Button(this);
        infoToggle.setText("显示信息");
        infoToggle.setOnClickListener(v -> setVideoInfoVisible(!videoInfoVisible));
        FrameLayout.LayoutParams infoParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.END);
        infoParams.setMargins(dp(14), dp(14), dp(14), dp(14));
        infoToggle.setElevation(dp(10));
        root.addView(infoToggle, infoParams);

        root.addOnLayoutChangeListener((view, left, top, right, bottom,
                                        oldLeft, oldTop, oldRight, oldBottom) -> {
            applyAspectRatio();
            updateInfoPanelBounds();
        });
        setContentView(root);
    }

    private void setVideoInfoVisible(boolean visible) {
        videoInfoVisible = visible;
        if (panelView != null) panelView.setVisibility(visible ? View.VISIBLE : View.GONE);
        if (statsView != null) statsView.setVisibility(visible ? View.VISIBLE : View.GONE);
        if (infoToggle != null) infoToggle.setText(visible ? "隐藏信息" : "显示信息");
    }

    private LinearLayout panelView;

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
            if (previewRunning) {
                showStatus("UVC 预览正在运行；设备已重新扫描，无需重复打开。");
                return;
            }
            if (opening) {
                showStatus("UVC 设备正在打开，请稍候…");
                return;
            }
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
        if (previewRunning) {
            showStatus("UVC 预览已经运行，无需重复打开设备。");
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
                previewRunning = true;

                videoWidth = actual.getWidth();
                videoHeight = actual.getHeight();
                mainHandler.post(this::applyAspectRatio);
                final String modeText = describeMode(actual);
                final String supportedText = summarizeModes(modes);
                final String cameraDiagnostics = buildCameraDiagnostics(newCamera, actual, modes);
                baseDiagnosticsReport = cameraDiagnostics;
                diagnosticsReport = cameraDiagnostics;
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
                    if (diagnosticsView != null) diagnosticsView.setText(cameraDiagnostics);
                    if (colorReadButton != null) colorReadButton.setEnabled(true);
                });
            } catch (Exception e) {
                previewRunning = false;
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

    private String buildCameraDiagnostics(UvcCamera activeCamera,
                                           StreamMode actual,
                                           List<StreamMode> modes) {
        StringBuilder report = new StringBuilder();
        report.append("ITX Display UVC 诊断\n");
        report.append("实际模式：").append(describeMode(actual)).append("\n");
        report.append("设备报告模式数：").append(modes == null ? 0 : modes.size()).append("\n");

        try {
            CameraInfo info = activeCamera.getInfo();
            report.append("厂商：").append(orUnknown(info.getManufacturer())).append("\n");
            report.append("产品：").append(orUnknown(info.getProduct())).append("\n");
            report.append("VID:PID：")
                    .append(info.getVendorId() == null ? "--" : hex(info.getVendorId()))
                    .append(":")
                    .append(info.getProductId() == null ? "--" : hex(info.getProductId()))
                    .append("\n");
            report.append("UVC 版本：").append(orUnknown(info.getUvcVersion())).append("\n");
        } catch (Exception e) {
            report.append("设备描述符读取失败：").append(e.getClass().getSimpleName())
                    .append(" / ").append(safeErrorMessage(e)).append("\n");
        }

        try {
            CameraControls controls = activeCamera.getControls();
            report.append("色彩相关 UVC 控件支持情况：\n");
            ControlId[] colorIds = {
                    ControlId.BRIGHTNESS,
                    ControlId.CONTRAST,
                    ControlId.GAMMA,
                    ControlId.SATURATION,
                    ControlId.HUE,
                    ControlId.GAIN,
                    ControlId.BACKLIGHT_COMPENSATION
            };
            for (ControlId id : colorIds) {
                report.append("  ").append(controlLabel(id)).append("：")
                        .append(controls.get(id) == null ? "不支持/未声明" : "设备声明支持")
                        .append("\n");
            }
        } catch (Exception e) {
            report.append("UVC 控件能力读取失败：").append(e.getClass().getSimpleName())
                    .append(" / ").append(safeErrorMessage(e)).append("\n");
        }

        report.append("\n色彩元数据限制：当前 droiduvc API 未向应用暴露原始 UVC 帧、")
                .append("HDMI 输入色彩格式或可靠的 Full/Limited 范围标记。\n");
        report.append("当前处理：droiduvc 原生层先将 MJPEG/YUYV 转为 RGB，再输出到 Android Surface。\n");
        report.append("注意：本报告只读取设备信息和控件能力，不会修改采集卡设置。");
        return report.toString();
    }

    private String orUnknown(String value) {
        return value == null || value.trim().isEmpty() ? "未知/设备未提供" : value;
    }

    private String safeErrorMessage(Exception e) {
        String message = e.getMessage();
        return message == null || message.trim().isEmpty() ? "无详细错误信息" : message;
    }

    private String controlLabel(ControlId id) {
        switch (id) {
            case BRIGHTNESS: return "亮度";
            case CONTRAST: return "对比度";
            case GAMMA: return "伽马";
            case SATURATION: return "饱和度";
            case HUE: return "色调";
            case GAIN: return "增益";
            case BACKLIGHT_COMPENSATION: return "背光补偿";
            default: return id.name();
        }
    }

    private void readColorControlValues() {
        if (!previewRunning || executorShutdown || readingColorControls) {
            showStatus("请先连接并启动 UVC 预览。");
            return;
        }

        final int token = sessionGeneration;
        readingColorControls = true;
        if (colorReadButton != null) colorReadButton.setEnabled(false);
        showStatus("正在后台读取 UVC 色彩控件（只读）…");

        try {
            cameraExecutor.execute(() -> {
                String valueReport;
                UvcCamera activeCamera = camera;
                if (activeCamera == null || token != sessionGeneration) {
                    valueReport = "色彩控件读取已取消：UVC 会话已结束。";
                } else {
                    StringBuilder values = new StringBuilder("色彩相关 UVC 控件当前值（设备原始值）：\n");
                    try {
                        CameraControls controls = activeCamera.getControls();
                        ControlId[] colorIds = {
                                ControlId.BRIGHTNESS,
                                ControlId.CONTRAST,
                                ControlId.GAMMA,
                                ControlId.SATURATION,
                                ControlId.HUE,
                                ControlId.GAIN,
                                ControlId.BACKLIGHT_COMPENSATION
                        };

                        for (ControlId id : colorIds) {
                            Control control = controls.get(id);
                            values.append(controlLabel(id)).append("：");
                            if (control == null) {
                                values.append("不支持/未声明");
                            } else {
                                try {
                                    int currentValue = control.getValue();
                                    values.append("当前=").append(currentValue);
                                    try {
                                        ControlRange range = control.getRange();
                                        if (range != null) {
                                            values.append("；范围=").append(range.getMin())
                                                    .append("～").append(range.getMax())
                                                    .append("；步进=").append(range.getStep())
                                                    .append("；设备默认=").append(range.getDefault());
                                        } else {
                                            values.append("；范围信息不可用");
                                        }
                                    } catch (Exception rangeError) {
                                        values.append("；范围读取失败（")
                                                .append(rangeError.getClass().getSimpleName())
                                                .append("：").append(safeErrorMessage(rangeError)).append("）");
                                    }
                                } catch (Exception e) {
                                    values.append("读取失败（").append(e.getClass().getSimpleName())
                                            .append("：").append(safeErrorMessage(e)).append("）");
                                }
                            }
                            values.append("\n");
                        }
                    } catch (Exception e) {
                        values.append("控件读取失败：").append(e.getClass().getSimpleName())
                                .append(" / ").append(safeErrorMessage(e));
                    }
                    valueReport = values.toString();
                }

                final String resultText = valueReport;
                mainHandler.post(() -> {
                    readingColorControls = false;
                    if (token != sessionGeneration) {
                        if (colorReadButton != null) colorReadButton.setEnabled(previewRunning);
                        return;
                    }
                    diagnosticsReport = baseDiagnosticsReport + "\n\n" + resultText;
                    if (diagnosticsView != null) diagnosticsView.setText(diagnosticsReport);
                    if (colorReadButton != null) colorReadButton.setEnabled(previewRunning);
                    showStatus("UVC 色彩控件读取完成；没有修改采集卡参数。");
                });
            });
        } catch (RejectedExecutionException e) {
            readingColorControls = false;
            if (colorReadButton != null) colorReadButton.setEnabled(previewRunning);
            showStatus("诊断任务未能启动，请稍后重试。");
        }
    }

    private void copyDiagnosticsReport() {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            showStatus("无法访问系统剪贴板。");
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("ITX Display UVC diagnostics", diagnosticsReport));
        showStatus("UVC 诊断报告已复制。");
    }

    private void updateInfoPanelBounds() {
        if (previewContainer == null || panelView == null) return;
        int viewportWidth = previewContainer.getWidth();
        int viewportHeight = previewContainer.getHeight();
        if (viewportWidth <= 0 || viewportHeight <= 0) return;

        int horizontalMargin = dp(14);
        int availableWidth = Math.max(dp(220), viewportWidth - horizontalMargin * 2);
        int panelWidth = Math.min(dp(760), availableWidth);
        int availableHeight = Math.max(dp(220), viewportHeight - horizontalMargin * 2);
        int panelHeight = Math.min((int) (viewportHeight * 0.78f), availableHeight);

        ViewGroup.LayoutParams rawParams = panelView.getLayoutParams();
        if (!(rawParams instanceof FrameLayout.LayoutParams)) return;
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) rawParams;
        if (params.width != panelWidth || params.height != panelHeight) {
            params.width = panelWidth;
            params.height = panelHeight;
            params.gravity = Gravity.TOP | Gravity.START;
            params.setMargins(horizontalMargin, horizontalMargin,
                    horizontalMargin, horizontalMargin);
            panelView.setLayoutParams(params);
        }
    }

    private void applyAspectRatio() {
        if (previewContainer == null || previewView == null
                || videoWidth <= 0 || videoHeight <= 0) return;

        int containerWidth = previewContainer.getWidth();
        int containerHeight = previewContainer.getHeight();
        if (containerWidth <= 0 || containerHeight <= 0) return;

        // Fit/contain: size the TextureView layout to the negotiated stream ratio.
        // The root remains black, so any unused area becomes letterboxing.
        double videoRatio = (double) videoWidth / (double) videoHeight;
        double containerRatio = (double) containerWidth / (double) containerHeight;

        int fittedWidth;
        int fittedHeight;
        if (containerRatio > videoRatio) {
            fittedHeight = containerHeight;
            fittedWidth = Math.max(1, (int) Math.round(fittedHeight * videoRatio));
        } else {
            fittedWidth = containerWidth;
            fittedHeight = Math.max(1, (int) Math.round(fittedWidth / videoRatio));
        }

        ViewGroup.LayoutParams current = previewView.getLayoutParams();
        if (!(current instanceof FrameLayout.LayoutParams)) return;

        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) current;
        if (params.width != fittedWidth || params.height != fittedHeight
                || params.gravity != Gravity.CENTER) {
            params.width = fittedWidth;
            params.height = fittedHeight;
            params.gravity = Gravity.CENTER;
            previewView.setLayoutParams(params);
        }

        // No texture scaling matrix: the view itself now has the video aspect ratio.
        previewView.setTransform(null);
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
        previewRunning = false;
        readingColorControls = false;
        if (colorReadButton != null) colorReadButton.setEnabled(false);
        fpsWindowStart = 0L;
        renderedFrames = 0;
        displayedFps = 0f;
        // Surface callbacks can arrive during Activity teardown. Never submit work
        // to an executor after shutdown has started.
        if (!executorShutdown) {
            try {
                cameraExecutor.execute(this::closeCameraResources);
            } catch (RejectedExecutionException ignored) {
                // The executor may have been shut down concurrently by onDestroy().
            }
        }
    }

    private void closeCameraResources() {
        previewRunning = false;
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
        executorShutdown = true;
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
