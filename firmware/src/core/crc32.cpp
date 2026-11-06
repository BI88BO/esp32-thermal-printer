/* ============================================================================
 * 文件：core/crc32.cpp
 * 层次：纯逻辑层 / 通用校验
 * 职责：CRC32/IEEE 按位实现。多项式 0xEDB88320（反射形式）。
 * ========================================================================== */

#include "crc32.h"
#include <Arduino.h>

uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

uint32_t crc32_final(uint32_t crc)
{
    return ~crc;
}
