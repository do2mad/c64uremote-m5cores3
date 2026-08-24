// ============================================================================
//  C64uRemote  -  M5Stack CoreS3 / CoreS3 SE / CoreS3 Lite
//                 + Unit RFID2 (WS1850S) an Port A
// ----------------------------------------------------------------------------
//  Fernbedienung fuer den Commodore 64 Ultimate (c64u) / Ultimate64 Elite-II
//  ueber die ReST-API der Ultimate-Firmware (ab 3.11).
//
//  Basiert auf der Originalidee von Karl Prosser (@klumsy)
//      https://github.com/ReadyOS-C64/C64uRemote
//  sowie auf den Versionen fuer M5StickC Plus2, M5Dial und M5Stack Core von
//      Martin Oswald (@mad) - https://1MHz.de
//
//  Lizenz: MIT - siehe LICENSE im Projektstamm.
//    Copyright (c) 2026 Karl Prosser  (Originalprojekt C64uRemote,
//                                      https://github.com/ReadyOS-C64/C64uRemote)
//    Copyright (c) 2026 Martin Oswald (Portierung und Erweiterungen)
//
//  Diese Fassung bringt den vollen Funktionsumfang der Core-Version auf den
//  CoreS3 und stellt die Bedienung auf den kapazitiven Touchscreen um:
//
//    * Dashboard fuer 320x240: alle Kommandos direkt auf dem Hauptbildschirm
//    * REINE TOUCH-BEDIENUNG. Der CoreS3 hat keine Tasten A/B/C mehr, nur
//      POWER und RESET an der Seite. Kacheln und Listenzeilen werden direkt
//      angetippt, Listen folgen beim Wischen dem Finger, und rechts neben
//      langen Listen liegt eine ziehbare Scrollleiste. Die frei belegbaren
//      Tastenkuerzel der Core-Fassung sind ersatzlos entfallen - alle
//      Kommandos sind ohnehin Kacheln.
//    * Unit RFID2 (WS1850S, I2C 0x28) an Port A (G2 = SDA, G1 = SCL) -
//      optional
//        - Pfad einer Programmdatei auf eine NFC-Karte SCHREIBEN
//        - Pfad von der NFC-Karte LESEN, Datei von der microSD des CoreS3
//          holen und per ReST-API an den c64u senden + starten
//    * SD-Browser (Datei auch ohne Karte direkt starten)
//    * Streaming-Upload: auch grosse .d64 gehen in 1-kB-Bloecken durch
//    * WLAN-Einrichtung am Geraet (Setup > WLAN): bis zu vier Netze im NVS,
//      Netzsuche, Setup-Portal per eigenem Accesspoint, /wifi.txt auf der
//      SD-Karte laden und sichern, Zugangsdaten auf NFC-Karten. build_env.h
//      liefert nur noch die Startwerte beim allerersten Start.
//
//  Speicherhinweis: anders als der Core Basic hat der CoreS3 8 MB PSRAM.
//  Es wird deshalb ein Vollbild-Sprite (320x240x16 bit = 150 kB) im PSRAM
//  angelegt und in einem Rutsch ins Display geschoben - das ergibt
//  flimmerfreie Vollbilder und weiche Effekte. Klappt die Zuteilung nicht,
//  faellt der Code automatisch auf direktes Zeichnen zurueck.
// ============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <MFRC522_I2C.h>
#include <Adafruit_NeoPixel.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#if __has_include("build_env.h")
#include "build_env.h"
#else
#define C64U_WIFI_SSID ""
#define C64U_WIFI_PASSWORD ""
#define C64U_TARGET_HOST "192.168.0.64"
#define C64U_TARGET_PASSWORD ""
#endif

#include "1MHz_logo_rgb565.h"

namespace {

// ---------------------------------------------------------------------------
// Zeiten und Grenzwerte
// ---------------------------------------------------------------------------
constexpr uint32_t kModalMs            = 1500;
constexpr uint32_t kHttpTimeoutMs      = 3000;
constexpr uint32_t kWiFiRetryMs        = 10000;
constexpr uint32_t kConnectionProbeMs  = 15000;
// So lange wartet das Geraet nach einer aufgelegten WLAN-Karte auf die
// Verbindung, bevor es "Netz nicht da" meldet. Danach laeuft der Versuch
// ueber serviceWiFi ganz normal weiter.
constexpr uint32_t kWifiCardConnectMs  = 8000;
constexpr uint32_t kFrameMs            = 33;     // ~30 fps
constexpr uint32_t kStatusRefreshMs    = 1000;
constexpr uint8_t  kPowerOffConfirmDefDs = 7;   // 0,7 s (Zehntelsekunden)
constexpr uint32_t kRfidPollMs         = 250;

// ---------------------------------------------------------------------------
// Touch
//
// Der CoreS3 hat keine Tasten A/B/C mehr. Getippt wird direkt auf Kacheln,
// Listenzeilen und die Leiste am unteren Rand; lange Listen werden mit dem
// Finger geschoben oder ueber die Scrollleiste rechts gezogen.
// ---------------------------------------------------------------------------
// Weiter als so viele Pixel darf der Finger zwischen Auflegen und Abheben
// nicht wandern, damit es noch als Tipper zaehlt.
constexpr int      kTapSlopPx          = 10;
// Als Schieben gilt eine Bewegung, sobald die Liste tatsaechlich um mindestens
// eine Zeile gewandert ist - gerundet also ab einer halben Zeilenhoehe. Eine
// eigene Pixelschwelle gibt es bewusst nicht: sie wuerde nur eine Totzone
// erzeugen, in der weder geschoben noch getippt wird.
// Ein Tipper, der laenger dauert, zaehlt als langes Tippen (z. B. Ordner
// verlassen im SD-Browser).
constexpr uint32_t kLongTapMs          = 600;
// So lange bleibt eine automatisch geoeffnete Leseseite stehen, bevor sie
// zum Hauptbildschirm zurueckfaellt.
constexpr uint32_t kAutoRfidHoldMs     = 20000;
constexpr size_t   kMaxCpuChoices      = 16;
constexpr size_t   kMaxDirEntries      = 160;
constexpr size_t   kUploadChunk        = 1024;   // Bytes pro TCP-Write

// Mount-Modus fuer hochgeladene Disk-Images. "readwrite" ist die in der
// API-Doku dokumentierte Kombination fuer Uploads.
// Alternativen laut API: "unlinked" (beschreibbar, aber nichts wird
// zurueckgeschrieben) und "readonly" (schreibgeschuetzt).
constexpr const char* kMountMode = "readwrite";

// ---------------------------------------------------------------------------
// WLAN-Einrichtung
//
// Die Zugangsdaten stehen im NVS und lassen sich zur Laufzeit aendern -
// build_env.h liefert nur noch die Startwerte beim allerersten Start.
// Drei Wege fuehren zu neuen Zugangsdaten:
//   1. NFC-Karte mit einem NDEF-Text-Record
//   2. Datei /wifi.txt auf der SD-Karte
//   3. Setup-Portal: der CoreS3 macht einen eigenen Accesspoint auf
// Umgekehrt schreibt "Auf SD sichern" die gespeicherten Netze wieder als
// /wifi.txt heraus - so wandern sie ohne Tipperei auf das naechste Geraet.
// ---------------------------------------------------------------------------
constexpr size_t   kWifiProfileMax  = 4;        // gespeicherte Netze
constexpr size_t   kWifiScanMax     = 16;       // angezeigte Scan-Treffer
constexpr uint32_t kPortalIdleMs    = 300000;   // Portal nach 5 min beenden
constexpr uint32_t kPortalCloseMs   = 2500;     // Nachlauf nach dem Speichern

// Der Accesspoint des Setup-Portals. WPA2 verlangt mindestens acht Zeichen;
// das Passwort steht waehrend des Betriebs gross auf dem Display.
constexpr const char* kPortalSsid    = "C64uRemote-Setup";
constexpr const char* kPortalPass    = "c64ultimate";
constexpr uint8_t     kPortalDnsPort = 53;

// Konfigurationsdatei auf der SD-Karte
constexpr const char* kWifiFileSd    = "/wifi.txt";
constexpr const char* kWifiFileSdBak = "/wifi.bak";

// ---------------------------------------------------------------------------
// Hardware des CoreS3
//
// Port A (Grove, rot) ist der externe I2C-Bus und traegt das Unit RFID2.
// Achtung: die Belegung ist gegenueber dem Core Basic vertauscht -
// beim CoreS3 liegt SDA auf G2 und SCL auf G1.
//
// Die microSD haengt am selben SPI-Bus wie das Display (MOSI G37, SCK G36,
// MISO G35); nur die Chipselect-Leitung ist eigen (G4). SPI.begin() wird
// deshalb ausdruecklich mit diesen Pins aufgerufen, sonst landet der Bus auf
// der Vorgabebelegung des ESP32-S3 und die Karte wird nicht gefunden.
// ---------------------------------------------------------------------------
constexpr int     kI2cSdaPin   = 2;      // Port A, gelbe Ader
constexpr int     kI2cSclPin   = 1;      // Port A, weisse Ader
constexpr uint8_t kRfidAddr    = 0x28;
constexpr int     kSdSckPin    = 36;
constexpr int     kSdMisoPin   = 35;
constexpr int     kSdMosiPin   = 37;
constexpr int     kSdCsPin     = 4;

// ---------------------------------------------------------------------------
// Statuslampe an Port B
//
// Drei SK6812 (WS2812-kompatibel) in Reihe an der gelben Ader von Port B -
// die uebliche M5Stack Unit RGB.
// Das ist eine reine Ausgangsleitung - ob wirklich eine LED dranhaengt, laesst
// sich NICHT feststellen. Steckt nichts, gehen die Daten ins Leere; das ist
// folgenlos. Wer den Pin fuer etwas anderes braucht, schaltet die Lampe im
// Setup unter "Status-LED" ab.
// ---------------------------------------------------------------------------
constexpr int      kLedPin     = 9;        // Port B, gelbe Ader
constexpr uint16_t kLedCount   = 3;
// Abstand zweier Aktualisierungen. 20 ms reichen fuer ruhiges Pulsieren und
// halten die Buslast klein - drei LEDs sind in unter 100 us hinausgeschoben.
constexpr uint32_t kLedFrameMs = 20;
// Nachleuchten eines Ereignisblitzes (Karte gelesen, Fehler)
constexpr uint32_t kLedFlashMs = 600;

// ---------------------------------------------------------------------------
// Displaygeometrie (320 x 240, Rotation 1)
// ---------------------------------------------------------------------------
// Alle Masse sind auf den Finger ausgelegt, nicht auf Tasten: groessere
// Kacheln, hoehere Listenzeilen und eine deutlich hoehere Fussleiste, weil
// deren Felder (Zurueck, Blaettern) selbst Trefferflaechen sind.
constexpr int kScrW      = 320;
constexpr int kScrH      = 240;
constexpr int kBarH      = 20;                  // Statusleiste
constexpr int kLogoY     = kBarH;               // Logo-/Effektbereich
constexpr int kLogoH     = 108;
constexpr int kTileY0    = 132;
constexpr int kTileY1    = 168;
constexpr int kTileH     = 33;
constexpr int kTileW     = 58;
constexpr int kTileGap   = 6;
constexpr int kHintY     = 204;                 // Fussleiste 36 px hoch

// Ableitung fuer die hoehenrichtige Logo-Skalierung
constexpr int kLogoDrawW = (kLogoH * kLogoSrcW) / kLogoSrcH;
constexpr int kLogoOffX  = (kScrW - kLogoDrawW) / 2;

// ---------------------------------------------------------------------------
// Kommando-Kacheln auf dem Hauptbildschirm
// ---------------------------------------------------------------------------
enum TileId : uint8_t {
  kTileReset = 0,
  kTileReboot,
  kTileUltiMenu,
  kTilePowerOff,
  kTileCpu,
  kTileRfidRun,
  kTileSdBrowse,
  kTileStatus,
  kTileSetup,
  kTileCount
};

constexpr const char* kTileLabels[kTileCount] = {
    "RESET", "REBOOT", "MENU", "POWER", "CPU",
    "RFID", "SD", "STATUS", "SETUP",
};

// Die Reihenfolge der Einstellungen steckt an mehreren Stellen (Text, Wert,
// Aktion). Damit beim Einfuegen nichts verrutscht, gibt es dafuer Namen.
enum SettingsId : uint8_t {
  kSetNfcWrite = 0,
  kSetNfcInfo,
  kSetNfcDump,
  kSetNfcRestore,
  kSetNfcRandom,       // Zufallskarte fuer ein Verzeichnis
  kSetNfcCmd,          // letzte NFC-Aktion; danach kommen Schalter
  kSetCardConfirm,     // Abfragezeit der PowerOff-Befehlskarte (NFC-Cmd)
  kSetWifi,            // Aktion, wird in activateSetting() vorab behandelt
  kSetAutoNfc,
  kSetPowerOffTime,
  kSetAnimations,
  kSetEffect,
  kSetFxDetail,
  kSetAnimSpeed,
  kSetEffectTime,
  kSetStaticTime,
  kSetBrightness,
  kSetLed,
  kSetLedBright,
  kSetDiskAction,
  kSetDiskDrive,
  kSetBeep,
  kSetFactoryReset,
  kSetItemCount
};

// Bis hier (einschliesslich) sind es Aktionen, keine Schalter.
constexpr uint8_t kSetLastAction = kSetNfcCmd;

// NFC-Write steht bewusst an erster Stelle: nach "Setup" ist der Eintrag
// damit schon oben in der Liste und mit einem einzigen Tipper erreichbar.
constexpr const char* kSettingsItems[] = {
    "NFC-Write",
    "NFC-Info",
    "NFC-Dump",
    "NFC-Restore",
    "NFC-Zufall",
    "NFC-Cmd",
    "NFC-Cmd PowOff",
    "WLAN",
    "Auto-NFC",
    "PowerOff Zeit",
    "Animations",
    "Effect",
    "FX Detail",
    "Anim Speed",
    "Effect Time",
    "Static Time",
    "Brightness",
    "Status-LED",
    "LED Bright",
    "Disk Action",
    "Disk Drive",
    "Beep",
    "Factory Reset",
};
constexpr size_t kSettingsCount = sizeof(kSettingsItems) / sizeof(kSettingsItems[0]);
static_assert(kSettingsCount == static_cast<size_t>(kSetItemCount),
              "kSettingsItems und SettingsId sind aus dem Tritt geraten");

// Untermenue der WLAN-Einrichtung
enum WifiMenuId : uint8_t {
  kWifiScanNow = 0,
  kWifiFromSd,
  kWifiPortal,
  kWifiConnectSaved,
  kWifiToCard,
  kWifiToSd,
  kWifiDeleteOne,
  kWifiDeleteAll,
  kWifiMenuCount
};

constexpr const char* kWifiMenuItems[kWifiMenuCount] = {
    "Netz suchen",
    "Von SD laden",
    "Setup-Portal",
    "Gespeichert",
    "Auf NFC-Karte",
    "Auf SD sichern",
    "Netz loeschen",
    "Alle loeschen",
};

enum class ScreenMode : uint8_t {
  Home,
  CpuMenu,
  Status,
  Settings,
  SdBrowser,     // Datei auswaehlen (starten oder auf Karte schreiben)
  CmdPick,       // Befehl auswaehlen, der auf eine Karte geschrieben wird
  RfidRun,       // Karte auflegen -> Pfad lesen -> starten
  RfidWrite,     // Karte auflegen -> Pfad schreiben
  RfidInfo,      // Karte auflegen -> alle Infos anzeigen
  RfidDump,      // Karte auflegen -> Inhalt auf die SD sichern
  RfidRestore,   // Karte auflegen -> Dump von der SD zurueckschreiben
  WifiMenu,      // Untermenue der WLAN-Einrichtung
  WifiScan,      // Liste der gefundenen Netze
  WifiCard,      // Karte auflegen -> WLAN-Passwort lesen
  WifiPortal,    // Setup-Accesspoint laeuft
  WifiSaved,     // gespeicherte Netze: verbinden oder loeschen
  Busy,          // Upload laeuft, eigener Fortschrittsbildschirm
};

enum class HomeMode : uint8_t { Static, Water, RotoZoom, SineWave, Ripple, Raster };
enum class DisplayEffectMode : uint8_t { Auto, Static, Water, RotoZoom, SineWave, Ripple, Raster };
enum class AnimationSpeedMode : uint8_t { Slow, Normal, Fast };
enum class EffectDurationMode : uint8_t { Short, Normal, Long };
enum class StaticDurationMode : uint8_t { Short, Normal, Long };
enum class FxDetailMode : uint8_t { Half, Full };
enum class DiskActionMode : uint8_t { Mount, MountReset, MountRun };
enum class UploadDriveMode : uint8_t { AutoBus8, DriveA, DriveB };
// Hintergrundabfrage des RFID2 auf dem Hauptbildschirm
enum class AutoNfcMode : uint8_t { Off, Slow, Normal, Fast };

// Befehle, die statt eines Dateipfads auf einer NFC-Karte stehen koennen.
// Auf der Karte steht dann z. B. "CMD:RESET" oder "CMD:CPU=10".
enum class CardCmd : uint8_t {
  None,
  Reset,
  Reboot,
  UltiMenu,
  PowerOff,        // Argument = Bestaetigungszeit in Sekunden, 0 = sofort
  CpuSpeed,        // Argument = gewuenschter Wert, z. B. "10"
};

struct CardCommand {
  CardCmd cmd    = CardCmd::None;
  String  arg;                 // PowerOff: Sekunden, CpuSpeed: MHz
  bool    hasArg = false;
};

// ---------------------------------------------------------------------------
// Datenstrukturen
// ---------------------------------------------------------------------------
struct ApiResponse {
  bool   transportOk = false;
  bool   jsonOk      = false;
  bool   apiOk       = false;
  int    httpCode    = -1;
  String body;
  String errors;
};

struct ConnectionState {
  bool   wifiConnected   = false;
  bool   targetReachable = false;
  bool   authOk          = false;
  String detail          = "Not tested";
};

struct SettingsState {
  bool               animationsEnabled = true;
  DisplayEffectMode  effectMode        = DisplayEffectMode::Auto;
  FxDetailMode       fxDetail          = FxDetailMode::Half;
  AnimationSpeedMode animationSpeed    = AnimationSpeedMode::Normal;
  EffectDurationMode effectDuration    = EffectDurationMode::Normal;
  StaticDurationMode staticDuration    = StaticDurationMode::Normal;
  uint8_t            brightness        = 160;
  DiskActionMode     diskAction        = DiskActionMode::MountRun;
  UploadDriveMode    uploadDrive       = UploadDriveMode::AutoBus8;
  bool               beepEnabled       = true;
  AutoNfcMode        autoNfc           = AutoNfcMode::Normal;
  uint8_t            powerOffConfirmDs = kPowerOffConfirmDefDs;  // Zeitfenster Abfrage
  // Wie lange nach einem PowerOff-Kartenbefehl auf die Bestaetigung
  // gewartet wird (Sekunden). Die Karte muss so lange wieder aufgelegt
  // oder der Bildschirm angetippt werden.
  uint8_t            cardConfirmS      = 8;
  // Statuslampe an Port B (drei SK6812). Steckt nichts, bleibt das Ansteuern
  // folgenlos - wer den Pin anders braucht, schaltet sie hier ab.
  bool               ledEnabled        = true;
  uint8_t            ledBrightness     = 60;   // 0..255, gedaempft reicht voellig
};

struct HomeDemoState {
  HomeMode mode            = HomeMode::Static;
  uint8_t  nextEffectIndex = 0;
  uint32_t startedAtMs     = 0;
  bool     dirty           = true;
};

struct DirEntryInfo {
  String name;
  bool   isDir = false;
};

// Ein gespeichertes WLAN. Das Passwort bleibt im NVS und verlaesst das
// Geraet nur ueber das Setup-Portal (dort maskiert).
struct WifiProfile {
  String ssid;
  String pass;
};

// Ein Treffer aus dem Netzsuchlauf
struct WifiScanEntry {
  String  ssid;
  int32_t rssi = 0;
  bool    open = false;
};

struct AppState {
  ScreenMode screen        = ScreenMode::Home;
  ScreenMode returnScreen  = ScreenMode::Home;   // wohin nach Busy/RFID
  int        tileIndex     = 0;
  int        cpuIndex      = 0;
  int        settingsIndex = 0;

  // ---- c64u ----
  String cpuCategory;
  String cpuItem;
  String currentCpuValue = "Unknown";
  bool   cpuPathKnown    = false;
  String cpuWireOptions[kMaxCpuChoices];
  String cpuDisplayOptions[kMaxCpuChoices];
  size_t cpuChoiceCount = 0;

  bool            configReady = false;
  ConnectionState connection  = {};
  SettingsState   settings    = {};
  HomeDemoState   home        = {};

  // ---- WLAN-Einrichtung ----
  int      wifiMenuIndex   = 0;
  int      wifiScanIndex   = 0;
  int      wifiSavedIndex  = 0;
  size_t   wifiScanCount   = 0;
  bool     wifiSavedDelete = false;   // Liste im Loeschmodus
  bool     wifiSavedToCard = false;   // Liste schreibt das Netz auf eine Karte
  String   wifiPendingSsid;           // gewaehltes Netz, wartet auf Passwort
  String   wifiHint = "Karte auflegen...";
  bool     portalActive    = false;
  uint32_t portalTouchedMs = 0;       // letzter Zugriff (Leerlauf-Abschaltung)
  uint32_t portalCloseAtMs = 0;       // 0 = kein Abschaltwunsch

  // ---- Overlay ----
  String   modalText;
  uint16_t modalColor   = TFT_WHITE;
  uint32_t modalUntilMs = 0;
  bool     modalVisibleLast = false;

  // ---- Timing ----
  uint32_t lastWiFiAttemptMs     = 0;
  uint32_t lastConnectionProbeMs = 0;
  uint32_t lastStatusDrawMs      = 0;
  uint32_t lastRfidPollMs        = 0;

  bool     pendingPowerOff   = false;      // Kachel: zweites B bestaetigt
  uint32_t pendingPowerOffAtMs = 0;

  // ---- Redraw-Flags ----
  bool barDirty    = true;
  bool screenDirty = true;

  // ---- SD ----
  bool         sdReady = false;
  String       sdPath  = "/";
  DirEntryInfo sdEntries[kMaxDirEntries];
  size_t       sdCount = 0;
  int          sdIndex = 0;
  uint8_t      sdPickMode = 0;          // 0 starten, 1 auf Karte schreiben, 2 Dump zurueckspielen

  // ---- RFID ----
  bool   rfidReady   = false;
  String pendingPath;                   // Pfad, der auf die Karte soll
  String pendingCardText;               // fertiger Kartentext (Befehlskarten)
  int    cmdIndex = 0;                  // Auswahl im Befehlsmenue

  // Offene Rueckfrage eines PowerOff-Kartenbefehls
  bool     cardPowerOffPending = false;
  String   cardPowerOffUid;
  uint32_t cardPowerOffUntilMs = 0;
  String pendingDump;                   // Dump-Datei, die zurueckgeschrieben wird
  String lastCardPath;                  // zuletzt von Karte gelesener Pfad
  String rfidHint = "Karte auflegen...";
  // true, wenn die Leseseite von der Hintergrundabfrage geoeffnet wurde.
  // Sie faellt dann nach kAutoRfidHoldMs von selbst zum Hauptbildschirm zurueck.
  bool     autoRfidActive  = false;
  // Bildschirm, von dem aus die Hintergrundabfrage die Leseseite geoeffnet
  // hat. Dorthin geht es danach zurueck - wer im SD-Browser blaettert, will
  // nicht auf dem Hauptbildschirm landen.
  ScreenMode autoRfidFrom = ScreenMode::Home;
  // Zustand des SD-Browsers beim Verlassen. Eine Verzeichnis- oder
  // Zufallskarte laesst resolveRandomFile() ein anderes Verzeichnis einlesen
  // und baut dabei app.sdPath/sdEntries um - ohne diese Sicherung kaeme der
  // Benutzer in einem fremden Verzeichnis heraus.
  String     autoRfidSdPath;
  int        autoRfidSdIndex   = 0;
  int        autoRfidListStart = 0;
  uint32_t autoRfidUntilMs = 0;

  static constexpr size_t kMaxInfoLines = 16;
  String infoLines[kMaxInfoLines];     // Seite 1: Zusammenfassung
  size_t infoCount = 0;
  String infoLines2[kMaxInfoLines];    // Seite 2: Rohdaten und Technik
  size_t infoCount2 = 0;
  uint8_t infoPage = 0;
  String infoUid;                      // UID der bereits eingelesenen Karte

  // ---- Upload ----
  String   busyTitle;
  String   busyDetail;
  size_t   busySent  = 0;
  size_t   busyTotal = 0;

  // ---- Statuslampe ----
  uint32_t ledNextMs    = 0;      // naechste Aktualisierung
  uint32_t ledFlashMs   = 0;      // Ereignisblitz laeuft bis hierhin
  uint32_t ledFlashRgb  = 0;      // Farbe des Blitzes
  uint32_t ledLastRgb[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};  // zuletzt gesetzt
  uint8_t  ledLastBright = 0xFF;
} app;

Preferences  prefs;
MFRC522_I2C  rfid(kRfidAddr, -1);
MFRC522_I2C::MIFARE_Key rfidKey;

// Statuslampe an Port B. NEO_GRB ist die Reihenfolge der SK6812 in der
// M5Stack Unit RGB; WS2812 verhalten sich identisch.
Adafruit_NeoPixel gLed(kLedCount, kLedPin, NEO_GRB + NEO_KHZ800);

// Offscreen-Puffer im PSRAM. Klappt das Anlegen nicht, wird direkt ins
// Display gezeichnet - dann zeigt gDraw auf M5.Display und pushFrame() ist
// ein Leerlauf.
M5Canvas         canvas(&M5.Display);
lgfx::LovyanGFX* gDraw      = nullptr;
bool             gUseCanvas = false;

uint16_t rowBuf[kScrW] = {};
int16_t  logoXMap[kScrW] = {};
int16_t  logoYMap[kLogoH] = {};

// ---------------------------------------------------------------------------
// Kleine Helfer
// ---------------------------------------------------------------------------
String trimCopy(const String& value) { String r = value; r.trim(); return r; }
String configString(const char* v)   { return trimCopy(v == nullptr ? "" : v); }

// ---------------------------------------------------------------------------
// Netzkonfiguration zur Laufzeit
//
// Frueher kamen SSID, Passwort und Zieladresse fest aus build_env.h. Jetzt
// stehen sie im NVS und build_env.h liefert nur noch die Startwerte, solange
// nichts gespeichert ist. Damit muss fuer ein neues WLAN nichts mehr neu
// uebersetzt werden.
// ---------------------------------------------------------------------------
WifiProfile gWifiProfiles[kWifiProfileMax];
size_t      gWifiCount = 0;
size_t      gWifiTry   = 0;      // Profil fuer den naechsten Verbindungsversuch
String      gTargetHost;
String      gTargetPass;

WifiScanEntry gWifiScan[kWifiScanMax];

// Werte aus build_env.h (nur Startwerte)
const String& buildWifiSsid()  { static const String v = configString(C64U_WIFI_SSID); return v; }
const String& buildWifiPass()  { static const String v = configString(C64U_WIFI_PASSWORD); return v; }
const String& buildHost()      { static const String v = configString(C64U_TARGET_HOST); return v; }
const String& buildHostPass()  { static const String v = configString(C64U_TARGET_PASSWORD); return v; }

const String& targetHost()     { return gTargetHost; }
const String& targetPassword() { return gTargetPass; }

bool hasWiFiConfig()   { return gWifiCount > 0; }
bool hasTargetConfig() { return !gTargetHost.isEmpty(); }
bool configReady()     { return hasWiFiConfig() && hasTargetConfig(); }

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

uint16_t blend565(uint16_t from, uint16_t to, float t) {
  t = std::max(0.0f, std::min(1.0f, t));
  const uint8_t fr = ((from >> 11) & 0x1Fu) << 3;
  const uint8_t fg = ((from >> 5) & 0x3Fu) << 2;
  const uint8_t fb = (from & 0x1Fu) << 3;
  const uint8_t tr = ((to >> 11) & 0x1Fu) << 3;
  const uint8_t tg = ((to >> 5) & 0x3Fu) << 2;
  const uint8_t tb = (to & 0x1Fu) << 3;
  return rgb565(static_cast<uint8_t>(fr + (tr - fr) * t),
                static_cast<uint8_t>(fg + (tg - fg) * t),
                static_cast<uint8_t>(fb + (tb - fb) * t));
}

// Farbpalette
const uint16_t kColBg      = rgb565(8, 14, 30);
const uint16_t kColPanel   = rgb565(16, 25, 42);
const uint16_t kColPanelHi = rgb565(48, 94, 164);
const uint16_t kColLine    = rgb565(66, 96, 132);
const uint16_t kColLineHi  = rgb565(184, 228, 255);
const uint16_t kColText    = rgb565(212, 226, 248);
const uint16_t kColLabel   = rgb565(156, 190, 228);
const uint16_t kColOk      = rgb565(110, 230, 170);
const uint16_t kColWarn    = rgb565(255, 190, 84);
const uint16_t kColErr     = rgb565(255, 120, 96);
const uint16_t kColInfo    = rgb565(120, 220, 255);

void beep(uint16_t freq = 2000, uint32_t ms = 40) {
  if (!app.settings.beepEnabled) return;
  M5.Speaker.tone(freq, ms);
}

void setModal(const String& text, uint16_t color, uint32_t now, uint32_t durationMs = kModalMs) {
  app.modalText    = text;
  app.modalColor   = color;
  app.modalUntilMs = now + durationMs;
  app.home.dirty   = true;
  app.screenDirty  = true;
}

bool modalVisible(uint32_t now) {
  return !app.modalText.isEmpty() && now <= app.modalUntilMs;
}

// Vorwaertsdeklarationen
void drawBusyScreen();
void pushFrame();
void openSdBrowser(uint8_t forCard, uint32_t now);
void drawTiles();
void setScreen(ScreenMode next, uint32_t now);
void refreshCpuValue();
void beginWiFi(uint32_t now);
ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated);

// ---------------------------------------------------------------------------
// Labels fuer die Einstellungen
// ---------------------------------------------------------------------------
const char* effectLabel(DisplayEffectMode m) {
  switch (m) {
    case DisplayEffectMode::Auto:     return "Auto";
    case DisplayEffectMode::Static:   return "Static";
    case DisplayEffectMode::Water:    return "Water";
    case DisplayEffectMode::RotoZoom: return "RotoZoom";
    case DisplayEffectMode::SineWave: return "SineWave";
    case DisplayEffectMode::Ripple:   return "Ripple";
    case DisplayEffectMode::Raster:   return "Raster";
  }
  return "Auto";
}
const char* fxDetailLabel(FxDetailMode m) { return m == FxDetailMode::Full ? "Full" : "Half"; }
const char* animationSpeedLabel(AnimationSpeedMode m) {
  switch (m) {
    case AnimationSpeedMode::Slow:   return "Slow";
    case AnimationSpeedMode::Fast:   return "Fast";
    case AnimationSpeedMode::Normal: return "Normal";
  }
  return "Normal";
}
const char* effectDurationLabel(EffectDurationMode m) {
  switch (m) {
    case EffectDurationMode::Short: return "Short";
    case EffectDurationMode::Long:  return "Long";
    case EffectDurationMode::Normal:return "Normal";
  }
  return "Normal";
}
const char* staticDurationLabel(StaticDurationMode m) {
  switch (m) {
    case StaticDurationMode::Short: return "Short";
    case StaticDurationMode::Long:  return "Long";
    case StaticDurationMode::Normal:return "Normal";
  }
  return "Normal";
}
const char* diskActionLabel(DiskActionMode m) {
  switch (m) {
    case DiskActionMode::Mount:      return "Mount";
    case DiskActionMode::MountReset: return "Mnt+Reset";
    case DiskActionMode::MountRun:   return "Mnt+Run";
  }
  return "Mnt+Run";
}
// Waehlbare Zeitfenster in Zehntelsekunden: 0,5 bis 3,0 s
constexpr uint8_t kTimeSteps[] = {5, 7, 10, 15, 20, 25, 30};
constexpr size_t  kTimeStepCount = sizeof(kTimeSteps) / sizeof(kTimeSteps[0]);

uint8_t nextTimeStep(uint8_t current) {
  for (size_t i = 0; i < kTimeStepCount; ++i) {
    if (current < kTimeSteps[i]) return kTimeSteps[i];
    if (current == kTimeSteps[i]) return kTimeSteps[(i + 1) % kTimeStepCount];
  }
  return kTimeSteps[0];
}

String timeStepLabel(uint8_t ds) {
  return String(ds / 10) + "." + String(ds % 10) + "s";
}

// Bestaetigungsfenster fuer PowerOff-Kartenbefehle. Deutlich groesser als die
// Abfrage am Bildschirm, weil die Karte erst weggenommen und wieder aufgelegt
// wird.
constexpr uint8_t kCardConfirmSteps[] = {3, 5, 8, 15};
constexpr size_t  kCardConfirmCount = sizeof(kCardConfirmSteps) / sizeof(kCardConfirmSteps[0]);

uint8_t nextCardConfirm(uint8_t current) {
  for (size_t i = 0; i < kCardConfirmCount; ++i) {
    if (current < kCardConfirmSteps[i]) return kCardConfirmSteps[i];
    if (current == kCardConfirmSteps[i]) return kCardConfirmSteps[(i + 1) % kCardConfirmCount];
  }
  return kCardConfirmSteps[0];
}
String cardConfirmLabel(uint8_t s) { return String(s) + "s"; }

