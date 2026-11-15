# xbox-eth-gateway

Firmware custom per **SONOFF Dongle Max (Dongle-M)** che rifà le funzioni principali
dell'originale e aggiunge un gateway Bluetooth per un controller Xbox:

```
Controller Xbox --BLE--> Dongle (ESP32) --UDP su Ethernet--> PC Windows --> pad Xbox 360 virtuale (VIIPER)
                 <--vibrazione--                <--UDP--
Zigbee2MQTT / ZHA <--TCP 6638--> Dongle (ESP32) <--seriale--> chip Zigbee MG24
```

Stato: **fase 1 scritta e compilata.**

## Contenuto

| Cartella | Cosa c'è | Istruzioni |
|----------|----------|------------|
| `firmware/` | firmware ESP-IDF (Bluepad32, Ethernet, ponte Zigbee, pagina web) | [firmware/README.md](firmware/README.md) |
| `pc-receiver/` | ricevitore Python che alimenta VIIPER, con test | [pc-receiver/README.md](pc-receiver/README.md) |
| `docs/protocol.md` | protocollo UDP tra dongle e PC | |
| `third_party/bluepad32/` | Bluepad32 5.0.0-beta0 con BTstack (submodule: `git submodule update --init --recursive`) | |

## Dal sorgente al dongle funzionante

1. **Compila** il firmware: `idf.py set-target esp32` e `idf.py build` da `firmware/`
   (serve ESP-IDF v5.5.x).
2. **Controlla la flash** prima di scrivere: `flash_id` e `get_security_info`. Se flash
   encryption o secure boot sono attivi, fermati. Il backup con `read_flash` è
   facoltativo. Comandi in [firmware/README.md](firmware/README.md).
3. **Flash** via USB con `idf.py -p <PORTA> flash monitor`. Cancella il firmware
   originale dell'ESP32; il chip Zigbee non viene toccato.
4. **Pagina web**: apri `http://dongle-m.local` (o l'IP del dongle) e crea la password.
   Da lì si gestiscono rete, controller, Zigbee, PC e aggiornamenti.
5. **Zigbee2MQTT / ZHA**: il ponte resta sulla porta **6638** e l'hostname di default
   è `dongle-m`, come nel firmware originale: se usavi `dongle-m.local` non devi
   cambiare nulla. L'indirizzo IP invece può cambiare dopo il primo flash: se usavi l'IP, impostalo statico dalla pagina web.
6. **Controller**: accendilo, tieni premuto Pair per 3 secondi; al primo avvio il dongle
   è già in abbinamento (LED verde lampeggiante). In seguito l'abbinamento si avvia dalla
   pagina web o con una pressione breve del pulsante.
7. **PC**: installa VIIPER, avvia `python receiver.py` da `pc-receiver/` e prova con
   `joy.cpl`. Nella pagina web, sezione PC, puoi indicare l'IP del PC (vuoto = broadcast).

## LED

| LED | Significato |
|-----|-------------|
| Blu lampeggiante | rete assente |
| Blu fisso | rete presente |
| Verde lampeggiante | abbinamento Bluetooth attivo |
| Verde fisso | controller collegato |
| Giallo lampeggiante | "Find Device" dalla pagina web |

## Configurazione dei pin

I pin del Dongle Max sono i default di `idf.py menuconfig`, menu "Xbox Ethernet
Gateway": non serve cambiarli. Elenco completo in `firmware/main/Kconfig.projbuild`.

## Tornare al firmware originale

Con **SONOFF Dongle Flasher** via USB, oppure con
`esptool.py --chip auto --port <PORTA> write_flash 0 backup_originale.bin` se hai
fatto il backup.

## Limiti noti

- Un solo controller alla volta.
- Wi-Fi spento: la radio dell'ESP32 è riservata al Bluetooth.
- La pagina web è in HTTP sulla rete locale: usala solo su una rete di cui ti fidi.
- Non incluse: eWeLink-Remote Gateway, Channel Energy Scan, aggiornamenti automatici.
  MQTT, Webhooks, WireGuard e altre funzioni sono previste nelle fasi successive.

## Licenza

[PolyForm Noncommercial 1.0.0](LICENSE.md): uso, modifica e ridistribuzione consentiti solo per scopi non commerciali.

Bluepad32 (Apache 2.0) e BTstack (gratuito solo per uso non commerciale) restano sotto le rispettive licenze.
