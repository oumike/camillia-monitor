#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <WiFi.h>

#include "hal/display.h"
#include "mesh/mesh_radio.h"
#include "config/monitor_config.h"
#include "net/web_config.h"
#include "net/node_uplink.h"
#include "net/time_sync.h"
#include "net/mqtt_monitor.h"
#include "sensors/gps.h"
#include "sensors/env_sensor.h"
#include "sensors/battery.h"
#include "config/monitor_config.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace {
constexpr uint16_t kDrawBufferLines = 32;

// Body layout. The header ends at y=40 and there is no footer, so the panels
// run to within 8px of the bottom edge.
constexpr int32_t kBodyTop    = 44;
constexpr int32_t kBodyBottom = 232;
constexpr int32_t kGutter     = 5;
constexpr int32_t kColLeftX   = 8;
constexpr int32_t kColRightX  = 164;
constexpr int32_t kColWidth   = 148;
constexpr int32_t kLeftRows   = 4;
constexpr int32_t kLeftCardH  =
    (kBodyBottom - kBodyTop - kGutter * (kLeftRows - 1)) / kLeftRows;
constexpr uint32_t kBackgroundColor = 0x0A1014;
constexpr uint32_t kPanelColor = 0x172129;
constexpr uint32_t kPrimaryColor = 0x53D6B5;
constexpr uint32_t kSecondaryColor = 0xF3B562;
constexpr uint32_t kMutedColor = 0x8EA0AA;
constexpr uint32_t kTextColor = 0xF4F7F8;

HeltecDisplay display;
uint16_t drawBuffer[DISPLAY_WIDTH * kDrawBufferLines];
lv_obj_t *heardValue = nullptr;
lv_obj_t *storedValue = nullptr;
lv_obj_t *clockLabel = nullptr;

// MQTT census overlay. Null whenever the screen is closed, which is also how
// the refresh timer knows to do nothing.
lv_obj_t *mqttOverlay = nullptr;
lv_obj_t *mqttStatusLabel = nullptr;
lv_obj_t *mqttClockLabel = nullptr;
lv_obj_t *mqttConnLabel = nullptr;
lv_obj_t *mqttList = nullptr;
lv_obj_t *mqttCountLabels[MQTT_MON_SLOTS] = {};
lv_obj_t *mqttAccents[MQTT_MON_SLOTS] = {};
uint32_t  mqttRenderedSeq = 0;
int       mqttRenderedRows = -1;
lv_obj_t *msgsHeardValue = nullptr;
lv_obj_t *msgsTotalValue = nullptr;
lv_obj_t *telemetryValues = nullptr;
lv_obj_t *gpsIcon = nullptr;
lv_obj_t *wifiIcon = nullptr;
lv_obj_t *apiIcon = nullptr;

// Header status palette. Three states rather than two: "working", "up but not
// doing the thing yet", and "broken" are genuinely different answers, and
// collapsing the middle one into either of the others makes the header lie
// during the first seconds after boot — when someone is most likely watching it.
constexpr uint32_t kStatusOkColor   = 0x53D6B5;
constexpr uint32_t kStatusWarnColor = 0xF3B562;
constexpr uint32_t kStatusOffColor  = 0x4A5A66;
constexpr uint32_t kStatusBadColor  = 0xE87878;

// Recency bands for the MQTT channel accents. The stripe answers "is this
// channel alive?", which a topic count on its own cannot — a channel with 40
// topics that went quiet an hour ago looks identical to a busy one.
constexpr uint32_t kMqttFreshMs  = 60UL * 1000UL;         // green
constexpr uint32_t kMqttRecentMs = 15UL * 60UL * 1000UL;  // yellow
constexpr uint32_t kMqttStaleMs  = 60UL * 60UL * 1000UL;  // red, then grey

uint32_t mqttAccentFor(uint32_t lastHeardMs) {
    // A zero stamp means the seed reported an age older than this device's
    // uptime — heard, but long before we booted.
    if (lastHeardMs == 0) return kStatusOffColor;
    const uint32_t age = millis() - lastHeardMs;
    if (age <= kMqttFreshMs)  return kStatusOkColor;
    if (age <= kMqttRecentMs) return kStatusWarnColor;
    if (age <= kMqttStaleMs)  return kStatusBadColor;
    return kStatusOffColor;
}

// Defined below createStatusIcon; refreshMetrics drives it on the 1 s timer.
void refreshStatusIcons();

