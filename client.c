#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "pico/bootrom.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#include "lwip/apps/mqtt.h" // Cliente MQTT
#include "lwip/dns.h"       // DNS para resolver broker MQTT
#include "config.h"         // Configurações do usuário (SSID, senha, MQTT_SERVER)

#ifndef MQTT_SERVER
#error "MQTT_SERVER not defined in config.h or directly!"
#endif

#ifndef MQTT_TOPIC_LEN
#define MQTT_TOPIC_LEN 100
#endif

// --- Definições de Hardware e Estado do Ventilador ---
#define LED_CONNECTION_STATUS_PIN 12 // LED azul para status de conexão
#define BUZZER_PIN 10                // Pino do buzzer
#define JOYSTICK_X_PIN 27            // ADC1 para joystick
#define PIN_BUTTON_B 6               // Pino do botão B

static int fan_speed = 0;                      // Velocidade do ventilador (0-255)
static bool fan_on = false;                    // Estado do ventilador (ligado/desligado)
static bool auto_fan = false;                  // Modo automático do ventilador
static float current_simulated_temp_c = 25.0f; // Temperatura simulada
// --- FIM ---

// Estrutura para armazenar o estado do cliente MQTT
typedef struct MQTT_CLIENT_STATE_T
{
    mqtt_client_t *mqtt_client_inst;
    struct mqtt_connect_client_info_t client_info;
    char received_payload[128];
    char received_topic[MQTT_TOPIC_LEN];
    uint16_t received_payload_len;
    ip_addr_t mqtt_server_address;
    bool connection_attempt_done; // Flag para indicar se a primeira tentativa de conexão foi feita
} MQTT_CLIENT_STATE_T;

// Macros para debug e logs
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

// Parâmetros de operação
#define FAN_ADC_READ_INTERVAL_S 2
#define MQTT_KEEP_ALIVE_S 60
#define MQTT_SUBSCRIBE_QOS 1
#define MQTT_PUBLISH_QOS 1
#define MQTT_PUBLISH_RETAIN_STATUS 1
#define MQTT_PUBLISH_RETAIN_SIM_TEMP 0

#define MQTT_WILL_TOPIC "/pico/online"
#define MQTT_WILL_MESSAGE "0"
#define MQTT_WILL_QOS 1
#define MQTT_WILL_RETAIN 1

#ifndef MQTT_CLIENT_ID_PREFIX
#define MQTT_CLIENT_ID_PREFIX "pico_fan_simple" // Nome de cliente simplificado
#endif

// Protótipos das funções
static void mqtt_publish_request_cb(__unused void *arg, err_t err);
static void mqtt_subscribe_request_cb(void *arg, err_t err);
static void handle_incoming_mqtt_payload(void *arg, const u8_t *payload_data, u16_t payload_len, u8_t flags);
static void handle_incoming_mqtt_topic_info(void *arg, const char *topic_name, u32_t total_payload_len);
static void mqtt_connection_status_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
static void initiate_mqtt_connection(MQTT_CLIENT_STATE_T *current_state);
static void dns_resolution_cb(const char *hostname, const ip_addr_t *ip_address, void *callback_arg);
static void update_fan_buzzer_output();
static void publish_fan_status_and_temp(MQTT_CLIENT_STATE_T *current_state);
static void fan_joystick_adc_worker(async_context_t *context, async_at_time_worker_t *worker_task);
static void button_b_interrupt_handler(uint gpio_pin, uint32_t event_mask);

// Worker para ler o ADC do joystick periodicamente
static async_at_time_worker_t fan_adc_processing_worker = {.do_work = fan_joystick_adc_worker};

