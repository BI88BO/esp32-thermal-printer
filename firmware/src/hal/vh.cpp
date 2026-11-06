/* ============================================================================
 * 文件：hal/vh.cpp
 * 层次：硬件抽象层 (HAL) / 打印头升压电源
 * 职责：VH 引脚的开关、延迟断电窗口、强制断电。
 * 对应原工程：原工程散落在 em_printer.cpp 的 vh_on/vh_off 调用。
 * 依赖：config.h、hal/printhead.h（printhead_stb_idle）
 * ========================================================================== */

#include "vh.h"
#include <Arduino.h>
#include "config.h"
#include "printhead.h"

/* 极性是硬件接线属性（见 config.h 的 VH_ACTIVE_LOW 与实测结论），
 * 正常不会变，所以只在编译期决定，不提供运行时切换。 */
static bool     vh_on     = false;
static uint32_t vh_off_at = 0;      /* 延迟断电时刻，0 = 无计时 */

static inline void vh_pin(bool on)
{
#if VH_ACTIVE_LOW
    digitalWrite(PIN_VHEN, on ? LOW : HIGH);
#else
    digitalWrite(PIN_VHEN, on ? HIGH : LOW);
#endif
}

void vh_init(void)
{
    pinMode(PIN_VHEN, OUTPUT);
    vh_pin(false);              // 先按实际极性关断
    vh_on     = false;
    vh_off_at = 0;
    /* 上电保持关断：手册 3.7(b) 不打印时 VH 必须关闭，不做常开/预充 */
}

void vh_set(bool on)
{
    if (on)
    {
        vh_off_at = 0;                          /* 有活动就取消断电计时 */

        if (vh_on)
            return;

        printhead_stb_idle();                   /* 手册 3.7(c) */
        vh_pin(true);
        delay(VH_SETTLE_MS);                    /* 等 boost + 开关管建立 */
        vh_on = true;
        return;
    }

    delay(1);                                   /* 让最后一行加热完整结束 */

    if (vh_on && VH_ALWAYS_ON)
        return;

    /* 手册 3.7(b)(d) 要求不打印就断电，但这颗开关管爬升很慢，
     * 所以先保持 VH_HOLD_MS，超时再由 vh_tick() 真正切断 */
    if (VH_HOLD_MS > 0)
    {
        if (vh_on && vh_off_at == 0)
            vh_off_at = millis() + VH_HOLD_MS;
        return;
    }

    if (vh_on)
    {
        printhead_stb_idle();
        vh_pin(false);
        vh_on     = false;
        vh_off_at = 0;
    }
}

/* 无条件切断 VH。过热 / 断连时不能走"安排一个延迟、等 monitorTask 来关"
 * 那条路：那依赖任务调度，还可能被后续一次 vh_set(true) 取消。 */
void vh_force_off(void)
{
    printhead_stb_idle();
    vh_pin(false);
    vh_on     = false;
    vh_off_at = 0;
}

bool vh_is_on(void)
{
    return vh_on;
}

/* 由 monitorTask 每 100ms 调一次：保持窗口过了才真正切断 VH */
void vh_tick(void)
{
    if (!vh_on || vh_off_at == 0)
        return;

    if ((int32_t)(millis() - vh_off_at) < 0)
        return;

    vh_force_off();
}
