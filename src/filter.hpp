#pragma once
#include "animation.hpp"
#include "face_tracker.hpp"
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <vector>
namespace opa {
constexpr const char *filterId = "opa_6zone_distortion";
// A face contributes up to pointCount zones (eyes / nose / mouth); the shader's
// zone_data array is sized for maxFaces faces.
constexpr int maxFaces = 8;
constexpr int maxZones = maxFaces * pointCount;
// Debug markers: the 3 face-point anchors plus the 5 raw YuNet landmarks, per detected face.
constexpr int maxMarkers = maxFaces * (pointCount + landmarkCount);
enum class Mode { Static = 0, Shader = 1, Plugin = 2 };
struct Snapshot {
    AnimationController controller;
    Mode mode;
    int fps;
    double defaultDuration;
    int defaultEasing;
    double updateUs = 0, maxUpdateUs = 0;
    uint64_t evaluations = 0, parameterUpdates = 0, uniformCalls = 0;
    std::vector<std::string> logs;
    // Face tracking: last detections (normalised 0..100) plus lightweight status for the UI.
    std::vector<FaceRect> faces;
    bool faceTracking = false;
    bool faceAvailable = false;
    uint64_t faceSequence = 0;
    double faceDetectMs = 0;
    uint64_t faceDropped = 0;
    bool effectBlur = false;
    bool effectDebug = false;
    // Meme morphs: one animated, cumulative intensity per morph slot.
    std::array<MorphEffect, morphCount> morphs{};
};
class Engine {
  public:
    Engine() {
        // Morph slots are a fixed catalogue (one per MorphType); initialize once so their
        // animated intensity keeps the [-1..1] bounds without being reset on every reconfigure.
        for (int i = 0; i < morphCount; ++i)
            morphs[i].initialize(static_cast<MorphType>(i));
    }
    mutable std::mutex mutex;
    AnimationController controller;
    std::array<MorphEffect, morphCount> morphs{};
    Mode mode = Mode::Plugin;
    bool effectBlur = false;  // independent option: blur the whole detected face box
    bool effectDebug = false; // independent option: overlay the face points
    double faceBlurPx = 24;   // blur radius in pixels
    bool faceScale = true;    // scale point radius and offsets by the face height
    int fps = 0;
    double defaultDuration = 1;
    int defaultEasing = 5;
    double lastSample = -1, epoch = nowSeconds(), updateUs = 0, maxUpdateUs = 0;
    uint64_t evaluations = 0, parameterUpdates = 0, uniformCalls = 0;
    std::deque<std::string> logs;
    bool initialized = false;
    // --- face tracking (one distortion zone per detected face x enabled point) ---
    bool faceTracking = false;
    int faceFps = 10;
    int faceMax = 6;                                   // capped at maxFaces
    double faceScore = 0.7;
    int faceDetectHeight = 180;                        // downscale height of the detection frame
    double faceSmoothMs = 120;                         // temporal smoothing of the anchor points
    double nextFaceCapture = 0;
    std::shared_ptr<FaceTracker> tracker = std::make_shared<FaceTracker>();
    // Persistent per-face tracks so the points can be eased between the low-FPS detections instead
    // of jumping to the latest detection each frame (removes jitter). Faces are matched by nearest
    // centre; unmatched detections open a new track, unseen tracks expire.
    struct FaceTrack {
        double cx = 0, cy = 0, w = 0, h = 0;
        std::array<FacePoint, pointCount> anchors{};
        std::array<FacePoint, landmarkCount> landmarks{}; // raw YuNet landmarks, smoothed like anchors
        bool hasLandmarks = false;                        // false when the detector used box fractions
        double roll = 0;                                  // head in-plane rotation, radians
        double yaw = 0;                                   // head turn, radians
        double pitch = 0;                                 // head nod, radians
        bool valid = false;
    };
    std::array<FaceTrack, maxFaces> tracks{};
    double lastFaceSmoothTime = -1;
    void smoothFaces(const std::vector<FaceRect> &faces, double now);
    void configure(obs_data_t *settings);
    void command(int index, const std::string &action, double value = 0, double duration = -1,
                 int easing = -1, int returnOverride = -1);
    // Same Add/Set/Start/Stop/Reset semantics as command(), but for a meme morph slot.
    void commandMorph(int index, const std::string &action, double value = 0, double duration = -1,
                      int easing = -1, int returnOverride = -1);
    void startAll();
    void stopAll();
    void resetAll();
    // True while every morph intensity is at rest (0) and no trajectory is running. Combined with
    // magnitudesAtRest() this lets the filter skip the whole GPU pass when nothing is active.
    bool morphsAtRest() const {
        for (const auto &m : morphs)
            if (m.intensity.state.active || m.intensity.gpuValue() != 0.0)
                return false;
        return true;
    }
    Snapshot snapshot() const;
    void logLocked(const std::string &s);
    void sampleLocked(double now);
};
struct FilterEntry {
    std::string source, sourceUuid, filter, filterUuid;
};
std::shared_ptr<Engine> engineFor(obs_source_t *filter);
std::vector<FilterEntry> enumerateFilters();
obs_source_t *resolveFilter(obs_data_t *request); // Caller releases strong reference.
void registerFilter();
} // namespace opa
