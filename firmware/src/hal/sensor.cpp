/* ============================================================================
 * 文件：hal/sensor.cpp
 * 层次：硬件抽象层 (HAL) / 传感与补偿门面
 * 职责：决定"什么时候采什么"，然后把结果转发给上层。
 *       具体换算全部在子模块里，本文件不含任何公式。
 * 对应原工程：src/hal/em_adc.cpp
 *
 * 采集节奏由 app/tasks.cpp 的 monitorTask 决定：忙时 20ms、空闲 100ms。
 * 注意 sensor_update(printing) 这个参数只控制「要不要采电池」——
 * 打印时负载压降会让电池读数失真；NTC 温度是无条件采的，过温保护不能停。
 *
 * 依赖：hal/thermistor.h、hal/battery.h、hal/paper.h、core/temp_comp.h
 * 被谁用：app/tasks.cpp、app/command.cpp、app/job.cpp、hal/printer.cpp
 * ========================================================================== */

#include "sensor.h"
#include <Arduino.h>
#include "config.h"
#include "thermistor.h"
#include "battery.h"
#include "paper.h"
#include "temp_comp.h"

void sensor_begin(void)
{
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);     // 量程 0~3.3V

    /* 各路自己初始化滤波器（用第一批读数打底） */
    paper_begin();
    battery_begin();
    thermistor_begin();

    /* 先采两轮，让中值/EMA 有真实样本可用 */
    sensor_update(false);
    sensor_update(false);
}

void sensor_update(bool printing)
{
    paper_update();
    thermistor_update();

    /* 电池只在空闲时采样：打印时 0.8A 的负载会把电压拉低几百 mV，
     * 这时采到的值既不能反映真实电量，还会误触发欠压保护 */
    if (!printing)
        battery_update();
}

uint16_t sensor_battery_mv(void)
{
    return battery_mv();
}

uint8_t sensor_battery_pct(void)
{
    return battery_pct();
}

int8_t sensor_head_temp(void)
{
    return thermistor_celsius();
}

bool sensor_paper_ok(void)
{
    return paper_ok();
}

uint16_t sensor_heat_us_compensated(uint16_t base_us)
{
    /* 补偿公式与表 4 的推导见 core/temp_comp.cpp；
     * NTC 失效（-1）时 temp_comp 会原样返回基准值 */
    return temp_comp_heat_us(base_us, sensor_head_temp());
}

void sensor_print_debug(void)
{
#if DEBUG_SERIAL
    int8_t  t  = sensor_head_temp();
    int16_t t10 = thermistor_celsius_x10();
    Serial.printf("[SENSOR] bat %umV (%u%%)  temp %d.%d C  paper %s\n",
                  sensor_battery_mv(), sensor_battery_pct(),
                  t < 0 ? -1 : t, t < 0 ? 0 : (t10 % 10),
                  sensor_paper_ok() ? "OK" : "LACK");
#endif
}
