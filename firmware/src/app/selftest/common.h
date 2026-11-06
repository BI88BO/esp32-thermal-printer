/* ============================================================================
 * 文件：app/selftest/common.h
 * 层次：应用层 / 诊断页内部公用
 * 职责：所有诊断页共用的两件小事——能量标尺复位、页面起始模板。
 *
 * 被谁用：app/selftest/page_*.cpp（都在同目录，#include "common.h"）
 * ========================================================================== */
#ifndef SELFTEST_COMMON_H
#define SELFTEST_COMMON_H

#include <Arduino.h>
#include "config.h"
#include "printhead.h"

/* 进入 / 退出诊断页时都要把能量标尺复位成「正常路径」。
 * 两个函数体一样是有意的：begin 清掉上一次可能残留的 override，
 * end 保证这张纸打完后不会把裸脉宽留给下一次作业。
 * 写成两个名字是为了在成对调用的地方一眼看出意图，不要合并。
 *
 * 背景：诊断页主体与 App 正常打印走同一条能量路径
 *   heat_us = App 浓度 × 按实测头温的温度补偿，
 *   冷头起步 ×1.25 并随热状态平滑退出，头热起来最多再削 15%。
 * 只有「能量阶梯」四块是裸脉宽 override，不掺这些系数，仍是一把干净的尺子。
 * build 78~80 曾把整页钉在 800µs，那是按标称 P0=0.24W/dot
 * 折算的绝对安全窗口；但实测 27℃ 冷态连版本号都几乎打不出来，标尺失效。 */
static inline void test_heat_begin(void) { printhead_set_heat_override(0); }
static inline void test_heat_end(void)   { printhead_set_heat_override(0); }

#endif
