// Host unit tests for the sea scene model (app/sea_model.c). Pure C, no LVGL.
//   • waves move with time and stay within SEA_WAVE_AMP
//   • a detent moves the target lane by one, clamped to the lanes that exist
//   • the drawn lane eases onto the target without overshoot
//   • the surfer sways around the centre, never beyond +/- SURFER_SWAY_X
//   • different lanes give different paths
//   • obstacles spawn, drift left and retire
//   • hitting one on his lane knocks him off; another lane dodges it; he climbs back on and is safe
#include "unity.h"
#include "sea_model.h"
#include <math.h>

static sea_model_t m;

void setUp(void)    { m = (sea_model_t){0}; }
void tearDown(void) {}

void test_waves_move_with_time(void)
{
    float before = sea_model_wave_y(&m, 30.0f, 2);
    sea_model_tick(&m, 100);
    TEST_ASSERT_TRUE(fabsf(sea_model_wave_y(&m, 30.0f, 2) - before) > 0.01f);
}

void test_crests_travel_right_to_left(void)
{
    // Follow the highest point of the lane: its x must decrease as time passes.
    float x_prev = 0, best;
    for (int step = 0; step < 2; step++) {
        float bx = 0; best = 1e9f;
        for (float x = -60.0f; x <= 60.0f; x += 0.5f) {
            float y = sea_model_wave_y(&m, x, 2);
            if (y < best) { best = y; bx = x; }
        }
        if (step == 1) TEST_ASSERT_TRUE(bx < x_prev);
        x_prev = bx;
        sea_model_tick(&m, 100);
    }
}

void test_wave_height_is_bounded(void)
{
    for (int t = 0; t < 500; t++) {
        sea_model_tick(&m, 50);
        for (int l = 0; l < SEA_LANES; l++)
            for (int x = -180; x <= 180; x += 5)
                TEST_ASSERT_TRUE(fabsf(sea_model_wave_y(&m, (float)x, l)) <= SEA_WAVE_AMP + 0.001f);
    }
}

void test_nudge_changes_lane_by_one_and_clamps(void)
{
    sea_model_nudge(&m, +1);
    TEST_ASSERT_EQUAL_INT(1, m.lane_target);
    for (int i = 0; i < 20; i++) sea_model_nudge(&m, +1);
    TEST_ASSERT_EQUAL_INT(SEA_LANES - 1, m.lane_target);
    for (int i = 0; i < 20; i++) sea_model_nudge(&m, -1);
    TEST_ASSERT_EQUAL_INT(0, m.lane_target);
}

void test_lane_converges_without_overshoot(void)
{
    for (int i = 0; i < 3; i++) sea_model_nudge(&m, +1);
    float prev = m.lane;
    for (int i = 0; i < 400; i++) {
        sea_model_tick(&m, 50);
        TEST_ASSERT_TRUE(m.lane >= prev);
        TEST_ASSERT_TRUE(m.lane <= 3.0f + 0.001f);
        prev = m.lane;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, m.lane);
}

void test_surfer_stays_near_the_centre(void)
{
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 2000; i++) {
        sea_model_tick(&m, 50);
        TEST_ASSERT_TRUE(fabsf(m.surfer_x) <= SURFER_SWAY_X + 0.001f);
        if (m.surfer_x < lo) lo = m.surfer_x;
        if (m.surfer_x > hi) hi = m.surfer_x;
    }
    TEST_ASSERT_TRUE(hi - lo > SURFER_SWAY_X);   // he does move around
}

