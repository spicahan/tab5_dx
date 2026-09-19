// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_DXFT8_PA_TEST 1
#define CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD 1
#define CONFIG_GPIO_CTRL_FUNC_IN_IRAM 1
#define CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0 1
#define CONFIG_FREERTOS_UNICORE 0
#define IRAM_ATTR
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
const char *esp_err_to_name(esp_err_t error);
void pa_precheck_log(const char *format, ...);
#define ESP_LOGI(tag, ...) ((void)(tag), pa_precheck_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), pa_precheck_log(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), pa_precheck_log(__VA_ARGS__))

typedef int gpio_num_t;
enum { GPIO_NUM_3 = 3, GPIO_NUM_4 = 4, GPIO_NUM_16 = 16, GPIO_NUM_31 = 31,
       GPIO_NUM_32 = 32, GPIO_NUM_45 = 45, GPIO_NUM_47 = 47, GPIO_NUM_48 = 48 };
enum { GPIO_FLOATING, GPIO_MODE_INPUT_OUTPUT, GPIO_PULLDOWN_ENABLE,
       GPIO_PULLUP_DISABLE, GPIO_INTR_DISABLE };
typedef struct { uint64_t pin_bit_mask; int mode, pull_down_en, pull_up_en, intr_type; } gpio_config_t;
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode);
esp_err_t gpio_config(const gpio_config_t *config);

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
enum { I2C_NUM_0, I2C_CLK_SRC_DEFAULT };
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus);
typedef int i2s_port_t;
typedef void *i2s_chan_handle_t;
#define I2S_NUM_1 1

typedef struct pa_precheck_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; int dispatch_method; const char *name; } esp_timer_create_args_t;
#define ESP_TIMER_ISR 1
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_create(const esp_timer_create_args_t *config, esp_timer_handle_t *timer);

typedef unsigned TickType_t;
typedef int portMUX_TYPE;
typedef void *QueueHandle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10U)
#define pdTRUE 1
#define pdPASS 1
void pa_precheck_enter(portMUX_TYPE *mux);
void pa_precheck_exit(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux) pa_precheck_enter(mux)
#define portEXIT_CRITICAL(mux) pa_precheck_exit(mux)
#define portENTER_CRITICAL_ISR(mux) pa_precheck_enter(mux)
#define portEXIT_CRITICAL_ISR(mux) pa_precheck_exit(mux)
void vTaskDelay(TickType_t ticks);
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack,
                          void *argument, unsigned priority, void *handle, int core);
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);

typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
typedef struct { unsigned rx_buffer_size, tx_buffer_size; } usb_serial_jtag_driver_config_t;
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);
int usb_serial_jtag_read_bytes(void *buffer, size_t length, TickType_t timeout);
void usb_serial_jtag_vfs_use_driver(void);
