/* ============================================================================
 * 文件：core/heat_model.cpp
 * 层次：纯逻辑层 / 能量策略
 * 职责：热积累前馈模型 + 冷头补偿 + 点密度补偿 + 黑点统计。
 *
 * 模型：把打印头铝基板看成一个一阶热系统
 *   ds/dt = (target - s)/τ，精确解 s ← target + (s - target)·exp(-dt/τ)
 *   s 归一化到 0~1000：全黑行 + 基准浓度 + 1 遍 = 1000
 *   τ = 64 行 × 31ms ≈ 2s，与铝基板热时间常数同量级
 *
 * 为什么用精确解而不是线性近似：连打两张之间大约隔 2~3 秒
 *   （退纸 0.7s + 按键间隔 + VH 预热 1s），「dt>=τ 就归零」的近似会把
 *   余热清成 0，第二张又给满冷头补偿，反而制造「越打越黑」。
 *   精确解在 2.7s 时仍保留约 26% 余热，第二张起步自然淡一档。
 *
 * 计算量：每行一次 expf，ESP32 有单精度 FPU，相对 30ms 行周期可忽略；
 *         纯计算，不 delay、不抢其它核，不影响 RTOS 实时性。
 *
 * 诊断页的裸脉宽是「绝对能量标尺」：hal/printer.cpp 在 heat_pin 非 0 时
 * 直接返回，不经过本模块，标尺不会被补偿污染。
 * ========================================================================== */

#include "heat_model.h"
#include <Arduino.h>
#include "config.h"
#include <math.h>

/* 一行约 30.7ms，64 行 ≈ 2s */
#define THERM_TAU_MS  ((uint32_t)THERM_WINDOW_ROWS * 31UL)

/* 只被 printTask（经 hal/printer.cpp）串行读写，无需加锁。
 * cold_boost = 冷头起步额外能量的百分比，作业开始时按实测头温装载。 */
static uint16_t therm_state   = 0;
static uint32_t therm_last_ms = 0;
static uint8_t  cold_boost    = 0;

/* 过热暂停的现场快照，见 heat_model.h */
static uint16_t pause_therm = 0;
static uint8_t  pause_boost = 0;

/* ==================== 基础统计 ==================== */

uint8_t heat_count_black(const uint8_t *row, uint8_t g)
{
    static const uint8_t BITS[16] = {0, 1, 1, 2, 1, 2, 2, 3,
                                     1, 2, 2, 3, 2, 3, 3, 4};
    const uint8_t *p = row + (uint16_t)g * 8;
    uint8_t n = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        n += BITS[p[i] & 0x0F];
        n += BITS[p[i] >> 4];
    }
    return n;
}

/* ==================== 一阶热模型 ==================== */

static void therm_step(uint16_t target, uint32_t dt_ms)
{
    if (dt_ms == 0)
        return;
    float k = 1.0f - expf(-(float)dt_ms / (float)THERM_TAU_MS);
    float s = (float)therm_state + ((float)target - (float)therm_state) * k;
    if (s < 0.0f) s = 0.0f;
    if (s > THERM_STATE_FULL) s = (float)THERM_STATE_FULL;
    therm_state = (uint16_t)(s + 0.5f);
}

void heat_model_init(void)
{
    therm_state   = 0;
    therm_last_ms = millis();
    cold_boost    = 0;
}

void heat_model_job_begin(int8_t head_temp)
{
    /* 冷头起步的「资格」只看这张纸开局时头有多热：25℃ 及以下给满，
     * 到 COLD_START_TEMP_C 线性退到 0。NTC 读不到时保守不补，
     * 避免传感器故障时盲目加能量。
     * 作业内部的补偿幅度则由 s 驱动（见 heat_model_group_us），两者分工：
     * NTC 管开局基底，s 管打印过程中的热积累。 */
    const uint8_t BOOST_MAX = COLD_START_PCT - 100;
    if (head_temp < 0 || head_temp >= COLD_START_TEMP_C)
        cold_boost = 0;
    else if (head_temp <= 25)
        cold_boost = BOOST_MAX;
    else
        cold_boost = (uint8_t)((uint32_t)BOOST_MAX *
                               (COLD_START_TEMP_C - head_temp) / (COLD_START_TEMP_C - 25));
}

