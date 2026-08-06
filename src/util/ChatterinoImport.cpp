// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/ChatterinoImport.hpp"

#include "singletons/Paths.hpp"
#include "util/CombinePath.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

using namespace chatterino;
using namespace Qt::Literals;
using chatterino::chatterino_import::ImportOptions;

constexpr auto PENDING_IMPORT_FILENAME = "chatterino-import-pending.json";

QStringList importFileNames()
{
    return {
        u"settings.json"_s,
        u"commands.json"_s,
        u"window-layout.json"_s,
        u"user-data.json"_s,
        u"credentials.json"_s,
    };
}

QString pendingImportFilePath(const Paths &paths)
{
    return combinePath(paths.settingsDirectory, PENDING_IMPORT_FILENAME);
}

ExpectedStr<QByteArray> readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return makeUnexpected(u"Could not read %1: %2"_s.arg(
            QDir::toNativeSeparators(path), file.errorString()));
    }

    return file.readAll();
}

ExpectedStr<QJsonDocument> readJsonFile(const QString &path)
{
    auto bytes = readFile(path);
    if (!bytes)
    {
        return makeUnexpected(bytes.error());
    }

    QJsonParseError error;
    auto document = QJsonDocument::fromJson(*bytes, &error);
    if (error.error != QJsonParseError::NoError)
    {
        return makeUnexpected(u"%1 contains invalid JSON at offset %2: %3"_s.arg(
            QDir::toNativeSeparators(path), QString::number(error.offset),
            error.errorString()));
    }

    return document;
}

ExpectedStr<void> writeFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return makeUnexpected(u"Could not write %1: %2"_s.arg(
            QDir::toNativeSeparators(path), file.errorString()));
    }

    if (file.write(bytes) != bytes.size())
    {
        return makeUnexpected(u"Could not write all bytes to %1"_s.arg(
            QDir::toNativeSeparators(path)));
    }

    if (!file.commit())
    {
        return makeUnexpected(u"Could not save %1: %2"_s.arg(
            QDir::toNativeSeparators(path), file.errorString()));
    }

    return {};
}

ExpectedStr<void> writeJsonFile(const QString &path,
                                const QJsonDocument &document)
{
    return writeFile(path, document.toJson(QJsonDocument::Indented));
}

QString uniqueBackupPath(const QString &path, const QString &backupSuffix)
{
    auto backupPath = path + u"."_s + backupSuffix;
    for (int i = 2; QFileInfo::exists(backupPath); ++i)
    {
        backupPath = path + u"."_s + backupSuffix + u"-"_s +
                     QString::number(i);
    }
    return backupPath;
}

ExpectedStr<void> backupExistingFile(const QString &path,
                                     const QString &backupSuffix)
{
    if (!QFileInfo::exists(path))
    {
        return {};
    }

    auto backupPath = uniqueBackupPath(path, backupSuffix);
    if (!QFile::copy(path, backupPath))
    {
        return makeUnexpected(u"Could not back up %1 to %2"_s.arg(
            QDir::toNativeSeparators(path),
            QDir::toNativeSeparators(backupPath)));
    }

    return {};
}

ExpectedStr<void> validateSourceFile(const QString &sourcePath)
{
    if (!QFileInfo::exists(sourcePath))
    {
        return makeUnexpected(u"%1 was not found"_s.arg(
            QDir::toNativeSeparators(sourcePath)));
    }

    auto parsed = readJsonFile(sourcePath);
    if (!parsed)
    {
        return makeUnexpected(parsed.error());
    }

    return {};
}

void setNestedValue(QJsonObject &root, const QStringList &path,
                    const QJsonValue &value)
{
    if (path.isEmpty())
    {
        return;
    }

    if (path.size() == 1)
    {
        root.insert(path.front(), value);
        return;
    }

    auto key = path.front();
    auto child = root.value(key).toObject();
    setNestedValue(child, path.mid(1), value);
    root.insert(key, child);
}

QString optionValueOrDefault(const std::optional<QString> &value,
                             const QString &fallback)
{
    if (value.has_value() && !value->trimmed().isEmpty())
    {
        return *value;
    }

    return fallback;
}

QString importedPlatformIndicatorMode(const ImportOptions &options)
{
    return optionValueOrDefault(options.mergedPlatformIndicatorMode,
                                u"linecolor"_s)
        .toLower();
}

