# Änderungen / Changelog

C64uRemote für den **M5Stack CoreS3**. Neueste Version zuerst.

## v1.4.0 – 2026-09-27

### Deutsch

**Neu: Direktmodus – ohne Router, z. B. auf Treffen.** Der CoreS3 spannt auf Wunsch
selbst ein WLAN auf, und der c64u meldet sich direkt bei ihm an.

- Neue Einträge im WLAN-Menü: **Direktmodus** (an/aus) und **Direkt-Netz**
  (`192.168.4.x` oder `192.168.2.x`).
- Netz `C64uRemote-Direct`, Passwort `c64ultimate`. Der CoreS3 hat die `.1`, der
  c64u bekommt per DHCP die `.64`. Eine eigene Seite zeigt, was am c64u
  einzutragen ist, und ob er schon verbunden ist.
- Meldet sich zuerst ein anderes Gerät an, sucht der CoreS3 unter den angemeldeten
  Geräten weiter, bis der c64u antwortet.
- Ein zweiter Fernbediener kann sich als normaler Client ins Direktnetz
  einbuchen und spricht den c64u dann automatisch unter der `.64` an; seine
  Heimadresse bleibt erhalten.
- Wer ein gespeichertes Netz wählt oder eine WLAN-Karte auflegt, beendet den
  Direktmodus.
- Befehlskarten `CMD:DIRECT=192.168.4`, `CMD:DIRECT=192.168.2` und
  `CMD:DIRECT=OFF`, anzulegen unter *NFC-Cmd*.
- Die `wifi.txt` kennt dafür die Zeilen `direct`, `direct_ssid`, `direct_pass`
  und `direct_net`; *Auf SD sichern* schreibt sie mit heraus.

**Neu: CoreS3 per Karte ausschalten.** Eine Befehlskarte `CMD:M5OFF` schaltet den
CoreS3 selbst aus (am Akku ganz, an USB in den Tiefschlaf) – dieselbe Karte wie beim
M5Dial. Anzulegen unter *NFC-Cmd*. In den ersten acht Sekunden nach dem Start
wird sie ignoriert.

**Verbindung zum c64u zuverlässiger.**

- Anfragen an den c64u (Abfragen, Befehle, Uploads) laufen über einen eigenen,
  schlanken HTTP-Weg: Verbindungsaufbau ohne blockierendes Warten, die Antwort
  wird vollständig gelesen; bis zu drei Verbindungsversuche mit je 1,5 s.
- Netzname und Signalstärke werden höchstens einmal pro Sekunde beim
  WLAN-Treiber abgefragt statt tausendfach.
- Das Funkfeld des NFC-Lesers ist nur noch für die kurze Kartenprobe und
  während der Kartenbearbeitung an. Beim M5Dial hatte das dauerhaft
  eingeschaltete Feld den WLAN-Empfang gestört; hier spart es vor allem Strom.

**Versionsanzeige.** Die Statusseite zeigt jetzt die Firmware-Version im Titel
(`STATUS v1.4.0`), das Startprotokoll auf der seriellen Schnittstelle ebenfalls.

Die Nummer springt von 1.2.1 direkt auf 1.4.0: 1.3.x gab es nur für den M5Dial,
ab jetzt tragen wieder alle vier Geräte denselben Stand.

### English

**New: direct mode – no router, e.g. at meetings.** On request the CoreS3 opens a
WiFi network of its own and the c64u connects to it directly.

- New entries in the WiFi menu: **Direct mode** (on/off) and **Direct net**
  (`192.168.4.x` or `192.168.2.x`).
- Network `C64uRemote-Direct`, password `c64ultimate`. The CoreS3 has `.1`, the
  c64u gets `.64` via DHCP. A page of its own shows what to enter on the c64u
  and whether it is connected yet.
- If another device joins first, the CoreS3 keeps looking among the connected
  devices until the c64u answers.
- A second remote can join the direct network as a normal client and then
  addresses the c64u at `.64` automatically; its home address is kept.
- Choosing a stored network or presenting a WiFi card ends direct mode.
- Command cards `CMD:DIRECT=192.168.4`, `CMD:DIRECT=192.168.2` and
  `CMD:DIRECT=OFF`, created under *NFC-Cmd*.
- `wifi.txt` has the lines `direct`, `direct_ssid`, `direct_pass` and
  `direct_net` for this; *Save to SD* writes them out as well.

