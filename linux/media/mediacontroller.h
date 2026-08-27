#ifndef MEDIACONTROLLER_H
#define MEDIACONTROLLER_H

#include <QObject>
#include <QElapsedTimer>
#include "pulseaudiocontroller.h"

class QProcess;
class EarDetection;
class PlayerStatusWatcher;
class QDBusInterface;

class MediaController : public QObject
{
  Q_OBJECT
public:
  enum MediaState
  {
    Playing,
    Paused,
    Stopped
  };
  Q_ENUM(MediaState)
  enum EarDetectionBehavior
  {
    PauseWhenOneRemoved,
    PauseWhenBothRemoved,
    Disabled
  };
  Q_ENUM(EarDetectionBehavior)

  explicit MediaController(QObject *parent = nullptr);
  ~MediaController();

  void handleEarDetection(EarDetection*);
  void followMediaChanges();
  bool isActiveOutputDeviceAirPods();
  void handleConversationalAwareness(const QByteArray &data);
  void activateA2dpProfile();
  void activateA2dpProfileWithRetry(int attemptsLeft, int generation);
  void verifyA2dpProfileTook(const QString &requested, const QString &mac);
  void removeAudioOutputDevice();
  void restoreProfileIfWeTurnedItOff();
  void clearConnectedDevice();
  void setConnectedDeviceMacAddress(const QString &macAddress);
  bool isA2dpProfileAvailable();
  QString getPreferredA2dpProfile();
  bool restartWirePlumber();

  void setEarDetectionBehavior(EarDetectionBehavior behavior);
  inline EarDetectionBehavior getEarDetectionBehavior() const { return earDetectionBehavior; }

  void play();
  void pause();
  MediaState getCurrentMediaState() const;

Q_SIGNALS:
  void mediaStateChanged(MediaState state);

private:
  MediaState mediaStateFromPlayerctlOutput(const QString &output) const;
  QString getAudioDeviceName();
  QStringList getPlayingMediaPlayers();

  QStringList pausedByAppServices;
  int initialVolume = -1;
  QString connectedDeviceMacAddress;
  EarDetectionBehavior earDetectionBehavior = PauseWhenOneRemoved;
  QString m_deviceOutputName;
  PlayerStatusWatcher *playerStatusWatcher = nullptr;
  PulseAudioController *m_pulseAudio = nullptr;
  QString m_cachedA2dpProfile;
  // The profile this app last asked PulseAudio for. A request is accepted
  // synchronously and the BlueZ codec switch behind it can still fail seconds
  // later, after which WirePlumber persists whatever it fell back to. Holding
  // the request is what tells a fallback we never chose apart from a profile
  // somebody did choose.
  QString m_requestedProfile;
  // The device that request was made for. A disconnect clears
  // connectedDeviceMacAddress, so the request cannot be scoped by comparing
  // against it: the reconnect that follows is exactly when the correction is
  // owed. Pairing it with its MAC keeps it across that gap while stopping a
  // different device from inheriting it.
  QString m_requestedProfileMac;
  // Cancellation token for in-flight A2DP activation chains; bumped by anything
  // that supersedes a pending activation.
  int m_a2dpGeneration = 0;
  // Generation of the chain currently in flight, or -1 for none. Must be
  // generation-aware rather than a bare bool: a bool guard combined with a
  // generation bump lets the new caller be refused while the old chain aborts,
  // dropping the activation entirely.
  int m_a2dpPendingGeneration = -1;
  // True while the "off" profile we wrote is still in effect. WirePlumber
  // persists that value, so shutdown must undo it or the card is left sinkless.
  bool m_weTurnedItOff = false;
  QElapsedTimer m_lastWirePlumberRestart;
};

#endif // MEDIACONTROLLER_H