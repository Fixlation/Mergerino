// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeLiveChat.hpp"

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>

#include <array>
#include <vector>

namespace chatterino {

struct YouTubeLiveChatTestAccess {
    static QString continuation(const QJsonObject &renderer)
    {
        return YouTubeLiveChat::extractLiveChatContinuation(renderer);
    }

    static QString switchToLiveChat(const QJsonObject &renderer)
    {
        return YouTubeLiveChat::extractLiveChatViewContinuation(renderer, true);
    }

    static void probeInParallel(YouTubeLiveChat::LiveVideoProbe first,
                                YouTubeLiveChat::LiveVideoProbe second,
                                std::function<void(QString)> callback)
    {
        YouTubeLiveChat::probeLiveVideoIdInParallel(
            std::move(first), std::move(second), std::move(callback));
    }

    static void prepareDiscovery(YouTubeLiveChat &chat, QString videoId = {})
    {
        chat.running_ = true;
        chat.videoId_ = std::move(videoId);
    }

    static std::function<bool()> discoveryGuard(const YouTubeLiveChat &chat)
    {
        return chat.discoveryGuard();
    }

    static void sessionStarted(YouTubeLiveChat &chat)
    {
        chat.invalidateLiveChatSession();
    }

    static int retryDelay(const YouTubeLiveChat &chat)
    {
        return chat.liveDiscoveryRetryDelayMs();
    }

    static void cacheContext(YouTubeLiveChat &chat, const QString &html)
    {
        chat.cacheInnertubeContextFromHtml(html);
    }

    static QStringList context(const YouTubeLiveChat &chat)
    {
        return {chat.apiKey_, chat.clientVersion_, chat.visitorData_};
    }

    static void bootstrap(YouTubeLiveChat &chat, std::function<void()> onReady)
    {
        chat.bootstrapInnertubeContext(std::move(onReady), "Bootstrap failed");
    }

    static QString streamsVideoId(const QString &html)
    {
        return YouTubeLiveChat::extractLiveVideoId(html, false);
    }
};

}  // namespace chatterino

using namespace chatterino;

namespace {

struct PendingDiscovery {
    std::array<std::function<void(QString)>, 2> replies;
    std::vector<QString> results;

    PendingDiscovery()
    {
        YouTubeLiveChatTestAccess::probeInParallel(
            [this](auto callback) {
                this->replies[0] = std::move(callback);
            },
            [this](auto callback) {
                this->replies[1] = std::move(callback);
            },
            [this](QString videoId) {
                this->results.push_back(std::move(videoId));
            });
    }
};

const QString DISCOVERY_CONTEXT = QStringLiteral(
    R"({"INNERTUBE_API_KEY":"test-key","INNERTUBE_CLIENT_VERSION":"test-version","VISITOR_DATA":"test-visitor"})");

QJsonObject reload(const QString &token)
{
    return {{"reloadContinuationData", QJsonObject{{"continuation", token}}}};
}

QJsonObject chatRenderer(bool liveChatSelected = false,
                         QString liveChatToken = "all-chat")
{
    // YouTube's selector uses Top chat first and Live Chat second. Labels can
    // be localized, so selection must not depend on their English spelling.
    const QJsonArray items{
        QJsonObject{{"title", "Principais mensagens"},
                    {"selected", !liveChatSelected},
                    {"continuation", reload("top-chat")}},
        QJsonObject{{"title", "Chat ao vivo"},
                    {"selected", liveChatSelected},
                    {"continuation", reload(liveChatToken)}},
    };
    return {
        {"continuations", QJsonArray{reload("default-chat")}},
        {"header",
         QJsonObject{{"liveChatHeaderRenderer",
                      QJsonObject{{"viewSelector",
                                   QJsonObject{{"sortFilterSubMenuRenderer",
                                                QJsonObject{{"subMenuItems",
                                                             items}}}}}}}}},
    };
}

}  // namespace

TEST(YouTubeLiveChat, InitializesChatBeforeSwitchingToUnfilteredView)
{
    EXPECT_EQ(YouTubeLiveChatTestAccess::continuation(chatRenderer()),
              "default-chat");
    EXPECT_EQ(YouTubeLiveChatTestAccess::switchToLiveChat(chatRenderer()),
              "all-chat");
}

TEST(YouTubeLiveChat, DoesNotInitializeFromWatchPageSelectorTokens)
{
    auto renderer = chatRenderer();
    renderer.remove("continuations");
    EXPECT_TRUE(YouTubeLiveChatTestAccess::continuation(renderer).isEmpty());
}

