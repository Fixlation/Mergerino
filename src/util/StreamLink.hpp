// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <stdexcept>
#include <string>

class QObject;
class QProcess;

namespace chatterino {

struct StreamlinkSource {
    QString service;
    QString url;
};

// Parse Streamlink's machine-readable stream listing (highest quality first).
QStringList parseStreamlinkQualities(const QByteArray &output);

// The callback runs on context's thread. Destroying context cancels the query.
QProcess *getStreamQualities(
    const QString &channelURL, QObject *context,
    std::function<void(QStringList, QString)> callback);
void cancelStreamlinkQuery(QProcess *process);

class Exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#ifdef Q_OS_WIN
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink.exe";
#else
constexpr inline QStringView STREAMLINK_BINARY_NAME = u"streamlink";
#endif

// Open streamlink for given channel, quality and extra arguments
// the "Additional arguments" are fetched and added at the beginning of the
// streamlink call
void openStreamlink(const QString &channelURL, const QString &quality,
                    QStringList extraArguments = QStringList());

// Start opening streamlink for the given channel, reading settings like quality
// from settings and opening a quality dialog if the quality is "Choose"
void openStreamlinkForChannel(const QString &channel,
                              QStringView prefixURL = u"twitch.tv/");

}  // namespace chatterino
