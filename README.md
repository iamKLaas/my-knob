# my-knob: a surfer game on an ESP32-S3 round knob display

A small hobby firmware project in C on ESP-IDF. A surfer rides waves on a round 360×360 display.
The rotary knob moves the surfer between five wave lanes to dodge obstacles (buoys, logs, shark fins).
It started from the knob-starter template of the
IDA Embedded Workshop (MIT licensed, copyright Christian Dalager, see `LICENSE`) and I extended it with
agentic coding (Claude Code, Copilot) while learning how the pieces fit together.

## What comes from the workshop and what I added

| From the workshop template | Added in this project |
|---|---|
| Board, display (ST77916), touch (CST816) and knob bring-up | The sea scene: five lanes of waves and the surfer (`components/app/app_sea.c`, LVGL) |
| LVGL setup, theme tokens, haptic driver (DRV2605L over I2C) | The game model in plain C: lane selection with the knob, waves per lane, obstacles that drift in, falling and climbing back, grace period (`sea_model.c`) |
| Wi-Fi component, streamed upload, HTTP server | Host unit tests for the game model (`test/host/test_sea_model.c`) |
| Build with Docker, flash scripts, serial test console | Tuning of speeds, hit distance and timings |

The game model has no LVGL or ESP-IDF dependency, so it runs and is tested on the laptop with gcc and Unity.
The LVGL part only draws the model's state.

## Hardware and software

- Board: Guition JC3636K518C / Waveshare ESP32-S3 Knob Touch LCD 1.8
  (ESP32-S3R8, 360×360 round display, capacitive touch, rotary knob, haptic driver)
- ESP-IDF 5.3, LVGL 9, C
- Unit tests with Unity on the host, build in Docker, flashing with esptool

## Honest status

This is a learning project. The template and the workshop skills gave me a head start, and I used AI
tools for much of the game code. I am still learning the details underneath (knob decoding, display
driver, memory use with PSRAM). Next steps: read the knob driver and the display bring-up line by line,
and add a short video and photos here.

---

# knob-starter (original template documentation)

