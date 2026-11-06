/* ============================================================================
 * 文件：app/command.cpp
 * 层次：应用层 / 命令分发
 * 职责：BLE 命令 → 具体动作；作业生命周期的收尾、暂停、清场、状态上报。
 * 对应原工程：src/em_task.cpp 的命令处理部分
 *
 * 执行上下文提醒：
 *   onCommand() 由协议层在 BLE 回调里调用。这里所有的「重活」都只置标志：
 *     自检 → pendingTest，走纸 → pendingFeed，打印数据 → 入 lineQ 队列，
 *     真正的加热/走纸由 app/job.cpp 的 printTask 统一执行。
 *   唯一例外是 OTA_BEGIN/DATA 这类纯入队操作（写 flash 在 core/ota.cpp 的
 *   独立任务里）。这条约束保证蓝牙协议栈不会被几秒的忙等卡住。
 *
 * 状态机（jobState）与队列定义在 app/runtime.cpp，改状态前先看 runtime.h 的约定。
 * ========================================================================== */

#include "command.h"
#include <Arduino.h>
#include "config.h"
#include "protocol.h"
#include "printer.h"
#include "sensor.h"
#include "ota.h"
#include "runtime.h"
#include "led.h"
#include "printhead.h"
#include "vh.h"

/* ==================== 作业生命周期的四个出口 ==================== */

void command_send_status(void)
{
    /* state: 0=空闲 1=打印/收尾 2=过热暂停。
     * App 靠 2 区分「打印机在冷却、别超时、也别重新点打印」 */
    uint8_t st = (jobState == 0) ? 0 : ((jobState == 3) ? 2 : 1);
    proto_send_status(sensor_paper_ok() ? 0 : 1,
                      (uint8_t)(sensor_head_temp() < 0 ? 0 : sensor_head_temp()),
                      sensor_battery_mv(), st, doneLines,
                      sensor_battery_pct());
}

void command_end_job(bool feed, uint8_t err)
{
    /* 任何结束路径都无条件断电。以前只在 hwHeld 时借 printer_job_end() 关，
     * 自检 / 开局被过热挡下这些 hwHeld=false 的路径会漏掉 */
    vh_force_off();
    if (hwHeld)
    {
        printer_job_end(feed);
        hwHeld = false;
    }
    jobState = 0;
    if (err != 0)
        proto_send_error(err);
}

void command_pause_overheat(void)
{
    if (jobState == 3)
        return;

    resumeState  = jobState;
    printer_thermal_pause_save();     // 热状态也保留，续打不出现浓淡跳变

    /* 无论打印头是不是本任务持有的，先无条件断电。
     * 自检 / 作业开始前被过热挡下这些路径里 hwHeld 可能已经是 false，
     * 只靠 printer_job_end() 就会把 VH 一直留着——头自然降不下温。 */
    vh_force_off();
    if (hwHeld)
    {
        printer_job_end(false);      // 给回互斥量；不退纸、不走纸
        hwHeld = false;
    }
    jobState     = 3;
    pauseSinceMs = millis();
    lastRxMs     = millis();         // 暂停期间冻结断流超时
    /* 过热告警：闪 3 次。非阻塞，不会拖住 BLE 回调（原工程是 delay 硬等） */
    led_blink(LED_WARN_TIMES, LED_WARN_ON_MS, LED_WARN_OFF_MS);
    LOGF("[OVERHEAT] pause at %u/%u, head %dC, VH forced OFF\n",
         doneLines, totalLines, sensor_head_temp());
    command_send_status();           // 立刻让 App 知道，别等下一个 200ms
}

void command_force_idle(void)
{
    bool wasOn = vh_is_on();
    vh_force_off();

    jobState       = 0;
    resumeState    = 1;
    pauseSinceMs   = 0;
    totalLines     = 0;
    doneLines      = 0;
    dropLines      = 0;
    lastRxMs       = millis();
    pendingTest    = 0;
    pendingFeed    = 0;
    feedHeld       = false;
    firstPktLogged = false;
    if (lineQ != nullptr)
        xQueueReset(lineQ);

    LOGF("[BLE] force idle, VH %s -> OFF\n", wasOn ? "ON" : "OFF");
    /* hwHeld 留给 printTask 的 st==0 分支释放：BLE 回调里不碰互斥量 */
}

/* ==================== 命令分发 ====================
 * 帧格式与命令码见 newproject/PROTOCOL.md。
 * 这个函数是协议层 proto_init() 注册的回调，只做校验 + 置状态，不干重活。 */
