package com.zxg.miaopaperflash;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;

/** A5 wire format and the two gated, transport-independent E2S operations. */
public final class A5Protocol {
    /** 原厂版本形如 1.3.739；自定义固件用其他版本号即可被识别为非原厂。 */
    public static boolean isStockVersion(String version) {
        return version != null && version.matches("\\d+\\.\\d+\\.\\d+");
    }

    private static final int MAX_PAYLOAD = 4114; // Device maxLen; largest file chunk frame is 4114 bytes.
    private static final int MAX_IMAGE = 0x1f0000; // Conservative size for the inactive OTA slot.
    private static final int REQUEST = 1;
    private static final int RESPONSE = 2;
    private static final int SYSTEM = 1;
    private static final int SERIAL = 2, VERSION = 7, MODEL = 8, SELECT = 19;
    private static final int HANDSHAKE = 23, ROUTE = 240;
    private static final byte[] EMPTY = new byte[0];
    private static final SecureRandom RANDOM = new SecureRandom();

    private A5Protocol() {}

    /** Encodes an A5 command/response including the payload CRC32 and frame terminator. */
    public static byte[] frame(int parent, int child, int type, byte[] content) {
        if ((parent & ~0xff) != 0 || (child & ~0xff) != 0 || (type & ~0xff) != 0
                || content.length > MAX_PAYLOAD - 5) {
            throw new IllegalArgumentException("Invalid A5 frame fields");
        }
        int payloadLength = 5 + content.length;
        byte[] out = new byte[payloadLength + 9];
        out[0] = (byte) 0xa5;
        out[1] = 1;
        put16(out, 2, payloadLength);
        out[4] = (byte) parent;
        out[5] = (byte) child;
        out[6] = (byte) type;
        put16(out, 7, content.length);
        System.arraycopy(content, 0, out, 9, content.length);
        long crc = crc32(out, 4, payloadLength);
        for (int i = 0; i < 4; i++) out[4 + payloadLength + i] = (byte) (crc >>> (8 * i));
        out[out.length - 1] = 0x5a;
        return out;
    }

    private static int u8(byte b) { return b & 0xff; }
    private static int le16(byte[] b, int off) { return u8(b[off]) | (u8(b[off + 1]) << 8); }
    private static void put16(byte[] b, int off, int n) {
        b[off] = (byte) n;
        b[off + 1] = (byte) (n >>> 8);
    }
    private static long le32(byte[] b, int off) {
        return le16(b, off) | ((long) le16(b, off + 2) << 16);
    }
    private static long crc32(byte[] b, int off, int len) {
        // OEM CRC32Util.crc32_standard: the standard reflected CRC-32 with initial value
        // 896963873 (0x35769521), not zlib's default 0xffffffff.
        long crc = 896963873L ^ 0xffffffffL;
        for (int i = off; i < off + len; i++) {
            crc ^= b[i] & 0xff;
            for (int k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xedb88320L & -(crc & 1));
        }
        return (crc ^ 0xffffffffL) & 0xffffffffL;
    }

    @FunctionalInterface
    public interface Sender {
        /** Returns after the whole frame was written, or throws. Notifications may arrive inside send. */
        void send(byte[] frame) throws IOException;
    }

    /** The transport can prove no byte of this request was enqueued. */
    public static final class NotSentException extends IOException {
        private static final long serialVersionUID = 1L;

        public NotSentException(String message) { super(message); }
        public NotSentException(String message, Throwable cause) { super(message, cause); }
    }

    /** One in-flight request per connection. Feed its FF01 bytes via onBytes. */
    public static final class Exchange {
        private final Sender sender;
        private final byte[] rx = new byte[MAX_PAYLOAD + 9];
        private int used;
        private Pending pending;
        private IOException failure;

        public Exchange(Sender sender) { this.sender = sender; }

