/* ============================================================================
 * 文件：app/selftest/page_full.cpp
 * 层次：应用层 / 诊断页
 * 职责：整页自检（212 行）。五段版面各验一件事，见文件内注释。
 * 对应原工程：无
 * 依赖：selftest.h、common.h、config.h、printer.h
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "config.h"
#include "printer.h"

/* 版面高度（行） */
#define Z_TOP        0       // 顶部黑边     0..3
#define Z_VERT       4       // 竖线组       4..35
#define Z_HORZ       36      // 横线组      36..67
#define Z_CHECK      68      // 棋盘格      68..167
#define Z_SOLID      168     // 全黑块     168..207
#define Z_BOTTOM     208     // 底部黑边   208..211
#define Z_END        212

static void build_row(uint16_t y, uint8_t *row)
{
    memset(row, 0, TPH_BYTES);

    /* 整行黑的区域 */
    if (y < Z_VERT || (y >= Z_SOLID && y < Z_END)) {
        memset(row, 0xFF, TPH_BYTES);
        return;
    }

    /* 左右边框各 2 点，用来量幅面 */
    row[0] |= 0xC0;
    row[TPH_BYTES - 1] |= 0x03;

    if (y < Z_HORZ) {
        /* 竖线：每 8 点一条 1 点宽的线，共 48 条
         * 48 条 = 6 段 × 8 条，哪段缺了立刻看得出来 */
        for (uint16_t x = 0; x < TPH_DOTS; x += 8)
            row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    } else if (y < Z_CHECK) {
        /* 横线：每 8 行画 2 行，间距必须完全相等 */
        if (((y - Z_HORZ) % 8) < 2)
            memset(row, 0xFF, TPH_BYTES);
    } else {
        /* 棋盘：4×4 点一格 */
        uint16_t yy = y - Z_CHECK;
        for (uint16_t x = 0; x < TPH_DOTS; x++) {
            if ((((x >> 2) + (yy >> 2)) & 1) == 0)
                row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
}

bool selftest_print(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();

    uint8_t row[TPH_BYTES];
    for (uint16_t y = 0; y < Z_END; y++) {
        build_row(y, row);
        printer_job_row(row);
    }

    printer_job_end(true);                  // 关升压 + 退纸 7.5mm + 放锁
    test_heat_end();
    return true;
}
