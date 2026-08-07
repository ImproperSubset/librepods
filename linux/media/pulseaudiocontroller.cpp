#include "pulseaudiocontroller.h"
#include "logger.h"
#include <QThread>

PulseAudioController::PulseAudioController(QObject *parent)
    : QObject(parent), m_mainloop(nullptr), m_context(nullptr), m_initialized(false)
{
}

PulseAudioController::~PulseAudioController()
{
    // Stop the mainloop thread BEFORE touching the context. Every pa_* call
    // other than the mainloop's own lock/stop/free must be made with the lock
    // held; disconnecting the context while the poll loop is still running is a
    // data race on it.
    if (m_mainloop)
    {
        pa_threaded_mainloop_stop(m_mainloop);
    }
    if (m_context)
    {
        // Clear the state callback first so it cannot fire against a
        // half-destroyed controller.
        pa_context_set_state_callback(m_context, nullptr, nullptr);
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
        m_context = nullptr;
    }
    if (m_mainloop)
    {
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }
    m_initialized = false;
}

bool PulseAudioController::initialize()
{
    m_mainloop = pa_threaded_mainloop_new();
    if (!m_mainloop)
    {
        LOG_ERROR("Failed to create PulseAudio mainloop");
        return false;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_mainloop);
    m_context = pa_context_new(api, "LibrePods");
    if (!m_context)
    {
        LOG_ERROR("Failed to create PulseAudio context");
        return false;
    }

    pa_context_set_state_callback(m_context, contextStateCallback, this);
    
    if (pa_threaded_mainloop_start(m_mainloop) < 0)
    {
        LOG_ERROR("Failed to start PulseAudio mainloop");
        return false;
    }

    pa_threaded_mainloop_lock(m_mainloop);
    
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
    {
        LOG_ERROR("Failed to connect to PulseAudio");
        pa_threaded_mainloop_unlock(m_mainloop);
        return false;
    }

    // Wait for context to be ready
    while (pa_context_get_state(m_context) != PA_CONTEXT_READY)
    {
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(m_context)))
        {
            LOG_ERROR("PulseAudio context failed");
            pa_threaded_mainloop_unlock(m_mainloop);
            return false;
        }
        pa_threaded_mainloop_wait(m_mainloop);
    }

    pa_threaded_mainloop_unlock(m_mainloop);
    m_initialized = true;
    LOG_INFO("PulseAudio controller initialized");
    return true;
}

void PulseAudioController::contextStateCallback(pa_context *c, void *userdata)
{
    PulseAudioController *controller = static_cast<PulseAudioController*>(userdata);

    // If the server goes away (pipewire-pulse restart or crash), the context
    // stays FAILED forever. Without clearing m_initialized every later call
    // proceeds, gets a null operation back, and returns empty -- permanently
    // and silently. Record it so callers can see the controller is dead.
    const pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
    {
        if (controller->m_initialized)
        {
            LOG_ERROR("PulseAudio context lost (state " << state
                      << "); audio control is unavailable until reconnected");
        }
        controller->m_initialized = false;
    }

    pa_threaded_mainloop_signal(controller->m_mainloop, 0);
}

