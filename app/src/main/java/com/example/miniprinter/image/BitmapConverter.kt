package com.example.miniprinter.image

import android.graphics.Bitmap
import kotlin.math.max

/**
 * 位图 → 打印头位流
 *
 * 打印头参数：384 点/行，8 dot/mm，一行 = 48 字节，MSB 在前，1 = 加热（黑）
 *
 * 二值化用 Floyd-Steinberg 误差扩散：把当前像素的量化误差按 7/16、3/16、5/16、1/16
 * 分给右边、左下、下、右下。照片和渐变靠这个保住中间调，纯阈值会把灰阶全打成死黑或死白。
 */
object BitmapConverter {

    const val WIDTH = 384
    const val LINE_BYTES = 48

    data class Result(
        val preview: Bitmap,     // 用于界面上显示"打出来会是什么样"
        val rows: ByteArray,     // 每行 48 字节，长度 = lines * 48
        val lines: Int
    )

    /**
     * @param threshold 二值化阈值 0~255，越大越黑
     * @param dither    true = 误差扩散（照片）；false = 纯阈值（文字更锐）
     */
    fun convert(src: Bitmap, threshold: Int = 128, dither: Boolean = true): Result {
        /* 1. 按宽度等比缩放。横向点数由打印头物理决定，不能变 */
        val scale = WIDTH.toFloat() / src.width
        val h = max(1, (src.height * scale).toInt())
        val scaled = Bitmap.createScaledBitmap(src, WIDTH, h, true)

        /* 2. 转灰度。用亮度公式而不是单取某个通道，
         *    否则彩色图里红绿蓝的明暗关系会判错 */
        val gray = FloatArray(WIDTH * h)
        val px = IntArray(WIDTH * h)
        scaled.getPixels(px, 0, WIDTH, 0, 0, WIDTH, h)
        for (i in px.indices) {
            val c = px[i]
            val r = (c shr 16) and 0xFF
            val g = (c shr 8) and 0xFF
            val b = c and 0xFF
            gray[i] = 0.299f * r + 0.587f * g + 0.114f * b
        }
        scaled.recycle()

        /* 3. 二值化 + 误差扩散 + 打包 */
        val outPx = IntArray(WIDTH * h)
        val rows = ByteArray(h * LINE_BYTES)

        for (y in 0 until h) {
            for (x in 0 until WIDTH) {
                val idx = y * WIDTH + x
                val old = gray[idx].coerceIn(0f, 255f)
                val black = old < threshold
                val newVal = if (black) 0f else 255f

                outPx[idx] = if (black) 0xFF000000.toInt() else 0xFFFFFFFF.toInt()

                if (black) {
                    val byteIdx = y * LINE_BYTES + (x shr 3)
                    rows[byteIdx] =
                        (rows[byteIdx].toInt() or (0x80 shr (x and 7))).toByte()
                }

                if (dither) {
                    val err = old - newVal
                    spread(gray, x, y, err)
                }
            }
        }

        val preview = Bitmap.createBitmap(WIDTH, h, Bitmap.Config.ARGB_8888)
        preview.setPixels(outPx, 0, WIDTH, 0, 0, WIDTH, h)
        return Result(preview, rows, h)
    }

    /** 标准 Floyd-Steinberg 权重分配，越界的不扩散（误差直接丢掉） */
    private fun spread(gray: FloatArray, x: Int, y: Int, err: Float) {
        val w = WIDTH
        val h = gray.size / w

        if (x + 1 < w) gray[y * w + x + 1] += err * 7f / 16f
        if (y + 1 < h) {
            if (x > 0) gray[(y + 1) * w + x - 1] += err * 3f / 16f
            gray[(y + 1) * w + x] += err * 5f / 16f
            if (x + 1 < w) gray[(y + 1) * w + x + 1] += err * 1f / 16f
        }
    }
}
