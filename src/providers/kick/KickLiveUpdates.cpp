#include "providers/kick/KickLiveUpdates.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/liveupdates/BasicPubSubClient.hpp"
#include "providers/liveupdates/BasicPubSubManager.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json.hpp>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>

#include <charconv>
#include <functional>
#include <optional>
#include <utility>

using namespace Qt::Literals;

namespace {

constexpr std::chrono::seconds MAX_HEARTBEAT_INTERVAL{20};

// Kick's chat runs on a Pusher app whose key Kick can rotate (it did in
// Oct 2026). The current key is looked up from Kick once per session and
// again whenever Pusher rejects the one in use; this is the last known key,
// used until a lookup succeeds.
const QString FALLBACK_APP_KEY = u"34bf7a0ff419a2a775b9"_s;
const QString FALLBACK_CLUSTER = u"us2"_s;
const QString REALTIME_CONFIG_URL =
    u"https://web.kick.com/api/v1/realtime/connection"_s;
// However often the key is rejected, Kick is asked at most this often
constexpr std::chrono::minutes MIN_CONFIG_LOOKUP_INTERVAL{1};

QString pusherUrl(const QString &appKey, const QString &cluster)
{
    return u"wss://ws-" % cluster % u".pusher.com/app/" % appKey %
           u"?protocol=7&client=js&version=8.4.0&flash=false";
}

bool isValidAppKey(const QString &key)
{
    static const QRegularExpression pattern(u"^[A-Za-z0-9]{8,64}$"_s);
    return pattern.match(key).hasMatch();
}

bool isValidCluster(const QString &cluster)
{
    static const QRegularExpression pattern(u"^[a-z0-9-]{2,32}$"_s);
    return pattern.match(cluster).hasMatch();
}

bool stripPrefix(std::string_view &str, std::string_view prefix)
{
    if (str.starts_with(prefix))
    {
        str = str.substr(prefix.size());
        return true;
    }
    return false;
}

bool stripSuffix(std::string_view &str, std::string_view suffix)
{
    if (str.ends_with(suffix))
    {
        str = str.substr(0, str.size() - suffix.size());
        return true;
    }
    return false;
}

struct IDs {
    uint64_t roomID = 0;
    uint64_t channelID = 0;
};

IDs parseIDs(std::string_view channel)
{
    bool isChannel = false;
    if (stripPrefix(channel, "chatrooms.") || stripPrefix(channel, "chatroom_"))
    {
        stripSuffix(channel, ".v2");
    }
    else if (stripPrefix(channel, "channel_") ||
             stripPrefix(channel, "channel.") ||
             stripPrefix(channel, "predictions-channel-"))
    {
        isChannel = true;
    }

    uint64_t v = 0;
    std::from_chars(channel.data(), channel.data() + channel.size(), v);

    if (isChannel)
    {
        return IDs{.channelID = v};
    }
    return IDs{.roomID = v};
}

}  // namespace

