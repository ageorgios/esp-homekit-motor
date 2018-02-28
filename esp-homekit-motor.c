#include <stdio.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_sta.h>
#include <esp/uart.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include "wifi.h"
#include "button.h"

static void wifi_init() {
    struct sdk_station_config wifi_config = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASSWORD,
    };

    sdk_wifi_set_opmode(STATION_MODE);
    sdk_wifi_station_set_config(&wifi_config);
    sdk_wifi_station_connect();
}

static QueueHandle_t window_queue = NULL;

typedef enum {
    window_command_idle,
    window_command_open,
    window_command_close
} window_command;

typedef enum {
    window_state_idle,
    window_state_opening,
    window_state_closing,
} window_state;

static int duration = 0; //seconds
static window_state local_state = window_state_idle;
static int local_current_position = 0;
static int local_target_position = 0;

void on_update_target_position(homekit_characteristic_t *ch, homekit_value_t value, void *context);
homekit_characteristic_t current_position = HOMEKIT_CHARACTERISTIC_(CURRENT_POSITION, 0);
homekit_characteristic_t target_position  = HOMEKIT_CHARACTERISTIC_(TARGET_POSITION, 0, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update_target_position));
homekit_characteristic_t position_state   = HOMEKIT_CHARACTERISTIC_(POSITION_STATE, 0);

const int led_gpio = 2;
const int up_gpio = 12;
const int down_gpio = 4;
const int button1_gpio = 0; //14=D5 //0=button //9=reset //5=D1
const int button2_gpio = 5; 

#define start_up() gpio_write(up_gpio, 1);
#define stop_up() gpio_write(up_gpio, 0);
#define start_down() gpio_write(down_gpio, 1);
#define stop_down() gpio_write(down_gpio, 0);
#define led_on() gpio_write(led_gpio, 0);
#define led_off() gpio_write(led_gpio, 1);

void gpio_init() {
    gpio_enable(led_gpio, GPIO_OUTPUT);
    gpio_enable(up_gpio, GPIO_OUTPUT);
    gpio_enable(down_gpio, GPIO_OUTPUT);
    gpio_enable(button1_gpio, GPIO_INPUT);
    gpio_enable(button2_gpio, GPIO_INPUT);
    stop_up();
    stop_down();
    led_off();
}

void led_identify_task(void *_args) {
    for (int i=0; i<3; i++) {
        for (int j=0; j<2; j++) {
            led_on();
            vTaskDelay(100 / portTICK_PERIOD_MS);
            led_off();
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
    led_off();
    vTaskDelete(NULL);
}

void led_identify(homekit_value_t _value) {
    printf("LED identify\n");
    xTaskCreate(led_identify_task, "LED identify", 128, NULL, 2, NULL);
}

void on_update_target_position();

void send_window_command(window_command cmd) {
    xQueueSendToBack(window_queue, &cmd, 10);
}

void window_task_commands(void *context) {
    printf("Window Task Commands initialized");
    window_command cmd;
    while(1) {
        if (xQueueReceive(window_queue, &cmd, 100)) {
            switch(cmd) {
                case window_command_idle: {
                    printf("command IDLE received\n");
                }
                case window_command_open: {
                    printf("command OPEN received\n");
                    switch(local_state) {
                        case window_state_idle:
                            start_up();
                            homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_opening));
                            local_state = window_state_opening;
                            break;
                        case window_state_opening:
                            break;
                        case window_state_closing:
                            stop_down();
                            homekit_characteristic_notify(&target_position, HOMEKIT_UINT8(local_current_position));
                            homekit_characteristic_notify(&current_position, HOMEKIT_UINT8(local_current_position));
                            homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_idle));
                            local_state = window_state_idle;
                            break;
                    }
                    break;
                }
                case window_command_close: {
                    switch(local_state) {
                        printf("command CLOSE received\n");
                        case window_state_idle:
                            start_down();
                            homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_closing));
                            local_state = window_state_closing;
                            break;
                        case window_state_opening:
                            stop_up();
                            homekit_characteristic_notify(&target_position, HOMEKIT_UINT8(local_current_position));
                            homekit_characteristic_notify(&current_position, HOMEKIT_UINT8(local_current_position));
                            homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_idle));
                            local_state = window_state_idle;
                            break;
                        case window_state_closing:
                            break;
                    }
                    break;
                }
            }
        }
    }
}

