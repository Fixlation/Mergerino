// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/tiktok/TikTokLiveChat.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/websockets/WebSocketPool.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "util/PostToThread.hpp"

#include <QDateTime>
#include <QDir>
#include <QHostAddress>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <limits>

namespace {

using namespace chatterino;

const QColor TIKTOK_PLATFORM_ACCENT(37, 244, 238, 48);
const QColor TIKTOK_USERNAME_COLOR(254, 44, 85);
constexpr int TIKTOK_RECONNECT_DELAY_MS = 5000;
constexpr int TIKTOK_WAIT_FOR_LIVE_DELAY_MS = 15000;
constexpr int TIKTOK_BROWSER_TARGET_POLL_DELAY_MS = 750;
constexpr int TIKTOK_BROWSER_SNAPSHOT_DELAY_MS = 1500;
constexpr int TIKTOK_BROWSER_TARGET_POLL_LIMIT = 40;

struct BrowserChatMessage {
    QString owner;
    QString text;
    QString fullText;
};

struct BrowserGiftEvent {
    QString id;
    QString senderDisplayName;
    QString senderUniqueId;
    QString recipientDisplayName;
    QString giftName;
    uint32_t quantity = 1;
    uint32_t diamondCount = 0;
};

struct BrowserJoinEvent {
    QString id;
    QString displayName;
    QString uniqueId;
};

struct CachedTikTokProfile {
    QString displayName;
    QString uniqueId;
    QString avatarUrl;
};

QString tiktokProfileKey(QString value)
{
    value = value.trimmed();
    if (value.startsWith('@'))
    {
        value.remove(0, 1);
    }
    return value.toLower();
}

QHash<QString, CachedTikTokProfile> &tiktokProfileCache()
{
    static QHash<QString, CachedTikTokProfile> cache;
    return cache;
}

void rememberTikTokProfile(CachedTikTokProfile profile)
{
    if (profile.displayName.isEmpty() && profile.uniqueId.isEmpty())
    {
        return;
    }

    auto &cache = tiktokProfileCache();
    if (cache.size() >= 10000)
    {
        cache.clear();
    }

    const auto displayKey = tiktokProfileKey(profile.displayName);
    const auto idKey = tiktokProfileKey(profile.uniqueId);
    if (!displayKey.isEmpty())
    {
        cache.insert(displayKey, profile);
    }
    if (!idKey.isEmpty())
    {
        cache.insert(idKey, std::move(profile));
    }
}

CachedTikTokProfile cachedTikTokProfile(const QString &userName,
                                        const QString &userID)
{
    const auto &cache = tiktokProfileCache();
    const auto idKey = tiktokProfileKey(userID);
    if (!idKey.isEmpty())
    {
        if (const auto it = cache.constFind(idKey); it != cache.cend())
        {
            return *it;
        }
    }

    const auto nameKey = tiktokProfileKey(userName);
    if (!nameKey.isEmpty())
    {
        if (const auto it = cache.constFind(nameKey); it != cache.cend())
        {
            return *it;
        }
    }

    return {};
}

bool operator==(const BrowserChatMessage &lhs, const BrowserChatMessage &rhs)
{
    return lhs.owner == rhs.owner && lhs.text == rhs.text &&
           lhs.fullText == rhs.fullText;
}

QString trimTrailingSlash(QString value)
{
    while (value.size() > 1 && value.endsWith('/'))
    {
        value.chop(1);
    }

    return value;
}

QString decodeEscapedJsonString(QString value)
{
    if (value.isEmpty())
    {
        return {};
    }

    const auto json = QByteArray("{\"value\":\"") + value.toUtf8() + "\"}";
    const auto document = QJsonDocument::fromJson(json);
    if (document.isObject())
    {
        return document.object().value("value").toString().trimmed();
    }

    value.replace("\\/", "/");
    value.replace("\\\"", "\"");
    return value.trimmed();
}

QString extractFirstMatch(const QString &text, const QStringList &patterns,
                          bool decodeJsonString = false)
{
    for (const auto &pattern : patterns)
    {
        const QRegularExpression regex(pattern);
        const auto match = regex.match(text);
        if (!match.hasMatch())
        {
            continue;
        }

        auto captured = match.captured(1);
        if (decodeJsonString)
        {
            captured = decodeEscapedJsonString(captured);
        }
        return captured.trimmed();
    }

    return {};
}

QString normalizeTikTokSource(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    static const QRegularExpression usernameRegex("^@?([A-Za-z0-9._]+)$");

    if (const auto match = usernameRegex.match(value); match.hasMatch())
    {
        return QString("@%1").arg(match.captured(1).toLower());
    }

    if (!value.contains("://") &&
        (value.startsWith("www.", Qt::CaseInsensitive) ||
         value.startsWith("m.", Qt::CaseInsensitive) ||
         value.startsWith("tiktok.com/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    const QUrl url(value);
    if (!url.isValid() || url.host().isEmpty())
    {
        return {};
    }

    const auto host = url.host().toLower();
    if (!host.endsWith("tiktok.com"))
    {
        return {};
    }

    const auto path = trimTrailingSlash(url.path());
    static const QRegularExpression pathRegex("^/@([A-Za-z0-9._]+)(?:/live)?$");
    if (const auto match = pathRegex.match(path); match.hasMatch())
    {
        return QString("@%1").arg(match.captured(1).toLower());
    }

    return {};
}

std::vector<std::pair<QByteArray, QByteArray>> tiktokHeaders()
{
    return {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
        {"Accept", "*/*"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Origin", "https://www.tiktok.com"},
        {"Referer", "https://www.tiktok.com/"},
    };
}

QString localTimeZoneName()
{
    const auto timezoneId = QTimeZone::systemTimeZoneId();
    if (timezoneId.isEmpty())
    {
        return "UTC";
    }

    return QString::fromUtf8(timezoneId);
}

QUrl makeRoomInfoUrl(const QString &uniqueId)
{
    QUrl url("https://www.tiktok.com/api-live/user/room/");
    QUrlQuery query;
    query.addQueryItem("uniqueId", uniqueId);
    query.addQueryItem("sourceType", "54");
    query.addQueryItem("aid", "1988");
    query.addQueryItem("app_name", "tiktok_web");
    query.addQueryItem("device_platform", "web");
    url.setQuery(query);
    return url;
}

QUrl makeLivePageUrl(const QString &source)
{
    return QUrl(QString("https://www.tiktok.com/%1/live").arg(source));
}

void appendSimpleWords(MessageBuilder &builder, const QString &text)
{
    for (auto word : QStringView(text).tokenize(u' ', Qt::SkipEmptyParts))
    {
        builder.addWordFromUserMessage(word);
    }
}

MessagePtr makeSystemStatusMessage(const QString &text)
{
    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->platform = MessagePlatform::TikTok;
    builder->platformAccentColor = TIKTOK_PLATFORM_ACCENT;
    builder->messageText = text;
    builder->searchText = text;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                 MessageColor::System);
    return builder.release();
}

qint64 parseJsonLongLong(const QJsonValue &value)
{
    if (value.isDouble())
    {
        return static_cast<qint64>(value.toDouble());
    }

    if (value.isString())
    {
        bool ok = false;
        const auto parsed = value.toString().trimmed().toLongLong(&ok);
        if (ok)
        {
            return parsed;
        }
    }

    return 0;
}

uint32_t parseJsonUInt(const QJsonValue &value)
{
    return static_cast<uint32_t>(std::clamp<qint64>(
        parseJsonLongLong(value), 0, std::numeric_limits<uint32_t>::max()));
}

QString formatTikTokDiamondCount(uint32_t diamondCount)
{
    return diamondCount == 1 ? QStringLiteral("1 diamond")
                             : QStringLiteral("%1 diamonds")
                                   .arg(QString::number(diamondCount));
}

QString buildGiftEventText(const BrowserGiftEvent &event)
{
    QString text = QStringLiteral("sent ");
    text += event.giftName.trimmed().isEmpty() ? QStringLiteral("a gift")
                                               : event.giftName.trimmed();

    if (!event.recipientDisplayName.trimmed().isEmpty())
    {
        text += QStringLiteral(" to %1").arg(event.recipientDisplayName.trimmed());
    }

    if (event.quantity > 1)
    {
        text += QStringLiteral(" x%1").arg(QString::number(event.quantity));
    }

    if (event.diamondCount > 0)
    {
        text += QStringLiteral(" (%1)")
                    .arg(formatTikTokDiamondCount(event.diamondCount));
    }

    return text;
}

MessagePtr makeChatMessage(const BrowserChatMessage &message,
                           const QString &channelName)
{
    if (message.owner.isEmpty() || message.text.isEmpty())
    {
        return nullptr;
    }

    const auto profile = cachedTikTokProfile(message.owner, {});
    const auto loginName = !profile.uniqueId.isEmpty() ? profile.uniqueId
                                                       : message.owner;
    MessageBuilder builder;
    builder->platform = MessagePlatform::TikTok;
    builder->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    builder->loginName = loginName;
    builder->displayName = message.owner;
    builder->localizedName = message.owner;
    builder->userID = profile.uniqueId;
    builder->channelName = channelName;
    builder->usernameColor = TIKTOK_USERNAME_COLOR;
    builder->messageText = message.text;
    builder->searchText = message.owner + ": " + message.text;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder
        .emplace<TextElement>(message.owner + ":", MessageElementFlag::Username,
                              TIKTOK_USERNAME_COLOR,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, message.owner});
    appendSimpleWords(builder, message.text);
    return builder.release();
}

MessagePtr makeGiftMessage(const BrowserGiftEvent &event,
                           const QString &channelName)
{
    const auto displayName = !event.senderDisplayName.trimmed().isEmpty()
                                 ? event.senderDisplayName.trimmed()
                                 : event.senderUniqueId.trimmed();
    const auto loginName = !event.senderUniqueId.trimmed().isEmpty()
                               ? event.senderUniqueId.trimmed()
                               : displayName;
    if (displayName.isEmpty())
    {
        return nullptr;
    }

    const auto text = buildGiftEventText(event);
    if (text.isEmpty())
    {
        return nullptr;
    }

    MessageBuilder builder;
    builder->platform = MessagePlatform::TikTok;
    builder->flags.set(MessageFlag::Subscription);
    builder->id = event.id.trimmed().isEmpty()
                      ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                      : event.id.trimmed();
    builder->loginName = loginName;
    builder->displayName = displayName;
    builder->localizedName = displayName;
    builder->userID = event.senderUniqueId;
    builder->channelName = channelName;
    builder->usernameColor = TIKTOK_USERNAME_COLOR;
    builder->messageText = text;
    builder->searchText = displayName + ": " + text;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    builder->tiktokGiftDiamondCount = event.diamondCount;
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder
        .emplace<TextElement>(displayName + ":", MessageElementFlag::Username,
                              TIKTOK_USERNAME_COLOR,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, displayName});
    appendSimpleWords(builder, text);
    return builder.release();
}

MessagePtr makeJoinMessage(const BrowserJoinEvent &event,
                           const QString &channelName)
{
    const auto displayName = !event.displayName.trimmed().isEmpty()
                                 ? event.displayName.trimmed()
                                 : event.uniqueId.trimmed();
    const auto loginName = !event.uniqueId.trimmed().isEmpty()
                               ? event.uniqueId.trimmed()
                               : displayName;
    if (displayName.isEmpty())
    {
        return nullptr;
    }

    const auto text = QStringLiteral("joined the stream");
    MessageBuilder builder;
    builder->platform = MessagePlatform::TikTok;
    builder->platformAccentColor = TIKTOK_PLATFORM_ACCENT;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->flags.set(MessageFlag::TikTokJoinMessage);
    builder->id = event.id.trimmed().isEmpty()
                      ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                      : event.id.trimmed();
    builder->loginName = loginName;
    builder->displayName = displayName;
    builder->localizedName = displayName;
    builder->userID = event.uniqueId;
    builder->channelName = channelName;
    builder->usernameColor = TIKTOK_USERNAME_COLOR;
    builder->messageText = displayName + " " + text;
    builder->searchText = builder->messageText;
    builder->serverReceivedTime = QDateTime::currentDateTime();
    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder
        .emplace<TextElement>(displayName, MessageElementFlag::Username,
                              TIKTOK_USERNAME_COLOR,
                              FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, displayName});
    builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                 MessageColor::System);
    return builder.release();
}

