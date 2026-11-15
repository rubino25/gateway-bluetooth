// Ponte seriale tra l'EFR32MG24 e un client TCP (Zigbee2MQTT, ZHA,
// universal-silabs-flasher).
//
// Due task:
// - "mg24_tcp": accetta le connessioni e copia TCP -> UART. È l'unico che apre
//   e chiude i socket. Una nuova connessione sostituisce quella esistente, così
//   un client che si ricollega dopo una caduta non resta fuori.
// - "mg24_uart": aspetta gli eventi del driver UART e copia UART -> TCP.
// Il descrittore del client è condiviso tra i due sotto s_tx_lock: chi invia lo
// tiene per la durata della send(), chi sostituisce o chiude il client lo
// prende prima di cambiare il descrittore.
//
// Entrambi i task e l'interrupt della UART stanno sul core che non ospita il
// Bluetooth: il driver UART alloca l'interrupt sul core del task che lo
// installa, per questo l'installazione avviene dentro "mg24_uart".
//
// Sequenze di reset e bootloader: quelle dello YAML ESPHome di Sonoff per il
// Dongle-M (pin bootloader basso/alto, impulso di reset attivo basso di 100 ms,
// bootloader sempre a 115200 baud).

#include "mg24_bridge.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "settings.h"

static const char *TAG = "mg24";

#define MG24_UART        ((uart_port_t)CONFIG_GW_MG24_UART_NUM)
#define BOOTLOADER_BAUD  115200
#define SEQ_DELAY_MS     100

// Buffer del driver UART. A 921600 baud arrivano circa 92 kB/s: 16 kB di
// ricezione coprono oltre 150 ms di ritardo del task prima di perdere dati.
#define UART_RX_BUF      16384
#define UART_TX_BUF      8192
#define UART_QUEUE_LEN   32
#define UART_CHUNK       2048
#define TCP_CHUNK        1024

#define TCP_SEND_TIMEOUT_S 1
#define TCP_KEEPIDLE_S     10
#define TCP_KEEPINTVL_S    5
#define TCP_KEEPCNT_N      3
#define SELECT_TIMEOUT_MS  500

#define TASK_STACK       4096
// Sopra il server HTTP e i task UDP (5), sotto lo stack TCP/IP (18) e la
// ricezione Ethernet (15): Zigbee2MQTT non tollera ritardi lunghi sul ponte.
#define TASK_PRIO        10

// Core dei task del ponte: quello che non ospita il Bluetooth (controller e
// BTstack stanno sul core 0, vedi sdkconfig).
#if CONFIG_FREERTOS_UNICORE
#define BRIDGE_CORE 0
#elif defined(CONFIG_BTDM_CTRL_PINNED_TO_CORE) && CONFIG_BTDM_CTRL_PINNED_TO_CORE == 1
#define BRIDGE_CORE 0
#else
#define BRIDGE_CORE 1
#endif

static SemaphoreHandle_t s_ctl_lock; // serializza reset, bootloader e cambio baud
static SemaphoreHandle_t s_tx_lock;  // protegge s_client_fd durante send() e chiusura
static portMUX_TYPE s_stat_lock = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t s_uart_queue;
static SemaphoreHandle_t s_uart_ready; // dato da mg24_uart dopo l'installazione
static esp_err_t s_uart_init_err;
static int s_client_fd = -1;       // scritto solo da mg24_tcp, sotto s_tx_lock
static volatile bool s_client_dead; // send() fallita: mg24_tcp deve chiudere
static uint16_t s_tcp_port;
static bool s_started;

static mg24_status_t s_status;
static int64_t s_connected_since_us;

// ---------------------------------------------------------------------------
// Stato
// ---------------------------------------------------------------------------

static void status_set_client(bool connected, const char *ip)
{
    taskENTER_CRITICAL(&s_stat_lock);
    s_status.client_connected = connected;
    if (connected) {
        strlcpy(s_status.client_ip, ip, sizeof(s_status.client_ip));
        s_connected_since_us = esp_timer_get_time();
    } else {
        s_status.client_ip[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_stat_lock);
}

void mg24_get_status(mg24_status_t *out)
{
    if (out == NULL) {
        return;
    }
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_stat_lock);
    *out = s_status;
    out->connected_s = s_status.client_connected ? (uint32_t)((now - s_connected_since_us) / 1000000) : 0;
    taskEXIT_CRITICAL(&s_stat_lock);
}

