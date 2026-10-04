#pragma once
// Head orientation (roll / yaw / pitch) estimated from the five YuNet landmarks.
//
// A generic 3D face model is fitted to the 2D landmarks with cv::solvePnP. Only the *rotation* is
// used, so an approximate camera (focal length = image width, principal point at the centre) is
// good enough. Kept header-only and OpenCV-only so the tracker can call it on its worker thread and
// a test can feed synthetic landmarks without a camera.
//
//   roll  - in-plane tilt       (like resting an ear on the shoulder)  -> camera Z axis
//   yaw   - turn left / right   (like shaking the head "no")           -> camera Y axis
//   pitch - nod up / down       (like nodding "yes")                   -> camera X axis
//
// Signs are stated in image terms (image x points right, y points down):
//   roll  > 0 - the top of the head tips toward the right of the image (clockwise)
//   yaw   > 0 - the nose points toward the right of the image (the face turns right)
//   pitch > 0 - the face looks up (the nose points toward the top of the image)
#ifdef OPA_FACE_TRACKING
#include <array>
#include <cmath>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <vector>

namespace opa {

struct HeadPose {
    bool valid = false;
    double roll = 0;  // radians
    double yaw = 0;   // radians
    double pitch = 0; // radians
};

// Canonical face model in millimetres. Camera convention: x right, y down, z into the scene, with
// the nose tip at the origin (closest to the camera). Order matches the YuNet landmarks:
// right eye, left eye, nose tip, right mouth corner, left mouth corner. "Right"/"left" are the
// subject's, so the right eye sits on the image-left (negative x).
inline const std::array<cv::Point3f, 5> &headModel() {
    static const std::array<cv::Point3f, 5> model = {
        cv::Point3f(-225.0f, -170.0f, 135.0f), // right eye
        cv::Point3f(225.0f, -170.0f, 135.0f),  // left eye
        cv::Point3f(0.0f, 0.0f, 0.0f),         // nose tip
        cv::Point3f(-150.0f, 150.0f, 125.0f),  // right mouth corner
        cv::Point3f(150.0f, 150.0f, 125.0f),   // left mouth corner
    };
    return model;
}

// landmarks: [0] right eye, [1] left eye, [2] nose tip, [3] right mouth, [4] left mouth - all in
// detector pixel coordinates (origin top-left, y down).
inline HeadPose estimateHeadPose(const std::array<cv::Point2f, 5> &landmarks, double imgW, double imgH);

// Generic N-point fit: the same 3D->2D correspondences as any landmark set (a dense model can feed
// dozens of points, which is far more stable than the five YuNet points). EPNP is only the initial
// guess; solvePnPRefineLM then refines it, which removes most of the instability EPNP shows on the
// near-coplanar five-point case.
inline HeadPose estimateHeadPosePoints(const std::vector<cv::Point3f> &modelPts,
                                       const std::vector<cv::Point2f> &imagePts, double imgW,
                                       double imgH) {
    HeadPose pose;
    if (imgW <= 0.0 || imgH <= 0.0 || modelPts.size() < 4 || modelPts.size() != imagePts.size())
        return pose;

    const double focal = imgW; // focal length is unknown; the rotation is insensitive to it
    const cv::Mat camera = (cv::Mat_<double>(3, 3) << focal, 0.0, imgW * 0.5, 0.0, focal, imgH * 0.5,
                            0.0, 0.0, 1.0);

    cv::Vec3d rvec, tvec;
    bool ok = false;
    try {
        // EPNP works with as few as four points; SOLVEPNP_ITERATIVE would need six and throws.
        ok = cv::solvePnP(modelPts, imagePts, camera, cv::noArray(), rvec, tvec, false,
                          cv::SOLVEPNP_EPNP);
        if (ok) {
            // Refine the EPNP guess with Levenberg-Marquardt on the same correspondences. This is
            // what makes the pose stable when the points are nearly coplanar.
            cv::solvePnPRefineLM(modelPts, imagePts, camera, cv::noArray(), rvec, tvec);
        }
    } catch (const cv::Exception &) {
        return pose; // degenerate landmarks (e.g. all points collinear)
    }
    if (!ok)
        return pose;

    cv::Matx33d R;
    cv::Rodrigues(rvec, R);
    const double sy = std::sqrt(R(0, 0) * R(0, 0) + R(1, 0) * R(1, 0));
    pose.pitch = std::atan2(R(2, 1), R(2, 2));
    pose.yaw = std::atan2(-R(2, 0), sy);
    pose.roll = std::atan2(R(1, 0), R(0, 0));
    pose.valid = true;
    return pose;
}

inline HeadPose estimateHeadPose(const std::array<cv::Point2f, 5> &landmarks, double imgW, double imgH) {
    const std::vector<cv::Point3f> model(headModel().begin(), headModel().end());
    const std::vector<cv::Point2f> image(landmarks.begin(), landmarks.end());
    return estimateHeadPosePoints(model, image, imgW, imgH);
}

} // namespace opa
#endif // OPA_FACE_TRACKING
