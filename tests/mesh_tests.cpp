// Smoke test for the MediaPipe Face Mesh ONNX integration: loads the model + canonical 3D model
// through OpenCV DNN, runs one forward pass and checks a 468-landmark output is produced. This
// catches NHWC blob / output-packing bugs that a pure Python check would miss.
#include "face_mesh.hpp"

#include <cmath>
#include <cstdio>
#include <opencv2/core.hpp>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::printf("usage: mesh-tests <face_landmarks.onnx> <canonical_face_model.obj>\n");
        return 2;
    }
    opa::FaceMesh mesh;
    const bool ok = mesh.load(argv[1], argv[2]);
    std::printf("load=%d available=%d\n", (int)ok, (int)mesh.available());
    if (!mesh.available()) {
        std::printf("FAIL: face mesh model not available\n");
        return 1;
    }
    // A plain frame: the net still executes, exercising the NHWC blob + 1434-value output path.
    cv::Mat frame(360, 480, CV_8UC3, cv::Scalar(40, 40, 40));
    const opa::FaceMesh::Result r = mesh.run(frame, cv::Rect(140, 60, 200, 240));
    const bool finite = r.valid && std::isfinite(r.pts2d[0].x) && std::isfinite(r.nose.x);
    std::printf("valid=%d pts2d=%zu canonical=%zu finite=%d\n", (int)r.valid, r.pts2d.size(),
                r.canonical.size(), (int)finite);
    if (!finite || r.pts2d.size() != 468 || r.canonical.size() != 468) {
        std::printf("FAIL: face mesh forward did not produce 468 landmarks\n");
        return 1;
    }
    std::printf("PASS: face mesh loads and runs\n");
    return 0;
}
