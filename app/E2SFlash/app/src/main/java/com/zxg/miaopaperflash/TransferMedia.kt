package com.zxg.miaopaperflash

import android.content.ContentResolver
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ImageDecoder
import android.graphics.Matrix
import android.graphics.Paint
import android.media.ExifInterface
import android.net.Uri
import android.os.Build
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import kotlin.math.max
import kotlin.math.min

/** 电子书编码与预处理结果。 */
internal data class PreparedBook(
    val fileName: String,
    val defaultTitle: String,
    val bytes: ByteArray,
    val enc: Int,
    val sourceEncoding: String,
)

/** 墨水屏 1-bit 抖动/二值化算法（对齐原版 Miaopaper 4 种算法，展示直观中文名）。 */
internal enum class LockDitherMode(val label: String) {
    ATKINSON("清晰"),
    FLOYD("细腻"),
    BAYER("网点"),
    THRESHOLD("黑白"),
}

/** 原图适配取景框方式。 */
internal enum class LockFitMode {
    FILL,
    FIT,
}

/** 锁屏图编辑参数（所有平移量均定义在 250×122 逻辑取景坐标系下）。 */
internal data class LockEditParams(
    val offsetX: Float = 0f,
    val offsetY: Float = 0f,
    val zoom: Float = 1f,
    val rotation: Int = 0,
    val fitMode: LockFitMode = LockFitMode.FILL,
    val ditherMode: LockDitherMode = LockDitherMode.ATKINSON,
    val brightness: Int = 0,
    val contrast: Int = 0,
    val invert: Boolean = false,
) {
    val isDefault: Boolean
        get() = offsetX == 0f &&
            offsetY == 0f &&
            zoom == 1f &&
            rotation == 0 &&
            fitMode == LockFitMode.FILL &&
            ditherMode == LockDitherMode.ATKINSON &&
            brightness == 0 &&
            contrast == 0 &&
            !invert
}

/** 锁屏图 1-bit 渲染与打包结果（250×122 预览 + 4000 B 固件格式）。 */
internal data class RenderedLockImage(
    val preview: Bitmap,
    val payload: ByteArray,
)

internal object TransferMedia {
    const val LOCK_W = 250
    const val LOCK_VISIBLE_H = 122
    const val LOCK_TOTAL_H = 128
    const val LOCK_Y_OFFSET = 6
    const val MAX_BOOK_BYTES = 7 * 1024 * 1024
    const val MIN_ZOOM = 0.25f
    const val MAX_ZOOM = 6.0f

    private val gb2312Charset: Charset = Charset.forName("GB2312")
    private val gbkCharset: Charset = Charset.forName("GBK")

    // 原版 Miaopaper 4×4 Bayer 有序抖动矩阵
    private val bayer4x4 = intArrayOf(
        1, 9, 3, 11,
        13, 5, 15, 7,
        4, 12, 2, 10,
        16, 8, 14, 6,
    )

    /** 探测 TXT 编码并转换为阅读器原生支持的纯 ASCII 或 HZK16-safe GB2312 字节流。 */
    fun prepareTxtBook(fileName: String, raw: ByteArray): PreparedBook {
        if (raw.size < 16) {
            throw IOException("文件内容过短")
        }
        val cleanTitle = fileName
            .substringBeforeLast('.')
            .trim()
            .ifEmpty { "未命名" }

        val detected = detectEncoding(raw)
        if (detected == "ASCII") {
            val normalized = normalizeLineEndingsAscii(raw)
            if (normalized.size > MAX_BOOK_BYTES) {
                throw IOException("电子书超过 7 MB 上限")
            }
            return PreparedBook(
                fileName = fileName,
                defaultTitle = cleanTitle,
                bytes = normalized,
                enc = ReaderTransfer.ENC_ASCII,
                sourceEncoding = "ASCII",
            )
        }

        val text = decodeText(raw, detected)
        val gbBytes = encodeHzkGb2312(text)
        if (gbBytes.size < 16) {
            throw IOException("转码后内容为空")
        }
        if (gbBytes.size > MAX_BOOK_BYTES) {
            throw IOException("电子书超过 7 MB 上限")
        }
        return PreparedBook(
            fileName = fileName,
            defaultTitle = cleanTitle,
            bytes = gbBytes,
            enc = ReaderTransfer.ENC_GB2312,
            sourceEncoding = detected,
        )
    }

