/**
 * @file test_bin2hex.cpp
 * @author TheSomeMan
 * @date 2020-08-27
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */

#include "http_server_cb.h"
#include <cstring>
#include <utility>
#include "gtest/gtest.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log_wrapper.hpp"
#include "gw_cfg.h"
#include "json_ruuvi.h"
#include "flashfatfs.h"

using namespace std;

/*** Google-test class implementation
 * *********************************************************************************/

class TestHttpServerCb;
static TestHttpServerCb *g_pTestClass;

struct flash_fat_fs_t
{
    string                   mount_point;
    string                   partition_label;
    flash_fat_fs_num_files_t max_files;
};

extern "C" {

const char *
os_task_get_name(void)
{
    static const char g_task_name[] = "main";
    return const_cast<char *>(g_task_name);
}

} // extern "C"

class MemAllocTrace
{
    vector<void *> allocated_mem;

    std::vector<void *>::iterator
    find(void *ptr)
    {
        for (auto iter = this->allocated_mem.begin(); iter != this->allocated_mem.end(); ++iter)
        {
            if (*iter == ptr)
            {
                return iter;
            }
        }
        return this->allocated_mem.end();
    }

public:
    void
    add(void *ptr)
    {
        auto iter = find(ptr);
        assert(iter == this->allocated_mem.end()); // ptr was found in the list of allocated memory blocks
        this->allocated_mem.push_back(ptr);
    }
    void
    remove(void *ptr)
    {
        auto iter = find(ptr);
        assert(iter != this->allocated_mem.end()); // ptr was not found in the list of allocated memory blocks
        this->allocated_mem.erase(iter);
    }
    bool
    is_empty()
    {
        return this->allocated_mem.empty();
    }
};

class FileInfo
{
public:
    string fileName;
    string content;
    bool   flag_fail_to_open;

    FileInfo(string fileName, string content, const bool flag_fail_to_open)
        : fileName(std::move(fileName))
        , content(std::move(content))
        , flag_fail_to_open(flag_fail_to_open)
    {
    }

    FileInfo(string fileName, string content)
        : FileInfo(std::move(fileName), std::move(content), false)
    {
    }
};

class TestHttpServerCb : public ::testing::Test
{
private:
protected:
    void
    SetUp() override
    {
        esp_log_wrapper_init();
        g_pTestClass = this;

        this->m_malloc_cnt                        = 0;
        this->m_malloc_fail_on_cnt                = 0;
        this->m_flag_settings_saved_to_flash      = false;
        this->m_flag_settings_sent_to_nrf         = false;
        this->m_flag_settings_ethernet_ip_updated = false;
    }

    void
    TearDown() override
    {
        http_server_cb_deinit();
        this->m_files.clear();
        this->m_fd   = -1;
        g_pTestClass = nullptr;
        esp_log_wrapper_deinit();
    }

public:
    TestHttpServerCb();

    ~TestHttpServerCb() override;

    MemAllocTrace     m_mem_alloc_trace;
    uint32_t          m_malloc_cnt;
    uint32_t          m_malloc_fail_on_cnt;
    bool              m_flag_settings_saved_to_flash;
    bool              m_flag_settings_sent_to_nrf;
    bool              m_flag_settings_ethernet_ip_updated;
    flash_fat_fs_t    m_fatfs;
    bool              m_is_fatfs_mounted;
    bool              m_is_fatfs_mount_fail;
    vector<FileInfo>  m_files;
    file_descriptor_t m_fd;
};

TestHttpServerCb::TestHttpServerCb()
    : m_malloc_cnt(0)
    , m_malloc_fail_on_cnt(0)
    , m_flag_settings_saved_to_flash(false)
    , m_flag_settings_sent_to_nrf(false)
    , m_flag_settings_ethernet_ip_updated(false)
    , m_is_fatfs_mounted(false)
    , m_is_fatfs_mount_fail(false)
    , m_fd(-1)
    , Test()
{
}

