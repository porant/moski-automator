// Verifies the Import / Export preset text (src/preset.hpp) without OBS or a GUI.
// It round-trips a Preset through the exported JSON, checks the documented key layout, and makes
// sure clamping / forgiving parsing behave the way the dock's "Apply" button relies on.
#include "preset.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <cmath>
#include <cstdio>
using namespace opa;

static int failures = 0;
static void check(bool ok, const QString &what, const QString &detail = QString()) {
    if (!ok)
        ++failures;
    std::printf("%-4s %s%s\n", ok ? "PASS" : "FAIL", qUtf8Printable(what),
                detail.isEmpty() ? "" : qUtf8Printable("   [" + detail + "]"));
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    // --- export layout: flat object with the plugin's own setting keys ---
    Preset p;
    p.mode = 2;
    p.faceTracking = true;
    p.faceSmoothMs = 90;
    p.faceHoldMs = 1500;
    p.faceFadeMs = 800;
    p.effectBlur = true;
    p.faceBlurPx = 32;
    p.effectDebug = true;
    p.faceScale = false;
    p.defaultDurationMs = 750;
    p.defaultEasing = 7;
    p.points[0].enable = true;
    p.points[0].offsetX = -4.5;
    p.points[0].offsetY = 10;
    p.points[0].radius = 12.5;
    p.points[0].magnitude = 0.3;
    p.points[0].magMin = -1.5;
    p.points[0].magMax = 1.5;
    p.points[0].durationMs = 300;
    p.points[0].easing = 5;
    p.points[0].autoReturn = true;
    p.points[0].holdMs = 250;
    p.points[0].returnMs = 900;
    p.points[0].returnEasing = 6;
    p.points[1].enable = false;
    p.points[1].magnitude = -0.25;
    p.points[2].magnitude = 0.75;

    const QString text = presetToJson(p);
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &perr);
    check(perr.error == QJsonParseError::NoError, "exported text is valid JSON", perr.errorString());
    check(doc.isObject(), "exported text is a JSON object");
    const QJsonObject o = doc.object();
    check(o.value("mode").toInt() == 2, "mode exported");
    check(o.value("face_tracking").toBool(), "face_tracking exported");
    check(near(o.value("face_smooth_ms").toDouble(), 90), "face_smooth_ms exported");
    check(near(o.value("face_hold_ms").toDouble(), 1500), "face_hold_ms exported");
    check(near(o.value("face_fade_ms").toDouble(), 800), "face_fade_ms exported");
    check(o.value("effect_blur").toBool(), "effect_blur exported");
    check(near(o.value("face_blur_px").toDouble(), 32), "face_blur_px exported");
    check(o.value("effect_debug").toBool(), "effect_debug exported");
    check(!o.value("face_scale").toBool(), "face_scale exported");
    check(near(o.value("default_duration_ms").toDouble(), 750), "default_duration_ms exported");
    check(o.value("default_easing").toInt() == 7, "default_easing exported");
    check(o.value("point1_enable").toBool(), "point1_enable exported");
    check(near(o.value("point1_offset_x").toDouble(), -4.5), "point1_offset_x exported");
    check(near(o.value("point1_offset_y").toDouble(), 10), "point1_offset_y exported");
    check(near(o.value("point1_radius").toDouble(), 12.5), "point1_radius exported");
    check(near(o.value("point1_magnitude").toDouble(), 0.3), "point1_magnitude exported");
    check(near(o.value("point1_magnitude_min").toDouble(), -1.5), "point1_magnitude_min exported");
    check(near(o.value("point1_magnitude_max").toDouble(), 1.5), "point1_magnitude_max exported");
    check(near(o.value("point1_magnitude_duration_ms").toDouble(), 300),
          "point1_magnitude_duration_ms exported");
    check(o.value("point1_magnitude_easing").toInt() == 5, "point1_magnitude_easing exported");
    check(o.value("point1_magnitude_auto_return").toBool(), "point1_magnitude_auto_return exported");
    check(near(o.value("point1_magnitude_hold_ms").toDouble(), 250),
          "point1_magnitude_hold_ms exported");
    check(near(o.value("point1_magnitude_return_ms").toDouble(), 900),
          "point1_magnitude_return_ms exported");
    check(o.value("point1_magnitude_return_easing").toInt() == 6,
          "point1_magnitude_return_easing exported");
    check(!o.value("point2_enable").toBool(), "point2_enable exported");
    check(near(o.value("point3_magnitude").toDouble(), 0.75), "point3_magnitude exported");

    // --- round trip: export -> import gives back exactly what we started with ---
    Preset back;
    QString err = "unset";
    check(presetFromJson(text, back, &err), "exported text imports back", err);
    check(back.mode == p.mode && back.faceTracking == p.faceTracking &&
              near(back.faceSmoothMs, p.faceSmoothMs) && near(back.faceHoldMs, p.faceHoldMs) &&
              near(back.faceFadeMs, p.faceFadeMs) && back.effectBlur == p.effectBlur &&
              near(back.faceBlurPx, p.faceBlurPx) && back.effectDebug == p.effectDebug &&
              back.faceScale == p.faceScale && near(back.defaultDurationMs, p.defaultDurationMs) &&
              back.defaultEasing == p.defaultEasing,
          "round trip keeps the global settings");
    bool pointsOk = true;
    for (int i = 0; i < pointCount; ++i) {
        const auto &a = p.points[i];
        const auto &b = back.points[i];
        pointsOk = pointsOk && a.enable == b.enable && near(a.offsetX, b.offsetX) &&
                   near(a.offsetY, b.offsetY) && near(a.radius, b.radius) &&
                   near(a.magnitude, b.magnitude) && near(a.magMin, b.magMin) &&
                   near(a.magMax, b.magMax) && near(a.durationMs, b.durationMs) &&
                   a.easing == b.easing && a.autoReturn == b.autoReturn && near(a.holdMs, b.holdMs) &&
                   near(a.returnMs, b.returnMs) && a.returnEasing == b.returnEasing;
    }
    check(pointsOk, "round trip keeps every point's settings");

    // --- forgiving import: quoted numbers, numeric strings and a whole OBS message ---
    const QString quoted = QStringLiteral(
        "{\"mode\": \"1\", \"point1_magnitude\": \"0.4\", \"point1_radius\": 20,"
        " \"point1_magnitude_auto_return\": \"yes\"}");
    Preset q;
    check(presetFromJson(quoted, q, &err), "quoted / stringly-typed values import", err);
    check(q.mode == 1, "string mode parsed");
    check(near(q.points[0].magnitude, 0.4), "string magnitude parsed");
    check(near(q.points[0].radius, 20), "number radius parsed");
    check(q.points[0].autoReturn, "string boolean parsed");

    // The examples/six-zones-settings.json envelope must import too (d -> requestData -> filterSettings).
    const QString message = QStringLiteral(
        "{\"op\": 6, \"d\": {\"requestType\": \"SetSourceFilterSettings\", \"requestData\": {"
        "\"sourceName\": \"CAMERA\", \"filterName\": \"Distortion\", \"filterSettings\": {"
        "\"mode\": 2, \"point1_offset_y\": 10, \"point1_radius\": 12,"
        "\"point1_magnitude_target\": 0.3}}}}");
    Preset m;
    check(presetFromJson(message, m, &err), "OBS SetSourceFilterSettings envelope imports", err);
    check(m.mode == 2, "nested message mode parsed");
    check(near(m.points[0].offsetY, 10), "nested message offset parsed");
    check(near(m.points[0].radius, 12), "nested message radius parsed");


    // --- clamping: values outside the plugin's ranges are pulled back in ---
    const QString wild = QStringLiteral(
        "{\"mode\": 9, \"face_blur_px\": 500, \"point1_radius\": 900, \"point1_offset_x\": -500,"
        " \"point1_magnitude_min\": 2, \"point1_magnitude_max\": 1, \"point1_magnitude\": 5}");
    Preset w;
    check(presetFromJson(wild, w, &err), "out-of-range values still import", err);
    check(w.mode == 2, "mode clamped to 0..2");
    check(near(w.faceBlurPx, 128), "face_blur_px clamped");
    check(near(w.points[0].radius, 100), "radius clamped");
    check(near(w.points[0].offsetX, -100), "offset clamped");
    check(near(w.points[0].magMin, defaultMagnitudeMin) &&
              near(w.points[0].magMax, defaultMagnitudeMax),
          "inverted magnitude min/max falls back to the shader default");
    check(near(w.points[0].magnitude, defaultMagnitudeMax),
          "magnitude clamped into the (restored) range");

    // --- defaults: a preset with only a couple of keys keeps everything else ---
    Preset d;
    check(presetFromJson(QStringLiteral("{\"point2_magnitude\": 0.5}"), d, &err),
          "partial preset imports", err);
    check(near(d.points[1].magnitude, 0.5), "present key applied");
    check(d.mode == Preset().mode && near(d.points[0].radius, Preset().points[0].radius) &&
              d.points[0].enable,
          "missing keys keep their defaults");

    // --- error paths: the Apply button must be able to report a reason ---
    Preset bad;
    QString badErr;
    check(!presetFromJson(QStringLiteral("{ not json"), bad, &badErr), "invalid JSON is rejected");
    check(!badErr.isEmpty(), "invalid JSON reports a reason", badErr);
    check(!presetFromJson(QStringLiteral("[1, 2, 3]"), bad, &badErr), "non-object root is rejected");
    check(badErr.contains("object"), "non-object root reports a reason", badErr);
    check(!presetFromJson(QString(), bad, &badErr), "empty text is rejected");

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all preset import/export checks passed\n");
    return 0;
}

