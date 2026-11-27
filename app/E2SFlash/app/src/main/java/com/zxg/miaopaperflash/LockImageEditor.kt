package com.zxg.miaopaperflash

import android.graphics.Bitmap
import android.graphics.Matrix
import android.graphics.Paint
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTransformGestures
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
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** 独立全屏锁屏图片编辑界面（参考原版 Miaopaper：原图裁剪构图 + 实时墨水屏效果 + 4 种算法调节）。 */
@Composable
internal fun LockImageEditorScreen(
    source: Bitmap,
    initialParams: LockEditParams,
    cardBorder: Color,
    subSurface: Color,
    onCancel: () -> Unit,
    onConfirm: (LockEditParams, RenderedLockImage) -> Unit,
) {
    var params by remember(source, initialParams) { mutableStateOf(initialParams) }
    val rendered = remember(source, params) { TransferMedia.renderLockImage(source, params) }
    val previewBitmap = remember(rendered.preview) { rendered.preview.asImageBitmap() }

    BackHandler(onBack = onCancel)

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
    ) {
        Column(modifier = Modifier.fillMaxSize()) {
            // 顶部导航栏：取消 / 标题 / 完成
            EditorTopBar(
                cardBorder = cardBorder,
                onCancel = onCancel,
                onConfirm = { onConfirm(params, rendered) },
            )

            // 上方：墨水屏实时效果预览（所见即所得，点击或拖动下方构图同步更新）
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(subSurface)
                    .padding(horizontal = 20.dp, vertical = 12.dp),
                contentAlignment = Alignment.Center,
            ) {
                Surface(
                    modifier = Modifier
                        .height(104.dp)
                        .aspectRatio(TransferMedia.LOCK_W.toFloat() / TransferMedia.LOCK_VISIBLE_H.toFloat()),
                    shape = RoundedCornerShape(10.dp),
                    color = Color(0xFFFAFAF7),
                    border = BorderStroke(1.dp, cardBorder),
                    shadowElevation = 1.dp,
                ) {
                    Image(
                        bitmap = previewBitmap,
                        contentDescription = "效果预览",
                        contentScale = ContentScale.FillBounds,
                        filterQuality = FilterQuality.None,
                        modifier = Modifier.fillMaxSize(),
                    )
                }
            }

            HorizontalDivider(color = cardBorder)

            // 中间：原图自由裁剪画布（单指拖动 + 双指缩放，无外层滚动冲突）
            CropStage(
                source = source,
                params = params,
                accentColor = MaterialTheme.colorScheme.primary,
                onParamsChange = { params = it },
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f),
            )

            // 底部：算法选择 + 亮度/对比度滑块 + 快捷构图操作
            EditorControlPanel(
                params = params,
                cardBorder = cardBorder,
                onParamsChange = { params = it },
            )
        }
    }
}

@Composable
private fun EditorTopBar(
    cardBorder: Color,
    onCancel: () -> Unit,
    onConfirm: () -> Unit,
) {
    Surface(color = MaterialTheme.colorScheme.surface) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .statusBarsPadding()
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextButton(
                    onClick = onCancel,
                    contentPadding = PaddingValues(horizontal = 10.dp, vertical = 6.dp),
                ) {
                    Text(
                        text = "取消",
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(
                    text = "编辑锁屏图",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSurface,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.weight(1f),
                )
                Button(
                    onClick = onConfirm,
                    shape = RoundedCornerShape(10.dp),
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 0.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.primary,
                        contentColor = MaterialTheme.colorScheme.onPrimary,
                    ),
                    modifier = Modifier.height(34.dp),
                ) {
                    Text(
                        text = "完成",
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.Medium,
                    )
                }
            }
            HorizontalDivider(color = cardBorder)
        }
    }
}

/** 计算裁剪取景框在画布中的居中像素尺寸（严格保持 250:122 屏幕比例）。 */
private fun computeCropRect(canvasW: Float, canvasH: Float): Pair<Float, Float> {
    val ratio = TransferMedia.LOCK_W.toFloat() / TransferMedia.LOCK_VISIBLE_H.toFloat()
    val maxW = canvasW * 0.84f
    val maxH = canvasH * 0.78f
    var w = maxW
    var h = w / ratio
    if (h > maxH) {
        h = maxH
        w = h * ratio
    }
    return w.coerceAtLeast(1f) to h.coerceAtLeast(1f)
}

