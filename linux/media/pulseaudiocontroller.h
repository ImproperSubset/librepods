#ifndef PULSEAUDIOCONTROLLER_H
#define PULSEAUDIOCONTROLLER_H

#include <QString>
#include <QObject>
#include <pulse/pulseaudio.h>

#include <atomic>

class QTimer;

class PulseAudioController : public QObject
{
    Q_OBJECT

public:
    explicit PulseAudioController(QObject *parent = nullptr);
    ~PulseAudioController();

    bool initialize();
    QString getDefaultSink();
    QString getDefaultSinkMacAddress();
    int getSinkVolume(const QString &sinkName);
    bool setSinkVolume(const QString &sinkName, int volumePercent);
    bool setCardProfile(const QString &cardName, const QString &profileName);
    QString getCardNameForDevice(const QString &macAddress);
    QString getActiveCardProfile(const QString &cardName);
    bool isProfileAvailable(const QString &cardName, const QString &profileName);
    bool isDeviceSinkRunning(const QString &macAddress);

private:
    pa_threaded_mainloop *m_mainloop;
    pa_context *m_context;
    // Public methods must be called on the GUI thread; this is not a
    // thread-safe class. The atomic only makes the mainloop thread's write in
    // the state callback well-defined against the GUI thread's reads.
    std::atomic<bool> m_initialized;
    QTimer *m_reconnectTimer;
    int m_reconnectDelayMs;

    // Releases the mainloop and context in the order libpulse requires. Safe to
    // call when nothing is allocated, so initialize() can use it to reset.
    void teardown();
    // Arms the backoff timer that re-runs initialize(). Must run on the GUI
    // thread; the state callback reaches it through a queued invocation.
    void scheduleReconnect();

    static void contextStateCallback(pa_context *c, void *userdata);
    static void sinkInfoCallback(pa_context *c, const pa_sink_info *info, int eol, void *userdata);
    static void cardInfoCallback(pa_context *c, const pa_card_info *info, int eol, void *userdata);
    static void serverInfoCallback(pa_context *c, const pa_server_info *info, void *userdata);

    bool waitForOperation(pa_operation *op);
    QString getMacAddressBySinkName(const QString &sinkName);
};

#endif // PULSEAUDIOCONTROLLER_H
