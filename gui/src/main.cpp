#include <QApplication>
#include <QCoreApplication>
#include <QMetaObject>
#include <QtCore/Qt>

#include "Logging.hpp"
#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "canvas/core/media/hw_device.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/cpu.h>
}

#include <chrono>
#include <csignal>
#include <thread>

namespace {

// Graceful SIGINT/SIGTERM/SIGHUP -> QApplication::quit(). Audio on this system
// routes through the PipeWire ALSA plugin (pcm.pipewire), so the DEFAULT signal
// action (instant client death with no teardown) leaves the daemon playing the
// client's queued PCM — the "I killed the app but the audio is still playing"
// bug. Walking the normal quit path closes the stream, which stops the sound.
// A second signal forces an immediate exit (in case teardown hangs).
void signal_quit_thread() {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    ::sigaddset(&set, SIGHUP);
    int sig = 0;
    int hits = 0;
    while (::sigwait(&set, &sig) == 0) {
        ++hits;
        if (hits >= 2) {
            std::_Exit(1);  // second kill: force out instead of hanging in teardown
        }
        if (QCoreApplication::instance())
            QMetaObject::invokeMethod(QCoreApplication::instance(), "quit",
                                      Qt::QueuedConnection);
    }
}

void install_signal_quit() {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    ::sigaddset(&set, SIGHUP);
    // Block in this thread BEFORE QApplication/Qt spawn threads so every thread
    // inherits the mask; sigwait() then monopolizes these signals.
    ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread(signal_quit_thread).detach();
}

}  // namespace

int main(int argc, char* argv[]) {
    // Install first: Qt threads inherit the signal mask, and a very early kill
    // must still take the graceful path.
    install_signal_quit();
    // Start every launch with a fresh log file (see log_file_path()/reset_log_file()).
    canvas::gui::reset_log_file();
    canvas::gui::install_logging();
    // Always-on banner: records the exact build/run identity so a log capture
    // can be matched to the binary that produced it and stale runs are obvious.
    qWarning() << "eh: boot" << QApplication::applicationVersion()
               << "built" << __DATE__ << __TIME__;
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("canvas"));
    QApplication::setApplicationDisplayName(QStringLiteral("Nova Canvas Studio"));
    QApplication::setOrganizationName(QStringLiteral("Nova Canvas"));

    // Startup env report (always-on): the exact FFmpeg build and the hardware
    // decode surface so playback/export failures can be blamed on a stale ABI
    // or the absence of a GPU without digging. ALSA/PipeWire presence is
    // handled by AudioOutput later, but avversion here anchors the whole stack.
    const char* fv = av_version_info();
    const int ffver = LIBAVUTIL_VERSION_INT;
    // Feeding the manager triggers its own themed probe log ([hw] probing...),
    // so we just surface the outcome here rather than re-probing.
    const auto env_t0 = std::chrono::steady_clock::now();
    canvas::core::HwDeviceManager hw;
    (void)hw.device_ctx();
    const double env_probe_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - env_t0).count();
    qWarning().nospace()
        << "[env] qt=" << qVersion()
        << " ffmpeg=" << (fv ? fv : "?")
        << " lavutil=" << AV_VERSION_MAJOR(ffver)
        << "." << AV_VERSION_MINOR(ffver) << "." << AV_VERSION_MICRO(ffver)
        << " hw_decode=" << (hw.is_hardware()
                                 ? QString::fromStdString(hw.device_name())
                                 : QStringLiteral("software"))
        << " cpus=" << av_cpu_count()
        << " probe_ms=" << QString::number(env_probe_ms, 'f', 0);

<<<<<<< Updated upstream
    canvas::gui::apply_theme(app);
=======
    // Register the bundled Geist variable fonts; the app falls back to the
    // platform default family if either fails to load (missing/corrupt asset).
    if (QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Geist-Variable.ttf")) == -1)
        qWarning() << "[font] Geist-Variable.ttf failed to load";
    if (QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/GeistMono-Variable.ttf")) == -1)
        qWarning() << "[font] GeistMono-Variable.ttf failed to load";
    QFont ui_font = app.font();
    ui_font.setFamily(QStringLiteral("Geist"));
    app.setFont(ui_font);

    const bool light_theme = QSettings()
        .value(QStringLiteral("appearance/theme"), QStringLiteral("dark"))
        .toString() == QStringLiteral("light");
    // "HyprDark" auto-trigger: on Hyprland's native Wayland backend only,
    // Hyprland re-quantizes surfaces through its FP16 sRGB color-management
    // pipeline (XWayland is blitted raw), so the dark palette reads darker with
    // flattened blue there. The hypr_dark token set pre-lifts the base palette
    // by the measured shift so the on-screen result matches the design.
    // Detected via Hyprland's per-client env var; inert on xcb (byte-accurate
    // already) and under any other compositor. An explicit user choice in the
    // Nova Canvas > Appearance menu (appearance/hypr_dark) overrides the rule.
    const bool on_hyprland =
        !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE");
    QSettings appearance_settings;
    const bool hypr_dark =
        appearance_settings.contains(QStringLiteral("appearance/hypr_dark"))
            ? appearance_settings.value(QStringLiteral("appearance/hypr_dark")).toBool()
            : (!light_theme && on_hyprland
               && app.platformName() == QLatin1String("wayland"));
    canvas::gui::apply_theme(app, light_theme, hypr_dark);

    qWarning().nospace()
        << "[theme] mode=" << (hypr_dark ? "hypr-dark"
                                         : (light_theme ? "light" : "dark"))
        << " platform=" << app.platformName()
        << " qpa_env=" << qEnvironmentVariable("QT_QPA_PLATFORM")
        << " hyprland=" << (on_hyprland ? "yes" : "no");
    canvas::gui::log_theme_tokens();
>>>>>>> Stashed changes

    canvas::gui::MainWindow window;
    QApplication::setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.show();
    if (argc > 1) window.open_file(QString::fromLocal8Bit(argv[1]));
    return QApplication::exec();
}
