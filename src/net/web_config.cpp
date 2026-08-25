// ════════════════════════════════════════════════════════════════════════════
// net/web_config.cpp — WiFi bring-up and the settings portal
// ════════════════════════════════════════════════════════════════════════════
#include "web_config.h"
#include "monitor_config.h"
#include "mesh_channel_plan.h"
#include "mesh_radio.h"
#include "node_uplink.h"
#include "time_sync.h"
#include "gps.h"
#include "env_sensor.h"
#include "mqtt_monitor.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

namespace {

WebServer server(80);
DNSServer gDns;

bool  gRunning   = false;
bool  gApMode    = false;
bool  gCaptive   = false;
char  gIp[20]    = "";
WebCfgApplyCb gApply = nullptr;

// Last time a request was served, so a background rejoin never interrupts
// someone in the middle of configuring the device.
uint32_t gLastRequestMs = 0;
uint32_t gNextRejoinMs  = 0;

void noteRequest() { gLastRequestMs = millis(); }

// WiFi.status() as a word. "Timed out" alone cannot distinguish an AP that was
// never seen from one that refused the password from one that associated and
// then stalled waiting on DHCP — three different problems with three different
// fixes.
const char *wifiStatusName(wl_status_t st) {
    switch (st) {
        case WL_IDLE_STATUS:     return "idle";
        case WL_NO_SSID_AVAIL:   return "SSID not found";
        case WL_SCAN_COMPLETED:  return "scan complete";
        case WL_CONNECTED:       return "connected";
        case WL_CONNECT_FAILED:  return "connect failed (bad password?)";
        case WL_CONNECTION_LOST: return "connection lost";
        case WL_DISCONNECTED:    return "disconnected";
        default:                 return "unknown";
    }
}

// Run once when every attempt has failed. Says whether the network is even on
// the air and how strong it is here, which is the difference between "move the
// device" and "check the credentials".
void logNetworkScan() {
    Serial.println("[web] scanning for the configured network ...");
    const int found = WiFi.scanNetworks();
    if (found <= 0) {
        Serial.println("[web] scan found no networks at all — check the antenna or the band");
        WiFi.scanDelete();
        return;
    }

    bool seen = false;
    for (int i = 0; i < found; i++) {
        if (WiFi.SSID(i) != gCfg.wifiSsid) continue;
        seen = true;
        Serial.printf("[web] \"%s\" is on the air: rssi=%d dBm  channel=%d  enc=%d\n",
                      gCfg.wifiSsid, WiFi.RSSI(i), WiFi.channel(i), (int)WiFi.encryptionType(i));
        if (WiFi.RSSI(i) < -80) {
            Serial.println("[web] that is a weak signal; association often fails below -80 dBm");
        }
    }
    if (!seen) {
        Serial.printf("[web] \"%s\" was not among the %d networks seen — "
                      "wrong name, 5 GHz only, or out of range\n", gCfg.wifiSsid, found);
    }
    WiFi.scanDelete();
}

// 15 s was marginal. An ESP32-S3 associating with a busy 2.4 GHz network and
// then waiting on DHCP can legitimately take longer, and the cost of being
// patient at boot is far lower than the cost of giving up.
constexpr uint32_t kConnectTimeoutMs = 25000;

// Attempts before falling back to the SoftAP. A router that is still coming up
// often refuses the first association and accepts the next.
constexpr uint8_t kJoinAttempts = 2;

// While parked on the fallback AP, how often to try the stored network again,
// and how long the portal must have been idle before doing so.
constexpr uint32_t kRejoinIntervalMs = 60000;
constexpr uint32_t kPortalIdleMs     = 60000;
constexpr char     kApSsid[]         = "camillia-monitor";
constexpr char     kAuthUser[]       = "admin";

// A message shown once at the top of the next page render.
String gNotice;
bool   gNoticeIsError = false;

void notice(const char *msg, bool isError = false) {
    gNotice = msg;
    gNoticeIsError = isError;
}

// ── Auth ────────────────────────────────────────────────────────────────────
// HTTP Basic rather than camillia-mt's cookie session: there is one operator
// and one form here, and Basic is a few lines instead of a login page plus
// session handling. It is only as private as the link, which is why the portal
// says so rather than implying otherwise.
bool guard() {
    if (!gCfg.webAuthEnabled || !gCfg.webPass[0]) return true;
    if (server.authenticate(kAuthUser, gCfg.webPass)) return true;
    server.requestAuthentication();
    return false;
}

// ── HTML helpers ────────────────────────────────────────────────────────────
String esc(const char *s) {
    String out;
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += *p;          break;
        }
    }
    return out;
}

const char kStyle[] PROGMEM = R"CSS(
:root{color-scheme:dark;--bg:#0a1014;--panel:#172129;--line:#243542;--fg:#f4f7f8;
--muted:#8ea0aa;--accent:#53d6b5;--warn:#f3b562;--err:#e87878}
*{box-sizing:border-box}
body{margin:0;padding:24px 16px 64px;background:var(--bg);color:var(--fg);
font:15px/1.5 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:680px;margin:0 auto}
h1{font-size:20px;margin:0 0 2px;letter-spacing:.02em}
.sub{color:var(--muted);font-size:13px;margin:0 0 20px}
fieldset{border:1px solid var(--line);border-radius:8px;padding:16px;margin:0 0 16px;
background:var(--panel)}
legend{padding:0 8px;color:var(--accent);font-size:12px;letter-spacing:.09em;
text-transform:uppercase}
label{display:block;margin:12px 0 4px;font-size:13px;color:var(--muted)}
label:first-of-type{margin-top:0}
input[type=text],input[type=password],input[type=number],select,textarea{
width:100%;padding:8px 10px;background:#0f171d;color:var(--fg);
border:1px solid var(--line);border-radius:5px;font:inherit;font-size:14px}
textarea{min-height:130px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;
font-size:12px;resize:vertical}
input:focus,select:focus,textarea:focus{outline:none;border-color:var(--accent)}
.row{display:flex;gap:12px}.row>*{flex:1}
.chk{display:flex;align-items:center;gap:8px;margin:14px 0 0;color:var(--fg);font-size:14px}
.chk input{width:16px;height:16px;accent-color:var(--accent)}
.hint{color:var(--muted);font-size:12px;margin:4px 0 0}
button{padding:9px 18px;border-radius:5px;border:0;font:inherit;font-weight:600;
cursor:pointer;background:var(--accent);color:#06231c}
button.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}
button.danger{background:transparent;color:var(--err);border:1px solid var(--err)}
a.btn{display:inline-block;padding:9px 18px;border-radius:5px;text-decoration:none;
border:1px solid var(--line);color:var(--fg);font-weight:600;font-size:15px}
.actions{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:16px}
table.kv{width:100%;border-collapse:collapse;font-size:13px}
table.kv td{padding:5px 0;border-bottom:1px solid var(--line)}
table.kv td:first-child{color:var(--muted);width:45%}
table.kv tr:last-child td{border-bottom:0}
.note{padding:10px 12px;border-radius:5px;margin:0 0 16px;font-size:14px;
background:rgba(83,214,181,.12);border:1px solid var(--accent);color:var(--accent)}
.note.err{background:rgba(232,120,120,.12);border-color:var(--err);color:var(--err)}
.ch{border-top:1px solid var(--line);padding-top:12px;margin-top:14px}
.ch:first-of-type{border-top:0;padding-top:0;margin-top:0}
)CSS";

