# STING - How It Works

A technical explanation of the USB Port Liveness Tester: what it does, how it
decides, and what each result means. Written for someone who needs to
understand or defend the tool's output - a surveyor, a system owner, or an
engineer reviewing the method - not only for the person who runs it.

For the step-by-step operating procedure, see the [README](../README.md). This
document is about *why* the tool works and *what the evidence means*.

---

## 1. The question it answers

Shipboard equipment is often said to have its USB ports "disabled". In
practice that can mean anything from "the port is physically dead" to "nothing
shows up on the screen when you plug a stick in" - which is not the same thing
at all. A port can be electrically live, enumerate a device, even mount a
drive, while the operator sees nothing.

IACS UR E26 and E27 ask operators to control physical interfaces. To hold that
conversation you need evidence about what a port *actually does*, not what the
label says. STING produces that evidence: a timestamped record of exactly how
far a host went in talking to a standard USB device, for each port, saved so it
survives being unplugged.

The tester answers one precise question per port:

> When a normal USB device is plugged into this port, how far does the host
> get - no response, electrical signalling only, enumeration, full
> configuration with drivers - within a 30-second window?

Everything else in the tool exists to answer that question honestly and to
record the answer defensibly.

---

## 2. The hardware, and why this chip

The tester is a single **ESP32-S3-DevKitC-1**. The ESP32-S3 was chosen for one
specific reason: it has a **native USB device peripheral**. That lets the chip
*pretend to be* a USB device - a flash drive, a keyboard - rather than only
acting as a USB host or a serial gadget. Most microcontrollers, and the older
ESP32/ESP8266, cannot do this. That ability is the foundation of the whole
tool: to measure how a host treats a USB device, you must *be* a USB device.

The board has two USB connectors, and the firmware assigns them fixed roles to
remove any ambiguity:

| Connector | Role | Only ever connects to |
|---|---|---|
| **USB** (native) | the probe | the system under test |
| **UART** | the console | the laptop (flashing, reading reports) |

**The two are never connected at the same time.** This is the core of the
safety model: the board is either talking to the equipment *or* to the laptop,
never bridging the two. There is no path for data to flow from the system under
test to the laptop through the tester, because they are never both attached.

The ESP32-S3 also has WiFi and Bluetooth. Bluetooth is unused. WiFi is used
only for the optional live-read feature (section 10) and can be compiled out
entirely.

---

## 3. What happens when you plug the probe into a port

A USB connection goes through a fixed sequence of steps, defined by the USB
specification. The host (the equipment) drives every one of them; the device
(the tester) only responds. STING records the first time each step happens,
measured in milliseconds since the tester powered up. Those steps, in order:

1. **Power.** The port supplies 5 V and the board boots. If the board does not
   light up at all, the port supplies no power - which the operator records by
   hand, because a tester that never booted cannot log anything.

2. **Bus reset.** The host signals a USB bus reset - the electrical "I see
   something, let's start" handshake. Seeing this means the port is not just
   supplying dumb power; a USB host controller is active behind it.

3. **Enumeration.** The host asks the device to describe itself - it requests
   *descriptors* (device, configuration, string, etc.). **This is the pivotal
   step.** If the host requests descriptors, the port is *not* logically
   disabled: a USB stack is running and is willing to talk to whatever is
   plugged in.

4. **Configuration.** The host finishes setup and selects the device's
   configuration - the point at which it commits to using the device and loads
   class drivers.

5. **Mass storage.** The host's storage driver sends SCSI commands to the
   tester's fake drive: identify it, read its capacity, read sectors. This
   shows the storage class driver is active and the port will mount removable
   media.

6. **Keyboard (HID).** The host's keyboard driver attaches to the tester's
   keyboard interface. This shows the HID class driver is active and the port
   will accept an input device.

Each step is strictly "higher" than the last: you cannot configure without
enumerating, cannot read storage without configuring. So the furthest step
reached is a clean measure of how live the port is.

---

## 4. How the tester presents itself

When the probe is plugged in, the system under test sees an ordinary
**composite USB device** - one physical device offering two functions:

- **A small USB flash drive.** An 8 KB, read-only FAT12 volume labelled
  `STING TEST`. It is generated on the fly in the chip; nothing is ever written
  to it. It holds a plain-text `README.TXT` explaining what the device is, and
  optionally an antivirus test file (section 9).

- **A keyboard.** A standard HID keyboard that types one short, harmless marker
  string (section 8).

The device identifies itself as USB `303a:4006`, manufacturer `STING`, product
`USB Port Liveness Tester`, with the chip's MAC address as its serial number.
These identifiers are deliberately descriptive so the device is easy to find in
the host's own device logs and event logs - the tester *wants* to be
recognisable, not stealthy.

Presenting both a drive and a keyboard matters: a port may accept one class of
device but not another (for example, keyboards allowed, storage blocked by
policy). Offering both lets the report show exactly which class the port
accepted.

