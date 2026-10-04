#include "ui.hpp"
#include "filter.hpp"
#include "zone_canvas.hpp" // for ZoneView (the per-point value holder)
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include "json_builder.hpp"
#include "preset.hpp"
using namespace opa;

// The dock edits the three face points (eyes / nose / mouth). Positions come from
// face detection; here you configure each point's offset, radius, magnitude and animation.
class AnimatorPanel : public QWidget {
    QComboBox *filters = nullptr, *modeBox = nullptr, *pointBox = nullptr;
    QCheckBox *blurBox = nullptr, *debugBox = nullptr, *scaleBox = nullptr;
    QDoubleSpinBox *blurPxSpin = nullptr;
    std::vector<FilterEntry> entries;
    std::string filterUuid;
    std::weak_ptr<Engine> selected;

    std::array<ZoneView, pointCount> points{};
    QCheckBox *enabledBox = nullptr, *faceBox = nullptr;
    QDoubleSpinBox *smoothSpin = nullptr;
    QLabel *faceStatus = nullptr, *hint = nullptr;
    QSlider *cxSld = nullptr, *cySld = nullptr, *radSld = nullptr, *magSld = nullptr;
    QDoubleSpinBox *cxSpin = nullptr, *cySpin = nullptr, *radSpin = nullptr, *magSpin = nullptr;
    QDoubleSpinBox *magMinSpin = nullptr, *magMaxSpin = nullptr;
    QComboBox *testParam = nullptr;
    QDoubleSpinBox *testValue = nullptr, *durSpin = nullptr;
    QComboBox *easing = nullptr;
    QCheckBox *autoReturn = nullptr;
    QDoubleSpinBox *holdSpin = nullptr, *returnSpin = nullptr;
    QComboBox *returnEasing = nullptr;

    QTableWidget *table = nullptr;
    QDoubleSpinBox *value = nullptr, *duration = nullptr;
    QCheckBox *returnToBase = nullptr;
    QComboBox *tableEasing = nullptr;

    // JSON tab: one ready-to-copy CallVendorRequest per point, mirrored from the filter settings.
    QComboBox *jsonAction = nullptr, *jsonParam = nullptr, *jsonFormat = nullptr;
    QDoubleSpinBox *jsonValue = nullptr;
    std::array<QPlainTextEdit *, pointCount> jsonEdits{};

    // Import / Export tab: the whole automation state as plain copy-pasteable JSON text.
    QPlainTextEdit *presetEdit = nullptr;
    QLabel *presetStatus = nullptr;
    std::string presetUuid; // filter whose settings are currently shown, so a rescan cannot clobber edits

    QLabel *metrics = nullptr;
    QPlainTextEdit *logs = nullptr;
    QLineEdit *logFilter = nullptr;
    QCheckBox *pause = nullptr;

    QTimer *pushTimer = nullptr;
    QTabWidget *tabs = nullptr;
    bool updating = false;

    obs_source_t *resolve() const {
        if (filterUuid.empty())
            return nullptr;
        obs_data_t *r = obs_data_create();
        obs_data_set_string(r, "filterUuid", filterUuid.c_str());
        obs_source_t *f = resolveFilter(r);
        obs_data_release(r);
        return f;
    }
    static std::string prefix(int point) { return "point" + std::to_string(point + 1) + "_"; }

    void pull() {
        obs_source_t *f = resolve();
        if (!f)
            return;
        obs_data_t *s = obs_source_get_settings(f);
        for (int p = 0; p < pointCount; ++p) {
            const auto k = prefix(p);
            auto &v = points[p];
            v.enabled = obs_data_get_bool(s, (k + "enable").c_str());
            v.cx = obs_data_get_double(s, (k + "offset_x").c_str());
            v.cy = obs_data_get_double(s, (k + "offset_y").c_str());
            v.radius = obs_data_get_double(s, (k + "radius").c_str());
            v.magnitude = obs_data_get_double(s, (k + "magnitude").c_str());
            // Per-point magnitude clamp; fall back to the shader default when unset or inverted.
            v.magMin = obs_data_get_double(s, (k + "magnitude_min").c_str());
            v.magMax = obs_data_get_double(s, (k + "magnitude_max").c_str());
            if (!(v.magMax > v.magMin)) {
                v.magMin = defaultMagnitudeMin;
                v.magMax = defaultMagnitudeMax;
            }
            v.magnitude = std::clamp(v.magnitude, v.magMin, v.magMax);
        }
        updating = true;
        modeBox->setCurrentIndex(std::clamp((int)obs_data_get_int(s, "mode"), 0, 2));
        faceBox->setChecked(obs_data_get_bool(s, "face_tracking"));
        blurBox->setChecked(obs_data_get_bool(s, "effect_blur"));
        debugBox->setChecked(obs_data_get_bool(s, "effect_debug"));
        blurPxSpin->setValue(obs_data_get_double(s, "face_blur_px"));
        scaleBox->setChecked(obs_data_get_bool(s, "face_scale"));
        smoothSpin->setValue(obs_data_get_double(s, "face_smooth_ms"));
        const auto k = prefix(pointBox->currentIndex());
        const int defEase = std::clamp((int)obs_data_get_int(s, "default_easing"), 0, 30);
        int e = (int)obs_data_get_int(s, (k + "magnitude_easing").c_str());
        easing->setCurrentIndex(e < 0 ? defEase : std::clamp(e, 0, 30));
        const double defMs = obs_data_get_double(s, "default_duration_ms");
        const double ms = obs_data_get_double(s, (k + "magnitude_duration_ms").c_str());
        durSpin->setValue(ms < 0 ? defMs : ms);
        autoReturn->setChecked(obs_data_get_bool(s, (k + "magnitude_auto_return").c_str()));
        holdSpin->setValue(obs_data_get_double(s, (k + "magnitude_hold_ms").c_str()));
        returnSpin->setValue(obs_data_get_double(s, (k + "magnitude_return_ms").c_str()));
        returnEasing->setCurrentIndex(
            std::clamp<int>((int)obs_data_get_int(s, (k + "magnitude_return_easing").c_str()), 0, 30));
        updating = false;
        obs_data_release(s);
        obs_source_release(f);
        syncControls();
    }

