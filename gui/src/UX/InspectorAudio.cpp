#include "UX/InspectorAudio.hpp"

#include "UX/InspectorShared.hpp"
#include "UX/MainWindow.hpp"

#include "canvas/core/timeline/audio_mix.hpp"
#include "canvas/core/timeline/audio_processing.hpp"
#include "canvas/core/timeline/edit_ops.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "Widgets/timeline_widget.hpp"
#include "features/timeline/audio_targets.hpp"

namespace canvas::gui {

namespace {

// ── EQ response graph ─────────────────────────────────────────────────────
// A hand-painted frequency-response plot: x = Hz (log), y = dB (-24..+24).
// Only a visual reference — spin boxes drive the band values, the curve is a
// smooth path through the six band-gain points (shelves/rolloffs simplified as
// waypoint interpolation).
class EqGraphWidget final : public QWidget {
public:
    explicit EqGraphWidget(QWidget* parent = nullptr) : QWidget(parent) {
        bands_ = canvas::core::Clip::default_eq_bands();
        setMinimumHeight(140);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        apply_theme_style(this, [] {
            const ThemeTokens& t = tokens();
            return QStringLiteral("background-color: %1; border: 1px solid %2;"
                                  " border-radius: 8px;")
                .arg(css(t.surface_low), css(t.border_soft));
        });
    }

