/* ============================================================================
 * 文件：hal/led.cpp
 * 层次：硬件抽象层 (HAL) / 状态指示灯
 * 职责：非阻塞闪烁状态机。语义见 led.h。
 * 对应原工程：src/hal/em_hal.cpp 的 run_led()（原工程用 delay 阻塞）
 *
 * 状态机：把"闪 N 次"展开成时间轴——
 *   第 k 个半周期结束时切换一次电平，总共 2*N 个半周期。
 * 用 millis() 差值推进，所以 tick 间隔略大也只是边沿抖动，不会漏闪或卡死。
 * ========================================================================== */

#include "led.h"
#include <Arduino.h>
#include "config.h"

static uint8_t  remain_half = 0;      // 还剩几个半周期
static uint16_t on_ms_c     = 0;
static uint16_t off_ms_c    = 0;
static uint32_t next_at     = 0;      // 下一次切换的时刻
static bool     level_on    = false;

static inline void led_write(bool on)
{
#if LED_ENABLE
    digitalWrite(PIN_LED, LED_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
#else
    (void)on;
#endif
}

void led_init(void)
{
    remain_half = 0;
    level_on    = false;
#if LED_ENABLE
    pinMode(PIN_LED, OUTPUT);
#endif
    led_write(false);
}

void led_set(bool on)
{
    remain_half = 0;
    level_on    = on;
    led_write(on);
}

void led_blink(uint8_t times, uint16_t on_ms, uint16_t off_ms)
{
    if (times == 0)
        return;

    on_ms_c     = on_ms;
    off_ms_c    = off_ms;
    remain_half = (uint8_t)(times * 2);
    level_on    = true;
    led_write(true);
    next_at     = millis() + on_ms;
}

void led_tick(void)
{
    if (remain_half == 0)
        return;

    if ((int32_t)(millis() - next_at) < 0)
        return;

    remain_half--;
    level_on = !level_on;
    led_write(level_on);

    /* 亮→灭用 off_ms，灭→亮用 on_ms；最后半周期结束后保持熄灭 */
    uint16_t d = (remain_half == 0) ? 0 : (level_on ? on_ms_c : off_ms_c);
    next_at = millis() + d;
}

bool led_busy(void)
{
    return remain_half != 0;
}
