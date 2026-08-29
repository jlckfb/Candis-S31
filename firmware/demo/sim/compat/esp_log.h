#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void sim_log_print(const char *level, const char *tag, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, fmt, ...) sim_log_print("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) sim_log_print("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) sim_log_print("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) sim_log_print("D", tag, fmt, ##__VA_ARGS__)