@Composable
private fun CropStage(
    source: Bitmap,
    params: LockEditParams,
    accentColor: Color,
    onParamsChange: (LockEditParams) -> Unit,
    modifier: Modifier = Modifier,
) {
    var stageSize by remember { mutableStateOf(IntSize.Zero) }
    val latestParams by rememberUpdatedState(params)
    val latestStageSize by rememberUpdatedState(stageSize)
    val bitmapPaint = remember { Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG) }
    val drawMatrix = remember { Matrix() }

    Box(
        modifier = modifier
            .background(Color(0xFF1A1C1F))
            .onSizeChanged { stageSize = it }
            .pointerInput(source) {
                detectTransformGestures { centroid, pan, zoomChange, _ ->
                    val cur = latestParams
                    val sw = latestStageSize.width.coerceAtLeast(1).toFloat()
                    val sh = latestStageSize.height.coerceAtLeast(1).toFloat()
                    val (cropW, _) = computeCropRect(sw, sh)
                    val k = cropW / TransferMedia.LOCK_W.toFloat()

                    val oldZoom = cur.zoom
                    val newZoom = (oldZoom * zoomChange).coerceIn(TransferMedia.MIN_ZOOM, TransferMedia.MAX_ZOOM)
                    val zoomRatio = newZoom / oldZoom

                    // 绕双指中心缩放并叠加平移，保证手指下图像不飘移
                    val lx = (centroid.x - sw / 2f) / k
                    val ly = (centroid.y - sh / 2f) / k
                    val rawX = (cur.offsetX - lx) * zoomRatio + lx + pan.x / k
                    val rawY = (cur.offsetY - ly) * zoomRatio + ly + pan.y / k

                    val scale = TransferMedia.logicalScale(source.width, source.height, cur.copy(zoom = newZoom))
                    val swap = ((cur.rotation % 180) + 180) % 180 == 90
                    val halfDrawW = (if (swap) source.height else source.width) * scale / 2f
                    val halfDrawH = (if (swap) source.width else source.height) * scale / 2f
                    val limitX = max(TransferMedia.LOCK_W * 0.75f, halfDrawW + TransferMedia.LOCK_W * 0.35f)
                    val limitY = max(TransferMedia.LOCK_VISIBLE_H * 0.75f, halfDrawH + TransferMedia.LOCK_VISIBLE_H * 0.35f)

                    onParamsChange(
                        cur.copy(
                            offsetX = rawX.coerceIn(-limitX, limitX),
                            offsetY = rawY.coerceIn(-limitY, limitY),
                            zoom = newZoom,
                        ),
                    )
                }
            },
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            val w = size.width
            val h = size.height
            if (w <= 1f || h <= 1f) return@Canvas

            val (cropW, cropH) = computeCropRect(w, h)
            val cropLeft = (w - cropW) / 2f
            val cropTop = (h - cropH) / 2f
            val cropRight = cropLeft + cropW
            val cropBottom = cropTop + cropH
            val k = cropW / TransferMedia.LOCK_W.toFloat()
            val scale = TransferMedia.logicalScale(source.width, source.height, params) * k

            // 1) 取景框内部先铺白底（透明图或缩小留边时与真机白底一致）
            drawRect(
                color = Color(0xFFFAFAF7),
                topLeft = Offset(cropLeft, cropTop),
                size = Size(cropW, cropH),
            )

            // 2) 使用与 renderLockImage 完全同源的变换矩阵绘制原图
            clipRect(0f, 0f, w, h) {
                drawMatrix.reset()
                drawMatrix.postTranslate(-source.width / 2f, -source.height / 2f)
                if (params.rotation != 0) {
                    drawMatrix.postRotate(params.rotation.toFloat())
                }
                drawMatrix.postScale(scale, scale)
                drawMatrix.postTranslate(
                    w / 2f + params.offsetX * k,
                    h / 2f + params.offsetY * k,
                )
                drawContext.canvas.nativeCanvas.drawBitmap(source, drawMatrix, bitmapPaint)
            }

            // 3) 取景框外半透明暗遮罩，清晰展示原图裁剪范围
            val scrim = Color.Black.copy(alpha = 0.56f)
            drawRect(scrim, Offset(0f, 0f), Size(w, cropTop))
            drawRect(scrim, Offset(0f, cropBottom), Size(w, h - cropBottom))
            drawRect(scrim, Offset(0f, cropTop), Size(cropLeft, cropH))
            drawRect(scrim, Offset(cropRight, cropTop), Size(w - cropRight, cropH))

            // 4) 极淡三分辅助线
            val gridColor = Color.White.copy(alpha = 0.24f)
            val gridStroke = 1.dp.toPx()
            for (i in 1..2) {
                val gx = cropLeft + cropW * i / 3f
                val gy = cropTop + cropH * i / 3f
                drawLine(gridColor, Offset(gx, cropTop), Offset(gx, cropBottom), gridStroke)
                drawLine(gridColor, Offset(cropLeft, gy), Offset(cropRight, gy), gridStroke)
            }

            // 5) 取景框细线 + 四角珊瑚橙角标
            drawRect(
                color = Color.White.copy(alpha = 0.88f),
                topLeft = Offset(cropLeft, cropTop),
                size = Size(cropW, cropH),
                style = Stroke(width = 1.2.dp.toPx()),
            )
            val cornerLen = min(14.dp.toPx(), cropH * 0.2f)
            val cornerStroke = 2.6.dp.toPx()
            // 左上
            drawLine(accentColor, Offset(cropLeft, cropTop), Offset(cropLeft + cornerLen, cropTop), cornerStroke, StrokeCap.Round)
            drawLine(accentColor, Offset(cropLeft, cropTop), Offset(cropLeft, cropTop + cornerLen), cornerStroke, StrokeCap.Round)
            // 右上
            drawLine(accentColor, Offset(cropRight, cropTop), Offset(cropRight - cornerLen, cropTop), cornerStroke, StrokeCap.Round)
            drawLine(accentColor, Offset(cropRight, cropTop), Offset(cropRight, cropTop + cornerLen), cornerStroke, StrokeCap.Round)
            // 左下
            drawLine(accentColor, Offset(cropLeft, cropBottom), Offset(cropLeft + cornerLen, cropBottom), cornerStroke, StrokeCap.Round)
            drawLine(accentColor, Offset(cropLeft, cropBottom), Offset(cropLeft, cropBottom - cornerLen), cornerStroke, StrokeCap.Round)
            // 右下
            drawLine(accentColor, Offset(cropRight, cropBottom), Offset(cropRight - cornerLen, cropBottom), cornerStroke, StrokeCap.Round)
            drawLine(accentColor, Offset(cropRight, cropBottom), Offset(cropRight, cropBottom - cornerLen), cornerStroke, StrokeCap.Round)
        }
    }
}