uint32_t powerOffConfirmMs() { return app.settings.powerOffConfirmDs * 100u; }
const char* autoNfcLabel(AutoNfcMode m) {
  switch (m) {
    case AutoNfcMode::Off:  return "Off";
    case AutoNfcMode::Slow: return "1.5s";
    case AutoNfcMode::Fast: return "0.3s";
    default:                return "0.7s";
  }
}
// Abstand zwischen zwei Hintergrundabfragen des Lesers
uint32_t autoNfcIntervalMs(AutoNfcMode m) {
  switch (m) {
    case AutoNfcMode::Off:  return 0;
    case AutoNfcMode::Slow: return 1500;
    case AutoNfcMode::Fast: return 300;
    default:                return 700;
  }
}

const char* uploadDriveLabel(UploadDriveMode m) {
  switch (m) {
    case UploadDriveMode::DriveA: return "A fest";
    case UploadDriveMode::DriveB: return "B fest";
    default:                      return "Auto (8)";
  }
}

float animationSpeedFactor(AnimationSpeedMode m) {
  switch (m) {
    case AnimationSpeedMode::Slow: return 0.65f;
    case AnimationSpeedMode::Fast: return 1.45f;
    default: return 1.0f;
  }
}
float effectDurationFactor(EffectDurationMode m) {
  switch (m) {
    case EffectDurationMode::Short: return 0.60f;
    case EffectDurationMode::Long:  return 1.60f;
    default: return 1.0f;
  }
}
float staticDurationFactor(StaticDurationMode m) {
  switch (m) {
    case StaticDurationMode::Short: return 0.60f;
    case StaticDurationMode::Long:  return 3.20f;
    default: return 1.0f;
  }
}

// Die Lampe blendet schon bei kleinen Werten kraeftig, deshalb feinere
// Abstufung im unteren Bereich als beim Display.
uint8_t nextLedBrightness(uint8_t current) {
  static const uint8_t levels[] = {10, 20, 40, 60, 90, 130, 180, 255};
  const size_t count = sizeof(levels) / sizeof(levels[0]);
  for (size_t i = 0; i < count; ++i) {
    if (current < levels[i]) return levels[i];
    if (current == levels[i]) return levels[(i + 1) % count];
  }
  return levels[0];
}

uint8_t nextBrightnessValue(uint8_t current) {
  static const uint8_t levels[] = {32, 64, 96, 128, 160, 192, 224, 255};
  const size_t count = sizeof(levels) / sizeof(levels[0]);
  for (size_t i = 0; i < count; ++i) {
    if (current < levels[i]) return levels[i];
    if (current == levels[i]) return levels[(i + 1) % count];
  }
  return levels[0];
}

void applyBrightness() { M5.Display.setBrightness(app.settings.brightness); }

// ---------------------------------------------------------------------------
// Einstellungen laden / speichern (NVS)
// ---------------------------------------------------------------------------
void loadDefaultSettings() { app.settings = SettingsState(); }

void saveSettings() {
  prefs.begin("c64uremote", false);
  prefs.putBool ("anim_on",  app.settings.animationsEnabled);
  prefs.putUChar("fx_mode",  static_cast<uint8_t>(app.settings.effectMode));
  prefs.putUChar("fx_det",   static_cast<uint8_t>(app.settings.fxDetail));
  prefs.putUChar("anim_spd", static_cast<uint8_t>(app.settings.animationSpeed));
  prefs.putUChar("fx_time",  static_cast<uint8_t>(app.settings.effectDuration));
  prefs.putUChar("st_time",  static_cast<uint8_t>(app.settings.staticDuration));
  prefs.putUChar("bright",   app.settings.brightness);
  prefs.putUChar("disk_act", static_cast<uint8_t>(app.settings.diskAction));
  prefs.putUChar("updrive",  static_cast<uint8_t>(app.settings.uploadDrive));
  prefs.putBool ("beep",     app.settings.beepEnabled);
  prefs.putUChar("auto_nfc", static_cast<uint8_t>(app.settings.autoNfc));
  prefs.putUChar("po_time",  app.settings.powerOffConfirmDs);
  prefs.putUChar("card_cnf", app.settings.cardConfirmS);
  prefs.putBool ("led_on",   app.settings.ledEnabled);
  prefs.putUChar("led_br",   app.settings.ledBrightness);
  prefs.end();
}

void loadSettings() {
  loadDefaultSettings();
  prefs.begin("c64uremote", true);
  app.settings.animationsEnabled = prefs.getBool("anim_on", app.settings.animationsEnabled);
  const uint8_t fxMode   = prefs.getUChar("fx_mode",  static_cast<uint8_t>(app.settings.effectMode));
  const uint8_t fxDet    = prefs.getUChar("fx_det",   static_cast<uint8_t>(app.settings.fxDetail));
  const uint8_t animSpd  = prefs.getUChar("anim_spd", static_cast<uint8_t>(app.settings.animationSpeed));
  const uint8_t fxTime   = prefs.getUChar("fx_time",  static_cast<uint8_t>(app.settings.effectDuration));
  const uint8_t stTime   = prefs.getUChar("st_time",  static_cast<uint8_t>(app.settings.staticDuration));
  const uint8_t bright   = prefs.getUChar("bright",   app.settings.brightness);
  const uint8_t diskAct  = prefs.getUChar("disk_act", static_cast<uint8_t>(app.settings.diskAction));
  const uint8_t upDrive  = prefs.getUChar("updrive",  static_cast<uint8_t>(app.settings.uploadDrive));
  app.settings.beepEnabled = prefs.getBool("beep", app.settings.beepEnabled);
  const uint8_t autoNfc    = prefs.getUChar("auto_nfc", static_cast<uint8_t>(app.settings.autoNfc));
  app.settings.powerOffConfirmDs = prefs.getUChar("po_time", app.settings.powerOffConfirmDs);
  app.settings.cardConfirmS      = prefs.getUChar("card_cnf", app.settings.cardConfirmS);
  app.settings.ledEnabled        = prefs.getBool ("led_on", app.settings.ledEnabled);
  const uint8_t ledBr            = prefs.getUChar("led_br", app.settings.ledBrightness);
  prefs.end();

  if (fxMode  <= static_cast<uint8_t>(DisplayEffectMode::Raster))   app.settings.effectMode     = static_cast<DisplayEffectMode>(fxMode);
  if (fxDet   <= static_cast<uint8_t>(FxDetailMode::Full))          app.settings.fxDetail       = static_cast<FxDetailMode>(fxDet);
  if (animSpd <= static_cast<uint8_t>(AnimationSpeedMode::Fast))    app.settings.animationSpeed = static_cast<AnimationSpeedMode>(animSpd);
  if (fxTime  <= static_cast<uint8_t>(EffectDurationMode::Long))    app.settings.effectDuration = static_cast<EffectDurationMode>(fxTime);
  if (stTime  <= static_cast<uint8_t>(StaticDurationMode::Long))    app.settings.staticDuration = static_cast<StaticDurationMode>(stTime);
  if (diskAct <= static_cast<uint8_t>(DiskActionMode::MountRun))    app.settings.diskAction     = static_cast<DiskActionMode>(diskAct);
  if (upDrive <= static_cast<uint8_t>(UploadDriveMode::DriveB))     app.settings.uploadDrive    = static_cast<UploadDriveMode>(upDrive);
  if (autoNfc <= static_cast<uint8_t>(AutoNfcMode::Fast))           app.settings.autoNfc        = static_cast<AutoNfcMode>(autoNfc);
  app.settings.brightness    = std::max<uint8_t>(32, bright);
  app.settings.ledBrightness = std::max<uint8_t>(10, ledBr);

  // Zeitfenster auf den waehlbaren Bereich begrenzen (0,5 bis 3,0 s)
  auto clampTime = [](uint8_t v, uint8_t fallback) -> uint8_t {
    return (v >= kTimeSteps[0] && v <= kTimeSteps[kTimeStepCount - 1]) ? v : fallback;
  };
  app.settings.powerOffConfirmDs = clampTime(app.settings.powerOffConfirmDs, kPowerOffConfirmDefDs);
  if (app.settings.cardConfirmS < kCardConfirmSteps[0] ||
      app.settings.cardConfirmS > kCardConfirmSteps[kCardConfirmCount - 1]) {
    app.settings.cardConfirmS = 8;
  }
}

// ---------------------------------------------------------------------------
// Logo-Sampling direkt aus dem Flash (kein RAM-Cache!)
// ---------------------------------------------------------------------------
void buildLogoMaps() {
  for (int x = 0; x < kScrW; ++x) {
    const int rel = x - kLogoOffX;
    if (rel < 0 || rel >= kLogoDrawW) {
      logoXMap[x] = -1;                       // ausserhalb -> Hintergrund
    } else {
      logoXMap[x] = static_cast<int16_t>(std::min(kLogoSrcW - 1, (rel * kLogoSrcW) / kLogoDrawW));
    }
  }
  for (int y = 0; y < kLogoH; ++y) {
    logoYMap[y] = static_cast<int16_t>(std::min(kLogoSrcH - 1, (y * kLogoSrcH) / kLogoH));
  }
}

// Hintergrund links und rechts vom hoehenrichtig skalierten Logo.
// Das Logo selbst hat einen schwarzen Grund, deshalb hier ebenfalls schwarz.
constexpr uint16_t kLogoBg = TFT_BLACK;

// x/y sind Koordinaten INNERHALB des Logobereichs (0..319 / 0..kLogoH-1)
inline uint16_t logoPixel(int x, int y) {
  if (x < 0) x = 0;
  if (x >= kScrW) x = kScrW - 1;
  if (y < 0) y = 0;
  if (y >= kLogoH) y = kLogoH - 1;
  const int sx = logoXMap[x];
  if (sx < 0) return kLogoBg;
  return pgm_read_word(&commodore_logo_rgb565[logoYMap[y] * kLogoSrcW + sx]);
}

inline int wrapCoord(int value, int limit) {
  if (limit <= 0) return 0;
  value %= limit;
  if (value < 0) value += limit;
  return value;
}

// Schiebt eine fertige Bildzeile ins Display. Bei "Half"-Detail werden die
// berechneten Pixel horizontal verdoppelt und die Zeile zweimal ausgegeben.
inline void pushLogoRow(int y, int step) {
  if (step > 1) {
    for (int x = 0; x < kScrW; x += step) {
      const uint16_t c = rowBuf[x];
      for (int k = 1; k < step && x + k < kScrW; ++k) rowBuf[x + k] = c;
    }
  }
  for (int k = 0; k < step && y + k < kLogoH; ++k) {
    gDraw->pushImage(0, kLogoY + y + k, kScrW, 1, rowBuf);
  }
}

int fxStep() { return app.settings.fxDetail == FxDetailMode::Full ? 1 : 2; }

void drawStaticLogo() {
  gDraw->startWrite();
  for (int y = 0; y < kLogoH; ++y) {
    for (int x = 0; x < kScrW; ++x) rowBuf[x] = logoPixel(x, y);
    gDraw->pushImage(0, kLogoY + y, kScrW, 1, rowBuf);
  }
  gDraw->endWrite();
}

// Wasser- / Sinus-Verzerrung (zeilenweise Verschiebung)
void drawDistortedRows(uint32_t tickMs, bool waterMode) {
  const float t    = static_cast<float>(tickMs) * 0.001f;
  const int   step = fxStep();
  const float cx   = kScrW * 0.5f;

  gDraw->startWrite();
  for (int y = 0; y < kLogoH; y += step) {
    const float baseWave   = sinf(y * 0.10f + t * (waterMode ? 8.2f : 3.2f));
    const float secondWave = sinf(y * 0.035f - t * (waterMode ? 4.9f : 2.1f));
    const float xOffset    = waterMode ? (baseWave * 13.0f + secondWave * 7.0f)
                                       : (baseWave * 9.0f + secondWave * 11.0f);
    const float yOffset    = waterMode ? (secondWave * 3.5f) : (baseWave * 3.5f);
    const float shimmer    = waterMode ? std::max(0.0f, sinf(y * 0.07f + t * 10.8f)) : 0.0f;
    const int   srcY       = static_cast<int>(y + yOffset);

    for (int x = 0; x < kScrW; x += step) {
      const int sx = static_cast<int>(cx + (static_cast<float>(x) - cx) + xOffset);
      uint16_t color = logoPixel(sx, srcY);
      if (waterMode) {
        color = blend565(color, rgb565(180, 240, 255), shimmer * 0.28f);
      } else {
        color = blend565(color, rgb565(82, 180, 255), 0.08f);
      }
      rowBuf[x] = color;
    }
    pushLogoRow(y, step);
  }
  gDraw->endWrite();
}

void drawRotoZoom(uint32_t tickMs) {
  const float t     = static_cast<float>(tickMs) * 0.001f;
  const int   step  = fxStep();
  const float srcCx = kScrW * 0.5f;
  const float srcCy = kLogoH * 0.5f;
  const float angle = t * 1.8f;
  const float zoom  = 1.33f + 0.65f * sinf(t * 1.25f);
  const float cs    = cosf(angle) / zoom;
  const float sn    = sinf(angle) / zoom;

  gDraw->startWrite();
  for (int y = 0; y < kLogoH; y += step) {
    const float py = static_cast<float>(y) - srcCy;
    for (int x = 0; x < kScrW; x += step) {
      const float px = static_cast<float>(x) - srcCx;
      const int sx = wrapCoord(static_cast<int>(srcCx + px * cs - py * sn), kScrW);
      const int sy = wrapCoord(static_cast<int>(srcCy + px * sn + py * cs), kLogoH);
      rowBuf[x] = logoPixel(sx, sy);
    }
    pushLogoRow(y, step);
  }
  gDraw->endWrite();
}

void drawRipple(uint32_t tickMs) {
  const float t    = static_cast<float>(tickMs) * 0.001f;
  const int   step = fxStep();
  const float cx   = kScrW * 0.5f;
  const float cy   = kLogoH * 0.5f;
  const float lightX = -0.58f;
  const float lightY = -0.42f;
  const float maxRadius = sqrtf(static_cast<float>(kScrW * kScrW + kLogoH * kLogoH)) * 0.5f;
  const float travel = t * 118.0f;
  const float span   = maxRadius * 2.0f;
  const float waveFreq = 0.17f;
  const float timeFreq = 15.8f;
  const float displacementScale = 26.0f;
  const float sourceX[3] = {cx, cx - 54.0f, cx + 46.0f};
  const float sourceY[3] = {cy, cy + 30.0f, cy - 26.0f};
  const float sourceWeight[3] = {1.0f, 0.46f, 0.38f};

  gDraw->startWrite();
  for (int y = 0; y < kLogoH; y += step) {
    const float fy = static_cast<float>(y);
    for (int x = 0; x < kScrW; x += step) {
      const float fx = static_cast<float>(x);
      float gradX = 0.0f, gradY = 0.0f, waveMix = 0.0f;

      for (int i = 0; i < 3; ++i) {
        const float dx = fx - sourceX[i];
        const float dy = fy - sourceY[i];
        const float radius = sqrtf(dx * dx + dy * dy) + 0.0001f;

        float reflected = fmodf(radius + travel * (0.94f + 0.08f * i), span);
        float bounceDir = 1.0f;
        if (reflected > maxRadius) {
          reflected = span - reflected;
          bounceDir = -1.0f;
        }
        const float normR   = std::min(reflected / maxRadius, 1.0f);
        const float damping = 1.0f - normR * 0.55f;
        const float wave    = reflected * waveFreq - t * (timeFreq + i * 0.8f);
        const float slope   = cosf(wave) * waveFreq * damping * bounceDir * sourceWeight[i];

        gradX   += slope * (dx / radius);
        gradY   += slope * (dy / radius);
        waveMix += sinf(wave) * damping * sourceWeight[i];
      }

      const int sx = static_cast<int>(fx - gradX * displacementScale);
      const int sy = static_cast<int>(fy - gradY * displacementScale);
      uint16_t color = logoPixel(sx, sy);

      const float shade     = std::max(0.0f, (-gradX * lightX - gradY * lightY) * 0.85f);
      const float highlight = std::min(0.48f, shade * 0.52f);
      const float shadow    = std::min(0.26f, std::max(0.0f, (gradX * lightX + gradY * lightY) * 0.36f));

      color = blend565(color, rgb565(215, 244, 255), highlight);
      color = blend565(color, rgb565(18, 44, 76), shadow);
      color = blend565(color, rgb565(120, 196, 255), std::min(0.24f, fabsf(waveMix) * 0.10f));
      rowBuf[x] = color;
    }
    pushLogoRow(y, step);
  }
  gDraw->endWrite();
}

// Rasterbalken rund um einen Kasten mit dem Logo
void drawRasterBars(uint32_t tickMs) {
  const float t = static_cast<float>(tickMs) * 0.0014f;
  const uint16_t palette[] = {rgb565(255, 92, 164), rgb565(255, 190, 94),
                              rgb565(132, 255, 184), rgb565(96, 196, 255)};

  const int innerX = 46;
  const int innerY = 16;
  const int innerW = kScrW - 2 * innerX;
  const int innerH = kLogoH - 2 * innerY;

  gDraw->startWrite();
  for (int y = 0; y < kLogoH; ++y) {
    const int band = static_cast<int>(fmodf(y + t * 140.0f, 56.0f) / 14.0f) & 3;
    const uint16_t color = palette[band];

    for (int x = 0; x < kScrW; ++x) rowBuf[x] = color;

    if (y >= innerY && y < innerY + innerH) {
      const int srcY = ((y - innerY) * kLogoH) / innerH;
      for (int x = 0; x < innerW; ++x) {
        const int srcX = kLogoOffX + (x * kLogoDrawW) / innerW;
        rowBuf[innerX + x] = logoPixel(srcX, srcY);
      }
      const bool edge = (y == innerY) || (y == innerY + innerH - 1);
      if (edge) {
        for (int x = 0; x < innerW; ++x) rowBuf[innerX + x] = rgb565(132, 170, 255);
      } else {
        rowBuf[innerX] = rgb565(132, 170, 255);
        rowBuf[innerX + innerW - 1] = rgb565(132, 170, 255);
      }
    }
    gDraw->pushImage(0, kLogoY + y, kScrW, 1, rowBuf);
  }
  gDraw->endWrite();
}

// ---------------------------------------------------------------------------
// Effekt-Ablaufsteuerung
// ---------------------------------------------------------------------------
HomeMode homeModeFromEffect(DisplayEffectMode m) {
  switch (m) {
    case DisplayEffectMode::Water:    return HomeMode::Water;
    case DisplayEffectMode::RotoZoom: return HomeMode::RotoZoom;
    case DisplayEffectMode::SineWave: return HomeMode::SineWave;
    case DisplayEffectMode::Ripple:   return HomeMode::Ripple;
    case DisplayEffectMode::Raster:   return HomeMode::Raster;
    default:                          return HomeMode::Static;
  }
}

HomeMode cycleEffectByIndex(uint8_t index) {
  switch (index % 5u) {
    case 0: return HomeMode::Water;
    case 1: return HomeMode::RotoZoom;
    case 2: return HomeMode::SineWave;
    case 3: return HomeMode::Ripple;
    default:return HomeMode::Raster;
  }
}

HomeMode selectedCycleEffect() {
  if (app.settings.effectMode == DisplayEffectMode::Auto) {
    return cycleEffectByIndex(app.home.nextEffectIndex);
  }
  return homeModeFromEffect(app.settings.effectMode);
}

HomeMode currentHomeMode() {
  if (!app.settings.animationsEnabled) return HomeMode::Static;
  if (app.settings.effectMode == DisplayEffectMode::Static) return HomeMode::Static;
  return app.home.mode;
}

uint32_t homeModeDuration(HomeMode mode) {
  uint32_t baseMs;
  switch (mode) {
    case HomeMode::Static:
      return static_cast<uint32_t>(1200.0f * staticDurationFactor(app.settings.staticDuration));
    case HomeMode::RotoZoom:
    case HomeMode::Ripple:
      baseMs = 7000;
      break;
    default:
      baseMs = 5000;
      break;
  }
  return static_cast<uint32_t>(baseMs * effectDurationFactor(app.settings.effectDuration));
}

void enterHomeMode(HomeMode mode, uint32_t now) {
  app.home.mode        = mode;
  app.home.startedAtMs = now;
  app.home.dirty       = true;
}

void resetHomeAnimation(uint32_t now) {
  app.home.nextEffectIndex = 0;
  enterHomeMode(HomeMode::Static, now);
}

void updateHomeDemo(uint32_t now) {
  if (!app.settings.animationsEnabled || app.settings.effectMode == DisplayEffectMode::Static) {
    if (app.home.mode != HomeMode::Static || app.home.startedAtMs == 0) enterHomeMode(HomeMode::Static, now);
    return;
  }
  if (app.home.startedAtMs == 0) { enterHomeMode(HomeMode::Static, now); return; }

  if (now - app.home.startedAtMs < homeModeDuration(app.home.mode)) return;

  if (app.home.mode == HomeMode::Static) {
    enterHomeMode(selectedCycleEffect(), now);
  } else {
    if (app.settings.effectMode == DisplayEffectMode::Auto) {
      app.home.nextEffectIndex = static_cast<uint8_t>((app.home.nextEffectIndex + 1) % 5u);
    }
    enterHomeMode(HomeMode::Static, now);
  }
}

void drawHomeVisual(uint32_t now) {
  const HomeMode mode = currentHomeMode();
  const uint32_t phase = now - app.home.startedAtMs;
  const uint32_t tick  = static_cast<uint32_t>(static_cast<float>(phase) *
                                              animationSpeedFactor(app.settings.animationSpeed));
  switch (mode) {
    case HomeMode::Static:   drawStaticLogo();               break;
    case HomeMode::Water:    drawDistortedRows(tick, true);  break;
    case HomeMode::SineWave: drawDistortedRows(tick, false); break;
    case HomeMode::RotoZoom: drawRotoZoom(tick);             break;
    case HomeMode::Ripple:   drawRipple(tick);               break;
    case HomeMode::Raster:   drawRasterBars(tick);           break;
  }
}

// ===========================================================================
//  ReST-API des c64u
// ===========================================================================
String urlEncode(const String& value) {
  static const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String apiBaseUrl() { return String("http://") + targetHost(); }

String extractErrors(DynamicJsonDocument& doc) {
  if (!doc.containsKey("errors")) return "";
  String text;
  JsonArray errors = doc["errors"].as<JsonArray>();
  for (JsonVariant value : errors) {
    if (!text.isEmpty()) text += ", ";
    text += value.as<const char*>();
  }
  return text;
}

String jsonValueToString(JsonVariantConst value) {
  if (value.is<const char*>()) return String(value.as<const char*>());
  if (value.is<String>())      return value.as<String>();
  if (value.is<long>())        return String(value.as<long>());
  if (value.is<int>())         return String(value.as<int>());
  if (value.is<bool>())        return value.as<bool>() ? "Yes" : "No";
  String text;
  serializeJson(value, text);
  return text;
}

String extractDigits(const String& value) {
  String digits;
  for (size_t i = 0; i < value.length(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(value[i]))) digits += value[i];
  }
  return digits;
}

ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated) {
  ApiResponse result;
  if (!hasTargetConfig()) { result.errors = "Target host missing"; return result; }

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  const String url = apiBaseUrl() + path;
  if (!http.begin(url)) { result.errors = "HTTP begin failed"; return result; }

  if (authenticated && !targetPassword().isEmpty()) {
    http.addHeader("X-Password", targetPassword());
  }

  if (strcmp(method, "GET") == 0) {
    result.httpCode = http.GET();
  } else if (strcmp(method, "PUT") == 0) {
    result.httpCode = http.sendRequest("PUT", "");
  } else {
    http.end();
    result.errors = "Unsupported method";
    return result;
  }

  result.transportOk = result.httpCode > 0;
  if (result.transportOk) {
    result.body = http.getString();
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, result.body) == DeserializationError::Ok) {
      result.jsonOk  = true;
      result.errors  = extractErrors(doc);
      result.apiOk   = result.httpCode >= 200 && result.httpCode < 300 && result.errors.isEmpty();
    } else {
      result.apiOk = result.httpCode >= 200 && result.httpCode < 300;
    }
  } else {
    result.errors = http.errorToString(result.httpCode);
  }
  http.end();
  return result;
}

// ---------------------------------------------------------------------------
// CPU-Speed: Pfad in der Konfiguration suchen und Auswahl einlesen
// ---------------------------------------------------------------------------
String cpuLabelFromValue(const String& rawValue) {
  const String trimmed = trimCopy(rawValue);
  const String digits  = extractDigits(trimmed);
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    if (trimCopy(app.cpuWireOptions[i]) == trimmed ||
        extractDigits(app.cpuWireOptions[i]) == digits ||
        trimCopy(app.cpuDisplayOptions[i]).equalsIgnoreCase(trimmed)) {
      return app.cpuDisplayOptions[i];
    }
  }
  if (!digits.isEmpty()) return digits + " MHz";
  return trimmed.isEmpty() ? "Unknown" : trimmed;
}

int cpuIndexFromValue(const String& rawValue) {
  const String trimmed = trimCopy(rawValue);
  const String digits  = extractDigits(trimmed);
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    if (trimCopy(app.cpuWireOptions[i]) == trimmed ||
        extractDigits(app.cpuWireOptions[i]) == digits ||
        trimCopy(app.cpuDisplayOptions[i]).equalsIgnoreCase(trimmed)) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void setFallbackCpuChoices() {
  static const char* fallback[] = {" 1", " 2", " 3", " 4", " 6", " 8", "10", "12",
                                   "14", "16", "20", "24", "32", "40", "48", "64"};
  app.cpuChoiceCount = std::min(kMaxCpuChoices, sizeof(fallback) / sizeof(fallback[0]));
  for (size_t i = 0; i < app.cpuChoiceCount; ++i) {
    app.cpuWireOptions[i]    = fallback[i];
    app.cpuDisplayOptions[i] = trimCopy(fallback[i]) + " MHz";
  }
}

bool inspectCpuCategory(const String& category, String* itemOut, String* valueOut) {
  const ApiResponse response = sendApiRequest("GET", "/v1/configs/" + urlEncode(category), true);
  if (!response.apiOk) return false;

  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) return false;

  JsonVariant categoryObject = doc[category];
  if (categoryObject.isNull()) {
    for (JsonPair kv : doc.as<JsonObject>()) {
      if (String(kv.key().c_str()) != "errors" && kv.value().is<JsonObject>()) {
        categoryObject = kv.value();
        break;
      }
    }
  }
  if (categoryObject.isNull() || !categoryObject.is<JsonObject>()) return false;

  for (JsonPair kv : categoryObject.as<JsonObject>()) {
    const String key = kv.key().c_str();
    if (key.indexOf("CPU") >= 0 && key.indexOf("Speed") >= 0) {
      *itemOut  = key;
      *valueOut = jsonValueToString(kv.value());
      return true;
    }
  }
  return false;
}

bool refreshCpuChoices() {
  if (app.cpuCategory.isEmpty() || app.cpuItem.isEmpty()) { setFallbackCpuChoices(); return false; }

  const ApiResponse response = sendApiRequest(
      "GET", "/v1/configs/" + urlEncode(app.cpuCategory) + "/" + urlEncode(app.cpuItem), true);
  if (!response.apiOk) { setFallbackCpuChoices(); return false; }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) { setFallbackCpuChoices(); return false; }

  JsonVariant itemObject = doc[app.cpuCategory][app.cpuItem];
  if (itemObject.isNull()) { setFallbackCpuChoices(); return false; }

  app.cpuChoiceCount = 0;
  JsonArray values = itemObject["values"].as<JsonArray>();
  for (JsonVariant value : values) {
    if (app.cpuChoiceCount >= kMaxCpuChoices) break;
    const String wire   = jsonValueToString(value);
    const String digits = extractDigits(wire);
    app.cpuWireOptions[app.cpuChoiceCount]    = wire;
    app.cpuDisplayOptions[app.cpuChoiceCount] = digits.isEmpty() ? trimCopy(wire) : digits + " MHz";
    app.cpuChoiceCount += 1;
  }
  if (app.cpuChoiceCount == 0) { setFallbackCpuChoices(); return false; }

  app.currentCpuValue = cpuLabelFromValue(jsonValueToString(itemObject["current"]));
  return true;
}

bool resolveCpuPath(String* detailOut = nullptr) {
  if (app.cpuPathKnown) {
    if (app.cpuChoiceCount == 0) refreshCpuChoices();
    return true;
  }

  String item, value;
  if (inspectCpuCategory("U64 Specific Settings", &item, &value)) {
    app.cpuCategory     = "U64 Specific Settings";
    app.cpuItem         = item;
    app.currentCpuValue = cpuLabelFromValue(value);
    app.cpuPathKnown    = true;
    refreshCpuChoices();
    return true;
  }

  const ApiResponse listResponse = sendApiRequest("GET", "/v1/configs", true);
  if (!listResponse.apiOk) {
    if (detailOut) *detailOut = listResponse.errors.isEmpty() ? "Config list failed" : listResponse.errors;
    return false;
  }

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, listResponse.body) != DeserializationError::Ok) {
    if (detailOut) *detailOut = "Config list parse failed";
    return false;
  }

  JsonArray categories = doc["categories"].as<JsonArray>();
  for (JsonVariant valueVariant : categories) {
    const String category = valueVariant.as<const char*>();
    if (inspectCpuCategory(category, &item, &value)) {
      app.cpuCategory     = category;
      app.cpuItem         = item;
      app.currentCpuValue = cpuLabelFromValue(value);
      app.cpuPathKnown    = true;
      refreshCpuChoices();
      return true;
    }
  }
  if (detailOut) *detailOut = "CPU speed item not found";
  return false;
}

void refreshCpuValue() {
  String detail;
  if (!resolveCpuPath(&detail)) { app.currentCpuValue = detail; return; }
  refreshCpuChoices();
  app.barDirty = true;
}

// ---------------------------------------------------------------------------
// WLAN und Verbindungsstatus
// ---------------------------------------------------------------------------
// Verbindet mit dem naechsten gespeicherten Netz. Sind mehrere Profile
// hinterlegt, wandert der Versuch bei jedem Aufruf eins weiter - so werden
// nacheinander alle bekannten Netze durchprobiert.
void beginWiFi(uint32_t now) {
  if (!hasWiFiConfig()) return;
  if (gWifiTry >= gWifiCount) gWifiTry = 0;

  const WifiProfile& profile = gWifiProfiles[gWifiTry];

  // Waehrend das Setup-Portal laeuft, bleibt der Accesspoint bestehen.
  if (!app.portalActive) WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(profile.ssid.c_str(), profile.pass.c_str());
  app.lastWiFiAttemptMs = now;

  if (gWifiCount > 1) gWifiTry = (gWifiTry + 1) % gWifiCount;
}

void serviceWiFi(uint32_t now) {
  if (app.portalActive) return;          // Portal hat Vorrang
  if (!hasWiFiConfig()) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (app.lastWiFiAttemptMs == 0 || now - app.lastWiFiAttemptMs >= kWiFiRetryMs) beginWiFi(now);
}

void refreshConnectionStatus(uint32_t now, bool force = false) {
  const bool wasOk   = app.connection.wifiConnected && app.connection.authOk;
  const bool wasWifi = app.connection.wifiConnected;
  app.connection.wifiConnected = WiFi.status() == WL_CONNECTED;

  if (!configReady()) {
    app.connection.targetReachable = false;
    app.connection.authOk          = false;
    app.connection.detail          = hasWiFiConfig() ? "c64u-Adresse fehlt"
                                                     : "Setup > WLAN einrichten";
  } else if (!app.connection.wifiConnected) {
    app.connection.targetReachable = false;
    app.connection.authOk          = false;
    app.connection.detail          = "WiFi disconnected";
  } else if (force || app.lastConnectionProbeMs == 0 ||
             now - app.lastConnectionProbeMs >= kConnectionProbeMs) {
    app.lastConnectionProbeMs = now;

    const ApiResponse reach = sendApiRequest("GET", "/v1/version", false);
    app.connection.targetReachable = reach.transportOk;
    if (!reach.transportOk) {
      app.connection.authOk = false;
      app.connection.detail = reach.errors.isEmpty() ? "Target unreachable" : reach.errors;
    } else {
      const ApiResponse auth = sendApiRequest("GET", "/v1/version", true);
      app.connection.authOk = auth.apiOk;
      app.connection.detail = auth.apiOk ? "Reachable + auth ok"
                                         : (auth.errors.isEmpty() ? "Auth failed" : auth.errors);
    }
  }

  // Der Core zeichnet Listenseiten nur bei screenDirty neu. Kommt die
  // Verbindung im Hintergrund zustande, muessen WLAN-Menue, Netzliste,
  // Settings und Statusseite ihren Zustand aber sofort zeigen.
  const bool isOk = app.connection.wifiConnected && app.connection.authOk;
  if (isOk != wasOk || app.connection.wifiConnected != wasWifi) {
    app.barDirty    = true;
    app.screenDirty = true;
  }
}

// ===========================================================================
//  Statuslampe (SK6812 an Port B)
//
//  Die Lampe zeigt in einer Farbe dasselbe wie der Punkt links in der
//  Statusleiste, dazu ein paar Bewegungen, die man auch aus dem Augenwinkel
//  erkennt:
//
//    rot            kein WLAN
//    blau           WLAN da, c64u antwortet nicht
//    gelb           c64u erreichbar, Passwort stimmt nicht
//    gruen          alles verbunden
//    magenta lauf   Setup-Portal laeuft
//    cyan balken    Datei wird in den c64u geladen; die drei LEDs sind ein
//                   Fortschrittsbalken, der laufende Abschnitt pulsiert
//    kurzer Blitz   NFC-Karte gelesen (gruen) bzw. Fehler (rot)
//
//  Steckt keine LED an Port B, laufen die Daten ins Leere - das stoert nicht.
//  Erkennen laesst sich das nicht, deshalb der Schalter "Status-LED" im Setup.
// ===========================================================================
uint32_t ledRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

