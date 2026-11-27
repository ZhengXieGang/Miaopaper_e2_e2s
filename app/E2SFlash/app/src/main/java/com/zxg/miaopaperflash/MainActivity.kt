package com.zxg.miaopaperflash

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.ActivityNotFoundException
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Bitmap
import android.location.LocationManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InterruptedIOException
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt

private const val MAX_IMAGE = 0x1F0000
private const val PERM_REQ = 1

private val SUPPORTED_BLE_NAMES = setOf("Paperang_E2", "Paperang_E2S", "MPP_E2S")

private enum class AppTab(val label: String) {
    FLASH("刷机"),
    TRANSFER("传输"),
}

private enum class Tone { NEUTRAL, SUCCESS, WARNING, ERROR }

private enum class DeviceModel {
    E2,
    E2S,
    READER,
}

private fun resolveDeviceModel(name: String): DeviceModel? = when {
    name == "Paperang_E2" -> DeviceModel.E2
    name == "Paperang_E2S" -> DeviceModel.E2S
    name == "MPP_E2S" || name.startsWith("MPP_") -> DeviceModel.READER
    else -> null
}

private data class SelectedDevice(
    val device: BluetoothDevice,
    val name: String,
    val model: DeviceModel,
)

private class ScanEntry(
    val device: BluetoothDevice,
    val name: String,
    val model: DeviceModel,
    val rssi: Int,
)

private class FirmwareInfo(
    val fileName: String,
    val bytes: ByteArray,
    val sha256: String,
    val projectName: String?,
    val version: String?,
    val appElfSha256: ByteArray?,
)

private data class LockImageDraft(
    val fileName: String,
    val source: Bitmap,
    val params: LockEditParams = LockEditParams(),
    val rendered: RenderedLockImage,
)

private data class LockEditorSession(
    val fileName: String,
    val source: Bitmap,
    val initialParams: LockEditParams,
)

private data class UiState(
    val busy: Boolean = false,
    val title: String = "就绪",
    val detail: String = "",
    val progress: Float? = null,
    val tone: Tone = Tone.NEUTRAL,
)

class MainActivity : ComponentActivity() {

    private val currentTab = mutableStateOf(AppTab.FLASH)
    private val state = mutableStateOf(UiState())
    private val confirmFlash = mutableStateOf(false)
    private val devices = mutableStateOf<List<ScanEntry>>(emptyList())
    private val target = mutableStateOf<SelectedDevice?>(null)
    private val firmware = mutableStateOf<FirmwareInfo?>(null)
    private val bookDraft = mutableStateOf<PreparedBook?>(null)
    private val bookTitle = mutableStateOf("")
    private val lockDraft = mutableStateOf<LockImageDraft?>(null)
    private val lockEditor = mutableStateOf<LockEditorSession?>(null)
    private val scanning = mutableStateOf(false)
    private val scanned = mutableStateOf(false)

    private val main = Handler(Looper.getMainLooper())
    private val working = AtomicBoolean()
    private var worker: Thread? = null
    private var pendingAction: (() -> Unit)? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanTimeout: Runnable? = null

