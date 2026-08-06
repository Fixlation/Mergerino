// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/LinkResolver.hpp"

#include "Application.hpp"
#include "common/Env.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QCache>
#include <QColor>
#include <QHostAddress>
#include <QHostInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>
#include <QStringBuilder>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>

namespace chatterino {

namespace {

constexpr int PREVIEW_CACHE_SIZE = 256;
constexpr int MAX_GENERIC_REQUESTS = 4;
constexpr int MAX_PREVIEW_REDIRECTS = 5;
constexpr qsizetype MAX_PREVIEW_HTML_BYTES = 1024 * 1024;
constexpr int PREVIEW_REQUEST_TIMEOUT_MS = 15000;
constexpr int MAX_FAVICON_REDIRECTS = 3;
constexpr qsizetype MAX_FAVICON_BYTES = 256 * 1024;
constexpr int FAVICON_REQUEST_TIMEOUT_MS = 5000;

struct PreviewData {
    QString resolvedUrl;
    QString title;
    QString subtitle;
    QString siteName;
    QString imageUrl;
    QString faviconUrl;
    QColor accentColor;
    QSize imageSize{640, 360};
};

struct AccentBucket {
    qreal weight = 0;
    qreal red = 0;
    qreal green = 0;
    qreal blue = 0;
};

QColor accentColorFromImage(const QByteArray &bytes)
{
    QImage image;
    if (!image.loadFromData(bytes))
    {
        return {};
    }

    image = image.scaled(QSize(64, 64), Qt::KeepAspectRatio,
                         Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_ARGB32);

    std::array<AccentBucket, 24> buckets{};
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            const QColor color = QColor::fromRgba(image.pixel(x, y));
            if (color.alpha() < 48 || color.hsvSaturation() < 64 ||
                color.value() < 48)
            {
                continue;
            }

            const int hue = color.hsvHue();
            if (hue < 0)
            {
                continue;
            }

            const qreal alpha = color.alphaF();
            const qreal saturation = color.hsvSaturationF();
            const qreal value = color.valueF();
            const qreal weight =
                alpha * saturation * saturation * (0.5 + value);
            auto &bucket = buckets.at(
                static_cast<size_t>(std::clamp(hue / 15, 0, 23)));
            bucket.weight += weight;
            bucket.red += color.redF() * weight;
            bucket.green += color.greenF() * weight;
            bucket.blue += color.blueF() * weight;
        }
    }

    const auto dominant = std::max_element(
        buckets.cbegin(), buckets.cend(), [](const auto &left, const auto &right) {
            return left.weight < right.weight;
        });
    if (dominant == buckets.cend() || dominant->weight <= 0)
    {
        return {};
    }

    return QColor::fromRgbF(dominant->red / dominant->weight,
                            dominant->green / dominant->weight,
                            dominant->blue / dominant->weight);
}

QString cleanPreviewText(QString text, qsizetype maxLength)
{
    text = text.simplified();
    if (text.size() > maxLength)
    {
        text = text.left(maxLength - 1) + QChar(0x2026);
    }
    return text;
}

bool isWebUrl(const QUrl &url)
{
    const auto scheme = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty() &&
           (scheme == QStringLiteral("http") ||
            scheme == QStringLiteral("https"));
}

bool isSafeFetchUrl(const QUrl &url)
{
    if (!isWebUrl(url))
    {
        return false;
    }

    const auto host = url.host().toLower();
    if (!url.userInfo().isEmpty() ||
        host == QStringLiteral("localhost") ||
        host.endsWith(QStringLiteral(".localhost")) ||
        host.endsWith(QStringLiteral(".local")))
    {
        return false;
    }

    const auto port = url.port();
    if (port != -1 && port != 80 && port != 443)
    {
        return false;
    }

    QHostAddress address;
    if (address.setAddress(host) && !address.isGlobal())
    {
        return false;
    }
    return true;
}

QString safeImageUrl(const QUrl &url)
{
    if (!isSafeFetchUrl(url))
    {
        return {};
    }
    return url.toString(QUrl::FullyEncoded);
}

QString safeImageUrl(const QString &value)
{
    return safeImageUrl(QUrl(value));
}

QString normalizedPreviewKey(QUrl url)
{
    url.setFragment({});
    return url.adjusted(QUrl::NormalizePathSegments)
        .toString(QUrl::FullyEncoded);
}

bool hostMatchesDomain(const QString &host, const QString &domain)
{
    return host == domain ||
           host.endsWith(QStringLiteral(".") + domain);
}

bool isTikTokUrl(const QUrl &url)
{
    return hostMatchesDomain(url.host().toLower(),
                             QStringLiteral("tiktok.com"));
}

