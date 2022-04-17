/**
 * @file gw_cfg_default.c
 * @author TheSomeMan
 * @date 2021-08-29
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "gw_cfg_default.h"
#include <stdio.h>
#include <string.h>
#include "os_malloc.h"
#include "wifiman_md5.h"
#include "wifi_manager.h"
#include "str_buf.h"
#include "gw_cfg_log.h"

static const gw_cfg_eth_t g_gateway_config_default_eth = {
    .use_eth       = false,
    .eth_dhcp      = true,
    .eth_static_ip = { { "" } },
    .eth_netmask   = { { "" } },
    .eth_gw        = { { "" } },
    .eth_dns1      = { { "" } },
    .eth_dns2      = { { "" } },
};

static const gw_cfg_ruuvi_t g_gateway_config_default_ruuvi = {
        .http = {
            .use_http = true,
            .http_url = { { RUUVI_GATEWAY_HTTP_DEFAULT_URL } },
            .http_user = {{ "" }},
            .http_pass = {{ "" }},
        },
        .http_stat = {
            .use_http_stat = true,
            .http_stat_url = {{ RUUVI_GATEWAY_HTTP_STATUS_URL }},
            .http_stat_user = {{ "" }},
            .http_stat_pass = {{ "" }},
        },
        .mqtt = {
            .use_mqtt = false,
            .mqtt_transport = {{ MQTT_TRANSPORT_TCP }},
            .mqtt_server = {{ "test.mosquitto.org" }},
            .mqtt_port = 1883,
            .mqtt_prefix = {{ "" }},
            .mqtt_client_id = {{ "" }},
            .mqtt_user = {{ "" }},
            .mqtt_pass = {{ "" }},
        },
        .lan_auth = {
            .lan_auth_type = HTTP_SERVER_AUTH_TYPE_RUUVI,
            .lan_auth_user = { RUUVI_GATEWAY_AUTH_DEFAULT_USER },
            .lan_auth_pass = { "" },  // default password is set in gw_cfg_default_init
            .lan_auth_api_key = { "" },
        },
        .auto_update = {
            .auto_update_cycle = AUTO_UPDATE_CYCLE_TYPE_REGULAR,
            .auto_update_weekdays_bitmask = 0x7F,
            .auto_update_interval_from = 0,
            .auto_update_interval_to = 24,
            .auto_update_tz_offset_hours = 3,
        },
        .filter = {
            .company_id = RUUVI_COMPANY_ID,
            .company_use_filtering = true,
        },
        .scan = {
            .scan_coded_phy = false,
            .scan_1mbit_phy = true,
            .scan_extended_payload = true,
            .scan_channel_37 = true,
            .scan_channel_38 = true,
            .scan_channel_39 = true,
        },
        .coordinates = {{ "" }},
    };

static gw_cfg_t g_gw_cfg_default;

static void
gw_cfg_default_generate_lan_auth_password(
    const wifiman_wifi_ssid_t *const    p_gw_wifi_ssid,
    const nrf52_device_id_str_t *const  p_device_id,
    wifiman_md5_digest_hex_str_t *const p_lan_auth_default_password_md5)
{
    char tmp_buf[sizeof(RUUVI_GATEWAY_AUTH_DEFAULT_USER) + 1 + MAX_SSID_SIZE + 1 + sizeof(nrf52_device_id_str_t)];
    snprintf(
        tmp_buf,
        sizeof(tmp_buf),
        "%s:%s:%s",
        RUUVI_GATEWAY_AUTH_DEFAULT_USER,
        p_gw_wifi_ssid->ssid_buf,
        p_device_id->str_buf);

    *p_lan_auth_default_password_md5 = wifiman_md5_calc_hex_str(tmp_buf, strlen(tmp_buf));
}

static nrf52_device_id_str_t
gw_cfg_default_nrf52_device_id_to_str(const nrf52_device_id_t *const p_dev_id)
{
    nrf52_device_id_str_t device_id_str = { 0 };
    str_buf_t             str_buf       = {
        .buf  = device_id_str.str_buf,
        .size = sizeof(device_id_str.str_buf),
        .idx  = 0,
    };
    for (size_t i = 0; i < sizeof(p_dev_id->id); ++i)
    {
        if (0 != i)
        {
            str_buf_printf(&str_buf, ":");
        }
        str_buf_printf(&str_buf, "%02X", p_dev_id->id[i]);
    }
    return device_id_str;
}

void
gw_cfg_default_init(
    const gw_cfg_default_init_param_t *const p_init_param,
    bool (*p_cb_gw_cfg_default_json_read)(gw_cfg_t *const p_gw_cfg_default))
{
    memset(&g_gw_cfg_default, 0, sizeof(g_gw_cfg_default));

    g_gw_cfg_default.ruuvi_cfg = g_gateway_config_default_ruuvi;
    g_gw_cfg_default.eth_cfg   = g_gateway_config_default_eth;

    g_gw_cfg_default.wifi_cfg = *wifi_manager_default_config_init(&p_init_param->wifi_ap_ssid);

    gw_cfg_device_info_t *const p_def_dev_info = &g_gw_cfg_default.device_info;
    p_def_dev_info->wifi_ap_hostname           = p_init_param->wifi_ap_ssid;
    p_def_dev_info->esp32_fw_ver               = p_init_param->esp32_fw_ver;
    p_def_dev_info->nrf52_fw_ver               = p_init_param->nrf52_fw_ver;
    p_def_dev_info->nrf52_device_id            = gw_cfg_default_nrf52_device_id_to_str(&p_init_param->device_id);
    p_def_dev_info->nrf52_mac_addr             = mac_address_to_str(&p_init_param->nrf52_mac_addr);
    p_def_dev_info->esp32_mac_addr_wifi        = mac_address_to_str(&p_init_param->esp32_mac_addr_wifi);
    p_def_dev_info->esp32_mac_addr_eth         = mac_address_to_str(&p_init_param->esp32_mac_addr_eth);

    if (NULL != p_cb_gw_cfg_default_json_read)
    {
        if (p_cb_gw_cfg_default_json_read(&g_gw_cfg_default))
        {
            wifi_manager_set_default_config(&g_gw_cfg_default.wifi_cfg);
        }
    }

    ruuvi_gw_cfg_mqtt_t *const p_mqtt = &g_gw_cfg_default.ruuvi_cfg.mqtt;
    (void)snprintf(
        p_mqtt->mqtt_prefix.buf,
        sizeof(p_mqtt->mqtt_prefix.buf),
        "ruuvi/%s/",
        p_def_dev_info->nrf52_mac_addr.str_buf);
    (void)snprintf(
        p_mqtt->mqtt_client_id.buf,
        sizeof(p_mqtt->mqtt_client_id.buf),
        "%s",
        p_def_dev_info->nrf52_mac_addr.str_buf);

    wifiman_md5_digest_hex_str_t lan_auth_default_password_md5 = { 0 };
    gw_cfg_default_generate_lan_auth_password(
        &p_init_param->wifi_ap_ssid,
        &p_def_dev_info->nrf52_device_id,
        &lan_auth_default_password_md5);

    ruuvi_gw_cfg_lan_auth_t *const p_lan_auth = &g_gw_cfg_default.ruuvi_cfg.lan_auth;
    _Static_assert(
        sizeof(p_lan_auth->lan_auth_pass) >= sizeof(lan_auth_default_password_md5),
        "sizeof lan_auth_pass >= sizeof wifiman_md5_digest_hex_str_t");

    (void)snprintf(
        p_lan_auth->lan_auth_user.buf,
        sizeof(p_lan_auth->lan_auth_user.buf),
        "%s",
        RUUVI_GATEWAY_AUTH_DEFAULT_USER);
    (void)snprintf(
        p_lan_auth->lan_auth_pass.buf,
        sizeof(p_lan_auth->lan_auth_pass.buf),
        "%s",
        lan_auth_default_password_md5.buf);

    gw_cfg_log(&g_gw_cfg_default, "Gateway SETTINGS (default)", true);
}

void
gw_cfg_default_get(gw_cfg_t *const p_gw_cfg)
{
    *p_gw_cfg = g_gw_cfg_default;
}

gw_cfg_device_info_t
gw_cfg_default_device_info(void)
{
    return g_gw_cfg_default.device_info;
}

const ruuvi_gw_cfg_mqtt_t *
gw_cfg_default_get_mqtt(void)
{
    return &g_gw_cfg_default.ruuvi_cfg.mqtt;
}

const ruuvi_gw_cfg_lan_auth_t *
gw_cfg_default_get_lan_auth(void)
{
    return &g_gw_cfg_default.ruuvi_cfg.lan_auth;
}

gw_cfg_eth_t
gw_cfg_default_get_eth(void)
{
    return g_gw_cfg_default.eth_cfg;
}

const wifi_sta_config_t *
gw_cfg_default_get_wifi_sta_config_ptr(void)
{
    return &g_gw_cfg_default.wifi_cfg.wifi_config_sta;
}

const wifiman_wifi_ssid_t *
gw_cfg_default_get_wifi_ap_ssid(void)
{
    return &g_gw_cfg_default.device_info.wifi_ap_hostname;
}