---

## 5. The verdicts

From the furthest step reached, the tester assigns one of three verdicts:

| Verdict | What was observed | What it means |
|---|---|---|
| **DISABLED** | Powered, but no enumeration within 30 s | The host never tried to talk to the device. Consistent with a properly disabled port. |
| **ENUMERATES** | Descriptors requested, but no class driver bound | The port is **logically live**: a USB stack responded, even if it did not finish. |
| **ACTIVE** | Configured, with storage and/or keyboard accepted | The port is **fully live**: it will mount media and/or accept input. |

The important line for an E26/E27 discussion is the boundary between DISABLED
and ENUMERATES. **Any result other than DISABLED means the port is not
logically disabled**, regardless of whether anything appeared on the operator's
screen.

A subtlety worth stating plainly: a port that is blocked only by an
operating-system *policy* (for example "USB mass storage denied") is still a
live port. It will typically show ENUMERATES, or ACTIVE with only the keyboard
accepted. The per-stage detail in the report shows which, so the distinction
between "the port is dead" and "the port is open but the OS refuses storage" is
visible, not hidden inside a single verdict.

---

## 6. The 30-second window and the LED

The tester observes for **30 seconds** before a DISABLED verdict is final. A
host may be slow to enumerate, so declaring a port dead early would be unsafe.

The onboard RGB LED shows the state live:

| LED | Meaning |
|---|---|
| Red | powered, no host activity yet |
| Yellow | enumeration seen |
| Green | fully configured, class drivers active |
| **Blinking** (any colour) | still observing - result not yet final |
| **Solid** (any colour) | window complete and saved - safe to unplug |
| Blue | laptop/console mode: this power-up is not being recorded |
| Magenta flashes | the result could not be saved (log full or flash error) |

The operator waits for the LED to go **solid** before unplugging. The blink/
solid distinction is the signal that the result is safely written to flash.

---

## 7. How a result becomes evidence: the session log

Each plug-in is one **session**. The observations are written to the chip's
internal flash (an NVS partition), so a result survives being unplugged and
carried back to the laptop. The log keeps the newest **200 sessions**; once
full, the oldest is dropped so recording never stops (a ring buffer). Each
session has an absolute number that only increases, so order is unambiguous.

Because the board has no real-time clock, all internal times are **relative to
power-up** (e.g. "t+0.498 s"). A real date/time is available only when a phone,
connected to the WiFi live read during a test, sends its clock to the board;
that session is then stamped with a real local date and time (section 10).
Without a phone, the operator records the date separately and uses the `label`
command to tag a session with a port name (e.g. `FDS-USB1`).

The `last` console command prints only the most recent session, and the WiFi
live view always shows just the current one, so neither requires scrolling
through the whole log.

Writing to flash is scheduled carefully: a flash write briefly stalls USB
interrupt handling, so the firmware saves a changed session only once the bus
has been quiet for a moment, never in the middle of enumeration. The final,
authoritative save happens when the 30-second window closes. This is why the
operator waits for the solid LED.

**Distinguishing a laptop connection from a port test.** The board cannot sense
which port is powering it. So every power-up begins as a provisional test
session, and the moment a valid console command arrives on the UART port, the
firmware concludes "this is the laptop, not a port under test", discards that
provisional session, and stops recording until the next reboot (the LED turns
blue). This is why, when reading reports, the operator types a command promptly
rather than leaving the board sitting on the laptop - and why the session count
should always be checked against the number of ports actually tested.

---

## 8. The keyboard marker

Once the host has accepted the keyboard, the tester types a single marker
string: `stingusbtest`. Its only purpose is to demonstrate - and let the
operator witness - that the port accepted keyboard input.

Deliberate constraints:

- The marker is limited to the characters `a-z` and `0-9`. It contains **no
  Enter, Tab, Space, modifier or function keys**, so it cannot submit a dialog,
  run a command, or trigger a shortcut that needs a modifier.
- It is typed once.
- It can be disabled entirely at build time (`MARKER_ENABLED 0`), in which case
  no keystrokes are sent at all and keyboard acceptance is still detected from
  the host's driver activity.

The marker lands in whatever window has input focus on the host. On an HMI,
even plain letters can be hotkeys, so this is agreed with the system owner
before testing, or the marker is turned off.

---

## 9. File-access evidence and the EICAR antivirus test

Reaching the mass-storage stage shows the port *will* mount media. The tester
goes further and records **which sectors the host read**, because that reveals
how deeply the host inspected the volume:

- **Boot sector only** - the host merely identified the disk.
- **Directory sector** - the host read the file listing.
- **File sector** - the host read a file's contents. This is direct evidence
  that **file exchange is possible** on the port, and it is how a host that
  auto-inspects inserted media (AutoPlay / autorun-style behaviour) reveals
  itself.

The tester never initiates a transfer. Every sector counted was requested by
the host; the tester only served what was asked for and recorded the request.

### EICAR