static void onCommand(uint8_t type, const uint8_t *p, uint8_t n)
{
    /* OTA 期间冻结一切打印活动。放行 OTA 自己的命令和状态查询，
     * 其余（打印 / 自检 / 走纸 / 改浓度）一律拒绝：一边擦 app 分区一边
     * 拉 STB，纸会错位，VH 也可能来不及关。 */
    if (ota_active() &&
        type != CMD_OTA_INFO && type != CMD_OTA_BEGIN && type != CMD_OTA_DATA &&
        type != CMD_OTA_END && type != CMD_OTA_ABORT && type != CMD_GET_STATUS)
    {
        proto_send_error(ERR_PARAM);
        return;
    }

    switch (type)
    {
    case CMD_PRINT_BEGIN:
    {
        if (n < 2)
        {
            proto_send_error(ERR_PARAM);
            return;
        }
        /* 过热暂停中忽略重复下发：App 若在暂停时又发一次 BEGIN，
         * 会 reset 队列和 doneLines，长图就前功尽弃。回 ACK 保持幂等 */
        if (jobState == 3)
        {
            proto_send_ack(CMD_PRINT_BEGIN, 0);
            return;
        }
        if (!sensor_paper_ok())
        {
            proto_send_error(ERR_PAPER);
            return;
        }
        /* NTC 失效（返回 -1）也要拒绝：不能让 -1 >= 50 恒假把保护绕过 */
        int8_t ht = sensor_head_temp();
        if (ht >= OVERHEAT_C || ht < 0)
        {
            proto_send_error(ERR_OVERHEAT);
            return;
        }
        if (sensor_battery_mv() < BAT_LOW_MV)
        {
            proto_send_error(ERR_LOW_BAT);
            return;
        }

        /* 作业开始前按「当前实测头温」刷新一次能量：后台每秒才刷一次，
         * 刚开机或刚打完一张、头温变了，第一张会用到上一秒的旧补偿值 */
        printhead_set_heat_us(sensor_heat_us_compensated(userHeatUs));

        totalLines = (uint16_t)(p[0] | (p[1] << 8));
        doneLines  = 0;
        dropLines  = 0;
        xQueueReset(lineQ);
        proto_reset_stats();
        firstPktLogged = false;
        lastRxMs = millis();
        jobState = 1;
        led_blink(1, LED_SHORT_ON_MS, 0);      // 开始打印（对应原工程 LED_PRINTER_START）
        proto_send_ack(CMD_PRINT_BEGIN, 0);
        LOGF("[PRINT] begin %u lines\n", totalLines);
        break;
    }

    case CMD_PRINT_DATA:
    {
        if (n < 1)
            return;
        uint8_t rows = (uint8_t)((n - 1) / TPH_BYTES);

        /* 第一包把前几个字节打出来：全 0 说明图上本来就没有黑点，
         * 那打到纸上当然是白的，跟打印头无关 */
        if (!firstPktLogged)
        {
            firstPktLogged = true;
            if (n >= 5)
                LOGF("[DATA] row0 head: %02X %02X %02X %02X\n",
                     p[1], p[2], p[3], p[4]);
        }

        for (uint8_t i = 0; i < rows; i++)
        {
            if (xQueueSend(lineQ, p + 1 + i * TPH_BYTES, 0) != pdTRUE)
                dropLines++;                    // 队列满，丢弃但不断连接
        }
        lastRxMs = millis();
        break;
    }

    case CMD_PRINT_END:
        /* 暂停中收到 END 不能直接改 jobState（会把 3 覆盖掉），
         * 但要记住「数据已发完」：恢复后必须按收尾状态 2 跑，
         * 否则队列空了会等 3 秒断流超时、把已打完的图判成失败 */
        if (jobState == 3)
            resumeState = 2;
        else if (jobState == 1)
            jobState = 2;
        break;

    case CMD_FEED:
        if (n >= 2 && jobState == 0)
            pendingFeed = (uint16_t)(p[0] | (p[1] << 8));
        break;

    case CMD_SET_HEAT:
        if (n >= 2)
        {
            userHeatUs = (uint16_t)(p[0] | (p[1] << 8));
            printhead_set_heat_us(sensor_heat_us_compensated(userHeatUs));
            proto_send_ack(CMD_SET_HEAT, 0);
        }
        break;

    case CMD_SET_QUALITY:
        if (n >= 4)
        {
            printhead_set_preheat((uint16_t)(p[0] | (p[1] << 8)));
            printhead_set_repeat(p[2]);
            printhead_set_dot_comp(p[3]);
            proto_send_ack(CMD_SET_QUALITY, 0);
            LOGF("[QUAL] preheat %u, repeat %u, dotcomp %u%%\n",
                 printhead_get_preheat(), printhead_get_repeat(), printhead_get_dot_comp());
        }
        break;

    case CMD_SET_DRIVE:
        if (n >= 1 && jobState == 0)        // 打到一半换拍数会让纸错位
        {
            printhead_set_drive_mode(p[0]);
            proto_send_ack(CMD_SET_DRIVE, 0);
            LOGF("[DRIVE] %s, %u beats/line, line %u us\n",
                 printhead_get_drive_mode() ? "2-2" : "1-2",
                 printhead_get_drive_mode() ? 2 : 4, printhead_line_us());
        }
        break;

    /* 自检放到打印任务里跑：BLE 回调上下文不能阻塞好几秒，
     * 否则协议栈会卡住，App 那边表现为"连接假死" */
    case CMD_SELFTEST:
        if (n >= 1 && jobState == 0)
            pendingTest = (p[0] == 0) ? 1 : (uint8_t)(p[0] + 1);
        break;

    case CMD_GET_STATUS:
        command_send_status();
        break;

    /* ---- BLE OTA：写 flash 全部由 core/ota.cpp 的独立任务完成 ---- */
    case CMD_OTA_INFO:
        ota_send_info();
        break;

    case CMD_OTA_BEGIN:
    {
        if (n < 8)
        {
            /* 借 ota 层的参数检查回一个标准 OTA ACK，别让 App 干等 */
            ota_begin(0, 0, true);
            break;
        }
        uint32_t sz = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t cr = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        bool idle = (jobState == 0 && pendingTest == 0 &&
                     pendingFeed == 0 && !hwHeld);
        ota_begin(sz, cr, idle);
        break;
    }

    case CMD_OTA_DATA:
        ota_push(p, n);
        break;

    case CMD_OTA_END:
        ota_finish();
        break;

    case CMD_OTA_ABORT:
        ota_abort();
        break;

    default:
        /* 未知命令：回一个带错误码的 ACK（code != 0）。
         * 不这么做的话，对端（旧版 App、串口调试脚本、或者命令号写错）
         * 只能干等到自己的超时，串口日志里也看不到任何线索。
         * build 95 删掉 0x14/0x15 之后，这条路径才真正有可能被走到。 */
        proto_send_ack(type, 1);
        break;
    }
}

void command_init(void)
{
    proto_init(onCommand);
}
