// SPDX-FileCopyrightText: 2026 Mergerino Contributors
//
// SPDX-License-Identifier: MIT

#include "controllers/analytics/UsageAnalytics.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/Version.hpp"
#include "singletons/Settings.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include <cstddef>
#include <tuple>
#include <utility>

#if __has_include("controllers/analytics/PostHogProjectToken.local.hpp")
#    include "controllers/analytics/PostHogProjectToken.local.hpp"
#endif

namespace chatterino {
namespace {

constexpr int HEARTBEAT_INTERVAL_SECONDS = 30 * 60;
constexpr int REQUEST_TIMEOUT_MS = 5000;
constexpr int TELEMETRY_SCHEMA_VERSION = 1;
constexpr auto POSTHOG_CAPTURE_URL = "https://eu.i.posthog.com/i/v0/e/";

UsageAnalytics *INSTANCE = nullptr;

const QSet<QString> UPDATE_STAGE_ALLOWLIST{
    QStringLiteral("check"), QStringLiteral("download"),
    QStringLiteral("prepare"), QStringLiteral("handoff")};
const QSet<QString> UPDATE_RESULT_ALLOWLIST{
    QStringLiteral("started"), QStringLiteral("available"),
    QStringLiteral("up_to_date"), QStringLiteral("success"),
    QStringLiteral("failure")};

QString shortBuildCommit()
{
    const auto commit = Version::instance().commitHash().trimmed();
    return commit.isEmpty() ? QStringLiteral("unknown") : commit.left(7);
}

QString startupDurationBucket(qint64 milliseconds)
{
    if (milliseconds < 1000)
    {
        return QStringLiteral("under_1s");
    }
    if (milliseconds < 3000)
    {
        return QStringLiteral("1s-3s");
    }
    if (milliseconds < 10000)
    {
        return QStringLiteral("3s-10s");
    }
    return QStringLiteral("10s+");
}

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
    INSTANCE = this;
    this->heartbeatTimer_.setInterval(HEARTBEAT_INTERVAL_SECONDS * 1000);
    this->heartbeatTimer_.setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(&this->heartbeatTimer_, &QTimer::timeout,
                     [this] { this->sendHeartbeat(); });
}

UsageAnalytics::~UsageAnalytics()
{
    if (INSTANCE == this)
    {
        INSTANCE = nullptr;
    }
}

void UsageAnalytics::start(bool previousSessionCrashed,
                           qint64 guiStartupMilliseconds)
{
    this->sendHeartbeat();
    this->sendReliabilityEvent(
        QStringLiteral("startup"), QStringLiteral("success"),
        {{QStringLiteral("previous_session_crashed"), previousSessionCrashed},
         {QStringLiteral("startup_duration_bucket"),
          startupDurationBucket(guiStartupMilliseconds)}});
    this->heartbeatTimer_.start();
}

void UsageAnalytics::recordUpdateFlow(const QString &stage,
                                      const QString &result)
{
    if (INSTANCE != nullptr)
    {
        INSTANCE->recordUpdateFlow_(stage, result);
    }
}

void UsageAnalytics::recordUpdateFlow_(const QString &stage,
                                       const QString &result)
{
    if (!this->settings_.shareAnonymousUsageAnalytics.getValue() ||
        !UPDATE_STAGE_ALLOWLIST.contains(stage) ||
        !UPDATE_RESULT_ALLOWLIST.contains(result))
    {
        return;
    }

    this->sendEvent(QStringLiteral("update_flow"),
                    {{QStringLiteral("stage"), stage},
                     {QStringLiteral("result"), result}});
    if (result == QStringLiteral("failure"))
    {
        this->sendReliabilityEvent(
            QStringLiteral("update"), result,
            {{QStringLiteral("stage"), stage}});
    }
}

void UsageAnalytics::sendHeartbeat()
{
    this->sendEvent(
        QStringLiteral("app_heartbeat"),
        {{QStringLiteral("heartbeat_interval_seconds"),
          HEARTBEAT_INTERVAL_SECONDS}});
}

void UsageAnalytics::sendReliabilityEvent(const QString &category,
                                          const QString &result,
                                          QJsonObject properties)
{
    properties.insert(QStringLiteral("category"), category);
    properties.insert(QStringLiteral("result"), result);
    this->sendEvent(QStringLiteral("reliability_event"),
                    std::move(properties));
}

void UsageAnalytics::sendEvent(const QString &eventName,
                               QJsonObject properties)
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
    properties.insert(QStringLiteral("app_version"),
                      Version::instance().version());
    properties.insert(QStringLiteral("build_commit"), shortBuildCommit());
    properties.insert(QStringLiteral("telemetry_schema_version"),
                      TELEMETRY_SCHEMA_VERSION);
    properties.insert(QStringLiteral("$process_person_profile"), false);
    properties.insert(QStringLiteral("$geoip_disable"), true);
    QJsonObject event{
        {QStringLiteral("api_key"), projectToken},
        {QStringLiteral("event"), eventName},
        {QStringLiteral("distinct_id"), installationID},
        {QStringLiteral("properties"), properties},
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