// Farbe abdunkeln. `scale` ist 0..255 und wirkt auf alle drei Kanaele.
uint32_t ledScale(uint32_t rgb, uint8_t scale) {
  const uint16_t r = ((rgb >> 16) & 0xFF) * scale / 255;
  const uint16_t g = ((rgb >> 8) & 0xFF) * scale / 255;
  const uint16_t b = (rgb & 0xFF) * scale / 255;
  return ledRgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

// Dreieckskurve 0..255..0 ueber `periodMs`. Ergibt ein ruhiges Atmen ohne
// Gleitkomma und ohne Tabelle.
uint8_t ledPulse(uint32_t now, uint32_t periodMs) {
  const uint32_t phase = now % periodMs;
  const uint32_t half  = periodMs / 2;
  const uint32_t up    = (phase < half) ? phase : (periodMs - phase);
  return static_cast<uint8_t>((up * 255) / half);
}

// Ereignisblitz anfordern. Wird von der Kartenverarbeitung benutzt.
//
// ledFlashMs == 0 heisst ausdruecklich "kein Blitz". Ohne dieses Kennzeichen
// bliebe der alte Zeitpunkt fuer immer stehen, und der Vergleich
// (int32_t)(now - ledFlashMs) < 0 wuerde nach rund 25 Tagen Laufzeit wieder
// wahr - die Lampe zeigte dann wochenlang die alte Blitzfarbe.
void ledFlash(uint32_t rgb) {
  app.ledFlashRgb = rgb;
  app.ledFlashMs  = millis() + kLedFlashMs;
  if (app.ledFlashMs == 0) app.ledFlashMs = 1;
}

// Farbe, die dem aktuellen Verbindungszustand entspricht
uint32_t ledConnectionColour() {
  if (!app.connection.wifiConnected)   return ledRgb(255, 0, 0);      // rot
  if (!app.connection.targetReachable) return ledRgb(0, 80, 255);     // blau
  if (!app.connection.authOk)          return ledRgb(255, 170, 0);    // gelb
  return ledRgb(0, 255, 80);                                          // gruen
}

void serviceLed(uint32_t now) {
  if (!app.settings.ledEnabled) {
    // Einmal ausschalten, danach nichts mehr senden.
    if (app.ledLastRgb[0] != 0 || app.ledLastRgb[1] != 0 || app.ledLastRgb[2] != 0) {
      for (uint16_t i = 0; i < kLedCount; ++i) {
        gLed.setPixelColor(i, 0);
        app.ledLastRgb[i] = 0;
      }
      gLed.show();
    }
    return;
  }

  if (static_cast<int32_t>(now - app.ledNextMs) < 0) return;
  app.ledNextMs = now + kLedFrameMs;

  uint32_t rgb[kLedCount] = {0, 0, 0};

  const bool flashOn = (app.ledFlashMs != 0) &&
                       (static_cast<int32_t>(now - app.ledFlashMs) < 0);
  if (!flashOn) app.ledFlashMs = 0;      // abgelaufenen Blitz vergessen

  if (flashOn) {
    // Ereignisblitz hat Vorrang, betrifft alle drei und klingt aus.
    const uint32_t leftMs = app.ledFlashMs - now;
    const uint32_t c = ledScale(app.ledFlashRgb,
                                static_cast<uint8_t>((leftMs * 255) / kLedFlashMs));
    for (uint16_t i = 0; i < kLedCount; ++i) rgb[i] = c;

  } else if (app.screen == ScreenMode::Busy) {
    // Upload: die drei LEDs sind ein Fortschrittsbalken. Erledigte Abschnitte
    // leuchten voll, der gerade laufende pulsiert, der Rest bleibt dunkel.
    // Ist die Dateigroesse unbekannt, laeuft stattdessen ein Lauflicht.
    const uint32_t base = ledRgb(0, 220, 255);
    if (app.busyTotal > 0) {
      // Fortschritt in 256steln je LED, damit die Rechnung ganzzahlig
      // bleibt: 0 .. kLedCount*256
      const uint32_t done = (256ULL * kLedCount * app.busySent) / app.busyTotal;
      for (uint16_t i = 0; i < kLedCount; ++i) {
        const uint32_t lo = 256u * i;
        if (done >= lo + 256u) {
          rgb[i] = base;                                    // fertig
        } else if (done > lo || i == 0) {
          // Die erste LED pulsiert schon bei 0 % mit - sonst saehe der Start
          // eines Uploads aus, als waere die Lampe kaputt.
          // Der laufende Abschnitt pulsiert, damit man sieht, dass es weitergeht
          const uint8_t fill = static_cast<uint8_t>(60 + (done - lo) * 195 / 256);
          const uint8_t puls = static_cast<uint8_t>(120 + ledPulse(now, 500) * 135 / 255);
          rgb[i] = ledScale(base, static_cast<uint8_t>((fill * puls) / 255));
        } else {
          rgb[i] = 0;                                       // noch nichts
        }
      }
    } else {
      const uint16_t active = (now / 160) % kLedCount;
      for (uint16_t i = 0; i < kLedCount; ++i) {
        rgb[i] = (i == active) ? base : ledScale(base, 25);
      }
    }

  } else if (app.portalActive) {
    // Portal: magentafarbenes Lauflicht, deutlich langsamer als der Upload.
    const uint32_t base   = ledRgb(255, 0, 200);
    const uint16_t active = (now / 400) % kLedCount;
    for (uint16_t i = 0; i < kLedCount; ++i) {
      rgb[i] = (i == active) ? ledScale(base, 60 + ledPulse(now, 1600) * 195 / 255)
                             : ledScale(base, 25);
    }

  } else {
    // Verbindungszustand: alle drei in derselben Farbe.
    const uint32_t c = ledConnectionColour();
    for (uint16_t i = 0; i < kLedCount; ++i) rgb[i] = c;
  }

  // Nur senden, wenn sich wirklich etwas geaendert hat.
  bool changed = (app.settings.ledBrightness != app.ledLastBright);
  for (uint16_t i = 0; i < kLedCount && !changed; ++i) changed = (rgb[i] != app.ledLastRgb[i]);
  if (!changed) return;

  app.ledLastBright = app.settings.ledBrightness;
  gLed.setBrightness(app.settings.ledBrightness);
  for (uint16_t i = 0; i < kLedCount; ++i) {
    app.ledLastRgb[i] = rgb[i];
    gLed.setPixelColor(i, gLed.Color((rgb[i] >> 16) & 0xFF, (rgb[i] >> 8) & 0xFF, rgb[i] & 0xFF));
  }
  gLed.show();
}

bool requireNetwork(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) return true;
  beginWiFi(now);
  setModal("NO WIFI", kColWarn, now);
  return false;
}

// ---------------------------------------------------------------------------
// Einfache Maschinen-Kommandos
// ---------------------------------------------------------------------------
void clearPendingPowerOff() { app.pendingPowerOff = false; }

void simpleCommand(const char* path, const String& okText, const String& failText,
                   uint16_t okColor, uint32_t now) {
  if (!requireNetwork(now)) return;
  const ApiResponse response = sendApiRequest("PUT", path, true);
  if (response.apiOk) {
    beep(2400, 40);
    setModal(okText, okColor, now);
  } else {
    beep(500, 120);
    setModal(response.errors.isEmpty() ? failText : response.errors, kColErr, now, 2000);
  }
}

void performReset(uint32_t now)      { clearPendingPowerOff(); simpleCommand("/v1/machine:reset", "RESET", "RESET FAILED", kColOk, now); }
void performHardReset(uint32_t now)  { clearPendingPowerOff(); simpleCommand("/v1/machine:reboot", "REBOOT", "REBOOT FAILED", kColWarn, now); }
void performMenuButton(uint32_t now) { clearPendingPowerOff(); simpleCommand("/v1/machine:menu_button", "ULTIMATE MENU", "MENU FAILED", kColInfo, now); }

void performPowerOff(uint32_t now) {
  clearPendingPowerOff();
  simpleCommand("/v1/machine:poweroff", "POWER OFF", "POWEROFF FAILED", kColWarn, now);
}

void requestPowerOff(uint32_t now) {
  if (app.pendingPowerOff && (now - app.pendingPowerOffAtMs <= powerOffConfirmMs())) {
    performPowerOff(now);
    return;
  }
  app.pendingPowerOff     = true;
  app.pendingPowerOffAtMs = now;
  beep(900, 60);
  setModal("POWER OFF? NOCHMAL TIPPEN!", kColWarn, now, powerOffConfirmMs());
}

void setCpuSpeed(int cpuIndex, uint32_t now) {
  clearPendingPowerOff();
  if (!requireNetwork(now)) return;

  String detail;
  if (!resolveCpuPath(&detail)) { setModal(detail, kColErr, now, 2000); return; }
  if (app.cpuChoiceCount == 0) refreshCpuChoices();

  const int idx = std::max(0, std::min(cpuIndex, static_cast<int>(app.cpuChoiceCount) - 1));
  const String displayValue = app.cpuDisplayOptions[idx];
  const String path = "/v1/configs/" + urlEncode(app.cpuCategory) + "/" + urlEncode(app.cpuItem)
                    + "?value=" + urlEncode(app.cpuWireOptions[idx]);

  const ApiResponse response = sendApiRequest("PUT", path, true);
  if (response.apiOk) {
    refreshCpuValue();
    beep(2600, 40);
    setModal(displayValue, kColOk, now);
  } else {
    beep(500, 120);
    setModal(response.errors.isEmpty() ? "CPU SET FAILED" : response.errors, kColErr, now, 2000);
  }
}

void runConnectionTest(uint32_t now) {
  clearPendingPowerOff();
  refreshConnectionStatus(now, true);

  if (!configReady()) {
    setModal("CONFIG MISSING", kColWarn, now, 1800);
  } else if (!app.connection.wifiConnected) {
    beginWiFi(now);
    setModal("WIFI NOT READY", kColWarn, now, 1800);
  } else if (app.connection.authOk) {
    refreshCpuValue();
    setModal("AUTH OK", kColOk, now, 1400);
  } else if (app.connection.targetReachable) {
    setModal("AUTH FAILED", kColWarn, now, 1800);
  } else {
    setModal("TARGET OFFLINE", kColErr, now, 1800);
  }
  app.barDirty = true;
}

// ===========================================================================
//  PETSCII-Tastatureingabe (fuer Autostart nach dem Mounten)
// ===========================================================================
String toHex(const uint8_t* data, size_t len) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

// Schreibt bis zu 10 PETSCII-Zeichen in den Tastaturpuffer des C64
// ($0277..$0280) und setzt die Anzahl in NDX ($C6).
bool typeToC64(const uint8_t* petscii, size_t len) {
  if (len == 0 || len > 10) return false;

  // LSTX / NDX zuruecksetzen
  if (!sendApiRequest("PUT", "/v1/machine:writemem?address=C5&data=0000", true).apiOk) return false;
  if (!sendApiRequest("PUT", "/v1/machine:writemem?address=277&data=" + toHex(petscii, len), true).apiOk) return false;

  const uint8_t count = static_cast<uint8_t>(len);
  return sendApiRequest("PUT", "/v1/machine:writemem?address=C6&data=" + toHex(&count, 1), true).apiOk;
}

// Liest ein einzelnes Byte aus dem C64-Speicher (per DMA).
// readmem liefert die Daten als Binaeranhang, deshalb direkt aus dem Stream.
bool readC64Byte(uint16_t address, uint8_t* out) {
  if (WiFi.status() != WL_CONNECTED) return false;

  char path[72];
  snprintf(path, sizeof(path), "/v1/machine:readmem?address=%X&length=1", address);

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  if (!http.begin(apiBaseUrl() + path)) return false;
  if (!targetPassword().isEmpty()) http.addHeader("X-Password", targetPassword());

  bool ok = false;
  if (http.GET() == 200) {
    WiFiClient* stream = http.getStreamPtr();
    const uint32_t deadline = millis() + 2000;
    while (millis() < deadline) {
      if (stream->available()) {
        *out = static_cast<uint8_t>(stream->read());
        ok = true;
        break;
      }
      delay(5);
    }
  }
  http.end();
  return ok;
}

// $CC (BLNSW) ist 0, solange der Cursor blinkt - also genau dann, wenn BASIC
// auf Eingaben wartet. Waehrend LOAD/RUN ist der Wert ungleich 0.
// Damit laesst sich zuverlaessig erkennen, wann die Diskette fertig geladen
// hat, ohne mit festen Wartezeiten zu raten.
constexpr uint16_t kAddrCursorBlink = 0x00CC;

bool waitCursorBlinking(bool wantBlinking, uint32_t timeoutMs, const char* label) {
  const uint32_t start = millis();
  uint8_t stable = 0;
  uint32_t lastUi = 0;

  while (millis() - start < timeoutMs) {
    uint8_t value = 0xFF;
    if (readC64Byte(kAddrCursorBlink, &value)) {
      const bool blinking = (value == 0);
      if (blinking == wantBlinking) {
        if (++stable >= 2) return true;
      } else {
        stable = 0;
      }
    } else {
      stable = 0;
    }

    const uint32_t now = millis();
    // Diese Schleife blockiert bis zu 15 s. Die Lampe wird deshalb hier von
    // Hand mitgenommen, sonst friert sie auf ihrem letzten Wert ein.
    serviceLed(now);
    if (now - lastUi > 400) {
      lastUi = now;
      app.busyDetail = String(label) + " (" + String((timeoutMs - (now - start)) / 1000) + "s)";
      drawBusyScreen();
      pushFrame();
    }
    delay(250);
  }
  return false;
}

// Ermittelt das Laufwerk am IEC-Bus 8. Bei "Auto" wird die Zuordnung live
// beim c64u abgefragt, damit .d64 wirklich als Geraet 8 erscheint.
String resolveTargetDrive() {
  if (app.settings.uploadDrive == UploadDriveMode::DriveA) return "a";
  if (app.settings.uploadDrive == UploadDriveMode::DriveB) return "b";

  const ApiResponse response = sendApiRequest("GET", "/v1/drives", true);
  if (response.apiOk) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, response.body) == DeserializationError::Ok) {
      JsonArray drives = doc["drives"].as<JsonArray>();
      for (JsonVariant item : drives) {
        if (!item.is<JsonObject>()) continue;
        for (JsonPair kv : item.as<JsonObject>()) {
          const String key = kv.key().c_str();
          if (key != "a" && key != "b") continue;          // softiec ueberspringen
          if (!kv.value().is<JsonObject>()) continue;
          JsonObject drive = kv.value().as<JsonObject>();
          if (drive["bus_id"].as<int>() == 8) return key;
        }
      }
    }
  }
  return "a";   // Werkseinstellung von Laufwerk A ist Bus 8
}

// lO"*",8,1<CR>   ("lO" = abgekuerztes LOAD, passt so in 10 Zeichen)
bool typeLoadFirstFile() {
  static const uint8_t seq[] = {0x4C, 0xCF, 0x22, 0x2A, 0x22, 0x2C, 0x38, 0x2C, 0x31, 0x0D};
  return typeToC64(seq, sizeof(seq));
}

// rU<CR>          ("rU" = abgekuerztes RUN)
bool typeRun() {
  static const uint8_t seq[] = {0x52, 0xD5, 0x0D};
  return typeToC64(seq, sizeof(seq));
}

// ===========================================================================
//  Streaming-Upload von der microSD an den c64u
//  Grosse Dateien (z. B. 175 kB .d64) werden in kleinen Bloecken gesendet,
//  damit kein RAM-Puffer in Dateigroesse benoetigt wird.
// ===========================================================================
struct UploadResult {
  bool   ok       = false;
  int    httpCode = -1;
  String message;
};

void publishProgress(size_t sent, size_t total) {
  app.busySent  = sent;
  app.busyTotal = total;
  drawBusyScreen();
  pushFrame();
  // Der Upload blockiert die Hauptschleife. Damit die Lampe trotzdem
  // pulsiert, wird sie hier von Hand mitgenommen.
  serviceLed(millis());
}

// Liefert alles bis zum Doppel-CRLF und danach den Body (max. 1 kB).
UploadResult readHttpResponse(WiFiClient& client) {
  UploadResult result;
  const uint32_t deadline = millis() + 15000;

  String statusLine;
  while (client.connected() || client.available()) {
    if (millis() > deadline) { result.message = "Timeout"; return result; }
    if (!client.available()) { delay(5); continue; }
    statusLine = client.readStringUntil('\n');
    break;
  }
  statusLine.trim();
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace > 0) result.httpCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();

  // Header ueberspringen
  while (client.connected() || client.available()) {
    if (millis() > deadline) break;
    if (!client.available()) { delay(5); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
  }

  String body;
  while ((client.connected() || client.available()) && body.length() < 1024) {
    if (millis() > deadline) break;
    if (!client.available()) { delay(5); continue; }
    body += static_cast<char>(client.read());
  }

  result.ok = result.httpCode >= 200 && result.httpCode < 300;

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    const String errors = extractErrors(doc);
    if (!errors.isEmpty()) { result.ok = false; result.message = errors; }
  }
  if (result.message.isEmpty()) {
    result.message = result.ok ? "OK" : (result.httpCode == 403 ? "Forbidden (Passwort?)"
                                                               : String("HTTP ") + result.httpCode);
  }
  return result;
}

// urlPath        z. B. "/v1/runners:run_prg" - Argumente wie type/mode gehoeren
//                in die Query, NICHT als Multipart-Textfeld: die Ultimate-
//                Firmware behandelt jeden Multipart-Teil als Anhang und wuerde
//                ein vorangestelltes Textfeld als Datei ohne Endung deuten
//                ("Invalid type").
// multipartName  leer  -> Datei als reiner Body (octet-stream)
//                sonst -> multipart/form-data mit genau diesem einen Dateifeld
UploadResult uploadFile(const String& urlPath, File& file, const String& fileName,
                        const String& multipartName) {
  UploadResult result;
  const size_t fileSize = file.size();

  WiFiClient client;
  client.setTimeout(10);   // Sekunden (WiFiClient)
  if (!client.connect(targetHost().c_str(), 80)) {
    result.message = "Verbindung fehlgeschlagen";
    return result;
  }

  const bool multipart = !multipartName.isEmpty();
  const String boundary = "----C64uRemoteBoundary7A31";

  String head;
  String tail;
  size_t contentLength = fileSize;

  if (multipart) {
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"" + multipartName +
            "\"; filename=\"" + fileName + "\"\r\n";
    head += "Content-Type: application/octet-stream\r\n\r\n";
    tail  = "\r\n--" + boundary + "--\r\n";
    contentLength = head.length() + fileSize + tail.length();
  }

  String request;
  request.reserve(320);
  request += "POST " + urlPath + " HTTP/1.1\r\n";
  request += "Host: " + targetHost() + "\r\n";
  if (!targetPassword().isEmpty()) request += "X-Password: " + targetPassword() + "\r\n";
  request += "Content-Type: ";
  request += multipart ? ("multipart/form-data; boundary=" + boundary) : String("application/octet-stream");
  request += "\r\n";
  request += "Content-Length: " + String(contentLength) + "\r\n";
  request += "Connection: close\r\n\r\n";

  client.print(request);
  if (multipart && !head.isEmpty()) client.print(head);

  // ---- Datei blockweise senden ----
  static uint8_t buffer[kUploadChunk];
  size_t sent = 0;
  uint32_t lastUi = 0;
  while (sent < fileSize) {
    const int chunk = file.read(buffer, kUploadChunk);
    if (chunk <= 0) break;
    const size_t written = client.write(buffer, static_cast<size_t>(chunk));
    if (written != static_cast<size_t>(chunk)) {
      client.stop();
      result.message = "Upload abgebrochen";
      return result;
    }
    sent += written;

    const uint32_t now = millis();
    if (now - lastUi > 120) {
      lastUi = now;
      publishProgress(sent, fileSize);
    }
    if (!client.connected()) break;
  }
  if (multipart && !tail.isEmpty()) client.print(tail);
  client.flush();
  publishProgress(fileSize, fileSize);

  result = readHttpResponse(client);
  client.stop();
  return result;
}

// ===========================================================================
//  microSD
// ===========================================================================
bool initSd() {
  app.sdReady = SD.begin(kSdCsPin, SPI, 20000000);
  if (!app.sdReady) {
    delay(50);
    app.sdReady = SD.begin(kSdCsPin, SPI, 4000000);
  }
  return app.sdReady;
}

String lowerExt(const String& name) {
  const int dot = name.lastIndexOf('.');
  if (dot < 0) return "";
  String ext = name.substring(dot + 1);
  ext.toLowerCase();
  return ext;
}

bool isSupportedExt(const String& ext) {
  return ext == "prg" || ext == "crt" || ext == "sid" || ext == "mod" ||
         ext == "d64" || ext == "d71" || ext == "d81" || ext == "g64" || ext == "g71";
}

bool isDiskImageExt(const String& ext) {
  return ext == "d64" || ext == "d71" || ext == "d81" || ext == "g64" || ext == "g71";
}

String joinPath(const String& dir, const String& name) {
  if (dir.endsWith("/")) return dir + name;
  return dir + "/" + name;
}

String parentPath(const String& path) {
  if (path == "/" || path.isEmpty()) return "/";
  String p = path;
  if (p.endsWith("/")) p.remove(p.length() - 1);
  const int slash = p.lastIndexOf('/');
  if (slash <= 0) return "/";
  return p.substring(0, slash);
}

String baseName(const String& path) {
  const int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

void sortDirEntries() {
  // Ordner zuerst, danach alphabetisch (einfacher Insertion Sort, n <= 160)
  for (size_t i = 1; i < app.sdCount; ++i) {
    DirEntryInfo key = app.sdEntries[i];
    size_t j = i;
    while (j > 0) {
      const DirEntryInfo& prev = app.sdEntries[j - 1];
      const bool keyFirst = (key.isDir != prev.isDir) ? key.isDir
                                                      : (strcasecmp(key.name.c_str(), prev.name.c_str()) < 0);
      if (!keyFirst) break;
      app.sdEntries[j] = prev;
      --j;
    }
    app.sdEntries[j] = key;
  }
}

bool readDirectory(const String& path) {
  app.sdCount = 0;
  app.sdIndex = 0;

  if (!app.sdReady && !initSd()) return false;

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }

  while (app.sdCount < kMaxDirEntries) {
    File entry = dir.openNextFile();
    if (!entry) break;

    String name = baseName(String(entry.name()));
    const bool isDir = entry.isDirectory();
    entry.close();

    if (name.isEmpty() || name.startsWith(".")) continue;
    const String ext = lowerExt(name);
    if (!isDir) {
      // Zufallskarte: nur Ordner anbieten
      if (app.sdPickMode == 3) continue;
      else if (app.sdPickMode == 2) { if (ext != "nfc") continue; }
      else if (!isSupportedExt(ext)) continue;
    }

    app.sdEntries[app.sdCount].name  = name;
    app.sdEntries[app.sdCount].isDir = isDir;
    app.sdCount++;
  }
  dir.close();
  sortDirEntries();
  app.sdPath = path;
  return true;
}

// ===========================================================================
//  Netzkonfiguration: speichern, laden, pflegen
//
//  Speicher:  NVS-Namensraum "c64unet"
//               wn        Anzahl der Profile (0..kWifiProfileMax)
//               s0..s3    SSID
//               p0..p3    Passwort
//               host      Adresse des c64u
//               hpass     Passwort des c64u
//             Ist noch nichts gespeichert, kommen die Werte aus build_env.h.
//
//  Eingabewege: NFC-Karte, /wifi.txt auf der SD-Karte, Setup-Portal.
// ===========================================================================
constexpr const char* kNetNamespace = "c64unet";

void saveNetConfig() {
  prefs.begin(kNetNamespace, false);
  prefs.putUChar("wn", static_cast<uint8_t>(gWifiCount));
  for (size_t i = 0; i < kWifiProfileMax; ++i) {
    char keySsid[6];
    char keyPass[6];
    snprintf(keySsid, sizeof(keySsid), "s%u", static_cast<unsigned>(i));
    snprintf(keyPass, sizeof(keyPass), "p%u", static_cast<unsigned>(i));
    if (i < gWifiCount) {
      prefs.putString(keySsid, gWifiProfiles[i].ssid);
      prefs.putString(keyPass, gWifiProfiles[i].pass);
    } else {
      prefs.remove(keySsid);
      prefs.remove(keyPass);
    }
  }
  prefs.putString("host",  gTargetHost);
  prefs.putString("hpass", gTargetPass);
  prefs.end();
}

void loadNetConfig() {
  gWifiCount = 0;
  gWifiTry   = 0;

  prefs.begin(kNetNamespace, true);
  const uint8_t stored = prefs.getUChar("wn", 0);
  for (uint8_t i = 0; i < stored && gWifiCount < kWifiProfileMax; ++i) {
    char keySsid[6];
    char keyPass[6];
    snprintf(keySsid, sizeof(keySsid), "s%u", static_cast<unsigned>(i));
    snprintf(keyPass, sizeof(keyPass), "p%u", static_cast<unsigned>(i));
    const String ssid = prefs.getString(keySsid, "");
    if (ssid.isEmpty()) continue;
    gWifiProfiles[gWifiCount].ssid = ssid;
    gWifiProfiles[gWifiCount].pass = prefs.getString(keyPass, "");
    ++gWifiCount;
  }
  gTargetHost = prefs.getString("host",  "");
  gTargetPass = prefs.getString("hpass", "");
  prefs.end();

  // Leere Felder werden aus build_env.h aufgefuellt. Ein dort eingetragenes
  // c64u-Passwort laesst sich damit nicht auf "leer" setzen - dafuer den
  // Eintrag in build_env.h loeschen und neu uebersetzen.
  if (gTargetHost.isEmpty()) gTargetHost = buildHost();
  if (gTargetPass.isEmpty()) gTargetPass = buildHostPass();

  if (gWifiCount == 0 && !buildWifiSsid().isEmpty()) {
    gWifiProfiles[0].ssid = buildWifiSsid();
    gWifiProfiles[0].pass = buildWifiPass();
    gWifiCount = 1;
  }
}

// Neues Netz vorne einsortieren. Ein bereits bekanntes Netz bekommt nur ein
// neues Passwort, das aelteste faellt bei Bedarf hinten heraus.
bool wifiAddProfile(const String& ssidRaw, const String& pass, bool store = true) {
  const String ssid = trimCopy(ssidRaw);
  if (ssid.isEmpty() || ssid.length() > 32) return false;
  if (pass.length() > 63) return false;

  size_t found = kWifiProfileMax;
  for (size_t i = 0; i < gWifiCount; ++i) {
    if (gWifiProfiles[i].ssid == ssid) { found = i; break; }
  }

  if (found < gWifiCount) {
    gWifiProfiles[found].pass = pass;
    for (size_t i = found; i > 0; --i) {
      const WifiProfile tmp = gWifiProfiles[i];
      gWifiProfiles[i]      = gWifiProfiles[i - 1];
      gWifiProfiles[i - 1]  = tmp;
    }
  } else {
    if (gWifiCount < kWifiProfileMax) ++gWifiCount;
    for (size_t i = gWifiCount - 1; i > 0; --i) gWifiProfiles[i] = gWifiProfiles[i - 1];
    gWifiProfiles[0].ssid = ssid;
    gWifiProfiles[0].pass = pass;
  }

  gWifiTry = 0;
  if (store) saveNetConfig();
  return true;
}

bool wifiRemoveProfile(size_t index) {
  if (index >= gWifiCount) return false;
  for (size_t i = index; i + 1 < gWifiCount; ++i) gWifiProfiles[i] = gWifiProfiles[i + 1];
  --gWifiCount;
  gWifiProfiles[gWifiCount] = WifiProfile();
  gWifiTry = 0;
  saveNetConfig();
  return true;
}

void wifiClearProfiles() {
  for (size_t i = 0; i < kWifiProfileMax; ++i) gWifiProfiles[i] = WifiProfile();
  gWifiCount = 0;
  gWifiTry   = 0;
  saveNetConfig();
}

