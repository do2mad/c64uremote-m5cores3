% C64uRemote for M5Stack CoreS3
% User Manual
% Version 1.0

# Welcome

C64uRemote turns an **M5Stack CoreS3** into a convenient remote control for the
**Commodore 64 Ultimate (c64u)** and the **Ultimate64 Elite-II**. Over Wi-Fi you
control the machine remotely: reset, reboot, power off, open the Ultimate menu
and change the CPU speed.

With the optional **RFID2 reader** and a **microSD card** it becomes a
tap-to-launch console: you hold an NFC card to the device and the matching game
starts on the C64. The cards are compatible with the TeensyROM and Zaparoo
format – the same card works on both systems.

This project builds on the original idea by Karl Prosser (@klumsy) and was
extended for the M5Stack CoreS3 by Martin Oswald (@mad, 1MHz.de).

# What you need

**Required:**

- An M5Stack CoreS3, CoreS3 SE or CoreS3 Lite
- A Commodore 64 Ultimate or Ultimate64 Elite-II on the same Wi-Fi

**Optional, for the NFC features:**

- M5Stack Unit RFID2 (WS1850S), connected to **Port A** (the CoreS3 has no
  built-in NFC reader)
- An M5Stack Unit RGB (three SK6812) on **Port B** as a status lamp – nice to
  have, not needed
- A microSD card (formatted FAT32, up to 16 GB) in the CoreS3
- NFC cards: NTAG215 recommended, NTAG213/216 and MIFARE Classic also work

Without the reader or SD card, all other functions keep working normally.

# First-time setup

For the CoreS3 to find your C64, the Wi-Fi credentials and the C64's address must
be stored once. You can do that **right on the device** under *Setup → WiFi* –
the source code no longer has to be touched. The credentials go into internal
memory and survive every restart. The *Setting up Wi-Fi* chapter walks through
the details.

If you would rather supply the credentials while programming the device, put
them into `build_env.h` as before (see the technical documentation). They then
serve as the initial values for the very first start.

The connection status is always shown in the top left of the display:

| Display | Meaning |
|---|---|
| **C64U OK** (green dot) | Everything connected, ready |
| **NO C64U** (blue) | Wi-Fi present, but the C64 does not respond |
| **AUTH?** (yellow) | C64 reachable, but the password is wrong |
| **NO WIFI** (red) | No Wi-Fi connection |

To the right you see the IP address, the current CPU speed and whether the RFID
reader and SD card were detected.

# Touch operation

The CoreS3 no longer has buttons A, B and C – only **POWER** and **RESET** on
the left-hand side. It is operated entirely through the **touchscreen**.

| Gesture | Effect |
|---|---|
| **Tap** | select and run a tile or list row |
| **Swipe** | the list follows your finger, row by row |
| **Drag the scrollbar on the right** | quickly through long lists |
| **Back** (bottom left) | one level back |
| **▲ / ▼** (bottom right) | one row up or down |
| **Long tap** | in the SD browser: one folder up |

The bar along the bottom edge is a control surface in its own right: the back
field is always on the left, on list pages the two paging fields appear on the
right, and between them a short hint says what a tap does here right now.

**Paging through long lists.** When a list does not fit on the screen, a
**scrollbar** appears on the right. It shows where you are and how long the list
is – and it can be dragged directly with a finger. With many entries that takes
you from one end to the other in a single movement.

Swiping in the middle of the list works the same way: it follows your finger.
As soon as it moves, it is clear that you meant to page rather than select – so a
swipe never triggers an entry by accident, not even a slow one. If a list fits on
the screen entirely there is nothing to push; every tap stays a tap there.

**POWER and RESET** are hardware buttons of the CoreS3 and have nothing to do
with the C64: a short press of POWER switches the CoreS3 on, holding it for 6 s
switches it off. RESET restarts it.

# The home screen

At the top the status bar, in the middle the animated logo, below it all
commands as tiles:

```
RESET   REBOOT   MENU    POWER   CPU
JOY     RFID     SD      STATUS  SETUP
```

You simply tap a tile – one tap selects it and runs it straight away.

