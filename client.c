#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "pico/bootrom.h"

#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h" // Adicionado para clock_get_hz

#include "lwip/apps/mqtt.h"
#include "lwip/dns.h"
#include "config.h" // Para WIFI_SSID, WIFI_PASSWORD, MQTT_SERVER, etc.

#ifndef MQTT_SERVER
#error "MQTT_SERVER not defined in config.h or directly!"
#endif

#ifdef MQTT_CERT_INC
#include MQTT_CERT_INC
#endif

#ifndef MQTT_TOPIC_LEN
#define MQTT_TOPIC_LEN 100
#endif

// --- Definições de Hardware e Estado do Ventilador ---
#define LED_CONNECTION_STATUS_PIN 12
#define BUZZER_PIN 10
#define JOYSTICK_X_PIN 27 // ADC1
#define PIN_BUTTON_B 6

static int fan_speed = 0;
static bool fan_on = false;
static bool auto_fan = false;
static float current_simulated_temp_c = 25.0f;
// --- FIM ---

typedef struct MQTT_CLIENT_DATA_T_
{
    mqtt_client_t *mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    char received_payload[MQTT_OUTPUT_RINGBUF_SIZE];
    char received_topic[MQTT_TOPIC_LEN];
    uint16_t received_payload_len;
    ip_addr_t mqtt_server_address;
    bool mqtt_connection_established;
    int subscribed_topics_count;
    bool client_operation_should_stop;
} MQTT_CLIENT_DATA_T;

#ifndef DEBUG_printf
#ifndef NDEBUG
#define DEBUG_printf printf
#else
#define DEBUG_printf(...)
#endif
#endif
#ifndef INFO_printf
#define INFO_printf printf
#endif
#ifndef ERROR_printf
#define ERROR_printf printf
#endif

#define FAN_ADC_READ_INTERVAL_S 2
#define MQTT_KEEP_ALIVE_S 60
#define MQTT_SUBSCRIBE_QOS 1
#define MQTT_PUBLISH_QOS 1
#define MQTT_PUBLISH_RETAIN_STATUS 1
#define MQTT_PUBLISH_RETAIN_SIM_TEMP 0 // Temperatura simulada pode não precisar ser retida

#define MQTT_LWT_TOPIC "/pico/online"
#define MQTT_LWT_MESSAGE "0"
#define MQTT_LWT_QOS 1
#define MQTT_LWT_RETAIN 1

#ifndef MQTT_CLIENT_ID_PREFIX
#define MQTT_CLIENT_ID_PREFIX "pico_fan_ctrl"
#endif