    // Writes the three points' rest values and mirrors them into target/return, so Start and the
    // automatic return come back to the configured rest state.
    void writePositionLocked(obs_data_t *s) {
        static const char *names[pointParamCount] = {"enable", "offset_x", "offset_y", "radius",
                                                     "magnitude"};
        for (int p = 0; p < pointCount; ++p) {
            const auto &v = points[p];
            const auto k = prefix(p);
            obs_data_set_bool(s, (k + "enable").c_str(), v.enabled);
            obs_data_set_double(s, (k + "offset_x").c_str(), v.cx);
            obs_data_set_double(s, (k + "offset_y").c_str(), v.cy);
            obs_data_set_double(s, (k + "radius").c_str(), v.radius);
            obs_data_set_double(s, (k + "magnitude").c_str(), v.magnitude);
            obs_data_set_double(s, (k + "magnitude_min").c_str(), v.magMin);
            obs_data_set_double(s, (k + "magnitude_max").c_str(), v.magMax);
            const double flat[pointParamCount] = {v.enabled ? 1.0 : 0.0, v.cx, v.cy, v.radius,
                                                  v.magnitude};
            for (int i = 0; i < pointParamCount; ++i) {
                obs_data_set_double(s, (k + names[i] + "_target").c_str(), flat[i]);
                obs_data_set_double(s, (k + names[i] + "_return_value").c_str(), flat[i]);
            }
        }
    }
    void pushSet() {
        if (updating)
            return;
        obs_source_t *f = resolve();
        if (!f)
            return;
        obs_data_t *s = obs_source_get_settings(f);
        writePositionLocked(s);
        obs_source_update(f, s);
        obs_data_release(s);
        obs_source_release(f);
    }
    void pushLater() { pushTimer->start(40); }

    void select() {
        selected.reset();
        filterUuid.clear();
        const int i = filters->currentIndex();
        if (i >= 0 && i < (int)entries.size()) {
            filterUuid = entries[i].filterUuid;
            obs_source_t *f = resolve();
            if (f) {
                selected = engineFor(f);
                obs_source_release(f);
            }
        }
        pull();
        refresh();
        generateJson();
        // Refresh the Import / Export text only when the selected filter actually changed, so a
        // background rescan cannot clobber text the user is still editing.
        if (presetUuid != filterUuid) {
            presetUuid = filterUuid;
            fillPresetEdit();
            presetStatus->setText(QString());
        }
    }
    void reload() {
        std::string old = filterUuid;
        entries = enumerateFilters();
        filters->blockSignals(true);
        filters->clear();
        int found = -1;
        for (int n = 0; n < (int)entries.size(); ++n) {
            filters->addItem(QString::fromStdString(entries[n].source + " \u2192 " + entries[n].filter));
            if (entries[n].filterUuid == old)
                found = n;
        }
        if (found < 0 && !entries.empty())
            found = 0;
        if (found >= 0)
            filters->setCurrentIndex(found);
        filters->blockSignals(false);
        select();
    }

    void writeTimingLocked(obs_data_t *s, int index) {
        const auto k = parameterName(index);
        obs_data_set_double(s, (k + "_duration_ms").c_str(), durSpin->value());
        obs_data_set_int(s, (k + "_easing").c_str(), easing->currentIndex());
        obs_data_set_bool(s, (k + "_auto_return").c_str(), autoReturn->isChecked());
        obs_data_set_double(s, (k + "_hold_ms").c_str(), holdSpin->value());
        obs_data_set_double(s, (k + "_return_ms").c_str(), returnSpin->value());
        obs_data_set_int(s, (k + "_return_easing").c_str(), returnEasing->currentIndex());
    }
    void writeTiming(bool allPoints) {
        if (updating)
            return;
        obs_source_t *f = resolve();
        if (!f)
            return;
        obs_data_t *s = obs_source_get_settings(f);
        const int first = allPoints ? 0 : pointBox->currentIndex() * pointParamCount;
        const int last = allPoints ? paramCount : first + pointParamCount;
        for (int i = first; i < last; ++i)
            writeTimingLocked(s, i);
        obs_source_update(f, s);
        obs_data_release(s);
        obs_source_release(f);
        generateJson();
    }