namespace chatterino {

class KickLiveUpdatesClient
    : public BasicPubSubClient<QString, KickLiveUpdatesClient>,
      public std::enable_shared_from_this<KickLiveUpdatesClient>
{
public:
    KickLiveUpdatesClient(QPointer<KickChatServer> chatServer)
        : BasicPubSubClient(100)
        , lastHeartbeat_(std::chrono::steady_clock::now())
        , heartbeatInterval_(MAX_HEARTBEAT_INTERVAL)
        , chatServer_(std::move(chatServer))
    {
    }

    void onOpen() /* override */
    {
        BasicPubSubClient::onOpen();
        this->lastHeartbeat_ = std::chrono::steady_clock::now();
    }

    void onMessage(const QByteArray &msg);

    std::chrono::steady_clock::time_point lastHeartbeat() const
    {
        return this->lastHeartbeat_;
    }

    std::chrono::milliseconds heartbeatInterval() const
    {
        return this->heartbeatInterval_;
    }

    void checkHeartbeat();

    /// Pusher refused the app key, e.g. because Kick moved to a new one
    std::function<void()> onAppRejected;

    QByteArray encodeSubscription(const Subscription &subscription);
    QByteArray encodeUnsubscription(const Subscription &subscription);

private:
    void onMessageUi(const QByteArray &msg);

    std::chrono::steady_clock::time_point lastHeartbeat_;
    std::chrono::milliseconds heartbeatInterval_;
    QPointer<KickChatServer> chatServer_;
};

void KickLiveUpdatesClient::onMessage(const QByteArray &msg)
{
    runInGuiThread([weak = this->weak_from_this(), msg] {
        auto self = weak.lock();
        if (self)
        {
            self->onMessageUi(msg);
        }
    });
}

void KickLiveUpdatesClient::onMessageUi(const QByteArray &msg)
{
    boost::system::error_code ec;
    auto rootJv =
        boost::json::parse(std::string_view(msg.data(), msg.size()), ec);
    if (ec)
    {
        qCWarning(chatterinoKick) << "Failed to parse message:" << ec.message();
        return;
    }
    BoostJsonValue rootRef(rootJv);
    auto rootObj = rootRef.toObject();
    auto event = rootObj["event"].toStringView();

    auto dataStr = rootObj["data"].toStringView();
    BoostJsonValue data;
    boost::json::value dataJv;
    if (!dataStr.empty() && dataStr != "{}")
    {
        dataJv = boost::json::parse(dataStr, ec);
        data = BoostJsonValue(dataJv);
    }

    if (event == "pusher:pong")
    {
        this->lastHeartbeat_ = std::chrono::steady_clock::now();
    }
    else if (event == "pusher_internal:subscription_succeeded")
    {
        auto channel = rootObj["channel"].toStdString();
        // that's the main chat subscription
        if (channel.starts_with("chatrooms.") && channel.ends_with(".v2"))
        {
            auto ids = parseIDs(channel);
            if (this->chatServer_ && ids.roomID > 0)
            {
                this->chatServer_->onJoin(ids.roomID);
            }
        }
    }
    else if (event == "pusher:subscription_error")
    {
        qCWarning(chatterinoKick) << "Failed to subscribe" << msg;
    }
    else if (event == "pusher:error")
    {
        qCWarning(chatterinoKick) << "Pusher error" << msg;
        // 4000-4099 mean "don't come back unchanged", which is what a
        // rotated app key looks like (4001: app key not in this cluster)
        auto rawData = rootObj["data"];
        auto code = rawData.isObject() ? rawData["code"].toInt64()
                                       : data["code"].toInt64();
        if (code >= 4000 && code < 4100)
        {
            this->markFailed();
            if (this->onAppRejected)
            {
                this->onAppRejected();
            }
        }
    }
    else if (event == "pusher:connection_established")
    {
        std::chrono::seconds activityTimeout{
            data["activity_timeout"].toInt64()};
        if (activityTimeout.count() > 2 &&
            activityTimeout < this->heartbeatInterval_)
        {
            this->heartbeatInterval_ = activityTimeout;
        }
    }
    else
    {
        bool isApp = stripPrefix(event, "App\\Events\\");

        auto channel = rootObj["channel"].toStringView();
        auto ids = parseIDs(channel);
        if (this->chatServer_ && (ids.roomID > 0 || ids.channelID > 0))
        {
            bool handled = this->chatServer_->onAppEvent(
                ids.roomID, ids.channelID, event, data.toObject());
            if (!handled)
            {
                qCWarning(chatterinoKick).noquote()
                    << "Unknown event" << event << "isApp:" << isApp
                    << "channel:" << rootObj["channel"].toStringView()
                    << "data:" << dataStr;
            }
        }
    }
}

void KickLiveUpdatesClient::checkHeartbeat()
{
    if (!this->isOpen())
    {
        return;
    }

    if ((std::chrono::steady_clock::now() - this->lastHeartbeat_) >
        this->heartbeatInterval_ * 1.5)
    {
        qCDebug(chatterinoKick) << "Heartbeat timed out";
        this->close();
    }

    this->sendText(R"({"event":"pusher:ping","data":0})"_ba);
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
QByteArray KickLiveUpdatesClient::encodeSubscription(const Subscription &sub)
{
    return QByteArray::fromStdString(boost::json::serialize(boost::json::object{
        {"event", "pusher:subscribe"},
        {"data",
         boost::json::object{
             {"auth", ""},
             {"channel", sub.toStdString()},
         }},
    }));
}

QByteArray KickLiveUpdatesClient::encodeUnsubscription(const Subscription &sub)
{
    return QByteArray::fromStdString(boost::json::serialize(boost::json::object{
        {"event", "pusher:unsubscribe"},
        {"data",
         boost::json::object{
             {"channel", sub.toStdString()},
         }},
    }));
}
// NOLINTEND(readability-convert-member-functions-to-static)

class KickLiveUpdatesPrivate
    : public BasicPubSubManager<KickLiveUpdatesPrivate, KickLiveUpdatesClient>
{
public:
    KickLiveUpdatesPrivate();
    ~KickLiveUpdatesPrivate() override;

    Q_DISABLE_COPY_MOVE(KickLiveUpdatesPrivate);

    std::shared_ptr<KickLiveUpdatesClient> makeClient();
    void checkHeartbeats();
    void reconnect();
    void lookUpAppKey();
    void applyRealtimeConfig(const QJsonObject &root);

    std::chrono::milliseconds heartbeatInterval = MAX_HEARTBEAT_INTERVAL;
    QTimer heartbeatTimer;

    QString appKey = FALLBACK_APP_KEY;
    QString cluster = FALLBACK_CLUSTER;
    bool lookingUpAppKey = false;
    std::optional<std::chrono::steady_clock::time_point> lastAppKeyLookup;

private:
    friend KickLiveUpdates;
};

KickLiveUpdatesPrivate::KickLiveUpdatesPrivate()
    : BasicPubSubManager(pusherUrl(FALLBACK_APP_KEY, FALLBACK_CLUSTER), "kick")
{
    QObject::connect(&this->heartbeatTimer, &QTimer::timeout, this,
                     &KickLiveUpdatesPrivate::checkHeartbeats);
    this->heartbeatTimer.setInterval(this->heartbeatInterval);
    this->heartbeatTimer.setSingleShot(false);
    this->heartbeatTimer.start();
}

KickLiveUpdatesPrivate::~KickLiveUpdatesPrivate()
{
    this->stop();
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::shared_ptr<KickLiveUpdatesClient> KickLiveUpdatesPrivate::makeClient()
{
    auto client = std::make_shared<KickLiveUpdatesClient>(
        getApp()->getKickChatServer());
    client->onAppRejected = [self = QPointer<KickLiveUpdatesPrivate>(this)] {
        if (self)
        {
            self->lookUpAppKey();
        }
    };
    return client;
}

void KickLiveUpdatesPrivate::lookUpAppKey()
{
    if (getApp()->isTest() || this->lookingUpAppKey)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (this->lastAppKeyLookup &&
        now - *this->lastAppKeyLookup < MIN_CONFIG_LOOKUP_INTERVAL)
    {
        return;
    }
    this->lookingUpAppKey = true;
    this->lastAppKeyLookup = now;

    // Same request Kick's website makes, but only offering Pusher so Kick
    // answers with the Pusher app it wants clients on
    const QJsonObject body{
        {"client",
         QJsonObject{
             {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
             {"type", "web"},
         }},
        {"capabilities",
         QJsonObject{
             {"accepted_providers",
              QJsonArray{QJsonObject{{"provider", "pusher"}}}},
         }},
    };
    NetworkRequest(QUrl(REALTIME_CONFIG_URL), NetworkRequestType::Post)
        .caller(this)
        .headerList({
            {"User-Agent",
             "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
            {"Accept", "application/json"},
            {"Origin", "https://kick.com"},
            {"Referer", "https://kick.com/"},
        })
        .json(body)
        .timeout(15000)
        .onError([this](const NetworkResult &result) {
            this->lookingUpAppKey = false;
            qCWarning(chatterinoKick)
                << "Failed to look up Kick's chat key:" << result.formatError();
        })
        .onSuccess([this](const NetworkResult &result) {
            this->lookingUpAppKey = false;
            this->applyRealtimeConfig(result.parseJson());
        })
        .execute();
}

void KickLiveUpdatesPrivate::applyRealtimeConfig(const QJsonObject &root)
{
    const auto connections = root.value("data")
                                 .toObject()
                                 .value("connections")
                                 .toArray();
    for (const auto &value : connections)
    {
        const auto connection = value.toObject();
        if (connection.value("provider").toString() != u"pusher")
        {
            continue;
        }
        const auto credentials = connection.value("credentials").toObject();
        const auto key = credentials.value("app_key").toString();
        const auto newCluster = credentials.value("cluster").toString();
        if (!isValidAppKey(key) || !isValidCluster(newCluster))
        {
            continue;
        }
        if (key == this->appKey && newCluster == this->cluster)
        {
            return;
        }

        qCInfo(chatterinoKick).nospace()
            << "Kick's chat key changed to " << key << " (" << newCluster
            << "), reconnecting";
        this->appKey = key;
        this->cluster = newCluster;
        this->setHost(pusherUrl(key, newCluster));
        // open sockets come back on the new address once they close
        this->reconnect();
        return;
    }
    qCWarning(chatterinoKick) << "Kick did not offer a Pusher chat connection";
}

void KickLiveUpdatesPrivate::checkHeartbeats()
{
    auto minInterval = std::chrono::milliseconds::max();
    for (const auto &[id, client] : this->clients())
    {
        client->checkHeartbeat();
        minInterval = std::min(minInterval, client->heartbeatInterval());
    }
    if (minInterval != std::chrono::milliseconds::max())
    {
        this->heartbeatInterval = minInterval;
        this->heartbeatTimer.setInterval(this->heartbeatInterval);
    }
}

void KickLiveUpdatesPrivate::reconnect()
{
    for (const auto &[id, client] : this->clients())
    {
        client->close();
    }
}

KickLiveUpdates::KickLiveUpdates()
    : private_(new KickLiveUpdatesPrivate)
{
}
KickLiveUpdates::~KickLiveUpdates() = default;

void KickLiveUpdates::joinRoom(uint64_t roomID, uint64_t channelID)
{
    // Check the key once per session, when the first Kick chat opens
    if (!this->private_->lastAppKeyLookup)
    {
        this->private_->lookUpAppKey();
    }
    this->private_->subscribe(u"chatroom_" % QString::number(roomID));
    this->private_->subscribe(u"chatrooms." % QString::number(roomID));
    this->private_->subscribe(u"chatrooms." % QString::number(roomID) % u".v2");
    this->private_->subscribe(u"channel." % QString::number(channelID));
    this->private_->subscribe(u"channel_" % QString::number(channelID));
    this->private_->subscribe(u"predictions-channel-" %
                              QString::number(channelID));
}

void KickLiveUpdates::leaveRoom(uint64_t roomID, uint64_t channelID)
{
    this->private_->unsubscribe(u"chatroom_" % QString::number(roomID));
    this->private_->unsubscribe(u"chatrooms." % QString::number(roomID));
    this->private_->unsubscribe(u"chatrooms." % QString::number(roomID) %
                                u".v2");
    this->private_->unsubscribe(u"channel." % QString::number(channelID));
    this->private_->unsubscribe(u"channel_" % QString::number(channelID));
    this->private_->unsubscribe(u"predictions-channel-" %
                                QString::number(channelID));
}

void KickLiveUpdates::reconnect()
{
    this->private_->reconnect();
}

}  // namespace chatterino