        public byte[] request(int parent, int child, byte[] content, long timeoutMs) throws IOException {
            if (timeoutMs <= 0) throw new IllegalArgumentException("Expected a positive response timeout");
            byte[] out = frame(parent, child, REQUEST, content);
            Pending request = new Pending(parent, child);
            synchronized (this) {
                if (pending != null) throw new NotSentException("已有 A5 请求在等待应答");
                if (failure != null) throw new NotSentException(failure.getMessage(), failure);
                // Arm before transmitting: the response may even precede the final GATT write callback.
                pending = request;
                used = 0;
            }
            try {
                sender.send(out);
                long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
                synchronized (this) {
                    while (request.content == null) {
                        if (failure != null) throw failure;
                        long remaining = deadline - System.nanoTime();
                        if (remaining <= 0) throw new IOException("等待 A5 (" + parent + "," + child + ") 应答超时");
                        try {
                            TimeUnit.NANOSECONDS.timedWait(this, remaining);
                        } catch (InterruptedException e) {
                            Thread.currentThread().interrupt();
                            throw new InterruptedIOException("等待 A5 应答已取消");
                        }
                    }
                    return request.content;
                }
            } catch (IOException e) {
                // A5 has no request sequence number. Do not reuse a link after a failed request.
                fail(e);
                throw e;
            } finally {
                synchronized (this) {
                    pending = null;
                    used = 0; // Never carry an old, partial response into the next command.
                }
            }
        }

        public synchronized void fail(IOException cause) {
            if (failure == null) failure = cause;
            notifyAll();
        }

        public synchronized void onBytes(byte[] fragment) {
            if (pending == null || pending.content != null || failure != null) return;
            for (byte b : fragment) {
                rx[used++] = b;
                while (used > 0) {
                    int discard = 1;
                    if (u8(rx[0]) == 0xa5) {
                        if (used < 2) break;
                        if (rx[1] == 1) {
                            if (used < 4) break;
                            int payloadLength = le16(rx, 2);
                            if (payloadLength >= 5 && payloadLength <= MAX_PAYLOAD) {
                                int total = payloadLength + 9;
                                if (used < total) break;
                                if (rx[total - 1] == 0x5a
                                        && crc32(rx, 4, payloadLength) == le32(rx, 4 + payloadLength)) {
                                    if (le16(rx, 7) == payloadLength - 5 && rx[6] == RESPONSE
                                            && u8(rx[4]) == pending.parent && u8(rx[5]) == pending.child) {
                                        pending.content = Arrays.copyOfRange(rx, 9, 4 + payloadLength);
                                        used = 0;
                                        notifyAll();
                                        return;
                                    }
                                    discard = total;
                                }
                            }
                        }
                    }
                    used -= discard;
                    System.arraycopy(rx, discard, rx, 0, used);
                }
            }
        }

        private static final class Pending {
            final int parent, child;
            byte[] content;

            Pending(int parent, int child) { this.parent = parent; this.child = child; }
        }
    }

    public enum Operation { CHECK_TEST, RESTORE }

    public enum Outcome {
        TEST_INSTALL_BLOCKED, ALREADY_STOCK, RESTORE_BLOCKED, REFUSED, COMMIT_UNKNOWN,
        SELECTION_CONFIRMED_BOOT_UNVERIFIED, STOCK_BOOTED, FAILED,
        FLASH_BLOCKED, FLASH_REFUSED, FLASH_FAILED,
        FLASHED_BOOTED, FLASHED_BOOT_UNVERIFIED
    }

    public static final class Result {
        public final Outcome outcome;
        public final String detail;
        public final boolean stockObserved;

        private Result(Outcome outcome, String detail) { this(outcome, detail, false); }

        private Result(Outcome outcome, String detail, boolean stockObserved) {
            this.outcome = outcome;
            this.detail = detail;
            this.stockObserved = stockObserved;
        }
    }

    public interface Session extends AutoCloseable {
        String address();
        Exchange exchange();
        @Override void close();
    }

    @FunctionalInterface
    public interface Connector {
        /** A fresh ready session. A verification open follows confirmed selection and must be bounded. */
        Session open(boolean verification) throws IOException;
    }

