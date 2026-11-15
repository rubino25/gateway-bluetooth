# Tutti i .c di questa cartella vengono compilati: per aggiungere un modulo basta
# creare il file (e rilanciare la configurazione di CMake).
file(GLOB gw_srcs "${CMAKE_CURRENT_LIST_DIR}/*.c")

# I file della pagina web hanno nomi fissi: non aggiungerne altri senza
# aggiornare questa lista.
idf_component_register(
    SRCS ${gw_srcs}
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html" "www/app.css" "www/app.js"
    EMBED_FILES "www/dongle.png"
    PRIV_REQUIRES
        bluepad32 btstack
        esp_eth esp_netif esp_event lwip
        esp_http_server app_update
        nvs_flash driver esp_timer mbedtls json
        mdns
)