int main(void)
{
    stdio_init_all();
    sleep_ms(4000); // Aguarda estabilização da USB
    INFO_printf("MQTT Fan Controller (Simplified Structure) - Starting...\n");

    adc_init();
    adc_gpio_init(JOYSTICK_X_PIN); // Inicializa ADC para joystick

    // Inicializa LED de status
    gpio_init(LED_CONNECTION_STATUS_PIN);
    gpio_set_dir(LED_CONNECTION_STATUS_PIN, GPIO_OUT);
    gpio_put(LED_CONNECTION_STATUS_PIN, false);

    // Inicializa buzzer (PWM)
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    uint buzzer_pwm_slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    pwm_config pwm_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&pwm_cfg, 50);
    pwm_init(buzzer_pwm_slice_num, &pwm_cfg, false);

    // Inicializa botão B
    gpio_init(PIN_BUTTON_B);
    gpio_set_dir(PIN_BUTTON_B, GPIO_IN);
    gpio_pull_up(PIN_BUTTON_B);
    gpio_set_irq_enabled_with_callback(PIN_BUTTON_B, GPIO_IRQ_EDGE_FALL, true, &button_b_interrupt_handler);

    static MQTT_CLIENT_STATE_T client_state_data; // Instância única do estado do cliente

    if (cyw43_arch_init())
    {
        panic("CYW43 Architecture initialization failed!");
    }

    // Gera um ID único para o cliente MQTT
    char board_unique_id_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(board_unique_id_str, sizeof(board_unique_id_str));
    char mqtt_client_id_buffer[64];
    snprintf(mqtt_client_id_buffer, sizeof(mqtt_client_id_buffer), "%s_%s", MQTT_CLIENT_ID_PREFIX, board_unique_id_str);

    // Configura informações do cliente MQTT
    INFO_printf("Generated MQTT Client ID: %s\n", mqtt_client_id_buffer);
    client_state_data.client_info.client_id = mqtt_client_id_buffer;
    client_state_data.client_info.keep_alive = MQTT_KEEP_ALIVE_S;
    client_state_data.client_info.client_user = MQTT_USERNAME;
    client_state_data.client_info.client_pass = MQTT_PASSWORD;

    // Configura o Last Will and Testament (LWT)
    client_state_data.client_info.will_topic = MQTT_WILL_TOPIC;
    client_state_data.client_info.will_msg = MQTT_WILL_MESSAGE;
    client_state_data.client_info.will_qos = MQTT_WILL_QOS;
    client_state_data.client_info.will_retain = MQTT_WILL_RETAIN;

#if LWIP_ALTCP && LWIP_ALTCP_TLS && defined(MQTT_USE_TLS) && MQTT_USE_TLS == 1
#ifdef MQTT_CERT_INC
    static const uint8_t tls_root_ca_cert[] = TLS_ROOT_CERT;
    client_state_data.client_info.tls_config = altcp_tls_create_config_client(tls_root_ca_cert, sizeof(tls_root_ca_cert));
    INFO_printf("TLS for MQTT configured with CA certificate.\n");
#else
    client_state_data.client_info.tls_config = altcp_tls_create_config_client(NULL, 0);
    ERROR_printf("Warning: TLS for MQTT is enabled WITHOUT server certificate verification (INSECURE).\n");