    @FunctionalInterface
    public interface CommitCheck {
        /** Records the pending selection before any write; failure must stop the request. */
        void beforeSelection() throws IOException;
    }

    @FunctionalInterface
    public interface Progress {
        /** Called after chunk {@code done} of {@code total} is acknowledged. */
        void onChunk(int done, int total);
    }

    /** The only state-changing request is one empty (1,19), gated by test-firmware identity and route 1. */
    public static Result run(Operation operation, Connector connector, CommitCheck commitCheck) {
        try (Session session = connector.open(false)) {
            String version = inspect(session);
            if (operation == Operation.CHECK_TEST) {
                String diag = version.startsWith("hello-paperang-")
                        ? " · " + diagText(session.exchange()) : "";
                return new Result(Outcome.TEST_INSTALL_BLOCKED, version + diag,
                        isStockVersion(version));
            }
            if (isStockVersion(version)) {
                return new Result(Outcome.ALREADY_STOCK, "原厂 " + version, true);
            }

            Exchange exchange = session.exchange();
            byte[] route;
            try {
                route = query(exchange, ROUTE);
            } catch (IOException e) {
                return new Result(Outcome.RESTORE_BLOCKED, "固件不支持切回原厂");
            }
            if (route.length != 1) throw new IOException("路由长度错误");
            if (route[0] != 1) {
                return new Result(Outcome.RESTORE_BLOCKED, "设备未确认可切换");
            }

            commitCheck.beforeSelection();
            Reply selection;
            try {
                selection = reply(exchange.request(SYSTEM, SELECT, EMPTY, 10_000));
                if (selection.data.length != 0) throw new IOException("选择应答应为空");
            } catch (NotSentException e) {
                return new Result(Outcome.FAILED, "未发出切换指令");
            } catch (IOException e) {
                return new Result(Outcome.COMMIT_UNKNOWN, "切换结果未知");
            }
            if (selection.status == 2) {
                return new Result(Outcome.REFUSED, "设备拒绝切换");
            }
            if (selection.status != 1) {
                String detail = selection.status == 3 ? "设备无法确认切换"
                        : "切换状态异常 " + selection.status;
                return new Result(Outcome.COMMIT_UNKNOWN, detail);
            }
        } catch (IOException e) {
            return new Result(Outcome.FAILED, e.getMessage());
        }

        // A selection ACK says nothing about what actually booted. Use fresh read-only sessions.
        String detail = "未能重新连接";
        for (int i = 0; i < 2 && !Thread.currentThread().isInterrupted(); i++) {
            try (Session session = connector.open(true)) {
                String version = inspect(session);
                if (isStockVersion(version)) {
                    return new Result(Outcome.STOCK_BOOTED,
                            "已恢复原厂 " + version, true);
                }
                detail = "重连读到 " + version;
            } catch (IdentityException e) {
                detail = e.getMessage();
                break;
            } catch (IOException e) {
                detail = e.getMessage();
            }
        }
        return new Result(Outcome.SELECTION_CONFIRMED_BOOT_UNVERIFIED,
                "已切换，未确认启动：" + detail);
    }

    public static Result runFlash(byte[] image, String expectedSha256, Connector connector, Runnable beforeTrigger) {
        return runFlash(image, expectedSha256, connector, beforeTrigger, null);
    }

