/* ============================================================================
 * 文件：hal/button.h
 * 层次：硬件抽象层 (HAL) / 按键
 * 职责：把机械按键的电平变化翻译成三个语义事件（短按 / 长按开始 / 长按松手）。
 * 对应原工程：src/hal/em_button.h
 *
 * 本层只产生事件，不执行动作：短按出点检页、长按连续出纸都由
 * app/tasks.cpp 置标志、app/job.cpp 执行，避免按键任务里同步忙等。
 * ========================================================================== */
#ifndef BUTTON_H
#define BUTTON_H

#include <Arduino.h>

/*
 * 按键：短按打自检页，长按持续出纸（按住开始、松手停）。
 * 语义参考毕设原工程 em_button.cpp 的 motor_start()/motor_stop()，
 * 但改成事件化：按键任务只发事件，走纸由 printTask 分块执行。
 *
 * 去抖策略：10ms 采样一次，连续 3 次电平一致才认，
 * 也就是 30ms 的机械抖动窗口内不产生新事件。
 * 按下和松开各做一次，避免"按下抖出一串短按"。
 */
enum ButtonEvent {
    BTN_NONE = 0,
    BTN_SHORT,          // 短按松开：点检页
    BTN_LONG_START,     // 按住达到 KEY_LONG_MS：开始连续出纸
    BTN_LONG_RELEASE    // 长按后松手：停止出纸
};

void button_begin();

/* 每 10ms 调一次，返回本次产生的事件（没有则 BTN_NONE） */
ButtonEvent button_poll();

#endif
