/**
 * @file adv_post.c
 * @author Oleg Protasevich
 * @date 2020-09-04
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "adv_post.h"
#include <esp_task_wdt.h>
#include "bin2hex.h"
#include "cJSON.h"
#include "esp_err.h"
#include "os_task.h"
#include "freertos/FreeRTOS.h"
#include "http.h"
#include "mqtt.h"
#include "ruuvi_boards.h"
#include "api.h"
#include "types_def.h"
#include "ruuvi_endpoint_ca_uart.h"
#include "ruuvi_endpoints.h"
#include "ruuvi_gateway.h"
#include <ctype.h>
#include <string.h>
#include <time.h>
#include "time_task.h"
#include "attribs.h"
#include "metrics.h"
#include "os_malloc.h"
#include "esp_type_wrapper.h"
#include "wifi_manager.h"
#include "mac_addr.h"
#include "http_json.h"
#include "ruuvi_device_id.h"
#include "os_signal.h"
#include "os_timer_sig.h"

#define LOG_LOCAL_LEVEL LOG_LEVEL_INFO
#include "log.h"

typedef enum adv_post_sig_e
{
    ADV_POST_SIG_STOP               = OS_SIGNAL_NUM_0,
    ADV_POST_SIG_RETRANSMIT         = OS_SIGNAL_NUM_1,
    ADV_POST_SIG_DISABLE            = OS_SIGNAL_NUM_2,
    ADV_POST_SIG_ENABLE             = OS_SIGNAL_NUM_3,
    ADV_POST_SIG_NETWORK_WATCHDOG   = OS_SIGNAL_NUM_4,
    ADV_POST_SIG_TASK_WATCHDOG_FEED = OS_SIGNAL_NUM_5,
} adv_post_sig_e;

#define ADV_POST_SIG_FIRST (ADV_POST_SIG_STOP)
#define ADV_POST_SIG_LAST  (ADV_POST_SIG_TASK_WATCHDOG_FEED)

static void
adv_post_send_report(void *arg);

static void
adv_post_send_ack(void *arg);

static void
adv_post_cb_on_recv_device_id(void *arg);

static void
adv_post_send_get_all(void *arg);

static const char *TAG = "ADV_POST_TASK";

static adv_callbacks_fn_t adv_callback_func_tbl = {
    .AdvAckCallback    = adv_post_send_ack,
    .AdvReportCallback = adv_post_send_report,
    .AdvIdCallback     = adv_post_cb_on_recv_device_id,
    .AdvGetAllCallback = adv_post_send_get_all,
};

static uint32_t                       g_adv_post_nonce;
static os_signal_t *                  g_p_adv_post_sig;
static os_signal_static_t             g_adv_post_sig_mem;
static os_timer_sig_periodic_t *      g_p_adv_post_timer_sig_retransmit;
static os_timer_sig_periodic_static_t g_adv_post_timer_sig_retransmit_mem;
static os_timer_sig_periodic_t *      g_p_adv_post_timer_sig_watchdog_feed;
static os_timer_sig_periodic_static_t g_adv_post_timer_sig_watchdog_feed_mem;
static uint32_t                       g_adv_post_interval_ms = ADV_POST_DEFAULT_INTERVAL_SECONDS * 1000U;
static bool                           g_adv_post_flag_retransmission_disabled = false;
static TickType_t                     g_adv_post_last_successful_network_comm_timestamp;
static os_timer_sig_periodic_t *      g_p_adv_post_timer_sig_network_watchdog;
static os_timer_sig_periodic_static_t g_adv_post_timer_sig_network_watchdog_mem;

ATTR_PURE
static os_signal_num_e
adv_post_conv_to_sig_num(const adv_post_sig_e sig)
{
    return (os_signal_num_e)sig;
}

static adv_post_sig_e
adv_post_conv_from_sig_num(const os_signal_num_e sig_num)
{
    assert(((os_signal_num_e)ADV_POST_SIG_FIRST <= sig_num) && (sig_num <= (os_signal_num_e)ADV_POST_SIG_LAST));
    return (adv_post_sig_e)sig_num;
}

/** @brief serialise up to U64 into given buffer, MSB first. */
static inline void
u64_to_array(const uint64_t u64, uint8_t *const array, uint8_t bytes)
{
    const uint8_t offset = bytes - 1;

    while (bytes--)
    {
        array[offset - bytes] = (u64 >> (8U * bytes)) & 0xFFU;
    }
}

