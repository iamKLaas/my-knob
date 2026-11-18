// app_sea.c — the water is rendered by the CPU into one RGB565 buffer (PSRAM) shown on a canvas:
// per frame, one pass over the pixels fills the vertical colour fade between each pair of wave
// surfaces. That is far cheaper than hundreds of gradient rects (which held LVGL to ~5 fps) and the
// panel flush only ever sees the finished frame. The surfer is a separate pixel-art object on top.
#include "app_sea.h"
#include "sea_model.h"
#include "theme.h"
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#define SEA_TICK_MS     40
#define SEA_MAX_DT_MS   120                       // never let one slow frame jump the animation
#define SEA_TOP         ((int)SEA_LANE_Y0 - 12)   // sky above the farthest lane
#define SEA_H           (HUD_PANEL_DIA - SEA_TOP)
#define GRAD_STEPS      64
#define HALF            (HUD_PANEL_DIA / 2)

// 8-bit-style surfer: 16 x 11 body sprite at SPX px per pixel, plus a board drawn as a pixel run
// whose rows step with the wave slope (no rotation: tilted sprites would smear the pixel art).
#define SPX             3
#define SPR_W           16
#define SPR_H           11
#define BOARD_ROW       (SPR_H + 1)               // board rows; one spare row above/below for tilt
#define SURFER_W        (SPR_W * SPX)
#define SURFER_H        ((SPR_H + 5) * SPX)
#define TILT_SLOPE      0.2f                     // |slope| above this tilts the board a step

// Scene palette (art, not UI state — the ring/arc keep the theme state colours).
#define C_SKY    0x9ADCFF
// Colour of the water at each wave surface, far (light) to the bottom of the screen (deep). The
// water between two neighbouring surfaces fades from one colour to the next, so the lanes read as
// one continuous body of water instead of separate stripes.
static const uint32_t SURF_COL[SEA_LANES + 1] = {
    0x74CCFF, 0x58B4F8, 0x409CF0, 0x2C84E4, 0x1C6CD0, 0x1050B0,
};

static const char *const SPRITE[SPR_H] = {   // h hair, s skin, r trunks
    ".....hhhh.......",
    "....hhhhhh......",
    "....ssssss......",
    "....ssssss......",
    ".....ssss.......",
    "..sssrrrrsss....",
    "..s..rrrr..s....",
    ".....rrrr.......",
    ".....ss.ss......",
    ".....ss.ss......",
    ".....ss.ss......",
};

// Floating obstacles, drawn into the water buffer at OSPX px per art pixel, bottom row at the waterline.
#define OSPX            2
typedef struct { const char *const *rows; int w, h; } obst_art_t;
static const char *const ART_BUOY[] = {   // r red, w white, k dark
    "...k...", "..rrr..", "..www..", ".rrrrr.", ".wwwww.", ".rrrrr.", "..rrr..",
};
static const char *const ART_LOG[] = {    // b bark, l cut end
    "..bbbbbbbbbbbb..", ".llbbbbbbbbbbll.", ".llbbbbbbbbbbll.", "..bbbbbbbbbbbb..",
};
static const char *const ART_FIN[] = {    // g grey, d shadow, w foam
    ".....g.....", ".....gg....", "....gggg...", "....gggg...", "...ggggg...", "..dgggggg..", "wwwwwwwwwww",
};
static const obst_art_t OBST_ART[OBST_KINDS] = {
    { ART_BUOY, 7, 7 }, { ART_LOG, 16, 4 }, { ART_FIN, 11, 7 },
};

static lv_obj_t   *s_sea, *s_surfer;
static sea_model_t s_model;

static uint16_t *s_buf;                                  // SEA_H rows of HUD_PANEL_DIA RGB565 pixels
static uint16_t  s_lut[SEA_LANES][GRAD_STEPS];           // fade from surface l's colour to l+1's
static uint16_t  s_sky565;
static int16_t   s_top[SEA_LANES + 1][HUD_PANEL_DIA];    // y of each surface per column; [LANES] = bottom
static uint16_t  s_inv[SEA_LANES][HUD_PANEL_DIA];        // gradient steps per px, 8.8 fixed point

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)(((c >> 19) & 0x1F) << 11 | ((c >> 10) & 0x3F) << 5 | ((c >> 3) & 0x1F));
}

