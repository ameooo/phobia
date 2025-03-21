#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "hal/hal.h"

#include "libc.h"
#include "main.h"
#include "regfile.h"

/* The application allows you to startup PMC automatically at power up.
 * */

LD_TASK void app_AUTOSTART(void *pData)
{
	volatile int		*lknob = (volatile int *) pData;

	if (xTaskGetTickCount() >= (TickType_t) 1000) {

		/* If the application task was started much later than power up
		 * we will pause to give you time for flash programming.
		 * */
		vTaskDelay((TickType_t) 5000);
	}

	do {
		vTaskDelay((TickType_t) 100);

		if (		pm.lu_MODE == PM_LU_DISABLED
				&& pm.const_fb_U > pm.watt_uDC_minimal) {

			pm.fsm_req = PM_STATE_LU_STARTUP;

			vTaskDelay((TickType_t) 10);

			if (ap.auto_reg_ID != ID_NULL) {

				reg_SET_F(ap.auto_reg_ID, ap.auto_reg_DATA);
			}
		}
	}
	while (*lknob == PM_ENABLED);

	vTaskDelete(NULL);
}