void sendHead(const char *title) {
    server.sendContent(F("<!doctype html><html><head><meta charset=utf-8>"
                         "<meta name=viewport content='width=device-width,initial-scale=1'>"
                         "<title>"));
    server.sendContent(title);
    server.sendContent(F("</title><style>"));
    server.sendContent_P(kStyle);
    server.sendContent(F("</style></head><body><div class=wrap>"));
}

void sendNotice() {
    if (!gNotice.length()) return;
    server.sendContent(gNoticeIsError ? F("<p class='note err'>") : F("<p class=note>"));
    server.sendContent(gNotice);
    server.sendContent(F("</p>"));
    gNotice = "";
}

void beginChunked() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
}

// ── Status block ────────────────────────────────────────────────────────────
void sendStatus() {
    const ResolvedModem m = configResolveModem(gCfg);
    const RadioRxHealth h = MeshRadio::rxHealth();
    const uint32_t up = millis() / 1000U;

    char buf[160];
    server.sendContent(F("<fieldset><legend>Status</legend><table class=kv>"));

    snprintf(buf, sizeof(buf), "<tr><td>Uptime</td><td>%luh %02lum %02lus</td></tr>",
             (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
             (unsigned long)(up % 60));
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
             "<tr><td>Listening on</td><td>%.4f MHz &middot; SF%u &middot; BW%.0f &middot; CR4/%u</td></tr>",
             m.freq, m.sf, m.bw, m.cr);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Frequency slot</td><td>%lu of %lu</td></tr>",
             (unsigned long)m.slot, (unsigned long)m.slotCount);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Radio</td><td>%s</td></tr>",
             Radio.isReady() ? "ready" : "<span style='color:var(--err)'>not initialised</span>");
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
             "<tr><td>Packets heard</td><td>%lu ok &middot; %lu CRC fail &middot; %lu bad length</td></tr>",
             (unsigned long)h.good, (unsigned long)h.crcErr, (unsigned long)h.badLen);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Channel utilisation</td><td>%.1f%% of the last hour</td></tr>",
             Radio.channelUtilPercent());
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Network</td><td>%s &middot; %s</td></tr>",
             gApMode ? "SoftAP" : "station", gIp);
    server.sendContent(buf);

    const UplinkStats u = uplinkStats();
    snprintf(buf, sizeof(buf),
             "<tr><td>Nodes heard</td><td>%u known &middot; %u queued to send</td></tr>",
             u.known, u.pending);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf),
             "<tr><td>Uplink</td><td>%s &middot; %lu sent &middot; %lu failed</td></tr>",
             gCfg.ingestEnabled ? "on" : "off",
             (unsigned long)u.sent, (unsigned long)u.failed);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Reporting as</td><td>%s</td></tr>", uplinkOurNodeId());
    server.sendContent(buf);

    // Dropped observations. These counters exist to make loss visible; leaving
    // them uncounted-for on screen would be the same as not having them. A
    // healthy device shows zeros, so the row is only interesting when it is not.
    if (u.dropped || u.messagesDropped) {
        snprintf(buf, sizeof(buf),
                 "<tr><td>Dropped</td><td style='color:var(--warn)'>%u nodes (table full) "
                 "&middot; %u messages (queue full)</td></tr>",
                 u.dropped, u.messagesDropped);
    } else {
        snprintf(buf, sizeof(buf),
                 "<tr><td>Dropped</td><td>none &middot; %u messages queued</td></tr>",
                 u.messagesQueued);
    }
    server.sendContent(buf);

    char stamp[40];
    timeFormatUtc(stamp, sizeof(stamp));
    snprintf(buf, sizeof(buf), "<tr><td>Clock (UTC)</td><td>%s &middot; %s</td></tr>",
             stamp, timeSourceName());
    server.sendContent(buf);

    if (gpsHasPosition()) {
        snprintf(buf, sizeof(buf),
                 "<tr><td>GPS</td><td>%.5f, %.5f &middot; %u satellites</td></tr>",
                 gpsLatitudeI() / 1e7, gpsLongitudeI() / 1e7, gpsSatellites());
    } else {
        snprintf(buf, sizeof(buf), "<tr><td>GPS</td><td>no fix &middot; %u satellites</td></tr>",
                 gpsSatellites());
    }
    server.sendContent(buf);

    // The on-device footer that used to carry this is gone, so the portal is
    // now the only place the build identifies itself.
    snprintf(buf, sizeof(buf),
             "<tr><td>Firmware</td><td>%s &middot; Heltec V4 expansion</td></tr>", APP_VERSION);
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Environment sensor</td><td>%s</td></tr>",
             envSensorName());
    server.sendContent(buf);

    snprintf(buf, sizeof(buf), "<tr><td>Free heap</td><td>%lu KB</td></tr>",
             (unsigned long)(ESP.getFreeHeap() / 1024));
    server.sendContent(buf);

    server.sendContent(F("</table></fieldset>"));
}

