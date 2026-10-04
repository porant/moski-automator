#pragma once
// Cheap face detection for the face-point distortion filter.
//
// Detection runs on a dedicated worker thread using OpenCV's YuNet (FaceDetectorYN). The
// graphics/render thread only copies a *small* RGBA frame into a pending slot and reads the
// latest result back; it never runs the detector itself. This keeps the filter's render path
// free of CV work, matching the plugin's "GPU render thread stays the only GPU caller" rule.
//
// When the plugin is built without OpenCV (OPA_FACE_TRACKING not defined) the class is still
// compiled, but stays inert: available() == false, submit() is a no-op and result() is empty.
// That way no OBS build is forced to depend on OpenCV.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opa {

struct FacePoint {
    double x = 0;
    double y = 0;
};

// Raw YuNet landmarks kept per face (right eye, left eye, nose tip, right/left mouth corner).
constexpr int landmarkCount = 5;

// One detected face, normalised to 0..100 (percent of the frame width/height). Those are the
// same units the shader uses for a zone centre and radius, so a point can be written into a zone
// without any extra scaling.
struct FaceRect {
    double cx = 0;    // centre X, percent
    double cy = 0;    // centre Y, percent
    double w = 0;     // width, percent
    double h = 0;     // height, percent
    double score = 0; // confidence 0..1
    // Three anchors placed on the YuNet landmarks, percent units:
    //   [0] eyes (midpoint of both eyes), [1] nose tip, [2] mouth (midpoint of the corners).
    FacePoint anchor[3];
    // Raw YuNet landmarks in percent units, order: right eye, left eye, nose tip, right/left mouth
    // corner. Zeroed when the detector could not supply them. Kept for feature-anchored morphs
    // (eyes / nose / mouth) and for the debug overlay.
    FacePoint landmark[landmarkCount];
    bool landmarkValid = false; // false when the detector fell back to box fractions
    // Face in-plane rotation (roll) in radians, from the right-eye -> left-eye axis. 0 = upright,
    // positive = head tilted so the left eye goes down. Used to orient landmark-anchored morphs.
    double roll = 0;
    // Head turn (yaw) and nod (pitch) in radians, from a solvePnP fit of the five landmarks.
    double yaw = 0;
    double pitch = 0;
    bool poseValid = false;
};

struct FaceTrackerConfig {
    int maxFaces = 6;
    double scoreThreshold = 0.7;
    int detectHeight = 180; // height the frame is downscaled to before detection
};

// YuNet wrapper with a background worker. Thread-safety: submit()/result()/reset() are safe to
// call from the render thread; configure()/setModelPath() are called from the settings thread.
class FaceTracker {
  public:
    FaceTracker();
    ~FaceTracker();
    FaceTracker(const FaceTracker &) = delete;
    FaceTracker &operator=(const FaceTracker &) = delete;

    // True when the plugin was built with OpenCV and the model loaded successfully.
    bool available() const;
    std::string lastError() const;

    // Path to the YuNet ONNX model. Must be set before the tracker can become available.
    void setModelPath(const std::string &path);
    // Optional dense-landmark model (MediaPipe Face Mesh ONNX) + its canonical 3D model. When both
    // load, the tracker uses the dense landmarks for the anchors and the head pose; otherwise it
    // falls back to YuNet's five points.
    void setMeshModelPaths(const std::string &onnxPath, const std::string &canonicalObjPath);
    void configure(const FaceTrackerConfig &cfg);

    // Copy one tightly-packed, top-down RGBA8 frame into the pending slot. Cheap and
    // allocation-free after warm-up. If the worker has not consumed the previous pending frame
    // yet, that frame is dropped (counted in Result::dropped) instead of queueing up.
    void submit(const uint8_t *rgba, int width, int height, int linesize);

    struct Result {
        std::vector<FaceRect> faces; // sorted left-to-right by centre X
        uint64_t sequence = 0;       // increments on every completed detection
        double detectMs = 0;         // wall time of the last detection
        uint64_t processed = 0;      // frames actually run through the detector
        uint64_t dropped = 0;        // submitted frames overwritten before detection
    };
    Result result() const;
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace opa
