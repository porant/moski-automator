// Verifies the JSON tab's message builder (src/json_builder.hpp) without OBS or a GUI.
// It both compares against the documented envelope and parses the result with Qt's JSON reader,
// so the messages the dock shows are guaranteed to be valid, copy-pasteable CallVendorRequests.
#include "json_builder.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>
#include <cstdio>
using namespace opa;

static int failures = 0;
static void check(bool ok, const QString &what, const QString &detail = QString()) {
    if (!ok)
        ++failures;
    std::printf("%-4s %s%s\n", ok ? "PASS" : "FAIL", qUtf8Printable(what),
                detail.isEmpty() ? "" : qUtf8Printable("   [" + detail + "]"));
}

static QString join(const QStringList &lines) { return lines.join('\n'); }

int main() {
    // Number formatting: integers stay integers, fractions do not gain float noise.
    check(jsonNum(300) == "300", "integer stays integer", jsonNum(300));
    check(jsonNum(0) == "0", "zero is 0", jsonNum(0));
    check(jsonNum(-1.3333) == "-1.3333", "fraction keeps its digits", jsonNum(-1.3333));
    check(jsonNum(0.3) == "0.3", "0.3 has no float noise", jsonNum(0.3));
    check(jsonEscape(QStringLiteral("a\"b\\c")) == "a\\\"b\\\\c", "quotes and backslashes escaped",
          jsonEscape(QStringLiteral("a\"b\\c")));

    // The example from docs/API.md: point1_magnitude, +0.3, 300 ms, EaseOutCubic, auto return.
    QStringList fields;
    fields << "\"source\": \"" + jsonEscape("VEBka") + "\"";
    fields << "\"filter\": \"" + jsonEscape("moska") + "\"";
    fields << "\"parameter\": \"point1_magnitude\"";
    fields << "\"value\": " + jsonNum(0.3);
    fields << "\"durationMs\": " + jsonNum(300);
    fields << "\"easing\": \"" + QString::fromLatin1("EaseOutCubic") + "\"";
    fields << "\"returnToZero\": " + QString("true");

    const QString expectedFull = join({
        "{",
        "  \"op\": 6,",
        "  \"d\": {",
        "    \"requestType\": \"CallVendorRequest\",",
        "    \"requestId\": \"point1-add\",",
        "    \"requestData\": {",
        "      \"vendorName\": \"obs-parameter-animator\",",
        "      \"requestType\": \"Add\",",
        "      \"requestData\": {",
        "        \"source\": \"VEBka\",",
        "        \"filter\": \"moska\",",
        "        \"parameter\": \"point1_magnitude\",",
        "        \"value\": 0.3,",
        "        \"durationMs\": 300,",
        "        \"easing\": \"EaseOutCubic\",",
        "        \"returnToZero\": true",
        "      }",
        "    }",
        "  }",
        "}",
    });
    const QString full = vendorMessage(true, "point1-add", "Add", fields);
    check(full == expectedFull, "full message matches the documented envelope");

    // The vendor-only ("d") flavour is the exact block Streamer.bot accepts: no op, no requestId.
    const QString expectedVendor = join({
        "{",
        "  \"requestType\": \"CallVendorRequest\",",
        "  \"requestData\": {",
        "    \"vendorName\": \"obs-parameter-animator\",",
        "    \"requestType\": \"Add\",",
        "    \"requestData\": {",
        "      \"source\": \"VEBka\",",
        "      \"filter\": \"moska\",",
        "      \"parameter\": \"point1_magnitude\",",
        "      \"value\": 0.3,",
        "      \"durationMs\": 300,",
        "      \"easing\": \"EaseOutCubic\",",
        "      \"returnToZero\": true",
        "    }",
        "  }",
        "}",
    });
    const QString vendor = vendorMessage(false, "point1-add", "Add", fields);
    check(vendor == expectedVendor, "vendor-only message is the exact Streamer.bot accepted block");
    check(!vendor.contains("\"op\"") && !vendor.contains("requestId"),
          "vendor-only message has no op / requestId wrapper");

    // Both flavours must be valid JSON with the expected nesting, whatever the indentation.
    for (auto pair : {std::make_pair(full, true), std::make_pair(vendor, false)}) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(pair.first.toUtf8(), &err);
        check(err.error == QJsonParseError::NoError, "message parses as JSON", err.errorString());
        if (err.error != QJsonParseError::NoError)
            continue;
        const QJsonObject d = pair.second ? doc.object().value("d").toObject() : doc.object();
        check(pair.second ? doc.object().value("op").toInt() == 6 : true, "op is 6");
        check(d.value("requestType").toString() == "CallVendorRequest", "requestType wrapper");
        const QJsonObject vendorBody = d.value("requestData").toObject();
        check(vendorBody.value("vendorName").toString() == "obs-parameter-animator", "vendorName");
        check(vendorBody.value("requestType").toString() == "Add", "vendor requestType");
        const QJsonObject inner = vendorBody.value("requestData").toObject();
        check(inner.value("parameter").toString() == "point1_magnitude", "parameter");
        check(inner.value("value").toDouble() == 0.3, "value");
        check(inner.value("durationMs").toDouble() == 300, "durationMs");
        check(inner.value("easing").toString() == "EaseOutCubic", "easing");
        check(inner.value("returnToZero").toBool(), "returnToZero");
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all JSON builder checks passed\n");
    return 0;
}
