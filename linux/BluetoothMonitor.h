#ifndef BLUETOOTHMONITOR_H
#define BLUETOOTHMONITOR_H

#include <QObject>
#include <QtDBus/QtDBus>

// Forward declarations for D-Bus types
typedef QMap<QDBusObjectPath, QMap<QString, QVariantMap>> ManagedObjectList;
Q_DECLARE_METATYPE(ManagedObjectList)

class BluetoothMonitor : public QObject, protected QDBusContext
{
    Q_OBJECT
public:
    explicit BluetoothMonitor(QObject *parent = nullptr);
    ~BluetoothMonitor();

    bool checkAlreadyConnectedDevices();

    struct AirPodsDevice
    {
        QString address;
        QString path;
        bool connected = false;
        bool isValid() const { return !path.isEmpty(); }
    };

    // A paired AirPods device known to BlueZ, or an invalid entry if none.
    // BlueZ keeps Device1 objects (with cached UUIDs) for paired devices even
    // while disconnected, so this resolves a target without the app caching an
    // address that its own disconnect handling clears. Returns address, object
    // path and connected state together, from a single GetManagedObjects call.
    // Prefers a connected device when more than one pair is present.
    AirPodsDevice findPairedAirPods();

    // Asks BlueZ to connect the device. Asynchronous: BlueZ pages the device,
    // which routinely takes seconds, so this must not block the GUI thread.
    // Returns false if the path is empty or a connect to the same device is
    // already in flight.
    bool connectDeviceByPath(const QString &devicePath, const QString &macAddress);

signals:
    void deviceConnected(const QString &macAddress, const QString &deviceName);
    void deviceDisconnected(const QString &macAddress, const QString &deviceName);

private slots:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changedProps, const QStringList &invalidatedProps);

private:
    QDBusConnection m_dbus;
    void registerDBusService();
    bool isAirPodsDevice(const QString &devicePath);
    QString getDeviceName(const QString &devicePath);
    bool fetchManagedObjects(ManagedObjectList &out);
    // Address of the connect currently in flight, empty when none. Guards
    // against a burst of media-play events queueing duplicate Connect calls.
    QString m_connectInFlight;
};

#endif // BLUETOOTHMONITOR_H