    /**
     * First flash through the stock firmware's own A5 OTA service: the device writes this image to
     * its inactive OTA slot and selects it. Never call this on a non-stock or non-target device.
     */
    public static Result runFlash(byte[] image, String expectedSha256, Connector connector,
                                  Runnable beforeTrigger, Progress progress) {
        if (image == null || image.length < 0x1000 || image.length > MAX_IMAGE
                || (image[0] & 0xff) != 0xe9 || !sha256Hex(image).equalsIgnoreCase(expectedSha256)) {
            return new Result(Outcome.FLASH_BLOCKED, "镜像校验失败");
        }
        String endNote = "";
        String stockVersion;
        try (Session session = connector.open(false)) {
            String version = inspect(session);
            if (!isStockVersion(version)) {
                return new Result(Outcome.FLASH_BLOCKED,
                        "当前 " + version + "，仅原厂固件可刷入");
            }
            stockVersion = version;
            Exchange exchange = session.exchange();
            int rawBattery = intValue(query(exchange, 11));
            int battery = rawBattery / 10;
            if (rawBattery < 0 || battery < 20) {
                return new Result(Outcome.FLASH_BLOCKED,
                        "电量 " + battery + "%，需 ≥20%");
            }
            int maxLen = intValue(query(exchange, 20));
            if (maxLen < 200 || maxLen > 5000) {
                return new Result(Outcome.FLASH_BLOCKED, "设备参数不可用");
            }
            int chunk = Math.min(maxLen - 18, 4096);
            if (chunk < 256) {
                return new Result(Outcome.FLASH_BLOCKED, "设备块长不可用");
            }
            int maxCache = intValue(query(exchange, 32));
            if (maxCache < 1) {
                return new Result(Outcome.FLASH_BLOCKED, "设备缓存不可用");
            }

            Reply ack;
            byte[] start = concat(new byte[]{1, 2, 0, 1, 0, 2, 4, 0}, le32(image.length));
            try {
                ack = reply(exchange.request(2, 1, start, 15_000));
            } catch (NotSentException e) {
                return new Result(Outcome.FLASH_FAILED, "起始指令未发出");
            } catch (IOException e) {
                return new Result(Outcome.FLASH_FAILED, "起始无应答");
            }
            if (ack.status != 1) {
                return new Result(Outcome.FLASH_REFUSED, "设备拒绝写入");
            }

            int total = (image.length + chunk - 1) / chunk;
            int index = 1, resumes = 0;
            while (index <= total) {
                int offset = (index - 1) * chunk;
                int length = Math.min(chunk, image.length - offset);
                byte[] content = concat(le16(index), le16(length),
                        Arrays.copyOfRange(image, offset, offset + length));
                try {
                    ack = reply(exchange.request(2, 3, content, 20_000));
                } catch (NotSentException e) {
                    return new Result(Outcome.FLASH_FAILED, "块 " + index + "/" + total + " 未发出");
                } catch (IOException e) {
                    return new Result(Outcome.FLASH_FAILED, "块 " + index + "/" + total + " 无应答");
                }
                if (ack.status == 1) {
                    index++;
                    if (progress != null) progress.onChunk(index - 1, total);
                    continue;
                }
                int resume = ack.data.length >= 4 ? (int) le32(ack.data, 0) : -1;
                if (resume >= 1 && resume <= total && ++resumes <= 8) {
                    index = resume;
                    continue;
                }
                return new Result(Outcome.FLASH_FAILED,
                        "块 " + index + "/" + total + " 被拒");
            }

            // The stock's (2,2) handler closes the cache file, reads it back in full, and only then
            // posts the reply as a data_sync event with a zero-timeout enqueue. When that queue is
            // busy the reply is silently dropped even though the file is already complete. Treat a
            // missing reply as "unconfirmed", not as failure, and fall through to the OTA trigger.
            try {
                ack = reply(exchange.request(2, 2, EMPTY, 20_000));
                if (ack.status != 1) {
                    endNote = "结束状态 " + ack.status + "；";
                }
            } catch (IOException e) {
                endNote = "结束应答丢失；";
            }

        } catch (IOException e) {
            return new Result(Outcome.FLASH_FAILED, e.getMessage());
        }

        try {
            beforeTrigger.run();
        } catch (RuntimeException e) {
            return new Result(Outcome.FLASH_FAILED, "未记录状态，未触发");
        }
        // A5 has no request id, so a timed-out (2,2) poisons the upload Exchange by design.
        // Use a fresh session and handshake for the trigger. (1,19) also goes through the stock's
        // data_sync queue, so retry: the same drop that swallowed (2,2) can swallow the trigger.
        // A duplicate trigger is harmless; esp_ota_end validates the image before the slot changes.
        IOException triggerError = null;
        boolean triggerSent = false;
        for (int attempt = 1; attempt <= 3; attempt++) {
            try (Session triggerSession = connector.open(false)) {
                handshake(triggerSession.exchange());
                triggerSession.exchange().request(1, 19, EMPTY, 8_000);
                triggerSent = true;
                triggerError = null;
                break;
            } catch (NotSentException e) {
                triggerError = e;
                break;
            } catch (IOException e) {
                triggerError = e;
                if (attempt < 3) {
                    try {
                        Thread.sleep(2_000);
                    } catch (InterruptedException ie) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                }
            }
        }
        if (!triggerSent && triggerError instanceof NotSentException) {
            return new Result(Outcome.FLASH_FAILED, endNote + "触发未发出");
        }

        String detail = "未能重新连接";
        for (int i = 0; i < 3 && !Thread.currentThread().isInterrupted(); i++) {
            try (Session session = connector.open(true)) {
                String version = inspect(session);
                if (!stockVersion.equals(version)) {
                    return new Result(Outcome.FLASHED_BOOTED, "已启动 " + version);
                }
                detail = "仍是原厂 " + version;
                break;
            } catch (IOException e) {
                detail = e.getMessage();
            }
            try {
                Thread.sleep(2_000);
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
            }
        }
        return new Result(Outcome.FLASHED_BOOT_UNVERIFIED,
                "已触发，未确认启动：" + detail);
    }