extern "C" {

void *
os_malloc(const size_t size)
{
    if (++g_pTestClass->m_malloc_cnt == g_pTestClass->m_malloc_fail_on_cnt)
    {
        return nullptr;
    }
    void *ptr = malloc(size);
    assert(nullptr != ptr);
    g_pTestClass->m_mem_alloc_trace.add(ptr);
    return ptr;
}

void
os_free_internal(void *ptr)
{
    g_pTestClass->m_mem_alloc_trace.remove(ptr);
    free(ptr);
}

void *
os_calloc(const size_t nmemb, const size_t size)
{
    if (++g_pTestClass->m_malloc_cnt == g_pTestClass->m_malloc_fail_on_cnt)
    {
        return nullptr;
    }
    void *ptr = calloc(nmemb, size);
    assert(nullptr != ptr);
    g_pTestClass->m_mem_alloc_trace.add(ptr);
    return ptr;
}

char *
ruuvi_get_metrics(void)
{
    const char *p_metrics_str = "metrics_info";
    char *      p_buf         = static_cast<char *>(os_malloc(strlen(p_metrics_str) + 1));
    if (nullptr != p_buf)
    {
        strcpy(p_buf, p_metrics_str);
    }
    return p_buf;
}

void
settings_save_to_flash(const ruuvi_gateway_config_t *p_config)
{
    g_pTestClass->m_flag_settings_saved_to_flash = true;
}

void
ethernet_update_ip(void)
{
    g_pTestClass->m_flag_settings_ethernet_ip_updated = true;
}

void
ruuvi_send_nrf_settings(const ruuvi_gateway_config_t *p_config)
{
    g_pTestClass->m_flag_settings_sent_to_nrf = true;
}

const flash_fat_fs_t *
flashfatfs_mount(const char *mount_point, const char *partition_label, const flash_fat_fs_num_files_t max_files)
{
    assert(!g_pTestClass->m_is_fatfs_mounted);
    if (g_pTestClass->m_is_fatfs_mount_fail)
    {
        return nullptr;
    }
    g_pTestClass->m_fatfs.mount_point.assign(mount_point);
    g_pTestClass->m_fatfs.partition_label.assign(partition_label);
    g_pTestClass->m_fatfs.max_files  = max_files;
    g_pTestClass->m_is_fatfs_mounted = true;
    return &g_pTestClass->m_fatfs;
}

bool
flashfatfs_unmount(const flash_fat_fs_t **pp_ffs)
{
    assert(nullptr != pp_ffs);
    assert(*pp_ffs == &g_pTestClass->m_fatfs);
    assert(g_pTestClass->m_is_fatfs_mounted);
    g_pTestClass->m_is_fatfs_mounted = false;
    g_pTestClass->m_fatfs.mount_point.assign("");
    g_pTestClass->m_fatfs.partition_label.assign("");
    g_pTestClass->m_fatfs.max_files = 0;
    *pp_ffs                         = nullptr;
    return true;
}

bool
flashfatfs_get_file_size(const flash_fat_fs_t *p_ffs, const char *file_path, size_t *p_size)
{
    assert(p_ffs == &g_pTestClass->m_fatfs);
    for (auto &file : g_pTestClass->m_files)
    {
        if (0 == strcmp(file_path, file.fileName.c_str()))
        {
            *p_size = file.content.size();
            return true;
        }
    }
    *p_size = 0;
    return false;
}

file_descriptor_t
flashfatfs_open(const flash_fat_fs_t *p_ffs, const char *file_path)
{
    assert(p_ffs == &g_pTestClass->m_fatfs);
    for (auto &file : g_pTestClass->m_files)
    {
        if (0 == strcmp(file_path, file.fileName.c_str()))
        {
            if (file.flag_fail_to_open)
            {
                return (file_descriptor_t)-1;
            }
            return g_pTestClass->m_fd;
        }
    }
    return (file_descriptor_t)-1;
}

} // extern "C"

TestHttpServerCb::~TestHttpServerCb() = default;

#define TEST_CHECK_LOG_RECORD_HTTP_SERVER(level_, msg_) \
    ESP_LOG_WRAPPER_TEST_CHECK_LOG_RECORD("http_server", level_, msg_)

#define TEST_CHECK_LOG_RECORD_GW_CFG(level_, msg_) ESP_LOG_WRAPPER_TEST_CHECK_LOG_RECORD("gw_cfg", level_, msg_)

/*** Unit-Tests
 * *******************************************************************************************************/