    private val pickFirmware = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) loadFirmware(uri)
    }

    private val pickBook = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) loadBook(uri)
    }

    private val pickVisualImage = registerForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri != null) loadLockImage(uri)
    }

    private val pickContentImage = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) loadLockImage(uri)
    }

    private val pickDocImage = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) loadLockImage(uri)
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.scanRecord?.deviceName ?: result.device.name ?: return
            val model = resolveDeviceModel(name) ?: return
            if (name !in SUPPORTED_BLE_NAMES && model != DeviceModel.READER) return
            val entry = ScanEntry(result.device, name, model, result.rssi)
            main.post {
                val merged = (devices.value.filterNot { it.device.address == entry.device.address } + entry)
                    .sortedByDescending { it.rssi }
                devices.value = merged
            }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post {
                stopScan()
                scanned.value = true
                state.value = UiState(title = "扫描失败", detail = "错误码 $errorCode", tone = Tone.ERROR)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val versionLabel = readAppVersion()
        setContent {
            E2sTheme {
                val editor = lockEditor.value
                if (editor != null) {
                    val ext = LocalExtendedColors.current
                    LockImageEditorScreen(
                        source = editor.source,
                        initialParams = editor.initialParams,
                        cardBorder = ext.cardBorder,
                        subSurface = ext.subSurface,
                        onCancel = { lockEditor.value = null },
                        onConfirm = { params, rendered ->
                            lockDraft.value = LockImageDraft(
                                fileName = editor.fileName,
                                source = editor.source,
                                params = params,
                                rendered = rendered,
                            )
                            lockEditor.value = null
                        },
                    )
                } else {
                    ToolboxScreen(
                        versionLabel = versionLabel,
                        tab = currentTab.value,
                        state = state.value,
                        target = target.value,
                        devices = devices.value,
                        scanning = scanning.value,
                        scanned = scanned.value,
                        firmware = firmware.value,
                        book = bookDraft.value,
                        bookTitle = bookTitle.value,
                        lockDraft = lockDraft.value,
                        onSelectTab = { next ->
                            if (!state.value.busy && currentTab.value != next) {
                                stopScan()
                                currentTab.value = next
                                if (state.value.tone != Tone.NEUTRAL) {
                                    state.value = UiState()
                                }
                            }
                        },
                        onScan = { startScan() },
                        onStopScan = { finishScan() },
                        onSelectDevice = { entry ->
                            stopScan()
                            scanned.value = true
                            target.value = SelectedDevice(entry.device, entry.name, entry.model)
                            state.value = UiState()
                        },
                        onChangeDevice = {
                            target.value = null
                            startScan()
                        },
                        onPickFirmware = { launchDocumentPicker(pickFirmware, arrayOf("*/*")) },
                        onFlash = { confirmFlash.value = true },
                        onPickBook = { launchDocumentPicker(pickBook, arrayOf("*/*")) },
                        onBookTitleChange = { bookTitle.value = it },
                        onSendBook = { startSendBook() },
                        onPickLockImage = { launchLockImagePicker() },
                        onEditLockImage = {
                            val cur = lockDraft.value
                            if (cur != null && !state.value.busy) {
                                lockEditor.value = LockEditorSession(
                                    fileName = cur.fileName,
                                    source = cur.source,
                                    initialParams = cur.params,
                                )
                            }
                        },
                        onSendLockImage = { startSendLockImage() },
                        onCancel = { worker?.interrupt() },
                        onDismissStatus = {
                            if (!state.value.busy) state.value = UiState()
                        },
                    )
                    if (confirmFlash.value) {
                        ConfirmDialog(
                            target = target.value,
                            firmware = firmware.value,
                            onDismiss = { confirmFlash.value = false },
                            onConfirm = {
                                confirmFlash.value = false
                                startFlash()
                            },
                        )
                    }
                }
            }
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        enableEdgeToEdge()
    }

    private fun readAppVersion(): String {
        return try {
            @Suppress("DEPRECATION")
            val v = packageManager.getPackageInfo(packageName, 0).versionName
            if (v.isNullOrBlank()) "v1.2" else "v$v"
        } catch (_: RuntimeException) {
            "v1.2"
        }
    }

    private fun permissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private fun hasPermissions(): Boolean =
        permissions().all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    private fun locationEnabled(): Boolean {
        val manager = getSystemService(LOCATION_SERVICE) as? LocationManager ?: return false
        return if (Build.VERSION.SDK_INT >= 28) {
            manager.isLocationEnabled
        } else {
            manager.isProviderEnabled(LocationManager.GPS_PROVIDER) ||
                manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
        }
    }

    private fun withPermissions(action: () -> Unit) {
        if (hasPermissions()) {
            action()
            return
        }
        pendingAction = action
        requestPermissions(permissions(), PERM_REQ)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != PERM_REQ) return
        val action = pendingAction
        pendingAction = null
        if (action != null && hasPermissions()) {
            action()
        } else {
            state.value = UiState(title = "需要蓝牙权限", detail = "请在系统设置中允许", tone = Tone.ERROR)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startScan() = withPermissions {
        val adapter = (getSystemService(BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
        if (adapter == null || !adapter.isEnabled) {
            state.value = UiState(title = "请开启蓝牙", tone = Tone.ERROR)
            return@withPermissions
        }
        if (Build.VERSION.SDK_INT < 31 && !locationEnabled()) {
            state.value = UiState(
                title = "请开启定位服务",
                detail = "Android 11 及以下扫描 BLE 需要定位",
                tone = Tone.ERROR,
            )
            return@withPermissions
        }
        val le = adapter.bluetoothLeScanner
        if (le == null) {
            state.value = UiState(title = "蓝牙扫描不可用", tone = Tone.ERROR)
            return@withPermissions
        }
        stopScan()
        devices.value = emptyList()
        scanned.value = false
        scanning.value = true
        state.value = UiState()
        scanner = le
        try {
            le.startScan(
                emptyList(),
                ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
                scanCallback,
            )
        } catch (e: RuntimeException) {
            stopScan()
            state.value = UiState(title = "扫描失败", detail = e.message ?: "", tone = Tone.ERROR)
            return@withPermissions
        }
        val timeout = Runnable { finishScan() }
        scanTimeout = timeout
        main.postDelayed(timeout, 8_000)
    }

    private fun finishScan() {
        stopScan()
        scanned.value = true
        if (target.value != null || state.value.busy) return
        state.value = UiState()
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        scanTimeout?.let { main.removeCallbacks(it) }
        scanTimeout = null
        if (scanning.value) {
            try {
                scanner?.stopScan(scanCallback)
            } catch (_: RuntimeException) {
                // 蓝牙适配器可能已关闭
            }
        }
        scanning.value = false
        scanner = null
    }

    /* ---------------- 刷机任务 ---------------- */

    private fun startFlash() = withPermissions {
        stopScan()
        if (target.value == null) {
            state.value = UiState(title = "未选择设备", tone = Tone.WARNING)
            return@withPermissions
        }
        if (firmware.value == null) {
            state.value = UiState(title = "未选择固件", tone = Tone.WARNING)
            return@withPermissions
        }
        if (!working.compareAndSet(false, true)) return@withPermissions
        launch("E2-FLASH") { flashJob() }
    }

    private fun flashJob(): UiState {
        val selected = target.value ?: throw IOException("未选择设备")
        val fw = firmware.value ?: throw IOException("未选择固件")

        if (selected.model == DeviceModel.READER) {
            if (fw.projectName != "miaopaper_e2_e2s") {
                return UiState(
                    title = "固件不匹配",
                    detail = "仅支持 miaopaper_e2_e2s 固件",
                    tone = Tone.ERROR,
                )
            }
            val res = ReaderOtaFlasher.run(
                applicationContext,
                selected.device,
                fw.bytes,
                fw.appElfSha256,
            ) { title, detail, progress ->
                stage(title = title, detail = detail, progress = progress)
            }
            return if (res.verified) {
                UiState(title = res.title, detail = res.detail, tone = Tone.SUCCESS)
            } else {
                UiState(title = res.title, detail = res.detail, tone = Tone.WARNING)
            }
        }

        stage("检查设备…")
        val restore = A5Protocol.run(
            A5Protocol.Operation.RESTORE,
            SessionConnector(selected),
            A5Protocol.CommitCheck { },
        )
        when (restore.outcome) {
            A5Protocol.Outcome.ALREADY_STOCK -> stage("已是原厂固件", "开始写入")
            A5Protocol.Outcome.STOCK_BOOTED -> stage("已切回原厂", "开始写入")
            A5Protocol.Outcome.SELECTION_CONFIRMED_BOOT_UNVERIFIED ->
                return UiState(title = "未确认原厂启动", detail = restore.detail, tone = Tone.WARNING)
            else -> return UiState(title = "无法刷入", detail = restore.detail, tone = Tone.ERROR)
        }

        val result = A5Protocol.runFlash(
            fw.bytes,
            fw.sha256,
            SessionConnector(selected),
            {},
            A5Protocol.Progress { done, total ->
                stage(
                    title = "写入固件",
                    detail = "$done / $total",
                    progress = done.toFloat() / total.coerceAtLeast(1),
                )
            },
        )
        return when (result.outcome) {
            A5Protocol.Outcome.FLASHED_BOOTED ->
                UiState(title = result.detail, tone = Tone.SUCCESS)
            A5Protocol.Outcome.FLASHED_BOOT_UNVERIFIED ->
                UiState(title = "未确认启动", detail = result.detail, tone = Tone.WARNING)
            else -> UiState(title = "刷入失败", detail = result.detail, tone = Tone.ERROR)
        }
    }

    /* ---------------- 电子书 / 锁屏图传输任务 ---------------- */

    private fun startSendBook() = withPermissions {
        stopScan()
        val dev = target.value
        if (dev == null || dev.model != DeviceModel.READER) {
            state.value = UiState(title = "请先选择阅读器", tone = Tone.WARNING)
            return@withPermissions
        }
        val book = bookDraft.value
        if (book == null) {
            state.value = UiState(title = "未选择电子书", tone = Tone.WARNING)
            return@withPermissions
        }
        if (!working.compareAndSet(false, true)) return@withPermissions
        val chosenTitle = bookTitle.value.trim().ifEmpty { book.defaultTitle }
        val titleBytes = TransferMedia.encodeBookTitle(chosenTitle)
        launch("E2-BOOK") {
            ReaderTransfer.uploadBook(
                applicationContext,
                dev.device,
                titleBytes,
                book.bytes,
                book.enc,
            ) { title, detail, progress ->
                stage(title = title, detail = detail, progress = progress)
            }
            UiState(
                title = "电子书已发送",
                detail = "$chosenTitle · ${formatSize(book.bytes.size)}",
                tone = Tone.SUCCESS,
            )
        }
    }

    private fun startSendLockImage() = withPermissions {
        stopScan()
        val dev = target.value
        if (dev == null || dev.model != DeviceModel.READER) {
            state.value = UiState(title = "请先选择阅读器", tone = Tone.WARNING)
            return@withPermissions
        }
        val draft = lockDraft.value
        if (draft == null) {
            state.value = UiState(title = "未选择图片", tone = Tone.WARNING)
            return@withPermissions
        }
        if (!working.compareAndSet(false, true)) return@withPermissions
        val payload = draft.rendered.payload
        launch("E2-LOCK") {
            ReaderTransfer.uploadLockImage(
                applicationContext,
                dev.device,
                payload,
            ) { title, detail, progress ->
                stage(title = title, detail = detail, progress = progress)
            }
            UiState(title = "锁屏图已更新", tone = Tone.SUCCESS)
        }
    }

    /* ---------------- 本地文件加载 ---------------- */

    private fun launchDocumentPicker(
        launcher: androidx.activity.result.ActivityResultLauncher<Array<String>>,
        mimeTypes: Array<String>,
    ) {
        try {
            launcher.launch(mimeTypes)
        } catch (_: ActivityNotFoundException) {
            state.value = UiState(title = "无法打开文件选择器", detail = "未找到系统文件管理应用", tone = Tone.ERROR)
        } catch (e: RuntimeException) {
            state.value = UiState(title = "无法打开文件选择器", detail = e.message ?: "", tone = Tone.ERROR)
        }
    }

    // 拉起零权限系统相册选图（三级容错）：
    // 1) Android 原生照片选择器 PickVisualMedia（直接进系统相册，无需存储权限）；
    // 2) 回退 ACTION_GET_CONTENT（兼容所有图库与旧版/定制系统）；
    // 3) 兜底 ACTION_OPEN_DOCUMENT。
    private fun launchLockImagePicker() {
        if (ActivityResultContracts.PickVisualMedia.isPhotoPickerAvailable(this)) {
            try {
                pickVisualImage.launch(
                    PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly),
                )
                return
            } catch (_: ActivityNotFoundException) {
                // 回退至 GetContent
            } catch (_: SecurityException) {
                // 回退至 GetContent
            }
        }
        try {
            pickContentImage.launch("image/*")
            return
        } catch (_: ActivityNotFoundException) {
            // 兜底至 OpenDocument
        } catch (_: SecurityException) {
            // 兜底至 OpenDocument
        }
        try {
            pickDocImage.launch(arrayOf("image/*"))
        } catch (_: ActivityNotFoundException) {
            state.value = UiState(title = "无法打开相册", detail = "未找到可用的相册或图库应用", tone = Tone.ERROR)
        } catch (e: RuntimeException) {
            state.value = UiState(title = "无法打开相册", detail = e.message ?: "", tone = Tone.ERROR)
        }
    }

    private fun loadFirmware(uri: Uri) {
        if (!working.compareAndSet(false, true)) return
        launch("E2-FIRMWARE") {
            val name = displayName(uri, "firmware.bin")
            val bytes = readUriBytes(uri, MAX_IMAGE + 1)
            if (bytes.size > MAX_IMAGE) {
                return@launch UiState(title = "固件过大", detail = "不能超过 1.93 MB", tone = Tone.ERROR)
            }
            if (bytes.size < 0x1000 || (bytes[0].toInt() and 0xFF) != 0xE9) {
                return@launch UiState(title = "固件无效", detail = "需 ESP32 app .bin", tone = Tone.ERROR)
            }
            val desc = parseEspAppDesc(bytes)
            val loaded = FirmwareInfo(
                fileName = name,
                bytes = bytes,
                sha256 = sha256Hex(bytes),
                projectName = desc.first,
                version = desc.second,
                appElfSha256 = desc.third,
            )
            main.post { firmware.value = loaded }
            UiState()
        }
    }

    private fun loadBook(uri: Uri) {
        if (!working.compareAndSet(false, true)) return
        launch("E2-LOADBOOK") {
            stage("解析电子书…")
            val name = displayName(uri, "book.txt")
            val raw = readUriBytes(uri, TransferMedia.MAX_BOOK_BYTES * 2)
            val prepared = TransferMedia.prepareTxtBook(name, raw)
            main.post {
                bookDraft.value = prepared
                bookTitle.value = prepared.defaultTitle
            }
            UiState()
        }
    }

    private fun loadLockImage(uri: Uri) {
        if (!working.compareAndSet(false, true)) return
        launch("E2-LOADIMG") {
            stage("读取图片…")
            val name = displayName(uri, "lock.png")
            val bmp = TransferMedia.loadBitmap(contentResolver, uri)
            main.post {
                lockEditor.value = LockEditorSession(
                    fileName = name,
                    source = bmp,
                    initialParams = LockEditParams(),
                )
            }
            UiState()
        }
    }

    private fun readUriBytes(uri: Uri, maxBytes: Int): ByteArray {
        return contentResolver.openInputStream(uri)?.use { input ->
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                out.write(buffer, 0, count)
                if (out.size() > maxBytes) break
            }
            out.toByteArray()
        } ?: throw IOException("无法读取文件")
    }

    private fun displayName(uri: Uri, fallback: String): String {
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (index >= 0) cursor.getString(index)?.let { return it }
                }
            }
        } catch (_: RuntimeException) {
            // 回退至 URI 末尾段名
        }
        return uri.lastPathSegment ?: fallback
    }

    private fun launch(name: String, job: () -> UiState) {
        val thread = Thread({
            val result = try {
                job()
            } catch (e: IOException) {
                UiState(title = "失败", detail = e.message ?: "I/O 错误", tone = Tone.ERROR)
            } catch (e: RuntimeException) {
                UiState(title = "失败", detail = e.message ?: "未知错误", tone = Tone.ERROR)
            }
            working.set(false)
            main.post {
                if (isDestroyed) return@post
                worker = null
                setBusy(false)
                state.value = result
            }
        }, name)
        worker = thread
        setBusy(true)
        state.value = UiState(busy = true, title = "准备…", detail = "")
        thread.start()
    }

    /** 解析 ESP-IDF 镜像头部偏移 32 字节处的 esp_app_desc_t 描述信息（含 32 B app_elf_sha256）。 */
    private fun parseEspAppDesc(bytes: ByteArray): Triple<String?, String?, ByteArray?> {
        if (bytes.size < 176) return Triple(null, null, null)
        val isDesc = (bytes[32].toInt() and 0xFF) == 0x32 &&
            (bytes[33].toInt() and 0xFF) == 0x54 &&
            (bytes[34].toInt() and 0xFF) == 0xCD &&
            (bytes[35].toInt() and 0xFF) == 0xAB
        if (!isDesc) return Triple(null, null, null)
        val version = extractCStr(bytes, 32 + 16, 32)
        val project = extractCStr(bytes, 32 + 48, 32)
        val elfSha = bytes.copyOfRange(32 + 112, 32 + 112 + 32)
        return Triple(project, version, elfSha)
    }

    private fun extractCStr(bytes: ByteArray, offset: Int, maxLen: Int): String? {
        if (offset + maxLen > bytes.size) return null
        var end = offset
        val limit = offset + maxLen
        while (end < limit && bytes[end].toInt() != 0) {
            val ch = bytes[end].toInt() and 0xFF
            if (ch < 0x20 || ch > 0x7E) return null
            end++
        }
        if (end == offset) return null
        return String(bytes, offset, end - offset, StandardCharsets.US_ASCII).trim().ifEmpty { null }
    }

    private fun sha256Hex(data: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(data)
        val out = StringBuilder(digest.size * 2)
        for (b in digest) {
            out.append("0123456789abcdef"[(b.toInt() shr 4) and 15])
            out.append("0123456789abcdef"[b.toInt() and 15])
        }
        return out.toString()
    }

    private fun stage(title: String, detail: String = "", progress: Float? = null) {
        main.post {
            if (isDestroyed) return@post
            state.value = UiState(busy = true, title = title, detail = detail, progress = progress)
        }
    }

    private fun setBusy(busy: Boolean) {
        if (busy) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    override fun onDestroy() {
        stopScan()
        if (isFinishing) worker?.interrupt()
        super.onDestroy()
    }

    @SuppressLint("MissingPermission")
    private inner class SessionConnector(private val selected: SelectedDevice) : A5Protocol.Connector {
        override fun open(verification: Boolean): A5Protocol.Session {
            if (Thread.currentThread().isInterrupted) throw InterruptedIOException("已取消")
            if (verification) {
                stage("等待重启…")
            } else {
                stage("连接设备…")
            }
            return try {
                A5Flasher.open(applicationContext, selected.device, { }, verification)
            } catch (_: SecurityException) {
                throw IOException("缺少蓝牙权限")
            } catch (_: RuntimeException) {
                throw IOException("蓝牙连接失败")
            }
        }
    }
}

