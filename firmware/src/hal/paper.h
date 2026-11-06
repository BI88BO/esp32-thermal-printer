/* ============================================================================
 * 文件：hal/paper.h
 * 层次：硬件抽象层 (HAL) / 缺纸检测
 * 职责：光电传感器的电平读取与去抖。
 * 对应原工程：src/hal/em_hal.cpp 的 read_paper_statue()（原工程用中断）
 *
 * 我们不用中断：GPIO35 只能输入，且中断里改状态需要额外同步；
 * 打印任务本来每 20~100ms 就会调一次，轮询 + 去抖足够快，也更好读。
 *
 * 手册 5.1：有反射（有纸）= 低电平。
 *
 * 依赖：config.h（PIN_PAPER / PAPER_DEBOUNCE）
 * 被谁用：hal/sensor.cpp（门面）、app/command.cpp（打印前检查）
 * ========================================================================== */
#ifndef PAPER_H
#define PAPER_H

#include <Arduino.h>

void paper_begin(void);

/* 采一次并去抖：电平连续 PAPER_DEBOUNCE 次与当前判断不同才翻转 */
void paper_update(void);

/* true = 有纸 */
bool paper_ok(void);

#endif
