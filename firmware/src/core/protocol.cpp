/* ============================================================================
 * 文件：core/protocol.cpp
 * 层次：纯逻辑层 / BLE 帧编解码
 * 职责：逐字节喂入的状态机拼帧（proto_feed）、校验、回调分发；
 *       以及回帧（proto_send / status / error / ack）的组装。
 *
 * 关键约束：BLE 的 onWrite 一次可能只给一帧的一部分（MTU 分片），
 *   所以 proto_feed 必须能跨调用保持状态；坏帧计入 s_bad，
 *   app/job.cpp 断流超时时用它区分「协议对不上」和「数据没到」。
 *
 * 本层不碰硬件：发送只调 transport_send()（实现在 hal/ble.cpp），
 *   收到完整帧只调 proto_init() 注册的回调（实现在 app/command.cpp）。
 * ========================================================================== */

#include "protocol.h"
#include <Arduino.h>

/* 解析状态机 */
enum { ST_HEAD = 0, ST_TYPE, ST_LEN, ST_DATA, ST_CHK };

static proto_cb_t s_cb    = nullptr;
static uint8_t    s_state = ST_HEAD;
static uint8_t    s_type  = 0;
static uint8_t    s_len   = 0;
static uint8_t    s_idx   = 0;
static uint8_t    s_chk   = 0;
static uint8_t    s_buf[PROTO_MAX_PAYLOAD];

/* 统计被丢弃的帧：长度非法、校验错。
 * 联调时看这个数就能分清"链路没通"和"协议对不上" */
static uint16_t s_bad = 0;

uint16_t proto_bad_frames(void) { return s_bad; }
void     proto_reset_stats(void) { s_bad = 0; }

void proto_init(proto_cb_t cb)
{
    s_cb    = cb;
    s_state = ST_HEAD;
}

void proto_feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        uint8_t b = data[i];

        switch (s_state)
        {
        case ST_HEAD:
            if (b == PROTO_HEAD)
                s_state = ST_TYPE;
            break;

        case ST_TYPE:
            s_type = b;
            s_chk  = b;
            s_state = ST_LEN;
            break;

        case ST_LEN:
            s_len = b;
            if (s_len > PROTO_MAX_PAYLOAD)
            {
                s_bad++;
                s_state = ST_HEAD;          // 长度非法，重新找帧头
                break;
            }
            s_chk ^= b;
            s_idx = 0;
            s_state = (s_len == 0) ? ST_CHK : ST_DATA;
            break;

        case ST_DATA:
            s_buf[s_idx++] = b;
            s_chk ^= b;
            if (s_idx >= s_len)
                s_state = ST_CHK;
            break;

        case ST_CHK:
            if (b != s_chk)
                s_bad++;
            else if (s_cb != nullptr)
                s_cb(s_type, s_buf, s_len);
            s_state = ST_HEAD;
            break;
        }
    }
}

void proto_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    if (len > PROTO_MAX_PAYLOAD)
        return;

    uint8_t buf[PROTO_MAX_PAYLOAD + 4];
    uint8_t chk = type ^ len;

    buf[0] = PROTO_HEAD;
    buf[1] = type;
    buf[2] = len;

    for (uint8_t i = 0; i < len; i++)
    {
        buf[3 + i] = payload[i];
        chk ^= payload[i];
    }
    buf[3 + len] = chk;

    transport_send(buf, (uint16_t)len + 4);
}

void proto_send_status(uint8_t paper, uint8_t temp, uint16_t bat_mv,
                       uint8_t state, uint16_t done, uint8_t bat_pct)
{
    uint8_t p[8];
    p[0] = paper;
    p[1] = temp;
    p[2] = (uint8_t)(bat_mv & 0xFF);
    p[3] = (uint8_t)(bat_mv >> 8);
    p[4] = state;
    p[5] = (uint8_t)(done & 0xFF);
    p[6] = (uint8_t)(done >> 8);
    /* 第 8 字节：电量百分比。固件已经有滤波后的放电曲线换算（hal/battery.cpp），
     * 之前算完没往上送，App 只能显示电压。 */
    p[7] = bat_pct;
    proto_send(EVT_STATUS, p, sizeof(p));
}

void proto_send_error(uint8_t code)
{
    proto_send(EVT_ERROR, &code, 1);
}

void proto_send_ack(uint8_t cmd, uint8_t code)
{
    uint8_t p[2] = {cmd, code};
    proto_send(EVT_ACK, p, 2);
}
