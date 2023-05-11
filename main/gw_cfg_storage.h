/**
 * @file gw_cfg_storage.h
 * @author TheSomeMan
 * @date 2023-05-06
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#ifndef RUUVI_GATEWAY_ESP_GW_CFG_STORAGE_H
#define RUUVI_GATEWAY_ESP_GW_CFG_STORAGE_H

#include <stdbool.h>
#include "str_buf.h"

#define GW_CFG_STORAGE_MAX_FILE_NAME_LEN (15U)

#define GW_CFG_STORAGE_GW_CFG_DEFAULT "gw_cfg_default"
_Static_assert(
    sizeof(GW_CFG_STORAGE_GW_CFG_DEFAULT) <= (GW_CFG_STORAGE_MAX_FILE_NAME_LEN + 1),
    "sizeof(GW_CFG_STORAGE_GW_CFG_DEFAULT)");

#ifdef __cplusplus
extern "C" {
#endif

bool
gw_cfg_storage_check_file(const char* const p_file_name);

str_buf_t
gw_cfg_storage_read_file(const char* const p_file_name);

bool
gw_cfg_storage_write_file(const char* const p_file_name, const char* const p_content);

bool
gw_cfg_storage_delete_file(const char* const p_file_name);

void
gw_cfg_storage_deinit_erase_init(void);

#ifdef __cplusplus
}
#endif

#endif // RUUVI_GATEWAY_ESP_GW_CFG_STORAGE_H
