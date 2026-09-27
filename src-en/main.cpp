// ============================================================================
//  C64uRemote  -  M5Stack CoreS3 / CoreS3 SE / CoreS3 Lite
//                 + Unit RFID2 (WS1850S) on Port A
// ----------------------------------------------------------------------------
//  Remote control for the Commodore 64 Ultimate (c64u) / Ultimate64 Elite-II
//  via the ReST API of the Ultimate firmware (3.11 and later).
//
//  Based on the original idea by Karl Prosser (@klumsy)
//      https://github.com/ReadyOS-C64/C64uRemote
//  and on the versions for M5StickC Plus2, M5Dial and M5Stack Core by
//      Martin Oswald (@mad) - https://1MHz.de
//
//  License: MIT - see LICENSE in the project root.
//    Copyright (c) 2026 Karl Prosser  (original project C64uRemote,
//                                      https://github.com/ReadyOS-C64/C64uRemote)
//    Copyright (c) 2026 Martin Oswald (port and extensions)
//
//  This edition brings the full feature set of the Core version to the
//  CoreS3 and moves the controls onto the capacitive touchscreen:
//
//    * Dashboard for 320x240: all commands right on the main screen
//    * PURE TOUCH OPERATION. The CoreS3 no longer has buttons A/B/C, only
//      POWER and RESET on the side. Tiles and list rows are tapped directly,
//      tapped directly, lists follow the finger while swiping, and next to
//      long lists there is a draggable scrollbar. The freely assignable
//      button shortcuts of the Core edition are gone for good - every
//      command is a tile anyway.
//    * Unit RFID2 (WS1850S, I2C 0x28) on Port A (G2 = SDA, G1 = SCL) -
//      optional
//        - WRITE the path of a program file to an NFC card
//        - READ the path from the NFC card, fetch the file from the microSD
//          microSD and send it to the c64u via ReST API + start it
//    * SD browser (start a file directly, no card required)
//    * Streaming upload: even large .d64 files go through in 1 kB blocks
//    * Wi-Fi setup on the device (Setup > WiFi): up to four networks in NVS,
//      network scan, setup portal through an access point of its own, load
//      and save /wifi.txt on the SD card, credentials on NFC cards.
//      build_env.h only supplies the initial values for the very first start.
//
//  Memory note: unlike the Core Basic, the CoreS3 has 8 MB of PSRAM. A
//  full-screen sprite (320x240x16 bit = 150 kB) is therefore created in
//  PSRAM and pushed to the display in one go - that gives flicker-free full
//  screens and smooth effects. If the allocation fails, the code falls back
//  to drawing directly.
// ============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_netif_sta_list.h>
#include <HTTPClient.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
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
// Firmware version. Keep it up to date with every release - it is shown on
// the status page and in the boot log and belongs to the CHANGELOG entry.
// ---------------------------------------------------------------------------
constexpr const char* kFwVersion = "1.4.0";
constexpr const char* kFwDate    = "2026-09-27";

// ---------------------------------------------------------------------------
// Timings and limits
// ---------------------------------------------------------------------------
constexpr uint32_t kModalMs            = 1500;
constexpr uint32_t kApiRetryDelayMs   = 250;   // pause before the second attempt
constexpr uint32_t kHttpTimeoutMs      = 3000;
// Limit for setting up the connection to the c64u, per attempt. If no
// connection comes about, up to two more attempts follow (kApiConnectAttempts).
constexpr uint32_t kHttpConnectTimeoutMs = 1500;
// In the direct network the c64u is one radio hop away - 1 s is enough.
constexpr uint32_t kHttpConnectDirectMs  = 1000;
// How often a failed connection setup is tried (in total).
constexpr uint8_t  kApiConnectAttempts   = 3;
constexpr uint32_t kWiFiRetryMs        = 10000;
constexpr uint32_t kConnectionProbeMs  = 15000;
// This is how long the device waits for the connection after a WiFi card has
// been placed on the reader before it reports "network not there". After that
// the attempt simply carries on through serviceWiFi.
constexpr uint32_t kWifiCardConnectMs  = 8000;
constexpr uint32_t kFrameMs            = 33;     // ~30 fps
constexpr uint32_t kStatusRefreshMs    = 1000;

// ---- Battery indicator --------------------------------------------------
// The line below the status bar doubles as the charge gauge: the filled
// part is two pixels tall and coloured, the rest stays the dimmed line.
// On the charger it is turquoise and never blinks.
constexpr uint32_t kBattPollMs         = 5000;   // how often the level is read
constexpr uint32_t kBattBlinkMs        = 500;    // half period of the blinking
constexpr int      kBattGreen          = 50;     // green from here up
constexpr int      kBattYellow         = 20;     // yellow from here up
constexpr uint32_t kBarSwapMs          = 15000;  // swap RFID/SD <-> battery
constexpr int      kBattBlinkAt        = 10;     // below this red and blinking
constexpr uint8_t  kPowerOffConfirmDefDs = 7;   // 0.7 s (tenths of a second)
constexpr uint32_t kRfidPollMs         = 250;

// ---------------------------------------------------------------------------
// Touch
//
// The CoreS3 no longer has buttons A/B/C. Tiles, list rows and the bar along
// list rows and the bar along the bottom edge; long lists are pushed with the
// finger or dragged through the scrollbar on the right.
// ---------------------------------------------------------------------------
// The finger may not travel further than this many pixels between touching
// down and lifting off for it to still count as a tap.
constexpr int      kTapSlopPx          = 10;
// A movement counts as a push as soon as the list has actually travelled at
// least one row - rounded, that is from half a row height on. There is
// deliberately no pixel threshold of its own: it would only create a dead zone
// in which neither pushing nor tapping happens.
// A tap that lasts longer than this counts as a long tap (for example
// leaving a folder in the SD browser).
constexpr uint32_t kLongTapMs          = 600;
// An automatically opened read page stays up this long before it
// falls back to the main screen.
constexpr uint32_t kAutoRfidHoldMs     = 20000;
constexpr size_t   kMaxCpuChoices      = 16;
constexpr size_t   kMaxJoyChoices      = 6;
constexpr size_t   kMaxDirEntries      = 160;
constexpr size_t   kUploadChunk        = 1024;   // Bytes per TCP write
constexpr uint32_t kUploadSendMs      = 10000;  // send limit from the last progress
constexpr uint32_t kUploadReplyMs     = 15000;  // limit for the reply after the upload

// Mount mode for uploaded disk images. "readwrite" is the combination
// documented in the API docs for uploads.
// Alternatives per the API: "unlinked" (writable, but nothing is written
// back) and "readonly" (write protected).
constexpr const char* kMountMode = "readwrite";

// ---------------------------------------------------------------------------
// WiFi setup
//
// The credentials live in NVS and can be changed at runtime - build_env.h only
// supplies the initial values for the very first start.
// Three routes lead to new credentials:
//   1. NFC card with an NDEF text record
//   2. File /wifi.txt on the SD card
//   3. Setup portal: the CoreS3 opens an access point of its own
// The other way round, "Save to SD" writes the stored networks back out as
// /wifi.txt - so they move to the next device without any typing.
// ---------------------------------------------------------------------------
constexpr size_t   kWifiProfileMax  = 4;        // stored networks
constexpr size_t   kWifiScanMax     = 16;       // scan hits shown
constexpr uint32_t kPortalIdleMs    = 300000;   // close the portal after 5 min
constexpr uint32_t kPortalCloseMs   = 2500;     // grace period after saving

// The access point of the setup portal. WPA2 demands at least eight characters;
// the password is shown large on the display while the portal runs.
constexpr const char* kPortalSsid    = "C64uRemote-Setup";
constexpr const char* kPortalPass    = "c64ultimate";
constexpr uint8_t     kPortalDnsPort = 53;

// Configuration file on the SD card
constexpr const char* kWifiFileSd    = "/wifi.txt";

// ---------------------------------------------------------------------------
// Direct mode
//
// For meetings without a router: the CoreS3 opens its own WiFi and the
// c64u joins it. The c64u can only remember one network - so the SSID and
// password of the direct network are entered there once.
//
//   CoreS3  <net>.1     access point, hands out addresses via DHCP
//   c64u    <net>.64    first address the DHCP server hands out
//
// If another device (e.g. a phone) connects first, it gets
// .64 and the c64u the next one. The status test then tries all connected
// devices in turn until the c64u answers.
//
// A second device (M5Dial, M5Core ...) can join the direct network as a
// normal WiFi client. When it recognises the direct network's SSID, it also
// addresses the c64u at <net>.64 - the home address is kept.
// ---------------------------------------------------------------------------
constexpr const char* kDirectSsidDef    = "C64uRemote-Direct";
constexpr const char* kDirectPassDef    = "c64ultimate";
constexpr const char* kDirectNetDef     = "192.168.4";
constexpr const char* kDirectNetAlt     = "192.168.2";   // second choice in the menu
constexpr uint8_t     kDirectApHost     = 1;
constexpr uint8_t     kDirectC64Host    = 64;
constexpr uint8_t     kDirectChannel    = 6;
constexpr uint8_t     kDirectMaxClients = 8;
constexpr size_t      kDirectCandMax    = 11;    // .64 + up to ten devices
constexpr const char* kWifiFileSdBak = "/wifi.bak";

// ---------------------------------------------------------------------------
// CoreS3 hardware
//
// Port A (Grove, red) is the external I2C bus and carries the Unit RFID2.
// Careful: the assignment is swapped compared to the Core Basic - on the
// CoreS3, SDA sits on G2 and SCL on G1.
//
// The microSD hangs on the same SPI bus as the display (MOSI G37, SCK G36,
// MISO G35); only the chip select line is its own (G4). SPI.begin() is
// therefore called with these pins explicitly, otherwise the bus ends up on
// the ESP32-S3 default assignment and the card is not found.
// ---------------------------------------------------------------------------
constexpr int     kI2cSdaPin   = 2;      // Port A, yellow wire
constexpr int     kI2cSclPin   = 1;      // Port A, white wire
constexpr uint8_t kRfidAddr    = 0x28;
constexpr int     kSdSckPin    = 36;
constexpr int     kSdMisoPin   = 35;
constexpr int     kSdMosiPin   = 37;
constexpr int     kSdCsPin     = 4;

// ---------------------------------------------------------------------------
// Status lamp on Port B
//
// Three SK6812 (WS2812 compatible) chained on the yellow wire of Port B - the
// usual M5Stack Unit RGB. That is a pure output line - whether LEDs are
// actually attached CANNOT be detected.
// If nothing is plugged in, the data goes nowhere; that does no harm. Anyone
// who needs the pin for something else switches the lamp off in the setup
// under "Status-LED".
// ---------------------------------------------------------------------------
constexpr int      kLedPin     = 9;        // Port B, yellow wire
constexpr uint16_t kLedCount   = 3;
// Interval between two updates. 20 ms is enough for a calm pulse and keeps the
// bus load small - three LEDs are pushed out in under 100 us.
constexpr uint32_t kLedFrameMs = 20;
// Afterglow of an event flash (card read, error)
constexpr uint32_t kLedFlashMs = 600;

// ---------------------------------------------------------------------------
// Display geometry (320 x 240, rotation 1)
// ---------------------------------------------------------------------------
// All measurements are made for a finger, not for buttons: larger tiles,
// taller list rows and a distinctly taller bottom bar, because its fields
// (back, paging) are hit areas in their own right.
constexpr int kScrW      = 320;
constexpr int kScrH      = 240;
constexpr int kBarH      = 20;                  // Status bar
constexpr int kLogoY     = kBarH;               // Logo / effect area
constexpr int kLogoH     = 108;
constexpr int kTileY0    = 132;
constexpr int kTileY1    = 168;
constexpr int kTileH     = 33;
constexpr int kTileW     = 58;
constexpr int kTileGap   = 6;
constexpr int kHintY     = 204;                 // bottom bar is 36 px tall

// Derived value for height-correct logo scaling
constexpr int kLogoDrawW = (kLogoH * kLogoSrcW) / kLogoSrcH;
constexpr int kLogoOffX  = (kScrW - kLogoDrawW) / 2;

// ---------------------------------------------------------------------------
// Command tiles on the main screen
// ---------------------------------------------------------------------------
enum TileId : uint8_t {
  kTileReset = 0,
  kTileReboot,
  kTileUltiMenu,
  kTilePowerOff,
  kTileCpu,
  kTileJoySwap,
  kTileRfidRun,
  kTileSdBrowse,
  kTileStatus,
  kTileSetup,
  kTileCount
};

constexpr const char* kTileLabels[kTileCount] = {
    "RESET", "REBOOT", "MENU", "POWER", "CPU",
    "JOY", "RFID", "SD", "STATUS", "SETUP",
};

// The order of the settings is referenced in several places (text, value,
// action). So that nothing slips when inserting an entry, they have names.
enum SettingsId : uint8_t {
  kSetNfcWrite = 0,
  kSetNfcInfo,
  kSetNfcDump,
  kSetNfcRestore,
  kSetNfcRandom,       // random card for a directory
  kSetNfcCmd,          // last NFC action; everything after this is a switch
  kSetCardConfirm,     // prompt time of a PowerOff command card (NFC-Cmd)
  kSetWifi,            // action, handled up front in activateSetting()
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
  kSetJoystick,        // port mapping in the c64u (config "Joystick Swapper")
  kSetBeep,
  kSetFactoryReset,
  kSetItemCount
};

// Up to here (inclusive) these are actions, not switches.
constexpr uint8_t kSetLastAction = kSetNfcCmd;

// NFC-Write deliberately comes first: after "Setup" the entry is already at
// the top of the list and one single tap away.
constexpr const char* kSettingsItems[] = {
    "NFC-Write",
    "NFC-Info",
    "NFC-Dump",
    "NFC-Restore",
    "NFC-Random",
    "NFC-Cmd",
    "NFC-Cmd PowOff",
    "WiFi",
    "Auto-NFC",
    "PowerOff time",
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
    "Joystick",
    "Beep",
    "Factory Reset",
};
constexpr size_t kSettingsCount = sizeof(kSettingsItems) / sizeof(kSettingsItems[0]);
static_assert(kSettingsCount == static_cast<size_t>(kSetItemCount),
              "kSettingsItems and SettingsId have drifted apart");

// Submenu of the WiFi setup
enum WifiMenuId : uint8_t {
  kWifiDirect = 0,     // direct mode on/off (own access point)
  kWifiDirectNet,      // address range of the direct network
  kWifiScanNow,
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
    "Direct mode",
    "Direct net",
    "Scan networks",
    "Load from SD",
    "Setup portal",
    "Saved",
    "To NFC card",
    "Save to SD",
    "Delete network",
    "Delete all",
};

enum class ScreenMode : uint8_t {
  Home,
  CpuMenu,
  Status,
  Settings,
  SdBrowser,     // Select a file (start it or write it to a card)
  CmdPick,       // Select a command that gets written to a card
  RfidRun,       // Place card -> read path -> start
  RfidWrite,     // Place card -> write path
  RfidInfo,      // Place card -> show all info
  RfidDump,      // Place card -> back up contents to the SD
  RfidRestore,   // Place card -> restore dump from the SD
  WifiMenu,      // submenu of the WiFi setup
  WifiScan,      // list of the networks found
  WifiCard,      // place a card -> read the WiFi password
  WifiPortal,    // setup access point is running
  WifiSaved,     // stored networks: connect or delete
  WifiDirect,    // direct mode: show credentials for the c64u
  Busy,          // Upload in progress, dedicated progress screen
};

enum class HomeMode : uint8_t { Static, Water, RotoZoom, SineWave, Ripple, Raster };
enum class DisplayEffectMode : uint8_t { Auto, Static, Water, RotoZoom, SineWave, Ripple, Raster };
enum class AnimationSpeedMode : uint8_t { Slow, Normal, Fast };
enum class EffectDurationMode : uint8_t { Short, Normal, Long };
enum class StaticDurationMode : uint8_t { Short, Normal, Long };
enum class FxDetailMode : uint8_t { Half, Full };
enum class DiskActionMode : uint8_t { Mount, MountReset, MountRun };
enum class UploadDriveMode : uint8_t { AutoBus8, DriveA, DriveB };
// Background polling of the RFID2 on the main screen
enum class AutoNfcMode : uint8_t { Off, Slow, Normal, Fast };

// Commands that an NFC card may carry instead of a file path.
// The card then contains e.g. "CMD:RESET" or "CMD:CPU=10".
enum class CardCmd : uint8_t {
  None,
  Reset,
  Reboot,
  UltiMenu,
  PowerOff,        // Argument = confirmation time in seconds, 0 = immediately
  DeviceOff,       // switch the CoreS3 itself off ("CMD:M5OFF")
  CpuSpeed,        // Argument = desired value, e.g. "10"
  JoySwap,         // Argument = target value; without one it toggles
  Direct,          // argument = network (e.g. "192.168.4") or OFF; none = toggle
};

struct CardCommand {
  CardCmd cmd    = CardCmd::None;
  String  arg;                 // PowerOff: seconds, CpuSpeed: MHz, JoySwap: target
  bool    hasArg = false;
};

// ---------------------------------------------------------------------------
// Data structures
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
  uint8_t            powerOffConfirmDs = kPowerOffConfirmDefDs;  // Prompt time window
  // How long to wait for the confirmation after a PowerOff card command
  // (seconds). Within that time the card must be placed again
  // or the screen tapped.
  uint8_t            cardConfirmS      = 8;
  // Status lamp on Port B (three SK6812). If nothing is attached, driving it
  // does no harm - anyone who needs the pin otherwise switches it off here.
  bool               ledEnabled        = true;
  uint8_t            ledBrightness     = 60;   // 0..255, dimmed is plenty
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

// A stored WiFi network. The password stays in NVS and only leaves the
// device through the setup portal (masked there).
struct WifiProfile {
  String ssid;
  String pass;
};

// A hit from the network scan
struct WifiScanEntry {
  String  ssid;
  int32_t rssi = 0;
  bool    open = false;
};

struct AppState {
  ScreenMode screen        = ScreenMode::Home;
  ScreenMode returnScreen  = ScreenMode::Home;   // where to go after Busy/RFID
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

  String joyCategory;
  String joyItem;
  String joyValue;
  bool   joyPathKnown   = false;
  String joyOptions[kMaxJoyChoices];
  size_t joyChoiceCount = 0;

  bool            configReady = false;
  ConnectionState connection  = {};
  SettingsState   settings    = {};
  HomeDemoState   home        = {};

  // ---- WiFi setup ----
  int      wifiMenuIndex   = 0;
  int      wifiScanIndex   = 0;
  int      wifiSavedIndex  = 0;
  size_t   wifiScanCount   = 0;
  bool     wifiSavedDelete = false;   // list is in delete mode
  bool     wifiSavedToCard = false;   // list writes the network to a card
  String   wifiPendingSsid;           // chosen network, waiting for a password
  String   wifiHint = "place a card...";
  bool     portalActive    = false;
  uint32_t portalTouchedMs = 0;       // last access (idle shutdown)
  uint32_t portalCloseAtMs = 0;       // 0 = no shutdown requested

  // ---- Overlay ----
  String   modalText;
  uint16_t modalColor   = TFT_WHITE;
  uint32_t modalUntilMs = 0;
  bool     modalVisibleLast = false;

  // ---- Timing ----
  uint32_t lastWiFiAttemptMs     = 0;
  uint32_t lastConnectionProbeMs = 0;
  uint32_t lastStatusDrawMs      = 0;
  int      batteryLevel          = -1;     // 0-100, negative = no battery
  bool     batteryCharging       = false;
  int      vbusMilliVolt         = -1;     // -1 = device does not report it
  bool     batteryPolled         = false;
  uint32_t lastBatteryPollMs     = 0;
  uint32_t lastRfidPollMs        = 0;

  bool     pendingPowerOff   = false;      // Tile: a second B confirms
  uint32_t pendingPowerOffAtMs = 0;

  // ---- Redraw flags ----
  bool barDirty    = true;
  bool screenDirty = true;

  // ---- SD ----
  bool         sdReady = false;
  String       sdPath  = "/";
  DirEntryInfo sdEntries[kMaxDirEntries];
  size_t       sdCount = 0;
  int          sdIndex = 0;
  uint8_t      sdPickMode = 0;          // 0 start, 1 write to card, 2 restore dump

  // ---- RFID ----
  bool   rfidReady   = false;
  String pendingPath;                   // Path that is to go onto the card
  String pendingCardText;               // ready-made card text (command cards)
  int    cmdIndex = 0;                  // Selection in the command menu

  // Pending prompt of a PowerOff card command
  bool     cardPowerOffPending = false;
  String   cardPowerOffUid;
  uint32_t cardPowerOffUntilMs = 0;
  String pendingDump;                   // Dump file that is being restored
  String lastCardPath;                  // path most recently read from a card
  String rfidHint = "place a card...";
  // true if the read page was opened by the background polling.
  // It then returns to the main screen by itself after kAutoRfidHoldMs.
  bool     autoRfidActive  = false;
  // Screen from which the background poll opened the read page. That is where
  // it returns to afterwards - whoever is browsing the SD card does not want
  // to end up on the home screen.
  ScreenMode autoRfidFrom = ScreenMode::Home;
  // State of the SD browser when it was left. A directory or random card makes
  // resolveRandomFile() read a different directory and rebuilds
  // app.sdPath/sdEntries in the process - without this backup the user would
  // come back out in a foreign directory.
  String     autoRfidSdPath;
  int        autoRfidSdIndex   = 0;
  int        autoRfidListStart = 0;
  uint32_t autoRfidUntilMs = 0;

  static constexpr size_t kMaxInfoLines = 16;
  String infoLines[kMaxInfoLines];     // Page 1: summary
  size_t infoCount = 0;
  String infoLines2[kMaxInfoLines];    // Page 2: raw data and technical details
  size_t infoCount2 = 0;
  uint8_t infoPage = 0;
  String infoUid;                      // UID of the card that has already been read

  // ---- Upload ----
  String   busyTitle;
  String   busyDetail;
  size_t   busySent  = 0;
  size_t   busyTotal = 0;

  // ---- Status lamp ----
  uint32_t ledNextMs    = 0;      // next update
  uint32_t ledFlashMs   = 0;      // event flash runs until here
  uint32_t ledFlashRgb  = 0;      // colour of the flash
  uint32_t ledLastRgb[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};  // set last time
  uint8_t  ledLastBright = 0xFF;
} app;

Preferences  prefs;
MFRC522_I2C  rfid(kRfidAddr, -1);
MFRC522_I2C::MIFARE_Key rfidKey;

// Status lamp on Port B. NEO_GRB is the order of the SK6812 in the M5Stack
// Unit RGB; WS2812 behave identically.
Adafruit_NeoPixel gLed(kLedCount, kLedPin, NEO_GRB + NEO_KHZ800);

// Offscreen buffer in PSRAM. If the allocation fails, drawing goes straight
// to the display - gDraw then points at M5.Display and pushFrame() does
// nothing.
M5Canvas         canvas(&M5.Display);
lgfx::LovyanGFX* gDraw      = nullptr;
bool             gUseCanvas = false;

uint16_t rowBuf[kScrW] = {};
int16_t  logoXMap[kScrW] = {};
int16_t  logoYMap[kLogoH] = {};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
String trimCopy(const String& value) { String r = value; r.trim(); return r; }
String configString(const char* v)   { return trimCopy(v == nullptr ? "" : v); }