@Composable
private fun EditorControlPanel(
    params: LockEditParams,
    cardBorder: Color,
    onParamsChange: (LockEditParams) -> Unit,
) {
    Surface(color = MaterialTheme.colorScheme.surface) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .navigationBarsPadding(),
        ) {
            HorizontalDivider(color = cardBorder)
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 18.dp, vertical = 14.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                // 1) 原版 4 种墨水屏算法分段选择
                Surface(
                    modifier = Modifier.fillMaxWidth(),
                    shape = RoundedCornerShape(10.dp),
                    color = MaterialTheme.colorScheme.surfaceVariant,
                ) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(3.dp),
                    ) {
                        LockDitherMode.entries.forEach { mode ->
                            val selected = params.ditherMode == mode
                            Surface(
                                shape = RoundedCornerShape(8.dp),
                                color = if (selected) MaterialTheme.colorScheme.surface else Color.Transparent,
                                modifier = Modifier
                                    .weight(1f)
                                    .clip(RoundedCornerShape(8.dp))
                                    .clickable {
                                        if (!selected) onParamsChange(params.copy(ditherMode = mode))
                                    },
                            ) {
                                Text(
                                    text = mode.label,
                                    style = MaterialTheme.typography.labelMedium,
                                    fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium,
                                    color = if (selected) {
                                        MaterialTheme.colorScheme.onSurface
                                    } else {
                                        MaterialTheme.colorScheme.onSurfaceVariant
                                    },
                                    textAlign = TextAlign.Center,
                                    maxLines = 1,
                                    modifier = Modifier.padding(vertical = 7.dp),
                                )
                            }
                        }
                    }
                }

                // 2) 亮度（黑白模式下为阈值）与对比度滑块
                CompactAdjustSlider(
                    label = if (params.ditherMode == LockDitherMode.THRESHOLD) "阈值" else "亮度",
                    value = params.brightness,
                    onChange = { onParamsChange(params.copy(brightness = it)) },
                )
                CompactAdjustSlider(
                    label = "对比度",
                    value = params.contrast,
                    onChange = { onParamsChange(params.copy(contrast = it)) },
                )

                // 3) 4 项快捷操作：旋转 / 完整或铺满 / 反色 / 重置
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    EditorActionChip(
                        label = "旋转",
                        active = params.rotation != 0,
                        enabled = true,
                        cardBorder = cardBorder,
                        modifier = Modifier.weight(1f),
                        onClick = {
                            onParamsChange(
                                params.copy(
                                    rotation = (params.rotation + 90) % 360,
                                    offsetX = 0f,
                                    offsetY = 0f,
                                    zoom = 1f,
                                ),
                            )
                        },
                    )
                    EditorActionChip(
                        label = if (params.fitMode == LockFitMode.FILL) "完整" else "铺满",
                        active = params.fitMode == LockFitMode.FIT,
                        enabled = true,
                        cardBorder = cardBorder,
                        modifier = Modifier.weight(1f),
                        onClick = {
                            val nextFit = if (params.fitMode == LockFitMode.FILL) {
                                LockFitMode.FIT
                            } else {
                                LockFitMode.FILL
                            }
                            onParamsChange(
                                params.copy(
                                    fitMode = nextFit,
                                    offsetX = 0f,
                                    offsetY = 0f,
                                    zoom = 1f,
                                ),
                            )
                        },
                    )
                    EditorActionChip(
                        label = "反色",
                        active = params.invert,
                        enabled = true,
                        cardBorder = cardBorder,
                        modifier = Modifier.weight(1f),
                        onClick = { onParamsChange(params.copy(invert = !params.invert)) },
                    )
                    EditorActionChip(
                        label = "重置",
                        active = false,
                        enabled = !params.isDefault,
                        cardBorder = cardBorder,
                        modifier = Modifier.weight(1f),
                        onClick = { onParamsChange(LockEditParams()) },
                    )
                }
            }
        }
    }
}

