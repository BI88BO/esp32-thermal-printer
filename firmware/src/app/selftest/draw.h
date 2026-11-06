/* ============================================================================
 * 文件：app/selftest/draw.h
 * 层次：应用层 / 诊断页绘图基元
 * 职责：往一行 48 字节的缓冲区里画 3×5 点阵数字、画版本号+头温抬头。
 *        只负责"点怎么摆"，不负责开机、关机和能量。
 * 对应原工程：无
 *
 * 被谁用：page_dotcheck.cpp（版本号 + 阶梯右侧脉宽）、page_ruler.cpp（版本号）
 * ========================================================================== */
#ifndef SELFTEST_DRAW_H
#define SELFTEST_DRAW_H

#include <Arduino.h>

/* 在某一行的 x0 处画一个数字（3×5 字模，scale 倍放大）。
 * y 是整块内的行号 0..(5*scale-1)，越界自动忽略 */
void draw_digit_row(uint8_t *row, uint16_t x0, uint8_t d, uint8_t y, uint8_t scale);

/* 画一个十进制数，nd = 位数（含前导 0） */
void draw_num_row(uint8_t *row, uint16_t x0, uint16_t val, uint8_t y,
                  uint8_t scale, uint8_t nd);

/* 打印抬头：左侧 2 位固件版本号，右侧 3 位实测头温（℃）。
 * 会直接调 printer_job_row() 打 14 行（10 行数字 + 4 行留白），
 * 调用者必须已经 printer_job_begin() */
void draw_build_code(uint8_t code);

#endif
