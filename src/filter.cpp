#include "filter.hpp"
#include <cmath>
#include <cstring>
#include <graphics/vec2.h>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <util/platform.h>

namespace opa {
static std::mutex registryMutex;
static std::unordered_map<obs_source_t *, std::weak_ptr<Engine>> registry;
std::shared_ptr<Engine> engineFor(obs_source_t *s) {
    std::lock_guard lock(registryMutex);
    auto i = registry.find(s);
    return i == registry.end() ? nullptr : i->second.lock();
}
void Engine::logLocked(const std::string &s) {
    logs.push_back(std::to_string(nowSeconds() - epoch) + "s  " + s);
    if (logs.size() > 300)
        logs.pop_front();
}
void Engine::sampleLocked(double t) {
    const auto begin = Clock::now();
    for (int i = 0; i < paramCount; ++i) {
        auto &p = controller.at(i);
        if (!p.state.active)
            continue;
        const double before = p.gpuValue();
        p.sample(t);
        ++evaluations;
        if (p.gpuValue() != before)
            ++parameterUpdates;
    }
    for (auto &m : morphs) {
        auto &p = m.intensity;
        if (!p.state.active)
            continue;
        const double before = p.gpuValue();
        p.sample(t);
        ++evaluations;
        if (p.gpuValue() != before)
            ++parameterUpdates;
    }
    updateUs = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
    maxUpdateUs = std::max(maxUpdateUs, updateUs);
}
void Engine::configure(obs_data_t *s) {
    std::lock_guard lock(mutex);
    const auto nextMode = static_cast<Mode>(std::clamp<int>((int)obs_data_get_int(s, "mode"), 0, 2));
    if (initialized && nextMode != mode) {
        for (int i = 0; i < paramCount; ++i)
            controller.at(i).stop(nowSeconds());
        logLocked("Mode changed; trajectories stopped at current values");
    }
    mode = nextMode;
    effectBlur = obs_data_get_bool(s, "effect_blur");
    effectDebug = obs_data_get_bool(s, "effect_debug");
    faceBlurPx = std::clamp(obs_data_get_double(s, "face_blur_px"), 2.0, 128.0);
    faceScale = obs_data_get_bool(s, "face_scale");
    fps = (int)obs_data_get_int(s, "animation_fps");
    if (fps != 0 && fps != 30 && fps != 60 && fps != 120)
        fps = 0;
    defaultDuration = std::clamp(obs_data_get_double(s, "default_duration_ms") / 1000, 0.0, 3600.0);
    defaultEasing = std::clamp<int>((int)obs_data_get_int(s, "default_easing"), 0, 30);
    for (int i = 0; i < paramCount; ++i) {
        const auto key = parameterName(i);
        auto &p = controller.at(i);
        double base = p.boolean ? obs_data_get_bool(s, key.c_str()) : obs_data_get_double(s, key.c_str());
        if (!std::isfinite(base))
            base = p.baseValue;
        base = p.clamp(base);
        if (!initialized || base != p.baseValue) {
            p.state.active = false;
            p.state.phase = Phase::Idle;
            p.state.startValue = p.state.currentValue = p.state.targetValue = base;
            p.baseValue = base;
        }
        auto number = [&](const char *suffix, double fallback, double lo, double hi) {
            const double v = obs_data_get_double(s, (key + suffix).c_str());
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
        };
        p.configuredTarget = number("_target", base, p.minimum, p.maximum);
        // -1 means use shared default.
        const double ms = number("_duration_ms", -1, -1, 3600000);
        p.configuredDuration = ms < 0 ? defaultDuration : ms / 1000;
        int e = (int)obs_data_get_int(s, (key + "_easing").c_str());
        p.configuredEasing = e < 0 ? defaultEasing : std::clamp(e, 0, 30);
        p.returnDuration = number("_return_ms", 1000, 0, 3600000) / 1000;
        p.holdDuration = number("_hold_ms", 0, 0, 3600000) / 1000;
        p.returnValue = number("_return_value", base, p.minimum, p.maximum);
        p.returnEasing = std::clamp<int>((int)obs_data_get_int(s, (key + "_return_easing").c_str()), 0, 30);
        // Configuration changes affect subsequent launches, not active trajectories.
        p.configuredAutoReturn = obs_data_get_bool(s, (key + "_auto_return").c_str());
    }
    // --- meme morphs: same settings shape as a point parameter, keyed morph_<name>_* ---
    for (int i = 0; i < morphCount; ++i) {
        auto &p = morphs[i].intensity;
        const std::string k = std::string("morph_") + morphKey(i);
        morphs[i].enabled = obs_data_get_bool(s, (k + "_enabled").c_str());
        auto number = [&](const char *suffix, double fallback, double lo, double hi) {
            const double v = obs_data_get_double(s, (k + suffix).c_str());
            return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
        };
        double base = number("_value", 0, p.minimum, p.maximum);
        if (!initialized || base != p.baseValue) {
            p.state.active = false;
            p.state.phase = Phase::Idle;
            p.state.startValue = p.state.currentValue = p.state.targetValue = base;
            p.baseValue = base;
        }
        p.configuredTarget = number("_target", base, p.minimum, p.maximum);
        const double ms = number("_duration_ms", -1, -1, 3600000);
        p.configuredDuration = ms < 0 ? defaultDuration : ms / 1000;
        int e = (int)obs_data_get_int(s, (k + "_easing").c_str());
        p.configuredEasing = e < 0 ? defaultEasing : std::clamp(e, 0, 30);
        p.returnDuration = number("_return_ms", 1000, 0, 3600000) / 1000;
        p.holdDuration = number("_hold_ms", 200, 0, 3600000) / 1000;
        p.returnValue = number("_return_value", 0, p.minimum, p.maximum);
        p.returnEasing =
            std::clamp<int>((int)obs_data_get_int(s, (k + "_return_easing").c_str()), 0, 30);
        p.configuredAutoReturn = obs_data_get_bool(s, (k + "_auto_return").c_str());
    }
    // --- face tracking ---
    faceTracking = obs_data_get_bool(s, "face_tracking");
    faceFps = std::clamp<int>((int)obs_data_get_int(s, "face_fps"), 1, 60);
    faceMax = std::clamp<int>((int)obs_data_get_int(s, "face_max"), 1, maxFaces);
    faceScore = std::clamp(obs_data_get_double(s, "face_score"), 0.1, 0.95);
    faceDetectHeight = std::clamp<int>((int)obs_data_get_int(s, "face_height"), 96, 480);
    faceSmoothMs = std::clamp(obs_data_get_double(s, "face_smooth_ms"), 0.0, 2000.0);
    if (tracker) {
        FaceTrackerConfig c;
        c.maxFaces = faceMax;
        c.scoreThreshold = faceScore;
        c.detectHeight = faceDetectHeight;
        tracker->configure(c);
    }
    initialized = true;
}
void Engine::command(int index, const std::string &action, double value, double duration, int easing,
                     int returnOverride) {
    if (index < 0 || index >= paramCount)
        throw std::invalid_argument("Unknown parameter");
    std::lock_guard lock(mutex);
    if (mode != Mode::Plugin)
        throw std::invalid_argument("Select Plugin animation mode first");
    auto &p = controller.at(index);
    const double t = nowSeconds();
    const double d = duration < 0 ? p.configuredDuration : duration;
    const int e = easing < 0 ? p.configuredEasing : easing;
    const bool ret = returnOverride < 0 ? p.configuredAutoReturn : returnOverride != 0;
    if (action == "Stop")
        p.stop(t);
    else if (action == "Reset")
        // Back to the saved rest value of the parameter (magnitude: 0 unless it was changed).
        p.launch(p.returnValue, d, e, t, false);
    else if (action == "Start")
        p.launch(p.configuredTarget, d, e, t, ret);
    else if (action == "Add")
        p.add(value, d, e, t, ret);
    else if (action == "Set")
        p.launch(value, d, e, t, ret);
    else
        throw std::invalid_argument("Unknown action");
    lastSample = -1;
    logLocked(action + " " + parameterName(index) + " target=" + std::to_string(p.state.targetValue));
}
void Engine::commandMorph(int index, const std::string &action, double value, double duration,
                          int easing, int returnOverride) {
    if (index < 0 || index >= morphCount)
        throw std::invalid_argument("Unknown effect");
    std::lock_guard lock(mutex);
    if (mode != Mode::Plugin)
        throw std::invalid_argument("Select Plugin animation mode first");
    auto &p = morphs[index].intensity;
    const double t = nowSeconds();
    const double d = duration < 0 ? p.configuredDuration : duration;
    const int e = easing < 0 ? p.configuredEasing : easing;
    const bool ret = returnOverride < 0 ? p.configuredAutoReturn : returnOverride != 0;
    if (action == "Stop")
        p.stop(t);
    else if (action == "Reset")
        p.launch(p.returnValue, d, e, t, false);
    else if (action == "Start")
        p.launch(p.configuredTarget, d, e, t, ret);
    else if (action == "Add")
        p.add(value, d, e, t, ret);
    else if (action == "Set")
        p.launch(value, d, e, t, ret);
    else
        throw std::invalid_argument("Unknown action");
    lastSample = -1;
    logLocked(action + " effect " + morphKey(index) + " target=" + std::to_string(p.state.targetValue));
}
void Engine::startAll() {
    std::lock_guard lock(mutex);
    if (mode != Mode::Plugin)
        throw std::invalid_argument("Select Plugin animation mode first");
    const double t = nowSeconds();
    for (int i = 0; i < paramCount; ++i) {
        auto &p = controller.at(i);
        p.launch(p.configuredTarget, p.configuredDuration, p.configuredEasing, t, p.configuredAutoReturn);
    }
    for (auto &m : morphs) {
        auto &p = m.intensity;
        p.launch(p.configuredTarget, p.configuredDuration, p.configuredEasing, t, p.configuredAutoReturn);
    }
    lastSample = -1;
    logLocked("StartAll: shared start timestamp");
}
void Engine::stopAll() {
    std::lock_guard lock(mutex);
    const double t = nowSeconds();
    for (int i = 0; i < paramCount; ++i)
        controller.at(i).stop(t);
    for (auto &m : morphs)
        m.intensity.stop(t);
    lastSample = -1;
    logLocked("StopAll");
}
void Engine::resetAll() {
    std::lock_guard lock(mutex);
    if (mode != Mode::Plugin)
        throw std::invalid_argument("Select Plugin animation mode first");
    const double t = nowSeconds();
    for (int i = 0; i < paramCount; ++i) {
        auto &p = controller.at(i);
        p.launch(p.baseValue, p.configuredDuration, p.configuredEasing, t, false);
    }
    for (auto &m : morphs) {
        auto &p = m.intensity;
        p.launch(p.baseValue, p.configuredDuration, p.configuredEasing, t, false);
    }
    lastSample = -1;
    logLocked("ResetAll: animate to saved base values");
}
Snapshot Engine::snapshot() const {
    std::lock_guard lock(mutex);
    Snapshot s{controller,
               mode,
               fps,
               defaultDuration,
               defaultEasing,
               updateUs,
               maxUpdateUs,
               evaluations,
               parameterUpdates,
               uniformCalls,
               {logs.begin(), logs.end()}};
    s.morphs = morphs;
    s.faceTracking = faceTracking;
    s.effectBlur = effectBlur;
    s.effectDebug = effectDebug;
    if (tracker) {
        auto r = tracker->result();
        s.faces = std::move(r.faces);
        s.faceSequence = r.sequence;
        s.faceDetectMs = r.detectMs;
        s.faceDropped = r.dropped;
        s.faceAvailable = tracker->available();
    }
    return s;
}
void Engine::smoothFaces(const std::vector<FaceRect> &faces, double now) {
    // Time-based exponential easing toward the latest detection. Called every render frame (so the
    // points move smoothly at the render rate) while detections only arrive at the detection FPS.
    const double dt = (lastFaceSmoothTime < 0) ? 0.0 : std::clamp(now - lastFaceSmoothTime, 0.0, 0.5);
    lastFaceSmoothTime = now;
    const double tau = std::max(0.0, faceSmoothMs) / 1000.0;
    const double alpha = (tau <= 0.0 || dt <= 0.0) ? 1.0 : (1.0 - std::exp(-dt / tau));

    std::array<bool, maxFaces> used{};
    // Match each detection to the nearest existing track (within a limit); unmatched = new face.
    std::array<int, maxFaces> assign{};
    assign.fill(-1);
    for (size_t d = 0; d < faces.size() && d < maxFaces; ++d) {
        int best = -1;
        double bestDist = 25.0; // percent; farther than this is treated as a different face
        for (int t = 0; t < maxFaces; ++t) {
            if (used[t] || !tracks[t].valid)
                continue;
            const double dist = std::hypot(tracks[t].cx - faces[d].cx, tracks[t].cy - faces[d].cy);
            if (dist < bestDist) {
                bestDist = dist;
                best = t;
            }
        }
        if (best >= 0) {
            assign[d] = best;
            used[best] = true;
        }
    }
    for (size_t d = 0; d < faces.size() && d < maxFaces; ++d) {
        const FaceRect &f = faces[d];
        int t = assign[d];
        if (t < 0) {
            for (int s = 0; s < maxFaces; ++s) {
                if (!tracks[s].valid && !used[s]) {
                    t = s;
                    break;
                }
            }
            if (t < 0)
                continue; // no free slot
            tracks[t].cx = f.cx;
            tracks[t].cy = f.cy;
            tracks[t].w = f.w;
            tracks[t].h = f.h;
            for (int p = 0; p < pointCount; ++p)
                tracks[t].anchors[p] = f.anchor[p]; // snap on first appearance
            for (int l = 0; l < landmarkCount; ++l)
                tracks[t].landmarks[l] = f.landmark[l]; // snap on first appearance
            tracks[t].hasLandmarks = f.landmarkValid;
            tracks[t].roll = f.roll;
            tracks[t].yaw = f.yaw;
            tracks[t].pitch = f.pitch;
            tracks[t].valid = true;
        } else {
            auto &tr = tracks[t];
            tr.cx += (f.cx - tr.cx) * alpha;
            tr.cy += (f.cy - tr.cy) * alpha;
            tr.w += (f.w - tr.w) * alpha;
            tr.h += (f.h - tr.h) * alpha;
            for (int p = 0; p < pointCount; ++p) {
                tr.anchors[p].x += (f.anchor[p].x - tr.anchors[p].x) * alpha;
                tr.anchors[p].y += (f.anchor[p].y - tr.anchors[p].y) * alpha;
            }
            tr.roll += (f.roll - tr.roll) * alpha;
            tr.yaw += (f.yaw - tr.yaw) * alpha;
            tr.pitch += (f.pitch - tr.pitch) * alpha;
            if (f.landmarkValid) {
                if (!tr.hasLandmarks) {
                    // Landmarks just became available: snap instead of easing from a stale spot.
                    for (int l = 0; l < landmarkCount; ++l)
                        tr.landmarks[l] = f.landmark[l];
                } else {
                    for (int l = 0; l < landmarkCount; ++l) {
                        tr.landmarks[l].x += (f.landmark[l].x - tr.landmarks[l].x) * alpha;
                        tr.landmarks[l].y += (f.landmark[l].y - tr.landmarks[l].y) * alpha;
                    }
                }
                tr.hasLandmarks = true;
            } else {
                tr.hasLandmarks = false;
            }
        }
        used[t] = true;
    }
    for (int t = 0; t < maxFaces; ++t)
        if (tracks[t].valid && !used[t])
            tracks[t].valid = false; // face not seen this tick
}
static void enumFilter(obs_source_t *parent, obs_source_t *child, void *data) {
    if (std::string(obs_source_get_id(child)) != filterId)
        return;
    static_cast<std::vector<FilterEntry> *>(data)->push_back(
        {obs_source_get_name(parent), obs_source_get_uuid(parent), obs_source_get_name(child),
         obs_source_get_uuid(child)});
}
static bool enumSource(void *data, obs_source_t *source) {
    obs_source_enum_filters(source, enumFilter, data);
    return true;
}
std::vector<FilterEntry> enumerateFilters() {
    std::vector<FilterEntry> r;
    obs_enum_sources(enumSource, &r);
    obs_enum_scenes(enumSource, &r);
    return r;
}
obs_source_t *resolveFilter(obs_data_t *r) {
    const char *fu = obs_data_get_string(r, "filterUuid");
    if (*fu) {
        auto *f = obs_get_source_by_uuid(fu);
        if (f && engineFor(f))
            return f;
        if (f)
            obs_source_release(f);
        // Filters may not be globally registered: walk each source's filter chain.
        struct Search {
            std::string uuid;
            obs_source_t *found = nullptr;
        } search{fu};
        auto searchSource = [](void *ctx, obs_source_t *s) {
            obs_source_enum_filters(
                s,
                [](obs_source_t *, obs_source_t *f, void *ctx) {
                    auto &c = *static_cast<Search *>(ctx);
                    if (!c.found && c.uuid == obs_source_get_uuid(f))
                        c.found = obs_source_get_ref(f);
                },
                ctx);
            return !static_cast<Search *>(ctx)->found;
        };
        obs_enum_sources(searchSource, &search);
        if (!search.found)
            obs_enum_scenes(searchSource, &search);
        return search.found;
    }
    const char *su = obs_data_get_string(r, "sourceUuid");
    auto *source =
        *su ? obs_get_source_by_uuid(su) : obs_get_source_by_name(obs_data_get_string(r, "source"));
    if (!source)
        return nullptr;
    auto *f = obs_source_get_filter_by_name(source, obs_data_get_string(r, "filter"));
    obs_source_release(source);
    return f;
}
struct ShaderController {
    obs_source_t *source = nullptr;
    gs_effect_t *effect = nullptr;
    gs_eparam_t *zoneCount = nullptr, *zoneData = nullptr;
    gs_eparam_t *markerCount = nullptr, *markerData = nullptr;
    gs_eparam_t *faceCount = nullptr, *faceBox = nullptr, *faceRoll = nullptr;
    gs_eparam_t *blurFaces = nullptr, *blurPx = nullptr, *debugPoints = nullptr;
    gs_eparam_t *animate = nullptr, *size = nullptr, *time = nullptr;
    std::shared_ptr<Engine> engine = std::make_shared<Engine>();
    bool inCapture = false; // guards against recursive rendering while grabbing a detection frame
};
static const char *name(void *) {
    return "Face Points Distortion + Parameter Animator";
}
static void defaults(obs_data_t *s) {
    obs_data_set_default_int(s, "mode", 2);
    obs_data_set_default_bool(s, "effect_blur", false);
    obs_data_set_default_bool(s, "effect_debug", false);
    obs_data_set_default_bool(s, "face_scale", true);
    obs_data_set_default_double(s, "face_blur_px", 24);
    obs_data_set_default_int(s, "animation_fps", 0);
    obs_data_set_default_double(s, "default_duration_ms", 1000);
    obs_data_set_default_int(s, "default_easing", 5);
    AnimationController c;
    for (int i = 0; i < paramCount; ++i) {
        auto &p = c.at(i);
        const auto k = parameterName(i);
        if (p.boolean)
            obs_data_set_default_bool(s, k.c_str(), true);
        else
            obs_data_set_default_double(s, k.c_str(), p.baseValue);
        obs_data_set_default_double(s, (k + "_target").c_str(), p.baseValue);
        obs_data_set_default_double(s, (k + "_duration_ms").c_str(), -1);
        obs_data_set_default_int(s, (k + "_easing").c_str(), -1);
        obs_data_set_default_bool(s, (k + "_auto_return").c_str(), i % pointParamCount == 4);
        obs_data_set_default_double(s, (k + "_return_value").c_str(), p.baseValue);
        obs_data_set_default_double(s, (k + "_return_ms").c_str(), 1000);
        // A short hold by default: the pulse stays visible briefly after the last Add, then returns.
        obs_data_set_default_double(s, (k + "_hold_ms").c_str(), 200);
        obs_data_set_default_int(s, (k + "_return_easing").c_str(), 5);
    }
    // Meme morphs: same defaults shape as a point magnitude (strength 0..[-1..1], auto return).
    for (int i = 0; i < morphCount; ++i) {
        const std::string k = std::string("morph_") + morphKey(i);
        obs_data_set_default_bool(s, (k + "_enabled").c_str(), false);
        obs_data_set_default_double(s, (k + "_value").c_str(), 0);
        obs_data_set_default_double(s, (k + "_target").c_str(), 0);
        obs_data_set_default_double(s, (k + "_duration_ms").c_str(), -1);
        obs_data_set_default_int(s, (k + "_easing").c_str(), -1);
        obs_data_set_default_bool(s, (k + "_auto_return").c_str(), true);
        obs_data_set_default_double(s, (k + "_return_value").c_str(), 0);
        obs_data_set_default_double(s, (k + "_hold_ms").c_str(), 200);
        obs_data_set_default_double(s, (k + "_return_ms").c_str(), 800);
        obs_data_set_default_int(s, (k + "_return_easing").c_str(), 5);
    }
    // Face tracking is opt-in. Each detected face gets one zone per enabled point.
    obs_data_set_default_bool(s, "face_tracking", false);
    obs_data_set_default_int(s, "face_fps", 10);
    obs_data_set_default_int(s, "face_max", 4);
    obs_data_set_default_double(s, "face_score", 0.7);
    obs_data_set_default_int(s, "face_height", 180);
    obs_data_set_default_double(s, "face_smooth_ms", 120);
}
static void update(void *v, obs_data_t *s) {
    static_cast<ShaderController *>(v)->engine->configure(s);
}
static void destroy(void *v) {
    auto *f = static_cast<ShaderController *>(v);
    {
        std::lock_guard lock(registryMutex);
        registry.erase(f->source);
    }
    obs_enter_graphics();
    if (f->effect)
        gs_effect_destroy(f->effect);
    obs_leave_graphics();
    delete f;
}
static void *create(obs_data_t *s, obs_source_t *source) {
    auto *f = new ShaderController;
    f->source = source;
    char *path = obs_module_file("face-points.effect"), *error = nullptr;
    obs_enter_graphics();
    f->effect = path ? gs_effect_create_from_file(path, &error) : nullptr;
    obs_leave_graphics();
    bfree(path);
    if (!f->effect) {
        blog(LOG_ERROR, "[OPA] Shader compilation failed: %s", error ? error : "File missing");
        bfree(error);
        delete f;
        return nullptr;
    }
    bfree(error);
    f->zoneCount = gs_effect_get_param_by_name(f->effect, "zone_count");
    f->zoneData = gs_effect_get_param_by_name(f->effect, "zone_data");
    f->animate = gs_effect_get_param_by_name(f->effect, "animate");
    f->size = gs_effect_get_param_by_name(f->effect, "uv_size");
    f->time = gs_effect_get_param_by_name(f->effect, "elapsed_time");
    f->markerCount = gs_effect_get_param_by_name(f->effect, "marker_count");
    f->markerData = gs_effect_get_param_by_name(f->effect, "marker_data");
    f->faceCount = gs_effect_get_param_by_name(f->effect, "face_count");
    f->faceBox = gs_effect_get_param_by_name(f->effect, "face_box");
    f->faceRoll = gs_effect_get_param_by_name(f->effect, "face_roll");
    f->blurFaces = gs_effect_get_param_by_name(f->effect, "blur_faces");
    f->blurPx = gs_effect_get_param_by_name(f->effect, "blur_px");
    f->debugPoints = gs_effect_get_param_by_name(f->effect, "debug_points");
    if (!f->zoneCount || !f->zoneData || !f->animate || !f->size || !f->time || !f->markerCount ||
        !f->markerData || !f->faceCount || !f->faceBox || !f->faceRoll || !f->blurFaces ||
        !f->blurPx || !f->debugPoints) {
        blog(LOG_ERROR, "[OPA] Required shader uniforms missing");
        destroy(f);
        return nullptr;
    }
    // Optional face tracking: point the worker at the bundled YuNet model. When the plugin is
    // built without OpenCV the tracker stays inert and this is harmless.
    char *model = obs_module_file("face_detection_yunet_2023mar.onnx");
    if (model) {
        f->engine->tracker->setModelPath(model);
        bfree(model);
    }
    f->engine->configure(s);
    {
        std::lock_guard lock(registryMutex);
        registry[source] = f->engine;
    }
    return f;
}
// Grabs a small, downscaled RGBA frame of the filter's own input and hands it to the detector.
// This is the *only* extra GPU cost of face tracking and it only runs while (a) tracking is on and
// (b) a point magnitude or a meme morph is non-zero, at the configured detection FPS - not per
// rendered frame. We render the target ourselves with this filter disabled (guarded by inCapture)
// so the filter never renders itself, then read the texrender back through a stage surface at the
// detection resolution.
static void captureForDetection(ShaderController &f, int detectHeight) {
    obs_source_t *target = obs_filter_get_target(f.source);
    if (!target)
        return;
    const uint32_t w = obs_source_get_base_width(target), h = obs_source_get_base_height(target);
    if (!w || !h)
        return;
    int dh = std::clamp(detectHeight, 64, 720);
    int dw = static_cast<int>(std::lround(double(dh) * double(w) / double(h)));
    dw = std::clamp(dw, 2, 1280);
    if (dw & 1)
        ++dw; // even width keeps the downscale symmetric
    const bool wasEnabled = obs_source_enabled(f.source);
    if (wasEnabled)
        obs_source_set_enabled(f.source, false);
    f.inCapture = true;
    gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (tr && gs_texrender_begin(tr, dw, dh)) {
        vec4 clear;
        vec4_set(&clear, 0.0f, 0.0f, 0.0f, 1.0f);
        gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
        gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
        obs_source_video_render(target);
        gs_texrender_end(tr);
        if (gs_texture_t *tex = gs_texrender_get_texture(tr)) {
            gs_stagesurf_t *stage = gs_stagesurface_create(dw, dh, GS_RGBA);
            if (stage) {
                gs_stage_texture(stage, tex);
                uint8_t *data = nullptr;
                uint32_t linesize = 0;
                if (gs_stagesurface_map(stage, &data, &linesize)) {
                    f.engine->tracker->submit(data, dw, dh, (int)linesize);
                    gs_stagesurface_unmap(stage);
                }
                gs_stagesurface_destroy(stage);
            }
        }
    }
    if (tr)
        gs_texrender_destroy(tr);
    f.inCapture = false;
    if (wasEnabled)
        obs_source_set_enabled(f.source, true);
}
static void render(void *v, gs_effect_t *) {
    auto &f = *static_cast<ShaderController *>(v);
    if (f.inCapture) {
        // OBS asked us to render again while we were grabbing a detection frame; pass through so
        // the capture never recurses into this filter.
        obs_source_skip_video_filter(f.source);
        return;
    }
    auto *target = obs_filter_get_target(f.source);
    if (!target) {
        obs_source_skip_video_filter(f.source);
        return;
    }
    const auto w = obs_source_get_base_width(target), h = obs_source_get_base_height(target);
    if (!w || !h)
        return;
    // Read the animated point parameters, the meme morphs and the independent effect flags.
    std::array<double, pointCount> pEnable{}, pOffX{}, pOffY{}, pRadius{}, pMag{};
    std::array<float, morphCount> morphVal{};
    bool sine = false, active = false, effectBlur = false, effectDebug = false, faceScale = true;
    bool morphActive = false;
    double elapsed = 0, faceBlurPx = 24;
    {
        std::lock_guard lock(f.engine->mutex);
        auto &e = *f.engine;
        effectBlur = e.effectBlur;
        effectDebug = e.effectDebug;
        faceBlurPx = e.faceBlurPx;
        faceScale = e.faceScale;
        for (int i = 0; i < morphCount; ++i) {
            // A disabled morph is forced to 0 so the shader treats it as "off".
            const double mv = e.morphs[i].enabled ? e.morphs[i].intensity.gpuValue() : 0.0;
            morphVal[i] = (float)mv;
            morphActive = morphActive || mv != 0.0;
        }
        // Blur, debug and morphs are independent of the point morph, so they keep the filter active
        // even with every magnitude at rest. Otherwise the zero-work fast path applies.
        active = effectBlur || effectDebug || morphActive || !e.controller.magnitudesAtRest();
        if (active) {
            const double t = nowSeconds();
            if (e.mode == Mode::Plugin && (e.lastSample < 0 || e.fps == 0 || t >= e.lastSample)) {
                e.sampleLocked(t);
                e.lastSample =
                    e.fps == 0 ? t : e.epoch + (std::floor((t - e.epoch) * e.fps) + 1) / e.fps;
            }
            sine = e.mode == Mode::Shader;
            elapsed = t - e.epoch;
            for (int p = 0; p < pointCount; ++p) {
                const auto &q = e.controller.zones[p].parameters;
                pEnable[p] = q[0].gpuValue();
                pOffX[p] = q[1].gpuValue();
                pOffY[p] = q[2].gpuValue();
                pRadius[p] = q[3].gpuValue();
                pMag[p] = q[4].gpuValue();
            }
        }
    }
    if (!active) {
        obs_source_skip_video_filter(f.source);
        return;
    }
    // Face tracking: capture at the configured detection FPS, then ease the anchor points toward
    // the latest detections on every render frame so the low-FPS detection does not make the
    // points jitter. Positions live in Engine::tracks.
    {
        auto &e = *f.engine;
        const double now = nowSeconds();
        bool run = false, doCapture = false;
        {
            std::lock_guard lock(e.mutex);
            run = e.faceTracking && e.tracker && e.tracker->available();
            if (run && now >= e.nextFaceCapture) {
                e.nextFaceCapture = now + 1.0 / std::max(1, e.faceFps);
                doCapture = true;
            }
        }
        if (run) {
            if (doCapture)
                captureForDetection(f, e.faceDetectHeight);
            const auto faces = e.tracker->result().faces;
            std::lock_guard lock(e.mutex);
            e.smoothFaces(faces, now);
        } else {
            std::lock_guard lock(e.mutex);
            e.tracks = {};
            e.lastFaceSmoothTime = -1;
        }
    }
    // Throttled face-tracking status so a plain OBS log shows whether the detector is receiving
    // frames (available=1, seq increasing) and finding faces (faces>0) - or not.
    {
        static double lastFaceLog = 0;
        const double nowLog = nowSeconds();
        if (nowLog - lastFaceLog >= 5.0) {
            lastFaceLog = nowLog;
            auto &e = *f.engine;
            bool avail = false;
            {
                std::lock_guard lock(e.mutex);
                avail = e.faceTracking && e.tracker && e.tracker->available();
            }
            FaceTracker::Result r;
            if (e.tracker)
                r = e.tracker->result();
            // Raw (unsmoothed) landmark of the first detection plus the smoothed track landmark,
            // so the log shows whether the detector updates them and whether the track follows.
            double rroll = 0, ryaw = 0, rpitch = 0;
            double troll = 0, tyaw = 0, tpitch = 0; // radians
            bool lv = false, rpose = false, tvalid = false;
            if (!r.faces.empty()) {
                lv = r.faces[0].landmarkValid;
                rpose = r.faces[0].poseValid;
                rroll = r.faces[0].roll;
                ryaw = r.faces[0].yaw;
                rpitch = r.faces[0].pitch;
            }
            {
                std::lock_guard lock(e.mutex);
                tvalid = e.tracks[0].valid;
                troll = e.tracks[0].roll;
                tyaw = e.tracks[0].yaw;
                tpitch = e.tracks[0].pitch;
            }
            constexpr double kDeg = 57.29577951308232;
            const std::string terr = e.tracker ? e.tracker->lastError() : std::string();
            blog(LOG_INFO,
                 "[OPA] face: tracking=%d available=%d faces=%d seq=%llu detect=%.1fms | RAW lmValid=%d "
                 "poseValid=%d roll=%+.1f yaw=%+.1f pitch=%+.1f | TRACK valid=%d roll=%+.1f yaw=%+.1f "
                 "pitch=%+.1f (deg) err=\"%s\"",
                 (int)e.faceTracking, (int)avail, (int)r.faces.size(), (unsigned long long)r.sequence,
                 r.detectMs, (int)lv, (int)rpose, rroll * kDeg, ryaw * kDeg, rpitch * kDeg,
                 (int)tvalid, troll * kDeg, tyaw * kDeg, tpitch * kDeg, terr.c_str());
        }
    }
    // From the smoothed tracks build: distortion zones, debug markers and blur face boxes.
    std::array<float, maxZones * 4> zones{};
    std::array<float, maxMarkers * 4> markers{}; // 3 anchors + 5 landmarks per face
    std::array<float, maxFaces * 4> boxes{};
    std::array<float, maxFaces * 4> rolls{}; // per-face data; .x = head roll (radians)
    int zoneCount = 0, markerCount = 0, faceCount = 0;
    {
        std::lock_guard lock(f.engine->mutex);
        auto &e = *f.engine;
        for (int t = 0; t < maxFaces; ++t) {
            if (!e.tracks[t].valid)
                continue;
            const auto &tr = e.tracks[t];
            // Scale to the face: radius and offsets are then relative to the face height instead of
            // the frame, so a distant face gets proportionally smaller points.
            const double scale = faceScale ? std::clamp(tr.h / 100.0, 0.05, 3.0) : 1.0;
            // Face boxes feed the blur option and the debug head-roll indicator.
            if ((effectBlur || effectDebug) && faceCount < maxFaces) {
                float *b = boxes.data() + faceCount * 4;
                b[0] = (float)tr.cx;
                b[1] = (float)tr.cy;
                b[2] = (float)tr.w;
                b[3] = (float)tr.h;
                rolls[faceCount * 4] = (float)tr.roll;
                ++faceCount;
            }
            for (int p = 0; p < pointCount; ++p) {
                if (pEnable[p] < 0.5)
                    continue; // point disabled
                const double ax = tr.anchors[p].x + pOffX[p] * scale;
                const double ay = tr.anchors[p].y + pOffY[p] * scale;
                if (effectDebug && markerCount < maxMarkers) {
                    float *m = markers.data() + markerCount * 4;
                    m[0] = (float)ax;
                    m[1] = (float)ay;
                    m[2] = (float)(1.5 * std::clamp(scale, 0.6, 2.0)); // marker size, percent
                    m[3] = (float)p;                                   // point index -> colour
                    ++markerCount;
                }
                if (effectBlur == false && effectDebug == false && pMag[p] == 0.0)
                    continue; // pure morph with nothing to draw
                if (pMag[p] != 0.0 && pRadius[p] > 0.0 && zoneCount < maxZones) {
                    float *z = zones.data() + zoneCount * 4;
                    z[0] = (float)ax;
                    z[1] = (float)ay;
                    z[2] = (float)(pRadius[p] * scale);
                    z[3] = (float)pMag[p];
                    ++zoneCount;
                }
            }
            // Raw landmarks (eyes / nose tip / mouth corners) drawn with marker indices 3..7 so the
            // debug overlay shows the newer points too, not just the three derived anchors.
            if (effectDebug && tr.hasLandmarks) {
                for (int l = 0; l < landmarkCount && markerCount < maxMarkers; ++l) {
                    float *m = markers.data() + markerCount * 4;
                    m[0] = (float)tr.landmarks[l].x;
                    m[1] = (float)tr.landmarks[l].y;
                    m[2] = (float)(1.1 * std::clamp(scale, 0.6, 2.0)); // a bit smaller than anchors
                    m[3] = (float)(pointCount + l);                    // 3..7 -> distinct colour
                    ++markerCount;
                }
            }
        }
    }
    if (zoneCount == 0 && markerCount == 0 && faceCount == 0) {
        // Nothing to draw (no faces / all points disabled): keep sampling, skip the GPU pass.
        obs_source_skip_video_filter(f.source);
        return;
    }
    if (!obs_source_process_filter_begin(f.source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
        return;
    // libobs gs_technique_end clears effect parameter values; set them on EVERY draw.
    gs_effect_set_val(f.zoneData, zones.data(), sizeof(float) * 4 * maxZones);
    gs_effect_set_int(f.zoneCount, zoneCount);
    gs_effect_set_val(f.markerData, markers.data(), sizeof(float) * 4 * maxMarkers);
    gs_effect_set_int(f.markerCount, markerCount);
    gs_effect_set_val(f.faceBox, boxes.data(), sizeof(float) * 4 * maxFaces);
    gs_effect_set_val(f.faceRoll, rolls.data(), sizeof(float) * 4 * maxFaces);
    gs_effect_set_int(f.faceCount, faceCount);
    gs_effect_set_bool(f.blurFaces, effectBlur);
    gs_effect_set_float(f.blurPx, (float)faceBlurPx);
    gs_effect_set_bool(f.debugPoints, effectDebug);
    gs_effect_set_bool(f.animate, sine);
    vec2 size;
    vec2_set(&size, (float)w, (float)h);
    gs_effect_set_vec2(f.size, &size);
    gs_effect_set_float(f.time, sine ? (float)elapsed : 0.0f);
    {
        std::lock_guard lock(f.engine->mutex);
        f.engine->uniformCalls += 13;
    }
    obs_source_process_filter_end(f.source, f.effect, w, h);
}
static obs_property_t *easingProperty(obs_properties_t *p, const std::string &k, const char *label,
                                      bool inherited = false) {
    auto *l = obs_properties_add_list(p, k.c_str(), label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    if (inherited)
        obs_property_list_add_int(l, "Use default easing", -1);
    for (int i = 0; i < 31; ++i)
        obs_property_list_add_int(l, easingNames[i], i);
    return l;
}
static bool button(obs_properties_t *, obs_property_t *property, void *v) {
    if (!v)
        return false;
    auto &f = *static_cast<ShaderController *>(v);
    auto *s = obs_source_get_settings(f.source);
    f.engine->configure(s);
    obs_data_release(s);
    const std::string key = obs_property_name(property);
    try {
        if (key == "start_all")
            f.engine->startAll();
        else if (key == "stop_all")
            f.engine->stopAll();
        else if (key == "reset_all")
            f.engine->resetAll();
        else if (key.rfind("fx", 0) == 0) {
            // Meme-morph button, key form "fx<index>:<action>" e.g. "fx2:Add".
            const auto separator = key.find(':');
            const int index = std::stoi(key.substr(2, separator - 2));
            f.engine->commandMorph(index, key.substr(separator + 1));
        } else {
            const auto separator = key.find(':');
            const std::string action = key.substr(0, separator);
            const int index = parameterIndex(key.substr(separator + 1));
            f.engine->command(index, action);
        }
    } catch (const std::exception &ex) {
        blog(LOG_WARNING, "[OPA] %s", ex.what());
    }
    return false;
}
static obs_properties_t *properties(void *v) {
    auto *props = obs_properties_create();
    auto *m = obs_properties_add_list(props, "mode", "Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(m, "Static", 0);
    obs_property_list_add_int(m, "Shader sine animation", 1);
    obs_property_list_add_int(m, "Plugin animation", 2);
    obs_properties_add_bool(props, "effect_blur", "Blur faces (cover them)");
    obs_properties_add_float_slider(props, "face_blur_px", "Blur radius (px)", 2, 128, 2);
    obs_properties_add_bool(props, "effect_debug", "Debug: draw face points");
    obs_properties_add_bool(props, "face_scale", "Scale points to the face size");
    auto *fps = obs_properties_add_list(props, "animation_fps", "Animation FPS", OBS_COMBO_TYPE_LIST,
                                        OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(fps, "Match OBS rendering (recommended)", 0);
    for (int n : {30, 60, 120})
        obs_property_list_add_int(fps, std::to_string(n).c_str(), n);
    obs_properties_add_float(props, "default_duration_ms", "Default duration (ms)", 0, 3600000, 10);
    easingProperty(props, "default_easing", "Default easing");
    obs_properties_add_button2(props, "start_all", "Start all", button, v);
    obs_properties_add_button2(props, "stop_all", "Stop all", button, v);
    obs_properties_add_button2(props, "reset_all", "Reset all to saved base values", button, v);
    {
        auto *ft = obs_properties_create();
        obs_properties_add_bool(ft, "face_tracking", "Track faces (YuNet via OpenCV)");
        obs_properties_add_int_slider(ft, "face_fps", "Detection FPS", 1, 60, 1);
        obs_properties_add_int_slider(ft, "face_max", "Max faces", 1, maxFaces, 1);
        obs_properties_add_float_slider(ft, "face_score", "Score threshold", 0.1, 0.95, 0.05);
        obs_properties_add_int_slider(ft, "face_height", "Detection frame height (px)", 96, 480, 12);
        obs_properties_add_float_slider(ft, "face_smooth_ms", "Point smoothing (ms)", 0, 1000, 10);
        obs_properties_add_group(props, "face_tracking_group", "Face tracking", OBS_GROUP_NORMAL, ft);
    }
    AnimationController c;
    constexpr const char *labels[] = {"Enable (threshold 0.5)", "Offset X (%)", "Offset Y (%)",
                                      "Radius (%)", "Magnitude"};
    for (int z = 0; z < pointCount; ++z) {
        auto *group = obs_properties_create();
        for (int k = 0; k < pointParamCount; ++k) {
            const int index = z * pointParamCount + k;
            const auto key = parameterName(index);
            auto &p = c.at(index);
            auto *row = obs_properties_create();
            if (k == 0)
                obs_properties_add_bool(row, key.c_str(), "Enabled");
            else
                obs_properties_add_float_slider(row, key.c_str(), "Value", p.minimum, p.maximum,
                                                k == 4 ? .01 : .1);
            obs_properties_add_float(row, (key + "_target").c_str(), "Target of Start", p.minimum,
                                     p.maximum, k == 4 ? .01 : .1);
            obs_properties_add_float(row, (key + "_duration_ms").c_str(),
                                     "Move duration ms (-1 = default)", -1, 3600000, 1);
            easingProperty(row, key + "_easing", "Move easing", true);
            obs_properties_add_bool(row, (key + "_auto_return").c_str(),
                                    "Auto return after reaching the target");
            obs_properties_add_float(row, (key + "_return_value").c_str(), "Return endpoint", p.minimum,
                                     p.maximum, .01);
            obs_properties_add_float(row, (key + "_hold_ms").c_str(),
                                     "Return delay after the last Add (ms)", 0, 3600000, 10);
            obs_properties_add_float(row, (key + "_return_ms").c_str(), "Return duration (ms)", 0, 3600000,
                                     10);
            easingProperty(row, key + "_return_easing", "Return easing");
            for (const char *action : {"Start", "Stop", "Reset"})
                obs_properties_add_button2(row, (std::string(action) + ":" + key).c_str(), action, button, v);
            obs_properties_add_group(group, (key + "_controls").c_str(), labels[k], OBS_GROUP_NORMAL, row);
        }
        obs_properties_add_group(props, ("point" + std::to_string(z + 1)).c_str(), pointName(z),
                                 OBS_GROUP_NORMAL, group);
    }
    // Meme morphs: one group per morph; strength is a cumulative animated parameter (Add stacks).
    for (int i = 0; i < morphCount; ++i) {
        const std::string k = std::string("morph_") + morphKey(i);
        auto *row = obs_properties_create();
        obs_properties_add_bool(row, (k + "_enabled").c_str(), "Enabled");
        obs_properties_add_float_slider(row, (k + "_value").c_str(), "Strength", -1, 1, .01);
        obs_properties_add_float(row, (k + "_target").c_str(), "Target of Start", -1, 1, .01);
        obs_properties_add_float(row, (k + "_duration_ms").c_str(), "Move duration ms (-1 = default)",
                                 -1, 3600000, 1);
        easingProperty(row, k + "_easing", "Move easing", true);
        obs_properties_add_bool(row, (k + "_auto_return").c_str(), "Auto return after reaching the target");
        obs_properties_add_float(row, (k + "_return_value").c_str(), "Return endpoint", -1, 1, .01);
        obs_properties_add_float(row, (k + "_hold_ms").c_str(),
                                 "Return delay after the last Add (ms)", 0, 3600000, 10);
        obs_properties_add_float(row, (k + "_return_ms").c_str(), "Return duration (ms)", 0, 3600000, 10);
        easingProperty(row, k + "_return_easing", "Return easing");
        for (const char *action : {"Start", "Stop", "Reset"})
            obs_properties_add_button2(row, ("fx" + std::to_string(i) + ":" + action).c_str(), action,
                                       button, v);
        obs_properties_add_group(props, k.c_str(), morphName(i), OBS_GROUP_NORMAL, row);
    }
    return props;
}
void registerFilter() {
    obs_source_info info{};
    info.id = filterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.video_render = render;
    obs_register_source(&info);
}
} // namespace opa
