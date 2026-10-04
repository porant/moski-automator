#include "animation.hpp"
#include <cstdlib>
#include <iostream>
using namespace opa;
static int checks = 0;
static void require(bool v, const char *message) {
    ++checks;
    if (!v) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
static bool near(double a, double b, double epsilon = 1e-8) {
    return std::abs(a - b) < epsilon;
}
int main() {
    for (int i = 0; i < 31; ++i) {
        require(near(Easing::apply(i, 0), 0), "easing starts at zero");
        require(near(Easing::apply(i, 1), 1), "easing ends at one");
        require(Easing::parse(easingNames[i]) == i, "easing round trip");
        for (int k = 0; k <= 100; ++k)
            require(std::isfinite(Easing::apply(i, k / 100.0)), "finite easing");
    }
    require(near(Easing::apply(5, .5), .875), "EaseOutCubic reference");
    require(Easing::apply(23, .7) > 1, "Back preserves intentional easing overshoot");
    for (int fps : {30, 60, 120}) {
        AnimatedParameter p;
        p.initialize(0, 0, 100);
        p.launch(100, 1, 0, 0, false);
        for (int frame = 0; frame <= fps; ++frame)
            p.sample(frame / (double)fps);
        require(near(p.state.currentValue, 100) && !p.state.active, "duration independent of FPS");
    }
    AnimatedParameter p;
    p.initialize(0, 0, 100);
    p.launch(100, 1, 0, 0, false);
    p.sample(.37);
    p.launch(0, 1, 0, .37, false);
    require(near(p.state.startValue, 37), "retarget from exact current");
    p.sample(.57);
    require(near(p.state.currentValue, 29.6), "retarget trajectory");
    p.launch(80, 1, 0, .57, false);
    require(near(p.state.startValue, 29.6), "second interruption continuity");
    p.sample(.9);
    const double current = p.state.currentValue;
    p.launch(0, 1, 5, .9, false);
    require(near(p.state.startValue, current), "third interruption continuity");
    p.sample(2);
    require(near(p.state.currentValue, 0), "final zero");
    p.initialize(0, -100, 200);
    p.launch(50, 1, 0, 0, false);
    p.add(30, 1, 0, .2, false);
    require(near(p.state.startValue, 10) && near(p.state.targetValue, 80), "accumulative ADD during attack");
    p.add(-10, 1, 0, .3, false);
    require(near(p.state.targetValue, 70), "signed ADD");
    p.initialize(0, -100, 200);
    p.returnDuration = 1;
    p.holdDuration = .2;
    p.returnValue = 0;
    p.returnEasing = 0;
    p.launch(100, 1, 0, 0, true);
    p.sample(1.1);
    require(p.state.phase == Phase::Hold, "hold stage");
    p.sample(1.7);
    require(near(p.state.currentValue, 50), "return uses absolute timeline");
    p.add(20, 1, 0, 1.7, true);
    require(near(p.state.targetValue, 70), "ADD during return accumulates from current");
    p.sample(10);
    require(!p.state.active && near(p.state.currentValue, 0), "long dropped-frame gap traverses all phases");
    p.launch(-50, 1, 0, 11, true);
    p.sample(12.7);
    require(near(p.state.currentValue, -25), "negative return");
    p.launch(100, 0, 0, 20, true);
    p.sample(22);
    require(near(p.state.currentValue, 0), "zero attack duration");
    p.launch(100, 1, 0, 25, true);
    p.stop(25.5);
    p.sample(30);
    require(near(p.state.currentValue, 50) && !p.state.active, "Stop freezes current");
    // Cumulative pulse: every ADD accumulates on top of the pending target, the return delay is
    // restarted by the last ADD and the value comes back to the rest value afterwards.
    AnimatedParameter q;
    q.initialize(0, -1.3333, 1.3333);
    q.returnDuration = .8;
    q.holdDuration = .2;
    q.returnValue = 0;
    q.returnEasing = 0;
    q.add(.3, .3, 0, 0, true);
    q.add(.3, .3, 0, .1, true);
    q.add(.3, .3, 0, .2, true);
    require(near(q.state.targetValue, .9), "ADD accumulates on top of the pending target");
    q.sample(.5);
    require(near(q.state.currentValue, .9) && q.state.phase == Phase::Hold,
            "cumulative ADD reaches the accumulated target and holds");
    q.sample(.6);
    require(near(q.state.currentValue, .9) && q.state.phase == Phase::Hold,
            "return delay is not cut short by the move");
    q.add(.3, .3, 0, .6, true);
    require(near(q.state.targetValue, 1.2), "ADD during the delay keeps accumulating");
    q.sample(1.0);
    require(near(q.state.currentValue, 1.2) && q.state.phase == Phase::Hold,
            "the return delay restarts after the last ADD");
    q.sample(2.0);
    require(!q.state.active && near(q.state.currentValue, 0),
            "the pulse returns to the rest value and stops");
    AnimatedParameter b;
    b.initialize(1, 0, 1, true);
    b.launch(0, 1, 0, 0, false);
    b.sample(.4);
    require(b.gpuValue() == 1, "bool before threshold");
    b.sample(.6);
    require(b.gpuValue() == 0, "bool after threshold");
    AnimationController c;
    for (int i = 0; i < pointCount * pointParamCount; ++i) {
        auto &v = c.at(i);
        v.launch(v.baseValue == v.maximum ? v.minimum : v.maximum, (i + 1) / 10.0, i % 31, 0, false);
    }
    c.sample(.05);
    require(c.activeCount() == pointCount * pointParamCount, "all point parameters run independently");
    c.sample(4);
    require(c.activeCount() == 0, "all parameters finish independently");
    // The filter uses this to skip all CPU math and the GPU pass while magnitudes are silent.
    AnimationController rest;
    require(rest.magnitudesAtRest(), "a fresh controller has every magnitude at rest");
    rest.at(4).launch(.5, .3, 0, 0, true);
    require(!rest.magnitudesAtRest(), "a running magnitude trajectory leaves rest");
    rest.sample(.1);
    require(!rest.magnitudesAtRest(), "a magnitude above zero leaves rest");
    rest.sample(2);
    require(rest.magnitudesAtRest() && !rest.at(4).state.active, "a returned magnitude is at rest");
    rest.at(4).launch(0, .3, 0, 0, false);
    require(rest.magnitudesAtRest(), "launching to zero keeps magnitude at rest");
    rest.at(pointCount * pointParamCount - 1).launch(.25, .3, 0, 0, false);
    require(!rest.magnitudesAtRest(), "any point leaving zero breaks rest");
    // Magnitude is the one parameter whose range is configurable per point; every stored value
    // (rest, target, return endpoint and a running value) follows the configured min/max.
    require(isMagnitude(magnitudeParam) && isMagnitude(pointParamCount + magnitudeParam) &&
                !isMagnitude(magnitudeParam + 1),
            "isMagnitude identifies the magnitude parameter");
    AnimationController ranges;
    auto &mag = ranges.at(magnitudeParam);
    require(near(mag.minimum, defaultMagnitudeMin) && near(mag.maximum, defaultMagnitudeMax),
            "magnitude starts at the default range");
    mag.setBounds(-0.5, 2.0);
    require(near(mag.minimum, -0.5) && near(mag.maximum, 2.0), "setBounds updates the range");
    mag.launch(5.0, 1, 0, 0, false);
    require(near(mag.state.targetValue, 2.0), "target clamps to the configured maximum");
    mag.sample(1);
    require(near(mag.state.currentValue, 2.0), "the value reaches the clamped maximum");
    mag.add(-10, 1, 0, 5, false);
    mag.sample(6);
    require(near(mag.state.currentValue, -0.5), "the value clamps to the configured minimum");
    mag.setBounds(0.0, 1.0);
    require(near(mag.state.currentValue, 0.0) && near(mag.baseValue, 0.0),
            "narrowing the range re-clamps the stored values");
    mag.setBounds(2.0, 1.0);
    require(near(mag.minimum, 0.0) && near(mag.maximum, 1.0), "an inverted range is rejected");
    require(near(mag.clamp(1e9), 1.0) && near(mag.clamp(-1e9), 0.0), "clamp honours the new bounds");
    bool rejected = false;
    try {
        p.launch(NAN, 1, 0, 50, false);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "reject NaN");
    std::cout << "PASS: " << checks << " checks\n";
    // CPU-only throughput probe, not a whole-OBS CPU measurement.
    c = AnimationController{};
    for (int i = 0; i < pointCount * pointParamCount; ++i) {
        auto &v = c.at(i);
        v.launch(v.baseValue == v.maximum ? v.minimum : v.maximum, 1, i % 31, 0, false);
    }
    const auto start = Clock::now();
    double total = 0;
    for (int pass = 0; pass < 100000; ++pass) {
        c.sample(.5);
        total += c.at(4).state.currentValue;
    }
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / 100000;
    std::cout << "30 active samples: " << us << " us/batch (host-specific), checksum " << total << '\n';
}
