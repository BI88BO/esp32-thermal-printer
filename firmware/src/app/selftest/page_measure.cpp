/* ============================================================================
 * 文件：app/selftest/page_measure.cpp
 * 层次：应用层 / 诊断页
 * 职责：VH 压降测量页。两段窗口，方便万用表夹表笔。
 * 对应原工程：无
 * 依赖：selftest.h、common.h、config.h、printer.h、sensor.h
 *
 * 这是专业测量页，不通过按键 / 点检按钮触发（避免误触把纸打光）。
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "config.h"
#include "printer.h"
#include "sensor.h"

/* 测量模式：给万用表留出两次窗口
 *   1) 前 15 秒 VH 打开但不打印 —— 量空载电压
 *   2) 接着连续 150 行全黑 —— 量带载压降
 * 全程纸只在第二阶段走，第一阶段板子完全静止，方便夹表笔。
 * 负载脉宽 = 当前正常打印能量。 */
bool selftest_measure(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();

    /* 15 秒空载窗口：分 150 段 vTaskDelay，而不是一次睡到底。
     * 每段都回调度器，BLE / 温度采样 / 按键照常工作；头已经热了就提前收工，
     * 不无谓占着打印头锁。 */
    for (uint16_t i = 0; i < 150; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        int8_t ht = sensor_head_temp();
        if (ht < 0 || ht >= OVERHEAT_C)
            break;
    }

    uint8_t row[TPH_BYTES];
    memset(row, 0xFF, TPH_BYTES);
    for (uint16_t y = 0; y < 150; y++)
        printer_job_row(row);

    printer_job_end(true);
    test_heat_end();
    return true;
}
