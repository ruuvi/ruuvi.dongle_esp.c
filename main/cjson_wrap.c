/**
 * @file cjson_wrap.c
 * @author TheSomeMan
 * @date 2020-10-23
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "cjson_wrap.h"
#include <stdio.h>
#include <stdlib.h>

void
cjson_wrap_add_timestamp(cJSON *const p_object, const char *const p_name, const time_t timestamp)
{
    char timestamp_str[32];
    snprintf(timestamp_str, sizeof(timestamp_str), "%ld", timestamp);
    cJSON_AddStringToObject(p_object, p_name, timestamp_str);
}

cjson_wrap_str_t
cjson_wrap_print(const cJSON *p_object)
{
    const cjson_wrap_str_t json_str = {
        .p_str = cJSON_Print(p_object),
    };
    return json_str;
}

void
cjson_wrap_delete(cJSON **pp_object)
{
    cJSON_Delete(*pp_object);
    *pp_object = NULL;
}

cjson_wrap_str_t
cjson_wrap_print_and_delete(cJSON **pp_object)
{
    const cjson_wrap_str_t json_str = {
        .p_str = cJSON_Print(*pp_object),
    };
    cJSON_Delete(*pp_object);
    *pp_object = NULL;
    return json_str;
}

void
cjson_wrap_free_json_str(cjson_wrap_str_t *p_json_str)
{
    free((void *)p_json_str->p_str);
    p_json_str->p_str = NULL;
}

bool
json_wrap_copy_string_val(const cJSON *p_json_root, const char *p_attr_name, char *buf, const size_t buf_len)
{
    cJSON *p_json_attr = cJSON_GetObjectItem(p_json_root, p_attr_name);
    if (NULL == p_json_attr)
    {
        return false;
    }
    const char *p_str = cJSON_GetStringValue(p_json_attr);
    if (NULL == p_str)
    {
        return false;
    }
    snprintf(buf, buf_len, "%s", p_str);
    return true;
}

bool
json_wrap_get_bool_val(const cJSON *p_json_root, const char *p_attr_name, bool *p_val)
{
    const cJSON *p_json_attr = cJSON_GetObjectItem(p_json_root, p_attr_name);
    if (NULL == p_json_attr)
    {
        return false;
    }
    if (!(bool)cJSON_IsBool(p_json_attr))
    {
        return false;
    }
    *p_val = cJSON_IsTrue(p_json_attr);
    return true;
}

bool
json_wrap_get_uint16_val(const cJSON *p_json_root, const char *p_attr_name, uint16_t *p_val)
{
    const cJSON *p_json_attr = cJSON_GetObjectItem(p_json_root, p_attr_name);
    if (NULL == p_json_attr)
    {
        return false;
    }
    if (!(bool)cJSON_IsNumber(p_json_attr))
    {
        return false;
    }
    if (!((p_json_attr->valueint >= 0) && (p_json_attr->valueint <= UINT16_MAX)))
    {
        return false;
    }
    *p_val = (uint16_t)p_json_attr->valueint;
    return true;
}
