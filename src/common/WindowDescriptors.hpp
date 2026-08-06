// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ProviderId.hpp"

#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace chatterino {

/**
 * A WindowLayout contains one or more windows.
 * Only one of those windows can be the main window
 *
 * Each window contains a list of tabs.
 * Only one of those tabs can be marked as selected.
 *
 * Each tab contains a root node.
 * The root node is either a:
 *  - Split Node (for single-split tabs), or
 *  - Container Node (for multi-split tabs).
 *    This container node would then contain a list of nodes on its own, which could be split nodes or further container nodes
 **/

// from widgets/Window.hpp
enum class WindowType;
enum class ActivityTimeDisplayMode : std::uint8_t;
enum class MessagePlatform : std::uint8_t;
enum class PlatformIndicatorMode : std::uint8_t;

struct SplitDescriptor {
    // Twitch or mentions or watching or live or automod or whispers or IRC
    QString type_;

    // Twitch Channel name or IRC channel name
    QString channelName_;

    // IRC server
    int server_{-1};

    // Whether "Moderation Mode" (the sword icon) is enabled in this split or not
    bool moderationMode_{false};
    std::optional<bool> activityPane_;
    bool inputEnabled_{true};
    std::optional<bool> filterActivity_;
    std::optional<bool> filterActivityExplicit_;
    qreal activityMessageScale_{0.9};
    bool collapseGiftedSubscriptions_{true};
    std::optional<ActivityTimeDisplayMode> activityTimeDisplayMode_;
    bool slowerChatEnabled_{false};
    qreal slowerChatMessagesPerSecond_{5.0};
    bool slowerChatMessageAnimations_{true};
    bool streamDatabaseBadgeFeedVisible_{true};
    bool titleSettingsButtonVisible_{true};
    bool chatModeIndicatorVisible_{true};
    std::optional<bool> viewerCountEnabled_;
    uint32_t twitchActivityMinimumBits_{100};
    uint32_t kickActivityMinimumKicks_{100};
    uint32_t tiktokActivityMinimumDiamonds_{0};
    std::optional<PlatformIndicatorMode> platformIndicatorMode_;
    std::optional<MessagePlatform> selectedSendPlatform_;
    bool selectedSendAllPlatforms_{false};
    std::vector<MessagePlatform> customSelectedSendPlatforms_;
    std::vector<MessagePlatform> enabledSendPlatforms_;

    std::optional<bool> spellCheckOverride;

    QList<QUuid> filters_;

    uint64_t kickChannelID = 0;
    uint64_t kickUserID = 0;
    uint64_t kickRoomID = 0;

    bool mergedTwitchEnabled = false;
    QString mergedTwitchChannelName_;
    bool mergedKickEnabled = false;
    QString mergedKickChannelName_;
    bool mergedYoutubeEnabled = false;
    QString mergedYoutubeStreamUrl_;
    bool mergedTikTokEnabled = false;
    QString mergedTikTokSource_;
    bool mergedTikTokShowJoinMessages = true;

    static void loadFromJSON(SplitDescriptor &descriptor,
                             const QJsonObject &root, const QJsonObject &data);
};

struct SplitNodeDescriptor : SplitDescriptor {
    qreal flexH_ = 1;
    qreal flexV_ = 1;
};

struct ContainerNodeDescriptor;

using NodeDescriptor =
    std::variant<ContainerNodeDescriptor, SplitNodeDescriptor>;

struct ContainerNodeDescriptor {
    qreal flexH_ = 1;
    qreal flexV_ = 1;

    bool vertical_ = false;

    std::vector<NodeDescriptor> items_;
};

struct TabFolderDescriptor {
    QString id_;
    QString title_;
    bool expanded_{true};
};

struct TabDescriptor {
    static TabDescriptor loadFromJSON(const QJsonObject &root);

    QString customTitle_;
    QString folderId_;
    bool selected_{false};
    bool highlightsEnabled_{true};

    std::optional<NodeDescriptor> rootNode_;
};

struct WindowDescriptor {
    enum class State {
        None,
        Minimized,
        Maximized,
    };

    WindowType type_;
    State state_ = State::None;

    QRect geometry_;

    std::vector<TabFolderDescriptor> tabFolders_;
    std::vector<TabDescriptor> tabs_;
};

class WindowLayout
{
public:
    // A complete window layout has a single emote popup position that is shared among all windows
    QRect emotePopupBounds_;

    std::vector<WindowDescriptor> windows_;

    /// Selects the split containing the channel specified by @a name for the specified
    /// @a provider. Currently, only Twitch is supported as the provider
    /// and special channels (such as /mentions) are ignored.
    ///
    /// Tabs with fewer splits are preferred.
    /// Channels without filters are preferred.
    ///
    /// If no split with the channel exists, a new one is added.
    /// If no window exists, a new one is added.
    void activateOrAddChannel(ProviderId provider, const QString &name);
    static WindowLayout loadFromFile(const QString &path);
};

}  // namespace chatterino
