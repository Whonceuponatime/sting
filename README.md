# STING - USB Port Liveness Tester

Proves whether a USB interface port on shipboard equipment is **logically
disabled**, or merely shows nothing on screen.

The tester is a single ESP32-S3-DevKitC-1. Plugged into a port, it presents
itself as a small USB drive plus a keyboard and records, with timestamps, how
far the host gets in talking to it. The stored log is evidence for
IACS UR E26 / E27 discussions.

> MVP. The firmware compiles, but see [Status](#status) for what has and has
> not been verified on hardware.

## Hardware

- ESP32-S3-DevKitC-1 (the only device needed)
- USB-C to USB-A cables
- No isolator: the board is never connected to the laptop and the system at
  the same time

### Port roles (fixed)

| Port on the board | Role | Plugs into |
|---|---|---|
| **USB** (native) | the probe | the system under test, only |
| **UART** | laptop link: flashing and reading reports | the laptop, only |

**Never connect both ports at the same time.**

## Build and flash

Requires [PlatformIO](https://platformio.org/install/cli). With the **UART**
port connected to the laptop:

```
pio run -t upload
pio device monitor
```

Any serial terminal works for the CLI: 115200 baud, 8N1.

## Workflow

1. **Prepare.** Plug the UART port into the laptop, flash the firmware, open a
   serial terminal and type `clear` to wipe old logs. Optionally
   `label FDS-USB1` to tag the first port. Unplug.
2. **Test.** Plug the USB port into the system's interface port. The LED
   blinks while the tester observes and turns **solid** after 30 seconds, when
   the result is saved. Unplug once it is solid.
3. **Repeat** step 2 for each port. Every plug-in is a separate session.
4. **Report.** Plug the UART port into the laptop, open a serial terminal and
   type `report`.

To label every port, go back to the laptop between tests: `label <name>` tags
the next session only.

## LED

| LED | Meaning |
|---|---|
| Red | powered, no host activity |
| Yellow | enumeration seen |
| Green | fully configured, class drivers active |
| Blinking (any colour) | still observing, result not final yet |
| Solid (any colour) | 30 s window complete and saved: safe to unplug |
| Blue | laptop/CLI mode: this power-up is not recorded |
| Magenta flashes | the result could **not** be saved (log full or flash error) |

No light at all means the port supplies no power. Record that by hand; the
tester cannot log a session it never booted for.

## What is checked, in order

1. **Power** - the board boots, so the port supplies power.
2. **Bus activity** - the host resets the bus.
3. **Enumeration** - the host requests device descriptors. This is the key
   verdict: if it happens, the port is **not** logically disabled.
4. **Configuration** - the host selects the device configuration.
5. **Mass storage** - the host's storage driver sends SCSI commands: inquiry,
   capacity, sector reads. The report records *which* sectors the host read,
   so it distinguishes a host that merely identified the disk (boot sector
   only) from one that read the directory listing and the file contents. A
   host that reads the file sector has demonstrated that file exchange is
   possible on that port; a host that reads the volume on insertion
   (AutoPlay / autorun-style behaviour) shows up here too. The tester never
   initiates a transfer: every sector counted was requested by the host.
6. **Keyboard (HID)** - the host's keyboard driver attaches. The tester then
   types a harmless marker string, never commands.

## Verdicts

| Verdict | Meaning |
|---|---|
| **DISABLED** | powered, but no enumeration within 30 seconds |
| **ENUMERATES** | descriptors requested but no class driver bound: logically live |
| **ACTIVE** | configured, with storage and/or keyboard accepted: fully live |

A port blocked only by an operating-system policy (for example "USB storage
denied") is still a live port. It will show ENUMERATES, or ACTIVE with only
the keyboard accepted, and the per-stage detail shows which.

## CLI (UART port)

| Command | Action |
|---|---|
| `report` | print all sessions |
| `last` | print only the most recent session |
| `clear` | wipe the log |
| `label <name>` | tag the next session, e.g. `label FDS-USB1` (max 31 characters) |
| `help` | list commands |

The log keeps the newest 200 sessions; once full, the oldest is dropped to make
room, so recording never stops.

## Report contents (per session)

- Session number and label, and a real date/time when a phone set the clock
  (see the WiFi live read)
- Each stage reached, with its time since tester power-up
- Descriptor types requested, with counts
- Whether storage was read, how many sectors, and which sectors (boot /
  directory / file): how far the host went into the volume's contents
- Whether the keyboard was accepted, and how much of the marker was typed
- Final verdict

Layout of one session (values illustrative):

```
--- Session 1  [FDS-USB1] ---
  observed        30 s window complete (last update t+30.010 s)
  1 power         YES  t+0.000 s    tester booted from port power
  2 bus activity  YES  t+0.412 s    first bus reset (2 total)
  3 enumeration   YES  t+0.498 s    first descriptor request
                    requested: DEVICE x2, CONFIGURATION x2, STRING x6, DEVICE_QUALIFIER x1, HID_REPORT x1
  4 configuration YES  t+0.731 s    host selected the configuration
  5 mass storage  YES  t+0.902 s    first SCSI command from the host
                    inquiry: yes | capacity read: yes (t+0.950 s) | sectors read: 37 (first t+1.020 s)
                    sectors accessed: 0=boot, 1=FAT, 2=dir, 3=file
                    host access reached: file contents - the host read a file off the volume (file exchange is possible on this port)
  6 keyboard      YES  t+0.800 s    host keyboard (HID) driver attached
                    report descriptor fetched: yes | LED state written by host: yes | input reports collected: 25
                    marker: 12 of 12 keys typed (done t+2.900 s)
  VERDICT: ACTIVE - port is fully live: configured, storage and keyboard accepted
```

## What the system under test sees

- A composite USB device, `303a:4006`, manufacturer `STING`, product
  `USB Port Liveness Tester`, with the chip's MAC address as serial number.
  It is meant to be easy to find in the host's own device and event logs.
- A write-protected FAT12 volume `STING TEST` holding a plain-text
  `README.TXT` and, when `EICAR_ENABLED` is set, `EICAR.COM` (see below).
  Nothing can be written to it.
- A keyboard that types `stingusbtest` once, about 1.5 s after the host
  configures it. The marker is limited to `a-z` and `0-9`: no Enter, Tab,
  Space, modifier or function keys.

## Antivirus test (EICAR)

When `EICAR_ENABLED` is set (the default), the fake drive also carries
`EICAR.COM`, the standard **EICAR anti-malware test file** - a harmless,
industry-standard 68-byte string published specifically for testing antivirus
software. It is not malware and does nothing if run. It lets you check whether
the system under test runs antivirus that scans USB media: if it does, reading
the volume should make it alert on or quarantine the file. The report shows
whether the host read the EICAR sector; whether its antivirus reacted is
confirmed on the host side.

The string is stored XOR-obfuscated and reconstructed only in RAM when the host
reads it, so the genuine signature never appears in this firmware image or its
source - an antivirus on your build machine will not quarantine them. Set
`EICAR_ENABLED 0` in `main/config.h` to leave the file off the drive.

## WiFi live read

When `WIFI_READ_ENABLED` is set (the default), the tester raises its own WiFi
access point so you can watch a test live from a phone, without the laptop:

1. Join the WiFi network **`STING`** (open, no password).
2. Open **`http://sting.local/`** (Apple devices and most desktops) or
   **`http://192.168.4.1/`** (works everywhere, including Android).

The page shows the current test updating every second - a big verdict banner
(ACTIVE / ENUMERATES / DISABLED / WAITING / LAPTOP), the stages lighting up,
the sectors read - and links to the full stored report in the same text as the
UART console.

While the page is open, your phone also quietly sends its clock to the board,
so that session gets a real local **date and time** in the report. The board
has no clock of its own and is unpowered between tests, so sessions tested with
no phone connected show relative times (`t+…`) and a session number only. It is a closed point-to-point link: the board is
the access point and nothing is routed to any other network. The radio comes up
~1.5 s after USB so it never disturbs the first enumeration.

The radio is a convenience, not evidence. For a radio-silent, cleanly auditable
build - or where transmitters are not permitted - set `WIFI_READ_ENABLED 0` and
nothing is ever transmitted. See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md)
for the reasoning on when to leave it off.

> Note: `sting.local` resolves over mDNS, which works on Apple devices and most
> desktops but not in many Android browsers. On Android, use `192.168.4.1`.

## Things to know before relying on a result

- **Wait for the solid LED.** A session with no host activity is only written
  when the 30 s window closes. Unplugged earlier, it leaves no record at all.
  A session that did see host activity and is unplugged early is kept and
  reported as `INCOMPLETE`.
- **Timestamps are relative.** The board has no clock, so times are seconds
  since the tester powered up. Put the date and port name in the label and
  keep your own notes.
- **Laptop power-ups.** The board cannot sense which port powers it. A
  power-up is treated as a laptop connection, and removed from the log, as
  soon as a valid command arrives. If the board sits on the laptop for more
  than 30 s and is then reset or unplugged before any command is typed, it
  leaves a spurious DISABLED session behind. Type a command first, and check
  that the number of sessions matches the number of ports tested.
- **The keyboard marker lands wherever the input focus is.** It is letters
  only, but on an HMI single letters can be hotkeys. Agree this with the
  system owner, or build with `MARKER_ENABLED 0` in `main/config.h`: no
  keystrokes are sent and keyboard acceptance is still detected from the
  host's driver activity.
- **The host may log a second device at plug-in.** For the first fraction of
  a second after power-up, before this firmware starts, the ESP32-S3 itself
  presents its built-in "USB JTAG/serial debug unit" (`303a:1001`). A live
  host may record that device or a failed enumeration. It does not affect the
  tester's own log.
- **DISABLED is a statement about this tester, on this port, at this time.**
  It means the host did not enumerate a full-speed composite device within
  the window.

## Safety rules

- One connection at a time: never laptop and system together.
- The keyboard function types only the harmless marker string.
- `EICAR.COM` is the inert, standard antivirus *test* file, not malware.
- Get the system owner's permission before testing live equipment, and agree in
  advance on the keyboard marker and whether the WiFi radio may be used.

## Out of scope for the MVP

- File-extension filtering tests (for example the `.ccp`-only claim)
- Ethernet port testing
- Any payload beyond the benign marker and the EICAR antivirus test file

## Settings

All in [`main/config.h`](main/config.h): observation window, USB identity,
marker text, LED pins and brightness, log size, `EICAR_ENABLED`, and the WiFi
settings (`WIFI_READ_ENABLED`, SSID, password, hostname).

The RGB LED is on GPIO48 on DevKitC-1 v1.0 and GPIO38 on v1.1; the firmware
drives both.

## Source layout

| File | Purpose |
|---|---|
| `main/main.c` | session loop: when to save, LED state |
| `main/usb_probe.c` | USB device (TinyUSB) and everything it observes |
| `main/fake_disk.c` | the read-only FAT12 test volume, README and EICAR |
| `main/store.c` | session log in flash (NVS) |
| `main/cli.c` | UART commands |
| `main/report.c` | report text (shared by the UART console and WiFi) |
| `main/webread.c` | WiFi access point and live-read web server |
| `main/led.c` | RGB LED |
| `tests/host/` | PC-side checks of the test volume, verdict rules and report text |

See [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) for a full explanation of the
method and what each result means.

## Host tests

The fake volume, the verdict rules and the report text need no hardware and
can be checked on a PC with any C compiler:

```
gcc -std=c11 -Wall -Wextra -Imain tests/host/test_host.c main/fake_disk.c main/report.c -o test_host
./test_host
```

It prints one sample session per verdict and ends with `ALL CHECKS PASSED`.

## Status

Verified so far:

- The firmware builds cleanly (PlatformIO `espressif32@8.0.0`, ESP-IDF 6.1,
  TinyUSB 0.21, mDNS).
- Core probe tested on an ESP32-S3-DevKitC-1 (firmware 0.2): a live laptop port
  gave `ACTIVE` (enumerated, configured, volume read, keyboard accepted, marker
  typed into Notepad); a USB power bank gave `DISABLED` (powered, no bus
  activity). The UART CLI (`report`, `clear`, `label`) works, and the laptop
  connection used to read reports was correctly not recorded as a session.
- The host tests pass, and the generated test volume opens as a valid FAT12
  disk in an independent tool (7-Zip).

**Added since that hardware run and not yet bench-tested:** the per-sector
file-access detail, the EICAR antivirus test file, and the WiFi live read
(current firmware 0.5). Before relying on them, confirm on hardware:

1. EICAR: against a host running antivirus, check it alerts on or quarantines
   `EICAR.COM` when the volume is read, and that the report shows the EICAR
   sector as read.
2. WiFi: join the `STING` network and confirm the live page updates and the
   full report loads, at `sting.local` (Apple) and `192.168.4.1` (any device).
3. Re-confirm `ACTIVE` and `DISABLED` as above, since the firmware changed.
