// xbox-eth-gateway: firmware custom per SONOFF Dongle Max (Dongle-M).
// Controller Xbox via Bluetooth LE -> UDP su Ethernet -> PC, più ponte Zigbee
// (MG24 <-> TCP 6638) e pagina web di gestione.

#include "esp_err.h"
#include "esp_log.h"

#include "eth.h"
#include "gw_platform.h"
#include "led.h"
#include "mg24_bridge.h"
#include "net_link.h"
#include "settings.h"
#include "web_ui.h"

static const char *TAG = "main";

void app_main(void)
{
    // Le impostazioni servono a tutti gli altri moduli: senza non si parte.
    ESP_ERROR_CHECK(settings_init());

    // Gli altri moduli non sono bloccanti: se uno fallisce si prosegue, così il
    // ponte Zigbee e la pagina web restano raggiungibili per la diagnosi.
    esp_err_t err;

    err = led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED non avviato: %s", esp_err_to_name(err));
    }

    err = eth_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet non avviata: %s", esp_err_to_name(err));
    }

    err = mg24_bridge_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ponte MG24 non avviato: %s", esp_err_to_name(err));
    }

    err = net_link_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Collegamento col PC non avviato: %s", esp_err_to_name(err));
    }

    err = web_ui_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pagina web non avviata: %s", esp_err_to_name(err));
    }

    // Entra nel run loop di BTstack e non ritorna.
    gw_bt_run();
}