TEST(YouTubeLiveChat, SwitchesFilteredPollingResponseToLiveChat)
{
    EXPECT_EQ(YouTubeLiveChatTestAccess::switchToLiveChat(chatRenderer()),
              "all-chat");
}

TEST(YouTubeLiveChat, DoesNotReloadAnAlreadyUnfilteredFeed)
{
    EXPECT_TRUE(YouTubeLiveChatTestAccess::switchToLiveChat(chatRenderer(true))
                    .isEmpty());
}

TEST(YouTubeLiveChat, CanSwitchAgainAfterSessionRefresh)
{
    EXPECT_EQ(YouTubeLiveChatTestAccess::switchToLiveChat(chatRenderer()),
              "all-chat");
    EXPECT_TRUE(YouTubeLiveChatTestAccess::switchToLiveChat(chatRenderer(true))
                    .isEmpty());
    EXPECT_EQ(YouTubeLiveChatTestAccess::switchToLiveChat(
                  chatRenderer(false, "refreshed-all-chat")),
              "refreshed-all-chat");
}

TEST(YouTubeLiveChat, KeepsNormalContinuationWhenSelectorIsAbsent)
{
    const QJsonObject renderer{
        {"continuations",
         QJsonArray{
             QJsonObject{{"timedContinuationData",
                          QJsonObject{{"continuation", "incremental-chat"}}}}}},
    };
    EXPECT_EQ(YouTubeLiveChatTestAccess::continuation(renderer),
              "incremental-chat");
    EXPECT_TRUE(
        YouTubeLiveChatTestAccess::switchToLiveChat(renderer).isEmpty());
}

TEST(YouTubeLiveChat, MissingLiveChatTokenDoesNotSelectTopChat)
{
    EXPECT_EQ(YouTubeLiveChatTestAccess::continuation(chatRenderer(false, {})),
              "default-chat");
    EXPECT_TRUE(
        YouTubeLiveChatTestAccess::switchToLiveChat(chatRenderer(false, {}))
            .isEmpty());
}

TEST(YouTubeLiveChat, UsesFirstLiveProbeWithoutWaitingForTheOtherReply)
{
    for (int winner : {0, 1})
    {
        PendingDiscovery discovery;
        ASSERT_TRUE(discovery.replies[0]);
        ASSERT_TRUE(discovery.replies[1]);
        EXPECT_TRUE(discovery.results.empty());

        discovery.replies[winner]("live-video1");
        ASSERT_EQ(discovery.results.size(), 1);
        EXPECT_EQ(discovery.results.front(), "live-video1");

        // A different, later result must not reconnect to another stream.
        discovery.replies[1 - winner]("other-video");
        ASSERT_EQ(discovery.results.size(), 1);
        EXPECT_EQ(discovery.results.front(), "live-video1");
    }
}

TEST(YouTubeLiveChat, EmptyProbeDoesNotHideAnOutstandingLiveReply)
{
    for (int empty : {0, 1})
    {
        PendingDiscovery discovery;
        discovery.replies[empty]({});
        EXPECT_TRUE(discovery.results.empty());

        discovery.replies[1 - empty]("live-video1");
        ASSERT_EQ(discovery.results.size(), 1);
        EXPECT_EQ(discovery.results.front(), "live-video1");
    }
}

TEST(YouTubeLiveChat, FallsBackOnlyAfterBothFastProbesReturnEmpty)
{
    PendingDiscovery discovery;
    discovery.replies[1]({});
    EXPECT_TRUE(discovery.results.empty());
    discovery.replies[0]({});
    ASSERT_EQ(discovery.results.size(), 1);
    EXPECT_TRUE(discovery.results.front().isEmpty());
}

TEST(YouTubeLiveChat, StreamsProbeRequiresAnActiveLiveMarker)
{
    const QString pastVideo = QStringLiteral(
        R"(<link rel="canonical" href="https://www.youtube.com/watch?v=past-video1">{"videoRenderer":{"videoId":"past-video1","thumbnailOverlays":[{"style":"DEFAULT"}]}})");
    EXPECT_TRUE(YouTubeLiveChatTestAccess::streamsVideoId(pastVideo).isEmpty());
    const auto streamsPage =
        pastVideo +
        QStringLiteral(
            R"({"videoRenderer":{"videoId":"live-video1","badges":[{"style":"BADGE_STYLE_TYPE_LIVE_NOW"}]}})");
    EXPECT_EQ(YouTubeLiveChatTestAccess::streamsVideoId(streamsPage),
              "live-video1");
}

TEST(YouTubeLiveChat, SynchronousProbeRepliesResolveOnlyOnce)
{
    std::vector<QString> results;
    YouTubeLiveChatTestAccess::probeInParallel(
        [](auto callback) {
            callback("live-video1");
        },
        [](auto callback) {
            callback({});
        },
        [&results](QString videoId) {
            results.push_back(std::move(videoId));
        });
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results.front(), "live-video1");
}