void flushDisplay(lv_display_t *lvDisplay, const lv_area_t *area, uint8_t *pixels) {
    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    display.pushImage(area->x1, area->y1, width, height,
                      reinterpret_cast<lgfx::rgb565_t *>(pixels));
    lv_display_flush_ready(lvDisplay);
}

void readTouch(lv_indev_t *inputDevice, lv_indev_data_t *data) {
    LV_UNUSED(inputDevice);
    int32_t x = 0;
    int32_t y = 0;
    if (display.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        return;
    }
    data->state = LV_INDEV_STATE_RELEASED;
}

lv_obj_t *createMetricCard(lv_obj_t *parent, int32_t x, int32_t y,
                           const char *labelText, uint32_t accentColor,
                           lv_obj_t **valueLabel) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, kColWidth, kLeftCardH);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(kPanelColor), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 5, 0);

    lv_obj_t *accent = lv_obj_create(card);
    lv_obj_set_size(accent, 3, kLeftCardH - 14);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(accentColor), 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, 2, 0);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, labelText);
    lv_obj_set_style_text_color(label, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 13, -1);

    *valueLabel = lv_label_create(card);
    lv_label_set_text(*valueLabel, "--");
    lv_obj_set_style_text_color(*valueLabel, lv_color_hex(kTextColor), 0);
    // 16pt, not 24: four rows in the same height leaves ~33px of usable card,
    // and a 12pt title plus a 24pt value needs closer to 45.
    lv_obj_set_style_text_font(*valueLabel, &lv_font_montserrat_16, 0);
    lv_obj_align(*valueLabel, LV_ALIGN_BOTTOM_LEFT, 13, 1);
    return card;
}