    private static int intValue(byte[] data) {
        if (data.length >= 4) return (int) le32(data, 0);
        if (data.length >= 2) return le16(data, 0);
        return -1;
    }

    private static byte[] le16(int value) {
        return new byte[]{(byte) value, (byte) (value >>> 8)};
    }

    private static byte[] le32(int value) {
        return new byte[]{(byte) value, (byte) (value >>> 8), (byte) (value >>> 16), (byte) (value >>> 24)};
    }

    private static byte[] concat(byte[]... parts) {
        int total = 0;
        for (byte[] part : parts) total += part.length;
        byte[] out = new byte[total];
        int offset = 0;
        for (byte[] part : parts) {
            System.arraycopy(part, 0, out, offset, part.length);
            offset += part.length;
        }
        return out;
    }

    private static String sha256Hex(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder out = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                out.append("0123456789abcdef".charAt((b >> 4) & 15));
                out.append("0123456789abcdef".charAt(b & 15));
            }
            return out.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    private static String inspect(Session session) throws IOException {
        Exchange exchange = session.exchange();
        handshake(exchange);
        text(query(exchange, SERIAL), "序列号"); // 只要求非空；不绑定某一台设备
        if (!isSupportedModel(query(exchange, MODEL))) {
            throw new IdentityException("不支持的设备型号（仅支持 E2 / E2S）");
        }
        return text(query(exchange, VERSION), "版本");
    }

    /** 检查 (1,8) 返回的设备型号是否为 Paperang E2 或 E2S。 */
    private static boolean isSupportedModel(byte[] model) {
        // 裸 ASCII 简写（自定义/测试固件直返 "E2" 或 "E2S"）。
        if (isE2OrE2sAscii(model, 0, model.length)) return true;
        // 原厂 TLV 格式：TLV 1 为设备类型码（1..4 字节），TLV 2 为型号字符串（可能无或有末尾 NUL 填充）。
        // 示例：原厂 E2 返回 9 字节 {1, 1, 0, 3, 2, 2, 0, 'E', '2'}。
        if (model.length < 9 || model[0] != 1) return false;
        int typeLen = le16(model, 1);
        if (typeLen < 1 || typeLen > 4) return false;
        int name = typeLen + 3;
        if (model.length < name + 5 || model[name] != 2) return false;
        int nameLen = le16(model, name + 1);
        if (nameLen < 2 || nameLen != model.length - name - 3) return false;
        int start = name + 3;
        int end = model.length;
        // 剥离尾部 NUL 填充；若末尾混入非零脏字节，则不会被剥离从而精确拒绝。
        while (end > start && model[end - 1] == 0) end--;
        return isE2OrE2sAscii(model, start, end);
    }

