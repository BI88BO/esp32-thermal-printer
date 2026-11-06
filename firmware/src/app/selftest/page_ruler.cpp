/* ============================================================================
 * 文件：app/selftest/page_ruler.cpp
 * 层次：应用层 / 诊断页
 * 职责：走纸标尺页——判断"打不出来"到底是加热还是走纸。
 * 依赖：selftest.h、common.h、draw.h、config.h、printer.h、printhead.h
 * ========================================================================== */

#include "selftest.h"
#include <Arduino.h>
#include "common.h"
#include "draw.h"
#include "config.h"
#include "printer.h"
#include "printhead.h"

/* 走纸标尺：一条实心行 + 20 个空白行（= 80 拍），重复 10 次。
 * 每一段应当推进 80 x 0.03125mm = 2.5mm，10 段共 25mm。
 * 如果 10 条线均匀排开 = 走纸正常，问题在加热；
 * 如果挤在一起 / 只有一条 = 走纸丢了步，跟加热无关。 */
bool selftest_feed_ruler(uint32_t timeout_ms)
{
    if (!printer_job_begin(timeout_ms))
        return false;
    test_heat_begin();

    uint8_t row[TPH_BYTES];

    draw_build_code(FW_BUILD_CODE);

    for (uint8_t m = 0; m < 10; m++) {
        memset(row, 0xFF, TPH_BYTES);
        printer_job_row_raw(row, printhead_get_heat_us(), 300);        /* 一条实心行 */

        memset(row, 0, TPH_BYTES);
        for (uint8_t i = 0; i < 20; i++)            /* 20 行空白 = 80 拍 */
            printer_job_row_raw(row, printhead_get_heat_us(), 300);
    }

    printer_job_end(true);
    test_heat_end();
    return true;
}