    void set_bands(const std::array<canvas::core::Clip::EqBand, 6>& bands) {
        bands_ = bands;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = rect().adjusted(6, 6, -6, -6);
        constexpr double kMinDb = -24.0;
        constexpr double kMaxDb = 24.0;
        constexpr double kMinHz = 20.0;
        constexpr double kMaxHz = 20000.0;

        const auto x_for = [&](double hz) {
            const double lg = std::log(hz / kMinHz) / std::log(kMaxHz / kMinHz);
            return r.left() + lg * r.width();
        };
        const auto y_for = [&](double db) {
            const double f = (std::clamp(db, kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb);
            return r.bottom() - f * r.height();
        };

        // Grid: reference Hz ticks + 0 dB axis.
        const ThemeTokens& t = tokens();
        p.setPen(QPen(t.border_soft, 1));
        for (const double hz : {62.0, 250.0, 1000.0, 4000.0, 16000.0}) {
            const double x = x_for(hz);
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        p.setPen(QPen(t.border, 1));
        p.drawLine(QPointF(r.left(), y_for(0.0)), QPointF(r.right(), y_for(0.0)));

        // Axes labels.
        p.setPen(t.ink_faint);
        p.setFont(QFont(QStringLiteral("DejaVu Sans"), 7));
        p.drawText(QPointF(r.left() + 1, r.bottom() - 1), QStringLiteral("20"));
        p.drawText(QPointF(r.right() - 14, r.bottom() - 1), QStringLiteral("20K"));
        p.drawText(QPointF(r.left() + 1, y_for(-24.0) + 6), QStringLiteral("-24"));
        p.drawText(QPointF(r.left() + 1, y_for(24.0) - 2), QStringLiteral("+24"));

        // Response curve through the six bands.
        QPainterPath path;
        bool first = true;
        for (const auto& b : bands_) {
            const QPointF pt(x_for(b.frequency), y_for(b.gain));
            if (first) {
                path.moveTo(pt);
                first = false;
            } else {
                path.lineTo(pt);
            }
        }
        p.setPen(QPen(t.accent, 2));
        p.drawPath(path);

        // Band markers.
        p.setBrush(t.accent);
        p.setPen(Qt::NoPen);
        for (const auto& b : bands_)
            p.drawEllipse(QPointF(x_for(b.frequency), y_for(b.gain)), 3.0, 3.0);
    }

private:
    std::array<canvas::core::Clip::EqBand, 6> bands_{};
};

// ── Slider + numeric spin composite row ───────────────────────────────────
QWidget* make_slider_spin(double min, double max, int decimals, QWidget* parent,
                          QSlider** out_slider, QDoubleSpinBox** out_spin) {
    auto* host = new QWidget(parent);
    auto* lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    auto* slider = new QSlider(Qt::Horizontal, host);
    slider->setRange(0, 10000);
    // Don't let the slider's default minimum width (≈84px) force the row wider
    // than the inspector dock — the label/spin/reset on the right must stay
    // visible at any dock width.
    slider->setMinimumWidth(0);
    auto* spin = new QDoubleSpinBox(host);
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setMaximumWidth(74);
    spin->setKeyboardTracking(false);

    const auto spin_to_slider = [slider, min, max](double v) {
        slider->setValue(static_cast<int>(std::lround((v - min) / (max - min) * 10000.0)));
    };
    const auto slider_to_spin = [spin, min, max](int v) {
        spin->setValue(min + (max - min) * static_cast<double>(v) / 10000.0);
    };
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, host, spin_to_slider);
    QObject::connect(slider, &QSlider::valueChanged, host, slider_to_spin);

    QObject::connect(spin, &QDoubleSpinBox::editingFinished, host,
                     [spin, slider, min, max]() {
                         spin->setValue(min + (max - min) *
                                            static_cast<double>(slider->value()) / 10000.0);
                     });

    // Seed the knob to match the spin's initial value. The spin starts at its
    // built-in default (0.0) and the slider at its own default (0 = far-left on
    // the 0..10000 range), so without this they disagree until the user touches
    // the spin — e.g. a -100..+100 dB volume row would show 0.00 with the knob
    // hard against the left end instead of centered.
    spin_to_slider(spin->value());

    lay->addWidget(slider, 1);
    lay->addWidget(spin);
    if (out_slider) *out_slider = slider;
    if (out_spin) *out_spin = spin;
    return host;
}

QComboBox* make_dark_combo(QWidget* parent) {
    auto* cb = new QComboBox(parent);
    apply_theme_style(cb, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
                   "QComboBox { background-color: %1; color: %2; border: 1px solid %3;"
                   "  border-radius: 8px; padding: 3px 8px; font-size: 11px; }"
                   "QComboBox::drop-down { border: none; width: 14px; }"
                   "QComboBox QAbstractItemView { background-color: %4; color: %2;"
                   "  selection-background-color: %5; border: 1px solid %3;"
                   "  border-radius: 8px; padding: 2px; }")
            .arg(css(t.surface_higher), css(t.ink), css(t.border), css(t.surface_low),
                 css(t.accent));
    });
    return cb;
}

QDoubleSpinBox* make_band_spin(double lo, double hi, int decimals, double val, QWidget* parent,
                               int width) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(val);
    s->setDecimals(decimals);
    s->setMaximumWidth(width);
    s->setKeyboardTracking(false);
    apply_theme_style(s, [] {
        const ThemeTokens& t = tokens();
        return QStringLiteral(
                   "QDoubleSpinBox { background-color: %1; color: %2; border: 1px solid %3;"
                   "  border-radius: 8px; padding: 3px 8px; font-size: 11px; }")
            .arg(css(t.surface_higher), css(t.ink), css(t.border));
    });
    return s;
}

// ── Control registry ──────────────────────────────────────────────────────
struct AudioControls {
    QDoubleSpinBox* volume = nullptr;
    QSlider* volume_slider = nullptr;
    QDoubleSpinBox* pan = nullptr;
    QSlider* pan_slider = nullptr;
    QLabel* multi_hint = nullptr;

    QSlider* pitch_semi_slider = nullptr;
    QDoubleSpinBox* pitch_semi = nullptr;
    QSlider* pitch_cents_slider = nullptr;
    QDoubleSpinBox* pitch_cents = nullptr;

    InspectorCategory* speed_cat = nullptr;
    QSlider* speed_slider = nullptr;
    QDoubleSpinBox* speed_factor = nullptr;

    InspectorCategory* eq_cat = nullptr;
    EqGraphWidget* eq_graph = nullptr;
    std::vector<QDoubleSpinBox*> eq_freq;
    std::vector<QDoubleSpinBox*> eq_gain;
    std::vector<QDoubleSpinBox*> eq_q;
    std::vector<QComboBox*> eq_type;