QString PulseAudioController::getDefaultSink()
{
    if (!m_initialized) return QString();

    struct CallbackData {
        QString sinkName;
        pa_threaded_mainloop *mainloop;
    } data;
    data.mainloop = m_mainloop;

    auto callback = [](pa_context *c, const pa_server_info *info, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        if (info && info->default_sink_name)
        {
            d->sinkName = QString::fromUtf8(info->default_sink_name);
        }
        pa_threaded_mainloop_signal(d->mainloop, 0);
    };

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_get_server_info(m_context, callback, &data);
    if (op)
    {
        waitForOperation(op);
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    return data.sinkName;
}

QString PulseAudioController::getDefaultSinkMacAddress()
{
    return getMacAddressBySinkName(getDefaultSink());
}

// Identifies a sink by its `device.string` property rather than by parsing the
// sink name: WirePlumber >= 0.5.13 changed the name format, so substring
// matching against a MAC silently stopped working. See upstream issue #418.
QString PulseAudioController::getMacAddressBySinkName(const QString &sinkName)
{
    if (!m_initialized || sinkName.isEmpty()) return QString();

    struct CallbackData {
        QString sinkMacAddress;
        pa_threaded_mainloop *mainloop;
    } data;
    data.mainloop = m_mainloop;

    auto callback = [](pa_context *c, const pa_sink_info *info, int eol, void *userdata)
    {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        if (eol != 0) // eol < 0 is the error path; it must signal too
        {
            pa_threaded_mainloop_signal(d->mainloop, 0);
            return;
        }
        if (info)
        {
            // Same reasoning as the card path: this property's presentation is
            // what varies across WirePlumber versions, so read a second source
            // before giving up. api.bluez5.address carries the same colon-form
            // MAC on bluez devices.
            const char *addr = pa_proplist_gets(info->proplist, "device.string");
            if (!addr)
            {
                addr = pa_proplist_gets(info->proplist, "api.bluez5.address");
            }
            if (addr)
            {
                d->sinkMacAddress = QString::fromUtf8(addr);
            }
        }
    };

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_get_sink_info_by_name(m_context, sinkName.toUtf8().constData(), callback, &data);
    if (op)
    {
        waitForOperation(op);
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    return data.sinkMacAddress;
}

int PulseAudioController::getSinkVolume(const QString &sinkName)
{
    if (!m_initialized) return -1;

    struct CallbackData {
        int volume;
        QString targetSink;
        pa_threaded_mainloop *mainloop;
    } data;
    data.volume = -1;
    data.targetSink = sinkName;
    data.mainloop = m_mainloop;

    auto callback = [](pa_context *c, const pa_sink_info *info, int eol, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        if (eol != 0) // eol < 0 is the error path; it must signal too
        {
            pa_threaded_mainloop_signal(d->mainloop, 0);
            return;
        }
        if (info && QString::fromUtf8(info->name) == d->targetSink)
        {
            d->volume = (pa_cvolume_avg(&info->volume) * 100) / PA_VOLUME_NORM;
            pa_threaded_mainloop_signal(d->mainloop, 0);
        }
    };

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_get_sink_info_by_name(m_context, sinkName.toUtf8().constData(), callback, &data);
    if (op)
    {
        waitForOperation(op);
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    return data.volume;
}

bool PulseAudioController::setSinkVolume(const QString &sinkName, int volumePercent)
{
    if (!m_initialized) return false;

    pa_cvolume volume;
    pa_cvolume_set(&volume, 2, (volumePercent * PA_VOLUME_NORM) / 100);

    pa_threaded_mainloop_lock(m_mainloop);

    // Same as setCardProfile: a rejected request still completes as DONE.
    struct CallbackData {
        int success;
        pa_threaded_mainloop *mainloop;
    } data{0, m_mainloop};

    auto successCallback = [](pa_context *c, int success, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        d->success = success;
        pa_threaded_mainloop_signal(d->mainloop, 0);
    };

    pa_operation *op = pa_context_set_sink_volume_by_name(m_context, sinkName.toUtf8().constData(), &volume, successCallback, &data);

    bool completed = waitForOperation(op);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);

    return completed && data.success != 0;
}

bool PulseAudioController::setCardProfile(const QString &cardName, const QString &profileName)
{
    if (!m_initialized) return false;

    pa_threaded_mainloop_lock(m_mainloop);

    // The operation completing is NOT the same as the server accepting it: a
    // rejected request (no such card, no such profile, card destroyed under us)
    // still completes as PA_OPERATION_DONE with success == 0. Reporting that as
    // success makes every caller's failure handling dead code.
    struct CallbackData {
        int success;
        pa_threaded_mainloop *mainloop;
    } data{0, m_mainloop};

    auto successCallback = [](pa_context *c, int success, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        d->success = success;
        pa_threaded_mainloop_signal(d->mainloop, 0);
    };

    pa_operation *op = pa_context_set_card_profile_by_name(m_context,
        cardName.toUtf8().constData(),
        profileName.toUtf8().constData(),
        successCallback, &data);
    bool completed = waitForOperation(op);
    if (op) pa_operation_unref(op);
    pa_threaded_mainloop_unlock(m_mainloop);

    if (completed && data.success == 0) {
        LOG_WARN("PulseAudio rejected profile '" << profileName << "' on card '" << cardName << "'");
    }
    return completed && data.success != 0;
}

QString PulseAudioController::getCardNameForDevice(const QString &macAddress)
{
    if (!m_initialized) return QString();

    struct CallbackData {
        QString cardName;
        QString fallbackCardName;
        QString targetMac;
        QString targetMacUnderscored;
        pa_threaded_mainloop *mainloop;
    } data;
    data.targetMac = macAddress;
    data.targetMacUnderscored = QString(macAddress).replace(':', '_');
    data.mainloop = m_mainloop;

    auto callback = [](pa_context *c, const pa_card_info *info, int eol, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        if (eol != 0) // eol < 0 is the error path; it must signal too
        {
            pa_threaded_mainloop_signal(d->mainloop, 0);
            return;
        }
        if (info)
        {
            // Prefer `device.string` (the colon-form MAC) over parsing the card
            // name: WirePlumber >= 0.5.13 changed the name format. See upstream
            // issue #418. The name heuristic is kept as a fallback because that
            // property's presentation is exactly what varies between versions --
            // losing both would reproduce the no-audio bug this fixes.
            const char *addr = pa_proplist_gets(info->proplist, "device.string");
            const QString name = QString::fromUtf8(info->name);
            if (addr && d->targetMac.compare(QString::fromUtf8(addr), Qt::CaseInsensitive) == 0)
            {
                d->cardName = name;
            }
            else if (name.startsWith("bluez") &&
                     name.contains(d->targetMacUnderscored, Qt::CaseInsensitive))
            {
                d->fallbackCardName = name;
            }
        }
    };

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_get_card_info_list(m_context, callback, &data);
    if (op)
    {
        waitForOperation(op);
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    if (data.cardName.isEmpty() && !data.fallbackCardName.isEmpty())
    {
        LOG_WARN("Card matched by name heuristic, not device.string -- "
                 "this PipeWire/WirePlumber build reports the property differently: "
                 << data.fallbackCardName);
        return data.fallbackCardName;
    }

    return data.cardName;
}

QString PulseAudioController::getActiveCardProfile(const QString &cardName)
{
    if (!m_initialized || cardName.isEmpty()) return QString();

    struct CallbackData {
        QString activeProfile;
        pa_threaded_mainloop *mainloop;
    } data;
    data.mainloop = m_mainloop;

    auto callback = [](pa_context *c, const pa_card_info *info, int eol, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        if (eol != 0) // eol < 0 is the error path; it must signal too
        {
            pa_threaded_mainloop_signal(d->mainloop, 0);
            return;
        }
        // active_profile2 is documented as nullable.
        if (info && info->active_profile2 && info->active_profile2->name)
        {
            d->activeProfile = QString::fromUtf8(info->active_profile2->name);
        }
    };

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_get_card_info_by_name(m_context, cardName.toUtf8().constData(), callback, &data);
    if (op)
    {
        waitForOperation(op);
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    return data.activeProfile;
}

bool PulseAudioController::isProfileAvailable(const QString &cardName, const QString &profileName)
{
    if (!m_initialized) return false;

    struct CallbackData {
        bool available;
        QString targetCard;
        QString targetProfile;
        pa_threaded_mainloop *mainloop;
    } data;
    data.available = false;
    data.targetCard = cardName;
    data.targetProfile = profileName;
    data.mainloop = m_mainloop;

    auto callback = [](pa_context *c, const pa_card_info *info, int eol, void *userdata) {
        CallbackData *d = static_cast<CallbackData*>(userdata);
        if (eol != 0) // eol < 0 is the error path; it must signal too
        {
            pa_threaded_mainloop_signal(d->mainloop, 0);
            return;
        }
        if (info && QString::fromUtf8(info->name) == d->targetCard)
        {
            for (uint32_t i = 0; i < info->n_profiles; i++)
            {
                if (QString::fromUtf8(info->profiles[i].name) == d->targetProfile)
                {
                    d->available = true;
                    break;
                }
            }
            pa_threaded_mainloop_signal(d->mainloop, 0);
        }
    };

    pa_threaded_mainloop_lock(m_mainloop);
    pa_operation *op = pa_context_get_card_info_by_name(m_context, cardName.toUtf8().constData(), callback, &data);
    if (op)
    {
        waitForOperation(op);
        pa_operation_unref(op);
    }
    pa_threaded_mainloop_unlock(m_mainloop);

    return data.available;
}

bool PulseAudioController::waitForOperation(pa_operation *op)
{
    if (!op) return false;

    // Wake on the operation leaving RUNNING regardless of whether its info
    // callback signalled. Relying on the info callback alone blocks this
    // thread forever if the operation errors or is cancelled without one --
    // and this runs on the GUI thread. Safe to register here: the caller holds
    // the mainloop lock, so no state change can be dispatched before the wait.
    auto stateCallback = [](pa_operation *o, void *userdata) {
        pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop*>(userdata), 0);
    };
    pa_operation_set_state_callback(op, stateCallback, m_mainloop);

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
        pa_threaded_mainloop_wait(m_mainloop);
    }

    // The operation outlives this call only until the caller unrefs it, but
    // clear the callback so a late state change cannot reach a stale mainloop.
    pa_operation_set_state_callback(op, nullptr, nullptr);

    return pa_operation_get_state(op) == PA_OPERATION_DONE;
}