QStringList browserExecutableCandidates()
{
    QStringList candidates;
    auto addCandidate = [&candidates](const QString &candidate) {
        if (!candidate.isEmpty() && !candidates.contains(candidate))
        {
            candidates.push_back(candidate);
        }
    };

#ifdef Q_OS_WINDOWS
    const auto programFiles = qEnvironmentVariable("ProgramFiles");
    const auto programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)");
    const auto localAppData = qEnvironmentVariable("LocalAppData");

    auto addIfExists = [&addCandidate](const QString &path) {
        if (!path.isEmpty() && QFileInfo::exists(path))
        {
            addCandidate(path);
        }
    };

    addIfExists(localAppData + "/Google/Chrome/Application/chrome.exe");
    addIfExists(programFiles + "/Google/Chrome/Application/chrome.exe");
    addIfExists(programFilesX86 + "/Google/Chrome/Application/chrome.exe");
    addIfExists(localAppData + "/Chromium/Application/chrome.exe");
    addIfExists(programFiles + "/Chromium/Application/chrome.exe");
    addIfExists(programFilesX86 + "/Chromium/Application/chrome.exe");
    addIfExists(programFilesX86 +
                "/BraveSoftware/Brave-Browser/Application/brave.exe");
    addIfExists(programFiles +
                "/BraveSoftware/Brave-Browser/Application/brave.exe");
    addIfExists(localAppData +
                "/Microsoft/Edge/Application/msedge.exe");
    addIfExists(programFiles + "/Microsoft/Edge/Application/msedge.exe");
    addIfExists(programFilesX86 +
                "/Microsoft/Edge/Application/msedge.exe");

    addCandidate("chrome");
    addCandidate("brave");
    addCandidate("chromium");
    addCandidate("msedge");