    // AI Voice Isolation: per-clip engine picker. Backed by the real model
    // field (None / RNNoise / DeepFilterNet) and applied before the clip's
    // gains/mix in both playback and export. Engine entries whose backend is
    // not compiled in (voice_isolation_supported()==false, i.e. DeepFilterNet)
    // are listed but disabled.
    InspectorCategory* iso_cat = nullptr;
    QComboBox* iso_combo = nullptr;

    InspectorCategory* ai_leveler = nullptr;
    InspectorCategory* ai_remix = nullptr;
    QDoubleSpinBox* ai_amount = nullptr;

    QToolButton* mode_button = nullptr;
    bool updating = false;   // guards against commit/re-sync during refresh
    bool attached = false;   // selection signals already connected
};

std::map<MainWindow*, AudioControls>& audio_registry() {
    static std::map<MainWindow*, AudioControls> reg;
    return reg;
}

AudioControls* audio_lookup(MainWindow& mw) {
    const auto it = audio_registry().find(&mw);
    return it == audio_registry().end() ? nullptr : &it->second;
}

void set_processing_enabled(AudioControls& ac, bool on) {
    for (QDoubleSpinBox* s : {ac.pitch_semi, ac.pitch_cents, ac.speed_factor})
        if (s) s->setEnabled(on);
    for (QSlider* s : {ac.pitch_semi_slider, ac.pitch_cents_slider, ac.speed_slider})
        if (s) s->setEnabled(on);
    for (QDoubleSpinBox* s : ac.eq_freq) s->setEnabled(on);
    for (QDoubleSpinBox* s : ac.eq_gain) s->setEnabled(on);
    for (QDoubleSpinBox* s : ac.eq_q) s->setEnabled(on);
    for (QComboBox* c : ac.eq_type) c->setEnabled(on);
    if (ac.iso_combo) ac.iso_combo->setEnabled(on);
    for (InspectorCategory* cat : {ac.speed_cat, ac.eq_cat, ac.iso_cat})
        if (cat) {
            cat->set_feature_toggle_enabled(on);
            cat->setEnabled(on);
        }
}

// Reads a freshly-found selected audio clip into the widgets (no commit).
void populate_from_clip(AudioControls& ac, const canvas::core::Clip& clip) {
    ac.updating = true;
    if (ac.volume) ac.volume->setValue(clip.volume_db);
    if (ac.pan) ac.pan->setValue(clip.pan);
    if (ac.pitch_semi) ac.pitch_semi->setValue(clip.pitch_semitones);
    if (ac.pitch_cents) ac.pitch_cents->setValue(clip.pitch_cents);
    if (ac.speed_factor) ac.speed_factor->setValue(clip.speed_factor);
    if (ac.speed_cat) ac.speed_cat->set_feature_enabled(clip.speed_enabled);
    if (ac.eq_cat) ac.eq_cat->set_feature_enabled(clip.eq_enabled);
    for (std::size_t i = 0; i < clip.eq_bands.size(); ++i) {
        const auto& b = clip.eq_bands[i];
        if (i < ac.eq_type.size() && ac.eq_type[i])
            ac.eq_type[i]->setCurrentIndex(static_cast<int>(b.type));
        if (i < ac.eq_freq.size() && ac.eq_freq[i]) ac.eq_freq[i]->setValue(b.frequency);
        if (i < ac.eq_gain.size() && ac.eq_gain[i]) ac.eq_gain[i]->setValue(b.gain);
        if (i < ac.eq_q.size() && ac.eq_q[i]) ac.eq_q[i]->setValue(b.q);
    }
    if (ac.eq_graph) ac.eq_graph->set_bands(clip.eq_bands);
    if (ac.iso_combo)
        ac.iso_combo->setCurrentIndex(static_cast<int>(clip.voice_isolation));
    ac.updating = false;
}

}  // namespace

