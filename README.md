# C64uRemote für M5Stack CoreS3 (CoreS3 / SE / Lite)

Fernbedienung für den **Commodore 64 Ultimate (c64u)** bzw. **Ultimate64 Elite-II**
über die ReST-API der Ultimate-Firmware (ab 3.11), mit **Unit RFID2 (WS1850S)**
und der eingebauten **microSD**.

Basiert auf dem Original von Karl Prosser (@klumsy) und den Versionen für
M5StickC Plus2 / M5Dial / M5Stack Core von Martin Oswald (@mad, https://1MHz.de).

> **Der CoreS3 hat keinen eingebauten NFC-Leser.** Für die Kartenfunktionen wird
> das **Unit RFID2** an Port A gebraucht. Und er hat **keine Tasten A/B/C mehr** –
> bedient wird komplett über den Touchscreen.

---

## 1. Programmierumgebung

**Empfehlung: PlatformIO in Visual Studio Code.** Grund: die Konfiguration
(Board, Bibliotheksversionen, Partitionstabelle) liegt versioniert in
`platformio.ini`, ein Build ist damit reproduzierbar. Genau das braucht dieses
Projekt, weil ArduinoJson bewusst auf Version 6 festgenagelt ist (Version 7
kennt `DynamicJsonDocument` nicht mehr) und weil ein größeres App-Image nötig ist.

### Einrichtung

1. [Visual Studio Code](https://code.visualstudio.com) installieren
2. Erweiterung **PlatformIO IDE** installieren (Extensions → „platformio")
3. Diesen Ordner `M5CoreS3_C64uRemote` in VS Code öffnen
4. Optional `src/build_env.h` mit WLAN-Daten und c64u-Adresse füllen
   (Vorlage: `src/build_env.h.example`). Das ist **nicht mehr zwingend** – das
   WLAN lässt sich seit dieser Fassung komplett am Gerät einrichten, siehe
   Abschnitt *WLAN einrichten*. `build_env.h` liefert nur noch die Startwerte
   für den allerersten Start.
5. CoreS3 per USB-C anschließen, dann in der PlatformIO-Statusleiste unten
   auf **→ (Upload)** klicken

Kommt kein Port zustande: **RESET 3 s halten** (grüne LED an, Download-Modus)
und den Upload erneut starten.

Auf der Kommandozeile:

```bash
pio run                 # kompilieren
pio run -t upload       # flashen
pio device monitor      # serielle Ausgabe (115200 Baud)
```

### Was `platformio.ini` festlegt

| Einstellung | Wert | Warum |
|---|---|---|
| `board` | `m5stack-cores3` | CoreS3 / SE / Lite, ESP32-S3, 320x240 |
| `board_build.partitions` | `huge_app.csv` | WiFi + SD + RFID passen nicht in die Standardtabelle |
| `-DBOARD_HAS_PSRAM` | | der Offscreen-Puffer liegt im PSRAM |
| `-DARDUINO_USB_CDC_ON_BOOT=1` | | der CoreS3 meldet sich als natives USB-CDC |
| `ArduinoJson` | `^6.21.5` | Version 7 wäre API-inkompatibel |
| `MFRC522_I2C` | Git-Repo kkloesener | I2C-Treiber für RFID2/WS1850S |

Falls PlatformIO die MFRC522-Bibliothek nicht per Git ziehen kann (Firewall):
Repo herunterladen und in einen Unterordner `lib/MFRC522_I2C/` legen – PlatformIO
findet sie dort automatisch.

**Alternative Arduino IDE 2.x:** funktioniert auch, dann `main.cpp` in
`C64uRemote.ino` umbenennen, M5Unified + M5GFX + ArduinoJson 6.x + MFRC522_I2C
manuell über den Bibliotheksverwalter installieren und unter *Werkzeuge →
Partition Scheme* **Huge APP** wählen. Ohne diesen Schritt bricht das Hochladen
mit „text section exceeds available space" ab.

---

## 2. Verkabelung

| Gerät | Anschluss |
|---|---|
| Unit RFID2 (WS1850S) | **Port A** (rot, Grove) → I2C, **SDA G2 / SCL G1**, Adresse 0x28 |
| microSD | SD-Slot des CoreS3 (FAT32, bis 16 GB), SCK G36 / MISO G35 / MOSI G37 / CS G4 |
| Status-LED (3× SK6812) | **Port B** (schwarz, Grove) → Daten auf der **gelben Ader G9**, 5 V und GND aus dem Port |

**Stolperstelle:** Port A liegt beim CoreS3 auf G2/G1 – gegenüber dem Core Basic
(G21/G22) also nicht nur auf anderen Pins, sondern SDA und SCL sind zusätzlich
vertauscht. Wer ein RFID2 vom Core Basic übernimmt, muss also nichts umlöten,
aber im Code steht die Belegung entsprechend anders.

Die microSD teilt sich den SPI-Bus mit dem Display, deshalb ruft `setup()`
`SPI.begin()` ausdrücklich mit diesen drei Pins auf.

Alles drei ist optional: fehlt der Reader, die Karte oder die LED, laufen die
übrigen Funktionen normal weiter. Die Statusleiste zeigt oben rechts `RFID` und
`SD` grün, wenn erkannt.

**Zur LED:** Eine SK6812 (WS2812-kompatibel) ist eine reine Ausgangsleitung – ob
tatsächlich eine LED dranhängt, lässt sich **nicht** feststellen. Steckt nichts,
gehen die Daten ins Leere, und das ist folgenlos. Wer den Pin anders braucht,
schaltet die Lampe im Setup unter *Status-LED* ab. Der ESP32-S3 gibt 3,3 V
Logikpegel aus; die M5Stack-Units mit SK6812 sind dafür ausgelegt und laufen
direkt an Port B.

---

## 3. Bedienung

Der CoreS3 hat **keine Tasten A/B/C** mehr – nur POWER und RESET an der Seite,
und die gehören zum CoreS3 selbst, nicht zum C64. Bedient wird ausschliesslich
über den kapazitiven Touchscreen.

| Geste | Wirkung |
|---|---|
| **Antippen** | Kachel oder Listenzeile auswählen und ausführen (ein Tipper genügt) |
| **Wischen** | die Liste folgt dem Finger 1:1 |
| **Scrollleiste rechts ziehen** | schnell durch lange Listen |
| **Zurück** (unten links) | eine Ebene zurück |
| **▲ / ▼** (unten rechts) | eine Zeile hoch bzw. runter |
| **Lang antippen** (ab 0,6 s) | im SD-Browser: ein Verzeichnis zurück |

Die Fussleiste ist keine Tastenbeschriftung mehr, sondern selbst Bedienfläche
und mit 36 px entsprechend hoch:

```
[ <- Zurueck ]        Hinweistext         [ ^ ][ v ]
```

Das Zurück-Feld liegt immer links, die Blätterfelder erscheinen nur auf
Listenseiten, dazwischen steht ein kurzer Hinweis.

**Lange Listen.** Passen nicht alle Einträge auf den Bildschirm, erscheint rechts
eine **Scrollleiste**. Sie zeigt Position und Länge der Liste und lässt sich
direkt ziehen – bei vielen Einträgen kommt man damit in einer Bewegung ans andere
Ende. Der Griff ist nie kleiner als 28 px, sonst wäre er nicht mehr zu treffen.

Beim Wischen mitten in der Liste folgt diese dem Finger. Sobald sie sich um eine
Zeile bewegt hat, gilt die Bewegung als Schieben – ein Wisch löst deshalb nie
versehentlich einen Eintrag aus, auch ein langsamer nicht. Ein Tipper zählt nur,
wenn der Finger dabei höchstens 10 px wandert.

### Sonderfall SD-Browser

| Geste | Funktion im SD-Browser |
|---|---|
| **Antippen** | Datei starten bzw. Ordner öffnen |
| **Wischen** | Liste folgt dem Finger, Scrollleiste rechts für lange Listen |
| **Lang antippen** | ein Verzeichnis zurück |
| **Zurück** | ein Verzeichnis zurück, auf oberster Ebene zum Hauptbildschirm |
| `..` in der Liste | ebenfalls eine Ebene nach oben |

Die Titelzeile blendet `lang tippen = ..` als Erinnerung ein.

### Hauptbildschirm

Statusleiste, animiertes Logo und darunter **alle Kommandos als Kacheln**:

```
RESET   REBOOT   MENU    POWER   CPU
RFID    SD       STATUS  SETUP
```

| Kachel | Wirkung |
|---|---|
| `RESET` | `PUT /v1/machine:reset` |
| `REBOOT` | `PUT /v1/machine:reboot` |
| `MENU` | Ultimate-Menü öffnen/schließen (`machine:menu_button`) |
| `POWER` | c64u ausschalten – **immer** mit Sicherheitsabfrage: innerhalb der *PowerOff Zeit* (Werk: 0,7 s) dieselbe Kachel ein zweites Mal antippen |
| `CPU` | CPU-Geschwindigkeit lesen und setzen |
| `RFID` | Karte auflegen → Pfad lesen → Programm starten |
| `SD` | Datei auf SD wählen und direkt starten (ohne Karte) |
| `STATUS` | Netzwerk-/Verbindungsdetails, Antippen löst einen Verbindungstest aus |
| `SETUP` | Einstellungen (dort auch **NFC-Write**) |

Statusleiste links: Punkt + Text zeigen den Gesamtzustand –
**rot** kein WLAN · **blau** c64u nicht erreichbar · **gelb** Authentifizierung
fehlgeschlagen · **grün** alles OK. (Der CoreS3 hat keine RGB-LED wie der
MiniJoyC, deshalb wandert diese Anzeige ins Display.)

### Einstellungen

| Punkt | Werte |
|---|---|
| **NFC-Write** | Aktion: Datei auf der SD wählen → Karte auflegen → Pfad schreiben |
| **NFC-Info** | Aktion: Karte auflegen → alle Kartendaten anzeigen |
| **NFC-Dump** | Aktion: Karte auflegen → kompletten Inhalt auf die SD sichern |
| **NFC-Restore** | Aktion: Dump auswählen → Karte auflegen → zurückschreiben |
| **WLAN** | Aktion: Untermenü der WLAN-Einrichtung (siehe Abschnitt *WLAN einrichten*) |
| PowerOff Zeit | 0,5 / 0,7 / 1,0 / 1,5 / 2,0 / 2,5 / 3,0 s – Fenster für den zweiten Tipper auf die POWER-Kachel |
| Animations | On / Off |
| Effect | Auto, Static, Water, RotoZoom, SineWave, Ripple, Raster |
| FX Detail | Half / Full – der CoreS3 schafft dank PSRAM auch „Full" flüssig |
| Anim Speed | Slow / Normal / Fast |
| Effect Time | Short / Normal / Long |
| Static Time | Short / Normal / Long |
| Brightness | 32 … 255 – Display |
| Status-LED | On / Off – Lampe an Port B |
| LED Bright | 10 … 255 – Helligkeit der Lampe |
| Disk Action | Mount / Mnt+Reset / Mnt+Run (Verhalten nach dem Mounten eines Images) |
| Disk Drive | Auto (8) / A fest / B fest – Ziellaufwerk für Disk-Images |
| Beep | Bestätigungston On / Off |
| Factory Reset | Standardwerte |

Alles wird im Flash (NVS) gespeichert und beim Start wieder geladen.

---

## 4. RFID: Pfad auf die Karte schreiben und lesen

Die Karte speichert **den Pfad selbst** – keine Zuordnungstabelle, keine
Datenbank. Eine Karte funktioniert damit an jedem Gerät, das dieses Format kennt.

### Schreiben

1. Kachel `SETUP` antippen → erster Eintrag **NFC-Write** antippen
2. Datei auf der microSD antippen
3. Karte auf den Reader legen
4. Der Pfad wird geschrieben und **sofort zur Kontrolle zurückgelesen**;
   Meldung `KARTE OK` heißt: verifiziert

### Karte auslesen (NFC-Info)

Setup → **NFC-Info** → Karte auflegen. Angezeigt wird alles, was sich aus der
Karte herausholen lässt:

| Zeile | Inhalt |
|---|---|
| UID | vollständige Seriennummer plus Länge (4, 7 oder 10 Byte) |
| SAK | Select-Acknowledge-Byte |
| Typ | `NTAG215, 504 Byte`, `MIFARE Classic 1K, 1024 Byte, 16 Sektoren` … |
| Version | Hersteller und rohe `GET_VERSION`-Antwort |
| Format | nur bei Classic: welcher Schlüssel gegriffen hat |
| Inhalt | NDEF Text-Record oder Altformat, mit Zeichenzahl |
| Text | der komplette Kartentext |
| Pfad | Verzeichnis daraus |
| Datei | Dateiname daraus |
| Typ/SD | Endung, und ob die Datei auf der SD liegt – inklusive Größe |

Lange Werte werden **umgebrochen**, bevorzugt an einem Schrägstrich – Pfad und
Dateiname sind dadurch immer vollständig lesbar.

**Seite 2** erreichst du mit einem Tipper. Dort steht alles Technische:

| Zeile | Inhalt |
|---|---|
| `S 0-1` … `S 14-15` | kompletter Hex-Dump der Seiten 0 bis 15, je 8 Byte pro Zeile |
| Lock | die statischen Lock-Bytes aus Seite 2, plus Klartext „frei" / „gesperrt" |
| CC | Capability Container aus Seite 3, ausgewertet: NDEF-Version, Kapazität, les-/schreibbar |
| Schutz | Passwortschutz aus der Konfigurationsseite: aus, oder ab welcher Seite und ob Lesen mit betroffen ist |
| Zähler | NFC-Lesezähler des Chips, falls im Chip freigeschaltet |

Bei MIFARE Classic zeigt Seite 2 stattdessen die Blöcke 0, 4, 5, 6 und 8 als Hex
sowie den Schlüssel, mit dem die Authentifizierung geklappt hat.

Den genauen NTAG-Typ holt der CoreS3 über den `GET_VERSION`-Befehl (0x60) direkt
vom Chip. Kennt die Karte den Befehl nicht – ältere Ultralight zum Beispiel –
fällt er auf den Capability Container in Seite 3 zurück und meldet das auch.

### Karten kopieren (NFC-Dump / NFC-Restore)

Kopiert **jede** Karte – auch fremde MIFARE-Karten ohne NDEF-Text, rein die
Rohdaten. Setup → **NFC-Dump** → Karte auflegen. Der komplette lesbare Inhalt
landet als Textdatei in `/NFC-DUMPS/<uid>.nfc` auf der microSD:

```
# C64uRemote NFC-Dump
type NTAG215
uid  04 01 A1 01 C1 47 03
sak  00
P0   04 01 A1 8B
P1   01 C1 47 03
...
```

Bei MIFARE Classic probiert der Dump für jeden Sektor ein **Schlüssel­wörterbuch**
durch (Werksschlüssel `FFFF…`, MAD `A0A1…`, NDEF `D3F7…` und weitere gängige) und
notiert den gefundenen Schlüssel als Kommentar:

```
# Sektor 1  Key A FFFFFFFFFFFF
B4   00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
...
```

Nur Sektoren mit einem völlig unbekannten Schlüssel bleiben leer.

Setup → **NFC-Restore** → Dump aus `/NFC-DUMPS/` wählen → Zielkarte auflegen.
Der Browser filtert dabei auf `.nfc`.

**Was dabei geschrieben wird – und was nicht:**

| | |
|---|---|
| NTAG / Ultralight | nur Seiten **ab 4** bis zum Ende des Nutzbereichs |
| MIFARE Classic | Datenblöcke **und Sektor-Trailer**, aber **nicht** Block 0 |

Sektor-Trailer werden nur zurückgeschrieben, wenn ihre **Zugriffsbits in sich
stimmig** sind – ein ungültiges Muster würde den Sektor unwiderruflich sperren
und wird deshalb übersprungen. Die Datenblöcke eines Sektors schreibt der CoreS3
immer **vor** dem Trailer, damit der Zugriff nicht mitten im Sektor verloren geht.

**Warum die Rohdaten trotzdem nicht 1:1 übereinstimmen**

Bei MIFARE Classic gibt es drei Dinge, die physikalisch nicht kopierbar sind:

* **Block 0 (UID):** fest im Chip gebrannt, auf normalen Karten schreibgeschützt.
* **Schlüssel A im Trailer (Bytes 0–5):** MIFARE liefert Schlüssel A beim Lesen
  **immer als `00 00 00 00 00 00`** zurück – er ist prinzipiell nicht auslesbar.
  Der Dump trägt an dieser Stelle den tatsächlich gefundenen Schlüssel ein
  (damit die Datei nutzbar bleibt), aber byte-genau wie das Original kann er
  nicht sein.
* **Schlüssel B:** nur lesbar, wenn die Zugriffsbits es erlauben – sonst ebenfalls `00…`.

Identisch kopiert werden dagegen **alle Datenblöcke** sowie die Zugriffsbits und
Schlüssel B (soweit lesbar). Vergleichst du Quelle und Kopie über NFC-Info
Seite 2, unterscheiden sich also nur Block 0 und die ersten sechs Byte der
Trailer – alles andere stimmt überein. Sollte ein **Datenblock** abweichen, ist
das ein Fehler: sag mir welcher, dann schaue ich nach.

Die Dumps sind reiner Text – du kannst sie am Rechner ansehen, sichern oder mit
einem Editor anpassen, bevor du sie zurückschreibst.

### Lesen und starten

1. Kachel `RFID`
2. Karte auflegen → Pfad wird gelesen, die Datei von der microSD geholt und
   per ReST-API an den c64u geschickt
3. Fortschrittsbalken, danach `GESTARTET: <name>`

Karte abnehmen und die nächste auflegen – der Bildschirm bleibt im Lesemodus.

### Befehlskarten

Eine Karte kann statt eines Dateipfads auch einen Befehl tragen, der sofort
ausgeführt wird – ohne Menü und ohne SD-Karte:

```
CMD:RESET      CMD:REBOOT      CMD:MENU
CMD:POWEROFF=0     sofort ausschalten
CMD:POWEROFF=8     nachfragen, 8 s Zeit zum Bestätigen
CMD:CPU=10         CPU auf 10 MHz
```

Die Wartezeit steht also **auf der Karte**, `0` heißt ohne Nachfrage. Bestätigt
wird durch erneutes Auflegen derselben Karte (die UID muss passen) oder mit
einem Tipper auf den Bildschirm; eine andere Karte oder ein abgelaufener
Countdown brechen ab. Angelegt
werden solche Karten über `SETUP → NFC-Cmd`; die Liste enthält neben den festen
Befehlen alle CPU-Stufen, die dein c64u anbietet. Groß-/Kleinschreibung ist
egal, und es bleibt ein gewöhnlicher NDEF-Textrecord – jede NFC-App am Telefon
kann so eine Karte lesen oder schreiben. Die M5Dial-Fassung versteht dasselbe
Format.

### Karte einfach auflegen (Auto-NFC)

Die Hintergrundabfrage laeuft nicht nur auf dem Hauptbildschirm, sondern auch im
**SD-Browser**, auf der **Status**-Seite und im **CPU**-Menue; danach kehrt der
CoreS3 an genau diese Stelle zurueck. Ausgenommen sind die Seiten, auf denen
gerade mit Karten gearbeitet wird (Datei fuer *NFC-Write* auswaehlen, Dump
zurueckschreiben, Setup, WLAN-Einrichtung) - dort wuerde ein Programmstart der
Absicht des Benutzers widersprechen.

Die Kachel ist meist gar nicht nötig: Auf dem Hauptbildschirm fragt die Firmware
den Leser im Hintergrund ab. Wird eine Karte erkannt, wechselt sie von selbst in
den Lesemodus und startet das hinterlegte Programm. Danach bleibt die Leseseite
noch 20 Sekunden stehen, damit gleich die nächste Karte drankommt; danach – oder
sobald du den Bildschirm antippst – geht es zurück zum Hauptbildschirm.

Den Abstand stellst du im Setup unter `Auto-NFC` ein: `Off`, `1.5s`, `0.7s`
(Werkseinstellung) oder `0.3s`.

Damit das nicht bremst, wird für die reine Anwesenheitsprobe das Zeitfenster des
MFRC522 von 25 ms auf rund 2 ms verkürzt und unmittelbar danach – noch vor der
Kartenauswahl – wiederhergestellt. Eine Karte antwortet in unter 0,1 ms
(Frame Delay Time ≈ 86 µs), alle anderen Vorgänge behalten das volle
Zeitfenster. Grundlast bei `0.7s`: unter ein Prozent, also kein verpasster
Frame bei 30 fps.

### Unterstützte Karten

| Typ | Zugriff |
|---|---|
| **NTAG215** (empfohlen), NTAG213 / 216, MIFARE Ultralight | 4-Byte-Seiten, kein Schlüssel |
| **MIFARE Classic** 1K / 4K / Mini | 16-Byte-Blöcke, Schlüssel `D3F7…` (NDEF) oder `FFFF…` (Werk) |

Der Kartentyp wird über das SAK-Byte automatisch erkannt.

### Kartenformat: kompatibel zu TeensyROM / Zaparoo

Die Karten werden im selben Format beschrieben wie beim
[TeensyROM NFC-Loader](https://github.com/SensoriumEmbedded/TeensyROM/blob/main/docs/NFC_Loader.md)
und beim [Zaparoo-Projekt](https://github.com/ZaparooProject/zaparoo-core):

> Ein einzelner **NDEF-Record**, Typ **Text** (Well Known, UTF-8), Inhalt ist
> der Pfad zur Programmdatei.

```
SD:OneLoad v5/Bubble Bobble.crt
```

Damit funktioniert **dieselbe Karte an beiden Systemen**, solange die
Verzeichnisstruktur auf beiden SD-Karten gleich ist.

| Element | Bedeutung |
|---|---|
| `SD:` | Quelle – wird auch akzeptiert: `USB:`, `TR:` oder gar kein Präfix |
| Pfad | relativ zur Wurzel der SD-Karte, ohne führenden `/` |
| `?` als Dateiname | startet eine **zufällige** Datei aus dem Verzeichnis |
| Länge | max. 246 Zeichen (bei NTAG213 entsprechend weniger) |

Der CoreS3 schreibt immer `SD:` + Pfad ohne führenden Schrägstrich und macht beim
Lesen daraus wieder einen absoluten Pfad auf der eigenen Karte. `USB:` und `TR:`
werden beim Lesen ebenfalls akzeptiert und auf der SD gesucht – falls du eine
Karte vom TeensyROM mit einer anderen Quelle beschriftet hast.

Karten lassen sich damit auch **mit dem Handy** beschreiben: die kostenlose App
*NFC Tools* schreibt einen Text-Record (UTF-8, „Well Known") – genau das, was
hier erwartet wird.

**Ablage auf der Karte**

* **NTAG / Ultralight:** NDEF-TLV ab Seite 4. Die Seiten 0–3 (UID, Lock-Bytes,
  Capability Container) bleiben unberührt.
* **MIFARE Classic:** NDEF-TLV in den Datenblöcken ab Block 4, Sektor-Trailer
  werden übersprungen. Authentifiziert wird zuerst mit dem NDEF-Schlüssel
  `D3F7D3F7D3F7`, ersatzweise mit dem Werksschlüssel `FFFFFFFFFFFF`.
  **Trailer und MAD werden nie beschrieben** – eine Karte kann dadurch nicht
  unbrauchbar werden. Eine nicht NDEF-formatierte Classic-Karte ist dafür unter
  Umständen nur an diesem Gerät lesbar; NFC-Info zeigt an, welcher Schlüssel
  gegriffen hat.

**Toleranz beim Lesen:** Der TeensyROM trägt in seinen Tags eine falsche
Payload-Länge ein (konstant `0x10`), während die TLV-Länge stimmt. Beim letzten
Record der Nachricht hat deshalb die TLV-Länge Vorrang – sonst würde der Pfad
nach 13 Zeichen abgeschnitten. Füllbytes (`0x00`) und der TLV-Terminator
(`0xFE`) beenden den Text zusätzlich. Mehrfache Schrägstriche werden
zusammengefasst, `SD://Ordner/…` funktioniert also genauso wie `SD:Ordner/…`.

Das frühere Rohformat `C64UPATH` wird beim **Lesen** weiterhin erkannt, bereits
beschriebene Karten funktionieren also unverändert weiter. Geschrieben wird ab
jetzt immer NDEF.

### Was gestartet werden kann

| Endung | ReST-Aufruf |
|---|---|
| `.prg` | `POST /v1/runners:run_prg` |
| `.crt` | `POST /v1/runners:run_crt` |
| `.sid` | `POST /v1/runners:sidplay` |
| `.mod` | `POST /v1/runners:modplay` |
| `.d64 .d71 .d81 .g64 .g71` | `POST /v1/drives/<a\|b>:mount` (`type`, `mode=readwrite`) |

### Disk-Images: Laufwerk 8 und Autostart

`.d64` (und die anderen Image-Formate) landen auf dem Laufwerk, das am **IEC-Bus
8** hängt. Bei *Disk Drive = Auto (8)* fragt der CoreS3 vor dem Mounten
`GET /v1/drives` ab und sucht das Laufwerk mit `bus_id: 8` – normalerweise
Laufwerk A, aber wenn du im Ultimate-Menü die Bus-IDs getauscht hast, findet er
trotzdem das richtige. Danach wird `PUT /v1/drives/<x>:on` geschickt, damit das
Image auch dann am Bus liegt, wenn das Laufwerk vorher deaktiviert war.

Was danach passiert, steuert *Disk Action*:

* **Mount** – nur einlegen
* **Mnt+Reset** – einlegen + Reset
* **Mnt+Run** – einlegen, Reset, dann `lO"*",8,1` ⏎ und nach dem Laden `rU` ⏎
  (abgekürzte BASIC-Befehle, damit die Zeile in die 10 Byte des
  C64-Tastaturpuffers passt) – lädt und startet das erste Programm des Images

Die Wartezeiten sind dabei **nicht fest verdrahtet**: der CoreS3 pollt
`GET /v1/machine:readmem?address=CC` und beobachtet `$CC` (BLNSW). Der Wert ist
0, solange der Cursor blinkt, also während BASIC auf Eingaben wartet. Der Ablauf
ist damit: auf READY warten → LOAD tippen → warten bis der Cursor aufhört zu
blinken (Laden läuft) → warten bis er wieder blinkt (fertig) → RUN tippen. Ein
Spiel, das 40 Sekunden lädt, funktioniert damit genauso wie eines, das in zwei
Sekunden da ist. Timeout fürs Laden: 3 Minuten, danach Meldung
`LADEN DAUERT ZU LANGE`.

Der Upload läuft **streamend** in 1-kB-Blöcken direkt von der SD-Karte in den
TCP-Socket. Deshalb passen auch 175-kB-Images ohne Umweg durch den RAM
ohne PSRAM.

---

## 5. WLAN einrichten

Seit dieser Fassung braucht das WLAN keinen Neu-Build mehr. SSID, Passwort,
c64u-Adresse und c64u-Passwort liegen im internen Speicher (NVS) und lassen sich
jederzeit am Gerät ändern. Bis zu **vier Netze** werden gespeichert und beim
Verbinden nacheinander durchprobiert; beim Start sucht der CoreS3 einmal und
beginnt mit dem stärksten bekannten Netz.

Alles steckt unter **Setup → WLAN**:

| Eintrag | Wirkung |
|---|---|
| `Netz suchen` | Umgebung scannen, Netz aus der Liste wählen |
| `Von SD laden` | `/wifi.txt` von der microSD einlesen |
| `Setup-Portal` | eigener Accesspoint mit Weboberfläche |
| `Gespeichert` | gespeichertes Netz auswählen und verbinden |
| `Auf NFC-Karte` | gespeichertes Netz auf eine NFC-Karte schreiben |
| `Auf SD sichern` | alle gespeicherten Netze als `/wifi.txt` auf die microSD schreiben |
| `Netz löschen` | einzelnen Eintrag entfernen |
| `Alle löschen` | alle Zugangsdaten verwerfen |

### Weg 1: NFC-Karte

Nach *Netz suchen* → Netz auswählen fragt der CoreS3 nach dem Passwort und wartet
auf eine Karte. Auf der Karte steht ein ganz normaler **NDEF-Text-Record**:

```
WIFI:S:MeinWLAN;T:WPA;P:MeinPasswort;;
```

Das ist dasselbe Schema wie bei WLAN-QR-Codes. Steht auf der Karte nur ein
Passwort ohne `WIFI:`-Präfix, wird es dem vorher gewählten Netz zugeordnet.

Geschrieben wird die Karte mit dem Handy, z. B. mit **NFC Tools** (Android und
iOS): *Schreiben → Datensatz hinzufügen → Text*. Wichtig für iPhone-Nutzer: iOS
beschreibt nur NTAG213/215/216 bzw. Ultralight, **kein** MIFARE Classic.

Eine vollständige `WIFI:`-Karte wirkt auch **außerhalb** der Einrichtung: wird
sie auf dem Hauptbildschirm aufgelegt, speichert der CoreS3 das Netz und verbindet
sich sofort damit. Bis zu acht Sekunden wartet er auf die Verbindung und meldet
dann `WLAN AKTIV` mit IP-Adresse bzw. `NETZ NICHT DA`, wenn das Netz nicht in
Reichweite ist. Ist das Netz bereits verbunden, passiert nichts
(`SCHON VERBUNDEN`) – die Karte darf also liegen bleiben.

> Das Passwort steht unverschlüsselt auf der Karte und ist mit jedem Handy
> lesbar. Nach dem Einlernen die Karte am besten wieder überschreiben – im
> Gerät liegt es danach ohnehin im NVS.

### Weg 2: `/wifi.txt` auf der SD-Karte

Vorlage: `wifi.txt.example`. Die Datei wird beim Start automatisch gelesen,
solange noch kein Netz gespeichert ist, und jederzeit über
*Setup → WLAN → Von SD laden*.

```
ssid = MeinWLAN
pass = MeinWlanPasswort

ssid = Hotspot
pass = geheim123

host     = 192.168.0.64
hostpass =
```

Jede neue `ssid`-Zeile beginnt einen Eintrag, `#` und `;` leiten Kommentare ein.

Umgekehrt schreibt *Setup → WLAN → Auf SD sichern* alle gespeicherten Netze
samt `host` und `hostpass` als `/wifi.txt` auf die Karte. Eine dort schon
vorhandene `wifi.txt` wird vorher nach `wifi.bak` umbenannt. Damit lässt sich
ein zweites Gerät ohne Tipperei einrichten – die Passwörter stehen dabei im
Klartext auf der Karte.

### Weg 3: Setup-Portal

*Setup-Portal* macht aus dem CoreS3 für fünf Minuten einen eigenen Accesspoint
(`C64uRemote-Setup` / `c64ultimate`). Netzname, Passwort und die aufzurufende
Adresse stehen währenddessen groß auf dem Display. Im Browser wählst du ein Netz
aus der Liste oder tippst die SSID von Hand ein, gibst das Passwort und optional
Adresse und Passwort des c64u an.

Nach dem Speichern schaltet der CoreS3 den Accesspoint ab und verbindet sich mit
dem neuen Netz – dass die Browserverbindung dabei abbricht, ist normal.

Der Accesspoint läuft bewusst ohne Station-Teil auf festem Kanal 1. Bliebe die
Station aktiv, würde sie im Hintergrund weiter nach dem gespeicherten Netz
suchen, dabei den Kanal wechseln und angemeldete Handys abwerfen.

### Speicherort

Die Zugangsdaten liegen im NVS-Namensraum `c64unet`, getrennt von den
Bedieneinstellungen in `c64uremote`. *Factory Reset* lässt sie deshalb
unangetastet; verworfen werden sie nur über *WLAN → Alle löschen*.

---

## 6. Speicher: Vollbild-Sprite im PSRAM

Der CoreS3 hat 8 MB PSRAM – ganz anders als der Core Basic, für den diese
Firmware ursprünglich geschrieben wurde. `setup()` legt deshalb ein
Vollbild-Sprite an:

```cpp
canvas.setPsram(true);
canvas.setColorDepth(16);
gUseCanvas = canvas.createSprite(320, 240);   // 150 kB
gDraw      = gUseCanvas ? &canvas : &M5.Display;
```

Alle Zeichenaufrufe laufen über `gDraw`. Klappt die Zuteilung nicht, zeigt
`gDraw` auf das Display, `pushFrame()` wird zum Leerlauf und alles funktioniert
weiter – nur mit sichtbarem Bildaufbau.

Der Puffer behält seinen Inhalt zwischen zwei Bildern, die Teilaktualisierungen
(nur Statusleiste, nur Kacheln, nur Effektbereich) funktionieren also
unverändert. `render()` merkt sich, ob überhaupt gezeichnet wurde, und schiebt
nur dann: ein `pushSprite()` kostet 150 kB über denselben SPI-Bus, an dem auch
die SD-Karte hängt.

Das Logo liegt weiterhin im Flash und wird per `pgm_read_word` gelesen – ein
Cache dafür wäre bei 8 MB PSRAM zwar möglich, bringt aber nichts, weil ohnehin
jede Zeile einmal durchlaufen wird.

`FX Detail` gibt es weiterhin, aber der CoreS3 schafft `Full` dauerhaft flüssig.

---

## 7. Statuslampe an Port B

Drei **SK6812** an Port B (die übliche M5Stack Unit RGB) zeigen den Zustand,
ohne dass man aufs Display schauen muss:

| Lampe | Bedeutung |
|---|---|
| alle rot | keine WLAN-Verbindung |
| alle blau | WLAN da, der c64u antwortet nicht |
| alle gelb | c64u erreichbar, Passwort stimmt nicht |
| alle grün | alles verbunden |
| magenta, Lauflicht | Setup-Portal läuft |
| cyan als Fortschrittsbalken | Datei wird in den c64u geladen – erledigte Drittel leuchten voll, das laufende pulsiert |
| kurzer grüner Blitz | NFC-Karte gelesen bzw. beschrieben |
| kurzer roter Blitz | Karte leer, nicht lesbar oder Schreibfehler |

Die Farben entsprechen dem Punkt links in der Statusleiste. Ereignisblitze haben
Vorrang und klingen über 0,6 s aus.

Aktualisiert wird alle 20 ms und nur dann, wenn sich wirklich etwas geändert hat
– ein `show()` für drei LEDs dauert unter 100 µs. Während eines Uploads und
während der Wartephase nach dem Mounten blockiert die Hauptschleife; damit die
Lampen trotzdem laufen, werden sie dort von Hand mitgenommen.

Im Setup gibt es zwei Punkte dazu: **Status-LED** (On/Off) und **LED Bright**
(10 … 255, feiner abgestuft als die Displayhelligkeit, weil die LED schon bei
kleinen Werten kräftig blendet). Beides liegt im NVS und übersteht einen
Neustart.

---

## 8. Bekannte Fallstricke

* **Display und SD teilen den SPI-Bus.** Wenn die SD sporadisch nicht erkannt
  wird: der Code versucht automatisch 20 MHz und danach 4 MHz. Bleibt es dabei,
  hilft eine andere Karte (FAT32, ≤ 32 GB) meist sofort.
* **`X-Password`** wird nur gesendet, wenn `C64U_TARGET_PASSWORD` gesetzt ist.
  Ist im c64u ein Netzwerkpasswort konfiguriert und hier keins eingetragen,
  antwortet die API mit `403` → Statusleiste gelb, `AUTH?`.
* **`machine:poweroff`** ist U64-only und antwortet oft nicht mehr sauber – eine
  Fehlermeldung direkt nach dem Ausschalten ist normal.
* **Pfade > 128 Zeichen** passen nicht auf die Karte; der SD-Browser meldet das
  vor dem Schreiben. Ordnerstruktur flach halten.
* **Port A liegt auf G2/G1**, nicht auf G21/G22 wie beim Core Basic – und SDA
  und SCL sind vertauscht. Wer den Quelltext der Core-Fassung übernimmt, ohne
  `kI2cSdaPin`/`kI2cSclPin` anzupassen, findet das RFID2 nicht.
* **Kein eingebauter NFC-Leser.** Der CoreS3 hat weder RFID noch NFC an Bord;
  ohne Unit RFID2 bleiben die Kartenfunktionen grau.
* **Download-Modus:** meldet sich kein serieller Port, RESET 3 s halten, bis die
  grüne LED angeht.
* **Die Status-LED lässt sich nicht erkennen.** SK6812 ist eine reine
  Ausgangsleitung ohne Rückkanal. Ist keine angeschlossen, gehen die Daten ins
  Leere – ohne Nebenwirkung. Wer G9 anders benutzt, schaltet die Lampe im Setup
  ab, sonst liegt dort dauerhaft ein Datensignal an.

## Quellen

* [ReST API Calls – Ultimate Documentation](https://1541u-documentation.readthedocs.io/en/latest/api/api_calls.html)
* [mlund/ultimate64 (Referenz für PETSCII-Autostart und Mount)](https://github.com/mlund/ultimate64)
* [Unit RFID / RFID2 Arduino Tutorial – m5-docs](https://docs.m5stack.com/en/arduino/projects/unit/unit_rfid)
* [kkloesener/MFRC522_I2C](https://github.com/kkloesener/MFRC522_I2C)
* [ReadyOS-C64/C64uRemote (Originalprojekt)](https://github.com/ReadyOS-C64/C64uRemote)

---

## Entstehung

Portierung, Erweiterungen und Handbücher sind mit Unterstützung von
Claude (Anthropic) entstanden. Konzept, Idee, Hardware-Entscheidungen und
sämtliche Tests auf den echten Geräten: Martin Oswald (@mad).

---

## Lizenz

Dieses Projekt steht unter der **MIT-Lizenz**. Der vollständige Text liegt in
[`LICENSE`](LICENSE) im Projektstamm.

Ursprung ist das Projekt
[C64uRemote von Karl Prosser (@klumsy)](https://github.com/ReadyOS-C64/C64uRemote),
das er unter der
[MIT-Lizenz](https://github.com/ReadyOS-C64/C64uRemote/blob/main/LICENSE)
veröffentlicht hat. Diese Fassung für den M5Stack CoreS3 ist eine daraus abgeleitete
Erweiterung und steht unter denselben Bedingungen:

* Copyright (c) 2026 Karl Prosser – Originalprojekt
* Copyright (c) 2026 Martin Oswald (@mad, [1MHz.de](https://1MHz.de)) – Portierung und Erweiterungen

Die MIT-Lizenz erlaubt Benutzung, Veränderung und Weitergabe – auch
kommerziell. Einzige Bedingung: **Copyright-Vermerk und Lizenztext müssen
erhalten bleiben**, also in jeder Kopie oder abgeleiteten Fassung mitgeliefert
werden. Eine Gewährleistung gibt es nicht.

### Fremde Bestandteile

Die eingebundenen Bibliotheken haben ihre eigenen Lizenzen und Copyright-Inhaber:

| Bibliothek | Lizenz |
|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) | MIT, (c) M5Stack |
| [M5GFX](https://github.com/m5stack/M5GFX) | MIT, (c) M5Stack |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT, (c) Benoit Blanchon |
| [MFRC522_I2C](https://github.com/kkloesener/MFRC522_I2C) | siehe Repository |

Sie werden von PlatformIO beim Bauen geladen und liegen diesem Archiv nicht bei.