// ---------------------------------------------------------------------------
// Network configuration at runtime
//
// SSID, password and target address used to come straight from build_env.h.
// Now they live in NVS and build_env.h only supplies the initial values as
// long as nothing has been stored. A new WiFi network therefore no longer
// requires a rebuild.
// ---------------------------------------------------------------------------
WifiProfile gWifiProfiles[kWifiProfileMax];
size_t      gWifiCount = 0;
size_t      gWifiTry   = 0;      // profile for the next connection attempt
String      gTargetHost;
String      gTargetPass;

WifiScanEntry gWifiScan[kWifiScanMax];

// Values from build_env.h (initial values only)
const String& buildWifiSsid()  { static const String v = configString(C64U_WIFI_SSID); return v; }
const String& buildWifiPass()  { static const String v = configString(C64U_WIFI_PASSWORD); return v; }
const String& buildHost()      { static const String v = configString(C64U_TARGET_HOST); return v; }
const String& buildHostPass()  { static const String v = configString(C64U_TARGET_PASSWORD); return v; }

// Direct mode (see kDirectSsidDef). Stored in the same NVS namespace
// as the WiFi profiles.
bool   gDirectMode = false;
String gDirectSsid = kDirectSsidDef;
String gDirectPass = kDirectPassDef;
String gDirectNet  = kDirectNetDef;      // the first three parts, e.g. "192.168.4"
String gDirectHost;                      // address currently used in the direct network
bool   gDirectApUp = false;              // direct mode access point is running

String directIp(uint8_t host) { return gDirectNet + "." + String(static_cast<unsigned>(host)); }

// Three numbers 0..255 separated by dots - nothing more is checked.
bool directNetValid(const String& net) {
  int parts = 0;
  int value = -1;
  for (size_t i = 0; i <= net.length(); ++i) {
    const char c = (i < net.length()) ? net[i] : '.';
    if (c == '.') {
      if (value < 0 || value > 255) return false;
      ++parts;
      value = -1;
    } else if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
      if (value > 255) return false;
    } else {
      return false;
    }
  }
  return parts == 3;
}

// Network name and signal strength of the WiFi connection, queried from the
// driver at most once per second. WiFi.SSID() and WiFi.RSSI() ask the WiFi
// driver on EVERY call (esp_wifi_sta_get_ap_info). targetHost() needs the
// network name and is called all the time - thousands of driver requests per
// second that put needless load on the radio.
constexpr uint32_t kStaInfoMs = 1000;
String   gStaSsidCache;
int32_t  gStaRssiCache = 0;
uint32_t gStaInfoMs    = 0;
bool     gStaInfoValid = false;

void refreshStaInfo() {
  const uint32_t now = millis();
  if (gStaInfoValid && now - gStaInfoMs < kStaInfoMs) return;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    gStaSsidCache = String(reinterpret_cast<const char*>(ap.ssid));
    gStaRssiCache = ap.rssi;
  } else {
    gStaSsidCache = "";
    gStaRssiCache = 0;
  }
  gStaInfoMs    = now;
  gStaInfoValid = true;
}

const String& staSsid() {
  static const String kNone;
  if (WiFi.status() != WL_CONNECTED) { gStaInfoValid = false; return kNone; }
  refreshStaInfo();
  return gStaSsidCache;
}

int32_t staRssi() {
  if (WiFi.status() != WL_CONNECTED) { gStaInfoValid = false; return 0; }
  refreshStaInfo();
  return gStaRssiCache;
}

// Connected as a normal WiFi client to another device's direct network?
bool onDirectNetAsClient() {
  return !gDirectMode && !gDirectSsid.isEmpty() &&
         WiFi.status() == WL_CONNECTED && staSsid() == gDirectSsid;
}

// Network usable: in direct mode our own access point, otherwise the
// connection to the stored WiFi.
bool netReady() { return gDirectMode ? gDirectApUp : (WiFi.status() == WL_CONNECTED); }

const String& targetHost() {
  if (gDirectMode || onDirectNetAsClient()) return gDirectHost;
  return gTargetHost;
}
const String& targetPassword() { return gTargetPass; }

bool hasWiFiConfig()   { return gWifiCount > 0; }
bool hasTargetConfig() { return gDirectMode || !gTargetHost.isEmpty(); }
bool configReady()     { return (gDirectMode || hasWiFiConfig()) && hasTargetConfig(); }

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

// Colour palette
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

// Forward declarations
void drawBusyScreen();
void pushFrame();
void openSdBrowser(uint8_t forCard, uint32_t now);
void drawTiles();
void setScreen(ScreenMode next, uint32_t now);
void refreshCpuValue();
void beginWiFi(uint32_t now);
ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated);

// ---------------------------------------------------------------------------
// Labels for the settings
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
// Selectable time windows in tenths of a second: 0.5 to 3.0 s
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

// Confirmation window for PowerOff card commands. Much larger than for the
// prompt on screen, because the card first has to be taken away and put back
// on again.
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
// Interval between two background polls of the reader
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

// The lamp is dazzling even at low values, so the steps are finer at the
// bottom end than for the display.
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
// Load / save settings (NVS)
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

  // Clamp the time window to the selectable range (0.5 to 3.0 s)
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
// Logo sampling straight from flash (no RAM cache!)
// ---------------------------------------------------------------------------
void buildLogoMaps() {
  for (int x = 0; x < kScrW; ++x) {
    const int rel = x - kLogoOffX;
    if (rel < 0 || rel >= kLogoDrawW) {
      logoXMap[x] = -1;                       // outside -> background
    } else {
      logoXMap[x] = static_cast<int16_t>(std::min(kLogoSrcW - 1, (rel * kLogoSrcW) / kLogoDrawW));
    }
  }
  for (int y = 0; y < kLogoH; ++y) {
    logoYMap[y] = static_cast<int16_t>(std::min(kLogoSrcH - 1, (y * kLogoSrcH) / kLogoH));
  }
}

// Background left and right of the height-correctly scaled logo.
// The logo itself has a black background, hence black here as well.
constexpr uint16_t kLogoBg = TFT_BLACK;

// x/y are coordinates INSIDE the logo area (0..319 / 0..kLogoH-1)
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

// Pushes a finished image line to the display. With "Half" detail the
// computed pixels are doubled horizontally and the line is emitted twice.
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

// Water / sine distortion (line-by-line displacement)
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

// Raster bars around a box holding the logo
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
// Effect sequencing
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
//  ReST API of the c64u
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

// ---------------------------------------------------------------------------
// Own, lean HTTP path to the c64u
// ---------------------------------------------------------------------------
// HTTPClient and WiFiClient wait blocking while connecting and reading. On the
// home network most requests never arrived that way ("connection refused" or
// "read Timeout"), although the c64u answers every computer at once - while
// the pre-check, which keeps looking at the connection without waiting, got
// its reply EVERY time in about 50 ms. So all requests now work that way:
// connect and read the reply without waiting inside the network stack, and
// close the connection only once the c64u has ended it itself.
constexpr size_t kRawMaxBody = 16384;

// Sets up the connection. 0 = up, otherwise HTTPC_ERROR_CONNECTION_REFUSED.
int rawOpen(uint32_t connectMs, int* fdOut) {
  IPAddress ip;
  if (!ip.fromString(targetHost()) && !WiFi.hostByName(targetHost().c_str(), ip)) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return HTTPC_ERROR_CONNECTION_REFUSED;
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(80);
  addr.sin_addr.s_addr = static_cast<uint32_t>(ip);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 &&
      errno != EINPROGRESS) {
    ::close(fd);
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  const uint32_t startMs = millis();
  for (;;) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    struct timeval noWait = {0, 0};
    const int ready = ::select(fd + 1, nullptr, &writable, nullptr, &noWait);
    if (ready > 0) {
      int       sockErr = -1;
      socklen_t len     = sizeof(sockErr);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockErr, &len);
      if (sockErr != 0) { ::close(fd); return HTTPC_ERROR_CONNECTION_REFUSED; }
      break;
    }
    if (ready < 0 || millis() - startMs >= connectMs) {
      ::close(fd);
      return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    delay(2);
  }
  *fdOut = fd;
  return 0;
}

// Sends everything. timeoutMs counts from the last progress.
bool rawSendAll(int fd, const uint8_t* data, size_t len, uint32_t timeoutMs) {
  size_t   done   = 0;
  uint32_t lastMs = millis();
  while (done < len) {
    const int n = ::send(fd, data + done, len - done, MSG_DONTWAIT);
    if (n > 0) {
      done  += static_cast<size_t>(n);
      lastMs = millis();
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && millis() - lastMs < timeoutMs) {
      delay(2);
      continue;
    }
    return false;
  }
  return true;
}

// Reads the reply until the c64u closes the connection (or shortly after the
// last announced byte). Returns the HTTP status or an HTTPC_ERROR_...; body
// receives the payload (at most maxBody bytes).
int rawReadResponse(int fd, uint32_t timeoutMs, String* body, size_t maxBody) {
  String   raw;
  char     buf[512];
  uint32_t lastMs        = millis();
  int      headerEnd     = -1;
  long     contentLength = -1;
  bool     complete      = false;
  bool     closed        = false;
  raw.reserve(512);
  for (;;) {
    const int n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
      if (raw.length() < maxBody + 1024) raw.concat(buf, static_cast<unsigned>(n));
      lastMs = millis();
      if (headerEnd < 0) {
        headerEnd = raw.indexOf("\r\n\r\n");
        if (headerEnd >= 0) {
          String head = raw.substring(0, headerEnd);
          head.toLowerCase();
          const int at = head.indexOf("content-length:");
          if (at >= 0) contentLength = head.substring(at + 15).toInt();
        }
      }
      if (headerEnd >= 0 && contentLength >= 0 &&
          static_cast<long>(raw.length()) >= headerEnd + 4 + contentLength) {
        complete = true;
      }
      continue;
    }
    if (n == 0) { closed = true; break; }   // c64u hat geschlossen / c64u closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const uint32_t limit = complete ? 200 : timeoutMs;
      if (millis() - lastMs >= limit) break;
      delay(2);
      continue;
    }
    closed = true;   // Fehler, meist Reset / error, usually a reset
    break;
  }
  if (raw.isEmpty()) return closed ? HTTPC_ERROR_CONNECTION_LOST : HTTPC_ERROR_READ_TIMEOUT;
  if (headerEnd < 0) return HTTPC_ERROR_NO_HTTP_SERVER;
  if (!raw.startsWith("HTTP/")) return HTTPC_ERROR_NO_HTTP_SERVER;
  const int space = raw.indexOf(' ');
  const int code  = space > 0 ? raw.substring(space + 1, space + 4).toInt() : 0;
  if (code <= 0) return HTTPC_ERROR_NO_HTTP_SERVER;

  if (body != nullptr) {
    String head = raw.substring(0, headerEnd);
    head.toLowerCase();
    String payload = raw.substring(headerEnd + 4);
    if (head.indexOf("transfer-encoding: chunked") >= 0) {
      String plain;
      int    pos = 0;
      for (;;) {
        const int eol = payload.indexOf("\r\n", pos);
        if (eol < 0) break;
        const long size = strtol(payload.substring(pos, eol).c_str(), nullptr, 16);
        if (size <= 0) break;
        plain += payload.substring(eol + 2, eol + 2 + size);
        pos = eol + 2 + size + 2;
      }
      payload = plain;
    }
    if (payload.length() > maxBody) payload.remove(maxBody);
    *body = payload;
  }
  return code;
}

// One complete request: connect, send, read the reply, close.
int rawHttpRequest(const char* method, const String& path, bool authenticated, String* body,
                   size_t maxBody) {
  int fd = -1;
  const int opened = rawOpen(gDirectMode ? kHttpConnectDirectMs : kHttpConnectTimeoutMs, &fd);
  if (opened != 0) return opened;

  String req;
  req.reserve(160);
  req += method;
  req += " " + path + " HTTP/1.1\r\nHost: " + targetHost() + "\r\n";
  if (authenticated && !targetPassword().isEmpty()) req += "X-Password: " + targetPassword() + "\r\n";
  if (strcmp(method, "GET") != 0) req += "Content-Length: 0\r\n";
  req += "Connection: close\r\n\r\n";
  if (!rawSendAll(fd, reinterpret_cast<const uint8_t*>(req.c_str()), req.length(), kHttpTimeoutMs)) {
    ::close(fd);
    return HTTPC_ERROR_SEND_HEADER_FAILED;
  }
  const int code = rawReadResponse(fd, kHttpTimeoutMs, body, maxBody);
  ::close(fd);
  return code;
}

ApiResponse sendApiRequestOnce(const char* method, const String& path, bool authenticated) {
  ApiResponse result;
  if (!hasTargetConfig()) { result.errors = "Target host missing"; return result; }
  if (strcmp(method, "GET") != 0 && strcmp(method, "PUT") != 0) {
    result.errors = "Unsupported method";
    return result;
  }

  String body;
  result.httpCode    = rawHttpRequest(method, path, authenticated, &body, kRawMaxBody);
  result.transportOk = result.httpCode > 0;
  if (result.transportOk) {
    result.body = body;
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, result.body) == DeserializationError::Ok) {
      result.jsonOk = true;
      result.errors = extractErrors(doc);
      result.apiOk  = result.httpCode >= 200 && result.httpCode < 300 && result.errors.isEmpty();
    } else {
      result.apiOk = result.httpCode >= 200 && result.httpCode < 300;
    }
  } else {
    result.errors = HTTPClient::errorToString(result.httpCode);
  }
  return result;
}

// If no connection came about, the c64u received nothing - a fresh attempt is
// harmless, even for a PUT. Other errors (e.g. no reply any more) are not
// retried, since the command may already have been executed.
ApiResponse sendApiRequest(const char* method, const String& path, bool authenticated) {
  ApiResponse result = sendApiRequestOnce(method, path, authenticated);
  for (uint8_t attempt = 1; attempt < kApiConnectAttempts && hasTargetConfig() &&
                            result.httpCode == HTTPC_ERROR_CONNECTION_REFUSED;
       ++attempt) {
    delay(kApiRetryDelayMs);
    result = sendApiRequestOnce(method, path, authenticated);
  }
  return result;
}

// ---------------------------------------------------------------------------
// CPU speed: look up the path in the configuration and read the selection
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
// WiFi and connection status
// ---------------------------------------------------------------------------
// Connects to the next stored network. If several profiles are on file, the
// attempt moves on by one with every call - that way all known networks are
// tried one after another.
void startDirectAp(uint32_t now);

void beginWiFi(uint32_t now) {
  if (gDirectMode) { startDirectAp(now); return; }
  if (!hasWiFiConfig()) return;
  if (gWifiTry >= gWifiCount) gWifiTry = 0;

  const WifiProfile& profile = gWifiProfiles[gWifiTry];

  // While the setup portal is running the access point stays up.
  if (!app.portalActive) WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(profile.ssid.c_str(), profile.pass.c_str());
  app.lastWiFiAttemptMs = now;

  if (gWifiCount > 1) gWifiTry = (gWifiTry + 1) % gWifiCount;
}

int wifiProfileIndex(const String& ssid);

// Remembers which network the connection came up on. After a dropout that one
// is tried first instead of blindly taking the next. With two stored networks
// of which only one is reachable, this otherwise costs a full retry cycle
// every other time.
bool gWifiNoted = false;

// ---------------------------------------------------------------------------
// Direct mode: own access point
// ---------------------------------------------------------------------------
// Like the setup portal, without a station part: an active STA would search
// for the home network in the background, change channel and keep
// throwing the c64u off the network.
void startDirectAp(uint32_t now) {
  app.lastWiFiAttemptMs = now;
  if (app.portalActive) return;          // portal has priority, continues here afterwards

  IPAddress apIp;
  IPAddress leaseStart;
  if (!directNetValid(gDirectNet)) gDirectNet = kDirectNetDef;
  apIp.fromString(directIp(kDirectApHost));
  leaseStart.fromString(directIp(kDirectC64Host));

  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(60);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  // Start the AP first, then set the addresses. The other way round, depending
  // on the core version, the default 192.168.4.1 stays - the setup portal never
  // notices because it uses exactly this address.
  gDirectApUp = WiFi.softAP(gDirectSsid.c_str(), gDirectPass.c_str(), kDirectChannel,
                            0 /*sichtbar*/, kDirectMaxClients);
  delay(100);
  // The DHCP server hands out from <net>.64 - first come gets .64.
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0), leaseStart);
  delay(60);

  gDirectHost = directIp(kDirectC64Host);
  Serial.printf("Direct mode %s: SSID %s, IP %s, c64u %s\n",
                gDirectApUp ? "active" : "FAILED", gDirectSsid.c_str(),
                WiFi.softAPIP().toString().c_str(), gDirectHost.c_str());
}

void stopDirectAp() {
  if (!gDirectApUp) return;
  WiFi.softAPdisconnect(true);
  gDirectApUp = false;
}

// Addresses of all devices currently connected to the access point.
size_t directStationIps(uint32_t* out, size_t maxCount) {
  wifi_sta_list_t      wifiList;
  esp_netif_sta_list_t ipList;
  memset(&wifiList, 0, sizeof(wifiList));
  memset(&ipList, 0, sizeof(ipList));
  if (esp_wifi_ap_get_sta_list(&wifiList) != ESP_OK) return 0;
  if (esp_netif_get_sta_list(&wifiList, &ipList) != ESP_OK) return 0;
  size_t count = 0;
  for (int i = 0; i < ipList.num && count < maxCount; ++i) {
    if (ipList.sta[i].ip.addr != 0) out[count++] = ipList.sta[i].ip.addr;
  }
  return count;
}

// The c64u does not answer at the address just tried: take the next
// candidate. Order: first <net>.64, then all
// connected devices. After the last one it starts over.
//
// If the c64u has already answered at the current address, the switch only
// happens after the second failure in a row - it occasionally refuses on its
// own (see sendApiRequest), which should not change the address right away.
String  gDirectConfirmed;
uint8_t gDirectMisses = 0;

void directConfirmHost() {
  gDirectConfirmed = gDirectHost;
  gDirectMisses    = 0;
}

void directNextCandidate() {
  if (!gDirectMode) return;
  if (gDirectHost == gDirectConfirmed && ++gDirectMisses < 2) return;
  gDirectMisses = 0;

  String   cand[kDirectCandMax];
  size_t   count = 0;
  cand[count++] = directIp(kDirectC64Host);

  uint32_t ips[kDirectCandMax - 1];
  const size_t found = directStationIps(ips, kDirectCandMax - 1);
  for (size_t i = 0; i < found && count < kDirectCandMax; ++i) {
    const String ip = IPAddress(ips[i]).toString();
    if (ip != cand[0]) cand[count++] = ip;
  }

  size_t current = 0;
  for (size_t i = 0; i < count; ++i) {
    if (cand[i] == gDirectHost) { current = i; break; }
  }
  gDirectHost = cand[(current + 1) % count];
}

// Like staSsid(): ask the driver for the number of stations at most every
// 500 ms - serviceWiFi() needs it on every loop pass.
size_t directStationCount() {
  static size_t   cached = 0;
  static uint32_t atMs   = 0;
  static bool     valid  = false;
  if (!gDirectApUp) { valid = false; return 0; }
  const uint32_t now = millis();
  if (!valid || now - atMs >= 500) {
    cached = WiFi.softAPgetStationNum();
    atMs   = now;
    valid  = true;
  }
  return cached;
}

// After a device joins, the c64u needs a few seconds for DHCP and its HTTP
// server - the first test then often comes too early. Instead of asking again
// only after kConnectionProbeMs, check a few times in quick succession, on
// any page, until it answers.
constexpr uint32_t kDirectRecheckMs    = 3000;
constexpr uint8_t  kDirectRecheckCount = 6;
uint8_t  gDirectRechecks  = 0;
uint32_t gDirectRecheckAt = 0;

void directScheduleRechecks(uint32_t now) {
  gDirectRechecks  = kDirectRecheckCount;
  gDirectRecheckAt = now + kDirectRecheckMs;
}

bool directRecheckDue(uint32_t now) {
  if (!gDirectMode || app.connection.authOk) { gDirectRechecks = 0; return false; }
  if (gDirectRechecks == 0 || static_cast<int32_t>(now - gDirectRecheckAt) < 0) return false;
  --gDirectRechecks;
  gDirectRecheckAt = now + kDirectRecheckMs;
  return true;
}

// When the last device has left, restart the DHCP server. It hands out
// addresses via a pointer that only moves forward: if the c64u releases its
// address when leaving or asks for the old one when coming back, the server
// drops the entry and takes the next one (.65, .66 ...). A restart empties the
// table and puts the pointer back to <net>.64.
// Only with no devices connected - otherwise an address could be handed out
// twice, because the server forgets the existing assignments.
void directRestartDhcp() {
  IPAddress apIp;
  IPAddress leaseStart;
  apIp.fromString(directIp(kDirectApHost));
  leaseStart.fromString(directIp(kDirectC64Host));
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0), leaseStart);
  gDirectHost = directIp(kDirectC64Host);
  Serial.println("Direct mode: all devices gone, DHCP starts at .64 again");
}

void serviceWiFi(uint32_t now) {
  if (app.portalActive) return;          // the portal takes precedence
  if (gDirectMode) {
    // If the access point is gone (start failed or ended from outside),
    // restart it at the normal retry interval.
    if ((!gDirectApUp || WiFi.getMode() != WIFI_AP) &&
        (app.lastWiFiAttemptMs == 0 || now - app.lastWiFiAttemptMs >= kWiFiRetryMs)) {
      startDirectAp(now);
    }
    // Last device gone: reset DHCP so the c64u gets .64 again.
    static size_t lastStations = 0;
    const size_t stations = directStationCount();
    if (gDirectApUp && lastStations > 0 && stations == 0) directRestartDhcp();
    lastStations = stations;
    return;
  }
  if (!hasWiFiConfig()) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!gWifiNoted) {
      const int index = wifiProfileIndex(staSsid());
      if (index >= 0) gWifiTry = static_cast<size_t>(index);
      gWifiNoted = true;
    }
    return;
  }
  gWifiNoted = false;
  if (app.lastWiFiAttemptMs == 0 || now - app.lastWiFiAttemptMs >= kWiFiRetryMs) beginWiFi(now);
}

