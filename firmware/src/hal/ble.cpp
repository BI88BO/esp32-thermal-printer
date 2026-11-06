/* ============================================================================
 * 文件：hal/ble.cpp
 * 层次：硬件抽象层 (HAL) / 无线通信
 * 职责：BLE Server / Service / Characteristic 的生命周期，以及收发的最后一公里。
 * 对应原工程：src/hal/em_ble.cpp
 *
 * 数据流：
 *   手机写 RX 特征 → onWrite() → proto_feed()（core/protocol.cpp 拼帧）
 *                  → onCommand()（app/command.cpp 分发）
 *   固件回帧       → proto_send() → transport_send() → TX 特征 notify()
 *
 * 三个容易踩坑的地方：
 *   1) onWrite 在蓝牙协议栈的回调上下文里，只能把字节喂给解析器，绝不能
 *      在这里打一行、走一次纸或擦 flash；这也是自检/OTA 都要事件化的原因。
 *   2) 断连必须彻底清场：关 VH、清队列、清暂停现场。以前只写一句
 *      jobState=0，过热暂停时 VH 会一直挂着，重连后头温降不下来。
 *   3) MTU 在连接时请求大值（BLE_MTU=247），OTA 一帧才能单片发完；
 *      请求失败时 App 侧会重试并提示，这里不做额外处理。
 *
 * 依赖：config.h（UUID / 设备名 / MTU）、core/protocol.h、core/ota.h、
 *       app/runtime.h（共享状态与 command_force_idle）
 * ========================================================================== */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "ble.h"
#include <Arduino.h>
#include "config.h"
#include "protocol.h"
#include "ota.h"
#include "runtime.h"
#include "command.h"
#include "led.h"

/* TX 特征只在本文件内使用：protocol.cpp 通过 transport_send() 间接访问 */
static BLECharacteristic *txChar = nullptr;

/* ==================== 协议栈回调 ==================== */

class ServerCB : public BLEServerCallbacks
{
    void onConnect(BLEServer *s) override
    {
        bleConnected = true;
        BLEDevice::setMTU(BLE_MTU);             // 协商大 MTU，OTA 一帧能单片发完
        led_blink(1, LED_SHORT_ON_MS, 0);       // 连接提示（对应原工程 LED_CONNECT）
        LOGLN("[BLE] connected");
    }

    void onDisconnect(BLEServer *s) override
    {
        bleConnected = false;
        ota_abort();                            // 断连即放弃升级，当前固件完好
        command_force_idle();                   // 立即关 VH + 清队列/计数/暂停现场
        LOGLN("[BLE] disconnected");
        s->startAdvertising();
    }
};

class RxCB : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *c) override
    {
        uint8_t *d = c->getData();
        size_t   n = c->getLength();
        if (d != nullptr && n > 0)
            proto_feed(d, (uint16_t)n);         // 逐字节喂帧解析状态机
    }
};

/* ==================== 对外接口 ==================== */

void ble_init(void)
{
    BLEDevice::init(DEVICE_NAME);

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new ServerCB());

    BLEService *svc = server->createService(SERVICE_UUID);

    /* RX：手机写数据。同时开 WRITE 与 WRITE_NR，兼容不同手机的写入策略 */
    BLECharacteristic *rx = svc->createCharacteristic(
        CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCB());

    /* TX：固件主动 notify。CCCD 描述符必须加，否则手机订阅不上 */
    txChar = svc->createCharacteristic(CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    txChar->addDescriptor(new BLE2902());

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();
}

/* 由 core/protocol.cpp 调用：把已经组好头/类型/长度/校验的帧发回手机。
 * 未连接或 TX 还没建好时静默丢弃，不能阻塞打印任务。 */
void transport_send(const uint8_t *buf, uint16_t len)
{
    if (!bleConnected || txChar == nullptr)
        return;
    txChar->setValue((uint8_t *)buf, len);
    txChar->notify();
}
