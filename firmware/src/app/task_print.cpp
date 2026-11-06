/* ============================================================================
 * 文件：app/task_print.cpp
 * 层次：应用层 / 打印任务骨架
 * 职责：只有三件事——OTA 时冻结、按 jobState 分发到三个分支、循环。
 *        业务逻辑一行都不在这里（见 job_idle / job_overheat / job_run）。
 *
 * 为什么要单独一个任务：
 *   一行 384 点的加热是微秒级时序，中途不能切任务；但连续几百行全是
 *   delayMicroseconds 忙等会把同核任务饿死。所以打印、按键自检、走纸、
 *   长按出纸全部收敛到这一个任务里串行执行，靠 runtime.h 的事件标志触发。
 *   其它模块（BLE 回调、按键任务）只置标志，不直接调 printer_*。
 *
 * 任务参数（栈 4096 / 优先级 5 / core1）在 main.cpp 的 setup() 里创建，
 * 本文件只提供任务体。
 * ========================================================================== */

#include "job.h"
#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "printer.h"
#include "ota.h"
#include "job_internal.h"

void job_print_task(void *arg)
{
    (void)arg;

    for (;;)
    {
        /* OTA 写 flash 时打印头完全冻结：作业、按键自检、走纸都不许插进来。
         * 放在最前面，连 jobState 的判断都跳过，避免暂停中的作业被误恢复。 */
        if (ota_active())
        {
            if (hwHeld)
            {
                printer_job_end(false);
                hwHeld = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t st = jobState;

        if (st == 0)
        {
            job_idle_tick();
            continue;
        }

        if (st == 3)
        {
            job_overheat_tick();
            continue;
        }

        /* st == 1 或 2：真正打印 */
        job_run_tick(st);
    }
}