#elif defined(Q_OS_MACOS)
    addCandidate("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
    addCandidate(
        "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser");
    addCandidate("/Applications/Chromium.app/Contents/MacOS/Chromium");
    addCandidate("/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft "
                 "Edge");
#else
    addCandidate("google-chrome");
    addCandidate("google-chrome-stable");
    addCandidate("brave");
    addCandidate("brave-browser");
    addCandidate("chromium");
    addCandidate("chromium-browser");
    addCandidate("microsoft-edge");
    addCandidate("microsoft-edge-stable");
#endif

    return candidates;
}

QString findChromiumBrowserExecutable()
{
    for (const auto &candidate : browserExecutableCandidates())
    {
        const auto resolved = QStandardPaths::findExecutable(candidate);
        if (!resolved.isEmpty())
        {
            return resolved;
        }
        if (QFileInfo::exists(candidate))
        {
            return candidate;
        }
    }

    return {};
}

int allocateLocalPort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
    {
        return 0;
    }
    return server.serverPort();
}

QStringList buildBrowserArguments(const QString &pageUrl, int devToolsPort,
                                  const QString &profileDir)
{
    return {
        "--headless=new",
        "--disable-gpu",
        "--disable-extensions",
        "--disable-background-networking",
        "--disable-default-apps",
        "--hide-scrollbars",
        "--mute-audio",
        "--no-first-run",
        "--no-default-browser-check",
        "--remote-debugging-address=127.0.0.1",
        "--remote-allow-origins=*",
        QString("--remote-debugging-port=%1").arg(devToolsPort),
        QString("--user-data-dir=%1").arg(QDir::toNativeSeparators(profileDir)),
        "--window-size=1280,900",
        "--lang=en-GB",
        pageUrl,
    };
}

QUrl chooseBrowserTarget(const QJsonArray &targets, const QString &source)
{
    const auto livePath = QString("%1/live").arg(source);

    for (const auto &value : targets)
    {
        const auto object = value.toObject();
        if (object.value("type").toString() != "page")
        {
            continue;
        }

        const auto debuggerUrl =
            object.value("webSocketDebuggerUrl").toString().trimmed();
        if (debuggerUrl.isEmpty())
        {
            continue;
        }

        const auto pageUrl = object.value("url").toString();
        if (pageUrl.contains(livePath, Qt::CaseInsensitive) ||
            pageUrl.contains(source, Qt::CaseInsensitive))
        {
            return QUrl(debuggerUrl);
        }
    }

    for (const auto &value : targets)
    {
        const auto object = value.toObject();
        if (object.value("type").toString() == "page")
        {
            const auto debuggerUrl =
                object.value("webSocketDebuggerUrl").toString().trimmed();
            if (!debuggerUrl.isEmpty())
            {
                return QUrl(debuggerUrl);
            }
        }
    }

    return {};
}