// Protótipos
static void mqtt_publish_request_callback(__unused void *arg, err_t err);
static void mqtt_subscribe_request_callback(void *arg, err_t err);
static void handle_incoming_mqtt_payload(void *arg, const u8_t *payload_data, u16_t payload_len, u8_t flags);
static void handle_incoming_mqtt_topic_info(void *arg, const char *topic_name, u32_t total_payload_len);
static void mqtt_connection_status_callback(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
static void initiate_mqtt_client_connection(MQTT_CLIENT_DATA_T *client_data);
static void dns_resolution_callback(const char *hostname, const ip_addr_t *ip_address, void *callback_arg);
static void update_fan_buzzer_output();
static void publish_fan_status_and_temp(MQTT_CLIENT_DATA_T *client_data); // Nome ajustado
static void fan_joystick_adc_worker(async_context_t *context, async_at_time_worker_t *worker_task);
static void button_b_interrupt_handler(uint gpio_pin, uint32_t event_mask);

static async_at_time_worker_t fan_adc_processing_worker = {.do_work = fan_joystick_adc_worker};

int main(void)
{
    stdio_init_all();
    sleep_ms(4000);
    INFO_printf("MQTT Fan Controller (Simplified for Core Functionality) - Starting...\n");

    adc_init();
    adc_gpio_init(JOYSTICK_X_PIN); // ADC1 para Joystick

    gpio_init(LED_CONNECTION_STATUS_PIN);
    gpio_set_dir(LED_CONNECTION_STATUS_PIN, GPIO_OUT);
    gpio_put(LED_CONNECTION_STATUS_PIN, false);

    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint buzzer_pwm_slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&pwm_cfg, 50);
    pwm_init(buzzer_pwm_slice_num, &pwm_cfg, false);

    gpio_init(PIN_BUTTON_B);
    gpio_set_dir(PIN_BUTTON_B, GPIO_IN);
    gpio_pull_up(PIN_BUTTON_B);
    gpio_set_irq_enabled_with_callback(PIN_BUTTON_B, GPIO_IRQ_EDGE_FALL, true, &button_b_interrupt_handler);

    static MQTT_CLIENT_DATA_T client_data_instance;

    if (cyw43_arch_init())
    {
        panic("CYW43 Architecture initialization failed!");
    }

    char board_unique_id_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(board_unique_id_str, sizeof(board_unique_id_str));
    char mqtt_client_id_buffer[64];
    snprintf(mqtt_client_id_buffer, sizeof(mqtt_client_id_buffer), "%s_%s", MQTT_CLIENT_ID_PREFIX, board_unique_id_str);

    INFO_printf("Generated MQTT Client ID: %s\n", mqtt_client_id_buffer);
    client_data_instance.mqtt_client_info.client_id = mqtt_client_id_buffer;
    client_data_instance.mqtt_client_info.keep_alive = MQTT_KEEP_ALIVE_S;
    client_data_instance.mqtt_client_info.client_user = MQTT_USERNAME;
    client_data_instance.mqtt_client_info.client_pass = MQTT_PASSWORD;

    static char lwt_topic_buffer[MQTT_TOPIC_LEN];
    strncpy(lwt_topic_buffer, MQTT_LWT_TOPIC, sizeof(lwt_topic_buffer) - 1);
    lwt_topic_buffer[sizeof(lwt_topic_buffer) - 1] = '\0';
    client_data_instance.mqtt_client_info.will_topic = lwt_topic_buffer;
    client_data_instance.mqtt_client_info.will_msg = MQTT_LWT_MESSAGE;
    client_data_instance.mqtt_client_info.will_qos = MQTT_LWT_QOS;
    client_data_instance.mqtt_client_info.will_retain = MQTT_LWT_RETAIN;

#if LWIP_ALTCP && LWIP_ALTCP_TLS && defined(MQTT_USE_TLS) && MQTT_USE_TLS == 1
#ifdef MQTT_CERT_INC
    static const uint8_t tls_root_ca_cert[] = TLS_ROOT_CERT;
    client_data_instance.mqtt_client_info.tls_config = altcp_tls_create_config_client(tls_root_ca_cert, sizeof(tls_root_ca_cert));
    INFO_printf("TLS for MQTT configured with CA certificate.\n");
#else
    client_data_instance.mqtt_client_info.tls_config = altcp_tls_create_config_client(NULL, 0);
    ERROR_printf("Warning: TLS for MQTT is enabled WITHOUT server certificate verification (INSECURE).\n");
#endif
#endif

    cyw43_arch_enable_sta_mode();
    INFO_printf("Connecting to Wi-Fi network: %s...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000))
    {
        panic("Failed to connect to Wi-Fi network!");
    }
    INFO_printf("Successfully connected to Wi-Fi.\n");
    if (netif_default)
    {
        INFO_printf("Device IP Address: %s\n", ipaddr_ntoa(netif_ip_addr4(netif_default)));
    }

    cyw43_arch_lwip_begin();
    err_t dns_initiation_err = dns_gethostbyname(MQTT_SERVER, &client_data_instance.mqtt_server_address, dns_resolution_callback, &client_data_instance);
    cyw43_arch_lwip_end();

    if (dns_initiation_err == ERR_OK)
    {
        initiate_mqtt_client_connection(&client_data_instance);
    }
    else if (dns_initiation_err != ERR_INPROGRESS)
    {
        panic("DNS query initiation failed immediately!");
    }

    while (!client_data_instance.client_operation_should_stop)
    {
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));

        if (!client_data_instance.mqtt_connection_established &&
            (client_data_instance.mqtt_client_inst == NULL || !mqtt_client_is_connected(client_data_instance.mqtt_client_inst)))
        {
            if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) >= CYW43_LINK_UP)
            {
                INFO_printf("MQTT disconnected. Attempting to reconnect...\n");
                gpio_put(LED_CONNECTION_STATUS_PIN, false);
                cyw43_arch_lwip_begin();
                dns_initiation_err = dns_gethostbyname(MQTT_SERVER, &client_data_instance.mqtt_server_address, dns_resolution_callback, &client_data_instance);
                cyw43_arch_lwip_end();
                if (dns_initiation_err == ERR_OK)
                {
                    initiate_mqtt_client_connection(&client_data_instance);
                }
                else if (dns_initiation_err != ERR_INPROGRESS)
                {
                    ERROR_printf("DNS query for MQTT broker failed during reconnect attempt.\n");
                }
            }
            else
            {
                ERROR_printf("Wi-Fi link is down. Cannot attempt MQTT reconnection.\n");
                gpio_put(LED_CONNECTION_STATUS_PIN, false);
            }
            sleep_ms(5000);
        }
    }

    INFO_printf("MQTT client main loop terminated.\n");
    if (client_data_instance.mqtt_client_inst && mqtt_client_is_connected(client_data_instance.mqtt_client_inst))
    {
        mqtt_publish(client_data_instance.mqtt_client_inst, client_data_instance.mqtt_client_info.will_topic,
                     MQTT_LWT_MESSAGE, strlen(MQTT_LWT_MESSAGE), MQTT_LWT_QOS, MQTT_LWT_RETAIN,
                     mqtt_publish_request_callback, &client_data_instance);
        sleep_ms(100);
        mqtt_disconnect(client_data_instance.mqtt_client_inst);
    }
    gpio_put(LED_CONNECTION_STATUS_PIN, false);
    pwm_set_enabled(pwm_gpio_to_slice_num(BUZZER_PIN), false);
    cyw43_arch_deinit();
    INFO_printf("Application has finished.\n");
    return 0;
}

