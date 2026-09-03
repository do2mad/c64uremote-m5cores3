% C64uRemote für M5Stack CoreS3
% Benutzerhandbuch
% Version 1.0

# Willkommen

C64uRemote verwandelt einen **M5Stack CoreS3** in eine komfortable Fernbedienung
für den **Commodore 64 Ultimate (c64u)** und den **Ultimate64 Elite-II**. Über
das WLAN steuerst du den Rechner fern: Reset, Reboot, Ausschalten, das
Ultimate-Menü öffnen und die CPU-Geschwindigkeit umstellen.

Mit dem optionalen **RFID2-Leser** und einer **microSD-Karte** wird daraus eine
Spielekonsole zum Auflegen: Du hältst eine NFC-Karte an das Gerät, und das
zugehörige Spiel startet auf dem C64. Die Karten sind kompatibel zum TeensyROM-
und Zaparoo-Format – dieselbe Karte funktioniert an beiden Systemen.

Dieses Projekt baut auf der ursprünglichen Idee von Karl Prosser (@klumsy) auf
und wurde von Martin Oswald (@mad, 1MHz.de) für den M5Stack CoreS3 erweitert.

# Das brauchst du

**Zwingend erforderlich:**

- Ein M5Stack CoreS3, CoreS3 SE oder CoreS3 Lite
- Ein Commodore 64 Ultimate bzw. Ultimate64 Elite-II im selben WLAN

**Optional, für die NFC-Funktionen:**

- M5Stack Unit RFID2 (WS1850S), angeschlossen an **Port A** (der CoreS3 hat
  keinen eingebauten NFC-Leser)
- Eine M5Stack Unit RGB (drei SK6812) an **Port B** als Statuslampe – hübsch,
  aber nicht nötig
- Eine microSD-Karte (FAT32 formatiert, bis 16 GB) im CoreS3
- NFC-Karten: empfohlen NTAG215, es gehen auch NTAG213/216 und MIFARE Classic

Ohne Leser oder SD-Karte laufen alle übrigen Funktionen normal weiter.

# Erste Einrichtung

Damit der CoreS3 deinen C64 findet, müssen einmalig die WLAN-Zugangsdaten und die
Adresse des C64 hinterlegt werden. Das geht **direkt am Gerät** unter
*Setup → WLAN* – der Quelltext muss dafür nicht mehr angefasst werden. Die Daten
landen im internen Speicher und überstehen jeden Neustart. Wie das im Einzelnen
läuft, steht im Kapitel *WLAN einrichten*.

Wer die Zugangsdaten lieber schon beim Programmieren mitgibt, trägt sie
weiterhin in `build_env.h` ein (siehe technische Dokumentation). Sie gelten dann
als Startwerte für den allerersten Start.

Der Verbindungsstatus steht immer oben links im Display:

| Anzeige | Bedeutung |
|---|---|
| **C64U OK** (grüner Punkt) | Alles verbunden, bereit |
| **NO C64U** (blau) | WLAN da, aber der C64 antwortet nicht |
| **AUTH?** (gelb) | C64 erreichbar, aber Passwort stimmt nicht |
| **NO WIFI** (rot) | Keine WLAN-Verbindung |

Rechts daneben siehst du die IP-Adresse, die aktuelle CPU-Geschwindigkeit und
ob RFID-Leser und SD-Karte erkannt wurden.

# Bedienung per Touch

Der CoreS3 hat keine Tasten A, B und C mehr – nur noch **POWER** und **RESET**
an der linken Seite. Bedient wird er komplett über den **Touchscreen**.

| Geste | Wirkung |
|---|---|
| **Antippen** | Kachel oder Listenzeile auswählen und ausführen |
| **Wischen** | die Liste folgt dem Finger, Zeile für Zeile |
| **Scrollleiste rechts ziehen** | schnell durch lange Listen |
| **Zurück** (unten links) | eine Ebene zurück |
| **▲ / ▼** (unten rechts) | eine Zeile hoch bzw. runter |
| **Lang antippen** | im SD-Browser: einen Ordner zurück |

Die Leiste am unteren Rand ist selbst eine Bedienfläche: links liegt immer das
Zurück-Feld, rechts erscheinen auf Listenseiten die beiden Blätterfelder,
dazwischen steht ein kurzer Hinweis, was ein Tipper hier gerade bewirkt.

