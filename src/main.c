// SPDX-License-Identifier: GPL-3.0-only
#include <stdio.h>

#include "board.h"
#include "bflb_mtd.h"
#include "easyflash.h"
#include "rfparam_adapter.h"
#include "bl616_glb.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board_config.h"
#include "board_status.h"
#include "debug_log.h"
#include "pro2_ble.h"
#include "pro2_rumble.h"
#include "pro2_settings.h"
#include "usb_nintendo.h"

static void startup_task(void *arg)
{
    (void)arg;
    pro2_rumble_init();
    pro2_ble_start();

    /* BL616 Bluetooth controller initialization reconfigures peripheral
       clocks. Start USB only after BT is ready, then restore its clock just
       like the working DS5 dongle implementation. */
    while (!pro2_ble_stack_ready()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    GLB_Set_USB_CLK_From_WIFIPLL(1);
    usb_nintendo_init();
    LOG_INF("[BOOT] BLE ready; USB clock restored and device started\r\n");
    vTaskDelete(NULL);
}

int main(void)
{
    board_init();
    printf("\r\n=== Pro2 Nintendo Bridge / BL616 ===\r\n");
    printf("Board: %s, USB VID:PID 057e:2069\r\n", BOARD_NAME);

    /* ble1m10s1bredr0 reserves 32 KiB exchange memory on BL616. */
    GLB_Set_EM_Sel(GLB_WRAM128KB_EM32KB);

    bflb_mtd_init();
    easyflash_init();
    pro2_settings_init();
    if (rfparam_init(0, NULL, 0) != 0) {
        LOG_ERR("[BOOT] RF parameter init failed\r\n");
    }

    board_status_init();
    board_status_set(BOARD_STATUS_BOOT);
    xTaskCreate(board_status_task, "status", 512, NULL,
                configMAX_PRIORITIES - 4, NULL);
    xTaskCreate(startup_task, "startup", 2048, NULL,
                configMAX_PRIORITIES - 1, NULL);

    vTaskStartScheduler();
    for (;;) {}
    return 0;
}

void vApplicationIdleHook(void)
{
    __asm volatile("wfi");
}

void vAssertCalled(void)
{
    LOG_ERR("FreeRTOS assertion\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    LOG_ERR("FreeRTOS allocation failed\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    LOG_ERR("FreeRTOS stack overflow: %s\r\n", name ? name : "?");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
