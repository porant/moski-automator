#include "face_tracker.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef OPA_FACE_TRACKING
#include "head_pose.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>
#endif

namespace opa {

struct FaceTracker::Impl {
    // Settings owned by the settings thread, snapshotted by the worker.
    std::mutex cfgMutex;
    FaceTrackerConfig cfg;
    std::string modelPath, error;
    bool available = false;

    // Pending frame: written by the render thread, consumed by the worker.
    std::mutex pendingMutex;
    std::condition_variable cv;
    std::vector<uint8_t> pending;
    int pendingW = 0, pendingH = 0;
    bool hasPending = false, stopping = false;
#ifdef OPA_FACE_TRACKING
    // Set by configure()/setModelPath() so the worker (re-)creates the detector *before* the first
    // frame arrives; without this, available() would stay false and the render path would never
    // submit the first frame.
    bool wake = true;
#endif

    // Latest result: written by the worker, read by the render/UI/vendor threads.
    mutable std::mutex resultMutex;
    Result result;
    uint64_t dropped = 0;

    std::thread worker;

    Impl() {
#ifdef OPA_FACE_TRACKING
        worker = std::thread([this] { run(); });
#else
        error = "Face tracking built without OpenCV (OPA_FACE_TRACKING off)";
#endif
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }

#ifdef OPA_FACE_TRACKING
    static cv::Ptr<cv::FaceDetectorYN> createDetector(const std::string &path, const FaceTrackerConfig &c,
                                                      std::string &err) {
        try {
            cv::Size size(320, c.detectHeight > 0 ? c.detectHeight : 180);
            return cv::FaceDetectorYN::create(path, "", size, (float)c.scoreThreshold, 0.3f, 5000);
        } catch (const std::exception &ex) {
            err = ex.what();
            return nullptr;
        }
    }

