# Änderungen / Changelog

C64uRemote für den **M5Stack CoreS3**. Neueste Version zuerst.

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