QJsonObject importedAccounts(const QJsonObject &root)
{
    return root.value("accounts").toObject();
}

QString twitchChannelNameFromSplitData(const QJsonObject &data)
{
    auto channelName = data.value("channel").toString().trimmed();
    if (channelName.isEmpty())
    {
        channelName = data.value("name").toString().trimmed();
    }
    return channelName;
}

bool isRegularImportedTwitchChannel(const QString &channelName)
{
    return !channelName.isEmpty() && !channelName.startsWith(u'/') &&
           !channelName.startsWith(u'$');
}

QJsonObject mergedSplitDataForTwitchChannel(const QString &channelName)
{
    QJsonObject data;
    data.insert("type", u"merged"_s);
    data.insert("name", channelName);
    data.insert("twitchEnabled", true);
    data.insert("twitchChannel", channelName);
    data.insert("kickEnabled", false);
    data.insert("kickChannel", QString{});
    data.insert("youtubeEnabled", false);
    data.insert("youtubeStreamUrl", QString{});
    data.insert("tiktokEnabled", false);
    data.insert("tiktokSource", QString{});
    data.insert("tiktokShowJoinMessages", true);
    return data;
}

bool disableKickForImportedMergedSplit(QJsonObject &data)
{
    bool changed = false;
    if (data.value("kickEnabled").toBool(false))
    {
        data.insert("kickEnabled", false);
        changed = true;
    }
    else if (!data.contains("kickEnabled"))
    {
        data.insert("kickEnabled", false);
        changed = true;
    }

    if (!data.value("kickChannel").toString().trimmed().isEmpty())
    {
        data.insert("kickChannel", QString{});
        changed = true;
    }
    else if (!data.contains("kickChannel"))
    {
        data.insert("kickChannel", QString{});
        changed = true;
    }

    return changed;
}

bool patchImportedSplitNode(QJsonObject &node, const QString &indicatorMode)
{
    const auto nodeType = node.value("type").toString();
    if (nodeType == u"split"_s)
    {
        auto data = node.value("data").toObject();
        const auto dataType = data.value("type").toString();
        if (dataType == u"twitch"_s)
        {
            const auto channelName = twitchChannelNameFromSplitData(data);
            if (!isRegularImportedTwitchChannel(channelName))
            {
                return false;
            }

            node.insert("data", mergedSplitDataForTwitchChannel(channelName));
            node.insert("platformIndicatorMode", indicatorMode);
            return true;
        }

        if (dataType == u"merged"_s)
        {
            bool changed = disableKickForImportedMergedSplit(data);
            if (changed)
            {
                node.insert("data", data);
            }

            if (!node.value("platformIndicatorMode").isString())
            {
                node.insert("platformIndicatorMode", indicatorMode);
                changed = true;
            }
            return changed;
        }

        return false;
    }

    if (nodeType != u"horizontal"_s && nodeType != u"vertical"_s)
    {
        return false;
    }

    auto items = node.value("items").toArray();
    bool changed = false;
    for (qsizetype i = 0; i < items.size(); ++i)
    {
        auto child = items.at(i).toObject();
        if (child.isEmpty())
        {
            continue;
        }

        if (patchImportedSplitNode(child, indicatorMode))
        {
            items[i] = child;
            changed = true;
        }
    }

    if (changed)
    {
        node.insert("items", items);
    }
    return changed;
}

ExpectedStr<void> patchImportedWindowLayoutFile(const QString &path,
                                                const ImportOptions &options)
{
    auto document = readJsonFile(path);
    if (!document)
    {
        return makeUnexpected(document.error());
    }
    if (!document->isObject())
    {
        return makeUnexpected(u"%1 must contain a JSON object"_s.arg(
            QDir::toNativeSeparators(path)));
    }

    const auto indicatorMode = importedPlatformIndicatorMode(options);
    auto root = document->object();
    auto windows = root.value("windows").toArray();
    bool changed = false;

    for (qsizetype windowIndex = 0; windowIndex < windows.size();
         ++windowIndex)
    {
        auto window = windows.at(windowIndex).toObject();
        if (window.isEmpty())
        {
            continue;
        }

        auto tabs = window.value("tabs").toArray();
        bool windowChanged = false;
        for (qsizetype tabIndex = 0; tabIndex < tabs.size(); ++tabIndex)
        {
            auto tab = tabs.at(tabIndex).toObject();
            if (tab.isEmpty())
            {
                continue;
            }

            auto splits = tab.value("splits2").toObject();
            if (splits.isEmpty())
            {
                continue;
            }

            if (patchImportedSplitNode(splits, indicatorMode))
            {
                tab.insert("splits2", splits);
                tabs[tabIndex] = tab;
                windowChanged = true;
            }
        }

        if (windowChanged)
        {
            window.insert("tabs", tabs);
            windows[windowIndex] = window;
            changed = true;
        }
    }

    if (!changed)
    {
        return {};
    }

    root.insert("windows", windows);
    document->setObject(root);
    return writeJsonFile(path, *document);
}