**Blättern in langen Listen.** Passt eine Liste nicht auf den Bildschirm,
erscheint rechts eine **Scrollleiste**. Sie zeigt, wo du dich befindest und wie
lang die Liste ist – und sie lässt sich direkt mit dem Finger ziehen. Bei vielen
Einträgen kommst du damit in einer Bewegung ans andere Ende.

Genauso funktioniert das Wischen mitten in der Liste: sie folgt dem Finger.
Sobald sie sich bewegt, ist klar, dass du blättern und nicht auswählen wolltest –
ein Wisch löst deshalb nie versehentlich einen Eintrag aus, auch ein langsamer
nicht. Passt eine Liste komplett auf den Bildschirm, gibt es nichts zu schieben;
dort bleibt jeder Tipper ein Tipper.

**POWER und RESET** sind Hardware-Tasten des CoreS3 und haben nichts mit dem C64
zu tun: POWER kurz drücken schaltet den CoreS3 ein, 6 s halten schaltet ihn aus.
RESET startet ihn neu.

# Der Hauptbildschirm

Oben die Statusleiste, in der Mitte das animierte Logo, darunter alle Befehle
als Kacheln:

```
RESET   REBOOT   MENU    POWER   CPU
JOY     RFID     SD      STATUS  SETUP
```

Du tippst eine Kachel einfach an – ein Tipper wählt sie aus und führt sie
gleich aus.

| Kachel | Was passiert |
|---|---|
| **RESET** | Der C64 wird zurückgesetzt (wie die Reset-Taste) |
| **REBOOT** | Der C64 startet komplett neu |
| **MENU** | Öffnet oder schließt das Ultimate-Menü am C64 |
| **POWER** | Schaltet den C64 aus – zur Sicherheit zweimal antippen |
| **CPU** | CPU-Geschwindigkeit ansehen und ändern |
| **JOY** | Joystickports am C64 tauschen (Normal ↔ Swapped) |
| **RFID** | Karte auflegen und das darauf gespeicherte Spiel starten |
| **SD** | Ein Spiel direkt von der SD-Karte auswählen und starten |
| **STATUS** | Ausführliche Verbindungsinfos, Antippen startet einen Verbindungstest |
| **SETUP** | Einstellungen und NFC-Werkzeuge |

**Ausschalten (POWER):** Aus Versehen ausschalten wäre ärgerlich, deshalb kommt
zuerst die Abfrage *POWER OFF? NOCHMAL TIPPEN!*. Erst ein zweiter Tipper auf
dieselbe Kachel innerhalb des Zeitfensters schaltet wirklich aus. Ein Tipper
irgendwo daneben bricht ab.

# Die Statuslampe

Hängt an **Port B** eine **M5Stack Unit RGB** (drei SK6812), zeigt sie den
Zustand auch dann, wenn du gerade nicht aufs Display schaust:

| Lampe | Bedeutung |
|---|---|
| alle drei **rot** | keine WLAN-Verbindung |
| alle drei **blau** | WLAN da, aber der C64 antwortet nicht |
| alle drei **gelb** | C64 erreichbar, Passwort stimmt nicht |
| alle drei **grün** | alles verbunden, bereit |
| **magenta**, Lauflicht | Setup-Portal läuft |
| **cyan** als Balken | ein Spiel wird gerade in den C64 geladen |
| kurz **grün** (alle) | Karte erfolgreich gelesen oder beschrieben |
| kurz **rot** (alle) | Karte leer, nicht lesbar oder Schreibfehler |

Die Farben sind dieselben wie beim Punkt links oben im Display.

**Beim Laden** werden die drei LEDs zum Fortschrittsbalken: die erste leuchtet
voll, sobald ein Drittel übertragen ist, dann die zweite, dann die dritte. Der
gerade laufende Abschnitt pulsiert, damit man sieht, dass es weitergeht. Ist die
Dateigröße ausnahmsweise unbekannt, läuft stattdessen ein Lauflicht.

Unter *Setup → Status-LED* schaltest du sie ab, unter *LED Bright* stellst du
die Helligkeit ein (10 bis 255; 60 ist Werkseinstellung und für die meisten
Räume angenehm).