    // Build one ready-to-copy CallVendorRequest per point. Every message uses the source + filter
    // of the selected filter and the point's OWN saved settings (duration / easing / auto-return),
    // so the JSON matches what the dock and the vendor actually run.
    void generateJson() {
        static const char *const actionNames[] = {"Add", "Set", "Start", "Reset", "Stop"};
        const int actionIndex = std::clamp(jsonAction->currentIndex(), 0, 4);
        const QString vendorType = QString::fromLatin1(actionNames[actionIndex]);
        const int paramIndex = std::clamp(jsonParam->currentIndex(), 0, pointParamCount - 1);
        const bool withValue = actionIndex == 0 || actionIndex == 1; // Add / Set carry a value
        const bool full = jsonFormat->currentIndex() == 1;           // default: vendor request (d)

        std::string source, filter;
        for (const auto &entry : entries)
            if (!filterUuid.empty() && entry.filterUuid == filterUuid) {
                source = entry.source;
                filter = entry.filter;
                break;
            }

        obs_source_t *f = resolve();
        obs_data_t *s = f ? obs_source_get_settings(f) : nullptr;
        const double defMs = s ? obs_data_get_double(s, "default_duration_ms") : 1000.0;
        const int defEase = s ? (int)obs_data_get_int(s, "default_easing") : 5;

        for (int p = 0; p < pointCount; ++p) {
            const std::string key = parameterName(p * pointParamCount + paramIndex);
            double ms = s ? obs_data_get_double(s, (key + "_duration_ms").c_str()) : -1;
            if (!std::isfinite(ms) || ms < 0)
                ms = defMs;
            int ease = s ? (int)obs_data_get_int(s, (key + "_easing").c_str()) : -1;
            if (ease < 0)
                ease = defEase;
            ease = std::clamp(ease, 0, 30);
            // Per-point auto-return (mirrored into the "returnToZero" field of the request).
            const bool returnToZero = s ? obs_data_get_bool(s, (key + "_auto_return").c_str()) : false;

            QStringList fields;
            fields << "\"source\": \"" + jsonEscape(QString::fromStdString(source)) + "\"";
            fields << "\"filter\": \"" + jsonEscape(QString::fromStdString(filter)) + "\"";
            fields << "\"parameter\": \"" + QString::fromStdString(key) + "\"";
            if (withValue)
                fields << "\"value\": " + jsonNum(jsonValue->value());
            fields << "\"durationMs\": " + jsonNum(ms);
            fields << "\"easing\": \"" + QString::fromLatin1(easingNames[ease]) + "\"";
            fields << "\"returnToZero\": " + QString(returnToZero ? "true" : "false");

            const QString requestId =
                QStringLiteral("point%1-%2").arg(p + 1).arg(vendorType.toLower());
            const QString text =
                filterUuid.empty()
                    ? QStringLiteral("No filter selected. Pick a filter above, then press Regenerate.")
                    : vendorMessage(full, requestId, vendorType, fields);
            if (jsonEdits[p] && jsonEdits[p]->toPlainText() != text)
                jsonEdits[p]->setPlainText(text);
        }
        if (s)
            obs_data_release(s);
        if (f)
            obs_source_release(f);
    }
    // ---- Import / Export: the dock's automation settings as copy-pasteable JSON text ----
    // Reads the selected filter's settings into a Preset (what "Export from filter" writes out).
    Preset readPreset() const {
        Preset p;
        obs_source_t *f = resolve();
        if (!f)
            return p;
        obs_data_t *s = obs_source_get_settings(f);
        p.mode = std::clamp<int>((int)obs_data_get_int(s, "mode"), 0, 2);
        p.faceTracking = obs_data_get_bool(s, "face_tracking");
        p.faceSmoothMs = obs_data_get_double(s, "face_smooth_ms");
        p.effectBlur = obs_data_get_bool(s, "effect_blur");
        p.faceBlurPx = obs_data_get_double(s, "face_blur_px");
        p.effectDebug = obs_data_get_bool(s, "effect_debug");
        p.faceScale = obs_data_get_bool(s, "face_scale");
        p.defaultDurationMs = obs_data_get_double(s, "default_duration_ms");
        p.defaultEasing = (int)obs_data_get_int(s, "default_easing");
        for (int i = 0; i < pointCount; ++i) {
            auto &pt = p.points[i];
            const auto k = "point" + std::to_string(i + 1) + "_";
            pt.enable = obs_data_get_bool(s, (k + "enable").c_str());
            pt.offsetX = obs_data_get_double(s, (k + "offset_x").c_str());
            pt.offsetY = obs_data_get_double(s, (k + "offset_y").c_str());
            pt.radius = obs_data_get_double(s, (k + "radius").c_str());
            pt.magnitude = obs_data_get_double(s, (k + "magnitude").c_str());
            pt.magMin = obs_data_get_double(s, (k + "magnitude_min").c_str());
            pt.magMax = obs_data_get_double(s, (k + "magnitude_max").c_str());
            if (!(pt.magMax > pt.magMin)) {
                pt.magMin = defaultMagnitudeMin;
                pt.magMax = defaultMagnitudeMax;
            }
            const auto m = parameterName(i * pointParamCount + magnitudeParam);
            pt.durationMs = obs_data_get_double(s, (m + "_duration_ms").c_str());
            pt.easing = (int)obs_data_get_int(s, (m + "_easing").c_str());
            pt.autoReturn = obs_data_get_bool(s, (m + "_auto_return").c_str());
            pt.holdMs = obs_data_get_double(s, (m + "_hold_ms").c_str());
            pt.returnMs = obs_data_get_double(s, (m + "_return_ms").c_str());
            pt.returnEasing = (int)obs_data_get_int(s, (m + "_return_easing").c_str());
        }
        obs_data_release(s);
        obs_source_release(f);
        return p;
    }
    void fillPresetEdit() {
        if (!presetEdit)
            return;
        presetEdit->setPlainText(presetToJson(readPreset()));
    }
    // Writes one point's move / return timing from an explicit preset (the import path). Same keys
    // writeTimingLocked() writes, but taken from the preset instead of the current widgets.
    void writeTimingPointLocked(obs_data_t *s, int index, const PresetPoint &t) {
        const auto k = parameterName(index);
        obs_data_set_double(s, (k + "_duration_ms").c_str(), t.durationMs);
        obs_data_set_int(s, (k + "_easing").c_str(), t.easing);
        obs_data_set_bool(s, (k + "_auto_return").c_str(), t.autoReturn);
        obs_data_set_double(s, (k + "_hold_ms").c_str(), t.holdMs);
        obs_data_set_double(s, (k + "_return_ms").c_str(), t.returnMs);
        obs_data_set_int(s, (k + "_return_easing").c_str(), t.returnEasing);
    }
    // Applies a parsed preset to the selected filter: rest positions (plus their target / return
    // mirrors, so Start and the automatic return land on the imported state), the per-point
    // magnitude timing and the global options, then reloads the widgets.
    void applyPreset(const Preset &p) {
        obs_source_t *f = resolve();
        if (!f) {
            presetStatus->setText(
                "No filter selected. Add the MoskiFaceDetector filter, then press Refresh.");
            return;
        }
        for (int i = 0; i < pointCount; ++i) {
            auto &v = points[i];
            const auto &pt = p.points[i];
            v.enabled = pt.enable;
            v.cx = pt.offsetX;
            v.cy = pt.offsetY;
            v.radius = pt.radius;
            v.magMin = pt.magMin;
            v.magMax = pt.magMax;
            v.magnitude = std::clamp(pt.magnitude, pt.magMin, pt.magMax);
        }
        obs_data_t *s = obs_source_get_settings(f);
        writePositionLocked(s);
        for (int i = 0; i < pointCount; ++i)
            writeTimingPointLocked(s, i * pointParamCount + magnitudeParam, p.points[i]);
        obs_data_set_int(s, "mode", p.mode);
        obs_data_set_bool(s, "face_tracking", p.faceTracking);
        obs_data_set_double(s, "face_smooth_ms", p.faceSmoothMs);
        obs_data_set_bool(s, "effect_blur", p.effectBlur);
        obs_data_set_double(s, "face_blur_px", p.faceBlurPx);
        obs_data_set_bool(s, "effect_debug", p.effectDebug);
        obs_data_set_bool(s, "face_scale", p.faceScale);
        obs_data_set_double(s, "default_duration_ms", p.defaultDurationMs);
        obs_data_set_int(s, "default_easing", p.defaultEasing);
        obs_source_update(f, s);
        obs_data_release(s);
        obs_source_release(f);
        pull();
        refresh();
        generateJson();
    }
    int testIndex() const { return pointBox->currentIndex() * pointParamCount + testParam->currentIndex(); }
    void test(const std::string &action) {
        try {
            auto e = selected.lock();
            if (!e)
                throw std::runtime_error(
                    "No filter selected. Add the MoskiFaceDetector filter to a source, then press Refresh.");
            if (obs_source_t *f = resolve()) {
                obs_data_t *s = obs_source_get_settings(f);
                writePositionLocked(s);
                writeTimingLocked(s, testIndex());
                obs_source_update(f, s);
                obs_data_release(s);
                obs_source_release(f);
            }
            e->command(testIndex(), action, testValue->value(), durSpin->value() / 1000.0,
                       easing->currentIndex(), -1);
        } catch (const std::exception &ex) {
            QMessageBox::warning(this, "MoskiAutomator", ex.what());
        }
        refresh();
    }
    void run(const std::string &action) {
        try {
            auto e = selected.lock();
            if (!e)
                throw std::runtime_error(
                    "No filter selected. Add the MoskiFaceDetector filter to a source, then press Refresh.");
            if (action == "StartAll")
                e->startAll();
            else if (action == "StopAll")
                e->stopAll();
            else if (action == "ResetAll")
                e->resetAll();
            else {
                const int index = table->currentRow();
                if (index < 0)
                    throw std::runtime_error("Select a parameter row first.");
                e->command(index, action, value->value(), duration->value() / 1000,
                           tableEasing->currentIndex(), returnToBase->isChecked() ? 1 : 0);
            }
        } catch (const std::exception &ex) {
            QMessageBox::warning(this, "MoskiAutomator", ex.what());
        }
        refresh();
    }
    void playPoint() {
        try {
            auto e = selected.lock();
            if (!e)
                throw std::runtime_error("No filter selected.");
            const int p = pointBox->currentIndex();
            for (int k = 0; k < pointParamCount; ++k)
                e->command(p * pointParamCount + k, "Start");
        } catch (const std::exception &ex) {
            QMessageBox::warning(this, "MoskiAutomator", ex.what());
        }
        refresh();
    }

