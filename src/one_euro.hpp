#pragma once
// One-Euro filter (Casiez, Roussel & Vogel, CHI 2012).
//
// A low-latency, jitter-reducing filter for noisy real-time signals. Unlike a fixed low-pass it
// adapts its cutoff to the signal speed: slow motions are smoothed hard (little jitter) while fast
// motions let more signal through (little lag). Used here to stabilise the head-pose angles, whose
// per-detection estimates (roll/yaw/pitch from a handful of landmarks) are noisy.
//
// Header-only and dependency-free so it can be unit-tested without OpenCV/OBS.
#include <cmath>

namespace opa {

class OneEuroFilter {
  public:
    // freq: initial sample rate (Hz). minCutoff: base cutoff (Hz, lower = smoother).
    // beta: speed coefficient (higher = less lag on fast motion). dCutoff: cutoff of the derivative.
    OneEuroFilter(double freq = 30.0, double minCutoff = 1.5, double beta = 0.05,
                  double dCutoff = 1.0)
        : freq_(freq), minCutoff_(minCutoff), beta_(beta), dCutoff_(dCutoff) {}

    void reset() {
        have_ = false;
        xPrev_ = 0.0;
        dxPrev_ = 0.0;
    }

    // dt: time since the previous sample, in seconds (<=0 keeps the previous rate).
    double filter(double x, double dt) {
        if (dt > 0.0)
            freq_ = 1.0 / dt;
        if (!have_) {
            have_ = true;
            xPrev_ = x;
            dxPrev_ = 0.0;
            return x;
        }
        const double dx = (x - xPrev_) * freq_;                       // derivative (per second)
        const double edx = alpha(dCutoff_) * dx + (1.0 - alpha(dCutoff_)) * dxPrev_;
        const double cutoff = minCutoff_ + beta_ * std::fabs(edx);    // adapt to speed
        const double a = alpha(cutoff);
        const double xHat = a * x + (1.0 - a) * xPrev_;
        xPrev_ = xHat;
        dxPrev_ = edx;
        return xHat;
    }

  private:
    double alpha(double cutoff) const {
        const double tau = 1.0 / (2.0 * kPi * cutoff);
        const double te = 1.0 / freq_;
        return 1.0 / (1.0 + tau / te);
    }

    static constexpr double kPi = 3.14159265358979323846;
    double freq_, minCutoff_, beta_, dCutoff_;
    bool have_ = false;
    double xPrev_ = 0.0, dxPrev_ = 0.0;
};

} // namespace opa