void refreshConnectionStatus(uint32_t now, bool force = false) {
  const bool wasOk   = app.connection.wifiConnected && app.connection.authOk;
  const bool wasWifi = app.connection.wifiConnected;
  app.connection.wifiConnected = netReady();

  // Direct mode: when a new device connects to the access point, it is
  // usually the c64u - check right away, starting again at <net>.64.
  static size_t lastStations = 0;
  const size_t stations = directStationCount();
  if (gDirectMode && stations != lastStations) {
    app.barDirty    = true;
    app.screenDirty = true;
    if (stations > lastStations && !app.connection.authOk) {
      gDirectHost = directIp(kDirectC64Host);
      force       = true;
      directScheduleRechecks(now);
    }
  }
  lastStations = stations;
  // follow-up check after joining (see directRecheckDue)
  if (directRecheckDue(now)) force = true;

  if (gDirectMode && app.connection.wifiConnected && stations == 0) {
    // Nobody connected - then no attempt is needed.
    gDirectHost                    = directIp(kDirectC64Host);
    app.connection.targetReachable = false;
    app.connection.authOk          = false;
    app.connection.detail          = "Direct: c64u not connected";
  } else if (!configReady()) {
    app.connection.targetReachable = false;
    app.connection.authOk          = false;
    app.connection.detail          = hasWiFiConfig() ? "c64u address missing"
                                                     : "set up SETUP > WiFi";
  } else if (!app.connection.wifiConnected) {
    app.connection.targetReachable = false;
    app.connection.authOk          = false;
    app.connection.detail          = "WiFi disconnected";
  } else if (force || app.lastConnectionProbeMs == 0 ||
             now - app.lastConnectionProbeMs >= kConnectionProbeMs) {
    app.lastConnectionProbeMs = now;

    const ApiResponse reach = sendApiRequest("GET", "/v1/version", false);
    app.connection.targetReachable = reach.transportOk;
    // A c64u answers /v1/version with JSON - or, if it requires a password
    // and none was sent, with 401/403.
    const bool looksLikeC64u = reach.jsonOk || reach.httpCode == 401 || reach.httpCode == 403;
    if (gDirectMode && looksLikeC64u) directConfirmHost();
    if (!reach.transportOk) {
      app.connection.authOk = false;
      app.connection.detail = reach.errors.isEmpty() ? "Target unreachable" : reach.errors;
      directNextCandidate();                 // in direct mode try the next address
    } else if (gDirectMode && !looksLikeC64u) {
      // Someone answered in the direct network, but not a c64u - keep looking.
      app.connection.targetReachable = false;
      app.connection.authOk          = false;
      app.connection.detail          = "Direct: no c64u at " + gDirectHost;
      directNextCandidate();
    } else if (targetPassword().isEmpty()) {
      // Without a stored password the second request would be byte-identical to
      // the first - the X-Password header is only set when there is one. Every
      // saved request leaves room on the c64u for a second device.
      app.connection.authOk = reach.apiOk;
      app.connection.detail = reach.apiOk ? "Reachable + auth ok"
                                          : (reach.errors.isEmpty() ? "Auth failed" : reach.errors);
    } else {
      const ApiResponse auth = sendApiRequest("GET", "/v1/version", true);
      app.connection.authOk = auth.apiOk;
      app.connection.detail = auth.apiOk ? "Reachable + auth ok"
                                         : (auth.errors.isEmpty() ? "Auth failed" : auth.errors);
    }
  }

  // The Core only redraws list pages when screenDirty is set. If the
  // connection comes up in the background, the Wi-Fi menu, the network list,
  // the settings and the status page still have to show it right away.
  const bool isOk = app.connection.wifiConnected && app.connection.authOk;
  if (isOk != wasOk || app.connection.wifiConnected != wasWifi) {
    app.barDirty    = true;
    app.screenDirty = true;
  }
}

// ===========================================================================
//  Status lamp (SK6812 on Port B)
//
//  In one colour the lamp shows the same as the dot on the left of the
//  status bar, plus a few movements you can pick up even out of the corner
//  of your eye:
//
//    red             no Wi-Fi
//    blue            Wi-Fi up, the c64u does not answer
//    yellow          c64u reachable, the password is wrong
//    green           everything connected
//    magenta chase   the setup portal is running
//    cyan bar        a file is being loaded into the c64u; the three LEDs are
//                    a progress bar, the section in progress pulses
//    short flash     NFC card read (green) or an error (red)
//
//  If no LED is attached to Port B the data goes nowhere - that does no harm.
//  It cannot be detected, hence the "Status-LED" switch in the setup.
// ===========================================================================
uint32_t ledRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

// Darken a colour. `scale` is 0..255 and applies to all three channels.
uint32_t ledScale(uint32_t rgb, uint8_t scale) {
  const uint16_t r = ((rgb >> 16) & 0xFF) * scale / 255;
  const uint16_t g = ((rgb >> 8) & 0xFF) * scale / 255;
  const uint16_t b = (rgb & 0xFF) * scale / 255;
  return ledRgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

// Triangle curve 0..255..0 over `periodMs`. Gives a calm breathing without
// floating point and without a table.
uint8_t ledPulse(uint32_t now, uint32_t periodMs) {
  const uint32_t phase = now % periodMs;
  const uint32_t half  = periodMs / 2;
  const uint32_t up    = (phase < half) ? phase : (periodMs - phase);
  return static_cast<uint8_t>((up * 255) / half);
}

// Request an event flash. Used by the card handling.
//
// ledFlashMs == 0 explicitly means "no flash". Without that marker the old
// timestamp would stay put for good, and the comparison
// (int32_t)(now - ledFlashMs) < 0 would become true again after roughly 25 days
// of uptime - the lamp would then show the old flash colour for weeks.
void ledFlash(uint32_t rgb) {
  app.ledFlashRgb = rgb;
  app.ledFlashMs  = millis() + kLedFlashMs;
  if (app.ledFlashMs == 0) app.ledFlashMs = 1;
}

// Colour that corresponds to the current connection state
uint32_t ledConnectionColour() {
  if (!app.connection.wifiConnected)   return ledRgb(255, 0, 0);      // red
  if (!app.connection.targetReachable) return ledRgb(0, 80, 255);     // blue
  if (!app.connection.authOk)          return ledRgb(255, 170, 0);    // yellow
  return ledRgb(0, 255, 80);                                          // green
}

void serviceLed(uint32_t now) {
  if (!app.settings.ledEnabled) {
    // Switch off once, then send nothing more.
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
  if (!flashOn) app.ledFlashMs = 0;      // forget an expired flash

  if (flashOn) {
    // The event flash takes precedence, covers all three and fades out.
    const uint32_t leftMs = app.ledFlashMs - now;
    const uint32_t c = ledScale(app.ledFlashRgb,
                                static_cast<uint8_t>((leftMs * 255) / kLedFlashMs));
    for (uint16_t i = 0; i < kLedCount; ++i) rgb[i] = c;

  } else if (app.screen == ScreenMode::Busy) {
    // Upload: the three LEDs are a progress bar. Finished sections shine at
    // full brightness, the one in progress pulses, the rest stay dark. If the
    // file size is unknown, a running light takes over instead.
    const uint32_t base = ledRgb(0, 220, 255);
    if (app.busyTotal > 0) {
      // Progress in 256ths per LED so that the arithmetic stays integer:
      // 0 .. kLedCount*256
      const uint32_t done = (256ULL * kLedCount * app.busySent) / app.busyTotal;
      for (uint16_t i = 0; i < kLedCount; ++i) {
        const uint32_t lo = 256u * i;
        if (done >= lo + 256u) {
          rgb[i] = base;                                    // done
        } else if (done > lo || i == 0) {
          // The first LED already pulses at 0 % - otherwise the start of an
          // upload would look as if the lamp were broken.
          // The section in progress pulses, so you can see things are moving
          const uint8_t fill = static_cast<uint8_t>(60 + (done - lo) * 195 / 256);
          const uint8_t puls = static_cast<uint8_t>(120 + ledPulse(now, 500) * 135 / 255);
          rgb[i] = ledScale(base, static_cast<uint8_t>((fill * puls) / 255));
        } else {
          rgb[i] = 0;                                       // nothing yet
        }
      }
    } else {
      const uint16_t active = (now / 160) % kLedCount;
      for (uint16_t i = 0; i < kLedCount; ++i) {
        rgb[i] = (i == active) ? base : ledScale(base, 25);
      }
    }

  } else if (app.portalActive) {
    // Portal: a magenta running light, much slower than the upload.
    const uint32_t base   = ledRgb(255, 0, 200);
    const uint16_t active = (now / 400) % kLedCount;
    for (uint16_t i = 0; i < kLedCount; ++i) {
      rgb[i] = (i == active) ? ledScale(base, 60 + ledPulse(now, 1600) * 195 / 255)
                             : ledScale(base, 25);
    }

  } else {
    // Connection state: all three in the same colour.
    const uint32_t c = ledConnectionColour();
    for (uint16_t i = 0; i < kLedCount; ++i) rgb[i] = c;
  }

  // Only send when something has actually changed.
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
  if (netReady()) return true;
  beginWiFi(now);
  setModal("NO WIFI", kColWarn, now);
  return false;
}

// ---------------------------------------------------------------------------
// Simple machine commands
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
  setModal("POWER OFF? TAP AGAIN!", kColWarn, now, powerOffConfirmMs());
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

// ---------------------------------------------------------------------------
// Joystick ports
//
// The ReST API has no dedicated "machine:" command for swapping the ports.
// The c64u keeps the mapping as the configuration item "Joystick Swapper" in
// the category "U64 Specific Settings", so it is set through /v1/configs,
// just like the CPU speed. The list of values comes from the device and reads
// "Normal", "Swapped", "WASD Port 1", "WASD Port 2", depending on firmware.
// ---------------------------------------------------------------------------

// Short form for the card: "Swapped" -> "SWAPPED", "WASD Port 1" -> "WASD1".
String joyTokenFromValue(const String& value) {
  String upper = trimCopy(value);
  upper.toUpperCase();
  if (upper.indexOf("WASD") >= 0) {
    const String digits = extractDigits(upper);
    return digits.isEmpty() ? String("WASD") : ("WASD" + digits);
  }
  if (upper.indexOf("SWAP")   >= 0) return "SWAPPED";
  if (upper.indexOf("NORMAL") >= 0) return "NORMAL";
  upper.replace(" ", "");
  return upper;
}

// Looks up the value the c64u expects for a short form. While the list is
// still unknown the short form is passed through unchanged.
String joyValueFromToken(const String& token) {
  const String want = joyTokenFromValue(token);
  for (size_t i = 0; i < app.joyChoiceCount; ++i) {
    if (joyTokenFromValue(app.joyOptions[i]) == want) return app.joyOptions[i];
  }
  return trimCopy(token);
}

// Short text for the list and the message.
String joyLabelFromToken(const String& token) {
  const String t = joyTokenFromValue(token);
  if (t == "NORMAL")  return "Normal";
  if (t == "SWAPPED") return "Swapped";
  if (t == "WASD")    return "WASD";
  if (t.startsWith("WASD")) return "WASD P" + t.substring(4);
  return trimCopy(token);
}

// Looks for the item with "Joystick" in its name inside a category.
bool inspectJoyCategory(const String& category, String* itemOut, String* valueOut) {
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
    if (key.indexOf("Joystick") >= 0) {
      *itemOut  = key;
      *valueOut = jsonValueToString(kv.value());
      return true;
    }
  }
  return false;
}

// Fetches the list of values and the current state of the item found.
bool refreshJoyChoices() {
  if (app.joyCategory.isEmpty() || app.joyItem.isEmpty()) return false;

  const ApiResponse response = sendApiRequest(
      "GET", "/v1/configs/" + urlEncode(app.joyCategory) + "/" + urlEncode(app.joyItem), true);
  if (!response.apiOk) return false;

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, response.body) != DeserializationError::Ok) return false;

  JsonVariant itemObject = doc[app.joyCategory][app.joyItem];
  if (itemObject.isNull()) return false;

  app.joyChoiceCount = 0;
  JsonArray values = itemObject["values"].as<JsonArray>();
  for (JsonVariant value : values) {
    if (app.joyChoiceCount >= kMaxJoyChoices) break;
    app.joyOptions[app.joyChoiceCount] = trimCopy(jsonValueToString(value));
    app.joyChoiceCount += 1;
  }
  if (app.joyChoiceCount == 0) return false;

  app.joyValue = trimCopy(jsonValueToString(itemObject["current"]));
  return true;
}

// Finds category and item name once and remembers them.
bool resolveJoyPath(String* detailOut = nullptr) {
  if (app.joyPathKnown) {
    if (app.joyChoiceCount == 0) refreshJoyChoices();
    return true;
  }

  String item, value;
  if (inspectJoyCategory("U64 Specific Settings", &item, &value)) {
    app.joyCategory  = "U64 Specific Settings";
    app.joyItem      = item;
    app.joyValue     = trimCopy(value);
    app.joyPathKnown = true;
    refreshJoyChoices();
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
    if (inspectJoyCategory(category, &item, &value)) {
      app.joyCategory  = category;
      app.joyItem      = item;
      app.joyValue     = trimCopy(value);
      app.joyPathKnown = true;
      refreshJoyChoices();
      return true;
    }
  }
  if (detailOut) *detailOut = "Joystick item not found";
  return false;
}

// Sets the port mapping to a fixed value.
void applyJoystickValue(const String& wanted, uint32_t now) {
  clearPendingPowerOff();
  if (!requireNetwork(now)) return;

  String detail;
  if (!resolveJoyPath(&detail)) { setModal(detail, kColErr, now, 2000); return; }

  const String target = joyValueFromToken(wanted);
  const String path   = "/v1/configs/" + urlEncode(app.joyCategory) + "/" + urlEncode(app.joyItem)
                      + "?value=" + urlEncode(target);

  const ApiResponse response = sendApiRequest("PUT", path, true);
  if (response.apiOk) {
    app.joyValue = target;
    beep(2600, 40);
    setModal("JOY " + joyLabelFromToken(target), kColOk, now);
  } else {
    beep(500, 120);
    setModal(response.errors.isEmpty() ? "JOY SET FAILED" : response.errors, kColErr, now, 2000);
  }
}

// Toggles between "Normal" and "Swapped". If the c64u sits on one of the WASD
// modes, it goes back to "Normal".
void toggleJoystickSwap(uint32_t now) {
  clearPendingPowerOff();
  if (!requireNetwork(now)) return;

  String detail;
  if (!resolveJoyPath(&detail)) { setModal(detail, kColErr, now, 2000); return; }
  refreshJoyChoices();

  applyJoystickValue(joyTokenFromValue(app.joyValue) == "NORMAL" ? "SWAPPED" : "NORMAL", now);
}

// Steps through every value the c64u offers, used by the settings menu.
void cycleJoystickValue(uint32_t now) {
  clearPendingPowerOff();
  if (!requireNetwork(now)) return;

  String detail;
  if (!resolveJoyPath(&detail)) { setModal(detail, kColErr, now, 2000); return; }
  refreshJoyChoices();
  if (app.joyChoiceCount == 0) { setModal("JOY?", kColErr, now, 2000); return; }

  size_t index = 0;
  for (size_t i = 0; i < app.joyChoiceCount; ++i) {
    if (joyTokenFromValue(app.joyOptions[i]) == joyTokenFromValue(app.joyValue)) { index = i; break; }
  }
  index = (index + 1) % app.joyChoiceCount;
  applyJoystickValue(app.joyOptions[index], now);
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
//  PETSCII keyboard input (for autostart after mounting)
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

// Writes up to 10 PETSCII characters into the C64 keyboard buffer
// ($0277..$0280) and sets the count in NDX ($C6).
bool typeToC64(const uint8_t* petscii, size_t len) {
  if (len == 0 || len > 10) return false;

  // Reset LSTX / NDX
  if (!sendApiRequest("PUT", "/v1/machine:writemem?address=C5&data=0000", true).apiOk) return false;
  if (!sendApiRequest("PUT", "/v1/machine:writemem?address=277&data=" + toHex(petscii, len), true).apiOk) return false;

  const uint8_t count = static_cast<uint8_t>(len);
  return sendApiRequest("PUT", "/v1/machine:writemem?address=C6&data=" + toHex(&count, 1), true).apiOk;
}

// Reads a single byte from C64 memory (via DMA).
// readmem returns the data as a binary attachment, so read it from the stream.
bool readC64Byte(uint16_t address, uint8_t* out) {
  if (!netReady()) return false;

  char path[72];
  snprintf(path, sizeof(path), "/v1/machine:readmem?address=%X&length=1", address);

  String body;
  int code = rawHttpRequest("GET", path, true, &body, 64);
  for (uint8_t attempt = 1; attempt < kApiConnectAttempts && code == HTTPC_ERROR_CONNECTION_REFUSED;
       ++attempt) {
    delay(kApiRetryDelayMs);
    code = rawHttpRequest("GET", path, true, &body, 64);
  }
  if (code != 200 || body.length() < 1) return false;
  *out = static_cast<uint8_t>(body[0]);
  return true;
}

// $CC (BLNSW) is 0 while the cursor is blinking - that is, exactly when BASIC
// is waiting for input. During LOAD/RUN the value is non-zero.
// This reliably tells us when the disk has finished loading,
// without guessing with fixed wait times.
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
    // This loop blocks for up to 15 s. The lamp is therefore taken along by
    // hand here, otherwise it would freeze on its last value.
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

// Determines the drive on IEC bus 8. With "Auto" the assignment is queried
// live from the c64u so that .d64 really shows up as device 8.
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
          if (key != "a" && key != "b") continue;          // skip softiec
          if (!kv.value().is<JsonObject>()) continue;
          JsonObject drive = kv.value().as<JsonObject>();
          if (drive["bus_id"].as<int>() == 8) return key;
        }
      }
    }
  }
  return "a";   // Factory default of drive A is bus 8
}

// lO"*",8,1<CR>   ("lO" = abbreviated LOAD, fits into 10 characters)
bool typeLoadFirstFile() {
  static const uint8_t seq[] = {0x4C, 0xCF, 0x22, 0x2A, 0x22, 0x2C, 0x38, 0x2C, 0x31, 0x0D};
  return typeToC64(seq, sizeof(seq));
}

// rU<CR>          ("rU" = abbreviated RUN)
bool typeRun() {
  static const uint8_t seq[] = {0x52, 0xD5, 0x0D};
  return typeToC64(seq, sizeof(seq));
}

// ===========================================================================
//  Streaming upload from the microSD to the c64u
//  Large files (e.g. a 175 kB .d64) are sent in small blocks so that
//  no RAM buffer the size of the file is needed.
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
  // The upload blocks the main loop. So that the lamp still pulses, it is
  // taken along by hand here.
  serviceLed(millis());
}

// Returns everything up to the double CRLF and then the body (max. 1 kB).
UploadResult uploadResultFrom(int httpCode, const String& body) {
  UploadResult result;
  result.httpCode = httpCode;
  result.ok       = httpCode >= 200 && httpCode < 300;
  if (httpCode <= 0) {
    result.message = httpCode == HTTPC_ERROR_READ_TIMEOUT ? String("Timeout")
                                                          : HTTPClient::errorToString(httpCode);
    return result;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, body) == DeserializationError::Ok) {
    const String errors = extractErrors(doc);
    if (!errors.isEmpty()) { result.ok = false; result.message = errors; }
  }
  if (result.message.isEmpty()) {
    result.message = result.ok ? "OK" : (result.httpCode == 403 ? "Forbidden (password?)"
                                                               : String("HTTP ") + result.httpCode);
  }
  return result;
}

// urlPath        e.g. "/v1/runners:run_prg" - arguments such as type/mode belong
//                in the query, NOT as a multipart text field: the Ultimate
//                firmware treats every multipart part as an attachment and would
//                read a leading text field as a file without an extension
//                ("Invalid type").
// multipartName  empty -> file as a plain body (octet-stream)
//                otherwise -> multipart/form-data with exactly this one file field
UploadResult uploadFile(const String& urlPath, File& file, const String& fileName,
                        const String& multipartName) {
  UploadResult result;
  const size_t fileSize = file.size();

  int fd = -1;
  int opened = HTTPC_ERROR_CONNECTION_REFUSED;
  for (uint8_t attempt = 0; attempt < kApiConnectAttempts && opened != 0; ++attempt) {
    if (attempt > 0) delay(kApiRetryDelayMs);
    opened = rawOpen(gDirectMode ? kHttpConnectDirectMs : kHttpConnectTimeoutMs, &fd);
  }
  if (opened != 0) {
    result.message = "connection failed";
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

  bool sendOk = rawSendAll(fd, reinterpret_cast<const uint8_t*>(request.c_str()), request.length(),
                           kUploadSendMs);
  if (sendOk && multipart && !head.isEmpty()) {
    sendOk = rawSendAll(fd, reinterpret_cast<const uint8_t*>(head.c_str()), head.length(), kUploadSendMs);
  }

  // ---- Send the file block by block ----
  static uint8_t buffer[kUploadChunk];
  size_t sent = 0;
  uint32_t lastUi = 0;
  while (sendOk && sent < fileSize) {
    const int chunk = file.read(buffer, kUploadChunk);
    if (chunk <= 0) break;
    if (!rawSendAll(fd, buffer, static_cast<size_t>(chunk), kUploadSendMs)) {
      ::close(fd);
      result.message = "Upload aborted";
      return result;
    }
    sent += static_cast<size_t>(chunk);

    const uint32_t now = millis();
    if (now - lastUi > 120) {
      lastUi = now;
      publishProgress(sent, fileSize);
    }
  }
  if (sendOk && multipart && !tail.isEmpty()) {
    sendOk = rawSendAll(fd, reinterpret_cast<const uint8_t*>(tail.c_str()), tail.length(), kUploadSendMs);
  }
  if (!sendOk) {
    ::close(fd);
    result.message = "Upload aborted";
    return result;
  }
  publishProgress(fileSize, fileSize);

  String body;
  const int code = rawReadResponse(fd, kUploadReplyMs, &body, 1024);
  ::close(fd);
  return uploadResultFrom(code, body);
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
  // Folders first, then alphabetically (simple insertion sort, n <= 160)
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
      // random card: offer directories only
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
//  Network configuration: save, load, maintain
//
//  Storage:   NVS namespace "c64unet"
//               wn        number of profiles (0..kWifiProfileMax)
//               s0..s3    SSID
//               p0..p3    password
//               host      address of the c64u
//               hpass     password of the c64u
//               dmode     direct mode on (1) / off (0)
//               dssid     SSID of the direct network
//               dpass     password of the direct network
//               dnet      address range of the direct network, e.g. "192.168.4"
//             If nothing has been stored yet, the values come from build_env.h.
//
//  Input routes: NFC card, /wifi.txt on the SD card, setup portal.
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
  prefs.putBool("dmode",   gDirectMode);
  prefs.putString("dssid", gDirectSsid);
  prefs.putString("dpass", gDirectPass);
  prefs.putString("dnet",  gDirectNet);
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
  gDirectMode = prefs.getBool("dmode", false);
  gDirectSsid = prefs.getString("dssid", kDirectSsidDef);
  gDirectPass = prefs.getString("dpass", kDirectPassDef);
  gDirectNet  = prefs.getString("dnet",  kDirectNetDef);
  prefs.end();

  // Replace unusable direct network values with the defaults. WPA2
  // requires a password of eight to 63 characters.
  if (gDirectSsid.isEmpty() || gDirectSsid.length() > 32) gDirectSsid = kDirectSsidDef;
  if (gDirectPass.length() < 8 || gDirectPass.length() > 63) gDirectPass = kDirectPassDef;
  if (!directNetValid(gDirectNet)) gDirectNet = kDirectNetDef;
  gDirectHost = directIp(kDirectC64Host);

  // Empty fields are filled in from build_env.h. A c64u password entered there
  // can therefore not be set to "empty" - to do that, remove the entry from
  // build_env.h and rebuild.
  if (gTargetHost.isEmpty()) gTargetHost = buildHost();
  if (gTargetPass.isEmpty()) gTargetPass = buildHostPass();

  if (gWifiCount == 0 && !buildWifiSsid().isEmpty()) {
    gWifiProfiles[0].ssid = buildWifiSsid();
    gWifiProfiles[0].pass = buildWifiPass();
    gWifiCount = 1;
  }
}

// ---------------------------------------------------------------------------
// Switching direct mode
// ---------------------------------------------------------------------------
// Everything belonging to the old connection (status test, found address,
// CPU value) is discarded - then everything is checked afresh.
void resetDirectState() {
  app.connection            = ConnectionState();
  app.lastConnectionProbeMs = 0;
  app.currentCpuValue       = "Unknown";
  gDirectConfirmed          = "";
  gDirectMisses             = 0;
  gDirectHost               = directIp(kDirectC64Host);
}

// reconnect = false: the caller connects by itself afterwards (e.g. to
// a network just chosen).
void setDirectMode(bool on, uint32_t now, bool reconnect = true) {
  if (gDirectMode == on) return;
  gDirectMode = on;
  saveNetConfig();
  resetDirectState();
  if (!on) {
    stopDirectAp();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
  }
  app.lastWiFiAttemptMs = 0;
  if (reconnect && !app.portalActive) beginWiFi(now);   // starts the AP or connects to WiFi
}

// New address range. If direct mode is running, the AP restarts right away -
// the c64u then reconnects by itself.
void setDirectNet(const String& net, uint32_t now) {
  if (!directNetValid(net) || net == gDirectNet) return;
  gDirectNet = net;
  saveNetConfig();
  resetDirectState();
  if (gDirectMode && !app.portalActive) {
    stopDirectAp();
    startDirectAp(now);
  }
}

// Sort a new network in at the front. A network that is already known simply
// gets a new password; the oldest one drops off the back if need be.
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
// Reading card text in the WiFi scheme
//
//     WIFI:S:<ssid>;T:WPA;P:<password>;;
//
// This is the same format WiFi QR codes use. Any phone app that can write an
// NDEF text record will do. As a short form, "WIFI:<ssid>;<password>" is
// accepted as well.
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
    if (text[i] == ';') { ++i; continue; }      // field without a value
    ++i;                                        // skip the colon

    String value;
    while (i < text.length() && text[i] != ';') {
      if (text[i] == '\\' && i + 1 < text.length()) { value += text[i + 1]; i += 2; continue; }
      value += text[i++];
    }
    if (i < text.length()) ++i;                 // skip the semicolon

    key.toUpperCase();
    if (key == "S")      { ssid = value; sawField = true; }
    else if (key == "P") { pass = value; sawField = true; }
  }

  // Short form without field names: WIFI:<ssid>;<password>
  if (!sawField) {
    String rest = text.substring(5);
    const int sep = rest.indexOf(';');
    if (sep < 0) { ssid = trimCopy(rest); }
    else         { ssid = trimCopy(rest.substring(0, sep)); pass = rest.substring(sep + 1); }
    // A trailing semicolon is not part of the password
    while (pass.endsWith(";")) pass.remove(pass.length() - 1);
  }

  if (ssid.isEmpty()) return false;
  if (ssidOut) *ssidOut = ssid;
  if (passOut) *passOut = pass;
  return true;
}