ExpectedStr<void> patchImportedSettingsFile(const QString &path,
                                            const ImportOptions &options)
{
    auto document = readJsonFile(path);
    if (!document)
    {
        return makeUnexpected(document.error());
    }
    if (!document->isObject())
    {
        return makeUnexpected(u"%1 must contain a JSON object"_s.arg(
            QDir::toNativeSeparators(path)));
    }

    auto root = document->object();
    root.insert("accounts", importedAccounts(root));
    if (options.startupPromptAcknowledged.has_value())
    {
        setNestedValue(root, {"behaviour", "startupPromptAcknowledged"},
                       *options.startupPromptAcknowledged);
    }
    if (options.autorun.has_value())
    {
        setNestedValue(root, {"behaviour", "autorun"}, *options.autorun);
    }
    if (options.currentVersion.has_value())
    {
        setNestedValue(root, {"misc", "currentVersion"},
                       *options.currentVersion);
    }
    setNestedValue(root, {"appearance", "messages",
                          "mergedPlatformIndicatorMode"},
                   importedPlatformIndicatorMode(options));
    setNestedValue(root, {"appearance", "messages",
                          "platformEventHighlightStyle"},
                   optionValueOrDefault(options.platformEventHighlightStyle,
                                        u"gradient"_s).toLower());
    setNestedValue(root, {"appearance", "messages",
                          "platformEventHighlightCustomColor"},
                   optionValueOrDefault(
                       options.platformEventHighlightCustomColor,
                       u"#5a9146ff"_s));
    setNestedValue(root, {"appearance", "messages",
                          "platformAlertHighlightStyle"},
                   optionValueOrDefault(options.platformAlertHighlightStyle,
                                        u"gradient"_s).toLower());
    setNestedValue(root, {"appearance", "messages",
                          "platformAlertHighlightCustomColor"},
                   optionValueOrDefault(
                       options.platformAlertHighlightCustomColor,
                       u"#5a9146ff"_s));
    setNestedValue(root, {"appearance", "messages",
                          "platformHighlightStylesSplitMigrated"},
                   true);

    document->setObject(root);
    return writeJsonFile(path, *document);
}

ExpectedStr<void> writePendingImport(const QString &path,
                                     const QString &sourceDirectory,
                                     const ImportOptions &options)
{
    QJsonObject object;
    object.insert("sourceDirectory", sourceDirectory);
    if (options.startupPromptAcknowledged.has_value())
    {
        object.insert("startupPromptAcknowledged",
                      *options.startupPromptAcknowledged);
    }
    if (options.autorun.has_value())
    {
        object.insert("autorun", *options.autorun);
    }
    if (options.currentVersion.has_value())
    {
        object.insert("currentVersion", *options.currentVersion);
    }
    if (options.mergedPlatformIndicatorMode.has_value())
    {
        object.insert("mergedPlatformIndicatorMode",
                      *options.mergedPlatformIndicatorMode);
    }
    if (options.platformEventHighlightStyle.has_value())
    {
        object.insert("platformEventHighlightStyle",
                      *options.platformEventHighlightStyle);
    }
    if (options.platformEventHighlightCustomColor.has_value())
    {
        object.insert("platformEventHighlightCustomColor",
                      *options.platformEventHighlightCustomColor);
    }
    if (options.platformAlertHighlightStyle.has_value())
    {
        object.insert("platformAlertHighlightStyle",
                      *options.platformAlertHighlightStyle);
    }
    if (options.platformAlertHighlightCustomColor.has_value())
    {
        object.insert("platformAlertHighlightCustomColor",
                      *options.platformAlertHighlightCustomColor);
    }

    return writeJsonFile(path, QJsonDocument(object));
}

