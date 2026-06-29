/*
 * Treadmill Cadence Meter — ESP32-S3-BOX-3B PoC.
 *
 * Place the device on a treadmill. Each running step makes the treadmill "bang",
 * which the onboard accelerometer (ICM-4267x @ I2C 0x68) registers as an impact
 * spike. We detect impacts, compute a rolling cadence (steps/min over a recent
 * window), and show it big on the LCD. START button -> RUNNING screen with a big
 * number + STOP button -> back to IDLE.
 *
 * Hardware brought up via the patched esp_box_3 BSP (LCD + touch + shared GPIO48
 * reset). LVGL 8.4.
 */
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-box-3.h"
#include "icm42670.h"
#include "lvgl.h"

static const char *TAG = "cadence";

/* ---- Tunable step-detection parameters (iterate with the serial log) ---- */
/* IMU poll rate. The IMU and the GT911 touch share ONE legacy I2C bus; polling
 * too fast from this (higher-priority) task starves the LVGL task's touch read on
 * the same bus -> UI freezes. 50 Hz (20 ms) still gives ~15 samples per step and
 * leaves the bus free 75% of the time for touch. */
#define SAMPLE_HZ        50
#define SAMPLE_PERIOD_MS (1000 / SAMPLE_HZ)
/* Detection threshold is runtime-adjustable (Settings slider, persisted in NVS).
 * Units are g; the slider works in milli-g over [MIN_THRESH_MG, MAX_THRESH_MG].
 * RELEASE (hysteresis floor) is derived as RELEASE_FRAC * threshold. */
#define DEFAULT_THRESH_MG 180         /* 0.18 g default (more sensitive than old 0.22) */
#define MIN_THRESH_MG     60          /* 0.06 g = most sensitive */
#define MAX_THRESH_MG     400         /* 0.40 g = least sensitive */
#define RELEASE_FRAC      0.55f
#define MIN_STEP_MS      200          /* refractory: max ~300 spm, debounces one bang */
#define WINDOW_MS        10000        /* rolling window for cadence average */
#define STALE_MS         3000         /* no step in this long -> cadence decays to 0 */
#define BASELINE_ALPHA   0.01f        /* low-pass factor for gravity/DC baseline */
#define RING_N           16           /* recent step timestamps kept */

/* ---- Session history (bounded for ~1hr) ---- */
#define MAX_PTS          120          /* graph points (auto-downsamples for long runs) */
#define SESSION_SAMPLE_MS 5000        /* log live cadence every 5s; graph shows after ~10s.
                                       * Buffer auto-downsamples so a ~1hr run still fits. */

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
static volatile int      g_threshold_mg = DEFAULT_THRESH_MG; /* live detection threshold (milli-g) */

static icm42670_handle_t s_imu = NULL;

/* ---- NVS persistence for the sensitivity setting ---- */
#define NVS_NS   "cadence"
#define NVS_KEY  "sens_mg"

static void nvs_load_threshold(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_KEY, &v) == ESP_OK &&
            v >= MIN_THRESH_MG && v <= MAX_THRESH_MG) {
            g_threshold_mg = (int)v;
            ESP_LOGI(TAG, "Loaded sensitivity from NVS: %d mg", (int)v);
        }
        nvs_close(h);
    }
}

static void nvs_save_threshold(int mg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY, (int32_t)mg);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Saved sensitivity to NVS: %d mg", mg);
    }
}

/* ---- Session capture (UI-thread only: written + read from lv_timer / callbacks) ---- */
static int16_t  s_session_cad[MAX_PTS];
static int      s_session_pts   = 0;
static int      s_sample_int_ms = SESSION_SAMPLE_MS; /* effective interval (grows on downsample) */
static int64_t  s_session_start_us = 0;

/* ---- LVGL objects ---- */
static lv_obj_t *s_idle_scr;
static lv_obj_t *s_run_scr;
static lv_obj_t *s_summary_scr;
static lv_obj_t *s_settings_scr;
static lv_obj_t *s_cadence_label;
static lv_obj_t *s_cad_bar;          /* color-coded cadence zone bar */
static lv_obj_t *s_sum_avg_label;
static lv_obj_t *s_sum_detail_label;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_chart_ser;
static lv_obj_t *s_sens_slider;
static lv_obj_t *s_sens_value_label;