static void build_palette(void)
{
    s_sky565 = rgb565(C_SKY);
    for (int l = 0; l < SEA_LANES; l++) {
        uint32_t a = SURF_COL[l], b = SURF_COL[l + 1];
        for (int k = 0; k < GRAD_STEPS; k++) {
            int ch[3];
            for (int sh = 16, c = 0; sh >= 0; sh -= 8, c++)
                ch[c] = (int)(((a >> sh) & 0xFF) + ((int)((b >> sh) & 0xFF) - (int)((a >> sh) & 0xFF)) * k / (GRAD_STEPS - 1));
            s_lut[l][k] = rgb565((uint32_t)ch[0] << 16 | (uint32_t)ch[1] << 8 | (uint32_t)ch[2]);
        }
    }
}

static uint16_t obst_colour(char c)
{
    switch (c) {
    case 'r': return rgb565(0xE03030);
    case 'w': return rgb565(0xFCFCFC);
    case 'k': return rgb565(0x2A2A5A);
    case 'b': return rgb565(0x7A4A1E);
    case 'l': return rgb565(0xD8A860);
    case 'g': return rgb565(0x7C8CA0);
    default:  return rgb565(0x3C4A60);   // d
    }
}

static void draw_obstacles(void)
{
    for (int i = 0; i < SEA_OBST_MAX; i++) {
        const sea_obstacle_t *o = &s_model.obst[i];
        if (!o->active) continue;
        const obst_art_t *a = &OBST_ART[o->kind];
        int x0 = HALF + (int)lroundf(o->x) - a->w * OSPX / 2;
        int y0 = (int)lroundf(sea_model_obstacle_y(&s_model, i)) - a->h * OSPX + OSPX - SEA_TOP;   // bottom row dips 1 px in
        for (int r = 0; r < a->h * OSPX; r++) {
            int y = y0 + r;
            if (y < 0 || y >= SEA_H) continue;
            for (int q = 0; q < a->w * OSPX; q++) {
                int x = x0 + q;
                char ch = a->rows[r / OSPX][q / OSPX];
                if (x < 0 || x >= HUD_PANEL_DIA || ch == '.') continue;
                s_buf[y * HUD_PANEL_DIA + x] = obst_colour(ch);
            }
        }
    }
}

static void render_sea(void)
{
    for (int x = 0; x < HUD_PANEL_DIA; x++) {
        for (int l = 0; l < SEA_LANES; l++)
            s_top[l][x] = (int16_t)lroundf(sea_model_lane_y(l) + sea_model_wave_y(&s_model, (float)(x - HALF), l));
        s_top[SEA_LANES][x] = HUD_PANEL_DIA;
        for (int l = 0; l < SEA_LANES; l++) {
            int h = s_top[l + 1][x] - s_top[l][x];
            s_inv[l][x] = h > 0 ? (uint16_t)((GRAD_STEPS << 8) / h) : 0;
        }
    }
    int8_t band[HUD_PANEL_DIA];                           // -1 = still in the sky
    for (int x = 0; x < HUD_PANEL_DIA; x++) band[x] = -1;
    for (int y = SEA_TOP; y < HUD_PANEL_DIA; y++) {
        uint16_t *row = s_buf + (y - SEA_TOP) * HUD_PANEL_DIA;
        for (int x = 0; x < HUD_PANEL_DIA; x++) {
            int b = band[x];
            while (y >= s_top[b + 1][x]) b++;             // s_top[LANES] = panel height stops it
            band[x] = (int8_t)b;
            if (b < 0) { row[x] = s_sky565; continue; }
            int k = ((y - s_top[b][x]) * s_inv[b][x]) >> 8;
            row[x] = s_lut[b][k < GRAD_STEPS ? k : GRAD_STEPS - 1];
        }
    }
    draw_obstacles();
    lv_obj_invalidate(s_sea);
}

static int s_tilt;                            // -1 nose down, 0 flat, +1 nose up (to the left, the way he travels)

static void px(lv_layer_t *layer, lv_draw_rect_dsc_t *d, const lv_area_t *o, int x, int y, uint32_t c)
{
    lv_area_t a = { o->x1 + x * SPX, o->y1 + y * SPX, o->x1 + x * SPX + SPX - 1, o->y1 + y * SPX + SPX - 1 };
    d->bg_color = lv_color_hex(c);
    lv_draw_rect(layer, d, &a);
}

