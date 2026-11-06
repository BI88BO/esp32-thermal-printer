/* ============================================================================
 * 文件：hal/printhead.cpp
 * 层次：硬件抽象层 (HAL) / 打印头本体
 * 职责：STB 选通 + SPI 送行 + 单组加热 + 质量参数寄存器 + 行时间估算。
 * 依赖：config.h、core/heat_model.h（算脉宽）、hal/thermal_guard.h（熔断判定）、
 *       hal/vh.h（熔断时立即断电）、hal/sensor.h（间接，经 thermal_guard）
 * 被谁用：hal/printer.cpp（行编排）
 *
 * 关于 SPI 只写不收：hspi.transfer(buf, len) 是双向的，会把 MISO(GPIO16)
 * 读到的电平写回 buf。只要调用方每行都 memset 就不会暴露，但这里 row 指向
 * 调用方的缓冲区，覆写它属于越界副作用，所以用 writeBytes()。
 * 注意：transfer() 本身不会导致"只出第一行"，两种写法都能打满整块。
 * ========================================================================== */

#include "printhead.h"
#include <Arduino.h>
#include "config.h"
#include "heat_model.h"
#include "thermal_guard.h"
#include "vh.h"
#include "motor.h"
#include <SPI.h>

static const uint8_t STB_PIN[6] = {PIN_STB1, PIN_STB2, PIN_STB3, PIN_STB4, PIN_STB5, PIN_STB6};

/* 一拍的最短时间。空白行会跳过整组加热，拍就会挤在几微秒里发出去——
 * 那是上万 PPS，电机直接丢步。这里给每个拍槽兜底，保证节拍恒定。
 * 双相的一拍跨度是 1-2 相的两倍，所以允许的拍率要减半（1800µs ≈ 556PPS） */
#define MIN_BEAT_US       1200
#define MIN_BEAT_DUAL_US  1800

static SPIClass hspi(HSPI);

/* ==================== 质量参数寄存器 ====================
 * 全部由 App 通过 BLE 下发，setter 里统一做钳位，越界值不会进到加热路径。 */

static uint16_t heat_us     = HEAT_US_DEFAULT;   // 正常浓度（含每秒一次的温度补偿）
static uint16_t heat_pin    = 0;                 // 非 0 时强制使用这个加热时间（诊断页标尺）
static uint16_t preheat_us  = PREHEAT_US_DEFAULT;
static uint8_t  repeat_n    = REPEAT_DEFAULT;
static uint8_t  dotcomp_pct = DOTCOMP_PCT_DEFAULT;

/* 驱动方式：
 *   0 = 1-2 相（8 状态，一拍 4.5° = 0.03125mm，一行 4 拍）
 *   1 = 双相整步（4 状态，一拍 9° = 0.0625mm，一行 2 拍）
 *
 * 双相每个状态都是两相通电，力矩恒定；1-2 相里那两个单相拍在 这颗 H 桥驱动 上
 * 是"两输入全低 = 短接制动"，力矩会掉到 70% 再弹回来，纸会轻微一顿，
 * 表现就是纵向的横纹。双相没这个问题，而且同样纸速下拍率减半，余量更大。 */
static uint8_t drive2 = 0;

/* RTOS 让出计数器 */
static uint8_t row_yield_div = 0;

/* ==================== 基础动作 ==================== */

void printhead_stb_idle(void)
{
    for (uint8_t i = 0; i < 6; i++)
        digitalWrite(STB_PIN[i], LOW);
}

void printhead_send_row(const uint8_t *row)
{
    hspi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    hspi.writeBytes(row, TPH_BYTES);
    hspi.endTransaction();
    digitalWrite(PIN_LAT, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_LAT, HIGH);
}

void printhead_row_yield(void)
{
    if (++row_yield_div >= 8)
    {
        row_yield_div = 0;
        vTaskDelay(1);
    }
}