Optionally, the fake drive carries **`EICAR.COM`**. This holds the standard
**EICAR anti-malware test string** - a 68-byte pattern published by the
European Institute for Computer Antivirus Research specifically so that
antivirus software can be tested safely. **It is not malware.** It contains no
real malicious code and does nothing if run; every mainstream antivirus product
recognises it by prior agreement and treats it as a detection the same way it
would treat a real threat.

Its purpose here: to test whether the system under test runs antivirus that
scans USB media. If it does, reading the volume should make that antivirus
**alert on or quarantine** the file. The report notes whether the host read the
EICAR sector:

- If the host **read** the EICAR file, the operator then checks on the host
  side whether its antivirus reacted.
- If the host read the directory but **did not** read the EICAR file while
  reading others, an active antivirus may have blocked the read before it
  reached the device - itself a useful signal.

**How it is stored.** If the literal EICAR string sat in the firmware source or
in the compiled image, an antivirus on the *build* machine would quarantine
those files and break the build. So the string is stored XOR-obfuscated and is
reconstructed only in RAM, in the sector buffer, at the moment the host reads
it. The genuine, detectable signature therefore exists only on the fake drive
as the host reads it - never in the source tree or the firmware binary. The
feature can be compiled out entirely (`EICAR_ENABLED 0`).

---

## 10. The WiFi live read (optional)

So that results can be watched without a laptop, the tester can raise **its own
WiFi access point**. A phone joins that network and opens `http://192.168.4.1/`
in a browser, which shows the current test updating live (the stages lighting
up, the verdict, the sectors read) and links to the full stored report in the
same text form as the UART console.

While the page is open, the phone also sends its own clock to the board (its
browser knows the real time), so the session being viewed is stamped with a
real local date and time in the report - the board's only source of wall-clock
time, since it has no battery clock and is unpowered between tests.

This is a closed, point-to-point link: the tester *is* the access point, and
nothing is routed to any other network. The radio:

- is brought up about 1.5 seconds *after* USB starts, so it never competes with
  the timing-critical first enumeration;
- is a convenience for reading results, not part of the evidence chain;
- can be compiled out entirely (`WIFI_READ_ENABLED 0`) for a radio-silent,
  cleanly auditable build, or where transmitters are not permitted.

On the physics: the ESP32's radio is a ~100 mW 2.4 GHz transmitter, the same
band and power class as any phone or laptop already aboard, and will not
interfere with navigation or communications equipment on other bands. The
reason to consider leaving it off is not interference but governance: a device
that transmits while connected to controlled equipment is a different thing for
a system owner to authorise than a passive logger, and a radio-silent tool is
simpler to defend as evidence. The choice is the operator's, made with the
system owner.

---

## 11. Limitations - read before relying on a result

These are deliberately listed so results are not over-read:

- **"DISABLED" is a statement about this tester, on this port, at this time.**
  It means the host did not enumerate a full-speed composite device within 30
  seconds. It is strong evidence of a logically disabled port, not a
  mathematical proof that no device could ever work.
- **Wait for the solid LED.** A port with no activity is only recorded when the
  window closes. Unplugged earlier, it leaves no record. A port that *did* show
  activity and is unplugged early is kept and marked `INCOMPLETE`.
- **Times are relative.** No real-time clock; record the date and port name
  yourself, and use `label`.
- **Check the session count** against the number of ports tested, in case a
  laptop power-up was accidentally recorded (section 7).
- **The marker types into whatever has focus** on the host. Agree this with the
  owner or disable it.
- **The host may briefly log a second device.** For a fraction of a second
  before this firmware starts, the ESP32-S3 presents its own built-in
  USB-serial debug unit (`303a:1001`). A live host may record that. It comes
  from the chip and does not affect the tester's own log.
- **AV result must be confirmed host-side.** The tester shows whether the EICAR
  file was read; whether the host's antivirus reacted is verified on the host.

---

## 12. Safety rules

- **One connection at a time** - never the laptop and the system together.
- **The keyboard types only the harmless marker string** - never commands.
- **EICAR is an inert antivirus test file**, not malware.
- **Get the system owner's permission** before testing live equipment, and
  agree in advance on the keyboard marker and whether the WiFi radio may be
  used.

---

## 13. Source map

| File | Responsibility |
|---|---|
| `main/main.c` | session loop: when to save, LED state, laptop-vs-test logic |
| `main/usb_probe.c` | the USB device (TinyUSB) and every observation it records |
| `main/fake_disk.c` | the read-only FAT12 volume, README and EICAR |
| `main/store.c` | the session log in flash (NVS) |
| `main/report.c` | the report text (shared by the UART console and WiFi) |
| `main/cli.c` | the UART console commands |
| `main/webread.c` | the WiFi access point and live-read web server |
| `main/led.c` | the RGB LED |
| `main/config.h` | all build-time settings |
| `tests/host/` | PC-side checks of the volume, verdict rules and report text |
