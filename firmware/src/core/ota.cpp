/* ============================================================================
 * 文件：core/ota.cpp
 * 层次：纯逻辑层 / BLE 固件升级会话
 * 职责：升级会话状态机、分块序号与 ACK 窗口、CRC 累计、任务调度。
 *        真正写 flash 的动作在 core/ota_flash.cpp（Update 库的唯一封装），
 *        校验算法在 core/crc32.cpp。
 *
 * 传输：一块 = seq(2B) + 243B 数据；App 每 16 块停等一次 ACK。
 *   固件只对连续序号入队，队列排空 + 静默 40ms 后回的 ACK 里带真实
 *   next_seq，App 从缺口补发。丢中间块 / 丢最后一块都是同一条路径，
 *   不需要单独的 NACK 分支。
 *
 * 线程模型（关键）：
 *   ota_push() 跑在 BLE 回调里，只做序号校验 + 入队，绝不碰 flash；
 *   擦写由 otaTask（core1）独占执行，避免卡住协议栈。
 *
 * 安全：唯一会动 otadata 的地方是 ota_flash_end()，且必须先过 size 和
 *   CRC 两关。失败、15 秒无新块、蓝牙断连都只调 ota_flash_abort()。
 *   OTA 期间 app/command.cpp 冻结全部打印活动，VH 保持关断。
 * ========================================================================== */

#include "ota.h"
#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "protocol.h"
#include "crc32.h"
#include "ota_flash.h"

/* 队列里的控制消息用保留 seq，App 不会发这些值 */
#define OTA_SEQ_START   0xFFFC
#define OTA_SEQ_ABORT   0xFFFD
#define OTA_SEQ_END     0xFFFE

struct OtaMsg
{
    uint16_t seq;
    uint16_t len;
    uint8_t  data[OTA_BLOCK_DATA];
};

static QueueHandle_t s_q = nullptr;

/* BLE 回调 / printTask 会读这几个，必须 volatile */
static volatile bool     s_active   = false;   // 升级会话进行中
static volatile uint16_t s_expect   = 0;       // 下一个该收的块号
static volatile uint32_t s_last_rx  = 0;       // 最近一次收到块的时间
static volatile bool     s_abortReq = false;

/* 下面这些只在 otaTask 里读写，不需要 volatile */
static uint32_t s_size       = 0;
static uint32_t s_crcExpect  = 0;
static uint32_t s_written    = 0;       // 已成功写入 flash 的字节数
static uint32_t s_crcWritten = CRC32_INIT;
static uint16_t s_ackSeq     = 0;       // 上一次 ACK 报给 App 的 next_seq
static uint8_t  s_err        = OTA_OK;

/* ==================== 内部工具 ==================== */

/* ACK 里带上 next_seq 和已写字节数：App 据此补发缺口、更新进度条 */
static void send_ack(uint8_t cmd, uint8_t code)
{
    uint8_t p[8];
    p[0] = cmd;
    p[1] = code;
    p[2] = (uint8_t)(s_expect & 0xFF);
    p[3] = (uint8_t)(s_expect >> 8);
    p[4] = (uint8_t)(s_written & 0xFF);
    p[5] = (uint8_t)((s_written >> 8) & 0xFF);
    p[6] = (uint8_t)((s_written >> 16) & 0xFF);
    p[7] = (uint8_t)((s_written >> 24) & 0xFF);
    proto_send(EVT_OTA_ACK, p, sizeof(p));
}

/* 放弃会话：ota_flash_abort() 只重置写状态，不会改 otadata，
 * 所以当前正在运行的固件完好无损，下次开机还是它。 */
static void cleanup(void)
{
    ota_flash_abort();
    s_active   = false;
    s_abortReq = false;
    s_err      = OTA_OK;
    s_size     = 0;
    s_written  = 0;
    s_expect   = 0;
    s_ackSeq   = 0;
    if (s_q)
        xQueueReset(s_q);
}

/* ==================== 对外接口 ==================== */

bool ota_active(void) { return s_active; }

