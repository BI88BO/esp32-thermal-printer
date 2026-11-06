#ifndef BLE_H
#define BLE_H

#include <Arduino.h>

/* ============================================================================
 * 文件：hal/ble.h
 * 层次：硬件抽象层 (HAL) / 无线通信
 * 职责：BLE GATT 服务的建立、连接回调、以及协议层的发送通道。
 *
 * 对外只有两件事：
 *   ble_init()       —— setup() 里调一次，建服务并开始广播
 *   transport_send() —— core/protocol.cpp 回帧时调用（声明在 protocol.h）
 *
 * 层次约束：本文件只负责「字节怎么进、怎么出」，不解析帧、不认识打印命令。
 *           帧解析在 core/protocol.cpp，命令分发在 app/command.cpp。
 * ========================================================================== */

void ble_init(void);

#endif
