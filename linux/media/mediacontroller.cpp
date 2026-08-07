#include "mediacontroller.h"
#include "logger.h"
#include "eardetection.hpp"
#include "playerstatuswatcher.h"
#include "pulseaudiocontroller.h"

#include <QDebug>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QRegularExpression>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

namespace {
// The bluez card typically appears within a second or two of the link coming
// up; this budget covers a slow connect without retrying indefinitely.
constexpr int kA2dpActivationAttempts = 10;
constexpr int kA2dpActivationRetryMs = 500;
// Restarting WirePlumber is machine-wide and blocking; never do it repeatedly.
constexpr int kWirePlumberRestartMinIntervalMs = 60000;
constexpr int kWirePlumberRestartTimeoutMs = 5000;
}

MediaController::MediaController(QObject *parent) : QObject(parent) {
  m_pulseAudio = new PulseAudioController(this);
  if (!m_pulseAudio->initialize())
  {
    LOG_ERROR("Failed to initialize PulseAudio controller");
  }
}

void MediaController::handleEarDetection(EarDetection *earDetection)
{
  if (earDetectionBehavior == Disabled)
  {
    LOG_DEBUG("Ear detection is disabled, ignoring status");
    return;
  }

  bool primaryInEar = earDetection->isPrimaryInEar();
  bool secondaryInEar = earDetection->isSecondaryInEar();

  // Query once and reuse: each call is two blocking IPC round trips, and
  // re-querying would also let the three uses below disagree within one packet.
  const bool airpodsActive = isActiveOutputDeviceAirPods();

  LOG_DEBUG("Ear detection status: primaryInEar="
            << primaryInEar << ", secondaryInEar=" << secondaryInEar
            << ", isAirPodsActive=" << airpodsActive);

  // First handle playback pausing based on selected behavior
  bool shouldPause = false;
  bool shouldResume = false;

  if (earDetectionBehavior == PauseWhenOneRemoved)
  {
    shouldPause = !primaryInEar || !secondaryInEar;
    shouldResume = primaryInEar && secondaryInEar;
  }
  else if (earDetectionBehavior == PauseWhenBothRemoved)
  {
    shouldPause = !primaryInEar && !secondaryInEar;
    shouldResume = primaryInEar || secondaryInEar;
  }

  if (shouldPause && airpodsActive)
  {
    if (getCurrentMediaState() == Playing)
    {
      LOG_DEBUG("Pausing playback for ear detection");
      pause();
    }
  }

  // Then handle device profile switching
  if (primaryInEar || secondaryInEar)
  {
    LOG_INFO("At least one AirPod is in ear");
    activateA2dpProfile();

    // Re-query rather than reusing the value from the top of this function:
    // activateA2dpProfile() above may have just made the AirPods the active
    // output, which is precisely the case where a resume is wanted.
    if (shouldResume && !pausedByAppServices.isEmpty() && isActiveOutputDeviceAirPods())
    {
      play();
    }
  }
  else
  {
    LOG_INFO("Both AirPods are out of ear");
    removeAudioOutputDevice();
  }
}

void MediaController::setEarDetectionBehavior(EarDetectionBehavior behavior)
{
  earDetectionBehavior = behavior;
  LOG_INFO("Set ear detection behavior to: " << behavior);
}

void MediaController::followMediaChanges() {
  playerStatusWatcher = new PlayerStatusWatcher("", this);
  connect(playerStatusWatcher, &PlayerStatusWatcher::playbackStatusChanged,
          this, [this](const QString &status)
          {
            LOG_DEBUG("Playback status changed: " << status);
            MediaState state = mediaStateFromPlayerctlOutput(status);
            emit mediaStateChanged(state);
          });
}

bool MediaController::isActiveOutputDeviceAirPods() {
  if (connectedDeviceMacAddress.isEmpty()) {
    return false;
  }
  QString defaultSinkMacAddress = m_pulseAudio->getDefaultSinkMacAddress();
  LOG_DEBUG("Default sink MAC address: " << defaultSinkMacAddress);
  if (defaultSinkMacAddress.isEmpty()) {
    return false;
  }
  // Accept either separator: the sink property is colon-form, but fall back to
  // the underscored form the card path also tolerates.
  return defaultSinkMacAddress.compare(connectedDeviceMacAddress, Qt::CaseInsensitive) == 0 ||
         defaultSinkMacAddress.compare(QString(connectedDeviceMacAddress).replace(':', '_'),
                                       Qt::CaseInsensitive) == 0;
}

