/**
 * @file settings.c
 * @author Jukka Saari
 * @date 2019-11-27
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "settings.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ruuvi_gateway.h"
#include "cjson_wrap.h"
#include "gw_cfg_default.h"
#include "log.h"

#define RUUVI_GATEWAY_NVS_NAMESPACE         "ruuvi_gateway"
#define RUUVI_GATEWAY_NVS_CONFIGURATION_KEY "ruuvi_config"
#define RUUVI_GATEWAY_NVS_MAC_ADDR_KEY      "ruuvi_mac_addr"

#define RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_KEY   "ruuvi_auto_udp"
#define RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_VALUE (0xAACC5533U)

static const char TAG[] = "settings";

static bool
settings_nvs_open(nvs_open_mode_t open_mode, nvs_handle_t *p_handle)
{
    const char *nvs_name = RUUVI_GATEWAY_NVS_NAMESPACE;
    esp_err_t   err      = nvs_open(nvs_name, open_mode, p_handle);
    if (ESP_OK != err)
    {
        if (ESP_ERR_NVS_NOT_INITIALIZED == err)
        {
            LOG_WARN("NVS namespace '%s': StorageState is INVALID, need to erase NVS", nvs_name);
            return false;
        }
        else if (ESP_ERR_NVS_NOT_FOUND == err)
        {
            LOG_WARN("NVS namespace '%s' doesn't exist and mode is NVS_READONLY, try to create it", nvs_name);
            if (!settings_clear_in_flash())
            {
                LOG_ERR("Failed to create NVS namespace '%s'", nvs_name);
                return false;
            }
            LOG_INFO("NVS namespace '%s' created successfully", nvs_name);
            err = nvs_open(nvs_name, open_mode, p_handle);
            if (ESP_OK != err)
            {
                LOG_ERR_ESP(err, "Can't open NVS namespace: '%s'", nvs_name);
                return false;
            }
        }
        else
        {
            LOG_ERR_ESP(err, "Can't open NVS namespace: '%s'", nvs_name);
            return false;
        }
    }
    return true;
}

static bool
settings_nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    const esp_err_t esp_err = nvs_set_blob(handle, key, value, length);
    if (ESP_OK != esp_err)
    {
        LOG_ERR_ESP(esp_err, "Can't save config to NVS");
        return false;
    }
    return true;
}

bool
settings_check_in_flash(void)
{
    nvs_handle handle = 0;
    if (!settings_nvs_open(NVS_READONLY, &handle))
    {
        return false;
    }
    nvs_close(handle);
    return true;
}

bool
settings_clear_in_flash(void)
{
    LOG_DBG(".");
    nvs_handle handle = 0;
    if (!settings_nvs_open(NVS_READWRITE, &handle))
    {
        return false;
    }
    if (!settings_nvs_set_blob(
            handle,
            RUUVI_GATEWAY_NVS_CONFIGURATION_KEY,
            &g_gateway_config_default,
            sizeof(g_gateway_config_default)))
    {
        nvs_close(handle);
        return false;
    }
    nvs_close(handle);
    return true;
}

void
settings_save_to_flash(const ruuvi_gateway_config_t *p_config)
{
    LOG_DBG(".");
    nvs_handle handle = 0;
    if (!settings_nvs_open(NVS_READWRITE, &handle))
    {
        return;
    }
    (void)settings_nvs_set_blob(handle, RUUVI_GATEWAY_NVS_CONFIGURATION_KEY, p_config, sizeof(*p_config));
    nvs_close(handle);
}

static bool
settings_get_gw_cfg_from_nvs(nvs_handle handle, ruuvi_gateway_config_t *p_gw_cfg)
{
    size_t    sz      = 0;
    esp_err_t esp_err = nvs_get_blob(handle, RUUVI_GATEWAY_NVS_CONFIGURATION_KEY, NULL, &sz);
    if (ESP_OK != esp_err)
    {
        LOG_ERR_ESP(esp_err, "Can't read config from flash");
        return false;
    }

    if (sizeof(*p_gw_cfg) != sz)
    {
        LOG_WARN("Size of config in flash differs");
        return false;
    }

    esp_err = nvs_get_blob(handle, RUUVI_GATEWAY_NVS_CONFIGURATION_KEY, p_gw_cfg, &sz);
    if (ESP_OK != esp_err)
    {
        LOG_ERR_ESP(esp_err, "Can't read config from flash");
        return false;
    }

    if (RUUVI_GATEWAY_CONFIG_HEADER != p_gw_cfg->header)
    {
        LOG_WARN("Incorrect config header (0x%02X)", p_gw_cfg->header);
        return false;
    }
    if (RUUVI_GATEWAY_CONFIG_FMT_VERSION != p_gw_cfg->fmt_version)
    {
        LOG_WARN(
            "Incorrect config fmt version (exp 0x%02x, act 0x%02x)",
            RUUVI_GATEWAY_CONFIG_FMT_VERSION,
            p_gw_cfg->fmt_version);
        return false;
    }
    return true;
}

void
settings_get_from_flash(ruuvi_gateway_config_t *p_gateway_config)
{
    nvs_handle handle = 0;
    if (!settings_nvs_open(NVS_READONLY, &handle))
    {
        LOG_WARN("Using default config:");
        *p_gateway_config = g_gateway_config_default;
    }
    else
    {
        if (!settings_get_gw_cfg_from_nvs(handle, p_gateway_config))
        {
            LOG_INFO("Using default config:");
            *p_gateway_config = g_gateway_config_default;
        }
        else
        {
            LOG_INFO("Configuration from flash:");
        }
        nvs_close(handle);
    }
    gw_cfg_print_to_log(p_gateway_config);
}

mac_address_bin_t
settings_read_mac_addr(void)
{
    mac_address_bin_t mac_addr = { 0 };
    nvs_handle        handle   = 0;
    if (!settings_nvs_open(NVS_READONLY, &handle))
    {
        LOG_WARN("Use empty mac_addr");
    }
    else
    {
        size_t    sz      = sizeof(mac_addr);
        esp_err_t esp_err = nvs_get_blob(handle, RUUVI_GATEWAY_NVS_MAC_ADDR_KEY, &mac_addr, &sz);
        if (ESP_OK != esp_err)
        {
            LOG_WARN_ESP(esp_err, "Can't read mac_addr from flash");
        }
        nvs_close(handle);
    }
    return mac_addr;
}

void
settings_write_mac_addr(const mac_address_bin_t *const p_mac_addr)
{
    nvs_handle handle = 0;
    if (!settings_nvs_open(NVS_READWRITE, &handle))
    {
        LOG_ERR("%s failed", "settings_nvs_open");
    }
    else
    {
        if (!settings_nvs_set_blob(handle, RUUVI_GATEWAY_NVS_MAC_ADDR_KEY, p_mac_addr, sizeof(*p_mac_addr)))
        {
            LOG_ERR("%s failed", "settings_nvs_set_blob");
        }
        nvs_close(handle);
    }
}

void
settings_update_mac_addr(const mac_address_bin_t *const p_mac_addr)
{
    const mac_address_bin_t mac_addr = settings_read_mac_addr();
    if (0 != memcmp(&mac_addr, p_mac_addr, sizeof(*p_mac_addr)))
    {
        const mac_address_str_t new_mac_addr_str = mac_address_to_str(p_mac_addr);
        LOG_INFO("Save new MAC-address: %s", new_mac_addr_str.str_buf);
        settings_write_mac_addr(p_mac_addr);
    }
}

bool
settings_read_flag_rebooting_after_auto_update(void)
{
    uint32_t   flag_rebooting_after_auto_update = 0;
    nvs_handle handle                           = 0;
    if (!settings_nvs_open(NVS_READONLY, &handle))
    {
        LOG_WARN("settings_nvs_open failed, flag_rebooting_after_auto_update = false");
        return false;
    }
    size_t    sz      = sizeof(flag_rebooting_after_auto_update);
    esp_err_t esp_err = nvs_get_blob(
        handle,
        RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_KEY,
        &flag_rebooting_after_auto_update,
        &sz);
    if (ESP_OK != esp_err)
    {
        LOG_WARN_ESP(esp_err, "Can't read '%s' from flash", RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_KEY);
        nvs_close(handle);
        settings_write_flag_rebooting_after_auto_update(false);
    }
    else
    {
        nvs_close(handle);
    }
    if (RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_VALUE == flag_rebooting_after_auto_update)
    {
        return true;
    }
    return false;
}

void
settings_write_flag_rebooting_after_auto_update(const bool flag_rebooting_after_auto_update)
{
    LOG_INFO("SETTINGS: Write flag_rebooting_after_auto_update: %d", flag_rebooting_after_auto_update);
    nvs_handle handle = 0;
    if (!settings_nvs_open(NVS_READWRITE, &handle))
    {
        LOG_ERR("%s failed", "settings_nvs_open");
        return;
    }
    const uint32_t flag_rebooting_after_auto_update_val = flag_rebooting_after_auto_update
                                                              ? RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_VALUE
                                                              : 0;
    if (!settings_nvs_set_blob(
            handle,
            RUUVI_GATEWAY_NVS_FLAG_REBOOTING_AFTER_AUTO_UPDATE_KEY,
            &flag_rebooting_after_auto_update_val,
            sizeof(flag_rebooting_after_auto_update_val)))
    {
        LOG_ERR("%s failed", "settings_nvs_set_blob");
    }
    nvs_close(handle);
}
