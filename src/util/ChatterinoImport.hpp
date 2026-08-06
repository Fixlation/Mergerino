// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QString>
#include <QStringList>

#include <optional>

namespace chatterino {

class Paths;

namespace chatterino_import {

struct ImportOptions {
    std::optional<bool> startupPromptAcknowledged;
    std::optional<bool> autorun;
    std::optional<QString> currentVersion;
    std::optional<QString> mergedPlatformIndicatorMode;
    std::optional<QString> platformEventHighlightStyle;
    std::optional<QString> platformEventHighlightCustomColor;
    std::optional<QString> platformAlertHighlightStyle;
    std::optional<QString> platformAlertHighlightCustomColor;
};

struct ImportSummary {
    QString sourceDirectory;
    QString backupSuffix;
    QStringList importedFiles;
};

QString defaultSourceSettingsDirectory();
bool defaultSourceSettingsDirectoryExists();

bool hasPendingImport(const Paths &paths);
ExpectedStr<void> stageImportFromDefaultSource(const Paths &paths,
                                               const ImportOptions &options);
ExpectedStr<ImportSummary> applyPendingImport(const Paths &paths);
ExpectedStr<ImportSummary> importFromDirectory(const QString &sourceDirectory,
                                               const Paths &paths,
                                               const ImportOptions &options);

bool restartApplication();

}  // namespace chatterino_import

}  // namespace chatterino
