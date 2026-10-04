#pragma once
// Visual zone editor for the 6-Zone Distortion filter.
// Pure Qt widget (no libobs dependency) so it can also be rendered by tests/canvas_preview.
#include <QColor>
#include <QFont>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointF>
#include <QRectF>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace opa {

struct ZoneView {
    bool enabled = true;
    double cx = 25, cy = 25, radius = 10, magnitude = 0;
};

// A detected face for the overlay, in the same percent units as a zone.
struct FaceView {
    double cx = 0, cy = 0, w = 0, h = 0, score = 0;
};

// Coordinate model copied from the shader:
//   correctedUV.x = uv.x / ar, ar = height/width  -> corrected space is isotropic
//   center = (center_x/100, center_y/100), then center.x /= ar
//   radius is expressed in corrected units where the frame height spans 1
// On a frame drawn at its true aspect ratio this means:
//   pixel = (cx/100 * W, cy/100 * H) and pixel radius = radius/100 * H
class ZoneCanvas : public QWidget {
  public:
    static constexpr int kZones = 6;
    static constexpr double kMagMin = -1.3333, kMagMax = 1.3333;

    std::function<void(int)> onSelected;
    std::function<void(int)> onChanged;

    explicit ZoneCanvas(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 150);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        // Keep the source's aspect ratio: the panel adapts to the source instead of the source
        // being squeezed into a fixed frame.
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    // A zone has exactly one stored geometry: its rest position. Animations move away from it and
    // come back, so there is no second "target" copy to edit.
    void setValues(const std::array<ZoneView, kZones> &values) {
        values_ = values;
        update();
    }
    // Real source frame drawn behind the zones (pushed by the dock while it is visible), so the
    // zones can be placed against the actual image instead of a blank grid.
    void setSourceFrame(const QImage &frame) {
        if (frame.isNull() && frame_.isNull())
            return;
        frame_ = frame;
        // A captured frame carries the true source dimensions; keep the preview in its real
        // proportions instead of assuming 16:9.
        if (!frame_.isNull() && frame_.height() > 0)
            aspect_ = double(frame_.width()) / double(frame_.height());
        update();
    }
    // Real source aspect ratio (base width/height). The shader's coordinate model works for any
    // aspect ratio, so the preview must follow it too. Used by frameRect() even before a frame is
    // captured so the zones are placed against the correct proportions.
    void setAspectRatio(double ar) {
        if (ar <= 0.0 || std::fabs(aspect_ - ar) < 1e-9)
            return;
        aspect_ = ar;
        update();
    }
    double aspectRatio() const { return aspect_; }