static uint8_t begin_inner(uint32_t size, uint32_t crc, bool printer_idle)
{
    if (s_active || !printer_idle)
        return OTA_ERR_BUSY;

    uint32_t cap = ota_flash_next_size();
    if (cap == 0)
        return OTA_ERR_SPACE;
    if (size < 4096 || size > cap)
        return OTA_ERR_SIZE;

    if (s_q)
        xQueueReset(s_q);

    s_size       = size;
    s_crcExpect  = crc;
    s_written    = 0;
    s_crcWritten = CRC32_INIT;
    s_expect     = 0;
    s_ackSeq     = 0;
    s_err        = OTA_OK;
    s_abortReq   = false;
    s_active     = true;
    s_last_rx    = millis();

    /* 连 begin 也排队交给 otaTask：擦 flash 不能发生在 BLE 回调里 */
    OtaMsg m;
    m.seq = OTA_SEQ_START;
    m.len = 0;
    if (xQueueSend(s_q, &m, 0) != pdTRUE)
    {
        cleanup();
        return OTA_ERR_QUEUE;
    }
    LOGF("[OTA] begin size=%u crc=%08X -> 0x%06x (%u B)\n",
         size, crc, ota_flash_next_addr(), cap);
    return OTA_OK;
}

uint8_t ota_begin(uint32_t size, uint32_t crc, bool printer_idle)
{
    uint8_t c = begin_inner(size, crc, printer_idle);
    /* 失败时没有 START 控制消息进队列，otaTask 不会回 ACK，这里直接回，
     * 免得 App 一直停等。成功时由 otaTask 在 Update.begin 之后回。 */
    if (c != OTA_OK)
    {
        send_ack(CMD_OTA_BEGIN, c);
        LOGF("[OTA] begin rejected, code=%u\n", c);
    }
    return c;
}

void ota_push(const uint8_t *p, uint8_t n)
{
    if (!s_active)
    {
        send_ack(CMD_OTA_DATA, OTA_ERR_STATE);
        return;
    }
    if (n < 3 || n > OTA_BLOCK_DATA + 2)
        return;

    uint16_t seq = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    uint16_t len = (uint16_t)(n - 2);

    /* 保留段只允许固件内部用 */
    if (seq >= OTA_SEQ_START)
        return;

    s_last_rx = millis();

    /* 序号不连续就静默丢掉这一块，不推进 s_expect。
     * 队列排空后的 ACK 会带上真实的 next_seq，App 从缺口补发即可——
     * 这样丢中间一块、丢最后一块、连丢几块都不需要额外的 NACK 分支。 */
    if (seq != s_expect)
    {
        LOGF("[OTA] skip seq %u, expect %u\n", seq, s_expect);
        return;
    }

    OtaMsg m;
    m.seq = seq;
    m.len = len;
    memcpy(m.data, p + 2, len);

    if (xQueueSend(s_q, &m, 0) != pdTRUE)
    {
        /* 队列满：不推进 s_expect，App 等到的 ACK 会停在上一块，自动重发 */
        s_err = OTA_ERR_QUEUE;
        return;
    }

    s_expect++;
}

void ota_finish(void)
{
    if (!s_active)
    {
        send_ack(CMD_OTA_END, OTA_ERR_STATE);
        return;
    }
    OtaMsg m;
    m.seq = OTA_SEQ_END;
    m.len = 0;
    xQueueSend(s_q, &m, 0);
}

void ota_abort(void)
{
    if (!s_active)
        return;
    s_abortReq = true;
    s_last_rx  = millis();
}

void ota_send_info(void)
{
    uint32_t maxSize = ota_flash_next_size();

    uint8_t p[11];
    p[0] = FW_BUILD_CODE;
    p[1] = ota_flash_running_slot();
    p[2] = (uint8_t)(maxSize & 0xFF);
    p[3] = (uint8_t)((maxSize >> 8) & 0xFF);
    p[4] = (uint8_t)((maxSize >> 16) & 0xFF);
    p[5] = (uint8_t)((maxSize >> 24) & 0xFF);
    p[6] = s_active ? 1 : 0;
    p[7] = (uint8_t)(s_written & 0xFF);
    p[8] = (uint8_t)((s_written >> 8) & 0xFF);
    p[9] = (uint8_t)((s_written >> 16) & 0xFF);
    p[10] = (uint8_t)((s_written >> 24) & 0xFF);
    proto_send(EVT_OTA_INFO, p, sizeof(p));
}

