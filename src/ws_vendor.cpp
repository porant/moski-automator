#include "ws_vendor.hpp"
#include "filter.hpp"
#include "obs-websocket-api.h"
#include <obs-frontend-api.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
using namespace opa;
static obs_websocket_vendor vendor = nullptr;
static std::array<const char *, 11> actions = {"Add",      "Set",      "Reset",    "Stop",
                                               "Start",    "StartAll", "StopAll",  "ResetAll",
                                               "Get",      "List",     "FaceTrack"};
static bool has(obs_data_t *d, const char *k) {
    return obs_data_has_user_value(d, k);
}
static double number(obs_data_t *d, const char *k) {
    auto *i = obs_data_item_byname(d, k);
    if (!i || obs_data_item_gettype(i) != OBS_DATA_NUMBER) {
        if (i)
            obs_data_item_release(&i);
        throw std::invalid_argument(std::string("Missing numeric ") + k);
    }
    const double v = obs_data_item_get_double(i);
    obs_data_item_release(&i);
    if (!std::isfinite(v))
        throw std::invalid_argument(std::string("Non-finite ") + k);
    return v;
}
static void callback(obs_data_t *r, obs_data_t *out, void *priv) {
    obs_source_t *filter = nullptr;
    try {
        const std::string action = static_cast<const char *>(priv);
        if (action == "List") {
            auto *array = obs_data_array_create();
            for (const auto &entry : enumerateFilters()) {
                auto *item = obs_data_create();
                obs_data_set_string(item, "source", entry.source.c_str());
                obs_data_set_string(item, "sourceUuid", entry.sourceUuid.c_str());
                obs_data_set_string(item, "filter", entry.filter.c_str());
                obs_data_set_string(item, "filterUuid", entry.filterUuid.c_str());
                obs_data_array_push_back(array, item);
                obs_data_release(item);
            }
            obs_data_set_array(out, "filters", array);
            obs_data_array_release(array);
            obs_data_set_bool(out, "ok", true);
            return;
        }
        filter = resolveFilter(r);
        auto e = filter ? engineFor(filter) : nullptr;
        if (!e)
            throw std::invalid_argument("Native OPA filter not found; use List to obtain names/UUIDs");
        int index = -1;
        if (has(r, "parameter"))
            index = parameterIndex(obs_data_get_string(r, "parameter"));
        else if (has(r, "parameterId")) {
            const double id = number(r, "parameterId");
            if (id >= 0 && id < paramCount && id == std::floor(id))
                index = (int)id;
        }
        if (action == "FaceTrack") {
            // Enables/disables tracking and optionally tunes it; every field is optional, so a
            // call with no requestData just reports the current faces in the response below.
            obs_data_t *s = obs_source_get_settings(filter);
            if (has(r, "enabled"))
                obs_data_set_bool(s, "face_tracking", obs_data_get_bool(r, "enabled"));
            if (has(r, "fps"))
                obs_data_set_int(s, "face_fps", (int)number(r, "fps"));
            if (has(r, "maxFaces"))
                obs_data_set_int(s, "face_max", (int)number(r, "maxFaces"));
            if (has(r, "score"))
                obs_data_set_double(s, "face_score", number(r, "score"));
            if (has(r, "smoothMs"))
                obs_data_set_double(s, "face_smooth_ms", number(r, "smoothMs"));
            if (has(r, "blur"))
                obs_data_set_bool(s, "effect_blur", obs_data_get_bool(r, "blur"));
            if (has(r, "debug"))
                obs_data_set_bool(s, "effect_debug", obs_data_get_bool(r, "debug"));
            if (has(r, "blurPx"))
                obs_data_set_double(s, "face_blur_px", number(r, "blurPx"));
            if (has(r, "faceScale"))
                obs_data_set_bool(s, "face_scale", obs_data_get_bool(r, "faceScale"));
            obs_source_update(filter, s);
            obs_data_release(s);
        } else if (action == "StartAll")
            e->startAll();
        else if (action == "StopAll")
            e->stopAll();
        else if (action == "ResetAll")
            e->resetAll();
        else if (action != "Get") {
            if (index < 0)
                throw std::invalid_argument("Missing/invalid parameter or parameterId (0..29)");
            double value = 0, duration = -1;
            int easing = -1, ret = -1;
            if (action == "Add" || action == "Set")
                value = number(r, "value");
            if (has(r, "durationMs")) {
                duration = number(r, "durationMs") / 1000;
                if (duration < 0 || duration > 3600)
                    throw std::invalid_argument("durationMs must be 0..3600000");
            }
            if (has(r, "easing"))
                easing = Easing::parse(obs_data_get_string(r, "easing"));
            if (has(r, "returnToZero")) {
                auto *item = obs_data_item_byname(r, "returnToZero");
                const bool valid = obs_data_item_gettype(item) == OBS_DATA_BOOLEAN;
                obs_data_item_release(&item);
                if (!valid)
                    throw std::invalid_argument("returnToZero must be boolean");
                ret = obs_data_get_bool(r, "returnToZero") ? 1 : 0;
                // Historical field name retained; actual return endpoint is configurable.
            }
            e->command(index, action, value, duration, easing, ret);
        } else if ((has(r, "parameter") || has(r, "parameterId")) && index < 0)
            throw std::invalid_argument("Unknown parameter");
        auto s = e->snapshot();
        auto *array = obs_data_array_create();
        for (int i = 0; i < paramCount; ++i) {
            if (index >= 0 && index != i)
                continue;
            const auto &p = s.controller.at(i);
            auto *item = obs_data_create();
            obs_data_set_string(item, "parameter", parameterName(i).c_str());
            obs_data_set_int(item, "parameterId", i);
            obs_data_set_double(item, "current", p.state.currentValue);
            obs_data_set_double(item, "gpuValue", p.gpuValue());
            obs_data_set_double(item, "target", p.state.targetValue);
            obs_data_set_double(item, "progress", p.state.progress);
            obs_data_set_string(item, "state", phaseName(p.state.phase));
            obs_data_set_bool(item, "active", p.state.active);
            obs_data_set_string(item, "easing", easingNames[p.state.easingType]);
            obs_data_array_push_back(array, item);
            obs_data_release(item);
        }
        obs_data_set_array(out, "parameters", array);
        obs_data_array_release(array);
        obs_data_set_int(out, "mode", (int)s.mode);
        obs_data_set_int(out, "activeAnimations", s.controller.activeCount());
        obs_data_set_double(out, "cpuUpdateUs", s.updateUs);
        obs_data_set_double(out, "cpuMaxUpdateUs", s.maxUpdateUs);
        obs_data_set_int(out, "parameterUpdates", (long long)s.parameterUpdates);
        obs_data_set_int(out, "uniformCalls", (long long)s.uniformCalls);
        // Face tracking: last detections (percent units) plus status.
        auto *faces = obs_data_array_create();
        for (const auto &face : s.faces) {
            auto *item = obs_data_create();
            obs_data_set_double(item, "centerX", face.cx);
            obs_data_set_double(item, "centerY", face.cy);
            obs_data_set_double(item, "width", face.w);
            obs_data_set_double(item, "height", face.h);
            obs_data_set_double(item, "score", face.score);
            // Anchor points (percent): eyes, nose tip, mouth.
            auto *anchors = obs_data_array_create();
            for (int a = 0; a < pointCount; ++a) {
                auto *ap = obs_data_create();
                obs_data_set_string(ap, "name", pointName(a));
                obs_data_set_double(ap, "x", face.anchor[a].x);
                obs_data_set_double(ap, "y", face.anchor[a].y);
                obs_data_array_push_back(anchors, ap);
                obs_data_release(ap);
            }
            obs_data_set_array(item, "anchors", anchors);
            obs_data_array_release(anchors);
            obs_data_array_push_back(faces, item);
            obs_data_release(item);
        }
        obs_data_set_array(out, "faces", faces);
        obs_data_array_release(faces);
        obs_data_set_bool(out, "faceTracking", s.faceTracking);
        obs_data_set_bool(out, "faceAvailable", s.faceAvailable);
        obs_data_set_int(out, "faceSequence", (long long)s.faceSequence);
        obs_data_set_double(out, "faceDetectMs", s.faceDetectMs);
        obs_data_set_bool(out, "effectBlur", s.effectBlur);
        obs_data_set_bool(out, "effectDebug", s.effectDebug);
        obs_data_set_bool(out, "ok", true);
    } catch (const std::exception &ex) {
        obs_data_set_bool(out, "ok", false);
        obs_data_set_string(out, "error", ex.what());
    }
    if (filter)
        obs_source_release(filter);
}
// obs-websocket is unloaded before this module while OBS shuts down, so its proc handler is already
// gone once obs_module_unload() runs: calling the vendor API there crashes OBS with an access
// violation inside proc_handler_call(). Release the requests on OBS_FRONTEND_EVENT_EXIT instead -
// that fires while the frontend and every module are still alive.
static void frontendExit(enum obs_frontend_event event, void *) {
    if (event == OBS_FRONTEND_EVENT_EXIT)
        unregister_websocket_vendor();
}
bool register_websocket_vendor() {
    vendor = obs_websocket_register_vendor("obs-parameter-animator");
    if (!vendor)
        return false;
    for (const char *a : actions)
        if (!obs_websocket_vendor_register_request(vendor, a, callback, (void *)a)) {
            unregister_websocket_vendor();
            return false;
        }
    obs_frontend_add_event_callback(frontendExit, nullptr);
    return true;
}
void unregister_websocket_vendor() {
    if (!vendor)
        return;
    for (const char *a : actions)
        obs_websocket_vendor_unregister_request(vendor, a);
    vendor = nullptr;
}
void detach_websocket_vendor() {
    vendor = nullptr;
}
