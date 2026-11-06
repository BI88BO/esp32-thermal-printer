/* ============================================================================
 * 文件：hal/printer.cpp
 * 层次：硬件抽象层 (HAL) / 打印头门面与行编排
 * 职责：只做两件事——
 *         ① 作业生命周期：取锁 → 查热保护 → 开 VH → 推进热模型 → 收尾释放
 *         ② 一行的拍子编排：把「走一步电机 + 加热哪几组」按节拍铺开
 * 对应原工程：src/hal/em_printer.cpp 的任务编排部分
 *
 * 实现已经拆开，本文件只做组合（想改细节去对应文件）：
 *   VH 开关与时序      → hal/vh.cpp
 *   热保护与锁存       → hal/thermal_guard.cpp
 *   STB / SPI / 脉宽   → hal/printhead.cpp
 *   走纸节拍与相序     → hal/motor.cpp
 *   能量模型           → core/heat_model.cpp
 *
 * 一行怎么排拍（决定节拍是否均匀）：
 *   1-2 相：拍1→组1,2  拍2→组3,4  拍3→组5,6  拍4 空拍收尾
 *   双相  ：拍1→组1,2,3  拍2→组4,5,6
 *   每个拍槽的起点之间至少隔 min_beat_us，和这一行有多少黑点无关。
 *
 * 依赖：全部 hal/*、core/heat_model.h、config.h
 * 被谁用：app/job.cpp（唯一调用 job_row/job_end 的地方）、app/selftest.cpp
 * ========================================================================== */

#include "printer.h"
#include <Arduino.h>
#include "config.h"
#include "printhead.h"
#include "vh.h"
#include "thermal_guard.h"
#include "motor.h"
#include "heat_model.h"
#include "sensor.h"

/* 打印头互斥量：一次作业从 begin 到 end 全程持有 */
static SemaphoreHandle_t hw_mtx = nullptr;

/* ==================== 初始化 ==================== */

void printer_begin(void)
{
    /* 顺序不能反：VH 比打印头数据先建立会误加热 */
    motor_init();
    printhead_begin();
    vh_init();
    thermal_guard_init();
    heat_model_init();

    hw_mtx = xSemaphoreCreateMutex();
    configASSERT(hw_mtx);
}

/* ==================== 作业生命周期 ==================== */