private fun formatSize(bytes: Int): String = when {
    bytes < 1024 -> "$bytes B"
    bytes < 1024 * 1024 -> String.format(Locale.US, "%.1f KB", bytes / 1024.0)
    else -> String.format(Locale.US, "%.2f MB", bytes / (1024.0 * 1024.0))
}

/* ---------------- 主题与色板（与原 UI 规范 100% 一致） ---------------- */

private data class ExtendedColors(
    val cardBorder: Color,
    val subSurface: Color,
    val success: Color,
    val successContainer: Color,
    val warning: Color,
    val warningContainer: Color,
    val errorContainer: Color,
)

private val LightColors = lightColorScheme(
    primary = Color(0xFFEA580C),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFFFEDD5),
    onPrimaryContainer = Color(0xFF7C2D12),
    secondary = Color(0xFF52525B),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFF0EFEA),
    onSecondaryContainer = Color(0xFF27272A),
    tertiary = Color(0xFFB45309),
    onTertiary = Color.White,
    background = Color(0xFFF5F5F3),
    onBackground = Color(0xFF18181B),
    surface = Color(0xFFFFFFFF),
    onSurface = Color(0xFF18181B),
    surfaceVariant = Color(0xFFECECE7),
    onSurfaceVariant = Color(0xFF52525B),
    outline = Color(0xFFE5E5E0),
    error = Color(0xFFDC2626),
    onError = Color.White,
)

