/* ============================================================================
 * 文件：hal/printer.h
 * 层次：硬件抽象层 (HAL) / 打印头门面
 * 职责：作业级 API（begin / row / end / feed）。这是上层唯一该调的头文件：
 *        一次"作业"从 begin 到 end 独占打印头，中途不会被打断。
 * 对应原工程：src/hal/em_printer.h
 *
 * 本文件只是门面，真正的实现分散在四个模块，想改哪块去对应文件：
 *   VH 升压开关        → hal/vh.h
 *   热保护与锁存       → hal/thermal_guard.h
 *   STB / SPI / 脉宽   → hal/printhead.h
 *   相序表 / 走纸节拍  → hal/motor.h
 *   该加热多少 µs      → core/heat_model.h
 *
 * 并发模型：
 *   打印头（VH / STB / SPI / 电机）是独占资源，用互斥量保护。
 *   一次作业（一页图、一张自检页）从 job_begin 到 job_end 独占。
 *   跨任务的调用方统一走 job_begin / job_end，不要直接操作 VH 和 STB。
 *
 * 被谁用：app/job.cpp、app/command.cpp、app/selftest.cpp、main.cpp
 * ========================================================================== */
#ifndef PRINTER_H
#define PRINTER_H

#include <Arduino.h>

/*
 * 打印头 + 步进电机
 * 一行 = 48 字节 = 384 点 = 0.125mm
 */

/* 上电初始化：电机相位、VH 关断、STB、SPI、互斥量、热模型归零 */
void printer_begin(void);

/* 占用打印头并打开升压。timeout_ms 内拿不到锁返回 false（说明有别的作业在用）；
 * 过热 / NTC 失效同样返回 false，并置位热锁存 */
bool printer_job_begin(uint32_t timeout_ms);

/* 打一行，按当前驱动方式推进 4 拍或 2 拍。必须在 job_begin 与 job_end 之间调用 */
void printer_job_row(const uint8_t *row);

/* 老加热路径（不跳过空组、不算点数、固定 4 拍），只用于诊断对照页。
 * cool_us 是组间间隔，可用来验证"实心块打不黑是不是供电恢复时间不够" */
void printer_job_row_raw(const uint8_t *row, uint16_t us, uint16_t cool_us);

/* 结束作业：关升压（走延迟断电窗口），可选退纸，释放锁 */
void printer_job_end(bool feed_out);

/* 单独走纸（内部自己取锁）。1 拍 = 0.03125mm */
void printer_feed(uint32_t beats);

/* 过热暂停续打：保存 / 恢复热积累前馈状态，避免冷却后续打出现浓淡跳变。
 * save 在关 VH 之前调，restore 在重新 job_begin 之后调 */
void printer_thermal_pause_save(void);
void printer_thermal_pause_restore(void);

#endif