void window_task_state(void *context) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = duration*10 / portTICK_PERIOD_MS;
    printf("Window Task State initialized with Delay: %d ticks\n", xFrequency);
    while(1) {
        vTaskDelayUntil( &xLastWakeTime, xFrequency);
        switch(local_state) {
            case window_state_idle:
                break;
            case window_state_opening:
                local_current_position++;
                printf("Current Position: %d\n", local_current_position);
                if (local_current_position == local_target_position) {
                    stop_up();
                    homekit_characteristic_notify(&current_position, HOMEKIT_UINT8(local_current_position));
                    homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_idle));
                    local_state = window_state_idle;
                }
                break;
            case window_state_closing:
                local_current_position--;
                printf("Current Position: %d\n", local_current_position);
                if (local_current_position == local_target_position) {
                    stop_down();
                    homekit_characteristic_notify(&current_position, HOMEKIT_UINT8(local_current_position));
                    homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_idle));
                    local_state = window_state_idle;
                }
                break;
        }
    }
}


void window_init(int dur) {
    local_state = window_state_idle;
    local_current_position = 0;
    local_target_position = 0;
    duration = dur; //seconds
    window_queue = xQueueCreate(1, sizeof(window_command));
    xTaskCreate(window_task_commands, "Window Task Commands", 256, NULL, 2, NULL);
    xTaskCreate(window_task_state, "Window Task State", 256, NULL, 3, NULL);
}

void button_callback(uint8_t gpio_num, button_event_t event) {
    switch(local_state) {
        case window_state_idle:
            if (gpio_num == button1_gpio) { 
                printf("Button: Setting homekit target position to 100\n");
                homekit_characteristic_notify(&target_position, HOMEKIT_UINT8(100));
            }
            else if (gpio_num == button2_gpio) {
                printf("Button: Setting homekit target position to 0\n");
                homekit_characteristic_notify(&target_position, HOMEKIT_UINT8(0));
            }
            break;
        case window_state_opening:
        case window_state_closing:
            stop_up(); stop_down();
            homekit_characteristic_notify(&target_position, HOMEKIT_UINT8(local_current_position));
            homekit_characteristic_notify(&current_position, HOMEKIT_UINT8(local_current_position));
            homekit_characteristic_notify(&position_state, HOMEKIT_UINT8(window_state_idle));
            local_state = window_state_idle;
            break;
    }
}

void buttons_init() {
    if (button_create(button1_gpio, 0, 4000, button_callback)) {
        printf("Failed to initialize button\n");
    }
    if (button_create(button2_gpio, 0, 4000, button_callback)) {
        printf("Failed to initialize button\n");
    }
}

void on_update_target_position(homekit_characteristic_t *ch, homekit_value_t value, void *context) {
    if (value.format != homekit_format_uint8) {
        printf("Invalid value format: %d\n", value.format);
        return;
    };
    local_target_position = value.int_value;
    printf("HOMEKIT: Setting target position to %d\n", local_target_position);
    if (local_target_position > local_current_position) {
        printf("HOMEKIT: Sending Window OPEN\n");
        send_window_command(window_command_open);
    }
    else if (local_target_position < local_current_position) {
        printf("HOMEKIT: Sending Window CLOSE\n");
        send_window_command(window_command_close);
    }
    else {
        printf("HOMEKIT: No need to change position\n");
    }
    return;
}


homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_window, .services=(homekit_service_t*[]){
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sonoff T1 Motor"),
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "ageorgios"),
            HOMEKIT_CHARACTERISTIC(SERIAL_NUMBER, "037A2BABF19D"),
            HOMEKIT_CHARACTERISTIC(MODEL, "Sonoff T1"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "0.1"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, led_identify),
            NULL
        }),
        HOMEKIT_SERVICE(WINDOW, .primary=true, .characteristics=(homekit_characteristic_t*[]){
            HOMEKIT_CHARACTERISTIC(NAME, "Sonoff T1 Motor"),
            &current_position,
            &target_position,
            &position_state,
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "111-11-111"
};

void user_init(void) {
    uart_set_baud(0, 115200);
    
    wifi_init();
    gpio_init();
    homekit_server_init(&config);
    window_init(20);
    buttons_init();
}
