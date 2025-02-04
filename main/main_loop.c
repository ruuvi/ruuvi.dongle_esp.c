/**
 * @file main_loop.c
 * @author TheSomeMan
 * @date 2021-11-29
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "ruuvi_gateway.h"
#include "esp_task_wdt.h"
#include "os_signal.h"
#include "os_timer_sig.h"
#include "os_time.h"
#include "time_units.h"
#include "wifi_manager.h"
#include "ethernet.h"
#include "leds.h"
#include "mqtt.h"
#include "mdns.h"
#include "time_task.h"
#include "event_mgr.h"
#include "gw_status.h"
#include "os_malloc.h"
#include "gw_cfg.h"
#include "gw_cfg_default.h"
#include "gw_cfg_log.h"
#include "reset_task.h"
#include "runtime_stat.h"

#define LOG_LOCAL_LEVEL LOG_LEVEL_INFO
#include "log.h"

static const char TAG[] = "ruuvi_gateway";

#define MAIN_TASK_LOG_HEAP_STAT_PERIOD_MS       (100U)
#define MAIN_TASK_LOG_HEAP_USAGE_PERIOD_SECONDS (10U)

#define MAIN_TASK_CHECK_FOR_REMOTE_CFG_PERIOD_MS (60U * TIME_UNITS_SECONDS_PER_MINUTE * TIME_UNITS_MS_PER_SECOND)
#define MAIN_TASK_GET_HISTORY_TIMEOUT_MS         (70U * TIME_UNITS_MS_PER_SECOND)
#define MAIN_TASK_LOG_RUNTIME_STAT_PERIOD_MS     (30 * TIME_UNITS_MS_PER_SECOND)
#define MAIN_TASK_WATCHDOG_FEED_PERIOD_MS        (1 * TIME_UNITS_MS_PER_SECOND)

#define RUUVI_NUM_BYTES_IN_1KB (1024U)

typedef enum main_task_sig_e
{
    MAIN_TASK_SIG_LOG_HEAP_USAGE                      = OS_SIGNAL_NUM_0,
    MAIN_TASK_SIG_CHECK_FOR_FW_UPDATES                = OS_SIGNAL_NUM_1,
    MAIN_TASK_SIG_SCHEDULE_NEXT_CHECK_FOR_FW_UPDATES  = OS_SIGNAL_NUM_2,
    MAIN_TASK_SIG_SCHEDULE_RETRY_CHECK_FOR_FW_UPDATES = OS_SIGNAL_NUM_3,
    MAIN_TASK_SIG_DEFERRED_ETHERNET_ACTIVATION        = OS_SIGNAL_NUM_4,
    MAIN_TASK_SIG_WIFI_AP_STARTED                     = OS_SIGNAL_NUM_5,
    MAIN_TASK_SIG_WIFI_AP_STOPPED                     = OS_SIGNAL_NUM_6,
    MAIN_TASK_SIG_ACTIVATE_CFG_MODE                   = OS_SIGNAL_NUM_7,
    MAIN_TASK_SIG_DEACTIVATE_CFG_MODE                 = OS_SIGNAL_NUM_8,
    MAIN_TASK_SIG_TASK_RESTART_SERVICES               = OS_SIGNAL_NUM_9,
    MAIN_TASK_SIG_CHECK_FOR_REMOTE_CFG                = OS_SIGNAL_NUM_10,
    MAIN_TASK_SIG_NETWORK_CONNECTED                   = OS_SIGNAL_NUM_11,
    MAIN_TASK_SIG_NETWORK_DISCONNECTED                = OS_SIGNAL_NUM_12,
    MAIN_TASK_SIG_RECONNECT_NETWORK                   = OS_SIGNAL_NUM_13,
    MAIN_TASK_SIG_SET_DEFAULT_CONFIG                  = OS_SIGNAL_NUM_14,
    MAIN_TASK_SIG_ON_GET_HISTORY                      = OS_SIGNAL_NUM_15,
    MAIN_TASK_SIG_ON_GET_HISTORY_TIMEOUT              = OS_SIGNAL_NUM_16,
    MAIN_TASK_SIG_RELAYING_MODE_CHANGED               = OS_SIGNAL_NUM_17,
    MAIN_TASK_SIG_LOG_RUNTIME_STAT                    = OS_SIGNAL_NUM_18,
    MAIN_TASK_SIG_TASK_WATCHDOG_FEED                  = OS_SIGNAL_NUM_19,
} main_task_sig_e;

#define MAIN_TASK_SIG_FIRST (MAIN_TASK_SIG_LOG_HEAP_USAGE)
#define MAIN_TASK_SIG_LAST  (MAIN_TASK_SIG_TASK_WATCHDOG_FEED)

static os_signal_t*                   g_p_signal_main_task;
static os_signal_static_t             g_signal_main_task_mem;
static os_timer_sig_periodic_t*       g_p_timer_sig_log_heap_usage;
static os_timer_sig_periodic_static_t g_timer_sig_log_heap_usage;
static os_timer_sig_periodic_t*       g_p_timer_sig_log_runtime_stat;
static os_timer_sig_periodic_static_t g_timer_sig_log_runtime_stat;
static os_timer_sig_one_shot_t*       g_p_timer_sig_check_for_fw_updates;
static os_timer_sig_one_shot_static_t g_timer_sig_check_for_fw_updates_mem;
static os_timer_sig_one_shot_t*       g_p_timer_sig_deactivate_cfg_mode;
static os_timer_sig_one_shot_static_t g_p_timer_sig_deactivate_cfg_mode_mem;
static os_timer_sig_periodic_t*       g_p_timer_sig_check_for_remote_cfg;
static os_timer_sig_periodic_static_t g_timer_sig_check_for_remote_cfg_mem;
static os_timer_sig_one_shot_t*       g_p_timer_sig_get_history_timeout;
static os_timer_sig_one_shot_static_t g_timer_sig_get_history_timeout_mem;
static os_timer_sig_periodic_t*       g_p_timer_sig_task_watchdog_feed;
static os_timer_sig_periodic_static_t g_timer_sig_task_watchdog_feed_mem;
static os_timer_sig_one_shot_t*       g_p_timer_sig_deferred_ethernet_activation;
static os_timer_sig_one_shot_static_t g_timer_sig_deferred_ethernet_activation_mem;

static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_wifi_connected;
static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_eth_connected;
static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_wifi_disconnected;
static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_eth_disconnected;
static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_relaying_mode_changed;
static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_ap_started;
static event_mgr_ev_info_static_t g_main_loop_ev_info_mem_ap_stopped;

ATTR_PURE
static os_signal_num_e
main_task_conv_to_sig_num(const main_task_sig_e sig)
{
    return (os_signal_num_e)sig;
}

static main_task_sig_e
main_task_conv_from_sig_num(const os_signal_num_e sig_num)
{
    assert(((os_signal_num_e)MAIN_TASK_SIG_FIRST <= sig_num) && (sig_num <= (os_signal_num_e)MAIN_TASK_SIG_LAST));
    return (main_task_sig_e)sig_num;
}

static bool
check_if_checking_for_fw_updates_allowed2(const ruuvi_gw_cfg_auto_update_t* const p_cfg_auto_update)
{
    const int32_t tz_offset_seconds = (int32_t)p_cfg_auto_update->auto_update_tz_offset_hours
                                      * (int32_t)(TIME_UNITS_MINUTES_PER_HOUR * TIME_UNITS_SECONDS_PER_MINUTE);

    const time_t unix_time = os_time_get();
    time_t       cur_time  = unix_time + tz_offset_seconds;
    struct tm    tm_time   = { 0 };
    gmtime_r(&cur_time, &tm_time);

    if (AUTO_UPDATE_CYCLE_TYPE_MANUAL == p_cfg_auto_update->auto_update_cycle)
    {
        LOG_INFO("Check for fw updates - skip (manual updating mode)");
        return false;
    }

    LOG_INFO("Check for fw updates: Current day: %s", os_time_wday_name_mid(os_time_get_tm_wday(&tm_time)));
    for (os_time_wday_e wday = OS_TIME_WDAY_SUN; wday <= OS_TIME_WDAY_SAT; ++wday)
    {
        const bool flag_active = (0 != (p_cfg_auto_update->auto_update_weekdays_bitmask & (1U << (uint32_t)wday)))
                                     ? true
                                     : false;
        LOG_INFO("Check for fw updates: %s - %s", os_time_wday_name_mid(wday), flag_active ? "Yes" : "No");
    }

    const uint32_t cur_day_bit_mask = 1U << (uint8_t)tm_time.tm_wday;
    if (0 == (p_cfg_auto_update->auto_update_weekdays_bitmask & cur_day_bit_mask))
    {
        LOG_INFO("Check for fw updates - skip (weekday does not match)");
        return false;
    }
    LOG_INFO(
        "Check for fw updates: configured range [%02u:00 .. %02u:00], current time: %02u:%02u)",
        (printf_uint_t)p_cfg_auto_update->auto_update_interval_from,
        (printf_uint_t)p_cfg_auto_update->auto_update_interval_to,
        (printf_uint_t)tm_time.tm_hour,
        (printf_uint_t)tm_time.tm_min);
    if (!((tm_time.tm_hour >= p_cfg_auto_update->auto_update_interval_from)
          && (tm_time.tm_hour < p_cfg_auto_update->auto_update_interval_to)))
    {
        LOG_INFO("Check for fw updates - skip (current time is out of range)");
        return false;
    }
    return true;
}

static bool
check_if_checking_for_fw_updates_allowed(void)
{
    if (!wifi_manager_is_connected_to_wifi_or_ethernet())
    {
        LOG_INFO("Check for fw updates - skip (not connected to WiFi or Ethernet)");
        return false;
    }
    if (!time_is_synchronized())
    {
        LOG_INFO("Check for fw updates - skip (time is not synchronized)");
        return false;
    }
    const gw_cfg_t* p_gw_cfg = gw_cfg_lock_ro();

    const bool res = check_if_checking_for_fw_updates_allowed2(&p_gw_cfg->ruuvi_cfg.auto_update);

    gw_cfg_unlock_ro(&p_gw_cfg);
    return res;
}

static void
main_task_handle_sig_log_heap_usage(void)
{
    static uint32_t g_heap_usage_stat_cnt      = 0;
    static uint32_t g_heap_usage_min_free_heap = 0xFFFFFFFFU;
    static uint32_t g_heap_usage_max_free_heap = 0;
    static uint32_t g_heap_limit_cnt           = 0;

    const uint32_t free_heap = esp_get_free_heap_size();

    g_heap_usage_stat_cnt += 1;
    if (g_heap_usage_stat_cnt
        < ((MAIN_TASK_LOG_HEAP_USAGE_PERIOD_SECONDS * TIME_UNITS_MS_PER_SECOND) / MAIN_TASK_LOG_HEAP_STAT_PERIOD_MS))
    {
        if (free_heap < g_heap_usage_min_free_heap)
        {
            g_heap_usage_min_free_heap = free_heap;
        }
        if (free_heap > g_heap_usage_max_free_heap)
        {
            g_heap_usage_max_free_heap = free_heap;
        }
    }
    else
    {
        LOG_INFO(
            "free heap: %lu .. %lu",
            (printf_ulong_t)g_heap_usage_min_free_heap,
            (printf_ulong_t)g_heap_usage_max_free_heap);
        if (g_heap_usage_max_free_heap < (RUUVI_FREE_HEAP_LIM_KIB * RUUVI_NUM_BYTES_IN_1KB))
        {
            g_heap_limit_cnt += 1;
            if (g_heap_limit_cnt >= RUUVI_MAX_LOW_HEAP_MEM_CNT)
            {
                LOG_ERR(
                    "Only %uKiB of free memory left - probably due to a memory leak. Reboot the Gateway.",
                    (printf_uint_t)(g_heap_usage_min_free_heap / RUUVI_NUM_BYTES_IN_1KB));
                gateway_restart("Low memory");
            }
        }
        else
        {
            g_heap_limit_cnt = 0;
        }
        g_heap_usage_stat_cnt      = 0;
        g_heap_usage_min_free_heap = UINT32_MAX;
        g_heap_usage_max_free_heap = 0;
    }
}

static void
main_task_handle_sig_check_for_fw_updates(void)
{
    if (check_if_checking_for_fw_updates_allowed())
    {
        LOG_INFO("Check for fw updates: activate");
        http_server_user_req(HTTP_SERVER_USER_REQ_CODE_DOWNLOAD_LATEST_RELEASE_INFO);
    }
    else
    {
        main_task_schedule_retry_check_for_fw_updates();
    }
}

static void
main_task_handle_sig_schedule_next_check_for_fw_updates(void)
{
    const os_delta_ticks_t delay_ticks = pdMS_TO_TICKS(RUUVI_CHECK_FOR_FW_UPDATES_DELAY_AFTER_SUCCESS_SECONDS)
                                         * TIME_UNITS_MS_PER_SECOND;
    LOG_INFO(
        "Schedule next check for fw updates (after successful release_info downloading) after %lu seconds (%lu "
        "ticks)",
        (printf_ulong_t)RUUVI_CHECK_FOR_FW_UPDATES_DELAY_AFTER_SUCCESS_SECONDS,
        (printf_ulong_t)delay_ticks);
    os_timer_sig_one_shot_restart_with_period(g_p_timer_sig_check_for_fw_updates, delay_ticks, false);
}

static void
main_task_handle_sig_schedule_retry_check_for_fw_updates(void)
{
    const os_delta_ticks_t delay_ticks = pdMS_TO_TICKS(RUUVI_CHECK_FOR_FW_UPDATES_DELAY_BEFORE_RETRY_SECONDS)
                                         * TIME_UNITS_MS_PER_SECOND;
    LOG_INFO(
        "Schedule a recheck for fw updates after %lu seconds (%lu ticks)",
        (printf_ulong_t)RUUVI_CHECK_FOR_FW_UPDATES_DELAY_BEFORE_RETRY_SECONDS,
        (printf_ulong_t)delay_ticks);
    os_timer_sig_one_shot_restart_with_period(g_p_timer_sig_check_for_fw_updates, delay_ticks, false);
}

static void
main_task_handle_sig_deferred_ethernet_activation(void)
{
    LOG_INFO("MAIN_TASK_SIG_DEFERRED_ETHERNET_ACTIVATION");
    if (wifi_manager_is_connected_to_ethernet())
    {
        LOG_INFO("Ethernet is already active");
        return;
    }
    LOG_INFO("%s: ### Start Ethernet", __func__);
    ethernet_start();
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void
main_task_handle_sig_wifi_ap_started(void)
{
    LOG_INFO("MAIN_TASK_SIG_WIFI_AP_STARTED");
}

static void
main_task_handle_sig_wifi_ap_stopped(void)
{
    LOG_INFO("MAIN_TASK_SIG_WIFI_AP_STOPPED");
}

static void
main_task_handle_sig_activate_cfg_mode(void)
{
    LOG_INFO("MAIN_TASK_SIG_ACTIVATE_CFG_MODE");

    if (gw_status_get_cfg_mode())
    {
        LOG_INFO("Configuration mode is already active");
        if (gw_status_is_waiting_auto_cfg_by_wps())
        {
            LOG_INFO("### ACTIVATE_CFG_MODE: Disable WPS");
            wifi_manager_disable_wps();
            gw_status_clear_waiting_auto_cfg_by_wps();
        }
        return;
    }

    gw_status_set_cfg_mode();

    if (os_timer_sig_periodic_is_active(g_p_timer_sig_check_for_remote_cfg))
    {
        main_task_stop_timer_check_for_remote_cfg();
    }

    if (os_timer_sig_one_shot_is_active(g_p_timer_sig_check_for_fw_updates))
    {
        main_task_timer_sig_check_for_fw_updates_stop();
    }

    const bool flag_wait_until_relaying_stopped = false;
    gw_status_suspend_relaying(flag_wait_until_relaying_stopped);

    event_mgr_notify(EVENT_MGR_EV_CFG_MODE_ACTIVATED);
}

static void
main_task_handle_sig_deactivate_cfg_mode(void)
{
    LOG_INFO("MAIN_TASK_SIG_DEACTIVATE_CFG_MODE");

    timer_cfg_mode_deactivation_stop();

    if (NULL != g_p_timer_sig_deferred_ethernet_activation)
    {
        LOG_INFO("DEACTIVATE_CFG_MODE: Stop Ethernet deferred activation timer");
        os_timer_sig_one_shot_stop(g_p_timer_sig_deferred_ethernet_activation);
        os_timer_sig_one_shot_delete(&g_p_timer_sig_deferred_ethernet_activation);
        g_p_timer_sig_deferred_ethernet_activation = NULL;
    }

    if (!gw_status_get_cfg_mode())
    {
        LOG_WARN("DEACTIVATE_CFG_MODE: Configuration mode is not active");
        return;
    }

    if (!gw_cfg_is_empty())
    {
        gw_status_clear_cfg_mode();
    }

    if (wifi_manager_is_ap_active())
    {
        LOG_INFO("### DEACTIVATE_CFG_MODE: Stop Wi-Fi AP");
        wifi_manager_stop_ap();
    }

    if (gw_status_is_waiting_auto_cfg_by_wps())
    {
        LOG_INFO("### DEACTIVATE_CFG_MODE: Disable WPS");
        wifi_manager_disable_wps();
        gw_status_clear_waiting_auto_cfg_by_wps();
    }

    if (!gw_status_is_network_connected())
    {
        leds_simulate_ev_network_disconnected();
    }
    // Simulate on_get_history to restart g_p_timer_sig_get_history_timeout and call leds_notify_http_poll_ok.
    // This will switch LED to 'G' immediately, and we don't need to wait for the next HTTP poll.
    LOG_INFO("DEACTIVATE_CFG_MODE: Simulate on_get_history");
    main_task_on_get_history();

    if (gw_cfg_is_empty() || gw_cfg_get_eth_use_eth() || (!wifi_manager_is_sta_configured()))
    {
        if (gw_cfg_is_empty())
        {
            LOG_INFO("DEACTIVATE_CFG_MODE: Gateway has not configured yet, start Ethernet");
        }
        else if (gw_cfg_get_eth_use_eth())
        {
            LOG_INFO("DEACTIVATE_CFG_MODE: Gateway is configured to use Ethernet, start Ethernet");
        }
        else
        {
            LOG_INFO("DEACTIVATE_CFG_MODE: Gateway is configured to use Wi-Fi, but SSID is not set, start Ethernet");
        }
        if (wifi_manager_is_connected_to_ethernet())
        {
            LOG_INFO("DEACTIVATE_CFG_MODE: Ethernet is already active");
        }
        else
        {
            LOG_INFO("%s: ### Start Ethernet", __func__);
            ethernet_start();
        }
    }
    else
    {
        LOG_INFO("DEACTIVATE_CFG_MODE: Connect to Wi-Fi");
        if (wifi_manager_is_sta_active())
        {
            LOG_INFO("DEACTIVATE_CFG_MODE: Wi-Fi STA is already active");
        }
        else
        {
            wifi_manager_connect_async();
        }
    }

    if (!gw_status_get_cfg_mode())
    {
        LOG_INFO("DEACTIVATE_CFG_MODE: Send signal to restart services");
        main_task_send_sig_restart_services();

        LOG_INFO("DEACTIVATE_CFG_MODE: Send notification: EV_CFG_MODE_DEACTIVATED");
        event_mgr_notify(EVENT_MGR_EV_CFG_MODE_DEACTIVATED);

        const bool flag_wait_until_relaying_resumed = false;
        LOG_INFO("DEACTIVATE_CFG_MODE: Resume relaying");
        gw_status_resume_relaying(flag_wait_until_relaying_resumed);
    }
    else
    {
        LOG_INFO("DEACTIVATE_CFG_MODE: Configuration mode is still active - do not restart services");
    }
}

static void
main_task_handle_sig_check_for_remote_cfg(void)
{
    LOG_INFO("Check for remote_cfg: activate");
    http_server_user_req(HTTP_SERVER_USER_REQ_CODE_DOWNLOAD_GW_CFG);
}

static void
start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "mdns_init failed");
        return;
    }

    const wifiman_hostname_t* const p_hostname = gw_cfg_get_hostname();

    err = mdns_hostname_set(p_hostname->buf);
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "mdns_hostname_set failed");
    }
    LOG_INFO("### Start mDNS: Hostname: \"%s\", Instance: \"%s\"", p_hostname->buf, p_hostname->buf);
    err = mdns_instance_name_set(p_hostname->buf);
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "mdns_instance_name_set failed");
    }
    const uint16_t http_port = 80U;
    err                      = mdns_service_add(NULL, "_http", "_tcp", http_port, NULL, 0);
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "mdns_service_add failed");
    }
}

static void
stop_mdns(void)
{
    LOG_INFO("### Stop mDNS");
    mdns_free();
}

static void
main_task_handle_sig_network_connected(void)
{
    LOG_INFO("### Handle event: NETWORK_CONNECTED");

    const force_start_wifi_hotspot_e force_start_wifi_hotspot = settings_read_flag_force_start_wifi_hotspot();
    if (FORCE_START_WIFI_HOTSPOT_DISABLED != force_start_wifi_hotspot)
    {
        /* The Wi-Fi access point must be started each time it is rebooted after the configuration has been erased
         * until it is connected to the network. */
        settings_write_flag_force_start_wifi_hotspot(FORCE_START_WIFI_HOTSPOT_DISABLED);
    }

    start_mdns();

    gw_cfg_remote_refresh_interval_minutes_t remote_cfg_refresh_interval_minutes = 0;
    const bool  flag_use_remote_cfg = gw_cfg_get_remote_cfg_use(&remote_cfg_refresh_interval_minutes);
    static bool g_flag_initial_request_for_remote_cfg_performed = false;
    if (flag_use_remote_cfg && (!g_flag_initial_request_for_remote_cfg_performed) && (!wifi_manager_is_ap_active()))
    {
        g_flag_initial_request_for_remote_cfg_performed = true;
        LOG_INFO("Activate checking for remote cfg");
        os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_CHECK_FOR_REMOTE_CFG));
    }
}

