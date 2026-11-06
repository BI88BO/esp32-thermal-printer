/* ============================================================================
 * 文件：app/selftest/page_dotcheck.cpp
 * 层次：应用层 / 诊断页
 * 职责：打印头点检页（最常用的一页）。三段结构：
 *         ① 顶部 2 位版本号 + 3 位实测头温（℃）        → draw.cpp
 *         ② 能量阶梯：当前浓度的 57/68/83/100% 四个 5mm 实心块
 *         ③ 384 根竖条（分 4 相错开，看哪个点弱、哪个点死）
 * 依赖：selftest.h、common.h、draw.h、config.h、printer.h、printhead.h
 *
 * 四档比例来自规格书表 4（0.11/0.13/0.16/0.192 mJ），顶档 = 固件
 * userHeatUs 经实测头温补偿后的 heat_us。所以：
 *   - 未连 App 时用 config.h 的 3600 默认值；
 *   - 连上 App 后由 CMD_SET_HEAT 决定，App 必须先下发再发 SELFTEST；
 *   - 头温升高时表 4 系数下降，纸上的 4 位数字会跟着变小，这是设计。
 * 安全由 50℃ 熔断 + HEAT_US_MAX 兜底，不用标称 E0 折算的 800µs 窗口。
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "draw.h"
#include "config.h"
#include "printer.h"
#include "printhead.h"

bool selftest_dot_check(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();
    /* 相对标尺的顶档 = 本次作业实际用的加热时间（已含温度补偿） */
    uint16_t base_us = printhead_get_heat_us();

    /* 挂起 App 设过的质量档位：预加热 / 重复加热 / 点补偿都会让同一档位的
     * 实际能量成倍增加。诊断页必须是一把干净的尺子，打完再恢复。 */
    uint16_t old_preheat = printhead_get_preheat();
    uint8_t  old_repeat  = printhead_get_repeat();
    uint8_t  old_dotcomp = printhead_get_dot_comp();
    printhead_set_preheat(0);
    printhead_set_repeat(1);
    printhead_set_dot_comp(0);

    uint8_t row[TPH_BYTES];

    draw_build_code(FW_BUILD_CODE);

    /* ---------- 能量阶梯：按规格书表 4 的能量比例做相对标尺 ----------
     *   0.11 / 0.13 / 0.16 / 0.192 mJ 对应 57 / 68 / 83 / 100 %
     *   （正是原先 460/540/670/800µs 之间的比例关系）。
     * 顶档取本次作业的 base_us，四档随 App 浓度和头温自动缩放：
     *   27℃ 冷态 base≈3550µs -> 2020 / 2410 / 2950 / 3550µs，
     *   这才是这块拆机头能出色、能分出深浅的区间。
     * 纸上右侧打印实际脉宽（4 位），一眼能对上 App 当前浓度。 */
    static const uint8_t STEP_PCT[4] = {57, 68, 83, 100};
    /* 用 const 而不是 #define：宏在函数体中间定义会一直泄漏到文件末尾，
     * 后面再加别的诊断页时容易撞名字 */
    const uint8_t BAR_ROWS  = 40;        /* 5mm 高 */
    const uint8_t BAR_GAP   = 16;        /* 2mm 间隔 */
    const uint8_t BAR_BYTES = 35;        /* 前 280 点（35mm）实心，右侧印数字 */

    for (uint8_t i = 0; i < 4; i++) {
        uint16_t us = (uint16_t)((uint32_t)base_us * STEP_PCT[i] / 100);
        if (us < HEAT_US_MIN) us = HEAT_US_MIN;

        printhead_set_heat_override(us);

        for (uint8_t y = 0; y < BAR_ROWS; y++) {
            memset(row, 0, TPH_BYTES);
            memset(row, 0xFF, BAR_BYTES);

            /* 4 位数字 4 倍放大，垂直居中在块里 */
            if (y >= 10 && y < 30)
                draw_num_row(row, 288, us, (uint8_t)(y - 10), 4, 4);

            printer_job_row(row);
        }

        printhead_set_heat_override(0);       /* 块间留白回到正常能量，不留残值 */

        for (uint8_t y = 0; y < BAR_GAP; y++) {
            memset(row, 0, TPH_BYTES);
            printer_job_row(row);
        }
    }

    /* ---------- 384 根竖条 ---------- */
    memset(row, 0, TPH_BYTES);
    for (uint8_t y = 0; y < 4; y++)
        printer_job_row(row);

    for (uint8_t phase = 0; phase < 4; phase++) {
        for (uint8_t y = 0; y < 12; y++) {
            memset(row, 0, TPH_BYTES);
            for (uint16_t x = phase; x < TPH_DOTS; x += 4)
                row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            printer_job_row(row);
        }

        /* 段间留白，便于分辨是哪一段 */
        memset(row, 0, TPH_BYTES);
        for (uint8_t y = 0; y < 4; y++)
            printer_job_row(row);
    }

    printer_job_end(true);
    test_heat_end();

    printhead_set_preheat(old_preheat);
    printhead_set_repeat(old_repeat);
    printhead_set_dot_comp(old_dotcomp);
    return true;
}
