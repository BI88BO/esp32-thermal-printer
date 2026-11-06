#ifndef RUNTIME_H
#define RUNTIME_H

#include <Arduino.h>

/* ============================================================================
 * 文件：app/runtime.h
 * 层次：应用层 / 全局运行状态
 * 职责：集中声明所有「跨任务、跨模块」共享的状态量。
 * 对应原工程：src/em_device.h + src/em_device.cpp（全局设备对象）
 *
 * 为什么要有这一层：
 *   固件是 FreeRTOS 多任务：BLE 回调（协议栈上下文）、printTask（core1）、
 *   buttonTask（core1）、monitorTask（core0）都会读写同一批状态。
 *   以前它们全是 main.cpp 里的 static 变量，只能靠文件位置隐式共享；
 *   拆成模块后必须有一个显式的「谁都能看见」的地方，否则就得满地传参数。
 *   集中放还带来一个好处：看这一个文件就能掌握整台机器的运行状态。
 *
 * 读写约定（很重要，改代码前先看这里）：
 *   - 被中断/回调与其它任务同时访问的量一律 volatile；
 *   - BLE 回调里只「置标志 / 写队列」，绝不做几毫秒以上的事；
 *   - 打印头相关的现场（hwHeld / resume*）只有 job_print_task 读写；
 *   - command_force_idle() 是唯一的统一清场入口，断连和异常收尾都走它。
 *
 * 依赖：config.h（LINE_QUEUE_LEN / TPH_BYTES）
 * 被谁用：app/command.cpp、app/job.cpp、app/tasks.cpp、hal/ble.cpp、main.cpp
 * ========================================================================== */

/* ---------- 打印数据队列 ----------
 * App 一次发 4 行（48B/行），队列深 48 行≈6mm。
 * setup() 创建，之后永不释放；CMD_PRINT_BEGIN 和 command_force_idle() 会 reset。 */
extern QueueHandle_t lineQ;

/* ---------- 作业状态机 ----------
 * jobState：0=空闲  1=收数据中  2=数据收完在收尾  3=过热暂停（冷却后续打）
 * 注意 3 不是错误态：队列、doneLines、电机相位全部保留，不许清。 */
extern volatile uint8_t  jobState;
extern volatile uint16_t totalLines;   // App 声明的总行数
extern volatile uint16_t doneLines;    // 已打行数，App 的流控水位
extern volatile uint16_t dropLines;    // 队列满丢弃的行数（>0 说明图是残的）
extern volatile uint32_t lastRxMs;     // 最近一次收到数据的时间，用于断流超时

/* ---------- 过热暂停的现场 ----------
 * 进入暂停时把 jobState 存下来，温度降到恢复线后原样写回。
 * 若这里被清掉，长图就会从头重打 —— 这是 build 85 修掉的老毛病。 */
extern uint8_t  resumeState;
extern uint32_t pauseSinceMs;

/* ---------- 参数与资源占用 ----------
 * userHeatUs 是 App 滑杆设的「基准浓度」，每秒由 monitorTask 按头温
 * 重新折算成补偿后的值再下发给打印头；两者不要混为一个概念。 */
extern uint16_t userHeatUs;
extern bool     hwHeld;          // printTask 是否已持有打印头互斥量

/* ---------- 事件标志（回调/按键只置位，printTask 统一执行） ----------
 * 这样 BLE 回调和按键任务都不会同步跑几秒的加热/走纸，RTOS 不会被饿死。 */
extern volatile uint8_t  pendingTest;        // 0=无 1=整页自检 2=点检 …
extern volatile uint16_t pendingFeed;        // CMD_FEED 请求的拍数
extern volatile bool     feedHeld;           // 长按连续出纸中
extern volatile uint32_t feedHeldSinceMs;    // 长按起点，用于 30s 兜底防卡键

/* ---------- 连接与调试 ---------- */
extern volatile bool bleConnected;
extern bool          firstPktLogged;  // 每个作业第一包数据只打一次串口样本

/* 附：统一的清场入口 command_force_idle() 声明在 command.h、
 * 实现在 app/command.cpp，由 hal/ble.cpp 的 onDisconnect 调用。
 * 本文件只放数据，不放函数。 */

#endif