static void
main_task_handle_sig_network_disconnected(void)
{
    LOG_INFO("### Handle event: NETWORK_DISCONNECTED");
    stop_mdns();
}

static void
main_task_handle_sig_task_watchdog_feed(void)
{
    LOG_DBG("Feed watchdog");
    const esp_err_t err = esp_task_wdt_reset();
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "%s failed", "esp_task_wdt_reset");
    }
}

static void
main_task_handle_sig_network_reconnect(void)
{
    LOG_INFO("Perform network reconnect");
    if (gw_cfg_get_eth_use_eth())
    {
        LOG_INFO("%s: ### Stop Ethernet", __func__);
        ethernet_stop();
        LOG_INFO("%s: ### Start Ethernet", __func__);
        ethernet_start();
    }
    else
    {
        if (wifi_manager_is_sta_active())
        {
            wifi_manager_disconnect_wifi();
            wifi_manager_connect_async();
        }
    }
}

void
main_task_handle_sig_set_default_config(void)
{
    LOG_INFO("### Set default config");
    gw_cfg_t* p_gw_cfg = os_calloc(1, sizeof(*p_gw_cfg));
    if (NULL == p_gw_cfg)
    {
        LOG_ERR("Can't allocate memory for gw_cfg");
        return;
    }
    gw_cfg_default_get(p_gw_cfg);
    gw_cfg_log(p_gw_cfg, "Gateway SETTINGS", false);
    (void)gw_cfg_update(p_gw_cfg);
    os_free(p_gw_cfg);
    main_task_send_sig_deactivate_cfg_mode();
}

