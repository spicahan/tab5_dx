#pragma once
void rf_loopback_test_log(const char *format, ...);
#define ESP_LOGI(tag, ...) ((void)(tag), rf_loopback_test_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), rf_loopback_test_log(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), rf_loopback_test_log(__VA_ARGS__))
