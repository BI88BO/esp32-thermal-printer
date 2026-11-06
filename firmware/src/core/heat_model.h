#ifndef HEAT_MODEL_H
#define HEAT_MODEL_H

#include <Arduino.h>

/* ============================================================================
 * 文件：core/heat_model.h
 * 层次：纯逻辑层 / 能量策略（不碰任何 GPIO，可单独阅读与仿真）
 * 职责：决定「这一组点这一遍该加热多少 µs」。
 *             直接写在 hal/em_printer.cpp 里；我们把它连同温度策略抽成一层。
 *
 * 三种修正叠加的顺序（都在 heat_model_group_us() 里，别在别处重复补偿）：
 *   ① 冷头起步  ×1.25 → ×1.00：按作业开局头温给资格，随热状态 s 平滑退出
 *   ② 热积累削减 ×1.00 → ×0.85：s 越高削得越多，压住「越打越黑」
 *   ③ 点密度补偿 n²：黑点越多 VH 掉压越狠，按 n² 补时间
 *   ④ 钳位 HEAT_US_ABS_MAX：整条链路唯一的出口，三条分支共用。
 *      不能在任何一步提前 return，否则会出现「加了补偿反而更淡」的反向效果
 *      （build 88 及以前的 bug，详见 config.h 的 HEAT_US_ABS_MAX 说明）。
 *
 * 这是**开环前馈**：不看打印结果、不测误差、不反馈校正，不是闭环控制。
 * 参数依据规格书表 4 的 25→45℃ 斜率 -0.74%/℃ × 20℃ ≈ 15%（不外推到 50℃），
 * 具体推导见 config.h 的「冷态起步 + 热积累前馈」段。
 *
 * 依赖：config.h
 * 被谁用：hal/printer.cpp（group_heat_us / 行开头 / 作业开头 / 收尾日志）
 * ========================================================================== */

/* 统计第 g 组（8 字节 / 64 点）里的黑点数。
 * 用 16 项查表而不是 __builtin_popcount：后者在部分 xtensa 工具链上会退化成
 * 一个不够稳的库调用，而这段每行要跑 6 次，出错就是「整行不加热」。 */
uint8_t heat_count_black(const uint8_t *row, uint8_t g);

/* 上电初始化热状态。由 printer_begin() 调用一次 */
void heat_model_init(void);

/* 作业开始：按当前实测头温决定这一张纸有没有「冷头起步」资格。
 * 用连续插值而不是 ht<30 的硬阈值，否则头温刚跨过阈值会突然淡一档。 */
void heat_model_job_begin(int8_t head_temp);

/* 把「上次最后一行 → 现在」的停机时间按一阶散热模型计入。
 * 退纸、空闲、VH 预热都算散热；余热跨作业保留，连打第二张不会重给冷头补偿。 */
void heat_model_cool_to_now(void);

/* 每行开头调一次：按本行真正注入的能量推进热状态 s。
 * us 传「本行实际生效的基准脉宽」（诊断页是钉住的裸脉宽），repeat 是加热遍数。 */
void heat_model_row_begin(const uint8_t *row, uint16_t us, uint8_t repeat);

/* 算一组这一遍的加热时间(µs)。base_us 是补偿后的基准浓度，
 * black_dots 是本组黑点数，dotcomp_pct 是 App 设的点密度补偿百分比。 */
uint16_t heat_model_group_us(uint16_t base_us, uint8_t black_dots, uint8_t dotcomp_pct);

/* 过热暂停续打：保存 / 恢复热状态现场，避免冷却后续打出现浓淡分界。
 * save 在关 VH 之前调，restore 在重新 job_begin 之后调（覆盖它的停机冷却）。 */
void heat_model_pause_save(void);
void heat_model_pause_restore(void);

/* 只用于收尾时的串口日志，方便确认「连打几张 s 单调上升、脉宽单调下降」 */
uint16_t heat_model_state(void);
uint16_t heat_model_trim_pct(void);

#endif
