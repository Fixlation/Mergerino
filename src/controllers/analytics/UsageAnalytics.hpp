// SPDX-FileCopyrightText: 2026 Mergerino Contributors
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>
#include <QTimer>

namespace chatterino {

class Settings;

class UsageAnalytics final
{
public:
    explicit UsageAnalytics(Settings &settings);
    ~UsageAnalytics();

    void start(bool previousSessionCrashed, qint64 guiStartupMilliseconds);

    static void recordUpdateFlow(const QString &stage, const QString &result);

private:
    void recordUpdateFlow_(const QString &stage, const QString &result);

    void sendHeartbeat();
    void sendEvent(const QString &eventName, QJsonObject properties);
    void sendReliabilityEvent(const QString &category, const QString &result,
                              QJsonObject properties = {});
    QString installationID();

    Settings &settings_;
    QTimer heartbeatTimer_;
};

}  // namespace chatterino