static void
main_task_handle_sig_restart_services(void)
{
    LOG_INFO("Restart services");
    mqtt_app_stop();
    if (gw_cfg_get_mqtt_use_mqtt() && gw_status_is_relaying_via_mqtt_enabled())
    {
        mqtt_app_start_with_gw_cfg();
    }

    main_task_configure_periodic_remote_cfg_check();

    if (AUTO_UPDATE_CYCLE_TYPE_MANUAL != gw_cfg_get_auto_update_cycle())
    {
        const os_delta_ticks_t delay_ticks = pdMS_TO_TICKS(RUUVI_CHECK_FOR_FW_UPDATES_DELAY_BEFORE_RETRY_SECONDS)
                                             * TIME_UNITS_MS_PER_SECOND;
        LOG_INFO(
            "Restarting services: Restart firmware auto-updating, run next check after %lu seconds",
            (printf_ulong_t)RUUVI_CHECK_FOR_FW_UPDATES_DELAY_AFTER_REBOOT_SECONDS);
        main_task_timer_sig_check_for_fw_updates_restart(delay_ticks);
    }
    else
    {
        LOG_INFO("Restarting services: Stop firmware auto-updating");
        main_task_timer_sig_check_for_fw_updates_stop();
    }
}

static void
main_task_handle_sig_relaying_mode_changed(void)
{
    LOG_INFO("Relaying mode changed");

    if (gw_cfg_get_mqtt_use_mqtt())
    {
        if (gw_status_is_relaying_via_mqtt_enabled())
        {
            if (!gw_status_is_mqtt_started())
            {
                mqtt_app_start_with_gw_cfg();
            }
        }
        else
        {
            mqtt_app_stop();
        }
    }
    else
    {
        mqtt_app_stop();
    }
    gw_status_clear_mqtt_relaying_cmd();
    main_task_send_sig_log_runtime_stat();
}