void build_inspector_audio(MainWindow& mw, QVBoxLayout* audio_layout,
                           QToolButton* audio_mode_button) {
    AudioControls& ac = audio_registry()[&mw];
    ac.mode_button = audio_mode_button;
    auto* host = audio_layout->parentWidget();
    const auto tr = [&](const char* s) { return MainWindow::tr(s); };

    // --- Audio (Volume / Pan) ------------------------------------------------
    auto* audio = new InspectorCategory(tr("Audio"), true, host);
    {
        QSlider* vol_slider = nullptr;
        QDoubleSpinBox* vol_spin = nullptr;
        auto* vol_row = make_slider_spin(canvas::core::audio_mix::kVolumeDbSliderMin,
                                         canvas::core::audio_mix::kVolumeDbSliderMax,
                                         1, host, &vol_slider, &vol_spin);
        ac.volume = vol_spin;
        ac.volume_slider = vol_slider;
        vol_spin->setSuffix(QStringLiteral(" dB"));
        vol_spin->setMaximumWidth(110);
        // Drag the slider left to lower, right to raise; commit once the drag
        // releases so playback keeps streaming between drag steps.
        QObject::connect(vol_slider, &QSlider::sliderReleased, &mw,
                         [&mw]() { mw.apply_inspector_audio(); });
        // Live spectrum feedback: every drag/typing step re-renders the selected
        // clip's waveform at the knob's gain (no undo/commit per step); the
        // release handler above still records the one real volume edit.
        QObject::connect(vol_spin, &QDoubleSpinBox::valueChanged, &mw,
                         [&mw](double vol_db) { mw.preview_inspector_volume(static_cast<float>(vol_db)); });
        add_property_row(audio->body_layout(), tr("Volume (dB)"), vol_row);
    }
    QSlider* pan_slider = nullptr;
    QDoubleSpinBox* pan_spin = nullptr;
    auto* pan_row = make_slider_spin(canvas::core::audio_mix::kPanMin,
                                     canvas::core::audio_mix::kPanMax,
                                     2, host, &pan_slider, &pan_spin);
    ac.pan = pan_spin;
    ac.pan_slider = pan_slider;
    pan_spin->setSuffix(QStringLiteral(" L/R"));
    pan_spin->setMaximumWidth(110);
    // Drag right to pan right, left to pan left; commit once the drag releases
    // so playback keeps streaming between drag steps.
    QObject::connect(pan_slider, &QSlider::sliderReleased, &mw,
                     [&mw]() { mw.apply_inspector_audio(); });
    add_property_row(audio->body_layout(), tr("Pan"), pan_row);
    {
        // Multi-clip selection hint: Volume/Pan edits below apply to every
        // selected audio clip (Phase 4). Hidden for single-clip selections.
        auto* hint = new QLabel(host);
        apply_theme_style(hint, [] {
            return QStringLiteral("color: %1; font-size: 10px; padding: 0 4px;")
                .arg(css(tokens().ink_muted));
        });
        hint->setWordWrap(true);
        hint->setVisible(false);
        ac.multi_hint = hint;
        audio->body_layout()->addWidget(hint);
    }
    audio_layout->addWidget(audio);
    audio_layout->addSpacing(2);
    // Keep MainWindow's legacy mix spins pointing at these so the pre-split
    // apply_inspector_audio() (volume/pan commit path) keeps working unchanged.
    mw.inspector_audio_volume_ = ac.volume;
    mw.inspector_audio_pan_ = ac.pan;

    // --- Pitch ----------------------------------------------------------------
    auto* pitch = new InspectorCategory(tr("Pitch"), true, host);
    QSlider* s1 = nullptr;
    QDoubleSpinBox* sp1 = nullptr;
    auto* semi_row = make_slider_spin(canvas::core::audio_processing::kPitchSemitonesMin,
                                      canvas::core::audio_processing::kPitchSemitonesMax,
                                      0, host, &s1, &sp1);
    ac.pitch_semi_slider = s1;
    ac.pitch_semi = sp1;
    ac.pitch_semi->setSuffix(QStringLiteral(" st"));
    add_property_row(pitch->body_layout(), tr("Semi Tones"), semi_row);
    QSlider* s2 = nullptr;
    QDoubleSpinBox* sp2 = nullptr;
    auto* cents_row = make_slider_spin(canvas::core::audio_processing::kPitchCentsMin,
                                       canvas::core::audio_processing::kPitchCentsMax,
                                       0, host, &s2, &sp2);
    ac.pitch_cents_slider = s2;
    ac.pitch_cents = sp2;
    ac.pitch_cents->setSuffix(QStringLiteral(" ct"));
    add_property_row(pitch->body_layout(), tr("Cents"), cents_row);
    audio_layout->addWidget(pitch);

    // --- Speed Change ---------------------------------------------------------
    ac.speed_cat = new InspectorCategory(tr("Speed Change"), false, /*has_enable=*/true, host);
    ac.speed_cat->set_feature_toggle_enabled(false);
    QSlider* s3 = nullptr;
    QDoubleSpinBox* sp3 = nullptr;
    auto* speed_row = make_slider_spin(canvas::core::audio_processing::kSpeedMin,
                                       canvas::core::audio_processing::kSpeedMax,
                                       2, host, &s3, &sp3);
    ac.speed_slider = s3;
    ac.speed_factor = sp3;
    QToolButton* speed_row_reset = nullptr;
    add_property_row(ac.speed_cat->body_layout(), tr("Factor"), speed_row,
                     /*with_reset=*/true, &speed_row_reset);
    audio_layout->addWidget(ac.speed_cat);

    // "Reset to default": the category-header reset AND the Factor-row reset
    // both restore the speed to 1.00 (disabled tempo) and commit one undoable
    // edit. spin->setValue() keeps the linked slider in sync.
    const auto reset_speed = [&mw]() {
        AudioControls* acc = audio_lookup(mw);
        if (!acc || !acc->speed_factor) return;
        acc->speed_factor->setValue(1.0);
        apply_inspector_audio_processing(mw);
    };
    if (auto* rb = ac.speed_cat->reset_button())
        QObject::connect(rb, &QToolButton::clicked, &mw, reset_speed);
    if (speed_row_reset)
        QObject::connect(speed_row_reset, &QToolButton::clicked, &mw, reset_speed);

    // --- Equalizer ------------------------------------------------------------
    // Open by default so the bands are immediately editable — no collapsed/
    // hidden-by-default state.
    ac.eq_cat = new InspectorCategory(tr("Equalizer"), /*expanded=*/true, /*has_enable=*/true, host);
    ac.eq_cat->set_feature_toggle_enabled(false);
    // The EQ section sits at the bottom of the audio tab and gets generous
    // spacing so every value/suffix stays fully visible at any dock width.
    ac.eq_cat->body_layout()->setSpacing(12);
    ac.eq_graph = new EqGraphWidget(host);
    ac.eq_cat->body_layout()->addWidget(ac.eq_graph);

    // Band rows: B1..B6 | type | freq | gain | Q. All five columns are always
    // shown — never folded away per filter type — so the row layout is stable
    // and no label/value is ever hidden.
    for (int i = 0; i < canvas::core::audio_processing::kEqBandCount; ++i) {
        auto* row = new QWidget(host);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(8);
        auto* lbl = new QLabel(QStringLiteral("B%1").arg(i + 1), row);
        lbl->setFixedWidth(22);
        apply_theme_style(lbl, [] {
            return QStringLiteral("color: %1; font-size: 10px;")
                .arg(css(tokens().ink_muted));
        });
        auto* type = make_dark_combo(row);
        type->addItems({tr("Low Shelf"), tr("Bell"), tr("High Shelf"), tr("Low Pass"),
                        tr("High Pass"), tr("Notch")});
        auto* freq = make_band_spin(canvas::core::audio_processing::kEqFreqMin,
                                    canvas::core::audio_processing::kEqFreqMax, 0, 1000.0, row, 78);
        freq->setSuffix(QStringLiteral("Hz"));
        auto* gain = make_band_spin(canvas::core::audio_processing::kEqGainMin,
                                    canvas::core::audio_processing::kEqGainMax, 1, 0.0, row, 66);
        gain->setSuffix(QStringLiteral("dB"));
        auto* q = make_band_spin(canvas::core::audio_processing::kEqQMin,
                                 canvas::core::audio_processing::kEqQMax, 1, 1.0, row, 54);
        lay->addWidget(lbl);
        lay->addWidget(type, 1);
        lay->addWidget(freq);
        lay->addWidget(gain);
        lay->addWidget(q);

        ac.eq_type.push_back(type);
        ac.eq_freq.push_back(freq);
        ac.eq_gain.push_back(gain);
        ac.eq_q.push_back(q);
        ac.eq_cat->body_layout()->addWidget(row);
    }

    // --- AI Voice Isolation --------------------------------------------------
    // Real per-clip engine picker (model-backed, applied before the gains/mix
    // in playback AND export). A dropdown row = the engine list; "None" is the
    // default/off state. Modes whose backend isn't compiled in this build stay
    // listed but greyed (DeepFilterNet voice-from-music — seam reserved).
    ac.iso_cat = new InspectorCategory(tr("AI Voice Isolation"), true, /*has_enable=*/false, host);
    {
        auto* row = new QWidget(host);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);
        auto* combo = make_dark_combo(row);
        // Index i == VoiceIsolationMode i (None=0, RnNoise=1, DeepFilterNet=2);
        // keep this aligned with the enum order.
        combo->addItem(tr("None"));
        combo->addItem(tr("RNNoise — Noise Suppression"));
        combo->addItem(tr("DeepFilterNet — Voice from Music"));
        combo->setItemData(2, tr("Engine not built into this build"),
                           Qt::ToolTipRole);
        if (!canvas::core::voice_isolation_supported(
                canvas::core::VoiceIsolationMode::DeepFilterNet)) {
            if (auto* model_ = qobject_cast<QStandardItemModel*>(combo->model())) {
                if (QStandardItem* item = model_->item(2)) {
                    item->setEnabled(false);
                    item->setToolTip(MainWindow::tr(
                        "DeepFilterNet is not built into this build — RNNoise is the "
                        "shipped engine."));
                }
            }
        }
        lay->addWidget(combo, 1);
        ac.iso_combo = combo;
        add_property_row(ac.iso_cat->body_layout(), tr("Isolate"), row);
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw,
                         [&mw]() { apply_inspector_voice_isolation(mw); });
    }
    audio_layout->addWidget(ac.iso_cat);

    // --- AI sections (UI placeholders, not wired to the model) ---------------
    const auto make_ai = [&](const QString& title, bool with_amount) -> InspectorCategory* {
        auto* cat = new InspectorCategory(title, true, /*has_enable=*/true, host);
        cat->set_feature_toggle_enabled(false);
        cat->set_feature_enabled(false);
        if (with_amount) {
            auto* row = new QWidget(host);
            auto* lay = new QHBoxLayout(row);
            lay->setContentsMargins(0, 0, 0, 0);
            lay->setSpacing(6);
            auto* slider = new QSlider(Qt::Horizontal, row);
            slider->setRange(0, 100);
            slider->setValue(100);
            slider->setEnabled(false);
            slider->setMinimumWidth(0);
            auto* spin = make_numeric(0.0, 100.0, 100.0, row);
            spin->setDecimals(0);
            spin->setEnabled(false);
            lay->addWidget(slider, 1);
            lay->addWidget(spin);
            auto* settings = new QToolButton(row);
            settings->setIcon(icon("settings"));
            settings->setIconSize(QSize(14, 14));
            settings->setAutoRaise(true);
            settings->setToolTip(MainWindow::tr("Additional settings"));
            lay->addWidget(settings);
            add_property_row(cat->body_layout(), tr("Amount"), row);
        }
        audio_layout->addWidget(cat);
        return cat;
    };
    ac.ai_leveler = make_ai(tr("AI Dialogue Leveler"), /*with_amount=*/false);
    ac.ai_remix = make_ai(tr("AI Music Remixer"), /*with_amount=*/false);

    // Equalizer sits at the bottom of the audio tab where it has room to
    // breathe; its extra spacing keeps every band value fully visible.
    audio_layout->addSpacing(8);
    audio_layout->addWidget(ac.eq_cat);

    if (audio_mode_button) {
        audio_mode_button->setToolTip(MainWindow::tr(
            "Audio clip settings — select an audio clip (or a video clip with linked audio)\n"
            "to edit volume, pitch, speed and EQ."));
    }

    // ---- Wiring --------------------------------------------------------------
    QObject::connect(ac.volume, &QDoubleSpinBox::editingFinished, &mw, [&mw]() {
        mw.apply_inspector_audio();
    });
    QObject::connect(ac.pan, &QDoubleSpinBox::editingFinished, &mw, [&mw]() {
        mw.apply_inspector_audio();
    });

    const auto commit_processing = [&mw]() { apply_inspector_audio_processing(mw); };
    for (QDoubleSpinBox* spin : {ac.pitch_semi, ac.pitch_cents, ac.speed_factor})
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QSlider* slider : {ac.pitch_semi_slider, ac.pitch_cents_slider, ac.speed_slider})
        QObject::connect(slider, &QSlider::sliderReleased, &mw, commit_processing);
    for (QDoubleSpinBox* spin : ac.eq_freq)
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QDoubleSpinBox* spin : ac.eq_gain)
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QDoubleSpinBox* spin : ac.eq_q)
        QObject::connect(spin, &QDoubleSpinBox::editingFinished, &mw, commit_processing);
    for (QComboBox* combo : ac.eq_type)
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw, commit_processing);
    QObject::connect(ac.speed_cat, &InspectorCategory::feature_toggled, &mw, commit_processing);
    QObject::connect(ac.eq_cat, &InspectorCategory::feature_toggled, &mw, commit_processing);

    // Keep the EQ graph in sync with gain/freq edits live.
    const auto refresh_graph = [&ac]() {
        if (!ac.eq_graph) return;
        std::array<canvas::core::Clip::EqBand, 6> bands;
        for (int i = 0; i < 6 && static_cast<std::size_t>(i) < ac.eq_freq.size(); ++i) {
            bands[i].frequency = ac.eq_freq[i]->value();
            bands[i].gain = ac.eq_gain[i]->value();
            bands[i].q = i < static_cast<int>(ac.eq_q.size()) ? ac.eq_q[i]->value() : 1.0;
            bands[i].type = i < static_cast<int>(ac.eq_type.size())
                                ? static_cast<canvas::core::Clip::EqBand::Type>(
                                      ac.eq_type[i]->currentIndex())
                                : canvas::core::Clip::EqBand::Type::Bell;
        }
        ac.eq_graph->set_bands(bands);
    };
    for (QDoubleSpinBox* spin : ac.eq_gain)
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, &mw, refresh_graph);
    for (QComboBox* combo : ac.eq_type)
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), &mw, refresh_graph);
}