    /** 将书名编码为固件 0x10 要求的 2..19 字节 GB2312/ASCII（不截断半汉字）。 */
    fun encodeBookTitle(title: String): ByteArray {
        val trimmed = title.trim().ifEmpty { "Book" }
        val hasNonAscii = trimmed.any { it.code >= 0x80 }
        val out = ByteArrayOutputStream(20)
        if (!hasNonAscii) {
            for (ch in trimmed) {
                if (out.size() >= 19) break
                val c = ch.code
                if (c in 0x20..0x7E) out.write(c)
            }
        } else {
            val enc = gb2312Charset.newEncoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
            val tmp = ByteArray(4)
            var i = 0
            while (i < trimmed.length) {
                val cp = trimmed.codePointAt(i)
                i += Character.charCount(cp)
                if (cp in 0x20..0x7E) {
                    if (out.size() + 1 > 19) break
                    out.write(cp)
                } else {
                    val gb = encodeSingleGb2312(enc, cp, tmp)
                    if (gb != null) {
                        if (out.size() + 2 > 19) break
                        out.write(gb[0].toInt() and 0xFF)
                        out.write(gb[1].toInt() and 0xFF)
                    }
                }
            }
        }
        var bytes = out.toByteArray()
        if (bytes.isEmpty()) {
            bytes = "Book".toByteArray(StandardCharsets.US_ASCII)
        } else if (bytes.size == 1) {
            // 固件 0x10 要求 title_len >= 2
            bytes = byteArrayOf(bytes[0], 0x20)
        }
        return bytes
    }

    /**
     * 读取本地/相册图片并自动校正方向与降采样：
     * Android 9+ 优先走 ImageDecoder（原生支持 HEIC/WebP 与 EXIF 旋转且只读一次 URI）；
     * 异常或旧系统自动回退到单次流读取 + BitmapFactory，避免部分相册 URI 无法重复打开。
     */
    fun loadBitmap(resolver: ContentResolver, uri: Uri): Bitmap {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            try {
                val source = ImageDecoder.createSource(resolver, uri)
                val decoded = ImageDecoder.decodeBitmap(source) { decoder, info, _ ->
                    decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                    val w = info.size.width
                    val h = info.size.height
                    val longest = max(w, h)
                    if (longest > 1600) {
                        val ratio = 1600f / longest.toFloat()
                        decoder.setTargetSize(
                            (w * ratio).toInt().coerceAtLeast(1),
                            (h * ratio).toInt().coerceAtLeast(1),
                        )
                    }
                }
                return ensureSoftwareArgb(decoded)
            } catch (_: Throwable) {
                // 回退至单次流字节解码
            }
        }

