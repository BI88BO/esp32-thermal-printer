/* ============================================================================
 * 文件：hal/battery.h
 * 层次：硬件抽象层 (HAL) / 电池电压
 * 职责：分压采样 → 滤波 → 电压还原；再把空载电压按放电曲线换成百分比。
 *
 * 为什么只在空闲时采：打印时 0.8A 负载会把电压拉低几百 mV，
 * 这时采到的值既不能反映真实电量，还会误触发欠压保护（ERR_LOW_BAT）。
 * 调用方（app/tasks.cpp）用 sensor_update(printing) 控制要不要采。
 *
 * 依赖：config.h（PIN_BAT_ADC / BAT_DIVIDER / BAT_CAL）、hal/adc_filter.h
 * 被谁用：hal/sensor.cpp（门面）、app/command.cpp（作业开始前查欠压）
 * ========================================================================== */
#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>

void battery_begin(void);

/* 采一次。只应在空闲时调用（见文件头说明） */
void battery_update(void);

/* 滤波后的电池电压，mV */
uint16_t battery_mv(void);

/* 按放电曲线换算的剩余百分比 0~100 */
uint8_t battery_pct(void);

#endif
