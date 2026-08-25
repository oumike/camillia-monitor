#pragma once
// ════════════════════════════════════════════════════════════════════════════
// net/web_config.h — WiFi bring-up and the settings portal
//
// Deliberately a fraction of camillia-mt's web_config: no node list, no chat,
// no remote/VNC tab, no screenshots. A monitor's portal does two jobs —
// edit settings, and back them up or restore them.
//
// The other structural difference is lifecycle. camillia-mt runs its portal as
// a temporary session with an idle timeout, because it is a battery handheld
// whose WiFi competes with the job it is actually doing. This device is a
// mains-powered appliance that needs the network up permanently to reach the
// ingestor, so the server simply stays listening for as long as it is running.
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>

// Called on the main thread after settings are saved, so the caller can re-tune
// the radio and re-apply channel keys without the HTTP handler owning that.
typedef void (*WebCfgApplyCb)();

// Joins the configured network, or raises the onboarding SoftAP when no
// credentials are stored (and as a fallback when the join fails). Returns true
// once the HTTP server is listening either way.
bool webCfgBegin(WebCfgApplyCb onApply);

// Must be called from loop().
void webCfgLoop();

bool        webCfgIsAp();        // true when serving the onboarding/fallback SoftAP