        val raw = resolver.openInputStream(uri)?.use { input ->
            val out = ByteArrayOutputStream()
            val buf = ByteArray(16384)
            var total = 0
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
                total += n
                if (total > 32 * 1024 * 1024) {
                    throw IOException("图片文件过大")
                }
            }
            out.toByteArray()
        } ?: throw IOException("无法读取图片")

        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(raw, 0, raw.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            throw IOException("图片格式不支持")
        }

        var sample = 1
        while (bounds.outWidth / sample > 1600 || bounds.outHeight / sample > 1600) {
            sample *= 2
        }
        val opts = BitmapFactory.Options().apply {
            inSampleSize = sample
            inPreferredConfig = Bitmap.Config.ARGB_8888
        }
        val decoded = BitmapFactory.decodeByteArray(raw, 0, raw.size, opts)
            ?: throw IOException("图片解码失败")

        val rotation = readExifRotation(raw)
        val oriented = if (rotation == 0) {
            decoded
        } else {
            val matrix = Matrix().apply { postRotate(rotation.toFloat()) }
            val rotated = Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, matrix, true)
            if (rotated !== decoded) decoded.recycle()
            rotated
        }
        return ensureSoftwareArgb(oriented)
    }

    private fun ensureSoftwareArgb(bmp: Bitmap): Bitmap {
        val isHardware = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
            bmp.config == Bitmap.Config.HARDWARE
        if (!isHardware && bmp.config == Bitmap.Config.ARGB_8888) {
            return bmp
        }
        val copy = bmp.copy(Bitmap.Config.ARGB_8888, false)
            ?: throw IOException("图片格式转换失败")
        if (copy !== bmp) bmp.recycle()
        return copy
    }

    /** 计算在 250×122 逻辑取景坐标系下原图的有效缩放比例（与编辑器画布共用）。 */
    fun logicalScale(srcWidth: Int, srcHeight: Int, params: LockEditParams): Float {
        val swap = ((params.rotation % 180) + 180) % 180 == 90
        val rw = (if (swap) srcHeight else srcWidth).coerceAtLeast(1).toFloat()
        val rh = (if (swap) srcWidth else srcHeight).coerceAtLeast(1).toFloat()
        val base = when (params.fitMode) {
            LockFitMode.FILL -> max(LOCK_W / rw, LOCK_VISIBLE_H / rh)
            LockFitMode.FIT -> min(LOCK_W / rw, LOCK_VISIBLE_H / rh)
        }
        return base * params.zoom.coerceIn(MIN_ZOOM, MAX_ZOOM)
    }

    /**
     * 按当前编辑参数将原图变换到 250×122 可见区，执行亮度/对比度调整与
     * 1-bit 算法（Atkinson/Floyd-Steinberg/Bayer/黑白阈值），并按 E2S 固件
     * lock_image(y -> sy=y+6, bit=1 黑) 规范打包为 4000 字节。
     */
    fun renderLockImage(src: Bitmap, params: LockEditParams): RenderedLockImage {
        val work = Bitmap.createBitmap(LOCK_W, LOCK_VISIBLE_H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(work)
        canvas.drawColor(Color.WHITE)

        val scale = logicalScale(src.width, src.height, params)
        val matrix = Matrix().apply {
            postTranslate(-src.width / 2f, -src.height / 2f)
            if (params.rotation != 0) postRotate(params.rotation.toFloat())
            postScale(scale, scale)
            postTranslate(LOCK_W / 2f + params.offsetX, LOCK_VISIBLE_H / 2f + params.offsetY)
        }
        val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
        canvas.drawBitmap(src, matrix, paint)

        val count = LOCK_W * LOCK_VISIBLE_H
        val pixels = IntArray(count)
        work.getPixels(pixels, 0, LOCK_W, 0, 0, LOCK_W, LOCK_VISIBLE_H)
        work.recycle()

        // 亮度（-100..100）与经典对比度系数（-100..100）
        val contrastV = (params.contrast.coerceIn(-100, 100) * 1.6f)
        val contrastFactor = (259f * (contrastV + 255f)) / (255f * (259f - contrastV))
        val brightDelta = params.brightness.coerceIn(-100, 100) * 1.25f

        val gray = FloatArray(count)
        for (i in 0 until count) {
            val c = pixels[i]
            val a = (c ushr 24) and 0xFF
            val r = (c ushr 16) and 0xFF
            val g = (c ushr 8) and 0xFF
            val b = c and 0xFF
            // 透明区域与白底合成
            val cr = (r * a + 255 * (255 - a)) / 255f
            val cg = (g * a + 255 * (255 - a)) / 255f
            val cb = (b * a + 255 * (255 - a)) / 255f
            var luma = 0.299f * cr + 0.587f * cg + 0.114f * cb
            if (params.contrast != 0) {
                luma = (luma - 128f) * contrastFactor + 128f
            }
            if (params.brightness != 0) {
                luma += brightDelta
            }
            luma = luma.coerceIn(0f, 255f)
            gray[i] = if (params.invert) 255f - luma else luma
        }

        val isBlack = BooleanArray(count)
        when (params.ditherMode) {
            LockDitherMode.ATKINSON -> {
                // Atkinson 误差扩散：高光与暗部通透，适合墨水屏
                for (y in 0 until LOCK_VISIBLE_H) {
                    val row = y * LOCK_W
                    for (x in 0 until LOCK_W) {
                        val idx = row + x
                        val old = gray[idx]
                        val newVal = if (old < 128f) 0f else 255f
                        isBlack[idx] = (newVal == 0f)
                        val err = (old - newVal) / 8f
                        if (x + 1 < LOCK_W) gray[idx + 1] += err
                        if (x + 2 < LOCK_W) gray[idx + 2] += err
                        if (y + 1 < LOCK_VISIBLE_H) {
                            val next = idx + LOCK_W
                            if (x > 0) gray[next - 1] += err
                            gray[next] += err
                            if (x + 1 < LOCK_W) gray[next + 1] += err
                        }
                        if (y + 2 < LOCK_VISIBLE_H) {
                            gray[idx + LOCK_W * 2] += err
                        }
                    }
                }
            }

            LockDitherMode.FLOYD -> {
                // Floyd-Steinberg 误差扩散（对齐原版 Miaopaper）
                for (y in 0 until LOCK_VISIBLE_H) {
                    val row = y * LOCK_W
                    for (x in 0 until LOCK_W) {
                        val idx = row + x
                        val old = gray[idx]
                        val newVal = if (old < 128f) 0f else 255f
                        isBlack[idx] = (newVal == 0f)
                        val err = old - newVal
                        if (x + 1 < LOCK_W) gray[idx + 1] += err * (7f / 16f)
                        if (y + 1 < LOCK_VISIBLE_H) {
                            val next = idx + LOCK_W
                            if (x > 0) gray[next - 1] += err * (3f / 16f)
                            gray[next] += err * (5f / 16f)
                            if (x + 1 < LOCK_W) gray[next + 1] += err * (1f / 16f)
                        }
                    }
                }
            }

            LockDitherMode.BAYER -> {
                // 4×4 Bayer 有序抖动（对齐原版 Miaopaper）
                for (y in 0 until LOCK_VISIBLE_H) {
                    val row = y * LOCK_W
                    val byRow = (y and 3) shl 2
                    for (x in 0 until LOCK_W) {
                        val threshold = bayer4x4[byRow or (x and 3)] * 16f - 8f
                        isBlack[row + x] = gray[row + x] < threshold
                    }
                }
            }

            LockDitherMode.THRESHOLD -> {
                for (i in 0 until count) {
                    isBlack[i] = gray[i] < 128f
                }
            }
        }

        // 同步生成预览图与 4000 B 固件载荷（可见 y=0..121 写入 sy=y+6，bit=1 为黑）。
        val previewPixels = IntArray(count)
        val payload = ByteArray(ReaderTransfer.LOCK_IMAGE_BYTES)
        for (y in 0 until LOCK_VISIBLE_H) {
            val sy = y + LOCK_Y_OFFSET
            val bandBase = (sy shr 3) * LOCK_W
            val bitMask = 1 shl (sy and 7)
            val row = y * LOCK_W
            for (x in 0 until LOCK_W) {
                if (isBlack[row + x]) {
                    previewPixels[row + x] = 0xFF141414.toInt()
                    payload[bandBase + x] = (payload[bandBase + x].toInt() or bitMask).toByte()
                } else {
                    previewPixels[row + x] = 0xFFFAFAF7.toInt()
                }
            }
        }
        val preview = Bitmap.createBitmap(LOCK_W, LOCK_VISIBLE_H, Bitmap.Config.ARGB_8888)
        preview.setPixels(previewPixels, 0, LOCK_W, 0, 0, LOCK_W, LOCK_VISIBLE_H)
        return RenderedLockImage(preview = preview, payload = payload)
    }

    private fun readExifRotation(raw: ByteArray): Int {
        return try {
            ByteArrayInputStream(raw).use { input ->
                when (ExifInterface(input).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION,
                    ExifInterface.ORIENTATION_NORMAL,
                )) {
                    ExifInterface.ORIENTATION_ROTATE_90 -> 90
                    ExifInterface.ORIENTATION_ROTATE_180 -> 180
                    ExifInterface.ORIENTATION_ROTATE_270 -> 270
                    else -> 0
                }
            }
        } catch (_: RuntimeException) {
            0
        } catch (_: IOException) {
            0
        }
    }

    private fun detectEncoding(raw: ByteArray): String {
        if (raw.size >= 3 &&
            (raw[0].toInt() and 0xFF) == 0xEF &&
            (raw[1].toInt() and 0xFF) == 0xBB &&
            (raw[2].toInt() and 0xFF) == 0xBF
        ) {
            return "UTF-8"
        }
        if (raw.size >= 2 &&
            (raw[0].toInt() and 0xFF) == 0xFF &&
            (raw[1].toInt() and 0xFF) == 0xFE
        ) {
            return "UTF-16LE"
        }
        if (raw.size >= 2 &&
            (raw[0].toInt() and 0xFF) == 0xFE &&
            (raw[1].toInt() and 0xFF) == 0xFF
        ) {
            return "UTF-16BE"
        }
        var hasHigh = false
        for (b in raw) {
            if ((b.toInt() and 0xFF) >= 0x80) {
                hasHigh = true
                break
            }
        }
        if (!hasHigh) return "ASCII"
        if (isValidCharset(raw, StandardCharsets.UTF_8)) return "UTF-8"
        if (isValidCharset(raw, gbkCharset)) return "GBK"
        return "GBK"
    }

    private fun isValidCharset(raw: ByteArray, cs: Charset): Boolean {
        return try {
            val dec = cs.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
            dec.decode(ByteBuffer.wrap(raw))
            true
        } catch (_: CharacterCodingException) {
            false
        }
    }

    private fun decodeText(raw: ByteArray, encoding: String): String {
        return when (encoding) {
            "UTF-8" -> {
                val off = if (raw.size >= 3 &&
                    (raw[0].toInt() and 0xFF) == 0xEF &&
                    (raw[1].toInt() and 0xFF) == 0xBB &&
                    (raw[2].toInt() and 0xFF) == 0xBF
                ) 3 else 0
                String(raw, off, raw.size - off, StandardCharsets.UTF_8)
            }
            "UTF-16LE" -> String(raw, 2, raw.size - 2, StandardCharsets.UTF_16LE)
            "UTF-16BE" -> String(raw, 2, raw.size - 2, StandardCharsets.UTF_16BE)
            else -> String(raw, gbkCharset)
        }
    }

    private fun normalizeLineEndingsAscii(raw: ByteArray): ByteArray {
        val out = ByteArrayOutputStream(raw.size)
        var i = 0
        while (i < raw.size) {
            val c = raw[i].toInt() and 0xFF
            if (c == '\r'.code) {
                if (i + 1 < raw.size && (raw[i + 1].toInt() and 0xFF) == '\n'.code) {
                    i++
                }
                out.write('\n'.code)
            } else if (c != 0) {
                out.write(c)
            }
            i++
        }
        return out.toByteArray()
    }

    /** 逐码点转为严格 HZK16 范围（0xA1..0xF7 / 0xA1..0xFE）的 GB2312 流。 */
    private fun encodeHzkGb2312(text: String): ByteArray {
        val out = ByteArrayOutputStream(min(MAX_BOOK_BYTES, text.length * 2))
        val enc = gb2312Charset.newEncoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
        val tmp = ByteArray(4)
        var i = 0
        while (i < text.length) {
            val cp = text.codePointAt(i)
            i += Character.charCount(cp)
            when {
                cp == '\r'.code -> {
                    if (i < text.length && text[i] == '\n') i++
                    out.write('\n'.code)
                }
                cp == 0 || cp == 0xFEFF -> {
                    // 跳过 NUL 与 BOM
                }
                cp < 0x80 -> out.write(cp)
                else -> {
                    val gb = encodeSingleGb2312(enc, cp, tmp)
                    if (gb != null) {
                        out.write(gb[0].toInt() and 0xFF)
                        out.write(gb[1].toInt() and 0xFF)
                    } else {
                        // 超出 GB2312 的生僻字或符号用全角空格占位，避免字节错位
                        out.write(0xA1)
                        out.write(0xA1)
                    }
                }
            }
        }
        return out.toByteArray()
    }

    private fun encodeSingleGb2312(
        enc: java.nio.charset.CharsetEncoder,
        cp: Int,
        buf: ByteArray,
    ): ByteArray? {
        if (cp > 0xFFFF) return null
        enc.reset()
        val cb = CharBuffer.wrap(charArrayOf(cp.toChar()))
        val bb = ByteBuffer.wrap(buf)
        val res = enc.encode(cb, bb, true)
        if (res.isError || bb.position() != 2) return null
        val hi = buf[0].toInt() and 0xFF
        val lo = buf[1].toInt() and 0xFF
        if (hi !in 0xA1..0xF7 || lo !in 0xA1..0xFE) return null
        return buf
    }
}
