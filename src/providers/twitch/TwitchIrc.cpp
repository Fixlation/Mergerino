// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchIrc.hpp"

#include "Application.hpp"
#include "common/Aliases.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Image.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "util/IrcHelpers.hpp"

#include <QUrl>

namespace {

using namespace chatterino;

std::vector<int> correctCharacterPositions(const QString &content)
{
    std::vector<int> positions;
    positions.reserve(content.size());
    for (int i = 0; i < content.size(); ++i)
    {
        if (!content.at(i).isLowSurrogate())
        {
            positions.push_back(i);
        }
    }
    return positions;
}

bool isTrustedGiphyUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme().compare(
                              QStringLiteral("https"),
                              Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    const auto host = url.host().toLower();
    return host == QStringLiteral("giphy.com") ||
           host.endsWith(QStringLiteral(".giphy.com"));
}

QUrl smallerGiphyGifUrl(QUrl url)
{
    auto path = url.path();
    const auto originalFilename = QStringLiteral("giphy.gif");
    if (path.endsWith(originalFilename, Qt::CaseInsensitive))
    {
        path.chop(originalFilename.size());
        path += QStringLiteral("100w.gif");
        url.setPath(path);
    }
    return url;
}

void appendTwitchEmoteOccurrences(const QString &emote,
                                  std::vector<TwitchEmoteOccurrence> &vec,
                                  const std::vector<int> &correctPositions,
                                  const QString &originalMessage,
                                  int messageOffset)
{
    auto *app = getApp();
    if (!emote.contains(':'))
    {
        return;
    }

    auto parameters = emote.split(':');

    if (parameters.length() < 2)
    {
        return;
    }

    auto id = EmoteId{parameters.at(0)};

    auto occurrences = parameters.at(1).split(',');

    for (const QString &occurrence : occurrences)
    {
        auto coords = occurrence.split('-');

        if (coords.length() < 2)
        {
            return;
        }

        auto from = coords.at(0).toUInt() - messageOffset;
        auto to = coords.at(1).toUInt() - messageOffset;
        auto maxPositions = correctPositions.size();
        if (from > to || to >= maxPositions)
        {
            // Emote coords are out of range
            qCDebug(chatterinoTwitch)
                << "Emote coords" << from << "-" << to << "are out of range ("
                << maxPositions << ")";
            return;
        }

        auto start = correctPositions[from];
        auto end = correctPositions[to];
        if (start > end || start < 0 || end > originalMessage.length())
        {
            // Emote coords are out of range from the modified character positions
            qCDebug(chatterinoTwitch) << "Emote coords" << from << "-" << to
                                      << "are out of range after offsets ("
                                      << originalMessage.length() << ")";
            return;
        }

        auto name = EmoteName{originalMessage.mid(start, end - start + 1)};
        TwitchEmoteOccurrence emoteOccurrence{
            start,
            end,
            app->getEmotes()->getTwitchEmotes()->getOrCreateEmote(id, name),
            name,
        };
        if (emoteOccurrence.ptr == nullptr)
        {
            qCDebug(chatterinoTwitch)
                << "nullptr" << emoteOccurrence.name.string;
        }
        vec.push_back(std::move(emoteOccurrence));
    }
}

}  // namespace