TEST_F(TestHttpServerCb, http_server_cb_init_ok_deinit_ok) // NOLINT
{
    ASSERT_FALSE(g_pTestClass->m_is_fatfs_mounted);
    ASSERT_TRUE(http_server_cb_init());
    ASSERT_TRUE(g_pTestClass->m_is_fatfs_mounted);
    ASSERT_TRUE(esp_log_wrapper_is_empty());
    http_server_cb_deinit();
    ASSERT_TRUE(esp_log_wrapper_is_empty());
    ASSERT_FALSE(g_pTestClass->m_is_fatfs_mounted);
}

TEST_F(TestHttpServerCb, http_server_cb_deinit_of_not_initialized) // NOLINT
{
    ASSERT_FALSE(g_pTestClass->m_is_fatfs_mounted);
    http_server_cb_deinit();
    ASSERT_TRUE(esp_log_wrapper_is_empty());
    ASSERT_FALSE(g_pTestClass->m_is_fatfs_mounted);
}

TEST_F(TestHttpServerCb, http_server_cb_init_failed) // NOLINT
{
    ASSERT_FALSE(g_pTestClass->m_is_fatfs_mounted);
    this->m_is_fatfs_mount_fail = true;
    ASSERT_FALSE(http_server_cb_init());
    ASSERT_FALSE(g_pTestClass->m_is_fatfs_mounted);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "flashfatfs_mount: failed to mount partition 'fatfs_gwui'");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_json_ruuvi_ok) // NOLINT
{
    const char *expected_json
        = "{\n"
          "\t\"eth_dhcp\":\ttrue,\n"
          "\t\"eth_static_ip\":\t\"\",\n"
          "\t\"eth_netmask\":\t\"\",\n"
          "\t\"eth_gw\":\t\"\",\n"
          "\t\"eth_dns1\":\t\"\",\n"
          "\t\"eth_dns2\":\t\"\",\n"
          "\t\"use_http\":\tfalse,\n"
          "\t\"http_url\":\t\"https://network.ruuvi.com:443/gwapi/v1\",\n"
          "\t\"http_user\":\t\"\",\n"
          "\t\"use_mqtt\":\ttrue,\n"
          "\t\"mqtt_server\":\t\"test.mosquitto.org\",\n"
          "\t\"mqtt_port\":\t1883,\n"
          "\t\"mqtt_prefix\":\t\"ruuvi/30:AE:A4:02:84:A4\",\n"
          "\t\"mqtt_user\":\t\"\",\n"
          "\t\"gw_mac\":\t\"11:22:33:44:55:66\",\n"
          "\t\"use_filtering\":\ttrue,\n"
          "\t\"company_id\":\t\"0x0499\",\n"
          "\t\"coordinates\":\t\"\",\n"
          "\t\"use_coded_phy\":\tfalse,\n"
          "\t\"use_1mbit_phy\":\tfalse,\n"
          "\t\"use_extended_payload\":\tfalse,\n"
          "\t\"use_channel_37\":\tfalse,\n"
          "\t\"use_channel_38\":\tfalse,\n"
          "\t\"use_channel_39\":\tfalse\n"
          "}";
    ASSERT_TRUE(json_ruuvi_parse_http_body(
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}",
        &g_gateway_config));
    snprintf(gw_mac_sta.str_buf, sizeof(gw_mac_sta.str_buf), "11:22:33:44:55:66");

    esp_log_wrapper_clear();
    const http_server_resp_t resp = http_server_resp_json_ruuvi();

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_HEAP, resp.content_location);
    ASSERT_TRUE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_JSON, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_json), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_json), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("ruuvi.json: ") + string(expected_json));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_json_ruuvi_malloc_failed) // NOLINT
{
    ASSERT_TRUE(json_ruuvi_parse_http_body(
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}",
        &g_gateway_config));
    snprintf(gw_mac_sta.str_buf, sizeof(gw_mac_sta.str_buf), "11:22:33:44:55:66");
    cJSON_Hooks hooks = {
        .malloc_fn = &os_malloc,
        .free_fn   = &os_free_internal,
    };
    g_pTestClass->m_malloc_fail_on_cnt = 1;
    cJSON_InitHooks(&hooks);

    esp_log_wrapper_clear();
    const http_server_resp_t resp = http_server_resp_json_ruuvi();

    ASSERT_EQ(HTTP_RESP_CODE_503, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_ERROR, string("Can't create json object"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_json_ok) // NOLINT
{
    const char *expected_json
        = "{\n"
          "\t\"eth_dhcp\":\ttrue,\n"
          "\t\"eth_static_ip\":\t\"\",\n"
          "\t\"eth_netmask\":\t\"\",\n"
          "\t\"eth_gw\":\t\"\",\n"
          "\t\"eth_dns1\":\t\"\",\n"
          "\t\"eth_dns2\":\t\"\",\n"
          "\t\"use_http\":\tfalse,\n"
          "\t\"http_url\":\t\"https://network.ruuvi.com:443/gwapi/v1\",\n"
          "\t\"http_user\":\t\"\",\n"
          "\t\"use_mqtt\":\ttrue,\n"
          "\t\"mqtt_server\":\t\"test.mosquitto.org\",\n"
          "\t\"mqtt_port\":\t1883,\n"
          "\t\"mqtt_prefix\":\t\"ruuvi/30:AE:A4:02:84:A4\",\n"
          "\t\"mqtt_user\":\t\"\",\n"
          "\t\"gw_mac\":\t\"11:22:33:44:55:66\",\n"
          "\t\"use_filtering\":\ttrue,\n"
          "\t\"company_id\":\t\"0x0499\",\n"
          "\t\"coordinates\":\t\"\",\n"
          "\t\"use_coded_phy\":\tfalse,\n"
          "\t\"use_1mbit_phy\":\tfalse,\n"
          "\t\"use_extended_payload\":\tfalse,\n"
          "\t\"use_channel_37\":\tfalse,\n"
          "\t\"use_channel_38\":\tfalse,\n"
          "\t\"use_channel_39\":\tfalse\n"
          "}";
    ASSERT_TRUE(json_ruuvi_parse_http_body(
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}",
        &g_gateway_config));
    snprintf(gw_mac_sta.str_buf, sizeof(gw_mac_sta.str_buf), "11:22:33:44:55:66");

    esp_log_wrapper_clear();
    const http_server_resp_t resp = http_server_resp_json("ruuvi.json");

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_HEAP, resp.content_location);
    ASSERT_TRUE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_JSON, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_json), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_json), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("ruuvi.json: ") + string(expected_json));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_json_unknown) // NOLINT
{
    const http_server_resp_t resp = http_server_resp_json("unknown.json");

    ASSERT_EQ(HTTP_RESP_CODE_404, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_WARN, string("Request to unknown json: unknown.json"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_metrics_ok) // NOLINT
{
    const char *             expected_resp = "metrics_info";
    const http_server_resp_t resp          = http_server_resp_metrics();

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_HEAP, resp.content_location);
    ASSERT_TRUE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_PLAIN, resp.content_type);
    ASSERT_NE(nullptr, resp.p_content_type_param);
    ASSERT_EQ(string("version=0.0.4"), string(resp.p_content_type_param));
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_resp), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("metrics: ") + string(expected_resp));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_metrics_malloc_failed) // NOLINT
{
    g_pTestClass->m_malloc_fail_on_cnt = 1;
    const http_server_resp_t resp      = http_server_resp_metrics();

    ASSERT_EQ(HTTP_RESP_CODE_503, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, string("Not enough memory"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_get_content_type_by_ext) // NOLINT
{
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, http_get_content_type_by_ext(".html"));
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_CSS, http_get_content_type_by_ext(".css"));
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_CSS, http_get_content_type_by_ext(".scss"));
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_JAVASCRIPT, http_get_content_type_by_ext(".js"));
    ASSERT_EQ(HTTP_CONENT_TYPE_IMAGE_PNG, http_get_content_type_by_ext(".png"));
    ASSERT_EQ(HTTP_CONENT_TYPE_IMAGE_SVG_XML, http_get_content_type_by_ext(".svg"));
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_OCTET_STREAM, http_get_content_type_by_ext(".ttf"));
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_OCTET_STREAM, http_get_content_type_by_ext(".unk"));
}

