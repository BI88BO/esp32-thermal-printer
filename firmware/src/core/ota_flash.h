/* ============================================================================
 * 文件：core/ota_flash.h
 * 层次：纯逻辑层 / 固件镜像写入（Arduino Update 库的唯一封装）
 * 职责：把"写另一个 app 分区"这件事包起来，并且只暴露必要的动作。
 *        分区表是默认 4MB 双 OTA：otadata + app0(1.25MB) + app1(1.25MB)。
 *
 * 安全边界（很重要）：
 *   - 唯一会改 otadata 的地方是 ota_flash_end() 里的 Update.end(true)，
 *     调用前必须由 core/ota.cpp 验完 size 和 CRC。
 *   - 失败 / 超时 / 断连只能调 ota_flash_abort()，它执行 Update.abort()，
 *     **不动 otadata**，板子里继续跑原来那版固件。
 *
 * 依赖：Update / esp_ota_ops / esp_partition
 * 被谁用：core/ota.cpp（会话与任务）
 * ========================================================================== */
#ifndef OTA_FLASH_H
#define OTA_FLASH_H

#include <Arduino.h>

/* 目标分区（不是当前运行的那个）的起始地址与容量；取不到返回 0 */
uint32_t ota_flash_next_addr(void);
uint32_t ota_flash_next_size(void);

/* 当前运行的 slot：0=app0, 1=app1, 2=未知。用于 OTA_INFO 上报 */
uint8_t ota_flash_running_slot(void);

/* 开始写入。内部 Update.begin(size, U_FLASH)，成功返回 true */
bool ota_flash_begin(uint32_t size);

/* 写一块。返回是否整块写成功（部分写入也算失败） */
bool ota_flash_write(const uint8_t *p, uint16_t n);

/* 收尾：Update.end(true) 才切 otadata。返回 false 表示写入不完整/校验失败 */
bool ota_flash_end(void);

/* 放弃：Update.abort()，otadata 不变，当前固件完好 */
void ota_flash_abort(void);

/* 是否已成功 Update.begin（会话层用它判断"能不能接收数据块"） */
bool ota_flash_started(void);

/* 最近一次 Update 操作的错误码，仅用于日志 */
int ota_flash_last_error(void);

/* 新镜像起来后标记有效（只有 bootloader 开了 rollback 才有意义，
 * 默认没开，接口返回不支持，忽略即可） */
void ota_flash_confirm_running(void);

/* 重启进入新分区。会先 delay(350) 让 BLE 的 ACK notify 真正发出去 */
void ota_flash_reboot(void);

#endif
