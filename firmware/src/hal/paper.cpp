/* ============================================================================
 * 文件：hal/paper.cpp
 * 层次：硬件抽象层 (HAL) / 缺纸检测
 * 职责：电平读取与去抖。语义见 paper.h。
 * 依赖：config.h
 * ========================================================================== */

#include "paper.h"
#include <Arduino.h>
#include "config.h"

static bool    ok  = true;
static uint8_t cnt = 0;

void paper_begin(void)
{
    pinMode(PIN_PAPER, INPUT);          // GPIO35 只能作输入

    /* 用第一次实读作初值。原先固定写 true（"假设有纸"），开机就没纸时要等
     * 连续 3 次采样（约 300ms）才翻转成缺纸，这期间点打印会被误放行。
     * 直接采一次既能立刻反映真实状态，也省掉一次 300ms 的盲区。
     * 万一传感器上电未稳导致误判缺纸，方向也是安全的（拒绝打印）。 */
    ok  = (digitalRead(PIN_PAPER) == LOW);
    cnt = 0;
}

void paper_update(void)
{
    /* 手册 5.1：有反射（有纸）= 低电平 */
    bool raw = (digitalRead(PIN_PAPER) == LOW);

    if (raw != ok)
    {
        if (++cnt >= PAPER_DEBOUNCE)
        {
            ok  = raw;
            cnt = 0;
        }
    }
    else
    {
        cnt = 0;
    }
}

bool paper_ok(void)
{
    return ok;
}
