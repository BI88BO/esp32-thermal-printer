/* ============================================================================
 * 文件：app/selftest/page_load.cpp
 * 层次：应用层 / 诊断页
 * 职责：全黑负载页（150 行 = 19mm）。
 * 对应原工程：无
 * 依赖：selftest.h、common.h、config.h、printer.h
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "config.h"
#include "printer.h"

/* 负载测试页：连续 150 行全黑（19mm 纸）。
 * 六组轮流满点加热，VH 持续处在大电流状态，万用表读数才稳得住。
 * 用与正常打印相同的能量（不再钉 800µs 标称窗口），50℃ 熔断会按页中止，
 * 纸上从某行起变白属预期，不是故障。 */
bool selftest_load(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();

    uint8_t row[TPH_BYTES];
    memset(row, 0xFF, TPH_BYTES);

    for (uint16_t y = 0; y < 150; y++)
        printer_job_row(row);

    printer_job_end(true);
    test_heat_end();
    return true;
}
