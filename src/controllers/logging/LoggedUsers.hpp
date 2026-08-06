// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/logging/ChannelLog.hpp"
#include "singletons/Settings.hpp"

#include <algorithm>
#include <vector>

namespace chatterino::logging {

inline QString normalizeLoggedUserName(const QString &userName)
{
    auto normalized = userName.trimmed().toLower();
    while (normalized.startsWith('@'))
    {
        normalized.remove(0, 1);
    }
    return normalized;
}

inline std::vector<ChannelLog> normalizeLoggedUsers(
    const std::vector<ChannelLog> &users);
inline void replaceLoggedUsers(const std::vector<ChannelLog> &users);

inline bool containsLoggedUser(const std::vector<ChannelLog> &users,
                               const QString &normalized)
{
    return std::ranges::any_of(users, [&](const auto &loggedUser) {
        return loggedUser.channelName() == normalized;
    });
}

inline void appendNormalizedLoggedUsers(std::vector<ChannelLog> &target,
                                        const std::vector<ChannelLog> &users)
{
    for (const auto &user : users)
    {
        const auto normalized = normalizeLoggedUserName(user.channelName());
        if (normalized.isEmpty() || containsLoggedUser(target, normalized))
        {
            continue;
        }

        target.emplace_back(normalized);
    }
}

inline std::vector<ChannelLog> normalizeLoggedUsers(
    const std::vector<ChannelLog> &users)
{
    std::vector<ChannelLog> normalizedUsers;
    appendNormalizedLoggedUsers(normalizedUsers, users);

    return normalizedUsers;
}

inline std::vector<ChannelLog> loggedUsersSettingSnapshot(Settings &settings)
{
    return settings.loggedUsersSetting.getValueCopy();
}

inline std::vector<ChannelLog> loggedUsersVectorSnapshot(Settings &settings)
{
    const auto users = settings.loggedUsers.readOnly();
    if (!users)
    {
        return {};
    }

    return *users;
}

inline std::vector<ChannelLog> mergedLoggedUsers()
{
    auto *settings = getSettings();
    const auto settingUsers = loggedUsersSettingSnapshot(*settings);
    const auto vectorUsers = loggedUsersVectorSnapshot(*settings);

    auto users = normalizeLoggedUsers(settingUsers);
    appendNormalizedLoggedUsers(users, vectorUsers);

    return users;
}

inline void initLoggedUsers()
{
    auto *settings = getSettings();
    const auto settingUsers = loggedUsersSettingSnapshot(*settings);
    const auto vectorUsers = loggedUsersVectorSnapshot(*settings);

    auto normalized = normalizeLoggedUsers(settingUsers);
    appendNormalizedLoggedUsers(normalized, vectorUsers);

    if (normalized != normalizeLoggedUsers(settingUsers) ||
        normalized != normalizeLoggedUsers(vectorUsers))
    {
        replaceLoggedUsers(normalized);
    }
}

inline void replaceLoggedUsers(const std::vector<ChannelLog> &users)
{
    const auto normalized = normalizeLoggedUsers(users);
    auto *settings = getSettings();

    for (int i = static_cast<int>(settings->loggedUsers.raw().size()) - 1;
         i >= 0; --i)
    {
        settings->loggedUsers.removeAt(i);
    }

    for (const auto &user : normalized)
    {
        settings->loggedUsers.append(user);
    }

    settings->loggedUsersSetting.setValue(normalized);
}

inline std::vector<ChannelLog> loggedUsersSnapshot()
{
    return normalizeLoggedUsers(loggedUsersVectorSnapshot(*getSettings()));
}

inline bool isLoggedUser(const QString &userName)
{
    const auto normalized = normalizeLoggedUserName(userName);
    if (normalized.isEmpty())
    {
        return false;
    }

    return containsLoggedUser(loggedUsersSnapshot(), normalized);
}

inline bool addLoggedUser(const QString &userName)
{
    const auto normalized = normalizeLoggedUserName(userName);
    if (normalized.isEmpty() || isLoggedUser(normalized))
    {
        return false;
    }

    auto users = loggedUsersSnapshot();
    users.emplace_back(normalized);
    replaceLoggedUsers(users);
    return true;
}

inline bool removeLoggedUser(const QString &userName)
{
    const auto normalized = normalizeLoggedUserName(userName);
    if (normalized.isEmpty())
    {
        return false;
    }

    auto users = loggedUsersSnapshot();
    const auto oldSize = users.size();
    std::erase_if(users, [&](const auto &user) {
        return user.channelName() == normalized;
    });
    if (users.size() == oldSize)
    {
        return false;
    }

    replaceLoggedUsers(users);
    return true;
}

}  // namespace chatterino::logging
