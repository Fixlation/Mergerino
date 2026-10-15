// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/StreamLink.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/QualityPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <QCollator>
#include <QErrorMessage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStringBuilder>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace {

using namespace chatterino;

QString getStreamlinkPath()
{
    if (getSettings()->streamlinkUseCustomPath)
    {
        const QString path = getSettings()->streamlinkPath;
        return path.trimmed() % "/" % STREAMLINK_BINARY_NAME;
    }

    return STREAMLINK_BINARY_NAME.toString();
}

void showStreamlinkNotFoundError()
{
    static auto *msg = new QErrorMessage;
    msg->setWindowTitle("Mergerino - streamlink not found");

    if (getSettings()->streamlinkUseCustomPath)
    {
        msg->showMessage("Unable to find Streamlink executable\nMake sure "
                         "your custom path is pointing to the DIRECTORY "
                         "where the streamlink executable is located");
    }
    else
    {
        msg->showMessage(
            "Unable to find Streamlink executable.\nIf you have Streamlink "
            "installed, you might need to enable the custom path option");
    }
}

}  // namespace

namespace chatterino {

QStringList parseStreamlinkQualities(const QByteArray &output)
{
    const auto streams =
        QJsonDocument::fromJson(output).object().value("streams").toObject();
    QStringList qualities;
    for (auto it = streams.begin(); it != streams.end(); ++it)
    {
        if (it.value().isObject() && it.key() != "best" && it.key() != "worst")
        {
            qualities.append(it.key());
        }
    }
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(qualities.begin(), qualities.end(),
              [&collator](const auto &left, const auto &right) {
                  return collator.compare(left, right) > 0;
              });
    // Prefer video over audio and preserve Streamlink's exact quality names.
    for (const auto &audio :
         {QStringLiteral("audio"), QStringLiteral("audio_only")})
    {
        if (qualities.removeAll(audio))
        {
            qualities.append(audio);
        }
    }
    if (qualities.isEmpty() && streams.value("best").isObject())
    {
        qualities.append("best");
    }
    return qualities;
}

void cancelStreamlinkQuery(QProcess *process)
{
    process->setProperty("streamlinkQueryCancelled", true);
    if (process->state() == QProcess::NotRunning)
    {
        process->deleteLater();
    }
    else
    {
        process->kill();
    }
}

QProcess *getStreamQualities(const QString &channelURL, QObject *context,
                             std::function<void(QStringList, QString)> callback)
{
    auto *process = new QProcess;
    process->setProgram(getStreamlinkPath());
    auto arguments =
        QProcess::splitCommand(getSettings()->streamlinkOpts.getValue());
    arguments << "--json" << channelURL;
    process->setArguments(arguments);
    // Processes own their timeout and clean themselves up even after the popup
    // disconnects an obsolete query. No callback can outlive the popup.
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto completed = std::make_shared<bool>(false);
    auto finish = [completed, callback = std::move(callback), timer, process](
                      QStringList qualities, QString error) {
        if (std::exchange(*completed, true) ||
            process->property("streamlinkQueryCancelled").toBool())
        {
            return;
        }
        timer->stop();
        callback(std::move(qualities), std::move(error));
    };
    QObject::connect(context, &QObject::destroyed, process, [process] {
        cancelStreamlinkQuery(process);
    });
    QObject::connect(timer, &QTimer::timeout, context, [process, finish] {
        finish({}, "Streamlink timed out. Check the connection and refresh "
                   "qualities.");
        process->kill();
    });
    QObject::connect(
        process, &QProcess::errorOccurred, context,
        [finish](QProcess::ProcessError error) {
            finish(
                {},
                error == QProcess::FailedToStart
                    ? "Streamlink could not start. Check its installation and "
                      "path in Settings."
                    : "Streamlink could not load qualities. Try refreshing.");
        });
    QObject::connect(
        process, &QProcess::finished, context,
        [process, finish](int exitCode, QProcess::ExitStatus status) {
            auto qualities =
                parseStreamlinkQualities(process->readAllStandardOutput());
            if (status != QProcess::NormalExit || exitCode != 0 ||
                qualities.isEmpty())
            {
                finish({}, "No playable qualities were returned. The stream "
                           "may be offline or unsupported. Check Streamlink is "
                           "up to date, then refresh.");
                return;
            }
            finish(std::move(qualities), {});
        });
    QObject::connect(process, &QProcess::finished, process,
                     &QObject::deleteLater);
    QObject::connect(process, &QProcess::errorOccurred, process,
                     [process](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart)
                         {
                             process->deleteLater();
                         }
                     });
    // Defer even a synchronous FailedToStart until the caller has stored the
    // returned process. A consumer may also disappear before this runs.
    QTimer::singleShot(
        0, process, [process, timer, guard = QPointer<QObject>(context)] {
            if (!guard ||
                process->property("streamlinkQueryCancelled").toBool())
            {
                process->deleteLater();
                return;
            }
            timer->start(30000);
            process->start();
        });
    return process;
}

void openStreamlink(const QString &channelURL, const QString &quality,
                    QStringList extraArguments)
{
    QProcess process;
    auto *proc = &process;
    proc->setProgram(getStreamlinkPath());
    auto arguments = proc->arguments()
                     << std::move(extraArguments) << channelURL << quality;

    // Remove empty arguments before appending additional streamlink options
    // as the options might purposely contain empty arguments
    arguments.removeAll(QString());

    QString additionalOptions = getSettings()->streamlinkOpts.getValue();
    arguments << QProcess::splitCommand(additionalOptions);

    proc->setArguments(arguments);
    bool res = proc->startDetached();

    if (!res)
    {
        showStreamlinkNotFoundError();
    }
}

void openStreamlinkForChannel(const QString &channel, QStringView prefixURL)
{
    static const QString INFO_TEMPLATE("Opening %1 in Streamlink ...");

    auto *currentPage = dynamic_cast<SplitContainer *>(getApp()
                                                           ->getWindows()
                                                           ->getMainWindow()
                                                           .getNotebook()
                                                           .getSelectedPage());
    if (currentPage != nullptr)
    {
        auto *currentSplit = currentPage->getSelectedSplit();
        if (currentSplit != nullptr)
        {
            currentSplit->getChannel()->addSystemMessage(
                INFO_TEMPLATE.arg(channel));
        }
    }

    QString channelURL = prefixURL % channel;

    auto preferredQuality = getSettings()->preferredQuality.getEnum();

    if (preferredQuality == StreamLinkPreferredQuality::Choose)
    {
        QualityPopup::showDialog(
            {{prefixURL.contains(u"kick.com") ? "Kick" : "Twitch",
              channelURL}});

        return;
    }

    QStringList args;

    // Quality converted from Chatterino format to Streamlink format
    QString quality;
    // Streamlink qualities to exclude
    QString exclude;

    if (preferredQuality == StreamLinkPreferredQuality::High)
    {
        exclude = ">720p30";
        quality = "high,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::Medium)
    {
        exclude = ">540p30";
        quality = "medium,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::Low)
    {
        exclude = ">360p30";
        quality = "low,best";
    }
    else if (preferredQuality == StreamLinkPreferredQuality::AudioOnly)
    {
        quality = "audio,audio_only";
    }
    else
    {
        quality = "best";
    }
    if (!exclude.isEmpty())
    {
        args << "--stream-sorting-excludes" << exclude;
    }

    openStreamlink(channelURL, quality, args);
}

}  // namespace chatterino
