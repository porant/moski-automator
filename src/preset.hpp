#pragma once
// Header-only import / export of the dock's automation settings as plain copy-pasteable JSON text.
// Kept free of OBS and Qt-widget dependencies (QtCore only, plus the pure animation.hpp constants)
// so it can be unit tested without OBS running, mirroring json_builder.hpp / zone_canvas.hpp.
#include "animation.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
namespace opa {
// One point's automation settings. The exported JSON keys mirror the plugin's OBS setting names, so
// the same text can also be pasted straight into a SetSourceFilterSettings "filterSettings" block.
struct PresetPoint {
    bool enable = true;
    double offsetX = 0, offsetY = 0, radius = 10, magnitude = 0;
    // Per-point magnitude clamp (same default as the shader / the dock's slider range).
    double magMin = defaultMagnitudeMin, magMax = defaultMagnitudeMax;
    double durationMs = -1; // -1 = use the shared default
    int easing = -1;        // -1 = use the shared default
    bool autoReturn = false;
    double holdMs = 200, returnMs = 1000;
    int returnEasing = 5;
};
// The whole editable state of the dock: playback mode, face tracking / overlay options and the
// three points' rest positions plus their magnitude timing.
struct Preset {
    int mode = 2;
    bool faceTracking = false;
    double faceSmoothMs = 120;
    bool effectBlur = false;
    double faceBlurPx = 24;
    bool effectDebug = false;
    bool faceScale = true;
    double defaultDurationMs = 1000;
    int defaultEasing = 5;
    std::array<PresetPoint, pointCount> points{};
};
namespace preset_detail {
// Forgiving readers: accept a number, a bool or a numeric string so a hand-edited preset (or one
// copied from another tool that quotes its numbers) still imports.
inline double number(const QJsonObject &o, const QString &key, double fallback) {
    const QJsonValue v = o.value(key);
    if (v.isDouble())
        return v.toDouble();
    if (v.isBool())
        return v.toBool() ? 1.0 : 0.0;
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        if (ok)
            return d;
    }
    return fallback;
}
inline bool boolean(const QJsonObject &o, const QString &key, bool fallback) {
    const QJsonValue v = o.value(key);
    if (v.isBool())
        return v.toBool();
    if (v.isDouble())
        return v.toDouble() != 0.0;
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s == "true" || s == "1" || s == "on" || s == "yes")
            return true;
        if (s == "false" || s == "0" || s == "off" || s == "no")
            return false;
    }
    return fallback;
}
inline int integer(const QJsonObject &o, const QString &key, int fallback) {
    const QJsonValue v = o.value(key);
    if (v.isDouble())
        return (int)std::lround(v.toDouble());
    if (v.isBool())
        return v.toBool() ? 1 : 0;
    if (v.isString()) {
        bool ok = false;
        const int i = v.toString().trimmed().toInt(&ok);
        if (ok)
            return i;
    }
    return fallback;
}
// The exported text is a flat object, but users often paste a whole OBS message instead (for
// instance examples/six-zones-settings.json). Follow d -> requestData -> filterSettings so those
// also import, then read the flat keys from whatever object we end up with.
inline QJsonObject settingsObject(const QJsonObject &root) {
    QJsonObject o = root;
    for (const char *key : {"d", "requestData", "filterSettings"}) {
        const QJsonValue v = o.value(QString::fromLatin1(key));
        if (v.isObject())
            o = v.toObject();
    }
    return o;
}
inline QString pointKey(int point) { return QStringLiteral("point%1_").arg(point + 1); }
} // namespace preset_detail
// Serialise the preset to indented JSON. Integers stay integers, fractions keep their digits.
inline QString presetToJson(const Preset &p) {
    QJsonObject o;
    o.insert(QStringLiteral("mode"), p.mode);
    o.insert(QStringLiteral("face_tracking"), p.faceTracking);
    o.insert(QStringLiteral("face_smooth_ms"), p.faceSmoothMs);
    o.insert(QStringLiteral("effect_blur"), p.effectBlur);
    o.insert(QStringLiteral("face_blur_px"), p.faceBlurPx);
    o.insert(QStringLiteral("effect_debug"), p.effectDebug);
    o.insert(QStringLiteral("face_scale"), p.faceScale);
    o.insert(QStringLiteral("default_duration_ms"), p.defaultDurationMs);
    o.insert(QStringLiteral("default_easing"), p.defaultEasing);
    for (int i = 0; i < pointCount; ++i) {
        const auto &pt = p.points[i];
        const QString k = preset_detail::pointKey(i);
        o.insert(k + QStringLiteral("enable"), pt.enable);
        o.insert(k + QStringLiteral("offset_x"), pt.offsetX);
        o.insert(k + QStringLiteral("offset_y"), pt.offsetY);
        o.insert(k + QStringLiteral("radius"), pt.radius);
        o.insert(k + QStringLiteral("magnitude"), pt.magnitude);
        o.insert(k + QStringLiteral("magnitude_min"), pt.magMin);
        o.insert(k + QStringLiteral("magnitude_max"), pt.magMax);
        o.insert(k + QStringLiteral("magnitude_duration_ms"), pt.durationMs);
        o.insert(k + QStringLiteral("magnitude_easing"), pt.easing);
        o.insert(k + QStringLiteral("magnitude_auto_return"), pt.autoReturn);
        o.insert(k + QStringLiteral("magnitude_hold_ms"), pt.holdMs);
        o.insert(k + QStringLiteral("magnitude_return_ms"), pt.returnMs);
        o.insert(k + QStringLiteral("magnitude_return_easing"), pt.returnEasing);
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
}
// Parse a pasted preset. Missing keys keep the Preset defaults, out-of-range numbers are clamped to
// the same limits the plugin enforces, and an invalid document returns false with a readable reason.
inline bool presetFromJson(const QString &text, Preset &out, QString *error) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("Invalid JSON: %1 (at offset %2)")
                         .arg(err.errorString())
                         .arg(err.offset);
        return false;
    }
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("Expected a JSON object with the automation settings.");
        return false;
    }
    const QJsonObject s = preset_detail::settingsObject(doc.object());
    Preset r;
    r.mode = std::clamp(preset_detail::integer(s, QStringLiteral("mode"), r.mode), 0, 2);
    r.faceTracking = preset_detail::boolean(s, QStringLiteral("face_tracking"), r.faceTracking);
    r.faceSmoothMs =
        std::clamp(preset_detail::number(s, QStringLiteral("face_smooth_ms"), r.faceSmoothMs), 0.0,
                   2000.0);
    r.effectBlur = preset_detail::boolean(s, QStringLiteral("effect_blur"), r.effectBlur);
    r.faceBlurPx =
        std::clamp(preset_detail::number(s, QStringLiteral("face_blur_px"), r.faceBlurPx), 2.0, 128.0);
    r.effectDebug = preset_detail::boolean(s, QStringLiteral("effect_debug"), r.effectDebug);
    r.faceScale = preset_detail::boolean(s, QStringLiteral("face_scale"), r.faceScale);
    r.defaultDurationMs = std::clamp(
        preset_detail::number(s, QStringLiteral("default_duration_ms"), r.defaultDurationMs), 0.0,
        3600000.0);
    r.defaultEasing = std::clamp(
        preset_detail::integer(s, QStringLiteral("default_easing"), r.defaultEasing), 0, 30);
    for (int i = 0; i < pointCount; ++i) {
        auto &pt = r.points[i];
        const QString k = preset_detail::pointKey(i);
        pt.enable = preset_detail::boolean(s, k + QStringLiteral("enable"), pt.enable);
        pt.offsetX = std::clamp(preset_detail::number(s, k + QStringLiteral("offset_x"), pt.offsetX),
                                -100.0, 100.0);
        pt.offsetY = std::clamp(preset_detail::number(s, k + QStringLiteral("offset_y"), pt.offsetY),
                                -100.0, 100.0);
        pt.radius =
            std::clamp(preset_detail::number(s, k + QStringLiteral("radius"), pt.radius), 0.0, 100.0);
        pt.magMin = preset_detail::number(s, k + QStringLiteral("magnitude_min"), pt.magMin);
        pt.magMax = preset_detail::number(s, k + QStringLiteral("magnitude_max"), pt.magMax);
        if (!(pt.magMax > pt.magMin)) {
            pt.magMin = defaultMagnitudeMin;
            pt.magMax = defaultMagnitudeMax;
        }
        pt.magnitude =
            std::clamp(preset_detail::number(s, k + QStringLiteral("magnitude"), pt.magnitude),
                       pt.magMin, pt.magMax);
        pt.durationMs = std::clamp(
            preset_detail::number(s, k + QStringLiteral("magnitude_duration_ms"), pt.durationMs), -1.0,
            3600000.0);
        pt.easing = std::clamp(
            preset_detail::integer(s, k + QStringLiteral("magnitude_easing"), pt.easing), -1, 30);
        pt.autoReturn =
            preset_detail::boolean(s, k + QStringLiteral("magnitude_auto_return"), pt.autoReturn);
        pt.holdMs = std::clamp(
            preset_detail::number(s, k + QStringLiteral("magnitude_hold_ms"), pt.holdMs), 0.0,
            3600000.0);
        pt.returnMs = std::clamp(
            preset_detail::number(s, k + QStringLiteral("magnitude_return_ms"), pt.returnMs), 0.0,
            3600000.0);
        pt.returnEasing = std::clamp(
            preset_detail::integer(s, k + QStringLiteral("magnitude_return_easing"), pt.returnEasing),
            0, 30);
    }
    out = r;
    if (error)
        error->clear();
    return true;
}
} // namespace opa

