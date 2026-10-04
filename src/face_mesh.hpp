#pragma once
// Dense face landmarks via Google's MediaPipe Face Mesh V2 (ONNX), executed with OpenCV DNN.
//
// Gives 478 3D landmarks (468 face mesh + 10 iris) per face from a 256x256 face crop. Compared to
// YuNet's five points this is far more stable for the head pose and lets the three anchors sit
// exactly on the eyes / nose / mouth.
//
// Two important details:
//   * This ONNX is a tf2onnx export with an NHWC input ([1,256,256,3]); OpenCV DNN's usual NCHW
//     blob makes it fail, so the blob is assembled in NHWC by hand.
//   * The canonical 3D model is Google's canonical_face_model.obj, whose axes are x right, y UP,
//     z toward the viewer. It is converted here to this project's convention (x right, y down,
//     z into the scene) by negating y and z.
#ifdef OPA_FACE_TRACKING
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace opa {

class FaceMesh {
  public:
    struct Result {
        bool valid = false;
        std::vector<cv::Point3f> canonical; // 468 points, project convention (x right/y down/z in)
        std::vector<cv::Point2f> pts2d;     // 468 points in detection-frame pixels
        cv::Point2f eyeRight, eyeLeft, nose, mouth; // anchors in detection-frame pixels
        cv::Point2f mouthRight, mouthLeft;          // mouth corners (for the debug landmarks)
    };

    bool load(const std::string &onnxPath, const std::string &canonicalObjPath) {
        net_ = cv::dnn::Net();
        canonical_.clear();
        try {
            net_ = cv::dnn::readNetFromONNX(onnxPath);
        } catch (const cv::Exception &) {
            return false;
        }
        if (net_.empty())
            return false;

        std::ifstream f(canonicalObjPath);
        if (!f)
            return false;
        std::string line;
        while (std::getline(f, line) && canonical_.size() < 468) {
            if (line.size() < 2 || line[0] != 'v' || line[1] != ' ')
                continue;
            std::istringstream ss(line.substr(2));
            float x = 0, y = 0, z = 0;
            if (ss >> x >> y >> z)
                canonical_.emplace_back(x, -y, -z); // MediaPipe (y up, z out) -> ours (y down, z in)
        }
        return canonical_.size() == 468;
    }

    bool available() const { return !net_.empty() && canonical_.size() == 468; }

    // bgrFrame: whole detection frame (BGR). box: the face rectangle in that frame's pixels.
    Result run(const cv::Mat &bgrFrame, const cv::Rect &box) {
        Result r;
        if (!available() || bgrFrame.empty())
            return r;

        // Square crop around the face with a margin, clamped to the frame.
        const double side = std::max(box.width, box.height) * 1.5;
        cv::Point2f c(box.x + box.width * 0.5f, box.y + box.height * 0.5f);
        cv::Rect rect(cv::Rect(cvRound(c.x - side * 0.5), cvRound(c.y - side * 0.5), cvRound(side),
                               cvRound(side)));
        rect &= cv::Rect(0, 0, bgrFrame.cols, bgrFrame.rows);
        if (rect.width < 8 || rect.height < 8)
            return r;

        cv::Mat crop, rgb;
        cv::resize(bgrFrame(rect), crop, cv::Size(kSize, kSize));
        cv::cvtColor(crop, rgb, cv::COLOR_BGR2RGB);

        // NHWC float blob [1, H, W, 3] in [0,1].
        int shape[4] = {1, kSize, kSize, 3};
        cv::Mat blob(4, shape, CV_32F);
        float *bd = reinterpret_cast<float *>(blob.data);
        const float inv = 1.0f / 255.0f;
        for (int yy = 0; yy < kSize; ++yy) {
            const uchar *row = rgb.ptr<uchar>(yy);
            for (int xx = 0; xx < kSize; ++xx) {
                const int o = (yy * kSize + xx) * 3;
                bd[o + 0] = row[xx * 3 + 0] * inv;
                bd[o + 1] = row[xx * 3 + 1] * inv;
                bd[o + 2] = row[xx * 3 + 2] * inv;
            }
        }

        cv::Mat out;
        try {
            net_.setInput(blob);
            out = net_.forward("Identity");
        } catch (const cv::Exception &) {
            return r;
        }
        if (out.empty() || out.total() < 478 * 3)
            return r;

        const float *d = reinterpret_cast<const float *>(out.data);
        const double sc = double(rect.width) / kSize;
        auto lmpx = [&](int i) {
            return cv::Point2f(float(rect.x + d[i * 3 + 0] * sc), float(rect.y + d[i * 3 + 1] * sc));
        };

        r.pts2d.resize(468);
        for (int i = 0; i < 468; ++i)
            r.pts2d[i] = lmpx(i);
        // MediaPipe indices (verified): 468/473 = right/left eye iris centre, 1 = nose tip,
        // 13/14 = inner lips.
        r.eyeRight = lmpx(468);
        r.eyeLeft = lmpx(473);
        r.nose = lmpx(1);
        r.mouth = (lmpx(13) + lmpx(14)) * 0.5f;
        r.mouthRight = lmpx(61);
        r.mouthLeft = lmpx(291);
        r.canonical = canonical_;
        r.valid = true;
        return r;
    }

  private:
    static constexpr int kSize = 256;
    cv::dnn::Net net_;
    std::vector<cv::Point3f> canonical_;
};

} // namespace opa
#endif // OPA_FACE_TRACKING