    void refresh() {
        if (!isVisible())
            return;
        auto e = selected.lock();
        if (!e) {
            const QString msg = QStringLiteral(
                "No filter selected. Add the MoskiFaceDetector filter to a source, then press Refresh.");
            if (metrics->text() != msg)
                metrics->setText(msg);
            if (table->item(0, 0))
                table->clearContents();
            if (!faceStatus->text().isEmpty())
                faceStatus->setText(QString());
            return;
        }
        auto s = e->snapshot();
        const QString faceText =
            QString("Tracking %1 | faces %2 | detect %3 ms | seq %4")
                .arg(s.faceTracking ? (s.faceAvailable ? "ON" : "ON (model missing)") : "off")
                .arg((int)s.faces.size())
                .arg(s.faceDetectMs, 0, 'f', 1)
                .arg((qulonglong)s.faceSequence);
        if (faceStatus->text() != faceText)
            faceStatus->setText(faceText);
        for (int i = 0; i < paramCount; ++i) {
            const auto &p = s.controller.at(i);
            const QString cells[] = {QString::fromStdString(parameterName(i)),
                                     QString::number(p.state.currentValue, 'f', 4),
                                     QString::number(p.gpuValue(), 'f', 4),
                                     QString::number(p.state.targetValue, 'f', 4),
                                     QString::number(p.state.progress * 100, 'f', 1) + "%",
                                     phaseName(p.state.phase),
                                     easingNames[p.state.easingType]};
            for (int k = 0; k < 7; ++k) {
                auto *cell = table->item(i, k);
                if (!cell) {
                    cell = new QTableWidgetItem;
                    table->setItem(i, k, cell);
                }
                if (cell->text() != cells[k])
                    cell->setText(cells[k]);
            }
        }
        const QString metricsText = QString("Mode %1 | Animating: %2 | Active %3\nCPU math: last %4 "
                                            "us, max %5 us | Evaluations %6 | Value changes %7 | "
                                            "setters %8")
                                        .arg((int)s.mode)
                                        .arg(s.controller.activeCount() ? "YES" : "no")
                                        .arg(s.controller.activeCount())
                                        .arg(s.updateUs, 0, 'f', 2)
                                        .arg(s.maxUpdateUs, 0, 'f', 2)
                                        .arg((qulonglong)s.evaluations)
                                        .arg((qulonglong)s.parameterUpdates)
                                        .arg((qulonglong)s.uniformCalls);
        if (metrics->text() != metricsText)
            metrics->setText(metricsText);
        if (!pause->isChecked()) {
            QStringList lines;
            for (const auto &line : s.logs) {
                auto q = QString::fromStdString(line);
                if (q.contains(logFilter->text(), Qt::CaseInsensitive))
                    lines << q;
            }
            const auto text = lines.join('\n');
            if (logs->toPlainText() != text) {
                logs->setPlainText(text);
                logs->moveCursor(QTextCursor::End);
            }
        }
    }

    // The slider and the spin box of one point parameter are two views of the same number. Writing
    // that number into both here (while `updating` is held true so the valueChanged round-trip is
    // ignored) is what keeps them in lock-step no matter which control the user touched.
    // `sliderScale` is the number of slider steps per stored unit and must match the range that
    // addRow() created the slider with.
    void setPair(int which, double v) {
        QSlider *slider = nullptr;
        QDoubleSpinBox *spin = nullptr;
        double sliderScale = 10.0;
        switch (which) {
        case 0: slider = cxSld; spin = cxSpin; break;
        case 1: slider = cySld; spin = cySpin; break;
        case 2: slider = radSld; spin = radSpin; break;
        default: slider = magSld; spin = magSpin; sliderScale = 1000.0; break;
        }
        const bool guard = updating;
        updating = true;
        if (slider)
            slider->setValue((int)std::lround(v * sliderScale));
        if (spin)
            spin->setValue(v);
        updating = guard;
    }

    // The magnitude slider / spin span the selected point's configurable min..max. The caller must
    // hold `updating` true so changing the ranges (and the paired values) does not re-enter setters.
    void applyMagnitudeRange(const ZoneView &v) {
        const int lo = (int)std::lround(v.magMin * 1000.0);
        const int hi = (int)std::lround(v.magMax * 1000.0);
        magSld->setRange(std::min(lo, hi), std::max(lo, hi));
        magSpin->setRange(v.magMin, v.magMax);
        magMinSpin->setValue(v.magMin);
        magMaxSpin->setValue(v.magMax);
    }

    void syncControls() {
        const bool guard = updating;
        updating = true;
        const int z = pointBox->currentIndex();
        const auto &v = points[z];
        applyMagnitudeRange(v);
        setPair(0, v.cx);
        setPair(1, v.cy);
        setPair(2, v.radius);
        setPair(3, v.magnitude);
        enabledBox->setChecked(v.enabled);
        const QString hintText = QStringLiteral(
            "Positions are detected on every face (forehead / nose / mouth). Offset nudges the point "
            "(percent of the frame), radius is a percent of the frame height, magnitude is the "
            "distortion strength. Magnitude min / max clamp how far this point's magnitude may "
            "travel. The slider and the box always show the same value; Add / Set / Start animate "
            "this point on all detected faces.");
        if (hint->text() != hintText)
            hint->setText(hintText);
        updating = guard;
    }

