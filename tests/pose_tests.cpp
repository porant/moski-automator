// Synthetic round-trip check for estimateHeadPose(): projects the canonical 3D face model under a
// known rotation, feeds the resulting 2D landmarks back in and checks the recovered angles. This
// pins the axis/sign conventions without a camera.
#include "head_pose.hpp"
#include "one_euro.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
#include <opencv2/calib3d.hpp>

using namespace opa;

namespace {

constexpr double kFocal = 640.0, kWidth = 640.0, kHeight = 480.0, kDepth = 1000.0;

std::array<cv::Point2f, 5> project(const cv::Matx33d &R) {
    std::array<cv::Point2f, 5> out{};
    const auto &m = headModel();
    for (size_t i = 0; i < m.size(); ++i) {
        const cv::Vec3d X(m[i].x, m[i].y, m[i].z);
        cv::Vec3d Y = R * X;
        Y[2] += kDepth;
        out[i] = cv::Point2f((float)(kFocal * Y[0] / Y[2] + kWidth * 0.5),
                             (float)(kFocal * Y[1] / Y[2] + kHeight * 0.5));
    }
    return out;
}

constexpr double kRad = 3.14159265358979323846 / 180.0;

HeadPose recover(const cv::Matx33d &R) { return estimateHeadPose(project(R), kWidth, kHeight); }

// A denser, non-planar synthetic face: a 6x6 grid with per-point depth, span ~ a real face. Used to
// exercise estimateHeadPosePoints() with far more correspondences than YuNet's five.
std::vector<cv::Point3f> denseModel() {
    std::vector<cv::Point3f> m;
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            m.emplace_back((j - 2.5f) * 95.0f, (i - 2.5f) * 80.0f,
                           120.0f + 45.0f * std::sin(i * 1.3f + j * 0.9f));
    return m;
}

std::vector<cv::Point2f> projectPts(const std::vector<cv::Point3f> &m, const cv::Matx33d &R) {
    std::vector<cv::Point2f> out;
    out.reserve(m.size());
    for (const auto &p : m) {
        const cv::Vec3d X(p.x, p.y, p.z);
        cv::Vec3d Y = R * X;
        Y[2] += kDepth;
        out.emplace_back((float)(kFocal * Y[0] / Y[2] + kWidth * 0.5),
                         (float)(kFocal * Y[1] / Y[2] + kHeight * 0.5));
    }
    return out;
}

HeadPose recoverDense(const cv::Matx33d &R) {
    const auto model = denseModel();
    return estimateHeadPosePoints(model, projectPts(model, R), kWidth, kHeight);
}

int failures = 0;

void checkNear(const char *what, double got, double want) {
    const double gotDeg = got / kRad, wantDeg = want / kRad;
    const bool ok = std::fabs(gotDeg - wantDeg) < 2.0;
    std::printf("%-28s got=%+7.2f  want=%+7.2f  %s\n", what, gotDeg, wantDeg, ok ? "ok" : "FAIL");
    if (!ok)
        ++failures;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // an OpenCV abort() would otherwise swallow the output
    // Identity: an upright, forward-facing head must be neutral.
    {
        const HeadPose p = recover(cv::Matx33d::eye());
        std::printf("identity: valid=%d roll=%+.2f yaw=%+.2f pitch=%+.2f (deg)\n", (int)p.valid,
                    p.roll / kRad, p.yaw / kRad, p.pitch / kRad);
        checkNear("identity roll", p.roll, 0.0);
        checkNear("identity yaw", p.yaw, 0.0);
        checkNear("identity pitch", p.pitch, 0.0);
    }
    // Each camera axis must drive exactly one recovered angle, with a clean sign, so the documented
    // image-space semantics hold: Rx -> pitch, Ry -> yaw, Rz -> roll.
    const double a = 20.0 * kRad;
    const cv::Matx33d Rx(1, 0, 0, 0, std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a));
    const cv::Matx33d Ry(std::cos(a), 0, std::sin(a), 0, 1, 0, -std::sin(a), 0, std::cos(a));
    const cv::Matx33d Rz(std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1);
    const HeadPose px = recover(Rx), py = recover(Ry), pz = recover(Rz);
    std::printf("Rx(+20): roll=%+.2f yaw=%+.2f pitch=%+.2f\n", px.roll / kRad, px.yaw / kRad,
                px.pitch / kRad);
    std::printf("Ry(+20): roll=%+.2f yaw=%+.2f pitch=%+.2f\n", py.roll / kRad, py.yaw / kRad,
                py.pitch / kRad);
    std::printf("Rz(+20): roll=%+.2f yaw=%+.2f pitch=%+.2f\n", pz.roll / kRad, pz.yaw / kRad,
                pz.pitch / kRad);
    checkNear("Rz(+20) -> roll", pz.roll, 20.0 * kRad);
    checkNear("Ry(+20) -> yaw", py.yaw, 20.0 * kRad);
    checkNear("Rx(+20) -> pitch", px.pitch, 20.0 * kRad);
    // The other angles must stay near zero, i.e. the axes must not bleed into each other.
    checkNear("Rz(+20) -> yaw", pz.yaw, 0.0);
    checkNear("Rz(+20) -> pitch", pz.pitch, 0.0);
    checkNear("Ry(+20) -> roll", py.roll, 0.0);
    checkNear("Ry(+20) -> pitch", py.pitch, 0.0);
    checkNear("Rx(+20) -> roll", px.roll, 0.0);
    checkNear("Rx(+20) -> yaw", px.yaw, 0.0);

    // Dense (N-point) fit: the same axes must come back from a 36-point non-planar model, i.e. the
    // generic estimateHeadPosePoints() path that a dense landmark model would use.
    checkNear("dense Rx(+20) -> pitch", recoverDense(Rx).pitch, 20.0 * kRad);
    checkNear("dense Ry(+20) -> yaw", recoverDense(Ry).yaw, 20.0 * kRad);
    checkNear("dense Rz(+20) -> roll", recoverDense(Rz).roll, 20.0 * kRad);

    // One-Euro filter: a constant input must converge to that value, and a jittered input must come
    // out with a much smaller range (that is what stabilises the noisy pose angles).
    {
        OneEuroFilter f(60.0, 1.0, 0.0, 1.0); // beta = 0 -> plain low-pass, easy to reason about
        double v = 0.0;
        for (int i = 0; i < 300; ++i)
            v = f.filter(1.0, 1.0 / 60.0);
        const bool steadyOk = std::fabs(v - 1.0) < 1e-2;
        std::printf("%-28s got=%+7.3f  want=%+7.3f  %s\n", "one-euro steady state", v, 1.0,
                    steadyOk ? "ok" : "FAIL");
        if (!steadyOk)
            ++failures;

        OneEuroFilter g(60.0, 1.0, 0.0, 1.0);
        double outLo = 1e9, outHi = -1e9;
        for (int i = 0; i < 200; ++i) {
            const double out = g.filter((i % 2) ? 1.5 : 0.5, 1.0 / 60.0);
            if (out < outLo) outLo = out;
            if (out > outHi) outHi = out;
        }
        const double outRange = outHi - outLo; // input range is 1.0
        const bool jitterOk = outRange < 0.8;  // clearly damped (the raw range would be 1.0)
        std::printf("%-28s in_range=1.000 out_range=%.3f  %s\n", "one-euro jitter damping", outRange,
                    jitterOk ? "ok" : "FAIL");
        if (!jitterOk)
            ++failures;
    }

    std::printf(failures ? "FAIL: %d pose check(s) failed\n" : "PASS: head pose round-trip\n",
                failures);
    return failures ? 1 : 0;
}