#endif
#endif

    // Conecta à rede Wi-Fi
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

    // Resolve o endereço do broker MQTT via DNS
    cyw43_arch_lwip_begin();
    err_t dns_err = dns_gethostbyname(MQTT_SERVER, &client_state_data.mqtt_server_address, dns_resolution_cb, &client_state_data);
    cyw43_arch_lwip_end();

    // Se DNS OK, inicia conexão MQTT
    if (dns_err == ERR_OK)
    {
        initiate_mqtt_connection(&client_state_data);
    }
    else if (dns_err != ERR_INPROGRESS)
    {
        panic("DNS query initiation failed immediately!");
    }

    // Loop principal: mantém o programa rodando enquanto conectado ao MQTT
    while (!client_state_data.connection_attempt_done || (client_state_data.mqtt_client_inst && mqtt_client_is_connected(client_state_data.mqtt_client_inst)))
    {
        cyw43_arch_poll(); // Processa eventos de rede
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
    }

    INFO_printf("MQTT client main loop finished or connection lost.\n");
    // Ao sair, publica o WILL manualmente e desconecta
    if (client_state_data.mqtt_client_inst && mqtt_client_is_connected(client_state_data.mqtt_client_inst))
    {
        mqtt_publish(client_state_data.mqtt_client_inst, MQTT_WILL_TOPIC,
                     MQTT_WILL_MESSAGE, strlen(MQTT_WILL_MESSAGE), MQTT_WILL_QOS, MQTT_WILL_RETAIN,
                     mqtt_publish_request_cb, &client_state_data);
        sleep_ms(100);
        mqtt_disconnect(client_state_data.mqtt_client_inst);
    }

    // Desliga LED, buzzer e Wi-Fi ao finalizar
    gpio_put(LED_CONNECTION_STATUS_PIN, false);
    pwm_set_enabled(pwm_gpio_to_slice_num(BUZZER_PIN), false);
    cyw43_arch_deinit();
    INFO_printf("Application has finished.\n");
    return 0;
}

// Callback de publicação MQTT
static void mqtt_publish_request_cb(__unused void *arg, err_t err)
{
    if (err != ERR_OK)
    {
        ERROR_printf("MQTT publish request error: %d\n", err);
    }
}

// Callback de inscrição MQTT
static void mqtt_subscribe_request_cb(void *arg, err_t err)
{
    if (err != ERR_OK)
    {
        ERROR_printf("MQTT subscribe request error: %d\n", err);
    }
    else
    {
        DEBUG_printf("Subscription request successful.\n");
    }
}

// Atualiza o buzzer conforme o estado do ventilador
static void update_fan_buzzer_output()
{
    // Slice do PWM do buzzer
    uint buzzer_pwm_slice = pwm_gpio_to_slice_num(BUZZER_PIN);
    // Se o ventilador estiver ligado, ajusta a frequência do buzzer
    if (fan_on)
    {
        // Calcula a frequência do buzzer com base na velocidade do ventilador
        uint target_frequency = 300 + (uint)(((float)fan_speed / 255.0f) * 2000.0f);
        if (fan_speed == 0)
            target_frequency = 300; // Frequência mínima quando ventilador ligado mas sem velocidade

        if (target_frequency > 0)
        {

            uint32_t system_clock_hz = clock_get_hz(clk_sys);
            uint32_t pwm_clock_divider = 50; // Conforme configurado no main
            uint32_t pwm_effective_clock_hz = system_clock_hz / pwm_clock_divider;
            uint32_t wrap_value = (pwm_effective_clock_hz / target_frequency) - 1;

            // Limites de wrap_value
            if (wrap_value < 100)
                wrap_value = 100;
            if (wrap_value > 65535)
                wrap_value = 65535;

            pwm_set_wrap(buzzer_pwm_slice, wrap_value);
            pwm_set_gpio_level(BUZZER_PIN, wrap_value / 10); // 10% do ciclo de trabalho
            pwm_set_enabled(buzzer_pwm_slice, true);         // Habilita o PWM do buzzer
        }
        else
        {
            pwm_set_enabled(buzzer_pwm_slice, false);
        }
    }
    // Se o ventilador estiver desligado, desliga o buzzer
    else
    {
        pwm_set_enabled(buzzer_pwm_slice, false);
    }
}

