/*
 * FreeRTOSConfig.h  ——  USB / 蓝牙双模键鼠工程
 * 平台：STM32F103RCT6（48KB RAM / 256KB Flash）+ Keil MDK + ARM Compiler 6
 * 内核：FreeRTOS Kernel V11.3.1（取自 FreeRTOS-LTS，port 用 portable/GCC/ARM_CM3）
 *
 * 移植要点：
 *   1. 本文件放在 User 目录下，已在 Keil 工程 IncludePath 中包含；
 *   2. port.c（GCC/ARM_CM3）用宏把 vPortSVCHandler / xPortPendSVHandler /
 *      xPortSysTickHandler 改名对接启动文件里的向量名，因此
 *      User/stm32f10x_it.c 里这三个函数必须删除，否则重复定义；
 *   3. 会调用 FreeRTOS API 的中断，其优先级数值必须 >= 5
 *      （configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY），
 *      且 NVIC 优先级分组必须是 NVIC_PriorityGroup_4（4 位抢占、无子优先级）；
 *   4. 本工程里 USB(2) / TIM4(3) / USART2(蓝牙 HCI,3) 这几个中断优先级都高于 5，
 *      它们【不调用】任何 FreeRTOS API（只在中断里收包、置标志），
 *      所以不需要改优先级 —— 这样 USB 枚举和蓝牙 HCI 的实时性不受调度器影响。
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "stm32f10x.h"      /* 提供 SystemCoreClock 等定义 */

/*---------------------- 调度器基本配置 ----------------------*/
#define configUSE_PREEMPTION                       1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    1       /* Cortex-M3 用 CLZ 指令选任务 */
#define configUSE_TICKLESS_IDLE                    0
#define configCPU_CLOCK_HZ                         (SystemCoreClock)   /* 72MHz */
#define configTICK_RATE_HZ                         ((TickType_t)1000)  /* 1ms 一个 tick */
#define configMAX_PRIORITIES                       (8)       /* 本工程只需要几档优先级 */
#define configMINIMAL_STACK_SIZE                   ((unsigned short)128)   /* 空闲任务栈：128 字 = 512 字节 */
#define configMAX_TASK_NAME_LEN                    (16)
#define configIDLE_SHOULD_YIELD                    1
#define configUSE_TIME_SLICING                     1
#define configUSE_TASK_NOTIFICATIONS               1

/*---------------------- 内存与队列 ----------------------*/
/* RCT6 有 48KB RAM：本工程静态数据约 9KB，堆给 10KB 仍余 25KB 以上 */
#define configTOTAL_HEAP_SIZE                      ((size_t)(10 * 1024))
#define configSUPPORT_STATIC_ALLOCATION            0
#define configSUPPORT_DYNAMIC_ALLOCATION           1
#define configQUEUE_REGISTRY_SIZE                  0
#define configUSE_QUEUE_SETS                       0
#define configUSE_MUTEXES                          1
#define configUSE_RECURSIVE_MUTEXES                1
#define configUSE_COUNTING_SEMAPHORES              1
#define configUSE_NEWLIB_REENTRANT                 0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS    0
#define configENABLE_BACKWARD_COMPATIBILITY        1

/* tick 计数位宽（V11 写法，等价于旧的 configUSE_16_BIT_TICKS 0） */
#define configTICK_TYPE_WIDTH_IN_BITS              TICK_TYPE_WIDTH_32_BITS

/*---------------------- 钩子函数 ----------------------*/
#define configUSE_IDLE_HOOK                        0
#define configUSE_TICK_HOOK                        0
#define configUSE_MALLOC_FAILED_HOOK               1       /* 堆不足进 vApplicationMallocFailedHook */
#define configCHECK_FOR_STACK_OVERFLOW             2       /* 栈溢出检查（需要 vApplicationStackOverflowHook） */
#define configUSE_DAEMON_TASK_STARTUP_HOOK         0

/*---------------------- 运行时统计（本工程不用，省 RAM/Flash）----------------------*/
#define configGENERATE_RUN_TIME_STATS              0
#define configUSE_TRACE_FACILITY                   0
#define configUSE_STATS_FORMATTING_FUNCTIONS       0

/*---------------------- 软件定时器（本工程不用）----------------------*/
#define configUSE_TIMERS                           0
#define configTIMER_TASK_PRIORITY                  (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                   10
#define configTIMER_TASK_STACK_DEPTH               (configMINIMAL_STACK_SIZE * 2)

/*---------------------- 中断优先级（Cortex-M3，4 位）----------------------*/
#define configPRIO_BITS                            4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

/* 内核（PendSV/SysTick）用最低优先级 */
#define configKERNEL_INTERRUPT_PRIORITY            (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
/* 允许调用 FreeRTOS API 的中断最高优先级 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY       (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/*---------------------- 断言 ----------------------*/
#define configASSERT(x)                                                            \
    if ((x) == 0) {                                                                \
        taskDISABLE_INTERRUPTS();                                                  \
        for (;;) {                                                                 \
        }                                                                          \
    }

/*---------------------- 中断向量对接（port.c 中的实现改名）----------------------*/
#define vPortSVCHandler                            SVC_Handler
#define xPortPendSVHandler                         PendSV_Handler
#define xPortSysTickHandler                        SysTick_Handler

/*---------------------- 按需裁剪的 API ----------------------*/
#define INCLUDE_vTaskPrioritySet                   1
#define INCLUDE_uxTaskPriorityGet                  1
#define INCLUDE_vTaskDelete                        1
#define INCLUDE_vTaskSuspend                       1
#define INCLUDE_vTaskDelayUntil                    1
#define INCLUDE_vTaskDelay                         1
#define INCLUDE_xTaskGetSchedulerState             1
#define INCLUDE_xTaskGetCurrentTaskHandle          1
#define INCLUDE_uxTaskGetStackHighWaterMark        1
#define INCLUDE_xTaskGetIdleTaskHandle             1
#define INCLUDE_eTaskGetState                      1
#define INCLUDE_xTaskGetHandle                     1

#endif /* FREERTOS_CONFIG_H */