| Tile | What happens |
|---|---|
| **RESET** | The C64 is reset (like the reset button) |
| **REBOOT** | The C64 restarts completely |
| **MENU** | Opens or closes the Ultimate menu on the C64 |
| **POWER** | Powers off the C64 – tap twice for safety |
| **CPU** | View and change the CPU speed |
| **JOY** | Swap the joystick ports on the C64 (Normal ↔ Swapped) |
| **RFID** | Tap a card and start the game stored on it |
| **SD** | Pick a game directly from the SD card and start it |
| **STATUS** | Detailed connection info; a tap starts a connection test |
| **Setup** | Settings and NFC tools |

**Powering off (POWER):** Turning off by accident would be annoying, so the
prompt *POWER OFF? TAP AGAIN!* appears first. Only a second tap on the same
tile within the time window actually powers off; a tap anywhere else cancels.

# The status lamp

If an **M5Stack Unit RGB** (three SK6812) is attached to **Port B**, it shows
the state even when you are not looking at the display:

| Lamp | Meaning |
|---|---|
| all three **red** | no Wi-Fi connection |
| all three **blue** | Wi-Fi up, but the C64 does not answer |
| all three **yellow** | C64 reachable, the password is wrong |
| all three **green** | everything connected, ready |
| **magenta**, running light | the setup portal is running |
| **cyan** as a bar | a game is being loaded into the C64 right now |
| short **green** (all) | card read or written successfully |
| short **red** (all) | card empty, unreadable, or a write error |

The colours are the same as the dot at the top left of the display.

**While loading** the three LEDs become a progress bar: the first shines fully
once a third has been transferred, then the second, then the third. The section
in progress pulses so you can see things are moving. If the file size happens to
be unknown, a running light takes over instead.

Under *Setup → Status-LED* you switch it off, under *LED Bright* you set the
brightness (10 to 255; 60 is the factory setting and comfortable in most rooms).

> The CoreS3 **cannot** detect whether an LED is actually connected – a lamp
> like this sends nothing back. If none is plugged in, simply nothing happens.
> You only need to switch it off if you want Port B for something else.

# The battery indicator

The CoreS3 has a built-in battery. How full it is can be seen in two places in the
status bar.

**The line below the status bar** doubles as the level gauge: the filled part is
slightly thicker and coloured, the rest stays the dimmed line.

**At the right-hand end** two displays take turns every 15 seconds – `RFID` and
`SD` as before, and a battery symbol with the percentage inside.

The colour means the same in both places:

| Colour | Level |
|---|---|
| green | from 50 % |
| yellow | from 20 % |
| red | from 10 % |
| red, blinking | below 10 % |
| turquoise | on the charger |

While the CoreS3 is on external power a small **bolt** sits in front of the
battery symbol. It appears as soon as the cable is plugged in – the charge
controller reports the USB voltage, so it stays on with a full battery too.

The **status page** shows the level as a number as well.

# Changing the CPU speed

Tap the **CPU** tile. The current speed is shown at the top, the list of
options below – just tap the step you want. The CoreS3 reads the available
steps directly from the C64, so you
get exactly the values your device supports.

# Swapping the joystick ports

Some games expect the joystick in port 1, others in port 2. Instead of moving the
cable, the mapping can be swapped inside the C64.

Tap the **JOY** tile — every trigger toggles between *Normal* and
*Swapped*, and *JOY Swapped* or *JOY Normal* appears briefly.

*SETUP → Joystick* shows the current state and steps through every value your C64
offers: besides *Normal* and *Swapped* there may be *WASD P1* and *WASD P2*,
depending on the firmware — the keyboard then drives that port.

The Ultimate firmware has no dedicated remote command for this. The CoreS3 sets
the *Joystick Swapper* item in the C64 configuration, exactly like the CPU speed.
The state therefore survives until it is changed again, a reset included.

# Launching games by card (RFID)

Requirements: RFID2 reader connected, a microSD with your games inserted, and the
card written beforehand (see below).

## Just present the card (automatic)

Normally you do not have to operate anything at all: the CoreS3 checks in the
background whether a card is present. As soon as it finds one it switches to
read mode on its own and launches the stored program.

That applies not only on the home screen but also while you are **browsing the
SD card**, on the **STATUS** page and in the **CPU** menu. Afterwards the CoreS3
returns exactly to where you were.

It deliberately does **not** react while you are working with cards yourself:
picking a file for *NFC-Write*, restoring a dump, in the setup and in the Wi-Fi
configuration. Launching a program there would be the exact opposite of what you
are doing.

After the launch the reading screen stays up for about 20 seconds, so you can
present the next card right away. After that — or as soon as you tap the screen —
it goes back.

How often it looks is set under *Setup → Auto-NFC*:

