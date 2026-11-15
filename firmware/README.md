# Firmware xbox-eth-gateway

Firmware custom per **SONOFF Dongle Max (Dongle-M)**: collega un controller Xbox
(Bluetooth LE) oppure un DualSense PS5 (Bluetooth classico), ne inoltra lo stato al
PC via UDP su Ethernet, e mantiene il ponte Zigbee (MG24 ↔ TCP 6638) e una pagina web
di gestione.

> **Attenzione.** Il flash cancella il firmware originale del dongle. Prima di
> scrivere qualsiasi cosa sulla flash leggi "Controlli prima del primo flash".

## Prerequisiti

1. **ESP-IDF v5.5.x** (versione indicata da Bluepad32), installato con l'installer
   ufficiale per Windows: <https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/>.
   I comandi vanno dati dal terminale "ESP-IDF 5.5 PowerShell" creato dall'installer,
   oppure dopo aver eseguito `export.ps1` dalla cartella di ESP-IDF.
2. **Bluepad32 5.0.0-beta0** con BTstack in `third_party/bluepad32` (cartella alla
   radice del progetto, accanto a `firmware/`). È un submodule: dopo il clone
   basta `git submodule update --init --recursive`. In alternativa, a mano:

   ```powershell
   git clone --depth 1 --branch 5.0.0-beta0 https://github.com/ricardoquesada/bluepad32 third_party/bluepad32
   git -C third_party/bluepad32 submodule update --init --depth 1 external/btstack
   ```

   Per usare una copia in un'altra cartella basta impostare la variabile
   d'ambiente `BLUEPAD32_ROOT`.
3. Connessione a Internet alla prima configurazione: l'IDF Component Manager
   scarica `espressif/mdns` in `firmware/managed_components/`.

Licenze: Bluepad32 è Apache 2.0; BTstack è gratuito solo per uso non commerciale
(vedi `third_party/bluepad32/external/btstack/LICENSE`).

## Compilare

Dalla cartella `firmware/`:

```powershell
idf.py set-target esp32
idf.py build
```

`set-target` va dato una volta sola (crea `sdkconfig` da `sdkconfig.defaults`). Se
cambi `sdkconfig.defaults`, cancella `sdkconfig` e rilancia `set-target`.

All'avvio della configurazione compaiono due avvisi Kconfig di BTstack
(`BTSTACK_INTERNAL_BT ... "visible if"` e `BT ... defined without a type`): vengono
dal sorgente di BTstack e si possono ignorare.

## Configurazione (`idf.py menuconfig`)

| Menu | Cosa contiene |
|------|---------------|
| `Xbox Ethernet Gateway` | pin di Ethernet, MG24, LED e pulsante; nome, hostname, IP del PC, porta UDP (valori di default: diventano le impostazioni iniziali, poi modificabili dalla pagina web) |
| `Component config → Xbox Gateway - Bluetooth` | modalità di abbinamento BLE, Bluetooth classico, comportamento all'avvio, latenza (priorità, connessione BLE, statistiche) |
| `Component config → Bluepad32` | numero massimo di dispositivi, livello dei log |

I pin di default sono quelli del Dongle Max: non serve cambiarli.

### Opzioni Bluetooth

- **Abbinamento BLE**: di default "Legacy + bonding", cioè il comportamento di
  Bluepad32 5.0.0-beta0, che ha disattivato "LE Secure Connections" per
  compatibilità. Se il controller Xbox non si abbina o non si ricollega, prova
  "LE Secure Connections + bonding" e poi le altre voci. Dopo ogni cambio usa
  "dimentica i controller" dalla pagina web e rifai l'abbinamento.
- **Bluetooth classico (BR/EDR)**: acceso, serve al DualSense (il controller Xbox
  Series usa solo BLE). Il gateway resta raggiungibile in BR/EDR finché non c'è un
  controller collegato; la ricerca di controller classic nuovi gira solo con
  l'abbinamento attivo. Spegnendolo restano solo i controller BLE.
  Se nella cartella c'è già uno `sdkconfig` creato prima di questa modifica, il
  nuovo default non viene applicato da solo: cambia la voce da `menuconfig` oppure
  cancella `sdkconfig` e rilancia `set-target`.