@Composable
private fun CompactAdjustSlider(
    label: String,
    value: Int,
    onChange: (Int) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(44.dp),
        )
        Slider(
            value = value.toFloat(),
            onValueChange = { onChange(it.roundToInt()) },
            valueRange = -100f..100f,
            colors = SliderDefaults.colors(
                thumbColor = MaterialTheme.colorScheme.primary,
                activeTrackColor = MaterialTheme.colorScheme.primary,
                inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant,
            ),
            modifier = Modifier
                .weight(1f)
                .height(32.dp),
        )
        Spacer(Modifier.width(8.dp))
        Text(
            text = if (value > 0) "+$value" else value.toString(),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.End,
            modifier = Modifier.width(34.dp),
        )
    }
}

@Composable
private fun EditorActionChip(
    label: String,
    active: Boolean,
    enabled: Boolean,
    cardBorder: Color,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Surface(
        shape = RoundedCornerShape(9.dp),
        color = if (active) MaterialTheme.colorScheme.primaryContainer else Color.Transparent,
        border = BorderStroke(
            1.dp,
            when {
                !enabled -> cardBorder.copy(alpha = 0.45f)
                active -> MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)
                else -> cardBorder
            },
        ),
        modifier = modifier
            .height(34.dp)
            .clip(RoundedCornerShape(9.dp))
            .clickable(enabled = enabled, onClick = onClick),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.Medium,
                color = when {
                    !enabled -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.45f)
                    active -> MaterialTheme.colorScheme.onPrimaryContainer
                    else -> MaterialTheme.colorScheme.onSurface
                },
                maxLines = 1,
            )
        }
    }
}
