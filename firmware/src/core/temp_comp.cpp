/* ============================================================================
 * 文件：core/temp_comp.cpp
 * 层次：纯逻辑层 / 温度补偿
 * 职责：表 4 的分段线性插值与脉宽钳位。推导见 temp_comp.h。
 * 依赖：config.h
 *
 * 刻意保留 float：原实现是浮点插值 + 0.5f 四舍五入，改成定点会引入
 * ±1µs 的差异。补偿每秒只算一次，浮点开销可以忽略，等价性更要紧。
 * ========================================================================== */

#include "temp_comp.h"
#include <Arduino.h>
#include "config.h"

/* 25℃ 基准 = 1.000；5℃ = 1.231；45℃ = 0.846 */
static float factor(int8_t temp_c)
{
    if (temp_c <= 5)
        return 1.231f;
    if (temp_c <= 25)
        return 1.231f + (1.000f - 1.231f) * (temp_c - 5) / 20.0f;
    if (temp_c <= 45)
        return 1.000f + (0.846f - 1.000f) * (temp_c - 25) / 20.0f;
    /* 45℃ 以上维持 45℃ 的系数，不外推（见 temp_comp.h） */
    return 0.846f;
}

uint16_t temp_comp_permille(int8_t temp_c)
{
    return (uint16_t)(factor(temp_c) * 1000.0f + 0.5f);
}

uint16_t temp_comp_heat_us(uint16_t base_us, int8_t temp_c)
{
    if (temp_c < 0)
        return base_us;             // 传感器故障时按基准值，不做补偿

    uint32_t us = (uint32_t)(base_us * factor(temp_c) + 0.5f);
    if (us < HEAT_US_MIN) us = HEAT_US_MIN;
    if (us > HEAT_US_MAX) us = HEAT_US_MAX;
    return (uint16_t)us;
}
