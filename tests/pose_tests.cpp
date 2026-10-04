// Synthetic round-trip check for estimateHeadPose(): projects the canonical 3D face model under a
// known rotation, feeds the resulting 2D landmarks back in and checks the recovered angles. This
// pins the axis/sign conventions without a camera.
#include "head_pose.hpp"

#include <array>
#include <cmath>
#include <cstdio>
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

    std::printf(failures ? "FAIL: %d pose check(s) failed\n" : "PASS: head pose round-trip\n",
                failures);
    return failures ? 1 : 0;
}
