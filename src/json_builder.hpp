#pragma once
// Header-only builder for the copy-paste CallVendorRequest messages shown in the dock's JSON tab.
// Kept free of OBS/Qt-widget dependencies (only QtCore QString/QStringList) so it can be unit
// tested without OBS running, mirroring how zone_canvas.hpp is exercised by canvas-preview.
#include <QString>
#include <QStringList>
#include <cmath>
namespace opa {
// Escape a string so it can sit inside JSON double quotes.
inline QString jsonEscape(const QString &text) {
    QString out = text;
    out.replace('\\', "\\\\");
    out.replace('"', "\\\"");
    return out;
}
// Whole numbers stay integer-like (300, -1), fractions keep up to 10 significant digits so a
// value such as 0.3 does not serialise as 0.30000000000000004.
inline QString jsonNum(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
        return QString::number(static_cast<qlonglong>(v));
    return QString::number(v, 'g', 10);
}
// Join already-formatted "key": value field lines, each indented by `indent` spaces.
inline QString jsonFieldLines(const QStringList &fields, int indent) {
    const QString pad(indent, ' ');
    QString out;
    for (int i = 0; i < fields.size(); ++i) {
        out += pad + fields.at(i);
        out += (i + 1 < fields.size()) ? ",\n" : "\n";
    }
    return out;
}
// Wrap the innermost requestData fields into either the whole op:6 message or only the
// CallVendorRequest ("d") object for clients such as Streamer.bot.
inline QString vendorMessage(bool full, const QString &requestId, const QString &vendorType,
                             const QStringList &fields) {
    const QString inner = jsonFieldLines(fields, full ? 8 : 6);
    if (full)
        return "{\n  \"op\": 6,\n  \"d\": {\n    \"requestType\": \"CallVendorRequest\",\n"
               "    \"requestId\": \"" + requestId + "\",\n    \"requestData\": {\n"
               "      \"vendorName\": \"obs-parameter-animator\",\n"
               "      \"requestType\": \"" + vendorType + "\",\n      \"requestData\": {\n" +
               inner + "      }\n    }\n  }\n}";
    // Vendor request only: the inner "d" object WITHOUT op/requestId - exactly the shape
    // Streamer.bot accepts as a custom vendor action body (its "CallVendorRequest" payload).
    return "{\n  \"requestType\": \"CallVendorRequest\",\n  \"requestData\": {\n"
           "    \"vendorName\": \"obs-parameter-animator\",\n"
           "    \"requestType\": \"" + vendorType + "\",\n    \"requestData\": {\n" + inner +
           "    }\n  }\n}";
}
} // namespace opa
