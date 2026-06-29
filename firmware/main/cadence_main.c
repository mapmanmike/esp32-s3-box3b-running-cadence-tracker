/*
 * Treadmill Cadence Meter — ESP32-S3-BOX-3B PoC.
 *
 * Place the device on a treadmill. Each running step makes the treadmill "bang",
 * which the onboard accelerometer (ICM-4267x @ I2C 0x68) registers as an impact
 * spike. We detect impacts, compute a rolling cadence (steps/min over a recent
 * window), and show it big on the LCD.
 *
 * Touchscreen flow: START -> live steps/min (STOP) -> session summary (avg
 * cadence + total steps + duration + cadence-over-time line graph) -> DONE.
 * Peloton-styled dark/red theme.
 *
 * Hardware brought up via the patched esp_box_3 BSP (LCD + touch + shared GPIO48
 * reset). LVGL 8.4. See ../README.md and patches/apply_bsp_patches.py.
 */
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-box-3.h"
#include "icm42670.h"
#include "lvgl.h"

static const char *TAG = "cadence";

/* ---- Tunable step-detection parameters (iterate with the serial log) ---- */
#define SAMPLE_HZ        200          /* accelerometer poll rate */
#define SAMPLE_PERIOD_MS (1000 / SAMPLE_HZ)
#define THRESHOLD_G      0.35f        /* impact magnitude above baseline to count */
#define RELEASE_G        0.20f        /* must fall below this before next count (hysteresis) */
#define MIN_STEP_MS      200          /* refractory: max ~300 spm, debounces one bang */
#define WINDOW_MS        10000        /* rolling window for cadence average */
#define STALE_MS         3000         /* no step in this long -> cadence decays to 0 */
#define BASELINE_ALPHA   0.01f        /* low-pass factor for gravity/DC baseline */
#define RING_N           16           /* recent step timestamps kept */

/* ---- Session history (bounded for ~1hr) ---- */
#define MAX_PTS          120          /* graph points; 120 * 30s = 60 min */
#define SESSION_SAMPLE_MS 30000       /* log live cadence every 30s; 120 pts = 60 min */

/* ---- Peloton brand palette (approx from onepeloton.com) ---- */
#define PELO_BG    0x101012           /* near-black background */
#define PELO_RED   0xDF1E2D           /* signature Peloton red (primary CTA/accent) */
#define PELO_TEXT  0xFFFFFF           /* primary text */
#define PELO_GRAY  0x8A8A8E           /* captions / secondary */
#define PELO_CARD  0x1C1C1F           /* card / chart surface */

/* ---- Shared state (single writer = sampler task, single reader = UI timer) ---- */
static volatile int      g_cadence_spm = 0;
static volatile uint32_t g_step_count  = 0;
static volatile bool     g_running     = false;

static icm42670_handle_t s_imu = NULL;

/* ---- Session capture (UI-thread only: written + read from lv_timer / callbacks) ---- */
static int16_t  s_session_cad[MAX_PTS];
static int      s_session_pts   = 0;
static int      s_sample_int_ms = SESSION_SAMPLE_MS; /* effective interval (grows on downsample) */
static int64_t  s_session_start_us = 0;

/* ---- LVGL objects ---- */
static lv_obj_t *s_idle_scr;
static lv_obj_t *s_run_scr;
static lv_obj_t *s_summary_scr;
static lv_obj_t *s_cadence_label;
static lv_obj_t *s_sum_avg_label;
static lv_obj_t *s_sum_detail_label;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_chart_ser;

/* ---------------- Step detection / cadence (sampler task) ---------------- */