> Ob wirklich eine LED angeschlossen ist, kann der CoreS3 **nicht** erkennen –
> so eine LED sendet nichts zurück. Steckt keine dran, passiert einfach nichts.
> Abschalten musst du nur dann, wenn du Port B für etwas anderes brauchst.

# CPU-Geschwindigkeit ändern

Kachel **CPU** antippen. Oben steht die aktuelle Geschwindigkeit, darunter die
Auswahlliste – die gewünschte Stufe einfach antippen. Der CoreS3 liest die
verfügbaren Stufen direkt vom C64 aus, du bekommst also genau die Werte, die
dein Gerät kann.

# Joystickports tauschen

Manche Spiele erwarten den Joystick in Port 1, andere in Port 2. Statt das Kabel
umzustecken, lässt sich die Belegung im C64 vertauschen.

Kachel **JOY** antippen – jedes Auslösen schaltet zwischen *Normal* und
*Swapped* hin und her, kurz erscheint *JOY Swapped* bzw. *JOY Normal*.

Unter *SETUP → Joystick* steht der aktuelle Stand, und dort schaltest du durch
alle Werte, die dein C64 anbietet: neben *Normal* und *Swapped* je nach Firmware
auch *WASD P1* und *WASD P2* – dann steuert die Tastatur den jeweiligen Port.

Einen eigenen Fernsteuerbefehl gibt es dafür in der Ultimate-Firmware nicht. Der
CoreS3 setzt die Einstellung *Joystick Swapper* in der C64-Konfiguration, genau wie
bei der CPU-Geschwindigkeit. Der Stand bleibt deshalb erhalten, bis er wieder
geändert wird – auch über einen Reset hinweg.

# Spiele per Karte starten (RFID)

Voraussetzung: RFID2-Leser angeschlossen, microSD mit deinen Spielen eingelegt,
und die Karte wurde vorher beschrieben (siehe unten).

## Einfach auflegen (Automatik)

Im Normalfall musst du gar nichts bedienen: Der CoreS3 schaut im Hintergrund
regelmäßig nach, ob eine Karte aufliegt. Sobald er eine erkennt, wechselt er von
selbst in den Lesemodus und startet das hinterlegte Programm.

Das gilt nicht nur auf dem Hauptbildschirm, sondern auch, während du **im
SD-Browser blätterst**, auf der **Status**-Seite und im **CPU**-Menü. Danach
kehrt der CoreS3 genau dorthin zurück, wo du warst.

Bewusst **nicht** reagiert wird, während du gerade mit Karten arbeitest: beim
Auswählen einer Datei für *NFC-Write*, beim Zurückschreiben eines Dumps, im
Setup und in der WLAN-Einrichtung. Dort wäre ein Programmstart genau das
Gegenteil dessen, was du gerade tust.

Nach dem Start bleibt die Leseseite noch etwa 20 Sekunden stehen, du kannst also
gleich die nächste Karte auflegen. Danach – oder sobald du den Bildschirm
antippst – geht es zurück.

Wie oft nachgeschaut wird, stellst du unter *Setup → Auto-NFC* ein:

| Einstellung | Bedeutung |
|---|---|
| **Off** | keine Hintergrundabfrage, Karten nur über die Kachel RFID |
| **1.5s** | sehr sparsam |
| **0.7s** | Werkseinstellung, guter Kompromiss |
| **0.3s** | reagiert am schnellsten |

Die Abfrage ist so kurz, dass sie selbst in der Stellung *0.3s* weder die
Animation noch die Bedienung merklich bremst.

## Über die Kachel RFID

Willst du bewusst eine Karte einlesen – oder ist *Auto-NFC* ausgeschaltet – geht
es auch von Hand:

1. Kachel **RFID** antippen.
2. Die NFC-Karte auf den Leser legen.
3. Der CoreS3 liest den gespeicherten Pfad, holt die Datei von der SD-Karte und
   schickt sie an den C64. Ein Fortschrittsbalken zeigt den Upload.
4. Nach *GESTARTET: …* läuft das Spiel.

Karte abnehmen, nächste auflegen – der Bildschirm bleibt im Lesemodus, du kannst
also mehrere Karten hintereinander abspielen.