private val DarkColors = darkColorScheme(
    primary = Color(0xFFFB923C),
    onPrimary = Color(0xFF1C0D02),
    primaryContainer = Color(0xFF431D08),
    onPrimaryContainer = Color(0xFFFED7AA),
    secondary = Color(0xFFA1A1AA),
    onSecondary = Color(0xFF18181B),
    secondaryContainer = Color(0xFF262930),
    onSecondaryContainer = Color(0xFFE4E4E7),
    tertiary = Color(0xFFFBBF24),
    onTertiary = Color(0xFF261700),
    background = Color(0xFF121315),
    onBackground = Color(0xFFF4F4F5),
    surface = Color(0xFF1C1E22),
    onSurface = Color(0xFFF4F4F5),
    surfaceVariant = Color(0xFF262930),
    onSurfaceVariant = Color(0xFFA1A1AA),
    outline = Color(0xFF2B2E35),
    error = Color(0xFFF87171),
    onError = Color(0xFF450A0A),
)

private val LightExtended = ExtendedColors(
    cardBorder = Color(0xFFE5E5E0),
    subSurface = Color(0xFFF8F8F6),
    success = Color(0xFF15803D),
    successContainer = Color(0xFFDCFCE7),
    warning = Color(0xFFB45309),
    warningContainer = Color(0xFFFEF3C7),
    errorContainer = Color(0xFFFEE2E2),
)

private val DarkExtended = ExtendedColors(
    cardBorder = Color(0xFF2B2E35),
    subSurface = Color(0xFF16181B),
    success = Color(0xFF4ADE80),
    successContainer = Color(0xFF143320),
    warning = Color(0xFFFBBF24),
    warningContainer = Color(0xFF3B270B),
    errorContainer = Color(0xFF3B1214),
)

private val LocalExtendedColors = staticCompositionLocalOf { LightExtended }

@Composable
private fun E2sTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    CompositionLocalProvider(LocalExtendedColors provides if (dark) DarkExtended else LightExtended) {
        MaterialTheme(
            colorScheme = if (dark) DarkColors else LightColors,
            content = content,
        )
    }
}

/* ---------------- 主框架：顶栏 + 内容 + 底部双 Tab ---------------- */

@Composable
private fun ToolboxScreen(
    versionLabel: String,
    tab: AppTab,
    state: UiState,
    target: SelectedDevice?,
    devices: List<ScanEntry>,
    scanning: Boolean,
    scanned: Boolean,
    firmware: FirmwareInfo?,
    book: PreparedBook?,
    bookTitle: String,
    lockDraft: LockImageDraft?,
    onSelectTab: (AppTab) -> Unit,
    onScan: () -> Unit,
    onStopScan: () -> Unit,
    onSelectDevice: (ScanEntry) -> Unit,
    onChangeDevice: () -> Unit,
    onPickFirmware: () -> Unit,
    onFlash: () -> Unit,
    onPickBook: () -> Unit,
    onBookTitleChange: (String) -> Unit,
    onSendBook: () -> Unit,
    onPickLockImage: () -> Unit,
    onEditLockImage: () -> Unit,
    onSendLockImage: () -> Unit,
    onCancel: () -> Unit,
    onDismissStatus: () -> Unit,
) {
    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = { AppTopBar(versionLabel = versionLabel) },
        bottomBar = {
            BottomTabBar(
                current = tab,
                enabled = !state.busy,
                onSelect = onSelectTab,
            )
        },
    ) { inner ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(inner)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            AnimatedVisibility(
                visible = state.busy || state.tone != Tone.NEUTRAL || state.detail.isNotEmpty(),
                enter = fadeIn() + expandVertically(),
                exit = fadeOut() + shrinkVertically(),
            ) {
                StatusBanner(state = state, onCancel = onCancel, onDismiss = onDismissStatus)
            }

            when (tab) {
                AppTab.FLASH -> {
                    DeviceSectionCard(
                        readerOnly = false,
                        step = 1,
                        target = target,
                        devices = devices,
                        scanning = scanning,
                        scanned = scanned,
                        busy = state.busy,
                        onScan = onScan,
                        onStopScan = onStopScan,
                        onSelect = onSelectDevice,
                        onChangeDevice = onChangeDevice,
                    )
                    FirmwareSectionCard(
                        firmware = firmware,
                        busy = state.busy,
                        onPickFirmware = onPickFirmware,
                    )
                    Spacer(Modifier.height(2.dp))
                    PrimaryFullButton(
                        label = "刷入固件",
                        enabled = target != null && firmware != null && !state.busy,
                        onClick = onFlash,
                    )
                }

                AppTab.TRANSFER -> {
                    val readerTarget = target?.takeIf { it.model == DeviceModel.READER }
                    val readerDevices = devices.filter { it.model == DeviceModel.READER }
                    DeviceSectionCard(
                        readerOnly = true,
                        step = null,
                        target = readerTarget,
                        devices = readerDevices,
                        scanning = scanning,
                        scanned = scanned,
                        busy = state.busy,
                        onScan = onScan,
                        onStopScan = onStopScan,
                        onSelect = onSelectDevice,
                        onChangeDevice = onChangeDevice,
                    )
                    BookTransferCard(
                        book = book,
                        title = bookTitle,
                        readerReady = readerTarget != null,
                        busy = state.busy,
                        onPickBook = onPickBook,
                        onTitleChange = onBookTitleChange,
                        onSendBook = onSendBook,
                    )
                    LockImageTransferCard(
                        draft = lockDraft,
                        readerReady = readerTarget != null,
                        busy = state.busy,
                        onPickImage = onPickLockImage,
                        onEditImage = onEditLockImage,
                        onSend = onSendLockImage,
                    )
                }
            }
        }
    }
}