/* ---- Cadence zone bar geometry + novice-runner color zones (steps/min) ---- */
#define CAD_BAR_W    264
#define CAD_BAR_MIN  120
#define CAD_BAR_MAX  200
static uint32_t zone_color(int spm)
{
    if (spm < 150) return 0xE03B3B;       /* red    - low */
    if (spm < 160) return 0xF08A24;       /* orange - building */
    if (spm < 170) return 0x2E9E44;       /* green  - good */
    if (spm < 180) return 0x17A2A2;       /* teal   - strong */
    return 0x2E6FE0;                       /* blue   - elite */
}

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

            /* Live thresholds from the Settings slider (milli-g -> g). */
            float threshold_g = g_threshold_mg / 1000.0f;
            float release_g   = threshold_g * RELEASE_FRAC;

            int64_t now = esp_timer_get_time();
            if (!primed) {
                /* settle baseline before detecting */
                static int settle = 0;
                if (++settle > SAMPLE_HZ / 2) { primed = true; settle = 0; }
            } else {
                if (!above && d > threshold_g) {
                    above = true;
                    if ((now - last_step_us) / 1000 >= MIN_STEP_MS) {
                        last_step_us = now;
                        step_ts_us[head] = now;
                        head = (head + 1) % RING_N;
                        if (count < RING_N) count++;
                        g_step_count++;
                        /* Rate-limit the step log: at high sensitivity steps can
                         * fire very often, and logging every one floods the
                         * USB-Serial-JTAG console. When nothing is draining that
                         * FIFO (running untethered), the blocking writes starve
                         * the UI task and freeze the display. Cap to ~2 logs/sec. */
                        static int64_t last_log_us = 0;
                        if ((now - last_log_us) / 1000 >= 500) {
                            last_log_us = now;
                            ESP_LOGI(TAG, "STEP #%u  d=%.2fg  baseline=%.2f  thr=%.2f",
                                     (unsigned)g_step_count, d, baseline, threshold_g);
                        }
                    }
                } else if (above && d < release_g) {
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

static void settings_btn_cb(lv_event_t *e)
{
    show_screen(s_settings_scr);
}

static void settings_back_cb(lv_event_t *e)
{
    show_screen(s_idle_scr);
}

/* Slider drag: update live threshold + label. Persist on release. */
static void sens_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int mg = (int)lv_slider_get_value(s_sens_slider);
    g_threshold_mg = mg;
    lv_label_set_text_fmt(s_sens_value_label, "%.2f g", mg / 1000.0f);
    if (code == LV_EVENT_RELEASED) {
        nvs_save_threshold(mg);
        ESP_LOGI(TAG, "Sensitivity set: threshold=%.2fg", mg / 1000.0f);
    }
}

static void ui_update_timer(lv_timer_t *t)
{
    if (!g_running) return;
    int spm = g_cadence_spm;
    lv_label_set_text_fmt(s_cadence_label, "%d", spm);

    /* Color-coded cadence zone bar. */
    lv_bar_set_value(s_cad_bar, spm, LV_ANIM_ON);
    lv_obj_set_style_bg_color(s_cad_bar, lv_color_hex(zone_color(spm)), LV_PART_INDICATOR);

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
    lv_obj_align(start_btn, LV_ALIGN_CENTER, 0, 20);

    /* gear/settings button (bottom-right) */
    lv_obj_t *gear = lv_btn_create(s_idle_scr);
    lv_obj_set_size(gear, 56, 44);
    lv_obj_set_style_bg_color(gear, lv_color_hex(PELO_CARD), LV_PART_MAIN);
    lv_obj_set_style_radius(gear, 10, LV_PART_MAIN);
    lv_obj_align(gear, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_add_event_cb(gear, settings_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gl, lv_color_hex(PELO_TEXT), LV_PART_MAIN);
    lv_obj_center(gl);

    /* ---- RUNNING screen ---- */
    s_run_scr = make_screen();
    s_cadence_label = make_label(s_run_scr, "0", &lv_font_montserrat_48, PELO_RED);
    lv_obj_align(s_cadence_label, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *unit = make_label(s_run_scr, "steps / min",
                                &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align_to(unit, s_cadence_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    /* color-coded cadence zone bar */
    s_cad_bar = lv_bar_create(s_run_scr);
    lv_obj_set_size(s_cad_bar, CAD_BAR_W, 16);
    lv_obj_align(s_cad_bar, LV_ALIGN_CENTER, 0, 6);
    lv_bar_set_range(s_cad_bar, CAD_BAR_MIN, CAD_BAR_MAX);  /* visual span across zones */
    lv_bar_set_value(s_cad_bar, CAD_BAR_MIN, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_cad_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cad_bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_cad_bar, lv_color_hex(PELO_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cad_bar, lv_color_hex(zone_color(0)), LV_PART_INDICATOR);

    /* Threshold tick marks: thin lines on the bar + small numbers below, at the
     * novice-zone boundaries (150/160/170/180 spm). */
    static const int ticks[] = {150, 160, 170, 180};
    for (int i = 0; i < 4; i++) {
        int x = ((ticks[i] - CAD_BAR_MIN) * CAD_BAR_W) / (CAD_BAR_MAX - CAD_BAR_MIN)
                - CAD_BAR_W / 2;  /* offset from bar center */
        lv_obj_t *tick = lv_obj_create(s_run_scr);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 2, 22);
        lv_obj_set_style_bg_color(tick, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tick, LV_OPA_60, LV_PART_MAIN);
        lv_obj_align_to(tick, s_cad_bar, LV_ALIGN_CENTER, x, 0);
        lv_obj_t *tl = make_label(s_run_scr, "", &lv_font_montserrat_14, PELO_GRAY);
        lv_label_set_text_fmt(tl, "%d", ticks[i]);
        lv_obj_align_to(tl, s_cad_bar, LV_ALIGN_OUT_BOTTOM_MID, x, 4);
    }

    lv_obj_t *stop_btn = make_big_button(s_run_scr, "STOP",
                                         lv_color_hex(PELO_RED), stop_btn_cb);
    lv_obj_set_size(stop_btn, 200, 56);
    lv_obj_align(stop_btn, LV_ALIGN_BOTTOM_MID, 0, -12);

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

    /* ---- SETTINGS screen ---- */
    s_settings_scr = make_screen();
    lv_obj_t *settitle = make_label(s_settings_scr, "SETTINGS",
                                    &lv_font_montserrat_28, PELO_TEXT);
    lv_obj_align(settitle, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *senscap = make_label(s_settings_scr, "step sensitivity",
                                   &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align(senscap, LV_ALIGN_TOP_MID, 0, 58);

    s_sens_slider = lv_slider_create(s_settings_scr);
    lv_obj_set_size(s_sens_slider, 256, 16);
    lv_obj_align(s_sens_slider, LV_ALIGN_CENTER, 0, -6);
    /* Inverted feel: left = more sensitive. We map slider value directly to
     * threshold mg, and label the ends, so left (low mg) = most sensitive. */
    lv_slider_set_range(s_sens_slider, MIN_THRESH_MG, MAX_THRESH_MG);
    lv_slider_set_value(s_sens_slider, g_threshold_mg, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sens_slider, lv_color_hex(PELO_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sens_slider, lv_color_hex(PELO_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sens_slider, lv_color_hex(PELO_RED), LV_PART_KNOB);
    lv_obj_add_event_cb(s_sens_slider, sens_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_sens_slider, sens_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *lend = make_label(s_settings_scr, "more", &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align_to(lend, s_sens_slider, LV_ALIGN_OUT_TOP_LEFT, 0, -4);
    lv_obj_t *rend = make_label(s_settings_scr, "less", &lv_font_montserrat_20, PELO_GRAY);
    lv_obj_align_to(rend, s_sens_slider, LV_ALIGN_OUT_TOP_RIGHT, 0, -4);

    s_sens_value_label = lv_label_create(s_settings_scr);
    lv_label_set_text_fmt(s_sens_value_label, "%.2f g", g_threshold_mg / 1000.0f);
    lv_obj_set_style_text_font(s_sens_value_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_sens_value_label, lv_color_hex(PELO_TEXT), LV_PART_MAIN);
    lv_obj_align_to(s_sens_value_label, s_sens_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_obj_t *back_btn = make_big_button(s_settings_scr, "BACK",
                                         lv_color_hex(PELO_RED), settings_back_cb);
    lv_obj_set_size(back_btn, 150, 44);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_scr_load(s_idle_scr);
    bsp_display_unlock();

    lv_timer_create(ui_update_timer, 250, NULL);
}

/* ---------------- main ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Treadmill cadence meter starting");
    /* Report WHY the chip last reset — this is the key diagnostic for the
     * freeze/brick (crash vs. task watchdog vs. interrupt watchdog vs. brownout). */
    ESP_LOGW(TAG, "reset_reason=%d (1=PWRON 4=SW 6=WDT/INT 7=TG0WDT 8=TG1WDT "
                  "9=RTCWDT 11=TGWDT_CPU 12=SW_CPU 13=RTCWDT_CPU 15=BROWNOUT)",
             (int)esp_reset_reason());

    /* NVS for the persisted sensitivity setting. */
    esp_err_t nret = nvs_flash_init();
    if (nret == ESP_ERR_NVS_NO_FREE_PAGES || nret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_load_threshold();

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

    /* Priority 3: BELOW the BSP's LVGL task (priority 5) so the UI/touch always
     * wins the shared I2C bus. Equal priority here starves the UI -> freeze. */
    xTaskCreate(sampler_task, "sampler", 4096, NULL, 3, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
