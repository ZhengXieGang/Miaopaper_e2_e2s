package com.zxg.miaopaperflash

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.pm.PackageManager
import android.content.res.Configuration
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
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
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
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
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

private val SUPPORTED_BLE_NAMES = setOf("Paperang_E2", "Paperang_E2S")

private enum class Tone { NEUTRAL, SUCCESS, WARNING, ERROR }

private enum class DeviceModel(val label: String) {
    E2("E2"),
    E2S("E2S"),
}

private fun resolveDeviceModel(name: String): DeviceModel? = when (name) {
    "Paperang_E2" -> DeviceModel.E2
    "Paperang_E2S" -> DeviceModel.E2S
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
)

private data class UiState(
    val busy: Boolean = false,
    val title: String = "就绪",
    val detail: String = "",
    val progress: Float? = null,
    val tone: Tone = Tone.NEUTRAL,
)

class MainActivity : ComponentActivity() {

    private val state = mutableStateOf(UiState())
    private val confirmFlash = mutableStateOf(false)
    private val devices = mutableStateOf<List<ScanEntry>>(emptyList())
    private val target = mutableStateOf<SelectedDevice?>(null)
    private val firmware = mutableStateOf<FirmwareInfo?>(null)
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

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.scanRecord?.deviceName ?: result.device.name ?: return
            if (name !in SUPPORTED_BLE_NAMES) return
            val model = resolveDeviceModel(name) ?: return
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
        setContent {
            E2sTheme {
                E2sScreen(
                    state = state.value,
                    target = target.value,
                    devices = devices.value,
                    scanning = scanning.value,
                    scanned = scanned.value,
                    firmware = firmware.value,
                    onScan = { startScan() },
                    onStopScan = { finishScan() },
                    onSelect = { entry ->
                        stopScan()
                        scanned.value = true
                        target.value = SelectedDevice(entry.device, entry.name, entry.model)
                        state.value = UiState()
                    },
                    onChangeDevice = {
                        target.value = null
                        startScan()
                    },
                    onPickFirmware = { pickFirmware.launch(arrayOf("*/*")) },
                    onFlash = { confirmFlash.value = true },
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

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        enableEdgeToEdge()
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
            } catch (e: RuntimeException) {
                // 蓝牙适配器可能已关闭
            }
        }
        scanning.value = false
        scanner = null
    }