QString browserSnapshotScript()
{
    static const QString script = QString::fromUtf8(R"JS(
(() => {
  const result = {
    pageTitle: document.title || '',
    roomId: '',
    ownerUniqueId: '',
    ownerNickname: '',
    liveTitle: '',
    liveRoomStatus: -1,
    showLiveChat: false,
    enableChat: false,
    messages: [],
    giftEvents: [],
    joinEvents: [],
    profileUpdates: []
  };

  function safeNumber(value) {
    const numeric = Number(value ?? 0);
    return Number.isFinite(numeric) ? numeric : 0;
  }

  function profileAvatarUrl(user) {
    const image = user?.avatar_medium || user?.avatar_thumb || user?.avatar_large;
    const urls = image?.url_list;
    if (!Array.isArray(urls)) {
      return '';
    }
    return String(urls.find((url) => String(url).startsWith('http')) || urls[0] || '');
  }

  function queueProfile(user) {
    const queue = window.__mergerinoTikTokGiftTap?.profileQueue;
    if (!Array.isArray(queue) || !user) {
      return;
    }

    const displayName = String(user.nickname || '');
    const uniqueId = String(user.unique_id || user.display_id || '');
    if (!displayName && !uniqueId) {
      return;
    }

    queue.push({
      displayName,
      uniqueId,
      avatarUrl: profileAvatarUrl(user)
    });
    if (queue.length > 200) {
      queue.splice(0, queue.length - 200);
    }
  }

  function findImContext() {
    const roots = [
      document.querySelector('[data-e2e="gift-container"]'),
      document.querySelector('[data-e2e="social-message"]'),
      document.querySelector('[data-e2e="chat-message"]')
    ].filter(Boolean);

    for (const root of roots) {
      let current = root;
      while (current) {
        for (const key of Object.keys(current)) {
          if (!key.startsWith('__reactFiber$')) {
            continue;
          }

          let fiber = current[key];
          const seen = new Set();
          while (fiber && !seen.has(fiber)) {
            seen.add(fiber);
            const props = fiber.memoizedProps || fiber.pendingProps;
            const ctx = props && (props.value || props.imContext || props.context);
            if (ctx && typeof ctx === 'object' && ctx.imInstance) {
              return ctx;
            }
            fiber = fiber.return;
          }
        }

        current = current.parentElement;
      }
    }

    return null;
  }

  function installGiftTap() {
    if (!window.__mergerinoTikTokGiftTap) {
      window.__mergerinoTikTokGiftTap = {
        installed: false,
        queue: [],
        joinQueue: [],
        profileQueue: []
      };
    }

    if (!Array.isArray(window.__mergerinoTikTokGiftTap.joinQueue)) {
      window.__mergerinoTikTokGiftTap.joinQueue = [];
    }
    if (!Array.isArray(window.__mergerinoTikTokGiftTap.profileQueue)) {
      window.__mergerinoTikTokGiftTap.profileQueue = [];
    }

    if (window.__mergerinoTikTokGiftTap.installed) {
      return;
    }

    const ctx = findImContext();
    const events = ctx?.imInstance?._messageEvents;
    if (!events || typeof events.runCallbackByMethod !== 'function' ||
        typeof events.pbReader?.decodePayload !== 'function') {
      return;
    }

    const originalRunCallbackByMethod = events.runCallbackByMethod;
    events.runCallbackByMethod = function(method, message, extra) {
      if (method === 'WebcastGiftMessage' && message?.payload) {
        try {
          const decoded = this.pbReader.decodePayload('GiftMessage', message.payload);
          Promise.resolve(decoded).then((payload) => {
            try {
              queueProfile(payload?.user);
              const gift = payload?.gift || {};
              const quantity = Math.max(
                safeNumber(payload?.repeat_count),
                safeNumber(payload?.combo_count),
                safeNumber(payload?.group_count),
                1
              );
              const perGiftDiamonds = safeNumber(gift?.diamond_count);
              const totalDiamonds = Math.max(
                safeNumber(payload?.fan_ticket_count),
                safeNumber(payload?.room_fan_ticket_count),
                perGiftDiamonds,
                perGiftDiamonds * quantity
              );

              window.__mergerinoTikTokGiftTap.queue.push({
                id: String(message.msg_id || payload?.common?.msg_id || ''),
                senderDisplayName: String(payload?.user?.nickname || ''),
                senderUniqueId: String(payload?.user?.unique_id || payload?.user?.display_id || ''),
                recipientDisplayName: String(payload?.to_member_nickname || payload?.to_user?.nickname || ''),
                giftName: String(gift?.name || ''),
                quantity,
                diamondCount: totalDiamonds
              });

              if (window.__mergerinoTikTokGiftTap.queue.length > 40) {
                window.__mergerinoTikTokGiftTap.queue.splice(
                  0,
                  window.__mergerinoTikTokGiftTap.queue.length - 40
                );
              }
            } catch {
            }
          }).catch(() => {});
        } catch {
        }
      }

      if (method === 'WebcastMemberMessage' && message?.payload) {
        try {
          const decoded = this.pbReader.decodePayload('MemberMessage', message.payload);
          Promise.resolve(decoded).then((payload) => {
            try {
              queueProfile(payload?.user);
              window.__mergerinoTikTokGiftTap.joinQueue.push({
                id: String(message.msg_id || payload?.common?.msg_id || ''),
                displayName: String(payload?.user?.nickname || ''),
                uniqueId: String(payload?.user?.unique_id || payload?.user?.display_id || '')
              });

              if (window.__mergerinoTikTokGiftTap.joinQueue.length > 100) {
                window.__mergerinoTikTokGiftTap.joinQueue.splice(
                  0,
                  window.__mergerinoTikTokGiftTap.joinQueue.length - 100
                );
              }
            } catch {
            }
          }).catch(() => {});
        } catch {
        }
      }

      if (method === 'WebcastChatMessage' && message?.payload) {
        try {
          const decoded = this.pbReader.decodePayload('ChatMessage', message.payload);
          Promise.resolve(decoded).then((payload) => {
            try {
              queueProfile(payload?.user);
            } catch {
            }
          }).catch(() => {});
        } catch {
        }
      }

      return originalRunCallbackByMethod.call(this, method, message, extra);
    };

    window.__mergerinoTikTokGiftTap.installed = true;
  }

  try {
    const sigiNode = document.getElementById('SIGI_STATE');
    if (sigiNode && sigiNode.textContent) {
      const sigi = JSON.parse(sigiNode.textContent);
      const currentRoom = sigi.CurrentRoom || {};
      const liveRoom = sigi.LiveRoom || {};
      const liveRoomUserInfo = liveRoom.liveRoomUserInfo || {};
      const roomInfo = currentRoom.roomInfo || {};
      const liveRoomData = liveRoomUserInfo.liveRoom || roomInfo.liveRoom || {};
      const userData = liveRoomUserInfo.user || roomInfo.user || {};

      result.roomId = String(
        currentRoom.roomId ||
        roomInfo.roomId ||
        liveRoomData.id_str ||
        userData.roomId ||
        ''
      );
      result.ownerUniqueId = String(currentRoom.anchorUniqueId || userData.uniqueId || '');
      result.ownerNickname = String(userData.nickname || '');
      result.liveTitle = String(liveRoomData.title || '');
      result.liveRoomStatus = Number(
        liveRoom.liveRoomStatus ?? liveRoomData.status ?? -1
      );
      result.showLiveChat = Boolean(currentRoom.showLiveChat);
      result.enableChat = Boolean(currentRoom.enableChat);
    }
  } catch {
  }

  const rows = Array.from(document.querySelectorAll('[data-e2e="chat-message"]'));
  result.messages = rows.slice(-60).map((row) => {
    const ownerNode =
      row.querySelector('[data-e2e="message-owner-name"]');
    const owner = ownerNode?.textContent?.trim() || '';
    const ownerHeader = ownerNode?.parentElement?.parentElement;
    const messageNode = ownerHeader?.nextElementSibling;
    const directText = messageNode?.innerText?.trim() || '';
    const lines = (row.innerText || '')
      .split(/\n+/)
      .map((line) => line.trim())
      .filter(Boolean);

    let ownerIndex = -1;
    if (owner) {
      ownerIndex = lines.findIndex((line) => line === owner);
    }

    let textLines = ownerIndex >= 0 ? lines.slice(ownerIndex + 1) : lines;
    if (!textLines.length && lines.length > 1) {
      textLines = lines.slice(1);
    }

    return {
      owner,
      text: directText || textLines.join(' ').trim(),
      full: lines.join('\n')
    };
  }).filter((message) => message.owner && message.text);

  installGiftTap();
  if (window.__mergerinoTikTokGiftTap?.queue?.length) {
    result.giftEvents = window.__mergerinoTikTokGiftTap.queue.splice(
      0,
      window.__mergerinoTikTokGiftTap.queue.length
    );
  }
  if (window.__mergerinoTikTokGiftTap?.joinQueue?.length) {
    result.joinEvents = window.__mergerinoTikTokGiftTap.joinQueue.splice(
      0,
      window.__mergerinoTikTokGiftTap.joinQueue.length
    );
  }
  if (window.__mergerinoTikTokGiftTap?.profileQueue?.length) {
    result.profileUpdates = window.__mergerinoTikTokGiftTap.profileQueue.splice(
      0,
      window.__mergerinoTikTokGiftTap.profileQueue.length
    );
  }

  return JSON.stringify(result);
})()
)JS");

    return script;
}

size_t visibleMessageOverlap(const std::vector<BrowserChatMessage> &previous,
                             const std::vector<BrowserChatMessage> &current)
{
    const auto maxOverlap = std::min(previous.size(), current.size());
    for (size_t overlap = maxOverlap; overlap > 0; --overlap)
    {
        bool matches = true;
        for (size_t index = 0; index < overlap; ++index)
        {
            if (!(previous[previous.size() - overlap + index] ==
                  current[index]))
            {
                matches = false;
                break;
            }
        }

        if (matches)
        {
            return overlap;
        }
    }

    return 0;
}

