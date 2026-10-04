#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace opa {
using Clock = std::chrono::steady_clock;
inline double nowSeconds() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
constexpr double pi = 3.14159265358979323846;
inline const std::array<const char *, 31> easingNames = {
    "Linear",         "EaseInQuad",    "EaseOutQuad",    "EaseInOutQuad",    "EaseInCubic",  "EaseOutCubic",
    "EaseInOutCubic", "EaseInQuart",   "EaseOutQuart",   "EaseInOutQuart",   "EaseInQuint",  "EaseOutQuint",
    "EaseInOutQuint", "EaseInSine",    "EaseOutSine",    "EaseInOutSine",    "EaseInExpo",   "EaseOutExpo",
    "EaseInOutExpo",  "EaseInCirc",    "EaseOutCirc",    "EaseInOutCirc",    "EaseInBack",   "EaseOutBack",
    "EaseInOutBack",  "EaseInElastic", "EaseOutElastic", "EaseInOutElastic", "EaseInBounce", "EaseOutBounce",
    "EaseInOutBounce"};
inline double bounceOut(double t) {
    constexpr double n = 7.5625, d = 2.75;
    if (t < 1 / d)
        return n * t * t;
    if (t < 2 / d) {
        t -= 1.5 / d;
        return n * t * t + .75;
    }
    if (t < 2.5 / d) {
        t -= 2.25 / d;
        return n * t * t + .9375;
    }
    t -= 2.625 / d;
    return n * t * t + .984375;
}
struct Easing {
    static int parse(const std::string &name) {
        for (int i = 0; i < 31; ++i)
            if (name == easingNames[i])
                return i;
        throw std::invalid_argument("Unknown easing: " + name);
    }
    static double apply(int type, double x) {
        x = std::clamp(x, 0.0, 1.0);
        if (x == 0 || x == 1 || type == 0)
            return x;
        if (type < 0 || type >= 31)
            throw std::invalid_argument("Invalid easing ID");
        const int family = (type - 1) / 3, direction = (type - 1) % 3;
        auto in = [family](double t) {
            if (family <= 3)
                return std::pow(t, family + 2);
            switch (family) {
            case 4:
                return 1 - std::cos(t * pi / 2);
            case 5:
                return t == 0 ? 0.0 : std::pow(2.0, 10 * t - 10);
            case 6:
                return 1 - std::sqrt(std::max(0.0, 1 - t * t));
            case 7:
                return 2.70158 * t * t * t - 1.70158 * t * t;
            case 8:
                return t == 0 || t == 1
                           ? t
                           : -std::pow(2.0, 10 * t - 10) * std::sin((10 * t - 10.75) * (2 * pi / 3));
            default:
                return 1 - bounceOut(1 - t);
            }
        };
        // Standard Back/Elastic InOut constants differ from piecewise In/Out.
        if (direction == 2 && family == 7) {
            constexpr double c = 1.70158 * 1.525;
            return x < .5 ? std::pow(2 * x, 2) * ((c + 1) * 2 * x - c) / 2
                          : (std::pow(2 * x - 2, 2) * ((c + 1) * (2 * x - 2) + c) + 2) / 2;
        }
        if (direction == 2 && family == 8) {
            const double c = 2 * pi / 4.5;
            return x < .5 ? -std::pow(2.0, 20 * x - 10) * std::sin((20 * x - 11.125) * c) / 2
                          : std::pow(2.0, -20 * x + 10) * std::sin((20 * x - 11.125) * c) / 2 + 1;
        }
        if (direction == 0)
            return in(x);
        if (direction == 1)
            return 1 - in(1 - x);
        return x < .5 ? in(2 * x) / 2 : 1 - in(2 - 2 * x) / 2;
    }
};
enum class Phase { Idle, Attack, Hold, Return };
inline const char *phaseName(Phase p) {
    switch (p) {
    case Phase::Attack:
        return "ANIMATE";
    case Phase::Hold:
        return "HOLD";
    case Phase::Return:
        return "RETURN";
    default:
        return "IDLE";
    }
}
struct AnimationState {
    double startValue = 0, currentValue = 0, targetValue = 0, startTime = 0, duration = 1, progress = 0;
    int easingType = 5;
    bool active = false;
    Phase phase = Phase::Idle;
};
struct AnimatedParameter {
    AnimationState state;
    double minimum = 0, maximum = 100, baseValue = 0, configuredTarget = 0;
    double configuredDuration = 1, returnDuration = 1, holdDuration = 0, returnValue = 0;
    int configuredEasing = 5, returnEasing = 5;
    bool autoReturn = false, configuredAutoReturn = false;
    double scheduledReturnDuration = 1, scheduledHoldDuration = 0, scheduledReturnValue = 0;
    int scheduledReturnEasing = 5;
    bool boolean = false;
    double clamp(double v) const {
        return std::clamp(v, minimum, maximum);
    }
    void initialize(double base, double lo, double hi, bool isBool = false) {
        baseValue = base;
        minimum = lo;
        maximum = hi;
        boolean = isBool;
        state.currentValue = state.startValue = state.targetValue = base;
        configuredTarget = base;
        returnValue = base;
    }
    // Re-target the allowed range at runtime. Magnitude is the only parameter whose per-point
    // min/max is user configurable, so every stored value is re-clamped to keep the parameter
    // (rest value, pending target, return endpoint and a running trajectory) inside the new range.
    // An inverted or non-finite range is rejected, leaving the current bounds untouched.
    void setBounds(double lo, double hi) {
        if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo)
            return;
        minimum = lo;
        maximum = hi;
        baseValue = clamp(baseValue);
        configuredTarget = clamp(configuredTarget);
        returnValue = clamp(returnValue);
        scheduledReturnValue = clamp(scheduledReturnValue);
        state.startValue = clamp(state.startValue);
        state.currentValue = clamp(state.currentValue);
        state.targetValue = clamp(state.targetValue);
    }
    // Absolute stage timestamps make long frame gaps traverse attack/hold/return correctly.
    void sample(double time) {
        for (int transitions = 0; state.active && transitions < 3; ++transitions) {
            if (state.phase == Phase::Hold) {
                if (time < state.startTime + scheduledHoldDuration)
                    return;
                state.startTime += scheduledHoldDuration;
                state.startValue = state.currentValue;
                state.targetValue = scheduledReturnValue;
                state.duration = scheduledReturnDuration;
                state.easingType = scheduledReturnEasing;
                state.phase = Phase::Return;
            }
            state.progress =
                state.duration <= 0 ? 1 : std::clamp((time - state.startTime) / state.duration, 0.0, 1.0);
            state.currentValue =
                clamp(state.startValue + (state.targetValue - state.startValue) *
                                             Easing::apply(state.easingType, state.progress));
            if (state.progress < 1)
                return;
            state.currentValue = state.targetValue;
            if (state.phase == Phase::Attack && autoReturn && state.targetValue != scheduledReturnValue) {
                state.startTime += state.duration;
                state.phase = Phase::Hold;
            } else {
                state.active = false;
                state.phase = Phase::Idle;
            }
        }
    }
    void launch(double target, double seconds, int easing, double time, bool returnAfter) {
        if (!std::isfinite(target) || !std::isfinite(seconds) || seconds < 0 || seconds > 3600 ||
            easing < 0 || easing >= 31)
            throw std::invalid_argument("Invalid target/duration/easing");
        sample(time); // Evaluate interrupted trajectory at the exact command time.
        autoReturn = returnAfter;
        scheduledReturnDuration = returnDuration;
        scheduledHoldDuration = holdDuration;
        scheduledReturnValue = returnValue;
        scheduledReturnEasing = returnEasing;
        state.startValue = state.currentValue;
        state.targetValue = clamp(target);
        state.startTime = time;
        state.duration = seconds;
        state.easingType = easing;
        state.active = true;
        state.phase = Phase::Attack;
        state.progress = 0;
        if (state.startValue == state.targetValue &&
            (!returnAfter || state.targetValue == scheduledReturnValue)) {
            state.active = false;
            state.phase = Phase::Idle;
            state.progress = 1;
        }
        sample(time);
    }
    void add(double delta, double seconds, int easing, double time, bool returnAfter) {
        if (!std::isfinite(delta))
            throw std::invalid_argument("Non-finite delta");
        sample(time);
        // Accumulate pending target during attack/hold, but add to current during decay.
        const double basis =
            state.active && state.phase != Phase::Return ? state.targetValue : state.currentValue;
        launch(clamp(basis + delta), seconds, easing, time, returnAfter);
    }
    void stop(double time) {
        sample(time);
        state.active = false;
        state.phase = Phase::Idle;
        state.targetValue = state.currentValue;
    }
    double gpuValue() const {
        return boolean ? (state.currentValue >= .5 ? 1.0 : 0.0) : state.currentValue;
    }
};
// The plugin animates three "points" that are drawn on every detected face, anchored on the YuNet
// landmarks: 0 = eyes (midpoint), 1 = nose tip, 2 = mouth (midpoint). Each point keeps five
// parameters: enable (bool), offset_x, offset_y (percent, added to the detected anchor), radius
// (percent) and magnitude.
constexpr int pointCount = 3;
constexpr int pointParamCount = 5;
constexpr int paramCount = pointCount * pointParamCount;
// Index of "magnitude" within one point's five parameters. Its min/max is the one range the user
// can narrow or widen per point (settings keys pointN_magnitude_min / pointN_magnitude_max).
constexpr int magnitudeParam = 4;
constexpr double defaultMagnitudeMin = -1.3333, defaultMagnitudeMax = 1.3333;
inline bool isMagnitude(int index) {
    return index % pointParamCount == magnitudeParam;
}
struct ZoneController {
    std::array<AnimatedParameter, pointParamCount> parameters;
};
struct AnimationController {
    std::array<ZoneController, pointCount> zones;
    AnimationController() {
        for (int z = 0; z < pointCount; ++z) {
            auto &p = zones[z].parameters;
            p[0].initialize(1, 0, 1, true);       // enable
            p[1].initialize(0, -100, 100);        // offset_x
            p[2].initialize(0, -100, 100);        // offset_y
            p[3].initialize(10, 0, 100);          // radius
            p[4].initialize(0, defaultMagnitudeMin, defaultMagnitudeMax); // magnitude
            p[4].autoReturn = p[4].configuredAutoReturn = true;
        }
    }
    AnimatedParameter &at(int i) {
        return zones.at(i / 5).parameters.at(i % 5);
    }
    const AnimatedParameter &at(int i) const {
        return zones.at(i / 5).parameters.at(i % 5);
    }
    void sample(double t) {
        for (auto &z : zones)
            for (auto &p : z.parameters)
                if (p.state.active)
                    p.sample(t);
    }
    int activeCount() const {
        int n = 0;
        for (const auto &z : zones)
            for (const auto &p : z.parameters)
                n += p.state.active;
        return n;
    }
    // True while every magnitude is at its rest value (0) and no magnitude trajectory is
    // running. At rest the original shader is a plain texture copy, so the filter can skip
    // sampling, uniform uploads and the GPU pass entirely.
    bool magnitudesAtRest() const {
        for (const auto &z : zones) {
            const auto &m = z.parameters[4];
            if (m.state.active || m.gpuValue() != 0.0)
                return false;
        }
        return true;
    }
};
inline std::string parameterName(int index) {
    constexpr const char *suffix[] = {"enable", "offset_x", "offset_y", "radius", "magnitude"};
    return "point" + std::to_string(index / pointParamCount + 1) + "_" +
           suffix[index % pointParamCount];
}
inline int parameterIndex(const std::string &name) {
    for (int i = 0; i < pointCount * pointParamCount; ++i)
        if (name == parameterName(i))
            return i;
    return -1;
}
// Human-readable name of a point (0 eyes, 1 nose, 2 mouth).
inline const char *pointName(int point) {
    constexpr const char *names[pointCount] = {"Forehead", "Nose", "Mouth"};
    return (point >= 0 && point < pointCount) ? names[point] : "?";
}
} // namespace opa