void MediaController::handleConversationalAwareness(const QByteArray &data) {
    if (data.size() < 10) {
        LOG_ERROR("Invalid conversational awareness packet");
        return;
    }

    uint8_t flag = (uint8_t)data[9];

    switch (flag) {
    case 0x01:
        LOG_INFO("Conversational awareness event: voice detected");

        if (initialVolume == -1 && isActiveOutputDeviceAirPods()) {
            QString sink = m_pulseAudio->getDefaultSink();
            initialVolume = m_pulseAudio->getSinkVolume(sink);
            LOG_DEBUG("Initial volume saved: " << initialVolume << "%");
        }

        if (initialVolume != -1) {
            QString sink = m_pulseAudio->getDefaultSink();
            int target = initialVolume * 0.20;
            m_pulseAudio->setSinkVolume(sink, target);
            LOG_INFO("Volume lowered to " << target << "%");
        }
        break;

    case 0x08:
        LOG_INFO("Conversational awareness disabled");
        initialVolume = -1;
        break;

    case 0x09:
        LOG_INFO("Conversational awareness enabled");
        break;

    default:
        LOG_INFO("Conversational awareness event: voice ended");

        if (initialVolume != -1 && isActiveOutputDeviceAirPods()) {
            QString sink = m_pulseAudio->getDefaultSink();
            m_pulseAudio->setSinkVolume(sink, initialVolume);
            LOG_INFO("Volume restored to " << initialVolume << "%");
            initialVolume = -1;
        }
        break;
    }
}


bool MediaController::isA2dpProfileAvailable() {
  if (m_deviceOutputName.isEmpty()) {
    return false;
  }

  return m_pulseAudio->isProfileAvailable(m_deviceOutputName, "a2dp-sink-sbc_xq") || 
         m_pulseAudio->isProfileAvailable(m_deviceOutputName, "a2dp-sink-sbc") ||
         m_pulseAudio->isProfileAvailable(m_deviceOutputName, "a2dp-sink");
}

QString MediaController::getPreferredA2dpProfile() {
  if (m_deviceOutputName.isEmpty()) {
    return QString();
  }

  if (!m_cachedA2dpProfile.isEmpty() && 
      m_pulseAudio->isProfileAvailable(m_deviceOutputName, m_cachedA2dpProfile)) {
    return m_cachedA2dpProfile;
  }

  QStringList profiles = {"a2dp-sink-sbc_xq", "a2dp-sink-sbc", "a2dp-sink"};

  for (const QString &profile : profiles) {
    if (m_pulseAudio->isProfileAvailable(m_deviceOutputName, profile)) {
      LOG_INFO("Selected best available A2DP profile: " << profile);
      m_cachedA2dpProfile = profile;
      return profile;
    }
  }

  m_cachedA2dpProfile.clear();
  return QString();
}

bool MediaController::restartWirePlumber() {
  // Restarting the audio server kills every stream on the machine and blocks
  // this (GUI) thread for 2s, so rate-limit it hard. Now that card resolution
  // retries properly, reaching here at all should be rare.
  if (m_lastWirePlumberRestart.isValid() &&
      m_lastWirePlumberRestart.elapsed() < kWirePlumberRestartMinIntervalMs) {
    LOG_WARN("Skipping WirePlumber restart; one was attempted "
             << m_lastWirePlumberRestart.elapsed() << "ms ago");
    return false;
  }
  m_lastWirePlumberRestart.start();

  LOG_INFO("Restarting WirePlumber to rediscover A2DP profiles");
  // Bounded wait: QProcess::execute() uses waitForFinished(-1), so a systemd
  // user manager that is busy or wedged would freeze this (GUI) thread with no
  // limit at all.
  QProcess systemctl;
  systemctl.start("systemctl", QStringList() << "--user" << "restart" << "wireplumber");
  if (!systemctl.waitForFinished(kWirePlumberRestartTimeoutMs)) {
    LOG_ERROR("systemctl restart wireplumber did not finish in time; killing it");
    systemctl.kill();
    systemctl.waitForFinished(1000);
    return false;
  }
  if (systemctl.exitStatus() == QProcess::NormalExit && systemctl.exitCode() == 0) {
    LOG_INFO("WirePlumber restarted successfully");
    QThread::sleep(2);
    return true;
  }
  LOG_ERROR("Failed to restart WirePlumber. Do you use wireplumber?");
  return false;
}