static esp_err_t
adv_put_to_table(const adv_report_t *const p_adv)
{
    metrics_received_advs_increment();
    if (!adv_table_put(p_adv))
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool
parse_adv_report_from_uart(const re_ca_uart_payload_t *const p_msg, adv_report_t *const p_adv)
{
    if (NULL == p_msg)
    {
        return false;
    }
    if (NULL == p_adv)
    {
        return false;
    }
    if (RE_CA_UART_ADV_RPRT != p_msg->cmd)
    {
        return false;
    }
    const re_ca_uart_ble_adv_t *const p_report = &(p_msg->params.adv);
    if (p_report->adv_len > sizeof(p_adv->data_buf))
    {
        LOG_ERR(
            "Got advertisement with len=%u, max allowed len=%u",
            (unsigned)p_report->adv_len,
            (unsigned)sizeof(p_adv->data_buf));
        return false;
    }
    mac_address_bin_init(&p_adv->tag_mac, p_report->mac);
    p_adv->timestamp = time(NULL);
    p_adv->rssi      = p_report->rssi_db;
    p_adv->data_len  = p_report->adv_len;
    memcpy(p_adv->data_buf, p_report->adv, p_report->adv_len);

    return true;
}

static void
adv_post_send_ack(void *arg)
{
    (void)arg;
    // Do something
}

static void
adv_post_cb_on_recv_device_id(void *p_arg)
{
    const re_ca_uart_payload_t *const p_uart_payload = (re_ca_uart_payload_t *)p_arg;

    nrf52_device_id_t nrf52_device_id = { 0 };
    u64_to_array(p_uart_payload->params.device_id.id, &nrf52_device_id.id[0], sizeof(nrf52_device_id.id));

    mac_address_bin_t nrf52_mac_addr = { 0 };
    u64_to_array(p_uart_payload->params.device_id.addr, &nrf52_mac_addr.mac[0], sizeof(nrf52_mac_addr.mac));

    LOG_INFO("nRF52 DEVICE ID : 0x%016llx", p_uart_payload->params.device_id.id);
    LOG_INFO("nRF52 MAC ADDR  : 0x%016llx", p_uart_payload->params.device_id.addr);

    ruuvi_device_id_set(&nrf52_device_id, &nrf52_mac_addr);
}

static void
adv_post_send_report(void *arg)
{
    adv_report_t adv_report = { 0 };

    if (!parse_adv_report_from_uart((re_ca_uart_payload_t *)arg, &adv_report))
    {
        return;
    }
    const esp_err_t ret = adv_put_to_table(&adv_report);
    if (ESP_ERR_NO_MEM == ret)
    {
        LOG_WARN("Adv report table full, adv dropped");
    }
    if (gw_cfg_get_mqtt_use_mqtt())
    {
        if (0 == (xEventGroupGetBits(status_bits) & MQTT_CONNECTED_BIT))
        {
            LOG_WARN("Can't send, MQTT is not connected yet");
        }
        else
        {
            if (mqtt_publish_adv(&adv_report))
            {
                adv_post_update_last_successful_network_comm_timestamp();
            }
            else
            {
                LOG_ERR("%s failed", "mqtt_publish_adv");
            }
        }
    }
}

static void
adv_post_send_get_all(void *arg)
{
    (void)arg;
    ruuvi_send_nrf_settings();
}

static void
adv_post_log(const adv_report_table_t *p_reports)
{
    LOG_INFO("Advertisements in table: %u", (printf_uint_t)p_reports->num_of_advs);
    for (num_of_advs_t i = 0; i < p_reports->num_of_advs; ++i)
    {
        const adv_report_t *p_adv = &p_reports->table[i];

        const mac_address_str_t mac_str = mac_address_to_str(&p_adv->tag_mac);
        LOG_DUMP_INFO(
            p_adv->data_buf,
            p_adv->data_len,
            "i: %d, tag: %s, rssi: %d, timestamp: %ld",
            i,
            mac_str.str_buf,
            p_adv->rssi,
            p_adv->timestamp);
    }
}

static bool
adv_post_check_is_connected(const uint32_t nonce)
{
    const EventBits_t status = xEventGroupGetBits(status_bits);
    if (0 == (status & (WIFI_CONNECTED_BIT | ETH_CONNECTED_BIT)))
    {
        return false;
    }
    const ruuvi_gateway_config_t *p_gw_cfg = gw_cfg_lock_ro();
    if (p_gw_cfg->http.use_http)
    {
        cjson_wrap_str_t json_str = cjson_wrap_str_null();
        if (!http_create_status_online_json_str(
                time(NULL),
                &g_gw_mac_sta_str,
                p_gw_cfg->coordinates.buf,
                nonce,
                &json_str))
        {
            LOG_ERR("Not enough memory to generate json");
        }
        else
        {
            LOG_INFO("HTTP POST %s: %s", p_gw_cfg->http.http_url, json_str.p_str);
            http_send(json_str.p_str);
            cjson_wrap_free_json_str(&json_str);
        }
    }
    gw_cfg_unlock_ro(&p_gw_cfg);
    return true;
}

static bool
adv_post_check_is_disconnected(void)
{
    bool flag_connected = true;

    const EventBits_t status = xEventGroupGetBits(status_bits);
    if (0 == (status & (WIFI_CONNECTED_BIT | ETH_CONNECTED_BIT)))
    {
        flag_connected = false;
    }
    return flag_connected;
}

static void
adv_post_retransmit_advs(const adv_report_table_t *p_reports, const bool flag_connected)
{
    if (0 == p_reports->num_of_advs)
    {
        return;
    }
    if (!flag_connected)
    {
        LOG_WARN("Can't send, no network connection");
        return;
    }
    if (!time_is_valid(p_reports->table[0].timestamp))
    {
        LOG_WARN("Can't send, the time has not yet been synchronized");
        return;
    }

    if (!wifi_manager_is_connected_to_wifi_or_ethernet())
    {
        LOG_WARN("Can't send, no network connection");
        return;
    }
    if (http_send_advs(p_reports, g_adv_post_nonce))
    {
        adv_post_update_last_successful_network_comm_timestamp();
    }
    g_adv_post_nonce += 1;
}

static void
adv_post_do_retransmission(bool *const p_flag_connected)
{
    static adv_report_table_t g_adv_reports_buf;

    // for thread safety copy the advertisements to a separate buffer for posting
    adv_table_read_retransmission_list_and_clear(&g_adv_reports_buf);

    adv_post_log(&g_adv_reports_buf);

    if (!*p_flag_connected)
    {
        *p_flag_connected = adv_post_check_is_connected(g_adv_post_nonce);
        g_adv_post_nonce += 1;
    }
    else
    {
        *p_flag_connected = adv_post_check_is_disconnected();
    }

    if (0 != g_adv_reports_buf.num_of_advs)
    {
        if (*p_flag_connected)
        {
            adv_post_retransmit_advs(&g_adv_reports_buf, *p_flag_connected);
        }
        else
        {
            LOG_WARN("Can't send, no network connection");
        }
    }
}

static void
adv_post_wdt_add_and_start(void)
{
    LOG_INFO("TaskWatchdog: Register current thread");
    const esp_err_t err = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "%s failed", "esp_task_wdt_add");
    }
    LOG_INFO("TaskWatchdog: Start timer");
    os_timer_sig_periodic_start(g_p_adv_post_timer_sig_watchdog_feed);
}

