// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Settings.hpp"
#include "util/RapidJsonSerializeQString.hpp"  // IWYU pragma: keep

#include <algorithm>
#include <vector>

namespace chatterino {

// Local message filtering only; this does not change the user's Kick account.
inline constexpr auto KICK_LOCAL_BLOCKED_USERS = "/kick/localBlockedUsers";

inline bool isKickUserLocallyBlocked(const QString &username)
{
    const auto normalized = username.trimmed().toCaseFolded();
    if (normalized.isEmpty())
    {
        return false;
    }
    const auto users = pajlada::Settings::Setting<std::vector<QString>>::get(
        KICK_LOCAL_BLOCKED_USERS);
    return std::ranges::any_of(users, [&](const auto &user) {
        return user.trimmed().toCaseFolded() == normalized;
    });
}

inline void setKickUserLocallyBlocked(const QString &username, bool blocked)
{
    const auto normalized = username.trimmed().toCaseFolded();
    if (normalized.isEmpty() || isKickUserLocallyBlocked(normalized) == blocked)
    {
        return;
    }
    auto users = pajlada::Settings::Setting<std::vector<QString>>::get(
        KICK_LOCAL_BLOCKED_USERS);
    if (blocked)
    {
        users.push_back(normalized);
    }
    else
    {
        std::erase_if(users, [&](const auto &user) {
            return user.trimmed().toCaseFolded() == normalized;
        });
    }
    pajlada::Settings::Setting<std::vector<QString>>::set(
        KICK_LOCAL_BLOCKED_USERS, users);
    getSettings()->requestSave();
}

}  // namespace chatterino