// ── Settings form ───────────────────────────────────────────────────────────
void sendForm() {
    char buf[320];
    char psk[48];

    server.sendContent(F("<form method=post action=/save autocomplete=off>"));

    // Identity
    server.sendContent(F("<fieldset><legend>Identity</legend>"
                         "<label>Monitor name</label>"));
    snprintf(buf, sizeof(buf), "<input type=text name=name maxlength=31 value=\"%s\">",
             esc(gCfg.monitorName).c_str());
    server.sendContent(buf);
    server.sendContent(F("<p class=hint>How this device labels itself to the ingestor. "
                         "Not announced on the mesh &mdash; the monitor never transmits.</p>"
                         "</fieldset>"));

    // WiFi
    server.sendContent(F("<fieldset><legend>WiFi</legend><label>Network (SSID)</label>"));
    snprintf(buf, sizeof(buf), "<input type=text name=ssid maxlength=32 autocomplete=off value=\"%s\">",
             esc(gCfg.wifiSsid).c_str());
    server.sendContent(buf);
    server.sendContent(F("<label>Password</label>"
                         "<input type=password name=pass maxlength=63 autocomplete=new-password "
                         "placeholder='(unchanged)'>"
                         "<p class=hint>Leave blank to keep the stored password. "
                         "Changing the network reboots the device.</p></fieldset>"));

    // Ingestor
    server.sendContent(F("<fieldset><legend>Ingestor</legend>"));
    snprintf(buf, sizeof(buf),
             "<div class=chk><input type=checkbox name=ingest id=ingest %s>"
             "<label for=ingest style='margin:0'>Send discovered nodes to the ingestor</label></div>",
             gCfg.ingestEnabled ? "checked" : "");
    server.sendContent(buf);
    server.sendContent(F("<label>API root</label>"));
    snprintf(buf, sizeof(buf),
             "<input type=text name=url maxlength=159 value=\"%s\" "
             "placeholder='http://10.0.0.5:3000/api'>", esc(gCfg.ingestUrl).c_str());
    server.sendContent(buf);
    server.sendContent(F("<p class=hint>Root of the ingestor API, not one route &mdash; "
                         "the firmware calls four of them. Pasting a full "
                         "<code>/nodes/heard</code> URL is fine; it is trimmed to the "
                         "root on save.</p>"
                         "<label>API key (optional)</label>"
                         "<input type=password name=token maxlength=79 autocomplete=new-password "
                         "placeholder='(unchanged)'>"
                         "<p class=hint>Sent as <code>x-api-key</code>. Leave blank if the "
                         "ingestor runs with <code>INGEST_API_KEYS</code> unset.</p>"
                         "<label>Re-report interval (seconds)</label>"));
    snprintf(buf, sizeof(buf), "<input type=number name=interval min=5 max=3600 value=%lu>",
             (unsigned long)gCfg.ingestIntervalS);
    server.sendContent(buf);
    server.sendContent(F("<p class=hint>A node is reported the moment it is first heard and "
                         "whenever something new is learned about it. This interval only "
                         "controls how often an unchanged node is reported again, to keep "
                         "its last-heard time current.</p>"));
    server.sendContent(F("</fieldset>"));

    // MQTT
    server.sendContent(F("<fieldset><legend>MQTT</legend>"));
    snprintf(buf, sizeof(buf),
             "<div class=chk><input type=checkbox name=mqtt id=mqtt %s>"
             "<label for=mqtt style='margin:0'>Watch an MQTT broker</label></div>",
             gCfg.mqttEnabled ? "checked" : "");
    server.sendContent(buf);
    server.sendContent(F("<div class=row><div><label>Broker</label>"));
    snprintf(buf, sizeof(buf), "<input type=text name=mqtthost maxlength=63 value=\"%s\">",
             esc(gCfg.mqttServer).c_str());
    server.sendContent(buf);
    server.sendContent(F("</div><div><label>Port</label>"));
    snprintf(buf, sizeof(buf), "<input type=number name=mqttport min=1 max=65535 value=%u>",
             gCfg.mqttPort);
    server.sendContent(buf);
    server.sendContent(F("</div></div><div class=row><div><label>Username</label>"));
    snprintf(buf, sizeof(buf), "<input type=text name=mqttuser maxlength=31 value=\"%s\">",
             esc(gCfg.mqttUser).c_str());
    server.sendContent(buf);
    server.sendContent(F("</div><div><label>Password</label>"
                         "<input type=password name=mqttpass maxlength=47 "
                         "autocomplete=new-password placeholder='(unchanged)'></div></div>"
                         "<label>Topic root</label>"));
    snprintf(buf, sizeof(buf),
             "<input type=text name=mqttroot maxlength=47 value=\"%s\" placeholder='msh/US'>",
             esc(gCfg.mqttRoot).c_str());
    server.sendContent(buf);
    snprintf(buf, sizeof(buf),
             "<div class=chk><input type=checkbox name=mqtttls id=mqtttls %s>"
             "<label for=mqtttls style='margin:0'>Connect with TLS</label></div>",
             gCfg.mqttTls ? "checked" : "");
    server.sendContent(buf);
    server.sendContent(F("<p class=hint>Subscribe-only: the device watches "
                         "<code>&lt;root&gt;/2/e/#</code> and counts what arrives, per "
                         "channel. It never publishes, and MQTT traffic is deliberately "
                         "kept out of the node and message totals &mdash; those stay a "
                         "record of what this radio heard over the air. Tap "
                         "<em>Monitor</em> on the device screen to watch the census. "
                         "TLS is unauthenticated (no certificate pinning).</p>"));
    if (mqttTopicsMissed()) {
        snprintf(buf, sizeof(buf),
                 "<p class=hint>Broker session: <strong>%s</strong> &middot; "
                 "<span style='color:var(--warn)'>%lu topics missed</span> (arriving faster "
                 "than they can be reported)</p>",
                 mqttConnected() ? "connected" : (mqttBlockedReason() ? mqttBlockedReason() : "idle"),
                 (unsigned long)mqttTopicsMissed());
    } else {
        snprintf(buf, sizeof(buf), "<p class=hint>Broker session: <strong>%s</strong></p>",
                 mqttConnected() ? "connected" : (mqttBlockedReason() ? mqttBlockedReason() : "idle"));
    }
    server.sendContent(buf);
    server.sendContent(F("</fieldset>"));

    // Radio
    server.sendContent(F("<fieldset><legend>Radio</legend><label>Region</label>"
                         "<select name=region>"));
    for (uint8_t i = 0; i < kRegionCount; i++) {
        snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s (%.3f&ndash;%.3f MHz)</option>",
                 kRegions[i].code,
                 strcmp(kRegions[i].code, gCfg.region) == 0 ? " selected" : "",
                 kRegions[i].code, kRegions[i].freqStart, kRegions[i].freqEnd);
        server.sendContent(buf);
    }
    server.sendContent(F("</select>"));

    snprintf(buf, sizeof(buf),
             "<div class=chk><input type=checkbox name=preset id=preset %s>"
             "<label for=preset style='margin:0'>Use a standard modem preset</label></div>",
             gCfg.usePreset ? "checked" : "");
    server.sendContent(buf);

    server.sendContent(F("<label>Preset</label><select name=presetidx>"));
    for (uint8_t i = 0; i < PRESET_COUNT; i++) {
        snprintf(buf, sizeof(buf), "<option value=%u%s>%s &mdash; SF%u, BW%.0f, CR4/%u</option>",
                 i, (gCfg.modemPreset == i) ? " selected" : "",
                 kPresets[i].name, kPresets[i].sf, kPresets[i].bw, kPresets[i].cr);
        server.sendContent(buf);
    }
    server.sendContent(F("</select>"
                         "<p class=hint>With a preset selected the bandwidth, spreading factor "
                         "and coding rate below are ignored.</p><div class=row><div>"
                         "<label>Bandwidth</label><select name=bw>"));
    for (uint8_t i = 0; i < kBwCodeCount; i++) {
        snprintf(buf, sizeof(buf), "<option value=%u%s>%.2f kHz</option>",
                 kBwCodes[i], (gCfg.bwCode == kBwCodes[i]) ? " selected" : "",
                 loraBwFromCode(kBwCodes[i]));
        server.sendContent(buf);
    }
    server.sendContent(F("</select></div><div><label>Spreading factor</label>"));
    snprintf(buf, sizeof(buf), "<input type=number name=sf min=%d max=%d value=%u>",
             LORA_SF_MIN, LORA_SF_MAX, gCfg.loraSf);
    server.sendContent(buf);
    server.sendContent(F("</div><div><label>Coding rate (4/n)</label>"));
    snprintf(buf, sizeof(buf), "<input type=number name=cr min=%d max=%d value=%u>",
             LORA_CR_MIN, LORA_CR_MAX, gCfg.loraCr);
    server.sendContent(buf);
    server.sendContent(F("</div></div>"));

    snprintf(buf, sizeof(buf),
             "<div class=chk><input type=checkbox name=pinslot id=pinslot %s>"
             "<label for=pinslot style='margin:0'>Pin the frequency slot</label></div>"
             "<label>Slot number</label><input type=number name=slot min=0 value=%lu>",
             gCfg.pinSlot ? "checked" : "", (unsigned long)gCfg.freqSlot);
    server.sendContent(buf);
    server.sendContent(F("<p class=hint>Unpinned, the slot is derived from a hash of the "
                         "channel name, exactly as Meshtastic does it. Pin it when running "
                         "custom settings &mdash; a narrow bandwidth splits the band into "
                         "hundreds of slots and the hash is unlikely to land on the one "
                         "your mesh uses.</p></fieldset>"));

    // Channels
    server.sendContent(F("<fieldset><legend>Channels</legend>"
                         "<p class=hint style='margin:0 0 14px'>Keys the monitor decrypts with. "
                         "Slot 0 is the primary. A PSK is base64 &mdash; "
                         "<code>AQ==</code> is the public default key. Leave a slot blank to "
                         "disable it.</p>"));
    for (int i = 0; i < MESH_CHANNELS; i++) {
        configFormatPsk(gCfg.channels[i].key, gCfg.channels[i].keyLen, psk, sizeof(psk));
        snprintf(buf, sizeof(buf),
                 "<div class=ch><div class=row><div><label>Slot %d name</label>"
                 "<input type=text name=cn%d maxlength=15 value=\"%s\"></div>"
                 "<div><label>PSK</label>"
                 "<input type=text name=ck%d maxlength=47 value=\"%s\"></div></div></div>",
                 i, i, esc(gCfg.channels[i].name).c_str(), i, esc(psk).c_str());
        server.sendContent(buf);
    }
    server.sendContent(F("</fieldset>"));

    // Display
    server.sendContent(F("<fieldset><legend>Display</legend><label>Units</label>"
                         "<select name=units>"));
    snprintf(buf, sizeof(buf),
             "<option value=0%s>Imperial (&deg;F, inHg)</option>"
             "<option value=1%s>Metric (&deg;C, hPa)</option>",
             gCfg.useMetric ? "" : " selected", gCfg.useMetric ? " selected" : "");
    server.sendContent(buf);
    server.sendContent(F("</select><p class=hint>Affects the screen only. Everything "
                         "reported to the ingestor stays in the units Meshtastic defines, "
                         "so a consumer of that data never has to know how this device is "
                         "configured.</p>"
                         "<label>Timezone</label><select name=tz>"));
    bool tzMatched = false;
    for (uint8_t i = 0; i < kTzOptionCount; i++) {
        const bool sel = (strcmp(gCfg.tz, kTzOptions[i].posix) == 0);
        if (sel) tzMatched = true;
        snprintf(buf, sizeof(buf), "<option value=\"%s\"%s>%s</option>",
                 esc(kTzOptions[i].posix).c_str(), sel ? " selected" : "",
                 esc(kTzOptions[i].label).c_str());
        server.sendContent(buf);
    }
    // A zone restored from a backup written by a build with a different list
    // would otherwise vanish from the form and be silently replaced on the next
    // save by whatever happened to be first.
    if (!tzMatched && gCfg.tz[0]) {
        snprintf(buf, sizeof(buf), "<option value=\"%s\" selected>%s (stored)</option>",
                 esc(gCfg.tz).c_str(), esc(gCfg.tz).c_str());
        server.sendContent(buf);
    }
    server.sendContent(F("</select><p class=hint>Sets the clock on the device screen. "
                         "Daylight saving is handled by the rule built into each zone. "
                         "Times sent to the ingestor are always UTC.</p></fieldset>"));

    // Security
    server.sendContent(F("<fieldset><legend>Portal access</legend>"));
    snprintf(buf, sizeof(buf),
             "<div class=chk><input type=checkbox name=auth id=auth %s>"
             "<label for=auth style='margin:0'>Require a password to open this page</label></div>"
             "<label>Password</label>"
             "<input type=password name=webpass maxlength=63 autocomplete=new-password "
                         "placeholder='(unchanged)'>",
             gCfg.webAuthEnabled ? "checked" : "");
    server.sendContent(buf);
    server.sendContent(F("<p class=hint>Username is <code>admin</code>. This is HTTP Basic auth "
                         "over plain HTTP &mdash; it keeps casual visitors out of the settings, "
                         "it does not protect the password on an untrusted network.</p>"
                         "</fieldset>"));

    server.sendContent(F("<div class=actions><button type=submit>Save settings</button></div>"
                         "</form>"));
}