static void mqtt_publish_request_callback(__unused void *arg, err_t err)
{
    if (err != ERR_OK)
    {
        ERROR_printf("MQTT publish request callback error: %d\n", err);
    }
}

static void mqtt_subscribe_request_callback(void *arg, err_t err)
{
    MQTT_CLIENT_DATA_T *client_data = (MQTT_CLIENT_DATA_T *)arg;
    if (err != ERR_OK)
    {
        ERROR_printf("MQTT subscribe request callback error: %d\n", err);
    }
    else
    {
        client_data->subscribed_topics_count++;
        DEBUG_printf("Successfully subscribed to a topic. Total subscriptions: %d\n", client_data->subscribed_topics_count);
    }
}

static void update_fan_buzzer_output()
{
    uint buzzer_pwm_slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    if (fan_on)
    {
        uint target_frequency = 300 + (uint)(((float)fan_speed / 255.0f) * 2000.0f);
        if (fan_speed == 0 && fan_on)
            target_frequency = 300; // Frequência mínima se fan_speed=0 mas fan_on=true
        else if (fan_speed == 0)
            target_frequency = 0;

        if (target_frequency > 0)
        {
            uint32_t system_clock_hz = clock_get_hz(clk_sys);
            uint32_t pwm_clock_divider = 50;
            uint32_t pwm_effective_clock_hz = system_clock_hz / pwm_clock_divider;
            uint32_t wrap_value = (pwm_effective_clock_hz / target_frequency) - 1;

            if (wrap_value < 100)
                wrap_value = 100;
            if (wrap_value > 0xFFFF)
                wrap_value = 0xFFFF;

            pwm_set_wrap(buzzer_pwm_slice, wrap_value);
            pwm_set_gpio_level(BUZZER_PIN, wrap_value / 10);
            pwm_set_enabled(buzzer_pwm_slice, true);
        }
        else
        {
            pwm_set_enabled(buzzer_pwm_slice, false);
        }
    }
    else
    {
        pwm_set_enabled(buzzer_pwm_slice, false);
    }
}