TEST(YouTubeLiveChat, FocusingWaitingChannelSupersedesOldDiscoveryReplies)
{
    YouTubeLiveChat chat("@test");
    YouTubeLiveChatTestAccess::prepareDiscovery(chat);
    auto oldReply = YouTubeLiveChatTestAccess::discoveryGuard(chat);
    ASSERT_TRUE(oldReply());
    EXPECT_EQ(YouTubeLiveChatTestAccess::retryDelay(chat), 60000);

    chat.setDiscoveryForeground(true);
    EXPECT_FALSE(oldReply());
    EXPECT_EQ(YouTubeLiveChatTestAccess::retryDelay(chat), 5000);
    auto focusedReply = YouTubeLiveChatTestAccess::discoveryGuard(chat);
    ASSERT_TRUE(focusedReply());

    // Repeated focus notifications must not keep restarting the check.
    chat.setDiscoveryForeground(true);
    EXPECT_TRUE(focusedReply());
    chat.setDiscoveryForeground(false);
    EXPECT_EQ(YouTubeLiveChatTestAccess::retryDelay(chat), 60000);
}

TEST(YouTubeLiveChat, FocusingFoundStreamDoesNotRestartChatInitialization)
{
    YouTubeLiveChat chat("@test");
    YouTubeLiveChatTestAccess::prepareDiscovery(chat, "live-video1");
    auto loadingReply = YouTubeLiveChatTestAccess::discoveryGuard(chat);
    chat.setDiscoveryForeground(true);
    EXPECT_TRUE(loadingReply());
    chat.setDiscoveryForeground(false);
    chat.setDiscoveryForeground(true);
    EXPECT_TRUE(loadingReply());
}

TEST(YouTubeLiveChat, LateDiscoveryReplyIsIgnoredAfterSessionStartsOrStops)
{
    auto chat = std::make_unique<YouTubeLiveChat>("@test");
    YouTubeLiveChatTestAccess::prepareDiscovery(*chat);
    auto discoveryReply = YouTubeLiveChatTestAccess::discoveryGuard(*chat);
    ASSERT_TRUE(discoveryReply());
    YouTubeLiveChatTestAccess::sessionStarted(*chat);
    EXPECT_FALSE(discoveryReply());

    auto lateReply = YouTubeLiveChatTestAccess::discoveryGuard(*chat);
    ASSERT_TRUE(lateReply());
    chat->stop();
    EXPECT_FALSE(lateReply());
    chat.reset();
    EXPECT_FALSE(lateReply());
}

TEST(YouTubeLiveChat, DiscoveryContextSkipsAnExtraBootstrapRequest)
{
    YouTubeLiveChat chat("@test");
    YouTubeLiveChatTestAccess::prepareDiscovery(chat);
    YouTubeLiveChatTestAccess::cacheContext(chat, DISCOVERY_CONTEXT);
    EXPECT_EQ(YouTubeLiveChatTestAccess::context(chat),
              (QStringList{"test-key", "test-version", "test-visitor"}));
    bool ready = false;
    YouTubeLiveChatTestAccess::bootstrap(chat, [&ready] {
        ready = true;
    });
    EXPECT_TRUE(ready);

    // The other discovery page must not replace context already in use.
    YouTubeLiveChatTestAccess::cacheContext(
        chat,
        R"({"INNERTUBE_API_KEY":"other-key","INNERTUBE_CLIENT_VERSION":"other-version","VISITOR_DATA":"other-visitor"})");
    EXPECT_EQ(YouTubeLiveChatTestAccess::context(chat),
              (QStringList{"test-key", "test-version", "test-visitor"}));
}

TEST(YouTubeLiveChat, IncompleteDiscoveryContextKeepsBootstrapAvailable)
{
    YouTubeLiveChat chat("@test");
    YouTubeLiveChatTestAccess::cacheContext(
        chat,
        R"({"INNERTUBE_API_KEY":"test-key","INNERTUBE_CLIENT_VERSION":"test-version"})");
    EXPECT_EQ(YouTubeLiveChatTestAccess::context(chat),
              (QStringList{"", "", ""}));
    YouTubeLiveChatTestAccess::cacheContext(chat, DISCOVERY_CONTEXT);
    EXPECT_EQ(YouTubeLiveChatTestAccess::context(chat),
              (QStringList{"test-key", "test-version", "test-visitor"}));
}
