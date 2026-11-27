package com.zxg.miaopaperflash;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Calendar;
import java.util.TimeZone;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/**
 * 阅读器固件 BLE 内容传输客户端（电子书 0x10..0x12 / 锁屏图 0x30..0x32 / 对时 0xDD）。
 *
 * <p>严格对齐 firmware/e2_e2s_mp 的 Miaopaper 兼容 GATT 服务（0x221F/0x331F 与 0x1F10/0x1F1F），
 * 仅在需要操作时短连接并在完成后释放链路，避免长期占用蓝牙。
 */
@SuppressLint("MissingPermission")
final class ReaderTransfer {
    private static final String TAG = "E2S-XFER";
    private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());

    private static final UUID SVC_MAIN = UUID.fromString("0000221f-0000-1000-8000-00805f9b34fb");
    private static final UUID CHR_MAIN = UUID.fromString("0000331f-0000-1000-8000-00805f9b34fb");
    private static final UUID SVC_RXTX = UUID.fromString("00001f10-0000-1000-8000-00805f9b34fb");
    private static final UUID CHR_RXTX = UUID.fromString("00001f1f-0000-1000-8000-00805f9b34fb");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    static final int LOCK_IMAGE_BYTES = 4000;
    static final int ENC_ASCII = 0;
    static final int ENC_GB2312 = 1;

    private static final int OP_BOOK_BEGIN = 0x10;
    private static final int OP_BOOK_DATA = 0x11;
    private static final int OP_BOOK_END = 0x12;
    private static final int OP_CATALOG_COMPACT = 0x1A;
    private static final int OP_LOCK_BEGIN = 0x30;
    private static final int OP_LOCK_DATA = 0x31;
    private static final int OP_LOCK_END = 0x32;
    private static final int OP_OTA_INFO = 0x50;
    private static final int OP_SET_TIME = 0xDD;

    private static final int ST_OK = 0x00;
    private static final int ST_SLOTS_FULL = 0x01;
    private static final int ST_NO_SPACE = 0x02;
    private static final int ST_BAD_PARAM = 0xFD;
    private static final int ST_BUSY = 0xFE;
    private static final int ST_FAIL = 0xFF;

    private static final long IO_TIMEOUT_MS = 5_000;
    private static final long CONNECT_TIMEOUT_MS = 10_000;
    private static final long FALLBACK_TIMEOUT_MS = 16_000;
    private static final long WINDOW_TIMEOUT_MS = 4_000;
    private static final long RECONNECT_COOLDOWN_MS = 500;

    private static volatile long lastCloseMs = 0L;

    interface Listener {
        void onStage(String title, String detail, Float progress);
    }

    /** 上传电子书到阅读器（自动对时、清理残槽、窗口流控传输）。 */
    static void uploadBook(Context context, BluetoothDevice device,
                           byte[] titleBytes, byte[] bookData, int enc,
                           Listener listener) throws IOException {
        if (titleBytes == null || titleBytes.length < 2 || titleBytes.length > 19) {
            throw new IOException("书名长度无效");
        }
        if (bookData == null || bookData.length < 16 || bookData.length > 7 * 1024 * 1024) {
            throw new IOException("电子书体积超出支持范围");
        }

        listener.onStage("连接阅读器…", "", null);
        Session s = openSession(context, device);
        try {
            checkUnlockedBestEffort(s);
            syncTimeBestEffort(s);
            compactCatalogBestEffort(s);

            listener.onStage("准备传输…", "", 0f);
            byte[] beginPkt = new byte[6 + titleBytes.length];
            beginPkt[0] = (byte) OP_BOOK_BEGIN;
            beginPkt[1] = (byte) titleBytes.length;
            System.arraycopy(titleBytes, 0, beginPkt, 2, titleBytes.length);
            int total = bookData.length;
            beginPkt[2 + titleBytes.length] = (byte) ((total >>> 16) & 0xFF);
            beginPkt[3 + titleBytes.length] = (byte) ((total >>> 8) & 0xFF);
            beginPkt[4 + titleBytes.length] = (byte) (total & 0xFF);
            beginPkt[5 + titleBytes.length] = (byte) (enc == ENC_GB2312 ? ENC_GB2312 : ENC_ASCII);

            s.clearNotifications();
            s.writeMain(beginPkt, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            byte[] ack = s.awaitNotify(OP_BOOK_BEGIN, 6_000);
            if (ack == null || ack.length < 2) {
                throw new IOException("设备无响应");
            }
            int st = ack[1] & 0xFF;
            if (st != ST_OK) {
                throw new IOException(bookStatusError(st));
            }

            streamBookData(s, bookData, listener);

            listener.onStage("保存书籍…", "", 1f);
            s.clearNotifications();
            s.writeMain(new byte[]{(byte) OP_BOOK_END}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            byte[] endAck = s.awaitNotify(OP_BOOK_END, 12_000);
            if (endAck == null || endAck.length < 6) {
                throw new IOException("等待保存确认超时");
            }
            int endSt = endAck[1] & 0xFF;
            int written = ((endAck[3] & 0xFF) << 16)
                    | ((endAck[4] & 0xFF) << 8)
                    | (endAck[5] & 0xFF);
            if (endSt != ST_OK || written != total) {
                throw new IOException("校验失败（已收 " + written + " / " + total + " B）");
            }
        } finally {
            s.close();
        }
    }

    /** 上传 4000 字节锁屏图片到阅读器（逐包确认，零丢包）。 */
    static void uploadLockImage(Context context, BluetoothDevice device,
                                byte[] lockData, Listener listener) throws IOException {
        if (lockData == null || lockData.length != LOCK_IMAGE_BYTES) {
            throw new IOException("锁屏图大小必须为 4000 B");
        }

        listener.onStage("连接阅读器…", "", null);
        Session s = openSession(context, device);
        try {
            checkUnlockedBestEffort(s);
            syncTimeBestEffort(s);

            listener.onStage("传输锁屏图…", "0 / 4000 B", 0f);
            s.clearNotifications();
            byte[] begin = new byte[]{
                    (byte) OP_LOCK_BEGIN,
                    0x00,
                    (byte) ((LOCK_IMAGE_BYTES >>> 8) & 0xFF),
                    (byte) (LOCK_IMAGE_BYTES & 0xFF),
            };
            s.writeMain(begin, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            byte[] bAck = s.awaitNotify(OP_LOCK_BEGIN, 5_000);
            if (bAck == null || bAck.length < 2) {
                throw new IOException("设备无响应");
            }
            int st = bAck[1] & 0xFF;
            if (st != ST_OK) {
                throw new IOException(lockStatusError(st));
            }

            // 头部 3 B（0x31 + off u16 BE），按 MTU 安全切分，逐包确认。
            int chunkMax = Math.max(16, Math.min(200, s.mtu - 6));
            int off = 0;
            while (off < LOCK_IMAGE_BYTES) {
                if (Thread.currentThread().isInterrupted()) {
                    throw new InterruptedIOException("已取消");
                }
                int take = Math.min(chunkMax, LOCK_IMAGE_BYTES - off);
                byte[] pkt = new byte[3 + take];
                pkt[0] = (byte) OP_LOCK_DATA;
                pkt[1] = (byte) ((off >>> 8) & 0xFF);
                pkt[2] = (byte) (off & 0xFF);
                System.arraycopy(lockData, off, pkt, 3, take);

                s.clearNotifications();
                s.writeMain(pkt, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                byte[] dAck = s.awaitNotify(OP_LOCK_DATA, 5_000);
                if (dAck == null || dAck.length < 2 || (dAck[1] & 0xFF) != ST_OK) {
                    int dst = (dAck != null && dAck.length >= 2) ? (dAck[1] & 0xFF) : ST_FAIL;
                    throw new IOException("写入锁屏图失败：" + lockStatusError(dst));
                }
                off += take;
                listener.onStage(
                        "传输锁屏图…",
                        off + " / " + LOCK_IMAGE_BYTES + " B",
                        (float) off / LOCK_IMAGE_BYTES
                );
            }

            listener.onStage("应用锁屏图…", "", 1f);
            s.clearNotifications();
            s.writeMain(new byte[]{(byte) OP_LOCK_END}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            byte[] eAck = s.awaitNotify(OP_LOCK_END, 6_000);
            if (eAck == null || eAck.length < 2 || (eAck[1] & 0xFF) != ST_OK) {
                int est = (eAck != null && eAck.length >= 2) ? (eAck[1] & 0xFF) : ST_FAIL;
                throw new IOException("保存锁屏图失败：" + lockStatusError(est));
            }
        } finally {
            s.close();
        }
    }

    private static void streamBookData(Session s, byte[] data, Listener listener) throws IOException {
        int total = data.length;
        int chunkMax = Math.max(16, Math.min(243, s.mtu - 4));
        // 固件收包队列深 48：把在途窗口控制在 16 包 / 4 KiB 以内，杜绝队列溢出丢包。
        int windowBytes = Math.min(4096, Math.max(chunkMax * 4, chunkMax * 16));

        int sent = 0;
        int acked = 0;
        long startMs = SystemClock.elapsedRealtime();
        long lastUiMs = 0L;

        while (sent < total) {
            if (Thread.currentThread().isInterrupted()) {
                throw new InterruptedIOException("已取消");
            }
            acked = drainBookWatermarks(s, acked, sent, chunkMax);

            if ((sent - acked) >= windowBytes) {
                byte[] ntf = s.awaitNotify(OP_BOOK_DATA, WINDOW_TIMEOUT_MS);
                if (ntf == null) {
                    throw new IOException("传输超时，设备未确认进度");
                }
                int w = parseU24Watermark(ntf);
                if (w == 0 && sent > chunkMax) {
                    throw new IOException("设备中止了接收，请重试");
                }
                if (w > acked) {
                    acked = w;
                }
                continue;
            }

            int take = Math.min(chunkMax, total - sent);
            byte[] pkt = new byte[1 + take];
            pkt[0] = (byte) OP_BOOK_DATA;
            System.arraycopy(data, sent, pkt, 1, take);
            s.writeMain(pkt, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            sent += take;

            long now = SystemClock.elapsedRealtime();
            if (now - lastUiMs >= 150L || sent == total) {
                lastUiMs = now;
                float secs = Math.max(0.1f, (now - startMs) / 1000f);
                float kbps = (Math.max(acked, sent / 2) / 1024f) / secs;
                String detail = String.format("%d / %d KB (%.1f KB/s)",
                        sent / 1024, (total + 1023) / 1024, kbps);
                listener.onStage("传输电子书", detail, (float) sent / total);
            }
        }

        // 等待末尾几包被固件写入完成（最多 1.5 s），最终以 0x12 返回为准。
        long drainEnd = SystemClock.elapsedRealtime() + 1_500;
        while (acked < total) {
            long rem = drainEnd - SystemClock.elapsedRealtime();
            if (rem <= 0) break;
            byte[] ntf = s.awaitNotify(OP_BOOK_DATA, Math.min(rem, 250L));
            if (ntf == null) continue;
            int w = parseU24Watermark(ntf);
            if (w == 0) {
                throw new IOException("设备中止了接收，请重试");
            }
            if (w > acked) {
                acked = w;
            }
        }
    }

    private static int drainBookWatermarks(Session s, int curAcked, int sent, int chunkMax) throws IOException {
        int acked = curAcked;
        while (true) {
            byte[] ntf = s.pollNotify(OP_BOOK_DATA);
            if (ntf == null) return acked;
            int w = parseU24Watermark(ntf);
            if (w == 0 && sent > chunkMax) {
                throw new IOException("设备中止了接收，请重试");
            }
            if (w > acked) {
                acked = w;
            }
        }
    }

    private static int parseU24Watermark(byte[] ntf) {
        if (ntf == null || ntf.length < 4) return 0;
        return ((ntf[1] & 0xFF) << 16) | ((ntf[2] & 0xFF) << 8) | (ntf[3] & 0xFF);
    }

    /** 探测新固件锁屏状态（老固件无 0x50 响应时静默跳过）。 */
    private static void checkUnlockedBestEffort(Session s) throws IOException {
        try {
            s.clearNotifications();
            s.writeMain(new byte[]{(byte) OP_OTA_INFO}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            byte[] rsp = s.awaitNotify(OP_OTA_INFO, 800);
            if (rsp != null && rsp.length >= 9 && (rsp[1] & 0xFF) == ST_OK) {
                boolean locked = ((rsp[8] & 0xFF) & (1 << 3)) != 0;
                if (locked) {
                    throw new IOException("阅读器已锁屏，请先解锁");
                }
            }
        } catch (IOException e) {
            if (e.getMessage() != null && e.getMessage().contains("锁屏")) {
                throw e;
            }
            // 旧版固件可能未实现 0x50，忽略继续
        }
    }

    /** 连接时顺手向 0x1F1F 写入本地时间 0xDD，无应答且失败不影响传输。 */
    private static void syncTimeBestEffort(Session s) {
        if (s.rxtxChr == null) return;
        try {
            long nowMs = System.currentTimeMillis();
            int offsetSec = TimeZone.getDefault().getOffset(nowMs) / 1000;
            long localUnix = (nowMs / 1000L) + offsetSec;
            Calendar cal = Calendar.getInstance();
            int year = cal.get(Calendar.YEAR);
            int month = cal.get(Calendar.MONTH) + 1;
            int day = cal.get(Calendar.DAY_OF_MONTH);
            int dow = cal.get(Calendar.DAY_OF_WEEK); // 1=Sun, 2=Mon .. 7=Sat
            int week = (dow == Calendar.SUNDAY) ? 7 : (dow - 1);

            byte[] pkt = new byte[10];
            pkt[0] = (byte) OP_SET_TIME;
            pkt[1] = (byte) ((localUnix >>> 24) & 0xFF);
            pkt[2] = (byte) ((localUnix >>> 16) & 0xFF);
            pkt[3] = (byte) ((localUnix >>> 8) & 0xFF);
            pkt[4] = (byte) (localUnix & 0xFF);
            pkt[5] = (byte) ((year >>> 8) & 0xFF);
            pkt[6] = (byte) (year & 0xFF);
            pkt[7] = (byte) month;
            pkt[8] = (byte) day;
            pkt[9] = (byte) week;
            s.writeRxTx(pkt);
        } catch (IOException ignored) {
        }
    }

    private static void compactCatalogBestEffort(Session s) {
        try {
            s.clearNotifications();
            s.writeMain(new byte[]{(byte) OP_CATALOG_COMPACT}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            s.awaitNotify(OP_CATALOG_COMPACT, 1_500);
        } catch (IOException ignored) {
        }
    }

    private static String bookStatusError(int st) {
        switch (st) {
            case ST_SLOTS_FULL: return "书库目录已满，请先删书";
            case ST_NO_SPACE: return "存储空间不足";
            case ST_BAD_PARAM: return "书名或文件参数无效";
            case ST_BUSY: return "设备忙，请稍后重试";
            case ST_FAIL: return "写入被拒绝（请确认阅读器已解锁）";
            default: return String.format("设备拒绝（0x%02X）", st & 0xFF);
        }
    }

    private static String lockStatusError(int st) {
        switch (st) {
            case ST_BAD_PARAM: return "图片尺寸不符";
            case ST_BUSY: return "设备忙，请稍后重试";
            case ST_FAIL: return "写入被拒绝（请确认阅读器已解锁）";
            default: return String.format("错误码 0x%02X", st & 0xFF);
        }
    }

    private static void awaitCooldown() throws InterruptedIOException {
        long closedAt = lastCloseMs;
        if (closedAt == 0L) return;
        long remain = RECONNECT_COOLDOWN_MS - (SystemClock.elapsedRealtime() - closedAt);
        if (remain > 0L) sleepInterruptedly(remain);
    }

    private static void sleepInterruptedly(long ms) throws InterruptedIOException {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("已取消");
        }
    }

    private static Session openSession(Context context, BluetoothDevice device) throws IOException {
        awaitCooldown();
        try {
            return openSingle(context, device, false, CONNECT_TIMEOUT_MS);
        } catch (InterruptedIOException e) {
            throw e;
        } catch (IOException first) {
            if (Thread.currentThread().isInterrupted()) throw first;
            Log.w(TAG, "直连失败，改用 autoConnect 重试: " + first.getMessage());
            sleepInterruptedly(300L);
            awaitCooldown();
            return openSingle(context, device, true, FALLBACK_TIMEOUT_MS);
        }
    }

    private static Session openSingle(Context context, BluetoothDevice device,
                                      boolean autoConnect, long timeoutMs) throws IOException {
        Session s = new Session();
        boolean ok = false;
        try {
            s.connect(context, device, autoConnect, timeoutMs);
            ok = true;
            return s;
        } catch (RuntimeException e) {
            throw new IOException("蓝牙连接失败", e);
        } finally {
            if (!ok) s.close();
        }
    }

    private static final class Session {
        private enum Stage { CONNECT, DISCOVER, MTU, CCCD, WRITE }

        private final ArrayDeque<byte[]> notifies = new ArrayDeque<>();
        private BluetoothGatt gatt;
        private BluetoothGattCharacteristic mainChr;
        BluetoothGattCharacteristic rxtxChr;
        private Stage pending;
        private long deadline;
        private IOException failure;
        private boolean closed;
        private boolean connected;
        private boolean disconnectedAck;
        int mtu = 23;

        void connect(Context context, BluetoothDevice device,
                     boolean autoConnect, long timeoutMs) throws IOException {
            openGattOnMain(context, device, autoConnect, timeoutMs);
            synchronized (this) {
                perform(Stage.DISCOVER, 8_000, () -> gatt.discoverServices());
                BluetoothGattService svc = gatt.getService(SVC_MAIN);
                if (svc == null) {
                    throw new IOException("未找到阅读器服务，请确认已开启阅读器蓝牙");
                }
                mainChr = svc.getCharacteristic(CHR_MAIN);
                if (mainChr == null) {
                    throw new IOException("未找到阅读器数据通道");
                }
                BluetoothGattService rxSvc = gatt.getService(SVC_RXTX);
                if (rxSvc != null) {
                    rxtxChr = rxSvc.getCharacteristic(CHR_RXTX);
                }
                BluetoothGattDescriptor cccd = mainChr.getDescriptor(CCCD);
                if (cccd == null) {
                    throw new IOException("缺少通知描述符");
                }
                if (!gatt.setCharacteristicNotification(mainChr, true)) {
                    throw new IOException("无法启用通知");
                }
                try {
                    perform(Stage.MTU, IO_TIMEOUT_MS, () -> gatt.requestMtu(247));
                } catch (IOException e) {
                    Log.w(TAG, "MTU 协商降级至 " + mtu + ": " + e.getMessage());
                    failure = null;
                }
                perform(Stage.CCCD, IO_TIMEOUT_MS, () -> writeCccd(cccd));
                gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
            }
        }

        private void openGattOnMain(Context context, BluetoothDevice device,
                                    boolean autoConnect, long timeoutMs) throws IOException {
            synchronized (this) {
                checkLive();
                pending = Stage.CONNECT;
                deadline = SystemClock.elapsedRealtime() + timeoutMs;
            }
            CountDownLatch latch = new CountDownLatch(1);
            AtomicReference<RuntimeException> err = new AtomicReference<>();
            Runnable task = () -> {
                try {
                    synchronized (Session.this) {
                        if (closed || failure != null) return;
                        gatt = device.connectGatt(context, autoConnect, callback,
                                BluetoothDevice.TRANSPORT_LE);
                        if (gatt == null) {
                            fail(new IOException("无法发起蓝牙连接"));
                        }
                    }
                } catch (RuntimeException e) {
                    err.set(e);
                } finally {
                    latch.countDown();
                }
            };
            if (Looper.myLooper() == Looper.getMainLooper()) {
                task.run();
            } else {
                if (!MAIN_HANDLER.post(task)) {
                    throw new IOException("主线程任务投递失败");
                }
                try {
                    if (!latch.await(IO_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                        throw new IOException("发起连接超时");
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("已取消");
                }
            }
            if (err.get() != null) throw err.get();
            synchronized (this) {
                awaitStage(Stage.CONNECT);
            }
        }

        synchronized void writeMain(byte[] payload, int writeType) throws IOException {
            perform(Stage.WRITE, IO_TIMEOUT_MS, () -> writeChr(mainChr, payload, writeType));
        }

        synchronized void writeRxTx(byte[] payload) throws IOException {
            if (rxtxChr == null) return;
            perform(Stage.WRITE, IO_TIMEOUT_MS,
                    () -> writeChr(rxtxChr, payload, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE));
        }

        synchronized void clearNotifications() {
            notifies.clear();
        }

        synchronized byte[] pollNotify(int expectedOp) throws IOException {
            checkLive();
            while (!notifies.isEmpty()) {
                byte[] p = notifies.pollFirst();
                if (p != null && p.length > 0 && (p[0] & 0xFF) == expectedOp) {
                    return p;
                }
            }
            return null;
        }

        synchronized byte[] awaitNotify(int expectedOp, long timeoutMs) throws IOException {
            long end = SystemClock.elapsedRealtime() + timeoutMs;
            while (true) {
                byte[] hit = pollNotify(expectedOp);
                if (hit != null) return hit;
                long rem = end - SystemClock.elapsedRealtime();
                if (rem <= 0) return null;
                try {
                    wait(rem);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("已取消");
                }
            }
        }

        private void perform(Stage stage, long timeoutMs, BooleanSupplier action) throws IOException {
            checkLive();
            pending = stage;
            deadline = SystemClock.elapsedRealtime() + timeoutMs;
            if (!action.getAsBoolean()) {
                pending = null;
                throw new IOException("蓝牙写入失败");
            }
            awaitStage(stage);
        }

        private void awaitStage(Stage stage) throws IOException {
            while (pending == stage) {
                checkLive();
                long rem = deadline - SystemClock.elapsedRealtime();
                if (rem <= 0) {
                    pending = null;
                    throw new IOException("蓝牙响应超时");
                }
                try {
                    wait(rem);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    pending = null;
                    throw new InterruptedIOException("已取消");
                }
            }
            checkLive();
        }

        private void checkLive() throws IOException {
            if (failure != null) throw failure;
            if (closed) throw new IOException("连接已关闭");
        }

        private void finish(Stage stage, int status) {
            if (pending != stage) return;
            pending = null;
            if (status != BluetoothGatt.GATT_SUCCESS) {
                fail(new IOException("GATT 错误 " + status));
                return;
            }
            notifyAll();
        }

        private void fail(IOException e) {
            if (failure == null) failure = e;
            pending = null;
            notifyAll();
        }

        @SuppressWarnings("deprecation")
        private boolean writeCccd(BluetoothGattDescriptor desc) {
            byte[] val = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeDescriptor(desc, val) == BluetoothStatusCodes.SUCCESS;
            }
            desc.setValue(val);
            return gatt.writeDescriptor(desc);
        }

        @SuppressWarnings("deprecation")
        private boolean writeChr(BluetoothGattCharacteristic chr, byte[] payload, int writeType) {
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeCharacteristic(chr, payload, writeType) == BluetoothStatusCodes.SUCCESS;
            }
            chr.setWriteType(writeType);
            chr.setValue(payload);
            return gatt.writeCharacteristic(chr);
        }

        void close() {
            BluetoothGatt toClose;
            synchronized (this) {
                if (closed) return;
                closed = true;
                pending = null;
                toClose = gatt;
                notifyAll();
                if (toClose != null && connected) {
                    try {
                        toClose.disconnect();
                    } catch (RuntimeException ignored) {
                    }
                    long end = SystemClock.elapsedRealtime() + 500;
                    while (!disconnectedAck) {
                        long rem = end - SystemClock.elapsedRealtime();
                        if (rem <= 0) break;
                        try {
                            wait(rem);
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            break;
                        }
                    }
                }
                gatt = null;
            }
            if (toClose != null) {
                try {
                    toClose.close();
                } catch (RuntimeException ignored) {
                }
            }
            lastCloseMs = SystemClock.elapsedRealtime();
        }

        private final BluetoothGattCallback callback = new BluetoothGattCallback() {
            @Override
            public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
                synchronized (Session.this) {
                    if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                        connected = true;
                        finish(Stage.CONNECT, BluetoothGatt.GATT_SUCCESS);
                    } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                        connected = false;
                        disconnectedAck = true;
                        if (!closed) {
                            fail(new IOException("连接已断开"));
                        } else {
                            notifyAll();
                        }
                    } else if (status != BluetoothGatt.GATT_SUCCESS) {
                        fail(new IOException("连接失败（状态 " + status + "）"));
                    }
                }
            }

            @Override
            public void onServicesDiscovered(BluetoothGatt g, int status) {
                synchronized (Session.this) {
                    finish(Stage.DISCOVER, status);
                }
            }

            @Override
            public void onMtuChanged(BluetoothGatt g, int newMtu, int status) {
                synchronized (Session.this) {
                    if (status == BluetoothGatt.GATT_SUCCESS && newMtu >= 23) {
                        mtu = newMtu;
                    }
                    finish(Stage.MTU, status);
                }
            }

            @Override
            public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
                synchronized (Session.this) {
                    finish(Stage.CCCD, status);
                }
            }

            @Override
            public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
                synchronized (Session.this) {
                    finish(Stage.WRITE, status);
                }
            }

            @Override
            public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
                if (value != null) pushNotify(value);
            }

            @SuppressWarnings("deprecation")
            @Override
            public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
                if (Build.VERSION.SDK_INT < 33) {
                    byte[] val = c.getValue();
                    if (val != null) pushNotify(val);
                }
            }

            private void pushNotify(byte[] raw) {
                synchronized (Session.this) {
                    notifies.addLast(Arrays.copyOf(raw, raw.length));
                    notifyAll();
                }
            }
        };
    }
}