| Setting | Meaning |
|---|---|
| **Off** | no background polling, cards only through the RFID tile |
| **1.5s** | very frugal |
| **0.7s** | factory default, a good compromise |
| **0.3s** | quickest to react |

The probe is short enough that even at *0.3s* it slows neither the animation nor
the controls noticeably.

## Through the RFID tile

If you want to read a card deliberately — or if *Auto-NFC* is switched off — the
manual route still works:

1. Tap the **RFID** tile.
2. Place the NFC card on the reader.
3. The CoreS3 reads the stored path, fetches the file from the SD card and sends it
   to the C64. A progress bar shows the upload.
4. After *GESTARTET: …* ("started") the game runs.

Remove the card, tap the next one – the screen stays in read mode, so you can
play several cards one after another.

## Which files work

| Extension | What it is |
|---|---|
| `.prg` | Program (loaded and started) |
| `.crt` | Cartridge |
| `.sid` | SID music |
| `.mod` | Amiga MOD music |
| `.d64 .d71 .d81 .g64 .g71` | Disk images |

For disk images the image is mounted as drive 8. What happens next is set under
*Disk Action* in the setup: mount only, mount and reset, or mount and
automatically start the first program.

# Command cards

A card does not have to point at a game – it can also carry a **command**.
Present it and the command runs immediately, with no menu and no SD card needed.

| Card | Effect |
|---|---|
| **Reset** | Resets the C64 |
| **Reboot** | Restarts the C64 completely |
| **Ultimate Menu** | Opens or closes the Ultimate menu |
| **PowerOff direct** | Powers off immediately |
| **PowerOff with prompt** | Asks first – present the same card a second time within the time window to confirm |
| **CPU x MHz** | Sets the CPU to the value stored on the card |
| **Swap Joystick** | Swaps the joystick ports (Normal ↔ Swapped) |
| **Joystick Normal / Swapped / WASD P1 / WASD P2** | Sets the port mapping to that fixed value |

## Creating a command card

1. Choose **NFC-Cmd** in the settings.
2. Pick the command from the list. After the fixed entries come the joystick
   mappings and then all CPU steps your C64 offers, so a "CPU 10 MHz" card is
   a single click. *JOY* or *CPU* on the right tells the two blocks apart.
3. Present the card; *KARTE OK* means written and verified.

## PowerOff with prompt

The waiting time lives **on the card**, not in the device. When writing, the
value comes from *NFC-Cmd PowOff* (3, 5, 8 or 15 seconds, 8 s by default).
Presenting such a card shows *POWER OFF? NOCHMAL!* with a countdown. To power
off:

- present the **same card** again, or
- **tap the screen**

If the countdown expires or a different card appears, nothing happens. A card
with a time of **0** powers off immediately without asking.


## Switching the CoreS3 off by card

A card holding `CMD:M5OFF` switches the **CoreS3 itself** off – without a
prompt, because presenting the card is already a deliberate act. On battery it
goes off completely; with USB attached it only goes into deep sleep and starts
again with the power or reset button. So that a card still lying on the reader
at power-up does not switch the device off again right away, it is ignored for
the first eight seconds after start (*REMOVE CARD*). The same card also switches
an M5Dial off.

## What is stored on the card

The command is plain text in an NDEF record – you can inspect or write it with
any NFC app on your phone:

```
CMD:RESET
CMD:REBOOT
CMD:MENU
CMD:POWEROFF=0      power off immediately
CMD:POWEROFF=8      ask first, 8 seconds to confirm
CMD:M5OFF           switch the CoreS3 itself off
CMD:CPU=10          set the CPU to 10 MHz
CMD:JOY             toggle the joystick ports
CMD:JOY=SWAPPED     set the ports fixed; also NORMAL, WASD1, WASD2
CMD:DIRECT=192.168.4  switch direct mode on with this network
CMD:DIRECT=OFF      direct mode off, back to the stored WiFi
```

Case does not matter. The M5Dial and M5Stack Core editions understand the same
format, so one card works on both.

# Launching games directly from SD (no card)

Open the **SD** tile. You see the files and folders on your SD card. This lets
you start something without an NFC card – handy for trying things out.

In the SD browser the following also applies:

| Gesture | Function |
|---|---|
| **Tap** | start a file or open a folder |
| **Swipe** | the list follows the finger; scrollbar on the right for long lists |
| **Long tap** | one folder up |
| **Back** (bottom left) | one folder up; at the top level back to the home screen |
| `..` in the list | also moves one folder up |

