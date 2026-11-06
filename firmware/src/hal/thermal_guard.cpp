/* ============================================================================
 * 文件：hal/thermal_guard.cpp
 * 层次：硬件抽象层 (HAL) / 打印头热保护
 * 职责：热锁存的两级状态与判定逻辑。阈值与语义见 thermal_guard.h。
 * 依赖：config.h、hal/sensor.h
 * ========================================================================== */

#include "thermal_guard.h"
#include <Arduino.h>
#include "config.h"
#include "sensor.h"

/* 本作业触发过熔断：读一次即清，避免上层对同一次事件反复报错 */
static volatile bool latched = false;

/* 跨作业热锁存：降到 OVERHEAT_RECOVER_C 才解 */
static volatile bool trip = false;

void thermal_guard_init(void)
{
    latched = false;
    trip    = false;
}

bool thermal_guard_blocked(void)
{
    int8_t ht = sensor_head_temp();
    if (ht >= OVERHEAT_C || ht < 0)
    {
        /* 超温 / NTC 失效：本作业后续所有行都不再加热，只把纸走完。
         * 热锁存跨作业保持，必须实测温度降到恢复线以下才允许再开新作业。
         * 关键：不只是"不拉 STB"——VH 还开着时打印头一直带电，
         * 靠上层任务过一会儿再来关，中间这一段温度是降不下来的。
         * 断电动作由调用方做（见 printhead_heat_group） */
        latched = true;
        trip    = true;
        return true;
    }
    return false;
}

bool thermal_guard_can_start(void)
{
    int8_t ht0 = sensor_head_temp();

    /* NTC 读不到就按失效保护处理，绝不能靠 -1 >= 50 恒假蒙混过去 */
    if (ht0 < 0)
    {
        latched = true;
        trip    = true;
        return false;
    }

    if (trip)
    {
        /* 上次熔断后的滞回：必须实测冷却到 45℃ 以下才解锁，
         * 否则头温在 50℃ 附近抖一下就反复跳闸、半页白半页黑 */
        if (ht0 > OVERHEAT_RECOVER_C)
        {
            latched = true;
            return false;
        }
        trip = false;
    }

    if (ht0 >= OVERHEAT_C)
    {
        latched = true;
        trip    = true;
        return false;
    }

    latched = false;
    return true;
}

bool thermal_guard_take_latched(void)
{
    bool v = latched;
    latched = false;
    return v;
}

void thermal_guard_tick(void)
{
    if (!trip)
        return;

    int8_t ht = sensor_head_temp();
    if (ht >= 0 && ht <= OVERHEAT_RECOVER_C)
    {
        trip    = false;
        latched = false;
        LOGLN("[OVERHEAT] cooled, latch released");
    }
}

bool thermal_guard_active(void)
{
    return trip;
}
