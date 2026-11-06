/* ============================================================================
 * 文件：hal/vh.h
 * 层次：硬件抽象层 (HAL) / 打印头升压电源
 * 职责：只负责 VH（7.2V 升压）的开关与有效电平，以及"关掉后延时断电"。
 *            抽出来的目的：这是打印头唯一的电源开关，过热/断连/暂停都要碰它，
 *            集中一处才不会漏关（build 87 就是漏了三条路径导致不降温）。
 *
 * 依赖：config.h（VH_ACTIVE_LOW / VH_SETTLE_MS / VH_HOLD_MS / VH_ALWAYS_ON） *       hal/printhead.h 的 printhead_stb_idle()（开关 VH 前后必须让 STB 空闲）
 * 被谁用：hal/printhead.cpp（熔断时立即断电）、hal/printer.cpp（作业开始/结束）、
 *        app/job.cpp（过热暂停保底）、app/command.cpp（结束作业 / 断连清场）、
 *        app/tasks.cpp（100ms 周期处理延迟断电窗口）
 *
 * 时序要求（规格书 3.7）：
 *   (b) 不打印时关闭 VH
 *   (c) 打开 VH 前先让 STB 全低，避免锁存器残留在升压建立瞬间误加热
 *   (d) 打印结束立刻关闭
 * ========================================================================== */
#ifndef VH_H
#define VH_H

#include <Arduino.h>

/* 引脚初始化 + 上电保持关断。由 printer_begin() 调用一次 */
void vh_init(void);

/* 开 / 关。开的时候内部会 delay(VH_SETTLE_MS) 等升压建立；
 * 关的时候默认走"延迟断电窗口"，由 vh_tick() 到点真正切断 */
void vh_set(bool on);

/* 立即强制切断，不等延迟窗口、不依赖任何任务调度。
 * 唯一能保证"现在就断电"的接口：过热熔断、暂停、BLE 断连都必须用它。
 * 幂等，可以在 BLE 回调甚至一行加热内部安全调用 */
void vh_force_off(void);

bool vh_is_on(void);

/* 由 monitorTask 周期调用（100ms）：延迟断电窗口到点后真正切断 */
void vh_tick(void);

#endif
