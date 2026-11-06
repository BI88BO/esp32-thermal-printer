/* ============================================================================
 * 文件：hal/thermistor.h
 * 层次：硬件抽象层 (HAL) / 打印头热敏电阻（NTC）
 * 职责：ADC → 分压还原阻值 → Beta 公式 → 摄氏度；含滤波与故障判定。
 *
 * 这是**唯一**给过热保护提供温度的来源，所以两件事必须守住：
 *   ① 采样不能被打印任务挡住（由 monitorTask 无条件周期性调用）
 *   ② NTC 失效不能悄悄变成"温度正常"：连续 1 秒读数越界才判故障，
 *      期间沿用上一次有效值；判故障后 celsius() 返回 -1，
 *      上层的 >=50℃ 判断会因此走失效保护分支，而不是恒假绕过
 *
 * 依赖：config.h（PIN_TEMP_ADC / NTC_* 参数）、hal/adc_filter.h
 * 被谁用：hal/sensor.cpp（门面）、hal/thermal_guard.cpp（间接，经 sensor）
 * ========================================================================== */
#ifndef THERMISTOR_H
#define THERMISTOR_H

#include <Arduino.h>

/* 初始化滤波器：用第一批读数打底，避免上电时从 0 慢慢爬 */
void thermistor_begin(void);

/* 采一次。周期由 monitorTask 决定（忙 20ms / 空闲 100ms） */
void thermistor_update(void);

/* 摄氏度，四舍五入到整数。NTC 失效返回 -1（上层据此走失效保护） */
int8_t thermistor_celsius(void);

/* 摄氏度 ×10，保留一位小数，给串口日志和点检页用 */
int16_t thermistor_celsius_x10(void);

/* 最近一次读数是否有效 */
bool thermistor_valid(void);

#endif