// Counterpart to parseWifiText: build a card text from SSID and password.
// Special characters are escaped with a backslash as in the QR scheme.
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

// Is a WiFi card on the reader without anyone having asked for one?
bool textLooksLikeWifi(const String& text) {
  String head = trimCopy(text).substring(0, 5);
  head.toUpperCase();
  return head == "WIFI:";
}

// ---------------------------------------------------------------------------
// /wifi.txt from the SD card
//
//     # comment
//     ssid = MyWiFi
//     pass = secret
//
//     ssid = SecondNet
//     pass = alsosecret
//
//     host     = 192.168.0.64
//     hostpass =
//
//     direct      = off              direct mode: on / off
//     direct_ssid = C64uRemote-Direct
//     direct_pass = c64ultimate      at least eight characters
//     direct_net  = 192.168.4        CoreS3 = .1, c64u = .64
//
// Every new "ssid" line begins a new entry. The file may hold up to
// kWifiProfileMax networks.
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
    if (errorOut) *errorOut = "no SD card";
    return 0;
  }
  File file = SD.open(kWifiFileSd, FILE_READ);
  if (!file) {
    if (errorOut) *errorOut = "wifi.txt missing";
    return 0;
  }

  size_t added     = 0;
  bool   sawDirect = false;     // file contains direct mode settings
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
    else if (key == "direct") {
      String v = value;
      v.toLowerCase();
      gDirectMode = (v == "1" || v == "on" || v == "an" || v == "ja" || v == "yes" || v == "true");
      sawDirect   = true;
    }
    else if (key == "direct_ssid") {
      if (!value.isEmpty() && value.length() <= 32) gDirectSsid = value;
      sawDirect = true;
    }
    else if (key == "direct_pass" || key == "direct_password") {
      if (value.length() >= 8 && value.length() <= 63) gDirectPass = value;
      sawDirect = true;
    }
    else if (key == "direct_net") {
      if (directNetValid(value)) gDirectNet = value;
      sawDirect = true;
    }
  }
  flush();
  file.close();

  gDirectHost = directIp(kDirectC64Host);
  saveNetConfig();
  // A file with only direct mode settings is not an error.
  if (added == 0 && !sawDirect && errorOut) *errorOut = "no SSID in wifi.txt";
  return added;
}

// ---------------------------------------------------------------------------
// Counterpart to loadWifiFromSd: write all stored networks together with the
// address and password of the c64u to the SD card as /wifi.txt.
//
// An existing file is renamed to /wifi.bak beforehand, so nothing is lost.
// The passwords sit in the file in plain text - the header of the file points
// that out once more.
//
// Returns: number of networks written, 0 on error.
// ---------------------------------------------------------------------------
size_t saveWifiToSd(String* errorOut) {
  if (gWifiCount == 0) {
    if (errorOut) *errorOut = "nothing stored";
    return 0;
  }
  if (!app.sdReady && !initSd()) {
    if (errorOut) *errorOut = "no SD card";
    return 0;
  }

  // Move an existing file aside. An older backup has to give way for that,
  // otherwise SD.rename() fails.
  if (SD.exists(kWifiFileSd)) {
    if (SD.exists(kWifiFileSdBak)) SD.remove(kWifiFileSdBak);
    if (!SD.rename(kWifiFileSd, kWifiFileSdBak)) SD.remove(kWifiFileSd);
  }

  File file = SD.open(kWifiFileSd, FILE_WRITE);
  if (!file) {
    if (errorOut) *errorOut = "writing failed";
    return 0;
  }

  file.println("# ---------------------------------------------------------------------------");
  file.println("# C64uRemote - WiFi credentials for the M5Stack Core");
  file.println("#");
  file.println("# Written by the device through  SETUP > WiFi > Save to SD.");
  file.println("# It is read back through  SETUP > WiFi > Load from SD");
  file.println("# or at start-up, as long as no network is stored in the device.");
  file.println("#");
  file.println("# Careful: the passwords are in plain text here.");
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

  file.println("# Address and password of the Ultimate 64 / 1541 Ultimate II+");
  file.print("host     = ");
  file.println(gTargetHost);
  file.print("hostpass = ");
  file.println(gTargetPass);
  file.println();

  file.println("# Direct mode: own WiFi without a router, c64u at <direct_net>.64");
  file.print("direct      = ");
  file.println(gDirectMode ? "on" : "off");
  file.print("direct_ssid = ");
  file.println(gDirectSsid);
  file.print("direct_pass = ");
  file.println(gDirectPass);
  file.print("direct_net  = ");
  file.println(gDirectNet);

  file.flush();
  file.close();

  if (written == 0) {
    if (errorOut) *errorOut = "no network to save";
    SD.remove(kWifiFileSd);
    return 0;
  }
  return written;
}

// ---------------------------------------------------------------------------
// Network scan. The call blocks for a few seconds - the caller shows a notice
// beforehand.
// ---------------------------------------------------------------------------
void wifiRunScan() {
  app.wifiScanCount = 0;
  app.wifiScanIndex = 0;

  if (!app.portalActive && WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  // In direct mode only the AP runs - but only the station part can scan.
  // It is added for the duration of the scan; connected devices may briefly
  // lose contact.
  const bool directScan = gDirectMode && !app.portalActive && WiFi.getMode() == WIFI_AP;
  if (directScan) WiFi.mode(WIFI_AP_STA);

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
  if (directScan) WiFi.mode(WIFI_AP);
}

// Before the first connection attempt, pick out the strongest known network.
// Only worthwhile if more than one profile is stored.
void wifiPickBestProfile() {
  if (gDirectMode || gWifiCount < 2) return;

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
//  Setup portal: an access point of its own with a small web interface
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
  page += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += "<title>C64uRemote WiFi</title>";
  page += kPortalStyle;
  page += "</head><body><h1>C64uRemote</h1>";
  page += "<p>Enter the WiFi credentials. They are stored in the device - "
          "the source code no longer has to be rebuilt for that.</p>";

  page += "<form method=\"POST\" action=\"/save\">";

  page += "<label for=\"ssid\">Networks found</label>";
  page += "<select id=\"ssid\" name=\"ssid\">";
  page += "<option value=\"\">-- please choose --</option>";
  for (size_t i = 0; i < app.wifiScanCount; ++i) {
    page += "<option value=\"" + htmlEscape(gWifiScan[i].ssid) + "\">";
    page += htmlEscape(gWifiScan[i].ssid);
    page += " (" + String(static_cast<int>(gWifiScan[i].rssi)) + " dBm)";
    page += "</option>";
  }
  page += "</select>";

  page += "<label for=\"ssid2\">or type an SSID</label>";
  page += "<input id=\"ssid2\" name=\"ssid2\" maxlength=\"32\" autocomplete=\"off\">";

  page += "<label for=\"pass\">WiFi password</label>";
  page += "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"63\">";

  page += "<hr><label for=\"host\">c64u address (optional)</label>";
  page += "<input id=\"host\" name=\"host\" value=\"" + htmlEscape(gTargetHost) + "\">";
  page += "<label for=\"hpass\">c64u password (optional)</label>";
  page += "<input id=\"hpass\" name=\"hpass\" type=\"password\" maxlength=\"63\">";

  page += "<button type=\"submit\">Save and connect</button></form>";

  page += "<hr><p>Stored networks (" + String(static_cast<unsigned>(gWifiCount)) + "/" +
          String(static_cast<unsigned>(kWifiProfileMax)) + "):</p><ul>";
  if (gWifiCount == 0) page += "<li>none yet</li>";
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
  page += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += kPortalStyle;
  page += "</head><body><h1>";
  page += ok ? "Saved" : "Nothing saved";
  page += "</h1><p>";
  if (ok) {
    page += "The device now shuts the access point down and connects to \"";
    page += htmlEscape(ssid);
    page += "\". This connection breaks off in the process - that is normal.";
  } else {
    page += "No SSID was given. <a style=\"color:#8ce4ff\" href=\"/\">Back</a>";
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

  // The access point deliberately runs without a station part. If the STA
  // stays active it keeps looking for the stored network in the background -
  // the radio channel changes while it does, and phones that had joined get
  // dropped after a few seconds ("the portal ends by itself").
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  gDirectApUp = false;                        // a running direct AP is replaced
  delay(60);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);                       // no modem sleep in AP mode
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  // Fixed channel 1, so the AP can no longer be switched over.
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
  Serial.printf("Setup portal active: SSID %s, IP %s, heap %u\n", kPortalSsid,
                WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

void stopPortal(uint32_t now) {
  if (!app.portalActive) return;
  gPortal.stop();
  gPortalDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);    // switched off when the portal started
  app.portalActive    = false;
  app.portalCloseAtMs = 0;
  app.lastWiFiAttemptMs = 0;      // reconnect immediately
  beginWiFi(now);
  Serial.println("Setup portal stopped");
}

void servicePortal(uint32_t now) {
  if (!app.portalActive) return;

  gPortalDns.processNextRequest();
  gPortal.handleClient();

  // As long as a device is connected, the idle clock does not run on.
  if (WiFi.softAPgetStationNum() > 0) app.portalTouchedMs = now;

  if (app.portalCloseAtMs != 0 &&
      static_cast<int32_t>(now - app.portalCloseAtMs) >= 0) {
    stopPortal(now);
    // Without the switch the portal page would stay up with SSID and password
    // on it although the access point is already shut down.
    if (app.screen == ScreenMode::WifiPortal) setScreen(ScreenMode::WifiMenu, now);
    setModal("WIFI SAVED", kColOk, now, 2000);
    return;
  }
  if (now - app.portalTouchedMs >= kPortalIdleMs) {
    stopPortal(now);
    if (app.screen == ScreenMode::WifiPortal) setScreen(ScreenMode::WifiMenu, now);
    setModal("PORTAL STOPPED", kColWarn, now, 1800);
  }
}

// ===========================================================================
//  Send the file to the c64u and start it
// ===========================================================================
void startFileOnC64(const String& fullPath, uint32_t now) {
  const String ext  = lowerExt(fullPath);
  const String name = baseName(fullPath);

  if (!requireNetwork(now)) return;
  if (!isSupportedExt(ext)) {
    Serial.printf("not supported: '%s'  (extension '%s')\n", fullPath.c_str(), ext.c_str());
    app.rfidHint = name.isEmpty() ? String("Pfad leer") : name;
    setModal(ext.isEmpty() ? String("KEINE DATEIENDUNG") : ("TYP UNBEKANNT: " + ext),
             kColErr, now, 2600);
    return;
  }

  if (!app.sdReady && !initSd()) { setModal("NO SD CARD", kColErr, now, 2000); return; }

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
    // type and mode as query arguments, the file as the only multipart part
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

  // ---- Disk image: activate the drive and start the first program ----
  if (diskImage) {
    app.busyTitle  = "Laufwerk 8";
    app.busySent   = 0;
    app.busyTotal  = 0;
    app.busyDetail = "Laufwerk einschalten";
    drawBusyScreen();
    pushFrame();

    // ":on" switches the drive on resp. resets it. That way the image just
    // mounted is on the IEC bus even if drive A was disabled
    // beforehand.
    sendApiRequest("PUT", "/v1/drives/" + targetDrive + ":on", true);

    if (app.settings.diskAction != DiskActionMode::Mount) {
      app.busyDetail = "Reset";
      drawBusyScreen();
      pushFrame();
      sendApiRequest("PUT", "/v1/machine:reset", true);

      if (app.settings.diskAction == DiskActionMode::MountRun) {
        // Wait for the BASIC prompt, then write LOAD"*",8,1 into the
        // keyboard buffer.
        waitCursorBlinking(true, 15000, "waiting for READY");
        app.busyDetail = "LOAD \"*\",8,1";
        drawBusyScreen();
        pushFrame();
        typeLoadFirstFile();

        // Wait for the load to finish: first the cursor has to stop blinking
        // (LOAD running), then it blinks again (READY) - then comes RUN.
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
  setModal(String(diskImage ? "RUNNING: " : "GESTARTET: ") + name, kColOk, millis(), 2000);
}

// ===========================================================================
//  RFID2 (WS1850S) - reading and writing NFC cards
// ---------------------------------------------------------------------------
//  The card format is deliberately identical to the one used by the TeensyROM
//  NFC loader resp. the Zaparoo/TapTo project, so the same card works on both
//  systems:
//
//      A single NDEF record, type "Text" (Well Known, UTF-8),
//      content = path to the program file, e.g.  SD:OneLoad v5/Bubble Bobble.crt
//
//  Allowed prefixes: "SD:", "USB:", "TR:" or none at all (then SD applies).
//  A "?" as the file name starts a random file from the directory.
//
//  Layout on the card:
//
//  A) NTAG213/215/216, MIFARE Ultralight (SAK 0x00, 4-byte pages, no
//     key): NDEF TLV from page 4 on. Pages 0..3 (UID, lock bytes,
//     capability container) are left untouched.
//
//  B) MIFARE Classic 1K/4K/Mini (16-byte blocks): NDEF TLV in the
//     data blocks from block 4 on (sector trailers are skipped).
//     Authentication is tried first with the NDEF key D3F7D3F7D3F7,
//     alternatively with the factory key FFFFFFFFFFFF. Trailers and MAD
//     are never written - a card can therefore not be bricked, but an
//     unformatted Classic card may then only be readable on this
//     device.
//
//  In addition, the old raw format "C64UPATH" is still recognised when
//  reading, so that already written cards keep working.
// ===========================================================================
constexpr size_t  kMaxTextLen   = 246;   // like TeensyROM
constexpr size_t  kNdefBufSize  = 288;   // TLV + record + text + reserve
constexpr uint8_t kUlDataPage   = 4;     // NDEF starts on page 4
constexpr uint8_t kMagic[8]     = {'C', '6', '4', 'U', 'P', 'A', 'T', 'H'};

enum class CardKind : uint8_t { None, Classic, Ultralight };

// MIFARE Classic keys: first the NDEF standard, then the factory key
MFRC522_I2C::MIFARE_Key kKeyNdef    = {{0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}};
MFRC522_I2C::MIFARE_Key kKeyFactory = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
bool gClassicNdefFormatted = false;      // Result of the last authentication
bool gClassicTryNdefFirst  = true;       // reset per card

uint8_t trailerForBlock(uint8_t block) { return static_cast<uint8_t>((block / 4) * 4 + 3); }

// Linear index -> data block, sector trailers are skipped.
// 0->4, 1->5, 2->6, 3->8, 4->9, 5->10, 6->12 ...
uint8_t classicDataBlock(size_t index) {
  const size_t sector = 1 + index / 3;
  return static_cast<uint8_t>(sector * 4 + (index % 3));
}

// After a failed auth the card is in HALT state and only answers
// WUPA any more - REQA (PICC_IsNewCardPresent) then no longer finds
// it. So wake it up explicitly here and select it again.
bool reselectCard() {
  uint8_t atqa[2];
  uint8_t size = sizeof(atqa);
  if (rfid.PICC_WakeupA(atqa, &size) != MFRC522_I2C::STATUS_OK) return false;
  return rfid.PICC_Select(&(rfid.uid), 0) == MFRC522_I2C::STATUS_OK;
}

bool classicAuth(uint8_t block) {
  const uint8_t trailer = trailerForBlock(block);

  // Try the key that worked last time first, otherwise every block costs
  // an unnecessary failed attempt plus a re-select.
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
// Key dictionary for cloning foreign MIFARE Classic cards
// ---------------------------------------------------------------------------
const uint8_t kKeyDict[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},   // factory key
    {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},   // MAD / NDEF sector 0
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7},   // NDEF data
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

// Trailer block of an arbitrary block (1K/4K)
uint8_t blockTrailer(uint16_t block) {
  if (block < 128) return static_cast<uint8_t>((block / 4) * 4 + 3);
  const uint16_t b = block - 128;
  return static_cast<uint8_t>(128 + (b / 16) * 16 + 15);
}
bool blockIsTrailer(uint16_t block) { return blockTrailer(block) == block; }

// Checks whether the access bits of a trailer (bytes 6,7,8) are internally
// consistent. An invalid pattern would irreversibly lock the sector on write
// - such trailers are therefore NOT written.
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

// Authenticates the sector of a trailer block using the dictionary.
// Returns the key that was found and its type (A/B).
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
      if (!reselectCard()) return false;   // card gone
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

// ---------------------------------------------------------------------------
// NFC reader RF field only when needed
//
// The driver leaves the 13.56 MHz field on permanently after start. On the
// M5Dial, whose reader sits right next to the WiFi antenna, that noticeably
// disturbed WiFi reception. Here the reader sits further away, but the field is
// still only on for the actual probe and only while a card is being processed
// - which also saves power.
// ---------------------------------------------------------------------------
bool gRfidFieldOn = true;   // nach PCD_Init an / on after PCD_Init

void rfidFieldOn() {
  if (gRfidFieldOn) return;
  rfid.PCD_AntennaOn();
  gRfidFieldOn = true;
  delay(5);   // Karte braucht nach dem Einschalten ~5 ms / card needs ~5 ms after power-up
}

void rfidFieldOff() {
  if (!gRfidFieldOn) return;
  rfid.PCD_AntennaOff();
  gRfidFieldOn = false;
}

bool gRfidHold = false;   // processed card still present, field stays on
bool rfidHeldCardGone();

bool cardPresent() {
  if (!rfidHeldCardGone()) return false;
  rfidFieldOn();
  const bool found = rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial();
  if (found) gClassicTryNdefFirst = true;   // determine anew for every new card
  else       rfidFieldOff();
  return found;
}

// ---------------------------------------------------------------------------
// Fast presence probe for the background polling
//
// If no card is present the MFRC522 waits after the REQA command until its
// internal timer expires - PCD_Init() sets 0x03E8 = 1000 steps of 25 us
// each for that, i.e. 25 ms. The main loop stalls for exactly that long.
//
// A card, however, answers far faster (frame delay time at 106 kBit/s:
// about 86 us). For the plain probe the time window is therefore shortened
// to ~2 ms and restored to its original value immediately afterwards -
// still before selecting the card. Authentication, reading and writing
// therefore run unchanged with the full time window.
// ---------------------------------------------------------------------------
uint16_t gRfidTimerReload = 0x03E8;         // read from the chip in initRfid()
constexpr uint16_t kRfidProbeReload = 80;   // 80 * 25 us = 2 ms

void setRfidTimerReload(uint16_t ticks) {
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegH, static_cast<byte>(ticks >> 8));
  rfid.PCD_WriteRegister(MFRC522_I2C::TReloadRegL, static_cast<byte>(ticks & 0xFF));
}

bool cardPresentQuick() {
  if (!rfidHeldCardGone()) return false;
  rfidFieldOn();
  setRfidTimerReload(kRfidProbeReload);
  const bool present = rfid.PICC_IsNewCardPresent();
  setRfidTimerReload(gRfidTimerReload);     // restore before the selection
  if (!present || !rfid.PICC_ReadCardSerial()) {
    rfidFieldOff();
    return false;
  }
  gClassicTryNdefFirst = true;
  return true;
}

// ---------------------------------------------------------------------------
// Holding on to a processed card
//
// After processCard() the card has been put to sleep with HLTA. With the field
// permanently on it no longer answers REQA - so it is not executed or written a
// second time while it stays on the reader. Switching the field off and on
// again, however, wakes it up fresh and it counts as a new card: then it beeped
// every second, the card was written again and again, and pulling it off in the
// middle gave a write error. So after a processed card the field stays on until
// the card is gone. Whether it is still there is checked with WUPA, which also
// wakes a sleeping card; it is put back to sleep right away.
// ---------------------------------------------------------------------------
void rfidHoldCard() {
  rfidRelease();   // HLTA + Crypto aus / HLTA + crypto off
  gRfidHold = true;
}

uint8_t gRfidHoldMisses = 0;

bool rfidHeldCardGone() {
  if (!gRfidHold) return true;
  setRfidTimerReload(kRfidProbeReload);
  uint8_t atqa[2];
  uint8_t size = sizeof(atqa);
  const bool there = rfid.PICC_WakeupA(atqa, &size) == MFRC522_I2C::STATUS_OK;
  setRfidTimerReload(gRfidTimerReload);
  if (there) {
    rfid.PICC_HaltA();
    gRfidHoldMisses = 0;
    return false;
  }
  // Only two misses in a row count as the card being gone.
  if (++gRfidHoldMisses < 2) return false;
  gRfidHoldMisses = 0;
  gRfidHold = false;
  rfidFieldOff();
  return true;
}

CardKind cardKind() {
  const uint8_t type = rfid.PICC_GetType(rfid.uid.sak);
  if (type == MFRC522_I2C::PICC_TYPE_MIFARE_MINI ||
      type == MFRC522_I2C::PICC_TYPE_MIFARE_1K ||
      type == MFRC522_I2C::PICC_TYPE_MIFARE_4K) {
    return CardKind::Classic;
  }
  // NTAG213/215/216 and Ultralight all report with SAK 0x00
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

// ---- Ultralight / NTAG: 4 bytes per page --------------------------------
// MIFARE_Read always returns 16 bytes, i.e. four pages at once.
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

// GET_VERSION (0x60) - on NTAG21x returns eight bytes with manufacturer,
// product type and memory size. Older Ultralight do not know the command.
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

// Product name derived from the storage size byte of the GET_VERSION reply
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
// Access to the NDEF data area, 16 bytes at a time
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
// NDEF: a single text record (Well Known, UTF-8, language "en")
//
//   TLV      : 03 <len> ... FE
//   Record   : D1 01 <plen> 54 | 02 'e' 'n' | <text>
//              D1 = MB|ME|SR|TNF=1 (Well Known), 54 = 'T'
//              status byte 02 = UTF-8, two-character language code
// ---------------------------------------------------------------------------
size_t buildNdefText(const String& text, uint8_t* out, size_t cap) {
  const size_t textLen    = text.length();
  const size_t payloadLen = 3 + textLen;          // status + "en" + text
  const size_t recordLen  = 4 + payloadLen;       // header + type length + length + type
  const size_t total      = 2 + recordLen + 1;    // TLV header + record + terminator
  if (payloadLen > 255 || total > cap) return 0;

  size_t i = 0;
  out[i++] = 0x03;                                       // TLV: NDEF message
  out[i++] = static_cast<uint8_t>(recordLen);
  out[i++] = 0xD1;                                       // MB|ME|SR|TNF=Well Known
  out[i++] = 0x01;                                       // type length
  out[i++] = static_cast<uint8_t>(payloadLen);
  out[i++] = 'T';                                        // type "Text"
  out[i++] = 0x02;                                       // UTF-8, 2-character language code
  out[i++] = 'e';
  out[i++] = 'n';
  for (size_t k = 0; k < textLen; ++k) out[i++] = static_cast<uint8_t>(text[k]);
  out[i++] = 0xFE;                                       // TLV: end

  // Pad to a multiple of 16 so that whole blocks get written
  while (i % 16 != 0 && i < cap) out[i++] = 0x00;
  return i;
}

// Searches the data stream for the first text record and returns its content.
bool parseNdefText(const uint8_t* data, size_t len, String* out) {
  size_t i = 0;

  // Walk the TLV chain until the NDEF message shows up
  size_t msgStart = 0;
  size_t msgLen   = 0;
  while (i < len) {
    const uint8_t tag = data[i++];
    if (tag == 0x00) continue;                     // NULL TLV
    if (tag == 0xFE) return false;                 // end without a message
    if (i >= len) return false;

    size_t tlvLen = data[i++];
    if (tlvLen == 0xFF) {                          // 3-byte length
      if (i + 1 >= len) return false;
      tlvLen = (static_cast<size_t>(data[i]) << 8) | data[i + 1];
      i += 2;
    }
    if (tag == 0x03) { msgStart = i; msgLen = tlvLen; break; }
    i += tlvLen;                                   // skip other TLV
  }
  if (msgLen == 0 || msgStart + msgLen > len) return false;

  // Walk through the records of the message
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

    // Some writers put a wrong payload length in - TeensyROM for instance
    // writes a constant 0x10 there even though the TLV length is correct.
    // On the last record (ME flag) the TLV length therefore takes
    // precedence, it is the more reliable figure.
    if ((header & 0x40) != 0 && payloadPos < end) {
      const size_t fromTlv = end - payloadPos;
      if (fromTlv != payloadLen) payloadLen = fromTlv;
    }
    if (payloadPos + payloadLen > len) {
      if (payloadPos >= len) return false;
      payloadLen = len - payloadPos;          // better to truncate than to give up
    }

    // Well Known "T" = text record
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
          if (b == 0x00 || b == 0xFE) break;   // padding bytes / end of TLV
          text += static_cast<char>(b);
        }
        *out = text;
        return true;
      }
    }

    p = payloadPos + payloadLen;
    if ((header & 0x40) != 0) break;               // ME: last record
  }
  return false;
}

// ---------------------------------------------------------------------------
// Read the card content: NDEF first, alternatively the old raw format "C64UPATH"
// ---------------------------------------------------------------------------
struct CardContent {
  bool   ok       = false;
  bool   isNdef   = false;
  bool   isLegacy = false;
  String text;                 // raw text from the card (with prefix)
  String error;
  uint8_t raw[16]  = {0};      // first 16 bytes, for the info display
  uint8_t raw2[16] = {0};      // the next 16 bytes
};

CardContent readCardContent(CardKind kind) {
  CardContent result;
  if (kind == CardKind::None) {
    result.error = "card type not supported";
    return result;
  }

  static uint8_t buffer[kNdefBufSize];
  memset(buffer, 0, sizeof(buffer));

  if (!cardReadChunk(kind, 0, buffer)) {
    result.error = (kind == CardKind::Classic) ? "block 4 not readable (key?)"
                                               : "page 4 not readable";
    return result;
  }
  memcpy(result.raw, buffer, 16);
  if (cardReadChunk(kind, 1, buffer + 16)) memcpy(result.raw2, buffer + 16, 16);

  // Old raw format?
  if (memcmp(buffer, kMagic, sizeof(kMagic)) == 0) {
    const uint8_t len = buffer[9];
    if (len == 0 || len > 128) {
      result.error = "invalid length";
      return result;
    }
    String path;
    path.reserve(len + 1);
    // The old format sat in chunks 1..8 behind the header
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

  // NDEF: get the length from the TLV and load only as much as needed
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

  result.error = (buffer[0] == 0x03) ? "NDEF without a text record" : "no NDEF text";
  return result;
}

// ---------------------------------------------------------------------------
// Write the card: always as an NDEF text record
// ---------------------------------------------------------------------------
bool writeCardText(CardKind kind, const String& text, String* errorOut) {
  if (kind == CardKind::None) {
    if (errorOut) *errorOut = "card type not supported";
    return false;
  }
  if (text.isEmpty() || text.length() > kMaxTextLen) {
    if (errorOut) *errorOut = "text too long (max " + String(kMaxTextLen) + ")";
    return false;
  }

  static uint8_t buffer[kNdefBufSize];
  const size_t total = buildNdefText(text, buffer, sizeof(buffer));
  if (total == 0) {
    if (errorOut) *errorOut = "NDEF does not fit";
    return false;
  }

  const size_t chunks = total / 16;
  for (size_t chunk = 0; chunk < chunks; ++chunk) {
    if (!cardWriteChunk(kind, chunk, buffer + chunk * 16)) {
      if (errorOut) {
        *errorOut = (chunk == 0) ? "card not writable"
                                 : "card too small from block " + String(chunk);
      }
      return false;
    }
  }

  // Check: read back and compare
  const CardContent check = readCardContent(kind);
  if (!check.ok || check.text != text) {
    if (errorOut) *errorOut = check.ok ? "Kontrolle abweichend" : check.error;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Back a card up to the SD and restore it from there
//
//  File format (text file, /NFC-DUMPS/<uid>.nfc):
//      # C64uRemote NFC dump
//      type NTAG215
//      uid  04 01 A1 01 C1 47 03
//      sak  00
//      P4   03 27 D1 01            <- Ultralight: 4 bytes per page
//      B4   00 11 ... (16 bytes)   <- Classic: 16 bytes per block
//
//  When restoring, only the payload data is touched:
//    * Ultralight/NTAG from page 4 on, at most up to the end of the user area.
//      Pages 0-3 (UID, lock, CC) and the configuration pages are off limits -
//      a card could be locked permanently there.
//    * Classic only data blocks, no sector trailers and not block 0.
//  The UID itself is fixed in the chip, so a true 1:1 copy is not
//  possible - the content is, and that is what matters here.
// ---------------------------------------------------------------------------
constexpr const char* kDumpDir = "/NFC-DUMPS";

// Last writable user page per NTAG type
uint16_t ntagLastUserPage(uint8_t storage) {
  switch (storage) {
    case 0x0F: return 39;    // NTAG213
    case 0x11: return 129;   // NTAG215
    case 0x13: return 225;   // NTAG216
    default:   return 39;    // when in doubt the smallest area
  }
}

bool dumpCardToSd(CardKind kind, String* fileOut, String* errorOut) {
  if (!app.sdReady && !initSd()) {
    if (errorOut) *errorOut = "no SD card";
    return false;
  }
  if (!SD.exists(kDumpDir) && !SD.mkdir(kDumpDir)) {
    if (errorOut) *errorOut = "folder NFC-DUMPS is missing";
    return false;
  }

  const String uid = cardUidString();
  const String path = String(kDumpDir) + "/" + uid + ".nfc";

  File out = SD.open(path, FILE_WRITE);
  if (!out) {
    if (errorOut) *errorOut = "cannot create the file";
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
    // Cloning foreign cards: authenticate per sector with the dictionary and back
    // up ALL blocks - even without NDEF content, purely the raw data.
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
          out.println("  no key found");
        }
      }
      if (!authed) continue;

      uint8_t buf[18] = {0};
      uint8_t sz = sizeof(buf);
      if (rfid.MIFARE_Read(static_cast<uint8_t>(block), buf, &sz) != MFRC522_I2C::STATUS_OK) {
        continue;
      }
      // Key A is never readable (comes back as 00..). So that the dump is
      // usable again, we write the key that was actually found into
      // the trailer.
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
    if (errorOut) *errorOut = "nothing readable";
    return false;
  }
  if (fileOut) *fileOut = path;
  Serial.printf("dump written: %s (%u entries)\n", path.c_str(),
                static_cast<unsigned>(written));
  return true;
}

// Split a line "P12  AA BB CC DD"
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
    if (errorOut) *errorOut = "no SD card";
    return false;
  }
  File in = SD.open(file, FILE_READ);
  if (!in) {
    if (errorOut) *errorOut = "dump not readable";
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
      if (index < 4 || index > lastPage) { skipped++; continue; }   // header and config off limits
      if (len < 4) continue;
      if (!ulWritePage(static_cast<uint8_t>(index), data)) {
        in.close();
        if (errorOut) *errorOut = "Seite " + String(index) + " not writable";
        return false;
      }
      written++;
    } else if (kind == CardKind::Classic && tag == 'B') {
      // Block 0 (manufacturer/UID) is write protected on normal cards.
      if (index == 0 || index > 255) { skipped++; continue; }
      if (len < 16) continue;

      const bool trailer = blockIsTrailer(index);

      // Only write the trailer if the access bits are consistent - an
      // invalid pattern would lock the sector permanently.
      if (trailer && !validAccessBits(data[6], data[7], data[8])) {
        skipped++;
        continue;
      }

      // Authenticate the target sector using the dictionary (a blank card uses FFFF..,
      // an already written one possibly some other known key).
      const uint8_t tr = blockTrailer(index);
      if (tr != destTrailer) {
        destTrailer = tr;
        destAuthed  = classicAuthDict(tr, nullptr, nullptr);
      }
      if (!destAuthed) { skipped++; continue; }

      // Data blocks are written before the trailer (ascending order
      // in the file), so we do not lose access.
      if (rfid.MIFARE_Write(static_cast<uint8_t>(index), data, 16) != MFRC522_I2C::STATUS_OK) {
        in.close();
        if (errorOut) *errorOut = String(trailer ? "Trailer " : "Block ") + String(index) +
                                  " not writable";
        return false;
      }
      written++;
    } else {
      skipped++;
    }
  }
  in.close();

  Serial.printf("restore: %u written, %u skipped\n",
                static_cast<unsigned>(written), static_cast<unsigned>(skipped));
  if (written == 0) {
    if (errorOut) *errorOut = "dump does not match the card";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Conversion between the card text and a path on our microSD
//
//   card:     SD:OneLoad v5/Bubble Bobble.crt
//   internal: /OneLoad v5/Bubble Bobble.crt
//
// The prefixes SD:, USB: and TR: come from the TeensyROM. We can only load
// from our own SD, but we still try the same path - that way a card works
// on both devices as long as the folders have the same names.
// ---------------------------------------------------------------------------
// Throw away everything below 0x20: some writer apps append a NUL,
// CR or LF to the text, which would make any extension unusable.
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
// Switching the device itself off (command card CMD:M5OFF)
//
// M5.Power.powerOff() switches off completely on battery. With USB attached
// the ESP32 keeps running - M5Unified then puts it into deep sleep, a press on
// the power or reset button starts it again.
// ---------------------------------------------------------------------------
constexpr uint32_t kDeviceOffBootGuardMs = 8000;

void shutdownDevice() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kColWarn, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.drawString("OFF", M5.Display.width() / 2, M5.Display.height() / 2);
  beep(1200, 60);
  delay(90);
  beep(700, 120);
  delay(600);
  if (app.sdReady) SD.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  M5.Power.powerOff();
}

