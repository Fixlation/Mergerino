// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Paths.hpp"

#include "common/Modes.hpp"
#include "singletons/Settings.hpp"
#include "util/CombinePath.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QStandardPaths>

#include <cassert>

using namespace std::literals;

namespace chatterino {

Paths::Paths()
{
    this->initAppFilePathHash();

    this->initCheckPortable();
    this->initRootDirectory();
    this->initSubDirectories();
}

bool Paths::createFolder(const QString &folderPath)
{
    return QDir().mkpath(folderPath);
}

bool Paths::isPortable() const
{
    return Modes::instance().isPortable;
}

QString Paths::cacheDirectory() const
{
    static const auto pathSetting = [] {
        QStringSetting cachePathSetting("/cache/path");

        cachePathSetting.connect([](const auto &newPath) {
            if (!newPath.isEmpty())
            {
                QDir().mkpath(newPath);
            }
        });

        return cachePathSetting;
    }();

    auto path = pathSetting.getValue();

    if (path.isEmpty())
    {
        return this->cacheDirectory_;
    }

    return path;
}

QString Paths::cacheFilePath(const QString &fileName) const
{
    return combinePath(this->cacheDirectory(), fileName);
}

void Paths::initAppFilePathHash()
{
    this->applicationFilePathHash =
        QCryptographicHash::hash(
            QCoreApplication::applicationFilePath().toUtf8(),
            QCryptographicHash::Sha224)
            .toBase64()
            .mid(0, 32)
            .replace("+", "-")
            .replace("/", "x");
}

void Paths::initCheckPortable()
{
    this->portable_ = QFileInfo::exists(
        combinePath(QCoreApplication::applicationDirPath(), "portable"));
}

void Paths::initRootDirectory()
{
    assert(this->portable_.has_value());

    // Root path = an explicitly configured data root, %APPDATA%/Mergerino,
    // or the folder that the executable resides in.

    this->rootAppDataDirectory = [&]() -> QString {
        const auto &modes = Modes::instance();

        if (!modes.dataRoot.isEmpty())
        {
            auto path = QDir::fromNativeSeparators(modes.dataRoot);
            if (QDir::isRelativePath(path))
            {
                path = QDir(QCoreApplication::applicationDirPath())
                           .absoluteFilePath(path);
            }
            return QDir::cleanPath(path);
        }

        // portable
        if (modes.isPortable)
        {
            return QCoreApplication::applicationDirPath();
        }

        // permanent installation
        QString path =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (path.isEmpty())
        {
            throw std::runtime_error("Could not create directory \""s +
                                     path.toStdString() + "\"");
        }

// Normalize the Windows app data folder casing for the branded app name.
#ifdef Q_OS_WIN
        path.replace(QCoreApplication::applicationName(), "Mergerino",
                     Qt::CaseInsensitive);
#endif
        return path;
    }();
}

void Paths::initSubDirectories()
{
    // required the app data directory to be set first
    assert(!this->rootAppDataDirectory.isEmpty());

    // create settings subdirectories and validate that they are created
    // properly
    auto makePath = [&](const QString &name) -> QString {
        auto path = combinePath(this->rootAppDataDirectory, name);

        if (!QDir().mkpath(path))
        {
            throw std::runtime_error("Could not create directory \""s +
                                     path.toStdString() + "\"");
        }

        return path;
    };

    makePath("");
    this->settingsDirectory = makePath("Settings");
    this->cacheDirectory_ = makePath("Cache");
    this->messageLogDirectory = makePath("Logs");
    this->miscDirectory = makePath("Misc");
    this->twitchProfileAvatars =
        makePath(combinePath("ProfileAvatars", "twitch"));
    this->pluginsDirectory = makePath("Plugins");
    this->themesDirectory = makePath("Themes");
    this->crashdumpDirectory = makePath("Crashes");
    this->dictionariesDirectory = makePath("Dictionaries");
#ifdef Q_OS_WIN
    this->ipcDirectory = makePath("IPC");
#else
    // NOTE: We do *NOT* use IPC on non-Windows platforms.
    // If we start, we should re-consider this directory.
    this->ipcDirectory = "/tmp";
#endif
}

}  // namespace chatterino