bool isMediaPreviewUrl(const QUrl &url)
{
    static const QStringList mediaDomains{
        QStringLiteral("youtube.com"),   QStringLiteral("youtu.be"),
        QStringLiteral("clips.twitch.tv"), QStringLiteral("twitch.tv"),
        QStringLiteral("x.com"),         QStringLiteral("twitter.com"),
        QStringLiteral("imgur.com"),
        QStringLiteral("prnt.sc"),       QStringLiteral("lightshot.com"),
        QStringLiteral("tiktok.com"),    QStringLiteral("instagram.com"),
        QStringLiteral("threads.net"),   QStringLiteral("reddit.com"),
        QStringLiteral("redd.it"),       QStringLiteral("vimeo.com"),
        QStringLiteral("streamable.com"), QStringLiteral("giphy.com"),
        QStringLiteral("tenor.com"),     QStringLiteral("bsky.app"),
        QStringLiteral("facebook.com"),  QStringLiteral("gyazo.com"),
        QStringLiteral("catbox.moe"),    QStringLiteral("imgbb.com"),
        QStringLiteral("postimg.cc"),    QStringLiteral("discordapp.com"),
        QStringLiteral("discordapp.net"),
    };

    const auto host = url.host().toLower();
    for (const auto &domain : mediaDomains)
    {
        if (hostMatchesDomain(host, domain))
        {
            return true;
        }
    }

    static const QStringList mediaExtensions{
        QStringLiteral(".jpg"),  QStringLiteral(".jpeg"),
        QStringLiteral(".png"),  QStringLiteral(".gif"),
        QStringLiteral(".webp"), QStringLiteral(".avif"),
        QStringLiteral(".bmp"),  QStringLiteral(".svg"),
        QStringLiteral(".mp4"),  QStringLiteral(".webm"),
        QStringLiteral(".mov"),  QStringLiteral(".m4v"),
        QStringLiteral(".m3u8"),
    };
    const auto path = url.path().toLower();
    return std::any_of(mediaExtensions.cbegin(), mediaExtensions.cend(),
                       [&path](const auto &extension) {
                           return path.endsWith(extension);
                       });
}

PreviewData basicPreview(const QUrl &url)
{
    PreviewData preview;
    preview.resolvedUrl = url.toString(QUrl::FullyEncoded);
    auto faviconUrl = url;
    faviconUrl.setPath(QStringLiteral("/favicon.ico"));
    faviconUrl.setQuery(QString{});
    faviconUrl.setFragment({});
    preview.faviconUrl = safeImageUrl(faviconUrl);
    preview.siteName = url.host();
    if (preview.siteName.startsWith(QStringLiteral("www."),
                                    Qt::CaseInsensitive))
    {
        preview.siteName.remove(0, 4);
    }

    auto path = QUrl::fromPercentEncoding(url.path().toUtf8());
    while (path.endsWith(QChar('/')))
    {
        path.chop(1);
    }
    preview.title = cleanPreviewText(path.section(QChar('/'), -1), 180);
    if (preview.title.isEmpty())
    {
        preview.title = preview.siteName;
    }
    return preview;
}

QString decodeHtmlText(QString text, qsizetype maxLength)
{
    text = QTextDocumentFragment::fromHtml(std::move(text))
               .toPlainText()
               .simplified();
    return cleanPreviewText(std::move(text), maxLength);
}

QHash<QString, QString> htmlTagAttributes(const QString &tag)
{
    static const QRegularExpression attributePattern(
        QStringLiteral(
            R"ATTR(([A-Za-z_:][A-Za-z0-9_:.\-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))ATTR"),
        QRegularExpression::CaseInsensitiveOption);

    QHash<QString, QString> attributes;
    auto match = attributePattern.globalMatch(tag);
    while (match.hasNext())
    {
        const auto attribute = match.next();
        QString value;
        for (int group = 2; group <= 4; ++group)
        {
            if (attribute.capturedLength(group) >= 0)
            {
                value = attribute.captured(group);
                break;
            }
        }
        attributes.insert(attribute.captured(1).toLower(), std::move(value));
    }
    return attributes;
}

QString firstMetadataValue(const QHash<QString, QString> &metadata,
                           std::initializer_list<QString> keys)
{
    for (const auto &key : keys)
    {
        const auto value = metadata.value(key);
        if (!value.isEmpty())
        {
            return value;
        }
    }
    return {};
}