static void sampler_task(void *arg)
{
    int64_t step_ts_us[RING_N] = {0};
    int     head = 0, count = 0;
    float   baseline = 1.0f;          /* ~1g at rest */
    bool    above = false;
    int64_t last_step_us = 0;
    bool    primed = false;

    while (1) {
        if (!g_running) {
            /* Reset detection state between sessions. */
            count = 0; head = 0; above = false; last_step_us = 0; primed = false;
            baseline = 1.0f;
            g_cadence_spm = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        icm42670_value_t a = {0};
        if (s_imu && icm42670_get_acce_value(s_imu, &a) == ESP_OK) {
            float m = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);

            /* Slow baseline tracks gravity + DC; let it settle for ~0.5s first. */
            baseline += BASELINE_ALPHA * (m - baseline);
            float d = fabsf(m - baseline);

            int64_t now = esp_timer_get_time();
            if (!primed) {
                /* settle baseline before detecting */
                static int settle = 0;
                if (++settle > SAMPLE_HZ / 2) { primed = true; settle = 0; }
            } else {
                if (!above && d > THRESHOLD_G) {
                    above = true;
                    if ((now - last_step_us) / 1000 >= MIN_STEP_MS) {
                        last_step_us = now;
                        step_ts_us[head] = now;
                        head = (head + 1) % RING_N;
                        if (count < RING_N) count++;
                        g_step_count++;
                        ESP_LOGI(TAG, "STEP #%u  d=%.2fg  baseline=%.2f",
                                 (unsigned)g_step_count, d, baseline);
                    }
                } else if (above && d < RELEASE_G) {
                    above = false;
                }
            }

            /* ---- compute rolling cadence ---- */
            /* Collect timestamps within WINDOW_MS of now. */
            int64_t cutoff = now - (int64_t)WINDOW_MS * 1000;
            int64_t newest = 0, oldest = 0;
            int in_win = 0;
            for (int i = 0; i < count; i++) {
                int64_t ts = step_ts_us[i];
                if (ts >= cutoff) {
                    if (in_win == 0 || ts < oldest) oldest = ts;
                    if (ts > newest) newest = ts;
                    in_win++;
                }
            }
            if (in_win >= 2 && (now - newest) / 1000 < STALE_MS) {
                int64_t span_ms = (newest - oldest) / 1000;
                if (span_ms > 0) {
                    int spm = (int)((int64_t)(in_win - 1) * 60000 / span_ms);
                    /* light smoothing toward new value */
                    g_cadence_spm = (g_cadence_spm * 1 + spm * 3) / 4;
                }
            } else if ((now - last_step_us) / 1000 >= STALE_MS) {
                g_cadence_spm = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* ---------------- UI ---------------- */

static void show_screen(lv_obj_t *scr)
{
    bsp_display_lock(0);
    lv_scr_load(scr);
    bsp_display_unlock();
}

static void start_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "START pressed");
    /* Reset session capture. */
    s_session_pts = 0;
    s_sample_int_ms = SESSION_SAMPLE_MS;
    s_session_start_us = esp_timer_get_time();
    g_step_count = 0;
    g_cadence_spm = 0;
    g_running = true;
    bsp_display_lock(0);
    lv_label_set_text(s_cadence_label, "0");
    bsp_display_unlock();
    show_screen(s_run_scr);
}

/* Append one cadence sample to the bounded session history. When the buffer is
 * full, downsample in place (halve point count, double the effective interval)
 * so a ~1hr+ session always fits and the graph still spans the whole run. */
static void session_push(int spm)
{
    if (s_session_pts >= MAX_PTS) {
        for (int i = 0; i < MAX_PTS / 2; i++) {
            s_session_cad[i] = s_session_cad[i * 2];
        }
        s_session_pts = MAX_PTS / 2;
        s_sample_int_ms *= 2;
    }
    s_session_cad[s_session_pts++] = (int16_t)spm;
}

static void stop_btn_cb(lv_event_t *e)
{
    g_running = false;  /* stop the sampler writer before we read shared state */

    int64_t elapsed_us = esp_timer_get_time() - s_session_start_us;
    int      elapsed_s = (int)(elapsed_us / 1000000);
    if (elapsed_s < 1) elapsed_s = 1;
    uint32_t steps = g_step_count;
    int avg_spm = (int)((int64_t)steps * 60 / elapsed_s);

    ESP_LOGI(TAG, "STOP: steps=%u  duration=%ds  avg=%d spm  pts=%d",
             (unsigned)steps, elapsed_s, avg_spm, s_session_pts);

    bsp_display_lock(0);
    lv_label_set_text_fmt(s_sum_avg_label, "%d", avg_spm);
    lv_label_set_text_fmt(s_sum_detail_label, "%u steps  -  %d:%02d",
                          (unsigned)steps, elapsed_s / 60, elapsed_s % 60);

    /* Populate the cadence-over-time line chart. */
    if (s_session_pts >= 2) {
        int maxv = 1;
        for (int i = 0; i < s_session_pts; i++)
            if (s_session_cad[i] > maxv) maxv = s_session_cad[i];
        if (maxv < 200) maxv = 200;  /* avoid clipping a low/flat trace */
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, maxv);
        lv_chart_set_point_count(s_chart, s_session_pts);
        for (int i = 0; i < s_session_pts; i++)
            lv_chart_set_value_by_id(s_chart, s_chart_ser, i, s_session_cad[i]);
        lv_chart_refresh(s_chart);
        lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_chart, LV_OBJ_FLAG_HIDDEN);  /* too short to graph */
    }
    bsp_display_unlock();

    show_screen(s_summary_scr);
}

static void done_btn_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "DONE pressed -> idle");
    s_session_pts = 0;
    show_screen(s_idle_scr);
}