**New: switch the CoreS3 off by card.** A command card `CMD:M5OFF` switches the
CoreS3 itself off (completely on battery, deep sleep on USB) – the same card as on
the M5Dial. Created under *NFC-Cmd*. It is ignored during the first eight
seconds after start.

**Connection to the c64u more reliable.**

- Requests to the c64u (queries, commands, uploads) go through an own, lean
  HTTP path: connecting without blocking waits, the reply is read completely;
  up to three connection attempts of 1.5 s each.
- Network name and signal strength are queried from the WiFi driver at most
  once per second instead of thousands of times.
- The NFC reader's RF field is now only on for the short card probe and while
  a card is being processed. On the M5Dial the permanently switched-on field
  disturbed WiFi reception; here it mainly saves power.

**Version display.** The status page now shows the firmware version in its title
(`STATUS v1.4.0`), and so does the boot log on the serial port.

The number jumps from 1.2.1 straight to 1.4.0: 1.3.x only existed for the M5Dial;
from now on all four devices carry the same version again.

## v1.2.1 – 2026-09-04

### Deutsch

**Stabilere Verbindung zum c64u.** Der HTTP-Server der Ultimate-Firmware weist
gelegentlich eine Verbindung ab („connection refused"), auch wenn Netz und
Adresse in Ordnung sind. Das führte bisher sofort zu *FAILED* und zu *Not
reached* in der Statuszeile.

- Ein abgewiesener Aufruf wird nach kurzer Pause **einmal automatisch
  wiederholt**. Nur bei Transportfehlern – dann ist beim c64u nichts
  angekommen, ein Befehl kann sich also nicht doppeln.
- Der zyklische Verbindungstest kostet nur noch **eine statt zwei Anfragen**,
  wenn kein Passwort hinterlegt ist. Die zweite war byte-gleich mit der ersten.
- Beim **Wiederverbinden** wird zuerst wieder das Netz versucht, mit dem es
  zuletzt geklappt hat. Sind zwei Netze gespeichert und nur eines ist
  erreichbar, wurde vorher nach jedem Aussetzer jedes zweite Mal zehn Sekunden
  am toten Netz gewartet.
- Ein Tastendruck wird **sofort** mit einem kurzen Ton bestätigt; die
  Rückmeldung über Erfolg oder Fehler kommt danach. Vorher piepte das Gerät
  erst nach dem Netzwerkaufruf – bei zähem Netz wirkte es dadurch, als sei die
  Taste nicht angekommen.

### English

**More robust connection to the c64u.** The HTTP server of the Ultimate firmware
occasionally refuses a connection ("connection refused") even though network and
address are fine. Until now that immediately produced *FAILED* and *Not reached*
in the status line.

- A refused call is **retried once automatically** after a short pause. Only on
  transport errors - nothing reached the c64u then, so a command cannot be
  doubled.
- The periodic connection test now costs **one request instead of two** when no
  password is stored. The second one was byte-identical to the first.
- When **reconnecting**, the network that last worked is tried first. With two
  networks stored of which only one is reachable, every other reconnect used to
  waste ten seconds on the dead one.
- A key press is acknowledged **immediately** with a short tone; the result,
  success or failure, follows afterwards. Previously the device only beeped
  after the network call - on a sluggish network that made it look as if the
  press had been lost.

## v1.2.0 – 2026-09-04

### Deutsch

**Neu: Akkuanzeige.** Der Ladestand ist jetzt auf einen Blick zu sehen, ohne
dass die volle Statusleiste enger wird.

- Der **Strich unter der Statusleiste** ist zugleich der Füllstandsbalken: der
  gefüllte Teil ist etwas dicker und farbig, der Rest bleibt die gedämpfte Linie.
- Rechts außen wechseln sich **`RFID`/`SD` und ein Akkusymbol mit der
  Prozentzahl** alle 15 Sekunden ab.
- Hängt das Gerät am Strom, steht ein **Ladeblitz** vor dem Symbol.
- Farben: ab 50 % grün, ab 20 % gelb, ab 10 % rot, darunter rot und blinkend.
  Am Ladekabel ist alles türkis und blinkt nie.
- Auf der **Status-Seite** steht der Ladestand zusätzlich als Text.

Der Ladestand wird höchstens alle fünf Sekunden vom Lade-IC gelesen; auf die
Bildwiederholrate wirkt sich die Anzeige nicht aus.

Firmware für M5Dial und M5StickC Plus2 ist seit v1.1.0 unverändert – die haben
keinen passenden Akku beziehungsweise keine Statusleiste.

### English

**New: battery indicator.** The charge level is now visible at a glance without
crowding the already full status bar.

- The **line below the status bar** doubles as the level gauge: the filled part
  is slightly thicker and coloured, the rest stays the dimmed line.
- At the right-hand end **`RFID`/`SD` and a battery symbol with the percentage**
  take turns every 15 seconds.
- While the device is on external power a **charge bolt** sits in front of the
  symbol.
- Colours: green from 50 %, yellow from 20 %, red from 10 %, below that red and
  blinking. On the charger everything is turquoise and never blinks.
- The **status page** shows the level as text as well.

The level is read from the charge controller at most every five seconds; the
indicator has no effect on the frame rate.

The firmware for the M5Dial and the M5StickC Plus2 is unchanged since v1.1.0 –
those have no suitable battery, respectively no status bar.

## v1.1.0 – 2026-09-03

### Deutsch

**Neu: Joystickports am C64 tauschen.** Manche Spiele wollen den Joystick in
Port 1, andere in Port 2 – jetzt lässt sich das umschalten, ohne das Kabel
umzustecken.

- Neue Kachel **JOY** auf dem Hauptbildschirm, direkt hinter *CPU*. Die untere Kachelreihe ist damit genauso voll wie die obere. Jedes Auslösen schaltet zwischen *Normal* und *Swapped* um.
- Neue Einstellung **Joystick** (hinter *Disk Drive*): zeigt den aktuellen Stand und
  schaltet durch **alle** Werte, die der c64u meldet – je nach Firmware auch
  *WASD Port 1* und *WASD Port 2*.
- Neue Befehlskarten: `CMD:JOY` schaltet um, `CMD:JOY=NORMAL`, `=SWAPPED`,
  `=WASD1`, `=WASD2` setzen fest. Beim Anlegen einer Karte stehen die
  Joystick-Werte zwischen den festen Befehlen und den CPU-Stufen; das Kürzel
  rechts (*JOY* / *CPU*) trennt die beiden Blöcke.
- Handbücher und README auf den neuen Stand gebracht.

**Hintergrund:** Die ReST-API der Ultimate-Firmware hat dafür keinen eigenen
`machine:`-Befehl. Die Belegung ist ein Konfigurationseintrag – im Test
*Joystick Swapper* in der Kategorie *U64 Specific Settings*. Gesetzt wird sie
über `PUT /v1/configs/<Kategorie>/<Eintrag>?value=…`, genau wie die CPU-Stufe.
Kategorie und Eintragsname sucht die Firmware zur Laufzeit (Schlüsselwort
„Joystick"), damit eine Umbenennung in einer künftigen Ultimate-Version nichts
kaputt macht.

### English

**New: swap the joystick ports on the C64.** Some games want the joystick in
port 1, others in port 2 – this can now be toggled without moving the cable.

- New **JOY** tile on the home screen, right after *CPU*. The lower tile row is now as full as the upper one. Every trigger toggles between *Normal* and *Swapped*.
- New setting **Joystick** (after *Disk Drive*): shows the current state and steps
  through **every** value the c64u reports – depending on the firmware also
  *WASD Port 1* and *WASD Port 2*.
- New command cards: `CMD:JOY` toggles, `CMD:JOY=NORMAL`, `=SWAPPED`, `=WASD1`,
  `=WASD2` set a fixed value. When writing a card the joystick values sit
  between the fixed commands and the CPU steps; the tag on the right
  (*JOY* / *CPU*) tells the two blocks apart.
- Manuals and README brought up to date.

**Background:** the ReST API of the Ultimate firmware has no dedicated
`machine:` command for this. The mapping is a configuration item – in testing
*Joystick Swapper* in the category *U64 Specific Settings*. It is set through
`PUT /v1/configs/<category>/<item>?value=…`, exactly like the CPU speed. The
firmware looks the category and item name up at runtime (keyword "Joystick") so
that a renaming in a future Ultimate version does not break anything.

## v1.0.0 – 2026-08-24

Erste Veröffentlichung als eigenes Repository: Firmware in deutscher und
englischer Fassung, Handbücher als PDF, MIT-Lizenz.

First release as its own repository: firmware in a German and an English
edition, manuals as PDF, MIT licence.