static void
adv_post_task(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    if (!os_signal_register_cur_thread(g_p_adv_post_sig))
    {
        LOG_ERR("%s failed", "os_signal_register_cur_thread");
        return;
    }

    LOG_INFO("%s started", __func__);
    os_timer_sig_periodic_start(g_p_adv_post_timer_sig_retransmit);
    os_timer_sig_periodic_start(g_p_adv_post_timer_sig_network_watchdog);

    adv_post_wdt_add_and_start();

    bool flag_stop      = false;
    bool flag_connected = false;
    while (!flag_stop)
    {
        os_signal_events_t sig_events = { 0 };
        if (!os_signal_wait_with_timeout(g_p_adv_post_sig, OS_DELTA_TICKS_INFINITE, &sig_events))
        {
            continue;
        }
        for (;;)
        {
            const os_signal_num_e sig_num = os_signal_num_get_next(&sig_events);
            if (OS_SIGNAL_NUM_NONE == sig_num)
            {
                break;
            }
            const adv_post_sig_e adv_post_sig = adv_post_conv_from_sig_num(sig_num);
            switch (adv_post_sig)
            {
                case ADV_POST_SIG_STOP:
                    LOG_INFO("Got ADV_POST_SIG_STOP");
                    flag_stop = true;
                    break;
                case ADV_POST_SIG_RETRANSMIT:
                    if (gw_cfg_get_mqtt_use_http() && !g_adv_post_flag_retransmission_disabled)
                    {
                        adv_post_do_retransmission(&flag_connected);
                    }
                    break;
                case ADV_POST_SIG_DISABLE:
                    g_adv_post_flag_retransmission_disabled = true;
                    break;
                case ADV_POST_SIG_ENABLE:
                    g_adv_post_flag_retransmission_disabled = false;
                    break;
                case ADV_POST_SIG_NETWORK_WATCHDOG:
                {
                    const TickType_t delta_ticks = xTaskGetTickCount()
                                                   - g_adv_post_last_successful_network_comm_timestamp;
                    const TickType_t timeout_ticks = pdMS_TO_TICKS(RUUVI_NETWORK_WATCHDOG_TIMEOUT_SECONDS) * 1000;
                    if (delta_ticks > timeout_ticks)
                    {
                        LOG_INFO(
                            "No networking for %lu seconds - reboot the gateway",
                            (printf_ulong_t)RUUVI_NETWORK_WATCHDOG_TIMEOUT_SECONDS);
                        esp_restart();
                    }
                    break;
                }
                case ADV_POST_SIG_TASK_WATCHDOG_FEED:
                {
                    const esp_err_t err = esp_task_wdt_reset();
                    if (ESP_OK != err)
                    {
                        LOG_ERR_ESP(err, "%s failed", "esp_task_wdt_reset");
                    }
                    break;
                }
            }
        }
    }
    LOG_INFO("Stop task adv_post");
    LOG_INFO("TaskWatchdog: Unregister current thread");
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle());
    os_timer_sig_periodic_stop(g_p_adv_post_timer_sig_retransmit);
    os_timer_sig_periodic_delete(&g_p_adv_post_timer_sig_retransmit);
    os_timer_sig_periodic_stop(g_p_adv_post_timer_sig_network_watchdog);
    os_timer_sig_periodic_delete(&g_p_adv_post_timer_sig_network_watchdog);

    LOG_INFO("TaskWatchdog: Stop timer");
    os_timer_sig_periodic_stop(g_p_adv_post_timer_sig_watchdog_feed);
    LOG_INFO("TaskWatchdog: Delete timer");
    os_timer_sig_periodic_delete(&g_p_adv_post_timer_sig_watchdog_feed);

    os_signal_unregister_cur_thread(g_p_adv_post_sig);
    os_signal_delete(&g_p_adv_post_sig);
}