void MediaController::activateA2dpProfile() {
  // Several entry points (startup, connect, wake, metadata, every ear-detection
  // packet) can fire seconds apart. Without this guard each would start its own
  // independent retry chain, and the last to resolve would win -- extending the
  // window in which a stale chain can act on superseded state.
  // Only refuse a duplicate of the CURRENT generation. Refusing across
  // generations would drop the activation: the newer caller gets turned away
  // while the older chain aborts on its generation check, and nothing runs.
  if (m_a2dpPendingGeneration == m_a2dpGeneration) {
    LOG_DEBUG("A2DP activation already in flight for this generation");
    return;
  }
  activateA2dpProfileWithRetry(kA2dpActivationAttempts, m_a2dpGeneration);
}

// PipeWire creates the bluez card asynchronously, some seconds after the
// Bluetooth link comes up, so the lookup done at connect time legitimately
// misses. Re-resolve here and retry: the cached empty name must not become a
// latch that blocks activation until the next ear-detection packet arrives.
//
// `generation` is a cancellation token. Anything that supersedes a pending
// activation -- removing the pods, or a new device -- bumps m_a2dpGeneration,
// so an in-flight chain aborts instead of switching the profile back on after
// the user has already cased the pods.
void MediaController::activateA2dpProfileWithRetry(int attemptsLeft, int generation) {
  // Order matters: check for supersession BEFORE clearing the marker, so a
  // stale chain's abort cannot clear a newer chain's marker and let a third
  // caller start a duplicate.
  if (generation != m_a2dpGeneration) {
    LOG_DEBUG("A2DP activation superseded, abandoning chain");
    return;
  }
  m_a2dpPendingGeneration = -1;

  if (connectedDeviceMacAddress.isEmpty()) {
    LOG_WARN("Connected device MAC address is empty, cannot activate A2DP profile");
    return;
  }

  if (m_deviceOutputName.isEmpty()) {
    m_deviceOutputName = getAudioDeviceName();
  }

  if (m_deviceOutputName.isEmpty()) {
    if (attemptsLeft > 1) {
      m_a2dpPendingGeneration = generation;
      QTimer::singleShot(kA2dpActivationRetryMs, this, [this, attemptsLeft, generation]() {
        activateA2dpProfileWithRetry(attemptsLeft - 1, generation);
      });
    } else {
      LOG_ERROR("No Bluetooth card appeared for " << connectedDeviceMacAddress
                << "; cannot activate A2DP profile");
    }
    return;
  }

  if (!isA2dpProfileAvailable()) {
    LOG_WARN("A2DP profile not available, attempting to restart WirePlumber");
    if (restartWirePlumber()) {
      m_deviceOutputName = getAudioDeviceName();
      if (!isA2dpProfileAvailable()) {
        LOG_ERROR("A2DP profile still not available after WirePlumber restart");
        return;
      }
    } else {
      // The restart was rate-limited or failed. Profiles are often just not
      // enumerated yet, so re-arm rather than treating this as terminal --
      // otherwise a transient gap ends activation for good.
      if (attemptsLeft > 1) {
        m_a2dpPendingGeneration = generation;
        QTimer::singleShot(kA2dpActivationRetryMs, this, [this, attemptsLeft, generation]() {
          activateA2dpProfileWithRetry(attemptsLeft - 1, generation);
        });
      } else {
        LOG_ERROR("A2DP profile unavailable and WirePlumber restart unavailable");
      }
      return;
    }
  }

  QString preferredProfile = getPreferredA2dpProfile();
  if (preferredProfile.isEmpty()) {
    LOG_ERROR("No suitable A2DP profile found");
    return;
  }

  // Ear-detection packets repeat while the state is unchanged; re-setting an
  // already-active profile only churns the sink, so make this idempotent.
  // Any A2DP profile counts, not just the preferred one -- forcing a switch
  // away from a working AAC/aptX/LDAC sink would tear it down mid-playback and
  // silently downgrade the codec.
  const QString activeProfile = m_pulseAudio->getActiveCardProfile(m_deviceOutputName);
  if (activeProfile.startsWith("a2dp-sink")) {
    LOG_DEBUG("An A2DP profile is already active: " << activeProfile);
    m_weTurnedItOff = false;
    return;
  }

  LOG_INFO("Activating A2DP profile for AirPods: " << preferredProfile);
  if (!m_pulseAudio->setCardProfile(m_deviceOutputName, preferredProfile)) {
    LOG_ERROR("Failed to activate A2DP profile: " << preferredProfile);
    // The card may have been destroyed under us; force a re-resolve next time
    // rather than latching on a name that no longer exists.
    m_deviceOutputName.clear();
    return;
  }
  m_weTurnedItOff = false;
  LOG_INFO("A2DP profile activated successfully");
}