// Publica o status do ventilador e temperatura simulada via MQTT
static void publish_fan_status_and_temp(MQTT_CLIENT_STATE_T *current_state)
{
    if (!current_state->connection_attempt_done || !mqtt_client_is_connected(current_state->mqtt_client_inst))
    {
        return;
    }
    char payload_buffer[20];

    // Publica o estado do ventilador, velocidade, modo automático e temperatura simulada
    mqtt_publish(current_state->mqtt_client_inst, "/fan/status/state",
                 fan_on ? "ON" : "OFF", fan_on ? 2 : 3,
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS,
                 mqtt_publish_request_cb, current_state);

    snprintf(payload_buffer, sizeof(payload_buffer), "%d", fan_speed);
    mqtt_publish(current_state->mqtt_client_inst, "/fan/status/speed",
                 payload_buffer, strlen(payload_buffer),
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS,
                 mqtt_publish_request_cb, current_state);

    mqtt_publish(current_state->mqtt_client_inst, "/fan/status/auto_mode",
                 auto_fan ? "ON" : "OFF", auto_fan ? 2 : 3,
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_STATUS,
                 mqtt_publish_request_cb, current_state);

    snprintf(payload_buffer, sizeof(payload_buffer), "%.1f", current_simulated_temp_c);
    mqtt_publish(current_state->mqtt_client_inst, "/fan/status/simulated_temp",
                 payload_buffer, strlen(payload_buffer),
                 MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN_SIM_TEMP,
                 mqtt_publish_request_cb, current_state);
}

// Worker periódico: lê joystick e ajusta fan_speed/temp no modo automático
static void fan_joystick_adc_worker(async_context_t *context, async_at_time_worker_t *worker_task)
{
    MQTT_CLIENT_STATE_T *current_state = (MQTT_CLIENT_STATE_T *)worker_task->user_data;
    bool fan_settings_changed_by_auto_mode = false;

    adc_select_input(1); // Seleciona ADC1 (joystick eixo x)
    uint16_t joystick_adc_raw_value = adc_read();

    // Se modo automático e ventilador ligado, ajusta velocidade e temperatura simulada
    if (auto_fan && fan_on)
    {
        int new_speed_from_joystick = joystick_adc_raw_value / 16; // Converte ADC para 0-255
        if (fan_speed != new_speed_from_joystick)
        {
            fan_speed = new_speed_from_joystick;
            fan_settings_changed_by_auto_mode = true;
        }
        float new_sim_temp = ((float)fan_speed / 255.0f) * 50.0f; // Atualiza temp simulada
        if ((current_simulated_temp_c - new_sim_temp) > 0.05f)
        {
            current_simulated_temp_c = new_sim_temp;
            fan_settings_changed_by_auto_mode = true;
        }

        if (fan_settings_changed_by_auto_mode)
        {
            update_fan_buzzer_output(); // Atualiza buzzer se necessário
        }
    }

    // Publica status se mudou ou para atualização periódica
    if (current_state->connection_attempt_done && mqtt_client_is_connected(current_state->mqtt_client_inst))
    {
        if (fan_settings_changed_by_auto_mode || (auto_fan && fan_on))
        {
            publish_fan_status_and_temp(current_state);
        }
    }
    async_context_add_at_time_worker_in_ms(context, worker_task, FAN_ADC_READ_INTERVAL_S * 1000);
}