The title bar shows *long tap = ..* as a reminder.

# Writing and managing NFC cards

All card tools are found under **Setup** at the very top. Open the setup with the
Setup tile; the first four entries are the NFC actions.

## NFC-Write: assign a game to a card

1. Open the setup and tap **NFC-Write**.
2. Tap the game file you want on the SD card.
3. Place the NFC card on the reader.
4. The path is written and immediately read back for verification. *KARTE OK*
   means success.

The format is compatible with the TeensyROM/Zaparoo system. A card written this
way works on both devices, as long as the folder structure on both SD cards is
the same. You can also write cards with your phone (the *NFC Tools* app, text
record).

## NFC-Info: read out a card

Shows everything on the card: UID, card type, memory size, the stored path and
whether the file actually exists on the SD card. A tap pages to
**page 2**, which shows technical raw data, lock bits, access protection and the
read counter.

Handy: this is how you find cards whose game you renamed or moved – the *Datei*
("file") line then reports "NICHT auf SD" ("not on SD").

## NFC-Dump and NFC-Restore: copying cards

- **NFC-Dump** saves the complete card content as a text file in the folder
  `/NFC-DUMPS/` on the SD card. Works with any card, including foreign MIFARE
  cards without text.
- **NFC-Restore** writes such a dump back onto a (blank) card.

This lets you clone cards. One note: a card's serial number (UID) is fixed in the
chip and cannot be copied – but the content is copied completely. For game cards
that is enough, because what matters there is the stored path.

# Setting up Wi-Fi

Everything for this sits under **Setup → WiFi**. Tap an entry directly; swipe
to page. The CoreS3 remembers up to **four networks** and
tries them one after another when connecting – handy if you move between home
and a phone hotspot.

| Entry | Effect |
|---|---|
| **Direct mode** | Own WiFi without a router, the c64u connects directly (see chapter *Direct mode*) |
| **Direct net** | Address range of the direct network: `192.168.4.x` or `192.168.2.x` |
| **Scan networks** | Search the area and pick a network from the list |
| **Load from SD** | Read `wifi.txt` from the microSD |
| **Setup portal** | Access point of its own with a web interface |
| **Saved** | Pick a stored network and connect |
| **To NFC card** | Write a stored network to an NFC card |
| **Save to SD** | Write all stored networks to the microSD as `wifi.txt` |
| **Delete network** | Remove a single entry |
| **Delete all** | Discard all credentials |

## Route 1: scan, then hand over the password on a card

*Scan networks* shows the networks found after a few seconds, along with their
signal strength. A network that is already stored is marked *known* and connects
straight away; an *open* network needs no password.

For everything else the CoreS3 asks for the password and waits for an NFC card.
The card carries an ordinary text record in the same scheme that Wi-Fi QR codes
use:

```
WIFI:S:MyWiFi;T:WPA;P:MyPassword;;
```

Write the card with a phone, for example with the **NFC Tools** app
(*Write → Add a record → Text*). If the card holds only the password without the
`WIFI:` prefix, it is assigned to the network you picked beforehand.

> The password sits unencrypted on the card. Once you are set up, overwrite the
> card – the device keeps the password in internal memory anyway.

## Presenting a card is enough

A complete `WIFI:` card also works **outside** the setup: present it on the home
screen and the CoreS3 stores the network and connects to it right away. It waits
up to eight seconds and then reports *WIFI ACTIVE* with the IP address, or
*NETWORK NOT THERE* if the network is out of range. If that network is already
connected, nothing happens (*ALREADY CONNECTED*) – so the card may simply stay
on the reader.

That gives a second device its credentials in a matter of seconds.

## Route 2: a file on the SD card

Put a text file `wifi.txt` in the root directory of the microSD:

```
ssid = MyWiFi
pass = MyWiFiPassword

ssid = Hotspot
pass = secret123

host     = 192.168.0.64
hostpass =
```

Every new `ssid` line begins a new entry; lines starting with `#` are comments.
The file is read at start-up (as long as no network is stored yet) and at any
time through *Load from SD*.

The other way round, **Save to SD** writes all stored networks together with
`host` and `hostpass` back out in exactly this format. An existing `wifi.txt` is
renamed to `wifi.bak` first, so nothing is lost. That makes setting up a second
device a no-typing affair – bear in mind that the passwords sit on the card in
plain text.