void heat_model_cool_to_now(void)
{
    uint32_t now = millis();
    therm_step(0, now - therm_last_ms);
    therm_last_ms = now;
}

void heat_model_row_begin(const uint8_t *row, uint16_t us, uint8_t repeat)
{
    /* 归一化到「全黑 + 基准浓度 + 1 遍」= 1000：浓度拉高、repeat 加倍、
     * 满版黑图都会让 s 爬得更快；稀疏文字几乎不积累。 */
    uint16_t dots = 0;
    for (uint8_t g = 0; g < 6; g++)
        dots += heat_count_black(row, g);

    uint64_t e  = (uint64_t)dots * us * repeat * THERM_STATE_FULL
                  / ((uint32_t)TPH_DOTS * HEAT_US_DEFAULT);
    if (e > THERM_STATE_FULL) e = THERM_STATE_FULL;

    uint32_t now = millis();
    therm_step((uint16_t)e, now - therm_last_ms);
    therm_last_ms = now;
}

uint16_t heat_model_trim_pct(void)
{
    /* 热状态越高，同样脉宽打出来越黑，按比例收回来。只往下调，
     * 永远不会超过 App 设的浓度；最多削 THERM_TRIM_MAX_PCT(15%)。 */
    return (uint16_t)(100 - (uint32_t)therm_state * THERM_TRIM_MAX_PCT
                            / THERM_STATE_FULL);
}

uint16_t heat_model_group_us(uint16_t base_us, uint8_t black_dots, uint8_t dotcomp_pct)
{
    uint16_t base = base_us;

    /* ① 冷头起步：s=0 给满 25%，随热状态平滑退到 0。
     * 取代旧版「固定前 24 行 ×1.25」硬切——那个第 25 行会留一道浓淡分界，
     * 而且只补开头、不管后面热了要削。 */
    if (cold_boost)
    {
        uint32_t b = (uint32_t)cold_boost * (THERM_STATE_FULL - therm_state)
                     / THERM_STATE_FULL;
        base = (uint16_t)((uint32_t)base * (100 + b) / 100);
    }

    /* ② 热积累削减：连打黑图 / 连打几张后 s 升高，按表 4 斜率提前收能量 */
    base = (uint16_t)((uint32_t)base * heat_model_trim_pct() / 100);

    /* ③ 点密度补偿。黑点越多，同时导通的点越多，VH 被拉得越低，
     * 同样时间打出来的颜色越浅，所以按 n² 补偿——电流线性叠加，
     * 但能量 ∝ V²，掉压后需要更长的时间 */
    uint32_t t = base;
    if (dotcomp_pct != 0 && black_dots >= DOTCOMP_MIN_DOTS)
    {
        /* 用 64 位中间量：base(≤5000) × dotcomp(≤150) × 64 × 64 已经到 3.07e9，
         * 离 uint32 上限 4.29e9 只剩 28% 余量。哪天把 DOTCOMP_PCT_MAX 往上
         * 提一点（比如 250）就会静默溢出，打出一片过深的黑 —— 这种 bug 现场
         * 极难复现，不如一开始就用 64 位算。 */
        uint32_t extra = (uint32_t)((uint64_t)base * dotcomp_pct * black_dots * black_dots
                                    / (4096ULL * 100ULL));
        t = (uint32_t)base + extra;
    }

    /* ④ 钳位——这里是整条链路的唯一出口，三条分支共用，绝不提前 return。
     * build 88 及以前点补偿分支单独钳 HEAT_US_MAX、dotcomp=0 分支直接返回，
     * 造成两个后果：
     *   · 标准档冷头能到 5000µs，超出固件自定上限 25%；
     *   · 加深档开了点补偿反而被钳回 4000，比标准档更淡（档位语义反了）。
     * 上限用 HEAT_US_ABS_MAX = 浓度上限 × 冷头满补，理由见 config.h。 */
    return (t > HEAT_US_ABS_MAX) ? HEAT_US_ABS_MAX : (uint16_t)t;
}

/* ==================== 暂停现场 ==================== */

void heat_model_pause_save(void)
{
    pause_therm = therm_state;
    pause_boost = cold_boost;
}

void heat_model_pause_restore(void)
{
    therm_state   = pause_therm;
    cold_boost    = pause_boost;
    therm_last_ms = millis();
}

uint16_t heat_model_state(void)
{
    return therm_state;
}
