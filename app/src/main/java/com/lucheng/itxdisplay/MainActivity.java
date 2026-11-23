package com.lucheng.itxdisplay;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {
    private static final String ACTION_USB_PERMISSION =
            "com.lucheng.itxdisplay.USB_PERMISSION";

    private UsbManager usbManager;
    private TextView statusView;
    private TextView deviceView;
    private Button scanButton;
    private UsbDevice selectedDevice;
    private boolean receiverRegistered = false;

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
                    statusView.setText("USB 已授权：" + device.getDeviceName()
                            + "\n下一步：打开 UVC 视频流");
                } else {
                    statusView.setText("USB 授权未通过，请重新连接并允许访问。");
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
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);

        usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        buildUi();
        registerUsbReceiver();

        if (Build.VERSION.SDK_INT >= 23
                && checkSelfPermission(Manifest.permission.CAMERA)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, 100);
        } else {
            scanDevices();
        }
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(28, 22, 28, 22);
        root.setBackgroundColor(0xFF101318);

        TextView title = new TextView(this);
        title.setText("ITX Display  ·  USB 视频输入");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(22);
        root.addView(title);

        statusView = new TextView(this);
        statusView.setText("正在检查 USB 摄像设备…");
        statusView.setTextColor(0xFFB8C5D6);
        statusView.setTextSize(15);
        statusView.setPadding(0, 18, 0, 12);
        root.addView(statusView);

        deviceView = new TextView(this);
        deviceView.setTextColor(0xFFFFFFFF);
        deviceView.setTextSize(14);
        deviceView.setPadding(0, 8, 0, 18);
        root.addView(deviceView);

        scanButton = new Button(this);
        scanButton.setText("重新扫描 USB 设备");
        scanButton.setOnClickListener(v -> scanDevices());
        root.addView(scanButton);

        TextView footer = new TextView(this);
        footer.setText("目标：1920×1080 · 50 FPS · MJPEG 优先 · 无音频");
        footer.setTextColor(0xFF7F8B9B);
        footer.setTextSize(12);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        footer.setPadding(0, 20, 0, 0);
        root.addView(footer);

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
            deviceView.setText("未发现 UVC 视频设备。\n请检查 OTG 转接头、采集卡供电和 USB 连接。");
            statusView.setText("等待 USB 视频设备连接");
            return;
        }

        UsbDevice device = candidates.get(0);
        selectedDevice = device;
        deviceView.setText("设备：" + safeName(device) + "\n"
                + "VID:PID  " + hex(device.getVendorId()) + ":" + hex(device.getProductId())
                + "\n接口数：" + device.getInterfaceCount()
                + "\n检测到 " + candidates.size() + " 个 USB 视频设备");

        if (usbManager.hasPermission(device)) {
            statusView.setText("USB 已授权。UVC 视频流接入将在下一构建步骤启用。");
        } else {
            statusView.setText("发现视频设备，正在请求 USB 访问权限…");
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
        return String.format(java.util.Locale.US, "%04X", value);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (usbManager != null) scanDevices();
    }

    @Override
    protected void onDestroy() {
        if (receiverRegistered) {
            unregisterReceiver(usbReceiver);
            receiverRegistered = false;
        }
        // The UVC camera, stream surface and UsbDeviceConnection will be closed here
        // when the streaming controller is added.
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 100) scanDevices();
    }
}