int wifiProfileIndex(const String& ssid) {
  for (size_t i = 0; i < gWifiCount; ++i) {
    if (gWifiProfiles[i].ssid == ssid) return static_cast<int>(i);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Kartentext im WLAN-Schema lesen
//
//     WIFI:S:<ssid>;T:WPA;P:<passwort>;;
//
// Das ist dasselbe Format, das WLAN-QR-Codes verwenden. Jede Handy-App, die
// einen NDEF-Text-Record schreiben kann, ist damit einsetzbar. Als Kurzform
// wird auch "WIFI:<ssid>;<passwort>" akzeptiert.
// ---------------------------------------------------------------------------
bool parseWifiText(const String& raw, String* ssidOut, String* passOut) {
  String text = trimCopy(raw);
  if (text.length() < 6) return false;

  String head = text.substring(0, 5);
  head.toUpperCase();
  if (head != "WIFI:") return false;

  String ssid;
  String pass;
  bool   sawField = false;

  unsigned i = 5;
  while (i < text.length()) {
    String key;
    while (i < text.length() && text[i] != ':' && text[i] != ';') key += text[i++];
    if (i >= text.length()) break;
    if (text[i] == ';') { ++i; continue; }      // Feld ohne Wert
    ++i;                                        // Doppelpunkt ueberspringen

    String value;
    while (i < text.length() && text[i] != ';') {
      if (text[i] == '\\' && i + 1 < text.length()) { value += text[i + 1]; i += 2; continue; }
      value += text[i++];
    }
    if (i < text.length()) ++i;                 // Semikolon ueberspringen

    key.toUpperCase();
    if (key == "S")      { ssid = value; sawField = true; }
    else if (key == "P") { pass = value; sawField = true; }
  }

  // Kurzform ohne Feldnamen: WIFI:<ssid>;<passwort>
  if (!sawField) {
    String rest = text.substring(5);
    const int sep = rest.indexOf(';');
    if (sep < 0) { ssid = trimCopy(rest); }
    else         { ssid = trimCopy(rest.substring(0, sep)); pass = rest.substring(sep + 1); }
    // Ein abschliessendes Semikolon gehoert nicht zum Passwort
    while (pass.endsWith(";")) pass.remove(pass.length() - 1);
  }

  if (ssid.isEmpty()) return false;
  if (ssidOut) *ssidOut = ssid;
  if (passOut) *passOut = pass;
  return true;
}

// Gegenstueck zu parseWifiText: aus SSID und Passwort einen Kartentext bauen.
// Sonderzeichen werden wie im QR-Schema mit Backslash geschuetzt.
String wifiCardText(const String& ssid, const String& pass) {
  auto escape = [](const String& in) {
    String out;
    for (unsigned i = 0; i < in.length(); ++i) {
      const char c = in[i];
      if (c == '\\' || c == ';' || c == ':' || c == ',' || c == '"') out += '\\';
      out += c;
    }
    return out;
  };
  String text = "WIFI:S:" + escape(ssid) + ";T:";
  text += pass.isEmpty() ? "nopass" : "WPA";
  text += ";P:" + escape(pass) + ";;";
  return text;
}

// Steht eine WLAN-Karte auf dem Leser, ohne dass gerade danach gefragt wurde?
bool textLooksLikeWifi(const String& text) {
  String head = trimCopy(text).substring(0, 5);
  head.toUpperCase();
  return head == "WIFI:";
}

// ---------------------------------------------------------------------------
// /wifi.txt von der SD-Karte
//
//     # Kommentar
//     ssid = MeinWLAN
//     pass = geheim
//
//     ssid = Zweitnetz
//     pass = auchgeheim
//
//     host     = 192.168.0.64
//     hostpass =
//
// Jede neue Zeile "ssid" beginnt einen neuen Eintrag. Die Datei darf bis zu
// kWifiProfileMax Netze enthalten.
// ---------------------------------------------------------------------------
String stripQuotes(const String& value) {
  String v = trimCopy(value);
  if (v.length() >= 2 &&
      ((v.startsWith("\"") && v.endsWith("\"")) || (v.startsWith("'") && v.endsWith("'")))) {
    v = v.substring(1, v.length() - 1);
  }
  return v;
}

size_t loadWifiFromSd(String* errorOut) {
  if (!app.sdReady && !initSd()) {
    if (errorOut) *errorOut = "keine SD-Karte";
    return 0;
  }
  File file = SD.open(kWifiFileSd, FILE_READ);
  if (!file) {
    if (errorOut) *errorOut = "wifi.txt fehlt";
    return 0;
  }

  size_t added = 0;
  String ssid;
  String pass;

  auto flush = [&]() {
    if (!ssid.isEmpty() && wifiAddProfile(ssid, pass, false)) ++added;
    ssid = "";
    pass = "";
  };

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;

    const int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    key.trim();
    key.toLowerCase();
    const String value = stripQuotes(line.substring(eq + 1));

    if (key == "ssid")                              { flush(); ssid = value; }
    else if (key == "pass" || key == "password")    { pass = value; }
    else if (key == "host")                         { if (!value.isEmpty()) gTargetHost = value; }
    else if (key == "hostpass" || key == "hostpassword") { gTargetPass = value; }
  }
  flush();
  file.close();

  saveNetConfig();
  if (added == 0 && errorOut) *errorOut = "keine SSID in wifi.txt";
  return added;
}

// ---------------------------------------------------------------------------
// Gegenstueck zu loadWifiFromSd: alle gespeicherten Netze samt Adresse und
// Passwort des c64u als /wifi.txt auf die SD-Karte schreiben.
//
// Eine bereits vorhandene Datei wird vorher nach /wifi.bak umbenannt, es geht
// also nichts verloren. Die Passwoerter stehen im Klartext in der Datei -
// darauf weist der Kopf der Datei noch einmal hin.
//
// Rueckgabe: Anzahl der geschriebenen Netze, 0 bei einem Fehler.
// ---------------------------------------------------------------------------
size_t saveWifiToSd(String* errorOut) {
  if (gWifiCount == 0) {
    if (errorOut) *errorOut = "nichts gespeichert";
    return 0;
  }
  if (!app.sdReady && !initSd()) {
    if (errorOut) *errorOut = "keine SD-Karte";
    return 0;
  }

  // Vorhandene Datei zur Seite legen. Ein alter Sicherungsstand muss dafuer
  // weichen, sonst schlaegt SD.rename() fehl.
  if (SD.exists(kWifiFileSd)) {
    if (SD.exists(kWifiFileSdBak)) SD.remove(kWifiFileSdBak);
    if (!SD.rename(kWifiFileSd, kWifiFileSdBak)) SD.remove(kWifiFileSd);
  }

  File file = SD.open(kWifiFileSd, FILE_WRITE);
  if (!file) {
    if (errorOut) *errorOut = "schreiben ging nicht";
    return 0;
  }

  file.println("# ---------------------------------------------------------------------------");
  file.println("# C64uRemote - WLAN-Zugangsdaten fuer den M5Stack Core");
  file.println("#");
  file.println("# Vom Geraet geschrieben ueber  Setup > WLAN > Auf SD sichern.");
  file.println("# Zurueckgelesen wird die Datei ueber  Setup > WLAN > Von SD laden");
  file.println("# oder beim Start, solange noch kein Netz im Geraet gespeichert ist.");
  file.println("#");
  file.println("# Achtung: die Passwoerter stehen hier im Klartext.");
  file.println("# ---------------------------------------------------------------------------");
  file.println();

  size_t written = 0;
  for (size_t i = 0; i < gWifiCount; ++i) {
    if (gWifiProfiles[i].ssid.isEmpty()) continue;
    file.print("ssid = ");
    file.println(gWifiProfiles[i].ssid);
    file.print("pass = ");
    file.println(gWifiProfiles[i].pass);
    file.println();
    ++written;
  }

  file.println("# Adresse und Passwort des Ultimate 64 / 1541 Ultimate II+");
  file.print("host     = ");
  file.println(gTargetHost);
  file.print("hostpass = ");
  file.println(gTargetPass);

  file.flush();
  file.close();

  if (written == 0) {
    if (errorOut) *errorOut = "kein Netz zu sichern";
    SD.remove(kWifiFileSd);
    return 0;
  }
  return written;
}

// ---------------------------------------------------------------------------
// Netzsuchlauf. Der Aufruf blockiert einige Sekunden - der Aufrufer zeigt
// vorher einen Hinweis an.
// ---------------------------------------------------------------------------
void wifiRunScan() {
  app.wifiScanCount = 0;
  app.wifiScanIndex = 0;

  if (!app.portalActive && WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

  const int found = WiFi.scanNetworks(false, false);
  for (int i = 0; i < found && app.wifiScanCount < kWifiScanMax; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;

    bool duplicate = false;
    for (size_t k = 0; k < app.wifiScanCount; ++k) {
      if (gWifiScan[k].ssid == ssid) { duplicate = true; break; }
    }
    if (duplicate) continue;

    gWifiScan[app.wifiScanCount].ssid = ssid;
    gWifiScan[app.wifiScanCount].rssi = WiFi.RSSI(i);
    gWifiScan[app.wifiScanCount].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    ++app.wifiScanCount;
  }
  WiFi.scanDelete();
}

// Vor dem ersten Verbindungsversuch das staerkste bekannte Netz heraussuchen.
// Nur sinnvoll, wenn mehr als ein Profil gespeichert ist.
void wifiPickBestProfile() {
  if (gWifiCount < 2) return;

  WiFi.mode(WIFI_STA);
  const int found = WiFi.scanNetworks(false, false);
  int     bestProfile = -1;
  int32_t bestRssi    = -1000;
  for (int i = 0; i < found; ++i) {
    const int profile = wifiProfileIndex(WiFi.SSID(i));
    if (profile >= 0 && WiFi.RSSI(i) > bestRssi) {
      bestRssi    = WiFi.RSSI(i);
      bestProfile = profile;
    }
  }
  WiFi.scanDelete();
  if (bestProfile >= 0) gWifiTry = static_cast<size_t>(bestProfile);
}

// ===========================================================================
//  Setup-Portal: eigener Accesspoint mit kleiner Weboberflaeche
// ===========================================================================
WebServer gPortal(80);
DNSServer gPortalDns;
bool      gPortalRoutesReady = false;

String htmlEscape(const String& raw) {
  String out;
  out.reserve(raw.length() + 8);
  for (unsigned i = 0; i < raw.length(); ++i) {
    const char c = raw[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

const char* kPortalStyle =
    "<style>body{background:#0b1220;color:#dce6f8;font-family:system-ui,sans-serif;"
    "margin:0;padding:18px;}h1{font-size:20px;color:#8ce4ff;margin:0 0 4px;}"
    "p{color:#9cbee4;font-size:13px;margin:4px 0 14px;}"
    "label{display:block;margin:12px 0 4px;font-size:13px;color:#9cbee4;}"
    "input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;"
    "border:1px solid #426084;background:#101a2c;color:#dce6f8;font-size:16px;}"
    "button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;"
    "background:#2f6bb0;color:#fff;font-size:16px;}"
    "ul{padding-left:18px;color:#9cbee4;font-size:13px;}"
    "hr{border:0;border-top:1px solid #24344f;margin:20px 0;}</style>";

void portalSendRoot() {
  app.portalTouchedMs = millis();

  String page;
  page.reserve(3000);
  page += "<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += "<title>C64uRemote WLAN</title>";
  page += kPortalStyle;
  page += "</head><body><h1>C64uRemote</h1>";
  page += "<p>WLAN-Zugangsdaten eintragen. Sie werden im Geraet gespeichert - "
          "der Quelltext muss dafuer nicht mehr uebersetzt werden.</p>";

  page += "<form method=\"POST\" action=\"/save\">";

  page += "<label for=\"ssid\">Gefundene Netze</label>";
  page += "<select id=\"ssid\" name=\"ssid\">";
  page += "<option value=\"\">-- bitte waehlen --</option>";
  for (size_t i = 0; i < app.wifiScanCount; ++i) {
    page += "<option value=\"" + htmlEscape(gWifiScan[i].ssid) + "\">";
    page += htmlEscape(gWifiScan[i].ssid);
    page += " (" + String(static_cast<int>(gWifiScan[i].rssi)) + " dBm)";
    page += "</option>";
  }
  page += "</select>";

  page += "<label for=\"ssid2\">oder SSID von Hand</label>";
  page += "<input id=\"ssid2\" name=\"ssid2\" maxlength=\"32\" autocomplete=\"off\">";

  page += "<label for=\"pass\">WLAN-Passwort</label>";
  page += "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"63\">";

  page += "<hr><label for=\"host\">c64u-Adresse (optional)</label>";
  page += "<input id=\"host\" name=\"host\" value=\"" + htmlEscape(gTargetHost) + "\">";
  page += "<label for=\"hpass\">c64u-Passwort (optional)</label>";
  page += "<input id=\"hpass\" name=\"hpass\" type=\"password\" maxlength=\"63\">";

  page += "<button type=\"submit\">Speichern und verbinden</button></form>";

  page += "<hr><p>Gespeicherte Netze (" + String(static_cast<unsigned>(gWifiCount)) + "/" +
          String(static_cast<unsigned>(kWifiProfileMax)) + "):</p><ul>";
  if (gWifiCount == 0) page += "<li>noch keins</li>";
  for (size_t i = 0; i < gWifiCount; ++i) page += "<li>" + htmlEscape(gWifiProfiles[i].ssid) + "</li>";
  page += "</ul></body></html>";

  gPortal.send(200, "text/html; charset=utf-8", page);
}

void portalSendSaved() {
  String ssid = trimCopy(gPortal.arg("ssid2"));
  if (ssid.isEmpty()) ssid = trimCopy(gPortal.arg("ssid"));
  const String pass  = gPortal.arg("pass");
  const String host  = trimCopy(gPortal.arg("host"));
  const String hpass = gPortal.arg("hpass");

  bool ok = false;
  if (!ssid.isEmpty()) ok = wifiAddProfile(ssid, pass, false);
  if (!host.isEmpty())  gTargetHost = host;
  if (!hpass.isEmpty()) gTargetPass = hpass;
  saveNetConfig();

  String page;
  page.reserve(900);
  page += "<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += kPortalStyle;
  page += "</head><body><h1>";
  page += ok ? "Gespeichert" : "Nichts gespeichert";
  page += "</h1><p>";
  if (ok) {
    page += "Das Geraet schaltet den Accesspoint jetzt ab und verbindet sich mit \"";
    page += htmlEscape(ssid);
    page += "\". Diese Verbindung bricht dabei ab - das ist normal.";
  } else {
    page += "Es war keine SSID angegeben. <a style=\"color:#8ce4ff\" href=\"/\">Zurueck</a>";
  }
  page += "</p></body></html>";

  gPortal.send(200, "text/html; charset=utf-8", page);

  app.portalTouchedMs = millis();
  if (ok) app.portalCloseAtMs = millis() + kPortalCloseMs;
}

void portalRedirect() {
  app.portalTouchedMs = millis();
  gPortal.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  gPortal.send(302, "text/plain", "");
}

void startPortal(uint32_t now) {
  if (app.portalActive) return;

  // Der Accesspoint laeuft bewusst ohne Station-Teil. Bleibt die STA aktiv,
  // sucht sie im Hintergrund weiter nach dem gespeicherten Netz - dabei
  // wechselt der Funkkanal, und angemeldete Handys fliegen nach wenigen
  // Sekunden wieder raus ("Portal beendet sich von selbst").
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(60);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);                       // kein Modem-Sleep im AP-Betrieb
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  // Fester Kanal 1, damit der AP nicht mehr umgeschaltet werden kann.
  WiFi.softAP(kPortalSsid, kPortalPass, 1 /*Kanal*/, 0 /*sichtbar*/, 4 /*Clients*/);
  delay(120);

  if (!gPortalRoutesReady) {
    gPortal.on("/", HTTP_GET, portalSendRoot);
    gPortal.on("/save", HTTP_POST, portalSendSaved);
    gPortal.onNotFound(portalRedirect);
    gPortalRoutesReady = true;
  }
  gPortalDns.setErrorReplyCode(DNSReplyCode::NoError);
  gPortalDns.start(kPortalDnsPort, "*", WiFi.softAPIP());
  gPortal.begin();

  app.portalActive    = true;
  app.portalTouchedMs = now;
  app.portalCloseAtMs = 0;
  Serial.printf("Setup-Portal aktiv: SSID %s, IP %s, Heap %u\n", kPortalSsid,
                WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

void stopPortal(uint32_t now) {
  if (!app.portalActive) return;
  gPortal.stop();
  gPortalDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);    // beim Portalstart abgeschaltet
  app.portalActive    = false;
  app.portalCloseAtMs = 0;
  app.lastWiFiAttemptMs = 0;      // sofort neu verbinden
  beginWiFi(now);
  Serial.println("Setup-Portal beendet");
}

void servicePortal(uint32_t now) {
  if (!app.portalActive) return;

  gPortalDns.processNextRequest();
  gPortal.handleClient();

  // Solange ein Geraet angemeldet ist, laeuft die Leerlaufuhr nicht weiter.
  if (WiFi.softAPgetStationNum() > 0) app.portalTouchedMs = now;

  if (app.portalCloseAtMs != 0 &&
      static_cast<int32_t>(now - app.portalCloseAtMs) >= 0) {
    stopPortal(now);
    // Ohne Wechsel bliebe die Portalseite mit SSID und Passwort stehen,
    // obwohl der Accesspoint schon abgeschaltet ist.
    if (app.screen == ScreenMode::WifiPortal) setScreen(ScreenMode::WifiMenu, now);
    setModal("WLAN GESPEICHERT", kColOk, now, 2000);
    return;
  }
  if (now - app.portalTouchedMs >= kPortalIdleMs) {
    stopPortal(now);
    if (app.screen == ScreenMode::WifiPortal) setScreen(ScreenMode::WifiMenu, now);
    setModal("PORTAL BEENDET", kColWarn, now, 1800);
  }
}

// ===========================================================================
//  Datei an den c64u schicken und starten
// ===========================================================================
void startFileOnC64(const String& fullPath, uint32_t now) {
  const String ext  = lowerExt(fullPath);
  const String name = baseName(fullPath);

  if (!requireNetwork(now)) return;
  if (!isSupportedExt(ext)) {
    Serial.printf("Nicht unterstuetzt: '%s'  (Endung '%s')\n", fullPath.c_str(), ext.c_str());
    app.rfidHint = name.isEmpty() ? String("Pfad leer") : name;
    setModal(ext.isEmpty() ? String("KEINE DATEIENDUNG") : ("TYP UNBEKANNT: " + ext),
             kColErr, now, 2600);
    return;
  }

  if (!app.sdReady && !initSd()) { setModal("KEINE SD-KARTE", kColErr, now, 2000); return; }

  File file = SD.open(fullPath, FILE_READ);
  if (!file) { setModal("DATEI FEHLT", kColErr, now, 2200); return; }
  if (file.isDirectory()) { file.close(); setModal("IST EIN ORDNER", kColErr, now, 2000); return; }

  app.returnScreen = app.screen;
  app.screen       = ScreenMode::Busy;
  app.busyTitle    = "Upload";
  app.busyDetail   = name;
  app.busySent     = 0;
  app.busyTotal    = file.size();
  drawBusyScreen();
  pushFrame();

  UploadResult result;
  const bool diskImage = isDiskImageExt(ext);
  String targetDrive;

  if (diskImage) {
    targetDrive = resolveTargetDrive();
    app.busyDetail = name + "  ->  Laufwerk " + targetDrive;
    drawBusyScreen();
    pushFrame();
    // type und mode als Query-Argumente, die Datei als einziger Multipart-Teil
    const String mountUrl = "/v1/drives/" + targetDrive + ":mount"
                            + "?type=" + ext + "&mode=" + kMountMode;
    Serial.println(mountUrl.c_str());
    result = uploadFile(mountUrl, file, name, "file");
  } else if (ext == "prg") {
    result = uploadFile("/v1/runners:run_prg", file, name, "");
  } else if (ext == "crt") {
    result = uploadFile("/v1/runners:run_crt", file, name, "");
  } else if (ext == "sid") {
    result = uploadFile("/v1/runners:sidplay", file, name, "");
  } else {  // mod
    result = uploadFile("/v1/runners:modplay", file, name, "");
  }
  file.close();

  const uint32_t after = millis();
  if (!result.ok) {
    beep(500, 160);
    app.screen = app.returnScreen;
    app.screenDirty = true;
    app.home.dirty  = true;
    setModal(result.message, kColErr, after, 2600);
    return;
  }

  // ---- Disk-Image: Laufwerk aktivieren und erstes Programm starten ----
  if (diskImage) {
    app.busyTitle  = "Laufwerk 8";
    app.busySent   = 0;
    app.busyTotal  = 0;
    app.busyDetail = "Laufwerk einschalten";
    drawBusyScreen();
    pushFrame();

    // ":on" schaltet das Laufwerk ein bzw. setzt es zurueck. Damit liegt das
    // gerade gemountete Image auch dann am IEC-Bus, wenn Laufwerk A vorher
    // deaktiviert war.
    sendApiRequest("PUT", "/v1/drives/" + targetDrive + ":on", true);

    if (app.settings.diskAction != DiskActionMode::Mount) {
      app.busyDetail = "Reset";
      drawBusyScreen();
      pushFrame();
      sendApiRequest("PUT", "/v1/machine:reset", true);

      if (app.settings.diskAction == DiskActionMode::MountRun) {
        // Auf den BASIC-Prompt warten, dann LOAD"*",8,1 in den
        // Tastaturpuffer schreiben.
        waitCursorBlinking(true, 15000, "warte auf READY");
        app.busyDetail = "LOAD \"*\",8,1";
        drawBusyScreen();
        pushFrame();
        typeLoadFirstFile();

        // Ladevorgang abwarten: erst muss der Cursor aufhoeren zu blinken
        // (LOAD laeuft), danach blinkt er wieder (READY) - dann kommt RUN.
        waitCursorBlinking(false, 6000, "starte LOAD");
        if (waitCursorBlinking(true, 180000, "lade von Diskette")) {
          app.busyDetail = "RUN";
          drawBusyScreen();
          pushFrame();
          typeRun();
        } else {
          beep(700, 140);
          app.screen      = app.returnScreen;
          app.screenDirty = true;
          app.home.dirty  = true;
          setModal("LADEN DAUERT ZU LANGE", kColWarn, millis(), 2600);
          return;
        }
      }
    }
  }

  beep(2800, 60);
  app.screen      = app.returnScreen;
  app.screenDirty = true;
  app.home.dirty  = true;
  setModal(String(diskImage ? "LAEUFT: " : "GESTARTET: ") + name, kColOk, millis(), 2000);
}

// ===========================================================================
//  RFID2 (WS1850S) - NFC-Karten lesen und beschreiben
// ---------------------------------------------------------------------------
//  Das Kartenformat ist bewusst identisch zu dem des TeensyROM NFC-Loaders
//  bzw. des Zaparoo/TapTo-Projekts, damit dieselbe Karte an beiden Systemen
//  funktioniert:
//
//      Ein einzelner NDEF-Record, Typ "Text" (Well Known, UTF-8),
//      Inhalt = Pfad zur Programmdatei, z. B.  SD:OneLoad v5/Bubble Bobble.crt
//
//  Erlaubte Praefixe: "SD:", "USB:", "TR:" oder gar keins (dann gilt SD).
//  Ein "?" als Dateiname startet eine zufaellige Datei aus dem Verzeichnis.
//
//  Ablage auf der Karte:
//
//  A) NTAG213/215/216, MIFARE Ultralight (SAK 0x00, 4-Byte-Seiten, kein
//     Schluessel): NDEF-TLV ab Seite 4. Seiten 0..3 (UID, Lock-Bytes,
//     Capability Container) bleiben unberuehrt.
//
//  B) MIFARE Classic 1K/4K/Mini (16-Byte-Bloecke): NDEF-TLV in den
//     Datenbloecken ab Block 4 (Sektor-Trailer werden uebersprungen).
//     Authentifiziert wird zuerst mit dem NDEF-Schluessel D3F7D3F7D3F7,
//     ersatzweise mit dem Werksschluessel FFFFFFFFFFFF. Trailer und MAD
//     werden nie geschrieben - eine Karte kann dadurch nicht unbrauchbar
//     werden, eine unformatierte Classic-Karte ist dann aber ggf. nur an
//     diesem Geraet lesbar.
//
//  Zusaetzlich wird beim Lesen weiterhin das alte Rohformat "C64UPATH"
//  erkannt, damit bereits beschriebene Karten weiter funktionieren.
// ===========================================================================
constexpr size_t  kMaxTextLen   = 246;   // wie TeensyROM
constexpr size_t  kNdefBufSize  = 288;   // TLV + Record + Text + Reserve
constexpr uint8_t kUlDataPage   = 4;     // NDEF beginnt auf Seite 4
constexpr uint8_t kMagic[8]     = {'C', '6', '4', 'U', 'P', 'A', 'T', 'H'};

enum class CardKind : uint8_t { None, Classic, Ultralight };

// MIFARE-Classic-Schluessel: erst der NDEF-Standard, dann der Werksschluessel
MFRC522_I2C::MIFARE_Key kKeyNdef    = {{0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}};
MFRC522_I2C::MIFARE_Key kKeyFactory = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
bool gClassicNdefFormatted = false;      // Ergebnis der letzten Authentifizierung
bool gClassicTryNdefFirst  = true;       // pro Karte zurueckgesetzt

uint8_t trailerForBlock(uint8_t block) { return static_cast<uint8_t>((block / 4) * 4 + 3); }

// Linearer Index -> Datenblock, Sektor-Trailer werden ausgelassen.
// 0->4, 1->5, 2->6, 3->8, 4->9, 5->10, 6->12 ...
uint8_t classicDataBlock(size_t index) {
  const size_t sector = 1 + index / 3;
  return static_cast<uint8_t>(sector * 4 + (index % 3));
}

// Nach einem fehlgeschlagenen Auth ist die Karte im HALT-Zustand und
// antwortet nur noch auf WUPA - REQA (PICC_IsNewCardPresent) findet sie dann
// nicht mehr. Deshalb hier gezielt aufwecken und neu auswaehlen.
bool reselectCard() {
  uint8_t atqa[2];
  uint8_t size = sizeof(atqa);
  if (rfid.PICC_WakeupA(atqa, &size) != MFRC522_I2C::STATUS_OK) return false;
  return rfid.PICC_Select(&(rfid.uid), 0) == MFRC522_I2C::STATUS_OK;
}

bool classicAuth(uint8_t block) {
  const uint8_t trailer = trailerForBlock(block);

  // Den zuletzt erfolgreichen Schluessel zuerst probieren, sonst kostet jeder
  // Block einen unnoetigen Fehlversuch samt Neuauswahl.
  MFRC522_I2C::MIFARE_Key* keys[2] = {
      gClassicTryNdefFirst ? &kKeyNdef : &kKeyFactory,
      gClassicTryNdefFirst ? &kKeyFactory : &kKeyNdef};

  for (int i = 0; i < 2; ++i) {
    if (rfid.PCD_Authenticate(MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A, trailer, keys[i],
                              &(rfid.uid)) == MFRC522_I2C::STATUS_OK) {
      gClassicNdefFormatted = (keys[i] == &kKeyNdef);
      gClassicTryNdefFirst  = gClassicNdefFormatted;
      return true;
    }
    rfid.PCD_StopCrypto1();
    if (!reselectCard()) return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Schluessel-Woerterbuch fuer das Klonen fremder MIFARE-Classic-Karten
// ---------------------------------------------------------------------------
const uint8_t kKeyDict[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},   // Werksschluessel
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},   // MAD / NDEF Sektor 0
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},   // NDEF-Daten
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5},
    {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A},
    {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97},
    {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91},
    {0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6},
    {0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9},
};
constexpr size_t kKeyDictCount = sizeof(kKeyDict) / sizeof(kKeyDict[0]);

// Trailer-Block eines beliebigen Blocks (1K/4K)
uint8_t blockTrailer(uint16_t block) {
  if (block < 128) return static_cast<uint8_t>((block / 4) * 4 + 3);
  const uint16_t b = block - 128;
  return static_cast<uint8_t>(128 + (b / 16) * 16 + 15);
}
bool blockIsTrailer(uint16_t block) { return blockTrailer(block) == block; }

// Prueft, ob die Zugriffsbits eines Trailers (Bytes 6,7,8) in sich stimmig
// sind. Ein ungueltiges Muster wuerde den Sektor beim Schreiben unwiderruflich
// sperren - solche Trailer werden daher NICHT geschrieben.
bool validAccessBits(uint8_t b6, uint8_t b7, uint8_t b8) {
  const uint8_t c1i = b6 & 0x0F;          // ~C1
  const uint8_t c2i = b6 >> 4;            // ~C2
  const uint8_t c1  = b7 >> 4;            //  C1
  const uint8_t c3i = b7 & 0x0F;          // ~C3
  const uint8_t c3  = b8 >> 4;            //  C3
  const uint8_t c2  = b8 & 0x0F;          //  C2
  return c1i == static_cast<uint8_t>(~c1 & 0x0F) &&
         c2i == static_cast<uint8_t>(~c2 & 0x0F) &&
         c3i == static_cast<uint8_t>(~c3 & 0x0F);
}

// Authentifiziert den Sektor eines Trailer-Blocks per Woerterbuch.
// Liefert den gefundenen Schluessel und den Typ (A/B) zurueck.
bool classicAuthDict(uint8_t trailer, uint8_t* keyOut, char* typeOut) {
  const uint8_t cmds[2] = {MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A,
                           MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B};
  const char letters[2] = {'A', 'B'};

  for (size_t i = 0; i < kKeyDictCount; ++i) {
    MFRC522_I2C::MIFARE_Key key;
    memcpy(key.keyByte, kKeyDict[i], 6);
    for (int t = 0; t < 2; ++t) {
      if (rfid.PCD_Authenticate(cmds[t], trailer, &key, &(rfid.uid)) ==
          MFRC522_I2C::STATUS_OK) {
        if (keyOut) memcpy(keyOut, kKeyDict[i], 6);
        if (typeOut) *typeOut = letters[t];
        return true;
      }
      rfid.PCD_StopCrypto1();
      if (!reselectCard()) return false;   // Karte weg
    }
  }
  return false;
}

bool rfidReadBlock(uint8_t block, uint8_t* out16) {
  if (!classicAuth(block)) return false;
  uint8_t buffer[18];
  uint8_t size = sizeof(buffer);
  if (rfid.MIFARE_Read(block, buffer, &size) != MFRC522_I2C::STATUS_OK) return false;
  memcpy(out16, buffer, 16);
  return true;
}

bool rfidWriteBlock(uint8_t block, const uint8_t* data16) {
  if (!classicAuth(block)) return false;
  return rfid.MIFARE_Write(block, const_cast<uint8_t*>(data16), 16) == MFRC522_I2C::STATUS_OK;
}

void rfidRelease() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

bool cardPresent() {
  const bool found = rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial();
  if (found) gClassicTryNdefFirst = true;   // bei jeder neuen Karte neu ermitteln
  return found;
}

// ---------------------------------------------------------------------------
// Schnelle Anwesenheitsprobe fuer die Hintergrundabfrage
//
// Liegt keine Karte auf, wartet der MFRC522 nach dem REQA-Kommando, bis sein
// interner Timer ablaeuft - PCD_Init() stellt dafuer 0x03E8 = 1000 Schritte
// zu je 25 us ein, also 25 ms. Genau so lange haengt die Hauptschleife.
//
// Eine Karte antwortet aber weit schneller (Frame Delay Time bei 106 kBit/s:
// rund 86 us). Fuer die reine Probe wird das Zeitfenster deshalb auf ~2 ms
// verkuerzt und unmittelbar danach - noch vor der Kartenauswahl - wieder auf
// den Ausgangswert gesetzt. Authentifizierung, Lesen und Schreiben laufen
// damit unveraendert mit dem vollen Zeitfenster.
// ---------------------------------------------------------------------------
uint16_t gRfidTimerReload = 0x03E8;         // in initRfid() vom Chip gelesen
constexpr uint16_t kRfidProbeReload = 80;   // 80 * 25 us = 2 ms

void setRfidTimerReload(uint16_t ticks) {
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegH, static_cast<byte>(ticks >> 8));
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegL, static_cast<byte>(ticks & 0xFF));
}

bool cardPresentQuick() {
  setRfidTimerReload(kRfidProbeReload);
  const bool present = rfid.PICC_IsNewCardPresent();
  setRfidTimerReload(gRfidTimerReload);     // vor der Auswahl zurueckstellen
  if (!present) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;
  gClassicTryNdefFirst = true;
  return true;
}

CardKind cardKind() {
  const uint8_t type = rfid.PICC_GetType(rfid.uid.sak);
  if (type == MFRC522_I2C::PICC_TYPE_MIFARE_MINI ||
      type == MFRC522_I2C::PICC_TYPE_MIFARE_1K ||
      type == MFRC522_I2C::PICC_TYPE_MIFARE_4K) {
    return CardKind::Classic;
  }
  // NTAG213/215/216 und Ultralight melden sich alle mit SAK 0x00
  if (type == MFRC522_I2C::PICC_TYPE_MIFARE_UL) return CardKind::Ultralight;
  return CardKind::None;
}

const char* cardKindLabel(CardKind kind) {
  switch (kind) {
    case CardKind::Classic:    return "MIFARE Classic";
    case CardKind::Ultralight: return "NTAG / Ultralight";
    default:                   return "unbekannt";
  }
}

// ---- Ultralight / NTAG: 4 Byte pro Seite --------------------------------
// MIFARE_Read liefert immer 16 Byte, also vier Seiten auf einmal.
bool ulRead16(uint8_t page, uint8_t* out16) {
  uint8_t buffer[18];
  uint8_t size = sizeof(buffer);
  if (rfid.MIFARE_Read(page, buffer, &size) != MFRC522_I2C::STATUS_OK) return false;
  memcpy(out16, buffer, 16);
  return true;
}

bool ulWritePage(uint8_t page, const uint8_t* data4) {
  return rfid.MIFARE_Ultralight_Write(page, const_cast<uint8_t*>(data4), 4) ==
         MFRC522_I2C::STATUS_OK;
}

bool ulWrite16(uint8_t firstPage, const uint8_t* data16) {
  for (uint8_t i = 0; i < 4; ++i) {
    if (!ulWritePage(static_cast<uint8_t>(firstPage + i), data16 + i * 4)) return false;
  }
  return true;
}

String cardUidString() {
  String uid;
  char buf[4];
  for (uint8_t i = 0; i < rfid.uid.size; ++i) {
    snprintf(buf, sizeof(buf), "%02X", rfid.uid.uidByte[i]);
    uid += buf;
  }
  return uid;
}

// GET_VERSION (0x60) - liefert bei NTAG21x acht Bytes mit Hersteller,
// Produkttyp und Speichergroesse. Aeltere Ultralight kennen den Befehl nicht.
bool ulGetVersion(uint8_t* out8) {
  uint8_t cmd[3] = {0x60, 0, 0};
  if (rfid.PCD_CalculateCRC(cmd, 1, &cmd[1]) != MFRC522_I2C::STATUS_OK) return false;

  uint8_t back[10];
  uint8_t backLen = sizeof(back);
  if (rfid.PCD_TransceiveData(cmd, 3, back, &backLen, nullptr, 0, true) !=
      MFRC522_I2C::STATUS_OK) {
    return false;
  }
  if (backLen < 8) return false;
  memcpy(out8, back, 8);
  return true;
}

// Produktname aus dem Storage-Size-Byte der GET_VERSION-Antwort
const char* ntagNameFromStorage(uint8_t storage) {
  switch (storage) {
    case 0x0B: return "NTAG210";
    case 0x0E: return "NTAG212";
    case 0x0F: return "NTAG213";
    case 0x11: return "NTAG215";
    case 0x13: return "NTAG216";
    default:   return "NTAG/UL";
  }
}

uint16_t ntagBytesFromStorage(uint8_t storage) {
  switch (storage) {
    case 0x0B: return 48;
    case 0x0E: return 128;
    case 0x0F: return 144;
    case 0x11: return 504;
    case 0x13: return 888;
    default:   return 0;
  }
}

String hexBytes(const uint8_t* data, size_t len, size_t maxLen = 16) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  const size_t n = std::min(len, maxLen);
  out.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
    if (i + 1 < n) out += ' ';
  }
  if (len > n) out += " ...";
  return out;
}

// ---------------------------------------------------------------------------
// Zugriff auf den NDEF-Datenbereich, 16 Byte am Stueck
// ---------------------------------------------------------------------------
bool cardReadChunk(CardKind kind, size_t chunk, uint8_t* out16) {
  if (kind == CardKind::Classic) return rfidReadBlock(classicDataBlock(chunk), out16);
  return ulRead16(static_cast<uint8_t>(kUlDataPage + chunk * 4), out16);
}

bool cardWriteChunk(CardKind kind, size_t chunk, const uint8_t* data16) {
  if (kind == CardKind::Classic) return rfidWriteBlock(classicDataBlock(chunk), data16);
  return ulWrite16(static_cast<uint8_t>(kUlDataPage + chunk * 4), data16);
}

// ---------------------------------------------------------------------------
// NDEF: ein einzelner Text-Record (Well Known, UTF-8, Sprache "en")
//
//   TLV      : 03 <len> ... FE
//   Record   : D1 01 <plen> 54 | 02 'e' 'n' | <text>
//              D1 = MB|ME|SR|TNF=1 (Well Known), 54 = 'T'
//              Statusbyte 02 = UTF-8, Sprachcode zwei Zeichen
// ---------------------------------------------------------------------------
size_t buildNdefText(const String& text, uint8_t* out, size_t cap) {
  const size_t textLen    = text.length();
  const size_t payloadLen = 3 + textLen;          // Status + "en" + Text
  const size_t recordLen  = 4 + payloadLen;       // Header + Typlaenge + Laenge + Typ
  const size_t total      = 2 + recordLen + 1;    // TLV-Kopf + Record + Terminator
  if (payloadLen > 255 || total > cap) return 0;

  size_t i = 0;
  out[i++] = 0x03;                                       // TLV: NDEF-Nachricht
  out[i++] = static_cast<uint8_t>(recordLen);
  out[i++] = 0xD1;                                       // MB|ME|SR|TNF=Well Known
  out[i++] = 0x01;                                       // Typlaenge
  out[i++] = static_cast<uint8_t>(payloadLen);
  out[i++] = 'T';                                        // Typ "Text"
  out[i++] = 0x02;                                       // UTF-8, Sprachcode 2 Zeichen
  out[i++] = 'e';
  out[i++] = 'n';
  for (size_t k = 0; k < textLen; ++k) out[i++] = static_cast<uint8_t>(text[k]);
  out[i++] = 0xFE;                                       // TLV: Ende

  // Auf ein Vielfaches von 16 auffuellen, damit ganze Bloecke geschrieben werden
  while (i % 16 != 0 && i < cap) out[i++] = 0x00;
  return i;
}