The smallest honest firmware for the Guition JC3636K518C / Waveshare ESP32-S3-Knob-Touch-LCD-1.8
(ESP32-S3R8, 1.8" 360×360 round ST77916 display, CST816 touch, rotary knob, DRV2605L haptics).
Turn the dial to pick which wave the surfer rides, hold to stream 2 MiB to a laptop over Wi-Fi.
About 2,500 lines of hand-written C (the generated font, the display init table and vendored Unity are not counted). Most files stay under 200 lines; `components/app/app_sea.c` (the sea scene) is the exception at about 275. Pure logic is host-tested with gcc + Unity.

## Layout

```
components/board/   pins + capabilities (the ONLY place a GPIO number may appear; linted)
components/ui/      ST77916 + CST816 + LVGL bring-up, knob driver, backlight, Muli 28 font
components/theme/   design tokens (tokens.h) + colour = state policy, no LVGL dependency
components/haptic/  DRV2605L on the touch I2C bus
components/net/     Wi-Fi STA, buffered GET, streamed PUT through one 16 KB PSRAM chunk,
                    HTTP server (GET /status, POST /data) with host-tested admission rules
components/app/     the state machine (app.c), its screen, the sea scene with the surfer
                    (app_sea.c, LVGL) and its pure game model (sea_model.c), the upload
                    exercise, the sink for data POSTed to the knob (app_serve.c)
components/testctl/ dev-only serial test console (CONFIG_KNOB_TESTCTL): inject knob/hold, snapshot
main/main.c         boot order
test/host/          Unity suites: board pins, knob decoder, sea model (game logic), theme wiring, upload part maths, server admission rules
scripts/            build.sh (Docker), flash.sh (host esptool), test.sh (gcc), gen-fonts.sh,
                    testctl.py + verify-screens.sh (drive the device over USB, see verify-on-device)
design/             tokens.json / tokens.h, principles.md, fonts + OFL licences
```

## Build, flash, test

```bash
cp components/net/include/wifi_secrets.example.h components/net/include/wifi_secrets.h  # edit it
scripts/build.sh                 # docker espressif/idf:release-v5.3 → build/knob_starter.bin
scripts/flash.sh                 # host esptool --no-stub over /dev/ttyACM0 (full image)
scripts/flash.sh /dev/ttyACM0 app   # later: app image only
scripts/test.sh                  # host unit tests + GPIO-literal lint, no IDF needed
python3 receiver.py              # on the laptop, for the upload exercise (receiver.py is in templates/receiver of the IDA Embedded Workshop repo, not included here)
scripts/testctl.py knob +3       # inject 3 detents over USB  -> state=idle counter=3
scripts/testctl.py snap shot.png # pixel-exact LVGL screenshot
scripts/verify-screens.sh        # scripted state walk with snapshots, evidence in build/verify/
```

Flash from the host, never from the container (no serial access there). `--no-stub` is required on
this board's native USB. On the K518 the USB-C plug orientation picks the chip: `303a:1001`
(`/dev/ttyACM0`) is the S3 you flash; `1a86:7523` (`/dev/ttyUSB0`) is the Bluetooth sidecar.

Verified green with ESP-IDF 5.3.5, lvgl 9.6.0~1, esp_lvgl_port 2.9.0, esp_lcd_st77916 1.0.1,
esp_lcd_touch_cst816s 1.1.2, esp_lcd_touch 1.2.1 (pinned in `dependencies.lock`).
Image 1.36 MB of a 9 MB app partition; LVGL's 128 KB pool lives in PSRAM.

## API summary

| Module | Calls |
|---|---|
| ui | `ui_init()`, `ui_i2c_bus()`, `ui_lock(ms)` / `ui_unlock()` |
| knob | `knob_init(on_step)` — `on_step(+1/-1)` runs on the LVGL task; `knob_take_steps()` |
| backlight | `backlight_init()`, `backlight_set_percent(pct)` |
| theme | `theme_state_color(state)`, `theme_state_motion_ms(state)`; tokens `HUD_*` |
| haptic | `haptic_init(bus)`, `haptic_click()`, `haptic_play(effect)` |
| net | `net_init()`, `net_is_connected()`, `net_wait_connected(ms)`, `net_rssi()`, `net_wifi_join(ssid, pass)`, `net_http_get(url, buf, cap)`, `net_http_put_stream(url, len, rewind_cb, read_cb, ctx, progress_cb)`, `net_set_result_cb(cb)`, `net_serve_start(sink, max_body)` |
| app | `app_init()`; states `APP_IDLE / UPLOADING / DONE / ERROR`; test hooks `app_inject_knob/hold/reset()`, `app_state_line()` |
| testctl | `testctl_init()` — USB Serial/JTAG console: `knob ±N`, `hold`, `reset`, `state`, `snap`, `ping` |

Rules the code demonstrates: LVGL objects are touched only on the LVGL task (callbacks, timers)
or inside `ui_lock()`; the network runs on its own task and reports back through a callback and
atomics; nothing is ever buffered whole — the upload pulls bytes from a `read_cb` through one
shared chunk; every pin is a `BOARD_*` macro.

## Exercise ladder (from the workshop template)

These exercises come with the template; I have not done most of them. The code here has since
changed: the knob now also steers the surfer, and the verify script only checks the knob counter,
not the sea scene.

1. **Hello screen** — change the title label; rebuild, flash `app`, see it.
2. **Knob counter** — make the counter wrap at 0..99 (`on_knob_step` in `app.c`).
3. **Touch target** — add a second tap zone (an `lv_obj` with its own event callback).
4. **Colour = state** — add `APP_PAUSED` mapped to `THEME_STATE_PAUSED`; the ring goes amber.
5. **Haptic cue** — a different `haptic_play()` effect per state change.
6. **Host test** — move the wrap logic from rung 2 into a pure `.c` and test it in `test/host`.
7. **Wi-Fi + RSSI** — already on screen; show "weak" below −75 dBm.
8. **GET JSON** — `net_http_get()` a value (e.g. a time API) from a network task; render it.
9. **Stream 2 MiB** — hold the screen; watch the arc and `heap[before put]` / `heap[after put]`.
10. **Survive a drop** — pull the AP mid-upload; add `X-Offset` resume using the receiver's GET.
11. **Be called** (stretch) — `curl --data '{"value":42}' http://<knob-ip>/data`; keep the body in
    the sink (`app_serve.c`), parse and show the number on the LVGL tick.

After each rung, extend `scripts/verify-screens.sh` with the state you added and run it.

Hints: never call `net_*` from an LVGL callback (rung 8 needs `xTaskCreate`); if the panel is
dark the backlight did not init; if colours are wrong you inverted twice.
