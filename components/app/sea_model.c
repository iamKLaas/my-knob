// sea_model.c — see sea_model.h.
#include "sea_model.h"
#include <math.h>

#define TWO_PI          6.28318530718f
#define EASE_TAU_MS     180.0f      // larger = lazier lane change
#define PHASE_RAD_PER_S 2.4f        // base wave speed; lanes scale it for depth

// Per lane, far to near: wavelength (px), speed factor, height (px), a second harmonic for variety.
static const float WAVELEN[SEA_LANES] = { 120.0f, 140.0f, 160.0f, 190.0f, 220.0f };
static const float SPEED[SEA_LANES]   = { 0.6f,   0.8f,   1.0f,   1.3f,   1.6f   };
static const float AMP[SEA_LANES]     = { 6.0f,   9.0f,   13.0f,  17.0f,  SEA_WAVE_AMP };

void sea_model_nudge(sea_model_t *m, int dir)
{
    int t = m->lane_target + (dir < 0 ? -1 : 1);
    m->lane_target = t < 0 ? 0 : (t > SEA_LANES - 1 ? SEA_LANES - 1 : t);
}

static unsigned rnd(sea_model_t *m)
{
    m->rng = m->rng * 1664525u + 1013904223u;
    return m->rng >> 8;
}

static void spawn(sea_model_t *m)
{
    for (int i = 0; i < SEA_OBST_MAX; i++) {
        if (m->obst[i].active) continue;
        // Half of them aim at the lane he is on, so he has to react; the rest fill the sea.
        int lane = rnd(m) % 2 ? m->lane_target : (int)(rnd(m) % SEA_LANES);
        m->obst[i] = (sea_obstacle_t){ 1, (int)(rnd(m) % OBST_KINDS), lane, OBST_SPAWN_X };
        return;
    }
}

static void obstacles_tick(sea_model_t *m, float dt_ms)
{
    m->spawn_ms -= dt_ms;
    if (m->spawn_ms <= 0.0f) { spawn(m); m->spawn_ms = 900.0f + (float)(rnd(m) % 900u); }

    int ground = m->fall_ms <= 0.0f && m->safe_ms <= 0.0f;
    for (int i = 0; i < SEA_OBST_MAX; i++) {
        sea_obstacle_t *o = &m->obst[i];
        if (!o->active) continue;
        o->x -= OBST_SPEED * dt_ms / 1000.0f;
        if (o->x < -OBST_SPAWN_X) { o->active = 0; continue; }
        if (ground && fabsf(o->x - m->surfer_x) < OBST_HIT_DX && fabsf(m->lane - (float)o->lane) < OBST_HIT_LANE) {
            m->fall_ms = (float)SURFER_FALL_MS;
            m->hits++;
            o->active = 0;
            ground = 0;
        }
    }
    if (m->fall_ms > 0.0f) {
        m->fall_ms -= dt_ms;
        if (m->fall_ms <= 0.0f) { m->fall_ms = 0.0f; m->safe_ms = (float)SURFER_SAFE_MS; }
    } else if (m->safe_ms > 0.0f) {
        m->safe_ms -= dt_ms;
    }
}

void sea_model_tick(sea_model_t *m, unsigned dt_ms)
{
    float dt = (float)dt_ms / 1000.0f;
    m->phase = fmodf(m->phase + PHASE_RAD_PER_S * dt, TWO_PI * 1000.0f);
    // dt/(dt+tau) is the stable discrete form of 1-exp(-dt/tau): never overshoots for any dt.
    m->lane += ((float)m->lane_target - m->lane) * ((float)dt_ms / ((float)dt_ms + EASE_TAU_MS));
    m->sway = fmodf(m->sway + SURFER_SWAY_RAD_S * dt, TWO_PI);
    m->surfer_x = SURFER_SWAY_X * sinf(m->sway);
    obstacles_tick(m, (float)dt_ms);
}

float sea_model_obstacle_y(const sea_model_t *m, int i)
{
    const sea_obstacle_t *o = &m->obst[i];
    return sea_model_lane_y(o->lane) + sea_model_wave_y(m, o->x, o->lane);
}

int sea_model_fall_state(const sea_model_t *m)
{
    if (m->fall_ms <= 0.0f) return 0;
    return m->fall_ms > (float)SURFER_CLIMB_MS ? 1 : 2;
}

float sea_model_lane_y(int lane)
{
    return SEA_LANE_Y0 + SEA_LANE_GAP * (float)lane;
}

float sea_model_wave_y(const sea_model_t *m, float x, int lane)
{
    if (lane < 0 || lane >= SEA_LANES) return 0.0f;
    float a = TWO_PI * x / WAVELEN[lane] + m->phase * SPEED[lane];   // + : crests move right to left
    float h = sinf(a) + 0.3f * sinf(2.3f * a + 1.0f);                // |h| <= 1.3
    return -AMP[lane] * h / 1.3f;
}

float sea_model_surfer_y(const sea_model_t *m, float x)
{
    float l = m->lane < 0.0f ? 0.0f : (m->lane > (float)(SEA_LANES - 1) ? (float)(SEA_LANES - 1) : m->lane);
    int a = (int)l;
    int b = a + 1 < SEA_LANES ? a + 1 : a;
    float f = l - (float)a;
    float ya = sea_model_lane_y(a) + sea_model_wave_y(m, x, a);
    float yb = sea_model_lane_y(b) + sea_model_wave_y(m, x, b);
    return ya + (yb - ya) * f;
}

float sea_model_surfer_slope(const sea_model_t *m)
{
    return (sea_model_surfer_y(m, m->surfer_x + 1.0f) - sea_model_surfer_y(m, m->surfer_x - 1.0f)) / 2.0f;
}
