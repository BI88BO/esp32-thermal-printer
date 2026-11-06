#ifndef COMMAND_H
#define COMMAND_H

#include <Arduino.h>

/* ============================================================================
 * 文件：app/command.h
 * 层次：应用层 / 命令分发
 * 职责：把 BLE 帧翻译成动作，并维护作业生命周期的四个出口。
 * 对应原工程：src/em_task.cpp 里的命令/事件处理部分
 *
 * 四个出口（都在这里实现，别在别的模块里自己收摊）：
 *   command_end_job()        —— 作业正常/异常结束，关 VH、释放打印头、可选报错
 *   command_pause_overheat() —— 过热暂停，保留队列与断点，冷却后由 job.cpp 续打
 *   command_force_idle()     —— BLE 断连，彻底清场回到确定可用的空闲态
 *   command_send_status()    —— 组 STATUS 帧上报（打印中 200ms、空闲 1s）
 *
 * 依赖：core/protocol.h、core/ota.h、hal/printer.h、hal/sensor.h、app/runtime.h
 * 被谁用：main.cpp（初始化）、hal/ble.cpp（断连）、app/job.cpp、
 *         app/tasks.cpp（周期上报）、core/protocol.cpp（回调注册）
 * ========================================================================== */

/* 注册命令回调到协议层，setup() 里调一次 */
void command_init(void);

/* 上报当前状态：paper / temp / battery / state / doneLines。
 * state=2 表示过热暂停，App 据此停止发送、不重复点打印。 */
void command_send_status(void);

/* 结束当前作业。feed=true 退纸把内容露出来；err!=0 时回 ERROR 帧 */
void command_end_job(bool feed, uint8_t err);

/* 进入过热暂停：关 VH、释放打印头，但队列 / doneLines / 电机相位全部保留 */
void command_pause_overheat(void);

/* 断连清场：无条件关 VH + 清状态 + 复位队列（幂等，可在 BLE 回调里调用） */
void command_force_idle(void);

#endif
