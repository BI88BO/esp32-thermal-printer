/* ============================================================================
 * 文件：hal/adc_filter.h
 * 层次：硬件抽象层 (HAL) / ADC 通用滤波（纯头文件，模板不能分离编译）
 * 职责：三个可复用的滤波/采样原语，被 hal/thermistor 和 hal/battery 共用。
 * 对应原工程：src/hal/em_adc.cpp 里的滤波部分（原工程只有一档均值）
 *
 * 为什么抽出来：NTC 和电池是同一种问题——ADC 读数被 BLE 射频和
 * VH 大电流干扰，单次采样不可信。两级串联：
 *   adc_trimmed_mean  去掉单次极值毛刺（软平均）
 *   MedianFilter<5>   抑制偶发野值（比均值抗脉冲，且不拖慢响应）
 *   EmaFilter         时间维平滑（a 越小越平滑、响应越慢）
 *
 * 依赖：Arduino.h
 * 被谁用：hal/thermistor.cpp、hal/battery.cpp
 * ========================================================================== */
#ifndef ADC_FILTER_H
#define ADC_FILTER_H

#include <Arduino.h>
#include <string.h>

/* 截尾均值：采 n 次，去掉一个最大值和一个最小值再平均 */
static inline uint32_t adc_trimmed_mean(uint8_t pin, uint8_t n)
{
    uint32_t sum = 0;
    uint32_t vmin = 0xFFFFFFFF, vmax = 0;

    for (uint8_t i = 0; i < n; i++)
    {
        uint32_t v = (uint32_t)analogReadMilliVolts(pin);
        sum += v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    if (n <= 2)
        return sum / n;
    return (sum - vmin - vmax) / (n - 2);
}

/* 中值滤波：窗口 N 个样本，取排序后的中间值 */
template <uint8_t N>
class MedianFilter
{
public:
    void reset(uint16_t v)
    {
        for (uint8_t i = 0; i < N; i++) buf[i] = v;
        idx = 0;
    }

    uint16_t push(uint16_t v)
    {
        buf[idx] = v;
        idx = (uint8_t)((idx + 1) % N);

        uint16_t tmp[N];
        memcpy(tmp, buf, sizeof(tmp));

        /* 插入排序：N 很小（5），比调用 qsort 划算 */
        for (uint8_t i = 1; i < N; i++)
        {
            uint16_t key = tmp[i];
            int8_t j = (int8_t)(i - 1);
            while (j >= 0 && tmp[j] > key)
            {
                tmp[j + 1] = tmp[j];
                j--;
            }
            tmp[j + 1] = key;
        }
        return tmp[N / 2];
    }

private:
    uint16_t buf[N] = {0};
    uint8_t  idx = 0;
};

/* 一阶低通（指数滑动平均）：y = y + a*(x-y) */
class EmaFilter
{
public:
    void reset(float v)
    {
        y = v;
        ready = true;
    }

    float push(float v, float a)
    {
        if (!ready)
        {
            y = v;
            ready = true;
        }
        else
        {
            y += a * (v - y);
        }
        return y;
    }

private:
    float y = 0.0f;
    bool  ready = false;
};

#endif
