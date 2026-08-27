#include "BluetoothMonitor.h"
#include "logger.h"

#include <QDebug>
#include <QDBusObjectPath>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {
// BlueZ pages the device before answering; the default D-Bus timeout (25s) is
// generous but this makes the bound explicit.
constexpr int kConnectTimeoutMs = 20000;
// Bound for the blocking GetManagedObjects lookup, which runs on the GUI
// thread. Short: a wedged bluetoothd must not freeze the UI.
constexpr int kManagedObjectsTimeoutMs = 3000;
}

BluetoothMonitor::BluetoothMonitor(QObject *parent)
    : QObject(parent), m_dbus(QDBusConnection::systemBus())
{
    // Register meta-types for D-Bus interaction
    qDBusRegisterMetaType<QDBusObjectPath>();
    qDBusRegisterMetaType<ManagedObjectList>();

    if (!m_dbus.isConnected())
    {
        LOG_WARN("Failed to connect to system D-Bus");
        return;
    }

    registerDBusService();
    checkAlreadyConnectedDevices(); // Check for already connected devices on startup
}

BluetoothMonitor::~BluetoothMonitor()
{
    m_dbus.disconnectFromBus(m_dbus.name());
}

void BluetoothMonitor::registerDBusService()
{
    // Match signals for PropertiesChanged on any BlueZ Device interface
    if (!m_dbus.connect("", "", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList))))
    {
        LOG_WARN("Failed to connect to D-Bus PropertiesChanged signal");
    }
}

bool BluetoothMonitor::isAirPodsDevice(const QString &devicePath)
{
    QDBusInterface deviceInterface("org.bluez", devicePath, "org.freedesktop.DBus.Properties", m_dbus);

    // Get UUIDs to check if it's an AirPods device
    QDBusReply<QVariant> uuidsReply = deviceInterface.call("Get", "org.bluez.Device1", "UUIDs");
    if (!uuidsReply.isValid())
    {
        return false;
    }

    QStringList uuids = uuidsReply.value().toStringList();
    return uuids.contains("74ec2172-0bad-4d01-8f77-997b2be0722a");
}

QString BluetoothMonitor::getDeviceName(const QString &devicePath)
{
    QDBusInterface deviceInterface("org.bluez", devicePath, "org.freedesktop.DBus.Properties", m_dbus);
    QDBusReply<QVariant> nameReply = deviceInterface.call("Get", "org.bluez.Device1", "Name");
    if (nameReply.isValid())
    {
        return nameReply.value().toString();
    }
    return "Unknown";
}

bool BluetoothMonitor::fetchManagedObjects(ManagedObjectList &out)
{
    QDBusInterface objectManager("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", m_dbus);
    // This is a blocking call on the GUI thread, reached from the media-play
    // path. The D-Bus default of 25s would freeze the UI for that long if
    // bluetoothd were wedged; a stalled lookup should fail fast instead.
    objectManager.setTimeout(kManagedObjectsTimeoutMs);
    QDBusMessage reply = objectManager.call("GetManagedObjects");

    if (reply.type() == QDBusMessage::ErrorMessage)
    {
        LOG_WARN("Failed to get managed objects: " << reply.errorMessage());
        return false;
    }

    if (reply.arguments().isEmpty())
    {
        LOG_WARN("GetManagedObjects returned no arguments");
        return false;
    }

    QVariant firstArg = reply.arguments().constFirst();
    QDBusArgument arg = firstArg.value<QDBusArgument>();
    arg >> out;
    return true;
}

// Identifies AirPods by BlueZ's cached UUIDs rather than by
// QBluetoothDeviceInfo::serviceUuids(), which is empty for a device built from
// an address alone and so never matches.
BluetoothMonitor::AirPodsDevice BluetoothMonitor::findPairedAirPods()
{
    AirPodsDevice result;

    ManagedObjectList managedObjects;
    if (!fetchManagedObjects(managedObjects))
    {
        return result;
    }

    for (auto it = managedObjects.constBegin(); it != managedObjects.constEnd(); ++it)
    {
        if (!it.value().contains("org.bluez.Device1"))
        {
            continue;
        }
        const QVariantMap &props = it.value().value("org.bluez.Device1");
        if (!props.contains("UUIDs") || !props.contains("Address"))
        {
            continue;
        }
        if (!props["UUIDs"].toStringList().contains("74ec2172-0bad-4d01-8f77-997b2be0722a"))
        {
            continue;
        }
        // Require Paired: BlueZ also keeps Device1 objects for merely
        // discovered devices, and asking one of those to connect either fails
        // or provokes a pairing attempt.
        if (!props.value("Paired").toBool())
        {
            continue;
        }

        AirPodsDevice candidate;
        candidate.address = props["Address"].toString();
        candidate.path = it.key().path();
        candidate.connected = props.value("Connected").toBool();

        // ManagedObjectList is a QMap, so iteration is ordered by object path,
        // i.e. by MAC. With two paired sets that would deterministically pick
        // the lower MAC -- possibly the pair sitting in its case. Prefer a
        // connected device, which is the one actually in use.
        if (candidate.connected)
        {
            return candidate;
        }
        if (!result.isValid())
        {
            result = candidate;
        }
    }
    return result;
}