// Publica os status solicitados: state, speed, auto_mode, simulated_temp
static void publish_fan_status_and_temp(MQTT_CLIENT_DATA_T *client_data)
{
    if (!client_data->mqtt_connection_established || !mqtt_client_is_connected(client_data->mqtt_client_inst))
    {
        return;
    }
    char payload_buffer[20];

    mqtt_publish(client_data->mqtt_client_inst, "/fan/status/state",
                 fan_on ? "ON" : "OFF", fan_on ? 2 : 3,
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS,
                 mqtt_publish_request_callback, client_data);

    snprintf(payload_buffer, sizeof(payload_buffer), "%d", fan_speed);
    mqtt_publish(client_data->mqtt_client_inst, "/fan/status/speed",
                 payload_buffer, strlen(payload_buffer),
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS,
                 mqtt_publish_request_callback, client_data);

    mqtt_publish(client_data->mqtt_client_inst, "/fan/status/auto_mode",
                 auto_fan ? "ON" : "OFF", auto_fan ? 2 : 3,
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS,
                 mqtt_publish_request_callback, client_data);

    snprintf(payload_buffer, sizeof(payload_buffer), "%.1f", current_simulated_temp_c);
    mqtt_publish(client_data->mqtt_client_inst, "/fan/status/simulated_temp",
                 payload_buffer, strlen(payload_buffer),
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_SIM_TEMP,
                 mqtt_publish_request_callback, client_data);
}

static void fan_joystick_adc_worker(async_context_t *context, async_at_time_worker_t *worker_task)
{
    MQTT_CLIENT_DATA_T *client_data = (MQTT_CLIENT_DATA_T *)worker_task->user_data;
    bool fan_speed_or_temp_changed_by_auto = false;

    adc_select_input(1); // ADC1 para JOYSTICK_X_PIN
    uint16_t joystick_adc_raw = adc_read();
    // O valor do joystick não é mais publicado diretamente para simplificar, apenas usado internamente.

    if (auto_fan && fan_on)
    {
        int new_calculated_speed = joystick_adc_raw / 16; // 0-4095 -> 0-255
        if (fan_speed != new_calculated_speed)
        {
            fan_speed = new_calculated_speed;
            fan_speed_or_temp_changed_by_auto = true; // Marcar para notificar a mudança de velocidade
        }
        // Atualiza current_simulated_temp_c sempre que auto_fan está ativo,
        // pois a "condição" (joystick) pode mudar mesmo que a velocidade discretizada (0-255) não mude.
        float new_simulated_temp = ((float)fan_speed / 255.0f) * 50.0f; // Simula temp 0-50C
        if (abs(current_simulated_temp_c - new_simulated_temp) > 0.05f)
        { // Verifica se houve mudança na temp
            current_simulated_temp_c = new_simulated_temp;
            fan_speed_or_temp_changed_by_auto = true; // Marca para notificar a mudança de temperatura
        }

        if (fan_speed_or_temp_changed_by_auto)
        {
            update_fan_buzzer_output(); // Atualiza o buzzer se a velocidade mudou
        }
    }
    if (client_data->mqtt_connection_established && mqtt_client_is_connected(client_data->mqtt_client_inst))
    {
        if (fan_speed_or_temp_changed_by_auto)
        {
            publish_fan_status_and_temp(client_data);
        }
    }
    async_context_add_at_time_worker_in_ms(context, worker_task, FAN_ADC_READ_INTERVAL_S * 500);
}

