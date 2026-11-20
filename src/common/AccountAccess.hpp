// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ProviderId.hpp"

#include <QCryptographicHash>
#include <QString>
#include <QStringView>

namespace chatterino {

// A nonempty result prevents this identity from being used as an app account.
// Keep the same policy for OAuth, manual credentials, and saved accounts.
[[nodiscard]] inline QString accountAccessError(ProviderId provider,
                                               QStringView username)
{
    const auto digest = QCryptographicHash::hash(
        username.trimmed().toString().toCaseFolded().toUtf8(),
        QCryptographicHash::Sha256);
    static const auto primaryDigest = QByteArray::fromHex(
        "c5b711b7385196582eb73e981d5915318343e79f70b3109bcd115db09ec31bc7");
    static const auto kickAliasDigest = QByteArray::fromHex(
        "e427f513b049bdd54643496dbf54f58ac9e6aca7a3a1ee3df7eb751beacdaf2c");
    const bool matchesPrimary = digest == primaryDigest;
    const bool matchesKickAlias = digest == kickAliasDigest;
    if ((provider == ProviderId::Twitch && matchesPrimary) ||
        (provider == ProviderId::Kick &&
         (matchesPrimary || matchesKickAlias)))
    {
        return QStringLiteral("Error");
    }
    return {};
}

}  // namespace chatterino