/* ==================== 收尾与重启 ==================== */

static void do_finish(void)
{
    uint8_t code = s_err;

    if (code == OTA_OK && !ota_flash_started())
        code = OTA_ERR_STATE;
    if (code == OTA_OK && s_written != s_size)
        code = OTA_ERR_INCOMPLETE;
    if (code == OTA_OK && crc32_final(s_crcWritten) != s_crcExpect)
        code = OTA_ERR_CRC;
    if (code == OTA_OK && !ota_flash_end())
    {
        LOGF("[OTA] Update.end failed, error=%d\n", ota_flash_last_error());
        code = OTA_ERR_WRITE;
    }

    send_ack(CMD_OTA_END, code);

    if (code != OTA_OK)
    {
        LOGF("[OTA] FAILED code=%u, written=%u/%u, current firmware untouched\n",
             code, s_written, s_size);
        cleanup();
        return;
    }

    LOGF("[OTA] OK %u B, crc %08X, reboot into other slot\n",
         s_written, s_crcExpect);
    ota_flash_reboot();     // 不返回
}

static void otaTask(void *)
{
    OtaMsg m;

    for (;;)
    {
        if (s_abortReq)
        {
            LOGLN("[OTA] aborted");
            cleanup();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!s_active)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            if (m.seq == OTA_SEQ_START)
            {
                if (!ota_flash_begin(s_size))
                {
                    LOGF("[OTA] Update.begin failed, error=%d\n", ota_flash_last_error());
                    s_err = OTA_ERR_SPACE;
                }
                send_ack(CMD_OTA_BEGIN, s_err);
                if (s_err != OTA_OK)
                    cleanup();
                continue;
            }
            if (m.seq == OTA_SEQ_END)
            {
                do_finish();     // 成功则不返回（重启）
                continue;
            }
            if (m.seq == OTA_SEQ_ABORT)
            {
                cleanup();
                continue;
            }

            if (!ota_flash_started())
            {
                s_err = OTA_ERR_STATE;
                continue;
            }
            if (s_written + m.len > s_size)
            {
                s_err = OTA_ERR_SIZE;
                continue;
            }

            if (!ota_flash_write(m.data, m.len))
            {
                LOGF("[OTA] write failed at seq %u, error=%d\n",
                     m.seq, ota_flash_last_error());
                s_err = OTA_ERR_WRITE;
                continue;
            }
            s_written    += m.len;
            s_crcWritten  = crc32_update(s_crcWritten, m.data, m.len);
            continue;
        }

        /* 队列排空 + 链路静默一小会儿：回一次进度 ACK。
         * App 是停等发送，所以只有它发完一批才会出现静默窗口；
         * 丢块时 next_seq 会停在实际缺口，App 据此补发，不需要 NACK。 */
        uint32_t now = millis();
        if (ota_flash_started() && s_expect != s_ackSeq &&
            (now - s_last_rx) > OTA_ACK_IDLE_MS)
        {
            s_ackSeq = s_expect;
            send_ack(CMD_OTA_DATA, s_err);
        }

        /* 长时间没有任何新块：别把半截会话永远挂着 */
        if ((now - s_last_rx) > OTA_TIMEOUT_MS)
        {
            LOGLN("[OTA] timeout, abort");
            cleanup();
        }
    }
}

void ota_init(void)
{
    s_q = xQueueCreate(OTA_QUEUE_LEN, sizeof(OtaMsg));
    configASSERT(s_q);
    ota_flash_confirm_running();
    /* core1：写 flash 时尽量别和 core0 的 BLE / 温度采样抢同一核 */
    xTaskCreatePinnedToCore(otaTask, "ota", 4096, nullptr, 4, nullptr, 1);
}
