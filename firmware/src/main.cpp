/* ============================================================================
 * MiniPrinter 重做版固件 —— 入口与架构总览
 * 层次：入口（只做初始化，不含任何业务逻辑）
 * 对应原工程：src/main.cpp（那边也只有十几行，做初始化后把控制权交给 em_task）
 *
 * ----------------------------------------------------------------------------
 * 一、目录结构（三代划分：硬件 / 业务 / 纯逻辑，每个文件只讲一件事）
 * ----------------------------------------------------------------------------
 *   src/
 *     main.cpp            ★ 本文件：上电初始化 + 创建 3 个 FreeRTOS 任务
 *     config.h              全部编译期常量：引脚、能量、时序、阈值、版本号
 *
 *     hal/                 硬件抽象层：直接碰 GPIO / SPI / BLE / ADC
 *       — 打印链路 —
 *       printer.h/.cpp       打印头门面：作业生命周期（begin/row/end/feed）+ 拍子编排
 *       printhead.h/.cpp     打印头本体：STB 选通、SPI 送行、单组加热、质量参数寄存器
 *       vh.h/.cpp            升压电源：开关、有效电平、延迟断电、强制断电
 *       thermal_guard.h/.cpp 热保护：两级锁存（本作业熔断 / 跨作业滞回）与自动解锁
 *       motor.h/.cpp         步进电机：8 拍相序、双相整步、走纸节拍
 *       — 传感 —
 *       sensor.h/.cpp        门面：决定什么时候采什么
 *       thermistor.h/.cpp    NTC 头温：分压还原、Beta 换算、开路短路判定
 *       battery.h/.cpp       电池电压与放电曲线
 *       paper.h/.cpp         缺纸检测与去抖
 *       adc_filter.h         ADC 通用滤波（截尾均值 / 中值 / EMA，被上面两路共用）
 *       — 人机 —
 *       button.h/.cpp        按键去抖与短按/长按事件
 *       led.h/.cpp           状态灯：非阻塞闪烁状态机（原工程是 delay 硬等）
 *       ble.h/.cpp           BLE GATT 服务、连接回调、transport_send
 *
 *     app/                 应用层：任务骨架 + 业务处理
 *       runtime.h/.cpp       ★ 全局运行状态（跨任务共享的变量都在这）
 *       command.h/.cpp       BLE 命令分发 + 作业收尾/暂停/清场/状态上报
 *       — 任务（只管调度，不写业务）—
 *       job.h                打印任务入口 job_print_task()
 *       task_print.cpp       打印任务骨架：循环 + 按 jobState 分发
 *       task_button.h/.cpp   按键任务：10ms 轮询，只置事件标志
 *       task_monitor.h/.cpp  监控任务：采样节奏、补偿刷新、状态上报
 *       — 打印任务的三个分支（业务，一个分支一个文件）—
 *       job_internal.h       分支之间的约定（仅 app/ 内部使用）
 *       job_idle.cpp         空闲：自检 / 走纸 / 长按连续出纸
 *       job_overheat.cpp     过热暂停：等冷却、断点续打、放弃条件
 *       job_run.cpp          运行：取行打印、收尾判定、断流超时
 *       — 诊断页 —
 *       selftest.h           对外声明
 *       selftest/            一页一个文件，改哪页只看哪页
 *         common.h             能量标尺复位
 *         draw.h/.cpp          3×5 点阵数字、版本号+头温抬头
 *         page_full.cpp        整页自检
 *         page_dotcheck.cpp    打印头点检（最常用）
 *         page_load.cpp        全黑负载
 *         page_measure.cpp     VH 压降测量
 *         page_copy.cpp        原工程复刻
 *         page_ruler.cpp       走纸标尺
 *
 *     core/                 纯逻辑层：不碰硬件，可单独阅读
 *       protocol.h/.cpp      BLE 帧编解码（拼帧、校验、回帧）
 *       heat_model.h/.cpp    能量策略：热积累前馈、冷头补偿、n² 点补偿、统一钳位
 *       temp_comp.h/.cpp     表 4 温度→脉宽补偿（纯查表插值）
 *       crc32.h/.cpp         CRC32/IEEE 增量计算
 *       ota.h/.cpp           OTA 会话：序号、ACK 窗口、CRC 累计、任务
 *       ota_flash.h/.cpp     Update 库唯一封装：写备用分区、切 otadata、重启
 *
 *   与原工程的对应关系：
 *     config.h          ↔ em_config.h
 *     app/runtime.*     ↔ em_device.*        （全局状态集中定义）
 *     app/job + tasks   ↔ em_task.*          （任务拆分得更细）
 *     hal/*             ↔ hal/em_printer, em_motor, em_spi, em_adc, em_ble,
 *                          em_button, em_hal（LED/蜂鸣器）
 *     core/protocol.*   ↔ utils/em_queue.* + em_ble 的收发部分
 *
 * ----------------------------------------------------------------------------
 * 二、一次打印的数据流（顺着这条线读代码最快）
 * ----------------------------------------------------------------------------
 *   手机 App
 *     → hal/ble.cpp onWrite()            收到字节
 *     → core/protocol.cpp proto_feed()  拼成一帧、校验
 *     → app/command.cpp onCommand()     翻译成动作，只置状态/入队
 *     → app/runtime.cpp lineQ           48 行环形队列
 *     → app/job.cpp job_print_task()    逐行取出
 *     → hal/printer.cpp printer_job_row()   拍子编排：谁先走拍、谁后加热
 *          ├─ core/heat_model.cpp           算这一行该加热多久
 *          ├─ hal/printhead.cpp             SPI 送 48B + 6 组 STB 选通
 *          ├─ hal/thermal_guard.cpp         每次拉 STB 前查一次（熔断就断电）
 *          ├─ hal/vh.cpp                    升压开关与延迟断电
 *          └─ hal/motor.cpp                 走 4 拍（1-2 相）或 2 拍（双相）
 *     → app/command.cpp command_send_status()  每 200ms 回报 doneLines 给 App 做流控
 *
 * ----------------------------------------------------------------------------
 * 三、任务与分核
 * ----------------------------------------------------------------------------
 *   print    core1  优先级 5  打印/自检/走纸（唯一能碰打印头）
 *   ota      core1  优先级 4  擦写 flash（由 core/ota.cpp 自己创建）
 *   button   core1  优先级 3  10ms 按键轮询，只发事件
 *   monitor  core0  优先级 2  温度/电池/纸张采样、每秒刷新补偿、状态上报、
 *                             状态灯 tick、VH 延迟断电与热锁存解锁
 *   loop     core1  优先级 1  空转，什么都不做
 *
 *   BLE 回调不在上表里：它跑在协议栈上下文，只做「喂字节 / 置标志」。
 *
 * ----------------------------------------------------------------------------
 * 四、阅读建议
 * ----------------------------------------------------------------------------
 *   1. 先看 config.h 弄清所有可调参数；
 *   2. 再看 app/runtime.h 弄清有哪些全局状态；
 *   3. 按上面的数据流读一遍，就能把整台机器串起来；
 *   4. 想改打印质量时，从 core/heat_model.cpp 和 hal/printer.cpp 的
 *      group_heat_us()/heat_group() 入手，别在 command.cpp 里加补偿。
 *
 * 安全边界（机芯规格书）：50℃ 熔断 / 45℃ 恢复、VH 作业前预热
 * 1000ms / 结束立刻关断、HEAT_US_MAX 4000µs。详见 config.h 对应段落。
 * ========================================================================== */


