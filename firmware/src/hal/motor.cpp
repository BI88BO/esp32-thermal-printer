/* ============================================================================
 * 文件：hal/motor.cpp
 * 层次：硬件抽象层 (HAL) / 步进电机
 * 职责：把「走一拍 / 走一段」封装成与打印逻辑无关的动作。
 *
 * 为什么单独一个文件：相序表是纯硬件知识，和「这一行该加热多久」毫无关系。
 *   拆开后想改走纸手感（加减速曲线、换驱动方式）只需要动这里；
 *   想改打印质量只需要动 core/heat_model.cpp，两边不会互相牵连。
 *
 * RTOS 注意：feed 里原本是 240 拍 × 3ms = 720ms 纯忙等，会独占 core1，
 *   温度采样和按键全停摆。现在每拍先 vTaskDelay(1)，再用 micros()
 *   把剩余时间补齐——节拍下限不缩短（电机不丢步），只是可能略长。
 * ========================================================================== */

#include "motor.h"
#include <Arduino.h>
#include "config.h"

/* 1-2 相 8 拍序列，列为 {AP, AM, BP, BM}
 * 2-1-2-1 交替：两相 / 单相 / 两相 / 单相 ...
 * 一拍 = 4.5° = 0.03125mm，一行 4 拍 = 0.125mm
 * SEQ 里偶数下标（0/2/4/6）正好都是「两相通电」，所以双相只要跳着走 */
static const uint8_t SEQ[8][4] = {
    {0, 1, 1, 0},   /* 拍1  A- B+ */
    {0, 0, 1, 0},   /* 拍2     B+ */
    {1, 0, 1, 0},   /* 拍3  A+ B+ */
    {1, 0, 0, 0},   /* 拍4  A+    */
    {1, 0, 0, 1},   /* 拍5  A+ B- */
    {0, 0, 0, 1},   /* 拍6     B- */
    {0, 1, 0, 1},   /* 拍7  A- B- */
    {0, 1, 0, 0}};  /* 拍8  A-    */

/* 当前停在 8 个状态里的哪一个。只被本模块与 printer.cpp 的作业内代码访问，
 * 且都在持有 hw_mtx 时串行执行，不需要加锁或 volatile。 */
static uint8_t seq_pos = 0;

void motor_init(void)
{
    pinMode(PIN_AP, OUTPUT);
    pinMode(PIN_AM, OUTPUT);
    pinMode(PIN_BP, OUTPUT);
    pinMode(PIN_BM, OUTPUT);
    motor_off();
}

void motor_off(void)
{
    digitalWrite(PIN_AP, LOW);
    digitalWrite(PIN_AM, LOW);
    digitalWrite(PIN_BP, LOW);
    digitalWrite(PIN_BM, LOW);
}

/* 输出当前相位，再前进 stride 个状态。stride=2 时只在偶数相位间跳 */
void motor_step(bool dual)
{
    digitalWrite(PIN_AP, SEQ[seq_pos][0]);
    digitalWrite(PIN_AM, SEQ[seq_pos][1]);
    digitalWrite(PIN_BP, SEQ[seq_pos][2]);
    digitalWrite(PIN_BM, SEQ[seq_pos][3]);
    seq_pos = (seq_pos + (dual ? 2 : 1)) & 7;
}

void motor_align_dual(void)
{
    seq_pos &= 6;
}

void motor_feed(uint32_t beats)
{
    /* 走纸固定用 1-2 相半步：0.03125mm 的分辨率更细，
     * 而且和打印时的驱动方式无关，退纸距离才稳定 */
    uint32_t d = STEP_START_US;
    for (uint32_t i = 0; i < beats; i++)
    {
        uint32_t t0 = micros();
        motor_step(false);

        /* 先让出 1 个 tick，再用 micros() 把剩下的时间补齐：
         * 节拍下限不缩短（电机不会丢步），只是可能略长，走纸更稳 */
        if (d > 1200)
            vTaskDelay(1);

        uint32_t dt = micros() - t0;
        if (dt < d)
            delayMicroseconds(d - dt);
        if (d > STEP_DELAY_US)
            d -= 100;
    }
    motor_off();
}