@Composable
private fun AppTopBar(versionLabel: String) {
    val ext = LocalExtendedColors.current
    Surface(color = MaterialTheme.colorScheme.surface) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .statusBarsPadding()
                    .padding(horizontal = 20.dp, vertical = 14.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "E2 / E2S 工具箱",
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f),
                )
                Surface(
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant,
                ) {
                    Text(
                        text = versionLabel,
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    )
                }
            }
            HorizontalDivider(color = ext.cardBorder)
        }
    }
}

@Composable
private fun BottomTabBar(
    current: AppTab,
    enabled: Boolean,
    onSelect: (AppTab) -> Unit,
) {
    val ext = LocalExtendedColors.current
    Surface(color = MaterialTheme.colorScheme.surface) {
        Column(modifier = Modifier.fillMaxWidth()) {
            HorizontalDivider(color = ext.cardBorder)
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(horizontal = 20.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                AppTab.entries.forEach { tab ->
                    val selected = tab == current
                    val fg = if (selected) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    }
                    Column(
                        modifier = Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(14.dp))
                            .clickable(enabled = enabled) { onSelect(tab) }
                            .padding(vertical = 4.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(3.dp),
                    ) {
                        Box(
                            modifier = Modifier
                                .width(56.dp)
                                .height(30.dp)
                                .background(
                                    color = if (selected) {
                                        MaterialTheme.colorScheme.primaryContainer
                                    } else {
                                        Color.Transparent
                                    },
                                    shape = RoundedCornerShape(15.dp),
                                ),
                            contentAlignment = Alignment.Center,
                        ) {
                            when (tab) {
                                AppTab.FLASH -> ChipIcon(color = fg, modifier = Modifier.size(18.dp))
                                AppTab.TRANSFER -> TransferIcon(color = fg, modifier = Modifier.size(18.dp))
                            }
                        }
                        Text(
                            text = tab.label,
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                            color = fg,
                        )
                    }
                }
            }
        }
    }
}

/* ---------------- 通用基础组件 ---------------- */

@Composable
private fun PrimaryFullButton(
    label: String,
    enabled: Boolean,
    onClick: () -> Unit,
    compact: Boolean = false,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        shape = RoundedCornerShape(if (compact) 12.dp else 14.dp),
        contentPadding = PaddingValues(horizontal = 16.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = MaterialTheme.colorScheme.primary,
            contentColor = MaterialTheme.colorScheme.onPrimary,
        ),
        modifier = Modifier
            .fillMaxWidth()
            .height(if (compact) 44.dp else 52.dp),
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

@Composable
private fun CardActionButton(
    label: String,
    enabled: Boolean,
    primary: Boolean = false,
    onClick: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    val shape = RoundedCornerShape(10.dp)
    val modifier = Modifier
        .width(88.dp)
        .height(36.dp)
    if (primary) {
        Button(
            onClick = onClick,
            enabled = enabled,
            shape = shape,
            contentPadding = PaddingValues(0.dp),
            elevation = ButtonDefaults.buttonElevation(defaultElevation = 1.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.primary,
                contentColor = MaterialTheme.colorScheme.onPrimary,
            ),
            modifier = modifier,
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
            )
        }
    } else {
        OutlinedButton(
            onClick = onClick,
            enabled = enabled,
            shape = shape,
            border = BorderStroke(
                1.dp,
                if (enabled) ext.cardBorder else ext.cardBorder.copy(alpha = 0.45f),
            ),
            contentPadding = PaddingValues(0.dp),
            modifier = modifier,
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelLarge,
                fontWeight = FontWeight.Medium,
                color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun SmallOutlinedButton(label: String, enabled: Boolean = true, onClick: () -> Unit) {
    val ext = LocalExtendedColors.current
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        shape = RoundedCornerShape(8.dp),
        border = BorderStroke(1.dp, if (enabled) ext.cardBorder else ext.cardBorder.copy(alpha = 0.45f)),
        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 0.dp),
        modifier = Modifier.height(32.dp),
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.Medium,
            color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
            maxLines = 1,
        )
    }
}

@Composable
private fun ToolboxCard(
    leading: @Composable () -> Unit,
    title: String,
    extraAction: (@Composable () -> Unit)? = null,
    action: @Composable () -> Unit,
    content: @Composable () -> Unit,
) {
    val ext = LocalExtendedColors.current
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, ext.cardBorder),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 18.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                leading()
                Spacer(Modifier.width(10.dp))
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f),
                )
                if (extraAction != null) {
                    extraAction()
                    Spacer(Modifier.width(8.dp))
                }
                action()
            }
            content()
        }
    }
}

@Composable
private fun StepBadge(step: Int, completed: Boolean) {
    val bg = if (completed) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant
    val fg = if (completed) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant
    Box(
        modifier = Modifier
            .size(24.dp)
            .background(bg, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        if (completed) {
            CheckIcon(color = fg, modifier = Modifier.size(13.dp))
        } else {
            Text(
                text = step.toString(),
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.Bold,
                color = fg,
            )
        }
    }
}

@Composable
private fun CardIconBadge(active: Boolean, icon: @Composable (Color) -> Unit) {
    val bg = if (active) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant
    val fg = if (active) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant
    Box(
        modifier = Modifier
            .size(26.dp)
            .background(bg, RoundedCornerShape(8.dp)),
        contentAlignment = Alignment.Center,
    ) {
        icon(fg)
    }
}

@Composable
private fun StatusBanner(
    state: UiState,
    onCancel: () -> Unit,
    onDismiss: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    val (bg, accent) = when (state.tone) {
        Tone.SUCCESS -> ext.successContainer to ext.success
        Tone.WARNING -> ext.warningContainer to ext.warning
        Tone.ERROR -> ext.errorContainer to MaterialTheme.colorScheme.error
        Tone.NEUTRAL -> MaterialTheme.colorScheme.surface to MaterialTheme.colorScheme.primary
    }
    val borderColor = if (state.tone == Tone.NEUTRAL) ext.cardBorder else accent.copy(alpha = 0.28f)

    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = bg,
        border = BorderStroke(1.dp, borderColor),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 18.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                StatusIcon(tone = state.tone, busy = state.busy, tint = accent, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(10.dp))
                Text(
                    text = state.title,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f),
                )
                val progress = state.progress
                if (state.busy && progress != null) {
                    Text(
                        text = "${(progress.coerceIn(0f, 1f) * 100).roundToInt()}%",
                        style = MaterialTheme.typography.labelLarge,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        color = accent,
                    )
                } else if (!state.busy) {
                    SmallOutlinedButton(label = "关闭", onClick = onDismiss)
                }
            }

            if (state.detail.isNotEmpty() || state.busy) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (state.detail.isNotEmpty()) {
                        Text(
                            text = state.detail,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.weight(1f),
                        )
                    } else {
                        Spacer(Modifier.weight(1f))
                    }
                    if (state.busy) {
                        Spacer(Modifier.width(12.dp))
                        SmallOutlinedButton(label = "取消", onClick = onCancel)
                    }
                }
            }

            if (state.busy) {
                val progress = state.progress
                val track = MaterialTheme.colorScheme.surfaceVariant
                if (progress != null) {
                    LinearProgressIndicator(
                        progress = { progress.coerceIn(0f, 1f) },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(6.dp)
                            .clip(CircleShape),
                        color = accent,
                        trackColor = track,
                    )
                } else {
                    LinearProgressIndicator(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(6.dp)
                            .clip(CircleShape),
                        color = accent,
                        trackColor = track,
                    )
                }
            }
        }
    }
}