- **Accetta nuovi controller a ogni avvio**: spento. All'avvio l'abbinamento è
  attivo solo se non c'è ancora nessun controller memorizzato.

### Latenza

- **Priorità del run loop Bluetooth**: 19, sopra rete, ponte MG24 e pagina web
  (ESP-IDF di default darebbe 1). Richiede il tick di FreeRTOS a 1000 Hz, già in
  `sdkconfig.defaults`; con 100 Hz il log all'avvio lo segnala.
- **Connessione BLE a bassa latenza**: acceso. Il gateway apre la connessione con
  intervallo 7,5 ms e latency 0 (BTstack di default: 10–30 ms, latency 4).
  - Il controller può chiedere altri parametri. Nel log compaiono i valori usati
    (`Connessione BLE aperta/aggiornata: intervallo …`) e le sue richieste
    (`Il controller chiede …`).
  - Se il controller rallenta la connessione, prova "Rifiuta le richieste di
    connessione BLE più lenta di 10 ms". Alcuni controller, se rifiutati, si
    scollegano.
- **Statistiche nel log**: accese. Ogni 10 s, con un controller collegato, una riga
  `Tempi 10 s (…)` riporta:
  - l'intervallo BLE in uso;
  - l'intervallo tra due report consecutivi (minimo, medio, massimo e per fasce);
  - il tempo dal report all'invio UDP.

  Per misurare l'intervallo reale muovi uno stick di continuo: il controller Xbox
  manda un report solo quando qualcosa cambia.
- **IP del PC**: impostalo dalla pagina web. Se resta vuoto, il gateway parte in
  broadcast e passa da solo all'unicast verso il PC appena questo risponde (il
  ricevitore manda un pacchetto al secondo). Torna al broadcast dopo 5 s di
  silenzio.
  - Col PC in Wi-Fi il broadcast può arrivare con decine di ms di ritardo.
  - Con due ricevitori accesi sulla stessa rete, senza IP impostato riceve l'ultimo
    che si è fatto sentire.

## Controlli prima del primo flash

Li fa l'utente, con il dongle collegato via USB. `<PORTA>` è la porta seriale del
dongle (per esempio `COM5`).

```powershell
esptool.py --chip auto --port <PORTA> flash_id
esptool.py --chip auto --port <PORTA> get_security_info
```

- `flash_id` deve riportare una flash da **16 MB**.
- Leggi con attenzione l'esito di `get_security_info`. **Se risultano attivi
  flash encryption o secure boot, fermati qui**: il firmware custom non si può
  scrivere in modo normale e un tentativo può rendere il dongle inutilizzabile.

Backup della flash originale (facoltativo ma consigliato, dura qualche minuto):

```powershell
esptool.py --chip auto --port <PORTA> read_flash 0 ALL backup_originale.bin
```

Il backup contiene anche i dati di calibrazione e le impostazioni del firmware
originale. Conservalo: non va pubblicato.

## Flash

Lo fa l'utente, solo dopo i controlli qui sopra. Dalla cartella `firmware/`:

```powershell
idf.py -p <PORTA> flash monitor
```

Il primo flash scrive bootloader, tabella delle partizioni e applicazione, e
cancella il firmware originale. Il chip Zigbee (MG24) ha una flash sua e **non
viene toccato**.

`monitor` mostra i log sulla seriale (115200 baud); si esce con `Ctrl+]`.

Dopo il primo flash gli aggiornamenti si possono fare dalla pagina web (upload
OTA di `build/xbox_eth_gateway.bin`), senza USB.

### Partizioni (flash da 16 MB)

| Nome | Uso | Dimensione |
|------|-----|------------|
| `nvs` | impostazioni, password (hash), chiavi Bluetooth | 64 KB |
| `otadata` | slot OTA attivo | 8 KB |
| `phy_init` | calibrazione radio | 4 KB |
| `coredump` | diagnostica dopo un crash | 80 KB |
| `ota_0`, `ota_1` | due copie del firmware (aggiornamento OTA) | 4 MB l'una |

## Abbinamento del controller

