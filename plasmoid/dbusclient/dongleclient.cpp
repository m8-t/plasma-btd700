#include "dongleclient.h"

#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {
const QString kService = QStringLiteral("org.btd700ctl.Dongle");
const QString kPath = QStringLiteral("/org/btd700ctl/Dongle");
const QString kInterface = QStringLiteral("org.btd700ctl.Dongle1");
const QString kPropsInterface = QStringLiteral("org.freedesktop.DBus.Properties");
constexpr int kCallTimeoutMs = 15000;

template <typename T>
bool assign(T& field, const T& value) {
    if (field == value) return false;
    field = value;
    return true;
}

QStringList toStringList(const QVariant& v) {
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<QStringList>(v.value<QDBusArgument>());
    return v.toStringList();
}
}

DongleClient::DongleClient(QObject* parent)
    : QObject(parent), m_bus(QDBusConnection::sessionBus()) {
    resetProperties();
    if (!m_bus.isConnected()) return;

    m_watcher = new QDBusServiceWatcher(kService, m_bus,
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceRegistered, this, &DongleClient::serviceUp);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &DongleClient::serviceDown);

    m_bus.connect(kService, kPath, kPropsInterface, QStringLiteral("PropertiesChanged"),
                  this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));

    checkService();
}

void DongleClient::checkService() {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    msg << kService;

    auto* w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* watcher) {
        QDBusPendingReply<bool> reply = *watcher;
        watcher->deleteLater();
        if (reply.isValid() && reply.value() && !m_serviceAvailable) serviceUp();
    });
}

void DongleClient::serviceUp() {
    if (m_serviceAvailable) return;
    m_serviceAvailable = true;
    emit serviceAvailableChanged();
    fetchAll();
}

void DongleClient::serviceDown() {
    m_generation++;
    if (m_serviceAvailable) {
        m_serviceAvailable = false;
        emit serviceAvailableChanged();
    }
    resetProperties();
}

void DongleClient::fetchAll() {
    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kPropsInterface,
                                                      QStringLiteral("GetAll"));
    msg << kInterface;

    const int generation = m_generation;
    auto* w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg, kCallTimeoutMs), this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher* watcher) {
        QDBusPendingReply<QVariantMap> reply = *watcher;
        watcher->deleteLater();
        if (generation != m_generation) return;
        if (reply.isError()) {
            setError(reply.error().name(), reply.error().message());
            return;
        }
        const QVariantMap props = reply.value();
        for (auto it = props.cbegin(); it != props.cend(); ++it)
            applyProperty(it.key(), it.value());
    });
}

void DongleClient::onPropertiesChanged(const QString& interface, const QVariantMap& changed,
                                       const QStringList& invalidated) {
    if (interface != kInterface) return;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it)
        applyProperty(it.key(), it.value());
    if (!invalidated.isEmpty() && m_serviceAvailable) fetchAll();
}

void DongleClient::applyProperty(const QString& name, const QVariant& v) {
    if (name == QLatin1String("Present")) {
        if (assign(m_present, v.toBool())) emit presentChanged();
    } else if (name == QLatin1String("State")) {
        if (assign(m_state, v.toString())) emit stateChanged();
    } else if (name == QLatin1String("AudioMode")) {
        if (assign(m_audioMode, v.toString())) emit audioModeChanged();
    } else if (name == QLatin1String("Transport")) {
        if (assign(m_transport, v.toString())) emit transportChanged();
    } else if (name == QLatin1String("SupportedCodecs")) {
        if (assign(m_supportedCodecs, toStringList(v))) emit supportedCodecsChanged();
    } else if (name == QLatin1String("ActiveCodecs")) {
        if (assign(m_activeCodecs, toStringList(v))) emit activeCodecsChanged();
    } else if (name == QLatin1String("SampleRate")) {
        if (assign(m_sampleRate, v.toUInt())) emit sampleRateChanged();
    } else if (name == QLatin1String("BitDepth")) {
        if (assign(m_bitDepth, v.toUInt())) emit bitDepthChanged();
    } else if (name == QLatin1String("GamingAvailable")) {
        if (assign(m_gamingAvailable, v.toBool())) emit gamingAvailableChanged();
    } else if (name == QLatin1String("FirmwareVersion")) {
        if (assign(m_firmwareVersion, v.toString())) emit firmwareVersionChanged();
    }
}

void DongleClient::resetProperties() {
    applyProperty(QStringLiteral("Present"), false);
    applyProperty(QStringLiteral("State"), QStringLiteral("none"));
    applyProperty(QStringLiteral("AudioMode"), QStringLiteral("unknown"));
    applyProperty(QStringLiteral("Transport"), QStringLiteral("unknown"));
    applyProperty(QStringLiteral("SupportedCodecs"), QStringList());
    applyProperty(QStringLiteral("ActiveCodecs"), QStringList());
    applyProperty(QStringLiteral("SampleRate"), 0u);
    applyProperty(QStringLiteral("BitDepth"), 0u);
    applyProperty(QStringLiteral("GamingAvailable"), false);
    applyProperty(QStringLiteral("FirmwareVersion"), QString());
}

void DongleClient::call(const QString& method, const QVariantList& args) {
    if (!m_serviceAvailable) {
        setError(QStringLiteral("org.freedesktop.DBus.Error.ServiceUnknown"),
                 QStringLiteral("btd700d is not running"));
        return;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
    msg.setArguments(args);

    m_pending++;
    if (m_pending == 1) emit busyChanged();

    auto* w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg, kCallTimeoutMs), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* watcher) {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        m_pending--;
        if (m_pending == 0) emit busyChanged();
        if (reply.isError()) setError(reply.error().name(), reply.error().message());
        else clearError();
    });
}

void DongleClient::setError(const QString& name, const QString& message) {
    if (m_errorName == name && m_errorMessage == message) return;
    m_errorName = name;
    m_errorMessage = message;
    emit errorChanged();
}

void DongleClient::clearError() { setError(QString(), QString()); }

void DongleClient::setAudioMode(const QString& mode) { call(QStringLiteral("SetAudioMode"), {mode}); }
void DongleClient::setCodec(const QString& codec) { call(QStringLiteral("SetCodec"), {codec}); }
void DongleClient::connectHeadphones() { call(QStringLiteral("Connect")); }
void DongleClient::disconnectHeadphones() { call(QStringLiteral("Disconnect")); }
void DongleClient::refresh() { call(QStringLiteral("Refresh")); }
