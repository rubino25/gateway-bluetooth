// LED RGB di stato (tre canali LEDC a 5 kHz) e pulsante utente.
//
// Un solo task fa tutto: campiona il pulsante ogni 10 ms (antirimbalzo) e
// aggiorna il colore ogni 100 ms leggendo lo stato di rete e Bluetooth.
//
// Priorità delle indicazioni:
//   1. "Find Device": giallo lampeggiante per 10 s (anche a LED disattivato)
//   2. LED disattivato: spento
//   3. controller collegato: verde fisso
//   4. rete presente (link e indirizzo IP): blu fisso
//   5. rete assente: blu lampeggiante

#include "led.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "eth.h"
#include "gw_platform.h"
#include "settings.h"

static const char *TAG = "led";

#define LED_MODE      LEDC_LOW_SPEED_MODE
#define LED_TIMER     LEDC_TIMER_1
#define LED_FREQ_HZ   5000
#define LED_RES       LEDC_TIMER_8_BIT
#define LED_LEVEL     128 // luminosità (0..255) dei colori di stato
// Nello YAML ESPHome di Sonoff le uscite non sono invertite: LED attivi alti.
#define LED_ACTIVE_LOW 0

#define TICK_MS            10
#define LED_UPDATE_MS      100
#define NET_BLINK_MS       500
#define FIND_BLINK_MS      250
#define FIND_DURATION_MS   10000
#define DEBOUNCE_MS        30
#define SHORT_PRESS_MAX_MS 1500

// Core del task: quello che non ospita il Bluetooth (controller e BTstack
// stanno sul core 0, vedi sdkconfig), così i 100 risvegli al secondo del
// pulsante non interrompono mai BTstack.
#if CONFIG_FREERTOS_UNICORE
#define LED_CORE 0
#elif defined(CONFIG_BTDM_CTRL_PINNED_TO_CORE) && CONFIG_BTDM_CTRL_PINNED_TO_CORE == 1
#define LED_CORE 0
#else
#define LED_CORE 1
#endif

static const int s_led_gpio[3] = {CONFIG_GW_LED_R_GPIO, CONFIG_GW_LED_G_GPIO, CONFIG_GW_LED_B_GPIO};
static const ledc_channel_t s_led_ch[3] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2};

static bool s_started;
static bool s_ledc_ok;
static volatile bool s_enabled = true;
static volatile uint32_t s_find_requests; // incrementato da led_find_device()

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    static int32_t last = -1;
    int32_t packed = ((int32_t)r << 16) | ((int32_t)g << 8) | b;
    if (!s_ledc_ok || packed == last) {
        return;
    }
    last = packed;
    const uint8_t v[3] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        ledc_set_duty(LED_MODE, s_led_ch[i], v[i]);
        ledc_update_duty(LED_MODE, s_led_ch[i]);
    }
}

static esp_err_t init_ledc(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LED_MODE,
        .duty_resolution = LED_RES,
        .timer_num = LED_TIMER,
        .freq_hz = LED_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        const ledc_channel_config_t ch = {
            .gpio_num = s_led_gpio[i],
            .speed_mode = LED_MODE,
            .channel = s_led_ch[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LED_TIMER,
            .duty = 0,
            .hpoint = 0,
            .flags.output_invert = LED_ACTIVE_LOW,
        };
        err = ledc_channel_config(&ch);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void init_button(void)
{
    // GPIO34 è solo ingresso e non ha pull-up/pull-down interni: la resistenza
    // di pull-up è sulla scheda.
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GW_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static bool button_raw(void)
{
    return gpio_get_level(CONFIG_GW_BUTTON_GPIO) == 0; // attivo basso
}

static void update_led(int64_t now, int64_t find_until)
{
    if (now < find_until) {
        bool on = ((now / FIND_BLINK_MS) % 2) == 0;
        set_rgb(on ? LED_LEVEL : 0, on ? LED_LEVEL : 0, 0); // giallo
        return;
    }
    if (!s_enabled) {
        set_rgb(0, 0, 0);
        return;
    }

    gw_bt_status_t bt = {0};
    gw_bt_get_status(&bt);
    if (bt.connected) {
        set_rgb(0, LED_LEVEL, 0);
        return;
    }

    eth_status_t eth = {0};
    eth_get_status(&eth);
    if (!(eth.link_up && eth.has_ip)) {
        bool on = ((now / NET_BLINK_MS) % 2) == 0;
        set_rgb(0, 0, on ? LED_LEVEL : 0);
        return;
    }
    if (bt.pairing_enabled) {
        // Abbinamento attivo: verde lampeggiante veloce, riscontro del pulsante.
        bool on = ((now / FIND_BLINK_MS) % 2) == 0;
        set_rgb(0, on ? LED_LEVEL : 0, 0);
        return;
    }
    set_rgb(0, 0, LED_LEVEL);
}

static void led_task(void *arg)
{
    TickType_t period = pdMS_TO_TICKS(TICK_MS);
    if (period == 0) {
        period = 1;
    }

    // Stato iniziale del pulsante = lettura attuale: un pulsante già premuto
    // all'accensione non conta come pressione.
    bool pressed = button_raw();
    bool candidate = pressed;
    int64_t candidate_since = now_ms();
    int64_t pressed_at = candidate_since;
    bool ignore_release = pressed;

    uint32_t find_seen = s_find_requests;
    int64_t find_until = 0;
    int64_t next_led = 0;

    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, period);
        int64_t now = now_ms();

        // Pulsante: il nuovo livello vale solo se resta stabile per DEBOUNCE_MS.
        bool raw = button_raw();
        if (raw != candidate) {
            candidate = raw;
            candidate_since = now;
        } else if (candidate != pressed && now - candidate_since >= DEBOUNCE_MS) {
            pressed = candidate;
            if (pressed) {
                pressed_at = now;
            } else if (ignore_release) {
                ignore_release = false;
            } else if (now - pressed_at < SHORT_PRESS_MAX_MS) {
                ESP_LOGI(TAG, "pulsante: abbinamento Bluetooth abilitato");
                gw_bt_set_pairing(true);
            }
        }

        uint32_t find_req = s_find_requests;
        if (find_req != find_seen) {
            find_seen = find_req;
            find_until = now + FIND_DURATION_MS;
            next_led = 0;
        }

        if (now >= next_led) {
            next_led = now + LED_UPDATE_MS;
            update_led(now, find_until);
        }
    }
}

esp_err_t led_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    gw_settings_t cfg;
    settings_get(&cfg);
    s_enabled = cfg.led_enabled;

    esp_err_t err = init_ledc();
    s_ledc_ok = (err == ESP_OK);
    if (!s_ledc_ok) {
        ESP_LOGE(TAG, "LEDC: %s", esp_err_to_name(err));
    }

    // Il pulsante funziona anche se il LED non è partito.
    init_button();
    if (xTaskCreatePinnedToCore(led_task, "led", 4096, NULL, 3, NULL, LED_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return err;
}

void led_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

void led_find_device(void)
{
    // Chiamata da un solo task (pagina web): un incremento semplice basta; il
    // task del LED vede il cambiamento e fa partire i 10 secondi.
    s_find_requests++;
}
