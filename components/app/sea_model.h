#pragma once
// sea_model — pure model for the sea scene: SEA_LANES rows of waves that roll right to left across
// the whole screen, and a surfer who rides one lane, sways gently around the screen centre and slides along its crests. The dial picks the
// lane (one detent = one lane nearer/farther); each lane has its own height, length and speed, so
// the path the surfer takes depends on the wave he chose. No LVGL / ESP-IDF dependencies
// (host-tested in test/host/test_sea_model.c). Zero-initialised state = lane 0, x = 0, phase 0.
// Obstacles (buoys, logs, shark fins) drift in from the right on a random lane; touching one on his
// lane knocks him off the board (SURFER_FALL_MS in the water), then he climbs back on and is safe for
// SURFER_SAFE_MS. Changing lane in time dodges them. Not thread-safe: LVGL task only. Coordinates: x px from screen centre, y px from screen top.

#define SEA_LANES        5
#define SEA_LANE_Y0      100.0f   // calm waterline of the farthest lane
#define SEA_LANE_GAP     50.0f    // distance between lane waterlines
#define SEA_WAVE_AMP     22.0f    // max |sea_model_wave_y| in px, any lane
#define SURFER_SWAY_X    30.0f    // he stays near the centre and sways left/right by at most this, px
#define SURFER_SWAY_RAD_S 0.8f    // sway speed

#define SEA_OBST_MAX     6
#define OBST_SPEED       80.0f    // px/s obstacles drift leftwards (faster than the surfer: they catch up)
#define OBST_SPAWN_X     200.0f   // enter just off the right edge, retire at -OBST_SPAWN_X
#define OBST_HIT_DX      16.0f    // horizontal reach of a collision, px
#define OBST_HIT_LANE    0.45f    // lane distance that still counts as a hit (changing lane dodges)
#define SURFER_FALL_MS   1400u    // time in the water after a hit; the last SURFER_CLIMB_MS he climbs back on
#define SURFER_CLIMB_MS  400u
#define SURFER_SAFE_MS   1500u    // grace period after climbing back on

typedef enum { OBST_BUOY, OBST_LOG, OBST_FIN, OBST_KINDS } sea_obst_kind_t;

typedef struct {
    int   active;
    int   kind;         // sea_obst_kind_t
    int   lane;         // 0 .. SEA_LANES-1
    float x;            // px from centre
} sea_obstacle_t;

typedef struct {
    float phase;        // radians; advances with time
    int   lane_target;  // lane the dial selected, 0 (far) .. SEA_LANES-1 (near)
    float lane;         // lane the surfer is drawn on; eases toward lane_target
    float sway;         // radians, wraps at 2*pi
    float surfer_x;     // px from centre: SURFER_SWAY_X * sin(sway)
    sea_obstacle_t obst[SEA_OBST_MAX];
    float spawn_ms;     // countdown to the next obstacle
    unsigned rng;       // spawn randomness; deterministic so tests can replay it
    float fall_ms;      // > 0: he is in the water
    float safe_ms;      // > 0: cannot be hit
    int   hits;         // falls so far
} sea_model_t;

// One dial detent: +1 = one lane nearer (down the screen), -1 = one lane farther. Clamped.
void sea_model_nudge(sea_model_t *m, int dir);

// Advance the waves, the surfer's run and the lane change over `dt_ms`.
void sea_model_tick(sea_model_t *m, unsigned dt_ms);

// Calm waterline y of `lane`.
float sea_model_lane_y(int lane);

// Crest offset in px (negative = up) of `lane` at column `x`; |result| <= SEA_WAVE_AMP.
float sea_model_wave_y(const sea_model_t *m, float x, int lane);

// Screen y of the water under the surfer (blends two lanes while he changes lane) at column `x`.
float sea_model_surfer_y(const sea_model_t *m, float x);

// Slope (dy/dx, screen y grows downward) of the surfer's path at his position. > 0 means the water
// ahead of him (he travels left) is higher.
float sea_model_surfer_slope(const sea_model_t *m);

// Screen y of the water under obstacle `i` (it floats on its lane's waves).
float sea_model_obstacle_y(const sea_model_t *m, int i);

// 0 = riding, 1 = in the water, 2 = climbing back on.
int sea_model_fall_state(const sea_model_t *m);
