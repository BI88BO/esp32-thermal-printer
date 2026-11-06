/* ============================================================================
 * 文件：hal/led.h
 * 层次：硬件抽象层 (HAL) / 状态指示灯
 * 职责：一颗单色 LED 的亮灭与闪烁，全部**非阻塞**。
 * 对应原工程：src/hal/em_hal.cpp 的 run_led()
 *
 * 与原工程的区别（重要）：
 *   原工程 run_led() 里是 digitalWrite + delay(100) + digitalWrite…
 *   也就是点亮期间把 CPU 焊死 300~500ms。那在裸机单循环里能跑，
 *   在 FreeRTOS 里会直接饿死打印/采样任务，所以这里改成状态机：
 *   只记录"从什么时候开始闪、闪几次"，由 led_tick() 按 millis() 推进。
 *
 * 硬件：原理图上 LED1/LED2 经 R11 1kΩ 接在网络 "LED"，接 GPIO18，
 *       低电平点亮（原工程 digitalWrite(PIN_LED, 0) 即点亮）。
 *       LED3 是 3V3 经 R23 的电源指示灯，不受软件控制。
 *
 * 依赖：config.h（PIN_LED / LED_ACTIVE_LOW / LED_ENABLE）
 * 被谁用：hal/ble.cpp（连接提示）、app/command.cpp（开始打印、告警）、
 *        app/tasks.cpp（周期 tick）
 * ========================================================================== */
#ifndef LED_H
#define LED_H

#include <Arduino.h>

/* 上电初始化：按有效电平熄灭 */
void led_init(void);

/* 常亮 / 常灭（会打断正在进行的闪烁） */
void led_set(bool on);

/* 闪 times 次：每次亮 on_ms、灭 off_ms，非阻塞、立即返回。
 * 闪烁期间再调用会覆盖上一次（后来者优先） */
void led_blink(uint8_t times, uint16_t on_ms, uint16_t off_ms);

/* 周期调用（20~100ms 一次）：按 millis 推进闪烁相位。
 * 即使调用间隔比 on_ms 大，也只影响边沿精度，不会漏闪 */
void led_tick(void);

/* 当前是否在闪（调试用） */
bool led_busy(void);

#endif
