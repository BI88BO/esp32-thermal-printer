/* ============================================================================
 * 文件：app/runtime.cpp
 * 层次：应用层 / 全局运行状态
 * 职责：runtime.h 里所有状态量的唯一定义处（含初值）。
 *
 * 注意：这里没有任何业务逻辑，只有变量定义。
 *       想改「某个状态什么时候变」，请去 command.cpp / job.cpp，别在这里加代码。
 * ========================================================================== */

#include "runtime.h"
#include <Arduino.h>
#include "config.h"

QueueHandle_t lineQ = nullptr;

volatile uint8_t  jobState   = 0;
volatile uint16_t totalLines = 0;
volatile uint16_t doneLines  = 0;
volatile uint16_t dropLines  = 0;
volatile uint32_t lastRxMs   = 0;

uint8_t  resumeState   = 1;
uint32_t pauseSinceMs  = 0;

uint16_t userHeatUs = HEAT_US_DEFAULT;
bool     hwHeld     = false;

volatile uint8_t  pendingTest      = 0;
volatile uint16_t pendingFeed      = 0;
volatile bool     feedHeld         = false;
volatile uint32_t feedHeldSinceMs = 0;

volatile bool bleConnected   = false;
bool          firstPktLogged = false;