// ---------------------------------------------------------------------------
// Command cards
//
// Instead of a file path a card may also carry a command for the c64u.
// Format: the prefix "CMD:" followed by the keyword, optionally with
// an argument after an equals sign.
//
//     CMD:RESET
//     CMD:REBOOT
//     CMD:MENU
//     CMD:POWEROFF=0      switch off immediately
//     CMD:POWEROFF=8      ask, 8 s time for the confirmation
//     CMD:POWEROFF        ask, using the time configured on the device
//     CMD:M5OFF           switch the CoreS3 itself off
//     CMD:CPU=10          set the CPU to 10 MHz
//     CMD:JOY             toggle the joystick ports (Normal <-> Swapped)
//     CMD:JOY=SWAPPED     set the ports fixed; also NORMAL, WASD1, WASD2
//     CMD:DIRECT=192.168.4  switch direct mode on with this network
//     CMD:DIRECT=OFF      direct mode off, back to the stored WiFi
//     CMD:DIRECT          toggle direct mode
//
// Upper/lower case and blanks do not matter. The content stays an
// ordinary NDEF text record, any NFC app can read such a card.
// The M5Dial version uses the same format - one card runs on both.
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
  else if (body == "M5OFF" || body == "DIALOFF") cmd.cmd = CardCmd::DeviceOff;
  else if (body == "CPU")      cmd.cmd = CardCmd::CpuSpeed;
  else if (body == "JOY" || body == "JOYSTICK") cmd.cmd = CardCmd::JoySwap;
  else if (body == "DIRECT" || body == "DIREKT") cmd.cmd = CardCmd::Direct;
  else return false;

  if (cmd.cmd == CardCmd::CpuSpeed && arg.isEmpty()) return false;

  *out = cmd;
  return true;
}

// Confirmation time of a PowerOff command in seconds.
// Without an argument the device setting applies, 0 means "no prompt".
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
    case CardCmd::DeviceOff: return "CMD:M5OFF";
    case CardCmd::CpuSpeed: return "CMD:CPU=" + c.arg;
    case CardCmd::JoySwap:  return (c.hasArg && !c.arg.isEmpty())
                                   ? ("CMD:JOY=" + joyTokenFromValue(c.arg))
                                   : String("CMD:JOY");
    case CardCmd::Direct:   return c.hasArg ? ("CMD:DIRECT=" + c.arg) : String("CMD:DIRECT");
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
                      : ("PowerOff, " + String(sec) + "s confirm");
    }
    case CardCmd::DeviceOff: return "CoreS3 Power Off";
    case CardCmd::CpuSpeed: return "CPU " + c.arg + " MHz";
    case CardCmd::JoySwap:  return (c.hasArg && !c.arg.isEmpty())
                                   ? ("Joystick " + joyLabelFromToken(c.arg))
                                   : String("Swap Joystick");
    case CardCmd::Direct: {
      String a = c.arg;
      a.toUpperCase();
      if (!c.hasArg)   return "Direct mode on/off";
      if (a == "OFF")  return "Direct mode off";
      return "Direct mode " + c.arg + ".x";
    }
    default:                return "?";
  }
}

// ---- Selection list for writing a card ------------------------------------
// Fixed commands first, then the joystick values and the CPU speeds the
// c64u offers.
constexpr size_t kCmdFixedCount = 10;

size_t cmdListCount() { return kCmdFixedCount + app.joyChoiceCount + app.cpuChoiceCount; }

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
    case 5: c.cmd = CardCmd::DeviceOff; return c;
    case 6: c.cmd = CardCmd::JoySwap; return c;   // toggle, without argument
    // Direct mode: first the network currently set, then the other of the
    // two usual ones, last "off".
    case 7:
      c.cmd = CardCmd::Direct; c.arg = gDirectNet; c.hasArg = true;
      return c;
    case 8:
      c.cmd    = CardCmd::Direct;
      c.arg    = (gDirectNet == kDirectNetDef) ? String(kDirectNetAlt) : String(kDirectNetDef);
      c.hasArg = true;
      return c;
    case 9: c.cmd = CardCmd::Direct; c.arg = "OFF"; c.hasArg = true; return c;
    default: break;
  }
  size_t rest = index - kCmdFixedCount;
  if (rest < app.joyChoiceCount) {
    c.cmd    = CardCmd::JoySwap;
    c.arg    = joyTokenFromValue(app.joyOptions[rest]);
    c.hasArg = true;
    return c;
  }
  rest -= app.joyChoiceCount;
  if (rest < app.cpuChoiceCount) {
    c.cmd    = CardCmd::CpuSpeed;
    c.arg    = extractDigits(app.cpuDisplayOptions[rest]);
    c.hasArg = true;
  }
  return c;
}

// Tag shown next to the list entry.
const char* cmdListTag(size_t index) {
  if (index < kCmdFixedCount) return "";
  if (index - kCmdFixedCount < app.joyChoiceCount) return "JOY";
  return "CPU";
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

  // Collapse double slashes. The notation "SD://folder/..." is
  // common, otherwise "//folder/..." would remain and SD.open() would
  // find nothing.
  String clean;
  clean.reserve(t.length() + 1);
  for (size_t i = 0; i < t.length(); ++i) {
    const char c = t[i];
    if (c == '/' && !clean.isEmpty() && clean[clean.length() - 1] == '/') continue;
    clean += c;
  }
  if (!clean.startsWith("/")) clean = "/" + clean;

  // We only keep a trailing slash if nothing follows it
  return clean;
}

String pathToCardText(const String& path) {
  String p = path;
  while (p.startsWith("/")) p.remove(0, 1);
  return "SD:" + p;
}

// "?" as the file name: pick a random file from the directory
// "?" as the file name or a plain directory path -> random selection
bool pathIsRandom(const String& path) {
  return path.endsWith("/?") || path == "?" || path.endsWith("/");
}

// Is there a directory on the SD under this path?
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
// Collect all available information about the card that is present
// ---------------------------------------------------------------------------
void addInfoLine(const String& text) {
  if (app.infoCount < AppState::kMaxInfoLines) app.infoLines[app.infoCount++] = text;
}

void addRawLine(const String& text) {
  if (app.infoCount2 < AppState::kMaxInfoLines) app.infoLines2[app.infoCount2++] = text;
}

// Configuration page of NTAG21x (that is where the password protection lives)
uint8_t ntagConfigPage(uint8_t storage) {
  switch (storage) {
    case 0x0F: return 0x29;   // NTAG213
    case 0x11: return 0x83;   // NTAG215
    case 0x13: return 0xE3;   // NTAG216
    default:   return 0;
  }
}

// NFC counter (READ_CNT 0x39) - only active when enabled in the chip
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
// Page 2: raw data and technical details
// ---------------------------------------------------------------------------
uint8_t gInfoStorage     = 0;      // storage byte from GET_VERSION
bool    gInfoHaveVersion = false;

void collectCardRaw(CardKind kind, uint8_t storage, bool haveVersion) {
  app.infoCount2 = 0;

  if (kind == CardKind::Ultralight) {
    // Pages 0..15 in blocks of eight, each of which fits on one line
    for (uint8_t base = 0; base < 16; base += 4) {
      uint8_t data[16] = {0};
      char label[12];
      if (!ulRead16(base, data)) {
        snprintf(label, sizeof(label), "S %2u-%-2u", base, base + 3);
        addRawLine(String(label) + "   not readable");
        continue;
      }
      snprintf(label, sizeof(label), "S %2u-%-2u", base, base + 1);
      addRawLine(String(label) + "   " + hexBytes(data, 8));
      snprintf(label, sizeof(label), "S %2u-%-2u", base + 2, base + 3);
      addRawLine(String(label) + "   " + hexBytes(data + 8, 8));
    }

    // Page 2 = lock bytes, page 3 = capability container
    uint8_t head[16] = {0};
    if (ulRead16(0, head)) {
      addRawLine("Lock      " + hexBytes(head + 10, 2) +
                 String((head[10] || head[11]) ? "  (gesperrt)" : "  (frei)"));
      const uint8_t* cc = head + 12;
      if (cc[0] == 0xE1) {
        addRawLine("CC        NDEF " + String(cc[1] >> 4) + "." + String(cc[1] & 0x0F) +
                   ", " + String(static_cast<uint16_t>(cc[2]) * 8) + " Byte, " +
                   String(cc[3] == 0x00 ? "read/write" : "read only"));
      } else {
        addRawLine("CC        " + hexBytes(cc, 4) + "  (not NDEF formatted)");
      }
    }

    // Password protection and read counter - both last, because an
    // unsupported command deselects the card.
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
      addRawLine("counter   " + String(counter) + " read operations");
    } else {
      addRawLine("counter   not enabled in the chip");
    }

  } else if (kind == CardKind::Classic) {
    static const uint8_t blocks[] = {0, 4, 5, 6, 8};
    for (size_t i = 0; i < sizeof(blocks); ++i) {
      uint8_t data[16] = {0};
      const String label = "Block " + String(blocks[i]) + (blocks[i] < 10 ? "   " : "  ");
      if (rfidReadBlock(blocks[i], data)) addRawLine(label + hexBytes(data, 16));
      else                                addRawLine(label + "not readable");
    }
    addRawLine(String("key       ") + (gClassicNdefFormatted ? " D3F7D3F7D3F7 (NDEF)"
                                                             : " FFFFFFFFFFFF (Werk)"));
  }
}