/* ==================== 能量策略的薄封装 ====================
 * 整套一阶热模型、冷头起步、热积累削减、n² 点密度补偿、最终钳位都在
 * core/heat_model.cpp。这里只保留一个「标尺例外」，不重复任何计算。 */
static uint16_t group_heat_us(uint8_t n)
{
    /* 自检页的裸脉宽是「绝对能量标尺」：不掺温度补偿、不掺点密度补偿，
     * 也不掺热积累削减，不受 App 设过的质量档位影响。否则同一档位每张纸
     * 的实际能量都不同，阶梯就失去横向比较的意义。 */
    if (heat_pin)
        return heat_pin;

    return heat_model_group_us(heat_us, n, dotcomp_pct);
}

void printhead_heat_group(const uint8_t *row, uint8_t g)
{
    /* 行级过热熔断：保护必须落在「真正拉 STB」这一层，任何调用路径
     * （App 打印 / 按键点检 / 全部诊断页 / raw 对照页）都绕不过去，
     * 也不依赖上层任务什么时候被调度到。
     * 光"不拉 STB"不够：VH 还开着时打印头一直带电，温度降不下来，
     * 所以这里直接强制断电（幂等，行内调用安全）。 */
    if (thermal_guard_blocked())
    {
        vh_force_off();
        return;                     // 走纸节拍由调用方 slot_pad 兜底，纸照走
    }

    uint8_t n = heat_count_black(row, g);

#if SKIP_BLANK_GROUPS
    if (n == 0)
        return;
#endif

    uint16_t t = group_heat_us(n);

    for (uint8_t p = 0; p < repeat_n; p++)
    {
        if (preheat_us)
        {
            digitalWrite(STB_PIN[g], HIGH);
            delayMicroseconds(preheat_us);
            digitalWrite(STB_PIN[g], LOW);
            delayMicroseconds(PREHEAT_GAP_US);
        }

        digitalWrite(STB_PIN[g], HIGH);
        delayMicroseconds(t);
        digitalWrite(STB_PIN[g], LOW);

        if (p + 1 < repeat_n)
            delayMicroseconds(REPEAT_HOLD_US);
    }

    delayMicroseconds(COOL_US);
}

bool printhead_heat_group_raw(uint8_t g, uint16_t us, uint16_t cool_us)
{
    /* raw 路径不数黑点，整组直接拉，也没有 repeat / 预加热 / 点补偿。
     * 热保护照做，但这里只置锁存、不断电——与改动前的行为保持一致：
     * 调用方随后还会走完 4 拍走纸，再由上层 job_end 关 VH。 */
    if (thermal_guard_blocked())
        return false;

    digitalWrite(STB_PIN[g], HIGH);
    delayMicroseconds(us);
    digitalWrite(STB_PIN[g], LOW);
    delayMicroseconds(cool_us);
    return true;
}

/* ==================== 一行的拍子参数 ==================== */

uint8_t printhead_groups_per_beat(void)  { return drive2 ? 3 : 2; }
uint32_t printhead_min_beat_us(void)     { return drive2 ? MIN_BEAT_DUAL_US : MIN_BEAT_US; }

/* ==================== 初始化 ==================== */

void printhead_begin(void)
{
    pinMode(PIN_LAT, OUTPUT);
    digitalWrite(PIN_LAT, HIGH);

    for (uint8_t i = 0; i < 6; i++)
    {
        pinMode(STB_PIN[i], OUTPUT);
        digitalWrite(STB_PIN[i], LOW);
    }

    hspi.begin(PIN_SCK, 16, PIN_SDA, -1);
    hspi.setFrequency(2000000);
}

/* ==================== 参数 setter / getter ==================== */

void printhead_set_heat_us(uint16_t us)
{
    if (us < HEAT_US_MIN) us = HEAT_US_MIN;
    if (us > HEAT_US_MAX) us = HEAT_US_MAX;
    heat_us = us;
}