TEST_F(TestHttpServerCb, resp_file_index_html_fail_partition_not_ready) // NOLINT
{
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 1;
    FileInfo                fileInfo      = FileInfo("index.html", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("index.html");
    ASSERT_EQ(HTTP_RESP_CODE_503, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "Try to find file: index.html");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "GWUI partition is not ready");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_index_html_fail_file_name_too_long) // NOLINT
{
    const char *            file_name     = "a1234567890123456789012345678901234567890123456789012345678901234567890";
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo(file_name, expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file(file_name);
    ASSERT_EQ(HTTP_RESP_CODE_503, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: ") + string(file_name));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(
        ESP_LOG_ERROR,
        string("Temporary buffer is not enough for the file path '") + string(file_name) + string("'"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_index_html) // NOLINT
{
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("index.html", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("index.html");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "Try to find file: index.html");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File index.html was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_index_html_gzipped) // NOLINT
{
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 2;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("index.html.gz", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("index.html");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_GZIP, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: index.html"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File index.html.gz was opened successfully, fd=2");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_app_js_gzipped) // NOLINT
{
    const char *            expected_resp = "app_js_gzipped";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("app.js.gz", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("app.js");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_JAVASCRIPT, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_GZIP, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: app.js"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File app.js.gz was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_app_css_gzipped) // NOLINT
{
    const char *            expected_resp = "slyle_css_gzipped";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("style.css.gz", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("style.css");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_CSS, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_GZIP, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: style.css"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File style.css.gz was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_binary_without_extension) // NOLINT
{
    const char *            expected_resp = "binary_data";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("binary", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("binary");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_OCTET_STREAM, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: binary"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File binary was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_unknown_html) // NOLINT
{
    ASSERT_TRUE(http_server_cb_init());

    const http_server_resp_t resp = http_server_resp_file("unknown.html");
    ASSERT_EQ(HTTP_RESP_CODE_404, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: unknown.html"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, string("Can't find file: unknown.html"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, resp_file_index_html_failed_on_open) // NOLINT
{
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("index.html", expected_resp, true);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_resp_file("index.html");
    ASSERT_EQ(HTTP_RESP_CODE_503, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "Try to find file: index.html");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, string("Can't open file: index.html"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_get_default) // NOLINT
{
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("index.html.gz", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_cb_on_get("");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_GZIP, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("GET /"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: index.html"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File index.html.gz was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_get_index_html) // NOLINT
{
    const char *            expected_resp = "index_html_content";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("index.html.gz", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_cb_on_get("index.html");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_GZIP, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("GET /index.html"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: index.html"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File index.html.gz was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_get_app_js) // NOLINT
{
    const char *            expected_resp = "app_js_gzipped";
    const file_descriptor_t fd            = 1;
    ASSERT_TRUE(http_server_cb_init());
    FileInfo fileInfo = FileInfo("app.js.gz", expected_resp);
    this->m_files.emplace_back(fileInfo);
    this->m_fd = fd;

    const http_server_resp_t resp = http_server_cb_on_get("app.js");
    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FATFS, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_JAVASCRIPT, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_GZIP, resp.content_encoding);
    ASSERT_EQ(fd, resp.select_location.fatfs.fd);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("GET /app.js"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("Try to find file: app.js"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "File app.js.gz was opened successfully, fd=1");
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_get_ruuvi_json) // NOLINT
{
    const char *expected_json
        = "{\n"
          "\t\"eth_dhcp\":\ttrue,\n"
          "\t\"eth_static_ip\":\t\"\",\n"
          "\t\"eth_netmask\":\t\"\",\n"
          "\t\"eth_gw\":\t\"\",\n"
          "\t\"eth_dns1\":\t\"\",\n"
          "\t\"eth_dns2\":\t\"\",\n"
          "\t\"use_http\":\tfalse,\n"
          "\t\"http_url\":\t\"https://network.ruuvi.com:443/gwapi/v1\",\n"
          "\t\"http_user\":\t\"\",\n"
          "\t\"use_mqtt\":\ttrue,\n"
          "\t\"mqtt_server\":\t\"test.mosquitto.org\",\n"
          "\t\"mqtt_port\":\t1883,\n"
          "\t\"mqtt_prefix\":\t\"ruuvi/30:AE:A4:02:84:A4\",\n"
          "\t\"mqtt_user\":\t\"\",\n"
          "\t\"gw_mac\":\t\"11:22:33:44:55:66\",\n"
          "\t\"use_filtering\":\ttrue,\n"
          "\t\"company_id\":\t\"0x0499\",\n"
          "\t\"coordinates\":\t\"\",\n"
          "\t\"use_coded_phy\":\tfalse,\n"
          "\t\"use_1mbit_phy\":\tfalse,\n"
          "\t\"use_extended_payload\":\tfalse,\n"
          "\t\"use_channel_37\":\tfalse,\n"
          "\t\"use_channel_38\":\tfalse,\n"
          "\t\"use_channel_39\":\tfalse\n"
          "}";
    ASSERT_TRUE(json_ruuvi_parse_http_body(
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}",
        &g_gateway_config));
    snprintf(gw_mac_sta.str_buf, sizeof(gw_mac_sta.str_buf), "11:22:33:44:55:66");

    esp_log_wrapper_clear();
    const http_server_resp_t resp = http_server_cb_on_get("ruuvi.json");

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_HEAP, resp.content_location);
    ASSERT_TRUE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_JSON, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_json), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_json), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("GET /ruuvi.json"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("ruuvi.json: ") + string(expected_json));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_get_metrics) // NOLINT
{
    const char *             expected_resp = "metrics_info";
    const http_server_resp_t resp          = http_server_cb_on_get("metrics");

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_HEAP, resp.content_location);
    ASSERT_TRUE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_PLAIN, resp.content_type);
    ASSERT_NE(nullptr, resp.p_content_type_param);
    ASSERT_EQ(string("version=0.0.4"), string(resp.p_content_type_param));
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_resp), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("GET /metrics"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_INFO, string("metrics: ") + string(expected_resp));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_post_ruuvi_ok) // NOLINT
{
    const char *expected_resp = "{}";
    memset(&g_gateway_config, 0, sizeof(g_gateway_config));
    const http_server_resp_t resp = http_server_cb_on_post_ruuvi(
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}");

    ASSERT_TRUE(this->m_flag_settings_saved_to_flash);
    ASSERT_TRUE(this->m_flag_settings_sent_to_nrf);
    ASSERT_TRUE(this->m_flag_settings_ethernet_ip_updated);

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FLASH_MEM, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_JSON, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_resp), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("POST /ruuvi.json"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "Got SETTINGS:");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_dhcp not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_static_ip not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_netmask not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_gw not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_dns1 not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_dns2 not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "use_mqtt: 1");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_server: test.mosquitto.org");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_prefix: ruuvi/30:AE:A4:02:84:A4");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_port: 1883");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_user: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_pass: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "use_http: 0");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "http_url: https://network.ruuvi.com:443/gwapi/v1");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "http_user: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "http_pass: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "use_filtering: 1");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "company_id not found or invalid");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "coordinates not found");
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("Got SETTINGS from browser:"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use eth dhcp: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth static ip: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth netmask: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth gw: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth dns1: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth dns2: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use mqtt: 1"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt server: test.mosquitto.org"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt port: 1883"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt prefix: ruuvi/30:AE:A4:02:84:A4"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt user: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt password: ********"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use http: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: http url: https://network.ruuvi.com:443/gwapi/v1"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: http user: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: http pass: ********"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: coordinates: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use company id filter: 1"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: company id: 0x0000"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan coded phy: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan 1mbit/phy: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan extended payload: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan channel 37: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan channel 38: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan channel 39: 0"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_post_ruuvi_malloc_failed) // NOLINT
{
    memset(&g_gateway_config, 0, sizeof(g_gateway_config));
    this->m_malloc_fail_on_cnt    = 1;
    const http_server_resp_t resp = http_server_cb_on_post_ruuvi(
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}");

    ASSERT_FALSE(this->m_flag_settings_saved_to_flash);
    ASSERT_FALSE(this->m_flag_settings_sent_to_nrf);
    ASSERT_FALSE(this->m_flag_settings_ethernet_ip_updated);

    ASSERT_EQ(HTTP_RESP_CODE_503, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("POST /ruuvi.json"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, string("Failed to parse json or no memory"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_post_ruuvi_json_ok) // NOLINT
{
    const char *expected_resp = "{}";
    memset(&g_gateway_config, 0, sizeof(g_gateway_config));
    const http_server_resp_t resp = http_server_cb_on_post(
        "ruuvi.json",
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}");

    ASSERT_TRUE(this->m_flag_settings_saved_to_flash);
    ASSERT_TRUE(this->m_flag_settings_sent_to_nrf);
    ASSERT_TRUE(this->m_flag_settings_ethernet_ip_updated);

    ASSERT_EQ(HTTP_RESP_CODE_200, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_FLASH_MEM, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_APPLICATION_JSON, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(strlen(expected_resp), resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_NE(nullptr, resp.select_location.memory.p_buf);
    ASSERT_EQ(string(expected_resp), string(reinterpret_cast<const char *>(resp.select_location.memory.p_buf)));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, string("POST /ruuvi.json"));
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "Got SETTINGS:");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_dhcp not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_static_ip not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_netmask not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_gw not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_dns1 not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "eth_dns2 not found");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "use_mqtt: 1");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_server: test.mosquitto.org");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_prefix: ruuvi/30:AE:A4:02:84:A4");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_port: 1883");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_user: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "mqtt_pass: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "use_http: 0");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "http_url: https://network.ruuvi.com:443/gwapi/v1");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "http_user: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "http_pass: ");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_DEBUG, "use_filtering: 1");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "company_id not found or invalid");
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_ERROR, "coordinates not found");
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("Got SETTINGS from browser:"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use eth dhcp: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth static ip: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth netmask: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth gw: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth dns1: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: eth dns2: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use mqtt: 1"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt server: test.mosquitto.org"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt port: 1883"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt prefix: ruuvi/30:AE:A4:02:84:A4"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt user: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: mqtt password: ********"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use http: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: http url: https://network.ruuvi.com:443/gwapi/v1"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: http user: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: http pass: ********"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: coordinates: "));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use company id filter: 1"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: company id: 0x0000"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan coded phy: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan 1mbit/phy: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan extended payload: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan channel 37: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan channel 38: 0"));
    TEST_CHECK_LOG_RECORD_GW_CFG(ESP_LOG_INFO, string("config: use scan channel 39: 0"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_post_unknown_json) // NOLINT
{
    memset(&g_gateway_config, 0, sizeof(g_gateway_config));
    const http_server_resp_t resp = http_server_cb_on_post(
        "unknown.json",
        "{"
        "\"use_mqtt\":true,"
        "\"mqtt_server\":\"test.mosquitto.org\","
        "\"mqtt_port\":1883,"
        "\"mqtt_prefix\":\"ruuvi/30:AE:A4:02:84:A4\","
        "\"mqtt_user\":\"\","
        "\"mqtt_pass\":\"\","
        "\"use_http\":false,"
        "\"http_url\":\"https://network.ruuvi.com:443/gwapi/v1\","
        "\"http_user\":\"\","
        "\"http_pass\":\"\","
        "\"use_filtering\":true"
        "}");

    ASSERT_FALSE(this->m_flag_settings_saved_to_flash);
    ASSERT_FALSE(this->m_flag_settings_sent_to_nrf);
    ASSERT_FALSE(this->m_flag_settings_ethernet_ip_updated);

    ASSERT_EQ(HTTP_RESP_CODE_404, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_WARN, string("POST /unknown.json"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}

TEST_F(TestHttpServerCb, http_server_cb_on_delete) // NOLINT
{
    const http_server_resp_t resp = http_server_cb_on_delete("unknown.json");

    ASSERT_EQ(HTTP_RESP_CODE_404, resp.http_resp_code);
    ASSERT_EQ(HTTP_CONTENT_LOCATION_NO_CONTENT, resp.content_location);
    ASSERT_FALSE(resp.flag_no_cache);
    ASSERT_EQ(HTTP_CONENT_TYPE_TEXT_HTML, resp.content_type);
    ASSERT_EQ(nullptr, resp.p_content_type_param);
    ASSERT_EQ(0, resp.content_len);
    ASSERT_EQ(HTTP_CONENT_ENCODING_NONE, resp.content_encoding);
    ASSERT_EQ(nullptr, resp.select_location.memory.p_buf);
    TEST_CHECK_LOG_RECORD_HTTP_SERVER(ESP_LOG_WARN, string("DELETE /unknown.json"));
    ASSERT_TRUE(esp_log_wrapper_is_empty());
}
