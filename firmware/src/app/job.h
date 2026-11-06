#ifndef JOB_H
#define JOB_H

#include <Arduino.h>

/* ============================================================================
 * 文件：app/job.h
 * 层次：应用层 / 打印任务入口
 * 职责：只声明 FreeRTOS 任务体 job_print_task()。
 *        任务内部的三个状态分支已拆到对应文件，本模块不含业务逻辑：
 *          app/task_print.cpp      循环骨架 + 按 jobState 分发
 *          app/job_idle.cpp        st=0 空闲：自检 / 走纸 / 长按出纸
 *          app/job_overheat.cpp    st=3 过热暂停：等冷却、断点续打、放弃条件
 *          app/job_run.cpp         st=1/2 运行：取行打印、收尾、断流超时
 *          app/job_internal.h      三个分支之间的约定（仅供 app/ 内部）
 *
 * 为什么要单独一个任务：
 *   一行 384 点的加热是微秒级时序，中途不能切任务；但连续几百行全是
 *   delayMicroseconds 忙等会把同核任务饿死。所以打印、按键自检、走纸、
 *   长按出纸全部收敛到这一个任务里串行执行，靠 runtime.h 的事件标志触发。
 *   其它模块（BLE 回调、按键任务）只置标志，不直接调 printer_*。
 *
 * 状态机（jobState 定义在 app/runtime.cpp）：
 *   0 空闲 / 1 收数据中 / 2 收尾 / 3 过热暂停
 *
 * 任务参数（栈 4096 / 优先级 5 / core1）在 main.cpp 的 setup() 里创建。
 * ========================================================================== */

/* FreeRTOS 任务体，由 main.cpp 用 xTaskCreatePinnedToCore 挂到 core1 */
void job_print_task(void *arg);

#endif