/* ---------------- 设备卡片（刷机/传输共用） ---------------- */

@Composable
private fun DeviceSectionCard(
    readerOnly: Boolean,
    step: Int?,
    target: SelectedDevice?,
    devices: List<ScanEntry>,
    scanning: Boolean,
    scanned: Boolean,
    busy: Boolean,
    onScan: () -> Unit,
    onStopScan: () -> Unit,
    onSelect: (ScanEntry) -> Unit,
    onChangeDevice: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    ToolboxCard(
        leading = {
            if (step != null) {
                StepBadge(step = step, completed = target != null)
            } else {
                CardIconBadge(active = target != null) { fg ->
                    BluetoothIcon(color = fg, modifier = Modifier.size(15.dp))
                }
            }
        },
        title = if (readerOnly) "阅读器" else "设备",
        action = {
            when {
                scanning -> CardActionButton("停止", enabled = !busy, primary = true, onClick = onStopScan)
                target != null -> CardActionButton("更换", enabled = !busy, onClick = onChangeDevice)
                scanned -> CardActionButton("重新扫描", enabled = !busy, primary = true, onClick = onScan)
                else -> CardActionButton("扫描", enabled = !busy, primary = true, onClick = onScan)
            }
        },
    ) {
        if (target != null) {
            Surface(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(12.dp),
                color = ext.subSurface,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.45f)),
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 14.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(
                        modifier = Modifier
                            .size(36.dp)
                            .background(MaterialTheme.colorScheme.primaryContainer, RoundedCornerShape(10.dp)),
                        contentAlignment = Alignment.Center,
                    ) {
                        BluetoothIcon(
                            color = MaterialTheme.colorScheme.onPrimaryContainer,
                            modifier = Modifier.size(18.dp),
                        )
                    }
                    Spacer(Modifier.width(12.dp))
                    Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                text = target.name,
                                style = MaterialTheme.typography.bodyLarge,
                                fontWeight = FontWeight.Medium,
                                color = MaterialTheme.colorScheme.onSurface,
                            )
                            if (!readerOnly && target.model == DeviceModel.READER) {
                                Spacer(Modifier.width(8.dp))
                                ReaderTag()
                            }
                        }
                        Text(
                            text = target.device.address,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            return@ToolboxCard
        }

        if (scanning) {
            LinearProgressIndicator(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(4.dp)
                    .clip(CircleShape),
                color = MaterialTheme.colorScheme.primary,
                trackColor = MaterialTheme.colorScheme.surfaceVariant,
            )
            Text(
                text = "扫描中…",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        if (devices.isNotEmpty()) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                devices.forEach { entry ->
                    Surface(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(12.dp))
                            .clickable(enabled = !busy) { onSelect(entry) },
                        shape = RoundedCornerShape(12.dp),
                        color = ext.subSurface,
                        border = BorderStroke(1.dp, ext.cardBorder),
                    ) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(horizontal = 14.dp, vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Box(
                                modifier = Modifier
                                    .size(34.dp)
                                    .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(9.dp)),
                                contentAlignment = Alignment.Center,
                            ) {
                                BluetoothIcon(
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier.size(16.dp),
                                )
                            }
                            Spacer(Modifier.width(12.dp))
                            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text(
                                        text = entry.name,
                                        style = MaterialTheme.typography.bodyLarge,
                                        fontWeight = FontWeight.Medium,
                                        color = MaterialTheme.colorScheme.onSurface,
                                    )
                                    if (!readerOnly && entry.model == DeviceModel.READER) {
                                        Spacer(Modifier.width(8.dp))
                                        ReaderTag()
                                    }
                                }
                                Text(
                                    text = entry.device.address,
                                    style = MaterialTheme.typography.bodySmall,
                                    fontFamily = FontFamily.Monospace,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(3.dp)) {
                                SignalBarsIcon(
                                    rssi = entry.rssi,
                                    activeColor = MaterialTheme.colorScheme.primary,
                                    inactiveColor = MaterialTheme.colorScheme.surfaceVariant,
                                    modifier = Modifier.size(width = 18.dp, height = 13.dp),
                                )
                                Text(
                                    text = "${entry.rssi} dBm",
                                    style = MaterialTheme.typography.labelSmall,
                                    fontFamily = FontFamily.Monospace,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
            }
        } else if (!scanning) {
            EmptySlotBox(
                enabled = !busy,
                text = when {
                    readerOnly && scanned -> "未找到阅读器，请确认已开启蓝牙"
                    readerOnly -> "开启阅读器蓝牙后点击扫描"
                    scanned -> "未找到设备"
                    else -> "开启蓝牙后点击扫描"
                },
                icon = { fg -> BluetoothIcon(color = fg, modifier = Modifier.size(16.dp)) },
                onClick = onScan,
            )
        }
    }
}

