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
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

/**
 * 阅读器固件原生 BLE 自更新客户端（0x221F / 0x331F，协议 opcode 0x50..0x54）。
 *
 * <p>用于在第三方阅读器固件运行时直接通过蓝牙升级新版本，无需切回原厂同步模式，
 * 且阅读器侧仅原地覆写当前第三方槽，完全不动官方原厂槽与分区表。
 */
@SuppressLint("MissingPermission")
final class ReaderOtaFlasher {
    private static final String TAG = "E2S-MPOTA";
    private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());

    private static final UUID SVC_MAIN = UUID.fromString("0000221f-0000-1000-8000-00805f9b34fb");
    private static final UUID CHR_MAIN = UUID.fromString("0000331f-0000-1000-8000-00805f9b34fb");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    static final int OP_INFO = 0x50;
    static final int OP_BEGIN = 0x51;
    static final int OP_DATA = 0x52;
    static final int OP_END = 0x53;
    static final int OP_ABORT = 0x54;

    static final int ST_OK = 0x00;
    static final int ST_LOW_BATT = 0x10;
    static final int ST_STAGE_DIRTY = 0x11;
    static final int ST_BAD_IMAGE = 0x12;
    static final int ST_SHA_MISMATCH = 0x13;
    static final int ST_SEQ = 0x14;
    static final int ST_FLASH_ERR = 0x15;
    static final int ST_LOCKED = 0x16;
    static final int ST_BAD_PARAM = 0xFD;
    static final int ST_BUSY = 0xFE;
    static final int ST_FAIL = 0xFF;

    private static final long IO_TIMEOUT_MS = 5_000;
    private static final long INFO_TIMEOUT_MS = 4_000;
    private static final long BEGIN_TIMEOUT_MS = 18_000;
    private static final long END_TIMEOUT_MS = 25_000;
    private static final long DIRECT_CONNECT_TIMEOUT_MS = 10_000;
    private static final long FALLBACK_CONNECT_TIMEOUT_MS = 20_000;
    private static final long REBOOT_VERIFY_TIMEOUT_MS = 50_000;
    private static final long WINDOW_WAIT_TIMEOUT_MS = 2_500;
    private static final long RECONNECT_COOLDOWN_MS = 600;

    private static volatile long lastCloseMs = 0L;

    interface Listener {
        void onStage(String title, String detail, Float progress);
    }

    static final class Result {
        final boolean verified;
        final String title;
        final String detail;

        Result(boolean verified, String title, String detail) {
            this.verified = verified;
            this.title = title;
            this.detail = detail;
        }
    }

    static final class DeviceInfo {
        final int protoVer;
        final int maxSize;
        final int runningSlot;
        final int flags;
        final int battPct;
        final byte[] appElfSha256;
        final String version;
        final String project;

        DeviceInfo(int protoVer, int maxSize, int runningSlot, int flags, int battPct,
                   byte[] appElfSha256, String version, String project) {
            this.protoVer = protoVer;
            this.maxSize = maxSize;
            this.runningSlot = runningSlot;
            this.flags = flags;
            this.battPct = battPct;
            this.appElfSha256 = appElfSha256;
            this.version = version;
            this.project = project;
        }

        boolean charging() { return (flags & (1 << 0)) != 0; }
        boolean stageOk() { return (flags & (1 << 1)) != 0; }
        boolean locked() { return (flags & (1 << 3)) != 0; }
    }

    static Result run(Context context, BluetoothDevice device, byte[] firmware,
                      byte[] expectedAppElfSha256, Listener listener) throws IOException {
        if (firmware == null || firmware.length < 0x10000) {
            throw new IOException("固件体积过小，不是有效的阅读器镜像");
        }
        byte[] fullSha256 = sha256(firmware);

        listener.onStage("连接阅读器…", "", null);
        Session session = openSession(context, device, false, DIRECT_CONNECT_TIMEOUT_MS, FALLBACK_CONNECT_TIMEOUT_MS);
        boolean transferStarted = false;
        boolean verifiedApplied = false;
        try {
            listener.onStage("检查设备…", "", null);
            DeviceInfo info = queryInfo(session);
            if (info.locked()) {
                throw new IOException("阅读器已锁屏，请先解锁");
            }
            if (!info.stageOk()) {
                throw new IOException("暂存区未就绪，无法更新");
            }
            if (firmware.length > info.maxSize) {
                throw new IOException("固件过大（上限 " + (info.maxSize / 1024) + " KB）");
            }
            if (!info.charging() && info.battPct != 0xFF && info.battPct < 20) {
                throw new IOException("电量不足（" + info.battPct + "%），需 ≥20% 或充电中");
            }

            listener.onStage("准备写入…", "", 0f);
            int[] params = sendBegin(session, firmware.length, fullSha256);
            int chunkMax = Math.max(16, Math.min(params[0], Math.max(16, session.mtu - 7)));
            int windowBytes = Math.max(512, Math.min(params[1], 8192));
            transferStarted = true;

            streamData(session, firmware, chunkMax, windowBytes, listener);

            listener.onStage("校验固件…", "", 1f);
            sendEnd(session);
            verifiedApplied = true;
        } catch (IOException e) {
            if (transferStarted && !verifiedApplied) {
                try {
                    session.writePacket(new byte[]{(byte) OP_ABORT},
                            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
                } catch (IOException ignored) {
                    // 尽力取消，连接可能已断开
                }
            }
            throw e;
        } finally {
            session.close();
        }

        // 设备端已校验完毕，正在纯 IRAM 覆写当前第三方槽（约 4~6 秒）并重启。
        listener.onStage("写入并重启…", "请勿断电", null);
        sleepInterruptedly(5_000);

        listener.onStage("验证新固件…", "", null);
        try {
            Session verifySession = openSession(context, device, true, REBOOT_VERIFY_TIMEOUT_MS, 0);
            try {
                DeviceInfo after = queryInfo(verifySession);
                if (expectedAppElfSha256 != null && after.appElfSha256 != null) {
                    if (Arrays.equals(expectedAppElfSha256, after.appElfSha256)) {
                        String ver = after.version.isEmpty() ? "新版本" : "v" + after.version;
                        return new Result(true, "更新完成", ver);
                    }
                    return new Result(false, "版本未变化", "请检查电量后重试");
                }
                return new Result(true, "更新完成", "已重新上线");
            } finally {
                verifySession.close();
            }
        } catch (InterruptedIOException e) {
            throw e;
        } catch (IOException e) {
            return new Result(false, "已写入，未确认回连", "等待阅读器重启完成即可");
        }
    }

    private static DeviceInfo queryInfo(Session session) throws IOException {
        session.clearNotifications();
        session.writePacket(new byte[]{(byte) OP_INFO}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        byte[] rsp = session.awaitNotify(OP_INFO, INFO_TIMEOUT_MS);
        if (rsp == null) {
            throw new IOException("当前阅读器固件暂不支持蓝牙直刷，请先在原厂同步模式更新一次本版本");
        }
        if (rsp.length < 10 || (rsp[1] & 0xFF) != ST_OK) {
            int st = rsp.length >= 2 ? (rsp[1] & 0xFF) : ST_FAIL;
            throw new IOException("读取阅读器状态失败：" + statusMessage(st));
        }
        int protoVer = rsp[2] & 0xFF;
        int maxSize = ((rsp[3] & 0xFF) << 24) | ((rsp[4] & 0xFF) << 16)
                | ((rsp[5] & 0xFF) << 8) | (rsp[6] & 0xFF);
        int runningSlot = rsp[7] & 0xFF;
        int flags = rsp[8] & 0xFF;
        int battPct = rsp[9] & 0xFF;

        byte[] elfSha = null;
        String version = "";
        String project = "";
        int pos = 10;
        if (rsp.length >= pos + 32) {
            elfSha = Arrays.copyOfRange(rsp, pos, pos + 32);
            pos += 32;
            if (rsp.length > pos) {
                int vlen = rsp[pos++] & 0xFF;
                if (pos + vlen <= rsp.length) {
                    version = new String(rsp, pos, vlen, StandardCharsets.US_ASCII);
                    pos += vlen;
                }
            }
            if (rsp.length > pos) {
                int plen = rsp[pos++] & 0xFF;
                if (pos + plen <= rsp.length) {
                    project = new String(rsp, pos, plen, StandardCharsets.US_ASCII);
                }
            }
        }
        return new DeviceInfo(protoVer, maxSize, runningSlot, flags, battPct, elfSha, version, project);
    }

    private static int[] sendBegin(Session session, int totalLen, byte[] sha256) throws IOException {
        byte[] pkt = new byte[1 + 4 + 32];
        pkt[0] = (byte) OP_BEGIN;
        pkt[1] = (byte) ((totalLen >>> 24) & 0xFF);
        pkt[2] = (byte) ((totalLen >>> 16) & 0xFF);
        pkt[3] = (byte) ((totalLen >>> 8) & 0xFF);
        pkt[4] = (byte) (totalLen & 0xFF);
        System.arraycopy(sha256, 0, pkt, 5, 32);

        session.clearNotifications();
        session.writePacket(pkt, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        byte[] rsp = session.awaitNotify(OP_BEGIN, BEGIN_TIMEOUT_MS);
        if (rsp == null) {
            throw new IOException("启动更新超时，请保持蓝牙靠近重试");
        }
        int st = rsp.length >= 2 ? (rsp[1] & 0xFF) : ST_FAIL;
        if (st != ST_OK || rsp.length < 6) {
            throw new IOException("设备拒绝更新：" + statusMessage(st));
        }
        int chunkMax = ((rsp[2] & 0xFF) << 8) | (rsp[3] & 0xFF);
        int windowBytes = ((rsp[4] & 0xFF) << 8) | (rsp[5] & 0xFF);
        return new int[]{chunkMax, windowBytes};
    }

    private static void streamData(Session session, byte[] firmware, int chunkMax,
                                   int windowBytes, Listener listener) throws IOException {
        int total = firmware.length;
        int sent = 0;
        int acked = 0;
        int retries = 0;
        long lastUiMs = 0L;
        long startMs = SystemClock.elapsedRealtime();

        while (acked < total) {
            if (Thread.currentThread().isInterrupted()) {
                throw new InterruptedIOException("已取消");
            }

            // 先非阻塞抽干已到达的水位/错序通知。
            int[] stAck = drainDataNotifies(session, acked);
            if (stAck[0] == ST_SEQ) {
                retries++;
                if (retries > 24) {
                    throw new IOException("蓝牙传输连续丢包过多，请靠近设备后重试");
                }
                acked = stAck[1];
                sent = stAck[1];
                Log.w(TAG, "seq resync -> offset=" + sent + " (retry " + retries + ")");
            } else {
                acked = stAck[1];
            }

            if (acked >= total) {
                break;
            }

            if (sent < total && (sent - acked) < windowBytes) {
                int take = Math.min(chunkMax, total - sent);
                byte[] pkt = new byte[4 + take];
                pkt[0] = (byte) OP_DATA;
                pkt[1] = (byte) ((sent >>> 16) & 0xFF);
                pkt[2] = (byte) ((sent >>> 8) & 0xFF);
                pkt[3] = (byte) (sent & 0xFF);
                System.arraycopy(firmware, sent, pkt, 4, take);
                session.writePacket(pkt, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
                sent += take;

                long now = SystemClock.elapsedRealtime();
                if (now - lastUiMs >= 150L || sent == total) {
                    lastUiMs = now;
                    float elapsedSec = Math.max(0.1f, (now - startMs) / 1000f);
                    float kbPerSec = (acked / 1024f) / elapsedSec;
                    String detail = String.format("%d / %d KB (%.1f KB/s)",
                            sent / 1024, (total + 1023) / 1024, kbPerSec);
                    listener.onStage("传输固件", detail, (float) sent / total);
                }
            } else {
                // 窗口满或等待末尾水位：阻塞等待下一条 0x52 通知。
                byte[] ntf = session.awaitNotify(OP_DATA, WINDOW_WAIT_TIMEOUT_MS);
                if (ntf == null) {
                    retries++;
                    if (retries > 24) {
                        throw new IOException("传输超时，未收到设备进度确认");
                    }
                    Log.w(TAG, "window timeout at sent=" + sent + " acked=" + acked + ", resending");
                    sent = acked;
                    continue;
                }
                int[] parsed = parseDataNotify(ntf);
                if (parsed[0] == ST_OK) {
                    if (parsed[1] > acked) {
                        acked = parsed[1];
                    }
                } else if (parsed[0] == ST_SEQ) {
                    retries++;
                    if (retries > 24) {
                        throw new IOException("蓝牙传输连续丢包过多，请靠近设备后重试");
                    }
                    acked = parsed[1];
                    sent = parsed[1];
                } else {
                    throw new IOException("设备写入暂存区失败：" + statusMessage(parsed[0]));
                }
            }
        }
        listener.onStage("传输固件", String.format("%d / %d KB", (total + 1023) / 1024, (total + 1023) / 1024), 1f);
    }

    private static int[] drainDataNotifies(Session session, int curAcked) throws IOException {
        int acked = curAcked;
        while (true) {
            byte[] ntf = session.pollNotify(OP_DATA);
            if (ntf == null) {
                return new int[]{ST_OK, acked};
            }
            int[] parsed = parseDataNotify(ntf);
            if (parsed[0] == ST_OK) {
                if (parsed[1] > acked) {
                    acked = parsed[1];
                }
            } else if (parsed[0] == ST_SEQ) {
                return parsed;
            } else {
                throw new IOException("设备写入暂存区失败：" + statusMessage(parsed[0]));
            }
        }
    }

    private static int[] parseDataNotify(byte[] ntf) throws IOException {
        if (ntf.length < 6) {
            throw new IOException("设备进度通知格式异常");
        }
        int st = ntf[1] & 0xFF;
        int written = ((ntf[2] & 0xFF) << 24) | ((ntf[3] & 0xFF) << 16)
                | ((ntf[4] & 0xFF) << 8) | (ntf[5] & 0xFF);
        return new int[]{st, written};
    }

    private static void sendEnd(Session session) throws IOException {
        session.clearNotifications();
        session.writePacket(new byte[]{(byte) OP_END}, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        byte[] rsp = session.awaitNotify(OP_END, END_TIMEOUT_MS);
        if (rsp == null) {
            throw new IOException("等待固件校验结果超时");
        }
        int st = rsp.length >= 2 ? (rsp[1] & 0xFF) : ST_FAIL;
        if (st != ST_OK) {
            throw new IOException("固件校验未通过：" + statusMessage(st));
        }
    }

    private static String statusMessage(int st) {
        switch (st) {
            case ST_OK: return "成功";
            case ST_LOW_BATT: return "电量不足 20%（未充电）";
            case ST_STAGE_DIRTY: return "Flash 暂存区非空（存在未知数据）";
            case ST_BAD_IMAGE: return "镜像无效或非 miaopaper_e2_e2s 固件";
            case ST_SHA_MISMATCH: return "整包 SHA-256 校验不匹配";
            case ST_SEQ: return "数据包序号错乱";
            case ST_FLASH_ERR: return "Flash 读写错误";
            case ST_LOCKED: return "设备已锁屏，请先解锁";
            case ST_BAD_PARAM: return "参数或大小非法";
            case ST_BUSY: return "设备忙（有其他传输进行中）";
            default: return String.format("错误码 0x%02X", st & 0xFF);
        }
    }

    private static byte[] sha256(byte[] data) throws IOException {
        try {
            return MessageDigest.getInstance("SHA-256").digest(data);
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("系统缺少 SHA-256 算法", e);
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

    private static Session openSession(Context context, BluetoothDevice device,
                                       boolean autoOnly, long primaryTimeoutMs,
                                       long fallbackTimeoutMs) throws IOException {
        awaitCooldown();
        if (autoOnly) {
            return openSingle(context, device, true, primaryTimeoutMs);
        }
        try {
            return openSingle(context, device, false, primaryTimeoutMs);
        } catch (InterruptedIOException e) {
            throw e;
        } catch (IOException first) {
            if (fallbackTimeoutMs <= 0 || Thread.currentThread().isInterrupted()) {
                throw first;
            }
            Log.w(TAG, "直连阅读器失败，改用 autoConnect 重试: " + first.getMessage());
            sleepInterruptedly(350L);
            awaitCooldown();
            return openSingle(context, device, true, fallbackTimeoutMs);
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
                    throw new IOException("未找到阅读器 BLE 主服务 (0x221F)");
                }
                mainChr = svc.getCharacteristic(CHR_MAIN);
                if (mainChr == null) {
                    throw new IOException("未找到阅读器数据特征 (0x331F)");
                }
                BluetoothGattDescriptor cccd = mainChr.getDescriptor(CCCD);
                if (cccd == null) {
                    throw new IOException("缺少 0x331F 通知描述符");
                }
                if (!gatt.setCharacteristicNotification(mainChr, true)) {
                    throw new IOException("无法启用 0x331F 通知");
                }
                try {
                    perform(Stage.MTU, IO_TIMEOUT_MS, () -> gatt.requestMtu(247));
                } catch (IOException e) {
                    Log.w(TAG, "requestMtu failed, using mtu=" + mtu + ": " + e.getMessage());
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
                            fail(new IOException("无法启动 BLE 连接"));
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
                        throw new IOException("发起蓝牙连接超时");
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("已取消");
                }
            }
            if (err.get() != null) {
                throw err.get();
            }
            synchronized (this) {
                awaitStage(Stage.CONNECT);
            }
        }

        synchronized void writePacket(byte[] payload, int writeType) throws IOException {
            perform(Stage.WRITE, IO_TIMEOUT_MS, () -> writeChr(payload, writeType));
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
                if (hit != null) {
                    return hit;
                }
                long rem = end - SystemClock.elapsedRealtime();
                if (rem <= 0) {
                    return null;
                }
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
                throw new IOException("BLE 操作发起失败：" + stage.name());
            }
            awaitStage(stage);
        }

        private void awaitStage(Stage stage) throws IOException {
            while (pending == stage) {
                checkLive();
                long rem = deadline - SystemClock.elapsedRealtime();
                if (rem <= 0) {
                    pending = null;
                    throw new IOException("BLE 操作超时：" + stage.name());
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
                fail(new IOException("GATT 错误 " + status + " (" + stage.name() + ")"));
                return;
            }
            notifyAll();
        }

        private void fail(IOException e) {
            if (failure == null) {
                failure = e;
            }
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
        private boolean writeChr(byte[] payload, int writeType) {
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeCharacteristic(mainChr, payload, writeType) == BluetoothStatusCodes.SUCCESS;
            }
            mainChr.setWriteType(writeType);
            mainChr.setValue(payload);
            return gatt.writeCharacteristic(mainChr);
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
                            fail(new IOException("阅读器蓝牙连接已断开（状态 " + status + "）"));
                        } else {
                            notifyAll();
                        }
                    } else if (status != BluetoothGatt.GATT_SUCCESS) {
                        fail(new IOException("蓝牙连接失败（状态 " + status + "）"));
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
                if (value != null) {
                    pushNotify(value);
                }
            }

            @SuppressWarnings("deprecation")
            @Override
            public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
                if (Build.VERSION.SDK_INT < 33) {
                    byte[] val = c.getValue();
                    if (val != null) {
                        pushNotify(val);
                    }
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