// Sucht im Datenstrom den ersten Text-Record und gibt seinen Inhalt zurueck.
bool parseNdefText(const uint8_t* data, size_t len, String* out) {
  size_t i = 0;

  // TLV-Kette abklappern, bis die NDEF-Nachricht kommt
  size_t msgStart = 0;
  size_t msgLen   = 0;
  while (i < len) {
    const uint8_t tag = data[i++];
    if (tag == 0x00) continue;                     // NULL-TLV
    if (tag == 0xFE) return false;                 // Ende ohne Nachricht
    if (i >= len) return false;

    size_t tlvLen = data[i++];
    if (tlvLen == 0xFF) {                          // 3-Byte-Laenge
      if (i + 1 >= len) return false;
      tlvLen = (static_cast<size_t>(data[i]) << 8) | data[i + 1];
      i += 2;
    }
    if (tag == 0x03) { msgStart = i; msgLen = tlvLen; break; }
    i += tlvLen;                                   // anderes TLV ueberspringen
  }
  if (msgLen == 0 || msgStart + msgLen > len) return false;

  // Records der Nachricht durchgehen
  size_t p = msgStart;
  const size_t end = msgStart + msgLen;
  while (p < end) {
    const uint8_t header = data[p++];
    const bool shortRec = (header & 0x10) != 0;
    const bool hasId    = (header & 0x08) != 0;
    const uint8_t tnf   = header & 0x07;
    if (p >= end) return false;

    const uint8_t typeLen = data[p++];
    size_t payloadLen = 0;
    if (shortRec) {
      if (p >= end) return false;
      payloadLen = data[p++];
    } else {
      if (p + 3 >= end) return false;
      payloadLen = (static_cast<size_t>(data[p]) << 24) | (static_cast<size_t>(data[p + 1]) << 16) |
                   (static_cast<size_t>(data[p + 2]) << 8) | data[p + 3];
      p += 4;
    }
    uint8_t idLen = 0;
    if (hasId) {
      if (p >= end) return false;
      idLen = data[p++];
    }

    const size_t typePos    = p;
    const size_t payloadPos = p + typeLen + idLen;

    // Manche Schreiber tragen eine falsche Payload-Laenge ein - der
    // TeensyROM schreibt dort z. B. konstant 0x10, obwohl die TLV-Laenge
    // korrekt ist. Beim letzten Record (ME-Flag) hat deshalb die TLV-Laenge
    // Vorrang, sie ist die verlaesslichere Angabe.
    if ((header & 0x40) != 0 && payloadPos < end) {
      const size_t fromTlv = end - payloadPos;
      if (fromTlv != payloadLen) payloadLen = fromTlv;
    }
    if (payloadPos + payloadLen > len) {
      if (payloadPos >= len) return false;
      payloadLen = len - payloadPos;          // lieber kuerzen als aufgeben
    }

    // Well Known "T" = Text-Record
    if (tnf == 0x01 && typeLen == 1 && data[typePos] == 'T' && payloadLen >= 1) {
      const uint8_t status  = data[payloadPos];
      const uint8_t langLen = status & 0x3F;
      if (payloadLen > static_cast<size_t>(1 + langLen)) {
        const size_t textPos = payloadPos + 1 + langLen;
        const size_t textLen = payloadLen - 1 - langLen;
        String text;
        text.reserve(textLen + 1);
        for (size_t k = 0; k < textLen; ++k) {
          const uint8_t b = data[textPos + k];
          if (b == 0x00 || b == 0xFE) break;   // Fuellbytes / TLV-Ende
          text += static_cast<char>(b);
        }
        *out = text;
        return true;
      }
    }

    p = payloadPos + payloadLen;
    if ((header & 0x40) != 0) break;               // ME: letzter Record
  }
  return false;
}

// ---------------------------------------------------------------------------
// Karteninhalt lesen: erst NDEF, ersatzweise das alte Rohformat "C64UPATH"
// ---------------------------------------------------------------------------
struct CardContent {
  bool   ok       = false;
  bool   isNdef   = false;
  bool   isLegacy = false;
  String text;                 // Rohtext von der Karte (mit Praefix)
  String error;
  uint8_t raw[16]  = {0};      // erste 16 Byte, fuer die Infoanzeige
  uint8_t raw2[16] = {0};      // die naechsten 16 Byte
};

CardContent readCardContent(CardKind kind) {
  CardContent result;
  if (kind == CardKind::None) {
    result.error = "Kartentyp nicht unterstuetzt";
    return result;
  }

  static uint8_t buffer[kNdefBufSize];
  memset(buffer, 0, sizeof(buffer));

  if (!cardReadChunk(kind, 0, buffer)) {
    result.error = (kind == CardKind::Classic) ? "Block 4 nicht lesbar (Schluessel?)"
                                               : "Seite 4 nicht lesbar";
    return result;
  }
  memcpy(result.raw, buffer, 16);
  if (cardReadChunk(kind, 1, buffer + 16)) memcpy(result.raw2, buffer + 16, 16);

  // Altes Rohformat?
  if (memcmp(buffer, kMagic, sizeof(kMagic)) == 0) {
    const uint8_t len = buffer[9];
    if (len == 0 || len > 128) {
      result.error = "Laenge ungueltig";
      return result;
    }
    String path;
    path.reserve(len + 1);
    // Das alte Format lag in den Chunks 1..8 hinter dem Kopf
    static const uint8_t legacyBlocks[] = {5, 6, 8, 9, 10, 12, 13, 14};
    size_t remaining = len;
    for (size_t i = 0; i < 8 && remaining > 0; ++i) {
      uint8_t data[16] = {0};
      const bool ok = (kind == CardKind::Classic)
                          ? rfidReadBlock(legacyBlocks[i], data)
                          : ulRead16(static_cast<uint8_t>(8 + i * 4), data);
      if (!ok) {
        result.error = "Lesefehler (Altformat)";
        return result;
      }
      const size_t take = std::min<size_t>(16, remaining);
      for (size_t k = 0; k < take; ++k) path += static_cast<char>(data[k]);
      remaining -= take;
    }
    result.ok       = true;
    result.isLegacy = true;
    result.text     = path;
    return result;
  }

  // NDEF: Laenge aus dem TLV holen und nur so viel nachladen wie noetig
  size_t needed = sizeof(buffer);
  if (buffer[0] == 0x03) {
    needed = (buffer[1] == 0xFF)
                 ? 4 + ((static_cast<size_t>(buffer[2]) << 8) | buffer[3])
                 : 2 + static_cast<size_t>(buffer[1]) + 1;
    needed = std::min(needed, sizeof(buffer));
  }

  size_t have = 16;
  for (size_t chunk = 1; have < needed; ++chunk) {
    if (!cardReadChunk(kind, chunk, buffer + have)) break;
    have += 16;
    if (have + 16 > sizeof(buffer)) break;
  }

  String text;
  if (parseNdefText(buffer, have, &text)) {
    result.ok     = true;
    result.isNdef = true;
    result.text   = text;
    return result;
  }

  result.error = (buffer[0] == 0x03) ? "NDEF ohne Text-Record" : "kein NDEF-Text";
  return result;
}

// ---------------------------------------------------------------------------
// Karte beschreiben: immer als NDEF-Text-Record
// ---------------------------------------------------------------------------
bool writeCardText(CardKind kind, const String& text, String* errorOut) {
  if (kind == CardKind::None) {
    if (errorOut) *errorOut = "Kartentyp nicht unterstuetzt";
    return false;
  }
  if (text.isEmpty() || text.length() > kMaxTextLen) {
    if (errorOut) *errorOut = "Text zu lang (max " + String(kMaxTextLen) + ")";
    return false;
  }

  static uint8_t buffer[kNdefBufSize];
  const size_t total = buildNdefText(text, buffer, sizeof(buffer));
  if (total == 0) {
    if (errorOut) *errorOut = "NDEF passt nicht";
    return false;
  }

  const size_t chunks = total / 16;
  for (size_t chunk = 0; chunk < chunks; ++chunk) {
    if (!cardWriteChunk(kind, chunk, buffer + chunk * 16)) {
      if (errorOut) {
        *errorOut = (chunk == 0) ? "Karte nicht beschreibbar"
                                 : "Karte zu klein ab Block " + String(chunk);
      }
      return false;
    }
  }

  // Kontrolle: zurueklesen und vergleichen
  const CardContent check = readCardContent(kind);
  if (!check.ok || check.text != text) {
    if (errorOut) *errorOut = check.ok ? "Kontrolle abweichend" : check.error;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Karte auf die SD sichern und von dort wieder zurueckschreiben
//
//  Dateiformat (Textdatei, /NFC-DUMPS/<uid>.nfc):
//      # C64uRemote NFC-Dump
//      type NTAG215
//      uid  04 01 A1 01 C1 47 03
//      sak  00
//      P4   03 27 D1 01            <- Ultralight: 4 Byte je Seite
//      B4   00 11 ... (16 Byte)    <- Classic: 16 Byte je Block
//
//  Beim Zurueckschreiben werden nur die Nutzdaten angefasst:
//    * Ultralight/NTAG ab Seite 4, hoechstens bis zum Ende des Nutzbereichs.
//      Seiten 0-3 (UID, Lock, CC) und die Konfigurationsseiten bleiben tabu -
//      dort liesse sich eine Karte dauerhaft sperren.
//    * Classic nur Datenbloecke, keine Sektor-Trailer und nicht Block 0.
//  Die UID selbst ist fest im Chip, eine echte 1:1-Kopie ist damit nicht
//  moeglich - der Inhalt aber schon, und darauf kommt es hier an.
// ---------------------------------------------------------------------------
constexpr const char* kDumpDir = "/NFC-DUMPS";

// Letzte beschreibbare Nutzseite je NTAG-Typ
uint16_t ntagLastUserPage(uint8_t storage) {
  switch (storage) {
    case 0x0F: return 39;    // NTAG213
    case 0x11: return 129;   // NTAG215
    case 0x13: return 225;   // NTAG216
    default:   return 39;    // im Zweifel der kleinste Bereich
  }
}

bool dumpCardToSd(CardKind kind, String* fileOut, String* errorOut) {
  if (!app.sdReady && !initSd()) {
    if (errorOut) *errorOut = "keine SD-Karte";
    return false;
  }
  if (!SD.exists(kDumpDir) && !SD.mkdir(kDumpDir)) {
    if (errorOut) *errorOut = "Ordner NFC-DUMPS fehlt";
    return false;
  }

  const String uid = cardUidString();
  const String path = String(kDumpDir) + "/" + uid + ".nfc";

  File out = SD.open(path, FILE_WRITE);
  if (!out) {
    if (errorOut) *errorOut = "Datei nicht anlegbar";
    return false;
  }

  uint8_t version[8] = {0};
  const bool haveVersion = (kind == CardKind::Ultralight) && ulGetVersion(version);
  if (kind == CardKind::Ultralight && !haveVersion) reselectCard();

  out.println("# C64uRemote NFC-Dump");
  out.print("type ");
  out.println(kind == CardKind::Classic ? cardKindLabel(kind)
                                        : (haveVersion ? ntagNameFromStorage(version[6])
                                                       : "NTAG/UL"));
  out.print("uid  ");
  out.println(hexBytes(rfid.uid.uidByte, rfid.uid.size, 10).c_str());
  out.print("sak  ");
  out.println(hexBytes(&rfid.uid.sak, 1).c_str());

  size_t written = 0;
  char label[8];

  if (kind == CardKind::Ultralight) {
    const uint16_t lastPage = haveVersion ? ntagLastUserPage(version[6]) : 39;
    for (uint16_t page = 0; page <= lastPage; page += 4) {
      uint8_t data[16] = {0};
      if (!ulRead16(static_cast<uint8_t>(page), data)) break;
      for (uint16_t k = 0; k < 4 && (page + k) <= lastPage; ++k) {
        snprintf(label, sizeof(label), "P%-4u", static_cast<unsigned>(page + k));
        out.print(label);
        out.println(hexBytes(data + k * 4, 4).c_str());
        written++;
      }
    }
  } else {
    // Klonen fremder Karten: pro Sektor mit dem Woerterbuch anmelden und ALLE
    // Bloecke sichern - auch ohne NDEF-Inhalt, rein die Rohdaten.
    const uint8_t type = rfid.PICC_GetType(rfid.uid.sak);
    const uint16_t lastBlock = (type == MFRC522_I2C::PICC_TYPE_MIFARE_4K) ? 255
                             : (type == MFRC522_I2C::PICC_TYPE_MIFARE_MINI) ? 19 : 63;

    uint8_t curTrailer = 0xFF;
    bool    authed     = false;
    uint8_t sectorKey[6] = {0};
    for (uint16_t block = 0; block <= lastBlock; ++block) {
      const uint8_t tr = blockTrailer(block);
      if (tr != curTrailer) {
        curTrailer = tr;
        char keyType = '?';
        authed = classicAuthDict(tr, sectorKey, &keyType);
        if (authed) {
          uint8_t* key = sectorKey;
          out.print("# Sektor ");
          out.print(String(block / 4).c_str());
          out.print("  Key ");
          out.print(String(keyType).c_str());
          out.print(" ");
          out.println(hexBytes(key, 6).c_str());
        } else {
          out.print("# Sektor ");
          out.print(String(block / 4).c_str());
          out.println("  kein Schluessel gefunden");
        }
      }
      if (!authed) continue;

      uint8_t buf[18] = {0};
      uint8_t sz = sizeof(buf);
      if (rfid.MIFARE_Read(static_cast<uint8_t>(block), buf, &sz) != MFRC522_I2C::STATUS_OK) {
        continue;
      }
      // Schluessel A ist nie lesbar (kommt als 00.. zurueck). Damit der Dump
      // wieder brauchbar ist, tragen wir den tatsaechlich gefundenen Schluessel
      // in den Trailer ein.
      if (blockIsTrailer(block)) memcpy(buf, sectorKey, 6);

      snprintf(label, sizeof(label), "B%-4u", static_cast<unsigned>(block));
      out.print(label);
      out.println(hexBytes(buf, 16).c_str());
      written++;
    }
  }

  out.close();

  if (written == 0) {
    SD.remove(path);
    if (errorOut) *errorOut = "nichts lesbar";
    return false;
  }
  if (fileOut) *fileOut = path;
  Serial.printf("Dump geschrieben: %s (%u Eintraege)\n", path.c_str(),
                static_cast<unsigned>(written));
  return true;
}

// Eine Zeile "P12  AA BB CC DD" zerlegen
bool parseDumpLine(const String& line, char* kindOut, uint16_t* indexOut,
                   uint8_t* data, size_t* lenOut) {
  if (line.length() < 4) return false;
  const char tag = line[0];
  if (tag != 'P' && tag != 'B') return false;

  size_t i = 1;
  uint16_t index = 0;
  bool haveDigit = false;
  while (i < line.length() && isdigit(static_cast<unsigned char>(line[i]))) {
    index = static_cast<uint16_t>(index * 10 + (line[i] - '0'));
    haveDigit = true;
    ++i;
  }
  if (!haveDigit) return false;

  size_t len = 0;
  while (i < line.length() && len < 16) {
    while (i < line.length() && line[i] == ' ') ++i;
    if (i + 1 >= line.length()) break;
    const char hi = line[i], lo = line[i + 1];
    if (!isxdigit(static_cast<unsigned char>(hi)) || !isxdigit(static_cast<unsigned char>(lo))) break;
    auto nib = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      return static_cast<uint8_t>((c | 0x20) - 'a' + 10);
    };
    data[len++] = static_cast<uint8_t>((nib(hi) << 4) | nib(lo));
    i += 2;
  }
  if (len == 0) return false;

  *kindOut  = tag;
  *indexOut = index;
  *lenOut   = len;
  return true;
}

bool restoreDumpToCard(CardKind kind, const String& file, String* errorOut) {
  if (!app.sdReady && !initSd()) {
    if (errorOut) *errorOut = "keine SD-Karte";
    return false;
  }
  File in = SD.open(file, FILE_READ);
  if (!in) {
    if (errorOut) *errorOut = "Dump nicht lesbar";
    return false;
  }

  uint16_t lastPage = 39;
  if (kind == CardKind::Ultralight) {
    uint8_t version[8] = {0};
    if (ulGetVersion(version)) lastPage = ntagLastUserPage(version[6]);
    else                       reselectCard();
  }

  size_t written = 0, skipped = 0;
  uint8_t destTrailer = 0xFF;
  bool    destAuthed  = false;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line[0] == '#') continue;

    char tag = 0;
    uint16_t index = 0;
    uint8_t data[16] = {0};
    size_t len = 0;
    if (!parseDumpLine(line, &tag, &index, data, &len)) continue;

    if (kind == CardKind::Ultralight && tag == 'P') {
      if (index < 4 || index > lastPage) { skipped++; continue; }   // Kopf und Konfig tabu
      if (len < 4) continue;
      if (!ulWritePage(static_cast<uint8_t>(index), data)) {
        in.close();
        if (errorOut) *errorOut = "Seite " + String(index) + " nicht schreibbar";
        return false;
      }
      written++;
    } else if (kind == CardKind::Classic && tag == 'B') {
      // Block 0 (Hersteller/UID) ist auf normalen Karten schreibgeschuetzt.
      if (index == 0 || index > 255) { skipped++; continue; }
      if (len < 16) continue;

      const bool trailer = blockIsTrailer(index);

      // Trailer nur schreiben, wenn die Zugriffsbits stimmig sind - ein
      // ungueltiges Muster wuerde den Sektor dauerhaft sperren.
      if (trailer && !validAccessBits(data[6], data[7], data[8])) {
        skipped++;
        continue;
      }

      // Zielsektor per Woerterbuch anmelden (die leere Karte nutzt FFFF..,
      // eine schon beschriebene evtl. einen anderen bekannten Schluessel).
      const uint8_t tr = blockTrailer(index);
      if (tr != destTrailer) {
        destTrailer = tr;
        destAuthed  = classicAuthDict(tr, nullptr, nullptr);
      }
      if (!destAuthed) { skipped++; continue; }

      // Datenbloecke werden vor dem Trailer geschrieben (aufsteigende
      // Reihenfolge in der Datei), daher verlieren wir den Zugriff nicht.
      if (rfid.MIFARE_Write(static_cast<uint8_t>(index), data, 16) != MFRC522_I2C::STATUS_OK) {
        in.close();
        if (errorOut) *errorOut = String(trailer ? "Trailer " : "Block ") + String(index) +
                                  " nicht schreibbar";
        return false;
      }
      written++;
    } else {
      skipped++;
    }
  }
  in.close();

  Serial.printf("Restore: %u geschrieben, %u uebersprungen\n",
                static_cast<unsigned>(written), static_cast<unsigned>(skipped));
  if (written == 0) {
    if (errorOut) *errorOut = "Dump passt nicht zur Karte";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Umrechnung zwischen dem Kartentext und einem Pfad auf unserer microSD
//
//   Karte:  SD:OneLoad v5/Bubble Bobble.crt
//   intern: /OneLoad v5/Bubble Bobble.crt
//
// Die Praefixe SD:, USB: und TR: stammen vom TeensyROM. Wir koennen nur von
// der eigenen SD laden, versuchen es aber trotzdem mit demselben Pfad - so
// funktioniert eine Karte an beiden Geraeten, solange die Ordner gleich heissen.
// ---------------------------------------------------------------------------
// Alles unterhalb von 0x20 rauswerfen: manche Schreib-Apps haengen ein NUL,
// CR oder LF an den Text, das wuerde jede Endung unbrauchbar machen.
String sanitizeCardText(const String& text) {
  String out;
  out.reserve(text.length() + 1);
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (static_cast<uint8_t>(c) >= 0x20) out += c;
  }
  out.trim();
  return out;
}

// ---------------------------------------------------------------------------
// Kommandokarten
//
// Statt eines Dateipfads kann auf einer Karte auch ein Befehl fuer den c64u
// stehen. Format: das Praefix "CMD:" gefolgt vom Schluesselwort, optional mit
// einem Argument hinter einem Gleichheitszeichen.
//
//     CMD:RESET
//     CMD:REBOOT
//     CMD:MENU
//     CMD:POWEROFF=0      sofort ausschalten
//     CMD:POWEROFF=8      nachfragen, 8 s Zeit fuer die Bestaetigung
//     CMD:POWEROFF        nachfragen mit der am Geraet eingestellten Zeit
//     CMD:CPU=10          CPU auf 10 MHz stellen
//
// Gross-/Kleinschreibung und Leerzeichen sind egal. Der Inhalt bleibt ein
// gewoehnlicher NDEF-Textrecord, jede NFC-App kann so eine Karte lesen.
// Dasselbe Format benutzt die M5Dial-Fassung - eine Karte laeuft an beiden.
// ---------------------------------------------------------------------------
constexpr const char* kCardCmdPrefix = "CMD:";

bool parseCardCommand(const String& text, CardCommand* out) {
  String t = text;
  t.trim();
  String head = t.substring(0, 4);
  head.toUpperCase();
  if (head != kCardCmdPrefix) return false;

  String body = t.substring(4);
  body.trim();

  String arg;
  const int eq = body.indexOf('=');
  if (eq >= 0) {
    arg  = body.substring(eq + 1);
    body = body.substring(0, eq);
    arg.trim();
    body.trim();
  }
  body.toUpperCase();

  CardCommand cmd;
  cmd.arg    = arg;
  cmd.hasArg = (eq >= 0);

  if      (body == "RESET")    cmd.cmd = CardCmd::Reset;
  else if (body == "REBOOT")   cmd.cmd = CardCmd::Reboot;
  else if (body == "MENU")     cmd.cmd = CardCmd::UltiMenu;
  else if (body == "POWEROFF") cmd.cmd = CardCmd::PowerOff;
  else if (body == "CPU")      cmd.cmd = CardCmd::CpuSpeed;
  else return false;

  if (cmd.cmd == CardCmd::CpuSpeed && arg.isEmpty()) return false;

  *out = cmd;
  return true;
}

// Bestaetigungszeit eines PowerOff-Befehls in Sekunden.
// Ohne Argument gilt die Geraeteeinstellung, 0 bedeutet "ohne Nachfrage".
uint8_t cardPowerOffSeconds(const CardCommand& c) {
  if (!c.hasArg) return app.settings.cardConfirmS;
  const long v = c.arg.toInt();
  if (v <= 0) return 0;
  return static_cast<uint8_t>(std::min<long>(v, 60));
}

String cardCommandText(const CardCommand& c) {
  switch (c.cmd) {
    case CardCmd::Reset:    return "CMD:RESET";
    case CardCmd::Reboot:   return "CMD:REBOOT";
    case CardCmd::UltiMenu: return "CMD:MENU";
    case CardCmd::PowerOff: return "CMD:POWEROFF=" + String(cardPowerOffSeconds(c));
    case CardCmd::CpuSpeed: return "CMD:CPU=" + c.arg;
    default:                return "";
  }
}

String cardCommandLabel(const CardCommand& c) {
  switch (c.cmd) {
    case CardCmd::Reset:    return "Reset";
    case CardCmd::Reboot:   return "Reboot";
    case CardCmd::UltiMenu: return "Ultimate Menu";
    case CardCmd::PowerOff: {
      const uint8_t sec = cardPowerOffSeconds(c);
      return sec == 0 ? String("PowerOff direkt")
                      : ("PowerOff, " + String(sec) + "s Abfrage");
    }
    case CardCmd::CpuSpeed: return "CPU " + c.arg + " MHz";
    default:                return "?";
  }
}

// ---- Auswahlliste zum Beschreiben einer Karte -----------------------------
// Feste Befehle zuerst, danach alle CPU-Stufen, die der c64u anbietet.
constexpr size_t kCmdFixedCount = 5;

size_t cmdListCount() { return kCmdFixedCount + app.cpuChoiceCount; }

CardCommand cmdListAt(size_t index) {
  CardCommand c;
  switch (index) {
    case 0: c.cmd = CardCmd::Reset;    return c;
    case 1: c.cmd = CardCmd::Reboot;   return c;
    case 2: c.cmd = CardCmd::UltiMenu; return c;
    case 3: c.cmd = CardCmd::PowerOff; c.arg = "0"; c.hasArg = true; return c;
    case 4:
      c.cmd    = CardCmd::PowerOff;
      c.arg    = String(app.settings.cardConfirmS);
      c.hasArg = true;
      return c;
    default: break;
  }
  const size_t cpu = index - kCmdFixedCount;
  if (cpu < app.cpuChoiceCount) {
    c.cmd    = CardCmd::CpuSpeed;
    c.arg    = extractDigits(app.cpuDisplayOptions[cpu]);
    c.hasArg = true;
  }
  return c;
}

String stripSourcePrefix(const String& text, String* sourceOut) {
  String t = sanitizeCardText(text);
  const int colon = t.indexOf(':');
  if (colon > 0 && colon <= 3) {
    String prefix = t.substring(0, colon);
    prefix.toUpperCase();
    if (prefix == "SD" || prefix == "USB" || prefix == "TR") {
      if (sourceOut) *sourceOut = prefix;
      t = t.substring(colon + 1);
    }
  }
  return t;
}

String cardTextToPath(const String& text, String* sourceOut = nullptr) {
  String t = stripSourcePrefix(text, sourceOut);

  // Doppelte Schraegstriche zusammenfassen. Die Schreibweise "SD://Ordner/..."
  // ist verbreitet, uebrig bliebe sonst "//Ordner/..." und SD.open() findet
  // damit nichts.
  String clean;
  clean.reserve(t.length() + 1);
  for (size_t i = 0; i < t.length(); ++i) {
    const char c = t[i];
    if (c == '/' && !clean.isEmpty() && clean[clean.length() - 1] == '/') continue;
    clean += c;
  }
  if (!clean.startsWith("/")) clean = "/" + clean;

  // Abschliessenden Schraegstrich behalten wir nur, wenn danach nichts mehr kommt
  return clean;
}

String pathToCardText(const String& path) {
  String p = path;
  while (p.startsWith("/")) p.remove(0, 1);
  return "SD:" + p;
}

// "?" als Dateiname: eine zufaellige Datei aus dem Verzeichnis auswaehlen
// "?" als Dateiname oder ein reiner Verzeichnispfad -> Zufallsauswahl
bool pathIsRandom(const String& path) {
  return path.endsWith("/?") || path == "?" || path.endsWith("/");
}

// Liegt unter diesem Pfad ein Verzeichnis auf der SD?
bool pathIsDirectory(const String& path) {
  // Ohne diesen Versuch waere eine Verzeichniskarte direkt nach dem
  // Einschalten wertlos: die SD ist dann noch nicht angemeldet, der Pfad
  // gaebe kein Verzeichnis her und landete als Datei ohne Endung im
  // Upload.
  if (!app.sdReady && !initSd()) return false;
  File probe = SD.open(path);
  const bool isDir = probe && probe.isDirectory();
  if (probe) probe.close();
  return isDir;
}

// ---------------------------------------------------------------------------
// Zufallskarte: Verzeichnis statt Datei auswaehlen (app.sdPickMode == 3)
//
// Im Browser steht dafuer eine zusaetzliche Zeile ueber den Ordnern. Sie
// waehlt das gerade geoeffnete Verzeichnis selbst aus - ohne sie kaeme man
// nie an einen Ordner heran, weil ein Tipper darauf hineinwechselt.
// ---------------------------------------------------------------------------
bool sdPickHereRow() { return app.sdPickMode == 3; }

int sdBrowserCount() {
  return static_cast<int>(app.sdCount) + (app.sdPath != "/" ? 1 : 0) +
         (sdPickHereRow() ? 1 : 0);
}

// Zeile -> Bedeutung:  -1 = "..",  -2 = dieses Verzeichnis,  >= 0 = Eintrag
int sdBrowserSlot(int row) {
  if (app.sdPath != "/") { if (row == 0) return -1; row--; }
  if (sdPickHereRow())   { if (row == 0) return -2; row--; }
  return row;
}

// Verzeichnis als Zufallspfad schreiben:  "/Games" -> "/Games/?"
// Das Fragezeichen ist eindeutig und wird beim Auflegen auch dann erkannt,
// wenn die SD noch gar nicht gelesen wurde.
String randomCardPath(const String& dir) {
  String d = dir;
  if (!d.endsWith("/")) d += "/";
  return d + "?";
}

String resolveRandomFile(const String& path) {
  String dir = path;
  if (dir.endsWith("/?"))     dir = dir.substring(0, dir.length() - 2);
  else if (dir.endsWith("/")) dir = dir.substring(0, dir.length() - 1);
  if (dir.isEmpty()) dir = "/";

  // readDirectory() filtert je nach Auswahlmodus. Steht der noch auf "Dump"
  // oder "Ordner" vom letzten Besuch im Browser, bliebe die Liste hier leer
  // und die Zufallskarte faende nichts.
  const uint8_t savedPick = app.sdPickMode;
  app.sdPickMode = 0;
  const bool listed = readDirectory(dir);
  app.sdPickMode = savedPick;
  if (!listed) return "";

  size_t files = 0;
  for (size_t i = 0; i < app.sdCount; ++i) {
    if (!app.sdEntries[i].isDir) files++;
  }
  if (files == 0) return "";

  size_t pick = static_cast<size_t>(random(static_cast<long>(files)));
  for (size_t i = 0; i < app.sdCount; ++i) {
    if (app.sdEntries[i].isDir) continue;
    if (pick == 0) return joinPath(dir, app.sdEntries[i].name);
    --pick;
  }
  return "";
}

// ---------------------------------------------------------------------------
// Alle verfuegbaren Informationen der aufliegenden Karte einsammeln
// ---------------------------------------------------------------------------
void addInfoLine(const String& text) {
  if (app.infoCount < AppState::kMaxInfoLines) app.infoLines[app.infoCount++] = text;
}

void addRawLine(const String& text) {
  if (app.infoCount2 < AppState::kMaxInfoLines) app.infoLines2[app.infoCount2++] = text;
}

// Konfigurationsseite von NTAG21x (dort steht der Passwortschutz)
uint8_t ntagConfigPage(uint8_t storage) {
  switch (storage) {
    case 0x0F: return 0x29;   // NTAG213
    case 0x11: return 0x83;   // NTAG215
    case 0x13: return 0xE3;   // NTAG216
    default:   return 0;
  }
}

// NFC-Zaehler (READ_CNT 0x39) - nur aktiv, wenn im Chip freigeschaltet
bool ulReadCounter(uint32_t* out) {
  uint8_t cmd[4] = {0x39, 0x02, 0, 0};
  if (rfid.PCD_CalculateCRC(cmd, 2, &cmd[2]) != MFRC522_I2C::STATUS_OK) return false;
  uint8_t back[8];
  uint8_t backLen = sizeof(back);
  if (rfid.PCD_TransceiveData(cmd, 4, back, &backLen, nullptr, 0, true) !=
      MFRC522_I2C::STATUS_OK) {
    return false;
  }
  if (backLen < 3) return false;
  *out = static_cast<uint32_t>(back[0]) | (static_cast<uint32_t>(back[1]) << 8) |
         (static_cast<uint32_t>(back[2]) << 16);
  return true;
}

// ---------------------------------------------------------------------------
// Seite 2: Rohdaten und technische Details
// ---------------------------------------------------------------------------
uint8_t gInfoStorage     = 0;      // Storage-Byte aus GET_VERSION
bool    gInfoHaveVersion = false;

void collectCardRaw(CardKind kind, uint8_t storage, bool haveVersion) {
  app.infoCount2 = 0;

  if (kind == CardKind::Ultralight) {
    // Seiten 0..15 in Achterbloecken, das passt jeweils in eine Zeile
    for (uint8_t base = 0; base < 16; base += 4) {
      uint8_t data[16] = {0};
      char label[12];
      if (!ulRead16(base, data)) {
        snprintf(label, sizeof(label), "S %2u-%-2u", base, base + 3);
        addRawLine(String(label) + "   nicht lesbar");
        continue;
      }
      snprintf(label, sizeof(label), "S %2u-%-2u", base, base + 1);
      addRawLine(String(label) + "   " + hexBytes(data, 8));
      snprintf(label, sizeof(label), "S %2u-%-2u", base + 2, base + 3);
      addRawLine(String(label) + "   " + hexBytes(data + 8, 8));
    }

    // Seite 2 = Lock-Bytes, Seite 3 = Capability Container
    uint8_t head[16] = {0};
    if (ulRead16(0, head)) {
      addRawLine("Lock      " + hexBytes(head + 10, 2) +
                 String((head[10] || head[11]) ? "  (gesperrt)" : "  (frei)"));
      const uint8_t* cc = head + 12;
      if (cc[0] == 0xE1) {
        addRawLine("CC        NDEF " + String(cc[1] >> 4) + "." + String(cc[1] & 0x0F) +
                   ", " + String(static_cast<uint16_t>(cc[2]) * 8) + " Byte, " +
                   String(cc[3] == 0x00 ? "les-/schreibbar" : "nur lesbar"));
      } else {
        addRawLine("CC        " + hexBytes(cc, 4) + "  (nicht NDEF-formatiert)");
      }
    }

    // Passwortschutz und Lesezaehler - beides zuletzt, weil ein nicht
    // unterstuetzter Befehl die Karte abmeldet.
    const uint8_t cfgPage = haveVersion ? ntagConfigPage(storage) : 0;
    if (cfgPage != 0) {
      uint8_t cfg[16] = {0};
      if (ulRead16(cfgPage, cfg)) {
        const uint8_t auth0 = cfg[3];
        const uint8_t prot  = (cfg[4] & 0x80) ? 1 : 0;
        addRawLine(String("Schutz    ") +
                   (auth0 >= 0xFF ? String("aus")
                                  : ("ab Seite " + String(auth0) +
                                     (prot ? ", Lesen+Schreiben" : ", nur Schreiben"))));
      }
    }
    uint32_t counter = 0;
    if (ulReadCounter(&counter)) {
      addRawLine("Zaehler   " + String(counter) + " Lesevorgaenge");
    } else {
      addRawLine("Zaehler   im Chip nicht aktiviert");
    }

  } else if (kind == CardKind::Classic) {
    static const uint8_t blocks[] = {0, 4, 5, 6, 8};
    for (size_t i = 0; i < sizeof(blocks); ++i) {
      uint8_t data[16] = {0};
      const String label = "Block " + String(blocks[i]) + (blocks[i] < 10 ? "   " : "  ");
      if (rfidReadBlock(blocks[i], data)) addRawLine(label + hexBytes(data, 16));
      else                                addRawLine(label + "nicht lesbar");
    }
    addRawLine(String("Schluessel") + (gClassicNdefFormatted ? " D3F7D3F7D3F7 (NDEF)"
                                                             : " FFFFFFFFFFFF (Werk)"));
  }
}