    // The canvas keeps the source's aspect ratio as its *preferred* size, but must never force a
    // minimum taller than the dock: an OBS dock has no scrollbar, so a forced minimum would make
    // the widget overflow and clip the preview. frameRect() letterboxes whatever space is left, so
    // letting the widget shrink keeps the frame at the correct aspect at any dock size.
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return frameHeightForWidth(w); }
    QSize sizeHint() const override { return QSize(480, frameHeightForWidth(480)); }
    void setLive(const std::array<ZoneView, kZones> &live, const std::array<bool, kZones> &active) {
        // The dock polls at 10 Hz; repainting an unchanged canvas only causes visible churn.
        bool changed = liveActive_ != active;
        for (int z = 0; z < kZones && !changed; ++z) {
            const ZoneView &a = live_[z], &b = live[z];
            changed = a.enabled != b.enabled || a.cx != b.cx || a.cy != b.cy ||
                      a.radius != b.radius || a.magnitude != b.magnitude;
        }
        if (!changed)
            return;
        live_ = live;
        liveActive_ = active;
        update();
    }
    void setSelected(int zone) {
        selected_ = std::clamp(zone, 0, kZones - 1);
        update();
    }
    int selected() const { return selected_; }

    // Detected faces drawn as an overlay (percent units). Only repaints when the list changes.
    void setDetectedFaces(const std::vector<FaceView> &faces) {
        if (faces_.size() == faces.size()) {
            bool same = true;
            for (size_t i = 0; i < faces.size() && same; ++i)
                same = faces_[i].cx == faces[i].cx && faces_[i].cy == faces[i].cy &&
                       faces_[i].w == faces[i].w && faces_[i].h == faces[i].h;
            if (same)
                return;
        }
        faces_ = faces;
        update();
    }

    std::array<ZoneView, kZones> &values() { return values_; }
    const std::array<ZoneView, kZones> &values() const { return values_; }

    void setMagnitude(int z, double v) {
        values_[z].magnitude = std::clamp(v, kMagMin, kMagMax);
        update();
        if (onChanged)
            onChanged(z);
    }
    void setRadius(int z, double v) {
        values_[z].radius = std::clamp(v, 0.0, 100.0);
        update();
        if (onChanged)
            onChanged(z);
    }
    void setCenter(int z, double x, double y) {
        values_[z].cx = std::clamp(x, 0.0, 100.0);
        values_[z].cy = std::clamp(y, 0.0, 100.0);
        update();
        if (onChanged)
            onChanged(z);
    }
    void setEnabled(int z, bool e) {
        values_[z].enabled = e;
        update();
        if (onChanged)
            onChanged(z);
    }
    // Geometry helpers are public so tests can drive real mouse coordinates.
    QRectF frameRect() const {
        const double aspect = aspect_;
        double w = width() - 8, h = height() - 8;
        if (w / h > aspect)
            w = h * aspect;
        else
            h = w / aspect;
        return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
    }
    QPointF toPix(const QRectF &r, double cx, double cy) const {
        return {r.left() + cx / 100.0 * r.width(), r.top() + cy / 100.0 * r.height()};
    }
    double radiusPix(const QRectF &r, double radius) const { return radius / 100.0 * r.height(); }

  protected:
    void fromPix(const QRectF &r, const QPointF &p, double &cx, double &cy) const {
        cx = std::clamp((p.x() - r.left()) / r.width() * 100.0, 0.0, 100.0);
        cy = std::clamp((p.y() - r.top()) / r.height() * 100.0, 0.0, 100.0);
    }

    static QColor zoneColor(int z) { return QColor::fromHsv((z * 58 + 200) % 360, 205, 255); }

    // Rest state of a zone: the single stored position. The live (animating) position is drawn as a
    // separate outline in paintEvent().
    void drawZone(QPainter &p, const QRectF &r, int z, const ZoneView &v) {
        const QPointF c = toPix(r, v.cx, v.cy);
        const double rad = radiusPix(r, v.radius);
        const double strength = std::min(1.0, std::fabs(v.magnitude) / kMagMax);
        QColor col = zoneColor(z);
        col.setAlpha(v.enabled ? (int)(55 + 175 * strength) : 90);

        QPen pen(col, 2.2);
        pen.setStyle(Qt::SolidLine);
        if (z == selected_) {
            pen.setColor(Qt::white);
            pen.setWidthF(2.8);
            pen.setStyle(Qt::SolidLine);
        }

        if (v.enabled && v.radius > 0) {
            QColor fill = zoneColor(z);
            fill.setAlpha((int)(25 + 95 * strength));
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawEllipse(c, rad, rad);
        }
        p.setBrush(Qt::NoBrush);
        // Dark halo keeps the zone readable on top of the live source frame.
        p.setPen(QPen(QColor(0, 0, 0, 160), pen.widthF() + 1.8));
        p.drawEllipse(c, rad, rad);
        p.setPen(pen);
        p.drawEllipse(c, rad, rad);

        // Number badge at the centre keeps overlapping zones readable.
        QColor badge = zoneColor(z);
        badge.setAlpha(v.enabled ? 235 : 110);
        const double br = std::min(rad, 9.0);
        p.setPen(QPen(QColor(20, 20, 24), 1));
        p.setBrush(badge);
        p.drawEllipse(c, br, br);
        QFont f = p.font();
        f.setPointSizeF(7.5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(14, 14, 18));
        p.drawText(QRectF(c.x() - br, c.y() - br, br * 2, br * 2), Qt::AlignCenter,
                   QString::number(z + 1));
        f.setBold(false);
        p.setFont(f);
        if (z == selected_) {
            p.setPen(Qt::white);
            p.drawText(QRectF(c.x() - 90, c.y() + rad + 3, 180, 16), Qt::AlignHCenter,
                       QStringLiteral("zone %1   mag %2   radius %3   %4")
                           .arg(z + 1)
                           .arg(v.magnitude, 0, 'f', 2)
                           .arg(v.radius, 0, 'f', 1)
                           .arg(v.enabled ? "" : "(disabled)"));
        }
        if (!v.enabled) {
            p.setPen(QColor(255, 170, 130));
            p.drawText(QRectF(c.x() - 60, c.y() - rad - 16, 120, 14), Qt::AlignHCenter, "OFF");
        }
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(26, 26, 31));
        const QRectF r = frameRect();
        if (frame_.isNull())
            p.fillRect(r, QColor(46, 46, 54));
        else
            p.drawImage(r, frame_);
        p.setPen(QPen(QColor(120, 120, 132), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);

        // The dotted grid is fainter on a real frame but stays visible on the empty placeholder.
        p.setPen(QPen(frame_.isNull() ? QColor(80, 80, 92, 255) : QColor(255, 255, 255, 70), 1,
                      Qt::DotLine));
        for (int i = 1; i <= 3; ++i) {
            const double x = r.left() + r.width() * i / 4.0;
            const double y = r.top() + r.height() * i / 4.0;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }

        // Detected faces (YuNet) as dashed cyan boxes, drawn under the zone markers.
        for (size_t i = 0; i < faces_.size(); ++i) {
            const FaceView &f = faces_[i];
            const double x = r.left() + (f.cx - f.w / 2) / 100.0 * r.width();
            const double y = r.top() + (f.cy - f.h / 2) / 100.0 * r.height();
            const double fw = f.w / 100.0 * r.width();
            const double fh = f.h / 100.0 * r.height();
            p.setPen(QPen(QColor(0, 220, 220, 215), 1.6, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(x, y, fw, fh));
            p.setPen(QColor(0, 220, 220));
            p.drawText(QRectF(x, y - 14, 140, 12), Qt::AlignLeft,
                       QStringLiteral("face %1  %2%")
                           .arg(i + 1)
                           .arg((int)std::lround(f.score * 100)));
        }

        // The selected zone is painted last so its handles are never covered by a neighbour.
        for (int pass = 0; pass < 2; ++pass)
            for (int z = 0; z < kZones; ++z) {
                const bool isSel = (z == selected_);
                if ((pass == 0) == isSel)
                    continue;
                drawZone(p, r, z, values_[z]);
            }

        // live (animating) position, only while the zone actually moves away from its rest state
        for (int z = 0; z < kZones; ++z) {
            if (!liveActive_[z])
                continue;
            const ZoneView &v = live_[z];
            const QPointF c = toPix(r, v.cx, v.cy);
            const double rad = radiusPix(r, v.radius);
            QPen pen(QColor(255, 255, 255, 210), 1.4, Qt::DashDotLine);
            p.setPen(QPen(QColor(0, 0, 0, 170), 3.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, rad, rad);
            p.setPen(pen);
            p.drawEllipse(c, rad, rad);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;
        const QRectF r = frameRect();
        const QPointF p = e->position();
        int hit = -1;
        double best = 1e9;
        for (int z = 0; z < kZones; ++z) {
            const QPointF c = toPix(r, values_[z].cx, values_[z].cy);
            const double d = std::hypot(p.x() - c.x(), p.y() - c.y());
            const double grab = std::max(radiusPix(r, values_[z].radius), 9.0);
            if (d <= grab && d < best) {
                best = d;
                hit = z;
            }
        }
        if (hit < 0)
            return;
        selected_ = hit;
        dragging_ = hit;
        const QPointF c = toPix(r, values_[hit].cx, values_[hit].cy);
        const double d = std::hypot(p.x() - c.x(), p.y() - c.y());
        const double rad = radiusPix(r, values_[hit].radius);
        resizing_ = rad > 4 && std::fabs(d - rad) <= 8;
        if (onSelected)
            onSelected(hit);
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (dragging_ < 0)
            return;
        const QRectF r = frameRect();
        auto &v = values_[dragging_];
        if (resizing_) {
            const QPointF c = toPix(r, v.cx, v.cy);
            const double d = std::hypot(e->position().x() - c.x(), e->position().y() - c.y());
            v.radius = std::clamp(d / r.height() * 100.0, 0.0, 100.0);
        } else {
            fromPix(r, e->position(), v.cx, v.cy);
        }
        update();
        if (onChanged)
            onChanged(dragging_);
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (dragging_ >= 0 && onChanged)
            onChanged(dragging_);
        dragging_ = -1;
        resizing_ = false;
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        const QRectF r = frameRect();
        for (int z = kZones - 1; z >= 0; --z) {
            const QPointF c = toPix(r, values_[z].cx, values_[z].cy);
            const double grab = std::max(radiusPix(r, values_[z].radius), 9.0);
            if (std::hypot(e->position().x() - c.x(), e->position().y() - c.y()) <= grab) {
                values_[z].enabled = !values_[z].enabled;
                update();
                if (onChanged)
                    onChanged(z);
                return;
            }
        }
    }
    void wheelEvent(QWheelEvent *e) override {
        const QRectF r = frameRect();
        int z = selected_;
        for (int i = 0; i < kZones; ++i) {
            const QPointF c = toPix(r, values_[i].cx, values_[i].cy);
            const double grab = std::max(radiusPix(r, values_[i].radius), 9.0);
            if (std::hypot(e->position().x() - c.x(), e->position().y() - c.y()) <= grab) {
                z = i;
                break;
            }
        }
        const double steps = e->angleDelta().y() / 120.0;
        if (steps == 0)
            return;
        selected_ = z;
        if (e->modifiers() & Qt::ShiftModifier)
            setRadius(z, values_[z].radius + steps);
        else
            setMagnitude(z, values_[z].magnitude + steps * 0.05);
        if (onSelected)
            onSelected(z);
        e->accept();
    }

  private:
    // Height that keeps the widget (and therefore the frame, minus its 8px margin) at the source's
    // aspect ratio for a given width. Only a *preferred* size: the layout may shrink the widget when
    // the dock is smaller, and frameRect() letterboxes the frame in whatever space is left.
    int frameHeightForWidth(int w) const {
        const double inner = std::max(24.0, double(w) - 8.0);
        return int(std::lround(inner / aspect_ + 8.0));
    }

    std::array<ZoneView, kZones> values_{};
    std::array<ZoneView, kZones> live_{};
    std::array<bool, kZones> liveActive_{};
    std::vector<FaceView> faces_{};
    QImage frame_;
    double aspect_ = 16.0 / 9.0;
    int selected_ = 0;
    int dragging_ = -1;
    bool resizing_ = false;
};

} // namespace opa