void attach_inspector_audio(MainWindow& mw, TimelineWidget* timeline) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || !ac->volume || !timeline || ac->attached) return;
    ac->attached = true;
    QObject::connect(timeline, &TimelineWidget::clip_selected, &mw,
                     [&mw](const canvas::core::Clip*) { update_inspector_audio_full(mw); });
    QObject::connect(timeline, &TimelineWidget::clips_range_selected, &mw,
                     [&mw](std::vector<canvas::core::ClipId>) { update_inspector_audio_full(mw); });
}

void update_inspector_audio_full(MainWindow& mw) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || !ac->volume || !mw.project_) return;

    const auto targets = resolve_audio_targets(mw.project_->sequence, mw.selected_clip_ids_);
    const bool has_audio = !targets.empty();

    if (has_audio) populate_from_clip(*ac, targets.front().clip);

    // Enabled for audio clips and for video clips with a linked audio mate.
    if (ac->mode_button) ac->mode_button->setEnabled(has_audio);
    set_processing_enabled(*ac, has_audio);
    if (ac->volume) ac->volume->setEnabled(has_audio);
    if (ac->volume_slider) ac->volume_slider->setEnabled(has_audio);
    if (ac->pan) ac->pan->setEnabled(has_audio);

    // Multi-selection hint: values shown are the FIRST target's, but Volume/Pan
    // commits land on every resolved audio clip.
    if (ac->multi_hint) {
        const int n = static_cast<int>(targets.size());
        ac->multi_hint->setVisible(n > 1);
        if (n > 1)
            ac->multi_hint->setText(
                MainWindow::tr("%1 clips selected — Volume/Pan apply to all").arg(n));
    }
}