bool printer_job_begin(uint32_t timeout_ms)
{
    if (hw_mtx == nullptr)
        return false;

    /* 所有作业（App 打印 / 按键自检 / 诊断命令）统一从这里进。
     * 过热、NTC 失效、跨作业热锁存都在 thermal_guard_can_start() 里判断，
     * 让自检页那条同步路径也能被挡住。阈值依据见 thermal_guard.h */
    if (!thermal_guard_can_start())
        return false;

    if (xSemaphoreTake(hw_mtx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;

    /* 作业开始前把相位对齐到"两相通电"那四个状态之一。
     * 走纸（motor_feed）固定用 8 状态半步，会把相位留在奇数位，
     * 双相模式下踩到单相态力矩更差 */
    if (printhead_get_drive_mode())
        motor_align_dual();

    /* 冷头起步资格按开局实测头温决定，具体插值见 core/heat_model.cpp */
    heat_model_job_begin(sensor_head_temp());

    vh_set(true);          /* 内部可能 delay(VH_SETTLE_MS) 等升压建立 */

    /* 从「上一次最后一行」到「现在」全部按停机冷却计入：中间经过退纸、
     * 空闲、VH 预热，这些时段 STB 都没拉，头确实在散热。
     * τ≈2s，1 秒预热只散掉约 4 成，连打两张时热状态仍会累积——
     * 这正是第二张不再重复给满起步补偿、不会越打越黑的原因。 */
    heat_model_cool_to_now();
    return true;
}

static inline void slot_pad(uint32_t t0)
{
    uint32_t lim = printhead_min_beat_us();
    uint32_t dt  = micros() - t0;
    if (dt < lim)
        delayMicroseconds(lim - dt);
}

void printer_job_row(const uint8_t *row)
{
    /* 先按这一行真正注入的能量推进热状态，后面的脉宽都据此微调 */
    heat_model_row_begin(row, printhead_get_heat_us(), printhead_get_repeat());

    printhead_send_row(row);

    if (printhead_get_drive_mode())
    {
        uint32_t t0;

        delayMicroseconds(RECOVER_US);
        t0 = micros();
        motor_step(true);
        printhead_heat_group(row, 0); printhead_heat_group(row, 1); printhead_heat_group(row, 2);
        slot_pad(t0);

        delayMicroseconds(RECOVER_US);
        t0 = micros();
        motor_step(true);
        printhead_heat_group(row, 3); printhead_heat_group(row, 4); printhead_heat_group(row, 5);
        slot_pad(t0);
    }
    else
    {
        uint32_t t0;

        delayMicroseconds(RECOVER_US);
        t0 = micros();
        motor_step(false);
        printhead_heat_group(row, 0); printhead_heat_group(row, 1);
        slot_pad(t0);

        delayMicroseconds(RECOVER_US);
        t0 = micros();
        motor_step(false);
        printhead_heat_group(row, 2); printhead_heat_group(row, 3);
        slot_pad(t0);

        delayMicroseconds(RECOVER_US);
        t0 = micros();
        motor_step(false);
        printhead_heat_group(row, 4); printhead_heat_group(row, 5);
        slot_pad(t0);

        delayMicroseconds(RECOVER_US);
        t0 = micros();
        motor_step(false);
        slot_pad(t0);
    }

    printhead_row_yield();
}

/* 老版本的加热路径：6 组各打一次，不算点数、不跳过、不管驱动方式。
 * cool_us 可指定——组间间隔越长，升压和储能电容越有时间恢复，
 * 用来验证"实心块打不黑是不是供电跟不上"。 */
void printer_job_row_raw(const uint8_t *row, uint16_t us, uint16_t cool_us)
{
    /* 对照页也走与正常打印相同的能量上限：复刻的是时序结构
     * （6 组全加热 / 4 拍 / 行尾延时），不是原工程那个 3000µs 的能量。
     * 800µs 的标称安全窗口在这块拆机头上冷态不出色，见
     * printhead_set_heat_override() 的说明。 */
    if (us > HEAT_US_MAX) us = HEAT_US_MAX;

    /* 对照页同样要推进热状态，否则连打几张诊断页后热状态一直停留在 0，
     * 下一张正式打印会误判成冷头。空白行虽然也拉 STB，但锁存数据为 0，
     * 驱动管不导通、不发热，按黑点数统计是对的。
     * 这里必须传本函数真正注入的 us，而不是 printhead_get_heat_us()：
     * 对照页的脉宽由调用者给定（通常与当前浓度不同），用错的基准会让
     * 热状态偏大或偏小，进而污染紧随其后的正式打印。 */
    heat_model_row_begin(row, us, printhead_get_repeat());

    printhead_send_row(row);

    for (uint8_t g = 0; g < 6; g++)
    {
        /* raw 路径不经过 heat_group，熔断检查由 printhead 内部完成。
         * 中断而不是 return：后面的 4 拍走纸必须照常完成，否则纸位错乱 */
        if (!printhead_heat_group_raw(g, us, cool_us))
            break;
    }

    for (uint8_t b = 0; b < 4; b++)
    {
        delayMicroseconds(RECOVER_US);
        motor_step(false);
        delayMicroseconds(3000);
    }
    printhead_row_yield();
}

/* ==================== 收尾 ==================== */

void printer_thermal_pause_save(void)
{
    heat_model_pause_save();
}

void printer_thermal_pause_restore(void)
{
    heat_model_pause_restore();
}

void printer_job_end(bool feed_out)
{
    /* 一张打完留个热状态快照，串起来看就能确认「连打几张 s 单调上升、
     * 脉宽单调下降」，不用拆机也不用示波器。 */
    LOGF("[THERM] s %u/%u  trim %u%%\n",
         heat_model_state(), THERM_STATE_FULL, heat_model_trim_pct());
    vh_set(false);

    if (feed_out)
        motor_feed(FEED_AFTER_BEATS);      // 退纸让内容露出来

    if (hw_mtx != nullptr)
        xSemaphoreGive(hw_mtx);
}

void printer_feed(uint32_t beats)
{
    if (hw_mtx == nullptr)
        return;

    /* 拿不到锁说明正在打图，这时候别插一脚 */
    if (xSemaphoreTake(hw_mtx, pdMS_TO_TICKS(500)) != pdTRUE)
        return;

    motor_feed(beats);
    xSemaphoreGive(hw_mtx);
}
