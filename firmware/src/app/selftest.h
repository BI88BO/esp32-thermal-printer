/* ============================================================================
 * 文件：app/selftest.h
 * 层次：应用层 / 自检与诊断页（对外入口）
 * 职责：生成不依赖手机也能验机的各种测试页。
 *       实现在 app/selftest/ 目录下，一页一个文件：
 *         page_full.cpp      整页自检（竖线 / 横线 / 棋盘 / 全黑块）
 *         page_dotcheck.cpp  打印头点检（版本号 + 头温 + 能量阶梯 + 384 竖条）
 *         page_load.cpp      全黑负载（量带载压降）
 *         page_measure.cpp   VH 测量（15s 空载窗口 + 150 行满载）
 *         page_copy.cpp      波形复刻（时序结构对照）
 *         page_ruler.cpp     走纸标尺（10 条 2.5mm 间隔的线）
 *         draw.cpp           绘图基元（3×5 点阵数字、版本号抬头）
 *         common.h           能量标尺复位
 *
 * RTOS 约束：这些页都会同步跑几百行，只允许由 app/job.cpp 的打印任务调用；
 * 绝不能在 BLE 回调或按键任务里直接调，否则会卡住协议栈。
 * 行内 STB 时序不能切任务，让出点由 hal/printhead.cpp 的 row_yield()
 * 放在行边界（每 8 行 1 tick）。
 *
 * 页面编号由 app/runtime.h 的 pendingTest 选择，映射在 app/job.cpp：
 *   1 整页自检  2 打印头点检  3 全黑负载  4 测量+点检  5 VH 测量
 *   6 波形复刻    7 走纸标尺
 *
 * 所有页面返回 bool：false = 打印头被别的作业占着（或过热），没打成。
 * ========================================================================== */
#ifndef SELFTEST_H
#define SELFTEST_H

#include <Arduino.h>

/* 整页自检。版面分五段，每段只验一件事：
 *   竖线组  -> STB 六段拼接是否对齐、有没有整段缺失
 *   横线组  -> 走纸是否均匀（每 8 行一条，间距应完全相等）
 *   棋盘格  -> 分辨率与抖动（4x4 方格，边缘应锐利无粘连）
 *   全黑块  -> 浓度与供电（发灰＝升压不足）
 *   外边框  -> 打印宽度是否满 48mm */
bool selftest_print(uint32_t timeout_ms);

/* 打印头点检：顶部 2 位版本号 + 3 位头温，中间四档能量阶梯（右侧印实际
 * 脉宽），底部 384 根竖条分 4 相位错开。哪根浅、哪根缺一眼就能数出位置。
 * 拆机头 / 用久了 / 换纸之后都该打一张看看 */
bool selftest_dot_check(uint32_t timeout_ms);

/* 全黑负载：连续 150 行全黑（约 19mm、3~4 秒），把 VH 拉到最大负载，
 * 方便用万用表盯着读数。测 VH 压降、量输入电流、贴温度都靠它 */
bool selftest_load(uint32_t timeout_ms);

/* VH 测量：先 15 秒只开 VH 不打印（量空载电压），再 150 行全黑（量压降） */
bool selftest_measure(uint32_t timeout_ms);

/* 波形复刻：按固定时序结构打三个方块，
 * 验的是走纸/选通时序，不是能量大小 */
bool selftest_waveform(uint32_t timeout_ms);

/* 走纸标尺：一条实心行 + 20 行空白，重复 10 次，每段应推进 2.5mm。
 * 10 条线均匀排开 = 走纸正常；挤在一起 = 丢步，与加热无关 */
bool selftest_feed_ruler(uint32_t timeout_ms);

#endif