void apply_inspector_audio_processing(MainWindow& mw) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || ac->updating) return;

    canvas::core::Track::Kind kind;
    std::size_t index;
    canvas::core::Clip clip;
    // Target the selected audio clip, or the linked audio mate of a video clip.
    if (!mw.find_audio_target(kind, index, clip)) return;

    const float semi = static_cast<float>(ac->pitch_semi ? ac->pitch_semi->value() : 0.0);
    const float cents = static_cast<float>(ac->pitch_cents ? ac->pitch_cents->value() : 0.0);
    const float speed = static_cast<float>(ac->speed_factor ? ac->speed_factor->value() : 1.0);
    const bool speed_on = ac->speed_cat ? ac->speed_cat->feature_enabled() : false;
    const bool eq_on = ac->eq_cat ? ac->eq_cat->feature_enabled() : false;

    std::array<canvas::core::Clip::EqBand, 6> bands{};
    for (int i = 0; i < 6; ++i) {
        bands[i].type = static_cast<canvas::core::Clip::EqBand::Type>(
            ac->eq_type[i] ? ac->eq_type[i]->currentIndex() : 0);
        bands[i].frequency = static_cast<float>(ac->eq_freq[i]->value());
        bands[i].gain = static_cast<float>(ac->eq_gain[i]->value());
        bands[i].q = static_cast<float>(ac->eq_q[i]->value());
    }

    const bool same = clip.pitch_semitones == semi && clip.pitch_cents == cents &&
                      clip.speed_factor == speed && clip.speed_enabled == speed_on &&
                      clip.eq_enabled == eq_on && clip.eq_bands == bands;
    if (same) return;

    auto cmd = canvas::core::set_clip_audio_processing(
        mw.project_->sequence, kind, index, clip.id, semi, cents, speed, speed_on, eq_on, bands);
    if (!cmd) return;
    mw.undo_.record(std::move(cmd));
    mw.has_unsaved_changes_ = true;
    mw.refresh_timeline();
    mw.push_audio_mix_snapshot();
    qWarning() << "[edit] CLIP-AUDIO-PROCESSING kind="
               << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
               << "track=" << index << "clip=" << clip.id << "semi=" << semi << "cents=" << cents
               << "speed=" << speed << "eq_on=" << eq_on;
}

