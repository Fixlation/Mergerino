// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/Args.hpp"
#include "singletons/Settings.hpp"

#include <gtest/gtest.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#ifdef USEWINSDK
#    include "util/WindowsHelper.hpp"

#    include <QSettings>
#endif

using namespace chatterino;

TEST(SettingsPersistence, CustomTimeoutButtonsSurviveRepeatedRestarts)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    Args args;
    const std::vector<TimeoutButton> expected{
        {"s", 10}, {"m", 1}, {"m", 5}, {"m", 10},
        {"m", 30}, {"h", 1}, {"d", 1}, {"w", 2},
    };

    {
        Settings settings(args, directory.path(), /*isTest=*/true);
        settings.timeoutButtons.setValue(expected);
        EXPECT_EQ(settings.requestSave(),
                  pajlada::Settings::SettingManager::SaveResult::Success);
    }

    for (int restart = 0; restart < 3; ++restart)
    {
        SCOPED_TRACE(restart);
        Settings settings(args, directory.path(), /*isTest=*/true);
        EXPECT_EQ(settings.timeoutButtons.getValue(), expected);
        std::ignore = settings.requestSave();
    }
}

#ifdef USEWINSDK
TEST(SettingsPersistence, TestAutorunUsesItsOwnSettingsInsteadOfWindowsStartup)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const bool expected = !isRegisteredForStartup();
    QFile file(directory.filePath("settings.json"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const auto data =
        QJsonDocument(
            QJsonObject{{"behaviour", QJsonObject{{"autorun", expected}}}})
            .toJson();
    ASSERT_EQ(file.write(data), data.size());
    file.close();

    Args args;
    Settings settings(args, directory.path(), /*isTest=*/true);
    EXPECT_EQ(settings.autorun.getValue(), expected);
}

TEST(SettingsPersistence, TestAutorunChangesLeaveWindowsStartupUntouched)
{
    QSettings startup(
        QStringLiteral(
            R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)"),
        QSettings::NativeFormat);
    const auto previous = startup.value("Mergerino");
    const bool previouslyPresent = startup.contains("Mergerino");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    Args args;
    {
        Settings settings(args, directory.path(), /*isTest=*/true);
        settings.autorun.setValue(true);
        settings.autorun.setValue(false);
        settings.autorun.setValue(true);
    }
    startup.sync();
    EXPECT_EQ(startup.contains("Mergerino"), previouslyPresent);
    EXPECT_EQ(startup.value("Mergerino"), previous);
}
#endif
