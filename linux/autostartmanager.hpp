#ifndef AUTOSTARTMANAGER_HPP
#define AUTOSTARTMANAGER_HPP

#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QProcess>
#include <QStringList>
#include <QDebug>

class AutoStartManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool autoStartEnabled READ autoStartEnabled WRITE setAutoStartEnabled NOTIFY autoStartEnabledChanged)

public:
    explicit AutoStartManager(QObject *parent = nullptr) : QObject(parent)
    {
        const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        const QString appName = QCoreApplication::applicationName();

        QDir().mkpath(configDir + "/autostart");
        m_autostartFilePath = configDir + "/autostart/" + appName + ".desktop";

        m_systemdUnitName = appName + ".service";
        m_systemdUnitPath = configDir + "/systemd/user/" + m_systemdUnitName;
    }

    bool autoStartEnabled() const
    {
        if (hasSystemdUnit())
        {
            return isSystemdUnitEnabled();
        }
        return QFile::exists(m_autostartFilePath);
    }

    void setAutoStartEnabled(bool enabled)
    {
        if (autoStartEnabled() == enabled)
        {
            return;
        }

        bool ok = false;
        if (hasSystemdUnit())
        {
            ok = setSystemdUnitEnabled(enabled);
            // The .desktop autostart file and the systemd unit both trigger startup,
            // so any leftover .desktop file would produce a duplicate instance.
            if (ok && QFile::exists(m_autostartFilePath))
            {
                QFile::remove(m_autostartFilePath);
            }
        }
        else
        {
            ok = enabled ? createAutoStartEntry() : removeAutoStartEntry();
        }

        if (ok)
        {
            emit autoStartEnabledChanged(enabled);
        }
    }

private:
    bool hasSystemdUnit() const
    {
        return QFile::exists(m_systemdUnitPath);
    }

    bool isSystemdUnitEnabled() const
    {
        QProcess p;
        p.start("systemctl", QStringList{"--user", "is-enabled", "--quiet", m_systemdUnitName});
        if (!p.waitForFinished(2000))
        {
            p.kill();
            return false;
        }
        return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    }

    bool setSystemdUnitEnabled(bool enabled)
    {
        const QStringList args{"--user", enabled ? QStringLiteral("enable") : QStringLiteral("disable"), m_systemdUnitName};
        QProcess p;
        p.start("systemctl", args);
        if (!p.waitForFinished(5000))
        {
            qWarning() << "systemctl" << args << "timed out";
            p.kill();
            return false;
        }
        if (p.exitCode() != 0)
        {
            qWarning() << "systemctl" << args << "failed:" << p.readAllStandardError();
            return false;
        }
        return true;
    }

    bool createAutoStartEntry()
    {
        QFile desktopFile(m_autostartFilePath);
        if (!desktopFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            qWarning() << "Failed to create autostart file:" << desktopFile.errorString();
            return false;
        }

        QString appPath = QCoreApplication::applicationFilePath();
        if (appPath.contains(' '))
        {
            appPath = "\"" + appPath + "\"";
        }

        const QString content = QStringLiteral(
                                    "[Desktop Entry]\n"
                                    "Type=Application\n"
                                    "Name=%1\n"
                                    "Exec=%2 --hide\n"
                                    "Icon=%3\n"
                                    "Comment=%4\n"
                                    "X-GNOME-Autostart-enabled=true\n"
                                    "Terminal=false\n")
                                    .arg(
                                        QCoreApplication::applicationName(),
                                        appPath,
                                        QCoreApplication::applicationName().toLower(),
                                        QCoreApplication::applicationName() + " autostart");

        desktopFile.write(content.toUtf8());
        desktopFile.close();
        return true;
    }

    bool removeAutoStartEntry()
    {
        if (QFile::exists(m_autostartFilePath))
        {
            return QFile::remove(m_autostartFilePath);
        }
        return true;
    }

    QString m_autostartFilePath;
    QString m_systemdUnitName;
    QString m_systemdUnitPath;

signals:
    void autoStartEnabledChanged(bool enabled);
};

#endif // AUTOSTARTMANAGER_HPP
