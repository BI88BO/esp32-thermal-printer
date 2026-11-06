package com.example.miniprinter.image

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import kotlin.math.max

/**
 * 文字 → 位图
 *
 * 注意这里的尺寸单位是"打印点"而不是像素：打印头 384 点宽、8 dot/mm，
 * 所以字号 24 点 = 3mm 高，一行能放 384/24 = 16 个汉字左右。
 * 用点作单位，预览和实物就是 1:1，不用心算换算。
 */
object TextRenderer {

    const val WIDTH = 384
    private const val PADDING = 8          // 左右留白，避免贴边糊掉

    data class Result(val bitmap: Bitmap, val lineCount: Int)

    /**
     * @param text      支持 \n 手动换行，超宽自动折行
     * @param textSize  字号（打印点），常用 20~32
     * @param bold      标题用粗体
     */
    fun render(text: String, textSize: Float = 24f, bold: Boolean = false): Result {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.BLACK
            this.textSize = textSize
            typeface = if (bold) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
        }

        val maxWidth = WIDTH - PADDING * 2
        val lines = wrap(text, paint, maxWidth)

        val fm = paint.fontMetrics
        val lineHeight = (fm.descent - fm.ascent + textSize * 0.25f)
        val height = max(1, (lineHeight * lines.size + PADDING).toInt())

        val bmp = Bitmap.createBitmap(WIDTH, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(Color.WHITE)

        var baseline = PADDING - fm.ascent
        for (line in lines) {
            canvas.drawText(line, PADDING.toFloat(), baseline, paint)
            baseline += lineHeight
        }

        return Result(bmp, lines.size)
    }

    /** 先按 \n 分段，再按可用宽度逐字折行 */
    private fun wrap(text: String, paint: Paint, maxWidth: Int): List<String> {
        val out = mutableListOf<String>()

        for (paragraph in text.split("\n")) {
            if (paragraph.isEmpty()) {
                out.add("")
                continue
            }
            var start = 0
            var end = 0
            while (end < paragraph.length) {
                end++
                val candidate = paragraph.substring(start, end)
                if (paint.measureText(candidate) > maxWidth) {
                    /* 超宽了，退回一个字符 */
                    val cut = max(start + 1, end - 1)
                    out.add(paragraph.substring(start, cut))
                    start = cut
                    end = cut
                }
            }
            if (start < paragraph.length) out.add(paragraph.substring(start))
        }

        if (out.isEmpty()) out.add("")
        return out
    }
}