Al primo avvio il gateway è già in abbinamento. Altrimenti avvialo dalla pagina
web, sezione Controller (interruttore Pairing): vale per entrambi i controller.

**Controller Xbox (Series X|S, Bluetooth LE)**

1. Accendi il controller e tieni premuto il tasto Pair (in alto, vicino alla porta
   USB) per 3 secondi: il logo lampeggia veloce.
2. Quando il controller è collegato il logo resta acceso fisso e l'abbinamento si
   chiude da solo.

**DualSense (PS5, Bluetooth classico)**

1. A controller spento tieni premuti insieme **Create** (a sinistra del touchpad) e
   **PS** finché la barra luminosa lampeggia veloce.
2. Il gateway lo trova entro una decina di secondi; a collegamento fatto la barra
   resta accesa fissa (blu) e l'abbinamento si chiude da solo.

La pagina web mostra il tipo di controller collegato e il tipo di collegamento
(Bluetooth LE o Bluetooth Classic).

Sul PC (protocollo v2, `docs/protocol.md`; serve il `pc-receiver` aggiornato):

- il controller Xbox compare come pad **Xbox 360** virtuale;
- il DualSense compare come **DualSense** virtuale: tasti, stick, grilletti
  analogici, touchpad con clic, giroscopio, accelerometro, tasto microfono e
  batteria arrivano al PC; vibrazione, colore della barra luminosa e LED giocatore
  chiesti dal gioco tornano al controller. In alternativa (`dualsense = "xbox360"`
  in `pc-receiver/config.toml`) compare come pad Xbox 360: Croce = A, Cerchio = B,
  Quadrato = X, Triangolo = Y, Create = Back, Options = Start, PS = Guide.
- Non arrivano: grilletti adattivi, aptica audio, altoparlante e microfono del
  DualSense (VIIPER 0.9.0 non li gestisce).

Da sapere:

- Ogni controller ricorda **un solo host Bluetooth**: abbinarlo al gateway gli fa
  perdere l'abbinamento Bluetooth con il PC o con la console (e viceversa).
- Si gestisce **un controller alla volta**. Con un controller collegato il gateway
  non cerca e non accetta altri controller: per usarne un altro scollega prima
  quello in uso.
- Per ricollegare un controller già abbinato basta accenderlo (tasto Xbox o PS),
  anche con l'abbinamento spento. Se sono accesi tutti e due, vince il primo che
  si collega.
- "Scollega" dalla pagina web blocca per 30 secondi il ritorno del controller Xbox,
  che altrimenti si ricollegherebbe subito. Per il DualSense il blocco è di 5
  secondi: torna solo quando premi di nuovo PS.
- Dopo "Forget All Controllers" un DualSense che premi PS viene rifiutato: va
  rimesso in abbinamento con Create + PS.

## Ripristinare il firmware originale

Con **SONOFF Dongle Flasher** (web, Home Assistant o Docker), via USB. Lo strumento
è citato nella guida ufficiale del Dongle-M:
<https://dongle.sonoff.tech/guide/dongle-m/running-esphome-firmware-on-dongle-m/>

In alternativa, se hai fatto il backup:

```powershell
esptool.py --chip auto --port <PORTA> write_flash 0 backup_originale.bin
```

## Struttura

```
firmware/
  CMakeLists.txt         progetto ESP-IDF; aggancia Bluepad32 e BTstack da third_party/
  sdkconfig.defaults     default di progetto (flash 16 MB, Bluetooth, Bluepad32 custom)
  partitions.csv         tabella delle partizioni
  components/
    gw_bt_config/        opzioni Bluetooth in menuconfig
  main/
    main.c               avvio dei moduli
    gw_platform.c        Bluetooth: "custom platform" di Bluepad32
    net_link.c           UDP verso il PC (INPUT v2, RUMBLE/FEEDBACK, keepalive)
    eth.c                Ethernet, DHCP o IP statico, mDNS
    mg24_bridge.c        ponte seriale MG24 ↔ TCP
    settings.c           impostazioni in NVS
    led.c                LED RGB
    web_ui.c, www/       pagina web di gestione
    protocol.h           pacchetti UDP v1 e v2
```
