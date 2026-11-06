/* ============================================================================
 * 文件：hal/thermistor.cpp
 * 层次：硬件抽象层 (HAL) / 打印头热敏电阻（NTC）
 * 职责：采样、滤波、分压还原、Beta 公式换算、开路短路故障判定。
 * 依赖：config.h、hal/adc_filter.h、math.h
 * ========================================================================== */

#include "thermistor.h"
#include <Arduino.h>
#include "config.h"
#include "adc_filter.h"
#include <math.h>

static MedianFilter<5> f_tmp_mv;
static EmaFilter       e_tmp_mv;

static int16_t temp_c10   = 250;     // 温度 ×10，保留一位小数
static bool    temp_valid = false;

/* 连续失效的起点。0 = 当前读数正常 */
static uint32_t bad_since = 0;

void thermistor_begin(void)
{
    uint32_t t = adc_trimmed_mean(PIN_TEMP_ADC, 8);
    f_tmp_mv.reset((uint16_t)t);
    e_tmp_mv.reset((float)t);
}

void thermistor_update(void)
{
    uint32_t mv = adc_trimmed_mean(PIN_TEMP_ADC, 16);

    /* 开短路检测：分压点贴到 0 或 3.3V 说明传感器没接好。
     * 打印时 VH 大电流会干扰 ADC，偶发一两次越界很常见。以前一次越界
     * 就把 temp_valid 清成 false，celsius() 返回 -1，过热保护的
     * 「>= 50」判断恒为假——等于保护被毛刺悄悄关掉了。
     * 现在按「连续 1 秒」计时（采样周期 20ms/100ms 都适用），期间沿用
     * 上一次有效温度；连续失效才判传感器故障，由打印层按失效保护处理。 */
    if (mv < 30 || mv > 3250)
    {
        if (bad_since == 0)
            bad_since = millis();
        if (millis() - bad_since >= 1000)
            temp_valid = false;
        return;
    }
    bad_since = 0;

    /* 先滤电压再换算温度：温度与电阻是非线性的，
     * 在 mV 域做滤波才不会因为温度换算放大误差 */
    uint16_t fmv = f_tmp_mv.push((uint16_t)mv);
    float    smv = e_tmp_mv.push((float)fmv, 0.3f);

    /* 分压还原 NTC 阻值 */
    float r;
    if (NTC_TO_GND)
        r = NTC_PULL * smv / (3300.0f - smv);
    else
        r = NTC_PULL * (3300.0f - smv) / smv;

    /* Beta 公式：1/T = 1/T0 + ln(R/R25)/B */
    float t = 1.0f / (1.0f / (273.15f + 25.0f) + logf(r / NTC_R25) / NTC_B) - 273.15f;

    if (t < -20.0f) t = -20.0f;
    if (t > 120.0f) t = 120.0f;

    temp_c10   = (int16_t)(t * 10.0f + 0.5f);
    temp_valid = true;
}

int8_t thermistor_celsius(void)
{
    if (!temp_valid)
        return -1;
    return (int8_t)((temp_c10 + 5) / 10);
}

int16_t thermistor_celsius_x10(void)
{
    return temp_c10;
}

bool thermistor_valid(void)
{
    return temp_valid;
}
