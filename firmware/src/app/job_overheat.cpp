/* ============================================================================
 * 文件：app/job_overheat.cpp
 * 层次：应用层 / 打印任务 · 过热暂停分支
 * 职责：jobState=3 时的工作——保底断电、等降温、恢复续打、放弃条件。
 * 对应原工程：无。原工程过热直接中止，长图永远打不完。
 *
 * 断点续打为什么要保留这么多现场：
 *   暂停时 **不清队列、不清 doneLines、不动电机相位**，恢复时重新
 *   printer_job_begin()（内部清热锁存 + 预热 VH 1s）后从 lineQ 的下一行
 *   接着打。热积累模型状态另用 printer_thermal_pause_restore() 覆盖回去，
 *   否则 job_begin 会按停机时间把热状态冷却到 0，续打那几行突然深 15%，
 *   纸上留一道明显的浓淡分界。
 *
 * 放弃条件只有两个：
 *   ① NTC 失效（读不到温度，等也等不回来）
 *   ② 冷却超过 OVERHEAT_PAUSE_MAX_MS(3 分钟)，说明散热异常
 *
 * 依赖：app/runtime.h、app/command.h、hal/printer.h、hal/sensor.h、
 *       hal/printhead.h、hal/vh.h、hal/thermal_guard.h
 * ========================================================================== */

#include "job_internal.h"
#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "command.h"
#include "printer.h"
#include "printhead.h"
#include "sensor.h"
#include "vh.h"
#include "thermal_guard.h"
#include "protocol.h"

void job_overheat_tick(void)
{
    /* 暂停期每轮保底断电一次（幂等、只写几个 GPIO）：
     * 只要有任何路径误把 VH 打开，下一轮就会被拉回去 */
    vh_force_off();

    lastRxMs = millis();         // 冻结断流超时，别把暂停当断流
    int8_t ht = sensor_head_temp();

    if (ht < 0 || millis() - pauseSinceMs > OVERHEAT_PAUSE_MAX_MS)
    {
        /* NTC 失效不会自愈；冷却超时说明散热异常，别无限等下去 */
        LOGLN("[OVERHEAT] give up: NTC bad or cool-down timeout");
        command_end_job(false, ERR_OVERHEAT);
        return;
    }

    if (ht <= OVERHEAT_RECOVER_C && printer_job_begin(1000))
    {
        printer_thermal_pause_restore();        // 覆盖 job_begin 的停机冷却
        hwHeld   = true;
        jobState = resumeState;
        LOGF("[OVERHEAT] resume at %u/%u, head %dC\n",
             doneLines, totalLines, ht);
        command_send_status();
        return;
    }

    /* 还没凉到恢复线，或者打印头被别的作业占着，等下一轮 */
    vTaskDelay(pdMS_TO_TICKS(100));
}
