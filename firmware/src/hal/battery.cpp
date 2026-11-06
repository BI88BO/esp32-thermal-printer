/* ============================================================================
 * 文件：hal/battery.cpp
 * 层次：硬件抽象层 (HAL) / 电池电压
 * 职责：采样、滤波、电压还原、放电曲线插值。
 * 依赖：config.h、hal/adc_filter.h
 * ========================================================================== */

#include "battery.h"
#include <Arduino.h>
#include "config.h"
#include "adc_filter.h"

static MedianFilter<5> f_bat_mv;
static EmaFilter       e_bat_mv;

static uint16_t bat_mv_filt = 0;

/* 单节锂电放电曲线（空载电压 → 剩余百分比）
 * 动力电芯放电平台比普通电芯略平，可按实测微调 */
static const struct { uint16_t mv; uint8_t pct; } CURVE[] = {
    {4200, 100}, {4050, 90}, {3950, 80}, {3850, 68},
    {3750, 55},  {3650, 38}, {3550, 25}, {3450, 15},
    {3350, 7},   {3200, 3},  {3000, 0}};

static const uint8_t CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

void battery_begin(void)
{
    uint32_t b = adc_trimmed_mean(PIN_BAT_ADC, 8);
    f_bat_mv.reset((uint16_t)b);
    e_bat_mv.reset((float)b);
}

void battery_update(void)
{
    uint32_t mv = adc_trimmed_mean(PIN_BAT_ADC, 16);
    float v = (float)mv * BAT_DIVIDER * BAT_CAL;
    float f = e_bat_mv.push(v, 0.3f);
    bat_mv_filt = (uint16_t)(f + 0.5f);
}

uint16_t battery_mv(void)
{
    return bat_mv_filt;
}

uint8_t battery_pct(void)
{
    uint16_t mv = bat_mv_filt;

    if (mv >= CURVE[0].mv)
        return 100;
    if (mv <= CURVE[CURVE_N - 1].mv)
        return 0;

    for (uint8_t i = 0; i < CURVE_N - 1; i++)
    {
        if (mv <= CURVE[i].mv && mv > CURVE[i + 1].mv)
        {
            /* 分段线性插值 */
            uint16_t v1 = CURVE[i].mv, v2 = CURVE[i + 1].mv;
            uint8_t  p1 = CURVE[i].pct, p2 = CURVE[i + 1].pct;
            return (uint8_t)(p2 + (uint32_t)(mv - v2) * (p1 - p2) / (v1 - v2));
        }
    }
    return 0;
}