void collectCardInfo(CardKind kind) {
  app.infoCount    = 0;
  app.infoCount2   = 0;
  gInfoStorage     = 0;
  gInfoHaveVersion = false;

  // ---- Identification ----
  addInfoLine("UID       " + hexBytes(rfid.uid.uidByte, rfid.uid.size, 10) +
              "  (" + String(rfid.uid.size) + " Byte)");
  addInfoLine("SAK       0x" + hexBytes(&rfid.uid.sak, 1));

  // ---- Type, size, version ----
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

    // GET_VERSION: if it fails, the card has to be selected again
    if (ulGetVersion(version)) {
      gInfoStorage     = version[6];
      gInfoHaveVersion = true;
      const uint16_t fromVer = ntagBytesFromStorage(version[6]);
      if (fromVer) userBytes = fromVer;
      addInfoLine(String("Typ       ") + ntagNameFromStorage(version[6]) + ", " +
                  String(userBytes) + " Byte");
      addInfoLine(String("Version   ") + (version[1] == 0x04 ? "NXP  " : "") + hexBytes(version, 8));
    } else {
      reselectCard();          // make it responsive again after the failed attempt
      addInfoLine(String("Typ       NTAG / Ultralight (Type 2)") +
                  (userBytes ? (", " + String(userBytes) + " Byte") : String("")));
      addInfoLine("version   GET_VERSION not supported");
    }
  }

  // ---- Content ----
  const CardContent content = readCardContent(kind);

  if (kind == CardKind::Classic) {
    addInfoLine(String("Format    ") + (gClassicNdefFormatted ? "NDEF-key        D3F7.."
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

  // Directory and file name separated so that both are fully readable
  addInfoLine("Pfad      " + parentPath(path));
  addInfoLine("Datei     " + baseName(path));

  String state = "no SD card";
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
  addInfoLine("Typ/SD    " + (ext.isEmpty() ? String("no extension") : ext) + ", " + state);

  collectCardRaw(kind, gInfoStorage, gInfoHaveVersion);
}

// ===========================================================================
//  User interface
// ===========================================================================
constexpr int kTitleY   = 22;
constexpr int kTitleH   = 20;
constexpr int kListY    = 46;
// Taller rows than in the button edition (24 px there): a fingertip is about
// 8 mm wide, which is roughly 40 px on this display. 26 px is the compromise
// between hit rate and the number of visible entries.
constexpr int kRowH     = 26;
constexpr int kListRows = 6;                    // 46 + 6*26 = 202

// ---- Scrollbar next to the list -----------------------------------------
// It appears as soon as there are more entries than rows fit, shows the
// position and length of the list, and can be dragged directly.
constexpr int kScrollW   = 16;                  // width of the hit area
constexpr int kScrollX   = kScrW - kScrollW;    // left edge
constexpr int kScrollTop = kListY;
constexpr int kScrollH   = kListRows * kRowH;
constexpr int kThumbMinH = 28;                  // smallest possible thumb
// Right edge of the list rows - they end before the scrollbar.
constexpr int kListRight = kScrollX - 2;

// Small font helpers, so that no font type appears in the rest of the code.
inline void fontSmall() { gDraw->setFont(&fonts::Font0); }
inline void fontText()  { gDraw->setFont(&fonts::Font2); }
inline void fontBig()   { gDraw->setFont(&fonts::Font4); }

// The datum type is deliberately obtained via decltype so that no
// library-internal type name (textdatum_t) has to appear in the code.
using DatumT = decltype(middle_center);

// Draws text and truncates it if it would be wider than maxW.
void drawClipped(const String& text, int x, int y, int maxW, uint16_t color, DatumT datum) {
  gDraw->setTextColor(color);
  gDraw->setTextDatum(datum);
  String out = text;
  while (out.length() > 1 && gDraw->textWidth(out) > maxW) out.remove(out.length() - 1);
  gDraw->drawString(out, x, y);
}

// ---- Status bar ---------------------------------------------------------
// ---- Battery ------------------------------------------------------------
// Level and charging state come from M5Unified. Devices without a battery
// return a negative value - the line then simply stays as it was.
void refreshBattery(uint32_t now) {
  if (app.batteryPolled && now - app.lastBatteryPollMs < kBattPollMs) return;
  app.batteryPolled     = true;
  app.lastBatteryPollMs = now;
  app.batteryLevel      = M5.Power.getBatteryLevel();
  app.batteryCharging   = (M5.Power.isCharging() == m5::Power_Class::is_charging);
  app.vbusMilliVolt     = M5.Power.getVBUSVoltage();
}

// Lowest step reached? On the charger this never counts as a warning.
bool batteryLow() {
  return app.batteryLevel >= 0 && !app.batteryCharging &&
         app.batteryLevel < kBattBlinkAt;
}

// Dark half of the blink phase.
bool batteryDark(uint32_t now) {
  return batteryLow() && ((now / kBattBlinkMs) % 2) == 1;
}

// Colour for the current level. On the charger always turquoise.
uint16_t batteryColor() {
  if (app.batteryCharging)             return kColInfo;
  if (app.batteryLevel >= kBattGreen)  return kColOk;
  if (app.batteryLevel >= kBattYellow) return kColWarn;
  return kColErr;
}

// Short text for the status page.
String batteryText() {
  if (app.batteryLevel < 0) return "no battery";
  String text(app.batteryLevel);
  text += "%";
  if (app.batteryCharging) text += " charging";
  return text;
}

// The line below the status bar. An empty bar would be invisible, so a short
// stub remains - otherwise the blinking could not be seen.
void drawBatteryLine() {
  const uint32_t now = millis();
  refreshBattery(now);

  gDraw->drawFastHLine(0, kBarH - 1, kScrW, kColLine);
  if (app.batteryLevel < 0) return;
  if (batteryDark(now))     return;

  const uint16_t color = batteryColor();

  int width = (kScrW * app.batteryLevel) / 100;
  if (width < 4) width = 4;
  gDraw->drawFastHLine(0, kBarH - 2, width, color);
  gDraw->drawFastHLine(0, kBarH - 1, width, color);
}

// Is the right-hand end currently showing the battery instead of RFID/SD?
// Without a readable level it stays on RFID/SD.
bool batteryPhase(uint32_t now) {
  return app.batteryLevel >= 0 && ((now / kBarSwapMs) % 2) == 1;
}

// Is the device on external power? The AXP2101 in the CoreS3 reports the
// VBUS voltage; the IP5306 in the Core cannot (-1), so there only "charging"
// counts - with a full battery on the cable the bolt stays away.
bool batteryOnPower() {
  return app.batteryCharging || app.vbusMilliVolt > 4000;
}

// Small bolt to the left of the battery symbol. Drawn row by row so the
// shape holds at 5 x 7 pixels: two diagonals with a crossbar.
void drawChargeBolt() {
  constexpr int boltX = 264, boltY = 6;
  static constexpr int8_t rows[7][2] = {{3,2},{2,2},{1,2},{0,5},{2,2},{1,2},{0,2}};
  const uint16_t color = batteryColor();
  for (int i = 0; i < 7; ++i) {
    gDraw->drawFastHLine(boltX + rows[i][0], boltY + i, rows[i][1], color);
  }
}

// Small battery symbol with the percentage inside, 35 x 12 pixels - exactly
// the space "RFID" and "SD" take otherwise. It is not filled: the line below
// the bar already shows the level, here the colour carries the message and
// the number stays easy to read.
void drawBatterySymbol() {
  constexpr int bodyX = 272, bodyY = 4, bodyW = 32, bodyH = 12;
  const uint16_t color = batteryColor();

  gDraw->drawRect(bodyX, bodyY, bodyW, bodyH, color);
  gDraw->fillRect(bodyX + bodyW, bodyY + 4, 3, 4, color);

  String text(app.batteryLevel);
  text += "%";
  fontSmall();
  drawClipped(text, bodyX + bodyW / 2, kBarH / 2, bodyW - 4, color, middle_center);
}

void drawStatusBar() {
  const bool wifiOk   = netReady();
  const bool targetOk = app.connection.targetReachable;
  const bool authOk   = app.connection.authOk;

  uint16_t stateColor = kColErr;
  const char* stateText = "NO WIFI";
  if (wifiOk && authOk)        { stateColor = kColOk;   stateText = "C64U OK"; }
  else if (wifiOk && targetOk) { stateColor = kColWarn; stateText = "AUTH?";   }
  else if (wifiOk)             { stateColor = kColInfo; stateText = "NO C64U"; }

  gDraw->fillRect(0, 0, kScrW, kBarH, kColPanel);
  drawBatteryLine();

  gDraw->fillCircle(10, kBarH / 2, 5, stateColor);
  fontSmall(); 
  drawClipped(stateText, 20, kBarH / 2, 62, kColText, middle_left);

  // In direct mode this shows the access point's own address.
  const String host = !wifiOk     ? String("---")
                    : gDirectMode ? WiFi.softAPIP().toString()
                                  : WiFi.localIP().toString();
  fontSmall(); 
  drawClipped(host, 86, kBarH / 2, 84, kColLabel, middle_left);

  fontSmall(); 
  drawClipped(String("CPU ") + app.currentCpuValue, 176, kBarH / 2, 84, kColText, middle_left);

  // On the right RFID/SD and the battery symbol take turns. When the battery
  // blinks, the symbol blinks with it.
  const uint32_t nowMs = millis();
  if (batteryPhase(nowMs)) {
    if (!batteryDark(nowMs)) {
      if (batteryOnPower()) drawChargeBolt();
      drawBatterySymbol();
    }
  } else {
    fontSmall(); 
    drawClipped(app.rfidReady ? "RFID" : "-", 268, kBarH / 2, 26, app.rfidReady ? kColOk : kColLine, middle_left);
    fontSmall(); 
    drawClipped(app.sdReady ? "SD" : "-", 300, kBarH / 2, 20, app.sdReady ? kColOk : kColLine, middle_left);
  }

  app.lastStatusDrawMs = millis();
  app.barDirty = false;
}

void drawTitleBar(const char* title) {
  gDraw->fillRect(0, kTitleY, kScrW, kTitleH, kColBg);
  fontText(); 
  drawClipped(title, kScrW / 2, kTitleY + kTitleH / 2, kScrW - 20, kColLineHi, middle_center);
}

// ---- Bottom bar ---------------------------------------------------------
// Directions for the arrow symbols
constexpr int kArrowNone  = -1;
constexpr int kArrowUp    = 0;
constexpr int kArrowDown  = 1;
constexpr int kArrowLeft  = 2;
constexpr int kArrowRight = 3;

void drawArrow(int cx, int cy, int dir, uint16_t color) {
  constexpr int kW = 9;   // half width across the direction
  constexpr int kH = 8;   // half length towards the tip
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

// On the CoreS3 the bottom bar is no longer a button legend but a control
// surface of its own:
//
//   [ <- Back ]            hint text            [ ^ ][ v ]
//
// The back field is always on the left; the two paging fields only appear on
// list pages. The hit areas for them are in hitBack() and
// hitPageUp()/hitPageDown() and have to match these measurements.
constexpr int kBackW    = 78;                 // width of the back field
constexpr int kPageW    = 46;                 // width of each paging field
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
    drawClipped("Back", 30, cy, kBackW - 36, TFT_WHITE, middle_left);
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

// ---- Command tiles ------------------------------------------------------
// Tiles are centred per row so that a bottom row that is not full
// still sits centred underneath the one above.
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
  // The box has to sit entirely inside the logo band (20..127). If it reached
  // into the tiles, a strip would stay behind when the modal expires: the home
  // branch of render() only redraws the logo area.
  const int y = 50;

  gDraw->fillRoundRect(x, y, boxW, boxH, 10, rgb565(6, 10, 18));
  gDraw->drawRoundRect(x, y, boxW, boxH, 10, app.modalColor);
  gDraw->fillRoundRect(x + 6, y + 6, boxW - 12, 6, 3, app.modalColor);

  fontBig();
  const bool tooWide = gDraw->textWidth(app.modalText) > (boxW - 20);
  if (tooWide) { fontText(); } else { fontBig(); } 
  drawClipped(app.modalText, kScrW / 2, y + 44, boxW - 20, app.modalColor, middle_center);
}

// ---- List row -----------------------------------------------------------
void drawListRow(int row, const String& left, const String& right, bool selected) {
  const int y = kListY + row * kRowH;
  const uint16_t fill   = selected ? kColPanelHi : kColPanel;
  const uint16_t border = selected ? kColLineHi  : kColLine;

  // The row ends before the scrollbar so the two do not overlap.
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
// Start of the list window
//
// `gListStart` is the first visible row and therefore the single source of
// truth about which section is on screen right now.
//
// With buttons the window was derived from the selection and always centred
// on it. On a touchscreen that does not work: the user pushes the list with
// a finger without selecting anything - the window would otherwise jump
// straight back to the selection. `listWindowStart()` therefore only clamps
// to the valid range; the view is pulled along explicitly through
// `listEnsureVisible()` when the selection itself moves (paging fields of
// the bottom bar).
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

// Height of the thumb: proportional to the visible share, but never smaller
// than kThumbMinH - otherwise long lists would make it impossible to hit.
int scrollThumbHeight(int count) {
  if (count <= kListRows) return kScrollH;
  return std::max(kThumbMinH, (kScrollH * kListRows) / count);
}

void drawScrollbar(int start, int count) {
  if (count <= kListRows) return;             // everything fits, no bar needed

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

// Hit area of the scrollbar. It begins exactly where the list rows end -
// otherwise a tap on the right edge of a row would jump around in the list
// instead of selecting the entry.
bool hitScrollbar(int x, int y) {
  return x >= kListRight && y >= kScrollTop && y < kScrollTop + kScrollH;
}

// Derive the start of the window from a touch in the scrollbar. The thumb
// centres itself under the finger while doing so.
void scrollbarSeek(int y, int count) {
  if (count <= kListRows) return;
  const int thumbH   = scrollThumbHeight(count);
  const int span     = std::max(1, kScrollH - thumbH);
  const int maxStart = count - kListRows;
  int rel = y - kScrollTop - thumbH / 2;
  rel = std::max(0, std::min(rel, span));
  gListStart = std::max(0, std::min((rel * maxStart + span / 2) / span, maxStart));
}

// ---- CPU menu -----------------------------------------------------------
void drawCpuMenu() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("CPU SPEED");
  fontSmall(); 
  drawClipped(String("current: ") + app.currentCpuValue, kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20, kColOk, top_center);

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
  drawHintBar("tap a value", true, true);
}

// ---- Status page --------------------------------------------------------
void drawStatusScreen() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar((String("STATUS   v") + kFwVersion).c_str());

  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  auto line = [](int row, const char* label, const String& value, uint16_t color) {
    const int y = kListY + row * 20;
    fontText(); 
    drawClipped(label, 16, y, 120, kColLabel, top_left);
    fontText(); 
    drawClipped(value, kScrW - 16, y, 180, color, top_right);
  };

  if (gDirectMode && !app.portalActive) {
    // Direct mode: own access point instead of a WiFi connection
    line(0, "Direct", gDirectApUp ? gDirectSsid : String("starting..."),
         gDirectApUp ? kColOk : kColWarn);
    line(1, "IP",     gDirectApUp ? WiFi.softAPIP().toString() : String("---"), kColText);
    line(2, "Devices", String(static_cast<unsigned>(directStationCount())), kColText);
  } else {
    line(0, "WiFi",   wifiOk ? staSsid() : String("disconnected"), wifiOk ? kColOk : kColWarn);
    line(1, "IP",     wifiOk ? WiFi.localIP().toString() : String("---"), kColText);
    line(2, "RSSI",   wifiOk ? String(staRssi()) + " dBm" : String("---"), kColText);
  }
  line(3, "c64u",   targetHost(), kColText);
  line(4, "Target", app.connection.targetReachable ? "reachable" : "not reached",
       app.connection.targetReachable ? kColOk : kColWarn);
  line(5, "Auth",   app.connection.authOk ? "ok" : "not verified",
       app.connection.authOk ? kColOk : kColWarn);
  line(6, "CPU",    app.currentCpuValue, kColText);

  const String hw = String("SD ") + (app.sdReady ? "ok" : "-") +
                    "   RFID " + (app.rfidReady ? "ok" : "-") +
                    "   Heap " + String(ESP.getFreeHeap() / 1024) + "k" +
                    "   Batt " + batteryText();
  fontSmall(); 
  drawClipped(hw, 16, kListY + 7 * 20 - 2, kScrW - 32, kColLabel, top_left);
  fontSmall();
  drawClipped(app.connection.detail, 16, kListY + 7 * 20 + 8, kScrW - 32, kColInfo, top_left);

  drawHintBar("tap the area = connection test");
}

// ---- Settings -----------------------------------------------------------
String settingsValue(size_t index) {
  switch (index) {
    case kSetNfcWrite:      return app.rfidReady ? "card" : "no RFID";
    case kSetNfcInfo:       return app.rfidReady ? "read" : "no RFID";
    case kSetNfcDump:       return app.rfidReady ? "back up" : "no RFID";
    case kSetNfcRestore:    return app.rfidReady ? "write" : "no RFID";
    case kSetNfcRandom:     return app.rfidReady ? "folder" : "no RFID";
    case kSetNfcCmd:        return app.rfidReady ? "command" : String("no RFID");
    case kSetCardConfirm:   return cardConfirmLabel(app.settings.cardConfirmS);
    case kSetWifi:          return gDirectMode ? String("Direct")
                                 : WiFi.status() == WL_CONNECTED
                                       ? staSsid()
                                       : String(gWifiCount == 0 ? "set up" : "offline");
    case kSetAutoNfc:       return app.rfidReady ? autoNfcLabel(app.settings.autoNfc) : String("no RFID");
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
                                                           : String("LED off");
    case kSetDiskAction:    return diskActionLabel(app.settings.diskAction);
    case kSetDiskDrive:     return uploadDriveLabel(app.settings.uploadDrive);
    case kSetJoystick:      return app.joyValue.isEmpty() ? String("?")
                                                          : joyLabelFromToken(app.joyValue);
    case kSetBeep:          return app.settings.beepEnabled ? "On" : "Off";
    case kSetFactoryReset:  return "Now";
  }
  return "";
}

// ---------------------------------------------------------------------------
// WiFi setup
// ---------------------------------------------------------------------------
String wifiMenuValue(size_t index) {
  switch (index) {
    case kWifiDirect:       return gDirectMode ? "on" : "off";
    case kWifiDirectNet:    return gDirectNet;
    case kWifiScanNow:
      return app.wifiScanCount == 0 ? String("scan")
                                    : String(static_cast<unsigned>(app.wifiScanCount));
    case kWifiFromSd:       return app.sdReady ? "wifi.txt" : "no SD";
    case kWifiPortal:       return app.portalActive ? "on" : "off";
    case kWifiConnectSaved: return String(static_cast<unsigned>(gWifiCount));
    case kWifiToCard:       return gWifiCount == 0 ? "empty" : "write";
    case kWifiToSd:         return !app.sdReady  ? "no SD"
                                 : gWifiCount == 0 ? "empty"
                                                   : "wifi.txt";
    case kWifiDeleteOne:    return gWifiCount == 0 ? "empty" : "choose";
    case kWifiDeleteAll:    return "Reset";
  }
  return "";
}

void drawWifiMenu() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("WIFI");

  fontSmall();
  const bool online = WiFi.status() == WL_CONNECTED;
  if (gDirectMode) {
    drawClipped(String("Direct: ") + gDirectSsid, kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20,
                gDirectApUp ? kColInfo : kColWarn, top_center);
  } else {
    drawClipped(online ? staSsid() : String(gWifiCount == 0 ? "no network stored"
                                                              : "not connected"),
                kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20, online ? kColOk : kColWarn,
                top_center);
  }

  const int count    = static_cast<int>(kWifiMenuCount);
  const int selected = std::max(0, std::min(app.wifiMenuIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    drawListRow(row, kWifiMenuItems[index], wifiMenuValue(index), index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("tap an entry", true, true);
}

void drawWifiScan() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("CHOOSE NETWORK");

  const int count = static_cast<int>(app.wifiScanCount);
  if (count == 0) {
    fontText();
    drawClipped("no network found", kScrW / 2, kListY + 40, kScrW - 20, kColWarn, middle_center);
    drawHintBar("");
    return;
  }

  const int selected = std::max(0, std::min(app.wifiScanIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    String    info  = String(static_cast<int>(gWifiScan[index].rssi)) + " dBm";
    if (wifiProfileIndex(gWifiScan[index].ssid) >= 0) info = "known";
    else if (gWifiScan[index].open)                   info = "open";
    drawListRow(row, gWifiScan[index].ssid, info, index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("tap an entry", true, true);
}

void drawWifiSaved() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar(app.wifiSavedDelete ? "DELETE NETWORK"
                                   : (app.wifiSavedToCard ? "TO NFC CARD" : "SAVED"));

  const int count = static_cast<int>(gWifiCount);
  if (count == 0) {
    fontText();
    drawClipped("no network stored yet", kScrW / 2, kListY + 40, kScrW - 20,
                kColWarn, middle_center);
    drawHintBar("");
    return;
  }

  const int selected = std::max(0, std::min(app.wifiSavedIndex, count - 1));
  const int start    = listWindowStart(selected, count);
  const bool online  = WiFi.status() == WL_CONNECTED;

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    const bool active = online && staSsid() == gWifiProfiles[index].ssid;
    drawListRow(row, gWifiProfiles[index].ssid,
                active ? "active" : (gWifiProfiles[index].pass.isEmpty() ? "open" : ""),
                index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar(app.wifiSavedDelete ? "tap = delete"
                                  : (app.wifiSavedToCard ? "tap = to card"
                                                         : "tap = connect"),
              true, true);
}

void drawWifiCard() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("WIFI PASSWORD");

  gDraw->fillRoundRect(24, kListY + 8, kScrW - 48, 108, 10, kColPanel);
  gDraw->drawRoundRect(24, kListY + 8, kScrW - 48, 108, 10, kColInfo);

  fontSmall();
  drawClipped("Network:", kScrW / 2, kListY + 26, kScrW - 70, kColLabel, middle_center);
  fontText();
  drawClipped(app.wifiPendingSsid.isEmpty() ? String("from the card") : app.wifiPendingSsid,
              kScrW / 2, kListY + 46, kScrW - 70, kColOk, middle_center);
  fontSmall();
  drawClipped(app.rfidReady ? "place the card with the password" : "RFID2 not found",
              kScrW / 2, kListY + 72, kScrW - 70, app.rfidReady ? kColText : kColErr,
              middle_center);
  drawClipped("text card: WIFI:S:..;P:..;;", kScrW / 2, kListY + 92, kScrW - 70,
              kColLabel, middle_center);

  drawClipped(app.wifiHint, kScrW / 2, kListY + 130, kScrW - 20, kColInfo, middle_center);
  drawHintBar("");
}

void drawWifiPortal() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("SETUP PORTAL");

  fontSmall();
  drawClipped("Connect to this network:", kScrW / 2, kListY + 6, kScrW - 20,
              kColLabel, middle_center);
  fontText();
  drawClipped(kPortalSsid, kScrW / 2, kListY + 26, kScrW - 20, kColOk, middle_center);

  fontSmall();
  drawClipped("Password:", kScrW / 2, kListY + 50, kScrW - 20, kColLabel, middle_center);
  fontText();
  drawClipped(kPortalPass, kScrW / 2, kListY + 70, kScrW - 20, kColOk, middle_center);

  fontSmall();
  drawClipped("then open in a browser:", kScrW / 2, kListY + 94, kScrW - 20,
              kColLabel, middle_center);
  fontText();
  drawClipped(app.portalActive ? WiFi.softAPIP().toString() : String("---"),
              kScrW / 2, kListY + 114, kScrW - 20, kColInfo, middle_center);

  const uint32_t leftMs = (millis() - app.portalTouchedMs >= kPortalIdleMs)
                              ? 0
                              : (kPortalIdleMs - (millis() - app.portalTouchedMs));
  fontSmall();
  drawClipped(String("ends in ") + String(leftMs / 1000) + "s", kScrW / 2, kListY + 138,
              kScrW - 20, kColLabel, middle_center);
  drawHintBar("Back ends the portal");
}

// Direct mode: everything to enter on the c64u at a glance.
void drawWifiDirect() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("DIRECT MODE");

  fontSmall();
  drawClipped("Enter as WiFi on the c64u:", kScrW / 2, kListY + 6, kScrW - 20,
              kColLabel, middle_center);
  fontText();
  drawClipped(gDirectSsid, kScrW / 2, kListY + 26, kScrW - 20, kColOk, middle_center);

  fontSmall();
  drawClipped("Password:", kScrW / 2, kListY + 50, kScrW - 20, kColLabel, middle_center);
  fontText();
  drawClipped(gDirectPass, kScrW / 2, kListY + 70, kScrW - 20, kColOk, middle_center);

  fontText();
  drawClipped(String("c64u: ") + targetHost(), kScrW / 2, kListY + 96, kScrW - 20,
              kColInfo, middle_center);

  const size_t stations = directStationCount();
  String   state;
  uint16_t color = kColWarn;
  if (!gDirectApUp)                        state = "access point starting...";
  else if (app.connection.authOk)          { state = "c64u connected"; color = kColOk; }
  else if (app.connection.targetReachable) state = "c64u: check password";
  else if (stations == 0)                  state = "waiting for the c64u";
  else                                     state = String(static_cast<unsigned>(stations)) +
                                                   " device(s), looking for c64u";
  fontSmall();
  drawClipped(state, kScrW / 2, kListY + 120, kScrW - 20, color, middle_center);
  drawClipped(String("CoreS3: ") + (gDirectApUp ? WiFi.softAPIP().toString() : String("---")),
              kScrW / 2, kListY + 138, kScrW - 20, kColLabel, middle_center);
  drawHintBar("Off: WiFi > Direct mode");
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
  drawHintBar("tap = change", true, true);
}

// ---- SD browser ---------------------------------------------------------
void drawSdBrowser() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar(app.sdPickMode == 3 ? "RANDOM FOLDER"
               : app.sdPickMode == 1 ? "WRITE TO CARD"
               : app.sdPickMode == 2 ? "CHOOSE DUMP" : "SD CARD");
  fontSmall();
  drawClipped("long tap = ..", 8, kTitleY + kTitleH / 2, 90, kColLabel, middle_left);
  fontSmall(); 
  drawClipped(app.sdPath, kScrW / 2, kTitleY + kTitleH + 2, kScrW - 20, kColInfo, top_center);

  const int count = sdBrowserCount();

  if (count == 0) {
    fontText(); 
    drawClipped("no matching files", kScrW / 2, kListY + 40, kScrW - 20, kColWarn, middle_center);
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
        left  = "[this folder]";
        right = "random";
      } else {
        const DirEntryInfo& e = app.sdEntries[slot];
        left  = e.name;
        right = e.isDir ? "dir" : lowerExt(e.name);
      }
      drawListRow(row, left, right, index == selected);
    }
    drawScrollbar(start, count);
  }
  drawHintBar(app.sdPickMode ? "tap = choose" : "long tap = folder up", true, true);
}

