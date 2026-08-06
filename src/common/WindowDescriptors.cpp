// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/WindowDescriptors.hpp"

#include "common/QLogging.hpp"
#include "messages/Message.hpp"
#include "singletons/Settings.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/Window.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace chatterino {

namespace {

QJsonArray loadWindowArray(const QString &settingsPath)
{
    QFile file(settingsPath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    QByteArray data = file.readAll();
    QJsonDocument document = QJsonDocument::fromJson(data);
    QJsonArray windows_arr = document.object().value("windows").toArray();
    return windows_arr;
}

PlatformIndicatorMode fallbackPlatformIndicatorMode()
{
    return getSettings()->mergedPlatformIndicatorMode.getEnum();
}

std::optional<PlatformIndicatorMode> loadPlatformIndicatorMode(
    const QJsonValue &value)
{
    if (value.isDouble())
    {
        switch (value.toInt(-1))
        {
            case static_cast<int>(PlatformIndicatorMode::Badge):
                return PlatformIndicatorMode::Badge;
            case static_cast<int>(PlatformIndicatorMode::LineColor):
                return PlatformIndicatorMode::LineColor;
            case static_cast<int>(PlatformIndicatorMode::Both):
                return PlatformIndicatorMode::Both;
            case static_cast<int>(PlatformIndicatorMode::None):
                return PlatformIndicatorMode::None;
            default:
                return fallbackPlatformIndicatorMode();
        }
    }

    if (!value.isString())
    {
        return std::nullopt;
    }

    const auto text = value.toString().trimmed();
    if (text.isEmpty())
    {
        return fallbackPlatformIndicatorMode();
    }

    if (auto parsed = qmagicenum::enumCast<PlatformIndicatorMode>(
            text, qmagicenum::CASE_INSENSITIVE))
    {
        return parsed;
    }

    const auto normalized = text.toLower();
    auto compacted = normalized;
    compacted.remove(QChar(' '));
    compacted.remove(QChar('-'));
    compacted.remove(QChar('_'));

    if (compacted == u"highlights" || compacted == u"highlight" ||
        compacted == u"linecolor" || compacted == u"platformlinecolor" ||
        compacted == u"platformhighlights" ||
        compacted == u"platformhighlights(default)")
    {
        return PlatformIndicatorMode::LineColor;
    }
    if (compacted == u"logos" || compacted == u"logo" ||
        compacted == u"badge" || compacted == u"platformbadge" ||
        compacted == u"platformbadge(default)" ||
        compacted == u"platformlogo" || compacted == u"platformlogos")
    {
        return PlatformIndicatorMode::Badge;
    }
    if (compacted == u"platformlinecolor+badge" ||
        compacted == u"platformhighlights+logos")
    {
        return PlatformIndicatorMode::Both;
    }
    if (compacted == u"none")
    {
        return PlatformIndicatorMode::None;
    }

    return fallbackPlatformIndicatorMode();
}

std::optional<MessagePlatform> loadMessagePlatform(const QJsonValue &value)
{
    if (value.isDouble())
    {
        switch (value.toInt(-1))
        {
            case static_cast<int>(MessagePlatform::AnyOrTwitch):
                return MessagePlatform::AnyOrTwitch;
            case static_cast<int>(MessagePlatform::Kick):
                return MessagePlatform::Kick;
            case static_cast<int>(MessagePlatform::YouTube):
                return MessagePlatform::YouTube;
            case static_cast<int>(MessagePlatform::TikTok):
                return MessagePlatform::TikTok;
            default:
                return std::nullopt;
        }
    }

    if (!value.isString())
    {
        return std::nullopt;
    }

    auto compacted = value.toString().trimmed().toLower();
    compacted.remove(QChar(' '));
    compacted.remove(QChar('-'));
    compacted.remove(QChar('_'));

    if (compacted == u"twitch" || compacted == u"anyortwitch")
    {
        return MessagePlatform::AnyOrTwitch;
    }
    if (compacted == u"kick")
    {
        return MessagePlatform::Kick;
    }
    if (compacted == u"youtube")
    {
        return MessagePlatform::YouTube;
    }
    if (compacted == u"tiktok")
    {
        return MessagePlatform::TikTok;
    }

    return std::nullopt;
}

std::vector<MessagePlatform> loadMessagePlatforms(const QJsonValue &value)
{
    std::vector<MessagePlatform> platforms;
    if (!value.isArray())
    {
        return platforms;
    }

    const auto array = value.toArray();
    platforms.reserve(array.size());
    for (const auto &item : array)
    {
        if (auto platform = loadMessagePlatform(item))
        {
            platforms.push_back(*platform);
        }
    }
    return platforms;
}

template <typename T>
T loadNodes(const QJsonObject &obj)
{
    static_assert("loadNodes must be called with the SplitNodeDescriptor "
                  "or ContainerNodeDescriptor type");
}

template <>
SplitNodeDescriptor loadNodes(const QJsonObject &root)
{
    SplitNodeDescriptor descriptor;

    descriptor.flexH_ = root.value("flexh").toDouble(1.0);
    descriptor.flexV_ = root.value("flexv").toDouble(1.0);

    auto data = root.value("data").toObject();

    SplitDescriptor::loadFromJSON(descriptor, root, data);

    return descriptor;
}

template <>
ContainerNodeDescriptor loadNodes(const QJsonObject &root)
{
    ContainerNodeDescriptor descriptor;

    descriptor.flexH_ = root.value("flexh").toDouble(1.0);
    descriptor.flexV_ = root.value("flexv").toDouble(1.0);

    descriptor.vertical_ = root.value("type").toString() == "vertical";

    for (QJsonValue _val : root.value("items").toArray())
    {
        auto _obj = _val.toObject();

        auto _type = _obj.value("type");
        if (_type == "split")
        {
            descriptor.items_.emplace_back(
                loadNodes<SplitNodeDescriptor>(_obj));
        }
        else
        {
            descriptor.items_.emplace_back(
                loadNodes<ContainerNodeDescriptor>(_obj));
        }
    }

    return descriptor;
}

const QList<QUuid> loadFilters(QJsonValue val)
{
    QList<QUuid> filterIds;

    if (!val.isUndefined())
    {
        const auto array = val.toArray();
        filterIds.reserve(array.size());
        for (const auto &id : array)
        {
            filterIds.append(QUuid::fromString(id.toString()));
        }
    }

    return filterIds;
}

}  // namespace

void SplitDescriptor::loadFromJSON(SplitDescriptor &descriptor,
                                   const QJsonObject &root,
                                   const QJsonObject &data)
{
    descriptor.type_ = data.value("type").toString();
    descriptor.server_ = data.value("server").toInt(-1);
    descriptor.moderationMode_ = root.value("moderationMode").toBool();
    if (root["activityPane"].isBool())
    {
        descriptor.activityPane_ = root.value("activityPane").toBool();
    }
    descriptor.inputEnabled_ = root.value("inputEnabled").toBool(true);
    if (root["filterActivity"].isBool())
    {
        descriptor.filterActivity_ = root.value("filterActivity").toBool();
    }
    if (root["filterActivityExplicit"].isBool())
    {
        descriptor.filterActivityExplicit_ =
            root.value("filterActivityExplicit").toBool();
    }
    descriptor.activityMessageScale_ =
        root.value("activityMessageScale").toDouble(0.9);
    descriptor.collapseGiftedSubscriptions_ =
        root.value("collapseGiftedSubscriptions").toBool(true);
    if (auto activityTimeDisplayMode = root["activityTimeDisplayMode"];
        activityTimeDisplayMode.isString())
    {
        descriptor.activityTimeDisplayMode_ =
            qmagicenum::enumCast<ActivityTimeDisplayMode>(
                activityTimeDisplayMode.toString(),
                qmagicenum::CASE_INSENSITIVE);
    }
    descriptor.slowerChatEnabled_ =
        root.value("slowerChatEnabled").toBool(false);
    descriptor.slowerChatMessagesPerSecond_ =
        root.value("slowerChatMessagesPerSecond").toDouble(5.0);
    descriptor.slowerChatMessageAnimations_ =
        root.value("slowerChatMessageAnimations").toBool(true);
    descriptor.streamDatabaseBadgeFeedVisible_ =
        root.value("streamDatabaseBadgeFeedVisible").toBool(true);
    descriptor.titleSettingsButtonVisible_ =
        root.value("titleSettingsButtonVisible").toBool(true);
    descriptor.chatModeIndicatorVisible_ =
        root.value("chatModeIndicatorVisible").toBool(true);
    if (root["viewerCountEnabled"].isBool())
    {
        descriptor.viewerCountEnabled_ =
            root.value("viewerCountEnabled").toBool();
    }
    descriptor.twitchActivityMinimumBits_ = static_cast<uint32_t>(
        root.value("twitchActivityMinimumBits").toInt(100));
    descriptor.kickActivityMinimumKicks_ = static_cast<uint32_t>(
        root.value("kickActivityMinimumKicks").toInt(100));
    descriptor.tiktokActivityMinimumDiamonds_ = static_cast<uint32_t>(
        root.value("tiktokActivityMinimumDiamonds").toInt(0));
    if (auto platformIndicatorMode =
            loadPlatformIndicatorMode(root["platformIndicatorMode"]))
    {
        descriptor.platformIndicatorMode_ = *platformIndicatorMode;
    }
    if (auto selectedSendPlatform =
            loadMessagePlatform(root["selectedSendPlatform"]))
    {
        descriptor.selectedSendPlatform_ = *selectedSendPlatform;
    }
    descriptor.selectedSendAllPlatforms_ =
        root.value("selectedSendAllPlatforms").toBool(false);
    descriptor.customSelectedSendPlatforms_ =
        loadMessagePlatforms(root["customSelectedSendPlatforms"]);
    descriptor.enabledSendPlatforms_ =
        loadMessagePlatforms(root["enabledSendPlatforms"]);
    if (data.contains("channel"))
    {
        descriptor.channelName_ = data.value("channel").toString();
    }
    else
    {
        descriptor.channelName_ = data.value("name").toString();
    }
    descriptor.filters_ = loadFilters(root.value("filters"));

    auto spellOverride = root["checkSpelling"];
    if (spellOverride.isBool())
    {
        descriptor.spellCheckOverride = spellOverride.toBool();
    }
    if (descriptor.type_ == u"kick")
    {
        descriptor.kickChannelID =
            static_cast<uint64_t>(data["channelID"].toInt());
        descriptor.kickUserID = static_cast<uint64_t>(data["userID"].toInt());
        descriptor.kickRoomID = static_cast<uint64_t>(data["roomID"].toInt());
    }
    else if (descriptor.type_ == u"merged")
    {
        descriptor.mergedTwitchEnabled = data["twitchEnabled"].toBool(true);
        descriptor.mergedTwitchChannelName_ =
            data["twitchChannel"].toString();
        descriptor.mergedKickEnabled = data["kickEnabled"].toBool(true);
        descriptor.mergedKickChannelName_ = data["kickChannel"].toString();
        descriptor.mergedYoutubeEnabled =
            data["youtubeEnabled"].toBool(false);
        descriptor.mergedYoutubeStreamUrl_ =
            data["youtubeStreamUrl"].toString();
        descriptor.mergedTikTokEnabled = data["tiktokEnabled"].toBool(false);
        descriptor.mergedTikTokSource_ = data["tiktokSource"].toString();
        descriptor.mergedTikTokShowJoinMessages =
            data["tiktokShowJoinMessages"].toBool(true);
    }
}

TabDescriptor TabDescriptor::loadFromJSON(const QJsonObject &tabObj)
{
    TabDescriptor tab;
    // Load tab custom title
    QJsonValue titleVal = tabObj.value("title");
    if (titleVal.isString())
    {
        tab.customTitle_ = titleVal.toString();
    }

    QJsonValue folderIdVal = tabObj.value("folderId");
    if (folderIdVal.isString())
    {
        tab.folderId_ = folderIdVal.toString();
    }

    // Load tab selected state
    tab.selected_ = tabObj.value("selected").toBool(false);

    // Load tab "highlightsEnabled" state
    tab.highlightsEnabled_ = tabObj.value("highlightsEnabled").toBool(true);

    QJsonObject splitRoot = tabObj.value("splits2").toObject();

    // Load tab splits
    if (!splitRoot.isEmpty())
    {
        // root type
        auto nodeType = splitRoot.value("type").toString();
        if (nodeType == "split")
        {
            tab.rootNode_ = loadNodes<SplitNodeDescriptor>(splitRoot);
        }
        else if (nodeType == "horizontal" || nodeType == "vertical")
        {
            tab.rootNode_ = loadNodes<ContainerNodeDescriptor>(splitRoot);
        }
    }

    return tab;
}

WindowLayout WindowLayout::loadFromFile(const QString &path)
{
    WindowLayout layout;

    bool hasSetAMainWindow = false;

    // "deserialize"
    for (const QJsonValue &windowVal : loadWindowArray(path))
    {
        QJsonObject windowObj = windowVal.toObject();

        WindowDescriptor window;

        // Load window type
        QString typeVal = windowObj.value("type").toString();
        auto type = typeVal == "main" ? WindowType::Main : WindowType::Popup;

        if (type == WindowType::Main)
        {
            if (hasSetAMainWindow)
            {
                qCDebug(chatterinoCommon)
                    << "Window Layout file contains more than one Main window "
                       "- demoting to Popup type";
                type = WindowType::Popup;
            }
            hasSetAMainWindow = true;
        }

        window.type_ = type;

        // Load window state
        if (windowObj.value("state") == "minimized")
        {
            window.state_ = WindowDescriptor::State::Minimized;
        }
        else if (windowObj.value("state") == "maximized")
        {
            window.state_ = WindowDescriptor::State::Maximized;
        }

        // Load window geometry
        {
            int x = windowObj.value("x").toInt(-1);
            int y = windowObj.value("y").toInt(-1);
            int width = windowObj.value("width").toInt(-1);
            int height = windowObj.value("height").toInt(-1);

            window.geometry_ = QRect(x, y, width, height);
        }

        bool hasSetASelectedTab = false;

        // Load tab folders
        QJsonArray tabFolders = windowObj.value("tabFolders").toArray();
        for (const auto &folderVal : tabFolders)
        {
            const auto folderObj = folderVal.toObject();
            TabFolderDescriptor folder;
            folder.id_ = folderObj.value("id").toString();
            if (folder.id_.isEmpty())
            {
                continue;
            }
            folder.title_ =
                folderObj.value("title").toString(QStringLiteral("Folder"));
            folder.expanded_ = folderObj.value("expanded").toBool(true);
            window.tabFolders_.emplace_back(std::move(folder));
        }

        // Load window tabs
        QJsonArray tabs = windowObj.value("tabs").toArray();
        for (QJsonValue tabVal : tabs)
        {
            TabDescriptor tab = TabDescriptor::loadFromJSON(tabVal.toObject());
            if (tab.selected_)
            {
                if (hasSetASelectedTab)
                {
                    qCDebug(chatterinoCommon)
                        << "Window contains more than one selected tab - "
                           "demoting to unselected";
                    tab.selected_ = false;
                }
                hasSetASelectedTab = true;
            }
            window.tabs_.emplace_back(std::move(tab));
        }

        // Load emote popup position
        {
            auto emotePopup = windowObj["emotePopup"].toObject();
            layout.emotePopupBounds_ = QRect{
                emotePopup["x"].toInt(),
                emotePopup["y"].toInt(),
                emotePopup["width"].toInt(),
                emotePopup["height"].toInt(),
            };
        }

        layout.windows_.emplace_back(std::move(window));
    }

    return layout;
}

void WindowLayout::activateOrAddChannel(ProviderId provider,
                                        const QString &name)
{
    if (provider != ProviderId::Twitch || name.startsWith(u'/') ||
        name.startsWith(u'$'))
    {
        qCWarning(chatterinoWindowmanager)
            << "Only twitch channels can be set as active";
        return;
    }

    auto mainWindow = std::find_if(this->windows_.begin(), this->windows_.end(),
                                   [](const auto &win) {
                                       return win.type_ == WindowType::Main;
                                   });

    if (mainWindow == this->windows_.end())
    {
        this->windows_.emplace_back(WindowDescriptor{
            .type_ = WindowType::Main,
            .geometry_ = {-1, -1, -1, -1},
            .tabs_ =
                {
                    TabDescriptor{
                        .selected_ = true,
                        .rootNode_ = SplitNodeDescriptor{{
                            .type_ = "twitch",
                            .channelName_ = name,
                        }},
                    },
                },
        });
        return;
    }

    TabDescriptor *bestTab = nullptr;
    // The tab score is calculated as follows:
    // +2 for every split
    // +1 if the desired split has filters
    // Thus lower is better and having one split of a channel is preferred over multiple
    size_t bestTabScore = std::numeric_limits<size_t>::max();

    for (auto &tab : mainWindow->tabs_)
    {
        tab.selected_ = false;

        if (!tab.rootNode_)
        {
            continue;
        }

        // recursive visitor
        struct Visitor {
            const QString &spec;
            size_t score = 0;
            bool hasChannel = false;

            void operator()(const SplitNodeDescriptor &split)
            {
                this->score += 2;
                if (split.channelName_ == this->spec)
                {
                    this->hasChannel = true;
                    if (!split.filters_.empty())
                    {
                        this->score += 1;
                    }
                }
            }

            void operator()(const ContainerNodeDescriptor &container)
            {
                for (const auto &item : container.items_)
                {
                    std::visit(*this, item);
                }
            }
        } visitor{name};

        std::visit(visitor, *tab.rootNode_);

        if (visitor.hasChannel && visitor.score < bestTabScore)
        {
            bestTab = &tab;
            bestTabScore = visitor.score;
        }
    }

    if (bestTab)
    {
        bestTab->selected_ = true;
        return;
    }

    TabDescriptor tab{
        .selected_ = true,
        .rootNode_ = SplitNodeDescriptor{{
            .type_ = "twitch",
            .channelName_ = name,
        }},
    };
    mainWindow->tabs_.emplace_back(tab);
}

}  // namespace chatterino
