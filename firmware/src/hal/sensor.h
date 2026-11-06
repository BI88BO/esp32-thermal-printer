/* ============================================================================
 * 文件：hal/sensor.h
 * 层次：硬件抽象层 (HAL) / 传感与补偿门面
 * 职责：给上层一个统一的传感器入口。实际实现已按功能拆成四个模块：
 *         hal/thermistor.h   NTC 头温（换算 + 滤波 + 故障判定）
 *         hal/battery.h      电池电压与百分比
 *         hal/paper.h        缺纸检测与去抖
 *         core/temp_comp.h   表 4 温度→脉宽补偿（纯计算）
 *       本文件只负责"按什么节奏采"和把结果转出去。
 *
 * 温度这条线最关键：sensor_heat_us_compensated() 把 App 设的基准浓度按
 * 实测头温折算；50℃/45℃ 的熔断与恢复阈值写在 config.h，判定在
 * hal/thermal_guard.cpp（统一判定）与 app/job.cpp（任务级暂停）两层。
 *
 * ESP32 的 ADC 在 BLE 射频工作时噪声很大（几十 mV 毛刺常见），所以每个
 * 通道都是「截尾均值 → 中值滤波 → 一阶低通」三级串联，见 hal/adc_filter.h。
 * ========================================================================== */
#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

/* 初始化 ADC 与三路传感器。会先采两轮把滤波器打底，避免上电从 0 爬 */
void sensor_begin(void);

/* 周期调用（忙 20ms / 空闲 100ms）。printing=true 时跳过电池采样 */
void sensor_update(bool printing);

/* 电池电压（mV），已按分压比还原 */
uint16_t sensor_battery_mv(void);

/* 电量百分比 0~100，按单节锂电放电曲线分段线性插值 */
uint8_t sensor_battery_pct(void);

/* 打印头温度（℃）。返回 -1 表示传感器开路/短路 */
int8_t sensor_head_temp(void);

/* 缺纸检测，已做去抖：连续 3 次一致才改变状态 */
bool sensor_paper_ok(void);

/* 按温度补偿加热时间。基准值来自用户设置，返回实际下发给打印头的值。
 * 依据规格书表 4：5℃/25℃/45℃ 的推荐 Ton = 0.67 / 0.54 / 0.46 ms */
uint16_t sensor_heat_us_compensated(uint16_t base_us);

/* 调试用：把当前读数打到串口。仅 DEBUG_SERIAL=1 时编译进去 */
void sensor_print_debug(void);

#endif