void apply_inspector_voice_isolation(MainWindow& mw) {
    AudioControls* ac = audio_lookup(mw);
    if (!ac || ac->updating || !ac->iso_combo) return;

    canvas::core::Track::Kind kind;
    std::size_t index;
    canvas::core::Clip clip;
    // Target the selected audio clip, or the linked audio mate of a video clip.
    if (!mw.find_audio_target(kind, index, clip)) return;

    const auto mode = static_cast<canvas::core::VoiceIsolationMode>(
        ac->iso_combo->currentIndex());
    // An unsupported engine (DeepFilterNet) must never be selectable at commit
    // time even if the entry was force-enabled somehow.
    if (!canvas::core::voice_isolation_supported(mode)) return;
    if (clip.voice_isolation == mode) return;

    auto cmd = canvas::core::set_clip_voice_isolation(mw.project_->sequence, kind, index,
                                                      clip.id, mode);
    if (!cmd) return;
    mw.undo_.record(std::move(cmd));
    mw.has_unsaved_changes_ = true;
    mw.refresh_timeline();
    mw.push_audio_mix_snapshot();
    qWarning() << "[edit] CLIP-VOICE-ISOLATION kind="
               << (kind == canvas::core::Track::Kind::Video ? "V" : "A")
               << "track=" << index << "clip=" << clip.id
               << "mode=" << canvas::core::voice_isolation_mode_name(mode);
}

}  // namespace canvas::gui