// ---------------------------------------------------------------------------
// UART e GPIO dell'MG24
// ---------------------------------------------------------------------------

// Cambia il baud rate della UART, dopo aver svuotato la trasmissione, e scarta
// quanto ricevuto alla velocità precedente.
static esp_err_t apply_baud(uint32_t baud)
{
    uart_wait_tx_done(MG24_UART, pdMS_TO_TICKS(200));
    esp_err_t err = uart_set_baudrate(MG24_UART, baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_baudrate(%lu): %s", (unsigned long)baud, esp_err_to_name(err));
        return err;
    }
    uart_flush_input(MG24_UART);
    taskENTER_CRITICAL(&s_stat_lock);
    s_status.baud = baud;
    taskEXIT_CRITICAL(&s_stat_lock);
    return ESP_OK;
}

static void pulse_reset(void)
{
    gpio_set_level(CONFIG_GW_MG24_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(SEQ_DELAY_MS));
    gpio_set_level(CONFIG_GW_MG24_RESET_GPIO, 1);
}

// Reset in modalità applicazione. Chiamare con s_ctl_lock preso.
static esp_err_t reset_to_app_locked(void)
{
    gw_settings_t cfg;
    settings_get(&cfg);

    gpio_set_level(CONFIG_GW_MG24_BOOT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(SEQ_DELAY_MS));
    pulse_reset();

    taskENTER_CRITICAL(&s_stat_lock);
    s_status.bootloader_mode = false;
    taskEXIT_CRITICAL(&s_stat_lock);

    return apply_baud(cfg.mg24_baud);
}

esp_err_t mg24_reset(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    esp_err_t err = reset_to_app_locked();
    xSemaphoreGive(s_ctl_lock);
    ESP_LOGI(TAG, "reset in modalita' applicazione");
    return err;
}

esp_err_t mg24_enter_bootloader(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);

    taskENTER_CRITICAL(&s_stat_lock);
    s_status.bootloader_mode = true;
    taskEXIT_CRITICAL(&s_stat_lock);

    // Il bootloader Gecko parla sempre a 115200.
    esp_err_t err = apply_baud(BOOTLOADER_BAUD);
    vTaskDelay(pdMS_TO_TICKS(SEQ_DELAY_MS));
    gpio_set_level(CONFIG_GW_MG24_BOOT_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(SEQ_DELAY_MS));
    pulse_reset();
    // Il pin bootloader resta alto finché non si esce con mg24_exit_bootloader()
    // o mg24_reset().

    xSemaphoreGive(s_ctl_lock);
    ESP_LOGW(TAG, "MG24 in bootloader (UART a %d baud)", BOOTLOADER_BAUD);
    return err;
}

esp_err_t mg24_exit_bootloader(void)
{
    return mg24_reset();
}

esp_err_t mg24_set_baud(uint32_t baud)
{
    if (baud != 115200 && baud != 460800 && baud != 921600) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    gw_settings_t cfg;
    settings_get(&cfg);
    cfg.mg24_baud = baud;
    esp_err_t err = settings_set(&cfg);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_stat_lock);
        bool in_bootloader = s_status.bootloader_mode;
        taskEXIT_CRITICAL(&s_stat_lock);
        // In bootloader la UART resta a 115200: il nuovo valore si applica
        // all'uscita.
        if (!in_bootloader) {
            err = apply_baud(baud);
        }
    }
    xSemaphoreGive(s_ctl_lock);
    return err;
}

// Prima di un riavvio dell'ESP32 il pin bootloader torna basso: GPIO12 è un pin
// di strapping (tensione della flash) e non deve essere alto al reset.
static void on_shutdown(void)
{
    gpio_set_level(CONFIG_GW_MG24_BOOT_GPIO, 0);
}

