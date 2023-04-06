/**
 * @file http_json.c
 * @author TheSomeMan
 * @date 2021-02-03
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "http_json.h"
#include "bin2hex.h"
#include "os_malloc.h"
#include "runtime_stat.h"

static cJSON*
http_json_generate_records_data_attributes(cJSON* const p_json_data, const http_json_header_info_t header_info)
{
    if (NULL == cJSON_AddStringToObject(p_json_data, "coordinates", header_info.p_coordinates_str))
    {
        return NULL;
    }
    if (header_info.flag_use_timestamps && (!cjson_wrap_add_timestamp(p_json_data, "timestamp", header_info.timestamp)))
    {
        return NULL;
    }
    if (header_info.flag_use_nonce && (!cjson_wrap_add_uint32(p_json_data, "nonce", header_info.nonce)))
    {
        return NULL;
    }
    if (NULL == cJSON_AddStringToObject(p_json_data, "gw_mac", header_info.p_mac_addr->str_buf))
    {
        return NULL;
    }
    return cJSON_AddObjectToObject(p_json_data, "tags");
}

static bool
http_json_generate_records_tag_mac_section(
    cJSON* const              p_json_tags,
    const adv_report_t* const p_adv,
    const bool                flag_use_timestamps)
{
    const mac_address_str_t mac_str    = mac_address_to_str(&p_adv->tag_mac);
    cJSON* const            p_json_tag = cJSON_AddObjectToObject(p_json_tags, mac_str.str_buf);
    if (NULL == p_json_tag)
    {
        return false;
    }
    if (NULL == cJSON_AddNumberToObject(p_json_tag, "rssi", p_adv->rssi))
    {
        return false;
    }
    if (flag_use_timestamps)
    {
        if (!cjson_wrap_add_timestamp(p_json_tag, "timestamp", p_adv->timestamp))
        {
            return false;
        }
    }
    else
    {
        if (!cjson_wrap_add_timestamp(p_json_tag, "counter", p_adv->timestamp))
        {
            return false;
        }
    }
    char* p_hex_str = bin2hex_with_malloc(p_adv->data_buf, p_adv->data_len);
    if (NULL == p_hex_str)
    {
        return false;
    }
    if (NULL == cJSON_AddStringToObject(p_json_tag, "data", p_hex_str))
    {
        os_free(p_hex_str);
        return false;
    }
    os_free(p_hex_str);
    return true;
}

static bool
http_json_generate_records_data_section(
    cJSON* const                    p_json_root,
    const adv_report_table_t* const p_reports,
    const http_json_header_info_t   header_info)
{
    cJSON* const p_json_data = cJSON_AddObjectToObject(p_json_root, "data");
    if (NULL == p_json_data)
    {
        return false;
    }

    cJSON* const p_json_tags = http_json_generate_records_data_attributes(p_json_data, header_info);
    if (NULL == p_json_tags)
    {
        return false;
    }

    if (NULL != p_reports)
    {
        for (num_of_advs_t i = 0; i < p_reports->num_of_advs; ++i)
        {
            if (!http_json_generate_records_tag_mac_section(
                    p_json_tags,
                    &p_reports->table[i],
                    header_info.flag_use_timestamps))
            {
                return false;
            }
        }
    }
    return true;
}

static cJSON*
http_json_generate_records(const adv_report_table_t* const p_reports, const http_json_header_info_t header_info)
{
    cJSON* p_json_root = cJSON_CreateObject();
    if (NULL == p_json_root)
    {
        return NULL;
    }
    if (!http_json_generate_records_data_section(p_json_root, p_reports, header_info))
    {
        cjson_wrap_delete(&p_json_root);
        return NULL;
    }
    return p_json_root;
}

bool
http_json_create_records_str(
    const adv_report_table_t* const p_reports,
    const http_json_header_info_t   header_info,
    cjson_wrap_str_t* const         p_json_str)
{
    cJSON* p_json_root = http_json_generate_records(p_reports, header_info);
    if (NULL == p_json_root)
    {
        return false;
    }
    *p_json_str = cjson_wrap_print_and_delete(&p_json_root);
    if (NULL == p_json_str->p_str)
    {
        return false;
    }
    return true;
}

static bool
http_json_generate_task_info(const char* const p_task_name, const uint32_t min_free_stack_size, void* p_userdata)
{
    cJSON* const p_json_tasks = p_userdata;
    cJSON*       p_task_obj   = cJSON_AddObjectToObject(p_json_tasks, p_task_name);
    if (NULL == p_task_obj)
    {
        return false;
    }
    if (NULL == cJSON_AddNumberToObject(p_task_obj, "MIN_FREE_STACK_SIZE", min_free_stack_size))
    {
        return false;
    }
    return true;
}

static bool
http_json_generate_attributes_for_sensors(
    const adv_report_table_t* const p_reports,
    cJSON* const                    p_json_active_sensors,
    cJSON* const                    p_json_inactive_sensors,
    cJSON* const                    p_json_tasks)
{
    if (NULL == p_reports)
    {
        return true;
    }
    for (num_of_advs_t i = 0; i < p_reports->num_of_advs; ++i)
    {
        const adv_report_t* const p_adv   = &p_reports->table[i];
        const mac_address_str_t   mac_str = mac_address_to_str(&p_adv->tag_mac);
        if (0 != p_adv->samples_counter)
        {
            cJSON* p_json_obj = cJSON_CreateObject();
            if (NULL == p_json_obj)
            {
                return false;
            }
            cJSON_AddItemToArray(p_json_active_sensors, p_json_obj);
            if (NULL == cJSON_AddStringToObject(p_json_obj, "MAC", mac_str.str_buf))
            {
                return false;
            }
            if (!cjson_wrap_add_uint32(p_json_obj, "COUNTER", p_adv->samples_counter))
            {
                return false;
            }
        }
        else
        {
            cJSON* p_json_str = cJSON_CreateString(mac_str.str_buf);
            if (NULL == p_json_str)
            {
                return false;
            }
            cJSON_AddItemToArray(p_json_inactive_sensors, p_json_str);
        }
    }
    if (!runtime_stat_for_each_accumulated_info(&http_json_generate_task_info, p_json_tasks))
    {
        return false;
    }
    return true;
}

static bool
http_json_generate_status_attributes(
    cJSON* const                             p_json_root,
    const http_json_statistics_info_t* const p_stat_info,
    const adv_report_table_t* const          p_reports)
{
    if (NULL == cJSON_AddStringToObject(p_json_root, "DEVICE_ADDR", p_stat_info->nrf52_mac_addr.str_buf))
    {
        return false;
    }
    if (NULL == cJSON_AddStringToObject(p_json_root, "ESP_FW", p_stat_info->esp_fw.buf))
    {
        return false;
    }
    if (NULL == cJSON_AddStringToObject(p_json_root, "NRF_FW", p_stat_info->nrf_fw.buf))
    {
        return false;
    }
    if (NULL == cJSON_AddBoolToObject(p_json_root, "NRF_STATUS", p_stat_info->nrf_status))
    {
        return false;
    }
    if (!cjson_wrap_add_uint32(p_json_root, "UPTIME", p_stat_info->uptime))
    {
        return false;
    }
    if (!cjson_wrap_add_uint32(p_json_root, "NONCE", p_stat_info->nonce))
    {
        return false;
    }
    const char* const p_connection_type = p_stat_info->is_connected_to_wifi ? "WIFI" : "ETHERNET";
    if (NULL == cJSON_AddStringToObject(p_json_root, "CONNECTION", p_connection_type))
    {
        return false;
    }
    if (!cjson_wrap_add_uint32(p_json_root, "NUM_CONN_LOST", p_stat_info->network_disconnect_cnt))
    {
        return false;
    }
    if (NULL == cJSON_AddStringToObject(p_json_root, "RESET_REASON", p_stat_info->reset_reason.buf))
    {
        return false;
    }
    if (!cjson_wrap_add_uint32(p_json_root, "RESET_CNT", p_stat_info->reset_cnt))
    {
        return false;
    }
    if (NULL == cJSON_AddStringToObject(p_json_root, "RESET_INFO", p_stat_info->p_reset_info))
    {
        return false;
    }
    uint32_t num_sensors_seen = 0;
    if (NULL != p_reports)
    {
        for (num_of_advs_t i = 0; i < p_reports->num_of_advs; ++i)
        {
            const adv_report_t* const p_adv = &p_reports->table[i];
            if (0 != p_adv->samples_counter)
            {
                num_sensors_seen += 1;
            }
        }
    }
    if (!cjson_wrap_add_uint32(p_json_root, "SENSORS_SEEN", num_sensors_seen))
    {
        return false;
    }
    cJSON* p_json_active_sensors = cJSON_AddArrayToObject(p_json_root, "ACTIVE_SENSORS");
    if (NULL == p_json_active_sensors)
    {
        return false;
    }
    cJSON* p_json_inactive_sensors = cJSON_AddArrayToObject(p_json_root, "INACTIVE_SENSORS");
    if (NULL == p_json_inactive_sensors)
    {
        return false;
    }
    cJSON* p_json_tasks = cJSON_AddObjectToObject(p_json_root, "TASKS");
    if (NULL == p_json_tasks)
    {
        return false;
    }
    return http_json_generate_attributes_for_sensors(
        p_reports,
        p_json_active_sensors,
        p_json_inactive_sensors,
        p_json_tasks);
}

static cJSON*
http_json_generate_status(
    const http_json_statistics_info_t* const p_stat_info,
    const adv_report_table_t* const          p_reports)
{
    cJSON* p_json_root = cJSON_CreateObject();
    if (NULL == p_json_root)
    {
        return NULL;
    }
    if (!http_json_generate_status_attributes(p_json_root, p_stat_info, p_reports))
    {
        cjson_wrap_delete(&p_json_root);
        return NULL;
    }
    return p_json_root;
}

bool
http_json_create_status_str(
    const http_json_statistics_info_t* const p_stat_info,
    const adv_report_table_t* const          p_reports,
    cjson_wrap_str_t* const                  p_json_str)
{
    cJSON* p_json_root = http_json_generate_status(p_stat_info, p_reports);
    if (NULL == p_json_root)
    {
        return false;
    }
    *p_json_str = cjson_wrap_print_and_delete(&p_json_root);
    if (NULL == p_json_str->p_str)
    {
        return false;
    }
    return true;
}