static void draw_riding(lv_layer_t *layer, lv_draw_rect_dsc_t *d, const lv_area_t *o, int body_dy)
{
    for (int y = 0; y < SPR_H; y++) {
        for (int x = 0; x < SPR_W; x++) {
            char c = SPRITE[y][x];
            if (c == '.') continue;
            px(layer, d, o, x, y + 1 + body_dy, c == 'h' ? 0x5C2E0A : c == 'r' ? 0xE03030 : 0xF8B878);
        }
    }
    for (int x = 1; x < SPR_W - 1; x++) {        // board: dark outline rows around a yellow deck
        int dy = s_tilt * (7 - x) / 4;            // nose (left) rises when tilt > 0
        px(layer, d, o, x, BOARD_ROW + 1 - dy, 0x2A2A5A);
        px(layer, d, o, x, BOARD_ROW + 2 - dy, x == 7 || x == 8 ? 0xFCFCFC : 0xFCD83C);
        px(layer, d, o, x, BOARD_ROW + 3 - dy, 0x2A2A5A);
    }
}

// In the water: head and a waving arm at the surface, foam around him, the board floating upside down.
static void draw_fallen(lv_layer_t *layer, lv_draw_rect_dsc_t *d, const lv_area_t *o)
{
    int bob = (int)(s_model.fall_ms / 200.0f) & 1;
    for (int y = 0; y < 5; y++) {
        for (int x = 3; x < 11; x++) {
            char c = SPRITE[y][x];
            if (c == '.') continue;
            px(layer, d, o, x, y + 9 + bob, c == 'h' ? 0x5C2E0A : 0xF8B878);
        }
    }
    px(layer, d, o, 2, 10 + bob, 0xF8B878);      // arm up
    px(layer, d, o, 2, 11 + bob, 0xF8B878);
    for (int x = 2; x < 12; x++) px(layer, d, o, x, BOARD_ROW + 1, (x + bob) & 1 ? 0xFCFCFC : 0xB8E8FF);   // foam
    for (int x = 10; x < SPR_W; x++) {
        px(layer, d, o, x, BOARD_ROW + 1 + bob, 0x2A2A5A);   // upturned board drifting off
        px(layer, d, o, x, BOARD_ROW + 2 + bob, 0xFCFCFC);
    }
}

static void draw_surfer(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t o;
    lv_obj_get_coords(s_surfer, &o);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;

    int fall = sea_model_fall_state(&s_model);
    if (fall == 1) draw_fallen(layer, &d, &o);
    else if (fall == 2) draw_riding(layer, &d, &o, 3);                     // pulling himself back on
    else if (s_model.safe_ms <= 0.0f || ((int)(s_model.safe_ms / 120.0f) & 1)) draw_riding(layer, &d, &o, 0);   // blinks while safe
}

static void place_surfer(void)
{
    int y = (int)lroundf(sea_model_surfer_y(&s_model, s_model.surfer_x));
    lv_obj_set_pos(s_surfer, HALF + (int)lroundf(s_model.surfer_x) - SURFER_W / 2,
                   y - (BOARD_ROW + 2) * SPX + SPX);
    float slope = sea_model_surfer_slope(&s_model);      // > 0: water ahead (left) is higher
    int tilt = slope > TILT_SLOPE ? 1 : slope < -TILT_SLOPE ? -1 : 0;
    if (tilt != s_tilt) s_tilt = tilt;
    lv_obj_invalidate(s_surfer);
}

static void on_tick(lv_timer_t *t)
{
    static uint32_t last_ms;
    uint32_t now = lv_tick_get();
    uint32_t dt = last_ms ? now - last_ms : SEA_TICK_MS;
    last_ms = now;
    if (dt > SEA_MAX_DT_MS) dt = SEA_MAX_DT_MS;
    (void)t;
    sea_model_tick(&s_model, dt);
    render_sea();
    place_surfer();
}

void app_sea_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(C_SKY), 0);   // sky above the waterline

    s_buf = heap_caps_malloc((size_t)HUD_PANEL_DIA * SEA_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) { ESP_LOGE("sea", "no PSRAM for the sea buffer"); return; }
    build_palette();
    s_sea = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_sea, s_buf, HUD_PANEL_DIA, SEA_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_sea, 0, SEA_TOP);
    lv_obj_set_clickable(s_sea, false);            // taps fall through to the screen
    render_sea();

    s_surfer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_surfer);
    lv_obj_set_size(s_surfer, SURFER_W, SURFER_H);
    lv_obj_set_clickable(s_surfer, false);
    lv_obj_add_event_cb(s_surfer, draw_surfer, LV_EVENT_DRAW_MAIN, NULL);
    place_surfer();

    lv_timer_create(on_tick, SEA_TICK_MS, NULL);
}

void app_sea_nudge(int dir)
{
    sea_model_nudge(&s_model, dir);
}

void app_sea_reset(void)
{
    s_model = (sea_model_t){0};
    place_surfer();
}