bool BluetoothMonitor::connectDeviceByPath(const QString &devicePath, const QString &macAddress)
{
    if (devicePath.isEmpty())
    {
        LOG_WARN("No BlueZ device object for " << macAddress << "; cannot connect");
        return false;
    }

    // Media-play events arrive in bursts (scrubbing, track changes), and the
    // window while BlueZ pages the device is seconds long. Without this guard
    // each event queues another Connect, which BlueZ answers with InProgress.
    if (!m_connectInFlight.isEmpty())
    {
        LOG_DEBUG("Connect already in flight for " << m_connectInFlight << "; ignoring request");
        return false;
    }
    m_connectInFlight = macAddress;

    // Fire and observe, never block: BlueZ pages the device, which routinely
    // takes seconds. Shelling out to `bluetoothctl connect` instead would both
    // block this thread and, on failure, leave the tool scanning and dump its
    // entire discovery stream into the journal.
    QDBusMessage msg = QDBusMessage::createMethodCall(
        "org.bluez", devicePath, "org.bluez.Device1", "Connect");
    QDBusPendingCall call = m_dbus.asyncCall(msg, kConnectTimeoutMs);

    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, macAddress](QDBusPendingCallWatcher *self) {
                m_connectInFlight.clear();
                QDBusPendingReply<> reply = *self;
                if (reply.isError())
                {
                    // A duplicate or redundant request is expected, not a fault.
                    const QString name = reply.error().name();
                    if (name == QLatin1String("org.bluez.Error.InProgress") ||
                        name == QLatin1String("org.bluez.Error.AlreadyConnected"))
                    {
                        LOG_INFO("BlueZ Connect for " << macAddress << " was redundant: " << name);
                    }
                    else
                    {
                        LOG_WARN("BlueZ Connect failed for " << macAddress << ": "
                                 << reply.error().message());
                    }
                }
                else
                {
                    LOG_INFO("BlueZ Connect succeeded for " << macAddress);
                }
                self->deleteLater();
            });

    LOG_INFO("Asked BlueZ to connect " << macAddress << " (" << devicePath << ")");
    return true;
}

bool BluetoothMonitor::checkAlreadyConnectedDevices()
{
    ManagedObjectList managedObjects;
    if (!fetchManagedObjects(managedObjects))
    {
        return false;
    }

    bool deviceFound = false;

    for (auto it = managedObjects.constBegin(); it != managedObjects.constEnd(); ++it)
    {
        const QDBusObjectPath &objPath = it.key();
        const QMap<QString, QVariantMap> &interfaces = it.value();

        if (interfaces.contains("org.bluez.Device1"))
        {
            const QVariantMap &deviceProps = interfaces.value("org.bluez.Device1");

            // Check if the device has the necessary properties
            if (!deviceProps.contains("UUIDs") || !deviceProps.contains("Connected") ||
                !deviceProps.contains("Address") || !deviceProps.contains("Name"))
            {
                continue;
            }

            QStringList uuids = deviceProps["UUIDs"].toStringList();
            bool isAirPods = uuids.contains("74ec2172-0bad-4d01-8f77-997b2be0722a");

            if (isAirPods)
            {
                bool connected = deviceProps["Connected"].toBool();
                if (connected)
                {
                    QString macAddress = deviceProps["Address"].toString();
                    QString deviceName = deviceProps["Name"].toString();
                    emit deviceConnected(macAddress, deviceName);
                    LOG_DEBUG("Found already connected AirPods: " << macAddress << " Name: " << deviceName);
                    deviceFound = true;
                }
            }
        }
    }
    return deviceFound;
}

void BluetoothMonitor::onPropertiesChanged(const QString &interface, const QVariantMap &changedProps, const QStringList &invalidatedProps)
{
    Q_UNUSED(invalidatedProps);

    if (interface != "org.bluez.Device1")
    {
        return;
    }

    if (changedProps.contains("Connected"))
    {
        bool connected = changedProps["Connected"].toBool();
        QString path = QDBusContext::message().path();

        if (!isAirPodsDevice(path))
        {
            return;
        }

        QDBusInterface deviceInterface("org.bluez", path, "org.freedesktop.DBus.Properties", m_dbus);

        // Get the device address
        QDBusReply<QVariant> addrReply = deviceInterface.call("Get", "org.bluez.Device1", "Address");
        if (!addrReply.isValid())
        {
            return;
        }
        QString macAddress = addrReply.value().toString();
        QString deviceName = getDeviceName(path);

        if (connected)
        {
            emit deviceConnected(macAddress, deviceName);
            LOG_DEBUG("AirPods device connected:" << macAddress << " Name:" << deviceName);
        }
        else
        {
            emit deviceDisconnected(macAddress, deviceName);
            LOG_DEBUG("AirPods device disconnected:" << macAddress << " Name:" << deviceName);
        }
    }
}