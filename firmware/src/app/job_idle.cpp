/* ============================================================================
 * 文件：app/job_idle.cpp
 * 层次：应用层 / 打印任务 · 空闲分支
 * 职责：jobState=0 时处理三件一次性事件——
 *         ① 释放打印头（上一个作业可能刚结束）
 *         ② 长按连续出纸（feedHeld）
 *         ③ 自检 / 走纸请求（pendingTest / pendingFeed）
 *
 * 事件全都由别人置位，这里只负责执行：
 *   BLE 回调 → command.cpp 置 pendingTest/pendingFeed
 *   按键任务 → task_button.cpp 置 pendingTest 或 feedHeld
 * 这样保证「碰打印头的只有打印任务」这一条铁律。
 *
 * 依赖：app/runtime.h（事件标志与队列）、app/command.h、
 *       hal/printer.h、hal/sensor.h、hal/led.h、app/selftest.h
 * ========================================================================== */

#include "job_internal.h"
#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "command.h"
#include "printer.h"
#include "printhead.h"
#include "sensor.h"
#include "selftest.h"
#include "led.h"
#include "vh.h"
#include "thermal_guard.h"
#include "protocol.h"

void job_idle_tick(void)
{
    /* 进空闲说明上一个作业已经收尾。这里再兜一次：任何路径漏放锁，
     * 下一轮就会被释放，不会永远占着打印头 */
    if (hwHeld)
    {
        printer_job_end(false);
        hwHeld = false;
    }

    /* ---- 长按连续出纸 ----
     * 按住期间每轮只走一小段就回来检查按键，松手即停。
     * printer_feed 内部每拍都会让出，不会独占 core1；这里 return 掉
     * 下面的 20ms 等待，保持连续出纸的手感。 */
    if (feedHeld)
    {
        /* 松手事件万一丢了也不能无限走纸 */
        if (millis() - feedHeldSinceMs > LONG_FEED_MAX_MS)
        {
            feedHeld = false;
            LOGLN("[KEY] feed timeout, auto stop");
        }
        else
        {
            vh_force_off();                 // 走纸不加热，VH 必须关着
            printer_feed(LONG_FEED_CHUNK_BEATS);
            return;                         // 不睡，立刻回来再看按键
        }
    }

    /* ---- 自检 / 诊断页 ---- */
    if (pendingTest)
    {
        uint8_t k = pendingTest;
        pendingTest = 0;

        /* 缺纸时诊断页一律不出纸：诊断页既加热又走纸，没纸只会空转、
         * 白白磨发热体。正式打印在 CMD_PRINT_BEGIN 里就挡住了，
         * 但自检 / 按键这条路径以前完全不查纸——"App 提示缺纸却还能
         * 打出东西"说的就是这里。
         * 事件在这里已经消耗掉，装纸后重新按一次即可，不会反复重试。 */
        if (!sensor_paper_ok())
        {
            LOGLN("[TEST] no paper, skipped");
            led_blink(LED_WARN_TIMES, LED_WARN_ON_MS, LED_WARN_OFF_MS);
            if (bleConnected)
                proto_send_error(ERR_PAPER);
        }
        else
        {
            bool ok;

            /* 按键/开机自检同样先按实测头温刷新，别用上一秒的旧补偿 */
            printhead_set_heat_us(sensor_heat_us_compensated(userHeatUs));

            if (k == 2)
                ok = selftest_dot_check(500);
            else if (k == 3)
                ok = selftest_load(500);
            else if (k == 4)
            {
                /* 15 秒空载（量电压）+ 4 秒满载（量压降）+ 点检页 */
                selftest_measure(500);
                ok = selftest_dot_check(500);
            }
            else if (k == 5)
            {
                ok = selftest_measure(500);
            }
            else if (k == 6)
                ok = selftest_waveform(500);        /* App selftest(5)：波形复刻页 */
            else if (k == 7)
                ok = selftest_feed_ruler(500);      /* App selftest(6)：走纸标尺页 */
            else
                ok = selftest_print(500);

            LOGF("[TEST] kind %u %s\n", k, ok ? "done" : "busy");

            if (thermal_guard_take_latched())
            {
                LOGLN("[TEST] OVERHEAT: aborted");
                if (bleConnected)
                    proto_send_error(ERR_OVERHEAT);
            }
        }
    }

    /* ---- 单次走纸（App 的 CMD_FEED） ---- */
    if (pendingFeed)
    {
        uint16_t beats = pendingFeed;
        pendingFeed = 0;
        printer_feed(beats);        /* 走纸内部每拍都会 vTaskDelay 让出 */
    }

    /* 空闲轮询 20ms：够快，又不至于空转烧 CPU */
    vTaskDelay(pdMS_TO_TICKS(20));
}