class BrowserDevToolsListener final : public WebSocketListener
{
public:
    BrowserDevToolsListener(std::weak_ptr<bool> lifetime,
                            std::function<void()> onOpen,
                            std::function<void(QByteArray)> onMessage,
                            std::function<void()> onClose)
        : lifetime_(std::move(lifetime))
        , onOpen_(std::move(onOpen))
        , onMessage_(std::move(onMessage))
        , onClose_(std::move(onClose))
    {
    }

    void onOpen() override
    {
        runInGuiThread([lifetime = this->lifetime_, callback = this->onOpen_] {
            if (lifetime.lock())
            {
                callback();
            }
        });
    }

    void onTextMessage(QByteArray data) override
    {
        runInGuiThread(
            [lifetime = this->lifetime_, callback = this->onMessage_,
             data = std::move(data)]() mutable {
                if (lifetime.lock())
                {
                    callback(std::move(data));
                }
            });
    }

    void onBinaryMessage(QByteArray data) override
    {
        this->onTextMessage(std::move(data));
    }

    void onClose(std::unique_ptr<WebSocketListener> /* self */) override
    {
        runInGuiThread([lifetime = this->lifetime_, callback = this->onClose_] {
            if (lifetime.lock())
            {
                callback();
            }
        });
    }

private:
    std::weak_ptr<bool> lifetime_;
    std::function<void()> onOpen_;
    std::function<void(QByteArray)> onMessage_;
    std::function<void()> onClose_;
};

}  // namespace

namespace chatterino {

struct TikTokLiveChat::BrowserSession {
    QString executable;
    QString pageUrl;

    int devToolsPort = 0;
    int targetPollAttempts = 0;
    int nextRequestId = 0;
    int snapshotRequestId = 0;

    bool socketOpen = false;
    bool snapshotInFlight = false;

    std::vector<BrowserChatMessage> visibleMessages;

