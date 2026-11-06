/* ============================================================================
 * 文件：app/task_button.cpp
 * 层次：应用层 / 按键任务
 * 职责：10ms 轮询按键，把短按 / 长按开始 / 长按松手翻译成事件标志。
 * 对应原工程：src/em_task.cpp 的 task_button() 与 key_*_handle()
 *
 * 为什么只置标志、不直接执行：
 *   原工程的长按是在按键任务里同步 motor_start/stop 一直跑。那是裸机单循环，
 *   在 FreeRTOS 上会独占一个核几秒。这里改成「置 feedHeld 标志，由打印任务
 *   每轮走 LONG_FEED_CHUNK_BEATS 拍」，松手最多多走 0.25mm，且按键任务
 *   始终能及时响应松开事件。
 *
 * 依赖：hal/button.h（去抖与事件）、app/runtime.h（事件标志）
 * ========================================================================== */

#include "task_button.h"
#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "button.h"

void task_button(void *arg)
{
    (void)arg;
    button_begin();

    for (;;)
    {
        ButtonEvent e = button_poll();

        if (e == BTN_SHORT)
        {
            /* 短按 = 点检页（版本号 + 能量阶梯 + 384 竖条），不连手机也能验机。
             * 只置事件，由打印任务统一执行：按键任务以前直接同步跑几百行，
             * 等于在 core1 上连续忙等几秒，把自己和温度/状态任务全饿死。 */
            pendingTest = 2;
        }
        else if (e == BTN_LONG_START)
        {
            /* 参考原工程 key_long_click_handle()：按住到点就开始走纸。
             * 这里只置标志，真正走纸由打印任务分块执行 */
            feedHeld = true;
            feedHeldSinceMs = millis();
            LOGLN("[KEY] long press: feed start");
        }
        else if (e == BTN_LONG_RELEASE)
        {
            /* 对应原工程 key_long_click_free_handle() 的 motor_stop() */
            feedHeld = false;
            LOGLN("[KEY] long press: feed stop");
        }

        vTaskDelay(pdMS_TO_TICKS(10));           // 按键 10ms 轮询
    }
}
