// Renders the ZoneCanvas visual editor to PNG files.
// Used to review the panel layout without launching OBS.
//   canvas-preview [output-directory]
#include "zone_canvas.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QWheelEvent>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace opa;

static int failures = 0;
static void check(bool ok, const QString &what, const QString &detail = QString()) {
    if (!ok)
        ++failures;
    std::printf("%-4s %s%s\n", ok ? "PASS" : "FAIL", qUtf8Printable(what),
                detail.isEmpty() ? "" : qUtf8Printable("   [" + detail + "]"));
}

static void sendMouse(QWidget &w, QEvent::Type type, const QPointF &p, Qt::MouseButton button,
                      Qt::MouseButtons buttons) {
    QMouseEvent ev(type, p, p, p, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&w, &ev);
}

static void sendWheel(QWidget &w, const QPointF &p, int delta, Qt::KeyboardModifiers mods) {
    QWheelEvent ev(p, p, QPoint(), QPoint(0, delta), Qt::NoButton, mods, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&w, &ev);
}

// Drives the real event handlers so the interactive behaviour is regression tested.
static int selftest() {
    ZoneCanvas c;
    c.resize(760, 430);
    std::array<ZoneView, ZoneCanvas::kZones> values{};
    for (int z = 0; z < ZoneCanvas::kZones; ++z)
        values[z] = {true, 10.0 + z * 16.0, 30.0, 5.0, 0.0};
    c.setValues(values);
    c.setSelected(0);

    const QRectF r = c.frameRect();
    check(r.width() > 0 && std::fabs(r.width() / r.height() - 16.0 / 9.0) < 1e-9, "frame is 16:9",
          QString("w=%1 h=%2").arg(r.width()).arg(r.height()));

    // 1. dragging the centre moves the zone and leaves the radius alone
    const QPointF from = c.toPix(r, values[0].cx, values[0].cy);
    const QPointF to = c.toPix(r, values[0].cx + 8, values[0].cy + 30);
    sendMouse(c, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i)
        sendMouse(c, QEvent::MouseMove, from + (to - from) * (i / 8.0), Qt::NoButton,
                  Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    check(std::fabs(c.values()[0].cx - 18.0) < 0.6 && std::fabs(c.values()[0].cy - 60.0) < 0.6,
          "drag moves the centre", QString("cx=%1 cy=%2").arg(c.values()[0].cx).arg(c.values()[0].cy));
    check(std::fabs(c.values()[0].radius - 5.0) < 1e-9, "drag does not resize",
          QString("radius=%1").arg(c.values()[0].radius));

    // 2. dragging the rim resizes instead of moving
    const QPointF c1 = c.toPix(r, c.values()[1].cx, c.values()[1].cy);
    const QPointF rim(c1.x() + c.radiusPix(r, c.values()[1].radius), c1.y());
    const QPointF rim2 = rim + QPointF(c.radiusPix(r, 15.0), 0);
    sendMouse(c, QEvent::MouseButtonPress, rim, Qt::LeftButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseMove, rim2, Qt::NoButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, rim2, Qt::LeftButton, Qt::NoButton);
    check(std::fabs(c.values()[1].radius - 20.0) < 1.0, "edge drag resizes",
          QString("radius=%1").arg(c.values()[1].radius));
    check(std::fabs(c.values()[1].cx - 26.0) < 1e-9, "resize keeps the centre");

    // 3. wheel = magnitude of the hovered zone
    QWheelEvent we(c.toPix(r, c.values()[2].cx, c.values()[2].cy),
                   c.toPix(r, c.values()[2].cx, c.values()[2].cy), QPoint(), QPoint(0, 120),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&c, &we);
    check(std::fabs(c.values()[2].magnitude - 0.05) < 1e-9, "wheel raises magnitude",
          QString("mag=%1").arg(c.values()[2].magnitude));

    // 4. shift+wheel = radius
    sendWheel(c, c.toPix(r, c.values()[2].cx, c.values()[2].cy), 120, Qt::ShiftModifier);
    check(std::fabs(c.values()[2].radius - 6.0) < 1e-9, "shift+wheel changes radius",
          QString("radius=%1").arg(c.values()[2].radius));

    // 5. double click toggles enabled
    sendMouse(c, QEvent::MouseButtonDblClick, c.toPix(r, c.values()[3].cx, c.values()[3].cy),
              Qt::LeftButton, Qt::LeftButton);
    check(!c.values()[3].enabled, "double click toggles the zone off");

    // 6. clicking a zone selects it
    sendMouse(c, QEvent::MouseButtonPress, c.toPix(r, c.values()[4].cx, c.values()[4].cy),
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, c.toPix(r, c.values()[4].cx, c.values()[4].cy),
              Qt::LeftButton, Qt::NoButton);
    check(c.selected() == 4, "click selects the zone", QString("selected=%1").arg(c.selected()));

    // 7. the live overlay is display-only: it never rewrites the stored rest position
    const double magBefore = c.values()[0].magnitude;
    const double cxBefore = c.values()[0].cx;
    auto live = c.values();
    live[0].magnitude = 1.25;
    live[0].cx = 90;
    std::array<bool, ZoneCanvas::kZones> liveOn{};
    liveOn[0] = true;
    c.setLive(live, liveOn);
    check(std::fabs(c.values()[0].magnitude - magBefore) < 1e-9 &&
              std::fabs(c.values()[0].cx - cxBefore) < 1e-9,
          "live overlay never rewrites stored values");
    // Repeating an identical live state must not repaint (the dock polls the engine at 10 Hz).
    c.setLive(live, liveOn);
    c.setLive(live, liveOn);
    check(std::fabs(c.values()[0].magnitude - magBefore) < 1e-9, "identical live updates stay stable");

    // 8. clamping
    c.setCenter(5, -50, 500);
    check(c.values()[5].cx == 0.0 && c.values()[5].cy == 100.0, "centre clamps to 0..100");
    c.setMagnitude(5, 9);
    check(std::fabs(c.values()[5].magnitude - ZoneCanvas::kMagMax) < 1e-9, "magnitude clamps");
    c.setRadius(5, -3);
    check(c.values()[5].radius == 0.0, "radius clamps at 0");

    // 9. the live source frame is painted inside the 16:9 frame, behind the zones
    QImage frame(48, 27, QImage::Format_RGBA8888);
    frame.fill(QColor(200, 40, 160));
    c.setSourceFrame(frame);
    const QImage painted = c.grab().toImage();
    // Probe a spot inside the frame that is clear of zones, grid lines and labels.
    const QPoint probe((int)(r.left() + r.width() * 0.35), (int)(r.top() + r.height() * 0.88));
    const QColor px = painted.pixelColor(probe);
    check(std::abs(px.red() - 200) < 6 && std::abs(px.green() - 40) < 6 && std::abs(px.blue() - 160) < 6,
          "source frame is painted behind the zones",
          QString("rgb=%1,%2,%3").arg(px.red()).arg(px.green()).arg(px.blue()));

    // 10. a non-16:9 source keeps its true proportions instead of being stretched
    c.setAspectRatio(4.0 / 3.0);
    check(std::fabs(c.aspectRatio() - 4.0 / 3.0) < 1e-9, "explicit aspect ratio is stored");
    const QRectF r43 = c.frameRect();
    check(std::fabs(r43.width() / r43.height() - 4.0 / 3.0) < 1e-9,
          "frameRect follows the source aspect ratio",
          QString("w=%1 h=%2").arg(r43.width()).arg(r43.height()));
    // The shader ties the zone radius to the frame height, so a zone stays a circle on any aspect.
    check(std::fabs(c.radiusPix(r43, 10.0) - r43.height() * 0.1) < 1e-9,
          "zone radius scales with frame height");
    // A captured 4:3 frame updates the aspect ratio from its real pixel dimensions.
    QImage tall(40, 30, QImage::Format_RGBA8888);
    tall.fill(QColor(10, 20, 30));
    c.setSourceFrame(tall);
    check(std::fabs(c.aspectRatio() - 4.0 / 3.0) < 1e-9, "captured frame sets the aspect ratio");

    std::printf("\n%s (%d failure(s))\n", failures ? "SELFTEST FAILED" : "SELFTEST OK", failures);
    return failures ? 1 : 0;
}

static std::array<ZoneView, ZoneCanvas::kZones> originalBase() {
    const double xs[6] = {25, 50, 75, 25, 50, 75};
    const double ys[6] = {25, 25, 10, 75, 75, 10};
    std::array<ZoneView, ZoneCanvas::kZones> b{};
    for (int z = 0; z < ZoneCanvas::kZones; ++z) {
        b[z].enabled = true;
        b[z].cx = xs[z];
        b[z].cy = ys[z];
        b[z].radius = 10;
        b[z].magnitude = 0;
    }
    return b;
}

static bool save(QWidget &w, const QString &path) {
    const QImage img = w.grab().toImage();
    const bool ok = img.save(path);
    std::printf("%-10s %s  (%dx%d)\n", ok ? "wrote" : "FAILED", qUtf8Printable(path), img.width(),
                img.height());
    return ok;
}

int main(int argc, char **argv) {
    bool selfTest = false;
    QString outDir = QDir::currentPath();
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--selftest")
            selfTest = true;
        else if (a == "--qt-plugins" && i + 1 < argc)
            qputenv("QT_PLUGIN_PATH", argv[++i]);
        else if (!a.startsWith("--"))
            outDir = a;
    }
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (selfTest)
        return selftest();
    QDir().mkpath(outDir);

    auto values = originalBase();
    // A plausible "six zones at once" configuration from the README. These are the rest positions:
    // each zone animates away from them and returns to them.
    values[1].magnitude = 0.2;
    values[2].magnitude = 0.2;
    values[3].magnitude = 0.2;
    values[5].magnitude = 0.2;
    values[5].enabled = false; // show the "OFF" state

    ZoneCanvas c;
    c.resize(760, 430);

    c.setValues(values);
    c.setSelected(0);
    save(c, QDir(outDir).filePath("canvas_rest.png"));

    c.setSelected(2);
    save(c, QDir(outDir).filePath("canvas_zone3.png"));

    // live overlay: zones 1 and 5 are mid-flight between their rest position and the animated value
    auto live = values;
    std::array<bool, ZoneCanvas::kZones> active{};
    live[0].magnitude = 0.62;
    live[0].radius = 13;
    live[0].cx = 27;
    live[0].cy = 22;
    active[0] = true;
    live[4].magnitude = -0.4;
    live[4].radius = 16;
    active[4] = true;
    c.setSelected(0);
    c.setLive(live, active);
    save(c, QDir(outDir).filePath("canvas_live.png"));

    // non-16:9 source: the preview must keep the real proportions instead of stretching to 16:9
    QImage portrait(240, 426, QImage::Format_RGBA8888); // ~9:16
    portrait.fill(QColor(30, 60, 90));
    auto portraitValues = values;
    portraitValues[0].cx = 50;
    portraitValues[0].cy = 50;
    c.setValues(portraitValues);
    c.setSelected(0);
    c.setSourceFrame(portrait);
    // The panel follows the source proportions: resize by width and let the height follow.
    c.resize(760, c.heightForWidth(760));
    save(c, QDir(outDir).filePath("canvas_portrait.png"));

    std::printf("done\n");
    return 0;
}