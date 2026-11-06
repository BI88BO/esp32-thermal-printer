/* ============================================================================
 * 文件：hal/printhead.h
 * 层次：硬件抽象层 (HAL) / 打印头本体
 * 职责：把一行 48 字节变成纸上的一行点。具体是四件事：
 *         ① SPI 把 48B 锁进打印头移位寄存器
 *         ② STB 分时选通，一次只让 1~3 组（64~192 点）同时发热
 *         ③ 按算好的脉宽拉 STB（脉宽怎么来的不归这里管，见 core/heat_model）
 *         ④ 保存 App 下发的质量参数（浓度 / 预加热 / 遍数 / 点补偿 / 驱动方式）
 *
 *
 * 不归本文件管（各有一户）：
 *   VH 升压开关        → hal/vh.cpp
 *   热保护与锁存       → hal/thermal_guard.cpp
 *   该加热多少 µs      → core/heat_model.cpp
 *   相序表 / 走纸节拍  → hal/motor.cpp
 *   一行的拍子怎么编排 → hal/printer.cpp
 *
 * 一行 48B = 384 点 = 6 组 × 64 点，一组 8 字节。
 *   1-2 相：拍1→组1,2  拍2→组3,4  拍3→组5,6  拍4 空拍收尾（共 4 拍）
 *   双相  ：拍1→组1,2,3  拍2→组4,5,6                （共 2 拍）
 *
 * 并发：所有 *_heat_group / send_row 只能由持锁的打印任务调用（见 hal/printer.h）。
 * ========================================================================== */
#ifndef PRINTHEAD_H
#define PRINTHEAD_H

#include <Arduino.h>

/* 引脚 + SPI 初始化（LAT / 6 路 STB / HSPI）。由 printer_begin() 调用一次 */
void printhead_begin(void);

/* 6 组 STB 全部拉回空闲电平。开/关 VH 前后都要做，
 * 否则锁存器里残留的数据可能在升压建立的瞬间误加热 */
void printhead_stb_idle(void);

/* 把一行 48B 通过 SPI 送进打印头，然后打一个 LAT 锁存脉冲 */
void printhead_send_row(const uint8_t *row);

/* 加热第 g 组（8 字节 / 64 点）。
 * 内部按 repeat 遍重复加热、按需插预加热、并按点密度补偿算脉宽。
 * 全白组直接跳过（SKIP_BLANK_GROUPS）。
 * 拉 STB 之前会做一次热保护检查；熔断时断电并直接返回，走纸节拍由调用方兜底 */
void printhead_heat_group(const uint8_t *row, uint8_t g);

/* 裸脉宽加热第 g 组：不数黑点、不跳过空组、不重复、不掺任何补偿，
 * 组间间隔由调用者指定。只给 raw 对照页验证供电恢复时间用。
 * 返回 false 表示已熔断（调用方应停止后续组，但走纸仍要照常完成） */
bool printhead_heat_group_raw(uint8_t g, uint16_t us, uint16_t cool_us);

/* RTOS 让出：每 8 行让出 1 个 tick。
 * 一行内部绝不能切任务（微秒级时序会被拉长），但连续几百行纯忙等会饿死同核任务 */
void printhead_row_yield(void);

/* ---- 一行的拍子参数（给 hal/printer.cpp 编排用） ---- */
uint8_t  printhead_groups_per_beat(void);   // 1-2 相 2 组 / 双相 3 组
uint32_t printhead_min_beat_us(void);       // 拍槽下限，防电机丢步

/* ---- 能量参数 ---- */
/* 加热时间(µs)：影响浓度和速度。App 浓度滑杆 + SET_HEAT 最终落到这里 */
void     printhead_set_heat_us(uint16_t us);
uint16_t printhead_get_heat_us(void);

/* 临时把加热时间钉住（诊断页标尺用），传 0 恢复正常。
 * 钉住期间 set_heat_us() 不再起作用，否则每秒一次的温度补偿会覆盖它 */
void printhead_set_heat_override(uint16_t us);

/* ---- 打印质量 ----
 * 三个手段解决的是「点与点深浅不一」，拆机头尤其明显：
 *   preheat  预加热脉冲(µs)   高阻点先抬到接近发色阈值，0 = 关闭
 *   repeat   同一行加热遍数   1 = 关闭，2~3 能量叠加，成倍变慢
 *   dot_comp 点密度补偿(%)    一组黑点越多 VH 掉得越狠，按 n² 加时间，0 = 关闭
 */
void     printhead_set_preheat(uint16_t us);
void     printhead_set_repeat(uint8_t n);
void     printhead_set_dot_comp(uint8_t pct);
uint16_t printhead_get_preheat(void);
uint8_t  printhead_get_repeat(void);
uint8_t  printhead_get_dot_comp(void);

/* ---- 驱动方式 ----
 * dual = 0：1-2 相半步，一拍 0.03125mm，一行 4 拍，平均电流小
 * dual = 1：双相整步，  一拍 0.0625mm， 一行 2 拍，力矩恒定、走纸更稳 */
void    printhead_set_drive_mode(uint8_t dual);
uint8_t printhead_get_drive_mode(void);

/* 单行耗时(µs)：mm/s = 0.125 * 1e6 / line_us。
 * 返回的是「满行」保守上限，只用于串口日志与界面估算 */
uint32_t printhead_line_us(void);

#endif
