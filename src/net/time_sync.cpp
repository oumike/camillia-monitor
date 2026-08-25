// ════════════════════════════════════════════════════════════════════════════
// net/time_sync.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "time_sync.h"

#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

namespace {

// Which source last set the clock. Internal: callers ask timeSourceName() for
// something to display and timeIsValid() for whether to trust a timestamp.
enum class TimeSource : uint8_t { None = 0, Ntp, Gps };

// Anything before this is the ESP32's unset clock (which starts at 1970) or a
// wildly wrong reading. The ingestor applies the same floor to reported times,
// so agreeing on it here means the device never sends something the server will
// only throw away.
constexpr uint32_t kEarliestPlausibleEpoch = 1577836800UL;  // 2020-01-01 UTC

// UTC everywhere, deliberately. A monitor's timestamps are compared against
// other gateways' and stored server-side; a local offset would be one more
// thing to get wrong for no benefit, and the config portal has no timezone
// field precisely because nothing here needs one.
constexpr long kGmtOffsetSec     = 0;
constexpr int  kDaylightOffsetSec = 0;

constexpr char kNtpPrimary[]   = "pool.ntp.org";
constexpr char kNtpSecondary[] = "time.nist.gov";
constexpr char kNtpTertiary[]  = "time.google.com";

TimeSource sSource = TimeSource::None;

// millis() at the moment the clock was first set, with the epoch it was set to.
// Together these convert a past millis() stamp into a real time.
uint32_t sSyncedAtMs    = 0;
uint32_t sSyncedEpoch   = 0;

bool     sSntpStarted   = false;
bool     sWasConnected  = false;
uint32_t sLastPollMs    = 0;

constexpr uint32_t kPollIntervalMs = 1000;

uint32_t rawEpoch() {
    const time_t now = time(nullptr);
    return (now < (time_t)kEarliestPlausibleEpoch) ? 0 : (uint32_t)now;
}

void startSntp() {
    configTime(kGmtOffsetSec, kDaylightOffsetSec, kNtpPrimary, kNtpSecondary, kNtpTertiary);
    sSntpStarted = true;
}

void markSynced(TimeSource source, uint32_t epoch) {
    sSource      = source;
    sSyncedEpoch = epoch;
    sSyncedAtMs  = millis();

    char stamp[32];
    timeFormatUtc(stamp, sizeof(stamp));
    Serial.printf("[time] clock set from %s: %s\n", timeSourceName(), stamp);
}

}  // namespace

void timeSyncBegin() {
    // Nothing blocking here. The IDF's SNTP client runs in the background and
    // retries by itself, so this only has to be armed once the interface is up.
    if (WiFi.status() == WL_CONNECTED) {
        startSntp();
        sWasConnected = true;
    }
}

void timeSyncLoop() {
    if (millis() - sLastPollMs < kPollIntervalMs) return;
    sLastPollMs = millis();

    const bool connected = (WiFi.status() == WL_CONNECTED);

    // Arm SNTP on the first association, and again after a reconnect: the client
    // is bound to an interface that went away, and left alone it would sit there
    // never retrying while the clock quietly drifted.
    if (connected && (!sSntpStarted || !sWasConnected)) startSntp();
    sWasConnected = connected;

    if (sSource == TimeSource::Ntp) return;   // already authoritative

    const uint32_t epoch = rawEpoch();
    if (epoch == 0) return;                   // SNTP has not landed yet

    // Reaching here with a GPS-set clock means NTP has now answered; NTP takes
    // over, because it does not drift between fixes the way a clock last
    // touched by a GPS reading does.
    markSynced(TimeSource::Ntp, epoch);
}

void timeSyncNoteGpsFix(uint32_t epochSeconds) {
    if (epochSeconds < kEarliestPlausibleEpoch) return;
    if (sSource == TimeSource::Ntp) return;   // NTP outranks GPS

    // Set the system clock too, so anything reading time() directly agrees with
    // this module rather than seeing 1970.
    const timeval tv = { .tv_sec = (time_t)epochSeconds, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    // Only announce the first fix; the GPS re-reports time continuously and
    // logging every one would bury everything else.
    const bool first = (sSource != TimeSource::Gps);
    sSource      = TimeSource::Gps;
    sSyncedEpoch = epochSeconds;
    sSyncedAtMs  = millis();
    if (first) {
        char stamp[32];
        timeFormatUtc(stamp, sizeof(stamp));
        Serial.printf("[time] clock set from GPS: %s\n", stamp);
    }
}

bool timeIsValid() { return sSource != TimeSource::None; }

// Internal: every caller is in this file. timeEpochAtMillis() is the one the
// rest of the firmware needs, because a timestamp is only ever wanted for a
// moment that has already passed.
static uint32_t timeEpoch() {
    if (!timeIsValid()) return 0;
    const uint32_t epoch = rawEpoch();
    if (epoch != 0) return epoch;
    // The system clock should always be set by now; fall back to counting from
    // the last sync rather than returning nothing.
    return sSyncedEpoch + (millis() - sSyncedAtMs) / 1000U;
}

uint32_t timeEpochAtMillis(uint32_t whenMs) {
    if (!timeIsValid()) return 0;

    const uint32_t now = millis();
    if (whenMs > now) return 0;               // clock wrapped or a bad stamp

    // A moment from before the clock was set cannot be dated. Returning
    // something anyway would put a confidently wrong timestamp on the packet;
    // returning 0 lets the caller omit the field and the ingestor apply its own
    // arrival time, which is the honest answer.
    if (whenMs < sSyncedAtMs) return 0;

    return timeEpoch() - ((now - whenMs) / 1000U);
}


const char *timeSourceName() {
    switch (sSource) {
        case TimeSource::Ntp: return "NTP";
        case TimeSource::Gps: return "GPS";
        default:              return "unset";
    }
}

void timeFormatUtc(char *out, size_t cap) {
    if (!out || cap == 0) return;
    const uint32_t epoch = timeEpoch();
    if (epoch == 0) { strncpy(out, "not set", cap - 1); out[cap - 1] = '\0'; return; }

    const time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%SZ", &tm);
}

void timeFormatClock(char *out, size_t cap) {
    if (!out || cap == 0) return;
    const uint32_t epoch = timeEpoch();
    if (epoch == 0) { strncpy(out, "--:--", cap - 1); out[cap - 1] = '\0'; return; }

    // localtime_r, not gmtime_r: this is the wall clock a person reads off the
    // screen. The TZ environment variable is set from config at boot, so the
    // zone's own DST rule applies without anything here knowing about it.
    // Everything reported upstream still uses timeEpoch(), which stays UTC.
    const time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%H:%M", &tm);
}