// ── Backup / restore ────────────────────────────────────────────────────────
void sendBackupSection() {
    server.sendContent(F(
        "<fieldset><legend>Backup &amp; restore</legend>"
        "<p class=hint style='margin:0 0 14px'>The backup is a JSON file holding every "
        "setting on this page &mdash; including the WiFi password, the ingestor token and "
        "the channel keys, because a backup that omits them cannot actually restore the "
        "device. Treat the file as a secret.</p>"
        "<div class=actions><a class=btn href=/backup>Download backup</a></div>"
        "<form method=post action=/restore enctype='multipart/form-data'>"
        "<label>Restore from a backup file</label>"
        "<input type=file name=backup accept='.yaml,.yml,.txt'>"
        "<div class=actions><button type=submit class=ghost>Restore</button></div>"
        "</form>"
        "<form method=post action=/reset "
        "onsubmit=\"return confirm('Erase all settings and return to defaults?')\">"
        "<div class=actions><button type=submit class=danger>Factory reset</button></div>"
        "</form></fieldset>"));
}

// ── YAML backup ─────────────────────────────────────────────────────────────
// Hand-rolled rather than pulled from a library: the document is a flat map of
// known keys plus one list, the firmware is the only writer, and a YAML parser
// costs more flash than the whole config page.
//
// Every string is quoted and escaped. Unquoted YAML would mangle several real
// values here — a timezone like "IST-5:30" reads as a sexagesimal number, and
// an SSID of "yes" or "no" reads as a boolean.
void yamlString(String &out, const char *key, const char *value) {
    out += key;
    out += ": \"";
    for (const char *p = value; p && *p; p++) {
        if (*p == '\\' || *p == '"') out += '\\';
        out += *p;
    }
    out += "\"\n";
}