@Composable
private fun EmptySlotBox(
    enabled: Boolean,
    text: String,
    icon: @Composable (Color) -> Unit,
    onClick: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .clickable(enabled = enabled, onClick = onClick),
        shape = RoundedCornerShape(12.dp),
        color = ext.subSurface,
        border = BorderStroke(1.dp, ext.cardBorder),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                modifier = Modifier
                    .size(34.dp)
                    .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(9.dp)),
                contentAlignment = Alignment.Center,
            ) {
                icon(MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.width(12.dp))
            Text(
                text = text,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ReaderTag() {
    val ext = LocalExtendedColors.current
    Surface(shape = RoundedCornerShape(6.dp), color = ext.successContainer) {
        Text(
            text = "阅读器",
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            color = ext.success,
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 2.dp),
        )
    }
}

/* ---------------- Tab 1：固件卡片 ---------------- */

@Composable
private fun FirmwareSectionCard(
    firmware: FirmwareInfo?,
    busy: Boolean,
    onPickFirmware: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    ToolboxCard(
        leading = { StepBadge(step = 2, completed = firmware != null) },
        title = "固件",
        action = {
            CardActionButton(
                label = if (firmware == null) "选择文件" else "更换",
                enabled = !busy,
                primary = firmware == null,
                onClick = onPickFirmware,
            )
        },
    ) {
        if (firmware == null) {
            EmptySlotBox(
                enabled = !busy,
                text = "选择本地 .bin 文件",
                icon = { fg -> ChipIcon(color = fg, modifier = Modifier.size(18.dp)) },
                onClick = onPickFirmware,
            )
            return@ToolboxCard
        }

        Surface(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(12.dp),
            color = ext.subSurface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.45f)),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 14.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Box(
                    modifier = Modifier
                        .size(36.dp)
                        .background(MaterialTheme.colorScheme.primaryContainer, RoundedCornerShape(10.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    ChipIcon(
                        color = MaterialTheme.colorScheme.onPrimaryContainer,
                        modifier = Modifier.size(18.dp),
                    )
                }
                Spacer(Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Text(
                        text = firmware.fileName,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    val meta = buildList {
                        firmware.projectName?.let { add(it) }
                        firmware.version?.let { add("v$it") }
                        add(formatSize(firmware.bytes.size))
                        add("SHA ${firmware.sha256.take(8)}…")
                    }.joinToString(" · ")
                    Text(
                        text = meta,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/* ---------------- Tab 2：电子书与锁屏图片传输卡片 ---------------- */

@Composable
private fun BookTransferCard(
    book: PreparedBook?,
    title: String,
    readerReady: Boolean,
    busy: Boolean,
    onPickBook: () -> Unit,
    onTitleChange: (String) -> Unit,
    onSendBook: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    ToolboxCard(
        leading = {
            CardIconBadge(active = book != null) { fg ->
                BookIcon(color = fg, modifier = Modifier.size(15.dp))
            }
        },
        title = "电子书",
        action = {
            CardActionButton(
                label = if (book == null) "选择 TXT" else "更换",
                enabled = !busy,
                primary = book == null,
                onClick = onPickBook,
            )
        },
    ) {
        if (book == null) {
            EmptySlotBox(
                enabled = !busy,
                text = "选择本地 .txt 文件",
                icon = { fg -> BookIcon(color = fg, modifier = Modifier.size(16.dp)) },
                onClick = onPickBook,
            )
            return@ToolboxCard
        }

        Surface(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(12.dp),
            color = ext.subSurface,
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.45f)),
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 14.dp, vertical = 12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = "书名",
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(36.dp),
                    )
                    BasicTextField(
                        value = title,
                        onValueChange = { if (it.length <= 24) onTitleChange(it) },
                        enabled = !busy,
                        singleLine = true,
                        textStyle = MaterialTheme.typography.bodyLarge.copy(
                            color = MaterialTheme.colorScheme.onSurface,
                            fontWeight = FontWeight.Medium,
                        ),
                        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
                        modifier = Modifier.weight(1f),
                    )
                }
                HorizontalDivider(color = ext.cardBorder)
                val encDesc = if (book.enc == ReaderTransfer.ENC_ASCII) {
                    "ASCII"
                } else if (book.sourceEncoding == "GBK") {
                    "GB2312"
                } else {
                    "${book.sourceEncoding} → GB2312"
                }
                Text(
                    text = "$encDesc · ${formatSize(book.bytes.size)}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }

        PrimaryFullButton(
            label = "发送电子书",
            enabled = readerReady && !busy,
            compact = true,
            onClick = onSendBook,
        )
    }
}

@Composable
private fun LockImageTransferCard(
    draft: LockImageDraft?,
    readerReady: Boolean,
    busy: Boolean,
    onPickImage: () -> Unit,
    onEditImage: () -> Unit,
    onSend: () -> Unit,
) {
    ToolboxCard(
        leading = {
            CardIconBadge(active = draft != null) { fg ->
                ImageIcon(color = fg, modifier = Modifier.size(15.dp))
            }
        },
        title = "锁屏图片",
        extraAction = if (draft != null) {
            { SmallOutlinedButton(label = "编辑", enabled = !busy, onClick = onEditImage) }
        } else {
            null
        },
        action = {
            CardActionButton(
                label = if (draft == null) "选择图片" else "更换",
                enabled = !busy,
                primary = draft == null,
                onClick = onPickImage,
            )
        },
    ) {
        if (draft == null) {
            EmptySlotBox(
                enabled = !busy,
                text = "选择相册图片并编辑",
                icon = { fg -> ImageIcon(color = fg, modifier = Modifier.size(16.dp)) },
                onClick = onPickImage,
            )
            return@ToolboxCard
        }

        val previewBmp = remember(draft.rendered.preview) { draft.rendered.preview.asImageBitmap() }

        Surface(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(TransferMedia.LOCK_W.toFloat() / TransferMedia.LOCK_VISIBLE_H.toFloat())
                .clip(RoundedCornerShape(12.dp))
                .clickable(enabled = !busy, onClick = onEditImage),
            shape = RoundedCornerShape(12.dp),
            color = Color(0xFFFAFAF7),
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.45f)),
        ) {
            Image(
                bitmap = previewBmp,
                contentDescription = "锁屏预览",
                contentScale = ContentScale.FillBounds,
                filterQuality = FilterQuality.None,
                modifier = Modifier.fillMaxSize(),
            )
        }

        PrimaryFullButton(
            label = "设为锁屏图",
            enabled = readerReady && !busy,
            compact = true,
            onClick = onSend,
        )
    }
}

/* ---------------- 弹窗：刷入确认 ---------------- */

@Composable
private fun ConfirmDialog(
    target: SelectedDevice?,
    firmware: FirmwareInfo?,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = MaterialTheme.colorScheme.surface,
        title = {
            Text(
                text = "刷入固件？",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
            )
        },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Surface(
                    shape = RoundedCornerShape(12.dp),
                    color = ext.subSurface,
                    border = BorderStroke(1.dp, ext.cardBorder),
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(12.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        if (target != null) {
                            DialogMetaRow("设备", target.name)
                            DialogMetaRow("地址", target.device.address, mono = true)
                        }
                        if (firmware != null) {
                            DialogMetaRow("固件", firmware.fileName)
                            DialogMetaRow("大小", formatSize(firmware.bytes.size), mono = true)
                            DialogMetaRow("SHA", "${firmware.sha256.take(12)}…", mono = true)
                        }
                    }
                }
                Text(
                    text = if (target?.model == DeviceModel.READER) {
                        "原地更新阅读器固件并重启，不动原厂槽，电量需 ≥20%。"
                    } else {
                        "写入非运行 OTA 槽并重启，电量需 ≥20%。"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            Button(onClick = onConfirm, shape = RoundedCornerShape(10.dp)) {
                Text("刷入")
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text("取消")
            }
        },
    )
}

@Composable
private fun DialogMetaRow(label: String, value: String, mono: Boolean = false) {
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(64.dp),
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodySmall,
            fontFamily = if (mono) FontFamily.Monospace else FontFamily.Default,
            fontWeight = FontWeight.Medium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
    }
}

/* ---------------- 自绘极简矢量图标组件（零额外依赖） ---------------- */

@Composable
private fun CheckIcon(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val path = Path().apply {
            moveTo(size.width * 0.18f, size.height * 0.54f)
            lineTo(size.width * 0.42f, size.height * 0.78f)
            lineTo(size.width * 0.84f, size.height * 0.24f)
        }
        drawPath(
            path = path,
            color = color,
            style = Stroke(
                width = size.minDimension * 0.17f,
                cap = StrokeCap.Round,
                join = StrokeJoin.Round,
            ),
        )
    }
}

@Composable
private fun BluetoothIcon(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val cx = size.width * 0.48f
        val path = Path().apply {
            moveTo(cx - size.width * 0.22f, size.height * 0.30f)
            lineTo(cx + size.width * 0.24f, size.height * 0.70f)
            lineTo(cx, size.height * 0.88f)
            lineTo(cx, size.height * 0.12f)
            lineTo(cx + size.width * 0.24f, size.height * 0.30f)
            lineTo(cx - size.width * 0.22f, size.height * 0.70f)
        }
        drawPath(
            path = path,
            color = color,
            style = Stroke(
                width = size.minDimension * 0.12f,
                cap = StrokeCap.Round,
                join = StrokeJoin.Round,
            ),
        )
    }
}

