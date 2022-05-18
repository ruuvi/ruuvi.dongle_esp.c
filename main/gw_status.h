/**
 * @file gw_status.h
 * @author TheSomeMan
 * @date 2022-05-05
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#ifndef RUUVI_GATEWAY_ESP_GW_STATUS_H
#define RUUVI_GATEWAY_ESP_GW_STATUS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool
gw_status_init(void);

void
gw_status_set_wifi_connected(void);

void
gw_status_clear_wifi_connected(void);

void
gw_status_set_eth_connected(void);

void
gw_status_clear_eth_connected(void);

bool
gw_status_is_network_connected(void);

void
gw_status_set_mqtt_connected(void);

void
gw_status_clear_mqtt_connected(void);

bool
gw_status_is_mqtt_connected(void);

#ifdef __cplusplus
}
#endif

#endif // RUUVI_GATEWAY_ESP_GW_STATUS_H