# Ricevitore PC

Riceve via UDP lo stato del controller dal dongle e lo passa a
[VIIPER](https://github.com/Alia5/VIIPER), che fa comparire in Windows un
controller virtuale:

- controller **Xbox** → controller **Xbox 360** virtuale;
- **DualSense** (PS5) → **DualSense** virtuale, con touchpad (anche il clic),
  giroscopio e accelerometro, tasto microfono e batteria. Barra luminosa e LED
  giocatore impostati dal gioco tornano al controller vero, come la vibrazione.

```
dongle --UDP 47900 (INPUT v2)--> receiver.py --TCP 3242 (localhost)--> VIIPER --> controller virtuale
       <--FEEDBACK v2-----------             <--vibrazione, luci-----
```

Il protocollo è la versione 2 (`../docs/protocol.md`): serve il firmware del
2026-11-14 o successivo. Il ricevitore accetta anche il firmware precedente (v1).

**Limiti** (di VIIPER 0.9.0, non del dongle): i grilletti adattivi, l'aptica audio,
altoparlante e microfono del DualSense non arrivano al PC; la batteria viene letta
solo quando il controller virtuale viene creato.

Serve Python 3.11 o successivo; a runtime si usa solo la libreria standard.

## 1. Preparare il PC (una volta)

Copiare la cartella `pc-receiver` sul PC (la sottocartella `.venv` non serve) e fare
**doppio clic su `installa.bat`**. Installa o imposta quello che manca e salta quello
che c'è già:

| Cosa | Dettagli |
|------|----------|
| Python 3.12 | solo se manca Python 3.11 o successivo; per l'utente corrente |
| [usbip-win2](https://github.com/vadimgrn/usbip-win2) 0.9.8.1 | driver dei dispositivi USB virtuali |
| Firewall | regola che lascia arrivare a Python i pacchetti UDP del dongle (porta di `config.toml`), solo dalla rete locale; toglie le eventuali regole che bloccano Python |
| VIIPER 0.9.0 | con il suo installer: va in `%LOCALAPPDATA%\VIIPER`, con avvio automatico |
| PATH | aggiunge `C:\Program Files\USBip` al PATH dell'utente |
| Avvio automatico | alla fine chiede se far partire il ricevitore da solo all'accesso a Windows (vedi [Avvio automatico](#avvio-automatico)) |

Windows chiede una volta il permesso di amministratore (per driver e firewall). Le
versioni sono quelle provate; i file scaricati vengono controllati (SHA-256 per quelli
di GitHub, firma digitale per Python). Se alla fine chiede di riavviare, riavviare.

Si può rilanciare quando si vuole: `installa.bat -CheckOnly` controlla soltanto, senza
cambiare nulla. Senza domanda sull'avvio automatico: `installa.bat -AvvioAutomatico` lo
attiva, `installa.bat -NoAvvioAutomatico` lo lascia com'è. Va rilanciato se si cambia la porta in `config.toml` o la versione di
Python, perché la regola del firewall dipende da entrambe.

A mano, senza `installa.bat`: scaricare `viiper-windows-amd64.exe` da
<https://github.com/Alia5/VIIPER/releases>, eseguire `.\viiper-windows-amd64.exe install`
accettando **Install USBIP**, riavviare, e consentire Python quando Windows Firewall lo
chiede.

Lasciare le impostazioni predefinite di VIIPER: collegamento automatico del
dispositivo al PC locale (`--api.auto-attach-local-client`) attivo e autenticazione
non richiesta da localhost (`--api.require-localhost-auth=false`).

## 2. Configurare

`config.toml`, nella stessa cartella, funziona così com'è se VIIPER è sullo stesso PC.
Le voci che può servire cambiare:

| Voce | Significato |
|------|-------------|
| `[udp] port` | porta UDP, uguale a quella impostata nel dongle (default 47900) |
| `[udp] allowed_sources` | se non è vuota, accetta pacchetti solo dall'IP del dongle |
| `[mapping] invert_y` | mettere `false` se l'asse verticale degli stick è rovesciato (solo pad Xbox 360) |
| `[mapping] dualsense` | `"dualsense"` (default) o `"xbox360"`: come compare un DualSense; la seconda per i giochi che non lo riconoscono |
| `[timeouts]` | rilascio dei comandi (1 s) e rimozione del controller virtuale (5 s) |

La regola del firewall la crea `installa.bat`. Se Windows Firewall chiede comunque se
consentire Python sulla rete, consentire: con «Annulla» Windows crea regole che
bloccano Python e i pacchetti del dongle non arrivano più (`installa.bat` le toglie).

## 3. Avviare

| File | Cosa fa |
|------|---------|
| `avvia.bat` | avvia VIIPER e il ricevitore **in background, senza finestra**; la sua finestra si chiude da sola appena il ricevitore è in ascolto, o resta aperta con l'errore |
| `ferma.bat` | ferma ricevitore, controller virtuale e VIIPER avviati in background |
| `avvia-con-finestra.bat` | come prima: tutto con i messaggi in una finestra, che va lasciata aperta (Ctrl+C o chiusura fermano tutto); per le prove e i problemi |

In tutti e due i modi l'avvio:

1. trova `usbip.exe` di usbip-win2 (in `C:\Program Files\USBip`: l'installer del
   driver **non** lo aggiunge al PATH, lo fa `installa.bat`);
2. chiude un eventuale VIIPER già aperto (prima di `installa.bat` quello dell'avvio
   automatico non vedeva `usbip.exe`) e lo riavvia con la cartella di usbip-win2 nel
   PATH; il log di VIIPER finisce in `viiper.log`;
3. avvia il ricevitore.

In background i messaggi vanno in `avvio-automatico.log`; se il ricevitore si ferma
per un errore riparte da solo (vedi sotto). Ne gira uno solo per volta: se è già in
esecuzione, `avvia.bat` lo dice e non ne avvia un altro.
Se `usbip.exe` o `viiper.exe` stanno in cartelle diverse dal solito, indicarle in
`config.toml`, sezione `[launcher]`.

Il controller virtuale compare quando il dongle segnala un controller collegato e
sparisce 5 secondi dopo che il controller si è scollegato.

Senza l'avvio unico (VIIPER già avviato a mano con usbip nel PATH): `python receiver.py`.

### Avvio automatico

Fa partire VIIPER e il ricevitore da soli, in background come `avvia.bat`, 30 secondi
dopo l'accesso a Windows. Non serve l'amministratore. `installa.bat` chiede se
attivarlo; dopo si usano questi file, con doppio clic:

| File | Cosa fa |
|------|---------|
| `avvio-automatico-attiva.bat` | lo attiva (parte dal prossimo accesso; subito: `avvia.bat`) |
| `avvio-automatico-disattiva.bat` | ferma il ricevitore in background e toglie l'avvio automatico |
| `avvio-automatico-stato.bat` | dice se è attivo, se sta girando e dov'è il log |

`ferma.bat` ferma anche quello partito all'accesso, che riparte al prossimo accesso.

- È un'attività dell'Utilità di pianificazione dell'utente corrente, chiamata
  `xbox-eth-gateway ricevitore`. Se Windows non la lascia creare, al suo posto c'è un
  collegamento nella cartella Esecuzione automatica.
- I messaggi vanno in `avvio-automatico.log`, quelli di VIIPER in `viiper.log`.
- Se il ricevitore si ferma per un errore riparte da solo, dopo 5 s e poi con attese
  sempre più lunghe fino a 60 s. Se VIIPER si chiude viene riavviato.
- Per vedere i messaggi in una finestra: prima `ferma.bat`, poi `avvia-con-finestra.bat`.
- Se si sposta la cartella `pc-receiver`, rilanciare `avvio-automatico-attiva.bat` dalla
  nuova posizione.

Le stesse azioni da PowerShell:

```
powershell -ExecutionPolicy Bypass -File avvio-automatico.ps1 -Attiva
```

Al posto di `-Attiva`: `-Disattiva`, `-Stato`, `-Avvia` (`avvia.bat`), `-Ferma` (`ferma.bat`).

## 4. Provare con `joy.cpl`

1. Avviare `avvia.bat` e accendere il controller abbinato al dongle.
2. Premere Win+R, scrivere `joy.cpl`, Invio: in elenco deve esserci un controller
   Xbox 360. Selezionarlo e aprire **Proprietà**.
3. Muovere stick, grilletti e tasti del controller vero e controllare che la
   finestra li segua. Lo stick in alto deve portare la croce in alto: se va in
   basso, mettere `invert_y = false` in `config.toml` e riavviare il ricevitore.

`joy.cpl` non prova la vibrazione: per quella serve un gioco o un gamepad tester.

Con un **DualSense** in `joy.cpl` compare un controller PlayStation (il DualSense
virtuale) al posto del pad Xbox 360. Per provare touchpad, sensori, barra luminosa e
LED serve un programma che riconosca il DualSense, per esempio le impostazioni dei
controller di Steam (che mostrano giroscopio e touchpad e cambiano il colore della
barra). Se un gioco non riconosce il DualSense, mettere `dualsense = "xbox360"` in
`config.toml`.

## Problemi frequenti

**Il controller virtuale non compare, o non arrivano pacchetti dal dongle**

`installa.bat -CheckOnly` dice cosa manca (Python, driver, VIIPER, firewall);
`installa.bat` lo sistema.

**"il ricevitore è già in esecuzione"**

Gira già quello in background (`avvia.bat` o avvio automatico): `ferma.bat` lo ferma.
Oppure è aperta una finestra di `avvia-con-finestra.bat`: chiuderla.
`avvio-automatico-stato.bat` dice cosa gira.

**In background non funziona niente**

Leggere `avvio-automatico.log`, oppure `ferma.bat` e poi `avvia-con-finestra.bat`: i
messaggi sono gli stessi.

**`409 Failed to auto-attach device: exec: "usbip": executable file not found in %PATH%`**

VIIPER non trova `usbip.exe`. Se usbip-win2 è installato basta avviare con
`avvia.bat`, che gli passa il PATH giusto. Se non è installato (succede lanciando
l'`.exe` scaricato con `server`, senza `install`):

1. Chiudere `viiper server`.
2. Lanciare `installa.bat`.
3. Riavviare il PC se lo chiede.
4. Avviare `avvia.bat`.

Il ricevitore intanto riprova da solo ogni 3 secondi e toglie da VIIPER i dispositivi
rimasti a metà.

## Strumenti di prova

| Comando | A cosa serve |
|---------|--------------|
| `python dump.py` | stampa i pacchetti INPUT che arrivano (senza VIIPER); non insieme a `receiver.py` |
| `python simulate.py` | finto dongle: 10 secondi di stick che ruotano e tasti a turno (`--dualsense`: in più touchpad e sensori; `--v1`: vecchio formato) |
| `python fake_viiper.py` | finto server VIIPER (Xbox 360 e DualSense): stampa i report ricevuti, **non** crea controller in Windows |

Senza hardware e senza driver, in tre terminali:

```
python fake_viiper.py --rumble 2
python receiver.py
python simulate.py
```

Con VIIPER vero al posto di `fake_viiper.py`, `simulate.py` muove il controller
virtuale in `joy.cpl` senza bisogno del dongle.

## Test

```
py -3.11 -m venv .venv
.venv\Scripts\python -m pip install pytest
.venv\Scripts\python -m pytest
```

## File

| File | Contenuto |
|------|-----------|
| `installa.bat`, `installa.ps1` | preparazione del PC, una volta sola |
| `avvia.bat`, `ferma.bat` | avvio in background (senza finestra) e arresto |
| `avvia-con-finestra.bat`, `launcher.py` | avvio unico di VIIPER e ricevitore (`launcher.py --auto`: senza finestra) |
| `avvio-automatico.ps1`, `avvio-automatico-*.bat` | avvio in background, subito o all'accesso a Windows: avvia, ferma, attiva, disattiva, stato |
| `avvio-automatico.log`, `viiper.log` | log dell'avvio automatico e di VIIPER (creati quando servono) |
| `receiver.py` | ciclo principale: UDP, filtro `seq`, timeout, vibrazione |
| `protocol.py` | pacchetti INPUT e RUMBLE/FEEDBACK, v1 e v2 (specifica in `../docs/protocol.md`) |
| `mapping.py` | da stato Bluepad32 a report Xbox 360 o DualSense; ritorno di vibrazione e luci |
| `viiper_client.py` | client TCP di VIIPER |
| `config.py`, `config.toml` | configurazione |
| `tests/` | test `pytest` |