    private fun startFlash() = withPermissions {
        stopScan()
        if (target.value == null) {
            state.value = UiState(title = "未选择设备", detail = "先扫描并选择设备", tone = Tone.WARNING)
            return@withPermissions
        }
        if (firmware.value == null) {
            state.value = UiState(title = "未选择固件", detail = "先选择 .bin 文件", tone = Tone.WARNING)
            return@withPermissions
        }
        if (!working.compareAndSet(false, true)) return@withPermissions
        launch("E2-FLASH") { flashJob() }
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

    private fun flashJob(): UiState {
        val selected = target.value ?: throw IOException("未选择设备")
        val fw = firmware.value ?: throw IOException("未选择固件")

        // 若设备当前运行第三方固件，先切回原厂槽等待重启；若已是原厂则直接进入写入流程。
        stage("检查设备…")
        val restore = A5Protocol.run(
            A5Protocol.Operation.RESTORE,
            SessionConnector(selected),
            A5Protocol.CommitCheck { },
        )
        when (restore.outcome) {
            A5Protocol.Outcome.ALREADY_STOCK -> stage("已是原厂固件", "直接写入")
            A5Protocol.Outcome.STOCK_BOOTED -> stage("已切回原厂", "准备写入")
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

    private fun loadFirmware(uri: Uri) {
        if (!working.compareAndSet(false, true)) return
        launch("E2-FIRMWARE") {
            val name = displayName(uri)
            val bytes = contentResolver.openInputStream(uri)?.use { input ->
                val out = ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    out.write(buffer, 0, count)
                    if (out.size() > MAX_IMAGE) break
                }
                out.toByteArray()
            } ?: throw IOException("无法读取文件")
            if (bytes.size > MAX_IMAGE) {
                return@launch UiState(
                    title = "固件过大",
                    detail = "不能超过 1.93 MB",
                    tone = Tone.ERROR,
                )
            }
            if (bytes.size < 0x1000 || (bytes[0].toInt() and 0xFF) != 0xE9) {
                return@launch UiState(
                    title = "固件无效",
                    detail = "需 ESP32 app .bin",
                    tone = Tone.ERROR,
                )
            }
            val desc = parseEspAppDesc(bytes)
            val loaded = FirmwareInfo(
                fileName = name,
                bytes = bytes,
                sha256 = sha256Hex(bytes),
                projectName = desc.first,
                version = desc.second,
            )
            main.post { firmware.value = loaded }
            UiState()
        }
    }

    private fun displayName(uri: Uri): String {
        try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (index >= 0) cursor.getString(index)?.let { return it }
                }
            }
        } catch (e: RuntimeException) {
            // 回退至 URI 末尾段名
        }
        return uri.lastPathSegment ?: "firmware.bin"
    }

    /** 解析 ESP-IDF 镜像头部偏移 32 字节处的 esp_app_desc_t 描述信息。 */
    private fun parseEspAppDesc(bytes: ByteArray): Pair<String?, String?> {
        if (bytes.size < 144) return Pair(null, null)
        // esp_app_desc_t 魔数：0xABCD5432（小端字节序：32 54 CD AB）
        val isDesc = (bytes[32].toInt() and 0xFF) == 0x32 &&
            (bytes[33].toInt() and 0xFF) == 0x54 &&
            (bytes[34].toInt() and 0xFF) == 0xCD &&
            (bytes[35].toInt() and 0xFF) == 0xAB
        if (!isDesc) return Pair(null, null)
        val version = extractCStr(bytes, 32 + 16, 32)
        val project = extractCStr(bytes, 32 + 48, 32)
        return Pair(project, version)
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
                stage("连接设备…", "打开同步模式")
            }
            return try {
                A5Flasher.open(applicationContext, selected.device, { }, verification)
            } catch (e: SecurityException) {
                throw IOException("缺少蓝牙权限")
            } catch (e: RuntimeException) {
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

private data class ExtendedColors(
    val cardBorder: Color,
    val subSurface: Color,
    val success: Color,
    val successContainer: Color,
    val warning: Color,
    val warningContainer: Color,
    val errorContainer: Color,
    val e2BadgeBg: Color,
    val e2BadgeFg: Color,
    val e2sBadgeBg: Color,
    val e2sBadgeFg: Color,
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
    e2BadgeBg = Color(0xFFEEF2FF),
    e2BadgeFg = Color(0xFF3730A3),
    e2sBadgeBg = Color(0xFFFFEDD5),
    e2sBadgeFg = Color(0xFFC2410C),
)

private val DarkExtended = ExtendedColors(
    cardBorder = Color(0xFF2B2E35),
    subSurface = Color(0xFF16181B),
    success = Color(0xFF4ADE80),
    successContainer = Color(0xFF143320),
    warning = Color(0xFFFBBF24),
    warningContainer = Color(0xFF3B270B),
    errorContainer = Color(0xFF3B1214),
    e2BadgeBg = Color(0xFF1E2440),
    e2BadgeFg = Color(0xFFA5B4FC),
    e2sBadgeBg = Color(0xFF431D08),
    e2sBadgeFg = Color(0xFFFDBA74),
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

@Composable
private fun E2sScreen(
    state: UiState,
    target: SelectedDevice?,
    devices: List<ScanEntry>,
    scanning: Boolean,
    scanned: Boolean,
    firmware: FirmwareInfo?,
    onScan: () -> Unit,
    onStopScan: () -> Unit,
    onSelect: (ScanEntry) -> Unit,
    onChangeDevice: () -> Unit,
    onPickFirmware: () -> Unit,
    onFlash: () -> Unit,
    onCancel: () -> Unit,
    onDismissStatus: () -> Unit,
) {
    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = { AppTopBar() },
        bottomBar = {
            BottomActionBar(
                target = target,
                firmware = firmware,
                busy = state.busy,
                onFlash = onFlash,
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
            DeviceSectionCard(
                target = target,
                devices = devices,
                scanning = scanning,
                scanned = scanned,
                busy = state.busy,
                onScan = onScan,
                onStopScan = onStopScan,
                onSelect = onSelect,
                onChangeDevice = onChangeDevice,
            )
            FirmwareSectionCard(
                firmware = firmware,
                busy = state.busy,
                onPickFirmware = onPickFirmware,
            )
        }
    }
}

/** 顶部标题栏：保持与下方卡片 20.dp 垂直对齐基线。 */
@Composable
private fun AppTopBar() {
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
                    text = "E2 / E2S 刷机助手",
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
                        text = "v1.1",
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

/** 卡片右上角固定等大实体按钮（扫描 / 选择文件 / 更换 / 停止），保证一致宽高与右边缘对齐。 */
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

/** 状态栏内的紧凑实体按钮（取消 / 关闭），与整体边框规范一致。 */
@Composable
private fun SmallOutlinedButton(label: String, onClick: () -> Unit) {
    val ext = LocalExtendedColors.current
    OutlinedButton(
        onClick = onClick,
        shape = RoundedCornerShape(8.dp),
        border = BorderStroke(1.dp, ext.cardBorder),
        contentPadding = PaddingValues(0.dp),
        modifier = Modifier
            .width(64.dp)
            .height(32.dp),
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelMedium,
            fontWeight = FontWeight.Medium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
        )
    }
}

/** 通用步骤卡片骨架：统一步骤徽章、标题行、右对齐操作按钮与内边距。 */
@Composable
private fun SectionCard(
    step: Int,
    completed: Boolean,
    title: String,
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
                StepBadge(step = step, completed = completed)
                Spacer(Modifier.width(10.dp))
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.weight(1f),
                )
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

@Composable
private fun DeviceSectionCard(
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
    SectionCard(
        step = 1,
        completed = target != null,
        title = "设备",
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
                            Spacer(Modifier.width(8.dp))
                            ModelTag(model = target.model)
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
            return@SectionCard
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
                text = "正在扫描…",
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
                                    Spacer(Modifier.width(8.dp))
                                    ModelTag(model = entry.model)
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
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .clickable(enabled = !busy, onClick = onScan),
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
                        BluetoothIcon(
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(16.dp),
                        )
                    }
                    Spacer(Modifier.width(12.dp))
                    Text(
                        text = if (scanned) "未找到设备，请在原厂固件打开同步模式" else "打开同步模式后点击扫描",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun ModelTag(model: DeviceModel) {
    val ext = LocalExtendedColors.current
    val (bg, fg) = when (model) {
        DeviceModel.E2 -> ext.e2BadgeBg to ext.e2BadgeFg
        DeviceModel.E2S -> ext.e2sBadgeBg to ext.e2sBadgeFg
    }
    Surface(shape = RoundedCornerShape(6.dp), color = bg) {
        Text(
            text = model.label,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            color = fg,
            modifier = Modifier.padding(horizontal = 7.dp, vertical = 2.dp),
        )
    }
}

@Composable
private fun FirmwareSectionCard(
    firmware: FirmwareInfo?,
    busy: Boolean,
    onPickFirmware: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    SectionCard(
        step = 2,
        completed = firmware != null,
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
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .clickable(enabled = !busy, onClick = onPickFirmware),
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
                        ChipIcon(
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(18.dp),
                        )
                    }
                    Spacer(Modifier.width(12.dp))
                    Text(
                        text = "选择本地 .bin 文件",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            return@SectionCard
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

@Composable
private fun BottomActionBar(
    target: SelectedDevice?,
    firmware: FirmwareInfo?,
    busy: Boolean,
    onFlash: () -> Unit,
) {
    val ext = LocalExtendedColors.current
    Surface(color = MaterialTheme.colorScheme.surface) {
        Column(modifier = Modifier.fillMaxWidth()) {
            HorizontalDivider(color = ext.cardBorder)
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .navigationBarsPadding()
                    .padding(horizontal = 20.dp, vertical = 12.dp),
            ) {
                Button(
                    onClick = onFlash,
                    enabled = target != null && firmware != null && !busy,
                    shape = RoundedCornerShape(14.dp),
                    contentPadding = PaddingValues(horizontal = 16.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.primary,
                        contentColor = MaterialTheme.colorScheme.onPrimary,
                    ),
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(52.dp),
                ) {
                    Text(
                        text = "刷入固件",
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.SemiBold,
                    )
                }
            }
        }
    }
}

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
                            DialogMetaRow("设备", "${target.name} (${target.model.label})")
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
                    text = "写入非运行 OTA 槽并重启，电量需 ≥50%。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            Button(
                onClick = onConfirm,
                shape = RoundedCornerShape(10.dp),
            ) {
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
            // 上下引脚
            drawLine(color, Offset(size.width * p, pad * 0.35f), Offset(size.width * p, pad), stroke, StrokeCap.Round)
            drawLine(color, Offset(size.width * p, size.height - pad), Offset(size.width * p, size.height - pad * 0.35f), stroke, StrokeCap.Round)
            // 左右引脚
            drawLine(color, Offset(pad * 0.35f, size.height * p), Offset(pad, size.height * p), stroke, StrokeCap.Round)
            drawLine(color, Offset(size.width - pad, size.height * p), Offset(size.width - pad * 0.35f, size.height * p), stroke, StrokeCap.Round)
        }
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