void test_surfer_y_follows_his_lane(void)
{
    m.lane = m.lane_target = 0;
    float y0 = sea_model_surfer_y(&m, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(SEA_WAVE_AMP, sea_model_lane_y(0), y0);
    m.lane = m.lane_target = 4;
    float y4 = sea_model_surfer_y(&m, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(SEA_WAVE_AMP, sea_model_lane_y(4), y4);
    TEST_ASSERT_TRUE(y4 > y0);
}

void test_lanes_give_different_paths(void)
{
    float range[2];
    int lanes[2] = { 0, 4 };
    for (int k = 0; k < 2; k++) {
        sea_model_t s = {0};
        s.lane = s.lane_target = (float)lanes[k];
        s.lane_target = lanes[k];
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 400; i++) {
            sea_model_tick(&s, 50);
            float y = sea_model_surfer_y(&s, s.surfer_x);
            if (y < lo) lo = y;
            if (y > hi) hi = y;
        }
        range[k] = hi - lo;
    }
    TEST_ASSERT_TRUE(range[1] > range[0] + 5.0f);   // the big near wave carries him up and down further
}

static void put_obstacle(int lane, float x)
{
    m.obst[0] = (sea_obstacle_t){ 1, OBST_LOG, lane, x };
    m.spawn_ms = 1e9f;   // keep the random spawner out of the way
}

void test_obstacles_spawn_drift_and_retire(void)
{
    int seen = 0, max_active = 0;
    float prev_x[SEA_OBST_MAX] = {0};
    for (int i = 0; i < 1200; i++) {
        sea_model_tick(&m, 50);
        int active = 0;
        for (int k = 0; k < SEA_OBST_MAX; k++) {
            if (!m.obst[k].active) continue;
            active++;
            TEST_ASSERT_TRUE(m.obst[k].lane >= 0 && m.obst[k].lane < SEA_LANES);
            TEST_ASSERT_TRUE(m.obst[k].kind >= 0 && m.obst[k].kind < OBST_KINDS);
            if (prev_x[k] != 0.0f && m.obst[k].x < OBST_SPAWN_X - 1.0f) TEST_ASSERT_TRUE(m.obst[k].x < prev_x[k]);
            prev_x[k] = m.obst[k].x;
        }
        for (int k = 0; k < SEA_OBST_MAX; k++) if (!m.obst[k].active) prev_x[k] = 0.0f;
        if (active > max_active) max_active = active;
        seen += active > 0;
    }
    TEST_ASSERT_TRUE(seen > 0);
    TEST_ASSERT_TRUE(max_active >= 2);
}

void test_hit_on_his_lane_knocks_him_off_then_he_recovers(void)
{
    put_obstacle(0, m.surfer_x + 5.0f);
    sea_model_tick(&m, 20);
    TEST_ASSERT_EQUAL_INT(1, m.hits);
    TEST_ASSERT_EQUAL_INT(1, sea_model_fall_state(&m));
    TEST_ASSERT_FALSE(m.obst[0].active);
    for (int t = 0; t < (int)(SURFER_FALL_MS - SURFER_CLIMB_MS) / 20; t++) sea_model_tick(&m, 20);
    TEST_ASSERT_EQUAL_INT(2, sea_model_fall_state(&m));
    for (int t = 0; t < (int)SURFER_CLIMB_MS / 20 + 2; t++) sea_model_tick(&m, 20);
    TEST_ASSERT_EQUAL_INT(0, sea_model_fall_state(&m));
    TEST_ASSERT_TRUE(m.safe_ms > 0.0f);
}

void test_other_lane_is_a_dodge(void)
{
    put_obstacle(3, m.surfer_x + 5.0f);
    sea_model_tick(&m, 20);
    TEST_ASSERT_EQUAL_INT(0, m.hits);
    TEST_ASSERT_EQUAL_INT(0, sea_model_fall_state(&m));
}

void test_no_second_hit_while_in_water_or_safe(void)
{
    put_obstacle(0, m.surfer_x + 5.0f);
    sea_model_tick(&m, 20);
    m.obst[1] = (sea_obstacle_t){ 1, OBST_BUOY, 0, m.surfer_x };
    sea_model_tick(&m, 20);
    TEST_ASSERT_EQUAL_INT(1, m.hits);
    m.fall_ms = 0.0f; m.safe_ms = 500.0f;
    m.obst[1] = (sea_obstacle_t){ 1, OBST_BUOY, 0, m.surfer_x };
    sea_model_tick(&m, 20);
    TEST_ASSERT_EQUAL_INT(1, m.hits);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_waves_move_with_time);
    RUN_TEST(test_crests_travel_right_to_left);
    RUN_TEST(test_wave_height_is_bounded);
    RUN_TEST(test_nudge_changes_lane_by_one_and_clamps);
    RUN_TEST(test_lane_converges_without_overshoot);
    RUN_TEST(test_surfer_stays_near_the_centre);
    RUN_TEST(test_surfer_y_follows_his_lane);
    RUN_TEST(test_lanes_give_different_paths);
    RUN_TEST(test_obstacles_spawn_drift_and_retire);
    RUN_TEST(test_hit_on_his_lane_knocks_him_off_then_he_recovers);
    RUN_TEST(test_other_lane_is_a_dodge);
    RUN_TEST(test_no_second_hit_while_in_water_or_safe);
    return UNITY_END();
}