// ---- Select a command for a card ----------------------------------------
void drawCmdPick() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);
  drawTitleBar("CARD COMMAND");
  fontSmall();
  drawClipped("will be written to the card", kScrW / 2, kTitleY + kTitleH + 2,
              kScrW - 20, kColInfo, top_center);

  const int count    = static_cast<int>(cmdListCount());
  const int selected = std::max(0, std::min(app.cmdIndex, count - 1));
  const int start    = listWindowStart(selected, count);

  for (int row = 0; row < kListRows && start + row < count; ++row) {
    const int index = start + row;
    const CardCommand c = cmdListAt(static_cast<size_t>(index));
    drawListRow(row, cardCommandLabel(c),
                cmdListTag(static_cast<size_t>(index)), index == selected);
  }
  drawScrollbar(start, count);
  drawHintBar("tap an entry", true, true);
}

// ---- RFID pages ---------------------------------------------------------
void drawRfidScreen() {
  const bool writeMode = (app.screen == ScreenMode::RfidWrite ||
                          app.screen == ScreenMode::RfidRestore);
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);

  const char* title = "READ CARD & STARTEN";
  if (app.screen == ScreenMode::RfidWrite)   title = "WRITE CARD";
  if (app.screen == ScreenMode::RfidDump)    title = "KARTE AUF SD SICHERN";
  if (app.screen == ScreenMode::RfidRestore) title = "RESTORE DUMPSCHREIBEN";
  drawTitleBar(title);

  const int boxY = kListY + 6;
  gDraw->fillRoundRect(16, boxY, kScrW - 32, 96, 10, kColPanel);
  gDraw->drawRoundRect(16, boxY, kScrW - 32, 96, 10, writeMode ? kColWarn : kColInfo);

  fontText(); 
  drawClipped(app.rfidReady ? "place the card on the reader" : "RFID2 not found", kScrW / 2, boxY + 26, kScrW - 50, app.rfidReady ? kColText : kColErr, middle_center);

  if (app.rfidReady) {
    fontSmall();
    drawClipped("MIFARE Classic  oder  NTAG213/215/216", kScrW / 2, boxY + 42,
                kScrW - 50, kColLabel, middle_center);
  }

  if (writeMode) {
    const bool restore = (app.screen == ScreenMode::RfidRestore);
    const bool command = !restore && !app.pendingCardText.isEmpty();
    fontSmall();
    drawClipped(restore ? "Dump:" : (command ? "Command:" : "Pfad:"), kScrW / 2, boxY + 52,
                kScrW - 50, kColLabel, middle_center);
    fontSmall();
    drawClipped(restore ? app.pendingDump : (command ? app.pendingCardText : app.pendingPath),
                kScrW / 2, boxY + 70, kScrW - 50, kColOk, middle_center);
  } else {
    fontSmall(); 
    drawClipped(app.lastCardPath.isEmpty() ? String("no card read yet") : app.lastCardPath, kScrW / 2, boxY + 62, kScrW - 50, kColLabel, middle_center);
  }

  fontText(); 
  drawClipped(app.rfidHint, kScrW / 2, boxY + 112, kScrW - 30, kColInfo, middle_center);

  if (app.cardPowerOffPending) {
    const int32_t left = static_cast<int32_t>(app.cardPowerOffUntilMs - millis());
    drawHintBar((String("POWER OFF? ") + String(std::max<int32_t>(0, left) / 1000 + 1) +
                 "s  tap the area = YES").c_str());
  } else {
    drawHintBar("");
  }
}

// ---- Card info ----------------------------------------------------------
void drawRfidInfoScreen() {
  gDraw->fillRect(0, kTitleY, kScrW, kHintY - kTitleY, kColBg);

  const bool page2 = (app.infoPage == 1);
  drawTitleBar(page2 ? "CARD INFO   ROHDATEN" : "CARD INFO");

  const String* lines = page2 ? app.infoLines2 : app.infoLines;
  const size_t  count = page2 ? app.infoCount2 : app.infoCount;

  if (app.infoCount == 0) {
    fontText();
    drawClipped(app.rfidReady ? "place the card on the reader" : "RFID2 not found",
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
    // Font0 is a fixed-width font - that way the line length can be
    // computed directly, without measuring every substring.
    const int charW   = std::max(1, gDraw->textWidth("W"));
    const size_t perRow = std::max<size_t>(8, static_cast<size_t>(valueW / charW));

    int y = kListY - 4;
    if (count == 0) {
      fontSmall();
      drawClipped("no raw data available", kValueX, y, valueW, kColLabel, top_left);
    }
    for (size_t i = 0; i < count && y <= kHintY - 12; ++i) {
      const String& line = lines[i];
      // The first ten characters are the label, after that comes the value
      fontSmall();
      drawClipped(line.substring(0, 10), 10, y, 66, kColLabel, top_left);

      String value = line.substring(10);
      if (value.isEmpty()) { y += kRowStep; continue; }

      // Wrap long values, preferably at a slash
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
    drawHintBar(page2 ? "page 2/2 - tap to turn" : "page 1/2 - tap to turn");
  }
}

// ---- Progress -----------------------------------------------------------
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
    drawClipped("please wait...", kScrW / 2, barY + 34, kScrW - 40, kColLabel, middle_center);
  }
  drawHintBar("UPLOAD RUNNING", false, false);
}

// ---- Overall output -----------------------------------------------------
void drawFullScreenFor(ScreenMode screen) {
  switch (screen) {
    case ScreenMode::Home:
      gDraw->fillRect(0, kTileY0 - 2, kScrW, kScrH - (kTileY0 - 2), kColBg);
      drawTiles();
      drawHintBar("tap a tile", false, false);
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
    case ScreenMode::WifiDirect:drawWifiDirect(); break;
    case ScreenMode::Busy:      drawBusyScreen(); break;
  }
}

// The offscreen buffer keeps its contents between two frames. The partial
// updates (status bar only, tiles only, effect area only) therefore work
// exactly as they do when drawing directly - at the end the finished image
// is pushed to the display in one go.
void pushFrame() {
  if (gUseCanvas) canvas.pushSprite(0, 0);
}

void render(uint32_t now) {
  const bool modalNow = modalVisible(now);

  // One pushSprite() costs 150 kB across the same SPI bus the SD card hangs
  // on. The image is therefore only pushed when something was actually drawn
  // in this pass - that keeps the dirty logic effective even with an
  // offscreen buffer.
  bool drawn = false;

  // While the battery bar blinks the bar has to be redrawn more often.
  const uint32_t barIntervalMs = batteryLow() ? kBattBlinkMs : kStatusRefreshMs;
  if (app.barDirty || now - app.lastStatusDrawMs >= barIntervalMs) {
    drawStatusBar();
    drawn = true;
  }

  if (app.screen == ScreenMode::Home) {
    const HomeMode mode = currentHomeMode();
    const bool animated = (mode != HomeMode::Static);

    if (app.screenDirty) {
      gDraw->fillRect(0, kTileY0 - 2, kScrW, kScrH - (kTileY0 - 2), kColBg);
      drawTiles();
      drawHintBar("tap a tile", false, false);
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
      app.screenDirty = true;      // Clean the overlay away again
    }
  }
  app.modalVisibleLast = modalNow;
  if (drawn) pushFrame();
}

void setScreen(ScreenMode next, uint32_t now) {
  if (app.screen == next) return;
  // As soon as the user navigates on their own, the read page no longer counts
  // as automatically opened.
  if (next != ScreenMode::RfidRun) app.autoRfidActive = false;
  app.screen      = next;
  gListStart      = 0;          // a new list starts at the top again
  app.screenDirty = true;
  app.home.dirty  = true;
  app.barDirty    = true;
  if (next == ScreenMode::Home) resetHomeAnimation(now);
  gDraw->fillRect(0, kTitleY, kScrW, kScrH - kTitleY, kColBg);
}

// ===========================================================================
//  WiFi setup: actions of the submenu
// ===========================================================================
void wifiConnectProfile(size_t index, uint32_t now) {
  if (index >= gWifiCount) return;
  gWifiTry              = index;
  app.lastWiFiAttemptMs = 0;
  setDirectMode(false, now, false);           // explicitly into WiFi: direct mode ends
  if (app.portalActive) stopPortal(now);      // stops and connects by itself
  else                  beginWiFi(now);
  setModal("CONNECTING...", kColInfo, now, 1800);
  setScreen(ScreenMode::WifiMenu, now);
}