    std::unique_ptr<QProcess> process;
    std::unique_ptr<QTemporaryDir> profileDir;
    std::unique_ptr<WebSocketPool> socketPool;
    WebSocketHandle socket;
};

TikTokLiveChat::TikTokLiveChat(QString source, bool showJoinMessages)
    : source_(std::move(source))
    , showJoinMessages_(showJoinMessages)
    , lifetimeGuard_(std::make_shared<bool>(true))
{
}

TikTokLiveChat::~TikTokLiveChat()
{
    this->stop();
}

void TikTokLiveChat::setShowJoinMessages(bool enabled)
{
    this->showJoinMessages_ = enabled;
}

void TikTokLiveChat::start()
{
    if (this->running_)
    {
        return;
    }

    if (!this->lifetimeGuard_)
    {
        this->lifetimeGuard_ = std::make_shared<bool>(true);
    }

    this->running_ = true;
    this->browserSnapshotReceived_ = false;
    this->failureReported_ = false;
    this->roomId_.clear();
    this->statusText_.clear();
    this->liveTitle_.clear();
    this->ownerNickname_.clear();
    this->ownerUniqueId_.clear();
    this->joinedRoomId_.clear();
    this->setLive(false);
    this->stopBrowserSession();

    const auto normalized = normalizeSource(this->source_);
    if (normalized.isEmpty())
    {
        this->setStatusText("Enter a TikTok @username or live URL.", true);
        this->failureReported_ = true;
        return;
    }

    if (this->resolvedSource_ != normalized)
    {
        this->resolvedSource_ = normalized;
        this->sourceResolved.invoke(normalized);
    }
    this->source_ = normalized;

    this->setStatusText(
        QString("Resolving TikTok LIVE room for %1.").arg(this->sourceLabel()));
    this->resolveRoom();
}

void TikTokLiveChat::stop()
{
    this->running_ = false;
    this->browserSnapshotReceived_ = false;
    this->stopBrowserSession();
    this->lifetimeGuard_.reset();
}

bool TikTokLiveChat::isLive() const
{
    return this->live_;
}

const QString &TikTokLiveChat::roomId() const
{
    return this->roomId_;
}

const QString &TikTokLiveChat::statusText() const
{
    return this->statusText_;
}

const QString &TikTokLiveChat::liveTitle() const
{
    return this->liveTitle_;
}

const QString &TikTokLiveChat::resolvedSource() const
{
    return this->resolvedSource_;
}

QString TikTokLiveChat::normalizeSource(const QString &source)
{
    return normalizeTikTokSource(source);
}

QString TikTokLiveChat::cachedAvatarUrl(const QString &userName,
                                        const QString &userID)
{
    return cachedTikTokProfile(userName, userID).avatarUrl;
}

void TikTokLiveChat::resolveRoom()
{
    if (!this->running_ || this->resolvedSource_.isEmpty())
    {
        return;
    }

    const auto uniqueId = this->resolvedSource_.mid(1);
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(makeRoomInfoUrl(uniqueId))
        .headerList(tiktokHeaders())
        .followRedirects(true)
        .onSuccess([this, weak](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto json = result.parseJson();
            if (json.value("statusCode").toInt(-1) != 0)
            {
                this->resolveRoomFromHtml();
                return;
            }

            const auto data = json.value("data").toObject();
            const auto user = data.value("user").toObject();
            const auto liveRoom = data.value("liveRoom").toObject();

            auto roomId = user.value("roomId").toString().trimmed();
            if (roomId.isEmpty())
            {
                const auto numericRoomId = parseJsonLongLong(user.value("roomId"));
                if (numericRoomId > 0)
                {
                    roomId = QString::number(numericRoomId);
                }
            }

            const auto uniqueId = user.value("uniqueId").toString().trimmed();
            const auto nickname = user.value("nickname").toString().trimmed();
            const auto liveTitle = liveRoom.value("title").toString().trimmed();
            const auto roomStatus =
                static_cast<int>(parseJsonLongLong(liveRoom.value("status")));

            if (uniqueId.isEmpty() && roomId.isEmpty())
            {
                this->resolveRoomFromHtml();
                return;
            }

            this->applyResolvedRoomInfo(roomId, uniqueId, nickname, liveTitle);
            if (roomStatus == 4)
            {
                this->waitForNextLive(
                    QString("TikTok room API reports %1 is currently not live. "
                            "Rechecking.")
                        .arg(this->sourceLabel()),
                    TIKTOK_WAIT_FOR_LIVE_DELAY_MS);
                return;
            }

            if (this->roomId_.isEmpty())
            {
                this->resolveRoomFromHtml();
                return;
            }

            this->failureReported_ = false;
            this->startBrowserSession();
        })
        .onError([this, weak](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            this->resolveRoomFromHtml();
        })
        .execute();
}

void TikTokLiveChat::resolveRoomFromHtml()
{
    if (!this->running_ || this->resolvedSource_.isEmpty())
    {
        return;
    }

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(makeLivePageUrl(this->resolvedSource_))
        .headerList(tiktokHeaders())
        .followRedirects(true)
        .onSuccess([this, weak](const NetworkResult &result) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            const auto html = QString::fromUtf8(result.getData());
            const auto roomId =
                extractFirstMatch(html, {R"tt("roomId":"([0-9]+)")tt"});
            const auto uniqueId = extractFirstMatch(
                html, {R"tt("uniqueId":"([A-Za-z0-9._]+)")tt"});
            const auto nickname = extractFirstMatch(
                html, {R"tt("nickname":"((?:\\.|[^"])*)")tt"}, true);
            const auto title = extractFirstMatch(
                html, {R"tt("title":"((?:\\.|[^"])*)")tt"}, true);

            if (roomId.isEmpty() && uniqueId.isEmpty())
            {
                this->setStatusText(
                    "Couldn't resolve that TikTok user. Try a username, "
                    "@username, or TikTok live URL.",
                    !this->failureReported_);
                this->failureReported_ = true;
                return;
            }

            this->applyResolvedRoomInfo(roomId, uniqueId, nickname, title);
            if (this->roomId_.isEmpty())
            {
                this->waitForNextLive(
                    QString("TikTok public page did not expose a live room ID "
                            "for %1. Rechecking.")
                        .arg(this->sourceLabel()),
                    TIKTOK_WAIT_FOR_LIVE_DELAY_MS);
                return;
            }

            this->failureReported_ = false;
            this->startBrowserSession();
        })
        .onError([this, weak](NetworkResult) {
            if (!weak.lock() || !this->running_)
            {
                return;
            }

            this->setStatusText("Couldn't resolve the TikTok LIVE room.",
                                !this->failureReported_);
            this->failureReported_ = true;
            this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
        })
        .execute();
}

void TikTokLiveChat::applyResolvedRoomInfo(QString roomId, QString uniqueId,
                                           QString nickname,
                                           QString liveTitle)
{
    if (!uniqueId.isEmpty())
    {
        const auto canonical = normalizeSource(uniqueId);
        if (!canonical.isEmpty() && canonical != this->resolvedSource_)
        {
            this->resolvedSource_ = canonical;
            this->source_ = canonical;
            this->sourceResolved.invoke(canonical);
        }
    }

    this->roomId_ = std::move(roomId);
    this->ownerUniqueId_ =
        !uniqueId.isEmpty() ? uniqueId : this->resolvedSource_.mid(1);
    this->ownerNickname_ = std::move(nickname);
    this->liveTitle_ = std::move(liveTitle);
}

void TikTokLiveChat::startBrowserSession()
{
    if (!this->running_)
    {
        return;
    }

    this->stopBrowserSession();
    this->browserSnapshotReceived_ = false;

    auto browser = std::make_unique<BrowserSession>();
    browser->executable = findChromiumBrowserExecutable();
    if (browser->executable.isEmpty())
    {
        this->setStatusText(
            "TikTok LIVE chat requires a local Chromium-based browser "
            "(Edge/Chrome/Brave/Chromium).",
            !this->failureReported_);
        this->failureReported_ = true;
        return;
    }

    browser->devToolsPort = allocateLocalPort();
    if (browser->devToolsPort <= 0)
    {
        this->setStatusText("Couldn't allocate a local browser debugging port.",
                            !this->failureReported_);
        this->failureReported_ = true;
        this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
        return;
    }

    browser->profileDir = std::make_unique<QTemporaryDir>(
        QDir::tempPath() + "/MergerinoTikTokXXXXXX");
    if (!browser->profileDir->isValid())
    {
        this->setStatusText(
            "Couldn't create a temporary browser profile for TikTok LIVE.",
            !this->failureReported_);
        this->failureReported_ = true;
        this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
        return;
    }

    browser->pageUrl = makeLivePageUrl(this->resolvedSource_).toString();
    browser->process = std::make_unique<QProcess>();
    browser->process->setProgram(browser->executable);
    browser->process->setArguments(buildBrowserArguments(
        browser->pageUrl, browser->devToolsPort, browser->profileDir->path()));
    browser->process->setProcessChannelMode(QProcess::MergedChannels);

    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QObject::connect(
        browser->process.get(),
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
            &QProcess::finished),
        [this, weak](int, QProcess::ExitStatus) {
            if (!weak.lock() || !this->running_ || !this->browser_)
            {
                return;
            }

            this->setLive(false);
            this->setStatusText(
                "TikTok LIVE browser session ended unexpectedly. Reconnecting.",
                !this->failureReported_);
            this->failureReported_ = true;
            this->stopBrowserSession();
            this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
        });
    QObject::connect(browser->process.get(), &QProcess::errorOccurred,
                     [this, weak](QProcess::ProcessError error) {
                         if (!weak.lock() || !this->running_ || !this->browser_)
                         {
                             return;
                         }

                         if (error == QProcess::FailedToStart)
                         {
                             this->setStatusText(
                                 "Couldn't start the local browser reader for "
                                 "TikTok LIVE.",
                                 !this->failureReported_);
                             this->failureReported_ = true;
                         }
                     });

    browser->process->start();
    this->browser_ = std::move(browser);
    this->setStatusText(
        QString("Launching local browser reader for TikTok LIVE on %1.")
            .arg(this->sourceLabel()));
    this->scheduleBrowserTargetCheck(TIKTOK_BROWSER_TARGET_POLL_DELAY_MS);
}

void TikTokLiveChat::stopBrowserSession()
{
    if (!this->browser_)
    {
        return;
    }

    this->browser_->socket.close();
    if (this->browser_->process &&
        this->browser_->process->state() != QProcess::NotRunning)
    {
        this->browser_->process->terminate();
        if (!this->browser_->process->waitForFinished(1500))
        {
            this->browser_->process->kill();
            this->browser_->process->waitForFinished(1500);
        }
    }

    this->browser_.reset();
}

void TikTokLiveChat::ensureBrowserTarget()
{
    if (!this->running_ || !this->browser_)
    {
        return;
    }

    const auto devToolsPort = this->browser_->devToolsPort;
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    NetworkRequest(QUrl(
        QString("http://127.0.0.1:%1/json/list").arg(devToolsPort)))
        .timeout(2000)
        .onSuccess([this, weak](const NetworkResult &result) {
            if (!weak.lock() || !this->running_ || !this->browser_)
            {
                return;
            }

            const auto targets = result.parseJsonArray();
            const auto debuggerUrl =
                chooseBrowserTarget(targets, this->resolvedSource_);
            if (!debuggerUrl.isValid())
            {
                if (++this->browser_->targetPollAttempts >=
                    TIKTOK_BROWSER_TARGET_POLL_LIMIT)
                {
                    this->setStatusText(
                        "Timed out waiting for the TikTok LIVE browser page.",
                        !this->failureReported_);
                    this->failureReported_ = true;
                    this->stopBrowserSession();
                    this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
                    return;
                }

                this->scheduleBrowserTargetCheck(
                    TIKTOK_BROWSER_TARGET_POLL_DELAY_MS);
                return;
            }

            this->failureReported_ = false;
            this->connectBrowserTarget(debuggerUrl);
        })
        .onError([this, weak](NetworkResult) {
            if (!weak.lock() || !this->running_ || !this->browser_)
            {
                return;
            }

            if (++this->browser_->targetPollAttempts >=
                TIKTOK_BROWSER_TARGET_POLL_LIMIT)
            {
                this->setStatusText(
                    "Couldn't attach to the local TikTok LIVE browser page.",
                    !this->failureReported_);
                this->failureReported_ = true;
                this->stopBrowserSession();
                this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
                return;
            }

            this->scheduleBrowserTargetCheck(
                TIKTOK_BROWSER_TARGET_POLL_DELAY_MS);
        })
        .execute();
}

void TikTokLiveChat::connectBrowserTarget(const QUrl &devtoolsUrl)
{
    if (!this->running_ || !this->browser_)
    {
        return;
    }

    this->browser_->socketPool =
        std::make_unique<WebSocketPool>("TikTokLiveChat");
    this->browser_->socket = this->browser_->socketPool->createSocket(
        {.url = devtoolsUrl},
        std::make_unique<BrowserDevToolsListener>(
            std::weak_ptr<bool>(this->lifetimeGuard_),
            [this] { this->handleBrowserSocketOpened(); },
            [this](QByteArray data) {
                this->handleBrowserSocketMessage(std::move(data));
            },
            [this] { this->handleBrowserSocketClosed(); }));

    this->setStatusText(
        QString("Connecting to the TikTok LIVE browser session for %1.")
            .arg(this->sourceLabel()));
}

void TikTokLiveChat::requestBrowserSnapshot()
{
    if (!this->running_ || !this->browser_ || !this->browser_->socketOpen ||
        this->browser_->snapshotInFlight)
    {
        return;
    }

    this->browser_->snapshotInFlight = true;
    this->browser_->snapshotRequestId = ++this->browser_->nextRequestId;

    QJsonObject params;
    params.insert("expression", browserSnapshotScript());
    params.insert("returnByValue", true);
    params.insert("awaitPromise", false);

    QJsonObject command;
    command.insert("id", this->browser_->snapshotRequestId);
    command.insert("method", "Runtime.evaluate");
    command.insert("params", params);

    this->browser_->socket.sendText(
        QJsonDocument(command).toJson(QJsonDocument::Compact));
}

void TikTokLiveChat::handleBrowserSocketOpened()
{
    if (!this->running_ || !this->browser_)
    {
        return;
    }

    this->browser_->socketOpen = true;
    this->browser_->snapshotInFlight = false;
    this->failureReported_ = false;
    this->setStatusText(
        QString("Connected to the TikTok LIVE browser session for %1. Reading "
                "chat.")
            .arg(this->sourceLabel()));
    this->scheduleBrowserSnapshot(1000);
}

void TikTokLiveChat::handleBrowserSocketClosed()
{
    if (!this->running_ || !this->browser_)
    {
        return;
    }

    this->setLive(false);
    this->setStatusText(
        "TikTok LIVE browser debugging connection closed. Reconnecting.",
        !this->failureReported_);
    this->failureReported_ = true;
    this->stopBrowserSession();
    this->scheduleResolve(TIKTOK_RECONNECT_DELAY_MS);
}

void TikTokLiveChat::handleBrowserSocketMessage(QByteArray data)
{
    if (!this->running_ || !this->browser_)
    {
        return;
    }

    const auto document = QJsonDocument::fromJson(data);
    if (!document.isObject())
    {
        return;
    }

    const auto root = document.object();
    if (root.value("id").toInt() != this->browser_->snapshotRequestId)
    {
        return;
    }

    this->browser_->snapshotInFlight = false;
    this->browser_->snapshotRequestId = 0;

    if (root.contains("error"))
    {
        this->setStatusText(
            "TikTok LIVE browser session rejected a chat snapshot request. "
            "Retrying.",
            !this->failureReported_);
        this->failureReported_ = true;
        this->scheduleBrowserSnapshot(TIKTOK_BROWSER_SNAPSHOT_DELAY_MS);
        return;
    }

    const auto resultObject = root.value("result").toObject();
    const auto evaluationResult = resultObject.value("result").toObject();
    const auto payload = evaluationResult.value("value").toString().toUtf8();
    if (payload.isEmpty())
    {
        this->setStatusText(
            "TikTok LIVE browser session returned an empty chat snapshot. "
            "Retrying.",
            !this->failureReported_);
        this->failureReported_ = true;
        this->scheduleBrowserSnapshot(TIKTOK_BROWSER_SNAPSHOT_DELAY_MS);
        return;
    }

    this->handleBrowserSnapshot(payload);
}

void TikTokLiveChat::handleBrowserSnapshot(const QByteArray &payload)
{
    if (!this->running_ || !this->browser_)
    {
        return;
    }

    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject())
    {
        this->setStatusText(
            "TikTok LIVE browser snapshot could not be decoded. Retrying.",
            !this->failureReported_);
        this->failureReported_ = true;
        this->scheduleBrowserSnapshot(TIKTOK_BROWSER_SNAPSHOT_DELAY_MS);
        return;
    }

    const auto root = document.object();
    const auto roomId = root.value("roomId").toString().trimmed();
    const auto ownerUniqueId = root.value("ownerUniqueId").toString().trimmed();
    const auto ownerNickname = root.value("ownerNickname").toString().trimmed();
    const auto liveTitle = root.value("liveTitle").toString().trimmed();
    const auto liveRoomStatus =
        static_cast<int>(parseJsonLongLong(root.value("liveRoomStatus")));
    const auto showLiveChat = root.value("showLiveChat").toBool();
    const auto enableChat = root.value("enableChat").toBool();

    this->applyResolvedRoomInfo(roomId, ownerUniqueId, ownerNickname, liveTitle);

    if (liveRoomStatus == 4)
    {
        this->stopBrowserSession();
        this->waitForNextLive(
            QString("TikTok page reports %1 is currently not live. "
                    "Rechecking.")
                .arg(this->sourceLabel()),
            TIKTOK_WAIT_FOR_LIVE_DELAY_MS);
        return;
    }

    const auto profileUpdateArray = root.value("profileUpdates").toArray();
    for (const auto &value : profileUpdateArray)
    {
        const auto object = value.toObject();
        rememberTikTokProfile({
            .displayName =
                object.value("displayName").toString().trimmed(),
            .uniqueId = object.value("uniqueId").toString().trimmed(),
            .avatarUrl = object.value("avatarUrl").toString().trimmed(),
        });
    }

    std::vector<BrowserChatMessage> currentMessages;
    const auto messageArray = root.value("messages").toArray();
    currentMessages.reserve(messageArray.size());
    for (const auto &value : messageArray)
    {
        const auto object = value.toObject();
        const auto owner = object.value("owner").toString().trimmed();
        const auto text = object.value("text").toString().trimmed();
        if (owner.isEmpty() || text.isEmpty())
        {
            continue;
        }

        currentMessages.push_back({owner, text,
                                   object.value("full").toString().trimmed()});
    }

    std::vector<BrowserGiftEvent> giftEvents;
    const auto giftEventArray = root.value("giftEvents").toArray();
    giftEvents.reserve(giftEventArray.size());
    for (const auto &value : giftEventArray)
    {
        const auto object = value.toObject();
        BrowserGiftEvent event;
        event.id = object.value("id").toString().trimmed();
        event.senderDisplayName =
            object.value("senderDisplayName").toString().trimmed();
        event.senderUniqueId =
            object.value("senderUniqueId").toString().trimmed();
        event.recipientDisplayName =
            object.value("recipientDisplayName").toString().trimmed();
        event.giftName = object.value("giftName").toString().trimmed();
        event.quantity = std::max<uint32_t>(
            1, parseJsonUInt(object.value("quantity")));
        event.diamondCount = parseJsonUInt(object.value("diamondCount"));
        if (event.senderDisplayName.isEmpty() && event.senderUniqueId.isEmpty())
        {
            continue;
        }

        giftEvents.push_back(std::move(event));
    }

    std::vector<BrowserJoinEvent> joinEvents;
    if (this->showJoinMessages_)
    {
        const auto joinEventArray = root.value("joinEvents").toArray();
        joinEvents.reserve(joinEventArray.size());
        for (const auto &value : joinEventArray)
        {
            const auto object = value.toObject();
            BrowserJoinEvent event;
            event.id = object.value("id").toString().trimmed();
            event.displayName =
                object.value("displayName").toString().trimmed();
            event.uniqueId = object.value("uniqueId").toString().trimmed();
            if (event.displayName.isEmpty() && event.uniqueId.isEmpty())
            {
                continue;
            }

            joinEvents.push_back(std::move(event));
        }
    }

    const bool firstSnapshot = !this->browserSnapshotReceived_;
    this->browserSnapshotReceived_ = true;
    this->failureReported_ = false;
    this->setLive(!this->roomId_.isEmpty());

    if (!this->roomId_.isEmpty() && this->joinedRoomId_ != this->roomId_)
    {
        this->joinedRoomId_ = this->roomId_;
        const auto joinedText =
            this->liveTitle_.trimmed().isEmpty()
                ? QString("Joined TikTok LIVE chat")
                : QString("Joined TikTok LIVE chat: %1")
                      .arg(this->liveTitle_.trimmed());
        this->systemMessageReceived.invoke(makeSystemStatusMessage(joinedText));
    }

    size_t emitIndex = 0;
    if (firstSnapshot)
    {
        emitIndex = currentMessages.size() > 20 ? currentMessages.size() - 20 : 0;
    }
    else
    {
        const auto overlap =
            visibleMessageOverlap(this->browser_->visibleMessages, currentMessages);
        if (!this->browser_->visibleMessages.empty() && overlap == 0)
        {
            emitIndex = currentMessages.empty() ? 0 : currentMessages.size() - 1;
        }
        else
        {
            emitIndex = overlap;
        }
    }

    const auto channelName = !this->ownerUniqueId_.isEmpty()
                                 ? this->ownerUniqueId_
                                 : this->resolvedSource_.mid(1);
    for (const auto &event : joinEvents)
    {
        auto message = makeJoinMessage(event, channelName);
        if (message)
        {
            this->messageReceived.invoke(message);
        }
    }

    for (const auto &event : giftEvents)
    {
        auto message = makeGiftMessage(event, channelName);
        if (message)
        {
            this->messageReceived.invoke(message);
        }
    }

    for (size_t index = emitIndex; index < currentMessages.size(); ++index)
    {
        auto message = makeChatMessage(currentMessages[index], channelName);
        if (message)
        {
            this->messageReceived.invoke(message);
        }
    }

    this->browser_->visibleMessages = std::move(currentMessages);

    if (this->liveTitle_.trimmed().isEmpty())
    {
        if (showLiveChat || enableChat || !this->browser_->visibleMessages.empty())
        {
            this->setStatusText(
                QString("Watching TikTok LIVE chat for %1")
                    .arg(this->sourceLabel()));
        }
        else
        {
            this->setStatusText(
                QString("Connected to TikTok LIVE for %1. Waiting for chat "
                        "activity.")
                    .arg(this->sourceLabel()));
        }
    }
    else
    {
        this->setStatusText(
            QString("Watching TikTok LIVE chat for %1")
                .arg(this->liveTitle_.trimmed()));
    }

    this->scheduleBrowserSnapshot(TIKTOK_BROWSER_SNAPSHOT_DELAY_MS);
}

void TikTokLiveChat::scheduleBrowserTargetCheck(int delayMs)
{
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QTimer::singleShot(delayMs, [this, weak] {
        if (!weak.lock() || !this->running_)
        {
            return;
        }

        this->ensureBrowserTarget();
    });
}

void TikTokLiveChat::scheduleBrowserSnapshot(int delayMs)
{
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QTimer::singleShot(delayMs, [this, weak] {
        if (!weak.lock() || !this->running_)
        {
            return;
        }

        this->requestBrowserSnapshot();
    });
}

void TikTokLiveChat::scheduleResolve(int delayMs)
{
    auto weak = std::weak_ptr<bool>(this->lifetimeGuard_);
    QTimer::singleShot(delayMs, [this, weak] {
        if (!weak.lock() || !this->running_)
        {
            return;
        }

        this->resolveRoom();
    });
}

void TikTokLiveChat::waitForNextLive(QString text, int retryDelayMs)
{
    this->setLive(false);
    this->roomId_.clear();
    this->liveTitle_.clear();
    this->browserSnapshotReceived_ = false;
    this->stopBrowserSession();
    this->setStatusText(std::move(text));
    this->scheduleResolve(retryDelayMs);
}

void TikTokLiveChat::setLive(bool live)
{
    if (this->live_ == live)
    {
        return;
    }

    this->live_ = live;
    this->liveStatusChanged.invoke();
}

void TikTokLiveChat::setStatusText(QString text, bool notifyAsSystemMessage)
{
    if (this->statusText_ == text)
    {
        return;
    }

    this->statusText_ = std::move(text);
    this->liveStatusChanged.invoke();

    if (notifyAsSystemMessage)
    {
        this->systemMessageReceived.invoke(
            makeSystemStatusMessage(this->statusText_));
    }
}

QString TikTokLiveChat::sourceLabel() const
{
    if (!this->resolvedSource_.isEmpty())
    {
        return this->resolvedSource_;
    }

    const auto normalized = normalizeSource(this->source_);
    return normalized.isEmpty() ? this->source_.trimmed() : normalized;
}

}  // namespace chatterino
