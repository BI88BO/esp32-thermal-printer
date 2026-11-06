/* ============================================================================
 * 文件：app/task_monitor.cpp
 * 层次：应用层 / 监控任务
 * 职责：采集节奏 + 周期维护 + 状态上报。
 * 对应原工程：src/em_task.cpp 的 task_report()
 *
 * 两条容易踩错的约定：
 *   ① sensor_update(printing) 的参数只控制「要不要采电池」，NTC 温度是
 *      无条件采的——过温保护不能停。打印时 0.8A 负载会把电池电压拉低几百 mV，
 *      那时采到的值既不准还会误触发欠压保护。
 *   ② 状态上报节奏必须跟打印状态走：打印中 200ms 一次，因为 App 的流控靠
 *      doneLines 判断队列水位，1s 的粒度太粗，48 行队列会被灌爆。
 *
 * 依赖：hal/sensor.h、hal/printer.h、hal/printhead.h、hal/vh.h、
 *       hal/thermal_guard.h、hal/led.h、app/command.h、app/runtime.h
 * ========================================================================== */

#include "task_monitor.h"
#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "command.h"
#include "sensor.h"
#include "printhead.h"
#include "vh.h"
#include "thermal_guard.h"
#include "led.h"

void task_monitor(void *arg)
{
    (void)arg;

    uint32_t sec_ms  = 0;      // 1 秒累计
    uint32_t stat_ms = 0;      // 状态上报累计

    for (;;)
    {
        /* 打印 / 自检 / 走纸期间加密到 20ms 采一次：熔断阈值降到 50℃ 后，
         * 100ms 一采再叠中值+EMA 滤波，动作会慢半拍。空闲仍 100ms 省电。
         * 本任务固定在 core0，打印任务在 core1，两者互不抢占。 */
        bool busy = (jobState != 0 || pendingTest != 0 || pendingFeed != 0);
        uint32_t period = busy ? SENSOR_POLL_ACTIVE_MS : SENSOR_POLL_IDLE_MS;

        sensor_update(jobState != 0);   // 打印中跳过电池采样（负载压降会误判）
        vh_tick();
        /* 温度采样一直在跑。锁存随温度自动解除，断连后没有新作业也能
         * 自己恢复到可打印状态 */
        thermal_guard_tick();
        /* 状态灯：非阻塞状态机，靠 millis 推进，本任务只管周期叫它一声 */
        led_tick();

        sec_ms  += period;
        stat_ms += period;

        if (sec_ms >= 1000)
        {
            sec_ms = 0;
            /* 每秒按打印头温度重算加热时间（规格书表 4） */
            printhead_set_heat_us(sensor_heat_us_compensated(userHeatUs));
            sensor_print_debug();
        }

        /* 状态上报节奏：打印中 200ms 一次，空闲时 1s 一次 */
        if (jobState != 0)
        {
            if (stat_ms >= 200)
            {
                stat_ms = 0;
                if (bleConnected)
                    command_send_status();
            }
        }
        else if (sec_ms == 0)
        {
            stat_ms = 0;
            if (bleConnected)
                command_send_status();
        }

        vTaskDelay(pdMS_TO_TICKS(period));
    }
}