void MediaController::removeAudioOutputDevice() {
  // Supersede any in-flight activation: the pods are out, so a chain still
  // waiting for the card to appear must not switch A2DP back on behind us.
  m_a2dpGeneration++;

  if (connectedDeviceMacAddress.isEmpty()) {
    LOG_WARN("Connected device MAC address is empty, cannot remove audio output device");
    return;
  }

  // Resolve the same way activation does. Bailing out on an empty name here
  // while activation re-resolves would silently drop a removal issued during
  // the window where the card has not appeared yet.
  if (m_deviceOutputName.isEmpty()) {
    m_deviceOutputName = getAudioDeviceName();
  }
  if (m_deviceOutputName.isEmpty()) {
    LOG_DEBUG("No Bluetooth card for " << connectedDeviceMacAddress
              << "; nothing to remove");
    return;
  }

  if (m_pulseAudio->getActiveCardProfile(m_deviceOutputName) == "off") {
    LOG_DEBUG("AirPods already removed as audio output device");
    // We were asked to turn it off and it already is -- possibly an "off"
    // persisted by an earlier session. Take ownership so shutdown still hands
    // the profile back rather than leaving the card sinkless indefinitely.
    m_weTurnedItOff = true;
    return;
  }

  LOG_INFO("Removing AirPods as audio output device");
  if (!m_pulseAudio->setCardProfile(m_deviceOutputName, "off")) {
    LOG_ERROR("Failed to remove AirPods as audio output device");
    m_deviceOutputName.clear();
    return;
  }
  // WirePlumber persists this to ~/.local/state/wireplumber/default-profile,
  // so it survives our process. Remember that we are the ones who set it, so
  // shutdown can undo it rather than leaving the card with no sink.
  m_weTurnedItOff = true;
}

void MediaController::restoreProfileIfWeTurnedItOff() {
  if (!m_weTurnedItOff || m_deviceOutputName.isEmpty()) {
    return;
  }
  if (m_pulseAudio->getActiveCardProfile(m_deviceOutputName) != "off") {
    return; // something else already changed it; leave the user's choice alone
  }
  const QString profile = getPreferredA2dpProfile();
  if (profile.isEmpty()) {
    return;
  }
  LOG_INFO("Restoring A2DP profile on shutdown so the card is not left off: " << profile);
  m_pulseAudio->setCardProfile(m_deviceOutputName, profile);
  m_weTurnedItOff = false;
}

void MediaController::clearConnectedDevice() {
  // Try to hand back the profile before dropping the state that records we owe
  // it. Disconnect is the ordinary end of a listening session, so clearing the
  // obligation here would mean the restore almost never runs.
  restoreProfileIfWeTurnedItOff();
  m_a2dpGeneration++;
  m_a2dpPendingGeneration = -1;
  connectedDeviceMacAddress.clear();
  m_deviceOutputName.clear();
  m_cachedA2dpProfile.clear();
  m_weTurnedItOff = false;
}

void MediaController::setConnectedDeviceMacAddress(const QString &macAddress) {
  // A new (or re-established) device supersedes any pending activation chain.
  m_a2dpGeneration++;
  connectedDeviceMacAddress = macAddress;
  m_deviceOutputName = getAudioDeviceName();
  m_cachedA2dpProfile.clear();
  LOG_INFO("Device output name set to: " << m_deviceOutputName);
}

MediaController::MediaState MediaController::mediaStateFromPlayerctlOutput(
    const QString &output) const {
  if (output == "Playing") {
    return MediaState::Playing;
  } else if (output == "Paused") {
    return MediaState::Paused;
  } else {
    return MediaState::Stopped;
  }
}