void yamlBool(String &out, const char *key, bool value) {
    out += key;
    out += (value ? ": true\n" : ": false\n");
}

void yamlNumber(String &out, const char *key, uint32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
    out += key;
    out += ": ";
    out += buf;
    out += "\n";
}

void buildBackupYaml(String &out) {
    out.reserve(1400);
    out += F("# Camillia Monitor configuration backup\n"
             "# Restore this file from the device's config portal.\n"
             "# Contains WiFi and API credentials and channel keys - treat as a secret.\n");
    yamlString(out, "type", "camillia-monitor-backup");
    yamlNumber(out, "version", MONITOR_CFG_VERSION);

    out += F("\n# Identity\n");
    yamlString(out, "monitorName", gCfg.monitorName);

    out += F("\n# WiFi\n");
    yamlString(out, "wifiSsid", gCfg.wifiSsid);
    yamlString(out, "wifiPass", gCfg.wifiPass);

    out += F("\n# Ingestor\n");
    yamlBool(out,   "ingestEnabled", gCfg.ingestEnabled);
    yamlString(out, "ingestUrl", gCfg.ingestUrl);
    yamlString(out, "ingestToken", gCfg.ingestToken);
    yamlNumber(out, "ingestIntervalS", gCfg.ingestIntervalS);

    out += F("\n# Radio\n");
    yamlString(out, "region", gCfg.region);
    yamlBool(out,   "usePreset", gCfg.usePreset);
    yamlNumber(out, "modemPreset", gCfg.modemPreset);
    yamlNumber(out, "bwCode", gCfg.bwCode);
    yamlNumber(out, "loraSf", gCfg.loraSf);
    yamlNumber(out, "loraCr", gCfg.loraCr);
    yamlBool(out,   "pinSlot", gCfg.pinSlot);
    yamlNumber(out, "freqSlot", gCfg.freqSlot);

    out += F("\n# MQTT\n");
    yamlBool(out,   "mqttEnabled", gCfg.mqttEnabled);
    yamlString(out, "mqttServer", gCfg.mqttServer);
    yamlNumber(out, "mqttPort", gCfg.mqttPort);
    yamlString(out, "mqttUser", gCfg.mqttUser);
    yamlString(out, "mqttPass", gCfg.mqttPass);
    yamlString(out, "mqttRoot", gCfg.mqttRoot);
    yamlBool(out,   "mqttTls", gCfg.mqttTls);

    out += F("\n# Display\n");
    yamlBool(out,   "useMetric", gCfg.useMetric);
    yamlString(out, "timezone", gCfg.tz);

    out += F("\n# Portal access\n");
    yamlBool(out,   "webAuthEnabled", gCfg.webAuthEnabled);
    yamlString(out, "webPass", gCfg.webPass);

    out += F("\n# Channels, slot 0 first\n"
             "channels:\n");
    for (int i = 0; i < MESH_CHANNELS; i++) {
        char psk[48];
        configFormatPsk(gCfg.channels[i].key, gCfg.channels[i].keyLen, psk, sizeof(psk));
        String row;
        yamlString(row, "name", gCfg.channels[i].name);
        out += "  - ";
        out += row;
        String pskRow;
        yamlString(pskRow, "psk", psk);
        out += "    ";
        out += pskRow;
    }
}

// ── YAML restore ────────────────────────────────────────────────────────────
// Splits at the FIRST colon only. Several values legitimately contain one — a
// URL's "http://", a timezone's "IST-5:30" — and splitting anywhere else would
// corrupt exactly those.
bool yamlSplit(char *line, char **key, char **value) {
    char *colon = strchr(line, ':');
    if (!colon) return false;
    *colon = '\0';
    *key = line;
    *value = colon + 1;

    while (**key == ' ' || **key == '\t' || **key == '-') (*key)++;
    char *kEnd = *key + strlen(*key);
    while (kEnd > *key && (kEnd[-1] == ' ' || kEnd[-1] == '\t')) *--kEnd = '\0';

    while (**value == ' ' || **value == '\t') (*value)++;
    char *vEnd = *value + strlen(*value);
    while (vEnd > *value && (vEnd[-1] == ' ' || vEnd[-1] == '\t' ||
                             vEnd[-1] == '\r' || vEnd[-1] == '\n')) *--vEnd = '\0';

    if (**value == '"') {
        char *src = *value + 1;
        char *dst = *value;
        while (*src && *src != '"') {
            if (*src == '\\' && src[1]) src++;
            *dst++ = *src++;
        }
        *dst = '\0';
    }
    return **key != '\0';
}

bool yamlTruthy(const char *v) {
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0;
}

