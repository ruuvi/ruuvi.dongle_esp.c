/**
 * @file ruuvi_gateway.h
 * @author Jukka Saari
 * @date 2019-11-27
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#ifndef RUUVI_GATEWAY_H
#define RUUVI_GATEWAY_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdint.h>
#include "mac_addr.h"
#include "cjson_wrap.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADV_POST_INTERVAL 10000

#define WIFI_CONNECTED_BIT   (1U << 0U)
#define MQTT_CONNECTED_BIT   (1U << 1U)
#define RESET_BUTTON_BIT     (1U << 2U)
#define ETH_DISCONNECTED_BIT (1U << 3U)
#define ETH_CONNECTED_BIT    (1U << 4U)

typedef enum nrf_command_e
{
    NRF_COMMAND_SET_FILTER   = 0,
    NRF_COMMAND_CLEAR_FILTER = 1,
} nrf_command_e;

extern EventGroupHandle_t status_bits;

void
settings_clear_in_flash(void);

void
ruuvi_send_nrf_settings(const ruuvi_gateway_config_t *p_config);

void
start_services(void);

#ifdef __cplusplus
}
#endif

#endif // RUUVI_GATEWAY_H