static void
main_task_handle_sig(const main_task_sig_e main_task_sig)
{
    switch (main_task_sig)
    {
        case MAIN_TASK_SIG_LOG_HEAP_USAGE:
            main_task_handle_sig_log_heap_usage();
            break;
        case MAIN_TASK_SIG_CHECK_FOR_FW_UPDATES:
            main_task_handle_sig_check_for_fw_updates();
            break;
        case MAIN_TASK_SIG_SCHEDULE_NEXT_CHECK_FOR_FW_UPDATES:
            main_task_handle_sig_schedule_next_check_for_fw_updates();
            break;
        case MAIN_TASK_SIG_SCHEDULE_RETRY_CHECK_FOR_FW_UPDATES:
            main_task_handle_sig_schedule_retry_check_for_fw_updates();
            break;
        case MAIN_TASK_SIG_DEFERRED_ETHERNET_ACTIVATION:
            main_task_handle_sig_deferred_ethernet_activation();
            break;
        case MAIN_TASK_SIG_WIFI_AP_STARTED:
            main_task_handle_sig_wifi_ap_started();
            break;
        case MAIN_TASK_SIG_WIFI_AP_STOPPED:
            main_task_handle_sig_wifi_ap_stopped();
            break;
        case MAIN_TASK_SIG_ACTIVATE_CFG_MODE:
            main_task_handle_sig_activate_cfg_mode();
            break;
        case MAIN_TASK_SIG_DEACTIVATE_CFG_MODE:
            main_task_handle_sig_deactivate_cfg_mode();
            break;
        case MAIN_TASK_SIG_TASK_RESTART_SERVICES:
            main_task_handle_sig_restart_services();
            break;
        case MAIN_TASK_SIG_CHECK_FOR_REMOTE_CFG:
            main_task_handle_sig_check_for_remote_cfg();
            break;
        case MAIN_TASK_SIG_NETWORK_CONNECTED:
            main_task_handle_sig_network_connected();
            break;
        case MAIN_TASK_SIG_NETWORK_DISCONNECTED:
            main_task_handle_sig_network_disconnected();
            break;
        case MAIN_TASK_SIG_RECONNECT_NETWORK:
            main_task_handle_sig_network_reconnect();
            break;
        case MAIN_TASK_SIG_RELAYING_MODE_CHANGED:
            main_task_handle_sig_relaying_mode_changed();
            break;
        case MAIN_TASK_SIG_SET_DEFAULT_CONFIG:
            main_task_handle_sig_set_default_config();
            break;
        case MAIN_TASK_SIG_ON_GET_HISTORY:
            LOG_INFO("MAIN_TASK_SIG_ON_GET_HISTORY");
            os_timer_sig_one_shot_stop(g_p_timer_sig_get_history_timeout);
            os_timer_sig_one_shot_start(g_p_timer_sig_get_history_timeout);
            leds_notify_http_poll_ok();
            break;
        case MAIN_TASK_SIG_ON_GET_HISTORY_TIMEOUT:
            LOG_INFO("MAIN_TASK_SIG_ON_GET_HISTORY_TIMEOUT");
            leds_notify_http_poll_timeout();
            break;
        case MAIN_TASK_SIG_LOG_RUNTIME_STAT:
            log_runtime_statistics();
            break;
        case MAIN_TASK_SIG_TASK_WATCHDOG_FEED:
            main_task_handle_sig_task_watchdog_feed();
            break;
    }
}

