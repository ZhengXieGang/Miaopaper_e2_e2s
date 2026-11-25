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
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** 单次 GATT 会话封装：工作线程阻塞等待每个阶段，Android BLE 回调固定投递到主线程。 */
@SuppressLint("MissingPermission")
final class A5Flasher implements A5Protocol.Session {
    private static final UUID SERVICE = uuid("ff00");
    private static final UUID DATA = uuid("ff01");
    private static final UUID WRITE = uuid("ff02");
    private static final UUID CONTROL = uuid("ff03");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final String TAG = "E2S";
    private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());

    private static final long IO_TIMEOUT_MS = 5_000;
    private static final long CONNECT_TIMEOUT_MS = 120_000;        // 重启验证连接：等待设备重启后恢复广播
    private static final long DIRECT_ATTEMPT_TIMEOUT_MS = 10_000;  // 首次直连：设备已在广播时应快速建链
    private static final long FALLBACK_CONNECT_TIMEOUT_MS = 20_000;// 回退连接：改用 autoConnect=true 捕获后续广播
    private static final long CLOSE_DISCONNECT_WAIT_MS = 600;      // close 前等待对端断连 ACK 上限
    private static final long RECONNECT_COOLDOWN_MS = 700;         // 连续关闭/重连间最小冷却间隔
    /* Stock 1.3.739 announces FF03 about 1 s after connect, before Android can subscribe; it does not repeat. */
    private static final long NOTICE_GRACE_MS = 3_000;

    private static volatile long lastCloseMs = 0L;

    private enum Stage {
        CONNECT("连接 GATT"), DISCOVER("发现服务"), MTU("协商 MTU"),
        SUBSCRIBE_DATA("写入 FF01 CCCD"), SUBSCRIBE_CONTROL("写入 FF03 CCCD"), WRITE("写入 FF02");

        final String label;
        Stage(String label) { this.label = label; }
    }

    private final String address;
    private final A5Protocol.Exchange exchange = new A5Protocol.Exchange(this::send);

    // 下列可变状态均由 this 监视器保护。
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic data, write, control;
    private BluetoothGattDescriptor dataCccd, controlCccd;
    private Stage pending;
    private long pendingDeadline, controlDeadline;
    private IOException failure;
    private boolean ready, closed, typeNotice, connected, disconnectedAck;
    private int mtu = 23;
    private int peerSegment;

    private A5Flasher(String address) { this.address = address; }

    static A5Flasher open(Context context, BluetoothDevice device, Consumer<String> status) throws IOException {
        return open(context, device, status, false);
    }

    static A5Flasher open(Context context, BluetoothDevice device, Consumer<String> status,
                          boolean background) throws IOException {
        if (background) {
            awaitCooldown();
            return openSingle(context, device, status, true, CONNECT_TIMEOUT_MS, true);
        }
        awaitCooldown();
        try {
            // 第一段：优先直连，广播活跃期通常 1 秒内建链
            return openSingle(context, device, status, false, DIRECT_ATTEMPT_TIMEOUT_MS, false);
        } catch (InterruptedIOException e) {
            throw e;
        } catch (IOException firstError) {
            if (Thread.currentThread().isInterrupted()) throw firstError;
            Log.w(TAG, "直连 GATT 失败，回退为 autoConnect 重试: " + firstError.getMessage());
            sleepInterruptedly(400L);
            awaitCooldown();
            // 第二段：改用 autoConnect=true，兼容刚断开尚未重启广播或用户稍后切入同步模式的场景
            return openSingle(context, device, status, true, FALLBACK_CONNECT_TIMEOUT_MS, false);
        }
    }

    private static A5Flasher openSingle(Context context, BluetoothDevice device, Consumer<String> status,
                                        boolean autoConnect, long timeoutMs,
                                        boolean background) throws IOException {
        A5Flasher session = new A5Flasher(device.getAddress());
        boolean opened = false;
        try {
            session.connect(context, device, status, autoConnect, timeoutMs, background);
            opened = true;
            return session;
        } catch (RuntimeException e) {
            throw new IOException("蓝牙连接或初始化失败", e);
        } finally {
            if (!opened) session.close();
        }
    }

    private static void awaitCooldown() throws InterruptedIOException {
        long closedAt = lastCloseMs;
        if (closedAt == 0L) return;
        long elapsed = SystemClock.elapsedRealtime() - closedAt;
        long remain = RECONNECT_COOLDOWN_MS - elapsed;
        if (remain > 0L) sleepInterruptedly(remain);
    }

    private static void sleepInterruptedly(long ms) throws InterruptedIOException {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("蓝牙操作已取消");
        }
    }

    @Override public String address() { return address; }
    @Override public A5Protocol.Exchange exchange() { return exchange; }

    private void connect(Context context, BluetoothDevice device, Consumer<String> status,
                         boolean autoConnect, long timeoutMs, boolean background) throws IOException {
        openGattOnMain(context, device, autoConnect, timeoutMs, background);
        synchronized (this) {
            perform(Stage.DISCOVER, 8_000, () -> gatt.discoverServices());

            BluetoothGattService service = gatt.getService(SERVICE);
            if (service == null) throw new IOException("未找到 A5 服务");
            data = service.getCharacteristic(DATA);
            write = service.getCharacteristic(WRITE);
            control = service.getCharacteristic(CONTROL);
            if (!hasProperty(data, BluetoothGattCharacteristic.PROPERTY_NOTIFY)
                    || !hasProperty(control, BluetoothGattCharacteristic.PROPERTY_NOTIFY)
                    || !hasProperty(write, BluetoothGattCharacteristic.PROPERTY_WRITE)) {
                throw new IOException("缺少 FF01/FF03 通知或 FF02 有响应写入能力");
            }
            dataCccd = data.getDescriptor(CCCD);
            controlCccd = control.getDescriptor(CCCD);
            if (dataCccd == null || controlCccd == null) throw new IOException("缺少通知 CCCD");

            checkLive();
            // 先在本地注册两个特征通知，避免早于 onDescriptorWrite 到达的 FF03 就绪通知丢失。
            if (!gatt.setCharacteristicNotification(data, true)
                    || !gatt.setCharacteristicNotification(control, true)) {
                throw new IOException("无法注册 FF01/FF03 通知");
            }
            perform(Stage.MTU, IO_TIMEOUT_MS, () -> gatt.requestMtu(185));
            perform(Stage.SUBSCRIBE_DATA, IO_TIMEOUT_MS, () -> enableCccd(dataCccd));
            perform(Stage.SUBSCRIBE_CONTROL, IO_TIMEOUT_MS, () -> enableCccd(controlCccd));
            controlDeadline = SystemClock.elapsedRealtime() + NOTICE_GRACE_MS;
            if (!typeNotice || peerSegment == 0) status.accept("等待 FF03 就绪通知（最多 3 秒）…");
            while (!typeNotice || peerSegment == 0) {
                checkLive();
                long remaining = controlDeadline - SystemClock.elapsedRealtime();
                if (remaining <= 0) break;
                try {
                    wait(remaining);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("蓝牙操作已取消");
                }
            }
            if (peerSegment == 0) {
                // 原厂固件可能在订阅 CCCD 生效前就发出单次 FF03，此时按协商 MTU 作为分片继续只读查询。
                peerSegment = mtu - 3;
                status.accept("未收到 FF03 就绪通知，按协商 MTU 继续（只读检查）。");
            }
            checkLive();
            ready = true;
            Log.i(TAG, "ready: mtu=" + mtu + " segment=" + peerSegment + " ff03=" + typeNotice);
        }
    }

    /** 在不持有 this 锁的情况下切回主线程发起 4 参数 connectGatt，防止回调抢锁或 ROM 丢回调。 */
    private void openGattOnMain(Context context, BluetoothDevice device, boolean autoConnect,
                                long timeoutMs, boolean background) throws IOException {
        synchronized (this) {
            checkLive();
            pending = Stage.CONNECT;
            pendingDeadline = SystemClock.elapsedRealtime() + timeoutMs;
        }
        CountDownLatch latch = new CountDownLatch(1);
        AtomicReference<RuntimeException> postError = new AtomicReference<>();
        Runnable task = () -> {
            try {
                synchronized (A5Flasher.this) {
                    if (closed || failure != null) return;
                    BluetoothGatt client = device.connectGatt(
                            context, autoConnect, callback, BluetoothDevice.TRANSPORT_LE);
                    gatt = client;
                    if (client == null) {
                        fail(new IOException("连接 GATT 未能启动"));
                    }
                }
            } catch (RuntimeException e) {
                postError.set(e);
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
                    throw new IOException("启动 GATT 连接等待主线程超时");
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new InterruptedIOException("蓝牙操作已取消");
            }
        }
        if (postError.get() != null) {
            throw postError.get();
        }
        synchronized (this) {
            try {
                awaitStage(Stage.CONNECT);
            } catch (IOException e) {
                if (e instanceof InterruptedIOException) throw e;
                String msg = e.getMessage();
                if (msg != null && msg.contains("超时")) {
                    throw new IOException(background ? "等待设备重连超时"
                            : "连接 GATT 超时，请确认设备已打开同步模式", e);
                }
                throw e;
            }
        }
    }

    private static UUID uuid(String shortId) {
        return UUID.fromString("0000" + shortId + "-c243-40b9-ac41-a10ba3ad5d1a");
    }

    private static boolean hasProperty(BluetoothGattCharacteristic c, int property) {
        return c != null && (c.getProperties() & property) != 0;
    }

    @SuppressWarnings("deprecation")
    private boolean enableCccd(BluetoothGattDescriptor descriptor) {
        byte[] value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
        if (Build.VERSION.SDK_INT >= 33) {
            return gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS;
        }
        return descriptor.setValue(value) && gatt.writeDescriptor(descriptor);
    }

    @SuppressWarnings("deprecation")
    private boolean writePart(byte[] value) {
        // The official app uses write-without-response on Android 7+ (setWriteType(1)).
        int type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE;
        if (Build.VERSION.SDK_INT >= 33) {
            return gatt.writeCharacteristic(write, value, type) == BluetoothStatusCodes.SUCCESS;
        }
        write.setWriteType(type);
        return write.setValue(value) && gatt.writeCharacteristic(write);
    }

    private synchronized void send(byte[] frame) throws IOException {
        boolean enqueued = false;
        try {
            checkLive();
            if (!ready || pending != null) throw new IOException("GATT 尚未就绪或已有操作");
            Log.i(TAG, "FF02 send " + frame.length + " B");
            int offset = 0;
            while (offset < frame.length) {
                checkLive();
                int max = Math.min(peerSegment, Math.min(mtu - 3, 512));
                int end = Math.min(offset + max, frame.length);
                byte[] part = Arrays.copyOfRange(frame, offset, end);
                pending = Stage.WRITE;
                pendingDeadline = SystemClock.elapsedRealtime() + IO_TIMEOUT_MS;
                if (!writePart(part)) throw new IOException("FF02 写入未进入 GATT 队列");
                enqueued = true;
                awaitStage(Stage.WRITE);
                offset = end;
            }
        } catch (IOException e) {
            fail(e);
            if (!enqueued) throw new A5Protocol.NotSentException(e.getMessage(), e);
            throw e;
        } catch (RuntimeException e) {
            IOException error = new IOException("FF02 写入出错", e);
            fail(error);
            throw error; // A platform exception is not proof that the write was not sent.
        }
    }

    // The caller holds the monitor; only one operation is ever in flight.
    private void perform(Stage stage, long timeoutMs, BooleanSupplier begin) throws IOException {
        checkLive();
        pending = stage;
        pendingDeadline = SystemClock.elapsedRealtime() + timeoutMs;
        if (!begin.getAsBoolean()) throw new IOException(stage.label + "未能启动");
        awaitStage(stage);
    }

    private void awaitStage(Stage stage) throws IOException {
        while (pending == stage) awaitChange(pendingDeadline, stage.label);
        checkLive();
    }

    private void awaitChange(long deadline, String label) throws IOException {
        checkLive();
        long remaining = deadline - SystemClock.elapsedRealtime();
        if (remaining <= 0) throw new IOException(label + "超时");
        try {
            wait(remaining);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new InterruptedIOException("蓝牙操作已取消");
        }
    }

    private void checkLive() throws IOException {
        if (failure != null) throw failure;
        if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException("蓝牙操作已取消");
    }

    private boolean isCurrent(BluetoothGatt client) {
        return !closed && failure == null && client == gatt;
    }

    private void fail(IOException error) {
        if (failure == null) {
            failure = error;
            Log.w(TAG, "session failed: " + error.getMessage());
        }
        exchange.fail(failure);
        notifyAll();
    }

    private void complete(Stage stage, int status) {
        if (pending != stage) return;
        if (SystemClock.elapsedRealtime() >= pendingDeadline) {
            fail(new IOException(stage.label + "超时"));
            return;
        }
        if (status != BluetoothGatt.GATT_SUCCESS) {
            fail(new IOException(stage.label + "失败（GATT " + status + "）"));
            return;
        }
        pending = null;
        notifyAll();
    }

    private void receiveControl(byte[] value) {
        if (value.length == 0 || (controlDeadline != 0 && !ready
                && SystemClock.elapsedRealtime() >= controlDeadline)) return;
        if (value[0] == 1) {
            if (value.length != 2 || value[1] != 3) {
                fail(new IOException("FF03 设备类型通知不符合 E2 / E2S"));
                return;
            }
            typeNotice = true;
        } else if (value[0] == 2) {
            int segment = value.length == 3 ? (value[1] & 0xff) | ((value[2] & 0xff) << 8) : 0;
            if (segment < 20 || segment > 514) {
                fail(new IOException("FF03 分段长度通知无效"));
                return;
            }
            peerSegment = segment;
        }
        notifyAll();
    }

    private void receive(BluetoothGatt client, BluetoothGattCharacteristic characteristic, byte[] value) {
        synchronized (this) {
            if (!isCurrent(client) || value == null) return;
            if (characteristic == control) {
                Log.i(TAG, "FF03 " + value.length + " B: " + hex(value));
                receiveControl(value);
            } else if (characteristic == data && ready) {
                Log.i(TAG, "FF01 " + value.length + " B: " + hex(value));
                exchange.onBytes(value);
            }
        }
    }

    private static String hex(byte[] data) {
        char[] out = new char[data.length * 2];
        for (int i = 0; i < data.length; i++) {
            out[2 * i] = "0123456789abcdef".charAt((data[i] >> 4) & 15);
            out[2 * i + 1] = "0123456789abcdef".charAt(data[i] & 15);
        }
        return new String(out);
    }

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt client, int status, int state) {
            synchronized (A5Flasher.this) {
                // 必须在 isCurrent(client) 之前记录断开事件，以便 close() 等待对端断开确认时收到通知。
                if (client == gatt && state == BluetoothProfile.STATE_DISCONNECTED) {
                    connected = false;
                    disconnectedAck = true;
                    A5Flasher.this.notifyAll();
                }
                if (!isCurrent(client)) return;
                if (status != BluetoothGatt.GATT_SUCCESS || state == BluetoothProfile.STATE_DISCONNECTED) {
                    fail(new IOException("连接断开或失败（GATT " + status + "）"));
                } else if (state == BluetoothProfile.STATE_CONNECTED) {
                    connected = true;
                    complete(Stage.CONNECT, status);
                }
            }
        }

        @Override public void onServicesDiscovered(BluetoothGatt client, int status) {
            synchronized (A5Flasher.this) {
                if (isCurrent(client)) complete(Stage.DISCOVER, status);
            }
        }

        @Override public void onMtuChanged(BluetoothGatt client, int value, int status) {
            synchronized (A5Flasher.this) {
                if (!isCurrent(client)) return;
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    if (value < 23 || value > 517) {
                        fail(new IOException("GATT 协商的 MTU 无效"));
                        return;
                    }
                    mtu = value;
                }
                complete(Stage.MTU, status);
            }
        }

        @Override public void onDescriptorWrite(BluetoothGatt client, BluetoothGattDescriptor descriptor,
                                               int status) {
            synchronized (A5Flasher.this) {
                if (!isCurrent(client)) return;
                if (descriptor == dataCccd) complete(Stage.SUBSCRIBE_DATA, status);
                else if (descriptor == controlCccd) complete(Stage.SUBSCRIBE_CONTROL, status);
            }
        }

        @Override public void onCharacteristicWrite(BluetoothGatt client, BluetoothGattCharacteristic c,
                                                     int status) {
            synchronized (A5Flasher.this) {
                if (isCurrent(client) && c == write) complete(Stage.WRITE, status);
            }
        }

        @Override public void onCharacteristicChanged(BluetoothGatt client, BluetoothGattCharacteristic c,
                                                       byte[] value) {
            receive(client, c, value);
        }

        @Override @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt client, BluetoothGattCharacteristic c) {
            receive(client, c, c.getValue()); // Android < 33
        }

        @Override public void onServiceChanged(BluetoothGatt client) {
            synchronized (A5Flasher.this) {
                if (isCurrent(client)) fail(new IOException("GATT 服务已改变；本次操作停止"));
            }
        }
    };

    private static void runOnMainSync(Runnable action) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            try { action.run(); } catch (RuntimeException ignored) { }
            return;
        }
        CountDownLatch latch = new CountDownLatch(1);
        if (MAIN_HANDLER.post(() -> {
            try {
                action.run();
            } catch (RuntimeException ignored) {
            } finally {
                latch.countDown();
            }
        })) {
            try {
                latch.await(CLOSE_DISCONNECT_WAIT_MS, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    @Override public void close() {
        BluetoothGatt client;
        boolean needDisconnect;
        synchronized (this) {
            if (closed) return;
            closed = true;
            ready = false;
            needDisconnect = connected;
            // 正常关闭必须唤醒所有等待者，但不应作为异常日志记录。
            if (failure == null) failure = new IOException("GATT 会话已关闭");
            exchange.fail(failure);
            notifyAll();
            client = gatt;
        }
        if (client != null) {
            if (needDisconnect) {
                // 不在持有 this 锁期间向主线程投递并等待断开完成，避免回调进锁引发死锁。
                runOnMainSync(client::disconnect);
                boolean savedInterrupt = Thread.interrupted();
                long deadline = SystemClock.elapsedRealtime() + CLOSE_DISCONNECT_WAIT_MS;
                synchronized (this) {
                    while (!disconnectedAck) {
                        long remain = deadline - SystemClock.elapsedRealtime();
                        if (remain <= 0L) break;
                        try {
                            wait(remain);
                        } catch (InterruptedException e) {
                            savedInterrupt = true;
                            break;
                        }
                    }
                }
                if (savedInterrupt) Thread.currentThread().interrupt();
            }
            runOnMainSync(client::close);
            lastCloseMs = SystemClock.elapsedRealtime();
        }
        synchronized (this) {
            gatt = null;
        }
    }
}
