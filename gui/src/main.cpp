#include <QApplication>
#include <QCoreApplication>
#include <QMetaObject>
#include <QtCore/Qt>

#include "Logging.hpp"
#include "UX/MainWindow.hpp"
#include "UX/theme.hpp"

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

    canvas::gui::apply_theme(app);

    canvas::gui::MainWindow window;
    QApplication::setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.setWindowIcon(canvas::gui::raw_icon("app_icon"));
    window.show();
    if (argc > 1) window.open_file(QString::fromLocal8Bit(argv[1]));
    return QApplication::exec();
}
