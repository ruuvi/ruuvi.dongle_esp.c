/**
 * @file mqtt.h
 * @author Jukka Saari
 * @date 2019-11-27
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#ifndef RUUVI_MQTT_H
#define RUUVI_MQTT_H

#include "ruuvi_gateway.h"

#ifdef __cplusplus
extern "C" {
#endif

void
mqtt_app_start(void);

void
mqtt_publish_table(const adv_report_table_t *p_table);

#ifdef __cplusplus
}
#endif

#endif // RUUVI_MQTT_H
