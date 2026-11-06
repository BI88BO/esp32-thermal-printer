/* ============================================================================
 * 文件：app/job_internal.h
 * 层次：应用层 / 打印任务内部约定
 * 职责：printTask 的三个状态分支，只在 app/ 内部使用，不对外暴露。
 *
 * 为什么要拆：
 *   build 92 及以前 job.cpp 是 254 行的单个函数，靠 if(st==0/1/2/3) 硬分四段，
 *   改过热策略要在一屏里找，改自检触发又要在同一函数里找。现在按状态拆成
 *   三个文件，每个文件只讲一件事：
 *     job_idle.cpp      st=0 空闲：自检 / 走纸 / 长按连续出纸
 *     job_overheat.cpp  st=3 过热暂停：等冷却、恢复续打、放弃条件
 *     job_run.cpp       st=1/2 运行：取行打印、收尾判定、断流超时
 *
 * 约定（三个分支都必须遵守）：
 *   1. 每个函数处理「一轮」，自己负责循环内的等待（vTaskDelay），
 *      返回后 task_print.cpp 的 for 循环会重新读 jobState 再分发。
 *   2. 原来分支里的 continue 在这里一律写成 return，语义完全等价。
 *   3. 三个分支共享 app/runtime.h 的全局状态与 lineQ，不额外传参。
 *      它们只会被 printTask 单线程调用，不需要加锁。
 *
 * 被谁用：app/task_print.cpp
 * ========================================================================== */
#ifndef JOB_INTERNAL_H
#define JOB_INTERNAL_H

#include <Arduino.h>

/* st = 0：空闲。处理一次性事件（自检 / 走纸 / 长按出纸）。
 * 连续出纸时会立刻返回不睡，其余情况内部等 20ms */
void job_idle_tick(void);

/* st = 3：过热暂停。保底断电 → 等降温 → 恢复续打或放弃。
 * 仍处于暂停时内部等 100ms；恢复/放弃后立即返回 */
void job_overheat_tick(void);

/* st = 1/2：正常运行。取一行打一行；队列空时判收尾或断流超时。
 * 参数 st 只用于区分「收尾判定要不要生效」，不要在这里改它 */
void job_run_tick(uint8_t st);

#endif