ImportOptions optionsFromPendingImport(const QJsonObject &object)
{
    ImportOptions options;
    if (object.contains("startupPromptAcknowledged"))
    {
        options.startupPromptAcknowledged =
            object.value("startupPromptAcknowledged").toBool();
    }
    if (object.contains("autorun"))
    {
        options.autorun = object.value("autorun").toBool();
    }
    if (object.contains("currentVersion"))
    {
        options.currentVersion = object.value("currentVersion").toString();
    }
    if (object.contains("mergedPlatformIndicatorMode"))
    {
        options.mergedPlatformIndicatorMode =
            object.value("mergedPlatformIndicatorMode").toString();
    }
    if (object.contains("platformEventHighlightStyle"))
    {
        options.platformEventHighlightStyle =
            object.value("platformEventHighlightStyle").toString();
    }
    if (object.contains("platformEventHighlightCustomColor"))
    {
        options.platformEventHighlightCustomColor =
            object.value("platformEventHighlightCustomColor").toString();
    }
    if (object.contains("platformAlertHighlightStyle"))
    {
        options.platformAlertHighlightStyle =
            object.value("platformAlertHighlightStyle").toString();
    }
    if (object.contains("platformAlertHighlightCustomColor"))
    {
        options.platformAlertHighlightCustomColor =
            object.value("platformAlertHighlightCustomColor").toString();
    }
    return options;
}

void retirePendingImportFile(const QString &path)
{
    auto suffix =
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    QFile::rename(path, path + u".failed-"_s + suffix);
}

#ifdef Q_OS_WIN
QString restartScriptPath()
{
    auto tempPath =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempPath.isEmpty())
    {
        tempPath = QDir::tempPath();
    }

    QDir().mkpath(tempPath);
    return QDir(tempPath).filePath(u"mergerino-restart-%1.cmd"_s.arg(
        QCoreApplication::applicationPid()));
}

QString windowsCommandPath(QString path)
{
    path = QDir::toNativeSeparators(path);
    if (path.startsWith(QStringLiteral("\\\\?\\")))
    {
        path = path.mid(4);
    }
    return path;
}
#endif

}  // namespace

namespace chatterino::chatterino_import {

QString defaultSourceSettingsDirectory()
{
#ifdef Q_OS_WIN
    auto appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appData.isEmpty())
    {
        return {};
    }

    QDir roaming(appData);
    roaming.cdUp();
    return roaming.filePath(u"Chatterino2/Settings"_s);
#else
    return {};
#endif
}

bool defaultSourceSettingsDirectoryExists()
{
    auto path = defaultSourceSettingsDirectory();
    return !path.isEmpty() && QFileInfo::exists(combinePath(path, "settings.json"));
}

bool hasPendingImport(const Paths &paths)
{
    return QFileInfo::exists(pendingImportFilePath(paths));
}

ExpectedStr<void> stageImportFromDefaultSource(const Paths &paths,
                                               const ImportOptions &options)
{
    auto sourceDirectory = defaultSourceSettingsDirectory();
    if (sourceDirectory.isEmpty())
    {
        return makeUnexpected(
            u"Could not determine Chatterino's settings directory."_s);
    }

    if (!QFileInfo::exists(sourceDirectory))
    {
        return makeUnexpected(u"Chatterino settings were not found at %1."_s.arg(
            QDir::toNativeSeparators(sourceDirectory)));
    }

    auto settingsValidation =
        validateSourceFile(combinePath(sourceDirectory, "settings.json"));
    if (!settingsValidation)
    {
        return makeUnexpected(settingsValidation.error());
    }

    return writePendingImport(pendingImportFilePath(paths), sourceDirectory,
                              options);
}

