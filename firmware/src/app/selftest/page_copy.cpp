/* ============================================================================
 * 文件：app/selftest/page_copy.cpp
 * 层次：应用层 / 诊断页
 * 职责：原工程复刻页——照 step_test.ino 的时序结构打三个方块。
 * 对应原工程：step_test/step_test.ino 的 print_square
 * 依赖：selftest.h、common.h、config.h、printer.h、printhead.h
 *
 * 这页验的是「走纸/选通时序」，不是能量大小：
 *   六组全加热、每行推进 4 拍、行尾再停 3ms、不做首印/点补偿。
 * 脉宽沿用当前正常打印能量（不照搬原工程的 3000µs，也不钉标称 800µs）。
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "config.h"
#include "printer.h"
#include "printhead.h"

bool selftest_original_copy(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();

    uint8_t row[TPH_BYTES];
    memset(row, 0xFF, TPH_BYTES);

    for (uint8_t blk = 0; blk < 3; blk++) {
        for (uint8_t i = 0; i < 64; i++) {
            printer_job_row_raw(row, printhead_get_heat_us(), 300);
            delayMicroseconds(3000);        /* 原工程 STEP_DELAY_US */
        }
    }

    printer_job_end(true);
    test_heat_end();
    return true;
}