static void
main_wdt_add_and_start(void)
{
    LOG_INFO("TaskWatchdog: Register current thread");
    const esp_err_t err = esp_task_wdt_add(xTaskGetCurrentTaskHandle());
    if (ESP_OK != err)
    {
        LOG_ERR_ESP(err, "%s failed", "esp_task_wdt_add");
    }
    LOG_INFO("TaskWatchdog: Start timer");
    os_timer_sig_periodic_start(g_p_timer_sig_task_watchdog_feed);
}

void
main_task_configure_periodic_remote_cfg_check(void)
{
    gw_cfg_remote_refresh_interval_minutes_t remote_cfg_refresh_interval_minutes = 0;

    const bool flag_use_remote_cfg = gw_cfg_get_remote_cfg_use(&remote_cfg_refresh_interval_minutes);
    if (flag_use_remote_cfg)
    {
        if (0 != remote_cfg_refresh_interval_minutes)
        {
            LOG_INFO(
                "Reading of the configuration from the remote server is active, period: %u minutes",
                (printf_uint_t)remote_cfg_refresh_interval_minutes);
            os_timer_sig_periodic_restart_with_period(
                g_p_timer_sig_check_for_remote_cfg,
                pdMS_TO_TICKS(
                    remote_cfg_refresh_interval_minutes * TIME_UNITS_SECONDS_PER_MINUTE * TIME_UNITS_MS_PER_SECOND),
                false);
        }
        else
        {
            LOG_WARN("Reading of the configuration from the remote server is active, but period is not set");
            os_timer_sig_periodic_stop(g_p_timer_sig_check_for_remote_cfg);
        }
    }
    else
    {
        LOG_INFO("### Reading of the configuration from the remote server is not active");
        os_timer_sig_periodic_stop(g_p_timer_sig_check_for_remote_cfg);
    }
}

