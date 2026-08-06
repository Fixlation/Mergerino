// SPDX-FileCopyrightText: 2026 Mergerino Contributors
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QTimer>

namespace chatterino {

class Settings;

class UsageAnalytics final
{
public:
    explicit UsageAnalytics(Settings &settings);

    void start();

private:
    void sendHeartbeat();
    QString installationID();

    Settings &settings_;
    QTimer heartbeatTimer_;
};

}  // namespace chatterino