void collectCardInfo(CardKind kind) {
  app.infoCount    = 0;
  app.infoCount2   = 0;
  gInfoStorage     = 0;
  gInfoHaveVersion = false;

  // ---- Kennung ----
  addInfoLine("UID       " + hexBytes(rfid.uid.uidByte, rfid.uid.size, 10) +
              "  (" + String(rfid.uid.size) + " Byte)");
  addInfoLine("SAK       0x" + hexBytes(&rfid.uid.sak, 1));

  // ---- Typ, Groesse, Version ----
  uint16_t userBytes = 0;
  if (kind == CardKind::Classic) {
    const uint8_t type = rfid.PICC_GetType(rfid.uid.sak);
    const char* name = "MIFARE Classic";
    if (type == MFRC522_I2C::PICC_TYPE_MIFARE_1K)   { name = "MIFARE Classic 1K";   userBytes = 1024; }
    if (type == MFRC522_I2C::PICC_TYPE_MIFARE_4K)   { name = "MIFARE Classic 4K";   userBytes = 4096; }
    if (type == MFRC522_I2C::PICC_TYPE_MIFARE_MINI) { name = "MIFARE Classic Mini"; userBytes = 320;  }
    addInfoLine(String("Typ       ") + name + ", " + String(userBytes) + " Byte, " +
                String(userBytes / 64) + " Sektoren");
  } else {
    uint8_t version[8] = {0};
    uint8_t cc[16] = {0};
    if (ulRead16(3, cc) && cc[2] != 0) userBytes = static_cast<uint16_t>(cc[2]) * 8;

    // GET_VERSION: schlaegt er fehl, muss die Karte neu ausgewaehlt werden
    if (ulGetVersion(version)) {
      gInfoStorage     = version[6];
      gInfoHaveVersion = true;
      const uint16_t fromVer = ntagBytesFromStorage(version[6]);
      if (fromVer) userBytes = fromVer;
      addInfoLine(String("Typ       ") + ntagNameFromStorage(version[6]) + ", " +
                  String(userBytes) + " Byte");
      addInfoLine(String("Version   ") + (version[1] == 0x04 ? "NXP  " : "") + hexBytes(version, 8));
    } else {
      reselectCard();          // nach dem Fehlversuch wieder ansprechbar machen
      addInfoLine(String("Typ       NTAG / Ultralight (Type 2)") +
                  (userBytes ? (", " + String(userBytes) + " Byte") : String("")));
      addInfoLine("Version   GET_VERSION nicht unterstuetzt");
    }
  }

  // ---- Inhalt ----
  const CardContent content = readCardContent(kind);

  if (kind == CardKind::Classic) {
    addInfoLine(String("Format    ") + (gClassicNdefFormatted ? "NDEF-Schluessel D3F7.."
                                                              : "Werksschluessel FFFF.."));
  }

  if (!content.ok) {
    addInfoLine("Inhalt    " + content.error);
    addInfoLine("Hinweis   Rohdaten auf Seite 2");
    collectCardRaw(kind, gInfoStorage, gInfoHaveVersion);
    return;
  }

  addInfoLine(String("Inhalt    ") +
              (content.isNdef ? "NDEF Text-Record" : "Altformat C64UPATH") + ", " +
              String(content.text.length()) + " Zeichen");
  addInfoLine("Text      " + content.text);

  String source;
  const String path = cardTextToPath(content.text, &source);

  if (pathIsRandom(path) || pathIsDirectory(path)) {
    addInfoLine("Pfad      " + path);
    addInfoLine("Datei     Verzeichnis - startet zufaellige Datei");
    collectCardRaw(kind, gInfoStorage, gInfoHaveVersion);
    return;
  }

  // Verzeichnis und Dateiname getrennt, damit beides vollstaendig lesbar ist
  addInfoLine("Pfad      " + parentPath(path));
  addInfoLine("Datei     " + baseName(path));

  String state = "keine SD-Karte";
  if (app.sdReady) {
    File probe = SD.open(path, FILE_READ);
    if (probe && !probe.isDirectory()) {
      state = "auf SD, " + String(static_cast<uint32_t>(probe.size() / 1024)) + " kB";
    } else {
      state = "NICHT auf SD gefunden";
    }
    if (probe) probe.close();
  }
  const String ext = lowerExt(path);
  addInfoLine("Typ/SD    " + (ext.isEmpty() ? String("ohne Endung") : ext) + ", " + state);

  collectCardRaw(kind, gInfoStorage, gInfoHaveVersion);
}

// ===========================================================================
//  Benutzeroberflaeche
// ===========================================================================
constexpr int kTitleY   = 22;
constexpr int kTitleH   = 20;
constexpr int kListY    = 46;
// Hoehere Zeilen als in der Tastenfassung (dort 24 px): eine Fingerkuppe ist
// rund 8 mm breit, das sind auf diesem Display etwa 40 px. 26 px sind der
// Kompromiss zwischen Trefferquote und Anzahl sichtbarer Eintraege.
constexpr int kRowH     = 26;
constexpr int kListRows = 6;                    // 46 + 6*26 = 202

// ---- Scrollleiste rechts neben der Liste --------------------------------
// Sie erscheint, sobald mehr Eintraege da sind als Zeilen passen, zeigt
// Position und Laenge der Liste an und laesst sich direkt ziehen.
constexpr int kScrollW   = 16;                  // Breite der Trefferflaeche
constexpr int kScrollX   = kScrW - kScrollW;    // linke Kante
constexpr int kScrollTop = kListY;
constexpr int kScrollH   = kListRows * kRowH;
constexpr int kThumbMinH = 28;                  // kleinstmoeglicher Griff
// Rechte Kante der Listenzeilen - sie enden vor der Scrollleiste.
constexpr int kListRight = kScrollX - 2;

// Kleine Font-Schalter, damit im restlichen Code kein Font-Typ auftaucht.
inline void fontSmall() { gDraw->setFont(&fonts::Font0); }
inline void fontText()  { gDraw->setFont(&fonts::Font2); }
inline void fontBig()   { gDraw->setFont(&fonts::Font4); }

// Der Datum-Typ wird bewusst per decltype geholt, damit hier kein
// bibliotheksinterner Typname (textdatum_t) im Code stehen muss.
using DatumT = decltype(middle_center);

// Zeichnet Text und kuerzt ihn, falls er breiter als maxW waere.
void drawClipped(const String& text, int x, int y, int maxW, uint16_t color, DatumT datum) {
  gDraw->setTextColor(color);
  gDraw->setTextDatum(datum);
  String out = text;
  while (out.length() > 1 && gDraw->textWidth(out) > maxW) out.remove(out.length() - 1);
  gDraw->drawString(out, x, y);
}

// ---- Statusleiste -------------------------------------------------------
void drawStatusBar() {
  const bool wifiOk   = WiFi.status() == WL_CONNECTED;
  const bool targetOk = app.connection.targetReachable;
  const bool authOk   = app.connection.authOk;

  uint16_t stateColor = kColErr;
  const char* stateText = "NO WIFI";
  if (wifiOk && authOk)        { stateColor = kColOk;   stateText = "C64U OK"; }
  else if (wifiOk && targetOk) { stateColor = kColWarn; stateText = "AUTH?";   }
  else if (wifiOk)             { stateColor = kColInfo; stateText = "NO C64U"; }

  gDraw->fillRect(0, 0, kScrW, kBarH, kColPanel);
  gDraw->drawFastHLine(0, kBarH - 1, kScrW, kColLine);

  gDraw->fillCircle(10, kBarH / 2, 5, stateColor);
  fontSmall(); 
  drawClipped(stateText, 20, kBarH / 2, 62, kColText, middle_left);

  const String host = wifiOk ? WiFi.localIP().toString() : String("---");
  fontSmall(); 
  drawClipped(host, 86, kBarH / 2, 84, kColLabel, middle_left);

  fontSmall(); 
  drawClipped(String("CPU ") + app.currentCpuValue, 176, kBarH / 2, 84, kColText, middle_left);

  fontSmall(); 
  drawClipped(app.rfidReady ? "RFID" : "-", 268, kBarH / 2, 26, app.rfidReady ? kColOk : kColLine, middle_left);
  fontSmall(); 
  drawClipped(app.sdReady ? "SD" : "-", 300, kBarH / 2, 20, app.sdReady ? kColOk : kColLine, middle_left);

  app.lastStatusDrawMs = millis();
  app.barDirty = false;
}

void drawTitleBar(const char* title) {
  gDraw->fillRect(0, kTitleY, kScrW, kTitleH, kColBg);
  fontText(); 
  drawClipped(title, kScrW / 2, kTitleY + kTitleH / 2, kScrW - 20, kColLineHi, middle_center);
}

// ---- Fussleiste unten ---------------------------------------------------
// Richtungen fuer die Pfeilsymbole
constexpr int kArrowNone  = -1;
constexpr int kArrowUp    = 0;
constexpr int kArrowDown  = 1;
constexpr int kArrowLeft  = 2;
constexpr int kArrowRight = 3;

void drawArrow(int cx, int cy, int dir, uint16_t color) {
  constexpr int kW = 9;   // halbe Breite quer zur Richtung
  constexpr int kH = 8;   // halbe Laenge in Richtung der Spitze
  switch (dir) {
    case kArrowUp:
      gDraw->fillTriangle(cx, cy - kH, cx - kW, cy + kH, cx + kW, cy + kH, color);
      break;
    case kArrowDown:
      gDraw->fillTriangle(cx, cy + kH, cx - kW, cy - kH, cx + kW, cy - kH, color);
      break;
    case kArrowLeft:
      gDraw->fillTriangle(cx - kH, cy, cx + kH, cy - kW, cx + kH, cy + kW, color);
      break;
    case kArrowRight:
      gDraw->fillTriangle(cx + kH, cy, cx - kH, cy - kW, cx - kH, cy + kW, color);
      break;
    default:
      break;
  }
}

// Die Fussleiste ist beim CoreS3 keine Tastenbeschriftung mehr, sondern
// selbst eine Bedienflaeche:
//
//   [ <- Zurueck ]        Hinweistext         [ ^ ][ v ]
//
// Das Zurueck-Feld liegt immer links, die beiden Blaetterfelder erscheinen
// nur auf Listenseiten. Die Trefferflaechen dazu stehen in hitBack() bzw.
// hitPageUp()/hitPageDown() und muessen mit diesen Massen zusammenpassen.
constexpr int kBackW    = 78;                 // Breite des Zurueck-Feldes
constexpr int kPageW    = 46;                 // Breite je Blaetterfeld
constexpr int kHintBarH = kScrH - kHintY;

bool hitBack(int x, int y) {
  return y >= kHintY && x < kBackW;
}
bool hitPageUp(int x, int y) {
  return y >= kHintY && x >= kScrW - 2 * kPageW && x < kScrW - kPageW;
}
bool hitPageDown(int x, int y) {
  return y >= kHintY && x >= kScrW - kPageW;
}

void drawHintBar(const char* text, bool withBack = true, bool withPaging = false) {
  const int cy = kHintY + kHintBarH / 2;

  gDraw->fillRect(0, kHintY, kScrW, kHintBarH, kColPanel);
  gDraw->drawFastHLine(0, kHintY, kScrW, kColLine);

  int textLeft  = 4;
  int textRight = kScrW - 4;

  if (withBack) {
    gDraw->fillRoundRect(2, kHintY + 3, kBackW - 4, kHintBarH - 6, 5, kColPanelHi);
    gDraw->drawRoundRect(2, kHintY + 3, kBackW - 4, kHintBarH - 6, 5, kColLineHi);
    drawArrow(18, cy, kArrowLeft, TFT_WHITE);
    fontSmall();
    drawClipped("Zurueck", 30, cy, kBackW - 36, TFT_WHITE, middle_left);
    textLeft = kBackW + 6;
  }

  if (withPaging) {
    const int upX   = kScrW - 2 * kPageW;
    const int downX = kScrW - kPageW;
    for (int x0 : {upX, downX}) {
      gDraw->fillRoundRect(x0 + 2, kHintY + 3, kPageW - 4, kHintBarH - 6, 5, kColPanelHi);
      gDraw->drawRoundRect(x0 + 2, kHintY + 3, kPageW - 4, kHintBarH - 6, 5, kColLineHi);
    }
    drawArrow(upX   + kPageW / 2, cy, kArrowUp,   TFT_WHITE);
    drawArrow(downX + kPageW / 2, cy, kArrowDown, TFT_WHITE);
    textRight = upX - 6;
  }

  if (text != nullptr && text[0] != '\0') {
    fontSmall();
    drawClipped(text, (textLeft + textRight) / 2, cy,
                std::max(20, textRight - textLeft), kColLabel, middle_center);
  }
}

// ---- Kommando-Kacheln ---------------------------------------------------
// Kacheln werden pro Zeile zentriert, damit auch eine nicht volle untere
// Zeile mittig unter der oberen sitzt.
void tileRect(int index, int* outX, int* outY) {
  constexpr int kPerRow = 5;
  const int row      = index / kPerRow;
  const int col      = index % kPerRow;
  const int inRow    = std::min(kPerRow, static_cast<int>(kTileCount) - row * kPerRow);
  const int rowWidth = inRow * kTileW + (inRow - 1) * kTileGap;
  const int x0       = (kScrW - rowWidth) / 2;

  *outX = x0 + col * (kTileW + kTileGap);
  *outY = (row == 0) ? kTileY0 : kTileY1;
}

void drawTiles() {
  gDraw->fillRect(0, kTileY0 - 2, kScrW, (kTileY1 + kTileH) - (kTileY0 - 2), kColBg);

  for (int i = 0; i < kTileCount; ++i) {
    int x = 0, y = 0;
    tileRect(i, &x, &y);
    const bool selected = (i == app.tileIndex);

    uint16_t fill   = selected ? kColPanelHi : kColPanel;
    uint16_t border = selected ? kColLineHi  : kColLine;
    uint16_t text   = selected ? TFT_WHITE   : kColText;

    if (i == kTilePowerOff) {
      border = selected ? kColWarn : rgb565(120, 80, 40);
      if (app.pendingPowerOff) fill = rgb565(140, 70, 30);
    }
    if (i == kTileRfidRun && !app.rfidReady) text = kColLine;
    if (i == kTileSdBrowse && !app.sdReady) text = kColLine;

    gDraw->fillRoundRect(x, y, kTileW, kTileH, 6, fill);
    gDraw->drawRoundRect(x, y, kTileW, kTileH, 6, border);
    fontSmall(); 
    drawClipped(kTileLabels[i], x + kTileW / 2, y + kTileH / 2, kTileW - 6, text, middle_center);
  }
}

void drawModalOverlay(uint32_t now) {
  if (!modalVisible(now)) return;

  const int boxW = 260;
  const int boxH = 74;
  const int x = (kScrW - boxW) / 2;
  // Der Kasten muss ganz im Logoband (20..127) liegen. Ragte er in die
  // Kacheln, bliebe beim Ablaufen des Modals ein Streifen stehen: der
  // Home-Zweig von render() zeichnet nur den Logobereich nach.
  const int y = 50;

  gDraw->fillRoundRect(x, y, boxW, boxH, 10, rgb565(6, 10, 18));
  gDraw->drawRoundRect(x, y, boxW, boxH, 10, app.modalColor);
  gDraw->fillRoundRect(x + 6, y + 6, boxW - 12, 6, 3, app.modalColor);

  fontBig();
  const bool tooWide = gDraw->textWidth(app.modalText) > (boxW - 20);
  if (tooWide) { fontText(); } else { fontBig(); } 
  drawClipped(app.modalText, kScrW / 2, y + 44, boxW - 20, app.modalColor, middle_center);
}

// ---- Listenzeile --------------------------------------------------------
void drawListRow(int row, const String& left, const String& right, bool selected) {
  const int y = kListY + row * kRowH;
  const uint16_t fill   = selected ? kColPanelHi : kColPanel;
  const uint16_t border = selected ? kColLineHi  : kColLine;

  // Die Zeile endet vor der Scrollleiste, damit sich beide nicht ueberlagern.
  gDraw->fillRoundRect(8, y, kListRight - 8, kRowH - 4, 6, fill);
  gDraw->drawRoundRect(8, y, kListRight - 8, kRowH - 4, 6, border);

  fontText();
  drawClipped(left, 18, y + (kRowH - 4) / 2,
              right.isEmpty() ? kListRight - 28 : kListRight - 118,
              selected ? TFT_WHITE : kColText, middle_left);
  if (!right.isEmpty()) {
    fontText();
    drawClipped(right, kListRight - 10, y + (kRowH - 4) / 2, 100,
                selected ? kColOk : kColLabel, middle_right);
  }
}

// ---------------------------------------------------------------------------
// Fensteranfang der Listen
//
// `gListStart` ist die erste sichtbare Zeile und damit die einzige Wahrheit
// darueber, welcher Ausschnitt gerade zu sehen ist.
//
// Bei Tastenbedienung wurde das Fenster aus der Auswahl abgeleitet und immer
// auf sie zentriert. Am Touchscreen geht das nicht: der Benutzer schiebt die
// Liste mit dem Finger, ohne dabei etwas auszuwaehlen - das Fenster wuerde
// sonst sofort wieder zur Auswahl zurueckspringen. `listWindowStart()` begrenzt
// deshalb nur noch auf den gueltigen Bereich; nachgezogen wird die Ansicht
// ausdruecklich ueber `listEnsureVisible()`, wenn sich die Auswahl selbst
// bewegt (Blaetterfelder der Fussleiste).
// ---------------------------------------------------------------------------
int gListStart = 0;

int listWindowStart(int /*selected*/, int count) {
  if (count <= kListRows) {
    gListStart = 0;
    return 0;
  }
  gListStart = std::max(0, std::min(gListStart, count - kListRows));
  return gListStart;
}

void listEnsureVisible(int selected, int count) {
  if (count <= kListRows) { gListStart = 0; return; }
  if (selected < gListStart)              gListStart = selected;
  if (selected >= gListStart + kListRows) gListStart = selected - kListRows + 1;
  gListStart = std::max(0, std::min(gListStart, count - kListRows));
}

// Hoehe des Griffs: proportional zum sichtbaren Anteil, aber nie kleiner als
// kThumbMinH - sonst liesse er sich bei langen Listen nicht mehr treffen.
int scrollThumbHeight(int count) {
  if (count <= kListRows) return kScrollH;
  return std::max(kThumbMinH, (kScrollH * kListRows) / count);
}

void drawScrollbar(int start, int count) {
  if (count <= kListRows) return;             // passt alles, keine Leiste noetig

  const int trackX = kScrollX + 3;
  const int trackW = kScrollW - 6;
  gDraw->fillRoundRect(trackX, kScrollTop, trackW, kScrollH, trackW / 2, kColPanel);
  gDraw->drawRoundRect(trackX, kScrollTop, trackW, kScrollH, trackW / 2, kColLine);

  const int thumbH   = scrollThumbHeight(count);
  const int span     = kScrollH - thumbH;
  const int maxStart = count - kListRows;
  const int thumbY   = kScrollTop + (maxStart > 0 ? (span * start) / maxStart : 0);

  gDraw->fillRoundRect(kScrollX + 1, thumbY, kScrollW - 2, thumbH, 4, kColPanelHi);
  gDraw->drawRoundRect(kScrollX + 1, thumbY, kScrollW - 2, thumbH, 4, kColLineHi);
}

// Trefferflaeche der Scrollleiste. Sie beginnt genau dort, wo die Listenzeilen
// enden - sonst wuerde ein Tipper auf den rechten Rand einer Zeile in der Liste
// herumspringen, statt den Eintrag auszuwaehlen.
bool hitScrollbar(int x, int y) {
  return x >= kListRight && y >= kScrollTop && y < kScrollTop + kScrollH;
}

// Fensteranfang aus einer Beruehrung in der Scrollleiste ableiten. Der Griff
// setzt sich dabei mittig unter den Finger.
void scrollbarSeek(int y, int count) {
  if (count <= kListRows) return;
  const int thumbH   = scrollThumbHeight(count);
  const int span     = std::max(1, kScrollH - thumbH);
  const int maxStart = count - kListRows;
  int rel = y - kScrollTop - thumbH / 2;
  rel = std::max(0, std::min(rel, span));
  gListStart = std::max(0, std::min((rel * maxStart + span / 2) / span, maxStart));
}