static void handle_incoming_mqtt_payload(void *arg, const u8_t *payload_data, u16_t payload_len, u8_t flags)
{
    MQTT_CLIENT_DATA_T *client_data = (MQTT_CLIENT_DATA_T *)arg;
    const char *base_topic = client_data->received_topic; // Usa a função para obter o nome base

    if (payload_len < sizeof(client_data->received_payload))
    {
        memcpy(client_data->received_payload, payload_data, payload_len);
        client_data->received_payload[payload_len] = '\0';
    }
    else
    {
        memcpy(client_data->received_payload, payload_data, sizeof(client_data->received_payload) - 1);
        client_data->received_payload[sizeof(client_data->received_payload) - 1] = '\0';
        ERROR_printf("Incoming MQTT payload for topic '%s' was truncated.\n", client_data->received_topic);
    }

    DEBUG_printf("MQTT Message: Topic='%s', Payload='%s'\n", client_data->received_topic, client_data->received_payload);
    bool fan_settings_changed_by_command = false;

    if (strcmp(base_topic, "/fan/control/set_state") == 0)
    {
        bool new_state = fan_on;
        if (strncmp(client_data->received_payload, "ON", payload_len) == 0)
            new_state = true;
        else if (strncmp(client_data->received_payload, "OFF", payload_len) == 0)
            new_state = false;
        else
        {
            INFO_printf("Invalid payload for /fan/control/set_state.\n");
            return;
        }
        if (fan_on != new_state)
        {
            fan_on = new_state;
            if (!fan_on)
                fan_speed = 0;
            fan_settings_changed_by_command = true;
        }
    }
    else if (strcmp(base_topic, "/fan/control/set_speed") == 0)
    {
        int new_speed = atoi(client_data->received_payload);
        if (new_speed < 0)
            new_speed = 0;
        if (new_speed > 255)
            new_speed = 255;
        if (fan_speed != new_speed || auto_fan)
        {
            fan_speed = new_speed;
            auto_fan = false;
            fan_settings_changed_by_command = true;
        }
    }
    else if (strcmp(base_topic, "/fan/control/set_auto_mode") == 0)
    {
        bool new_auto = auto_fan;
        if (strncmp(client_data->received_payload, "ON", payload_len) == 0)
            new_auto = true;
        else if (strncmp(client_data->received_payload, "OFF", payload_len) == 0)
            new_auto = false;
        else
        {
            INFO_printf("Invalid payload for /fan/control/set_auto_mode.\n");
            return;
        }
        if (auto_fan != new_auto)
        {
            auto_fan = new_auto;
            fan_settings_changed_by_command = true;
            if (auto_fan && fan_on)
            {
                adc_select_input(1);
                uint16_t current_adc_val = adc_read();
                fan_speed = current_adc_val / 16;
                current_simulated_temp_c = ((float)fan_speed / 255.0f) * 50.0f;
            }
        }
    }
    else if (strcmp(base_topic, "/exit") == 0)
    { // Mantido para desligamento limpo
        INFO_printf("MQTT /exit command received. Shutting down client...\n");
        client_data->client_operation_should_stop = true;
    }

    if (fan_settings_changed_by_command)
    {
        update_fan_buzzer_output();
        publish_fan_status_and_temp(client_data);
    }
}

static void handle_incoming_mqtt_topic_info(void *arg, const char *topic_name, u32_t total_payload_len)
{
    MQTT_CLIENT_DATA_T *client_data = (MQTT_CLIENT_DATA_T *)arg;
    strncpy(client_data->received_topic, topic_name, sizeof(client_data->received_topic) - 1);
    client_data->received_topic[sizeof(client_data->received_topic) - 1] = '\0';
}

static void mqtt_connection_status_callback(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    MQTT_CLIENT_DATA_T *client_data = (MQTT_CLIENT_DATA_T *)arg;
    client_data->mqtt_connection_established = false;

    if (status == MQTT_CONNECT_ACCEPTED)
    {
        client_data->mqtt_connection_established = true;
        gpio_put(LED_CONNECTION_STATUS_PIN, true); // LED AZUL (GPIO12) LIGADO
        INFO_printf("Successfully connected to MQTT broker.\n");

        mqtt_publish(client, "/fan/status/connection", "ON", 2, MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS, mqtt_publish_request_callback, client_data);

        if (client_data->mqtt_client_info.will_topic && mqtt_client_is_connected(client))
        {
            mqtt_publish(client, client_data->mqtt_client_info.will_topic, "1", 1, MQTT_LWT_QOS, MQTT_LWT_RETAIN, mqtt_publish_request_callback, client_data);
        }

        if (mqtt_client_is_connected(client))
        {
            mqtt_sub_unsub(client, "/fan/control/set_state", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_callback, client_data, true);
            mqtt_sub_unsub(client, "/fan/control/set_speed", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_callback, client_data, true);
            mqtt_sub_unsub(client, "/fan/control/set_auto_mode", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_callback, client_data, true);
            mqtt_sub_unsub(client, "/exit", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_callback, client_data, true); // Mantém /exit
        }

        publish_fan_status_and_temp(client_data); // Publica estado inicial

        fan_adc_processing_worker.user_data = client_data; // Worker do Joystick/ADC
        async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &fan_adc_processing_worker, 0);
    }
    else
    {
        gpio_put(LED_CONNECTION_STATUS_PIN, false); // LED AZUL (GPIO12) DESLIGADO
        ERROR_printf("MQTT connection attempt failed or disconnected. Status: %d\n", status);
        mqtt_publish(client, "/fan/status/connection", "OFF", 3, MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS, mqtt_publish_request_callback, client_data);
    }
}