namespace chatterino {

std::unordered_map<QString, QString> parseBadgeInfoTag(const QVariantMap &tags)
{
    std::unordered_map<QString, QString> infoMap;

    auto infoIt = tags.constFind("badge-info");
    if (infoIt == tags.end())
    {
        return infoMap;
    }

    auto info = infoIt.value().toString().split(',', Qt::SkipEmptyParts);

    for (const QString &badge : info)
    {
        infoMap.emplace(slashKeyValue(badge));
    }

    return infoMap;
}

std::vector<TwitchBadge> parseBadgeTag(const QVariantMap &tags,
                                       const QString &tagName)
{
    std::vector<TwitchBadge> b;

    auto badgesIt = tags.constFind(tagName);
    if (badgesIt == tags.end())
    {
        return b;
    }

    auto badges = badgesIt.value().toString().split(',', Qt::SkipEmptyParts);

    for (const QString &badge : badges)
    {
        if (!badge.contains('/'))
        {
            continue;
        }

        auto pair = slashKeyValue(badge);
        b.emplace_back(TwitchBadge{pair.first, pair.second});
    }

    return b;
}

std::vector<TwitchEmoteOccurrence> parseTwitchEmotes(const QVariantMap &tags,
                                                     const QString &content,
                                                     int messageOffset)
{
    // Twitch emotes
    std::vector<TwitchEmoteOccurrence> twitchEmotes;

    auto emotesTag = tags.find("emotes");

    if (emotesTag == tags.end())
    {
        return twitchEmotes;
    }

    QStringList emoteString = emotesTag.value().toString().split('/');
    auto correctPositions = correctCharacterPositions(content);
    for (const QString &emote : emoteString)
    {
        appendTwitchEmoteOccurrences(emote, twitchEmotes, correctPositions,
                                     content, messageOffset);
    }

    return twitchEmotes;
}

std::vector<TwitchEmoteOccurrence> parseTwitchGifs(const QVariantMap &tags,
                                                   const QString &content,
                                                   int messageOffset)
{
    std::vector<TwitchEmoteOccurrence> gifs;

    const auto gifsIt = tags.constFind(QStringLiteral("gifs"));
    if (gifsIt == tags.cend())
    {
        return gifs;
    }

    const auto tag = gifsIt.value().toString();
    const auto firstSeparator = tag.indexOf(QLatin1Char('|'));
    const auto secondSeparator = tag.indexOf(QLatin1Char('|'),
                                             firstSeparator + 1);
    if (firstSeparator <= 0 || secondSeparator <= firstSeparator + 1)
    {
        return gifs;
    }

    const auto range = tag.left(firstSeparator).split(QLatin1Char('-'));
    if (range.size() != 2)
    {
        return gifs;
    }

    bool validStart = false;
    bool validEnd = false;
    const auto taggedStart = range.at(0).toInt(&validStart);
    const auto taggedEnd = range.at(1).toInt(&validEnd);
    const auto startPosition = taggedStart - messageOffset;
    const auto endPosition = taggedEnd - messageOffset;
    if (!validStart || !validEnd || startPosition < 0 ||
        endPosition < startPosition)
    {
        return gifs;
    }

    const auto positions = correctCharacterPositions(content);
    if (endPosition >= static_cast<int>(positions.size()))
    {
        return gifs;
    }

    const auto start = positions.at(startPosition);
    const auto end = positions.at(endPosition);
    if (start < 0 || end < start || end >= content.size())
    {
        return gifs;
    }

    const auto gifID = tag.mid(firstSeparator + 1,
                               secondSeparator - firstSeparator - 1)
                           .trimmed();
    const QUrl sourceUrl(tag.mid(secondSeparator + 1).trimmed());
    if (gifID.isEmpty() || !isTrustedGiphyUrl(sourceUrl))
    {
        return gifs;
    }

    const auto displayUrl = smallerGiphyGifUrl(sourceUrl);
    const auto name = EmoteName{content.mid(start, end - start + 1)};
    const auto emote = std::make_shared<const Emote>(Emote{
        .name = name,
        .images = ImageSet{
            Image::fromAutoscaledUrl({displayUrl.toString()}, 100)},
        .tooltip = Tooltip{name.string.toHtmlEscaped() +
                           QStringLiteral("<br>GIPHY GIF")},
        .homePage = Url{QStringLiteral("https://giphy.com/gifs/") + gifID},
        .id = EmoteId{QStringLiteral("giphy:") + gifID},
    });

    gifs.push_back(TwitchEmoteOccurrence{start, end, emote, name});
    return gifs;
}

}  // namespace chatterino
