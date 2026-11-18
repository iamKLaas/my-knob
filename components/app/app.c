// app.c — the state machine and its inputs. Rules this file demonstrates:
//   • knob and touch callbacks run on the LVGL task → touch widgets directly, no lock needed;
//   • the upload runs on its own task and NEVER touches LVGL — a 200 ms lv_timer polls its
//     atomics and updates the arc/status, so there is exactly one owner of the screen;
//   • data POSTed to the knob arrives on the HTTP server task and is handed over the same way.
#include "app.h"
#include "app_screen.h"
#include "app_sea.h"
#include "app_upload.h"
#include "app_serve.h"
#include "ui.h"
#include "knob.h"
#include "haptic.h"
#include "net.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>

static const char *TAG = "app";

static app_state_t s_state = APP_IDLE;
static int         s_counter;
static uint32_t    s_rx_bytes;       // size of the last body POSTed to the knob
static int         s_rx_ticks;       // status-line ticks left to show it

#define RX_SHOW_TICKS 15             // 15 x 200 ms = 3 s

static void set_state(app_state_t next)
{
    if (next == s_state) return;
    ESP_LOGI(TAG, "state %d -> %d", (int)s_state, (int)next);
    s_state = next;
    app_screen_set_state(next);
}

// ---- inputs (LVGL task) ----

static void on_knob_step(int dir)
{
    s_counter += dir;
    app_sea_nudge(dir);
    app_screen_set_counter(s_counter);
}

static void on_long_press(void)
{
    if (s_state == APP_UPLOADING) return;
    haptic_click();
    if (app_upload_start()) {
        app_screen_set_progress(0, 1);
        set_state(APP_UPLOADING);
    } else {
        set_state(APP_ERROR);
    }
}

static void on_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) on_long_press();
}

// ---- test hooks (caller holds the LVGL lock) ----

void app_inject_knob(int steps)
{
    int dir = steps < 0 ? -1 : 1;
    for (int i = 0; i < (steps < 0 ? -steps : steps); i++) on_knob_step(dir);
}

void app_inject_hold(void) { on_long_press(); }

void app_inject_reset(void)
{
    s_counter = 0;
    app_sea_reset();
    app_screen_set_counter(0);
    set_state(APP_IDLE);
}

void app_state_line(char *buf, size_t cap)
{
    static const char *const NAMES[] = { "idle", "uploading", "done", "error" };
    snprintf(buf, cap, "state=%s counter=%d", NAMES[s_state], s_counter);
}

// ---- 200 ms status tick (LVGL task) ----

static void show_idle_status(void)
{
    char line[48];
    if (s_rx_ticks > 0) {
        s_rx_ticks--;
        snprintf(line, sizeof(line), "received %u B", (unsigned)s_rx_bytes);
    }
    else line[0] = '\0';                    // Wi-Fi still runs; its status just isn't shown
    app_screen_set_status(line);
}

static void on_tick(lv_timer_t *t)
{
    (void)t;
    uint64_t sent, total; bool done, ok;
    app_upload_poll(&sent, &total, &done, &ok);
    if (app_serve_poll(&s_rx_bytes)) {      // another device POSTed something to us
        s_rx_ticks = RX_SHOW_TICKS;
        haptic_click();
    }

    if (s_state == APP_UPLOADING) {
        app_screen_set_progress(sent, total);
        char line[48];
        snprintf(line, sizeof(line), "%u / %u KB", (unsigned)(sent / 1024), (unsigned)(total / 1024));
        app_screen_set_status(line);
    } else {
        show_idle_status();
    }
    if (done) set_state(ok ? APP_DONE : APP_ERROR);
}

void app_init(void)
{
    if (!ui_lock(1000)) { ESP_LOGE(TAG, "no LVGL lock — no screen"); return; }
    app_screen_build();
    lv_obj_add_event_cb(app_screen_touch_target(), on_touch, LV_EVENT_LONG_PRESSED, NULL);
    lv_timer_create(on_tick, 200, NULL);
    ui_unlock();

    knob_init(on_knob_step);   // takes the lock itself
    if (!app_serve_start()) ESP_LOGW(TAG, "HTTP server not started");
    ESP_LOGI(TAG, "ready: turn = steer surfer, hold = upload %u KB", APP_UPLOAD_BYTES / 1024);
}