ATTR_NORETURN
void
main_loop(void)
{
    LOG_INFO("Main loop started");
    main_wdt_add_and_start();

    if (gw_cfg_get_mqtt_use_mqtt())
    {
        mqtt_app_start_with_gw_cfg();
    }

    os_timer_sig_periodic_start(g_p_timer_sig_log_heap_usage);
    os_timer_sig_periodic_start(g_p_timer_sig_log_runtime_stat);
    os_timer_sig_one_shot_start(g_p_timer_sig_get_history_timeout);

    main_task_configure_periodic_remote_cfg_check();

    if (AUTO_UPDATE_CYCLE_TYPE_MANUAL != gw_cfg_get_auto_update_cycle())
    {
        LOG_INFO(
            "### Firmware auto-updating is active, run next check after %lu seconds",
            (printf_ulong_t)RUUVI_CHECK_FOR_FW_UPDATES_DELAY_AFTER_REBOOT_SECONDS);
        os_timer_sig_one_shot_start(g_p_timer_sig_check_for_fw_updates);
    }
    else
    {
        LOG_INFO("Firmware auto-updating is not active");
    }

    main_task_send_sig_log_runtime_stat();

    for (;;)
    {
        os_signal_events_t sig_events = { 0 };
        if (!os_signal_wait_with_timeout(g_p_signal_main_task, OS_DELTA_TICKS_INFINITE, &sig_events))
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
            const main_task_sig_e main_task_sig = main_task_conv_from_sig_num(sig_num);
            main_task_handle_sig(main_task_sig);
        }
    }
}

