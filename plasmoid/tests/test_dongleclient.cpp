// Headless test of DongleClient against the real btd700d on a private session bus.
// Run via ctest (needs dbus-run-session). The no-dongle phases need no hardware.
// Setting BTD700_TEST_FAKE_DIR enables the dongle-present phases, which expect a
// hidapi stand-in preloaded into the daemon (BTD700_TEST_PRELOAD) that treats
// $BTD700_TEST_FAKE_DIR/present as the plugged-in switch.
#include "dongleclient.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QProcess>

#include <cstdio>
#include <functional>

static int g_failures = 0;

static bool waitFor(const std::function<bool()>& cond, int timeoutMs = 3000) {
    QDeadlineTimer deadline(timeoutMs);
    while (!cond()) {
        if (deadline.hasExpired()) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

static void check(const char* what, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

static QProcess* startDaemon(QObject* parent) {
    auto* p = new QProcess(parent);
    p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (qEnvironmentVariableIsSet("BTD700_TEST_PRELOAD"))
        env.insert("LD_PRELOAD", qEnvironmentVariable("BTD700_TEST_PRELOAD"));
    p->setProcessEnvironment(env);
    p->start(QStringLiteral(BTD700D_PATH));
    return p;
}

static void stopDaemon(QProcess* p) {
    p->terminate();
    p->waitForFinished(5000);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString fakeDir = qEnvironmentVariable("BTD700_TEST_FAKE_DIR");
    const bool fake = !fakeDir.isEmpty();

    DongleClient c;
    waitFor([&] { return false; }, 300);
    check("no daemon: serviceAvailable false", !c.serviceAvailable());

    c.setCodec("sbc");
    check("no daemon: call reports ServiceUnknown, not busy",
          c.errorName() == "org.freedesktop.DBus.Error.ServiceUnknown" && !c.busy());
    c.clearError();

    QProcess* d = startDaemon(&app);
    check("daemon appears", waitFor([&] { return c.serviceAvailable(); }, 5000));
    check("initial GetAll applied", waitFor([&] { return c.transport() == "disconnected"; }));
    check("no dongle: present false, state none", !c.present() && c.state() == "none");

    c.setCodec("sbc");
    check("busy while call pending", c.busy());
    check("no dongle: SetCodec -> NotPresent",
          waitFor([&] { return c.errorName() == "org.btd700ctl.Error.NotPresent"; }) && !c.busy());
    c.setCodec("bogus");
    check("bad codec -> InvalidArgs",
          waitFor([&] { return c.errorName() == "org.freedesktop.DBus.Error.InvalidArgs"; }));
    c.setAudioMode("nonsense");
    check("bad mode -> InvalidArgs", waitFor([&] { return c.errorMessage().contains("nonsense"); }));
    c.connectHeadphones();
    check("no dongle: Connect -> NotPresent",
          waitFor([&] { return c.errorName() == "org.btd700ctl.Error.NotPresent"; }));
    c.clearError();
    check("clearError", c.errorName().isEmpty());

    if (fake) {
        QFile f(fakeDir + "/present");
        check("create present marker", f.open(QIODevice::WriteOnly));
        f.close();
        check("PropertiesChanged: Present true", waitFor([&] { return c.present(); }, 12000));
        check("PropertiesChanged: full state",
              waitFor([&] { return c.firmwareVersion() == "1.2.300" && c.sampleRate() == 48000; }, 5000)
              && c.bitDepth() == 24 && c.transport() == "le-audio"
              && c.supportedCodecs().contains("lc3") && c.activeCodecs() == QStringList{"aptx-adaptive"});

        c.setAudioMode("gaming");
        check("SetAudioMode gaming, transport preserved",
              waitFor([&] { return c.audioMode() == "gaming"; }) && c.transport() == "le-audio"
              && c.errorName().isEmpty());
        c.setCodec("lc3");
        check("SetCodec lc3", waitFor([&] { return c.activeCodecs() == QStringList{"lc3"}; }));
        c.setCodec("aptx-lite");
        check("SetCodec unsupported -> InvalidArgs",
              waitFor([&] { return c.errorName() == "org.freedesktop.DBus.Error.InvalidArgs"; }));
        c.connectHeadphones();
        check("Connect -> state connected", waitFor([&] { return c.state() == "connected"; }));
        c.refresh();
        check("Refresh succeeds", waitFor([&] { return !c.busy(); }, 5000) && c.errorName().isEmpty());
    }

    stopDaemon(d);
    check("daemon exit: serviceAvailable false", waitFor([&] { return !c.serviceAvailable(); }));
    check("daemon exit: state reset", !c.present() && c.supportedCodecs().isEmpty() && c.state() == "none");

    d = startDaemon(&app);
    check("daemon restart detected", waitFor([&] { return c.serviceAvailable(); }, 5000));
    check("restart: GetAll re-applied", waitFor([&] { return c.transport() != "unknown"; }));
    if (fake)
        check("restart: dongle state restored", waitFor([&] { return c.present() && c.firmwareVersion() == "1.2.300"; }, 12000));
    stopDaemon(d);

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