## Welche Dateien funktionieren

| Endung | Was es ist |
|---|---|
| `.prg` | Programm (wird geladen und gestartet) |
| `.crt` | Modul/Cartridge |
| `.sid` | SID-Musik |
| `.mod` | Amiga-MOD-Musik |
| `.d64 .d71 .d81 .g64 .g71` | Diskettenabbilder |

Bei Diskettenabbildern wird das Abbild als Laufwerk 8 eingelegt. Was danach
passiert, stellst du im Setup unter *Disk Action* ein: nur einlegen, einlegen
und Reset, oder einlegen und das erste Programm automatisch starten.

# Befehlskarten

Eine Karte muss nicht auf ein Spiel zeigen – sie kann auch einen **Befehl**
tragen. Aufgelegt löst sie ihn sofort aus, ganz ohne Menü und ohne SD-Karte.

| Karte | Wirkung |
|---|---|
| **Reset** | Setzt den C64 zurück |
| **Reboot** | Startet den C64 komplett neu |
| **Ultimate Menu** | Öffnet oder schließt das Ultimate-Menü |
| **PowerOff direkt** | Schaltet sofort aus |
| **PowerOff mit Abfrage** | Fragt nach – zum Bestätigen die Karte innerhalb des Zeitfensters ein zweites Mal auflegen |
| **CPU x MHz** | Stellt die CPU auf den auf der Karte hinterlegten Wert |
| **Joystick tauschen** | Vertauscht die Joystickports (Normal ↔ Swapped) |
| **Joystick Normal / Swapped / WASD P1 / WASD P2** | Setzt die Portbelegung fest auf diesen Wert |

## Eine Befehlskarte anlegen

1. **NFC-Cmd** in den Einstellungen wählen.
2. Aus der Liste den gewünschten Befehl aussuchen. Nach den festen Einträgen
   folgen erst die Joystick-Belegungen, dann alle CPU-Stufen, die dein C64
   anbietet – eine Karte „CPU 10 MHz" ist also ein einziger Klick. Rechts steht
   *JOY* oder *CPU*, damit du die beiden Blöcke auseinanderhältst.
3. Karte auflegen, *KARTE OK* bedeutet: geschrieben und geprüft.

## PowerOff mit Abfrage

Die Wartezeit steht **auf der Karte**, nicht im Gerät. Beim Anlegen wird der
Wert aus *NFC-Cmd PowOff* übernommen (3, 5, 8 oder 15 Sekunden, Werkseinstellung
8 s). Legst du so eine Karte auf, erscheint *POWER OFF? NOCHMAL!* mit einem
Countdown. Zum Ausschalten:

- die **gleiche Karte** noch einmal auflegen, oder
- den **Bildschirm antippen**

Läuft der Countdown ab oder kommt eine andere Karte, passiert nichts. Eine Karte
mit der Zeit **0** schaltet ohne Nachfrage sofort aus.

## Was auf der Karte steht

Der Befehl ist gewöhnlicher Text in einem NDEF-Record – du kannst ihn mit jeder
NFC-App am Telefon ansehen oder selbst schreiben:

```
CMD:RESET
CMD:REBOOT
CMD:MENU
CMD:POWEROFF=0      sofort ausschalten
CMD:POWEROFF=8      nachfragen, 8 Sekunden Zeit
CMD:CPU=10          CPU auf 10 MHz
CMD:JOY             Joystickports umschalten
CMD:JOY=SWAPPED     Ports fest setzen; auch NORMAL, WASD1, WASD2
```

Groß- und Kleinschreibung sind egal. Dasselbe Format verstehen die M5Dial- und
die M5Stack-Core-Fassung, eine Karte läuft also an allen Geräten.

# Spiele direkt von der SD starten (ohne Karte)

Kachel **SD** öffnen. Du siehst die Dateien und Ordner deiner SD-Karte. Damit
kannst du auch ohne NFC-Karte etwas starten – praktisch zum Ausprobieren.

Im SD-Browser gilt zusätzlich:

