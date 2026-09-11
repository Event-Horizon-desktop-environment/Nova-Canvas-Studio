#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QFontDatabase>
#include <QMetaObject>
#include <QSettings>
#include <QtCore/Qt>

#include "Logging.hpp"
#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"
#include "canvas/core/colorsci/wheels_ui.hpp"
#include "canvas/core/gpu/colorspace.hpp"
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
    // Always-on compiled-in constants report: proves which law the running
    // binary was built with. If this line ever disagrees with the source, the
    // binary is stale (wrong build dir / ccache / un-rebuilt) and NOTHING else
    // in the log can be trusted. Also anchors the YUV matrix so a BT.601 vs
    // BT.709 mismatch — the purple-skin suspect — is decided by the log alone.
    qWarning().nospace()
        << "[build] wheel_scales lift="
        << canvas::core::colorsci::kWheelLiftScale
        << " gamma=" << canvas::core::colorsci::kWheelGammaScale
        << " gain=" << canvas::core::colorsci::kWheelGainScale
        << " offset=" << canvas::core::colorsci::kWheelOffsetScale
        << " yuv=bt709 limited yuv2rgb(Y=1.164 R=1.793 G=-0.213/-0.533 B=2.112)";
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
    canvas::core::HwDeviceManager hw{"main"};
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
    canvas::gui::apply_theme(app, light_theme);

    canvas::gui::MainWindow window;
    QApplication::setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.show();
    if (argc > 1) window.open_file(QString::fromLocal8Bit(argv[1]));
    return QApplication::exec();
}
