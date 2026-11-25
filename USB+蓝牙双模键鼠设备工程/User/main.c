#include <stdint.h>

/* ---- FreeRTOS ---- */
#include <FreeRTOS.h>
#include <task.h>

#include <uart.h>
#include <key.h>
#include <joystick.h>
#include <tim2.h>
#include <usb_hid.h>
#include <bt_uart.h>
#include <Init_rda5876.h>
#include <bt_hci.h>

#define UART_BAUD_RATE      921600U

/* ============================================================================
 * 任务划分
 * ----------------------------------------------------------------------------
 * 当前版本采用"USB 优先、蓝牙持续在线"的双模调度，两条链路独立推进：
 *
 *   USB_TASK  优先级 3、栈 2KB、1ms 周期：
 *             推进 TinyUSB、键盘/鼠标/自定义 HID 扫描与上报；
 *             USB 未挂载时也持续运行，负责清理输入状态和执行模式管理。
 *   BT_TASK   优先级 3、栈 2KB、1ms 周期：
 *             蓝牙初始化成功后始终调用 BT_Task()，持续处理 HCI/L2CAP 事件。
 *             USB 输出时仅暂停蓝牙键鼠输入并发送空报告保活，不停止蓝牙任务。
 *   DIAG_TASK 优先级 1、栈 640B、5s 周期：打印 USB/蓝牙状态与堆余量。
 *
 * 蓝牙 HCI 串口和 USB 由中断完成数据收发，任务负责协议栈与状态推进；
 * 输入扫描、报告发送和彩虹动画周期由各模块内部的软件计时器分别控制。
 * ========================================================================== */

#define USB_TASK_PRIO       3u
#define USB_TASK_STACK      512u        /* 512 字 = 2KB */
#define BT_TASK_PRIO        3u
#define BT_TASK_STACK       512u        /* 512 字 = 2KB（HCI/L2CAP/SDP 调用链较深） */
#define DIAG_TASK_PRIO      1u
#define DIAG_TASK_STACK     160u        /* 160 字 = 640B */
#define DIAG_PERIOD_MS      5000u
#define TASK_PERIOD_MS      1u

/* 蓝牙是否可用（初始化成功后置 1，之后不再变化） */
static uint8_t s_btReady = 0U;

/* USB 任务：推进 USB 协议栈 + 输入扫描上报，必须每 1ms 跑一次 */
static void vUsbTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        UsbHid_Task();
        Joystick_AutoRecenter();        /* 静止时缓慢修正摇杆中位，消除光标自己漂移 */
        vTaskDelay(TASK_PERIOD_MS);
    }
}

/* 蓝牙任务：初始化成功后持续推进蓝牙协议栈、连接维护与双模保活 */
static void vBtTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        /* BT_Task() 同时负责输入报告和 HCI 事件处理，必须持续运行。
         * 输出链路由其内部 mux 逻辑选择：USB 模式仅暂停蓝牙键鼠输入，
         * 并通过空键盘报告保持连接，不重复发送标准键鼠动作。 */
        if (s_btReady != 0U)
        {
            BT_Task();
        }
        vTaskDelay(TASK_PERIOD_MS);
    }
}

/* 诊断任务：周期打印双模状态，便于排查 */
static void vDiagTask(void *pvParameters)
{
    TickType_t lastWake = xTaskGetTickCount();

    (void)pvParameters;

    for (;;)
    {
        UART1_Printf("[rtos] usb=%u pref=%u out=%u btState=%u reports=%lu joyLX=%d joyLY=%d heap=%u\r\n",
                     (unsigned)UsbHid_IsMounted(),
                     (unsigned)UsbHid_IsBtPreferred(),
                     (unsigned)UsbHid_IsBtForced(),
                     (unsigned)g_bt_state,
                     (unsigned long)g_bt_reports,
                     (int)Joystick_MapRawValue(JOYSTICK_LX, Joystick_ReadAxis(JOYSTICK_LX)),
                     (int)Joystick_MapRawValue(JOYSTICK_LY, Joystick_ReadAxis(JOYSTICK_LY)),
                     (unsigned)xPortGetFreeHeapSize());

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DIAG_PERIOD_MS));
    }
}

/* ============================================================================
 * FreeRTOS 钩子
 * ========================================================================== */

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    UART1_SendString("\r\n[malloc FAILED] heap exhausted!\r\n");
    for (;;)
    {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    UART1_Printf("\r\n[stack OVERFLOW] task=%s\r\n", pcTaskName);
    for (;;)
    {
    }
}

/* ============================================================================
 * main：硬件初始化（阻塞，调度器启动之前）→ 创建任务 → 启动调度器
 * ========================================================================== */

int main(void)
{
    uint32_t btStart;

    /* NVIC 分组必须先设成 Group 4（4 位抢占、无子优先级），
     * FreeRTOS 的 configMAX_SYSCALL_INTERRUPT_PRIORITY 屏蔽关系依赖它。 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    UsbHid_PreInit();               /* 初始化完成前不让 USB 主机开始枚举 */
    UART1_Init(UART_BAUD_RATE);
    UsbHid_TimeBaseInit();          /* USB 与蓝牙共用的 TIM4 1ms 时基 */

    Key_Init();
    Joystick_Init();                /* 蓝牙上电前记录摇杆中点，与手柄工程一致 */
    Tim2_Init();
    Key_FSM_Init();

    UART1_SendString("\r\n[dual] USB / Bluetooth keyboard and mouse (FreeRTOS)\r\n");

    btStart = hal_time_ms();
    if (Init_rda5876() == RDA_OK)
    {
        UART1_Printf("[bt] controller ready, init=%lu ms\r\n",
                     (unsigned long)(hal_time_ms() - btStart));
        if (BT_Probe() != 0U)
        {
            BT_Init();
            s_btReady = (g_bt_error == 0U) ? 1U : 0U;
        }
    }
    if (s_btReady == 0U)
    {
        g_bt_error = 1U;
        UART1_SendString("[bt] unavailable; USB mode remains active\r\n");
    }

    UsbHid_Init();
    UART1_SendString("[mux] USB priority after enumeration; otherwise Bluetooth\r\n");
    UART1_SendString("[key] A/B/LF=mouse L/R/M, X=CapsLock, Y=NumLock\r\n");
    UART1_SendString("[key] UP=Space, DN=Enter, left stick=move, right stick=wheel\r\n");
    UART1_SendString("[key] long-press RG 2s: switch AUTO(USB priority) / FORCE Bluetooth\r\n");

    /* 创建任务并启动调度器 */
    UART1_Printf("[rtos] heap free before tasks = %u bytes\r\n",
                 (unsigned)xPortGetFreeHeapSize());

    (void)xTaskCreate(vUsbTask,  "USB",  USB_TASK_STACK,  NULL, USB_TASK_PRIO,  NULL);
    (void)xTaskCreate(vBtTask,   "BT",   BT_TASK_STACK,   NULL, BT_TASK_PRIO,   NULL);
    (void)xTaskCreate(vDiagTask, "DIAG", DIAG_TASK_STACK, NULL, DIAG_TASK_PRIO, NULL);

    UART1_Printf("[rtos] heap free after tasks  = %u bytes\r\n",
                 (unsigned)xPortGetFreeHeapSize());
    UART1_SendString("[rtos] starting scheduler\r\n");

    vTaskStartScheduler();          /* 正常情况下不会返回 */

    UART1_SendString("[rtos] scheduler start FAILED\r\n");
    for (;;)
    {
    }
}
