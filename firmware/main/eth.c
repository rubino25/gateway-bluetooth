// Ethernet: EMAC interno dell'ESP32 + PHY IP101 in RMII, clock esterno a 50 MHz
// in ingresso su GPIO0. DHCP o IP statico dalle impostazioni, mDNS con hostname
// e servizio _http. Il Wi-Fi non viene mai inizializzato.
//
// Il driver si installa da un task temporaneo sul core che non ospita il
// Bluetooth: l'EMAC alloca l'interrupt sul core che lo crea e, con
// ETH_MAC_FLAG_PIN_TO_CORE, fissa lì anche il task di ricezione "emac_rx".

#include "eth.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "settings.h"

static const char *TAG = "eth";

// Sull'ESP32 il clock RMII esterno può entrare solo da GPIO0.
_Static_assert(CONFIG_GW_ETH_CLK_GPIO == 0, "Il clock RMII in ingresso sull'ESP32 e' solo su GPIO0");

// Tempi di alimentazione del PHY: spento, poi acceso e attesa che l'oscillatore
// a 50 MHz sia stabile prima di inizializzare l'EMAC (che senza clock non esce
// dal reset software).
#define PHY_POWER_OFF_MS 20
#define PHY_POWER_ON_MS  50

#define HTTP_PORT 80

// Core del driver Ethernet: quello che non ospita il Bluetooth (controller e
// BTstack stanno sul core 0, vedi sdkconfig).
#if CONFIG_FREERTOS_UNICORE
#define ETH_CORE 0
#elif defined(CONFIG_BTDM_CTRL_PINNED_TO_CORE) && CONFIG_BTDM_CTRL_PINNED_TO_CORE == 1
#define ETH_CORE 0
#else
#define ETH_CORE 1
#endif
#define INSTALL_TASK_STACK 4096
#define INSTALL_TASK_PRIO  5

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static eth_status_t s_status;

static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static bool s_started;

// Rilegge indirizzi e DNS dall'interfaccia e aggiorna lo stato.
static void update_status(bool set_link, bool link_up, bool set_ip, bool has_ip)
{
    eth_status_t st;
    taskENTER_CRITICAL(&s_lock);
    st = s_status;
    taskEXIT_CRITICAL(&s_lock);

    if (set_link) {
        st.link_up = link_up;
    }
    if (set_ip) {
        st.has_ip = has_ip;
    }
    if (!st.link_up) {
        st.has_ip = false;
    }

    st.ip[0] = st.netmask[0] = st.gateway[0] = st.dns[0] = '\0';
    if (st.has_ip && s_netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            esp_ip4addr_ntoa(&info.ip, st.ip, sizeof(st.ip));
            esp_ip4addr_ntoa(&info.netmask, st.netmask, sizeof(st.netmask));
            if (info.gw.addr != 0) {
                esp_ip4addr_ntoa(&info.gw, st.gateway, sizeof(st.gateway));
            }
        }
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
            esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, st.dns, sizeof(st.dns));
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_status = st;
    taskEXIT_CRITICAL(&s_lock);
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link su");
        update_status(true, true, false, false);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link giu'");
        update_status(true, false, true, false);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "driver avviato");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "driver fermato");
        update_status(true, false, true, false);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        if (ev->esp_netif != s_netif) {
            return;
        }
        ESP_LOGI(TAG, "indirizzo " IPSTR ", gateway " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        // Il link è per forza su: lo si segna anche qui nel caso l'evento
        // CONNECTED venga elaborato dopo.
        update_status(true, true, true, true);
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "indirizzo perso");
        update_status(false, false, true, false);
    }
}

// Alimentazione del PHY (e del suo oscillatore). All'avvio dell'ESP32 il pin è
// basso, così GPIO0 non riceve il clock durante lo strapping.
static void phy_power_on(void)
{
#if CONFIG_GW_ETH_POWER_GPIO >= 0
    gpio_set_level(CONFIG_GW_ETH_POWER_GPIO, 0);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GW_ETH_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(PHY_POWER_OFF_MS));
    gpio_set_level(CONFIG_GW_ETH_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(PHY_POWER_ON_MS));
#endif
}

// Applica l'IP statico. Ritorna false se i valori non sono utilizzabili: in
// quel caso resta attivo il DHCP.
static bool apply_static_ip(const gw_settings_t *cfg)
{
    esp_netif_ip_info_t info = {0};
    if (esp_netif_str_to_ip4(cfg->eth_ip, &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->eth_netmask, &info.netmask) != ESP_OK) {
        return false;
    }
    if (cfg->eth_gateway[0] != '\0' && esp_netif_str_to_ip4(cfg->eth_gateway, &info.gw) != ESP_OK) {
        return false;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "dhcpc_stop: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_netif_set_ip_info(s_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info: %s", esp_err_to_name(err));
        esp_netif_dhcpc_start(s_netif);
        return false;
    }

    if (cfg->eth_dns[0] != '\0') {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(cfg->eth_dns, &dns.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
    }
    return true;
}

