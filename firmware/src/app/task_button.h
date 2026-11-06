/* ============================================================================
 * 文件：app/task_button.h
 * 层次：应用层 / 按键任务
 * 职责：声明按键任务体（buttonTask）。
 *        它只做一件事：把按键事件翻译成 runtime 的事件标志，
 *        绝不直接碰打印头。
 *
 * 任务参数（栈 3072 / 优先级 3 / core1）在 main.cpp 的 setup() 里创建。
 * ========================================================================== */
#ifndef TASK_BUTTON_H
#define TASK_BUTTON_H

#include <Arduino.h>

void task_button(void *arg);

#endif