## Route 3: the setup portal

*Setup portal* turns the CoreS3 into an access point of its own for five minutes.
The display shows the network name, the password and the address you open in a
browser. There you enter SSID and password – either from the list of networks
found or by hand – and optionally the address and password of the C64.

After saving, the CoreS3 shuts the access point down and connects to the new
network. The browser connection breaking off in the process is normal.

# Direct mode: no router, e.g. at meetings

At a meeting there is often no WiFi – or one you would rather not put the
c64u on. For this case the CoreS3 opens a small WiFi network of its own and the
c64u connects to it directly. No router is needed.

| | |
|---|---|
| **Network name (SSID)** | `C64uRemote-Direct` |
| **Password** | `c64ultimate` |
| **CoreS3** | `192.168.4.1` |
| **c64u** | `192.168.4.64` |

## Once on the c64u

The c64u can only remember **one** WiFi network. In the Ultimate menu, enter
the direct network's name and password in the network/WiFi settings and leave
address assignment on **DHCP**. Back home, enter your home network on the c64u
again.

## On the CoreS3

Under **Setup → WiFi**, tap *Direct mode*. The CoreS3 leaves its normal WiFi,
starts the direct network and shows a page with everything the c64u needs:
network name, password, the c64u's address and the connection state
(*waiting for the c64u*, *c64u connected*).

As soon as the c64u joins, it gets the address `192.168.4.64` and the CoreS3
checks right away whether it answers. After that everything works as usual.
The c64u's stored home address is left untouched.

If another device (a phone, say) joins first, it gets `.64` and the c64u the
next address. That is not a problem: the CoreS3 then tries the connected devices
one after another until the c64u answers. The address it found is shown on the
direct mode page and in the status display.

Direct mode stays set after switching off – the CoreS3 starts straight into the
direct network next time.

**Switching off:** Tap the *Direct mode* entry in the WiFi menu once more. A tap on the direct mode page only goes back to the WiFi menu – so an accidental touch does not throw the c64u off the network. The CoreS3 then reconnects to the stored WiFi.
The same happens when you pick a network under *Saved* or present a WiFi card.

## Address range

*Direct net* switches between `192.168.4.x` and `192.168.2.x`. The CoreS3
always has `.1`, the c64u `.64`. If direct mode is running, the network
restarts right away; the c64u reconnects by itself.

For other values, `wifi.txt` on the SD card has lines of its own:

```
direct      = on                  on / off
direct_ssid = C64uRemote-Direct
direct_pass = c64ultimate         at least eight characters
direct_net  = 192.168.4           CoreS3 = .1, c64u = .64
```

*Save to SD* writes these lines out as well.

## Switching by card

*NFC-Cmd* offers three command cards for direct mode: first the network
currently set, then the other one (`192.168.4.x` or `192.168.2.x`), last
*Direct mode off*. The card then carries `CMD:DIRECT=192.168.4`,
`CMD:DIRECT=192.168.2` or `CMD:DIRECT=OFF`. Presenting it switches right away;
if direct mode is already running with that network, nothing happens – so the
card may stay where it is.

## More devices on the direct network

- **A phone or notebook** can join the direct network as well. The c64u's web
  interface is then at `http://192.168.4.64`.
- **A second M5 remote** joins like any other WiFi, most easily with a WiFi
  card `WIFI:S:C64uRemote-Direct;T:WPA;P:c64ultimate;;`. When it recognises
  the direct network, it addresses the c64u at `.64` automatically – its home
  address stays stored. Only **one** device switches direct mode on.

## Good to know

- The direct network draws a little more power than normal WiFi operation,
  because the radio has to stay on all the time.
- The range is fine for a table, less so across a hall.
- *Scan networks* also works in direct mode. During the scan the c64u may
  briefly lose contact; it reconnects by itself afterwards.

# Settings overview

In **Setup**, after the NFC actions, you find all settings. They are stored
permanently and reloaded after power-cycling.

## NFC and Wi-Fi

| Setting | Options |
|---|---|
| **NFC-Random** | Create a random-pick card for a directory |
| **NFC-Cmd** | Create a command card (see the *Command cards* chapter) |
| **NFC-Cmd PowOff** | Default prompt time for a PowerOff command card: 3, 5, 8, 15 s |
| **WiFi** | Wi-Fi setup submenu (see the *Setting up Wi-Fi* chapter) |
| **Auto-NFC** | Background polling interval: *Off*, *1.5s*, *0.7s*, *0.3s* |