#include <Arduino.h>
#include "config.h"
#include "runtime.h"
#include "command.h"
#include "job.h"
#include "task_button.h"
#include "task_monitor.h"
#include "printer.h"
#include "led.h"
#include "sensor.h"
#include "ble.h"
#include "ota.h"
#include "printhead.h"

/* ==================== 上电入口 ==================== */

void setup()
{
#if DEBUG_SERIAL
    Serial.begin(115200);
    delay(200);
#endif
    LOGLN("\n=== MiniPrinter v1 ===");

    /* 1) 硬件层：上电先把 VH 关断、STB 拉低、电机相位归零。
     *    顺序不能反：VH 比打印头数据先建立会误加热 */
    led_init();             // 状态灯先熄灭，别上电就常亮
    printer_begin();
    sensor_begin();

    /* 2) 运行时资源：行队列。CMD_PRINT_BEGIN / 断连都会 reset 它 */
    lineQ = xQueueCreate(LINE_QUEUE_LEN, TPH_BYTES);
    configASSERT(lineQ);

    /* 3) 逻辑层：注册命令回调 → 建 BLE 服务 → 启动 OTA 任务 */
    command_init();
    ble_init();
    ota_init();

    /* 4) 任务：打印与按键在 core1，采样在 core0，互不抢占 */
    xTaskCreatePinnedToCore(job_print_task, "print",  4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(task_button,    "button", 3072, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(task_monitor,   "monitor",3072, nullptr, 2, nullptr, 0);

    sensor_print_debug();
    LOGF("[SYS] heat %u us, line %u us, %.1f mm/s\n",
         printhead_get_heat_us(), printhead_line_us(),
         0.125f * 1000000.0f / printhead_line_us());
}

/* Arduino 的 loop() 在 FreeRTOS 下也是一个任务，这里让它彻底空转。
 * 所有实际工作都在上面创建的任务里，别往 loop() 里加延时或轮询。 */
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
