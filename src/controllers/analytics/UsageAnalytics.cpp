// SPDX-FileCopyrightText: 2026 Mergerino Contributors
//
// SPDX-License-Identifier: MIT

#include "controllers/analytics/UsageAnalytics.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/Version.hpp"
#include "singletons/Settings.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QUuid>

#include <cstddef>
#include <tuple>

#if __has_include("controllers/analytics/PostHogProjectToken.local.hpp")
#    include "controllers/analytics/PostHogProjectToken.local.hpp"
#endif

namespace chatterino {
namespace {

constexpr int HEARTBEAT_INTERVAL_SECONDS = 5 * 60;
constexpr int REQUEST_TIMEOUT_MS = 5000;
constexpr auto POSTHOG_CAPTURE_URL = "https://eu.i.posthog.com/i/v0/e/";

QString postHogProjectToken()
{
#ifdef MERGERINO_POSTHOG_PROJECT_TOKEN_OBFUSCATED
    QByteArray decoded;
    decoded.reserve(
        static_cast<qsizetype>(posthog_token_blob::DATA.size()));

    for (std::size_t i = 0; i < posthog_token_blob::DATA.size(); ++i)
    {
        const auto mask = static_cast<unsigned char>(
            (static_cast<unsigned int>(posthog_token_blob::KEY_A) +
             static_cast<unsigned int>(i) * 31U) ^
            (static_cast<unsigned int>(posthog_token_blob::KEY_B) +
             static_cast<unsigned int>(i) * 17U) ^
            (static_cast<unsigned int>(posthog_token_blob::KEY_C) +
             static_cast<unsigned int>(i) * 13U));

        decoded.append(
            static_cast<char>(posthog_token_blob::DATA[i] ^ mask));
    }

    return QString::fromUtf8(decoded).trimmed();
#else
    return qEnvironmentVariable("PHGTK").trimmed();
#endif
}

}  // namespace

UsageAnalytics::UsageAnalytics(Settings &settings)
    : settings_(settings)
{
    this->heartbeatTimer_.setInterval(HEARTBEAT_INTERVAL_SECONDS * 1000);
    this->heartbeatTimer_.setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(&this->heartbeatTimer_, &QTimer::timeout,
                     [this] { this->sendHeartbeat(); });
}

void UsageAnalytics::start()
{
    this->sendHeartbeat();
    this->heartbeatTimer_.start();
}

void UsageAnalytics::sendHeartbeat()
{
    if (!this->settings_.shareAnonymousUsageAnalytics.getValue())
    {
        return;
    }

    const auto projectToken = postHogProjectToken();
    if (projectToken.isEmpty())
    {
        return;
    }

    const auto installationID = this->installationID();
    QJsonObject properties{
        {"app_version", Version::instance().version()},
        {"heartbeat_interval_seconds", HEARTBEAT_INTERVAL_SECONDS},
        {"$process_person_profile", false},
        {"$geoip_disable", true},
    };
    QJsonObject event{
        {"api_key", projectToken},
        {"event", "app_heartbeat"},
        {"distinct_id", installationID},
        {"properties", properties},
    };

    NetworkRequest(POSTHOG_CAPTURE_URL, NetworkRequestType::Post)
        .timeout(REQUEST_TIMEOUT_MS)
        .hideRequestBody()
        .json(event)
        .execute();
}

QString UsageAnalytics::installationID()
{
    auto id = this->settings_.anonymousAnalyticsInstallationID.getValue()
                  .trimmed();
    if (!id.isEmpty())
    {
        return id;
    }

    id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    this->settings_.anonymousAnalyticsInstallationID.setValue(id);
    std::ignore = this->settings_.requestSave();
    return id;
}

}  // namespace chatterino