MediaController::MediaState MediaController::getCurrentMediaState() const
{
  return mediaStateFromPlayerctlOutput(PlayerStatusWatcher::getCurrentPlaybackStatus(""));
}

QStringList MediaController::getPlayingMediaPlayers()
{
  QStringList playingServices;
  QDBusConnection bus = QDBusConnection::sessionBus();

  QStringList services = bus.interface()->registeredServiceNames().value();
  for (const QString &service : services)
  {
    if (!service.startsWith("org.mpris.MediaPlayer2."))
    {
      continue;
    }

    QDBusInterface playerInterface(
        service,
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        bus);

    if (!playerInterface.isValid())
    {
      continue;
    }

    QVariant playbackStatus = playerInterface.property("PlaybackStatus");
    if (playbackStatus.isValid() && playbackStatus.toString() == "Playing")
    {
      playingServices << service;
      LOG_DEBUG("Found playing service: " << service);
    }
  }

  return playingServices;
}

void MediaController::play()
{
  if (pausedByAppServices.isEmpty())
  {
    LOG_INFO("No services to resume");
    return;
  }

  QDBusConnection bus = QDBusConnection::sessionBus();
  int resumedCount = 0;

  for (const QString &service : pausedByAppServices)
  {
    QDBusInterface playerInterface(
        service,
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        bus);

    if (!playerInterface.isValid())
    {
      LOG_WARN("Service no longer available: " << service);
      continue;
    }

    QDBusReply<void> reply = playerInterface.call("Play");
    if (reply.isValid())
    {
      LOG_INFO("Resumed playback for: " << service);
      resumedCount++;
    }
    else
    {
      LOG_ERROR("Failed to resume " << service << ": " << reply.error().message());
    }
  }

  if (resumedCount > 0)
  {
    LOG_INFO("Resumed " << resumedCount << " media player(s) via DBus");
    pausedByAppServices.clear();
  }
  else
  {
    LOG_ERROR("Failed to resume any media players via DBus");
  }
}

void MediaController::pause()
{
  QDBusConnection bus = QDBusConnection::sessionBus();
  QStringList services = bus.interface()->registeredServiceNames().value();

  pausedByAppServices.clear();
  int pausedCount = 0;

  for (const QString &service : services)
  {
    if (!service.startsWith("org.mpris.MediaPlayer2."))
    {
      continue;
    }

    QDBusInterface playerInterface(
        service,
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        bus);

    if (!playerInterface.isValid())
    {
      continue;
    }

    QVariant playbackStatus = playerInterface.property("PlaybackStatus");
    LOG_DEBUG("PlaybackStatus for " << service << ": " << playbackStatus.toString());
    if (!playbackStatus.isValid() || playbackStatus.toString() != "Playing")
    {
      continue;
    }

    QDBusReply<void> reply = playerInterface.call("Pause");
    LOG_DEBUG("Pausing service: " << service);
    if (reply.isValid())
    {
      LOG_INFO("Paused playback for: " << service);
      pausedByAppServices << service;
      pausedCount++;
    }
    else
    {
      LOG_ERROR("Failed to pause " << service << ": " << reply.error().message());
    }
  }

  if (pausedCount > 0)
  {
    LOG_INFO("Paused " << pausedCount << " media player(s) via DBus");
  }
  else
  {
    LOG_INFO("No playing media players found to pause");
  }
}

MediaController::~MediaController() {
  // Stopping while the pods are out of the ears would otherwise leave the card
  // on a persisted "off" with no process left to undo it -- the user's AirPods
  // would have no sink at all until they fixed it by hand. Restarting this
  // service is routine (it is what a Qt rebuild requires), so this matters.
  restoreProfileIfWeTurnedItOff();
}

QString MediaController::getAudioDeviceName()
{
  if (connectedDeviceMacAddress.isEmpty()) { return QString(); }

  QString cardName = m_pulseAudio->getCardNameForDevice(connectedDeviceMacAddress);
  if (cardName.isEmpty()) {
    // Expected while PipeWire is still creating the card; callers that retry
    // report the terminal failure, so this is not an error on its own.
    LOG_DEBUG("No matching Bluetooth card (yet) for MAC address: " << connectedDeviceMacAddress);
  }
  return cardName;
}