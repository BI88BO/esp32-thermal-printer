/* ============================================================================
 * 文件：app/job_run.cpp
 * 层次：应用层 / 打印任务 · 运行分支
 * 职责：jobState=1/2 时的每一轮——
 *         ① 独占打印头（拿锁 + 开 VH，只在第一轮做）
 *         ② 三道体检：热熔断 / NTC 失效 / 缺纸
 *         ③ 从 lineQ 取一行打出去
 *         ④ 队列空时：判收尾（st=2）或判断流超时
 * 对应原工程：src/em_task.cpp 的 run_printer()
 *
 * 为什么「熔断转暂停」而不是「直接结束作业」：
 *   长图一旦过热就从头重打，永远打不完。所以行级熔断只把作业切成
 *   jobState=3 的暂停，队列和 doneLines 都留着，凉了从断点续打。
 *
 * 依赖：app/runtime.h、app/command.h、hal/printer.h、hal/sensor.h、
 *       hal/printhead.h、hal/thermal_guard.h、core/protocol.h
 * ========================================================================== */

#include "job_internal.h"
#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "command.h"
#include "printer.h"
#include "printhead.h"
#include "sensor.h"
#include "thermal_guard.h"
#include "protocol.h"

void job_run_tick(uint8_t st)
{
    uint8_t row[TPH_BYTES];

    /* ---- ① 独占打印头 ---- */
    if (!hwHeld)
    {
        if (!printer_job_begin(1000))
        {
            /* 作业开始前就被热锁存挡下：同样暂停等冷却，不杀作业 */
            if (thermal_guard_take_latched())
            {
                command_pause_overheat();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(50));   // 自检占着头，等下次再试
            return;
        }
        hwHeld = true;
    }

    /* ---- ② 三道体检 ---- */
    /* 行级熔断由 hal/printhead.cpp 在拉 STB 之前拦下，不依赖本任务的调度
     * 时机。这里把标志转成「暂停等冷却」，而不是结束作业报错 */
    if (thermal_guard_take_latched())
    {
        command_pause_overheat();
        return;
    }

    int8_t ht = sensor_head_temp();
    if (ht < 0)
    {
        /* NTC 失效不是过热，等也等不回来，直接中止并报错 */
        command_end_job(false, ERR_OVERHEAT);
        return;
    }
    if (ht >= OVERHEAT_C)
    {
        command_pause_overheat();
        return;
    }
    if (!sensor_paper_ok())
    {
        command_end_job(false, ERR_PAPER);
        return;
    }

    /* ---- ③ 取一行打出去 ---- */
    if (xQueueReceive(lineQ, row, 0) == pdTRUE)
    {
        /* 每个作业的第一行留个样本：数据是不是黑的、基准加热时间多少。
         * 这里打印的是 heat_us 基准值；实际每行还会乘冷头起步 / 热积累
         * 系数，作业结束时的 [THERM] 行会给出当时的热状态和削减比例。 */
        if (doneLines == 0 && totalLines > 0)
            LOGF("[ROW] %02X %02X %02X %02X  heat %u us\n",
                 row[0], row[1], row[2], row[3], printhead_get_heat_us());

        printer_job_row(row);
        doneLines++;
        lastRxMs = millis();
        return;
    }

    /* ---- ④ 队列空：收尾判定 ---- */
    if (st == 2 && (uint16_t)(doneLines + dropLines) >= totalLines)
    {
        if (dropLines > 0)
        {
            /* 丢过行，图是残的，但还是要退纸让用户看到，并明确报错 */
            LOGF("[PRINT] done with %u dropped of %u\n", dropLines, totalLines);
            command_end_job(true, ERR_PARAM);
        }
        else
        {
            command_end_job(true, 0);
            command_send_status();
            LOGF("[PRINT] done %u/%u\n", doneLines, totalLines);
        }
        return;
    }

    /* 队列空但还没收尾：等 App 继续发 */
    if (millis() - lastRxMs > RX_TIMEOUT_MS)
    {
        /* 一行都没打、但收到过坏帧 —— 不是断流，是协议对不上（多半是长度超限被丢） */
        uint8_t code = (doneLines == 0 && proto_bad_frames() > 0) ? ERR_PARAM : ERR_TIMEOUT;
        LOGF("[PRINT] timeout %u/%u, bad frames %u -> code %u\n",
             doneLines, totalLines, proto_bad_frames(), code);
        command_end_job(false, code);
        return;
    }

    /* 队列空、还没超时：短睡一下再来，别空转 */
    vTaskDelay(pdMS_TO_TICKS(2));
}