void
adv_post_init(void)
{
    g_p_adv_post_sig = os_signal_create_static(&g_adv_post_sig_mem);
    os_signal_add(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_STOP));
    os_signal_add(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_RETRANSMIT));
    os_signal_add(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_DISABLE));
    os_signal_add(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_ENABLE));
    os_signal_add(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_NETWORK_WATCHDOG));
    os_signal_add(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_TASK_WATCHDOG_FEED));

    g_p_adv_post_timer_sig_retransmit = os_timer_sig_periodic_create_static(
        &g_adv_post_timer_sig_retransmit_mem,
        "adv_post_retransmit",
        g_p_adv_post_sig,
        adv_post_conv_to_sig_num(ADV_POST_SIG_RETRANSMIT),
        pdMS_TO_TICKS(ADV_POST_DEFAULT_INTERVAL_SECONDS * 1000));

    g_p_adv_post_timer_sig_network_watchdog = os_timer_sig_periodic_create_static(
        &g_adv_post_timer_sig_network_watchdog_mem,
        "adv_post_watchdog",
        g_p_adv_post_sig,
        adv_post_conv_to_sig_num(ADV_POST_SIG_NETWORK_WATCHDOG),
        pdMS_TO_TICKS(RUUVI_NETWORK_WATCHDOG_PERIOD_SECONDS * 1000));

    LOG_INFO("TaskWatchdog: adv_post: Create timer");
    g_p_adv_post_timer_sig_watchdog_feed = os_timer_sig_periodic_create_static(
        &g_adv_post_timer_sig_watchdog_feed_mem,
        "adv_post:wdog",
        g_p_adv_post_sig,
        adv_post_conv_to_sig_num(ADV_POST_SIG_TASK_WATCHDOG_FEED),
        pdMS_TO_TICKS(CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U / 3U));

    g_adv_post_nonce = esp_random();
    adv_table_init();
    api_callbacks_reg((void *)&adv_callback_func_tbl);
    const uint32_t stack_size = 1024U * 4U;
    if (!os_task_create_finite_without_param(&adv_post_task, "adv_post_task", stack_size, 1))
    {
        LOG_ERR("Can't create thread");
    }
}