static void
main_task_init_signals(void)
{
    g_p_signal_main_task = os_signal_create_static(&g_signal_main_task_mem);
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_LOG_HEAP_USAGE));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_CHECK_FOR_FW_UPDATES));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_SCHEDULE_NEXT_CHECK_FOR_FW_UPDATES));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_SCHEDULE_RETRY_CHECK_FOR_FW_UPDATES));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_DEFERRED_ETHERNET_ACTIVATION));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_WIFI_AP_STARTED));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_WIFI_AP_STOPPED));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_ACTIVATE_CFG_MODE));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_DEACTIVATE_CFG_MODE));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_TASK_RESTART_SERVICES));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_CHECK_FOR_REMOTE_CFG));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_NETWORK_CONNECTED));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_NETWORK_DISCONNECTED));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_RECONNECT_NETWORK));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_SET_DEFAULT_CONFIG));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_ON_GET_HISTORY));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_ON_GET_HISTORY_TIMEOUT));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_RELAYING_MODE_CHANGED));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_LOG_RUNTIME_STAT));
    os_signal_add(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_TASK_WATCHDOG_FEED));
}

void
main_task_init_timers(void)
{
    g_p_timer_sig_log_heap_usage = os_timer_sig_periodic_create_static(
        &g_timer_sig_log_heap_usage,
        "log_heap_usage",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_LOG_HEAP_USAGE),
        pdMS_TO_TICKS(MAIN_TASK_LOG_HEAP_STAT_PERIOD_MS));
    g_p_timer_sig_check_for_fw_updates = os_timer_sig_one_shot_create_static(
        &g_timer_sig_check_for_fw_updates_mem,
        "check_fw_updates",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_CHECK_FOR_FW_UPDATES),
        pdMS_TO_TICKS(RUUVI_CHECK_FOR_FW_UPDATES_DELAY_AFTER_REBOOT_SECONDS) * TIME_UNITS_MS_PER_SECOND);

    g_p_timer_sig_deactivate_cfg_mode = os_timer_sig_one_shot_create_static(
        &g_p_timer_sig_deactivate_cfg_mode_mem,
        "stop_cfg_mode",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_DEACTIVATE_CFG_MODE),
        pdMS_TO_TICKS(RUUVI_CFG_MODE_DEACTIVATION_DEFAULT_DELAY_SEC * TIME_UNITS_MS_PER_SECOND));

    g_p_timer_sig_check_for_remote_cfg = os_timer_sig_periodic_create_static(
        &g_timer_sig_check_for_remote_cfg_mem,
        "remote_cfg",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_CHECK_FOR_REMOTE_CFG),
        pdMS_TO_TICKS(MAIN_TASK_CHECK_FOR_REMOTE_CFG_PERIOD_MS));

    g_p_timer_sig_get_history_timeout = os_timer_sig_one_shot_create_static(
        &g_timer_sig_get_history_timeout_mem,
        "main_hist",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_ON_GET_HISTORY_TIMEOUT),
        pdMS_TO_TICKS(MAIN_TASK_GET_HISTORY_TIMEOUT_MS));

    g_p_timer_sig_log_runtime_stat = os_timer_sig_periodic_create_static(
        &g_timer_sig_log_runtime_stat,
        "log_runtime_stat",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_LOG_RUNTIME_STAT),
        pdMS_TO_TICKS(MAIN_TASK_LOG_RUNTIME_STAT_PERIOD_MS));

    g_p_timer_sig_deferred_ethernet_activation = os_timer_sig_one_shot_create_static(
        &g_timer_sig_deferred_ethernet_activation_mem,
        "deferred_eth",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_DEFERRED_ETHERNET_ACTIVATION),
        pdMS_TO_TICKS(
            pdMS_TO_TICKS(RUUVI_DELAY_BEFORE_ETHERNET_ACTIVATION_ON_FIRST_BOOT_SEC * TIME_UNITS_MS_PER_SECOND)));

    g_p_timer_sig_task_watchdog_feed = os_timer_sig_periodic_create_static(
        &g_timer_sig_task_watchdog_feed_mem,
        "main_wgod",
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_TASK_WATCHDOG_FEED),
        pdMS_TO_TICKS(MAIN_TASK_WATCHDOG_FEED_PERIOD_MS));
}