static esp_err_t init_gpio(void)
{
    // Il livello si imposta prima di abilitare l'uscita, così il pin non
    // passa mai dal valore sbagliato: bootloader basso, reset alto (inattivo).
    gpio_set_level(CONFIG_GW_MG24_BOOT_GPIO, 0);
    gpio_set_level(CONFIG_GW_MG24_RESET_GPIO, 1);
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_GW_MG24_BOOT_GPIO) | (1ULL << CONFIG_GW_MG24_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    gpio_set_level(CONFIG_GW_MG24_BOOT_GPIO, 0);
    gpio_set_level(CONFIG_GW_MG24_RESET_GPIO, 1);
    return ESP_OK;
}

static esp_err_t init_uart(uint32_t baud)
{
    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, // nessun hardware flow control
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(MG24_UART, UART_RX_BUF, UART_TX_BUF, UART_QUEUE_LEN, &s_uart_queue, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(MG24_UART, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(MG24_UART, CONFIG_GW_MG24_TX_GPIO, CONFIG_GW_MG24_RX_GPIO, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");
    taskENTER_CRITICAL(&s_stat_lock);
    s_status.baud = baud;
    taskEXIT_CRITICAL(&s_stat_lock);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// UART -> TCP
// ---------------------------------------------------------------------------

static bool send_all(int fd, const uint8_t *data, size_t len)
{
    while (len > 0) {
        int n = send(fd, data, len, 0);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            // Errore o timeout di invio (client bloccato): si abbandona.
            return false;
        }
    }
    return true;
}

// Inoltra al client i dati arrivati dall'MG24; senza client si scartano.
static void forward_to_client(const uint8_t *data, size_t len)
{
    bool sent = false;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if (s_client_fd >= 0 && !s_client_dead) {
        if (send_all(s_client_fd, data, len)) {
            sent = true;
        } else {
            ESP_LOGW(TAG, "invio al client fallito (errno %d)", errno);
            s_client_dead = true;
        }
    }
    xSemaphoreGive(s_tx_lock);

    if (sent) {
        taskENTER_CRITICAL(&s_stat_lock);
        s_status.bytes_from_radio += (uint32_t)len;
        taskEXIT_CRITICAL(&s_stat_lock);
    }
}

static void uart_task(void *arg)
{
    // Installazione del driver da qui, così l'interrupt finisce su questo core.
    uint32_t baud = (uint32_t)(uintptr_t)arg;
    uint8_t *buf = malloc(UART_CHUNK);
    esp_err_t err = buf != NULL ? init_uart(baud) : ESP_ERR_NO_MEM;
    s_uart_init_err = err;
    xSemaphoreGive(s_uart_ready);
    if (err != ESP_OK) {
        free(buf);
        vTaskDelete(NULL);
        return;
    }

    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_uart_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (ev.type) {
        case UART_DATA:
            // Si svuota tutto quello che c'è, non solo ev.size.
            for (;;) {
                size_t avail = 0;
                if (uart_get_buffered_data_len(MG24_UART, &avail) != ESP_OK || avail == 0) {
                    break;
                }
                if (avail > UART_CHUNK) {
                    avail = UART_CHUNK;
                }
                int n = uart_read_bytes(MG24_UART, buf, avail, 0);
                if (n <= 0) {
                    break;
                }
                forward_to_client(buf, (size_t)n);
            }
            break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "UART: dati persi (%s)", ev.type == UART_FIFO_OVF ? "FIFO" : "buffer pieno");
            uart_flush_input(MG24_UART);
            xQueueReset(s_uart_queue);
            break;
        default:
            // Errori di frame o parità: capitano durante reset e cambio di baud.
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// TCP -> UART e gestione delle connessioni
// ---------------------------------------------------------------------------

static void drop_client(const char *reason)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int fd = s_client_fd;
    s_client_fd = -1;
    s_client_dead = false;
    xSemaphoreGive(s_tx_lock);

    if (fd >= 0) {
        close(fd);
        status_set_client(false, NULL);
        ESP_LOGI(TAG, "client scollegato (%s)", reason);
    }
}

static void accept_client(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        ESP_LOGW(TAG, "accept: errno %d", errno);
        return;
    }

    int one = 1;
    int idle = TCP_KEEPIDLE_S, intvl = TCP_KEEPINTVL_S, cnt = TCP_KEEPCNT_N;
    struct timeval snd_to = {.tv_sec = TCP_SEND_TIMEOUT_S, .tv_usec = 0};
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

    // La nuova connessione sostituisce quella vecchia.
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int old = s_client_fd;
    s_client_fd = fd;
    s_client_dead = false;
    xSemaphoreGive(s_tx_lock);
    if (old >= 0) {
        close(old);
        ESP_LOGW(TAG, "client precedente sostituito");
    }

    char ip[16] = "";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    status_set_client(true, ip);
    ESP_LOGI(TAG, "client collegato da %s", ip);
}

static int open_listener(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void tcp_task(void *arg)
{
    static uint8_t buf[TCP_CHUNK];

    for (;;) {
        int listen_fd = open_listener(s_tcp_port);
        if (listen_fd < 0) {
            ESP_LOGE(TAG, "porta TCP %u non aperta (errno %d), riprovo", (unsigned)s_tcp_port, errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "in ascolto sulla porta TCP %u", (unsigned)s_tcp_port);

        for (;;) {
            int client = s_client_fd; // questo task è l'unico che lo modifica
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd, &rfds);
            int max_fd = listen_fd;
            if (client >= 0) {
                FD_SET(client, &rfds);
                if (client > max_fd) {
                    max_fd = client;
                }
            }
            struct timeval tv = {.tv_sec = 0, .tv_usec = SELECT_TIMEOUT_MS * 1000};
            int r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ESP_LOGE(TAG, "select: errno %d", errno);
                break;
            }

            if (client >= 0 && s_client_dead) {
                drop_client("errore di invio");
                continue;
            }
            if (r == 0) {
                continue;
            }

            if (FD_ISSET(listen_fd, &rfds)) {
                // Dopo la sostituzione "client" non è più valido: si ricomincia.
                accept_client(listen_fd);
                continue;
            }

            if (client >= 0 && FD_ISSET(client, &rfds)) {
                int n = recv(client, buf, sizeof(buf), 0);
                if (n > 0) {
                    uart_write_bytes(MG24_UART, buf, (size_t)n);
                    taskENTER_CRITICAL(&s_stat_lock);
                    s_status.bytes_to_radio += (uint32_t)n;
                    taskEXIT_CRITICAL(&s_stat_lock);
                } else if (n == 0) {
                    drop_client("chiuso dal client");
                } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    drop_client("errore di ricezione");
                }
            }
        }

        drop_client("server riavviato");
        close(listen_fd);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------
// Avvio
// ---------------------------------------------------------------------------

esp_err_t mg24_bridge_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    gw_settings_t cfg;
    settings_get(&cfg);
    s_tcp_port = cfg.mg24_tcp_port;

    // Prima di tutto i GPIO: bootloader basso e reset inattivo.
    ESP_RETURN_ON_ERROR(init_gpio(), TAG, "GPIO");
    esp_register_shutdown_handler(on_shutdown);

    s_ctl_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    // Mai cancellato: il task lo usa una sola volta, all'avvio.
    s_uart_ready = xSemaphoreCreateBinary();
    if (s_ctl_lock == NULL || s_tx_lock == NULL || s_uart_ready == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // I socket richiedono lo stack TCP/IP: lo si inizializza anche qui nel caso
    // l'Ethernet non sia partita (la chiamata è ripetibile).
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");

    // Il task installa la UART e poi segnala l'esito.
    if (xTaskCreatePinnedToCore(uart_task, "mg24_uart", TASK_STACK, (void *)(uintptr_t)cfg.mg24_baud, TASK_PRIO,
                                NULL, BRIDGE_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_uart_ready, portMAX_DELAY);
    ESP_RETURN_ON_ERROR(s_uart_init_err, TAG, "UART");

    // Reset dell'MG24 in modalità applicazione, come fa il firmware originale.
    xSemaphoreTake(s_ctl_lock, portMAX_DELAY);
    esp_err_t err = reset_to_app_locked();
    xSemaphoreGive(s_ctl_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset iniziale: %s", esp_err_to_name(err));
    }

    if (xTaskCreatePinnedToCore(tcp_task, "mg24_tcp", TASK_STACK, NULL, TASK_PRIO, NULL, BRIDGE_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "ponte avviato: UART%d a %lu baud, TCP %u", CONFIG_GW_MG24_UART_NUM,
             (unsigned long)cfg.mg24_baud, (unsigned)s_tcp_port);
    return ESP_OK;
}