static void ui_update_timer(lv_timer_t *t)
{
    if (!g_running) return;
    lv_label_set_text_fmt(s_cadence_label, "%d", g_cadence_spm);

    /* Log cadence into the session history at the effective sample interval. */
    static int64_t last_log_us = 0;
    int64_t now = esp_timer_get_time();
    if (last_log_us == 0) last_log_us = s_session_start_us;
    if ((now - last_log_us) / 1000 >= s_sample_int_ms) {
        last_log_us = now;
        session_push(g_cadence_spm);
    }
}

static lv_obj_t *make_big_button(lv_obj_t *parent, const char *txt,
                                 lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 200, 80);
    lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(PELO_BG), LV_PART_MAIN);
    return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    return l;
}

static void build_ui(void)
{
    bsp_display_lock(0);

    /* ---- IDLE screen ---- */
    s_idle_scr = make_screen();
    lv_obj_t *title = make_label(s_idle_scr, "PELOTON CADENCE",
                                 &lv_font_montserrat_28, PELO_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_t *sub = make_label(s_idle_scr, "treadmill step tracker",
                               &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    lv_obj_t *start_btn = make_big_button(s_idle_scr, "START",
                                          lv_color_hex(PELO_RED), start_btn_cb);
    lv_obj_align(start_btn, LV_ALIGN_CENTER, 0, 30);

    /* ---- RUNNING screen ---- */
    s_run_scr = make_screen();
    s_cadence_label = make_label(s_run_scr, "0", &lv_font_montserrat_48, PELO_RED);
    lv_obj_align(s_cadence_label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *unit = make_label(s_run_scr, "steps / min",
                                &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align_to(unit, s_cadence_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

    lv_obj_t *stop_btn = make_big_button(s_run_scr, "STOP",
                                         lv_color_hex(PELO_RED), stop_btn_cb);
    lv_obj_align(stop_btn, LV_ALIGN_BOTTOM_MID, 0, -20);

    /* ---- SUMMARY screen ---- */
    s_summary_scr = make_screen();
    lv_obj_t *stitle = make_label(s_summary_scr, "SESSION COMPLETE",
                                  &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align(stitle, LV_ALIGN_TOP_MID, 0, 10);

    s_sum_avg_label = make_label(s_summary_scr, "0", &lv_font_montserrat_48, PELO_RED);
    lv_obj_align(s_sum_avg_label, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_t *avgcap = make_label(s_summary_scr, "avg steps / min",
                                  &lv_font_montserrat_20, PELO_TEXT);
    lv_obj_align_to(avgcap, s_sum_avg_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    s_sum_detail_label = make_label(s_summary_scr, "0 steps  -  0:00",
                                    &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align(s_sum_detail_label, LV_ALIGN_TOP_MID, 0, 110);

    /* cadence-over-time line chart (between the detail line and DONE).
     * Screen is 240px tall: chart 138..186, DONE 198..232 -> no overlap. */
    s_chart = lv_chart_create(s_summary_scr);
    lv_obj_set_size(s_chart, 264, 48);
    lv_obj_align(s_chart, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(PELO_CARD), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_chart, 6, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x303034), LV_PART_MAIN); /* div lines */
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    s_chart_ser = lv_chart_add_series(s_chart, lv_color_hex(PELO_RED),
                                      LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR); /* no point markers */

    lv_obj_t *done_btn = make_big_button(s_summary_scr, "DONE",
                                         lv_color_hex(PELO_RED), done_btn_cb);
    lv_obj_set_size(done_btn, 150, 40);
    lv_obj_align(done_btn, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_scr_load(s_idle_scr);
    bsp_display_unlock();

    lv_timer_create(ui_update_timer, 250, NULL);
}

/* ---------------- main ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Treadmill cadence meter starting");

    lv_disp_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    /* IMU on the BSP I2C bus (bsp_i2c_init already called inside bsp_display_start
     * via touch init, but call is idempotent/safe). */
    bsp_i2c_init();
    s_imu = icm42670_create(BSP_I2C_NUM, ICM42670_I2C_ADDRESS);
    if (s_imu == NULL) {
        ESP_LOGE(TAG, "IMU create failed");
    } else {
        icm42670_cfg_t cfg = {
            .acce_fs = ACCE_FS_8G, .acce_odr = ACCE_ODR_200HZ,
            .gyro_fs = GYRO_FS_2000DPS, .gyro_odr = GYRO_ODR_200HZ,
        };
        icm42670_config(s_imu, &cfg);
        icm42670_acce_set_pwr(s_imu, ACCE_PWR_LOWNOISE);
        ESP_LOGI(TAG, "IMU ready (200Hz, 8G)");
    }

    build_ui();
    ESP_LOGI(TAG, "UI ready (IDLE). Tap START.");

    xTaskCreate(sampler_task, "sampler", 4096, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
