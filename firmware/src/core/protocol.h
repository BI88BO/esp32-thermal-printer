/* ============================================================================
 * 文件：core/protocol.h
 * 层次：纯逻辑层 / BLE 通信协议
 * 职责：定义帧格式、命令码、事件码、错误码与协议层接口。
 * 对应原工程：无（原工程用固定字节序列 A5 A5 A5 A5 0x01/02/03 下发密度）
 *
 * 帧格式（详见 newproject/PROTOCOL.md）：
 *   AA 55 | type(1) | len(1) | payload(len) | checksum(1)
 *   payload 上限 245 字节；App 一次装 4 行（4×48 + 1 = 193B）
 *
 * 命令在这里定义、在 app/command.cpp 分发；本层只负责拼帧与校验，
 * 不认识任何打印业务。transport_send() 由 hal/ble.cpp 实现（物理发送）。
 * ========================================================================== */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

/* 帧格式：A5 | TYPE | LEN | PAYLOAD[LEN] | CHK
 * CHK = TYPE ^ LEN ^ 所有 payload 字节 */

#define PROTO_HEAD        0xA5

/* 负载上限。必须 >= 1 + 48*NROWS（行序号 + N 行位图）：
 *   4 行 = 193，5 行 = 241。设为 245 留 4 字节余量。
 * 注意 LEN 是单字节，所以上限不能超过 255 */
#define PROTO_MAX_PAYLOAD 245

/* App -> 打印机 */
#define CMD_PRINT_BEGIN   0x01
#define CMD_PRINT_DATA    0x02
#define CMD_PRINT_END     0x03
#define CMD_FEED          0x10
#define CMD_SET_HEAT      0x11
#define CMD_SET_QUALITY   0x12        /* preheat(2) repeat(1) dotcomp(1) */
#define CMD_SET_DRIVE     0x13        /* mode(1): 0=1-2相 1=双相整步 */
#define CMD_GET_STATUS    0x20
#define CMD_SELFTEST      0x30        /* kind(1): 0=整页自检 1=打印头点检 2=负载页 3=负载+点检 */

/* ---- BLE OTA（build 86 起）----
 * 传的是「纯 app 镜像」firmware.bin，不是含 bootloader/分区表的 merged 包。
 * 分块停等：App 发一批后等 EVT_OTA_ACK，next_seq 告诉它固件实际收到哪块，
 * 缺口就从那里补发。固件把写 flash 放在独立任务，BLE 回调只入队。 */
#define CMD_OTA_INFO      0x40        /* 查询版本 / 当前 slot / 可写上限 */
#define CMD_OTA_BEGIN     0x41        /* size(4,LE) + crc32(4,LE) */
#define CMD_OTA_DATA      0x42        /* seq(2,LE) + data[<=243] */
#define CMD_OTA_END       0x43        /* 数据发完，校验并重启 */
#define CMD_OTA_ABORT     0x44        /* 放弃升级 */

/* 打印机 -> App */
#define EVT_STATUS        0x80
#define EVT_ACK           0x81
#define EVT_ERROR         0x82
#define EVT_OTA_INFO      0x83        /* build(1) slot(1) max_size(4) active(1) written(4) */
#define EVT_OTA_ACK       0x84        /* cmd(1) code(1) next_seq(2) received(4) */

/* 错误码（打印） */
#define ERR_PAPER         1
#define ERR_OVERHEAT      2
#define ERR_LOW_BAT       3
#define ERR_TIMEOUT       4
#define ERR_PARAM         5

/* 错误码（OTA ACK 的 code 字段，独立于上面的打印错误） */
#define OTA_OK             0
#define OTA_ERR_BUSY       1
#define OTA_ERR_SIZE       2
#define OTA_ERR_SPACE      3
#define OTA_ERR_STATE      4
#define OTA_ERR_WRITE      5
#define OTA_ERR_QUEUE      6
#define OTA_ERR_INCOMPLETE 7
#define OTA_ERR_CRC        8
#define OTA_ERR_ABORT      9

typedef void (*proto_cb_t)(uint8_t type, const uint8_t *payload, uint8_t len);

/* 注册命令回调，并复位解析状态 */
void proto_init(proto_cb_t cb);

/* 把收到的原始字节喂进来，内部负责找帧头、校验、分发 */
void proto_feed(const uint8_t *data, uint16_t len);

/* 组帧并通过传输层发出 */
void proto_send(uint8_t type, const uint8_t *payload, uint8_t len);
/* STATUS 帧：paper(1) temp(1) bat_mv(2) state(1) done(2) bat_pct(1)
 * bat_pct 是后加的第 8 字节，追加在帧尾——旧 App 按 len<7 判断仍然可用，
 * 只是读不到百分比。新增字段一律追加，不改已有字段的位置。 */
void proto_send_status(uint8_t paper, uint8_t temp, uint16_t bat_mv,
                       uint8_t state, uint16_t done, uint8_t bat_pct);
void proto_send_error(uint8_t code);
void proto_send_ack(uint8_t cmd, uint8_t code);

/* 被丢弃的帧数（长度非法或校验失败）。用来区分"没收到数据"和"收到了但不合法" */
uint16_t proto_bad_frames(void);
void     proto_reset_stats(void);

/* 由传输层（main.cpp）实现：把一帧原始字节发出去 */
void transport_send(const uint8_t *buf, uint16_t len);

#endif
