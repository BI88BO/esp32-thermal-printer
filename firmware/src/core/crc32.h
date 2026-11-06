/* ============================================================================
 * 文件：core/crc32.h
 * 层次：纯逻辑层 / 通用校验
 * 职责：CRC32/IEEE 增量计算，和 Java java.util.zip.CRC32、zlib 完全一致。
 *
 * 用法：
 *   uint32_t c = CRC32_INIT;
 *   c = crc32_update(c, buf, n);     // 可以分多次喂
 *   uint32_t value = crc32_final(c); // 取反得到最终校验值
 *
 * 按位实现不用查表：1.1MB 约 9M 次循环，240MHz 上几十 ms，只在升级时跑。
 *
 * 依赖：无
 * 被谁用：core/ota.cpp（固件侧校验）、App 侧用 java.util.zip.CRC32
 * ========================================================================== */
#ifndef CRC32_H
#define CRC32_H

#include <Arduino.h>

#define CRC32_INIT 0xFFFFFFFFu

/* 增量累加。可以分块调用，结果与一次性算完相同 */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n);

/* 收尾：按 IEEE 规范取反 */
uint32_t crc32_final(uint32_t crc);

#endif
