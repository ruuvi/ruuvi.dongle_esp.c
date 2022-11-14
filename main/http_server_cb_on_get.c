/**
 * @file http_server_cb_on_get.c
 * @author TheSomeMan
 * @date 2020-10-26
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "http_server_cb.h"
#include <string.h>
#include <stdlib.h>
#include "os_malloc.h"
#include "gw_cfg_ruuvi_json.h"
#include "http_server_resp.h"
#include "reset_task.h"
#include "metrics.h"
#include "time_task.h"
#include "adv_post.h"
#include "flashfatfs.h"
#include "ruuvi_gateway.h"

#if RUUVI_TESTS_HTTP_SERVER_CB
#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#else
#define LOG_LOCAL_LEVEL LOG_LEVEL_INFO
#endif
#include "log.h"

#if (LOG_LOCAL_LEVEL >= LOG_LEVEL_DEBUG) && !RUUVI_TESTS
#warning Debug log level prints out the passwords as a "plaintext".
#endif

#define HTTP_SERVER_DEFAULT_HISTORY_INTERVAL_SECONDS (60U)

typedef double cjson_double_t;

extern const flash_fat_fs_t* gp_ffs_gwui;

static const char TAG[] = "http_server";

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_json_ruuvi(void)
{
    const gw_cfg_t*  p_gw_cfg = gw_cfg_lock_ro();
    cjson_wrap_str_t json_str = cjson_wrap_str_null();
    if (!gw_cfg_ruuvi_json_generate(p_gw_cfg, &json_str))
    {
        gw_cfg_unlock_ro(&p_gw_cfg);
        return http_server_resp_503();
    }
    gw_cfg_unlock_ro(&p_gw_cfg);

    LOG_INFO("ruuvi.json: %s", json_str.p_str);
    const bool flag_no_cache        = true;
    const bool flag_add_header_date = true;
    return http_server_resp_data_in_heap(
        HTTP_CONENT_TYPE_APPLICATION_JSON,
        NULL,
        strlen(json_str.p_str),
        HTTP_CONENT_ENCODING_NONE,
        (const uint8_t*)json_str.p_str,
        flag_no_cache,
        flag_add_header_date);
}

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_json_github_latest_release(void)
{
    const http_server_download_info_t info = http_download_latest_release_info();
    if (info.is_error)
    {
        return http_server_resp_504();
    }

    LOG_DBG("github_latest_release.json: %s", info.p_json_buf);
    const bool flag_no_cache        = true;
    const bool flag_add_header_date = true;
    return http_server_resp_data_in_heap(
        HTTP_CONENT_TYPE_APPLICATION_JSON,
        NULL,
        strlen(info.p_json_buf),
        HTTP_CONENT_ENCODING_NONE,
        (const uint8_t*)info.p_json_buf,
        flag_no_cache,
        flag_add_header_date);
}

static bool
json_info_add_string(cJSON* p_json_root, const char* p_item_name, const char* p_val)
{
    if (NULL == cJSON_AddStringToObject(p_json_root, p_item_name, p_val))
    {
        LOG_ERR("Can't add json item: %s", p_item_name);
        return false;
    }
    return true;
}

static bool
json_info_add_uint32(cJSON* p_json_root, const char* p_item_name, const uint32_t val)
{
    if (NULL == cJSON_AddNumberToObject(p_json_root, p_item_name, (cjson_double_t)val))
    {
        LOG_ERR("Can't add json item: %s", p_item_name);
        return false;
    }
    return true;
}

static bool
json_info_add_items(cJSON* p_json_root, const bool flag_use_timestamps)
{
    if (!json_info_add_string(p_json_root, "ESP_FW", gw_cfg_get_esp32_fw_ver()->buf))
    {
        return false;
    }
    if (!json_info_add_string(p_json_root, "NRF_FW", gw_cfg_get_nrf52_fw_ver()->buf))
    {
        return false;
    }
    if (!json_info_add_string(p_json_root, "DEVICE_ADDR", gw_cfg_get_nrf52_mac_addr()->str_buf))
    {
        return false;
    }
    if (!json_info_add_string(p_json_root, "DEVICE_ID", gw_cfg_get_nrf52_device_id()->str_buf))
    {
        return false;
    }
    if (!json_info_add_string(p_json_root, "ETHERNET_MAC", gw_cfg_get_esp32_mac_addr_eth()->str_buf))
    {
        return false;
    }
    if (!json_info_add_string(p_json_root, "WIFI_MAC", gw_cfg_get_esp32_mac_addr_wifi()->str_buf))
    {
        return false;
    }
    const time_t        cur_time  = http_server_get_cur_time();
    adv_report_table_t* p_reports = os_malloc(sizeof(*p_reports));
    if (NULL == p_reports)
    {
        return false;
    }
    const bool flag_use_filter = flag_use_timestamps;
    adv_table_history_read(
        p_reports,
        cur_time,
        flag_use_timestamps,
        flag_use_timestamps ? HTTP_SERVER_DEFAULT_HISTORY_INTERVAL_SECONDS : 0,
        flag_use_filter);
    const num_of_advs_t num_of_advs = p_reports->num_of_advs;
    os_free(p_reports);

    if (!json_info_add_uint32(p_json_root, "TAGS_SEEN", num_of_advs))
    {
        return false;
    }
    if (!json_info_add_uint32(p_json_root, "BUTTON_PRESSES", g_cnt_cfg_button_pressed))
    {
        return false;
    }
    return true;
}

static bool
generate_json_info_str(cjson_wrap_str_t* p_json_str, const bool flag_use_timestamps)
{
    p_json_str->p_str = NULL;

    cJSON* p_json_root = cJSON_CreateObject();
    if (NULL == p_json_root)
    {
        LOG_ERR("Can't create json object");
        return false;
    }
    if (!json_info_add_items(p_json_root, flag_use_timestamps))
    {
        cjson_wrap_delete(&p_json_root);
        return false;
    }

    *p_json_str = cjson_wrap_print_and_delete(&p_json_root);
    if (NULL == p_json_str->p_str)
    {
        LOG_ERR("Can't create json string");
        return false;
    }
    return true;
}

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_json_info(void)
{
    const gw_cfg_t*  p_gw_cfg = gw_cfg_lock_ro();
    cjson_wrap_str_t json_str = cjson_wrap_str_null();
    if (!generate_json_info_str(&json_str, gw_cfg_get_ntp_use()))
    {
        gw_cfg_unlock_ro(&p_gw_cfg);
        return http_server_resp_503();
    }
    gw_cfg_unlock_ro(&p_gw_cfg);
    LOG_INFO("info.json: %s", json_str.p_str);
    const bool flag_no_cache        = true;
    const bool flag_add_header_date = true;
    return http_server_resp_data_in_heap(
        HTTP_CONENT_TYPE_APPLICATION_JSON,
        NULL,
        strlen(json_str.p_str),
        HTTP_CONENT_ENCODING_NONE,
        (const uint8_t*)json_str.p_str,
        flag_no_cache,
        flag_add_header_date);
}

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_json(const char* p_file_name, const bool flag_access_from_lan)
{
    if (0 == strcmp(p_file_name, "ruuvi.json"))
    {
        return http_server_resp_json_ruuvi();
    }
    if (0 == strcmp(p_file_name, "github_latest_release.json"))
    {
        return http_server_resp_json_github_latest_release();
    }
    if ((0 == strcmp(p_file_name, "info.json")) && (!flag_access_from_lan))
    {
        return http_server_resp_json_info();
    }
    LOG_WARN("Request to unknown json: %s", p_file_name);
    return http_server_resp_404();
}

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_metrics(void)
{
    const char* p_metrics = metrics_generate();
    if (NULL == p_metrics)
    {
        LOG_ERR("Not enough memory");
        return http_server_resp_503();
    }
    const bool flag_no_cache        = true;
    const bool flag_add_header_date = true;
    LOG_INFO("metrics: %s", p_metrics);
    return http_server_resp_data_in_heap(
        HTTP_CONENT_TYPE_TEXT_PLAIN,
        "version=0.0.4",
        strlen(p_metrics),
        HTTP_CONENT_ENCODING_NONE,
        (const uint8_t*)p_metrics,
        flag_no_cache,
        flag_add_header_date);
}

HTTP_SERVER_CB_STATIC
void
http_server_get_filter_from_params(
    const char* const p_params,
    const bool        flag_use_timestamps,
    const bool        flag_time_is_synchronized,
    bool*             p_flag_use_filter,
    uint32_t*         p_filter)
{
    if (flag_use_timestamps)
    {
        const char* const p_time_prefix   = "time=";
        const size_t      time_prefix_len = strlen(p_time_prefix);
        if (0 == strncmp(p_params, p_time_prefix, time_prefix_len))
        {
            *p_filter = (uint32_t)strtoul(&p_params[time_prefix_len], NULL, 0);
            if (flag_time_is_synchronized)
            {
                *p_flag_use_filter = true;
            }
        }
    }
    else
    {
        const char* const p_counter_prefix   = "counter=";
        const size_t      counter_prefix_len = strlen(p_counter_prefix);
        if (0 == strncmp(p_params, p_counter_prefix, counter_prefix_len))
        {
            *p_filter          = (uint32_t)strtoul(&p_params[counter_prefix_len], NULL, 0);
            *p_flag_use_filter = true;
        }
    }
}

HTTP_SERVER_CB_STATIC
bool
http_server_read_history(
    cjson_wrap_str_t*    p_json_str,
    const time_t         cur_time,
    const bool           flag_use_timestamps,
    const uint32_t       filter,
    const bool           flag_use_filter,
    num_of_advs_t* const p_num_of_advs)
{
    adv_report_table_t* p_reports = os_malloc(sizeof(*p_reports));
    if (NULL == p_reports)
    {
        return false;
    }
    adv_table_history_read(p_reports, cur_time, flag_use_timestamps, filter, flag_use_filter);
    *p_num_of_advs = p_reports->num_of_advs;

    const ruuvi_gw_cfg_coordinates_t coordinates = gw_cfg_get_coordinates();

    const bool res = http_json_create_records_str(
        p_reports,
        (http_json_header_info_t) {
            .flag_use_timestamps = flag_use_timestamps,
            .timestamp           = cur_time,
            .p_mac_addr          = gw_cfg_get_nrf52_mac_addr(),
            .p_coordinates_str   = coordinates.buf,
            .flag_use_nonce      = false,
            .nonce               = 0,
        },
        p_json_str);

    os_free(p_reports);
    return res;
}

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_history(const char* const p_params)
{
    const bool flag_use_timestamps       = gw_cfg_get_ntp_use();
    const bool flag_time_is_synchronized = time_is_synchronized();
    uint32_t   filter                    = flag_use_timestamps ? HTTP_SERVER_DEFAULT_HISTORY_INTERVAL_SECONDS : 0;
    bool       flag_use_filter           = (flag_use_timestamps && flag_time_is_synchronized) ? true : false;
    if (NULL != p_params)
    {
        http_server_get_filter_from_params(
            p_params,
            flag_use_timestamps,
            flag_time_is_synchronized,
            &flag_use_filter,
            &filter);
    }
    cjson_wrap_str_t json_str    = cjson_wrap_str_null();
    const time_t     cur_time    = http_server_get_cur_time();
    num_of_advs_t    num_of_advs = 0;
    if (!http_server_read_history(&json_str, cur_time, flag_use_timestamps, filter, flag_use_filter, &num_of_advs))
    {
        LOG_ERR("Not enough memory");
        return http_server_resp_503();
    }

    const bool flag_no_cache        = true;
    const bool flag_add_header_date = true;
    if (flag_use_filter)
    {
        if (flag_use_timestamps)
        {
            LOG_INFO("History on %u seconds interval: %s", (unsigned)filter, json_str.p_str);
        }
        else
        {
            LOG_INFO("History starting from counter %u: %s", (unsigned)filter, json_str.p_str);
        }
    }
    else
    {
        LOG_INFO("History (without filtering): %s", json_str.p_str);
    }
    if (0 != num_of_advs)
    {
        adv_post_last_successful_network_comm_timestamp_update();
    }

    main_task_on_get_history();

    return http_server_resp_data_in_heap(
        HTTP_CONENT_TYPE_APPLICATION_JSON,
        NULL,
        strlen(json_str.p_str),
        HTTP_CONENT_ENCODING_NONE,
        (const uint8_t*)json_str.p_str,
        flag_no_cache,
        flag_add_header_date);
}

HTTP_SERVER_CB_STATIC
http_content_type_e
http_get_content_type_by_ext(const char* p_file_ext)
{
    if (0 == strcmp(p_file_ext, ".html"))
    {
        return HTTP_CONENT_TYPE_TEXT_HTML;
    }
    if ((0 == strcmp(p_file_ext, ".css")) || (0 == strcmp(p_file_ext, ".scss")))
    {
        return HTTP_CONENT_TYPE_TEXT_CSS;
    }
    if (0 == strcmp(p_file_ext, ".js"))
    {
        return HTTP_CONENT_TYPE_TEXT_JAVASCRIPT;
    }
    if (0 == strcmp(p_file_ext, ".png"))
    {
        return HTTP_CONENT_TYPE_IMAGE_PNG;
    }
    if (0 == strcmp(p_file_ext, ".svg"))
    {
        return HTTP_CONENT_TYPE_IMAGE_SVG_XML;
    }
    if (0 == strcmp(p_file_ext, ".ttf"))
    {
        return HTTP_CONENT_TYPE_APPLICATION_OCTET_STREAM;
    }
    return HTTP_CONENT_TYPE_APPLICATION_OCTET_STREAM;
}

HTTP_SERVER_CB_STATIC
http_server_resp_t
http_server_resp_file(const char* file_path, const http_resp_code_e http_resp_code)
{
    LOG_DBG("Try to find file: %s", file_path);
    if (NULL == gp_ffs_gwui)
    {
        LOG_ERR("GWUI partition is not ready");
        return http_server_resp_503();
    }
    const flash_fat_fs_t* p_ffs = gp_ffs_gwui;

    const char* p_file_ext = strrchr(file_path, '.');
    if (NULL == p_file_ext)
    {
        p_file_ext = "";
    }

    size_t       file_size         = 0;
    bool         is_gzipped        = false;
    char         tmp_file_path[64] = { '\0' };
    const char*  suffix_gz         = ".gz";
    const size_t suffix_gz_len     = strlen(suffix_gz);
    if ((strlen(file_path) + suffix_gz_len) >= sizeof(tmp_file_path))
    {
        LOG_ERR("Temporary buffer is not enough for the file path '%s'", file_path);
        return http_server_resp_503();
    }
    if ((0 == strcmp(".js", p_file_ext)) || (0 == strcmp(".html", p_file_ext)) || (0 == strcmp(".css", p_file_ext)))
    {
        snprintf(tmp_file_path, sizeof(tmp_file_path), "%s%s", file_path, suffix_gz);
        if (flashfatfs_get_file_size(p_ffs, tmp_file_path, &file_size))
        {
            is_gzipped = true;
        }
    }
    if (!is_gzipped)
    {
        snprintf(tmp_file_path, sizeof(tmp_file_path), "%s", file_path);
        if (!flashfatfs_get_file_size(p_ffs, tmp_file_path, &file_size))
        {
            LOG_ERR("Can't find file: %s", tmp_file_path);
            return http_server_resp_404();
        }
    }
    const http_content_type_e     content_type     = http_get_content_type_by_ext(p_file_ext);
    const http_content_encoding_e content_encoding = is_gzipped ? HTTP_CONENT_ENCODING_GZIP : HTTP_CONENT_ENCODING_NONE;

    const file_descriptor_t fd = flashfatfs_open(p_ffs, tmp_file_path);
    if (fd < 0)
    {
        LOG_ERR("Can't open file: %s", tmp_file_path);
        return http_server_resp_503();
    }
    LOG_DBG("File %s was opened successfully, fd=%d", tmp_file_path, fd);
    const bool flag_no_cache = true;
    return http_server_resp_data_from_file(
        http_resp_code,
        content_type,
        NULL,
        file_size,
        content_encoding,
        fd,
        flag_no_cache);
}

http_server_resp_t
http_server_cb_on_get(
    const char* const               p_path,
    const char* const               p_uri_params,
    const bool                      flag_access_from_lan,
    const http_server_resp_t* const p_resp_auth)
{
    const char* p_file_ext = strrchr(p_path, '.');
    LOG_DBG(
        "http_server_cb_on_get /%s%s%s",
        p_path,
        (NULL != p_uri_params) ? "?" : "",
        (NULL != p_uri_params) ? p_uri_params : "");

    if ((NULL != p_file_ext) && (0 == strcmp(p_file_ext, ".json")))
    {
        return http_server_resp_json(p_path, flag_access_from_lan);
    }
    if (0 == strcmp(p_path, "metrics"))
    {
        return http_server_resp_metrics();
    }
    if (0 == strcmp(p_path, "history"))
    {
        return http_server_resp_history(p_uri_params);
    }
    const char* p_file_path = ('\0' == p_path[0]) ? "ruuvi.html" : p_path;
    return http_server_resp_file(p_file_path, (NULL != p_resp_auth) ? p_resp_auth->http_resp_code : HTTP_RESP_CODE_200);
}