// The scan blocks for a few seconds. So that the Core does not look frozen,
// the notice is drawn once more beforehand.
void wifiStartScan(uint32_t now) {
  setModal("SCANNING", kColInfo, now, 8000);
  render(now);
  wifiRunScan();
  app.modalText = "";

  if (app.wifiScanCount == 0) {
    setModal("NOTHING FOUND", kColWarn, now, 1800);
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

  // Already stored? Then a connection attempt is enough.
  const int known = wifiProfileIndex(entry.ssid);
  if (known >= 0) {
    wifiConnectProfile(static_cast<size_t>(known), now);
    return;
  }

  // An open network does not need a password.
  if (entry.open) {
    wifiAddProfile(entry.ssid, "");
    app.lastWiFiAttemptMs = 0;
    setDirectMode(false, now, false);
    beginWiFi(now);
    setModal("SAVED", kColOk, now, 1600);
    setScreen(ScreenMode::WifiMenu, now);
    return;
  }

  app.wifiPendingSsid = entry.ssid;
  app.wifiHint = app.rfidReady ? "place a card..." : "no RFID2 - use the portal";
  setScreen(ScreenMode::WifiCard, now);
}

void wifiMenuSelect(uint32_t now) {
  const int index = std::max(0, std::min(app.wifiMenuIndex, static_cast<int>(kWifiMenuCount) - 1));
  beep(2200, 25);

  switch (index) {
    case kWifiDirect:
      // Toggle: on shows the credentials for the c64u right away, off
      // returns to the stored WiFi.
      if (!gDirectMode) {
        if (app.portalActive) stopPortal(now);
        setDirectMode(true, now);
        setModal("DIRECT MODE ON", kColOk, now, 1600);
        setScreen(ScreenMode::WifiDirect, now);
      } else {
        setDirectMode(false, now);
        setModal("DIRECT MODE OFF", kColWarn, now, 1800);
      }
      break;

    case kWifiDirectNet:
      // Switch between the two usual ranges. A different range
      // can be set via direct_net in /wifi.txt.
      setDirectNet(gDirectNet == kDirectNetDef ? String(kDirectNetAlt) : String(kDirectNetDef), now);
      setModal(String("NET ") + gDirectNet + ".x", kColInfo, now, 1800);
      break;

    case kWifiScanNow:
      wifiStartScan(now);
      break;

    case kWifiFromSd: {
      String error;
      const bool   wasDirect = gDirectMode;
      const size_t added     = loadWifiFromSd(&error);
      if (added > 0 || error.isEmpty()) {
        // The file may have switched direct mode on or off.
        if (wasDirect && !gDirectMode) { stopDirectAp(); WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); }
        resetDirectState();
        app.lastWiFiAttemptMs = 0;
        gWifiTry = 0;
        beginWiFi(now);
        setModal(added > 0 ? String(static_cast<unsigned>(added)) + " NETWORK(S) LOADED"
                           : String("DIRECT MODE LOADED"), kColOk, now, 2000);
      } else {
        beep(500, 120);
        setModal(error.isEmpty() ? String("SD ERROR") : error, kColErr, now, 2400);
      }
      break;
    }

    case kWifiPortal:
      // Scan once before the portal so that the web interface can offer a
      // network list.
      setModal("PORTAL STARTING", kColInfo, now, 8000);
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
      // Write a stored network (for example one just entered through the
      // portal) to an NFC card - that way another device can be set up
      // without any typing.
      if (gWifiCount == 0) { setModal("NOTHING STORED", kColWarn, now, 1600); break; }
      if (!app.rfidReady)  { setModal("NO RFID2", kColErr, now, 1800); break; }
      app.wifiSavedDelete = false;
      app.wifiSavedToCard = true;
      app.wifiSavedIndex  = 0;
      setScreen(ScreenMode::WifiSaved, now);
      break;

    case kWifiToSd: {
      // Write all stored networks to the SD card as /wifi.txt - the
      // counterpart to "Load from SD" and thus a simple way to carry the
      // credentials over to a second device.
      if (gWifiCount == 0) { setModal("NOTHING STORED", kColWarn, now, 1600); break; }
      String error;
      const size_t written = saveWifiToSd(&error);
      if (written > 0) {
        setModal(String(static_cast<unsigned>(written)) + " NETWORK(S) ON SD", kColOk, now, 2000);
      } else {
        beep(500, 120);
        setModal(error.isEmpty() ? String("SD ERROR") : error, kColErr, now, 2400);
      }
      break;
    }

    case kWifiDeleteOne:
      if (gWifiCount == 0) { setModal("NOTHING STORED", kColWarn, now, 1600); break; }
      app.wifiSavedDelete = true;
      app.wifiSavedToCard = false;
      app.wifiSavedIndex  = 0;
      setScreen(ScreenMode::WifiSaved, now);
      break;

    case kWifiDeleteAll:
      wifiClearProfiles();
      WiFi.disconnect(false, true);
      setModal("ALL DELETED", kColWarn, now, 1800);
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
    setModal("DELETED: " + ssid, kColWarn, now, 1400);
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
//  Settings actions
// ===========================================================================
void activateSetting(uint32_t now) {
  // The WiFi entry is an action, but needs no card reader and is therefore
  // handled up front.
  if (app.settingsIndex == kSetWifi) {
    beep(2200, 25);
    app.wifiMenuIndex   = 0;
    app.wifiSavedDelete = false;
    app.wifiSavedToCard = false;
    setScreen(ScreenMode::WifiMenu, now);
    return;
  }

  // The remaining actions at the head of the list all need the card reader.
  if (app.settingsIndex <= kSetLastAction) {
    if (!app.rfidReady) {
      setModal("KEIN RFID2", kColErr, now, 1800);
      return;
    }
    beep(2200, 25);
    switch (app.settingsIndex) {
      case kSetNfcWrite:
        app.pendingCardText = "";        // NFC write
        openSdBrowser(1, now);
        break;
      case kSetNfcInfo:
        app.infoCount = 0;               // NFC info
        app.infoPage  = 0;
        app.infoUid   = "";
        app.rfidHint  = "waiting for a card";
        setScreen(ScreenMode::RfidInfo, now);
        break;
      case kSetNfcDump:
        app.rfidHint = "place a card to back it up";
        setScreen(ScreenMode::RfidDump, now);
        break;
      case kSetNfcRestore:
        app.pendingCardText = "";
        openSdBrowser(2, now);           // NFC restore: select dump
        break;
      case kSetNfcCmd:                   // NFC cmd: select command
        // The CPU speeds come from the c64u - load them once so that the list
        // offers the values that are actually possible.
        if (!app.cpuPathKnown) refreshCpuValue();
        if (!app.joyPathKnown) resolveJoyPath();
        app.cmdIndex = 0;
        setScreen(ScreenMode::CmdPick, now);
        break;
      case kSetNfcRandom:
        // NFC-Random: choose a directory
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
      // Make the toggle visible at once, without waiting for the next tick.
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
    case kSetJoystick:
      cycleJoystickValue(now);
      break;
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
//  Trigger tiles
// ===========================================================================
void openSdBrowser(uint8_t forCard, uint32_t now) {
  app.sdPickMode = forCard;
  if (!app.sdReady && !initSd()) {
    setModal("NO SD CARD", kColErr, now, 2000);
    return;
  }
  // Dumps always live in their own folder - start right there
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
  beep(1900, 18);        // acknowledge the press at once, the result follows
  switch (app.tileIndex) {
    case kTileReset:     performReset(now);      break;
    case kTileReboot:    performHardReset(now);  break;
    case kTileUltiMenu:  performMenuButton(now); break;
    case kTilePowerOff:
      requestPowerOff(now);          // Tile always asks
      app.screenDirty = true;
      break;

    case kTileCpu:
      clearPendingPowerOff();
      refreshCpuValue();
      app.cpuIndex = cpuIndexFromValue(app.currentCpuValue);
      setScreen(ScreenMode::CpuMenu, now);
      break;

    case kTileJoySwap:
      toggleJoystickSwap(now);
      break;

    case kTileRfidRun:
      clearPendingPowerOff();
      if (!app.rfidReady) { setModal("KEIN RFID2", kColErr, now, 1800); break; }
      app.rfidHint     = "waiting for a card";
      // Opened by hand: the back field leads to the home screen.
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
//  SD browser selection
// ===========================================================================
void sdBrowserSelect(uint32_t now) {
  const int count = sdBrowserCount();
  if (count == 0) return;

  const int index = std::max(0, std::min(app.sdIndex, count - 1));
  const int slot  = sdBrowserSlot(index);

  if (slot == -1) {
    const String up = parentPath(app.sdPath);
    readDirectory(up);
    gListStart      = 0;          // a new directory starts at the top
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
    app.rfidHint        = "place a card";
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
    app.rfidHint    = "place a card to restore it";
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
    app.rfidHint    = "place a card zum Schreiben";
    setScreen(ScreenMode::RfidWrite, now);
  } else {
    startFileOnC64(full, now);
  }
}

// ===========================================================================
//  RFID polling
// ===========================================================================
// ---------------------------------------------------------------------------
// Execute a card command
//
// PowerOff with confirmation runs via cardPowerOffPending: the card is
// removed and placed again within the time window. A press on
// B confirms as well, cancelling happens by simply waiting.
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

    case CardCmd::DeviceOff:
      // A card still lying on the reader at power-up would switch the device
      // off again right away - so ignore it during the first seconds.
      if (millis() < kDeviceOffBootGuardMs) {
        setModal("REMOVE CARD", kColWarn, now, 1500);
        return;
      }
      rfidRelease();
      shutdownDevice();
      return;

    case CardCmd::PowerOff: {
      const uint8_t sec = cardPowerOffSeconds(cmd);

      // Placing the same card a second time within the window confirms.
      if (app.cardPowerOffPending && uid == app.cardPowerOffUid &&
          static_cast<int32_t>(now - app.cardPowerOffUntilMs) < 0) {
        app.cardPowerOffPending = false;
        beep(1800, 60);
        performPowerOff(now);
        return;
      }

      if (sec == 0) {                       // "CMD:POWEROFF=0" - without a prompt
        beep(1800, 60);
        performPowerOff(now);
        return;
      }

      app.cardPowerOffPending = true;
      app.cardPowerOffUid     = uid;
      app.cardPowerOffUntilMs = now + sec * 1000u;
      beep(900, 60);
      app.rfidHint = "place the card once more";
      setModal("POWER OFF? CARD AGAIN!", kColWarn, now, sec * 1000u);
      return;
    }

    case CardCmd::JoySwap:
      beep(2400, 40);
      if (cmd.hasArg && !cmd.arg.isEmpty()) applyJoystickValue(cmd.arg, now);
      else                                  toggleJoystickSwap(now);
      return;

    case CardCmd::Direct: {
      // Cards read repeatedly (card left lying) must not upset anything:
      // "on with network X" and "off" therefore do nothing if the state
      // is already right. Only the card without an argument toggles.
      String a = cmd.arg;
      a.trim();
      String upper = a;
      upper.toUpperCase();
      const bool wantOff = cmd.hasArg && upper == "OFF";
      const bool toggle  = !cmd.hasArg || a.isEmpty();
      if (!toggle && !wantOff && !directNetValid(a)) {
      beep(500, 140);
        setModal("CARD INVALID", kColErr, now, 2000);
        return;
      }
      if (wantOff || (toggle && gDirectMode)) {
        if (!gDirectMode) { setModal("DIRECT MODE IS OFF", kColInfo, now, 1600); return; }
      beep(2600, 50);
        setDirectMode(false, now);
        setModal("DIRECT MODE OFF", kColWarn, now, 1800);
        return;
      }
      if (!toggle) {
        if (gDirectMode && gDirectNet == a) {
          setModal("DIRECT MODE RUNNING", kColOk, now, 1600);
          return;
        }
        setDirectNet(a, now);           // restarts a running AP right away
      }
      if (app.portalActive) stopPortal(now);
      beep(2600, 50);
      setDirectMode(true, now);
      setModal("DIRECT " + gDirectNet + ".x", kColOk, now, 2000);
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
      setModal("UNKNOWN COMMAND", kColErr, now, 2000);
      return;
  }
}

// Handles a card that has already been selected, according to the current screen.
void processCard(uint32_t now) {
  const CardKind kind = cardKind();
  if (kind == CardKind::None) {
    rfidRelease();
    app.rfidHint = "card type not supported";
    app.screenDirty = true;
    beep(500, 120);
    ledFlash(ledRgb(255, 0, 0));
    return;
  }

  const String uid = cardUidString();

  if (app.screen == ScreenMode::RfidInfo) {
    // Read once and leave it. Otherwise the display would be rebuilt on every
    // poll and overwritten with half the data when the card is removed.
    // A different card of course triggers a fresh read.
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
    setModal(ok ? "BACKED UP TO SD" : "SICHERN FEHLGESCHLAGEN",
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
    setModal(ok ? "CARD WRITTEN" : "WRITE ERROR",
             ok ? kColOk : kColErr, now, 2200);
    return;
  }

  if (app.screen == ScreenMode::RfidWrite) {
    String error;
    // Command cards bring their text ready-made, otherwise the file path is
    // translated into the card format.
    const String text = app.pendingCardText.isEmpty() ? pathToCardText(app.pendingPath)
                                                      : app.pendingCardText;
    const bool ok = writeCardText(kind, text, &error);
    rfidRelease();
    beep(ok ? 2800 : 500, ok ? 60 : 160);
    ledFlash(ok ? ledRgb(0, 255, 80) : ledRgb(255, 0, 0));
    app.rfidHint = ok ? (String(cardKindLabel(kind)) + "  " + uid) : error;
    app.screenDirty = true;
    if (ok) setModal("CARD OK", kColOk, now, 1800);
    else    setModal("WRITE ERROR", kColErr, now, 2200);
    return;
  }

  // ---- Read and start ----
  const CardContent content = readCardContent(kind);
  rfidRelease();

  if (!content.ok) {
    beep(500, 140);
    ledFlash(ledRgb(255, 0, 0));
    app.rfidHint = content.error;
    app.wifiHint = content.error;
    app.screenDirty = true;
    setModal("CARD EMPTY?", kColWarn, now, 2000);
    return;
  }

  // ---- WiFi password card on the setup page ----
  if (app.screen == ScreenMode::WifiCard) {
    String ssid = app.wifiPendingSsid;
    String pass = trimCopy(content.text);
    // Cards in the WiFi scheme bring the SSID along themselves. Anything else
    // counts as a plain password for the network chosen beforehand.
    parseWifiText(content.text, &ssid, &pass);

    if (ssid.isEmpty()) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.wifiHint = "card without SSID";
      app.screenDirty = true;
      setModal("NO NETWORK", kColErr, now, 2000);
      return;
    }
    // Program or command cards are certainly a mistake here.
    CardCommand strayCommand;
    if (!textLooksLikeWifi(content.text) &&
        (pass.startsWith("/") || parseCardCommand(content.text, &strayCommand))) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.wifiHint = "that is not a WiFi card";
      app.screenDirty = true;
      setModal("WRONG CARD", kColErr, now, 2200);
      return;
    }
    if (!wifiAddProfile(ssid, pass)) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.wifiHint = "SSID or password too long";
      app.screenDirty = true;
      setModal("CARD INVALID", kColErr, now, 2200);
      return;
    }

    beep(2800, 60);
    app.wifiPendingSsid   = "";
    app.wifiHint          = ssid;
    app.lastWiFiAttemptMs = 0;
    gWifiTry              = 0;
    setDirectMode(false, now, false);     // new network chosen: direct mode ends
    beginWiFi(now);
    setModal("WIFI SAVED", kColOk, now, 2000);
    setScreen(ScreenMode::WifiMenu, now);
    return;
  }

  // ---- WiFi card outside the setup ----
  //
  // The card is not a file path. Instead of merely pointing that out, the
  // network is taken over right away and a connection is made: placing the
  // the detour through SETUP > WiFi is not needed.
  if (textLooksLikeWifi(content.text)) {
    String ssid;
    String pass;
    if (!parseWifiText(content.text, &ssid, &pass) || ssid.isEmpty()) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = "WiFi card without SSID";
      app.screenDirty = true;
      setModal("NO NETWORK", kColErr, now, 2200);
      return;
    }

    // Card with our own direct network: this device is the
    // access point, there is nothing to connect to.
    if (gDirectMode && ssid == gDirectSsid) {
      beep(2400, 40);
      app.rfidHint = ssid;
      setModal("DIRECT MODE RUNNING", kColOk, now, 1800);
      return;
    }

    // If the connection is already up there is nothing to do. That also
    // catches the case where the card stays on the reader and the background
    // poll keeps spotting it.
    if (WiFi.status() == WL_CONNECTED && staSsid() == ssid) {
      beep(2400, 40);
      app.rfidHint = ssid;
      app.screenDirty = true;
      setModal("ALREADY CONNECTED", kColOk, now, 1800);
      return;
    }

    if (!wifiAddProfile(ssid, pass)) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = "SSID or password too long";
      app.screenDirty = true;
      setModal("CARD INVALID", kColErr, now, 2200);
      return;
    }

    // wifiAddProfile sorts the network to the front; from there this one
    // network is tried on purpose instead of walking the whole list.
    const int index = wifiProfileIndex(ssid);
    gWifiTry              = index >= 0 ? static_cast<size_t>(index) : 0;
    app.lastWiFiAttemptMs = 0;
    beep(2600, 50);

    app.rfidHint    = ssid;
    app.screenDirty = true;
    setModal("CONNECTING...", kColInfo, now, kWifiCardConnectMs + 1500);
    render(now);

    setDirectMode(false, now, false);         // WiFi card: direct mode ends
    if (app.portalActive) stopPortal(now);    // stops the AP and connects itself
    else                  beginWiFi(now);

    // Wait briefly for the result so that the feedback is worth something.
    // It waits no longer than kWifiCardConnectMs - the rest carries on through
    // the regular attempts in serviceWiFi.
    const uint32_t deadline = millis() + kWifiCardConnectMs;
    while (millis() < deadline && WiFi.status() != WL_CONNECTED) delay(100);

    const bool connected = WiFi.status() == WL_CONNECTED;
    app.modalText = "";
    beep(connected ? 2800 : 500, connected ? 60 : 140);
    app.rfidHint = connected ? (ssid + "  " + WiFi.localIP().toString())
                             : (ssid + " not reachable");
    app.screenDirty = true;
    setModal(connected ? "WIFI ACTIVE" : "NETWORK NOT THERE",
             connected ? kColOk : kColWarn, millis(), 2400);
    return;
  }

  Serial.printf("card read (%s): '%s'\n",
                content.isNdef ? "NDEF-Text" : "Altformat", content.text.c_str());

  // ---- Command card? Then neither SD nor file is needed ----
  CardCommand command;
  if (parseCardCommand(content.text, &command)) {
    runCardCommand(command, uid, now);
    return;
  }
  {
    // The card does carry the prefix, but no known keyword -
    // then it is certainly not a file path.
    String head = content.text.substring(0, 4);
    head.toUpperCase();
    if (head == kCardCmdPrefix) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = content.text;
      setModal("UNKNOWN COMMAND", kColErr, now, 2200);
      return;
    }
  }
  if (app.cardPowerOffPending) app.cardPowerOffPending = false;   // a different card aborts

  String path = cardTextToPath(content.text);

  // "?" as the file name or a directory: start a random file from it
  if (pathIsRandom(path) || pathIsDirectory(path)) {
    const String picked = resolveRandomFile(path);
    if (picked.isEmpty()) {
      beep(500, 140);
      ledFlash(ledRgb(255, 0, 0));
      app.rfidHint = "Verzeichnis leer: " + path;
      app.screenDirty = true;
      setModal("NOTHING FOUND", kColWarn, now, 2200);
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
// RFID sequence
//
// Two modes of operation:
//
//   1. On the RFID pages a full poll runs every 250 ms - the user is
//      waiting for the card there anyway.
//   2. On the main screen, depending on the "Auto NFC" setting, a fast
//      probe runs in the background. If a card is detected, the display
//      switches to read mode by itself and the stored program starts -
//      the RFID tile no longer has to be selected for that.
//
// Thanks to the shortened time window (see cardPresentQuick) the probe costs
// only a few milliseconds. At 0.7 s intervals the base load is therefore below
// one percent, no stutter of the effects is visible.
// ---------------------------------------------------------------------------
bool onRfidScreen() {
  return app.screen == ScreenMode::RfidRun   || app.screen == ScreenMode::RfidWrite ||
         app.screen == ScreenMode::RfidInfo  || app.screen == ScreenMode::RfidDump  ||
         app.screen == ScreenMode::RfidRestore || app.screen == ScreenMode::WifiCard;
}

// Pages on which the background poll is allowed to run.
//
// This used to be the home screen only - in the SD browser a card placed on
// the reader simply went unnoticed. Allowed now are all pages on which you are
// merely looking or paging. Excluded are the pages on which cards are being
// worked WITH: launching a program there would be the exact opposite of what
// the user is doing.
bool autoNfcScreen() {
  switch (app.screen) {
    case ScreenMode::Home:
    case ScreenMode::Status:
    case ScreenMode::CpuMenu:
      return true;
    case ScreenMode::SdBrowser:
      // Only while browsing normally. If a file is being picked for a card
      // (NFC-Write) or a dump chosen for restoring, a card placed on the
      // reader must not start anything.
      return app.sdPickMode == 0;
    default:
      return false;
  }
}

void serviceRfid(uint32_t now) {
  if (!app.rfidReady) return;

  // ---- 1. Regular polling on the RFID pages ----
  if (onRfidScreen()) {
    if (now - app.lastRfidPollMs < kRfidPollMs) return;
    app.lastRfidPollMs = now;
    if (!cardPresent()) return;
    processCard(now);
    rfidHoldCard();
    return;
  }

  // ---- 2. Background polling ----
  if (app.settings.autoNfc == AutoNfcMode::Off) return;
  if (!autoNfcScreen()) return;
  if (now - app.lastRfidPollMs < autoNfcIntervalMs(app.settings.autoNfc)) return;
  app.lastRfidPollMs = now;

  if (!cardPresentQuick()) return;

  // Card is present: switch to read mode, show the screen immediately and
  // trigger the same processing as on the RFID page.
  beep(2200, 30);
  app.rfidHint        = "card detected";
  app.autoRfidActive    = true;
  app.autoRfidFrom      = app.screen;
  app.autoRfidSdPath    = app.sdPath;
  app.autoRfidSdIndex   = app.sdIndex;
  app.autoRfidListStart = gListStart;
  setScreen(ScreenMode::RfidRun, now);
  render(millis());
  processCard(millis());
  rfidHoldCard();

  // After that the read page stays up for a while so that the next card
  // can be placed right away.
  app.autoRfidUntilMs = millis() + kAutoRfidHoldMs;
}

// ===========================================================================
//  Lists: counting, selection, movement
//
//  The three helpers bundle what every list page has in common. They sit
//  before moveSelection(), because they are needed there.
// ===========================================================================

// Pages with a scrollable list. Swiping and the scrollbar only make sense
// there.
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

// Number of entries in the list on the current screen
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

// Pointer to the selection index of the list on the current screen
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

// Move the selection by `delta` entries. Used by the paging fields of the
// bottom bar; the view is pulled along explicitly.
void moveSelection(int delta) {
  if (app.screen == ScreenMode::RfidInfo) {
    if (app.infoCount > 0) {
      app.infoPage    = app.infoPage ? 0 : 1;   // switch between the two pages
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
      app.rfidHint        = "place a card";
      setScreen(ScreenMode::RfidWrite, now);
      break;
    }
    case ScreenMode::RfidRun:
      // If a PowerOff prompt from the card is pending, B confirms it.
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
    case ScreenMode::WifiDirect:
      setScreen(ScreenMode::WifiMenu, now);
      break;

    case ScreenMode::Busy:      break;
  }
}

// Back to the place from which the background poll opened the read page.
//
// Two things have to be restored by hand here:
//   * setScreen() resets gListStart to 0 - the scroll position would otherwise
//     be gone after every card.
//   * A directory or random card makes resolveRandomFile() read a different
//     directory. app.sdPath and the file list then point somewhere else, and
//     the user would come back out in a foreign folder.
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
    case ScreenMode::WifiDirect:
      setScreen(ScreenMode::WifiMenu, now);     // direct mode keeps running
      break;
    case ScreenMode::RfidRun:
      // If the read page was opened by the background poll, Back leads to the
      // same place as the display time running out.
      restoreAutoRfidScreen(now);
      break;
    default:
      setScreen(ScreenMode::Home, now);
      break;
  }
}

// ===========================================================================
//  Touch operation
//
//  The CoreS3 no longer has buttons A/B/C, only POWER and RESET on the side.
//  It is therefore operated entirely through the capacitive touchscreen:
//
//    * tap a tile or a list row          -> select or run
//    * swipe vertically                  -> the list follows the finger 1:1
//    * drag the scrollbar on the right   -> quickly through long lists
//    * back field at the bottom left     -> one level back
//    * paging fields at the bottom right -> one row up or down
//    * long tap in the SD browser        -> one directory up
//
//  A tap only counts if the finger has not travelled further than
//  kTapSlopPx between touching down and lifting off - otherwise it was a push.
// ===========================================================================

// Screen row (0..kListRows-1) under the finger, otherwise -1
int listRowFromTouch(int y) {
  if (y < kListY || y >= kListY + kListRows * kRowH) return -1;
  return (y - kListY) / kRowH;
}

// Tile under the finger, otherwise -1
int tileFromTouch(int x, int y) {
  for (int i = 0; i < kTileCount; ++i) {
    int tx = 0, ty = 0;
    tileRect(i, &tx, &ty);
    if (x >= tx && x < tx + kTileW && y >= ty && y < ty + kTileH) return i;
  }
  return -1;
}

// Tap on a list row: set the selection first, then run it. Unlike on the
// M5Dial a single tap is enough - on a touchscreen that is the expected
// reaction.
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
  // ---- Bottom bar ---------------------------------------------------------
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
    // Everything else in the bar falls through into the page evaluation.
  }

  switch (app.screen) {
    case ScreenMode::Home: {
      const int tile = tileFromTouch(x, y);
      if (tile < 0) {
        // A tap next to the tiles cancels a running PowerOff prompt. Do not
        // draw here: the offscreen buffer is only pushed when render() drew
        // something in this pass. The dirty flag makes sure the tile is clean
        // again in the very next frame.
        if (app.pendingPowerOff) {
          clearPendingPowerOff();
          app.screenDirty = true;
        }
        return;
      }
      // The POWER tile is confirmed by a second tap. A tap on a DIFFERENT
      // tile cancels the prompt.
      if (app.tileIndex != tile) clearPendingPowerOff();
      app.tileIndex   = tile;
      app.screenDirty = true;      // outline the selection before the action runs
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
      // A long tap replaces the long "A" of the Core edition here: it moves
      // one directory up.
      if (longTap) { beep(1800, 20); handleBack(now); break; }
      const int row = listRowFromTouch(y);
      if (row >= 0) tapListRow(row, now);
      break;
    }

    case ScreenMode::Status:
      // A tap on the area triggers a connection test.
      beep(2000, 20);
      handleSelect(now);
      break;

    case ScreenMode::RfidInfo:
      // Tapping turns between the two info pages.
      if (app.infoCount > 0) { beep(1800, 20); moveSelection(+1); }
      break;

    case ScreenMode::RfidRun:
      // If a PowerOff prompt from a card is running, a tap confirms it.
      //
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
      // No cancelling by tap: an accidental contact would otherwise have
      // ended the access point in the middle of typing. It is only stopped
      // through the back field. The contact does hold the idle clock though.
      app.portalTouchedMs = now;
      break;

    case ScreenMode::WifiDirect:
      // A tap only goes back to the WiFi menu. Direct mode is switched off
      // there via "Direct mode" - an accidental touch should not throw the
      // c64u off the network.
      beep(2000, 20);
      handleBack(now);
      break;

    case ScreenMode::Busy:
      break;
  }
}

void handleTouch(uint32_t now) {
  static bool     down       = false;
  static int      startX     = 0;
  static int      startY     = 0;
  static int      startList  = 0;   // start of the window when touching down
  static uint32_t startMs    = 0;
  // Screen at touch-down. If it changes while the finger is still down (the
  // background poll opens the read page, an upload finishes), lifting off must
  // not arrive as a tap on the new page.
  static ScreenMode startScreen = ScreenMode::Home;
  static bool     dragging   = false;   // list is being pushed right now
  static bool     scrollDrag = false;   // scrollbar thumb is being dragged

  if (app.screen == ScreenMode::Busy) { down = false; return; }

  const auto& touch = M5.Touch.getDetail();
  const bool  pressed = touch.isPressed();
  const int   x = touch.x;
  const int   y = touch.y;

  // ---- Finger touched down ------------------------------------------------
  if (pressed && !down) {
    down       = true;
    dragging   = false;
    scrollDrag = false;
    startX     = x;
    startY     = y;
    startList  = gListStart;
    startMs    = now;
    startScreen = app.screen;
    // Once the user intervenes, a read page opened by the background poll no
    // longer falls back to the home screen by itself.
    app.autoRfidActive = false;

    // Touching down on the scrollbar jumps to that spot immediately and then
    // turns into dragging.
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

  // ---- Finger is down -----------------------------------------------------
  if (pressed && down) {
    if (scrollDrag) {
      scrollbarSeek(y, listCount());
      app.screenDirty = true;
      return;
    }
    if (!listScreen(app.screen)) return;

    // The list follows the finger. As soon as it does, it is clear that no
    // tap was meant - and that is exactly what was missing before: it only
    // reacted from a fixed swipe distance on, and a slow swipe ended up as a
    // selection instead.
    //
    // Two subtleties:
    //   * If the whole list fits on screen there is nothing to push. A wobble
    //     must not swallow the tap in that case.
    //   * The movement counts as a push exactly when the list has actually
    //     moved. A fixed threshold in pixels would be wrong here: if it sat
    //     below one row height, a dead zone would open up in between in
    //     which neither anything moves nor a tap arrives. The row count is
    //     rounded for that, so it starts already from half a row height
    //     (13 px) on.
    const int count = listCount();
    if (count <= kListRows) return;

    const int dy   = y - startY;
    const int rows = (dy + (dy >= 0 ? kRowH / 2 : -(kRowH / 2))) / kRowH;
    if (rows != 0) dragging = true;         // once pushed, always pushed

    if (dragging) {
      const int maxStart = count - kListRows;
      // Swiping downwards reveals entries further up.
      int want = startList - rows;
      want = std::max(0, std::min(want, maxStart));
      if (want != gListStart) {
        gListStart = want;
        app.screenDirty = true;
      }
    }
    return;
  }

  // ---- Finger lifted off --------------------------------------------------
  if (!pressed && down) {
    down = false;
    if (scrollDrag) { scrollDrag = false; return; }
    if (dragging) return;                                 // that was a push
    if (app.screen != startScreen) return;                // the page changed
    if (std::abs(x - startX) > kTapSlopPx) return;        // slid sideways
    if (std::abs(y - startY) > kTapSlopPx) return;
    handleTap(startX, startY, (now - startMs) >= kLongTapMs, now);
  }
}

// ===========================================================================
//  Hardware detection
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

  // Remember the time window set by PCD_Init() so that the fast probe
  // can restore it exactly afterwards.
  const uint16_t reload =
      static_cast<uint16_t>(rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegH) << 8) |
      rfid.PCD_ReadRegister(MFRC522_I2C::TReloadRegL);
  if (reload > kRfidProbeReload) gRfidTimerReload = reload;
  Serial.printf("RFID Zeitfenster = %u x 25 us\n", static_cast<unsigned>(gRfidTimerReload));
  rfidFieldOff();   // field only when needed (see rfidFieldOn)
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

  // ---- Offscreen buffer in PSRAM ----
  // 320 x 240 x 16 bit = 150 kB. If that fails, drawing goes direct.
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  gUseCanvas = canvas.createSprite(kScrW, kScrH);
  gDraw      = gUseCanvas ? static_cast<lgfx::LovyanGFX*>(&canvas)
                          : static_cast<lgfx::LovyanGFX*>(&M5.Display);
  if (!gUseCanvas) {
    Serial.println("no memory for the offscreen buffer - drawing directly.");
  }
  gDraw->setSwapBytes(true);
  gDraw->setTextWrap(false);
  gDraw->fillScreen(kColBg);

  M5.Speaker.setVolume(64);

  randomSeed(micros());   // for the random selection via "?" on the card
  buildLogoMaps();
  setFallbackCpuChoices();
  loadSettings();
  applyBrightness();

  // ---- Port B: status lamp ----
  // begin() sets the pin to output and pushes black out once. If nothing is
  // attached, simply nothing happens. With fewer than three LEDs, those
  // present simply take the first colours.
  gLed.begin();
  gLed.setBrightness(app.settings.ledBrightness);
  gLed.clear();
  gLed.show();

  // ---- Port A: I2C for the RFID2 ----
  Wire.begin(kI2cSdaPin, kI2cSclPin, 100000UL);
  app.rfidReady = initRfid();

  // ---- microSD ----
  // The card hangs on the same SPI bus as the display. The pins therefore
  // have to be set explicitly, otherwise SD.begin() looks on the ESP32-S3
  // default assignment.
  SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
  app.sdReady = initSd();

  // Network configuration from NVS; build_env.h only supplies the initial values.
  loadNetConfig();

  // If nothing is in NVS yet, /wifi.txt from the SD card may step in.
  if (gWifiCount == 0 && app.sdReady) {
    String error;
    const size_t added = loadWifiFromSd(&error);
    if (added > 0) Serial.printf("wifi.txt: %u network(s) taken over\n", static_cast<unsigned>(added));
    else           Serial.printf("wifi.txt: %s\n", error.c_str());
  }

  // ---- Start screen ----
  drawStatusBar();
  drawFullScreenFor(ScreenMode::Home);
  drawStaticLogo();
  pushFrame();

  app.configReady = configReady();

  // If several networks are stored, scan once and start with the strongest
  // known network.
  wifiPickBestProfile();
  beginWiFi(millis());
  refreshConnectionStatus(millis(), true);
  resetHomeAnimation(millis());

  if (gDirectMode) {
    setModal("DIRECT MODE", kColInfo, millis(), 2000);
  } else if (!hasWiFiConfig()) {
    setModal("SETUP > WIFI", kColWarn, millis(), 2600);
  } else if (!hasTargetConfig()) {
    setModal("c64u ADDRESS MISSING", kColWarn, millis(), 2600);
  } else if (!app.rfidReady) {
    setModal("RFID2 not found", kColWarn, millis(), 1800);
  } else if (!app.sdReady) {
    setModal("No SD card", kColWarn, millis(), 1800);
  }

  Serial.printf("C64uRemote CoreS3 v%s (%s)  RFID:%d  SD:%d  Canvas:%d  Heap:%u  PSRAM free:%u\n",
                kFwVersion, kFwDate,
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

  // The portal page shows a countdown - redrawing it once per second is
  // enough for that.
  // The direct mode page shows the connection state and is refreshed the same way.
  if ((app.screen == ScreenMode::WifiPortal || app.screen == ScreenMode::WifiDirect) &&
      now - app.lastStatusDrawMs >= kStatusRefreshMs) {
    app.screenDirty = true;
  }

  // PowerOff confirmation expires after the time window
  if (app.pendingPowerOff && (now - app.pendingPowerOffAtMs > powerOffConfirmMs())) {
    app.pendingPowerOff = false;
    if (app.screen == ScreenMode::Home) app.screenDirty = true;
  }
  // The same for a prompt that originates from a PowerOff card
  if (app.cardPowerOffPending &&
      static_cast<int32_t>(now - app.cardPowerOffUntilMs) >= 0) {
    app.cardPowerOffPending = false;
    app.rfidHint    = "confirmation expired";
    app.screenDirty = true;
  }

  handleTouch(now);
  serviceRfid(now);
  serviceLed(now);

  // A read page opened by the background poll disappears again after
  // kAutoRfidHoldMs - back to where it came from.
  if (app.autoRfidActive && app.screen == ScreenMode::RfidRun &&
      static_cast<int32_t>(now - app.autoRfidUntilMs) >= 0) {
    app.autoRfidActive = false;
    restoreAutoRfidScreen(now);
  }

  if (app.screen == ScreenMode::Home) updateHomeDemo(now);

  // Reload the CPU value once, as soon as the WiFi is up
  if (netReady() && app.currentCpuValue == "Unknown" &&
      app.screen == ScreenMode::Home) {
    refreshCpuValue();
  }

  if (nextFrameMs == 0 || static_cast<int32_t>(now - nextFrameMs) >= 0) {
    render(now);
    nextFrameMs = millis() + kFrameMs;
  }

  delay(2);
}