| Geste | Funktion |
|---|---|
| **Antippen** | Datei starten bzw. Ordner öffnen |
| **Wischen** | schnell durch lange Listen blättern |
| **Lang antippen** | einen Ordner zurück |
| **Zurück** (unten links) | einen Ordner zurück, ganz oben zum Hauptbildschirm |
| `..` in der Liste | ebenfalls einen Ordner nach oben |

In der Titelzeile steht *lang tippen = ..* als Erinnerung.

# NFC-Karten beschreiben und verwalten

Alle Karten-Werkzeuge findest du unter **Setup** ganz oben. Setup öffnest du mit
der Kachel Setup; die ersten vier Einträge sind die NFC-Aktionen.

## NFC-Write: Karte mit einem Spiel belegen

1. Setup öffnen und **NFC-Write** antippen.
2. Auf der SD-Karte die gewünschte Spieldatei antippen.
3. Die NFC-Karte auflegen.
4. Der Pfad wird geschrieben und sofort zur Kontrolle zurückgelesen. *KARTE OK*
   heißt: erfolgreich.

Das Format ist kompatibel zum TeensyROM/Zaparoo-System. Eine so beschriebene
Karte funktioniert an beiden Geräten, solange die Ordnerstruktur auf beiden
SD-Karten gleich ist. Du kannst Karten auch mit dem Handy beschreiben (App
*NFC Tools*, Text-Record).

## NFC-Info: Karte auslesen

Zeigt alles, was auf der Karte steht: UID, Kartentyp, Speichergröße, den
gespeicherten Pfad und ob die Datei auf der SD-Karte tatsächlich vorhanden ist.
Ein Tipper blättert auf **Seite 2**, die technische Rohdaten,
Lock-Bits, Zugriffsschutz und den Lesezähler zeigt.

Praktisch: So findest du Karten, deren Spiel du umbenannt oder verschoben hast –
die Zeile *Datei* meldet dann „NICHT auf SD".

## NFC-Dump und NFC-Restore: Karten kopieren

- **NFC-Dump** sichert den kompletten Karteninhalt als Textdatei im Ordner
  `/NFC-DUMPS/` auf der SD-Karte. Funktioniert mit allen Karten, auch fremden
  MIFARE-Karten ohne Text.
- **NFC-Restore** schreibt so einen Dump wieder auf eine (leere) Karte.

Damit klonst du Karten. Ein Hinweis: Die Seriennummer (UID) einer Karte ist fest
im Chip und lässt sich nicht kopieren – der Inhalt aber vollständig. Für
Spielkarten reicht das, denn dort zählt der gespeicherte Pfad.

# WLAN einrichten

Alles dazu steckt unter **Setup → WLAN**. Einen Eintrag tippst du direkt an;
geblättert wird durch Wischen. Der CoreS3 merkt sich bis zu **vier Netze** und
probiert sie beim Verbinden nacheinander durch – praktisch, wenn du zwischen
Zuhause und einem Handy-Hotspot wechselst.

| Eintrag | Wirkung |
|---|---|
| **Netz suchen** | Umgebung durchsuchen und ein Netz aus der Liste wählen |
| **Von SD laden** | `wifi.txt` von der microSD einlesen |
| **Setup-Portal** | Eigener Accesspoint mit Weboberfläche |
| **Gespeichert** | Gespeichertes Netz auswählen und verbinden |
| **Auf NFC-Karte** | Gespeichertes Netz auf eine NFC-Karte schreiben |
| **Auf SD sichern** | Alle gespeicherten Netze als `wifi.txt` auf die microSD |
| **Netz löschen** | Einen einzelnen Eintrag entfernen |
| **Alle löschen** | Alle Zugangsdaten verwerfen |

## Weg 1: Netz suchen und Passwort per Karte

*Netz suchen* zeigt nach ein paar Sekunden die gefundenen Netze mit ihrer
Signalstärke. Ein bereits gespeichertes Netz ist mit *bekannt* markiert und
verbindet sich sofort, ein offenes Netz mit *offen* braucht kein Passwort.

Bei allen anderen fragt der CoreS3 nach dem Passwort und wartet auf eine
NFC-Karte. Auf der Karte steht ein ganz normaler Textrecord im selben Schema,
das auch WLAN-QR-Codes benutzen:

```
WIFI:S:MeinWLAN;T:WPA;P:MeinPasswort;;
```

