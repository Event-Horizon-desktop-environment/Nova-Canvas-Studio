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
#include "canvas/core/media/gpu_select.hpp"
#include "canvas/core/media/hw_device.hpp"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/cpu.h>
}

#include <chrono>
#include <csignal>
#include <thread>

namespace {

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
            std::_Exit(1);
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
    ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread(signal_quit_thread).detach();
}

}

int main(int argc, char* argv[]) {
    install_signal_quit();
    canvas::gui::reset_log_file();
    canvas::gui::install_logging();
    qWarning() << "eh: boot" << QApplication::applicationVersion()
               << "built" << __DATE__ << __TIME__;
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

    {
        const QSettings settings;
        const std::string gpu = settings
            .value(QStringLiteral("settings/hw_gpu"), QStringLiteral(""))
            .toString()
            .toStdString();
        if (gpu == canvas::core::gpu_select::kCpuSentinel) {
            canvas::core::HwDeviceManager::set_preferred_backend("software");
        } else if (!gpu.empty()) {
            for (const auto& g : canvas::core::gpu_select::detect_gpus()) {
                if (g.pci_slot == gpu) {
                    canvas::core::HwDeviceManager::set_preferred_gpu(
                        g.backend, g.device_arg);
                    break;
                }
            }
        } else {
            const std::string backend = settings
                .value(QStringLiteral("settings/hw_backend"), QStringLiteral(""))
                .toString()
                .toStdString();
            if (!backend.empty())
                canvas::core::HwDeviceManager::set_preferred_backend(backend);
        }
    }

    const char* fv = av_version_info();
    const int ffver = LIBAVUTIL_VERSION_INT;
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
    const bool on_hyprland =
        !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE");
    QSettings appearance_settings;
    const bool hypr_dark =
        appearance_settings.contains(QStringLiteral("appearance/hypr_dark"))
            ? appearance_settings.value(QStringLiteral("appearance/hypr_dark")).toBool()
            : (!light_theme && on_hyprland
               && app.platformName() == QLatin1String("wayland"));

    {
        const QSettings settings;
        int restored = 0;
        for (int i = 0; i < canvas::gui::kThemeTokenFieldCount; ++i) {
            const auto f = static_cast<canvas::gui::ThemeTokenField>(i);
            const QString raw = settings
                .value(canvas::gui::theme_setting_key(f), QStringLiteral(""))
                .toString();
            const QColor c(raw);
            if (c.isValid()) {
                canvas::gui::set_token_override(f, c);
                ++restored;
            }
        }
        const QString legacy_accent = settings
            .value(QStringLiteral("settings/accent_color"), QStringLiteral(""))
            .toString();
        if (!legacy_accent.isEmpty()
            && !canvas::gui::token_override(canvas::gui::ThemeTokenField::Accent).isValid()) {
            const QColor c(legacy_accent);
            if (c.isValid()) {
                canvas::gui::set_token_override(
                    canvas::gui::ThemeTokenField::Accent, c);
                qWarning().nospace()
                    << "[theme] legacy settings/accent_color -> accent override ("
                    << legacy_accent << ")";
                ++restored;
            }
        }
        const QString legacy_playhead = settings
            .value(QStringLiteral("settings/playhead_color"), QStringLiteral(""))
            .toString();
        if (!legacy_playhead.isEmpty()
            && !canvas::gui::token_override(canvas::gui::ThemeTokenField::Playhead).isValid()) {
            const QColor c(legacy_playhead);
            if (c.isValid()) {
                canvas::gui::set_token_override(
                    canvas::gui::ThemeTokenField::Playhead, c);
                qWarning().nospace()
                    << "[theme] legacy settings/playhead_color -> playhead override ("
                    << legacy_playhead << ")";
                ++restored;
            }
        }
        qWarning().nospace()
            << "[theme] restored " << restored << "/"
            << canvas::gui::kThemeTokenFieldCount
            << " field override(s) from QSettings";
    }
    canvas::gui::apply_theme(app, light_theme, hypr_dark);

    qWarning().nospace()
        << "[theme] mode=" << (hypr_dark ? "hypr-dark"
                                         : (light_theme ? "light" : "dark"))
        << " platform=" << app.platformName()
        << " qpa_env=" << qEnvironmentVariable("QT_QPA_PLATFORM")
        << " hyprland=" << (on_hyprland ? "yes" : "no");
    canvas::gui::log_theme_tokens();

    canvas::gui::MainWindow window;
    QApplication::setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.setWindowIcon(canvas::gui::raw_icon("app_icon"));
    if (argc > 1) {
        window.show();
        window.open_file(QString::fromLocal8Bit(argv[1]));
    } else {
        window.enter_project_manager();
    }
    return QApplication::exec();
}