@Composable
private fun ChipIcon(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val stroke = size.minDimension * 0.10f
        val pad = size.minDimension * 0.22f
        val innerSize = size.minDimension - pad * 2f
        drawRoundRect(
            color = color,
            topLeft = Offset(pad, pad),
            size = Size(innerSize, innerSize),
            cornerRadius = CornerRadius(size.minDimension * 0.14f),
            style = Stroke(width = stroke),
        )
        drawRoundRect(
            color = color,
            topLeft = Offset(size.width * 0.38f, size.height * 0.38f),
            size = Size(size.width * 0.24f, size.height * 0.24f),
            cornerRadius = CornerRadius(size.minDimension * 0.05f),
        )
        val pinOffsets = listOf(0.36f, 0.64f)
        for (p in pinOffsets) {
            drawLine(color, Offset(size.width * p, pad * 0.35f), Offset(size.width * p, pad), stroke, StrokeCap.Round)
            drawLine(color, Offset(size.width * p, size.height - pad), Offset(size.width * p, size.height - pad * 0.35f), stroke, StrokeCap.Round)
            drawLine(color, Offset(pad * 0.35f, size.height * p), Offset(pad, size.height * p), stroke, StrokeCap.Round)
            drawLine(color, Offset(size.width - pad, size.height * p), Offset(size.width - pad * 0.35f, size.height * p), stroke, StrokeCap.Round)
        }
    }
}

@Composable
private fun TransferIcon(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val stroke = size.minDimension * 0.11f
        val s = Stroke(width = stroke, cap = StrokeCap.Round, join = StrokeJoin.Round)
        // 右上发送箭头
        val up = Path().apply {
            moveTo(size.width * 0.20f, size.height * 0.34f)
            lineTo(size.width * 0.78f, size.height * 0.34f)
            moveTo(size.width * 0.58f, size.height * 0.16f)
            lineTo(size.width * 0.78f, size.height * 0.34f)
            lineTo(size.width * 0.58f, size.height * 0.52f)
        }
        drawPath(up, color = color, style = s)
        // 左下回传箭头
        val down = Path().apply {
            moveTo(size.width * 0.80f, size.height * 0.66f)
            lineTo(size.width * 0.22f, size.height * 0.66f)
            moveTo(size.width * 0.42f, size.height * 0.48f)
            lineTo(size.width * 0.22f, size.height * 0.66f)
            lineTo(size.width * 0.42f, size.height * 0.84f)
        }
        drawPath(down, color = color, style = s)
    }
}

@Composable
private fun BookIcon(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val stroke = size.minDimension * 0.11f
        val s = Stroke(width = stroke, cap = StrokeCap.Round, join = StrokeJoin.Round)
        val p = Path().apply {
            // 左页
            moveTo(size.width * 0.50f, size.height * 0.24f)
            lineTo(size.width * 0.16f, size.height * 0.18f)
            lineTo(size.width * 0.16f, size.height * 0.76f)
            lineTo(size.width * 0.50f, size.height * 0.82f)
            // 右页
            lineTo(size.width * 0.84f, size.height * 0.76f)
            lineTo(size.width * 0.84f, size.height * 0.18f)
            lineTo(size.width * 0.50f, size.height * 0.24f)
            lineTo(size.width * 0.50f, size.height * 0.82f)
        }
        drawPath(p, color = color, style = s)
    }
}

@Composable
private fun ImageIcon(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val stroke = size.minDimension * 0.11f
        val pad = size.minDimension * 0.14f
        drawRoundRect(
            color = color,
            topLeft = Offset(pad, pad),
            size = Size(size.width - pad * 2, size.height - pad * 2),
            cornerRadius = CornerRadius(size.minDimension * 0.16f),
            style = Stroke(width = stroke),
        )
        drawCircle(
            color = color,
            radius = size.minDimension * 0.08f,
            center = Offset(size.width * 0.36f, size.height * 0.38f),
        )
        val hill = Path().apply {
            moveTo(size.width * 0.22f, size.height * 0.74f)
            lineTo(size.width * 0.44f, size.height * 0.52f)
            lineTo(size.width * 0.58f, size.height * 0.64f)
            lineTo(size.width * 0.70f, size.height * 0.48f)
            lineTo(size.width * 0.82f, size.height * 0.74f)
        }
        drawPath(
            hill,
            color = color,
            style = Stroke(width = stroke, cap = StrokeCap.Round, join = StrokeJoin.Round),
        )
    }
}

@Composable
private fun SignalBarsIcon(
    rssi: Int,
    activeColor: Color,
    inactiveColor: Color,
    modifier: Modifier = Modifier,
) {
    val level = when {
        rssi >= -60 -> 4
        rssi >= -72 -> 3
        rssi >= -84 -> 2
        else -> 1
    }
    Canvas(modifier = modifier) {
        val barWidth = size.width / 7f
        val gap = barWidth
        for (i in 0 until 4) {
            val hRatio = (i + 1) * 0.25f
            val h = size.height * hRatio
            val x = i * (barWidth + gap)
            drawRoundRect(
                color = if (i < level) activeColor else inactiveColor,
                topLeft = Offset(x, size.height - h),
                size = Size(barWidth, h),
                cornerRadius = CornerRadius(barWidth / 2f),
            )
        }
    }
}

@Composable
private fun StatusIcon(tone: Tone, busy: Boolean, tint: Color, modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val stroke = size.minDimension * 0.13f
        val r = size.minDimension * 0.44f
        val center = Offset(size.width / 2f, size.height / 2f)
        if (busy) {
            drawCircle(
                color = tint.copy(alpha = 0.25f),
                radius = r,
                center = center,
                style = Stroke(width = stroke),
            )
            drawCircle(
                color = tint,
                radius = r * 0.42f,
                center = center,
            )
            return@Canvas
        }
        when (tone) {
            Tone.SUCCESS -> {
                drawCircle(color = tint, radius = r, center = center, style = Stroke(width = stroke))
                val p = Path().apply {
                    moveTo(size.width * 0.29f, size.height * 0.52f)
                    lineTo(size.width * 0.44f, size.height * 0.67f)
                    lineTo(size.width * 0.72f, size.height * 0.35f)
                }
                drawPath(p, color = tint, style = Stroke(stroke, cap = StrokeCap.Round, join = StrokeJoin.Round))
            }
            Tone.WARNING -> {
                drawCircle(color = tint, radius = r, center = center, style = Stroke(width = stroke))
                drawLine(tint, Offset(center.x, size.height * 0.28f), Offset(center.x, size.height * 0.56f), stroke, StrokeCap.Round)
                drawCircle(tint, radius = stroke * 0.65f, center = Offset(center.x, size.height * 0.72f))
            }
            Tone.ERROR -> {
                drawCircle(color = tint, radius = r, center = center, style = Stroke(width = stroke))
                val d = r * 0.44f
                drawLine(tint, Offset(center.x - d, center.y - d), Offset(center.x + d, center.y + d), stroke, StrokeCap.Round)
                drawLine(tint, Offset(center.x + d, center.y - d), Offset(center.x - d, center.y + d), stroke, StrokeCap.Round)
            }
            Tone.NEUTRAL -> {
                drawCircle(color = tint, radius = r * 0.45f, center = center)
            }
        }
    }
}