Geschrieben wird die Karte mit dem Handy, zum Beispiel mit der App **NFC Tools**
(*Schreiben → Datensatz hinzufügen → Text*). Steht auf der Karte nur das
Passwort ohne das `WIFI:`-Präfix, wird es dem vorher gewählten Netz zugeordnet.

> Das Passwort steht unverschlüsselt auf der Karte. Nach dem Einrichten die
> Karte am besten wieder überschreiben – im Gerät liegt es danach ohnehin im
> internen Speicher.

## Karte auflegen genügt

Eine vollständige `WIFI:`-Karte wirkt auch **außerhalb** der Einrichtung: Legst
du sie auf dem Hauptbildschirm auf, speichert der CoreS3 das Netz und verbindet
sich sofort damit. Er wartet bis zu acht Sekunden und meldet dann *WLAN AKTIV*
mit der IP-Adresse oder *NETZ NICHT DA*, wenn das Netz nicht in Reichweite ist.
Ist das Netz schon verbunden, passiert nichts weiter (*SCHON VERBUNDEN*) – die
Karte darf also liegen bleiben.

So bekommt ein zweites Gerät seine Zugangsdaten in wenigen Sekunden.

## Weg 2: Datei auf der SD-Karte

Lege eine Textdatei `wifi.txt` in das Hauptverzeichnis der microSD:

```
ssid = MeinWLAN
pass = MeinWlanPasswort

ssid = Hotspot
pass = geheim123

host     = 192.168.0.64
hostpass =
```

Jede neue `ssid`-Zeile beginnt einen neuen Eintrag, Zeilen mit `#` sind
Kommentare. Gelesen wird die Datei beim Start (solange noch kein Netz
gespeichert ist) und jederzeit über *Von SD laden*.

Umgekehrt schreibt **Auf SD sichern** alle gespeicherten Netze samt `host` und
`hostpass` wieder in genau dieses Format heraus. Eine schon vorhandene
`wifi.txt` wird dabei nach `wifi.bak` umbenannt, es geht also nichts verloren.
Damit lässt sich ein zweites Gerät ohne Tipperei einrichten – die Passwörter
stehen dabei im Klartext auf der Karte.

## Weg 3: Setup-Portal

*Setup-Portal* macht aus dem CoreS3 für fünf Minuten einen eigenen Accesspoint.
Auf dem Display stehen Netzname, Passwort und die Adresse, die du im Browser
öffnest. Dort trägst du SSID und Passwort ein – wahlweise aus der Liste der
gefundenen Netze oder von Hand – und optional Adresse und Passwort des C64.

Nach dem Speichern schaltet der CoreS3 den Accesspoint ab und verbindet sich mit
dem neuen Netz. Dass die Browserverbindung dabei abbricht, ist normal.

# Einstellungen im Überblick

Im **Setup** findest du nach den NFC-Aktionen alle Einstellungen. Sie werden
dauerhaft gespeichert und nach dem Aus- und Einschalten wieder geladen.

## NFC und WLAN

| Einstellung | Auswahl |
|---|---|
| **NFC-Zufall** | Zufallskarte für ein Verzeichnis anlegen |
| **NFC-Cmd** | Befehlskarte anlegen (siehe Kapitel *Befehlskarten*) |
| **NFC-Cmd PowOff** | Vorgabe für die Abfragezeit einer PowerOff-Befehlskarte: 3, 5, 8, 15 s |
| **WLAN** | Untermenü der WLAN-Einrichtung (siehe Kapitel *WLAN einrichten*) |
| **Auto-NFC** | Abstand der Hintergrundabfrage: *Off*, *1.5s*, *0.7s*, *0.3s* |

## Ausschalten

| Einstellung | Auswahl |
|---|---|
| **PowerOff Zeit** | Wie lange die Ausschalt-Abfrage der POWER-Kachel gilt (0,5 bis 3,0 s) |

## Anzeige und Effekte