  public:
    explicit AnimatorPanel(QWidget *parent) : QWidget(parent) {
        auto *root = new QVBoxLayout(this);
        auto *top = new QHBoxLayout;
        filters = new QComboBox;
        top->addWidget(filters, 1);
        auto *refreshBtn = new QPushButton("Refresh");
        top->addWidget(refreshBtn);
        root->addLayout(top);

        tabs = new QTabWidget;
        root->addWidget(tabs, 1);
        pushTimer = new QTimer(this);
        pushTimer->setSingleShot(true);

        // ============================ VISUAL ============================
        auto *visual = new QWidget;
        auto *vl = new QVBoxLayout(visual);
        vl->setContentsMargins(6, 6, 6, 6);
        vl->setSpacing(8);

        auto *intro = new QLabel("The three points are placed on every detected face. Choose a "
                                 "point, set where it sits and how it animates.");
        intro->setWordWrap(true);
        vl->addWidget(intro);

        // ---- 1. Playback mode ----
        auto *modeGroup = new QGroupBox("Playback mode");
        auto *modeLay = new QHBoxLayout(modeGroup);
        modeLay->setSpacing(6);
        modeBox = new QComboBox;
        modeBox->addItems({"Static", "Shader sine animation", "Plugin animation"});
        modeBox->setToolTip("Static: the points stay where they are, no animation. "
                            "Shader: the GPU shader moves the magnitude by itself. "
                            "Plugin: this dock drives the animation (recommended).");
        modeLay->addWidget(modeBox, 1);
        vl->addWidget(modeGroup);

        // ---- 2. Face tracking ----
        auto *faceGroup = new QGroupBox("Face tracking");
        auto *fg = new QVBoxLayout(faceGroup);
        fg->setSpacing(4);
        auto *faceRow = new QHBoxLayout;
        faceBox = new QCheckBox("Track faces");
        faceBox->setToolTip("Detect faces and place the three points (forehead / nose / mouth) on each "
                            "of them.");
        faceRow->addWidget(faceBox);
        faceRow->addWidget(new QLabel("Smoothing"));
        smoothSpin = new QDoubleSpinBox;
        smoothSpin->setRange(0, 1000);
        smoothSpin->setSuffix(" ms");
        smoothSpin->setValue(120);
        smoothSpin->setFixedWidth(80);
        smoothSpin->setToolTip("Ease the points toward each detection over this time. 0 = no "
                               "smoothing (raw, jittery); higher = smoother but slower to follow.");
        faceRow->addWidget(smoothSpin);
        faceRow->addStretch(1);
        fg->addLayout(faceRow);
        faceStatus = new QLabel("Tracking off");
        faceStatus->setWordWrap(true);
        fg->addWidget(faceStatus);
        vl->addWidget(faceGroup);

        // ---- 3. Overlay options (independent of the distortion) ---------------
        auto *optGroup = new QGroupBox("Overlay options");
        auto *og = new QVBoxLayout(optGroup);
        og->setSpacing(4);
        auto *renderRow = new QHBoxLayout;
        blurBox = new QCheckBox("Blur faces");
        blurBox->setToolTip("Blur the whole detected face box (covers/anonimises the face). "
                            "Independent of the distortion.");
        blurPxSpin = new QDoubleSpinBox;
        blurPxSpin->setRange(2, 128);
        blurPxSpin->setSuffix(" px");
        blurPxSpin->setValue(24);
        blurPxSpin->setFixedWidth(78);
        blurPxSpin->setToolTip("Blur radius in pixels.");
        renderRow->addWidget(blurBox);
        renderRow->addWidget(blurPxSpin);
        renderRow->addSpacing(12);
        debugBox = new QCheckBox("Debug points");
        debugBox->setToolTip("Overlay the detection box and the three point anchors: red = forehead, "
                             "green = nose, blue = mouth. Independent of the distortion.");
        renderRow->addWidget(debugBox);
        renderRow->addStretch(1);
        og->addLayout(renderRow);
        auto *scaleRow = new QHBoxLayout;
        scaleBox = new QCheckBox("Scale points with face size");
        scaleBox->setToolTip("Radius and offsets are relative to the detected face height, so a "
                             "distant face gets proportionally smaller points.");
        scaleRow->addWidget(scaleBox);
        scaleRow->addStretch(1);
        og->addLayout(scaleRow);
        vl->addWidget(optGroup);

        // ---- 4. Point rest position ----
        auto *pointGroup = new QGroupBox("Point - rest position");
        auto *pg = new QVBoxLayout(pointGroup);
        pg->setSpacing(4);
        auto *prow = new QHBoxLayout;
        prow->addWidget(new QLabel("Point:"));
        pointBox = new QComboBox;
        for (int p = 0; p < pointCount; ++p)
            pointBox->addItem(pointName(p));
        pointBox->setToolTip("Which landmark point to edit: 1 = Forehead, 2 = Nose, 3 = Mouth.");
        prow->addWidget(pointBox);
        enabledBox = new QCheckBox("Draw this point");
        enabledBox->setToolTip("Draw the point on every detected face.");
        prow->addWidget(enabledBox);
        prow->addStretch(1);
        pg->addLayout(prow);

        auto *grid = new QGridLayout;
        grid->setVerticalSpacing(2);
        grid->setHorizontalSpacing(6);
        auto addRow = [&](int r, const QString &name, const QString &tip, QSlider *&sl,
                          QDoubleSpinBox *&sp, double lo, double hi, double scale, int decimals) {
            grid->addWidget(new QLabel(name), r, 0);
            sl = new QSlider(Qt::Horizontal);
            sl->setRange((int)std::lround(lo * scale), (int)std::lround(hi * scale));
            sl->setToolTip(tip);
            grid->addWidget(sl, r, 1);
            sp = new QDoubleSpinBox;
            sp->setRange(lo, hi);
            sp->setDecimals(decimals);
            sp->setSingleStep(1.0 / scale);
            sp->setFixedWidth(92);
            sp->setToolTip(tip);
            grid->addWidget(sp, r, 2);
        };
        addRow(0, "Offset X (%)", "Push the point left / right from its detected position.",
               cxSld, cxSpin, -100, 100, 10, 1);
        addRow(1, "Offset Y (%)", "Push the point up / down from its detected position.",
               cySld, cySpin, -100, 100, 10, 1);
        addRow(2, "Radius (%)", "Radius of the distortion zone, as a percent of the frame height.",
               radSld, radSpin, 0, 100, 10, 1);
        addRow(3, "Magnitude", "Distortion strength: above 0 bulges outwards, below 0 pinches "
                               "inwards. Default range -1.3333 to 1.3333; adjust min / max below.",
               magSld, magSpin, -1.3333, 1.3333, 1000, 3);
        auto *magZero = new QPushButton("Reset to 0");
        magZero->setFixedWidth(92);
        magZero->setToolTip("Set this point's magnitude back to 0 (its resting value).");
        grid->addWidget(magZero, 3, 3);
        auto magBox = [&](QDoubleSpinBox *&sp, const QString &tip) {
            sp = new QDoubleSpinBox;
            sp->setRange(-100, 100);
            sp->setDecimals(3);
            sp->setSingleStep(0.01);
            sp->setFixedWidth(92);
            sp->setToolTip(tip);
        };
        grid->addWidget(new QLabel("Magnitude min / max"), 4, 0);
        magBox(magMinSpin, "Lowest magnitude this point may reach (values are clamped to it).");
        grid->addWidget(magMinSpin, 4, 1);
        magBox(magMaxSpin, "Highest magnitude this point may reach (values are clamped to it).");
        grid->addWidget(magMaxSpin, 4, 2);
        pg->addLayout(grid);
        vl->addWidget(pointGroup);

        // ---- timing: outgoing move, then the automatic return after the last Add ----
        auto *timing = new QGroupBox("Move & auto-return timing");
        auto *tg = new QGridLayout(timing);
        tg->setVerticalSpacing(2);
        tg->setHorizontalSpacing(6);
        durSpin = new QDoubleSpinBox;
        durSpin->setRange(-1, 3600000);
        durSpin->setSuffix(" ms");
        durSpin->setValue(1000);
        durSpin->setFixedWidth(110);
        tg->addWidget(new QLabel("Move duration"), 0, 0);
        tg->addWidget(durSpin, 0, 1);
        easing = new QComboBox;
        for (auto *n : easingNames)
            easing->addItem(n);
        easing->setCurrentIndex(5);
        tg->addWidget(new QLabel("Move easing"), 0, 2);
        tg->addWidget(easing, 0, 3);
        autoReturn = new QCheckBox("auto return");
        autoReturn->setChecked(true);
        tg->addWidget(autoReturn, 0, 4);
        holdSpin = new QDoubleSpinBox;
        holdSpin->setRange(0, 3600000);
        holdSpin->setSuffix(" ms");
        holdSpin->setValue(200);
        holdSpin->setFixedWidth(110);
        tg->addWidget(new QLabel("Return delay after the last Add"), 1, 0);
        tg->addWidget(holdSpin, 1, 1);
        returnSpin = new QDoubleSpinBox;
        returnSpin->setRange(0, 3600000);
        returnSpin->setSuffix(" ms");
        returnSpin->setValue(800);
        returnSpin->setFixedWidth(110);
        tg->addWidget(new QLabel("Return duration"), 1, 2);
        tg->addWidget(returnSpin, 1, 3);
        returnEasing = new QComboBox;
        for (auto *n : easingNames)
            returnEasing->addItem(n);
        returnEasing->setCurrentIndex(5);
        tg->addWidget(returnEasing, 1, 4);
        auto *applyTiming = new QPushButton("Apply to this point");
        auto *applyTimingAll = new QPushButton("Apply to all points");
        tg->addWidget(applyTiming, 2, 0, 1, 2);
        tg->addWidget(applyTimingAll, 2, 2, 1, 2);
        vl->addWidget(timing);

        // ---- test: everything needed to see the effect without leaving the first tab ----
        auto *testBox = new QGroupBox("Test this point");
        auto *tt = new QGridLayout(testBox);
        tt->setVerticalSpacing(2);
        tt->setHorizontalSpacing(6);
        testParam = new QComboBox;
        for (const char *n : {"Enable", "Offset X", "Offset Y", "Radius", "Magnitude"})
            testParam->addItem(n);
        testParam->setCurrentIndex(4);
        testValue = new QDoubleSpinBox;
        testValue->setRange(-100, 100);
        testValue->setDecimals(4);
        testValue->setSingleStep(0.05);
        testValue->setValue(0.3);
        testValue->setFixedWidth(96);
        tt->addWidget(new QLabel("Parameter"), 0, 0);
        tt->addWidget(testParam, 0, 1);
        tt->addWidget(new QLabel("Value / delta"), 0, 2);
        tt->addWidget(testValue, 0, 3);
        auto *addBtn = new QPushButton("Add (accumulate)");
        auto *setBtn = new QPushButton("Set");
        auto *startBtn = new QPushButton("Start saved");
        auto *stopBtn = new QPushButton("Stop");
        auto *resetBtn = new QPushButton("Back to rest");
        auto *testButtons = new QHBoxLayout;
        for (auto *b : {addBtn, setBtn, startBtn, stopBtn, resetBtn})
            testButtons->addWidget(b);
        testButtons->addStretch(1);
        tt->addLayout(testButtons, 1, 0, 1, 5);
        vl->addWidget(testBox);

        auto *pointButtons = new QHBoxLayout;
        auto *playAll = new QPushButton("Start all points");
        auto *playPointBtn = new QPushButton("Start this point");
        auto *stopAll = new QPushButton("Stop all");
        auto *resetAll = new QPushButton("All points back to rest");
        for (auto *b : {playAll, playPointBtn, stopAll, resetAll})
            pointButtons->addWidget(b);
        pointButtons->addStretch(1);
        vl->addLayout(pointButtons);

        hint = new QLabel;
        hint->setWordWrap(true);
        vl->addWidget(hint);
        vl->addStretch(1);

        auto *visualScroll = new QScrollArea;
        visualScroll->setWidgetResizable(true);
        visualScroll->setFrameShape(QFrame::NoFrame);
        visualScroll->setWidget(visual);
        tabs->addTab(visualScroll, "Visual");

        // ============================ TABLE ============================
        auto *tablePage = new QWidget;
        auto *tl = new QVBoxLayout(tablePage);
        table = new QTableWidget(paramCount, 7);
        table->setHorizontalHeaderLabels(
            {"Parameter", "Current", "GPU value", "Target", "Progress", "State", "Easing"});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->setCurrentCell(4, 0);
        tl->addWidget(table, 1);
        auto *control = new QHBoxLayout;
        value = new QDoubleSpinBox;
        value->setRange(-100, 100);
        value->setDecimals(4);
        value->setValue(1);
        control->addWidget(new QLabel("Value / delta"));
        control->addWidget(value);
        duration = new QDoubleSpinBox;
        duration->setRange(0, 3600000);
        duration->setSuffix(" ms");
        duration->setValue(1000);
        control->addWidget(duration);
        tableEasing = new QComboBox;
        for (auto *n : easingNames)
            tableEasing->addItem(n);
        tableEasing->setCurrentIndex(5);
        control->addWidget(tableEasing);
        tl->addLayout(control);
        returnToBase = new QCheckBox("Auto return to the configured return value");
        returnToBase->setChecked(true);
        tl->addWidget(returnToBase);
        auto *buttons = new QHBoxLayout;
        for (const char *action :
             {"Start", "Add", "Set", "Reset", "Stop", "StartAll", "StopAll", "ResetAll"}) {
            auto *b = new QPushButton(action);
            buttons->addWidget(b);
            connect(b, &QPushButton::clicked, this, [this, action] { run(action); });
        }
        tl->addLayout(buttons);
        tabs->addTab(tablePage, "Table (advanced)");

        // ============================ LOG ============================
        auto *logPage = new QWidget;
        auto *ll = new QVBoxLayout(logPage);
        metrics = new QLabel("No filter selected");
        metrics->setWordWrap(true);
        ll->addWidget(metrics);
        auto *debug = new QHBoxLayout;
        logFilter = new QLineEdit;
        logFilter->setPlaceholderText("Filter command log (point1, Add, Stop...)");
        debug->addWidget(logFilter);
        pause = new QCheckBox("Pause log");
        debug->addWidget(pause);
        auto *clear = new QPushButton("Clear");
        debug->addWidget(clear);
        auto *copy = new QPushButton("Copy");
        debug->addWidget(copy);
        ll->addLayout(debug);
        logs = new QPlainTextEdit;
        logs->setReadOnly(true);
        logs->setMaximumBlockCount(300);
        ll->addWidget(logs, 1);
        tabs->addTab(logPage, "Log");

        // ============================ JSON ============================
        // One ready-to-paste CallVendorRequest per point, built from the selected source + filter
        // and each point's saved move duration / easing / auto-return settings.
        auto *jsonPage = new QWidget;
        auto *jl = new QVBoxLayout(jsonPage);
        auto *jsonTop = new QHBoxLayout;
        jsonAction = new QComboBox;
        jsonAction->addItems({"Add", "Set", "Start", "Reset", "Stop"});
        jsonAction->setToolTip("Vendor requestType written into every message.");
        jsonTop->addWidget(new QLabel("Action"));
        jsonTop->addWidget(jsonAction);
        jsonParam = new QComboBox;
        for (const char *n : {"Enable", "Offset X", "Offset Y", "Radius", "Magnitude"})
            jsonParam->addItem(n);
        jsonParam->setCurrentIndex(4);
        jsonParam->setToolTip("Which of the five per-point parameters the messages animate.");
        jsonTop->addWidget(new QLabel("Parameter"));
        jsonTop->addWidget(jsonParam);
        jsonValue = new QDoubleSpinBox;
        jsonValue->setRange(-100, 100);
        jsonValue->setDecimals(4);
        jsonValue->setSingleStep(0.05);
        jsonValue->setValue(0.3);
        jsonValue->setFixedWidth(96);
        jsonValue->setToolTip("Delta for Add / absolute value for Set; unused by Start, Stop, Reset.");
        jsonTop->addWidget(new QLabel("Value / delta"));
        jsonTop->addWidget(jsonValue);
        jsonFormat = new QComboBox;
        jsonFormat->addItems({"Vendor request (d)", "Full message (op 6)"});
        jsonFormat->setToolTip("Vendor request (d): the inner CallVendorRequest object that "
                               "Streamer.bot accepts as-is (default, no op / requestId). Full "
                               "message: the whole op:6 OBS WebSocket message.");
        jsonTop->addWidget(new QLabel("Format"));
        jsonTop->addWidget(jsonFormat);
        jsonTop->addStretch(1);
        jl->addLayout(jsonTop);
        auto *jsonNote = new QLabel(
            "One message per point, using the selected source + filter and each point's saved "
            "move duration, easing and auto-return (returnToZero). Edit any control to regenerate, "
            "then press Copy to grab a single point or Copy all points for all three.");
        jsonNote->setWordWrap(true);
        jl->addWidget(jsonNote);
        for (int p = 0; p < pointCount; ++p) {
            auto *group = new QGroupBox(QString("%1 (point%2)")
                                            .arg(QString::fromLatin1(pointName(p)))
                                            .arg(p + 1));
            auto *gv = new QVBoxLayout(group);
            auto *row = new QHBoxLayout;
            jsonEdits[p] = new QPlainTextEdit;
            jsonEdits[p]->setReadOnly(true);
            jsonEdits[p]->setMinimumHeight(150);
            row->addWidget(jsonEdits[p], 1);
            auto *copyJson = new QPushButton("Copy");
            copyJson->setToolTip("Copy this point's message to the clipboard.");
            row->addWidget(copyJson);
            connect(copyJson, &QPushButton::clicked, this,
                    [this, p] { QApplication::clipboard()->setText(jsonEdits[p]->toPlainText()); });
            gv->addLayout(row);
            jl->addWidget(group, 1);
        }
        auto *jsonButtons = new QHBoxLayout;
        auto *regenJson = new QPushButton("Regenerate");
        auto *copyAllJson = new QPushButton("Copy all points");
        jsonButtons->addWidget(regenJson);
        jsonButtons->addWidget(copyAllJson);
        jsonButtons->addStretch(1);
        jl->addLayout(jsonButtons);
        tabs->addTab(jsonPage, "JSON");

        // ============================ IMPORT / EXPORT ============================
        // The whole automation state as plain JSON text: copy it out to share settings, or paste
        // settings from another filter / machine and press Apply.
        auto *presetPage = new QWidget;
        auto *pl = new QVBoxLayout(presetPage);
        auto *presetNote = new QLabel(
            "Copy these automation settings to share them, or paste settings copied from another "
            "filter and press Apply. The text is plain JSON, one flat object with the plugin's "
            "setting names (mode, point1_offset_x, point1_magnitude, ...).");
        presetNote->setWordWrap(true);
        pl->addWidget(presetNote);
        presetEdit = new QPlainTextEdit;
        presetEdit->setPlaceholderText("Paste settings JSON here, then press Apply.");
        presetEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        pl->addWidget(presetEdit, 1);
        auto *presetButtons = new QHBoxLayout;
        auto *presetExport = new QPushButton("Export from filter");
        presetExport->setToolTip("Fill the box with the selected filter's current settings.");
        auto *presetCopy = new QPushButton("Copy");
        presetCopy->setToolTip("Copy the text to the clipboard.");
        auto *presetPaste = new QPushButton("Paste");
        presetPaste->setToolTip("Replace the text with the clipboard contents.");
        auto *presetApply = new QPushButton("Apply");
        presetApply->setToolTip("Parse the text and write it into the selected filter.");
        presetButtons->addWidget(presetExport);
        presetButtons->addWidget(presetCopy);
        presetButtons->addWidget(presetPaste);
        presetButtons->addWidget(presetApply);
        presetButtons->addStretch(1);
        pl->addLayout(presetButtons);
        presetStatus = new QLabel;
        presetStatus->setWordWrap(true);
        pl->addWidget(presetStatus);
        tabs->addTab(presetPage, "Import / Export");

        // ============================ WIRING ============================
        connect(clear, &QPushButton::clicked, this, [this] {
            if (auto e = selected.lock()) {
                std::lock_guard lock(e->mutex);
                e->logs.clear();
            }
            logs->clear();
        });
        connect(copy, &QPushButton::clicked, this,
                [this] { QApplication::clipboard()->setText(logs->toPlainText()); });
        connect(refreshBtn, &QPushButton::clicked, this, [this] { reload(); });
        connect(jsonAction, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { generateJson(); });
        connect(jsonParam, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { generateJson(); });
        connect(jsonFormat, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { generateJson(); });
        connect(jsonValue, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { generateJson(); });
        connect(regenJson, &QPushButton::clicked, this, [this] { generateJson(); });
        connect(copyAllJson, &QPushButton::clicked, this, [this] {
            QStringList parts;
            for (auto *e : jsonEdits)
                if (e && !e->toPlainText().isEmpty())
                    parts << e->toPlainText();
            QApplication::clipboard()->setText(parts.join("\n"));
        });
        connect(presetExport, &QPushButton::clicked, this, [this] {
            fillPresetEdit();
            presetStatus->setText("Exported the selected filter's settings.");
        });
        connect(presetCopy, &QPushButton::clicked, this,
                [this] { QApplication::clipboard()->setText(presetEdit->toPlainText()); });
        connect(presetPaste, &QPushButton::clicked, this,
                [this] { presetEdit->setPlainText(QApplication::clipboard()->text()); });
        connect(presetApply, &QPushButton::clicked, this, [this] {
            Preset p;
            QString error;
            if (!presetFromJson(presetEdit->toPlainText(), p, &error)) {
                presetStatus->setText("Import failed: " + error);
                return;
            }
            applyPreset(p);
            presetStatus->setText("Settings applied to the selected filter.");
        });
        connect(filters, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { select(); });
        connect(pushTimer, &QTimer::timeout, this, [this] { pushSet(); });
        connect(pointBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (!updating)
                pull();
        });
        connect(enabledBox, &QCheckBox::toggled, this, [this](bool on) {
            if (updating)
                return;
            points[pointBox->currentIndex()].enabled = on;
            pushLater();
        });
        connect(faceBox, &QCheckBox::toggled, this, [this](bool on) {
            if (updating)
                return;
            obs_source_t *f = resolve();
            if (!f)
                return;
            obs_data_t *s = obs_source_get_settings(f);
            obs_data_set_bool(s, "face_tracking", on);
            obs_source_update(f, s);
            obs_data_release(s);
            obs_source_release(f);
        });
        connect(smoothSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            if (updating)
                return;
            obs_source_t *f = resolve();
            if (!f)
                return;
            obs_data_t *s = obs_source_get_settings(f);
            obs_data_set_double(s, "face_smooth_ms", v);
            obs_source_update(f, s);
            obs_data_release(s);
            obs_source_release(f);
        });
        connect(modeBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int m) {
            if (updating)
                return;
            obs_source_t *f = resolve();
            if (!f)
                return;
            obs_data_t *s = obs_source_get_settings(f);
            obs_data_set_int(s, "mode", m);
            obs_source_update(f, s);
            obs_data_release(s);
            obs_source_release(f);
        });
        auto setBool = [this](const char *key, bool on) {
            if (updating)
                return;
            obs_source_t *f = resolve();
            if (!f)
                return;
            obs_data_t *s = obs_source_get_settings(f);
            obs_data_set_bool(s, key, on);
            obs_source_update(f, s);
            obs_data_release(s);
            obs_source_release(f);
        };
        connect(blurBox, &QCheckBox::toggled, this, [setBool](bool on) { setBool("effect_blur", on); });
        connect(debugBox, &QCheckBox::toggled, this, [setBool](bool on) { setBool("effect_debug", on); });
        connect(scaleBox, &QCheckBox::toggled, this, [setBool](bool on) { setBool("face_scale", on); });
        connect(blurPxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            if (updating)
                return;
            obs_source_t *f = resolve();
            if (!f)
                return;
            obs_data_t *s = obs_source_get_settings(f);
            obs_data_set_double(s, "face_blur_px", v);
            obs_source_update(f, s);
            obs_data_release(s);
            obs_source_release(f);
        });
        auto setter = [this](int which, double v) {
            auto &a = points[pointBox->currentIndex()];
            if (which == 0)
                a.cx = v;
            else if (which == 1)
                a.cy = v;
            else if (which == 2)
                a.radius = v;
            else
                a.magnitude = v;
            // Mirror the number into the paired control so the slider and the box never disagree.
            setPair(which, v);
            pushLater();
        };
        // The offset / radius sliders are scaled by 10 and the magnitude slider by 1000, so divide
        // the raw slider step back into value units here. setPair() uses the same scales.
        connect(cxSld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(0, v / 10.0); });
        connect(cxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(0, v); });
        connect(cySld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(1, v / 10.0); });
        connect(cySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(1, v); });
        connect(radSld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(2, v / 10.0); });
        connect(radSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(2, v); });
        connect(magSld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(3, v / 1000.0); });
        connect(magSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(3, v); });
        // "Reset to 0": put this point's magnitude back to its resting value (clamped to the point's
        // configured min / max, in case 0 sits outside them).
        connect(magZero, &QPushButton::clicked, this, [this] {
            auto &a = points[pointBox->currentIndex()];
            a.magnitude = std::clamp(0.0, a.magMin, a.magMax);
            setPair(3, a.magnitude);
            pushLater();
        });
        // Magnitude min / max define the per-point clamp. Keep max > min inside the spin range,
        // re-clamp the stored magnitude, then refresh the slider / spin ranges to match.
        connect(magMinSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            if (updating)
                return;
            auto &a = points[pointBox->currentIndex()];
            a.magMin = std::min(v, 99.999);
            if (!(a.magMax > a.magMin))
                a.magMax = a.magMin + 1e-3;
            a.magnitude = std::clamp(a.magnitude, a.magMin, a.magMax);
            syncControls();
            pushLater();
        });
        connect(magMaxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
            if (updating)
                return;
            auto &a = points[pointBox->currentIndex()];
            a.magMax = std::max(v, -99.999);
            if (!(a.magMax > a.magMin))
                a.magMin = a.magMax - 1e-3;
            a.magnitude = std::clamp(a.magnitude, a.magMin, a.magMax);
            syncControls();
            pushLater();
        });
        connect(applyTiming, &QPushButton::clicked, this, [this] { writeTiming(false); });
        connect(applyTimingAll, &QPushButton::clicked, this, [this] { writeTiming(true); });
        connect(addBtn, &QPushButton::clicked, this, [this] { test("Add"); });
        connect(setBtn, &QPushButton::clicked, this, [this] { test("Set"); });
        connect(startBtn, &QPushButton::clicked, this, [this] { test("Start"); });
        connect(stopBtn, &QPushButton::clicked, this, [this] { test("Stop"); });
        connect(resetBtn, &QPushButton::clicked, this, [this] { test("Reset"); });
        connect(playAll, &QPushButton::clicked, this, [this] { run("StartAll"); });
        connect(playPointBtn, &QPushButton::clicked, this, [this] { playPoint(); });
        connect(stopAll, &QPushButton::clicked, this, [this] { run("StopAll"); });
        connect(resetAll, &QPushButton::clicked, this, [this] { run("ResetAll"); });

        auto *timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start();

        auto *scan = new QTimer(this);
        scan->setInterval(2000);
        connect(scan, &QTimer::timeout, this, [this] {
            auto byUuid = [](const FilterEntry &a, const FilterEntry &b) {
                return a.filterUuid < b.filterUuid;
            };
            auto now = enumerateFilters();
            auto have = entries;
            std::sort(now.begin(), now.end(), byUuid);
            std::sort(have.begin(), have.end(), byUuid);
            bool same = now.size() == have.size();
            for (size_t i = 0; same && i < now.size(); ++i)
                same = now[i].filterUuid == have[i].filterUuid && now[i].filter == have[i].filter &&
                       now[i].source == have[i].source;
            if (!same)
                reload();
        });
        scan->start();
        reload();
    }
};

QWidget *createAnimatorPanel(QWidget *parent) {
    return new AnimatorPanel(parent);
}
