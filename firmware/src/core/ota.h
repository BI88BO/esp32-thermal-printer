/* ============================================================================
 * 文件：core/ota.h
 * 层次：纯逻辑层 / BLE 固件升级
 * 职责：双 app 分区 OTA 的对外接口与错误码。
 * 对应原工程：无（原工程没有 OTA）
 *
 * 原理：写进「当前没在跑」的那个 app 分区 → CRC32 校验 →
 *       Update.end() 改 otadata 启动指针 → ESP.restart()；
 *       失败/超时/断连只 abort，otadata 不变，开机还是原固件。
 *       详见 newproject/PROTOCOL.md 第 8 节。
 *
 * 注意：写 flash 全在独立 otaTask(core1)，BLE 回调只做序号校验 + 入队。
 * ========================================================================== */
#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

/*
 * BLE OTA：把 App 发来的纯 app 镜像（firmware.bin，不是 merged_firmware.bin）
 * 写进「另一个」1.25MB app 分区，校验后重启切换。
 *
 * 分区表是 PlatformIO 默认的 4MB default.csv，本来就有 otadata + app0 + app1，
 * 所以不需要换分区表、也不需要专用的 OTA bootloader。串口烧录 merged 包时会把
 * 0xE000 的 otadata 一并擦成 0xFF，bootloader 自动回到 app0，不会和 OTA 冲突。
 *
 * 并发模型：
 *   BLE 回调只做「序号校验 + CRC32 累加 + 入队」，绝不碰 flash；
 *   所有 Update.begin/write/end/abort 都在独立的 otaTask 里串行执行。
 *   否则 flash 擦除（几十 ms）会卡在 BLE 回调里，表现为连接假死甚至断连。
 */
void ota_init(void);

/* OTA 进行中：printTask 必须整段跳过，按键自检/走纸也不许插进来 */
bool ota_active(void);

/* 开始一次升级。size = app 镜像字节数，crc = CRC32(IEEE)。
 * printer_idle 由 main.cpp 判断（无作业、无排队自检/走纸）；忙则返回 BUSY。
 * 无论成功失败都会回一个 EVT_OTA_ACK(CMD_OTA_BEGIN)，App 一律停等它。 */
uint8_t ota_begin(uint32_t size, uint32_t crc, bool printer_idle);

/* 一帧 OTA_DATA：payload = seq(2, LE) + data。BLE 回调上下文调用 */
void ota_push(const uint8_t *payload, uint8_t n);

/* 数据发完，排队做校验 / 切换启动分区 / 重启 */
void ota_finish(void);

/* 断连、超时或 App 主动取消：放弃本次升级，当前运行的固件不受影响 */
void ota_abort(void);

/* 回复版本、当前 slot、最大可写镜像大小 */
void ota_send_info(void);

#endif
