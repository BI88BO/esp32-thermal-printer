/* ============================================================================
 * 文件：hal/button.cpp
 * 层次：硬件抽象层 (HAL) / 按键去抖
 * 职责：10ms 轮询一次，连续 KEY_DEBOUNCE 次电平一致才认；按住达到
 *       KEY_LONG_MS 立刻发 BTN_LONG_START，松手再发 BTN_LONG_RELEASE。
 *
 * 不要在按键回调里同步循环走纸——那是裸机单循环的做法；在 FreeRTOS 上
 * 只发事件，走纸由 app/job_idle.cpp 分块执行，期间其它任务照常调度。
 *
 * 上电时若按键正好被按住，pressMs 取当前 millis() 而不是 0，
 * 否则 millis()-0 会立刻误触发一次长按出纸。
 * ========================================================================== */

#include "button.h"
#include <Arduino.h>
#include "config.h"

/* 稳定状态：true = 按下（低电平有效） */
static bool     stable = false;
static uint8_t  diffCnt = 0;        // 连续不一致次数
static uint32_t pressMs = 0;
static bool     longSent = false;   // 本次按住是否已经发过 LONG_START

void button_begin()
{
    pinMode(PIN_KEY, INPUT_PULLUP);
    stable = (digitalRead(PIN_KEY) == LOW);
    diffCnt = 0;
    longSent = false;
    /* 上电时按键正好被按住也别当成「已经长按了 1.2 秒」：
     * 以现在为计时零点，否则 millis()-0 会立刻触发一次出纸 */
    pressMs = millis();
}

ButtonEvent button_poll()
{
    bool raw = (digitalRead(PIN_KEY) == LOW);

    if (raw != stable) {
        if (++diffCnt >= KEY_DEBOUNCE) {
            stable = raw;
            diffCnt = 0;

            if (stable) {                   // 按下
                pressMs = millis();
                longSent = false;
            } else {                        // 松开
                uint32_t held = millis() - pressMs;
                if (longSent || held >= KEY_LONG_MS)
                    return BTN_LONG_RELEASE;   // 长按结束，停纸
                if (held >= KEY_SHORT_MS)
                    return BTN_SHORT;          // 普通短按，点检页
            }
        }
    } else {
        diffCnt = 0;
    }

    /* 按住到点就发一次 START，不等松手 —— 用户能立刻感觉到纸在走。
     * longSent 保证整个按住期间只发一次；松手时再发 RELEASE 收尾。 */
    if (stable && !longSent && (millis() - pressMs) >= KEY_LONG_MS) {
        longSent = true;
        return BTN_LONG_START;
    }

    return BTN_NONE;
}