void
adv_post_set_period(const uint32_t period_ms)
{
    if (period_ms != g_adv_post_interval_ms)
    {
        LOG_INFO(
            "X-Ruuvi-Gateway-Rate: Change period from %u ms to %u ms",
            (printf_uint_t)g_adv_post_interval_ms,
            (printf_uint_t)period_ms);
        g_adv_post_interval_ms = period_ms;
        os_timer_sig_periodic_restart(g_p_adv_post_timer_sig_retransmit, pdMS_TO_TICKS(period_ms));
    }
}

void
adv_post_stop(void)
{
    LOG_INFO("adv_post_stop");
    if (!os_signal_send(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_STOP)))
    {
        LOG_ERR("%s failed", "os_signal_send");
    }
}

void
adv_post_disable_retransmission(void)
{
    LOG_INFO("adv_post_disable_retransmission");
    if (!os_signal_send(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_DISABLE)))
    {
        LOG_ERR("%s failed", "os_signal_send");
    }
}

void
adv_post_enable_retransmission(void)
{
    LOG_INFO("adv_post_enable_retransmission");
    if (!os_signal_send(g_p_adv_post_sig, adv_post_conv_to_sig_num(ADV_POST_SIG_ENABLE)))
    {
        LOG_ERR("%s failed", "os_signal_send");
    }
}

void
adv_post_update_last_successful_network_comm_timestamp(void)
{
    g_adv_post_last_successful_network_comm_timestamp = xTaskGetTickCount();
}