    private static boolean isE2OrE2sAscii(byte[] data, int start, int end) {
        int len = end - start;
        if (len == 2) return data[start] == 'E' && data[start + 1] == '2';
        if (len == 3) return data[start] == 'E' && data[start + 1] == '2' && data[start + 2] == 'S';
        return false;
    }

    private static void handshake(Exchange exchange) throws IOException {
        byte[] challenge = new byte[16];
        String hex = "0123456789abcdef";
        for (int i = 0; i < challenge.length; i++) challenge[i] = (byte) hex.charAt(RANDOM.nextInt(16));
        byte[] request = new byte[3 + challenge.length];
        request[0] = 1;
        put16(request, 1, challenge.length);
        System.arraycopy(challenge, 0, request, 3, challenge.length);
        byte[] tlvs = ok(exchange.request(SYSTEM, HANDSHAKE, request, 5_000), HANDSHAKE);
        int seen = 0;
        for (int off = 0; off < tlvs.length; ) {
            if (tlvs.length - off < 3) throw new IOException("握手参数头不完整");
            int id = u8(tlvs[off]);
            int length = le16(tlvs, off + 1);
            off += 3;
            if (id < 1 || id > 4 || (seen & (1 << id)) != 0 || length == 0 || length > tlvs.length - off) {
                throw new IOException("握手参数格式不正确");
            }
            if (id == 1) {
                if (length != challenge.length) throw new IOException("握手 challenge 长度不匹配");
                for (int i = 0; i < length; i++) {
                    if (tlvs[off + i] != challenge[i]) throw new IOException("握手 challenge 不匹配");
                }
            }
            seen |= 1 << id;
            off += length;
        }
        if ((seen & 0x0e) != 0x0e) throw new IOException("握手缺少必要参数");
    }

    /** 只读显示诊断 (1,241), 仅测试固件实现; 失败不影响主流程。 */
    private static String diagText(Exchange exchange) {
        try {
            byte[] data = query(exchange, 241);
            if (data.length == 0 || data.length > 120) return "(无诊断)";
            for (byte b : data) {
                if (u8(b) < 0x20 || u8(b) > 0x7e) return "(诊断含非文本)";
            }
            return new String(data, StandardCharsets.US_ASCII);
        } catch (IOException e) {
            return "(诊断不可读:" + e.getMessage() + ")";
        }
    }

    private static byte[] query(Exchange exchange, int child) throws IOException {
        return ok(exchange.request(SYSTEM, child, EMPTY, 3_000), child);
    }

    private static byte[] ok(byte[] content, int child) throws IOException {
        Reply reply = reply(content);
        if (reply.status != 1) throw new IOException("查询 (1," + child + ") 失败");
        return reply.data;
    }

    private static Reply reply(byte[] content) throws IOException {
        if (content.length < 3 || le16(content, 1) != content.length - 3) {
            throw new IOException("A5 状态/数据长度格式不正确");
        }
        return new Reply(u8(content[0]), Arrays.copyOfRange(content, 3, content.length));
    }

    private static String text(byte[] data, String field) throws IOException {
        if (data.length == 0 || data.length > 64) throw new IOException(field + "长度不正确");
        int end = data.length;
        while (end > 0 && data[end - 1] == 0) end--;
        if (end == 0) throw new IOException(field + "为空");
        for (int i = 0; i < end; i++) {
            if (u8(data[i]) < 0x21 || u8(data[i]) > 0x7e) throw new IOException(field + "包含无效字符");
        }
        return new String(data, 0, end, StandardCharsets.US_ASCII);
    }

    private static final class Reply {
        final int status;
        final byte[] data;
        Reply(int status, byte[] data) { this.status = status; this.data = data; }
    }

    private static final class IdentityException extends IOException {
        private static final long serialVersionUID = 1L;

        IdentityException(String message) { super(message); }
    }
}
