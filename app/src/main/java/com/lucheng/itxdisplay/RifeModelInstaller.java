package com.lucheng.itxdisplay;

import android.content.Context;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Downloads the pinned RIFE 4.6 ncnn model into app-private storage.
 * The files are checked against their Git blob SHA-1 and exact byte length before use.
 */
public final class RifeModelInstaller {
    private static final String REVISION =
            "a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7";
    private static final String BASE_URL =
            "https://raw.githubusercontent.com/nihui/rife-ncnn-vulkan/" + REVISION
                    + "/models/rife-v4.6/";

    private static final ModelFile[] FILES = {
            new ModelFile("flownet.param", 16532L,
                    "3cb69d4ae09b085a2b5b51eb3c2e93745f805644"),
            new ModelFile("flownet.bin", 10614320L,
                    "70de38ecc5b6f1b63afa56108bf76911f04ce630")
    };

    private RifeModelInstaller() {}

    public interface ProgressListener {
        void onProgress(String fileName, int percent);
    }

    public static File install(Context context, ProgressListener listener) throws IOException {
        File modelDirectory = new File(context.getFilesDir(), "models/rife-v4.6");
        if (!modelDirectory.isDirectory() && !modelDirectory.mkdirs()) {
            throw new IOException("无法创建 RIFE 模型目录");
        }

        for (ModelFile model : FILES) {
            File destination = new File(modelDirectory, model.name);
            if (isValid(destination, model)) {
                if (listener != null) listener.onProgress(model.name, 100);
                continue;
            }
            download(model, destination, listener);
        }
        return modelDirectory;
    }

    private static void download(ModelFile model, File destination,
                                 ProgressListener listener) throws IOException {
        File temporary = new File(destination.getParentFile(), model.name + ".download");
        if (temporary.exists() && !temporary.delete()) {
            throw new IOException("无法清理旧的模型临时文件：" + model.name);
        }

        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(BASE_URL + model.name).openConnection();
            connection.setRequestMethod("GET");
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(60000);
            connection.setInstanceFollowRedirects(true);
            connection.setUseCaches(false);
            connection.setRequestProperty("User-Agent", "ITX-Display-Android");
            connection.setRequestProperty("Accept-Encoding", "identity");

            int response = connection.getResponseCode();
            if (response != HttpURLConnection.HTTP_OK) {
                throw new IOException("下载 " + model.name + " 失败，HTTP " + response);
            }

            MessageDigest digest = newGitBlobDigest(model.size);
            long received = 0;
            int lastPercent = -1;
            byte[] buffer = new byte[64 * 1024];
            try (InputStream input = new BufferedInputStream(connection.getInputStream());
                 FileOutputStream output = new FileOutputStream(temporary)) {
                int count;
                while ((count = input.read(buffer)) != -1) {
                    received += count;
                    if (received > model.size) {
                        throw new IOException("模型文件大小超过预期：" + model.name);
                    }
                    digest.update(buffer, 0, count);
                    output.write(buffer, 0, count);
                    if (listener != null) {
                        int percent = (int) Math.min(100, received * 100 / model.size);
                        if (percent != lastPercent) {
                            lastPercent = percent;
                            listener.onProgress(model.name, percent);
                        }
                    }
                }
                output.getFD().sync();
            }

            if (received != model.size) {
                throw new IOException("模型文件长度不正确：" + model.name + "（" + received + " bytes）");
            }
            String actualHash = toHex(digest.digest());
            if (!model.gitBlobSha1.equals(actualHash)) {
                throw new IOException("模型文件校验失败：" + model.name);
            }

            if (destination.exists() && !destination.delete()) {
                throw new IOException("无法替换旧模型文件：" + model.name);
            }
            if (!temporary.renameTo(destination)) {
                throw new IOException("无法保存模型文件：" + model.name);
            }
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("当前 Android 环境不支持 SHA-1 校验", e);
        } finally {
            if (connection != null) connection.disconnect();
            if (temporary.exists()) temporary.delete();
        }
    }

    private static boolean isValid(File file, ModelFile model) throws IOException {
        if (!file.isFile() || file.length() != model.size) return false;
        MessageDigest digest;
        try {
            digest = newGitBlobDigest(model.size);
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("当前 Android 环境不支持 SHA-1 校验", e);
        }
        byte[] buffer = new byte[64 * 1024];
        try (InputStream input = new BufferedInputStream(new FileInputStream(file))) {
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return model.gitBlobSha1.equals(toHex(digest.digest()));
    }

    private static MessageDigest newGitBlobDigest(long size) throws NoSuchAlgorithmException {
        MessageDigest digest = MessageDigest.getInstance("SHA-1");
        digest.update(("blob " + size + "\0").getBytes(StandardCharsets.UTF_8));
        return digest;
    }

    private static String toHex(byte[] value) {
        StringBuilder out = new StringBuilder(value.length * 2);
        for (byte b : value) out.append(String.format(Locale.ROOT, "%02x", b & 0xFF));
        return out.toString();
    }

    private static final class ModelFile {
        final String name;
        final long size;
        final String gitBlobSha1;

        ModelFile(String name, long size, String gitBlobSha1) {
            this.name = name;
            this.size = size;
            this.gitBlobSha1 = gitBlobSha1;
        }
    }
}