// Callback de mensagem recebida via MQTT
static void handle_incoming_mqtt_payload(void *arg, const u8_t *payload_data, u16_t payload_len, u8_t flags)
{
    MQTT_CLIENT_STATE_T *current_state = (MQTT_CLIENT_STATE_T *)arg;

    if (payload_len < sizeof(current_state->received_payload))
    {
        memcpy(current_state->received_payload, payload_data, payload_len);
        current_state->received_payload[payload_len] = '\0';
    }
    else
    { // Trunca se maior
        memcpy(current_state->received_payload, payload_data, sizeof(current_state->received_payload) - 1);
        current_state->received_payload[sizeof(current_state->received_payload) - 1] = '\0';
    }
    current_state->received_payload_len = strlen(current_state->received_payload);

    DEBUG_printf("MQTT Message: Topic='%s', Payload='%s'\n", current_state->received_topic, current_state->received_payload);
    bool fan_settings_updated_by_command = false;

    // Comando para ligar/desligar ventilador
    if (strcmp(current_state->received_topic, "/fan/control/set_state") == 0)
    {
        bool new_fan_state = fan_on;
        if (strncmp(current_state->received_payload, "ON", current_state->received_payload_len) == 0)
            new_fan_state = true;
        else if (strncmp(current_state->received_payload, "OFF", current_state->received_payload_len) == 0)
            new_fan_state = false;
        else
        {
            INFO_printf("Invalid payload for /fan/control/set_state.\n");
            return;
        }

        if (fan_on != new_fan_state)
        {
            fan_on = new_fan_state;
            if (!fan_on)
                fan_speed = 0; // Se desligar, zera a velocidade
            fan_settings_updated_by_command = true;
        }
    }
    // Comando para alterar velocidade manualmente (só se não estiver no modo auto)
    else if (strcmp(current_state->received_topic, "/fan/control/set_speed") == 0)
    {
        int new_speed_val = atoi(current_state->received_payload);
        if (new_speed_val < 0)
            new_speed_val = 0;
        if (new_speed_val > 255)
            new_speed_val = 255;

        if (!auto_fan && fan_speed != new_speed_val)
        {
            fan_speed = new_speed_val;
            fan_settings_updated_by_command = true;
        }
    }
    // Comando para ativar/desativar modo automático
    else if (strcmp(current_state->received_topic, "/fan/control/set_auto_mode") == 0)
    {
        bool new_auto_mode_val = auto_fan;
        if (strncmp(current_state->received_payload, "ON", current_state->received_payload_len) == 0)
            new_auto_mode_val = true;
        else if (strncmp(current_state->received_payload, "OFF", current_state->received_payload_len) == 0)
            new_auto_mode_val = false;
        else
        {
            INFO_printf("Invalid payload for /fan/control/set_auto_mode.\n");
            return;
        }

        if (auto_fan != new_auto_mode_val)
        {
            auto_fan = new_auto_mode_val;
            fan_settings_updated_by_command = true;
            if (auto_fan && fan_on)
            { // Se modo auto ativado e ventilador ligado, atualiza fan_speed
                adc_select_input(1);
                uint16_t adc_val_now = adc_read();
                fan_speed = adc_val_now / 16;
                current_simulated_temp_c = ((float)fan_speed / 255.0f) * 50.0f;
            }
        }
    }
    // Comando para encerrar o programa
    else if (strcmp(current_state->received_topic, "/exit") == 0)
    {
        INFO_printf("MQTT /exit command received. Main loop will terminate if connection drops.\n");
        mqtt_disconnect(current_state->mqtt_client_inst); // Força a desconexão
    }

    if (fan_settings_updated_by_command)
    {
        update_fan_buzzer_output();
        publish_fan_status_and_temp(current_state);
    }
}

// Callback de tópico recebido via MQTT
static void handle_incoming_mqtt_topic_info(void *arg, const char *topic_name, u32_t total_payload_len)
{
    MQTT_CLIENT_STATE_T *current_state = (MQTT_CLIENT_STATE_T *)arg;
    strncpy(current_state->received_topic, topic_name, sizeof(current_state->received_topic) - 1);
    current_state->received_topic[sizeof(current_state->received_topic) - 1] = '\0';
}

// Callback de status de conexão MQTT
static void mqtt_connection_status_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    MQTT_CLIENT_STATE_T *current_state = (MQTT_CLIENT_STATE_T *)arg;
    current_state->connection_attempt_done = true; // Marca que a tentativa de conexão ocorreu

    if (status == MQTT_CONNECT_ACCEPTED)
    {
        gpio_put(LED_CONNECTION_STATUS_PIN, true);
        INFO_printf("Successfully connected to MQTT broker.\n");

        // Publica WILL reverso (online)
        mqtt_publish(client, MQTT_WILL_TOPIC, "1", 1, MQTT_WILL_QOS, MQTT_WILL_RETAIN, mqtt_publish_request_cb, current_state);

        // Inscreve-se nos tópicos de controle
        mqtt_sub_unsub(client, "/fan/control/set_state", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_cb, current_state, true);
        mqtt_sub_unsub(client, "/fan/control/set_speed", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_cb, current_state, true);
        mqtt_sub_unsub(client, "/fan/control/set_auto_mode", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_cb, current_state, true);
        mqtt_sub_unsub(client, "/exit", MQTT_SUBSCRIBE_QOS, mqtt_subscribe_request_cb, current_state, true);

        publish_fan_status_and_temp(current_state); // Publica estado inicial

        fan_adc_processing_worker.user_data = current_state;
        async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &fan_adc_processing_worker, 0);
    }
    else
    {
        gpio_put(LED_CONNECTION_STATUS_PIN, false);
        ERROR_printf("MQTT connection failed or disconnected. Status: %d\n", status);
    }
}