// One tall panel spanning both rows on the right. Built as two labels — a fixed
// column of names and a right-aligned column of values — rather than one block
// of text, because Montserrat is proportional and a single label would leave the
// numbers ragged.
void createTelemetryPanel(lv_obj_t *parent) {
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, kColRightX, kBodyTop);
    lv_obj_set_size(panel, kColWidth, kBodyBottom - kBodyTop);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kPanelColor), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);

    lv_obj_t *accent = lv_obj_create(panel);
    lv_obj_set_size(accent, 3, kBodyBottom - kBodyTop - 26);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(kSecondaryColor), 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, 2, 0);
    lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *heading = lv_label_create(panel);
    lv_label_set_text(heading, "THIS DEVICE");
    lv_obj_set_style_text_color(heading, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_12, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 11, -2);

    lv_obj_t *keys = lv_label_create(panel);
    lv_label_set_text(keys,
                      "TEMP\n"
                      "HUMIDITY\n"
                      "PR\n"
                      "BATTERY\n"
                      "CHARGE\n"
                      "CH UTIL\n"
                      "CH PEAK");
    lv_obj_set_style_text_color(keys, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(keys, 7, 0);
    lv_obj_align(keys, LV_ALIGN_TOP_LEFT, 11, 20);

    telemetryValues = lv_label_create(panel);
    lv_label_set_text(telemetryValues, "--");
    lv_obj_set_style_text_color(telemetryValues, lv_color_hex(kTextColor), 0);
    lv_obj_set_style_text_font(telemetryValues, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_line_space(telemetryValues, 7, 0);
    lv_obj_set_style_text_align(telemetryValues, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(telemetryValues, LV_ALIGN_TOP_RIGHT, 0, 20);
}

void refreshTelemetry() {
    EnvReading env{};
    const bool hasEnv = envRead(env);
    const bool metric = gCfg.useMetric;

    char temp[16], humid[16], press[16], volts[16], charge[16], util[16], peak[16];

    if (hasEnv) {
        if (metric) {
            snprintf(temp, sizeof(temp), "%.1f C", env.temperatureC);
            snprintf(press, sizeof(press), "%.0f hPa", env.pressureHpa);
        } else {
            snprintf(temp, sizeof(temp), "%.1f F", env.temperatureC * 9.0f / 5.0f + 32.0f);
            // Two decimals: inHg spans roughly 28-31 over all normal weather, so
            // a rounded integer would show the same number for days.
            snprintf(press, sizeof(press), "%.2f inHg", env.pressureHpa * 0.02953f);
        }
        // Relative humidity is a ratio — the same number in both systems.
        if (env.hasHumidity) snprintf(humid, sizeof(humid), "%.0f %%", env.humidityPct);
        else                 snprintf(humid, sizeof(humid), "--");
    } else {
        snprintf(temp, sizeof(temp), "--");
        snprintf(humid, sizeof(humid), "--");
        snprintf(press, sizeof(press), "--");
    }

    // Show whatever the divider actually measured, even below the
    // pack-present threshold. Collapsing a real-but-wrong reading into "--"
    // hides the one number that distinguishes "no battery fitted" from "the
    // sense-enable polarity is inverted and we are reading a fraction of the
    // pack" — which look identical from the outside.
    const float v = batteryVoltage();
    if (v > 0.05f) {
        snprintf(volts, sizeof(volts), "%.2f V", v);
        snprintf(charge, sizeof(charge), "%u %%", batteryPercent());
    } else {
        snprintf(volts, sizeof(volts), "USB");
        snprintf(charge, sizeof(charge), "--");
    }

    snprintf(util, sizeof(util), "%.1f %%", Radio.channelUtilPercent());
    snprintf(peak, sizeof(peak), "%.1f %%", Radio.channelUtilMaxPercent());

    // Assembled with the C library's snprintf, then set as a finished string.
    // LVGL's builtin sprintf compiles out float support unless LV_USE_FLOAT is
    // set, so lv_label_set_text_fmt("%.1f") emits literal garbage rather than a
    // number — which is what put "f%" on the screen.
    char body[128];
    snprintf(body, sizeof(body), "%s\n%s\n%s\n%s\n%s\n%s\n%s",
             temp, humid, press, volts, charge, util, peak);
    lv_label_set_text(telemetryValues, body);
}


// ── MQTT census overlay ─────────────────────────────────────────────────────
void formatMqttCount(uint32_t n, char *out, size_t cap) {
    // A pegged counter prints with a trailing '+', so it reads as "at least
    // this many" rather than as a number that quietly stopped being true.
    if (n >= kMqttCountMax) snprintf(out, cap, "%lu+", (unsigned long)n);
    else                    snprintf(out, cap, "%lu", (unsigned long)n);
}

void formatSpan(uint32_t ms, char *out, size_t cap) {
    const uint32_t sec = ms / 1000U;
    if (sec < 60)   { snprintf(out, cap, "%lus", (unsigned long)sec); return; }
    if (sec < 3600) { snprintf(out, cap, "%lum", (unsigned long)(sec / 60)); return; }
    snprintf(out, cap, "%luh%02lum", (unsigned long)(sec / 3600),
             (unsigned long)((sec % 3600) / 60));
}

void closeMqttOverlay(lv_event_t *e) {
    LV_UNUSED(e);
    if (!mqttOverlay) return;
    mqttCensusStop();          // counting exists only while the screen is up
    // Async: this runs from a click on the close glyph, which is a child of the
    // object being freed. Deleting it inline frees the widget LVGL is still
    // dispatching an event through. lv_obj_delete_async defers it to the end of
    // the cycle, after the event has unwound.
    lv_obj_delete_async(mqttOverlay);
    mqttOverlay = nullptr;
    mqttStatusLabel = nullptr;
    mqttClockLabel = nullptr;
    mqttConnLabel = nullptr;
    mqttList = nullptr;
    memset(mqttCountLabels, 0, sizeof(mqttCountLabels));
    mqttRenderedRows = -1;
}

void buildMqttList() {
    if (!mqttList) return;
    lv_obj_clean(mqttList);
    memset(mqttCountLabels, 0, sizeof(mqttCountLabels));
    memset(mqttAccents, 0, sizeof(mqttAccents));

    const int rows = mqttChannelCount();
    mqttRenderedRows = rows;

    if (rows == 0) {
        lv_obj_t *empty = lv_label_create(mqttList);
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(kMutedColor), 0);
        if (!mqttCensusSeeded()) {
            lv_label_set_text(empty, "Loading channels recorded so far...");
        } else if (const char *why = mqttBlockedReason()) {
            lv_label_set_text_fmt(empty,
                "%s.\n\nNothing can arrive until the broker session is up. "
                "Fix it in the config portal and this list fills in on its own.", why);
        } else {
            lv_label_set_text_fmt(empty,
                "Watching %s/2/e/#\n\nNo messages yet. Channels appear here as they "
                "are published, with the number of messages seen on each.", gCfg.mqttRoot);
        }
        return;
    }

    char countText[16];
    for (int i = 0; i < rows && i < MQTT_MON_SLOTS; i++) {
        const MqttChannelStat *st = mqttChannelAt(i);
        if (!st) break;

        lv_obj_t *row = lv_obj_create(mqttList);
        // Two per line. 49% rather than 50% leaves room for the gutter
        // pad_column draws between them.
        lv_obj_set_width(row, lv_pct(49));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(kPanelColor), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_pad_column(row, 3, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        // The same accent stripe the metric cards carry, so a channel row reads
        // as the same kind of object as a dashboard cell. Fixed height rather
        // than a percentage: the row sizes to its content, and a percentage
        // child of a content-sized parent is circular.
        lv_obj_t *accent = lv_obj_create(row);
        lv_obj_set_size(accent, 3, 12);
        lv_obj_set_style_bg_color(accent, lv_color_hex(mqttAccentFor(st->lastHeardMs)), 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_remove_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

        // The channel takes whatever width is left; the count is the column
        // that has to stay readable, so it is the one with a reserved size.
        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(kTextColor), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_label_set_text(name, st->channel);

        formatMqttCount(st->topics, countText, sizeof(countText));
        lv_obj_t *count = lv_label_create(row);
        lv_obj_set_width(count, 38);
        lv_obj_set_style_text_font(count, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(count, lv_color_hex(kPrimaryColor), 0);
        lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(count, LV_LABEL_LONG_CLIP);
        lv_label_set_text(count, countText);
        mqttCountLabels[i] = count;
        mqttAccents[i] = accent;
    }
}

void refreshMqttOverlay() {
    if (!mqttOverlay) return;

    // Rebuild only when the *list* changed. Counts move constantly; rebuilding
    // 32 rows every refresh would drop frames and reset the scroll position
    // under the reader's finger.
    const uint32_t seq = mqttChannelSeq();
    if (seq != mqttRenderedSeq || mqttChannelCount() != mqttRenderedRows) {
        mqttRenderedSeq = seq;
        buildMqttList();
    }

    if (mqttClockLabel) {
        char clock[8];
        timeFormatClock(clock, sizeof(clock));
        const char *shownClock = lv_label_get_text(mqttClockLabel);
        if (!shownClock || strcmp(shownClock, clock) != 0) {
            lv_label_set_text(mqttClockLabel, clock);
        }
    }

    if (mqttStatusLabel) {
        char status[96];
        if (const char *why = mqttBlockedReason()) {
            snprintf(status, sizeof(status), "%s", why);
        } else {
            char total[16], span[16];
            formatMqttCount(mqttTotalMessages(), total, sizeof(total));
            // How long the session has been counting, so the message count has
            // a denominator — 400 messages means nothing without one.
            formatSpan(millis() - mqttCensusStartedMs(), span, sizeof(span));
            const uint32_t other = mqttOtherMessages();
            if (other) {
                char otherText[16];
                formatMqttCount(other, otherText, sizeof(otherText));
                snprintf(status, sizeof(status), "%d ch  %s msgs heard  %s  (+%s off-list)",
                         mqttChannelCount(), total, span, otherText);
            } else {
                snprintf(status, sizeof(status), "%d ch  %s msgs heard  %s",
                         mqttChannelCount(), total, span);
            }
        }
        const char *shown = lv_label_get_text(mqttStatusLabel);
        if (!shown || strcmp(shown, status) != 0) lv_label_set_text(mqttStatusLabel, status);
    }

    if (mqttConnLabel) {
        const bool up = mqttConnected();
        const char *text = up ? "Connection Up" : "Connection Down";
        const char *shownConn = lv_label_get_text(mqttConnLabel);
        if (!shownConn || strcmp(shownConn, text) != 0) {
            lv_label_set_text(mqttConnLabel, text);
            lv_obj_set_style_text_color(mqttConnLabel,
                                        lv_color_hex(up ? kStatusOkColor : kStatusBadColor), 0);
        }
    }

    char countText[16];
    const int rows = mqttChannelCount();
    for (int i = 0; i < rows && i < MQTT_MON_SLOTS; i++) {
        const MqttChannelStat *st = mqttChannelAt(i);
        if (!st || !mqttCountLabels[i]) continue;

        formatMqttCount(st->topics, countText, sizeof(countText));
        const char *shown = lv_label_get_text(mqttCountLabels[i]);
        if (!shown || strcmp(shown, countText) != 0) {
            lv_label_set_text(mqttCountLabels[i], countText);
        }
        if (mqttAccents[i]) {
            lv_obj_set_style_bg_color(mqttAccents[i],
                                      lv_color_hex(mqttAccentFor(st->lastHeardMs)), 0);
        }
    }
}

// Bottom row: what the accent colours mean. Without it the stripes are
// decoration — a colour nobody can decode is worse than no colour, because it
// looks like it is saying something.
void buildMqttLegend(lv_obj_t *parent) {
    struct Band { uint32_t color; const char *label; };
    static const Band kBands[] = {
        { kStatusOkColor,   "<1m"   },
        { kStatusWarnColor, "<15m"  },
        { kStatusBadColor,  "<1h"   },
        { kStatusOffColor,  "older" },
    };

    lv_obj_t *legend = lv_obj_create(parent);
    lv_obj_set_size(legend, DISPLAY_WIDTH - 16, 14);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_remove_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(legend, 0, 0);
    lv_obj_set_style_pad_all(legend, 0, 0);
    lv_obj_set_style_pad_column(legend, 4, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lead = lv_label_create(legend);
    lv_label_set_text(lead, "heard");
    lv_obj_set_style_text_font(lead, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lead, lv_color_hex(kMutedColor), 0);

    for (const Band &b : kBands) {
        // Same 3x12 stripe as the rows use, so the key is the thing itself
        // rather than an approximation of it.
        lv_obj_t *swatch = lv_obj_create(legend);
        lv_obj_set_size(swatch, 3, 10);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(b.color), 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_set_style_radius(swatch, 2, 0);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *text = lv_label_create(legend);
        lv_label_set_text(text, b.label);
        lv_obj_set_style_text_font(text, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(text, lv_color_hex(kMutedColor), 0);
    }
}

void openMqttOverlay(lv_event_t *e) {
    LV_UNUSED(e);
    if (mqttOverlay) return;

    mqttCensusStart();
    uplinkRequestMqttSeed();
    mqttRenderedSeq = mqttChannelSeq();
    mqttRenderedRows = -1;

    mqttOverlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(mqttOverlay, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_pos(mqttOverlay, 0, 0);
    lv_obj_remove_flag(mqttOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(mqttOverlay, lv_color_hex(kBackgroundColor), 0);
    lv_obj_set_style_bg_opa(mqttOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mqttOverlay, 0, 0);
    lv_obj_set_style_radius(mqttOverlay, 0, 0);
    lv_obj_set_style_pad_all(mqttOverlay, 8, 0);

    lv_obj_t *heading = lv_label_create(mqttOverlay);
    lv_label_set_text(heading, "MQTT");
    lv_obj_set_style_text_color(heading, lv_color_hex(kTextColor), 0);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 2, 0);

    // Close control. A bare label rather than a button widget: it needs to be a
    // small glyph in the corner, and a button's default padding would make the
    // hit area collide with the heading. The touch target is widened instead by
    // giving the label its own padding.
    lv_obj_t *closeBtn = lv_label_create(mqttOverlay);
    lv_label_set_text(closeBtn, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(closeBtn, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(closeBtn, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_all(closeBtn, 6, 0);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, 4, -6);
    lv_obj_add_flag(closeBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(closeBtn, closeMqttOverlay, LV_EVENT_CLICKED, nullptr);

    // Same clock as the main header. Opening this screen covers the dashboard
    // entirely, and the time is the one thing from it worth not losing.
    mqttClockLabel = lv_label_create(mqttOverlay);
    lv_obj_set_style_text_color(mqttClockLabel, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(mqttClockLabel, &lv_font_montserrat_16, 0);
    lv_label_set_text(mqttClockLabel, "--:--");
    lv_obj_align(mqttClockLabel, LV_ALIGN_TOP_MID, 0, 0);

    mqttStatusLabel = lv_label_create(mqttOverlay);
    // Narrowed to leave the connection state a lane of its own; without this
    // the status text would run under it when the channel count grows.
    lv_obj_set_width(mqttStatusLabel, DISPLAY_WIDTH - 120);
    lv_label_set_long_mode(mqttStatusLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(mqttStatusLabel, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(mqttStatusLabel, &lv_font_montserrat_12, 0);
    lv_label_set_text(mqttStatusLabel, "");
    lv_obj_align(mqttStatusLabel, LV_ALIGN_TOP_LEFT, 2, 22);

    // Right-aligned on the status row: whether the broker session is actually
    // up. A timestamp was the wrong instrument here — on a busy broker it only
    // ever reads as the current time, so it says nothing until the feed is
    // already long dead. This answers the same question at a glance.
    mqttConnLabel = lv_label_create(mqttOverlay);
    lv_obj_set_style_text_font(mqttConnLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(mqttConnLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(mqttConnLabel, lv_color_hex(kStatusBadColor), 0);
    lv_label_set_text(mqttConnLabel, "Connection Down");
    lv_obj_align(mqttConnLabel, LV_ALIGN_TOP_RIGHT, 0, 22);

    mqttList = lv_obj_create(mqttOverlay);
    lv_obj_set_size(mqttList, DISPLAY_WIDTH - 16, DISPLAY_HEIGHT - 74);
    lv_obj_align(mqttList, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_bg_opa(mqttList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mqttList, 0, 0);
    lv_obj_set_style_pad_all(mqttList, 0, 0);
    // The scrollbar is drawn inside the object's right edge, over whatever is
    // there. The counts are right-aligned and would sit underneath it, so the
    // content is inset by more than the bar is wide and the bar gets a gutter
    // of its own.
    lv_obj_set_style_pad_right(mqttList, 10, 0);
    lv_obj_set_style_pad_row(mqttList, 3, 0);
    lv_obj_set_style_pad_column(mqttList, 3, 0);
    lv_obj_set_flex_flow(mqttList, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollbar_mode(mqttList, LV_SCROLLBAR_MODE_AUTO);

    buildMqttList();
    buildMqttLegend(mqttOverlay);
    refreshMqttOverlay();
}

void refreshMetrics(lv_timer_t *timer) {
    LV_UNUSED(timer);
    const UplinkStats u = uplinkStats();

    lv_label_set_text_fmt(heardValue, "%u", u.known);
    // Distinct MQTT channels in the ingestor, not a local tally: the store
    // aggregates every monitor that has reported to it, so this figure is only
    // the server's to give. Dashes until the first fetch lands, for the same
    // reason the totals below use them.
    if (u.mqttChannelsKnown) {
        lv_label_set_text_fmt(msgsHeardValue, "%lu",
                              static_cast<unsigned long>(u.mqttChannels));
    } else {
        lv_label_set_text(msgsHeardValue, "--");
    }

    if (u.messagesTotalKnown) {
        lv_label_set_text_fmt(msgsTotalValue, "%lu",
                              static_cast<unsigned long>(u.messagesTotal));
    } else {
        lv_label_set_text(msgsTotalValue, "--");
    }

    // Dashes rather than a number until the boot fetch lands. Showing the
    // locally-added count alone would read as the ingestor's total and be wrong
    // by however much it already held — which, on a mesh this device has been
    // watching for a while, is nearly all of it.
    if (u.ingestorTotalKnown) {
        lv_label_set_text_fmt(storedValue, "%lu",
                              static_cast<unsigned long>(u.ingestorTotal));
    } else {
        lv_label_set_text(storedValue, "--");
    }

    char clock[8];
    timeFormatClock(clock, sizeof(clock));
    lv_label_set_text(clockLabel, clock);

    refreshTelemetry();
    refreshStatusIcons();
    refreshMqttOverlay();
}

lv_obj_t *createStatusIcon(lv_obj_t *parent, const char *symbol) {
    lv_obj_t *icon = lv_label_create(parent);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(kStatusOffColor), 0);
    return icon;
}

void setIconColor(lv_obj_t *icon, uint32_t color) {
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
}

// Each icon answers one question, and only ever reports what it can actually
// observe. Nothing here infers: a GPS with satellites but no fix is amber
// rather than green, because "receiving" is not "knows where it is".
void refreshStatusIcons() {
    // ── GPS ─────────────────────────────────────────────────────────────────
    // Green once the receiver is talking to satellites, not only once it has
    // resolved a fix. Grey means nothing is being heard at all — an antenna or
    // wiring problem — which is the distinction worth showing at a glance. The
    // fix itself is visible in the portal, alongside the satellite count.
    if (gpsHasFix() || gpsSatellites() > 0) setIconColor(gpsIcon, kStatusOkColor);
    else                                    setIconColor(gpsIcon, kStatusOffColor);

    // ── WiFi ────────────────────────────────────────────────────────────────
    // The SoftAP is amber, not green: the device is serving its config portal
    // but has no route to anything, so calling it "connected" would be wrong in
    // exactly the situation where the user is trying to fix the connection.
    if (WiFi.status() == WL_CONNECTED) setIconColor(wifiIcon, kStatusOkColor);
    else if (webCfgIsAp())             setIconColor(wifiIcon, kStatusWarnColor);
    else                               setIconColor(wifiIcon, kStatusBadColor);

    // ── Ingestor endpoint ───────────────────────────────────────────────────
    switch (uplinkState()) {
        case UplinkState::Ok:        setIconColor(apiIcon, kStatusOkColor);   break;
        case UplinkState::Failing:   setIconColor(apiIcon, kStatusBadColor);  break;
        case UplinkState::Idle:      setIconColor(apiIcon, kStatusWarnColor); break;
        case UplinkState::NoNetwork: setIconColor(apiIcon, kStatusBadColor);  break;
        case UplinkState::Disabled:  setIconColor(apiIcon, kStatusOffColor);  break;
    }
}

// Provisional serial trace of everything heard, standing in until the monitor
// UI lands. Node identity and statistics are the next milestone; for now this
// exists so radio bring-up can be judged from a serial log alone.
void logPacket(const MeshPacket &pkt) {
    Serial.printf("[rx] %08lx -> %08lx  id=%08lx  ch=%02x  rssi=%.0f snr=%.1f  hops=%u/%u  ",
                  (unsigned long)pkt.hdr.from, (unsigned long)pkt.hdr.to,
                  (unsigned long)pkt.hdr.id, pkt.hdr.channel,
                  pkt.rssi, pkt.snr,
                  (unsigned)(pkt.hdr.flags & 0x07),          // hops remaining
                  (unsigned)((pkt.hdr.flags >> 5) & 0x07));  // hops at origin

    if (!pkt.decrypted) {
        // Channel hash 0 is PKI; anything else is a channel we hold no key for.
        Serial.printf("%s\n", pkt.hdr.channel == 0 ? "PKI (sealed)" : "encrypted (no key)");
        return;
    }

    Serial.printf("%s", portnumName(pkt.portnum));
    switch (pkt.portnum) {
        case NODEINFO_APP: {
            UserInfo u{};
            if (decodeUser(pkt.payload, pkt.payloadLen, u)) {
                Serial.printf("  \"%s\" (%s)", u.longName, u.shortName);
            }
            break;
        }
        case POSITION_APP: {
            PositionInfo p{};
            if (decodePosition(pkt.payload, pkt.payloadLen, p)) {
                Serial.printf("  %.5f, %.5f  %ldm", p.latI / 1e7, p.lonI / 1e7, (long)p.alt);
            }
            break;
        }
        case TELEMETRY_APP: {
            TelemetryInfo t{};
            if (decodeTelemetry(pkt.payload, pkt.payloadLen, t) && t.hasDeviceMetrics) {
                Serial.printf("  batt=%.0f%% %.2fV  chUtil=%.1f%%", t.battPct, t.voltage, t.chUtil);
            }
            break;
        }
        default:
            Serial.printf("  %u bytes", (unsigned)pkt.payloadLen);
            break;
    }
    Serial.println();
}

// Called by the config portal once new settings are stored. The portal owns no
// hardware; everything that has to touch the radio happens here, on the main
// thread, so a retune cannot land in the middle of an HTTP handler.
void applyConfig() {
    configApplyChannels(gCfg);
    configApplyTimezone(gCfg);
    const ResolvedModem m = configResolveModem(gCfg);
    if (Radio.isReady()) {
        Radio.reconfigure(m.freq, m.bw, m.sf, m.cr);
    }
}

void createDashboard() {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBackgroundColor), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // A flex row rather than absolute positions. The title is 24pt and the icons
    // sit at the other end of a 320px header, so hardcoded offsets would depend
    // on the exact rendered width of "Camillia Monitor" — a number that changes
    // with the font and silently overlaps the icons when it grows.
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, DISPLAY_WIDTH, 34);
    lv_obj_set_pos(header, 0, 6);
    lv_obj_set_style_pad_left(header, 12, 0);
    lv_obj_set_style_pad_right(header, 10, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Monitor");
    lv_obj_set_style_text_color(title, lv_color_hex(kTextColor), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title, openMqttOverlay, LV_EVENT_CLICKED, nullptr);

    // Middle child of the SPACE_BETWEEN row, so it sits centred in the gap
    // between the title and the icon group and stays there as either changes
    // width — the clock is the same 5 characters whatever the time.
    clockLabel = lv_label_create(header);
    lv_label_set_text(clockLabel, "--:--");
    lv_obj_set_style_text_color(clockLabel, lv_color_hex(kMutedColor), 0);
    lv_obj_set_style_text_font(clockLabel, &lv_font_montserrat_16, 0);

    lv_obj_t *icons = lv_obj_create(header);
    lv_obj_remove_style_all(icons);
    lv_obj_set_size(icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(icons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icons, 10, 0);

    // Left to right: GPS, WiFi, endpoint — the order they come up in, and the
    // order they depend on each other. The uplink is last because it is the
    // only one that needs another of them to be working.
    gpsIcon  = createStatusIcon(icons, LV_SYMBOL_GPS);
    wifiIcon = createStatusIcon(icons, LV_SYMBOL_WIFI);
    apiIcon  = createStatusIcon(icons, LV_SYMBOL_UPLOAD);

    const int32_t rowPitch = kLeftCardH + kGutter;
    createMetricCard(screen, kColLeftX, kBodyTop + rowPitch * 0,
                     "NODES HEARD", kPrimaryColor, &heardValue);
    createMetricCard(screen, kColLeftX, kBodyTop + rowPitch * 1,
                     "TOTAL NODES", 0x6CA9E8, &storedValue);
    createMetricCard(screen, kColLeftX, kBodyTop + rowPitch * 2,
                     "CHANNELS HEARD", kSecondaryColor, &msgsHeardValue);
    createMetricCard(screen, kColLeftX, kBodyTop + rowPitch * 3,
                     "TOTAL MESSAGES", 0xB98CE8, &msgsTotalValue);
    createTelemetryPanel(screen);

    refreshMetrics(nullptr);
    lv_timer_create(refreshMetrics, 1000, nullptr);
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(120);
    Serial.printf("\n[boot] Camillia Monitor %s\n", APP_VERSION);

    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);
    pinMode(BOARD_VEXT_ENABLE, OUTPUT);
    digitalWrite(BOARD_VEXT_ENABLE, BOARD_VEXT_ON_LEVEL);
    delay(20);

    display.init();
    display.setRotation(TFT_ROTATION_DEFAULT);
    display.setBrightness(TFT_BRIGHTNESS_DEFAULT);
    display.fillScreen(TFT_BLACK);

    lv_init();
    // Wrapped rather than cast: millis() returns unsigned long and the callback
    // wants uint32_t. Same width on this target, but they are distinct types, so
    // a static_cast between the function pointers is ill-formed.
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    lv_display_t *lvDisplay = lv_display_create(display.width(), display.height());
    lv_display_set_color_format(lvDisplay, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(lvDisplay, flushDisplay);
    lv_display_set_buffers(lvDisplay, drawBuffer, nullptr, sizeof(drawBuffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *touchInput = lv_indev_create();
    lv_indev_set_type(touchInput, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchInput, readTouch);
    lv_indev_set_display(touchInput, lvDisplay);
    lv_indev_set_scroll_limit(touchInput, 22);

    createDashboard();
    Serial.printf("[boot] display=%dx%d heap=%u psram=%u\n",
                  display.width(), display.height(), ESP.getFreeHeap(), ESP.getFreePsram());

    configLoad();
    configNvsReport("boot");
    configApplyTimezone(gCfg);
    configApplyChannels(gCfg);

    if (!Radio.init()) {
        Serial.println("[boot] radio bring-up FAILED — monitor is deaf");
    } else {
        // Tune to whatever the operator configured. init() comes up on the
        // compile-time default, which is only right for an unconfigured device.
        const ResolvedModem m = configResolveModem(gCfg);
        Radio.reconfigure(m.freq, m.bw, m.sf, m.cr);
    }

    uplinkBegin();
    gpsBegin();
    envBegin();
    batteryBegin();
    webCfgBegin(applyConfig);

    // After the network is up: SNTP can only arm once an interface exists.
    timeSyncBegin();
    mqttBegin();
}

void loop() {
    // Drain the radio before servicing the UI. A packet sits in the SX1262's
    // buffer until it is read, and the next one overwrites it, so reception has
    // to win against anything that might block: lv_timer_handler() is normally
    // sub-millisecond but is not bounded.
    MeshPacket pkt;
    while (Radio.pollRx(pkt)) {
        logPacket(pkt);
        uplinkNotePacket(pkt);
    }

    webCfgLoop();
    timeSyncLoop();
    mqttLoop();
    gpsLoop();
    uplinkLoop();

    lv_timer_handler();
    delay(5);
}