static void initiate_mqtt_client_connection(MQTT_CLIENT_DATA_T *client_data)
{
#if LWIP_ALTCP && LWIP_ALTCP_TLS && defined(MQTT_USE_TLS) && MQTT_USE_TLS == 1
    const int port_to_connect = MQTT_TLS_PORT;
#else
    const int port_to_connect = MQTT_PORT;
#endif

    if (client_data->mqtt_client_inst == NULL)
    {
        client_data->mqtt_client_inst = mqtt_client_new();
        if (!client_data->mqtt_client_inst)
        {
            panic("Failed to create new MQTT client instance!");
        }
    }
    else if (mqtt_client_is_connected(client_data->mqtt_client_inst) || mqtt_client_is_connected(client_data->mqtt_client_inst))
    {
        INFO_printf("MQTT client is already connected or connecting. Skipping.\n");
        return;
    }

    INFO_printf("Connecting to MQTT server: %s, Port: %d\n", ipaddr_ntoa(&client_data->mqtt_server_address), port_to_connect);

    cyw43_arch_lwip_begin();
    err_t connection_init_err = mqtt_client_connect(client_data->mqtt_client_inst,
                                                    &client_data->mqtt_server_address, port_to_connect,
                                                    mqtt_connection_status_callback, client_data,
                                                    &client_data->mqtt_client_info);
    cyw43_arch_lwip_end();

    if (connection_init_err != ERR_OK)
    {
        ERROR_printf("Failed to initiate MQTT connection procedure: %d\n", connection_init_err);
        client_data->mqtt_connection_established = false;
        gpio_put(LED_CONNECTION_STATUS_PIN, false);
        return;
    }

#if LWIP_ALTCP && LWIP_ALTCP_TLS && defined(MQTT_USE_TLS) && MQTT_USE_TLS == 1
    if (client_data->mqtt_client_inst && client_data->mqtt_client_inst->conn)
    {
        mbedtls_ssl_set_hostname(altcp_tls_context(client_data->mqtt_client_inst->conn), MQTT_SERVER);
    }
    else
    {
        ERROR_printf("MQTT connection object (conn) is NULL; cannot set hostname for TLS SNI.\n");
    }
#endif
    mqtt_set_inpub_callback(client_data->mqtt_client_inst,
                            handle_incoming_mqtt_topic_info,
                            handle_incoming_mqtt_payload,
                            client_data);
}

static void dns_resolution_callback(const char *hostname, const ip_addr_t *ip_address, void *callback_arg)
{
    MQTT_CLIENT_DATA_T *client_data = (MQTT_CLIENT_DATA_T *)callback_arg;
    if (ip_address)
    {
        client_data->mqtt_server_address = *ip_address;
        INFO_printf("DNS resolved for MQTT server '%s': %s\n", hostname, ipaddr_ntoa(ip_address));
        initiate_mqtt_client_connection(client_data);
    }
    else
    {
        ERROR_printf("Failed to resolve DNS for MQTT server: %s. Will retry.\n", hostname);
        client_data->mqtt_connection_established = false;
        gpio_put(LED_CONNECTION_STATUS_PIN, false);
    }
}

static void button_b_interrupt_handler(uint gpio_pin, uint32_t event_mask)
{
    if (gpio_pin == PIN_BUTTON_B)
    {
        static uint32_t last_press_timestamp_ms = 0;
        uint32_t current_timestamp_ms = to_ms_since_boot(get_absolute_time());
        if (current_timestamp_ms - last_press_timestamp_ms > 300)
        {
            last_press_timestamp_ms = current_timestamp_ms;
            reset_usb_boot(0, 0);
        }
    }
}