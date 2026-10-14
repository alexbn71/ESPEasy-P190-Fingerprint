# ESPEasy plugin P251 – Fingerprint reader (Grow R503 / R307)

[ESPEasy](https://github.com/letscontrolit/ESPEasy) plugin for the **Grow fingerprint modules** on a
serial port: R503 (round, with RGB LED ring), R307, R502, ZFM-20, AS608 and compatibles.

- Recognises a finger and sends its ID and score to the controllers and the rules
- Enrol, delete and count fingers with commands (from rules, HTTP or MQTT)
- R503 LED ring: automatic feedback (recognised / not recognised / enrolling) and a `led` command
- Touch detection on the WAKEUP pin, or polling when it is not wired
- Non blocking: the protocol runs as a state machine, ESPEasy keeps running while the module works

Built against ESPEasy `mega-20260720` on ESP32.

## Files

| File | Content |
|---|---|
| `src/_P251_Fingerprint.ino` | the plugin (settings page) |
| `src/src/PluginStructs/P251_data_struct.*` | state machine: scan, enrolment, commands |
| `src/src/PluginStructs/P251_protocol.h` | packet encoding / decoding, no ESPEasy dependencies |
| `test/test_protocol.cpp` | host unit tests of the protocol |

## Adding it to an ESPEasy build

1. Copy the `src/` folder of this repository over the `src/` folder of ESPEasy.
2. Enable the plugin in `src/Custom.h` of ESPEasy:
   ```c
   #define USES_P251   // Security - Fingerprint (Grow R503/R307)
   ```
3. Build a custom environment, e.g. `pio run -e custom_ESP32_4M316k`.

ESPEasy already reserves the slot of plugin 251, so no other file needs to be changed.

The [Build](.github/workflows/build.yml) workflow runs the unit tests and builds a small test firmware
(Switch, SysInfo, Dummy, P251) as the `espeasy-p251-test-firmware` artifact.

## Wiring (R503)

| Wire | | ESP32 |
|---|---|---|
| Red | VCC **3.3 V** (not 5 V) | 3V3 |
| Black | GND | GND |
| Yellow | TX of the module | RX GPIO of the task (e.g. GPIO16) |
| Green | RX of the module | TX GPIO of the task (e.g. GPIO17) |
| Blue | WAKEUP: low while a finger touches the sensor | optional *Touch* GPIO (e.g. GPIO4) |
| White | 3.3VT: power of the touch circuit | 3V3 |

With the *Touch* GPIO the module is only queried when a finger is on the sensor. Without it the
module is queried every *Scan interval* (default 250 ms).

## Settings

*Devices → Add → "Security - Fingerprint (Grow R503/R307)"*

| Setting | Default | |
|---|---|---|
| Serial port, GPIO RX / TX | – | hardware serial recommended (ESP32: Serial1/Serial2) |
| Touch (WAKEUP) | – | optional input GPIO |
| Baud rate | 57600 | factory setting of the module |
| Password / Address | 0x00000000 / 0xFFFFFFFF | factory settings |
| Scan interval | 250 ms | only without the touch GPIO |
| Minimum score | 0 | lower scores are reported as not recognised |
| Show result on the LED ring | on | flashes twice in the *Recognised* (blue) or *Not recognised* (red) color |

The device page shows whether the module answers and how many of its slots are used (R503: 200).

## Values

| Value | |
|---|---|
| `ID` | slot of the recognised finger (0..199 on the R503), **-1** = not recognised |
| `Score` | match score from the module (higher = better) |
| `Count` | number of fingers stored in the module |

They are sent once per touch, when a finger is scanned: the rules get `<taskname>#ID`,
`<taskname>#Score` and `<taskname>#Count`. The module must see the finger lifted before it scans again.

## Commands

| Command | |
|---|---|
| `fingerprint,enroll,<id>` | store a new finger at slot `<id>` (overwrites it): place, lift, place again |
| `fingerprint,delete,<id>` | delete slot `<id>` |
| `fingerprint,deleteall` | delete all fingers |
| `fingerprint,count` | read the number of stored fingers (event `#Count`) |
| `fingerprint,led,<mode>,<color>[,<speed>[,<times>]]` | LED ring, see below |
| `fingerprint,cancel` | stop an enrolment |

LED ring: `<mode>` = `breathing`, `flash`, `on`, `off`, `fadein`, `fadeout` (or 1..6), `<color>` =
`red`, `blue`, `purple`, `green`, `yellow`, `cyan`, `white` (or 1..7; green to white need the full RGB
R503), `<speed>` 0..255 (default 100), `<times>` 0..255 (0 = forever, default).
Example: `fingerprint,led,breathing,blue`.

Commands reach every P251 task: use a single P251 task per ESP.

## Events

| Event | |
|---|---|
| `<taskname>#EnrollScan=<n>` | enrolment: place the finger (capture `<n>` = 1 or 2) |
| `<taskname>#EnrollRemove=<n>` | enrolment: lift the finger |
| `<taskname>#Enrolled=<id>` | enrolment done |
| `<taskname>#EnrollFailed=<code>` | enrolment failed, see the codes below |
| `<taskname>#Deleted=<id>` | slot deleted, -1 = all |
| `<taskname>#Count=<n>` | answer to `fingerprint,count` |
| `<taskname>#Error=<code>` | `delete`, `deleteall`, `count` or `led` failed |

Error codes: `10` the two captures are not the same finger, `11` slot out of range, `24` flash
error, `253` cancelled, `254` no finger placed / lifted within 20 s, `255` no answer from the module.
Other values are confirmation codes of the module (see its datasheet).

## Rules example

Task named `Finger`, opening a door relay on GPIO26 for 2 seconds for fingers 0..9:

```
on Finger#ID do
  if %eventvalue1% >= 0 and %eventvalue1% < 10
    Pulse,26,1,2000
  endif
endon

on Finger#EnrollScan do
  LogEntry,'Place the finger (%eventvalue1%/2)'
endon

on Finger#Enrolled do
  LogEntry,'Finger stored at %eventvalue1%'
endon
```

Enrol from the browser: `http://<esp-ip>/control?cmd=fingerprint,enroll,0`.

## License

Like ESPEasy, this plugin is released under the [GNU General Public License v3.0](LICENSE)
(or, at your option, any later version).