PreviewData previewFromHtml(const QUrl &finalUrl, const QByteArray &body,
                            bool &foundMetadata)
{
    auto preview = basicPreview(finalUrl);
    const auto html = QString::fromUtf8(body);
    QHash<QString, QString> metadata;

    static const QRegularExpression metaPattern(
        QStringLiteral(R"(<meta\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::DotMatchesEverythingOption);
    auto metaMatch = metaPattern.globalMatch(html);
    while (metaMatch.hasNext())
    {
        const auto attributes =
            htmlTagAttributes(metaMatch.next().capturedView().toString());
        auto key = attributes.value(QStringLiteral("property")).toLower();
        if (key.isEmpty())
        {
            key = attributes.value(QStringLiteral("name")).toLower();
        }
        const auto content = attributes.value(QStringLiteral("content"));
        if (!key.isEmpty() && !content.isEmpty() &&
            !metadata.contains(key))
        {
            metadata.insert(std::move(key), content);
        }
    }

    QColor themeColor(
        metadata.value(QStringLiteral("theme-color")).trimmed());
    if (themeColor.isValid())
    {
        themeColor.setAlpha(255);
        preview.accentColor = themeColor;
    }

    static const QRegularExpression linkPattern(
        QStringLiteral(R"(<link\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::DotMatchesEverythingOption);
    int faviconScore = 0;
    auto linkMatch = linkPattern.globalMatch(html);
    while (linkMatch.hasNext())
    {
        const auto attributes =
            htmlTagAttributes(linkMatch.next().capturedView().toString());
        const auto rel = attributes.value(QStringLiteral("rel")).toLower();
        const auto href = attributes.value(QStringLiteral("href"));
        if (!rel.contains(QStringLiteral("icon")) || href.isEmpty())
        {
            continue;
        }

        const int score =
            rel.split(QRegularExpression(QStringLiteral(R"(\s+)")),
                      Qt::SkipEmptyParts)
                    .contains(QStringLiteral("icon"))
                ? 3
            : rel.contains(QStringLiteral("shortcut")) ? 2
                                                       : 1;
        if (score <= faviconScore)
        {
            continue;
        }

        const auto decodedHref =
            QTextDocumentFragment::fromHtml(href).toPlainText().trimmed();
        const auto candidate =
            safeImageUrl(finalUrl.resolved(QUrl(decodedHref)));
        if (!candidate.isEmpty())
        {
            preview.faviconUrl = candidate;
            faviconScore = score;
        }
    }

    auto title = firstMetadataValue(
        metadata, {QStringLiteral("og:title"),
                   QStringLiteral("twitter:title")});
    if (title.isEmpty())
    {
        static const QRegularExpression titlePattern(
            QStringLiteral(R"(<title\b[^>]*>(.*?)</title>)"),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::DotMatchesEverythingOption);
        const auto titleMatch = titlePattern.match(html);
        if (titleMatch.hasMatch())
        {
            title = titleMatch.captured(1);
        }
    }
    title = decodeHtmlText(std::move(title), 220);
    if (!title.isEmpty())
    {
        preview.title = std::move(title);
        foundMetadata = true;
    }

    auto description = firstMetadataValue(
        metadata, {QStringLiteral("og:description"),
                   QStringLiteral("twitter:description"),
                   QStringLiteral("description")});
    description = decodeHtmlText(std::move(description), 420);
    if (!description.isEmpty())
    {
        preview.subtitle = std::move(description);
        foundMetadata = true;
    }

    auto siteName = firstMetadataValue(
        metadata, {QStringLiteral("og:site_name"),
                   QStringLiteral("application-name")});
    siteName = decodeHtmlText(std::move(siteName), 100);
    if (!siteName.isEmpty())
    {
        preview.siteName = std::move(siteName);
        foundMetadata = true;
    }

    const auto imageValue = firstMetadataValue(
        metadata, {QStringLiteral("og:image"),
                   QStringLiteral("og:image:url"),
                   QStringLiteral("twitter:image"),
                   QStringLiteral("twitter:image:src")});
    if (!imageValue.isEmpty())
    {
        const auto decodedImageValue =
            QTextDocumentFragment::fromHtml(imageValue).toPlainText().trimmed();
        const auto imageUrl = finalUrl.resolved(QUrl(decodedImageValue));
        preview.imageUrl = safeImageUrl(imageUrl);
        if (!preview.imageUrl.isEmpty())
        {
            foundMetadata = true;
        }
    }

    bool widthOk = false;
    bool heightOk = false;
    const auto imageWidth =
        metadata.value(QStringLiteral("og:image:width")).toInt(&widthOk);
    const auto imageHeight =
        metadata.value(QStringLiteral("og:image:height")).toInt(&heightOk);
    if (widthOk && heightOk && imageWidth > 0 && imageHeight > 0 &&
        imageWidth <= 16384 && imageHeight <= 16384)
    {
        preview.imageSize = {imageWidth, imageHeight};
    }

    return preview;
}

QString twitchClipSlug(const QUrl &url)
{
    const auto host = url.host().toLower();
    const auto parts = url.path().split(QChar('/'), Qt::SkipEmptyParts);

    if (host == QStringLiteral("clips.twitch.tv") && !parts.isEmpty())
    {
        if (parts.front().compare(QStringLiteral("embed"),
                                  Qt::CaseInsensitive) == 0)
        {
            return QUrlQuery(url).queryItemValue(QStringLiteral("clip"),
                                                  QUrl::FullyDecoded)
                .trimmed();
        }
        return QUrl::fromPercentEncoding(parts.front().toUtf8());
    }

    if (host == QStringLiteral("twitch.tv") ||
        host == QStringLiteral("www.twitch.tv") ||
        host == QStringLiteral("m.twitch.tv"))
    {
        for (qsizetype i = 0; i + 1 < parts.size(); ++i)
        {
            if (parts.at(i).compare(QStringLiteral("clip"),
                                    Qt::CaseInsensitive) == 0)
            {
                return QUrl::fromPercentEncoding(parts.at(i + 1).toUtf8());
            }
        }
    }
    return {};
}

QString kickClipID(const QUrl &url)
{
    const auto host = url.host().toLower();
    if (host != QStringLiteral("kick.com") &&
        host != QStringLiteral("www.kick.com"))
    {
        return {};
    }

    const auto parts = url.path().split(QChar('/'), Qt::SkipEmptyParts);
    if (parts.size() != 3 ||
        parts.at(1).compare(QStringLiteral("clips"),
                            Qt::CaseInsensitive) != 0)
    {
        return {};
    }

    const auto clipID =
        QUrl::fromPercentEncoding(parts.at(2).toUtf8()).trimmed();
    static const QRegularExpression clipIDPattern(
        QStringLiteral(R"(^clip_[A-Za-z0-9]+$)"));
    return clipIDPattern.match(clipID).hasMatch() ? clipID : QString{};
}

PreviewData kickClipFallback(const QUrl &targetUrl)
{
    auto preview = basicPreview(targetUrl);
    preview.title = QStringLiteral("Kick Clip");
    preview.subtitle = QStringLiteral("Kick Clip");
    preview.siteName = QStringLiteral("Kick");
    preview.accentColor = QColor::fromRgb(0x53, 0xFC, 0x18);
    preview.faviconUrl.clear();
    return preview;
}

}  // namespace

bool isKickClipUrl(const QString &url)
{
    const QUrl parsed(url);
    return isWebUrl(parsed) && !kickClipID(parsed).isEmpty();
}

bool shouldShowLinkPreview(const QString &url)
{
    switch (linkPreviewModeSetting().getEnum())
    {
        case LinkPreviewMode::Disabled:
            return false;
        case LinkPreviewMode::All:
            return true;
        case LinkPreviewMode::MediaOnly: {
            const QUrl parsed(url);
            return isWebUrl(parsed) &&
                   (!twitchClipSlug(parsed).isEmpty() ||
                    !kickClipID(parsed).isEmpty() ||
                    isMediaPreviewUrl(parsed));
        }
    }
    return false;
}

bool shouldSuppressLinkPreview(const Message &message)
{
    if (message.platform != MessagePlatform::AnyOrTwitch &&
        message.platform != MessagePlatform::Kick)
    {
        return false;
    }

    const bool hasTwitchBotBadge =
        std::any_of(message.twitchBadges.cbegin(),
                    message.twitchBadges.cend(), [](const auto &badge) {
                        return badge.key_.compare(
                                   QStringLiteral("bot"),
                                   Qt::CaseInsensitive) == 0 ||
                               badge.key_.compare(
                                   QStringLiteral("chatbot"),
                                   Qt::CaseInsensitive) == 0;
                    });
    if (hasTwitchBotBadge)
    {
        return true;
    }

    const bool hasOtherBotBadge =
        std::any_of(message.externalBadges.cbegin(),
                    message.externalBadges.cend(), [](const auto &badge) {
                        return badge.endsWith(QStringLiteral(":bot"),
                                              Qt::CaseInsensitive);
                    });
    if (hasOtherBotBadge)
    {
        return true;
    }

    static const QSet<QString> knownBotAccounts{
        QStringLiteral("botrix"),
        QStringLiteral("botrixoficial"),
        QStringLiteral("coebot"),
        QStringLiteral("deepbot"),
        QStringLiteral("fossabot"),
        QStringLiteral("moobot"),
        QStringLiteral("nightbot"),
        QStringLiteral("sery_bot"),
        QStringLiteral("streamelements"),
        QStringLiteral("streamlabs"),
        QStringLiteral("wizebot"),
    };

    return knownBotAccounts.contains(
               message.loginName.trimmed().toCaseFolded()) ||
           knownBotAccounts.contains(
               message.displayName.trimmed().toCaseFolded());
}

class LinkResolverPrivate final : public QObject
{
public:
    LinkResolverPrivate()
        : cache_(PREVIEW_CACHE_SIZE)
        , network_(this)
    {
    }

    void resolveCard(LinkInfo *info)
    {
        QUrl targetUrl(info->originalUrl());
        if (!isWebUrl(targetUrl))
        {
            info->setTooltip(QStringLiteral("Unsupported link"));
            info->setState(LinkInfo::State::Errored);
            return;
        }
        targetUrl.setFragment({});

        const auto key = normalizedPreviewKey(targetUrl);
        if (const auto *cached = this->cache_.object(key))
        {
            this->apply(info, *cached);
            return;
        }

        const bool alreadyLoading = this->waiters_.contains(key);
        this->waiters_[key].append(QPointer<LinkInfo>(info));
        info->setTooltip(QStringLiteral("Loading preview..."));
        info->setState(LinkInfo::State::Loading);
        if (alreadyLoading)
        {
            return;
        }

        const auto twitchClip = twitchClipSlug(targetUrl);
        const auto kickClip = kickClipID(targetUrl);
        if (!twitchClip.isEmpty())
        {
            this->resolveTwitchClip(key, targetUrl, twitchClip);
        }
        else if (!kickClip.isEmpty())
        {
            this->resolveKickClip(key, targetUrl, kickClip);
        }
        else if (isTikTokUrl(targetUrl))
        {
            this->resolveTikTok(key, targetUrl);
        }
        else
        {
            this->resolveGeneric(key, targetUrl);
        }
    }

private:
    struct GenericJob {
        QString key;
        QUrl targetUrl;
    };

    struct DownloadState {
        QByteArray body;
        bool limitReached = false;
    };

    void validateHost(const QUrl &url, std::function<void(bool)> callback)
    {
        if (!isSafeFetchUrl(url))
        {
            callback(false);
            return;
        }

        QHostAddress literalAddress;
        if (literalAddress.setAddress(url.host()))
        {
            callback(literalAddress.isGlobal());
            return;
        }

        QHostInfo::lookupHost(
            url.host(), this,
            [callback = std::move(callback)](const QHostInfo &hostInfo) mutable {
                bool allowed = hostInfo.error() == QHostInfo::NoError &&
                               !hostInfo.addresses().isEmpty();
                for (const auto &address : hostInfo.addresses())
                {
                    if (!address.isGlobal())
                    {
                        allowed = false;
                        break;
                    }
                }
                callback(allowed);
            });
    }

    void fetchAccentColor(const QUrl &requestUrl, int redirectCount,
                          std::function<void(QColor)> callback)
    {
        this->validateHost(
            requestUrl,
            [this, requestUrl, redirectCount,
             callback = std::move(callback)](bool allowed) mutable {
                if (!allowed)
                {
                    callback({});
                    return;
                }

                QNetworkRequest request(requestUrl);
                request.setAttribute(
                    QNetworkRequest::RedirectPolicyAttribute,
                    QNetworkRequest::ManualRedirectPolicy);
                request.setAttribute(
                    QNetworkRequest::CookieLoadControlAttribute,
                    QNetworkRequest::Manual);
                request.setAttribute(
                    QNetworkRequest::CookieSaveControlAttribute,
                    QNetworkRequest::Manual);
                request.setHeader(
                    QNetworkRequest::UserAgentHeader,
                    QStringLiteral(
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/126.0 Safari/537.36 Mergerino/Favicon"));
                request.setRawHeader("Accept", "image/avif,image/webp,"
                                               "image/apng,image/svg+xml,"
                                               "image/*,*/*;q=0.5");

                auto *reply = this->network_.get(request);
                auto state = std::make_shared<DownloadState>();
                QObject::connect(
                    reply, &QIODevice::readyRead, this,
                    [reply, state] {
                        const auto remaining =
                            MAX_FAVICON_BYTES - state->body.size();
                        if (remaining <= 0)
                        {
                            state->limitReached = true;
                            reply->abort();
                            return;
                        }

                        auto chunk = reply->read(remaining + 1);
                        if (chunk.size() > remaining)
                        {
                            chunk.truncate(remaining);
                            state->limitReached = true;
                        }
                        state->body.append(chunk);
                        if (state->limitReached)
                        {
                            reply->abort();
                        }
                    });

                QTimer::singleShot(FAVICON_REQUEST_TIMEOUT_MS, reply, [reply] {
                    if (reply->isRunning())
                    {
                        reply->abort();
                    }
                });

                QObject::connect(
                    reply, &QNetworkReply::finished, this,
                    [this, requestUrl, redirectCount, reply,
                     state = std::move(state),
                     callback = std::move(callback)]() mutable {
                        if (!state->limitReached &&
                            reply->bytesAvailable() > 0)
                        {
                            const auto remaining =
                                MAX_FAVICON_BYTES - state->body.size();
                            auto chunk = reply->read(remaining + 1);
                            if (chunk.size() > remaining)
                            {
                                chunk.truncate(remaining);
                                state->limitReached = true;
                            }
                            state->body.append(chunk);
                        }

                        const auto status =
                            reply
                                ->attribute(
                                    QNetworkRequest::HttpStatusCodeAttribute)
                                .toInt();
                        auto redirect =
                            reply
                                ->attribute(
                                    QNetworkRequest::RedirectionTargetAttribute)
                                .toUrl();
                        const auto networkError = reply->error();
                        reply->deleteLater();

                        if (status >= 300 && status < 400 &&
                            redirect.isValid() &&
                            redirectCount < MAX_FAVICON_REDIRECTS)
                        {
                            redirect = requestUrl.resolved(redirect);
                            this->fetchAccentColor(
                                redirect, redirectCount + 1,
                                std::move(callback));
                            return;
                        }

                        if (networkError != QNetworkReply::NoError ||
                            status < 200 || status >= 300 ||
                            state->limitReached)
                        {
                            callback({});
                            return;
                        }
                        callback(accentColorFromImage(state->body));
                    });
            });
    }

    void finishPreview(const QString &key, PreviewData preview,
                       bool requestRelayout, std::function<void()> done)
    {
        this->finish(key, std::move(preview), requestRelayout);
        if (done)
        {
            done();
        }
    }

    void finishAfterAccent(const QString &key, PreviewData preview,
                           bool requestRelayout,
                           std::function<void()> done)
    {
        if (preview.faviconUrl.isEmpty())
        {
            this->finishPreview(key, std::move(preview), requestRelayout,
                                std::move(done));
            return;
        }

        const QUrl faviconUrl(preview.faviconUrl);
        this->fetchAccentColor(
            faviconUrl, 0,
            [this, key, preview = std::move(preview), requestRelayout,
             done = std::move(done)](QColor accentColor) mutable {
                if (accentColor.isValid())
                {
                    preview.accentColor = std::move(accentColor);
                }
                this->finishPreview(key, std::move(preview), requestRelayout,
                                    std::move(done));
            });
    }

    void finishAfterImageValidation(const QString &key, PreviewData preview,
                                    bool requestRelayout,
                                    std::function<void()> done = {})
    {
        if (preview.imageUrl.isEmpty())
        {
            this->finishAfterAccent(key, std::move(preview), requestRelayout,
                                    std::move(done));
            return;
        }

        const QUrl imageUrl(preview.imageUrl);
        this->validateHost(
            imageUrl,
            [this, key, preview = std::move(preview), requestRelayout,
             done = std::move(done)](bool allowed) mutable {
                if (!allowed)
                {
                    preview.imageUrl.clear();
                }
                this->finishAfterAccent(key, std::move(preview),
                                        requestRelayout, std::move(done));
            });
    }

    void apply(LinkInfo *info, const PreviewData &preview)
    {
        info->setPreview(preview.title, preview.subtitle, preview.siteName,
                         preview.accentColor);
        info->setThumbnail(
            preview.imageUrl.isEmpty()
                ? nullptr
                : Image::fromUrl({preview.imageUrl}, 1.0,
                                 preview.imageSize.isValid()
                                     ? preview.imageSize
                                     : QSize{640, 360}));

        if (getSettings()->unshortLinks && !preview.resolvedUrl.isEmpty())
        {
            info->setResolvedUrl(preview.resolvedUrl);
        }

        QString tooltip = preview.title;
        if (!preview.subtitle.isEmpty())
        {
            tooltip += QChar('\n');
            tooltip += preview.subtitle;
        }
        tooltip += QChar('\n');
        tooltip += preview.resolvedUrl.isEmpty() ? info->originalUrl()
                                                 : preview.resolvedUrl;
        info->setTooltip(std::move(tooltip));
        info->setState(LinkInfo::State::Resolved);
    }

    void finish(const QString &key, PreviewData preview, bool requestRelayout)
    {
        this->cache_.insert(key, new PreviewData(preview));
        const auto waiters = this->waiters_.take(key);
        for (const auto &waiter : waiters)
        {
            if (waiter)
            {
                this->apply(waiter.data(), preview);
            }
        }

        if (requestRelayout && !waiters.isEmpty())
        {
            if (auto *app = tryGetApp())
            {
                app->getWindows()->forceLayoutChannelViews();
            }
        }
    }

    void resolveTwitchClip(const QString &key, const QUrl &targetUrl,
                           const QString &clipSlug)
    {
        auto &twitchAccounts = getApp()->getAccounts()->twitch;
        auto account = twitchAccounts.getCurrent();
        const auto hasClipCredentials = [](const auto &candidate) {
            return candidate && !candidate->getOAuthClient().isEmpty() &&
                   !candidate->getOAuthToken().isEmpty();
        };
        if (!hasClipCredentials(account))
        {
            for (const auto &candidate : twitchAccounts.accounts)
            {
                if (hasClipCredentials(candidate))
                {
                    account = candidate;
                    break;
                }
            }
        }
        if (!hasClipCredentials(account))
        {
            this->resolveGeneric(key, targetUrl);
            return;
        }

        QUrl apiUrl(QStringLiteral("https://api.twitch.tv/helix/clips"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("id"), clipSlug);
        apiUrl.setQuery(query);

        NetworkRequest(apiUrl)
            .caller(this)
            .header("Client-Id", account->getOAuthClient())
            .header("Authorization",
                    QStringLiteral("Bearer ") + account->getOAuthToken())
            .header("Accept", "application/json")
            .timeout(10000)
            .onSuccess([this, key, targetUrl](const NetworkResult &result) {
                const auto clips = result.parseJson()
                                       .value(QStringLiteral("data"))
                                       .toArray();
                if (clips.isEmpty())
                {
                    this->resolveGeneric(key, targetUrl);
                    return;
                }

                const auto clip = clips.first().toObject();
                auto preview = basicPreview(targetUrl);
                preview.title = cleanPreviewText(
                    clip.value(QStringLiteral("title")).toString(), 180);
                if (preview.title.isEmpty())
                {
                    preview.title = QStringLiteral("Twitch Clip");
                }
                const auto creator = cleanPreviewText(
                    clip.value(QStringLiteral("creator_name")).toString(), 80);
                preview.subtitle = creator.isEmpty()
                                       ? QStringLiteral("Twitch Clip")
                                       : QStringLiteral("Clipped by %1")
                                             .arg(creator);
                preview.siteName = QStringLiteral("Twitch");
                preview.imageUrl = safeImageUrl(
                    clip.value(QStringLiteral("thumbnail_url")).toString());
                preview.imageSize = {480, 272};

                const QUrl resolved(
                    clip.value(QStringLiteral("url")).toString());
                if (isWebUrl(resolved))
                {
                    preview.resolvedUrl =
                        resolved.toString(QUrl::FullyEncoded);
                }
                this->finishAfterImageValidation(key, std::move(preview),
                                                 true);
            })
            .onError([this, key, targetUrl](const NetworkResult &) {
                this->resolveGeneric(key, targetUrl);
            })
            .execute();
    }

    void resolveKickClip(const QString &key, const QUrl &targetUrl,
                         const QString &clipID)
    {
        QUrl apiUrl(QStringLiteral("https://kick.com/api/v2/clips/"));
        apiUrl.setPath(QStringLiteral("/api/v2/clips/") + clipID);

        NetworkRequest(apiUrl)
            .caller(this)
            .header("Accept", "application/json")
            .timeout(10000)
            .onSuccess([this, key, targetUrl](const NetworkResult &result) {
                const auto clip = result.parseJson()
                                      .value(QStringLiteral("clip"))
                                      .toObject();
                if (clip.isEmpty())
                {
                    this->finishAfterImageValidation(
                        key, kickClipFallback(targetUrl), true);
                    return;
                }

                auto preview = kickClipFallback(targetUrl);
                preview.title = cleanPreviewText(
                    clip.value(QStringLiteral("title")).toString(), 180);
                if (preview.title.isEmpty())
                {
                    preview.title = QStringLiteral("Kick Clip");
                }

                const auto creatorObject =
                    clip.value(QStringLiteral("creator")).toObject();
                auto creator = cleanPreviewText(
                    creatorObject.value(QStringLiteral("username")).toString(),
                    80);
                if (creator.isEmpty())
                {
                    creator = cleanPreviewText(
                        creatorObject.value(QStringLiteral("slug")).toString(),
                        80);
                }
                preview.subtitle = creator.isEmpty()
                                       ? QStringLiteral("Kick Clip")
                                       : QStringLiteral("Clipped by %1")
                                             .arg(creator);
                preview.imageUrl = safeImageUrl(
                    clip.value(QStringLiteral("thumbnail_url")).toString());
                preview.imageSize = {480, 272};

                this->finishAfterImageValidation(key, std::move(preview),
                                                 true);
            })
            .onError([this, key, targetUrl](const NetworkResult &) {
                this->finishAfterImageValidation(
                    key, kickClipFallback(targetUrl), true);
            })
            .execute();
    }

    void resolveTikTok(const QString &key, const QUrl &targetUrl)
    {
        QUrl apiUrl(QStringLiteral("https://www.tiktok.com/oembed"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("url"),
                           targetUrl.toString(QUrl::FullyEncoded));
        apiUrl.setQuery(query);

        NetworkRequest(apiUrl)
            .caller(this)
            .header("Accept", "application/json")
            .timeout(10000)
            .onSuccess([this, key, targetUrl](const NetworkResult &result) {
                const auto root = result.parseJson();
                const auto thumbnail = safeImageUrl(
                    root.value(QStringLiteral("thumbnail_url")).toString());
                if (thumbnail.isEmpty())
                {
                    this->resolveGeneric(key, targetUrl);
                    return;
                }

                auto preview = basicPreview(targetUrl);
                preview.title = cleanPreviewText(
                    root.value(QStringLiteral("title")).toString(), 180);
                if (preview.title.isEmpty())
                {
                    preview.title = QStringLiteral("TikTok video");
                }

                auto author = cleanPreviewText(
                    root.value(QStringLiteral("author_name")).toString(), 80);
                if (author.startsWith(QChar('@')))
                {
                    author.remove(0, 1);
                }
                if (!author.isEmpty())
                {
                    preview.subtitle = QStringLiteral("By @%1").arg(author);
                }

                preview.siteName = QStringLiteral("TikTok");
                preview.imageUrl = thumbnail;
                preview.faviconUrl = safeImageUrl(
                    QStringLiteral("https://www.tiktok.com/favicon.ico"));

                const int imageWidth =
                    root.value(QStringLiteral("thumbnail_width")).toInt();
                const int imageHeight =
                    root.value(QStringLiteral("thumbnail_height")).toInt();
                if (imageWidth > 0 && imageHeight > 0 &&
                    imageWidth <= 16384 && imageHeight <= 16384)
                {
                    preview.imageSize = {imageWidth, imageHeight};
                }

                this->finishAfterImageValidation(key, std::move(preview),
                                                 true);
            })
            .onError([this, key, targetUrl](const NetworkResult &) {
                this->resolveGeneric(key, targetUrl);
            })
            .execute();
    }

    void resolveGeneric(const QString &key, const QUrl &targetUrl)
    {
        this->genericQueue_.enqueue({key, targetUrl});
        this->pumpGenericQueue();
    }

    void pumpGenericQueue()
    {
        while (this->activeGenericRequests_ < MAX_GENERIC_REQUESTS &&
               !this->genericQueue_.isEmpty())
        {
            const auto job = this->genericQueue_.dequeue();
            this->activeGenericRequests_++;
            this->startGenericFetch(job, job.targetUrl, 0);
        }
    }

    void startGenericFetch(const GenericJob &job, const QUrl &requestUrl,
                           int redirectCount)
    {
        this->validateHost(
            requestUrl,
            [this, job, requestUrl, redirectCount](bool allowed) {
                if (!allowed)
                {
                    this->completeGeneric(job, basicPreview(job.targetUrl));
                    return;
                }

                QNetworkRequest request(requestUrl);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                     QNetworkRequest::ManualRedirectPolicy);
                request.setAttribute(
                    QNetworkRequest::CookieLoadControlAttribute,
                    QNetworkRequest::Manual);
                request.setAttribute(
                    QNetworkRequest::CookieSaveControlAttribute,
                    QNetworkRequest::Manual);
                request.setHeader(
                    QNetworkRequest::UserAgentHeader,
                    QStringLiteral(
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                        "AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Chrome/126.0 Safari/537.36 Mergerino/LinkPreview"));
                request.setRawHeader(
                    "Accept",
                    "text/html,application/xhtml+xml,image/avif,image/webp,"
                    "image/apng,image/*;q=0.8,*/*;q=0.5");
                request.setRawHeader("Accept-Language", "en-US,en;q=0.8");

                auto *reply = this->network_.get(request);
                auto state = std::make_shared<DownloadState>();

                QObject::connect(
                    reply, &QIODevice::readyRead, this,
                    [reply, state] {
                        const auto remaining =
                            MAX_PREVIEW_HTML_BYTES - state->body.size();
                        if (remaining <= 0)
                        {
                            state->limitReached = true;
                            reply->abort();
                            return;
                        }

                        auto chunk = reply->read(remaining + 1);
                        if (chunk.size() > remaining)
                        {
                            chunk.truncate(remaining);
                            state->limitReached = true;
                        }
                        state->body.append(chunk);
                        if (state->limitReached)
                        {
                            reply->abort();
                        }
                    });

                QTimer::singleShot(PREVIEW_REQUEST_TIMEOUT_MS, reply,
                                   [reply] {
                                       if (reply->isRunning())
                                       {
                                           reply->abort();
                                       }
                                   });

                QObject::connect(
                    reply, &QNetworkReply::finished, this,
                    [this, job, requestUrl, redirectCount, reply,
                     state = std::move(state)]() mutable {
                        if (!state->limitReached && reply->bytesAvailable() > 0)
                        {
                            const auto remaining =
                                MAX_PREVIEW_HTML_BYTES - state->body.size();
                            if (remaining > 0)
                            {
                                state->body.append(reply->read(remaining));
                            }
                        }

                        const auto status =
                            reply
                                ->attribute(
                                    QNetworkRequest::HttpStatusCodeAttribute)
                                .toInt();
                        auto redirect =
                            reply
                                ->attribute(
                                    QNetworkRequest::RedirectionTargetAttribute)
                                .toUrl();
                        const auto contentType =
                            reply->header(QNetworkRequest::ContentTypeHeader)
                                .toString()
                                .toLower();
                        const auto networkError = reply->error();
                        reply->deleteLater();

                        if (status >= 300 && status < 400 &&
                            redirect.isValid())
                        {
                            redirect = requestUrl.resolved(redirect);
                            if (redirectCount < MAX_PREVIEW_REDIRECTS &&
                                isSafeFetchUrl(redirect))
                            {
                                this->startGenericFetch(job, redirect,
                                                        redirectCount + 1);
                            }
                            else
                            {
                                this->completeGeneric(
                                    job, basicPreview(job.targetUrl));
                            }
                            return;
                        }

                        const bool successfulStatus =
                            status >= 200 && status < 300;
                        const bool usableResponse =
                            networkError == QNetworkReply::NoError ||
                            state->limitReached;

                        if (successfulStatus &&
                            contentType.startsWith(QStringLiteral("image/")))
                        {
                            auto preview = basicPreview(requestUrl);
                            preview.imageUrl = safeImageUrl(requestUrl);
                            this->completeGeneric(job, std::move(preview));
                            return;
                        }

                        bool foundMetadata = false;
                        auto preview = previewFromHtml(
                            requestUrl, state->body, foundMetadata);
                        if ((successfulStatus && usableResponse) ||
                            foundMetadata)
                        {
                            this->completeGeneric(job, std::move(preview));
                        }
                        else
                        {
                            this->completeGeneric(
                                job, basicPreview(job.targetUrl));
                        }
                    });
            });
    }

    void completeGeneric(const GenericJob &job, PreviewData preview)
    {
        this->finishAfterImageValidation(
            job.key, std::move(preview), true,
            [this] { this->genericRequestFinished(); });
    }

    void genericRequestFinished()
    {
        this->activeGenericRequests_--;
        this->pumpGenericQueue();
    }

    QCache<QString, PreviewData> cache_;
    QHash<QString, QList<QPointer<LinkInfo>>> waiters_;
    QQueue<GenericJob> genericQueue_;
    int activeGenericRequests_ = 0;
    QNetworkAccessManager network_;
};

LinkResolverPrivate &linkResolverPrivate()
{
    static auto *resolver = new LinkResolverPrivate;
    return *resolver;
}

void LinkResolver::resolve(LinkInfo *info)
{
    using State = LinkInfo::State;

    assert(info);

    if (info->state() != State::Created)
    {
        // The link is already resolved or is currently loading
        return;
    }

    if (shouldShowLinkPreview(info->originalUrl()))
    {
        linkResolverPrivate().resolveCard(info);
        return;
    }

    if (!getSettings()->linkInfoTooltip)
    {
        return;
    }

    if (Env::get().linkResolverUrl.isEmpty())
    {
        info->setTooltip("Link previews are unavailable in this build.");
        info->setState(State::Errored);
        return;
    }

    info->setTooltip("Loading...");
    info->setState(State::Loading);

    NetworkRequest(Env::get().linkResolverUrl.arg(QString::fromUtf8(
                       QUrl::toPercentEncoding(info->originalUrl(), {}, "/:"))))
        .caller(info)
        .timeout(30000)
        .onSuccess([info](const NetworkResult &result) {
            const auto root = result.parseJson();
            QString response;
            QString url;
            ImagePtr thumbnail = nullptr;
            if (root["status"].toInt() == 200)
            {
                response = root["tooltip"].toString();

                if (root.contains("thumbnail"))
                {
                    info->setThumbnail(
                        Image::fromUrl({root["thumbnail"].toString()}));
                }
                if (getSettings()->unshortLinks && root.contains("link"))
                {
                    info->setResolvedUrl(root["link"].toString());
                }
            }
            else
            {
                response = root["message"].toString();
            }

            info->setTooltip(QUrl::fromPercentEncoding(response.toUtf8()));
            info->setState(State::Resolved);
        })
        .onError([info](const auto &result) {
            info->setTooltip(u"No link info found (" % result.formatError() %
                             u')');
            info->setState(State::Errored);
        })
        .execute();
}

}  // namespace chatterino