// ── Form field helpers ──────────────────────────────────────────────────────
void copyArg(const char *field, char *dst, size_t cap) {
    if (!server.hasArg(field)) return;
    strncpy(dst, server.arg(field).c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

// A blank password field means "keep what is stored", so an operator can edit
// any other setting without retyping every secret. The cost is that clearing a
// password needs its own gesture; factory reset is that gesture.
void copySecret(const char *field, char *dst, size_t cap) {
    if (!server.hasArg(field)) return;
    const String v = server.arg(field);
    if (!v.length()) return;
    strncpy(dst, v.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

uint32_t argU32(const char *field, uint32_t fallback) {
    if (!server.hasArg(field)) return fallback;
    const String v = server.arg(field);
    if (!v.length()) return fallback;
    return (uint32_t)strtoul(v.c_str(), nullptr, 10);
}

// ── Route handlers ──────────────────────────────────────────────────────────
void handleRoot() {
    noteRequest();
    if (!guard()) return;
    beginChunked();
    sendHead("Camillia Monitor");
    server.sendContent(F("<h1>Camillia Monitor</h1>"));
    server.sendContent(gApMode
        ? F("<p class=sub>Setup mode &mdash; join this device to your network to finish.</p>")
        : F("<p class=sub>Passive Meshtastic listener</p>"));
    sendNotice();
    sendStatus();
    sendForm();
    sendBackupSection();
    server.sendContent(F("</div></body></html>"));
    server.sendContent("");
}

// Redirecting after a network change sends the browser to an address that is
// about to stop existing — on the setup AP it is *this* address. Serve a page
// that says what is happening and where the device will be instead.
void sendRebootPage() {
    beginChunked();
    sendHead("Reconnecting");
    server.sendContent(F("<h1>Joining "));
    server.sendContent(esc(gCfg.wifiSsid));
    server.sendContent(F("</h1><p class=sub>The monitor is restarting to connect to your "
                         "network.</p><fieldset><legend>What happens next</legend>"
                         "<p class=hint style='margin:0'>This page will stop responding: the "
                         "setup network is shutting down. Once the monitor has joined, it "
                         "picks up an address from your router &mdash; find it there, or on "
                         "the device's own screen, and open that address to reach these "
                         "settings again.</p></fieldset>"
                         "</div></body></html>"));
    server.sendContent("");
}

void handleSave() {
    noteRequest();
    if (!guard()) return;

    const bool ssidWas = gCfg.wifiSsid[0];
    char prevSsid[sizeof(gCfg.wifiSsid)];
    strncpy(prevSsid, gCfg.wifiSsid, sizeof(prevSsid));

    copyArg("name", gCfg.monitorName, sizeof(gCfg.monitorName));
    copyArg("ssid", gCfg.wifiSsid, sizeof(gCfg.wifiSsid));
    copySecret("pass", gCfg.wifiPass, sizeof(gCfg.wifiPass));

    gCfg.ingestEnabled = server.hasArg("ingest");
    copyArg("url", gCfg.ingestUrl, sizeof(gCfg.ingestUrl));
    configNormalizeIngestUrl(gCfg);
    copySecret("token", gCfg.ingestToken, sizeof(gCfg.ingestToken));
    gCfg.ingestIntervalS = constrain(argU32("interval", gCfg.ingestIntervalS), 5UL, 3600UL);

    const bool mqttWas = gCfg.mqttEnabled;
    char prevBroker[sizeof(gCfg.mqttServer)];
    strncpy(prevBroker, gCfg.mqttServer, sizeof(prevBroker));

    gCfg.mqttEnabled = server.hasArg("mqtt");
    copyArg("mqtthost", gCfg.mqttServer, sizeof(gCfg.mqttServer));
    gCfg.mqttPort = (uint16_t)constrain(argU32("mqttport", gCfg.mqttPort), 1UL, 65535UL);
    copyArg("mqttuser", gCfg.mqttUser, sizeof(gCfg.mqttUser));
    copySecret("mqttpass", gCfg.mqttPass, sizeof(gCfg.mqttPass));
    copyArg("mqttroot", gCfg.mqttRoot, sizeof(gCfg.mqttRoot));
    gCfg.mqttTls = server.hasArg("mqtttls");
    const bool mqttChanged = (mqttWas != gCfg.mqttEnabled) ||
                             strncmp(prevBroker, gCfg.mqttServer, sizeof(prevBroker)) != 0;

    copyArg("region", gCfg.region, sizeof(gCfg.region));
    gCfg.usePreset   = server.hasArg("preset");
    gCfg.modemPreset = (uint8_t)constrain(argU32("presetidx", gCfg.modemPreset), 0UL,
                                          (uint32_t)PRESET_COUNT - 1);
    gCfg.bwCode      = loraCoerceBwCode((uint16_t)argU32("bw", gCfg.bwCode));
    gCfg.loraSf      = (uint8_t)constrain(argU32("sf", gCfg.loraSf), LORA_SF_MIN, LORA_SF_MAX);
    gCfg.loraCr      = (uint8_t)constrain(argU32("cr", gCfg.loraCr), LORA_CR_MIN, LORA_CR_MAX);
    gCfg.pinSlot     = server.hasArg("pinslot");
    gCfg.freqSlot    = argU32("slot", gCfg.freqSlot);

    int badPsk = -1;
    for (int i = 0; i < MESH_CHANNELS; i++) {
        char nameField[8], keyField[8];
        snprintf(nameField, sizeof(nameField), "cn%d", i);
        snprintf(keyField, sizeof(keyField), "ck%d", i);

        copyArg(nameField, gCfg.channels[i].name, sizeof(gCfg.channels[i].name));

        if (!server.hasArg(keyField)) continue;
        const String k = server.arg(keyField);
        if (!k.length()) {
            gCfg.channels[i].keyLen = 0;
            memset(gCfg.channels[i].key, 0, sizeof(gCfg.channels[i].key));
            continue;
        }
        uint8_t key[32], len = 0;
        if (configParsePsk(k.c_str(), key, len)) {
            memset(gCfg.channels[i].key, 0, sizeof(gCfg.channels[i].key));
            memcpy(gCfg.channels[i].key, key, len);
            gCfg.channels[i].keyLen = len;
        } else if (badPsk < 0) {
            // Reject the bad field, keep the rest of the save. Discarding an
            // entire form because one PSK has a typo in it loses work the
            // operator has no way to recover.
            badPsk = i;
        }
    }

    gCfg.useMetric = (argU32("units", gCfg.useMetric ? 1 : 0) != 0);
    copyArg("tz", gCfg.tz, sizeof(gCfg.tz));

    gCfg.webAuthEnabled = server.hasArg("auth");
    copySecret("webpass", gCfg.webPass, sizeof(gCfg.webPass));
    if (gCfg.webAuthEnabled && !gCfg.webPass[0]) {
        // Turning auth on with no password would lock nothing and, worse, look
        // like it had.
        gCfg.webAuthEnabled = false;
        notice("Portal password is empty, so password protection stays off.", true);
    }

    const bool saved = configSave();
    if (gApply) gApply();
    // Drops any existing session so the next loop dials the new broker.
    if (mqttChanged) mqttBegin();

    const bool ssidChanged = strncmp(prevSsid, gCfg.wifiSsid, sizeof(prevSsid)) != 0;

    if (!saved) {
        notice("Settings applied, but writing them to flash failed &mdash; "
               "they will not survive a reboot.", true);
    } else if (badPsk >= 0) {
        String m = "Saved, but the PSK for slot ";
        m += badPsk;
        m += " was not valid base64 and was left unchanged.";
        notice(m.c_str(), true);
    } else if (ssidChanged && ssidWas) {
        notice("Saved. Rebooting to join the new network.");
    } else {
        notice("Settings saved.");
    }

    // The radio retunes live, but a WiFi change cannot be applied from inside a
    // request handler still using the connection it would tear down.
    if (saved && ssidChanged) {
        sendRebootPage();
        delay(400);
        ESP.restart();
        return;
    }

    server.sendHeader("Location", "/");
    server.send(303);
}

void handleBackup() {
    noteRequest();
    if (!guard()) return;
    String yaml;
    buildBackupYaml(yaml);

    char stamp[80];
    snprintf(stamp, sizeof(stamp), "attachment; filename=\"%s-backup.yaml\"",
             gCfg.monitorName[0] ? gCfg.monitorName : "camillia-monitor");
    server.sendHeader("Content-Disposition", stamp);
    // text/yaml rather than application/x-yaml so a browser will show it if the
    // user opens it instead of saving.
    server.send(200, "text/yaml; charset=utf-8", yaml);
}

// Upload buffer. A backup is ~1.2 KB; 4 KB leaves room for growth and for the
// comments a user might add.
char   sRestoreBuf[4096];
size_t sRestoreLen = 0;
bool   sRestoreOk  = false;
bool   sRestoreOverflow = false;

// Receives the file body. Runs before the completion handler below, in chunks.
void handleRestoreUpload() {
    HTTPUpload &upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        sRestoreLen = 0;
        sRestoreOk = false;
        sRestoreOverflow = false;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        // Refuse an oversized file rather than keeping what fits. A truncated
        // YAML still parses — it would apply the first N settings and silently
        // drop the rest, which is worse than not restoring at all.
        if (sRestoreOverflow) return;   // keep draining the socket
        const size_t space = sizeof(sRestoreBuf) - sRestoreLen - 1;
        if (upload.currentSize > space) { sRestoreOverflow = true; return; }
        memcpy(sRestoreBuf + sRestoreLen, upload.buf, upload.currentSize);
        sRestoreLen += upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        if (sRestoreOverflow) return;
        sRestoreBuf[sRestoreLen] = '\0';
        sRestoreOk = true;
    }
}

void handleRestore() {
    noteRequest();
    if (!guard()) return;

    if (sRestoreOverflow) {
        notice("That file is too large to be a configuration backup.", true);
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }
    if (!sRestoreOk || sRestoreLen == 0) {
        notice("No file received &mdash; choose a backup file first.", true);
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }

    // Restore onto defaults rather than onto the running config, so a backup
    // written before a field existed produces that field's default instead of
    // whatever this device happens to have in it.
    MonitorConfig in;
    configDefaults(in);

    bool typeSeen = false;
    int  badPsk = -1;
    int  chanIdx = -1;

    for (char *line = strtok(sRestoreBuf, "\n"); line; line = strtok(nullptr, "\n")) {
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (*trimmed == '#' || *trimmed == '\0' || *trimmed == '\r') continue;

        // A list entry starts with "- ", and every entry here begins with name.
        const bool listItem = (trimmed[0] == '-' && trimmed[1] == ' ');

        char *key, *value;
        if (!yamlSplit(line, &key, &value)) continue;

        if (!strcmp(key, "type")) {
            typeSeen = (strcmp(value, "camillia-monitor-backup") == 0);
        } else if (!strcmp(key, "monitorName"))  strncpy(in.monitorName, value, sizeof(in.monitorName) - 1);
        else if (!strcmp(key, "wifiSsid"))       strncpy(in.wifiSsid, value, sizeof(in.wifiSsid) - 1);
        else if (!strcmp(key, "wifiPass"))       strncpy(in.wifiPass, value, sizeof(in.wifiPass) - 1);
        else if (!strcmp(key, "ingestEnabled"))  in.ingestEnabled = yamlTruthy(value);
        else if (!strcmp(key, "ingestUrl"))      strncpy(in.ingestUrl, value, sizeof(in.ingestUrl) - 1);
        else if (!strcmp(key, "ingestToken"))    strncpy(in.ingestToken, value, sizeof(in.ingestToken) - 1);
        else if (!strcmp(key, "ingestIntervalS")) in.ingestIntervalS = constrain((uint32_t)strtoul(value, nullptr, 10), 5UL, 3600UL);
        else if (!strcmp(key, "region"))         strncpy(in.region, value, sizeof(in.region) - 1);
        else if (!strcmp(key, "usePreset"))      in.usePreset = yamlTruthy(value);
        else if (!strcmp(key, "modemPreset"))    in.modemPreset = (uint8_t)constrain((int)strtol(value, nullptr, 10), 0, PRESET_COUNT - 1);
        else if (!strcmp(key, "bwCode"))         in.bwCode = loraCoerceBwCode((uint16_t)strtoul(value, nullptr, 10));
        else if (!strcmp(key, "loraSf"))         in.loraSf = (uint8_t)constrain((int)strtol(value, nullptr, 10), LORA_SF_MIN, LORA_SF_MAX);
        else if (!strcmp(key, "loraCr"))         in.loraCr = (uint8_t)constrain((int)strtol(value, nullptr, 10), LORA_CR_MIN, LORA_CR_MAX);
        else if (!strcmp(key, "pinSlot"))        in.pinSlot = yamlTruthy(value);
        else if (!strcmp(key, "freqSlot"))       in.freqSlot = (uint32_t)strtoul(value, nullptr, 10);
        else if (!strcmp(key, "mqttEnabled"))    in.mqttEnabled = yamlTruthy(value);
        else if (!strcmp(key, "mqttServer"))     strncpy(in.mqttServer, value, sizeof(in.mqttServer) - 1);
        else if (!strcmp(key, "mqttPort"))       in.mqttPort = (uint16_t)constrain((uint32_t)strtoul(value, nullptr, 10), 1UL, 65535UL);
        else if (!strcmp(key, "mqttUser"))       strncpy(in.mqttUser, value, sizeof(in.mqttUser) - 1);
        else if (!strcmp(key, "mqttPass"))       strncpy(in.mqttPass, value, sizeof(in.mqttPass) - 1);
        else if (!strcmp(key, "mqttRoot"))       strncpy(in.mqttRoot, value, sizeof(in.mqttRoot) - 1);
        else if (!strcmp(key, "mqttTls"))        in.mqttTls = yamlTruthy(value);
        else if (!strcmp(key, "useMetric"))      in.useMetric = yamlTruthy(value);
        else if (!strcmp(key, "timezone"))       strncpy(in.tz, value, sizeof(in.tz) - 1);
        else if (!strcmp(key, "webAuthEnabled")) in.webAuthEnabled = yamlTruthy(value);
        else if (!strcmp(key, "webPass"))        strncpy(in.webPass, value, sizeof(in.webPass) - 1);
        else if (!strcmp(key, "channels"))       { chanIdx = -1; memset(in.channels, 0, sizeof(in.channels)); }
        else if (!strcmp(key, "name") && listItem) {
            if (chanIdx + 1 < MESH_CHANNELS) {
                chanIdx++;
                strncpy(in.channels[chanIdx].name, value, sizeof(in.channels[chanIdx].name) - 1);
            }
        } else if (!strcmp(key, "psk") && chanIdx >= 0) {
            if (value[0]) {
                uint8_t k[32], len = 0;
                if (configParsePsk(value, k, len)) {
                    memcpy(in.channels[chanIdx].key, k, len);
                    in.channels[chanIdx].keyLen = len;
                } else if (badPsk < 0) {
                    badPsk = chanIdx;
                }
            }
        }
    }

    if (!typeSeen) {
        notice("That file is not a Camillia Monitor backup.", true);
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }

    configNormalizeIngestUrl(in);
    if (in.webAuthEnabled && !in.webPass[0]) in.webAuthEnabled = false;

    const bool ssidChanged = strncmp(gCfg.wifiSsid, in.wifiSsid, sizeof(in.wifiSsid)) != 0;
    gCfg = in;
    const bool saved = configSave();
    if (gApply) gApply();

    if (!saved) {
        notice("Restored, but writing to flash failed &mdash; "
               "the settings will not survive a reboot.", true);
    } else if (badPsk >= 0) {
        String m = "Restored, but the PSK for slot ";
        m += badPsk;
        m += " in that backup was not valid base64 and the slot is now disabled.";
        notice(m.c_str(), true);
    } else {
        notice("Settings restored.");
    }

    if (saved && ssidChanged) {
        sendRebootPage();
        delay(400);
        ESP.restart();
        return;
    }

    server.sendHeader("Location", "/");
    server.send(303);
}

void handleReset() {
    noteRequest();
    if (!guard()) return;
    configFactoryReset();
    if (gApply) gApply();
    server.sendHeader("Location", "/");
    server.send(303);
    delay(400);
    ESP.restart();
}

void handleNotFound() {
    // In AP mode every unknown host is the captive-portal probe; bounce it to
    // the setup page so the phone's "sign in to network" sheet lands somewhere
    // useful instead of on a 404.
    if (gApMode) {
        server.sendHeader("Location", String("http://") + gIp + "/");
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "text/plain", "Not found");
}

// Idempotent. WebServer::on() appends rather than replaces, so calling this
// again for each rejoin attempt would pile up handler objects for the life of
// the process — a leak that only shows on a device left running for weeks,
// which is exactly this one.
bool gRoutesRegistered = false;

void registerRoutes() {
    if (gRoutesRegistered) return;
    gRoutesRegistered = true;

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/backup", HTTP_GET, handleBackup);
    // Three-argument form: the upload handler streams the body, then the
    // completion handler runs once the whole file has arrived.
    server.on("/restore", HTTP_POST, handleRestore, handleRestoreUpload);
    server.on("/reset", HTTP_POST, handleReset);
    server.onNotFound(handleNotFound);
}

// One or more association attempts against the stored credentials. Returns
// true once the station has an address.
bool joinStation() {
    for (uint8_t attempt = 0; attempt < kJoinAttempts; attempt++) {
        // Clear a previous failed association, whose leftover state makes the
        // next begin() fail immediately. Deliberately *not* erasing the stored
        // AP record (second argument false): that record carries the last known
        // channel and BSSID, and dropping it forces a full scan on every
        // attempt — slower, which is the opposite of what a retry needs.
        WiFi.disconnect(false, false);
        delay(100);

        // Before begin(), not after: modem power-save during association adds
        // latency to exactly the handshake being waited on.
        WiFi.setSleep(false);

        const wl_status_t begun = WiFi.begin(gCfg.wifiSsid, gCfg.wifiPass);
        if (begun == WL_CONNECT_FAILED || begun == WL_NO_SHIELD) {
            Serial.printf("[web] WiFi.begin failed (%d)\n", (int)begun);
            continue;
        }
        Serial.printf("[web] joining \"%s\" (attempt %u/%u) ...\n",
                      gCfg.wifiSsid, (unsigned)(attempt + 1), (unsigned)kJoinAttempts);

        const uint32_t start = millis();
        while (millis() - start < kConnectTimeoutMs) {
            if (WiFi.status() == WL_CONNECTED) return true;
            if (WiFi.status() == WL_NO_SHIELD) {
                Serial.println("[web] WiFi stack unavailable");
                return false;
            }
            delay(100);
        }
        Serial.printf("[web] attempt %u timed out after %us — last status: %s\n",
                      (unsigned)(attempt + 1), (unsigned)(kConnectTimeoutMs / 1000),
                      wifiStatusName(WiFi.status()));
    }

    logNetworkScan();
    return false;
}

bool startAp() {
    gApMode = true;
    WiFi.disconnect(false);
    if (!WiFi.mode(WIFI_AP)) {
        Serial.println("[web] WIFI_AP mode failed");
        return false;
    }
    delay(120);
    if (!WiFi.softAP(kApSsid)) {
        Serial.println("[web] SoftAP start failed");
        return false;
    }
    delay(250);
    // Modem power-save off: with it on, the synchronous WebServer stalls on
    // inbound packets and page loads time out. camillia-mt found the same on
    // both the AP and STA paths.
    WiFi.setSleep(false);

    const IPAddress apIp = WiFi.softAPIP();
    apIp.toString().toCharArray(gIp, sizeof(gIp));

    // Captive-portal DNS: resolve every name to us so the joining device pops
    // the setup page by itself.
    gDns.setErrorReplyCode(DNSReplyCode::NoError);
    gCaptive = gDns.start(53, "*", apIp);
    if (!gCaptive) Serial.println("[web] captive DNS start failed (continuing without it)");

    registerRoutes();
    if (!gRunning) server.begin();
    gRunning = true;
    Serial.printf("[web] setup AP \"%s\" at http://%s/\n", kApSsid, gIp);
    return true;
}

}  // namespace

bool webCfgBegin(WebCfgApplyCb onApply) {
    gApply = onApply;

    if (!gCfg.wifiSsid[0]) {
        Serial.println("[web] no WiFi configured — starting setup AP");
        return startAp();
    }

    gApMode = false;
    if (!WiFi.mode(WIFI_STA)) {
        Serial.println("[web] STA mode failed — falling back to setup AP");
        return startAp();
    }

    if (!joinStation()) {
        Serial.println("[web] could not join — falling back to setup AP");
        return startAp();
    }

    WiFi.localIP().toString().toCharArray(gIp, sizeof(gIp));
    WiFi.setSleep(false);   // see the note in startAp()
    WiFi.setAutoReconnect(true);

    registerRoutes();
    server.begin();
    gRunning = true;
    Serial.printf("[web] config portal at http://%s/\n", gIp);
    return true;
}

void webCfgLoop() {
    if (!gRunning) return;
    if (gCaptive) gDns.processNextRequest();
    server.handleClient();

    // ── Recover from the fallback AP ────────────────────────────────────────
    // Without this, a router that was slow to come up leaves the monitor on its
    // own SoftAP indefinitely — off the network, unable to reach the ingestor,
    // and looking to the operator exactly like it had forgotten its WiFi
    // settings. An unattended device has to keep trying.
    if (!gApMode || !gCfg.wifiSsid[0]) return;
    if (millis() < gNextRejoinMs) return;
    // Never while someone is using the portal: the retry drops the AP, which
    // would disconnect them mid-configuration.
    if (gLastRequestMs && (millis() - gLastRequestMs) < kPortalIdleMs) return;

    gNextRejoinMs = millis() + kRejoinIntervalMs;
    Serial.println("[web] retrying the stored network");

    gDns.stop();
    gCaptive = false;
    if (!WiFi.mode(WIFI_STA)) { startAp(); return; }

    if (joinStation()) {
        // Restart rather than re-plumb a running server onto a new interface.
        // The boot path already brings up STA, SNTP, MQTT and the uplink in the
        // right order; reproducing that here would be a second, less-tested
        // copy of it.
        Serial.println("[web] joined — restarting to come up on the network");
        delay(200);
        ESP.restart();
    }

    Serial.println("[web] still unreachable — staying on the setup AP");
    startAp();
}

bool        webCfgIsAp()    { return gApMode; }