// ---- CPU-Menue ----------------------------------------------------------
void drawCpuMenu() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("CPU SPEED");
  fontSmall(); 
  drawClipped(String("aktuell: ") + app.currentCpuValue, kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20, kColOk, top_center);

  const int count    = static_cast<int>(app.cpuChoiceCount);
  const int selected = std::max(0, std::min(app.cpuIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListRow(row, app.cpuDisplayOptions[index],
                app.cpuDisplayOptions[index] == app.currentCpuValue ? "aktiv" : "",
                index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("Wert antippen", true, true);
}

// ---- Statusseite --------------------------------------------------------
void drawStatusScreen() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("STATUS");

  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  auto line = [](int row, const char* label, const String& value, uint16_t color) {
    const int y = kListY + row * 20;
    fontText(); 
    drawClipped(label, 16, y, 120, kColLabel, top_left);
    fontText(); 
    drawClipped(value, kScrW - 16, y, 180, color, top_right);
  };

  line(0, "WiFi",   wifiOk ? WiFi.SSID() : String("disconnected"), wifiOk ? kColOk : kColWarn);
  line(1, "IP",     wifiOk ? WiFi.localIP().toString() : String("---"), kColText);
  line(2, "RSSI",   wifiOk ? String(WiFi.RSSI()) + " dBm" : String("---"), kColText);
  line(3, "c64u",   targetHost(), kColText);
  line(4, "Target", app.connection.targetReachable ? "reachable" : "not reached",
       app.connection.targetReachable ? kColOk : kColWarn);
  line(5, "Auth",   app.connection.authOk ? "ok" : "not verified",
       app.connection.authOk ? kColOk : kColWarn);
  line(6, "CPU",    app.currentCpuValue, kColText);

  const String hw = String("SD ") + (app.sdReady ? "ok" : "-") +
                    "   RFID " + (app.rfidReady ? "ok" : "-") +
                    "   Heap " + String(ESP.getFreeHeap() / 1024) + "k";
  fontSmall(); 
  drawClipped(hw, 16, kListY + 7 * 20 - 2, kScrW - 32, kColLabel, top_left);
  fontSmall();
  drawClipped(app.connection.detail, 16, kListY + 7 * 20 + 8, kScrW - 32, kColInfo, top_left);

  drawHintBar("Flaeche antippen = Verbindungstest");
}

// ---- Einstellungen ------------------------------------------------------
String settingsValue(size_t index) {
  switch (index) {
    case kSetNfcWrite:      return app.rfidReady ? "Karte" : "kein RFID";
    case kSetNfcInfo:       return app.rfidReady ? "lesen" : "kein RFID";
    case kSetNfcDump:       return app.rfidReady ? "sichern" : "kein RFID";
    case kSetNfcRestore:    return app.rfidReady ? "schreiben" : "kein RFID";
    case kSetNfcRandom:     return app.rfidReady ? "Ordner" : "kein RFID";
    case kSetNfcCmd:        return app.rfidReady ? "Befehl" : String("kein RFID");
    case kSetCardConfirm:   return cardConfirmLabel(app.settings.cardConfirmS);
    case kSetWifi:          return WiFi.status() == WL_CONNECTED
                                       ? WiFi.SSID()
                                       : String(gWifiCount == 0 ? "einrichten" : "getrennt");
    case kSetAutoNfc:       return app.rfidReady ? autoNfcLabel(app.settings.autoNfc) : String("kein RFID");
    case kSetPowerOffTime:  return timeStepLabel(app.settings.powerOffConfirmDs);
    case kSetAnimations:    return app.settings.animationsEnabled ? "On" : "Off";
    case kSetEffect:        return effectLabel(app.settings.effectMode);
    case kSetFxDetail:      return fxDetailLabel(app.settings.fxDetail);
    case kSetAnimSpeed:     return animationSpeedLabel(app.settings.animationSpeed);
    case kSetEffectTime:    return effectDurationLabel(app.settings.effectDuration);
    case kSetStaticTime:    return staticDurationLabel(app.settings.staticDuration);
    case kSetBrightness:    return String(app.settings.brightness);
    case kSetLed:           return app.settings.ledEnabled ? "On" : "Off";
    case kSetLedBright:     return app.settings.ledEnabled ? String(app.settings.ledBrightness)
                                                           : String("LED aus");
    case kSetDiskAction:    return diskActionLabel(app.settings.diskAction);
    case kSetDiskDrive:     return uploadDriveLabel(app.settings.uploadDrive);
    case kSetBeep:          return app.settings.beepEnabled ? "On" : "Off";
    case kSetFactoryReset:  return "Now";
  }
  return "";
}

// ---------------------------------------------------------------------------
// WLAN-Einrichtung
// ---------------------------------------------------------------------------
String wifiMenuValue(size_t index) {
  switch (index) {
    case kWifiScanNow:
      return app.wifiScanCount == 0 ? String("suchen")
                                    : String(static_cast<unsigned>(app.wifiScanCount));
    case kWifiFromSd:       return app.sdReady ? "wifi.txt" : "keine SD";
    case kWifiPortal:       return app.portalActive ? "an" : "aus";
    case kWifiConnectSaved: return String(static_cast<unsigned>(gWifiCount));
    case kWifiToCard:       return gWifiCount == 0 ? "leer" : "schreiben";
    case kWifiToSd:         return !app.sdReady  ? "keine SD"
                                 : gWifiCount == 0 ? "leer"
                                                   : "wifi.txt";
    case kWifiDeleteOne:    return gWifiCount == 0 ? "leer" : "waehlen";
    case kWifiDeleteAll:    return "Reset";
  }
  return "";
}

void drawWifiMenu() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("WLAN");

  fontSmall();
  const bool online = WiFi.status() == WL_CONNECTED;
  drawClipped(online ? WiFi.SSID() : String(gWifiCount == 0 ? "kein Netz gespeichert"
                                                            : "nicht verbunden"),
              kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20, online ? kColOk : kColWarn,
              top_center);

  const int count    = static_cast<int>(kWifiMenuCount);
  const int selected = std::max(0, std::min(app.wifiMenuIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListRow(row, kWifiMenuItems[index], wifiMenuValue(index), index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("Eintrag antippen", true, true);
}

void drawWifiScan() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("NETZ WAEHLEN");

  const int count = static_cast<int>(app.wifiScanCount);
  if (count == 0) {
    fontText();
    drawClipped("kein Netz gefunden", kScrW / 2, kListY + 40, kScrW - 20, kColWarn, middle_center);
    drawHintBar("");
    return;
  }

  const int selected = std::max(0, std::min(app.wifiScanIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    String    info  = String(static_cast<int>(gWifiScan[index].rssi)) + " dBm";
    if (wifiProfileIndex(gWifiScan[index].ssid) >= 0) info = "bekannt";
    else if (gWifiScan[index].open)                   info = "offen";
    drawListRow(row, gWifiScan[index].ssid, info, index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("Eintrag antippen", true, true);
}

void drawWifiSaved() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar(app.wifiSavedDelete ? "NETZ LOESCHEN"
                                   : (app.wifiSavedToCard ? "AUF NFC-KARTE" : "GESPEICHERT"));

  const int count = static_cast<int>(gWifiCount);
  if (count == 0) {
    fontText();
    drawClipped("noch kein Netz gespeichert", kScrW / 2, kListY + 40, kScrW - 20,
                kColWarn, middle_center);
    drawHintBar("");
    return;
  }

  const int selected = std::max(0, std::min(app.wifiSavedIndex, count - 1));
  const int start    = listWindowStart(selected, count);
  const bool online  = WiFi.status() == WL_CONNECTED;

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    const bool active = online && WiFi.SSID() == gWifiProfiles[index].ssid;
    drawListRow(row, gWifiProfiles[index].ssid,
                active ? "aktiv" : (gWifiProfiles[index].pass.isEmpty() ? "offen" : ""),
                index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar(app.wifiSavedDelete ? "Antippen = loeschen"
                                  : (app.wifiSavedToCard ? "Antippen = auf Karte"
                                                         : "Antippen = verbinden"),
              true, true);
}

void drawWifiCard() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("WLAN-PASSWORT");

  gDraw->fillRoundRect(24, kListY + 8, kScrW - 48, 108, 10, kColPanel);
  gDraw->drawRoundRect(24, kListY + 8, kScrW - 48, 108, 10, kColInfo);

  fontSmall();
  drawClipped("Netz:", kScrW / 2, kListY + 26, kScrW - 70, kColLabel, middle_center);
  fontText();
  drawClipped(app.wifiPendingSsid.isEmpty() ? String("von der Karte") : app.wifiPendingSsid,
              kScrW / 2, kListY + 46, kScrW - 70, kColOk, middle_center);
  fontSmall();
  drawClipped(app.rfidReady ? "Karte mit Passwort auflegen" : "RFID2 nicht gefunden",
              kScrW / 2, kListY + 72, kScrW - 70, app.rfidReady ? kColText : kColErr,
              middle_center);
  drawClipped("Textkarte: WIFI:S:..;P:..;;", kScrW / 2, kListY + 92, kScrW - 70,
              kColLabel, middle_center);

  drawClipped(app.wifiHint, kScrW / 2, kListY + 130, kScrW - 20, kColInfo, middle_center);
  drawHintBar("");
}

void drawWifiPortal() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("SETUP-PORTAL");

  fontSmall();
  drawClipped("Mit diesem Netz verbinden:", kScrW / 2, kListY + 6, kScrW - 20,
              kColLabel, middle_center);
  fontText();
  drawClipped(kPortalSsid, kScrW / 2, kListY + 26, kScrW - 20, kColOk, middle_center);

  fontSmall();
  drawClipped("Passwort:", kScrW / 2, kListY + 50, kScrW - 20, kColLabel, middle_center);
  fontText();
  drawClipped(kPortalPass, kScrW / 2, kListY + 70, kScrW - 20, kColOk, middle_center);

  fontSmall();
  drawClipped("dann im Browser oeffnen:", kScrW / 2, kListY + 94, kScrW - 20,
              kColLabel, middle_center);
  fontText();
  drawClipped(app.portalActive ? WiFi.softAPIP().toString() : String("---"),
              kScrW / 2, kListY + 114, kScrW - 20, kColInfo, middle_center);

  const uint32_t leftMs = (millis() - app.portalTouchedMs >= kPortalIdleMs)
                              ? 0
                              : (kPortalIdleMs - (millis() - app.portalTouchedMs));
  fontSmall();
  drawClipped(String("endet in ") + String(leftMs / 1000) + "s", kScrW / 2, kListY + 138,
              kScrW - 20, kColLabel, middle_center);
  drawHintBar("Zurueck beendet das Portal");
}

void drawSettings() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("SETTINGS");

  const int count    = static_cast<int>(kSettingsCount);
  const int selected = std::max(0, std::min(app.settingsIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListRow(row, kSettingsItems[index], settingsValue(index), index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("tippen = aendern", true, true);
}

// ---- SD-Browser ---------------------------------------------------------
void drawSdBrowser() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar(app.sdPickMode == 3 ? "ZUFALLS-ORDNER"
               : app.sdPickMode == 1 ? "AUF KARTE SCHREIBEN"
               : app.sdPickMode == 2 ? "DUMP WAEHLEN" : "SD-KARTE");
  fontSmall();
  drawClipped("lang tippen = ..", 8, kTitleY + kTitleH / 2, 90, kColLabel, middle_left);
  fontSmall(); 
  drawClipped(app.sdPath, kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20, kColInfo, top_center);

  const int count = sdBrowserCount();

  if (count == 0) {
    fontText(); 
    drawClipped("keine passenden Dateien", kScrW / 2, kListY + 40, kScrW - 20, kColWarn, middle_center);
  } else {
    const int selected = std::max(0, std::min(app.sdIndex, count - 1));
    const int start    = listWindowStart(selected, count);

    for (int row = 0; row < kListRows && start + row < count; ++row) {
      const int index = start + row;
      String left, right;
      const int slot = sdBrowserSlot(index);
      if (slot == -1) {
        left  = "..";
        right = "up";
      } else if (slot == -2) {
        left  = "[dieser Ordner]";
        right = "Zufall";
      } else {
        const DirEntryInfo& e = app.sdEntries[slot];
        left  = e.name;
        right = e.isDir ? "dir" : lowerExt(e.name);
      }
      drawListRow(row, left, right, index == selected);
    }
    drawScrollbar(start, count);
  }
  drawHintBar(app.sdPickMode ? "tippen = waehlen" : "lang = Ordner hoch", true, true);
}

// ---- Befehl fuer eine Karte auswaehlen ----------------------------------
void drawCmdPick() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("KARTEN-BEFEHL");
  fontSmall();
  drawClipped("wird auf die Karte geschrieben", kScrW / 2, kTitleY + kTitleH + 2,
              kScrW - 20, kColInfo, top_center);

  const int count    = static_cast<int>(cmdListCount());
  const int selected = std::max(0, std::min(app.cmdIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    const CardCommand c = cmdListAt(static_cast<size_t>(index));
    drawListRow(row, cardCommandLabel(c),
                index < static_cast<int>(kCmdFixedCount) ? "" : "CPU", index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("Eintrag antippen", true, true);
}

// ---- RFID-Seiten --------------------------------------------------------
void drawRfidScreen() {
  const bool writeMode = (app.screen == ScreenMode::RfidWrite ||
                          app.screen == ScreenMode::RfidRestore);
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);

  const char* title = "KARTE LESEN & STARTEN";
  if (app.screen == ScreenMode::RfidWrite)   title = "KARTE SCHREIBEN";
  if (app.screen == ScreenMode::RfidDump)    title = "KARTE AUF SD SICHERN";
  if (app.screen == ScreenMode::RfidRestore) title = "DUMP ZURUECKSCHREIBEN";
  drawTitleBar(title);

  const int boxY = kListY + 6;
  gDraw->fillRoundRect(16, boxY, kScrW - 32, 96, 10, kColPanel);
  gDraw->drawRoundRect(16, boxY, kScrW - 32, 96, 10, writeMode ? kColWarn : kColInfo);

  fontText(); 
  drawClipped(app.rfidReady ? "Karte auf den Reader legen" : "RFID2 nicht gefunden", kScrW / 2, boxY + 26, kScrW - 50, app.rfidReady ? kColText : kColErr, middle_center);

  if (app.rfidReady) {
    fontSmall();
    drawClipped("MIFARE Classic  oder  NTAG213/215/216", kScrW / 2, boxY + 42,
                kScrW - 50, kColLabel, middle_center);
  }

  if (writeMode) {
    const bool restore = (app.screen == ScreenMode::RfidRestore);
    const bool command = !restore && !app.pendingCardText.isEmpty();
    fontSmall();
    drawClipped(restore ? "Dump:" : (command ? "Befehl:" : "Pfad:"), kScrW / 2, boxY + 52,
                kScrW - 50, kColLabel, middle_center);
    fontSmall();
    drawClipped(restore ? app.pendingDump : (command ? app.pendingCardText : app.pendingPath),
                kScrW / 2, boxY + 70, kScrW - 50, kColOk, middle_center);
  } else {
    fontSmall(); 
    drawClipped(app.lastCardPath.isEmpty() ? String("noch keine Karte gelesen") : app.lastCardPath, kScrW / 2, boxY + 62, kScrW - 50, kColLabel, middle_center);
  }

  fontText(); 
  drawClipped(app.rfidHint, kScrW / 2, boxY + 112, kScrW - 30, kColInfo, middle_center);

  if (app.cardPowerOffPending) {
    const int32_t left = static_cast<int32_t>(app.cardPowerOffUntilMs - millis());
    drawHintBar((String("POWER OFF? ") + String(std::max<int32_t>(0, left) / 1000 + 1) +
                 "s  Flaeche antippen = JA").c_str());
  } else {
    drawHintBar("");
  }
}

// ---- Karten-Info --------------------------------------------------------
void drawRfidInfoScreen() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);

  const bool page2 = (app.infoPage == 1);
  drawTitleBar(page2 ? "KARTEN-INFO   ROHDATEN" : "KARTEN-INFO");

  const String* lines = page2 ? app.infoLines2 : app.infoLines;
  const size_t  count = page2 ? app.infoCount2 : app.infoCount;

  if (app.infoCount == 0) {
    fontText();
    drawClipped(app.rfidReady ? "Karte auf den Reader legen" : "RFID2 nicht gefunden",
                kScrW / 2, kListY + 40, kScrW - 30,
                app.rfidReady ? kColText : kColErr, middle_center);
    fontSmall();
    drawClipped("MIFARE Classic  oder  NTAG213/215/216", kScrW / 2, kListY + 62,
                kScrW - 30, kColLabel, middle_center);
  } else {
    constexpr int kValueX  = 78;
    constexpr int kRowStep = 13;
    const int valueW = kScrW - kValueX - 8;

    fontSmall();
    // Font0 ist eine Festbreitenschrift - so laesst sich die Zeilenlaenge
    // direkt ausrechnen, ohne jede Teilzeichenkette zu vermessen.
    const int charW   = std::max(1, gDraw->textWidth("W"));
    const size_t perRow = std::max<size_t>(8, static_cast<size_t>(valueW / charW));

    int y = kListY - 4;
    if (count == 0) {
      fontSmall();
      drawClipped("keine Rohdaten vorhanden", kValueX, y, valueW, kColLabel, top_left);
    }
    for (size_t i = 0; i < count && y <= kHintY - 12; ++i) {
      const String& line = lines[i];
      // Die ersten zehn Zeichen sind die Beschriftung, danach der Wert
      fontSmall();
      drawClipped(line.substring(0, 10), 10, y, 66, kColLabel, top_left);

      String value = line.substring(10);
      if (value.isEmpty()) { y += kRowStep; continue; }

      // Lange Werte umbrechen, moeglichst an einem Schraegstrich
      while (!value.isEmpty() && y <= kHintY - 12) {
        size_t cut = std::min(perRow, static_cast<size_t>(value.length()));
        if (cut < static_cast<size_t>(value.length())) {
          const int slash = value.lastIndexOf('/', static_cast<int>(cut) - 1);
          if (slash > static_cast<int>(cut) / 2) cut = static_cast<size_t>(slash) + 1;
        }
        fontSmall();
        drawClipped(value.substring(0, cut), kValueX, y, valueW, kColText, top_left);
        value = value.substring(cut);
        y += kRowStep;
      }
    }
  }

  if (app.infoCount == 0) {
    drawHintBar("");
  } else {
    drawHintBar(page2 ? "Seite 2/2 - antippen blaettert" : "Seite 1/2 - antippen blaettert");
  }
}

// ---- Fortschritt --------------------------------------------------------
void drawBusyScreen() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar(app.busyTitle.c_str());

  const int barX = 24;
  const int barW = kScrW - 48;
  const int barY = kListY + 54;

  fontText(); 
  drawClipped(app.busyDetail, kScrW / 2, kListY + 24, kScrW - 40, kColText, middle_center);

  gDraw->drawRoundRect(barX, barY, barW, 18, 5, kColLine);
  if (app.busyTotal > 0) {
    const int filled = static_cast<int>((static_cast<uint64_t>(barW - 4) * app.busySent) / app.busyTotal);
    gDraw->fillRect(barX + 2, barY + 2, std::max(0, filled), 14, kColOk);
    char buf[48];
    snprintf(buf, sizeof(buf), "%u / %u kB",
             static_cast<unsigned>(app.busySent / 1024), static_cast<unsigned>(app.busyTotal / 1024));
    fontSmall(); 
    drawClipped(buf, kScrW / 2, barY + 34, kScrW - 40, kColLabel, middle_center);
  } else {
    fontSmall(); 
    drawClipped("bitte warten...", kScrW / 2, barY + 34, kScrW - 40, kColLabel, middle_center);
  }
  drawHintBar("UPLOAD LAEUFT", false, false);
}

// ---- Gesamtausgabe ------------------------------------------------------
void drawFullScreenFor(ScreenMode screen) {
  switch (screen) {
    case ScreenMode::Home:
      gDraw->fillRect(0, kTileY0 - 2, kScrW, kScrH - (kTileY0 - 2), kColBg);
      drawTiles();
      drawHintBar("Kachel antippen", false, false);
      app.home.dirty = true;
      break;
    case ScreenMode::CpuMenu:   drawCpuMenu();    break;
    case ScreenMode::Status:    drawStatusScreen();break;
    case ScreenMode::Settings:  drawSettings();   break;
    case ScreenMode::SdBrowser: drawSdBrowser();  break;
    case ScreenMode::CmdPick:   drawCmdPick();    break;
    case ScreenMode::RfidRun:
    case ScreenMode::RfidWrite:
    case ScreenMode::RfidDump:
    case ScreenMode::RfidRestore: drawRfidScreen(); break;
    case ScreenMode::RfidInfo:  drawRfidInfoScreen(); break;
    case ScreenMode::WifiMenu:  drawWifiMenu();   break;
    case ScreenMode::WifiScan:  drawWifiScan();   break;
    case ScreenMode::WifiSaved: drawWifiSaved();  break;
    case ScreenMode::WifiCard:  drawWifiCard();   break;
    case ScreenMode::WifiPortal:drawWifiPortal(); break;
    case ScreenMode::Busy:      drawBusyScreen(); break;
  }
}

// Der Offscreen-Puffer behaelt seinen Inhalt zwischen zwei Bildern. Die
// Teilaktualisierungen (nur Statusleiste, nur Kacheln, nur Effektbereich)
// funktionieren damit genau wie beim direkten Zeichnen - am Ende wird das
// fertige Bild in einem Rutsch ins Display geschoben.
void pushFrame() {
  if (gUseCanvas) canvas.pushSprite(0, 0);
}

void render(uint32_t now) {
  const bool modalNow = modalVisible(now);

  // Ein pushSprite() kostet 150 kB ueber denselben SPI-Bus, an dem auch die
  // SD-Karte haengt. Das Bild wird deshalb nur geschoben, wenn in diesem
  // Durchlauf ueberhaupt etwas gezeichnet wurde - die Dirty-Logik bleibt
  // damit auch mit Offscreen-Puffer wirksam.
  bool drawn = false;

  if (app.barDirty || now - app.lastStatusDrawMs >= kStatusRefreshMs) {
    drawStatusBar();
    drawn = true;
  }

  if (app.screen == ScreenMode::Home) {
    const HomeMode mode = currentHomeMode();
    const bool animated = (mode != HomeMode::Static);

    if (app.screenDirty) {
      gDraw->fillRect(0, kTileY0 - 2, kScrW, kScrH - (kTileY0 - 2), kColBg);
      drawTiles();
      drawHintBar("Kachel antippen", false, false);
      app.screenDirty = false;
      app.home.dirty  = true;
      drawn = true;
    }

    if (animated) {
      drawHomeVisual(now);
      if (modalNow) drawModalOverlay(now);
      drawn = true;
    } else if (app.home.dirty || modalNow != app.modalVisibleLast) {
      drawHomeVisual(now);
      if (modalNow) drawModalOverlay(now);
      app.home.dirty = false;
      drawn = true;
    }
  } else {
    if (app.screenDirty) {
      drawFullScreenFor(app.screen);
      app.screenDirty = false;
      drawn = true;
    }
    if (modalNow) {
      drawModalOverlay(now);
      drawn = true;
    } else if (app.modalVisibleLast) {
      app.screenDirty = true;      // Overlay wieder wegputzen
    }
  }
  app.modalVisibleLast = modalNow;
  if (drawn) pushFrame();
}

void setScreen(ScreenMode next, uint32_t now) {
  if (app.screen == next) return;
  // Sobald der Benutzer selbst navigiert, gilt die Leseseite nicht mehr als
  // automatisch geoeffnet.
  if (next != ScreenMode::RfidRun) app.autoRfidActive = false;
  app.screen      = next;
  gListStart      = 0;          // neue Liste faengt wieder oben an
  app.screenDirty = true;
  app.home.dirty  = true;
  app.barDirty    = true;
  if (next == ScreenMode::Home) resetHomeAnimation(now);
  gDraw->fillRect(0, kTitleY, kScrW, kScrH - kTitleY, kColBg);
}

// ===========================================================================
//  WLAN-Einrichtung: Aktionen des Untermenues
// ===========================================================================
void wifiConnectProfile(size_t index, uint32_t now) {
  if (index >= gWifiCount) return;
  gWifiTry              = index;
  app.lastWiFiAttemptMs = 0;
  if (app.portalActive) stopPortal(now);      // beendet und verbindet selbst
  else                  beginWiFi(now);
  setModal("VERBINDE...", kColInfo, now, 1800);
  setScreen(ScreenMode::WifiMenu, now);
}

// Der Suchlauf blockiert einige Sekunden. Damit der Core nicht eingefroren
// wirkt, wird der Hinweis vorher noch einmal ausgegeben.
void wifiStartScan(uint32_t now) {
  setModal("SUCHE NETZE", kColInfo, now, 8000);
  render(now);
  wifiRunScan();
  app.modalText = "";

  if (app.wifiScanCount == 0) {
    setModal("NICHTS GEFUNDEN", kColWarn, now, 1800);
    beep(500, 120);
    app.screenDirty = true;
    return;
  }
  beep(2400, 30);
  setScreen(ScreenMode::WifiScan, now);
}

void wifiChooseNetwork(int index, uint32_t now) {
  if (index < 0 || index >= static_cast<int>(app.wifiScanCount)) return;
  const WifiScanEntry entry = gWifiScan[index];

  // Bereits gespeichert? Dann reicht ein Verbindungsversuch.
  const int known = wifiProfileIndex(entry.ssid);
  if (known >= 0) {
    wifiConnectProfile(static_cast<size_t>(known), now);
    return;
  }

  // Offenes Netz braucht kein Passwort.
  if (entry.open) {
    wifiAddProfile(entry.ssid, "");
    app.lastWiFiAttemptMs = 0;
    beginWiFi(now);
    setModal("GESPEICHERT", kColOk, now, 1600);
    setScreen(ScreenMode::WifiMenu, now);
    return;
  }

  app.wifiPendingSsid = entry.ssid;
  app.wifiHint = app.rfidReady ? "Karte auflegen..." : "kein RFID2 - Portal nutzen";
  setScreen(ScreenMode::WifiCard, now);
}

void wifiMenuSelect(uint32_t now) {
  const int index = std::max(0, std::min(app.wifiMenuIndex, static_cast<int>(kWifiMenuCount) - 1));
  beep(2200, 25);

  switch (index) {
    case kWifiScanNow:
      wifiStartScan(now);
      break;

    case kWifiFromSd: {
      String error;
      const size_t added = loadWifiFromSd(&error);
      if (added > 0) {
        app.lastWiFiAttemptMs = 0;
        gWifiTry = 0;
        beginWiFi(now);
        setModal(String(static_cast<unsigned>(added)) + " NETZ(E) GELADEN", kColOk, now, 2000);
      } else {
        beep(500, 120);
        setModal(error.isEmpty() ? String("SD-FEHLER") : error, kColErr, now, 2400);
      }
      break;
    }

    case kWifiPortal:
      // Vor dem Portal einmal suchen, damit die Weboberflaeche eine Netzliste
      // anbieten kann.
      setModal("PORTAL STARTET", kColInfo, now, 8000);
      render(now);
      wifiRunScan();
      app.modalText = "";
      startPortal(now);
      setScreen(ScreenMode::WifiPortal, now);
      break;

    case kWifiConnectSaved:
      app.wifiSavedDelete = false;
      app.wifiSavedToCard = false;
      app.wifiSavedIndex  = 0;
      setScreen(ScreenMode::WifiSaved, now);
      break;

    case kWifiToCard:
      // Ein gespeichertes Netz (z.B. gerade ueber das Portal eingegeben) auf
      // eine NFC-Karte schreiben - damit laesst sich ein weiteres Geraet ohne
      // Tipperei einrichten.
      if (gWifiCount == 0) { setModal("NICHTS GESPEICHERT", kColWarn, now, 1600); break; }
      if (!app.rfidReady)  { setModal("KEIN RFID2", kColErr, now, 1800); break; }
      app.wifiSavedDelete = false;
      app.wifiSavedToCard = true;
      app.wifiSavedIndex  = 0;
      setScreen(ScreenMode::WifiSaved, now);
      break;

    case kWifiToSd: {
      // Alle gespeicherten Netze als /wifi.txt auf die SD-Karte schreiben -
      // das Gegenstueck zu "Von SD laden" und damit ein einfacher Weg, die
      // Zugangsdaten auf ein zweites Geraet zu bringen.
      if (gWifiCount == 0) { setModal("NICHTS GESPEICHERT", kColWarn, now, 1600); break; }
      String error;
      const size_t written = saveWifiToSd(&error);
      if (written > 0) {
        setModal(String(static_cast<unsigned>(written)) + " NETZ(E) AUF SD", kColOk, now, 2000);
      } else {
        beep(500, 120);
        setModal(error.isEmpty() ? String("SD-FEHLER") : error, kColErr, now, 2400);
      }
      break;
    }

    case kWifiDeleteOne:
      if (gWifiCount == 0) { setModal("NICHTS GESPEICHERT", kColWarn, now, 1600); break; }
      app.wifiSavedDelete = true;
      app.wifiSavedToCard = false;
      app.wifiSavedIndex  = 0;
      setScreen(ScreenMode::WifiSaved, now);
      break;

    case kWifiDeleteAll:
      wifiClearProfiles();
      WiFi.disconnect(false, true);
      setModal("ALLE GELOESCHT", kColWarn, now, 1800);
      break;
  }
  app.screenDirty = true;
}

void wifiSavedSelect(uint32_t now) {
  const int count = static_cast<int>(gWifiCount);
  if (count == 0) { setScreen(ScreenMode::WifiMenu, now); return; }

  const int index = std::max(0, std::min(app.wifiSavedIndex, count - 1));
  beep(2200, 25);

  if (app.wifiSavedDelete) {
    const String ssid = gWifiProfiles[index].ssid;
    wifiRemoveProfile(static_cast<size_t>(index));
    app.wifiSavedIndex = std::max(0, index - 1);
    setModal("GELOESCHT: " + ssid, kColWarn, now, 1400);
    if (gWifiCount == 0) setScreen(ScreenMode::WifiMenu, now);
    app.screenDirty = true;
    return;
  }

  if (app.wifiSavedToCard) {
    const WifiProfile& profile = gWifiProfiles[index];
    app.pendingCardText = wifiCardText(profile.ssid, profile.pass);
    app.pendingPath     = "";
    app.rfidHint        = profile.ssid;
    app.wifiSavedToCard = false;
    setScreen(ScreenMode::RfidWrite, now);
    return;
  }

  wifiConnectProfile(static_cast<size_t>(index), now);
}

// ===========================================================================
//  Aktionen der Einstellungen
// ===========================================================================
void activateSetting(uint32_t now) {
  // Der WLAN-Punkt ist eine Aktion, braucht aber keinen Kartenleser und wird
  // deshalb vorab behandelt.
  if (app.settingsIndex == kSetWifi) {
    beep(2200, 25);
    app.wifiMenuIndex   = 0;
    app.wifiSavedDelete = false;
    app.wifiSavedToCard = false;
    setScreen(ScreenMode::WifiMenu, now);
    return;
  }

  // Die uebrigen Aktionen am Listenanfang brauchen alle den Kartenleser.
  if (app.settingsIndex <= kSetLastAction) {
    if (!app.rfidReady) {
      setModal("KEIN RFID2", kColErr, now, 1800);
      return;
    }
    beep(2200, 25);
    switch (app.settingsIndex) {
      case kSetNfcWrite:
        app.pendingCardText = "";        // NFC-Write
        openSdBrowser(1, now);
        break;
      case kSetNfcInfo:
        app.infoCount = 0;               // NFC-Info
        app.infoPage  = 0;
        app.infoUid   = "";
        app.rfidHint  = "warte auf Karte";
        setScreen(ScreenMode::RfidInfo, now);
        break;
      case kSetNfcDump:
        app.rfidHint = "Karte auflegen zum Sichern";
        setScreen(ScreenMode::RfidDump, now);
        break;
      case kSetNfcRestore:
        app.pendingCardText = "";
        openSdBrowser(2, now);           // NFC-Restore: Dump waehlen
        break;
      case kSetNfcCmd:                   // NFC-Cmd: Befehl waehlen
        // Die CPU-Stufen kommen vom c64u - einmal nachladen, damit die Liste
        // die tatsaechlich moeglichen Werte anbietet.
        if (!app.cpuPathKnown) refreshCpuValue();
        app.cmdIndex = 0;
        setScreen(ScreenMode::CmdPick, now);
        break;
      case kSetNfcRandom:
        // NFC-Zufall: Verzeichnis waehlen
        app.pendingCardText = "";
        openSdBrowser(3, now);
        break;
      default:
        break;
    }
    return;
  }

  switch (app.settingsIndex) {
    case kSetAutoNfc: {
      uint8_t next = static_cast<uint8_t>(app.settings.autoNfc) + 1;
      if (next > static_cast<uint8_t>(AutoNfcMode::Fast)) next = 0;
      app.settings.autoNfc = static_cast<AutoNfcMode>(next);
      break;
    }
    case kSetCardConfirm: app.settings.cardConfirmS = nextCardConfirm(app.settings.cardConfirmS); break;
    case kSetPowerOffTime: app.settings.powerOffConfirmDs = nextTimeStep(app.settings.powerOffConfirmDs); break;
    case kSetAnimations:
      app.settings.animationsEnabled = !app.settings.animationsEnabled;
      break;
    case kSetEffect: {
      uint8_t next = static_cast<uint8_t>(app.settings.effectMode) + 1;
      if (next > static_cast<uint8_t>(DisplayEffectMode::Raster)) next = 0;
      app.settings.effectMode = static_cast<DisplayEffectMode>(next);
      break;
    }
    case kSetFxDetail:
      app.settings.fxDetail = (app.settings.fxDetail == FxDetailMode::Half) ? FxDetailMode::Full
                                                                           : FxDetailMode::Half;
      break;
    case kSetAnimSpeed: {
      uint8_t next = static_cast<uint8_t>(app.settings.animationSpeed) + 1;
      if (next > static_cast<uint8_t>(AnimationSpeedMode::Fast)) next = 0;
      app.settings.animationSpeed = static_cast<AnimationSpeedMode>(next);
      break;
    }
    case kSetEffectTime: {
      uint8_t next = static_cast<uint8_t>(app.settings.effectDuration) + 1;
      if (next > static_cast<uint8_t>(EffectDurationMode::Long)) next = 0;
      app.settings.effectDuration = static_cast<EffectDurationMode>(next);
      break;
    }
    case kSetStaticTime: {
      uint8_t next = static_cast<uint8_t>(app.settings.staticDuration) + 1;
      if (next > static_cast<uint8_t>(StaticDurationMode::Long)) next = 0;
      app.settings.staticDuration = static_cast<StaticDurationMode>(next);
      break;
    }
    case kSetBrightness:
      app.settings.brightness = nextBrightnessValue(app.settings.brightness);
      applyBrightness();
      break;
    case kSetLed:
      app.settings.ledEnabled = !app.settings.ledEnabled;
      // Umschalten sofort sichtbar machen, ohne auf den Takt zu warten.
      app.ledNextMs  = 0;
      for (uint32_t& c : app.ledLastRgb) c = 0xFFFFFFFFu;
      break;
    case kSetLedBright:
      app.settings.ledBrightness = nextLedBrightness(app.settings.ledBrightness);
      app.ledNextMs  = 0;
      break;
    case kSetDiskAction: {
      uint8_t next = static_cast<uint8_t>(app.settings.diskAction) + 1;
      if (next > static_cast<uint8_t>(DiskActionMode::MountRun)) next = 0;
      app.settings.diskAction = static_cast<DiskActionMode>(next);
      break;
    }
    case kSetDiskDrive: {
      uint8_t next = static_cast<uint8_t>(app.settings.uploadDrive) + 1;
      if (next > static_cast<uint8_t>(UploadDriveMode::DriveB)) next = 0;
      app.settings.uploadDrive = static_cast<UploadDriveMode>(next);
      break;
    }
    case kSetBeep:
      app.settings.beepEnabled = !app.settings.beepEnabled;
      break;
    case kSetFactoryReset:
      loadDefaultSettings();
      applyBrightness();
      break;
  }
  saveSettings();
  resetHomeAnimation(now);
  beep(2200, 25);
  app.screenDirty = true;
}

// ===========================================================================
//  Kacheln ausloesen
// ===========================================================================
void openSdBrowser(uint8_t forCard, uint32_t now) {
  app.sdPickMode = forCard;
  if (!app.sdReady && !initSd()) {
    setModal("KEINE SD-KARTE", kColErr, now, 2000);
    return;
  }
  // Dumps liegen immer im eigenen Ordner - direkt dort starten
  if (forCard == 2) app.sdPath = kDumpDir;
  if (!readDirectory(app.sdPath)) {
    app.sdPath = "/";
    if (!readDirectory("/")) {
      setModal("SD NICHT LESBAR", kColErr, now, 2000);
      return;
    }
  }
  setScreen(ScreenMode::SdBrowser, now);
}

void activateTile(uint32_t now) {
  switch (app.tileIndex) {
    case kTileReset:     performReset(now);      break;
    case kTileReboot:    performHardReset(now);  break;
    case kTileUltiMenu:  performMenuButton(now); break;
    case kTilePowerOff:
      requestPowerOff(now);          // Kachel fragt immer nach
      app.screenDirty = true;
      break;

    case kTileCpu:
      clearPendingPowerOff();
      refreshCpuValue();
      app.cpuIndex = cpuIndexFromValue(app.currentCpuValue);
      setScreen(ScreenMode::CpuMenu, now);
      break;

    case kTileRfidRun:
      clearPendingPowerOff();
      if (!app.rfidReady) { setModal("KEIN RFID2", kColErr, now, 1800); break; }
      app.rfidHint     = "warte auf Karte";
      // Von Hand geoeffnet: das Zurueck-Feld fuehrt zum Hauptbildschirm.
      app.autoRfidFrom = ScreenMode::Home;
      setScreen(ScreenMode::RfidRun, now);
      break;

    case kTileSdBrowse:
      clearPendingPowerOff();
      openSdBrowser(0, now);
      break;

    case kTileStatus:
      clearPendingPowerOff();
      refreshConnectionStatus(now, true);
      setScreen(ScreenMode::Status, now);
      break;

    case kTileSetup:
      clearPendingPowerOff();
      app.settingsIndex = 0;
      setScreen(ScreenMode::Settings, now);
      break;
  }
}

// ===========================================================================
//  SD-Browser Auswahl
// ===========================================================================
void sdBrowserSelect(uint32_t now) {
  const int count = sdBrowserCount();
  if (count == 0) return;

  const int index = std::max(0, std::min(app.sdIndex, count - 1));
  const int slot  = sdBrowserSlot(index);

  if (slot == -1) {
    const String up = parentPath(app.sdPath);
    readDirectory(up);
    gListStart      = 0;          // neues Verzeichnis faengt oben an
    app.screenDirty = true;
    return;
  }

  // Zufallskarte: das gerade geoeffnete Verzeichnis selbst nehmen
  if (slot == -2) {
    const String randomPath = randomCardPath(app.sdPath);
    if (pathToCardText(randomPath).length() > kMaxTextLen) {
      setModal("PFAD ZU LANG", kColErr, now, 2200);
      return;
    }
    app.pendingCardText = "";
    app.pendingPath     = randomPath;
    app.rfidHint        = "Karte auflegen";
    setScreen(ScreenMode::RfidWrite, now);
    return;
  }

  const DirEntryInfo& entry = app.sdEntries[slot];
  const String full = joinPath(app.sdPath, entry.name);

  if (entry.isDir) {
    readDirectory(full);
    gListStart      = 0;
    app.screenDirty = true;
    return;
  }

  if (app.sdPickMode == 2) {
    app.pendingDump = full;
    app.rfidHint    = "Karte auflegen zum Zurueckschreiben";
    setScreen(ScreenMode::RfidRestore, now);
    return;
  }

  if (app.sdPickMode == 1) {
    const String cardText = pathToCardText(full);
    if (cardText.length() > kMaxTextLen) {
      setModal("PFAD ZU LANG", kColErr, now, 2200);
      return;
    }
    app.pendingPath = full;
    app.rfidHint    = "Karte auflegen zum Schreiben";
    setScreen(ScreenMode::RfidWrite, now);
  } else {
    startFileOnC64(full, now);
  }
}

// ===========================================================================
//  RFID-Polling
// ===========================================================================
// ---------------------------------------------------------------------------
// Kartenbefehl ausfuehren
//
// PowerOff mit Bestaetigung laeuft ueber cardPowerOffPending: die Karte wird
// weggenommen und innerhalb des Zeitfensters erneut aufgelegt. Ein Druck auf
// B bestaetigt ebenfalls, ein Abbruch geschieht durch Abwarten.
// ---------------------------------------------------------------------------
void runCardCommand(const CardCommand& cmd, const String& uid, uint32_t now) {
  app.rfidHint    = cardCommandLabel(cmd);
  app.screenDirty = true;

  switch (cmd.cmd) {
    case CardCmd::Reset:
      beep(2600, 50);
      performReset(now);
      return;

    case CardCmd::Reboot:
      beep(2400, 50);
      performHardReset(now);
      return;

    case CardCmd::UltiMenu:
      beep(2200, 50);
      performMenuButton(now);
      return;

    case CardCmd::PowerOff: {
      const uint8_t sec = cardPowerOffSeconds(cmd);

      // Zweites Auflegen derselben Karte innerhalb des Fensters bestaetigt.
      if (app.cardPowerOffPending && uid == app.cardPowerOffUid &&
          static_cast<int32_t>(now - app.cardPowerOffUntilMs) < 0) {
        app.cardPowerOffPending = false;
        beep(1800, 60);
        performPowerOff(now);
        return;
      }

      if (sec == 0) {                       // "CMD:POWEROFF=0" - ohne Nachfrage
        beep(1800, 60);
        performPowerOff(now);
        return;
      }

      app.cardPowerOffPending = true;
      app.cardPowerOffUid     = uid;
      app.cardPowerOffUntilMs = now + sec * 1000u;
      beep(900, 60);
      app.rfidHint = "Karte nochmal auflegen";
      setModal("POWER OFF? KARTE NOCHMAL!", kColWarn, now, sec * 1000u);
      return;
    }

    case CardCmd::CpuSpeed: {
      if (!app.cpuPathKnown) refreshCpuValue();
      const int index = cpuIndexFromValue(cmd.arg);
      beep(2400, 40);
      setCpuSpeed(index, now);
      return;
    }

    default:
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      setModal("BEFEHL UNBEKANNT", kColErr, now, 2000);
      return;
  }
}

// Verarbeitet eine bereits ausgewaehlte Karte passend zum aktuellen Bildschirm.
void processCard(uint32_t now) {
  const CardKind kind = cardKind();
  if (kind == CardKind::None) {
    rfidRelease();
    app.rfidHint = "Kartentyp nicht unterstuetzt";
    app.screenDirty = true;
    beep(500, 120);
    ledFlash(ledRgb(255, 0, 0));
    return;
  }

  const String uid = cardUidString();

  if (app.screen == ScreenMode::RfidInfo) {
    // Einmal einlesen und stehen lassen. Sonst wuerde die Anzeige bei jedem
    // Poll neu aufgebaut und beim Wegnehmen der Karte mit halben Daten
    // ueberschrieben. Eine andere Karte loest natuerlich ein neues Einlesen aus.
    if (app.infoCount > 0 && uid == app.infoUid) {
      rfidRelease();
      return;
    }
    collectCardInfo(kind);
    app.infoUid = uid;
    rfidRelease();
    beep(2400, 40);
    app.screenDirty = true;
    return;
  }

  if (app.screen == ScreenMode::RfidDump) {
    String error, file;
    const bool ok = dumpCardToSd(kind, &file, &error);
    rfidRelease();
    beep(ok ? 2800 : 500, ok ? 60 : 160);
    ledFlash(ok ? ledRgb(0, 255, 80) : ledRgb(255, 0, 0));
    app.rfidHint    = ok ? ("gesichert: " + baseName(file)) : error;
    app.screenDirty = true;
    setModal(ok ? "AUF SD GESICHERT" : "SICHERN FEHLGESCHLAGEN",
             ok ? kColOk : kColErr, now, 2200);
    return;
  }

  if (app.screen == ScreenMode::RfidRestore) {
    String error;
    const bool ok = restoreDumpToCard(kind, app.pendingDump, &error);
    rfidRelease();
    beep(ok ? 2800 : 500, ok ? 60 : 160);
    ledFlash(ok ? ledRgb(0, 255, 80) : ledRgb(255, 0, 0));
    app.rfidHint    = ok ? (String(cardKindLabel(kind)) + "  " + uid) : error;
    app.screenDirty = true;
    setModal(ok ? "KARTE BESCHRIEBEN" : "SCHREIBFEHLER",
             ok ? kColOk : kColErr, now, 2200);
    return;
  }

  if (app.screen == ScreenMode::RfidWrite) {
    String error;
    // Befehlskarten bringen ihren Text fertig mit, sonst wird der Dateipfad
    // in das Kartenformat uebersetzt.
    const String text = app.pendingCardText.isEmpty() ? pathToCardText(app.pendingPath)
                                                      : app.pendingCardText;
    const bool ok = writeCardText(kind, text, &error);
    rfidRelease();
    beep(ok ? 2800 : 500, ok ? 60 : 160);
    ledFlash(ok ? ledRgb(0, 255, 80) : ledRgb(255, 0, 0));
    app.rfidHint = ok ? (String(cardKindLabel(kind)) + "  " + uid) : error;
    app.screenDirty = true;
    if (ok) setModal("KARTE OK", kColOk, now, 1800);
    else    setModal("SCHREIBFEHLER", kColErr, now, 2200);
    return;
  }

  // ---- Lesen und starten ----
  const CardContent content = readCardContent(kind);
  rfidRelease();

  if (!content.ok) {
    beep(500, 140);
    ledFlash(ledRgb(255, 0, 0));
    app.rfidHint = content.error;
    app.wifiHint = content.error;
    app.screenDirty = true;
    setModal("KARTE LEER?", kColWarn, now, 2000);
    return;
  }

  // ---- WLAN-Passwortkarte auf der Einrichtungsseite ----
  if (app.screen == ScreenMode::WifiCard) {
    String ssid = app.wifiPendingSsid;
    String pass = trimCopy(content.text);
    // Karten im WLAN-Schema bringen die SSID selbst mit. Alles andere gilt
    // als reines Passwort fuer das vorher gewaehlte Netz.
    parseWifiText(content.text, &ssid, &pass);

    if (ssid.isEmpty()) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.wifiHint = "Karte ohne SSID";
      app.screenDirty = true;
      setModal("KEIN NETZ", kColErr, now, 2000);
      return;
    }
    // Programm- oder Befehlskarten sind hier mit Sicherheit ein Versehen.
    CardCommand strayCommand;
    if (!textLooksLikeWifi(content.text) &&
        (pass.startsWith("/") || parseCardCommand(content.text, &strayCommand))) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.wifiHint = "das ist keine WLAN-Karte";
      app.screenDirty = true;
      setModal("FALSCHE KARTE", kColErr, now, 2200);
      return;
    }
    if (!wifiAddProfile(ssid, pass)) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.wifiHint = "SSID oder Passwort zu lang";
      app.screenDirty = true;
      setModal("KARTE UNGUELTIG", kColErr, now, 2200);
      return;
    }

    beep(2800, 60);
    app.wifiPendingSsid   = "";
    app.wifiHint          = ssid;
    app.lastWiFiAttemptMs = 0;
    gWifiTry              = 0;
    beginWiFi(now);
    setModal("WLAN GESPEICHERT", kColOk, now, 2000);
    setScreen(ScreenMode::WifiMenu, now);
    return;
  }

  // ---- WLAN-Karte ausserhalb der Einrichtung ----
  //
  // Die Karte ist kein Dateipfad. Statt nur darauf hinzuweisen, wird das Netz
  // gleich uebernommen und eine Verbindung aufgebaut: Karte auflegen genuegt,
  // der Umweg ueber Setup > WLAN entfaellt.
  if (textLooksLikeWifi(content.text)) {
    String ssid;
    String pass;
    if (!parseWifiText(content.text, &ssid, &pass) || ssid.isEmpty()) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = "WLAN-Karte ohne SSID";
      app.screenDirty = true;
      setModal("KEIN NETZ", kColErr, now, 2200);
      return;
    }

    // Laeuft die Verbindung bereits, ist nichts zu tun. Das faengt auch den
    // Fall ab, dass die Karte liegen bleibt und die Hintergrundabfrage sie
    // immer wieder erkennt.
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
      beep(2400, 40);
      app.rfidHint = ssid;
      app.screenDirty = true;
      setModal("SCHON VERBUNDEN", kColOk, now, 1800);
      return;
    }

    if (!wifiAddProfile(ssid, pass)) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = "SSID oder Passwort zu lang";
      app.screenDirty = true;
      setModal("KARTE UNGUELTIG", kColErr, now, 2200);
      return;
    }

    // wifiAddProfile sortiert das Netz nach vorne; von dort aus wird gezielt
    // dieses eine Netz versucht und nicht die ganze Liste durchgegangen.
    const int index = wifiProfileIndex(ssid);
    gWifiTry              = index >= 0 ? static_cast<size_t>(index) : 0;
    app.lastWiFiAttemptMs = 0;
    beep(2600, 50);

    app.rfidHint    = ssid;
    app.screenDirty = true;
    setModal("VERBINDE...", kColInfo, now, kWifiCardConnectMs + 1500);
    render(now);

    if (app.portalActive) stopPortal(now);    // beendet den AP und verbindet selbst
    else                  beginWiFi(now);

    // Kurz auf das Ergebnis warten, damit die Rueckmeldung etwas taugt. Laenger
    // als kWifiCardConnectMs wird nicht gewartet - der Rest laeuft ueber die
    // regelmaessigen Versuche in serviceWiFi weiter.
    const uint32_t deadline = millis() + kWifiCardConnectMs;
    while (millis() < deadline && WiFi.status() != WL_CONNECTED) delay(100);

    const bool connected = WiFi.status() == WL_CONNECTED;
    app.modalText = "";
    beep(connected ? 2800 : 500, connected ? 60 : 140);
    app.rfidHint = connected ? (ssid + "  " + WiFi.localIP().toString())
                             : (ssid + " nicht erreichbar");
    app.screenDirty = true;
    setModal(connected ? "WLAN AKTIV" : "NETZ NICHT DA",
             connected ? kColOk : kColWarn, millis(), 2400);
    return;
  }

  Serial.printf("Karte gelesen (%s): '%s'\n",
                content.isNdef ? "NDEF-Text" : "Altformat", content.text.c_str());

  // ---- Befehlskarte? Dann ist weder SD noch Datei noetig ----
  CardCommand command;
  if (parseCardCommand(content.text, &command)) {
    runCardCommand(command, uid, now);
    return;
  }
  {
    // Karte traegt zwar das Praefix, aber kein bekanntes Schluesselwort -
    // dann ist es sicher kein Dateipfad.
    String head = content.text.substring(0, 4);
    head.toUpperCase();
    if (head == kCardCmdPrefix) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = content.text;
      setModal("BEFEHL UNBEKANNT", kColErr, now, 2200);
      return;
    }
  }
  if (app.cardPowerOffPending) app.cardPowerOffPending = false;   // andere Karte bricht ab

  String path = cardTextToPath(content.text);

  // "?" als Dateiname oder ein Verzeichnis: zufaellige Datei daraus starten
  if (pathIsRandom(path) || pathIsDirectory(path)) {
    const String picked = resolveRandomFile(path);
    if (picked.isEmpty()) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = "Verzeichnis leer: " + path;
      app.screenDirty = true;
      setModal("NICHTS GEFUNDEN", kColWarn, now, 2200);
      return;
    }
    path = picked;
  }

  app.lastCardPath = path;
  app.rfidHint     = path;
  app.screenDirty  = true;
  beep(2600, 50);
  ledFlash(ledRgb(0, 255, 80));
  startFileOnC64(path, millis());
}