## Powering off

| Setting | Options |
|---|---|
| **PowerOff Zeit** | How long the prompt of the POWER tile stays valid (0.5 to 3.0 s) |

## Display and effects

| Setting | Options |
|---|---|
| **Animations** | Logo animations on/off |
| **Effect** | Which effect: Auto (cycle), Static, Water, RotoZoom, SineWave, Ripple, Raster |
| **FX Detail** | Half or Full – thanks to its PSRAM the CoreS3 runs Full smoothly too |
| **Anim Speed** | Animation speed: Slow / Normal / Fast |
| **Effect Time** | How long an effect runs: Short / Normal / Long |
| **Static Time** | How long the calm logo is shown in between |
| **Brightness** | Display brightness (32 to 255) |
| **Status-LED** | Switch the lamp on Port B on or off |
| **LED Bright** | Brightness of the lamp (10 to 255) |

## C64 options

| Setting | Options |
|---|---|
| **Disk Action** | After mounting a disk image: mount only, mount + reset, or mount + start the first program |
| **Disk Drive** | Target drive: Auto (bus 8), or fixed A / B |
| **Joystick** | Port mapping in the C64: *Normal*, *Swapped*, and depending on firmware *WASD P1* / *WASD P2* |

## Miscellaneous

| Setting | Options |
|---|---|
| **Beep** | Confirmation tone on/off |
| **Factory Reset** | Reset all settings to factory defaults |

# Frequently asked questions

**Now and then *Not reached* or *Not verified* shows up and clears again.**
The HTTP server in the c64u occasionally refuses a connection ("connection
refused") although network and address are fine – this happens even with only a
single device on the network. Since v1.2.1 the firmware retries a refused call
by itself after a short pause, so you will usually not notice. If it stays that
way, restarting the c64u helps.

**The c64u keeps dropping out although the CoreS3 has good reception.**
Since v1.4.0 this should no longer happen: the connection to the c64u was
reworked thoroughly (see CHANGELOG), and the c64u runs just as reliably on WiFi
as on a LAN cable. For
meetings without a router there is direct mode; there the c64u talks to the
CoreS3 directly.
If the c64u cannot be reached at all after many network changes although its
menu shows a connection: unplug its power briefly. Switching it off and on with
the button was not enough in that case.

**The CoreS3 shows "NO WIFI".** Check that the Wi-Fi name and password are stored
correctly and the router is in range. The CoreS3 retries the connection every few
seconds.

**"AUTH?" in the status bar.** A network password is set on the C64 that is not
stored on the CoreS3. Check the password in the C64 menu or enter it in
`build_env.h`.

**A card starts nothing / "TYP UNBEKANNT" (unknown type).** The Ultimate can only
start the file types listed above. Use **NFC-Info** to check which path and
extension are on the card and whether the file is on the SD.

**The SD card is not detected.** Format as FAT32, 32 GB maximum, and if needed
try a different card. The CoreS3 automatically tries two speeds.

**The PowerOff command reports an error.** That is normal – when powering off the
C64 often no longer responds cleanly.

**Can I use the same cards on the TeensyROM?** Yes. The card format is identical.
The only important thing is that the folders on both SD cards have the same
names.

# License

C64uRemote is released under the **MIT License**. The full text is in the file
`LICENSE` in the project root.

It originates from **C64uRemote by Karl Prosser (@klumsy)**,
<https://github.com/ReadyOS-C64/C64uRemote>, which he published under the MIT
License. This version is a derivative work and is released under the same terms:

* Copyright (c) 2026 Karl Prosser – original project
* Copyright (c) 2026 Martin Oswald (@mad, <https://1MHz.de>) – port and extensions

The MIT License allows you to use, modify and redistribute the software,
including commercially. The only condition: **the copyright notice and the
license text must be kept** and included with every copy. There is no warranty
and no liability.

The libraries used carry their own licenses: M5Unified and M5GFX (MIT,
© M5Stack), ArduinoJson (MIT, © Benoit Blanchon) and MFRC522_I2C
(<https://github.com/kkloesener/MFRC522_I2C>). PlatformIO fetches them at build
time.

# How this was made

The port, the extensions and these manuals were written with the help of
Claude (Anthropic). Concept, idea, hardware decisions and every test on real
devices: Martin Oswald (@mad).