void
main_task_subscribe_events(void)
{
    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_wifi_connected,
        EVENT_MGR_EV_WIFI_CONNECTED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_NETWORK_CONNECTED));

    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_eth_connected,
        EVENT_MGR_EV_ETH_CONNECTED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_NETWORK_CONNECTED));

    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_wifi_disconnected,
        EVENT_MGR_EV_WIFI_DISCONNECTED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_NETWORK_DISCONNECTED));

    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_eth_disconnected,
        EVENT_MGR_EV_ETH_DISCONNECTED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_NETWORK_DISCONNECTED));

    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_relaying_mode_changed,
        EVENT_MGR_EV_RELAYING_MODE_CHANGED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_RELAYING_MODE_CHANGED));

    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_ap_started,
        EVENT_MGR_EV_WIFI_AP_STARTED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_WIFI_AP_STARTED));

    event_mgr_subscribe_sig_static(
        &g_main_loop_ev_info_mem_ap_stopped,
        EVENT_MGR_EV_WIFI_AP_STOPPED,
        g_p_signal_main_task,
        main_task_conv_to_sig_num(MAIN_TASK_SIG_WIFI_AP_STOPPED));
}

bool
main_loop_init(void)
{
    main_task_init_signals();
    if (!os_signal_register_cur_thread(g_p_signal_main_task))
    {
        LOG_ERR("%s failed", "os_signal_register_cur_thread");
        return false;
    }
    return true;
}

void
main_task_schedule_next_check_for_fw_updates(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_SCHEDULE_NEXT_CHECK_FOR_FW_UPDATES));
}

void
main_task_schedule_retry_check_for_fw_updates(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_SCHEDULE_RETRY_CHECK_FOR_FW_UPDATES));
}

void
main_task_send_sig_restart_services(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_TASK_RESTART_SERVICES));
}

void
main_task_send_sig_activate_cfg_mode(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_ACTIVATE_CFG_MODE));
}

void
main_task_send_sig_deactivate_cfg_mode(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_DEACTIVATE_CFG_MODE));
}

void
main_task_send_sig_reconnect_network(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_RECONNECT_NETWORK));
}

void
main_task_send_sig_set_default_config(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_SET_DEFAULT_CONFIG));
}

void
main_task_send_sig_log_runtime_stat(void)
{
    os_timer_sig_periodic_simulate(g_p_timer_sig_log_runtime_stat);
}

void
main_task_timer_sig_check_for_fw_updates_restart(const os_delta_ticks_t delay_ticks)
{
    LOG_INFO("### Start timer: Check for firmware updates");
    os_timer_sig_one_shot_restart_with_period(g_p_timer_sig_check_for_fw_updates, delay_ticks, false);
}

void
main_task_timer_sig_check_for_fw_updates_stop(void)
{
    LOG_INFO("### Stop timer: Check for firmware updates");
    os_timer_sig_one_shot_stop(g_p_timer_sig_check_for_fw_updates);
}

void
main_task_start_timer_activation_ethernet_after_timeout(void)
{
    LOG_INFO("### Start timer: Activate Ethernet after timeout");
    os_timer_sig_one_shot_start(g_p_timer_sig_deferred_ethernet_activation);
}

void
main_task_stop_timer_activation_ethernet_after_timeout(void)
{
    LOG_INFO("### Stop timer: Activate Ethernet after timeout");
    os_timer_sig_one_shot_stop(g_p_timer_sig_deferred_ethernet_activation);
}

void
timer_cfg_mode_deactivation_start_with_delay(const TimeUnitsSeconds_t delay_sec)
{
    LOG_INFO("### Start timer for deactivation of configuration mode after timeout (%u seconds)", delay_sec);
    os_timer_sig_one_shot_stop(g_p_timer_sig_deactivate_cfg_mode);
    os_timer_sig_one_shot_restart_with_period(
        g_p_timer_sig_deactivate_cfg_mode,
        pdMS_TO_TICKS(delay_sec * TIME_UNITS_MS_PER_SECOND),
        false);
}

void
timer_cfg_mode_deactivation_start(void)
{
    timer_cfg_mode_deactivation_start_with_delay(RUUVI_CFG_MODE_DEACTIVATION_DEFAULT_DELAY_SEC);
}

void
timer_cfg_mode_deactivation_stop(void)
{
    LOG_INFO("### Stop the timer for the deactivation of Configuration Mode");
    os_timer_sig_one_shot_stop(g_p_timer_sig_deactivate_cfg_mode);
}

bool
timer_cfg_mode_deactivation_is_active(void)
{
    return os_timer_sig_one_shot_is_active(g_p_timer_sig_deactivate_cfg_mode);
}

void
main_task_stop_timer_check_for_remote_cfg(void)
{
    LOG_INFO("Stop timer: Check for remote cfg");
    os_timer_sig_periodic_stop(g_p_timer_sig_check_for_remote_cfg);
}

void
main_task_on_get_history(void)
{
    os_signal_send(g_p_signal_main_task, main_task_conv_to_sig_num(MAIN_TASK_SIG_ON_GET_HISTORY));
}

static void
start_wifi_ap_internal(const bool flag_block_req_from_lan)
{
    LOG_INFO("Send command to start Wi-Fi AP to Wi-Fi Manager");
    wifi_manager_start_ap(flag_block_req_from_lan);
    main_task_send_sig_activate_cfg_mode();
}

void
start_wifi_ap(void)
{
    const bool flag_block_req_from_lan = true;
    start_wifi_ap_internal(flag_block_req_from_lan);
}

void
start_wifi_ap_without_blocking_req_from_lan(void)
{
    const bool flag_block_req_from_lan = false;
    start_wifi_ap_internal(flag_block_req_from_lan);
}