static void start_mdns(const gw_settings_t *cfg)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set(cfg->hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set: %s", esp_err_to_name(err));
    }
    mdns_instance_name_set(cfg->device_name);
    err = mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mDNS: %s.local", cfg->hostname);
    }
}

static esp_err_t install_driver_on_core(void);

esp_err_t eth_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    gw_settings_t cfg;
    settings_get(&cfg);

    // esp_netif_init() si può chiamare più volte; il loop di default potrebbe
    // essere già stato creato da un altro modulo.
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_ERR_NO_MEM, TAG, "esp_netif_new");

    // L'hostname va impostato prima che parta il DHCP (finisce nell'opzione 12).
    err = esp_netif_set_hostname(s_netif, cfg.hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_hostname: %s", esp_err_to_name(err));
    }

    err = install_driver_on_core();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac_addr[6] = {0};
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_addr);

    bool dhcp = cfg.eth_dhcp;
    if (!dhcp && !apply_static_ip(&cfg)) {
        ESP_LOGW(TAG, "IP statico non valido: uso il DHCP");
        dhcp = true;
    }

    taskENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.dhcp = dhcp;
    memcpy(s_status.mac, mac_addr, sizeof(s_status.mac));
    taskEXIT_CRITICAL(&s_lock);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL), TAG,
                        "handler ETH_EVENT");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL), TAG,
                        "handler GOT_IP");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_ip_event, NULL), TAG,
                        "handler LOST_IP");

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
    ESP_RETURN_ON_FALSE(glue != NULL, ESP_ERR_NO_MEM, TAG, "esp_eth_new_netif_glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, glue), TAG, "esp_netif_attach");

    // mDNS prima dell'avvio del driver, così vede gli eventi dell'interfaccia.
    // Un errore qui non impedisce l'uso della rete.
    start_mdns(&cfg);

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth), TAG, "esp_eth_start");
    s_started = true;

    ESP_LOGI(TAG, "avviata: MAC %02x:%02x:%02x:%02x:%02x:%02x, %s, driver sul core %d", mac_addr[0], mac_addr[1],
             mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5], dhcp ? "DHCP" : "IP statico", ETH_CORE);
    return ESP_OK;
}

void eth_get_status(eth_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

// Accende il PHY e installa MAC, PHY e driver in s_eth. Va eseguita sul core
// ETH_CORE (vedi install_driver_on_core()).
static esp_err_t install_driver(void)
{
    phy_power_on();

    // MAC interno: RMII con clock a 50 MHz fornito dall'esterno su GPIO0.
    // Task di ricezione fissato sul core corrente (ETH_CORE).
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_GW_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_GW_ETH_MDIO_GPIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)CONFIG_GW_ETH_CLK_GPIO;

    // PHY IP101. Il pin di alimentazione è già gestito sopra: nessun reset
    // hardware da parte del driver (il default della macro sarebbe GPIO5).
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_GW_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_FAIL, TAG, "esp_eth_mac_new_esp32");
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "esp_eth_phy_new_ip101");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        // Causa tipica: manca il clock a 50 MHz (PHY non alimentato) oppure
        // indirizzo PHY o pin MDC/MDIO sbagliati.
        ESP_LOGE(TAG, "esp_eth_driver_install: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        s_eth = NULL;
        return err;
    }
    return ESP_OK;
}

// Mai cancellato: il task temporaneo lo usa una sola volta e dopo il "give" non
// tocca più nulla del chiamante.
static SemaphoreHandle_t s_install_done;
static esp_err_t s_install_err;

static void install_task(void *arg)
{
    s_install_err = install_driver();
    xSemaphoreGive(s_install_done);
    vTaskDelete(NULL);
}

// Esegue install_driver() su ETH_CORE e ne aspetta l'esito. Se il task non si
// può creare, l'installazione avviene nel task chiamante.
static esp_err_t install_driver_on_core(void)
{
    s_install_done = xSemaphoreCreateBinary();
    if (s_install_done != NULL &&
        xTaskCreatePinnedToCore(install_task, "eth_install", INSTALL_TASK_STACK, NULL, INSTALL_TASK_PRIO, NULL,
                                ETH_CORE) == pdPASS) {
        xSemaphoreTake(s_install_done, portMAX_DELAY);
        return s_install_err;
    }
    ESP_LOGW(TAG, "task di installazione non creato: driver sul core del chiamante");
    return install_driver();
}