    void run() {
        cv::Ptr<cv::FaceDetectorYN> detector;
        std::string loadError;
        for (;;) {
            std::vector<uint8_t> frame;
            int w = 0, h = 0;
            bool haveFrame = false;
            FaceTrackerConfig local;
            std::string path;
            bool rebuild = false;
            {
                std::unique_lock<std::mutex> lock(pendingMutex);
                // Wake on a new frame OR on a settings change, so the detector is created even
                // before the first frame (making available() meaningful to the render path).
                cv.wait(lock, [this] { return stopping || hasPending || wake; });
                if (stopping)
                    return;
                rebuild = wake;
                wake = false;
                if (hasPending) {
                    frame.swap(pending);
                    w = pendingW;
                    h = pendingH;
                    hasPending = false;
                    haveFrame = true;
                }
            }
            {
                std::lock_guard<std::mutex> lock(cfgMutex);
                local = cfg;
                path = modelPath;
            }
            if (rebuild || (!detector && !path.empty())) {
                detector = createDetector(path, local, loadError);
                std::lock_guard<std::mutex> lock(cfgMutex);
                available = detector != nullptr;
                error = detector ? std::string()
                                 : (path.empty() ? "YuNet model path is empty" : loadError);
            }
            if (!haveFrame || !detector || w <= 0 || h <= 0 || (int)frame.size() < w * h * 4)
                continue;
            const auto begin = std::chrono::steady_clock::now();
            Result fresh;
            try {
                cv::Mat rgba(h, w, CV_8UC4, frame.data());
                cv::Mat bgr;
                cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
                detector->setInputSize(cv::Size(w, h));
                cv::Mat faces;
                detector->detect(bgr, faces);
                for (int i = 0; i < faces.rows; ++i) {
                    const float *f = faces.ptr<float>(i);
                    FaceRect r;
                    r.cx = (f[0] + f[2] * 0.5f) / w * 100.0;
                    r.cy = (f[1] + f[3] * 0.5f) / h * 100.0;
                    r.w = f[2] / w * 100.0;
                    r.h = f[3] / h * 100.0;
                    r.score = f[14];
                    // YuNet landmarks: right eye, left eye, nose tip, right/left mouth corner.
                    const double rex = f[4], rey = f[5], lex = f[6], ley = f[7];
                    const double ntx = f[8], nty = f[9];
                    const double rmx = f[10], rmy = f[11], lmx = f[12], lmy = f[13];
                    const auto pctX = [w](double X) { return std::clamp(X / w * 100.0, -50.0, 150.0); };
                    const auto pctY = [h](double Y) { return std::clamp(Y / h * 100.0, -50.0, 150.0); };
                    // Keep the raw landmarks too, so feature-anchored morphs (eyes/nose/mouth) can
                    // use them later; the three anchors below are derived from the same points.
                    r.landmark[0] = {pctX(rex), pctY(rey)}; // right eye
                    r.landmark[1] = {pctX(lex), pctY(ley)}; // left eye
                    r.landmark[2] = {pctX(ntx), pctY(nty)}; // nose tip
                    r.landmark[3] = {pctX(rmx), pctY(rmy)}; // right mouth corner
                    r.landmark[4] = {pctX(lmx), pctY(lmy)}; // left mouth corner
                    if (ntx >= 0.0 && lex >= 0.0 && rex >= 0.0) {
                        r.landmarkValid = true;
                        // Head roll = angle of the right-eye -> left-eye axis (0 when upright).
                        r.roll = std::atan2(ley - rey, lex - rex);
                        // Full head orientation: fit a generic 3D face model to the landmarks so
                        // turn (yaw) and nod (pitch) follow too, not just the in-plane roll. Head
                        // pose is optional, so a failure here must never drop the detection.
                        try {
                            const std::array<cv::Point2f, landmarkCount> lmPix = {
                                cv::Point2f((float)rex, (float)rey),
                                cv::Point2f((float)lex, (float)ley),
                                cv::Point2f((float)ntx, (float)nty),
                                cv::Point2f((float)rmx, (float)rmy),
                                cv::Point2f((float)lmx, (float)lmy)};
                            const HeadPose pose = estimateHeadPose(lmPix, (double)w, (double)h);
                            r.yaw = pose.yaw;
                            r.pitch = pose.pitch;
                            r.poseValid = pose.valid;
                        } catch (const std::exception &ex) {
                            std::lock_guard<std::mutex> lock(cfgMutex);
                            error = std::string("pose failed: ") + ex.what();
                        } catch (...) {
                            std::lock_guard<std::mutex> lock(cfgMutex);
                            error = "pose failed: unknown exception";
                        }
                        // Face-local axes from the landmarks so the derived points follow head roll
                        // and pitch instead of always going straight up/down in frame space:
                        //   eyeMid - nose  = "up"   direction of the face
                        //   mouthMid - nose = "down" direction of the face
                        const double emx = (rex + lex) * 0.5, emy = (rey + ley) * 0.5;
                        const double mmx = (rmx + lmx) * 0.5, mmy = (rmy + lmy) * 0.5;
                        r.anchor[0] = {pctX(emx + 0.9 * (emx - ntx)),
                                       pctY(emy + 0.9 * (emy - nty))};               // forehead
                        r.anchor[1] = {pctX((emx + ntx) * 0.5), pctY((emy + nty) * 0.5)}; // nose bridge
                        r.anchor[2] = {pctX(mmx + 0.9 * (mmx - ntx)),
                                       pctY(mmy + 0.9 * (mmy - nty))};               // below chin
                    } else {
                        // Landmarks unavailable: fall back to fractions of the detection box.
                        r.anchor[0] = {r.cx, std::clamp(r.cy - 0.45 * r.h, -50.0, 150.0)};
                        r.anchor[1] = {r.cx, std::clamp(r.cy - 0.05 * r.h, -50.0, 150.0)};
                        r.anchor[2] = {r.cx, std::clamp(r.cy + 0.72 * r.h, -50.0, 150.0)};
                    }
                    fresh.faces.push_back(r);
                }
            } catch (const std::exception &ex) {
                std::lock_guard<std::mutex> lock(cfgMutex);
                error = ex.what();
            }
            std::sort(fresh.faces.begin(), fresh.faces.end(),
                      [](const FaceRect &a, const FaceRect &b) { return a.cx < b.cx; });
            if ((int)fresh.faces.size() > local.maxFaces)
                fresh.faces.resize(local.maxFaces);
            fresh.detectMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                    .count();
            {
                std::lock_guard<std::mutex> lock(resultMutex);
                fresh.sequence = result.sequence + 1;
                fresh.processed = result.processed + 1;
                fresh.dropped = dropped;
                result = std::move(fresh);
            }
        }
    }
#endif
};

FaceTracker::FaceTracker() : impl_(std::make_unique<Impl>()) {}
FaceTracker::~FaceTracker() = default;

bool FaceTracker::available() const {
    std::lock_guard<std::mutex> lock(impl_->cfgMutex);
    return impl_->available;
}
std::string FaceTracker::lastError() const {
    std::lock_guard<std::mutex> lock(impl_->cfgMutex);
    return impl_->error;
}

void FaceTracker::setModelPath(const std::string &path) {
    {
        std::lock_guard<std::mutex> lock(impl_->cfgMutex);
        impl_->modelPath = path;
    }
#ifdef OPA_FACE_TRACKING
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->wake = true; // (re-)create the detector even before any frame arrives
    }
    impl_->cv.notify_all();
#endif
}

void FaceTracker::configure(const FaceTrackerConfig &cfg) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->cfgMutex);
        changed = impl_->cfg.maxFaces != cfg.maxFaces ||
                  impl_->cfg.scoreThreshold != cfg.scoreThreshold ||
                  impl_->cfg.detectHeight != cfg.detectHeight;
        impl_->cfg = cfg;
    }
#ifdef OPA_FACE_TRACKING
    if (changed) {
        // Rebuilding reloads the ONNX model, so only do it when a detector field actually changed;
        // otherwise a settings update (e.g. dragging a zone) would reload the net every tick.
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->wake = true;
        impl_->cv.notify_all();
    }
#endif
}

void FaceTracker::submit(const uint8_t *rgba, int width, int height, int linesize) {
    if (!rgba || width <= 0 || height <= 0 || linesize < width * 4)
        return;
    const auto stride = (size_t)width * 4;
    std::lock_guard<std::mutex> lock(impl_->pendingMutex);
    if (impl_->hasPending)
        ++impl_->dropped; // the worker has not caught up; replace instead of queueing
    impl_->pending.resize(stride * height);
    for (int y = 0; y < height; ++y)
        std::copy(rgba + (size_t)y * linesize, rgba + (size_t)y * linesize + stride,
                  impl_->pending.begin() + (size_t)y * stride);
    impl_->pendingW = width;
    impl_->pendingH = height;
    impl_->hasPending = true;
    impl_->cv.notify_one();
}

FaceTracker::Result FaceTracker::result() const {
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    return impl_->result;
}

void FaceTracker::reset() {
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    impl_->result = Result{};
}

} // namespace opa

