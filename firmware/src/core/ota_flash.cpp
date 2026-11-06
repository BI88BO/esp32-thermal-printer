/* ============================================================================
 * 文件：core/ota_flash.cpp
 * 层次：纯逻辑层 / 固件镜像写入
 * 职责：Arduino Update 库的唯一封装，外加分区信息查询。
 * 对应原工程：无
 * 依赖：Update.h、esp_ota_ops.h、esp_partition.h、config.h
 * ========================================================================== */

#include "ota_flash.h"
#include <Arduino.h>
#include "config.h"

#include <Esp.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

/* Update.begin 是否已成功。写完或 abort 后清零 */
static bool s_started = false;

uint32_t ota_flash_next_addr(void)
{
    const esp_partition_t *n = esp_ota_get_next_update_partition(nullptr);
    return (n != nullptr) ? n->address : 0;
}

uint32_t ota_flash_next_size(void)
{
    const esp_partition_t *n = esp_ota_get_next_update_partition(nullptr);
    return (n != nullptr) ? n->size : 0;
}

uint8_t ota_flash_running_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == nullptr)
        return 2;
    if (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
        return 0;
    if (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
        return 1;
    return 2;
}

bool ota_flash_begin(uint32_t size)
{
    s_started = Update.begin(size, U_FLASH);
    return s_started;
}

bool ota_flash_write(const uint8_t *p, uint16_t n)
{
    if (!s_started)
        return false;
    size_t w = Update.write((uint8_t *)p, n);
    return w == n;
}

bool ota_flash_end(void)
{
    if (!s_started)
        return false;
    bool ok = Update.end(true);     // true = 更新 otadata，切启动分区
    s_started = false;
    return ok;
}

void ota_flash_abort(void)
{
    if (s_started)
        Update.abort();
    s_started = false;
}

bool ota_flash_started(void)
{
    return s_started;
}

int ota_flash_last_error(void)
{
    return (int)Update.getError();
}

void ota_flash_confirm_running(void)
{
    /* 新固件起来后，如果 bootloader 开了 rollback 且镜像仍是 PENDING_VERIFY，
     * 要在这里标记有效，否则下一次重启会被自动回滚到旧版本。
     * 默认 bootloader 没开这项，接口返回不支持，忽略即可。 */
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == nullptr)
        return;

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
    {
        esp_ota_mark_app_valid_cancel_rollback();
        LOGLN("[OTA] new image marked valid");
    }
}

void ota_flash_reboot(void)
{
    /* 留一点时间让 notify 真正发出去，App 才知道升级成功了 */
    delay(350);
    ESP.restart();
}
