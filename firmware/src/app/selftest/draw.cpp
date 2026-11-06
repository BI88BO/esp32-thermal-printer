/* ============================================================================
 * 文件：app/selftest/draw.cpp
 * 层次：应用层 / 诊断页绘图基元
 * 职责：3×5 点阵数字与版本号抬头。
 * 依赖：config.h、printer.h（抬头会直接打行）、sensor.h（读头温）
 * ========================================================================== */

#include "draw.h"
#include <Arduino.h>
#include "config.h"
#include "printer.h"
#include "sensor.h"

/* 3×5 点阵数字，2 倍放大后打在纸上。
 * 点检页最上方会打两位版本号（比如 89 = build 89），
 * 不用串口、不用连手机，看纸就知道板子里烧的是哪一版。 */
static const uint8_t DIGIT[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7},
    {0x7, 0x1, 0x7, 0x4, 0x7}, {0x7, 0x1, 0x7, 0x1, 0x7},
    {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1},
    {0x7, 0x5, 0x7, 0x5, 0x7}, {0x7, 0x5, 0x7, 0x1, 0x7},
};

void draw_digit_row(uint8_t *row, uint16_t x0, uint8_t d, uint8_t y, uint8_t scale)
{
    if (d > 9 || y >= 5 * scale)
        return;

    uint8_t bits = DIGIT[d][y / scale];
    for (uint8_t c = 0; c < 3; c++) {
        if (!(bits & (0x4 >> c)))
            continue;

        for (uint8_t sx = 0; sx < scale; sx++) {
            uint16_t px = (uint16_t)(x0 + c * scale + sx);
            if (px < TPH_DOTS)
                row[px >> 3] |= (uint8_t)(0x80 >> (px & 7));
        }
    }
}

void draw_num_row(uint8_t *row, uint16_t x0, uint16_t val, uint8_t y,
                  uint8_t scale, uint8_t nd)
{
    uint16_t div = 1;
    for (uint8_t i = 1; i < nd; i++)
        div *= 10;

    for (uint8_t i = 0; i < nd; i++) {
        draw_digit_row(row, (uint16_t)(x0 + i * (3 * scale + 2)),
                       (uint8_t)((val / div) % 10), y, scale);
        div /= 10;
    }
}

void draw_build_code(uint8_t code)
{
    uint8_t row[TPH_BYTES];
    uint8_t d[2] = { (uint8_t)(code / 10 % 10), (uint8_t)(code % 10) };

    /* 右侧 3 位数 = 这张纸开始打时的实测打印头温度(℃)，999 = NTC 没读到。
     * 连打几张对比这个数，就能直接看出「越打越好」是不是头温累积。 */
    int8_t   ht = sensor_head_temp();
    uint16_t tv = (ht < 0) ? 999 : (uint16_t)ht;

    for (uint8_t y = 0; y < 10; y++) {
        memset(row, 0, TPH_BYTES);
        for (uint8_t k = 0; k < 2; k++) {
            uint8_t bits = DIGIT[d[k]][y >> 1];
            for (uint8_t x = 0; x < 3; x++) {
                if (bits & (0x4 >> x)) {
                    for (uint8_t sx = 0; sx < 2; sx++) {
                        uint16_t px = (uint16_t)k * 8 + x * 2 + sx;
                        row[px >> 3] |= (uint8_t)(0x80 >> (px & 7));
                    }
                }
            }
        }
        draw_num_row(row, 200, tv, y, 2, 3);   /* 右侧：头温 ℃ */
        printer_job_row(row);
    }

    memset(row, 0, TPH_BYTES);
    for (uint8_t y = 0; y < 4; y++)
        printer_job_row(row);
}
