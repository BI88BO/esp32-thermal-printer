/* ============================================================================
 * 文件：app/selftest/page_copy.cpp
 * 层次：应用层 / 诊断页
 * 职责：波形复刻页——按固定时序结构打三个方块，用于对照加热与走纸节拍。
 * 依赖：selftest.h、common.h、config.h、printer.h、printhead.h
 *
 * 这页验的是「走纸/选通时序」，不是能量大小：
 *   六组全加热、每行推进 4 拍、行尾再停 3ms、不做首印/点补偿。
 * 脉宽沿用当前正常打印能量（不钉死某个固定值，也不用标称推荐值）。
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "config.h"
#include "printer.h"
#include "printhead.h"

bool selftest_waveform(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();

    uint8_t row[TPH_BYTES];
    memset(row, 0xFF, TPH_BYTES);

    for (uint8_t blk = 0; blk < 3; blk++) {
        for (uint8_t i = 0; i < 64; i++) {
            printer_job_row_raw(row, printhead_get_heat_us(), 300);
            delayMicroseconds(3000);        /* 方块之间的走纸间隔 */
        }
    }

    printer_job_end(true);
    test_heat_end();
    return true;
}
