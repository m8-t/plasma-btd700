#pragma once

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class DongleClient : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool serviceAvailable READ serviceAvailable NOTIFY serviceAvailableChanged)
    Q_PROPERTY(bool present READ present NOTIFY presentChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString audioMode READ audioMode NOTIFY audioModeChanged)
    Q_PROPERTY(QString transport READ transport NOTIFY transportChanged)
    Q_PROPERTY(QStringList supportedCodecs READ supportedCodecs NOTIFY supportedCodecsChanged)
    Q_PROPERTY(QStringList activeCodecs READ activeCodecs NOTIFY activeCodecsChanged)
    Q_PROPERTY(uint sampleRate READ sampleRate NOTIFY sampleRateChanged)
    Q_PROPERTY(uint bitDepth READ bitDepth NOTIFY bitDepthChanged)
    Q_PROPERTY(bool gamingAvailable READ gamingAvailable NOTIFY gamingAvailableChanged)
    Q_PROPERTY(QString firmwareVersion READ firmwareVersion NOTIFY firmwareVersionChanged)
    Q_PROPERTY(int headsetBattery READ headsetBattery NOTIFY headsetBatteryChanged)
    Q_PROPERTY(qint64 headsetBatteryUpdated READ headsetBatteryUpdated NOTIFY headsetBatteryUpdatedChanged)
    Q_PROPERTY(bool batteryReading READ batteryReading NOTIFY batteryReadingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorName READ errorName NOTIFY errorChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)

public:
    explicit DongleClient(QObject* parent = nullptr);

    bool serviceAvailable() const { return m_serviceAvailable; }
    bool present() const { return m_present; }
    QString state() const { return m_state; }
    QString audioMode() const { return m_audioMode; }
    QString transport() const { return m_transport; }
    QStringList supportedCodecs() const { return m_supportedCodecs; }
    QStringList activeCodecs() const { return m_activeCodecs; }
    uint sampleRate() const { return m_sampleRate; }
    uint bitDepth() const { return m_bitDepth; }
    bool gamingAvailable() const { return m_gamingAvailable; }
    QString firmwareVersion() const { return m_firmwareVersion; }
    int headsetBattery() const { return m_headsetBattery; }
    qint64 headsetBatteryUpdated() const { return m_headsetBatteryUpdated; }
    bool batteryReading() const { return m_batteryReading; }
    bool busy() const { return m_pending > 0; }
    QString errorName() const { return m_errorName; }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void setAudioMode(const QString& mode);
    Q_INVOKABLE void setCodec(const QString& codec);
    Q_INVOKABLE void connectHeadphones();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setBatteryReading(bool enabled);
    Q_INVOKABLE void clearError();

Q_SIGNALS:
    void serviceAvailableChanged();
    void presentChanged();
    void stateChanged();
    void audioModeChanged();
    void transportChanged();
    void supportedCodecsChanged();
    void activeCodecsChanged();
    void sampleRateChanged();
    void bitDepthChanged();
    void gamingAvailableChanged();
    void firmwareVersionChanged();
    void headsetBatteryChanged();
    void headsetBatteryUpdatedChanged();
    void batteryReadingChanged();
    void busyChanged();
    void errorChanged();

private Q_SLOTS:
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed,
                             const QStringList& invalidated);

private:
    void checkService();
    void serviceUp();
    void serviceDown();
    void fetchAll();
    void applyProperty(const QString& name, const QVariant& value);
    void resetProperties();
    void call(const QString& method, const QVariantList& args = {});
    void setError(const QString& name, const QString& message);

    QDBusConnection m_bus;
    QDBusServiceWatcher* m_watcher = nullptr;
    int m_generation = 0;
    int m_pending = 0;

    bool m_serviceAvailable = false;
    bool m_present = false;
    QString m_state;
    QString m_audioMode;
    QString m_transport;
    QStringList m_supportedCodecs;
    QStringList m_activeCodecs;
    uint m_sampleRate = 0;
    uint m_bitDepth = 0;
    bool m_gamingAvailable = false;
    QString m_firmwareVersion;
    int m_headsetBattery = -1;
    qint64 m_headsetBatteryUpdated = 0;
    bool m_batteryReading = false;
    QString m_errorName;
    QString m_errorMessage;
};
