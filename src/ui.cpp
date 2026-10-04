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
    }
    int testIndex() const { return pointBox->currentIndex() * pointParamCount + testParam->currentIndex(); }
    void test(const std::string &action) {
        try {
            auto e = selected.lock();
            if (!e)
                throw std::runtime_error(
                    "No filter selected. Add the Face Points filter to a source, then press Refresh.");
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
            QMessageBox::warning(this, "Parameter Animator", ex.what());
        }
        refresh();
    }
    void run(const std::string &action) {
        try {
            auto e = selected.lock();
            if (!e)
                throw std::runtime_error(
                    "No filter selected. Add the Face Points filter to a source, then press Refresh.");
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
            QMessageBox::warning(this, "Parameter Animator", ex.what());
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
            QMessageBox::warning(this, "Parameter Animator", ex.what());
        }
        refresh();
    }

    void refresh() {
        if (!isVisible())
            return;
        auto e = selected.lock();
        if (!e) {
            const QString msg = QStringLiteral(
                "No filter selected. Add the Face Points filter to a source, then press Refresh.");
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

    void syncControls() {
        updating = true;
        const int z = pointBox->currentIndex();
        const auto &v = points[z];
        cxSpin->setValue(v.cx);
        cySpin->setValue(v.cy);
        radSpin->setValue(v.radius);
        magSpin->setValue(v.magnitude);
        cxSld->setValue((int)std::lround(v.cx));
        cySld->setValue((int)std::lround(v.cy));
        radSld->setValue((int)std::lround(v.radius));
        magSld->setValue((int)std::lround(v.magnitude * 1000));
        enabledBox->setChecked(v.enabled);
        const QString hintText = QStringLiteral(
            "Positions come from face detection (eyes / nose / mouth). Offset nudges "
            "the point (percent), radius is percent of the frame height, magnitude is the distortion "
            "strength. Add/Set/Start animate the selected point on every detected face.");
        if (hint->text() != hintText)
            hint->setText(hintText);
        updating = false;
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
        vl->setSpacing(6);

        auto *modeRow = new QHBoxLayout;
        modeRow->addWidget(new QLabel("Mode:"));
        modeBox = new QComboBox;
        modeBox->addItems({"Static", "Shader sine animation", "Plugin animation"});
        modeRow->addWidget(modeBox);
        modeRow->addStretch(1);
        vl->addLayout(modeRow);

        // Blur and debug are independent of the point distortion, so they are plain checkboxes.
        auto *renderRow = new QHBoxLayout;
        blurBox = new QCheckBox("Blur faces");
        blurBox->setToolTip("Blur the whole detected face box (covers/anonimises the face). "
                            "Independent of the distortion.");
        debugBox = new QCheckBox("Debug points");
        debugBox->setToolTip("Overlay the detection box and the three point anchors: red = eyes, "
                             "green = nose, blue = mouth. Independent of the distortion.");
        blurPxSpin = new QDoubleSpinBox;
        blurPxSpin->setRange(2, 128);
        blurPxSpin->setSuffix(" px");
        blurPxSpin->setValue(24);
        blurPxSpin->setFixedWidth(78);
        blurPxSpin->setToolTip("Blur radius in pixels.");
        renderRow->addWidget(blurBox);
        renderRow->addWidget(blurPxSpin);
        renderRow->addSpacing(8);
        renderRow->addWidget(debugBox);
        renderRow->addStretch(1);
        vl->addLayout(renderRow);

        auto *scaleRow = new QHBoxLayout;
        scaleBox = new QCheckBox("Scale points to face size");
        scaleBox->setToolTip("Radius and offsets are relative to the detected face height, so a "
                            "distant face gets proportionally smaller points.");
        scaleRow->addWidget(scaleBox);
        scaleRow->addStretch(1);
        vl->addLayout(scaleRow);

        auto *faceRow = new QHBoxLayout;
        faceBox = new QCheckBox("Track faces");
        faceRow->addWidget(faceBox);
        faceRow->addWidget(new QLabel("smoothing"));
        smoothSpin = new QDoubleSpinBox;
        smoothSpin->setRange(0, 1000);
        smoothSpin->setSuffix(" ms");
        smoothSpin->setValue(120);
        smoothSpin->setFixedWidth(80);
        smoothSpin->setToolTip("Ease the points toward each detection over this time. 0 = no "
                               "smoothing (raw, jittery); higher = smoother but slower to follow.");
        faceRow->addWidget(smoothSpin);
        faceRow->addStretch(1);
        vl->addLayout(faceRow);
        faceStatus = new QLabel("Tracking off");
        faceStatus->setWordWrap(true);
        vl->addWidget(faceStatus);

        auto *prow = new QHBoxLayout;
        prow->addWidget(new QLabel("Point:"));
        pointBox = new QComboBox;
        for (int p = 0; p < pointCount; ++p)
            pointBox->addItem(pointName(p));
        prow->addWidget(pointBox);
        enabledBox = new QCheckBox("enabled");
        prow->addWidget(enabledBox);
        prow->addStretch(1);
        vl->addLayout(prow);

        auto *grid = new QGridLayout;
        grid->setVerticalSpacing(2);
        grid->setHorizontalSpacing(6);
        auto addRow = [&](int r, const QString &name, QSlider *&sl, QDoubleSpinBox *&sp, double lo,
                          double hi, double step, bool scaled) {
            grid->addWidget(new QLabel(name), r, 0);
            sl = new QSlider(Qt::Horizontal);
            sl->setRange(scaled ? (int)std::lround(lo * 1000) : (int)lo,
                         scaled ? (int)std::lround(hi * 1000) : (int)hi);
            grid->addWidget(sl, r, 1);
            sp = new QDoubleSpinBox;
            sp->setRange(lo, hi);
            sp->setSingleStep(step);
            sp->setDecimals(scaled ? 3 : 1);
            sp->setFixedWidth(92);
            grid->addWidget(sp, r, 2);
        };
        addRow(0, "Offset X", cxSld, cxSpin, -100, 100, 0.5, false);
        addRow(1, "Offset Y", cySld, cySpin, -100, 100, 0.5, false);
        addRow(2, "Radius", radSld, radSpin, 0, 100, 0.5, false);
        addRow(3, "Magnitude", magSld, magSpin, -1.3333, 1.3333, 0.05, true);
        vl->addLayout(grid);

        // ---- timing: outgoing move, then the automatic return after the last Add ----
        auto *timing = new QGroupBox("Duration and return");
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
        auto *testBox = new QGroupBox("Test");
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
        auto *playAll = new QPushButton("Play all points");
        auto *playPointBtn = new QPushButton("Play this point");
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
            pushLater();
        };
        connect(cxSld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(0, v); });
        connect(cxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(0, v); });
        connect(cySld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(1, v); });
        connect(cySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(1, v); });
        connect(radSld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(2, v); });
        connect(radSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(2, v); });
        connect(magSld, &QSlider::valueChanged, this, [this, setter](int v) { if (!updating) setter(3, v / 1000.0); });
        connect(magSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, setter](double v) { if (!updating) setter(3, v); });
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
