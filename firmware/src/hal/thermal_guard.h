/* ============================================================================
 * 文件：hal/thermal_guard.h
 * 层次：硬件抽象层 (HAL) / 打印头热保护
 * 职责：只做一件事——判断"现在还能不能加热"，并维护跨作业的热锁存。
 *        不含任何加热、走纸、断电动作：调用方拿到 true 之后自己决定怎么办。
 *
 * 对应原工程：原工程只有一个 >65℃ 的中止判断，没有锁存、没有滞回。
 *
 * 两级状态，别混：
 *   overheat_latched  本作业触发过熔断（读一次即清）→ 上层据此报错/暂停
 *   trip              跨作业热锁存（必须实测降到 45℃ 以下才解）→ 防止抖动着反复跳闸
 *
 * 阈值依据规格书：
 *   表 1 工作温度上限 50℃  → OVERHEAT_C
 *   恢复到 45℃ 才允许再打 → OVERHEAT_RECOVER_C（留 5℃ 滞回）
 *   表 3 的 65℃ 是永久损坏线，不是工作线，不能拿来当阈值
 *   NTC 读不到（-1）按失效保护处理：否则 "-1 >= 50" 恒假，保护形同虚设
 *
 * 依赖：config.h、hal/sensor.h（sensor_head_temp）
 * 被谁用：hal/printhead.cpp（每次拉 STB 之前）、hal/printer.cpp（作业开始前）、
 *        app/tasks.cpp（周期调用 tick 自动解锁）
 * ========================================================================== */
#ifndef THERMAL_GUARD_H
#define THERMAL_GUARD_H

#include <Arduino.h>

/* 上电初始化（清锁存） */
void thermal_guard_init(void);

/* 每拉一次 STB 之前问一次：返回 true 表示已熔断（超温或 NTC 失效），
 * 内部会置位两级锁存。调用方应放弃本次加热并立即 vh_force_off() */
bool thermal_guard_blocked(void);

/* 作业开始前的完整检查（含滞回）：返回 false 表示不许开始。
 * 内部会读一次实测头温，并处理好三种情况：
 *   NTC 失效 → 置锁存，拒绝
 *   锁存中且温度仍 >45℃ → 拒绝（不改锁存）
 *   温度 ≥50℃ → 置锁存，拒绝
 *   否则清 latched，放行 */
bool thermal_guard_can_start(void);

/* 读一次即清：本作业是否触发过熔断。覆盖"作业前被拒"和"打印中行级熔断"两种 */
bool thermal_guard_take_latched(void);

/* 周期调用（100ms）：头温降到恢复线以下就自动解除跨作业锁存。
 * 以前只有 job_begin 会解锁，断连后没有新作业时锁存一直挂着，
 * 重连后即使已经凉了，第一次点打印也会先"假暂停"一次 */
void thermal_guard_tick(void);

/* 只查询，不清 */
bool thermal_guard_active(void);

#endif