void printhead_set_heat_override(uint16_t us)
{
    /* 诊断页专用：上限与正常打印一致（HEAT_US_MAX，由 App 浓度滑杆控制）。
     * 早期钳到 800µs 是按规格书标称 P0=0.24W/dot 折算的
     * 「绝对安全窗口」，但实测 27℃ 冷态连 800µs 都几乎不出色 —— 若标称
     * 成立，800µs 已接近 E0 的 96%、应当接近饱满黑，因此这块拆机头的
     * 实际点功率远低于标称，标称折算不适用。
     * 安全改由 50℃ 熔断（表 1 工作温度上限）+ 用户自定浓度上限兜底。 */
    if (us > HEAT_US_MAX) us = HEAT_US_MAX;
    heat_pin = us;
}

uint16_t printhead_get_heat_us(void)
{
    return heat_pin ? heat_pin : heat_us;
}

void printhead_set_preheat(uint16_t us)
{
    if (us > PREHEAT_US_MAX) us = PREHEAT_US_MAX;
    preheat_us = us;
}

void printhead_set_repeat(uint8_t n)
{
    if (n < 1) n = 1;
    if (n > REPEAT_MAX) n = REPEAT_MAX;
    repeat_n = n;
}

void printhead_set_dot_comp(uint8_t pct)
{
    if (pct > DOTCOMP_PCT_MAX) pct = DOTCOMP_PCT_MAX;
    dotcomp_pct = pct;
}

uint16_t printhead_get_preheat(void)  { return preheat_us; }
uint8_t  printhead_get_repeat(void)   { return repeat_n; }
uint8_t  printhead_get_dot_comp(void) { return dotcomp_pct; }

void printhead_set_drive_mode(uint8_t dual)
{
    drive2 = dual ? 1 : 0;
    /* 切换后立刻把相位对齐到"两相通电"那四个状态之一。
     * 走纸（motor_feed）固定用 8 状态半步，会把相位留在奇数位，
     * 双相模式下踩到单相态力矩更差。这里与分层不冲突：
     * 只借用 motor 的相位对齐，不参与任何走纸节拍。 */
    motor_align_dual();
}

uint8_t printhead_get_drive_mode(void) { return drive2; }

/* ==================== 行时间估算 ==================== */

uint32_t printhead_line_us(void)
{
    /* 一组的实际耗时，按 printhead_heat_group() 的真实时序算：
     *   repeat × (预加热 + 间隙 + 主加热) + (repeat-1) × 遍间间隔 + 组尾冷却
     * 旧公式把预加热只算一次、还漏了 REPEAT_HOLD_US，在「最强」档低估约 5%，
     * 于是 App 显示 2.58mm/s、实际只有 2.46mm/s。 */
    uint16_t t = heat_pin ? heat_pin : heat_us;
    uint32_t per = (uint32_t)repeat_n *
                   ((uint32_t)t + (preheat_us ? (uint32_t)preheat_us + PREHEAT_GAP_US : 0));
    per += (uint32_t)(repeat_n - 1) * REPEAT_HOLD_US + COOL_US;

    uint32_t slot = (uint32_t)printhead_groups_per_beat() * per;
    if (slot < printhead_min_beat_us())
        slot = printhead_min_beat_us();

    /* 1-2 相是 3 个加热拍 + 1 个空拍（空拍只受 min_beat_us 约束），
     * 不是 4 个满拍——旧公式按 4×slot 算，高估了整整一个加热拍，
     * App 拿它做发送节流时就白等约 26%。双相是 2 个满拍，公式本来就准。 */
    uint32_t line;
    if (drive2)
        line = 2 * slot + 250;
    else
        line = 3 * slot + printhead_min_beat_us() + 250;

    /* SKIP_BLANK_GROUPS 开启后稀疏行会明显更快，所以这里返回的是「满行」
     * 保守上限，只用于串口日志；发送节奏由 App 的 doneLines 流控决定。 */
    return line;
}