// ---------------------------------------------------------------------------
// RFID-Ablauf
//
// Zwei Betriebsarten:
//
//   1. Auf den RFID-Seiten wird alle 250 ms voll abgefragt - dort wartet der
//      Benutzer ohnehin auf die Karte.
//   2. Auf dem Hauptbildschirm laeuft je nach Einstellung "Auto-NFC" eine
//      schnelle Probe im Hintergrund. Wird dabei eine Karte erkannt, wechselt
//      die Anzeige selbsttaetig in den Lesemodus und das hinterlegte Programm
//      startet - die Kachel RFID muss man dafuer nicht mehr anwaehlen.
//
// Die Probe kostet dank des verkuerzten Zeitfensters (siehe cardPresentQuick)
// nur wenige Millisekunden. Bei 0,7 s Abstand liegt die Grundlast damit unter
// einem Prozent, ein Ruckeln der Effekte ist nicht sichtbar.
// ---------------------------------------------------------------------------
bool onRfidScreen() {
  return app.screen == ScreenMode::RfidRun   || app.screen == ScreenMode::RfidWrite ||
         app.screen == ScreenMode::RfidInfo  || app.screen == ScreenMode::RfidDump  ||
         app.screen == ScreenMode::RfidRestore || app.screen == ScreenMode::WifiCard;
}

// Seiten, auf denen die Hintergrundabfrage laufen darf.
//
// Frueher war das nur der Hauptbildschirm - im SD-Browser wurde eine
// aufgelegte Karte schlicht nicht bemerkt. Erlaubt sind jetzt alle Seiten, auf
// denen man nur schaut oder blaettert. Ausgenommen bleiben die Seiten, auf
// denen gerade MIT Karten gearbeitet wird: dort waere ein Programmstart genau
// das Gegenteil dessen, was der Benutzer gerade tut.
bool autoNfcScreen() {
  switch (app.screen) {
    case ScreenMode::Home:
    case ScreenMode::Status:
    case ScreenMode::CpuMenu:
      return true;
    case ScreenMode::SdBrowser:
      // Nur beim normalen Blaettern. Wird gerade eine Datei fuer eine Karte
      // ausgesucht (NFC-Write) oder ein Dump zum Zurueckschreiben gewaehlt,
      // darf eine aufgelegte Karte nichts starten.
      return app.sdPickMode == 0;
    default:
      return false;
  }
}

void serviceRfid(uint32_t now) {
  if (!app.rfidReady) return;

  // ---- 1. Regulaere Abfrage auf den RFID-Seiten ----
  if (onRfidScreen()) {
    if (now - app.lastRfidPollMs < kRfidPollMs) return;
    app.lastRfidPollMs = now;
    if (!cardPresent()) return;
    processCard(now);
    return;
  }

  // ---- 2. Hintergrundabfrage ----
  if (app.settings.autoNfc == AutoNfcMode::Off) return;
  if (!autoNfcScreen()) return;
  if (now - app.lastRfidPollMs < autoNfcIntervalMs(app.settings.autoNfc)) return;
  app.lastRfidPollMs = now;

  if (!cardPresentQuick()) return;

  // Karte liegt auf: in den Lesemodus wechseln, Bild sofort zeigen und
  // dieselbe Verarbeitung anstossen wie auf der RFID-Seite.
  beep(2200, 30);
  app.rfidHint        = "Karte erkannt";
  app.autoRfidActive    = true;
  app.autoRfidFrom      = app.screen;
  app.autoRfidSdPath    = app.sdPath;
  app.autoRfidSdIndex   = app.sdIndex;
  app.autoRfidListStart = gListStart;
  setScreen(ScreenMode::RfidRun, now);
  render(millis());
  processCard(millis());

  // Danach bleibt die Leseseite noch eine Weile stehen, damit gleich die
  // naechste Karte aufgelegt werden kann.
  app.autoRfidUntilMs = millis() + kAutoRfidHoldMs;
}

// ===========================================================================
//  Listen: Zaehlung, Auswahl, Bewegung
//
//  Die drei Helfer buendeln, was jede Listenseite gemeinsam hat. Sie stehen
//  vor moveSelection(), weil sie dort gebraucht werden.
// ===========================================================================

// Seiten mit einer scrollbaren Liste. Nur dort sind Wischen und Scrollleiste
// sinnvoll.
bool listScreen(ScreenMode screen) {
  switch (screen) {
    case ScreenMode::CpuMenu:
    case ScreenMode::Settings:
    case ScreenMode::SdBrowser:
    case ScreenMode::CmdPick:
    case ScreenMode::WifiMenu:
    case ScreenMode::WifiScan:
    case ScreenMode::WifiSaved:
      return true;
    default:
      return false;
  }
}

// Anzahl der Eintraege der Liste auf dem aktuellen Bildschirm
int listCount() {
  switch (app.screen) {
    case ScreenMode::CpuMenu:   return static_cast<int>(app.cpuChoiceCount);
    case ScreenMode::Settings:  return static_cast<int>(kSettingsCount);
    case ScreenMode::SdBrowser: return sdBrowserCount();
    case ScreenMode::CmdPick:   return static_cast<int>(cmdListCount());
    case ScreenMode::WifiMenu:  return static_cast<int>(kWifiMenuCount);
    case ScreenMode::WifiScan:  return static_cast<int>(app.wifiScanCount);
    case ScreenMode::WifiSaved: return static_cast<int>(gWifiCount);
    default:                    return 0;
  }
}

// Zeiger auf den Auswahlindex der Liste des aktuellen Bildschirms
int* listIndex() {
  switch (app.screen) {
    case ScreenMode::CpuMenu:   return &app.cpuIndex;
    case ScreenMode::Settings:  return &app.settingsIndex;
    case ScreenMode::SdBrowser: return &app.sdIndex;
    case ScreenMode::CmdPick:   return &app.cmdIndex;
    case ScreenMode::WifiMenu:  return &app.wifiMenuIndex;
    case ScreenMode::WifiScan:  return &app.wifiScanIndex;
    case ScreenMode::WifiSaved: return &app.wifiSavedIndex;
    default:                    return nullptr;
  }
}

// Auswahl um `delta` Eintraege bewegen. Wird von den Blaetterfeldern der
// Fussleiste benutzt; die Ansicht zieht dabei ausdruecklich nach.
void moveSelection(int delta) {
  if (app.screen == ScreenMode::RfidInfo) {
    if (app.infoCount > 0) {
      app.infoPage    = app.infoPage ? 0 : 1;   // zwischen den Seiten wechseln
      app.screenDirty = true;
    }
    return;
  }

  const int count  = listCount();
  int*      target = listIndex();
  if (count <= 0 || target == nullptr) return;

  *target = ((*target + delta) % count + count) % count;
  listEnsureVisible(*target, count);
  app.screenDirty = true;
}

void handleSelect(uint32_t now) {
  switch (app.screen) {
    case ScreenMode::Home:      activateTile(now);          break;
    case ScreenMode::CpuMenu:   setCpuSpeed(app.cpuIndex, now); app.screenDirty = true; break;
    case ScreenMode::Settings:  activateSetting(now);       break;
    case ScreenMode::SdBrowser: sdBrowserSelect(now);       break;
    case ScreenMode::Status:    runConnectionTest(now); app.screenDirty = true; break;
    case ScreenMode::CmdPick: {
      const int count = static_cast<int>(cmdListCount());
      if (count <= 0) break;
      const int index = std::max(0, std::min(app.cmdIndex, count - 1));
      const CardCommand c = cmdListAt(static_cast<size_t>(index));
      app.pendingCardText = cardCommandText(c);
      app.pendingPath     = "";
      app.rfidHint        = "Karte auflegen";
      setScreen(ScreenMode::RfidWrite, now);
      break;
    }
    case ScreenMode::RfidRun:
      // Laeuft eine PowerOff-Rueckfrage von der Karte, bestaetigt B sie.
      if (app.cardPowerOffPending &&
          static_cast<int32_t>(now - app.cardPowerOffUntilMs) < 0) {
        app.cardPowerOffPending = false;
        beep(1800, 60);
        performPowerOff(now);
        break;
      }
      setScreen(ScreenMode::Home, now);
      break;
    case ScreenMode::RfidWrite:
    case ScreenMode::RfidDump:
    case ScreenMode::RfidRestore:
    case ScreenMode::RfidInfo:  setScreen(ScreenMode::Home, now); break;

    case ScreenMode::WifiMenu:
      wifiMenuSelect(now);
      break;
    case ScreenMode::WifiScan:
      if (app.wifiScanCount == 0) setScreen(ScreenMode::WifiMenu, now);
      else                        wifiChooseNetwork(app.wifiScanIndex, now);
      break;
    case ScreenMode::WifiSaved:
      wifiSavedSelect(now);
      break;
    case ScreenMode::WifiCard:
      app.wifiPendingSsid = "";
      setScreen(ScreenMode::WifiMenu, now);
      break;
    case ScreenMode::WifiPortal:
      stopPortal(now);
      setScreen(ScreenMode::WifiMenu, now);
      break;

    case ScreenMode::Busy:      break;
  }
}

// Zurueck an den Ort, von dem aus die Hintergrundabfrage die Leseseite
// geoeffnet hat.
//
// Zwei Dinge muessen dabei von Hand wiederhergestellt werden:
//   * setScreen() setzt gListStart auf 0 - die Blaetterposition waere sonst
//     nach jeder Karte weg.
//   * Eine Verzeichnis- oder Zufallskarte laesst resolveRandomFile() ein
//     anderes Verzeichnis einlesen. app.sdPath und die Dateiliste zeigen dann
//     woanders hin, und der Benutzer kaeme in einem fremden Ordner heraus.
void restoreAutoRfidScreen(uint32_t now) {
  const ScreenMode back = app.autoRfidFrom;
  setScreen(back, now);

  if (back == ScreenMode::SdBrowser) {
    if (!app.autoRfidSdPath.isEmpty() && app.sdPath != app.autoRfidSdPath) {
      readDirectory(app.autoRfidSdPath);
    }
    app.sdIndex = app.autoRfidSdIndex;
  }
  gListStart = app.autoRfidListStart;
}

void handleBack(uint32_t now) {
  clearPendingPowerOff();
  switch (app.screen) {
    case ScreenMode::Home:
      break;
    case ScreenMode::SdBrowser:
      if (app.sdPath != "/") {
        readDirectory(parentPath(app.sdPath));
        gListStart      = 0;
        app.screenDirty = true;
      } else {
        setScreen(ScreenMode::Home, now);
      }
      break;
    case ScreenMode::CmdPick:
    case ScreenMode::WifiMenu:
      setScreen(ScreenMode::Settings, now);
      break;
    case ScreenMode::WifiScan:
    case ScreenMode::WifiSaved:
    case ScreenMode::WifiCard:
      app.wifiPendingSsid = "";
      app.wifiSavedToCard = false;
      setScreen(ScreenMode::WifiMenu, now);
      break;
    case ScreenMode::WifiPortal:
      stopPortal(now);
      setScreen(ScreenMode::WifiMenu, now);
      break;
    case ScreenMode::RfidRun:
      // Wurde die Leseseite von der Hintergrundabfrage geoeffnet, fuehrt
      // Zurueck an denselben Ort wie das Ablaufen der Anzeigezeit.
      restoreAutoRfidScreen(now);
      break;
    default:
      setScreen(ScreenMode::Home, now);
      break;
  }
}

// ===========================================================================
//  Touch-Bedienung
//
//  Der CoreS3 hat keine Tasten A/B/C mehr, nur POWER und RESET an der Seite.
//  Bedient wird deshalb ausschliesslich ueber den kapazitiven Touchscreen:
//
//    * Kachel oder Listenzeile antippen  -> auswaehlen bzw. ausfuehren
//    * senkrecht wischen                 -> Liste folgt dem Finger 1:1
//    * Scrollleiste rechts ziehen        -> schnell durch lange Listen
//    * Zurueck-Feld unten links          -> eine Ebene zurueck
//    * Blaetterfelder unten rechts       -> eine Zeile hoch bzw. runter
//    * langes Tippen im SD-Browser       -> ein Verzeichnis zurueck
//
//  Ein Tipper zaehlt nur, wenn der Finger zwischen Aufsetzen und Abheben
//  nicht weiter als kTapSlopPx gewandert ist - sonst war es ein Schieben.
// ===========================================================================

// Bildschirmzeile (0..kListRows-1) unter dem Finger, sonst -1
int listRowFromTouch(int y) {
  if (y < kListY || y >= kListY + kListRows * kRowH) return -1;
  return (y - kListY) / kRowH;
}

// Kachel unter dem Finger, sonst -1
int tileFromTouch(int x, int y) {
  for (int i = 0; i < kTileCount; ++i) {
    int tx = 0, ty = 0;
    tileRect(i, &tx, &ty);
    if (x >= tx && x < tx + kTileW && y >= ty && y < ty + kTileH) return i;
  }
  return -1;
}

// Tipper auf eine Listenzeile: erst die Auswahl setzen, dann ausfuehren.
// Anders als beim M5Dial genuegt ein einziger Tipper - auf einem Touchscreen
// ist das die erwartete Reaktion.
void tapListRow(int row, uint32_t now) {
  const int count = listCount();
  int* target = listIndex();
  if (count <= 0 || target == nullptr) return;

  const int selected = std::max(0, std::min(*target, count - 1));
  const int start    = listWindowStart(selected, count);
  const int index    = start + row;
  if (index >= count) return;

  *target = index;
  beep(2000, 20);
  handleSelect(now);
}

void handleTap(int x, int y, bool longTap, uint32_t now) {
  // ---- Fussleiste ---------------------------------------------------------
  if (y >= kHintY) {
    if (hitBack(x, y) && app.screen != ScreenMode::Home && app.screen != ScreenMode::Busy) {
      beep(1800, 20);
      handleBack(now);
      return;
    }
    if (listScreen(app.screen)) {
      if (hitPageUp(x, y))   { beep(1800, 20); moveSelection(-1); return; }
      if (hitPageDown(x, y)) { beep(1800, 20); moveSelection(+1); return; }
    }
    // Alles andere in der Leiste faellt durch in die Auswertung der Seite.
  }

  switch (app.screen) {
    case ScreenMode::Home: {
      const int tile = tileFromTouch(x, y);
      if (tile < 0) {
        // Tipper neben die Kacheln bricht eine laufende PowerOff-Abfrage ab.
        // Nicht selbst zeichnen: der Offscreen-Puffer wird nur geschoben, wenn
        // render() in diesem Durchlauf etwas gezeichnet hat. Das Dirty-Flag
        // sorgt dafuer, dass die Kachel sofort im naechsten Bild sauber ist.
        if (app.pendingPowerOff) {
          clearPendingPowerOff();
          app.screenDirty = true;
        }
        return;
      }
      // Die POWER-Kachel wird durch einen zweiten Tipper bestaetigt. Ein
      // Tipper auf eine ANDERE Kachel bricht die Abfrage ab.
      if (app.tileIndex != tile) clearPendingPowerOff();
      app.tileIndex   = tile;
      app.screenDirty = true;      // Auswahl umranden, bevor die Aktion laeuft
      activateTile(now);
      break;
    }

    case ScreenMode::CpuMenu:
    case ScreenMode::Settings:
    case ScreenMode::CmdPick:
    case ScreenMode::WifiMenu:
    case ScreenMode::WifiScan:
    case ScreenMode::WifiSaved: {
      const int row = listRowFromTouch(y);
      if (row >= 0) tapListRow(row, now);
      break;
    }

    case ScreenMode::SdBrowser: {
      // Langes Tippen ersetzt hier das frueher lange "A" der Core-Fassung:
      // es fuehrt ein Verzeichnis nach oben.
      if (longTap) { beep(1800, 20); handleBack(now); break; }
      const int row = listRowFromTouch(y);
      if (row >= 0) tapListRow(row, now);
      break;
    }

    case ScreenMode::Status:
      // Ein Tipper auf die Flaeche loest einen Verbindungstest aus.
      beep(2000, 20);
      handleSelect(now);
      break;

    case ScreenMode::RfidInfo:
      // Antippen blaettert zwischen den beiden Infoseiten hin und her.
      if (app.infoCount > 0) { beep(1800, 20); moveSelection(+1); }
      break;

    case ScreenMode::RfidRun:
      // Laeuft eine PowerOff-Rueckfrage von der Karte, bestaetigt ein Tipper
      // sie.
      beep(2000, 20);
      handleSelect(now);
      break;

    case ScreenMode::RfidWrite:
    case ScreenMode::RfidDump:
    case ScreenMode::RfidRestore:
    case ScreenMode::WifiCard:
      beep(2000, 20);
      handleSelect(now);
      break;

    case ScreenMode::WifiPortal:
      // Kein Abbruch per Tipper: ein versehentlicher Kontakt haette sonst den
      // Accesspoint mitten in der Eingabe beendet. Beendet wird nur ueber das
      // Zurueck-Feld. Der Kontakt haelt aber die Leerlaufuhr an.
      app.portalTouchedMs = now;
      break;

    case ScreenMode::Busy:
      break;
  }
}

void handleTouch(uint32_t now) {
  static bool     down       = false;
  static int      startX     = 0;
  static int      startY     = 0;
  static int      startList  = 0;   // Fensteranfang beim Aufsetzen
  static uint32_t startMs    = 0;
  // Bildschirm beim Aufsetzen. Wechselt er, waehrend der Finger liegt (die
  // Hintergrundabfrage oeffnet die Leseseite, ein Upload endet), darf das
  // Abheben nicht als Tipper auf der neuen Seite ankommen.
  static ScreenMode startScreen = ScreenMode::Home;
  static bool     dragging   = false;   // Liste wird gerade geschoben
  static bool     scrollDrag = false;   // Griff der Scrollleiste wird gezogen

  if (app.screen == ScreenMode::Busy) { down = false; return; }

  const auto& touch = M5.Touch.getDetail();
  const bool  pressed = touch.isPressed();
  const int   x = touch.x;
  const int   y = touch.y;

  // ---- Finger aufgesetzt --------------------------------------------------
  if (pressed && !down) {
    down       = true;
    dragging   = false;
    scrollDrag = false;
    startX     = x;
    startY     = y;
    startList  = gListStart;
    startMs    = now;
    startScreen = app.screen;
    // Greift der Benutzer selbst ein, faellt die automatisch geoeffnete
    // Leseseite nicht mehr von allein zum Hauptbildschirm zurueck.
    app.autoRfidActive = false;

    // Aufsetzen auf der Scrollleiste springt sofort an die Stelle und geht
    // danach ins Ziehen ueber.
    if (listScreen(app.screen) && hitScrollbar(x, y)) {
      const int count = listCount();
      if (count > kListRows) {
        scrollDrag = true;
        scrollbarSeek(y, count);
        beep(1600, 10);
        app.screenDirty = true;
      }
    }
    return;
  }

  // ---- Finger liegt auf ---------------------------------------------------
  if (pressed && down) {
    if (scrollDrag) {
      scrollbarSeek(y, listCount());
      app.screenDirty = true;
      return;
    }
    if (!listScreen(app.screen)) return;

    // Die Liste folgt dem Finger. Sobald sie das tut, ist klar, dass kein
    // Tipper gemeint war - genau daran hat es vorher gefehlt: es wurde erst ab
    // einer festen Wischstrecke reagiert, und ein langsamer Wisch landete
    // stattdessen als Auswahl.
    //
    // Zwei Feinheiten:
    //   * Passt die Liste ganz auf den Bildschirm, gibt es nichts zu schieben.
    //     Dann darf ein Wackeln den Tipper auch nicht verschlucken.
    //   * Als Schieben gilt die Bewegung genau dann, wenn sich die Liste
    //     tatsaechlich bewegt hat. Eine feste Schwelle in Pixeln waere hier
    //     falsch: laege sie unter einer Zeilenhoehe, gaebe es dazwischen eine
    //     Totzone, in der sich weder etwas bewegt noch ein Tipper ankommt.
    //     Die Zeilenzahl wird dafuer gerundet, es geht also schon bei einer
    //     halben Zeilenhoehe (13 px) los.
    const int count = listCount();
    if (count <= kListRows) return;

    const int dy   = y - startY;
    const int rows = (dy + (dy >= 0 ? kRowH / 2 : -(kRowH / 2))) / kRowH;
    if (rows != 0) dragging = true;         // einmal geschoben bleibt geschoben

    if (dragging) {
      const int maxStart = count - kListRows;
      // Nach unten wischen zeigt weiter oben liegende Eintraege.
      int want = startList - rows;
      want = std::max(0, std::min(want, maxStart));
      if (want != gListStart) {
        gListStart = want;
        app.screenDirty = true;
      }
    }
    return;
  }

  // ---- Finger abgehoben ---------------------------------------------------
  if (!pressed && down) {
    down = false;
    if (scrollDrag) { scrollDrag = false; return; }
    if (dragging) return;                                 // war ein Schieben
    if (app.screen != startScreen) return;                // Seite hat gewechselt
    if (std::abs(x - startX) > kTapSlopPx) return;        // seitlich gerutscht
    if (std::abs(y - startY) > kTapSlopPx) return;
    handleTap(startX, startY, (now - startMs) >= kLongTapMs, now);
  }
}

// ===========================================================================
//  Hardware-Erkennung
// ===========================================================================
bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool initRfid() {
  if (!i2cDevicePresent(kRfidAddr)) return false;
  rfid.PCD_Init();
  delay(20);
  for (uint8_t i = 0; i < 6; ++i) rfidKey.keyByte[i] = 0xFF;

  // Das von PCD_Init() gesetzte Zeitfenster merken, damit die schnelle Probe
  // es hinterher exakt wiederherstellen kann.
  const uint16_t reload =
      static_cast<uint16_t>(rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegH) << 8) |
      rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegL);
  if (reload > kRfidProbeReload) gRfidTimerReload = reload;
  Serial.printf("RFID Zeitfenster = %u x 25 us\n", static_cast<unsigned>(gRfidTimerReload));
  return true;
}

}  // namespace

// ===========================================================================
//  setup()
// ===========================================================================
void setup() {
  auto cfg = M5.config();
  cfg.clear_display  = true;
  cfg.internal_spk   = true;
  cfg.internal_mic   = false;
  M5.begin(cfg);

  Serial.begin(115200);

  M5.Display.setRotation(1);              // 320 x 240
  M5.Display.setSwapBytes(true);
  M5.Display.setTextWrap(false);
  M5.Display.fillScreen(kColBg);

  // ---- Offscreen-Puffer im PSRAM ----
  // 320 x 240 x 16 bit = 150 kB. Klappt das nicht, wird direkt gezeichnet.
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  gUseCanvas = canvas.createSprite(kScrW, kScrH);
  gDraw      = gUseCanvas ? static_cast<lgfx::LovyanGFX*>(&canvas)
                          : static_cast<lgfx::LovyanGFX*>(&M5.Display);
  if (!gUseCanvas) {
    Serial.println("Kein Speicher fuer den Offscreen-Puffer - zeichne direkt.");
  }
  gDraw->setSwapBytes(true);
  gDraw->setTextWrap(false);
  gDraw->fillScreen(kColBg);

  M5.Speaker.setVolume(64);

  randomSeed(micros());   // fuer die Zufallsauswahl per "?" auf der Karte
  buildLogoMaps();
  setFallbackCpuChoices();
  loadSettings();
  applyBrightness();

  // ---- Port B: Statuslampe ----
  // begin() setzt den Pin auf Ausgang und schiebt einmal Schwarz hinaus.
  // Haengt nichts dran, passiert schlicht nichts. Sind es weniger als drei
  // LEDs, bekommen die vorhandenen einfach die ersten Farben.
  gLed.begin();
  gLed.setBrightness(app.settings.ledBrightness);
  gLed.clear();
  gLed.show();

  // ---- Port A: I2C fuer das RFID2 ----
  Wire.begin(kI2cSdaPin, kI2cSclPin, 100000UL);
  app.rfidReady = initRfid();

  // ---- microSD ----
  // Die Karte haengt am selben SPI-Bus wie das Display. Die Pins muessen
  // deshalb ausdruecklich gesetzt werden, sonst sucht SD.begin() auf der
  // Vorgabebelegung des ESP32-S3.
  SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
  app.sdReady = initSd();

  // Netzkonfiguration aus dem NVS; build_env.h liefert nur die Startwerte.
  loadNetConfig();

  // Steht noch nichts im NVS, darf /wifi.txt von der SD-Karte einspringen.
  if (gWifiCount == 0 && app.sdReady) {
    String error;
    const size_t added = loadWifiFromSd(&error);
    if (added > 0) Serial.printf("wifi.txt: %u Netz(e) uebernommen\n", static_cast<unsigned>(added));
    else           Serial.printf("wifi.txt: %s\n", error.c_str());
  }

  // ---- Startbildschirm ----
  drawStatusBar();
  drawFullScreenFor(ScreenMode::Home);
  drawStaticLogo();
  pushFrame();

  app.configReady = configReady();

  // Sind mehrere Netze gespeichert, einmal suchen und mit dem staerksten
  // bekannten Netz starten.
  wifiPickBestProfile();
  beginWiFi(millis());
  refreshConnectionStatus(millis(), true);
  resetHomeAnimation(millis());

  if (!hasWiFiConfig()) {
    setModal("SETUP > WLAN", kColWarn, millis(), 2600);
  } else if (!hasTargetConfig()) {
    setModal("c64u-ADRESSE FEHLT", kColWarn, millis(), 2600);
  } else if (!app.rfidReady) {
    setModal("RFID2 nicht gefunden", kColWarn, millis(), 1800);
  } else if (!app.sdReady) {
    setModal("Keine SD-Karte", kColWarn, millis(), 1800);
  }

  Serial.printf("C64uRemote CoreS3  RFID:%d  SD:%d  Canvas:%d  Heap:%u  PSRAM frei:%u\n",
                app.rfidReady ? 1 : 0, app.sdReady ? 1 : 0, gUseCanvas ? 1 : 0,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getFreePsram()));
}

// ===========================================================================
//  loop()
// ===========================================================================
void loop() {
  static uint32_t nextFrameMs = 0;

  M5.update();
  const uint32_t now = millis();

  servicePortal(now);
  serviceWiFi(now);
  refreshConnectionStatus(now);

  // Die Portalseite zeigt eine ablaufende Restzeit - einmal pro Sekunde neu
  // zeichnen reicht dafuer aus.
  if (app.screen == ScreenMode::WifiPortal && now - app.lastStatusDrawMs >= kStatusRefreshMs) {
    app.screenDirty = true;
  }

  // PowerOff-Bestaetigung verfaellt nach dem Zeitfenster
  if (app.pendingPowerOff && (now - app.pendingPowerOffAtMs > powerOffConfirmMs())) {
    app.pendingPowerOff = false;
    if (app.screen == ScreenMode::Home) app.screenDirty = true;
  }
  // Dasselbe fuer eine Rueckfrage, die von einer PowerOff-Karte stammt
  if (app.cardPowerOffPending &&
      static_cast<int32_t>(now - app.cardPowerOffUntilMs) >= 0) {
    app.cardPowerOffPending = false;
    app.rfidHint    = "Abfrage abgelaufen";
    app.screenDirty = true;
  }

  handleTouch(now);
  serviceRfid(now);
  serviceLed(now);

  // Eine von der Hintergrundabfrage geoeffnete Leseseite verschwindet nach
  // kAutoRfidHoldMs wieder - und zwar dorthin, wo sie herkam.
  if (app.autoRfidActive && app.screen == ScreenMode::RfidRun &&
      static_cast<int32_t>(now - app.autoRfidUntilMs) >= 0) {
    app.autoRfidActive = false;
    restoreAutoRfidScreen(now);
  }

  if (app.screen == ScreenMode::Home) updateHomeDemo(now);

  // CPU-Wert einmalig nachladen, sobald das WLAN steht
  if (WiFi.status() == WL_CONNECTED && app.currentCpuValue == "Unknown" &&
      app.screen == ScreenMode::Home) {
    refreshCpuValue();
  }

  if (nextFrameMs == 0 || static_cast<int32_t>(now - nextFrameMs) >= 0) {
    render(now);
    nextFrameMs = millis() + kFrameMs;
  }

  delay(2);
}