// Inicia conexão MQTT
static void initiate_mqtt_connection(MQTT_CLIENT_STATE_T *current_state)
{
#if LWIP_ALTCP && LWIP_ALTCP_TLS && defined(MQTT_USE_TLS) && MQTT_USE_TLS == 1
    const int port = MQTT_TLS_PORT;
#else
    const int port = MQTT_PORT;
#endif

    if (current_state->mqtt_client_inst == NULL)
    { // Cria instância apenas se não existir
        current_state->mqtt_client_inst = mqtt_client_new();
        if (!current_state->mqtt_client_inst)
        {
            panic("Failed to create MQTT client instance!");
        }
    }
    else if (mqtt_client_is_connected(current_state->mqtt_client_inst) || mqtt_client_is_connected(current_state->mqtt_client_inst))
    {
        INFO_printf("MQTT client is already connected or in the process of connecting.\n");
        return; // Evita múltiplas tentativas de conexão simultâneas
    }

    INFO_printf("Attempting to connect to MQTT server: %s, Port: %d\n", ipaddr_ntoa(&current_state->mqtt_server_address), port);

    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(current_state->mqtt_client_inst,
                                    &current_state->mqtt_server_address, port,
                                    mqtt_connection_status_cb, current_state,
                                    &current_state->client_info);
    cyw43_arch_lwip_end();

    if (err != ERR_OK)
    {
        ERROR_printf("Failed to initiate MQTT connection procedure: %d\n", err);
        gpio_put(LED_CONNECTION_STATUS_PIN, false);
        return;
    }

#if LWIP_ALTCP && LWIP_ALTCP_TLS && defined(MQTT_USE_TLS) && MQTT_USE_TLS == 1
    if (current_state->mqtt_client_inst && current_state->mqtt_client_inst->conn)
    {
        mbedtls_ssl_set_hostname(altcp_tls_context(current_state->mqtt_client_inst->conn), MQTT_SERVER);
    }
#endif
    mqtt_set_inpub_callback(current_state->mqtt_client_inst,
                            handle_incoming_mqtt_topic_info,
                            handle_incoming_mqtt_payload,
                            current_state);
}

// Callback de resolução de DNS
static void dns_resolution_cb(const char *hostname, const ip_addr_t *ip_address, void *callback_arg)
{
    MQTT_CLIENT_STATE_T *current_state = (MQTT_CLIENT_STATE_T *)callback_arg;
    if (ip_address)
    {
        current_state->mqtt_server_address = *ip_address;
        INFO_printf("DNS resolved for MQTT server '%s': %s\n", hostname, ipaddr_ntoa(ip_address));
        initiate_mqtt_connection(current_state);
    }
    else
    {
        ERROR_printf("Failed to resolve DNS for MQTT server: %s. Will retry via main loop.\n", hostname);
        gpio_put(LED_CONNECTION_STATUS_PIN, false);
        current_state->connection_attempt_done = false; // Permite nova tentativa de DNS/conexão no main loop
    }
}

// Handler de interrupção do botão B: reinicia em modo BOOTSEL
static void button_b_interrupt_handler(uint gpio_pin, uint32_t event_mask)
{
    if (gpio_pin == PIN_BUTTON_B && (event_mask & GPIO_IRQ_EDGE_FALL))
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