ExpectedStr<ImportSummary> applyPendingImport(const Paths &paths)
{
    auto pendingPath = pendingImportFilePath(paths);
    auto pending = readJsonFile(pendingPath);
    if (!pending)
    {
        retirePendingImportFile(pendingPath);
        return makeUnexpected(pending.error());
    }

    if (!pending->isObject())
    {
        retirePendingImportFile(pendingPath);
        return makeUnexpected(
            u"Pending Chatterino import file did not contain a JSON object."_s);
    }

    auto object = pending->object();
    auto sourceDirectory = object.value("sourceDirectory").toString();
    if (sourceDirectory.isEmpty())
    {
        retirePendingImportFile(pendingPath);
        return makeUnexpected(
            u"Pending Chatterino import did not include a source directory."_s);
    }

    auto result =
        importFromDirectory(sourceDirectory, paths, optionsFromPendingImport(object));
    if (!result)
    {
        retirePendingImportFile(pendingPath);
        return makeUnexpected(result.error());
    }

    QFile::remove(pendingPath);
    return result;
}

ExpectedStr<ImportSummary> importFromDirectory(const QString &sourceDirectory,
                                               const Paths &paths,
                                               const ImportOptions &options)
{
    const auto sourceCanonical = QDir(sourceDirectory).canonicalPath();
    const auto destinationCanonical = QDir(paths.settingsDirectory).canonicalPath();
    if (!sourceCanonical.isEmpty() && !destinationCanonical.isEmpty() &&
        sourceCanonical == destinationCanonical)
    {
        return makeUnexpected(
            u"Chatterino and Mergerino settings directories are the same."_s);
    }

    auto requiredSettings =
        validateSourceFile(combinePath(sourceDirectory, "settings.json"));
    if (!requiredSettings)
    {
        return makeUnexpected(requiredSettings.error());
    }

    const auto backupSuffix =
        u"before-chatterino-import-"_s +
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");

    ImportSummary summary{
        .sourceDirectory = sourceDirectory,
        .backupSuffix = backupSuffix,
    };

    for (const auto &fileName : importFileNames())
    {
        const auto sourcePath = combinePath(sourceDirectory, fileName);
        if (!QFileInfo::exists(sourcePath))
        {
            continue;
        }

        auto parsed = readJsonFile(sourcePath);
        if (!parsed)
        {
            return makeUnexpected(parsed.error());
        }

        const auto destinationPath = combinePath(paths.settingsDirectory, fileName);
        auto backup = backupExistingFile(destinationPath, backupSuffix);
        if (!backup)
        {
            return makeUnexpected(backup.error());
        }

        auto bytes = readFile(sourcePath);
        if (!bytes)
        {
            return makeUnexpected(bytes.error());
        }

        auto write = writeFile(destinationPath, *bytes);
        if (!write)
        {
            return makeUnexpected(write.error());
        }

        summary.importedFiles.append(fileName);
    }

    auto settingsPath = combinePath(paths.settingsDirectory, "settings.json");
    auto patch = patchImportedSettingsFile(settingsPath, options);
    if (!patch)
    {
        return makeUnexpected(patch.error());
    }

    if (summary.importedFiles.contains(u"window-layout.json"_s))
    {
        auto layoutPath = combinePath(paths.settingsDirectory,
                                      "window-layout.json");
        auto layoutPatch = patchImportedWindowLayoutFile(layoutPath, options);
        if (!layoutPatch)
        {
            return makeUnexpected(layoutPatch.error());
        }
    }

    return summary;
}

bool restartApplication()
{
#ifdef Q_OS_WIN
    const auto pid = QString::number(QCoreApplication::applicationPid());
    const auto workingDirectory =
        windowsCommandPath(QApplication::applicationDirPath());
    const auto applicationPath =
        windowsCommandPath(QApplication::applicationFilePath());
    const auto scriptPath = restartScriptPath();
    const auto script = QString(R"(@echo off
setlocal
set "PID=%1"
set "APPDIR=%2"
set "APP=%3"

:wait
tasklist /FI "PID eq %PID%" | find "%PID%" >nul
if not errorlevel 1 (
    ping -n 2 127.0.0.1 >nul
    goto wait
)

start "" /D "%APPDIR%" "%APP%" --activate-after-import
del "%~f0"
)")
                            .arg(pid, workingDirectory, applicationPath);

    auto write = writeFile(scriptPath, script.toLocal8Bit());
    if (!write)
    {
        return false;
    }

    return QProcess::startDetached("cmd.exe", {"/C", "call", scriptPath});
#else
    return QProcess::startDetached(QApplication::applicationFilePath(),
                                   QStringList{});
#endif
}

}  // namespace chatterino::chatterino_import