| Einstellung | Auswahl |
|---|---|
| **Animations** | Logo-Animationen ein/aus |
| **Effect** | Welcher Effekt: Auto (Wechsel), Static, Water, RotoZoom, SineWave, Ripple, Raster |
| **FX Detail** | Half oder Full – der CoreS3 schafft dank PSRAM auch Full flüssig |
| **Anim Speed** | Geschwindigkeit der Animationen: Slow / Normal / Fast |
| **Effect Time** | Wie lange ein Effekt läuft: Short / Normal / Long |
| **Static Time** | Wie lange das ruhige Logo dazwischen zu sehen ist |
| **Brightness** | Display-Helligkeit (32 bis 255) |
| **Status-LED** | Lampe an Port B ein- oder ausschalten |
| **LED Bright** | Helligkeit der Lampe (10 bis 255) |

## C64-Optionen

| Einstellung | Auswahl |
|---|---|
| **Disk Action** | Nach dem Einlegen eines Disk-Abbilds: nur einlegen (Mount), einlegen + Reset, oder einlegen + erstes Programm starten |
| **Disk Drive** | Ziellaufwerk: Auto (Bus 8), oder fest A / B |
| **Joystick** | Portbelegung im C64: *Normal*, *Swapped*, je nach Firmware *WASD P1* / *WASD P2* |

## Sonstiges

| Einstellung | Auswahl |
|---|---|
| **Beep** | Bestätigungston ein/aus |
| **Factory Reset** | Alle Einstellungen auf Werkseinstellung zurücksetzen |

# Häufige Fragen

**Der CoreS3 zeigt „NO WIFI".** Prüfe, ob WLAN-Name und Passwort korrekt
hinterlegt sind und der Router in Reichweite ist. Der CoreS3 versucht die
Verbindung alle paar Sekunden erneut.

**„AUTH?" in der Statusleiste.** Im C64 ist ein Netzwerkpasswort gesetzt, das im
CoreS3 nicht hinterlegt ist. Passwort im C64-Menü prüfen oder über das
Setup-Portal eintragen.

**Eine Karte startet nichts / „TYP UNBEKANNT".** Der Ultimate kann nur die oben
gelisteten Dateitypen starten. Prüfe mit **NFC-Info**, welcher Pfad und welche
Endung auf der Karte stehen und ob die Datei auf der SD liegt.

**Die SD-Karte wird nicht erkannt.** FAT32 formatieren, maximal 32 GB, und
notfalls eine andere Karte probieren. Der CoreS3 versucht automatisch zwei
Geschwindigkeiten.

**Das PowerOff-Kommando meldet einen Fehler.** Das ist normal – beim Ausschalten
antwortet der C64 oft nicht mehr sauber.

**Kann ich dieselben Karten am TeensyROM benutzen?** Ja. Das Kartenformat ist
identisch. Wichtig ist nur, dass die Ordner auf beiden SD-Karten gleich heißen.

# Lizenz

C64uRemote steht unter der **MIT-Lizenz**. Der vollständige Lizenztext liegt als
Datei `LICENSE` im Projektstamm.

Ursprung ist das Projekt **C64uRemote von Karl Prosser (@klumsy)**,
<https://github.com/ReadyOS-C64/C64uRemote>, das er unter der MIT-Lizenz
veröffentlicht hat. Diese Fassung ist eine daraus abgeleitete Erweiterung und
steht unter denselben Bedingungen:

* Copyright (c) 2026 Karl Prosser – Originalprojekt
* Copyright (c) 2026 Martin Oswald (@mad, <https://1MHz.de>) – Portierung und Erweiterungen

Die MIT-Lizenz erlaubt es, die Software zu benutzen, zu verändern und
weiterzugeben, auch kommerziell. Einzige Bedingung: **Copyright-Vermerk und
Lizenztext müssen erhalten bleiben** und jeder Kopie beiliegen. Eine
Gewährleistung oder Haftung ist ausgeschlossen.

Die eingebundenen Bibliotheken haben ihre eigenen Lizenzen: M5Unified und M5GFX
(MIT, © M5Stack), ArduinoJson (MIT, © Benoit Blanchon) sowie MFRC522_I2C
(<https://github.com/kkloesener/MFRC522_I2C>). Sie werden beim Bauen von
PlatformIO geladen.

# Entstehung

Portierung, Erweiterungen und Handbücher sind mit Unterstützung von Claude
(Anthropic) entstanden. Konzept, Idee, Hardware-Entscheidungen und sämtliche
Tests auf den echten Geräten: Martin Oswald (@mad).
