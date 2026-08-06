// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ChannelView.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/commands/builtin/PinMessage.hpp"
#include "controllers/filters/FilterSet.hpp"
#include "debug/Benchmark.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageThread.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/seventv/SeventvAccountManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/DistanceBetweenPoints.hpp"
#include "widgets/helper/ActivityMessageUtils.hpp"
#include "util/Helpers.hpp"
#include "util/IncognitoBrowser.hpp"
#include "util/PostToThread.hpp"
#include "util/QMagicEnum.hpp"
#include "util/Twitch.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/dialogs/ReplyThreadPopup.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ScrollbarHighlight.hpp"
#include "widgets/helper/SearchPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/TooltipWidget.hpp"
#include "widgets/Window.hpp"

#include <magic_enum/magic_enum_flags.hpp>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QEasingCurve>
#include <QGestureEvent>
#include <QGraphicsBlurEffect>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStringBuilder>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr size_t TOOLTIP_EMOTE_ENTRIES_LIMIT = 7;

using namespace chatterino;

constexpr int SCROLLBAR_PADDING = 8;
constexpr int BOTTOM_MESSAGE_PADDING = 8;
constexpr int MESSAGE_BUFFER_RETAIN_VIEWPORTS = 1;
constexpr qreal SLOW_CHAT_MESSAGE_INITIAL_OPACITY = 0.4;
constexpr qreal SLOW_CHAT_MESSAGE_INITIAL_OPACITY_HOLD = 0.0;
constexpr qreal SLOW_CHAT_MESSAGE_OPACITY_PROGRESS_SCALE = 1.05;
constexpr auto MESSAGE_LAYOUT_SHIFT_ANIMATION_DURATION =
    std::chrono::milliseconds(160);
constexpr auto SLOW_CHAT_MESSAGE_ANIMATION_DURATION =
    std::chrono::milliseconds(500);

enum class SlowChatQueuePriority {
    Normal,
    Vip,
    Moderator,
};

SlowChatQueuePriority slowChatQueuePriority(const MessagePtr &message)
{
    if (!message)
    {
        return SlowChatQueuePriority::Normal;
    }

    bool isVip = false;

    for (const auto &badge : message->twitchBadges)
    {
        if (badge.key_ == "moderator" || badge.key_ == "lead_moderator")
        {
            return SlowChatQueuePriority::Moderator;
        }

        if (badge.key_ == "vip")
        {
            isVip = true;
        }
    }

    for (const auto &element : message->elements)
    {
        if (!element)
        {
            continue;
        }

        const auto type = element->type();
        if (type == ModBadgeElement::TYPE)
        {
            return SlowChatQueuePriority::Moderator;
        }

        if (type == VipBadgeElement::TYPE)
        {
            isVip = true;
            continue;
        }

        if (type != BadgeElement::TYPE)
        {
            continue;
        }

        const auto &tooltip = element->getTooltip();
        if (tooltip.startsWith("Moderator", Qt::CaseInsensitive) ||
            tooltip.startsWith("Lead Moderator", Qt::CaseInsensitive))
        {
            return SlowChatQueuePriority::Moderator;
        }

        if (tooltip.startsWith("VIP", Qt::CaseInsensitive))
        {
            isVip = true;
        }
    }

    return isVip ? SlowChatQueuePriority::Vip
                 : SlowChatQueuePriority::Normal;
}

ChannelPtr commandChannelForMessage(const ChannelPtr &channel,
                                    MessagePlatform platform)
{
    const auto merged = std::dynamic_pointer_cast<MergedChannel>(channel);
    if (!merged)
    {
        return channel;
    }

    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch:
            return merged->twitchChannel();
        case MessagePlatform::Kick:
            return merged->kickChannel();
        default:
            return channel;
    }
}

QColor blendColorTowards(const QColor &base, const QColor &target, qreal factor)
{
    factor = std::clamp(factor, 0.0, 1.0);

    QColor result;
    result.setRgbF(base.redF() * (1.0 - factor) + target.redF() * factor,
                   base.greenF() * (1.0 - factor) +
                       target.greenF() * factor,
                   base.blueF() * (1.0 - factor) + target.blueF() * factor,
                   base.alphaF() * (1.0 - factor) + target.alphaF() * factor);
    return result;
}

qreal slowChatArrivalOpacity(qreal progress)
{
    const auto clampedProgress = std::clamp(progress, 0.0, 1.0);
    if (clampedProgress <= SLOW_CHAT_MESSAGE_INITIAL_OPACITY_HOLD)
    {
        return SLOW_CHAT_MESSAGE_INITIAL_OPACITY;
    }

    // Keep entering messages visibly dim until they are almost settled.
    static const QEasingCurve curve(QEasingCurve::InCubic);
    const auto normalizedProgress = std::clamp(
        (clampedProgress - SLOW_CHAT_MESSAGE_INITIAL_OPACITY_HOLD) /
            (1.0 - SLOW_CHAT_MESSAGE_INITIAL_OPACITY_HOLD) *
            SLOW_CHAT_MESSAGE_OPACITY_PROGRESS_SCALE,
        0.0, 1.0);
    const auto eased = curve.valueForProgress(normalizedProgress);
    return SLOW_CHAT_MESSAGE_INITIAL_OPACITY +
           ((1.0 - SLOW_CHAT_MESSAGE_INITIAL_OPACITY) * eased);
}

qreal slowChatArrivalOffset(qreal progress, qreal height)
{
    static const QEasingCurve curve(QEasingCurve::OutCubic);
    const auto eased = curve.valueForProgress(std::clamp(progress, 0.0, 1.0));
    return (1.0 - eased) * height;
}

qreal messageLayoutShiftProgress(qreal progress)
{
    static const QEasingCurve curve(QEasingCurve::OutCubic);
    return curve.valueForProgress(std::clamp(progress, 0.0, 1.0));
}

bool isSameChatMessage(const MessagePtr &left, const MessagePtr &right)
{
    if (left == right)
    {
        return true;
    }
    if (!left || !right || left->id.isEmpty() || right->id.isEmpty())
    {
        return false;
    }

    return left->platform == right->platform && left->id == right->id &&
           left->channelName.compare(right->channelName,
                                     Qt::CaseInsensitive) == 0;
}

template <typename F>
void runOnChannelViewThread(ChannelView *view, F &&callback)
{
    QPointer<ChannelView> weakView(view);
    runInGuiThread([weakView, callback = std::forward<F>(callback)]() mutable {
        if (weakView)
        {
            callback(*weakView);
        }
    });
}

bool isSevenTVEmote(const Emote &emote)
{
    if (emote.id.string.isEmpty())
    {
        return false;
    }

    const QUrl page(emote.homePage.string);
    return page.isValid() &&
           page.host().endsWith(QStringLiteral("7tv.app"),
                                Qt::CaseInsensitive) &&
           page.path().startsWith(QStringLiteral("/emotes/"));
}

QString editorTargetName(const SeventvEditorChannel &target)
{
    if (!target.displayName.isEmpty())
    {
        return target.displayName;
    }
    if (!target.username.isEmpty())
    {
        return target.username;
    }
    return target.connectionID;
}

QString editorTargetLabel(const SeventvEditorChannel &target)
{
    return editorTargetName(target);
}

bool editorTargetMatchesChannel(const SeventvEditorChannel &target,
                                const ChannelPtr &channel)
{
    if (!channel)
    {
        return false;
    }

    if (const auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        if (editorTargetMatchesChannel(target, merged->twitchChannel()) ||
            editorTargetMatchesChannel(target, merged->kickChannel()))
        {
            return true;
        }
        if (target.platform != QStringLiteral("YOUTUBE") ||
            !merged->config().youtubeEnabled || !merged->youtubeLiveChat())
        {
            return false;
        }

        auto source = merged->youtubeLiveChat()->resolvedSource().trimmed();
        if (source.startsWith('@'))
        {
            source.remove(0, 1);
        }
        auto targetUsername = target.username.trimmed();
        if (targetUsername.startsWith('@'))
        {
            targetUsername.remove(0, 1);
        }
        return (!target.connectionID.isEmpty() &&
                target.connectionID.compare(source, Qt::CaseInsensitive) ==
                    0) ||
               (!targetUsername.isEmpty() &&
                targetUsername.compare(source, Qt::CaseInsensitive) == 0);
    }

    if (const auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        if (!target.emoteSetID.isEmpty() &&
            target.emoteSetID == twitch->seventvEmoteSetID())
        {
            return true;
        }
    }
    if (const auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
    {
        if (!target.emoteSetID.isEmpty() &&
            target.emoteSetID == kick->seventvEmoteSetID())
        {
            return true;
        }
    }

    if (target.platform == QStringLiteral("TWITCH"))
    {
        const auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel);
        if (!twitch)
        {
            return false;
        }
        return (!target.emoteSetID.isEmpty() &&
                target.emoteSetID == twitch->seventvEmoteSetID()) ||
               (!target.connectionID.isEmpty() &&
                target.connectionID == twitch->roomId()) ||
               (!target.username.isEmpty() &&
                target.username.compare(twitch->getName(),
                                        Qt::CaseInsensitive) == 0);
    }

    if (target.platform == QStringLiteral("KICK"))
    {
        const auto kick = std::dynamic_pointer_cast<KickChannel>(channel);
        if (!kick)
        {
            return false;
        }
        return (!target.emoteSetID.isEmpty() &&
                target.emoteSetID == kick->seventvEmoteSetID()) ||
               (!target.connectionID.isEmpty() && kick->userID() != 0 &&
                target.connectionID == QString::number(kick->userID())) ||
               (!target.username.isEmpty() &&
                target.username.compare(kick->slug(), Qt::CaseInsensitive) ==
                    0);
    }

    return false;
}

std::vector<SeventvEditorChannel> availableEditorTargets()
{
    const auto &allTargets =
        SeventvAccountManager::instance().editorChannels();
    std::vector<SeventvEditorChannel> targets;
    QSet<QString> activeEmoteSets;
    for (const auto &target : allTargets)
    {
        const auto emoteSetID = target.emoteSetID.trimmed();
        if (emoteSetID.isEmpty() || activeEmoteSets.contains(emoteSetID))
        {
            continue;
        }
        activeEmoteSets.insert(emoteSetID);
        targets.push_back(target);
    }
    return targets;
}

void refreshEditorTargetInChannel(const SeventvEditorChannel &target,
                                  const ChannelPtr &channel)
{
    if (!channel)
    {
        return;
    }
    if (const auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        refreshEditorTargetInChannel(target, merged->twitchChannel());
        refreshEditorTargetInChannel(target, merged->kickChannel());
        return;
    }
    if (!editorTargetMatchesChannel(target, channel))
    {
        return;
    }
    if (const auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        twitch->refreshSevenTVChannelEmotes(false);
    }
    else if (const auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
    {
        kick->reloadSeventvEmotes(false);
    }
}

void refreshOpenEditorTarget(const SeventvEditorChannel &target)
{
    getApp()->getWindows()->getMainWindow().getNotebook().forEachSplit(
        [&](Split *split) {
            refreshEditorTargetInChannel(
                target, split->getIndirectChannel().get());
        });
}

struct SevenTVEditorMenuEntry {
    SeventvEditorChannel target;
    std::optional<SeventvManagedEmote> enabledEmote;
    QString error;
};

struct SevenTVEditorMenuState {
    QPointer<QMenu> addMenu;
    QPointer<QMenu> removeMenu;
    QPointer<QMenu> renameMenu;
    QPointer<QWidget> dialogParent;
    ChannelPtr feedbackChannel;
    QString emoteID;
    QString originalName;
    std::vector<SevenTVEditorMenuEntry> entries;
    int pending = 0;
};

void addDisabledMenuMessage(QMenu *menu, const QString &text)
{
    auto *action = menu->addAction(text);
    action->setEnabled(false);
}

void postSevenTVEditorFeedback(const ChannelPtr &channel,
                               const QString &message)
{
    if (channel)
    {
        channel->addSystemMessage(message);
    }
}

void populateSevenTVEditorMenus(
    const std::shared_ptr<SevenTVEditorMenuState> &state)
{
    if (state->addMenu == nullptr || state->removeMenu == nullptr ||
        state->renameMenu == nullptr)
    {
        return;
    }

    state->addMenu->clear();
    state->removeMenu->clear();
    state->renameMenu->clear();
    int failedLoads = 0;

    for (const auto &entry : state->entries)
    {
        if (!entry.error.isEmpty())
        {
            ++failedLoads;
            continue;
        }

        const auto target = entry.target;
        const auto targetLabel = editorTargetLabel(target);
        if (!entry.enabledEmote)
        {
            state->addMenu->addAction(
                targetLabel,
                [dialogParent = state->dialogParent,
                 feedbackChannel = state->feedbackChannel, target,
                 emoteID = state->emoteID,
                 originalName = state->originalName] {
                    QTimer::singleShot(
                        0, dialogParent,
                        [dialogParent, feedbackChannel, target, emoteID,
                         originalName] {
                            if (dialogParent == nullptr)
                            {
                                return;
                            }
                            bool accepted = false;
                            const auto name =
                                QInputDialog::getText(
                                    dialogParent,
                                    QStringLiteral("Add 7TV emote"),
                                    QStringLiteral("Add \"%1\" to %2 as:")
                                        .arg(originalName,
                                             editorTargetLabel(target)),
                                    QLineEdit::Normal, originalName, &accepted)
                                    .trimmed();
                            if (!accepted)
                            {
                                return;
                            }
                            if (name.isEmpty())
                            {
                                postSevenTVEditorFeedback(
                                    feedbackChannel,
                                    QStringLiteral(
                                        "The 7TV emote name cannot be empty."));
                                return;
                            }

                            SeventvAccountManager::instance()
                                .addEmoteToEditorChannel(
                                    target, emoteID, name,
                                    [target] {
                                        refreshOpenEditorTarget(target);
                                    },
                                    [feedbackChannel, target,
                                     originalName](const QString &error) {
                                        postSevenTVEditorFeedback(
                                            feedbackChannel,
                                            QStringLiteral(
                                                "Failed to add %1 to %2: %3")
                                                .arg(
                                                    originalName,
                                                    editorTargetLabel(target),
                                                    error));
                                    });
                        });
                });
            continue;
        }

        const auto enabledEmote = *entry.enabledEmote;
        state->removeMenu->addAction(
            targetLabel,
            [feedbackChannel = state->feedbackChannel, target,
             emoteID = state->emoteID, enabledName = enabledEmote.name] {
                SeventvAccountManager::instance()
                    .removeEmoteFromEditorChannel(
                        target, emoteID,
                        [target] {
                            refreshOpenEditorTarget(target);
                        },
                        [feedbackChannel, target,
                         enabledName](const QString &error) {
                            postSevenTVEditorFeedback(
                                feedbackChannel,
                                QStringLiteral(
                                    "Failed to remove %1 from %2: %3")
                                    .arg(enabledName,
                                         editorTargetLabel(target), error));
                        });
            });

        state->renameMenu->addAction(
            targetLabel,
            [dialogParent = state->dialogParent,
             feedbackChannel = state->feedbackChannel, target,
             emoteID = state->emoteID, enabledName = enabledEmote.name] {
                QTimer::singleShot(
                    0, dialogParent,
                    [dialogParent, feedbackChannel, target, emoteID,
                     enabledName] {
                        if (dialogParent == nullptr)
                        {
                            return;
                        }
                        bool accepted = false;
                        const auto name =
                            QInputDialog::getText(
                                dialogParent,
                                QStringLiteral("Rename 7TV emote"),
                                QStringLiteral("Rename \"%1\" in %2 to:")
                                    .arg(enabledName,
                                         editorTargetLabel(target)),
                                QLineEdit::Normal, enabledName, &accepted)
                                .trimmed();
                        if (!accepted)
                        {
                            return;
                        }
                        if (name.isEmpty())
                        {
                            postSevenTVEditorFeedback(
                                feedbackChannel,
                                QStringLiteral(
                                    "The 7TV emote name cannot be empty."));
                            return;
                        }

                        SeventvAccountManager::instance()
                            .renameEmoteInEditorChannel(
                                target, emoteID, name,
                                [target] {
                                    refreshOpenEditorTarget(target);
                                },
                                [feedbackChannel, target,
                                 enabledName](const QString &error) {
                                    postSevenTVEditorFeedback(
                                        feedbackChannel,
                                        QStringLiteral(
                                            "Failed to rename %1 in %2: %3")
                                            .arg(enabledName,
                                                 editorTargetLabel(target),
                                                 error));
                                });
                    });
            });
    }

    if (state->addMenu->isEmpty())
    {
        addDisabledMenuMessage(state->addMenu, QStringLiteral(
            "Already enabled in every available channel"));
    }
    if (state->removeMenu->isEmpty())
    {
        addDisabledMenuMessage(
            state->removeMenu,
            QStringLiteral("Not enabled in any available channel"));
    }
    if (state->renameMenu->isEmpty())
    {
        addDisabledMenuMessage(
            state->renameMenu,
            QStringLiteral("Not enabled in any available channel"));
    }
    if (failedLoads > 0)
    {
        const auto text =
            QStringLiteral("%1 channel set(s) could not be loaded")
                .arg(failedLoads);
        addDisabledMenuMessage(state->addMenu, text);
        addDisabledMenuMessage(state->removeMenu, text);
        addDisabledMenuMessage(state->renameMenu, text);
    }
}

void addSevenTVEditorMenu(QMenu *menu, const Emote &emote,
                          QWidget *dialogParent,
                          const ChannelPtr &feedbackChannel)
{
    if (!isSevenTVEmote(emote))
    {
        return;
    }

    auto &manager = SeventvAccountManager::instance();
    if (!manager.isLoggedIn())
    {
        return;
    }

    auto *seventvMenu = menu->addMenu(QStringLiteral("7TV"));
    auto *addMenu = seventvMenu->addMenu(QStringLiteral("Add"));
    auto *removeMenu = seventvMenu->addMenu(QStringLiteral("Remove"));
    auto *renameMenu = seventvMenu->addMenu(QStringLiteral("Rename"));
    const auto showStatus = [addMenu, removeMenu,
                             renameMenu](const QString &text) {
        addDisabledMenuMessage(addMenu, text);
        addDisabledMenuMessage(removeMenu, text);
        addDisabledMenuMessage(renameMenu, text);
    };
    if (manager.isBusy())
    {
        showStatus(QStringLiteral("7TV account is busy"));
        return;
    }
    if (!manager.editorChannelsLoaded())
    {
        showStatus(QStringLiteral("Loading editable channels…"));
        manager.refresh();
        return;
    }

    const auto targets = availableEditorTargets();
    if (targets.empty())
    {
        showStatus(QStringLiteral("No editable channels found"));
        return;
    }

    addDisabledMenuMessage(addMenu, QStringLiteral("Loading channel sets…"));
    addDisabledMenuMessage(removeMenu,
                           QStringLiteral("Loading channel sets…"));
    addDisabledMenuMessage(renameMenu,
                           QStringLiteral("Loading channel sets…"));

    auto state = std::make_shared<SevenTVEditorMenuState>();
    state->addMenu = addMenu;
    state->removeMenu = removeMenu;
    state->renameMenu = renameMenu;
    state->dialogParent = dialogParent;
    state->feedbackChannel = feedbackChannel;
    state->emoteID = emote.id.string;
    state->originalName = emote.name.string;
    state->entries.reserve(targets.size());
    for (const auto &target : targets)
    {
        state->entries.push_back({.target = target});
    }
    state->pending = static_cast<int>(state->entries.size());

    for (size_t index = 0; index < state->entries.size(); ++index)
    {
        manager.loadEditorChannelEmotes(
            state->entries.at(index).target,
            [state, index](std::vector<SeventvManagedEmote> emotes) {
                const auto match = std::ranges::find_if(
                    emotes, [state](const auto &candidate) {
                        return candidate.id == state->emoteID;
                    });
                if (match != emotes.end())
                {
                    state->entries.at(index).enabledEmote = *match;
                }
                if (--state->pending == 0)
                {
                    populateSevenTVEditorMenus(state);
                }
            },
            [state, index](const QString &error) {
                state->entries.at(index).error = error;
                if (--state->pending == 0)
                {
                    populateSevenTVEditorMenus(state);
                }
            });
    }
}

void addEmoteContextMenuItems(QMenu *menu, const Emote &emote,
                              QStringView kind, QWidget *dialogParent,
                              const ChannelPtr &feedbackChannel)
{
    auto *openAction = menu->addAction("&Open");
    auto *openMenu = new QMenu(menu);
    openAction->setMenu(openMenu);

    auto *copyAction = menu->addAction("&Copy");
    auto *copyMenu = new QMenu(menu);
    copyAction->setMenu(copyMenu);

    // Scale of the smallest image
    std::optional<qreal> baseScale;
    // Add copy and open links for images
    auto addImageLink = [&](const ImagePtr &image) {
        if (!image->isEmpty())
        {
            if (!baseScale)
            {
                baseScale = image->scale();
            }

            auto factor =
                QString::number(static_cast<int>(*baseScale / image->scale()));
            copyMenu->addAction("&" + factor + "x link", [url = image->url()] {
                crossPlatformCopy(url.string);
            });
            openMenu->addAction("&" + factor + "x link", [url = image->url()] {
                QDesktopServices::openUrl(QUrl(url.string));
            });
        }
    };

    addImageLink(emote.images.getImage1());
    addImageLink(emote.images.getImage2());
    addImageLink(emote.images.getImage3());

    // Copy and open emote page link
    if (!emote.homePage.string.isEmpty())
    {
        copyMenu->addSeparator();
        openMenu->addSeparator();

        copyMenu->addAction(u"Copy &" % kind % u" link",
                            [url = emote.homePage] {
                                crossPlatformCopy(url.string);
                            });
        openMenu->addAction(u"Open &" % kind % u" link",
                            [url = emote.homePage] {
                                QDesktopServices::openUrl(QUrl(url.string));
                            });
    }

    addSevenTVEditorMenu(menu, emote, dialogParent, feedbackChannel);
}

bool hasPlatformBadgeElement(const Message &message)
{
    return std::ranges::any_of(message.elements, [](const auto &element) {
        return element &&
               element->getFlags().has(MessageElementFlag::PlatformBadge);
    });
}

QColor defaultActivityPlatformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24, 36);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64, 60);
        case MessagePlatform::TikTok:
            return QColor(37, 244, 238, 48);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255, 36);
    }
}

void insertActivityPlatformBadge(Message &message)
{
    if (!message.platformAccentColor)
    {
        message.platformAccentColor =
            defaultActivityPlatformAccent(message.platform);
    }

    const auto badge = MergedChannel::platformBadge(message.platform);
    if (!badge)
    {
        return;
    }

    auto position = message.elements.begin();
    while (position != message.elements.end())
    {
        const auto &element = *position;
        if (!element ||
            !element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            break;
        }

        ++position;
    }

    message.elements.insert(
        position,
        std::make_unique<BadgeElement>(badge, MessageElementFlag::PlatformBadge));
}

QDateTime activityMessageTime(const Message &message)
{
    if (message.serverReceivedTime.isValid())
    {
        return message.serverReceivedTime.toLocalTime();
    }

    auto now = QDateTime::currentDateTime();
    auto fallback = QDateTime(now.date(), message.parseTime);
    if (fallback > now.addSecs(60))
    {
        fallback = fallback.addDays(-1);
    }
    return fallback;
}

void insertActivityTimeElement(Message &message,
                               ActivityTimeDisplayMode timeDisplayMode)
{
    auto position = message.elements.begin();
    while (position != message.elements.end())
    {
        const auto &element = *position;
        if (!element ||
            (!element->getFlags().has(MessageElementFlag::RepliedMessage) &&
             !element->getFlags().has(MessageElementFlag::PlatformBadge)))
        {
            break;
        }

        ++position;
    }

    message.elements.insert(
        position, std::make_unique<ActivityTimeElement>(
                      activityMessageTime(message), timeDisplayMode));
}

void replaceActivityTimeElement(Message &message,
                                ActivityTimeDisplayMode timeDisplayMode)
{
    for (auto &element : message.elements)
    {
        if (dynamic_cast<const TimestampElement *>(element.get()) == nullptr &&
            dynamic_cast<const ActivityTimeElement *>(element.get()) == nullptr)
        {
            continue;
        }

        element = std::make_unique<ActivityTimeElement>(
            activityMessageTime(message), timeDisplayMode);
        return;
    }

    insertActivityTimeElement(message, timeDisplayMode);
}

MessagePtr prepareActivityMessage(const MessagePtr &message,
                                  ActivityTimeDisplayMode timeDisplayMode)
{
    if (!message)
    {
        return message;
    }

    if (timeDisplayMode == ActivityTimeDisplayMode::Timestamp &&
        hasPlatformBadgeElement(*message))
    {
        return message;
    }

    auto copy = message->clone();
    if (!hasPlatformBadgeElement(*copy))
    {
        insertActivityPlatformBadge(*copy);
    }
    if (timeDisplayMode == ActivityTimeDisplayMode::Relative)
    {
        replaceActivityTimeElement(*copy, timeDisplayMode);
    }
    return copy;
}

MessagePtr makeActivityCompactMessage(const MessagePtr &message,
                                      const QString &text,
                                      ActivityTimeDisplayMode timeDisplayMode)
{
    auto compact = message->clone();
    compact->messageText = text;
    compact->searchText = text;
    compact->flags.unset(MessageFlag::Collapsed);
    compact->elements.clear();

    insertActivityPlatformBadge(*compact);
    compact->elements.emplace_back(std::make_unique<ActivityTimeElement>(
        activityMessageTime(*message), timeDisplayMode));

    compact->elements.emplace_back(std::make_unique<TextElement>(
        text, MessageElementFlag::Text, MessageColor::System));
    return compact;
}

struct ActivityCompactGiftSummary {
    QString name;
    int count = 0;
    bool isMembership = false;
};

std::optional<ActivityCompactGiftSummary> activityCompactGiftSummary(
    const Message &message)
{
    static const QRegularExpression REGEX(
        QStringLiteral(
            R"(^(.+?)\s+gifted\s+(\d+)\s+(subscriptions?|subs?|memberships?)$)"),
        QRegularExpression::CaseInsensitiveOption);

    const auto match = REGEX.match(message.messageText.trimmed());
    if (!match.hasMatch())
    {
        return std::nullopt;
    }

    return ActivityCompactGiftSummary{
        .name = match.captured(1).trimmed(),
        .count = match.captured(2).toInt(),
        .isMembership = match.captured(3).contains(
            QStringLiteral("membership"), Qt::CaseInsensitive),
    };
}

bool activityGiftSummariesCanMerge(const Message &previous,
                                   const ActivityCompactGiftSummary &prev,
                                   const Message &next,
                                   const ActivityCompactGiftSummary &cur)
{
    if (previous.platform != next.platform ||
        prev.isMembership != cur.isMembership ||
        prev.name.compare(cur.name, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    if (previous.serverReceivedTime.isValid() &&
        next.serverReceivedTime.isValid())
    {
        auto seconds = previous.serverReceivedTime.secsTo(
            next.serverReceivedTime);
        if (seconds < 0)
        {
            seconds = -seconds;
        }
        return seconds <= 45;
    }

    if (previous.parseTime.isValid() && next.parseTime.isValid())
    {
        auto seconds = previous.parseTime.secsTo(next.parseTime);
        if (seconds < 0)
        {
            seconds = -seconds;
        }
        return seconds <= 45;
    }

    return true;
}

QString activityMergedGiftSummaryText(const ActivityCompactGiftSummary &summary,
                                      int count)
{
    const auto unit = summary.isMembership
                          ? (count == 1 ? QStringLiteral("membership")
                                        : QStringLiteral("memberships"))
                          : (count == 1 ? QStringLiteral("sub")
                                        : QStringLiteral("subs"));
    return QStringLiteral("%1 gifted %2 %3")
        .arg(summary.name, QString::number(count), unit);
}

void addImageContextMenuItems(QMenu *menu,
                              const MessageLayoutElement *hoveredElement,
                              QWidget *dialogParent,
                              const ChannelPtr &feedbackChannel)
{
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &creator = hoveredElement->getCreator();
    auto creatorFlags = creator.getFlags();

    // Badge actions
    if (creatorFlags.hasAny({MessageElementFlag::Badges}))
    {
        if (const auto *badgeElement =
                dynamic_cast<const BadgeElement *>(&creator))
        {
            addEmoteContextMenuItems(menu, *badgeElement->getEmote(), u"badge",
                                     dialogParent, feedbackChannel);
        }
    }

    // Emote actions
    if (creatorFlags.hasAny(
            {MessageElementFlag::EmoteImage, MessageElementFlag::EmojiImage}))
    {
        if (const auto *emoteElement =
                dynamic_cast<const EmoteElement *>(&creator))
        {
            addEmoteContextMenuItems(menu, *emoteElement->getEmote(), u"emote",
                                     dialogParent, feedbackChannel);
        }
        else if (const auto *layeredElement =
                     dynamic_cast<const LayeredEmoteElement *>(&creator))
        {
            // Give each emote its own submenu
            for (auto &emote : layeredElement->getUniqueEmotes())
            {
                auto *emoteAction = menu->addAction(emote.ptr->name.string);
                auto *emoteMenu = new QMenu(menu);
                emoteAction->setMenu(emoteMenu);
                addEmoteContextMenuItems(emoteMenu, *emote.ptr, u"emote",
                                         dialogParent, feedbackChannel);
            }
        }
    }

    // add seperator
    if (!menu->actions().empty())
    {
        menu->addSeparator();
    }
}

void addLinkContextMenuItems(QMenu *menu,
                             const MessageLayoutElement *hoveredElement)
{
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &link = hoveredElement->getLink();

    if (link.type != Link::Url)
    {
        return;
    }

    // Link copy
    QString url = link.value;

    // open link
    menu->addAction("&Open link", [url] {
        QDesktopServices::openUrl(QUrl(url));
    });
    // open link default
    if (supportsIncognitoLinks())
    {
        menu->addAction("Open link &incognito", [url] {
            openLinkIncognito(url);
        });
    }
    menu->addAction("&Copy link", [url] {
        crossPlatformCopy(url);
    });

    menu->addSeparator();
}

void addHiddenContextMenuItems(QMenu *menu,
                               const MessageLayoutElement * /*hoveredElement*/,
                               const MessageLayoutPtr &layout,
                               QMouseEvent *event)
{
    if (!layout)
    {
        return;
    }

    if (event->modifiers() != Qt::ShiftModifier)
    {
        // NOTE: We currently require the modifier to be ONLY shift - we might want to check if shift is among the modifiers instead
        return;
    }

    if (!layout->getMessage()->id.isEmpty())
    {
        menu->addAction("Copy message &ID",
                        [messageID = layout->getMessage()->id] {
                            crossPlatformCopy(messageID);
                        });
    }

    auto message = layout->getMessagePtr();

    if (message)
    {
        menu->addAction("Copy message &JSON", [message] {
            auto jsonString = QJsonDocument{message->toJson()}.toJson(
                QJsonDocument::Indented);
            crossPlatformCopy(QString::fromUtf8(jsonString));
        });
    }
}

// Current function: https://www.desmos.com/calculator/vdyamchjwh
qreal highlightEasingFunction(qreal progress)
{
    if (progress <= 0.1)
    {
        return 1.0 - pow(10.0 * progress, 3.0);
    }
    return 1.0 + pow((20.0 / 9.0) * (0.5 * progress - 0.5), 3.0);
}

float getTooltipScale(EmoteTooltipScale emoteTooltipScale)
{
    switch (emoteTooltipScale)
    {
        case EmoteTooltipScale::Small:
            return 0.5F;
        case EmoteTooltipScale::Medium:
            return 1.0F;
        case EmoteTooltipScale::Large:
            return 1.5F;
        case EmoteTooltipScale::Huge:
            return 2.0F;

        default:
            return 1.0F;
    }
}

}  // namespace

namespace chatterino {

ChannelView::ChannelView(QWidget *parent, Context context, size_t messagesLimit)
    : ChannelView(InternalCtor{}, parent, nullptr, context, messagesLimit)
{
}

ChannelView::ChannelView(QWidget *parent, Split *split, Context context,
                         size_t messagesLimit)
    : ChannelView(InternalCtor{}, parent, split, context, messagesLimit)
{
    assert(parent != nullptr && split != nullptr &&
           "This constructor should only be used with non-null values (see "
           "documentation)");
}

ChannelView::ChannelView(InternalCtor /*tag*/, QWidget *parent, Split *split,
                         Context context, size_t messagesLimit)
    : BaseWidget(parent)
    , channel_(Channel::getEmpty())
    , split_(split)
    , scrollBar_(new Scrollbar(messagesLimit, this))
    , highlightAnimation_(this)
    , context_(context)
    , messages_(messagesLimit)
    , tooltipWidget_(new TooltipWidget(this))
{
    this->setMouseTracking(true);

    this->initializeLayout();
    this->initializeScrollbar();
    this->initializeSignals();

    this->cursors_.neutral =
        QCursor(getResources().scrolling.neutralScroll);
    this->cursors_.up = QCursor(getResources().scrolling.upScroll);
    this->cursors_.down = QCursor(getResources().scrolling.downScroll);

    this->pauseTimer_.setSingleShot(true);
    QObject::connect(&this->pauseTimer_, &QTimer::timeout, this, [this] {
        // remove elements that are finite
        std::erase_if(this->pauses_, [](const auto &p) {
            return p.second.has_value();
        });

        this->updatePauses();
    });

    this->slowChatTimer_.setSingleShot(true);
    QObject::connect(&this->slowChatTimer_, &QTimer::timeout, this, [this] {
        this->drainSlowChatQueue();
    });

    this->activityTimeRefreshTimer_.setInterval(1000);
    QObject::connect(&this->activityTimeRefreshTimer_, &QTimer::timeout, this,
                     [this] {
                         if (!this->shouldRefreshActivityTime())
                         {
                             this->activityTimeRefreshTimer_.stop();
                             return;
                         }

                         this->invalidateBuffers();
                     });

    this->messageLayoutShiftAnimationTimer_.setInterval(16);
    QObject::connect(&this->messageLayoutShiftAnimationTimer_, &QTimer::timeout,
                     this, [this] {
                         const auto now = SteadyClock::now();
                         std::erase_if(
                             this->messageLayoutShiftAnimations_,
                             [now](const auto &animation) {
                                 return now - animation.startedAt >=
                                        MESSAGE_LAYOUT_SHIFT_ANIMATION_DURATION;
                             });

                         if (this->messageLayoutShiftAnimations_.empty())
                         {
                             this->messageLayoutShiftAnimationTimer_.stop();
                         }

                         this->queueUpdate();
                     });

    this->messageArrivalAnimationTimer_.setInterval(16);
    QObject::connect(&this->messageArrivalAnimationTimer_, &QTimer::timeout,
                     this, [this] {
                         const auto now = SteadyClock::now();
                         std::erase_if(
                             this->messageArrivalAnimations_,
                             [now](const auto &animation) {
                                 return now - animation.startedAt >=
                                        SLOW_CHAT_MESSAGE_ANIMATION_DURATION;
                             });

                         if (this->messageArrivalAnimations_.empty())
                         {
                             this->messageArrivalAnimationTimer_.stop();
                         }

                         this->queueUpdate();
                     });

    // This shortcut is not used in splits, it's used in views that
    // don't have a SplitInput like the SearchPopup or EmotePopup.
    // See SplitInput::installKeyPressedEvent for the copy event
    // from views with a SplitInput.
    auto *shortcut = new QShortcut(QKeySequence::StandardKey::Copy, this);
    QObject::connect(shortcut, &QShortcut::activated, [this] {
        this->copySelectedText();
    });

    this->clickTimer_.setSingleShot(true);
    this->clickTimer_.setInterval(500);

    this->scrollTimer_.setInterval(20);
    QObject::connect(&this->scrollTimer_, &QTimer::timeout, this, [this] {
        this->scrollUpdateRequested();
    });

    this->grabGesture(Qt::PanGesture);

    // TODO: Figure out if we need this, and if so, why
    // StrongFocus means we can focus this event through clicking it
    // and tabbing to it from another widget. I don't currently know
    // of any place where you can, or where it would make sense,
    // to tab to a ChannelVieChannelView
    this->setFocusPolicy(Qt::FocusPolicy::ClickFocus);

    this->setupHighlightAnimationColors();
    this->highlightAnimation_.setDuration(1500);
    auto curve = QEasingCurve();
    curve.setCustomType(highlightEasingFunction);
    this->highlightAnimation_.setEasingCurve(curve);
    QObject::connect(&this->highlightAnimation_,
                     &QVariantAnimation::valueChanged, this, [this] {
                         this->queueUpdate();
                     });

    this->refreshMessageColors();
    this->messagePreferences_.connectSettings(getSettings(),
                                              this->signalHolder_);
}

void ChannelView::initializeLayout()
{
    this->goToBottom_ = new LabelButton("More messages below", this);
    this->goToBottom_->setStyleSheet(
        "background-color: rgba(0,0,0,0.66); color: #FFF;");
    this->goToBottom_->setVisible(false);

    QObject::connect(this->goToBottom_, &Button::leftClicked, this, [this] {
        QTimer::singleShot(180, this, [this] {
            this->scrollBar_->scrollToBottom(
                getSettings()->enableSmoothScrollingNewMessages.getValue());
        });
    });
}

void ChannelView::initializeScrollbar()
{
    // We can safely ignore the scroll bar's signal connection since the scroll bar will
    // always be destroyed before the ChannelView
    std::ignore = this->scrollBar_->getCurrentValueChanged().connect([this] {
        if (this->isVisible())
        {
            this->performLayout(true);
            this->queueUpdate();
        }
        else
        {
            this->layoutQueued_ = true;
        }
    });
}

void ChannelView::initializeSignals()
{
    this->signalHolder_.managedConnect(getApp()->getWindows()->wordFlagsChanged,
                                       [this] {
                                           this->queueLayout();
                                           this->update();
                                       });

    getSettings()->showLastMessageIndicator.connect(
        [this](auto, auto) {
            this->update();
        },
        this->signalHolder_);

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->gifRepaintRequested, [&] {
            if (!this->animationArea_.isEmpty())
            {
                this->queueUpdate(this->animationArea_);
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->layoutRequested, [&](Channel *channel) {
            if (this->isVisible() &&
                (channel == nullptr ||
                 this->underlyingChannel_.get() == channel))
            {
                this->queueLayout();
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->invalidateBuffersRequested,
        [this](Channel *channel) {
            if (this->isVisible() &&
                (channel == nullptr ||
                 this->underlyingChannel_.get() == channel))
            {
                this->invalidateBuffers();
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->reloadMessagesRequested,
        [this](Channel *channel) {
            if (this->isVisible() &&
                (channel == nullptr ||
                 this->underlyingChannel_.get() == channel))
            {
                this->messagesUpdated();
            }
        });

    this->signalHolder_.managedConnect(getApp()->getFonts()->fontChanged,
                                       [this] {
                                           this->queueLayout();
                                       });
}

Scrollbar *ChannelView::scrollbar()
{
    return this->scrollBar_;
}

bool ChannelView::pausable() const
{
    return this->pausable_;
}

void ChannelView::setPausable(bool value)
{
    this->pausable_ = value;
}

bool ChannelView::paused() const
{
    /// No elements in the map -> not paused
    return this->pausable() && !this->pauses_.empty();
}

void ChannelView::pause(PauseReason reason, std::optional<uint> msecs)
{
    bool wasUnpaused = !this->paused();

    if (msecs)
    {
        /// Msecs has a value
        auto timePoint = SteadyClock::now() + std::chrono::milliseconds(*msecs);
        auto it = this->pauses_.find(reason);

        if (it == this->pauses_.end())
        {
            /// No value found so we insert a new one.
            this->pauses_[reason] = timePoint;
        }
        else
        {
            /// If the new time point is newer then we override.
            auto &previousTimePoint = it->second;
            if (previousTimePoint.has_value() &&
                previousTimePoint.value() < timePoint)
            {
                previousTimePoint = timePoint;
            }
        }
    }
    else
    {
        /// Msecs is none -> pause is infinite.
        /// We just override the value.
        this->pauses_[reason] = std::nullopt;
    }

    this->updatePauses();

    if (wasUnpaused)
    {
        this->update();
    }
}

void ChannelView::unpause(PauseReason reason)
{
    if (this->pauses_.erase(reason) > 0)
    {
        this->updatePauses();
    }
}

void ChannelView::updatePauses()
{
    using namespace std::chrono;

    if (this->pauses_.empty())
    {
        this->unpaused();

        /// No pauses so we can stop the timer
        this->pauseEnd_ = std::nullopt;
        this->pauseTimer_.stop();

        this->scrollBar_->offsetMaximum(this->pauseScrollMaximumOffset_);
        this->scrollBar_->offsetMinimum(this->pauseScrollMinimumOffset_);
        this->pauseScrollMinimumOffset_ = 0;
        this->pauseScrollMaximumOffset_ = 0;

        this->queueLayout();
        // make sure we re-render
        this->update();
    }
    else if (std::any_of(this->pauses_.begin(), this->pauses_.end(),
                         [](auto &&value) {
                             return !value.second;
                         }))
    {
        /// Some of the pauses are infinite
        this->pauseEnd_ = std::nullopt;
        this->pauseTimer_.stop();
    }
    else
    {
        /// Get the maximum pause
        auto pauseEnd =
            std::max_element(this->pauses_.begin(), this->pauses_.end(),
                             [](auto &&a, auto &&b) {
                                 return a.second > b.second;
                             })
                ->second.value();

        if (pauseEnd != this->pauseEnd_)
        {
            /// Start the timer
            this->pauseEnd_ = pauseEnd;
            auto duration =
                duration_cast<milliseconds>(pauseEnd - SteadyClock::now());
            this->pauseTimer_.start(std::max(duration, 0ms));
        }
    }
}

void ChannelView::unpaused()
{
    /// Move selection
    this->selection_.shiftMessageIndex(this->pauseSelectionOffset_);
    this->doubleClickSelection_.shiftMessageIndex(this->pauseSelectionOffset_);

    this->pauseSelectionOffset_ = 0;
}

void ChannelView::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();

    this->setupHighlightAnimationColors();
    this->refreshMessageColors();
    this->invalidateBuffers();
}

void ChannelView::updateColorTheme()
{
    this->themeChangedEvent();
}

void ChannelView::refreshPlatformIndicatorMode()
{
    if (this->underlyingChannel_)
    {
        this->refreshMessages();
    }
    else
    {
        this->messagesUpdated();
    }
    this->queueUpdate();
}

void ChannelView::refreshActivityTimeDisplayMode()
{
    this->messagesUpdated();
    this->updateActivityTimeRefreshTimer();
    this->queueUpdate();
}

void ChannelView::setIsOverlay(bool isOverlay)
{
    this->isOverlay_ = isOverlay;
    this->themeChangedEvent();
}

void ChannelView::setupHighlightAnimationColors()
{
    this->highlightAnimation_.setStartValue(
        this->theme->messages.highlightAnimationStart);
    this->highlightAnimation_.setEndValue(
        this->theme->messages.highlightAnimationEnd);
}

void ChannelView::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);

    if (this->goToBottom_)
    {
        auto factor = this->scale();
        this->goToBottom_->setFont(
            getApp()->getFonts()->getFont(FontStyle::UiMedium, factor));
    }
}

void ChannelView::queueUpdate()
{
    this->update();
}

void ChannelView::queueUpdate(const QRect &area)
{
    this->update(area);
}

void ChannelView::invalidateBuffers()
{
    this->bufferInvalidationQueued_ = true;
    this->queueLayout();
    this->update();
}

void ChannelView::queueLayout()
{
    if (this->isVisible())
    {
        this->performLayout();
    }
    else
    {
        this->layoutQueued_ = true;
    }
}

void ChannelView::showEvent(QShowEvent * /*event*/)
{
    if (this->layoutQueued_)
    {
        this->performLayout(false, true);
    }
    this->updateActivityTimeRefreshTimer();
}

void ChannelView::performLayout(bool causedByScrollbar, bool causedByShow)
{
    // BenchmarkGuard benchmark("layout");

    this->layoutQueued_ = false;

    /// Get messages and check if there are at least 1
    const auto &messages = this->getMessagesSnapshot();

    this->showingLatestMessages_ =
        this->scrollBar_->isAtBottom() ||
        (!this->scrollBar_->isVisible() && !causedByScrollbar);

    const bool shouldAnimateLayoutShifts =
        this->isVisible() && this->shouldAnimateMessageAnimations() &&
        this->messageArrivalAnimations_.empty() && !causedByScrollbar;

    struct VisibleMessagePosition {
        MessageLayoutPtr layout;
        qreal y;
    };
    auto collectVisibleMessagePositions =
        [this, &messages]() -> std::vector<VisibleMessagePosition> {
        const auto relativeCurrentValue =
            this->scrollBar_->getRelativeCurrentValue();
        const auto start = size_t(relativeCurrentValue);
        std::vector<VisibleMessagePosition> positions;
        if (messages.size() <= start)
        {
            return positions;
        }

        auto y =
            -(messages[start]->getHeight() * (fmod(relativeCurrentValue, 1)));
        for (auto i = start; i < messages.size() && y <= this->height(); i++)
        {
            positions.push_back({messages[i], y});
            y += messages[i]->getHeight();
        }
        return positions;
    };
    const auto oldVisiblePositions =
        shouldAnimateLayoutShifts ? collectVisibleMessagePositions()
                                  : std::vector<VisibleMessagePosition>{};

    /// Layout visible messages
    const auto redrawRequired = this->layoutVisibleMessages(messages);

    /// Update scrollbar
    this->updateScrollbar(messages, causedByScrollbar, causedByShow);

    if (redrawRequired)
    {
        if (shouldAnimateLayoutShifts)
        {
            std::unordered_map<MessageLayout *, qreal> oldYByLayout;
            oldYByLayout.reserve(oldVisiblePositions.size());
            for (const auto &position : oldVisiblePositions)
            {
                oldYByLayout.emplace(position.layout.get(), position.y);
            }

            for (const auto &position : collectVisibleMessagePositions())
            {
                const auto it = oldYByLayout.find(position.layout.get());
                if (it == oldYByLayout.end())
                {
                    continue;
                }

                this->startOrUpdateMessageLayoutShiftAnimation(
                    position.layout, it->second, position.y);
            }
        }

        this->queueUpdate();
    }

    this->goToBottom_->setVisible(this->enableScrollingToBottom_ &&
                                  this->scrollBar_->isVisible() &&
                                  !this->scrollBar_->isAtBottom());
}

bool ChannelView::layoutVisibleMessages(
    const std::vector<MessageLayoutPtr> &messages)
{
    const auto relativeCurrentValue = this->scrollBar_->getRelativeCurrentValue();
    const auto start = size_t(relativeCurrentValue);
    const auto layoutWidth = this->getLayoutWidth();
    const auto flags = this->getFlags();
    auto redrawRequired = false;

    if (messages.size() > start)
    {
        auto y = -(messages[start]->getHeight() * (fmod(relativeCurrentValue, 1)));

        for (auto i = start; i < messages.size() && y <= this->height(); i++)
        {
            const auto &message = messages[i];

            redrawRequired |= message->layout(
                {
                    .messageColors = this->effectiveMessageColors(),
                    .flags = flags,
                    .width = layoutWidth,
                    .scale = this->activityMessageScale(),
                    .imageScale = this->activityMessageImageScale(),
                },
                this->bufferInvalidationQueued_);

            y += message->getHeight();
        }
        this->bufferInvalidationQueued_ = false;
    }

    return redrawRequired;
}

void ChannelView::updateScrollbar(const std::vector<MessageLayoutPtr> &messages,
                                  bool causedByScrollbar, bool causedByShow)
{
    if (messages.size() == 0)
    {
        this->scrollBar_->setVisible(false);
        return;
    }

    /// Layout the messages at the bottom
    qreal h = this->height() - BOTTOM_MESSAGE_PADDING;
    auto flags = this->getFlags();
    auto layoutWidth = this->getLayoutWidth();
    auto showScrollbar = false;

    // convert i to int since it checks >= 0
    for (auto i = int(messages.size()) - 1; i >= 0; i--)
    {
        auto *message = messages[i].get();

        message->layout(
            {
                .messageColors = this->effectiveMessageColors(),
                .flags = flags,
                .width = layoutWidth,
                .scale = this->activityMessageScale(),
                .imageScale = this->activityMessageImageScale(),
            },
            false);

        h -= message->getHeight();

        if (h < 0)  // break condition
        {
            this->scrollBar_->setPageSize(
                static_cast<qreal>(messages.size() - i) +
                (h / std::max(1, message->getHeight())));

            showScrollbar = true;
            break;
        }
    }

    /// Update scrollbar values
    this->scrollBar_->setVisible(showScrollbar);

    if (!showScrollbar && !causedByScrollbar)
    {
        this->scrollBar_->scrollToTop();
    }
    this->showScrollBar_ = showScrollbar;

    // If we were showing the latest messages and the scrollbar now wants to be
    // rendered, scroll to bottom
    if (this->enableScrollingToBottom_ && this->showingLatestMessages_ &&
        showScrollbar && !causedByScrollbar)
    {
        this->scrollBar_->scrollToBottom(
            !causedByShow &&
            getSettings()->enableSmoothScrollingNewMessages.getValue());
    }
}

void ChannelView::clearMessages()
{
    this->pendingSlowChatMessages_.clear();
    this->nextSlowChatMessageAt_.reset();
    this->slowChatTimer_.stop();
    this->notifySlowChatQueueCountChanged();
    this->clearMessageLayoutShiftAnimations();
    this->clearMessageArrivalAnimations();

    // Clear all stored messages in this chat widget
    this->messages_.clear();
    this->scrollBar_->clearHighlights();
    this->scrollBar_->resetBounds();
    this->scrollBar_->setPageSize(0);
    this->scrollBar_->setMaximum(0);
    this->scrollBar_->setMinimum(0);
    this->scrollBar_->scrollToTop(false);
    this->queueLayout();
    this->update();

    this->lastMessageHasAlternateBackground_ = false;
    this->lastMessageHasAlternateBackgroundReverse_ = true;
    this->activityGiftRecipientsToSuppress_ = 0;
    this->activitySuppressNextAnnouncementMessage_ = false;
}

Scrollbar &ChannelView::getScrollBar()
{
    return *this->scrollBar_;
}

QString ChannelView::getSelectedText()
{
    QString result = "";

    std::vector<MessageLayoutPtr> &messagesSnapshot =
        this->getMessagesSnapshot();

    Selection selection = this->selection_;

    if (selection.isEmpty())
    {
        return result;
    }

    const auto numMessages = messagesSnapshot.size();
    const auto indexStart = selection.selectionMin.messageIndex;
    const auto indexEnd = selection.selectionMax.messageIndex;

    if (indexEnd >= numMessages || indexStart >= numMessages)
    {
        // One of our messages is out of bounds
        return result;
    }

    for (auto msg = indexStart; msg <= indexEnd; msg++)
    {
        MessageLayoutPtr layout = messagesSnapshot[msg];
        auto from = msg == selection.selectionMin.messageIndex
                        ? selection.selectionMin.charIndex
                        : 0;
        auto to = msg == selection.selectionMax.messageIndex
                      ? selection.selectionMax.charIndex
                      : layout->getLastCharacterIndex() + 1;

        layout->addSelectionText(result, from, to);

        if (msg != indexEnd)
        {
            result += '\n';
        }
    }

    return result;
}

bool ChannelView::hasSelection()
{
    return !this->selection_.isEmpty();
}

void ChannelView::clearSelection()
{
    this->selection_ = Selection();
    this->queueLayout();
}

void ChannelView::copySelectedText()
{
    crossPlatformCopy(this->getSelectedText());
}

void ChannelView::setEnableScrollingToBottom(bool value)
{
    this->enableScrollingToBottom_ = value;
}

bool ChannelView::getEnableScrollingToBottom() const
{
    return this->enableScrollingToBottom_;
}

void ChannelView::setOverrideFlags(std::optional<MessageElementFlags> value)
{
    this->overrideFlags_ = value;
}

const std::optional<MessageElementFlags> &ChannelView::getOverrideFlags() const
{
    return this->overrideFlags_;
}

std::vector<MessageLayoutPtr> &ChannelView::getMessagesSnapshot()
{
    this->snapshotGuard_.guard();
    if (!this->paused() /*|| this->scrollBar_->isVisible()*/)
    {
        this->snapshot_ = this->messages_.getSnapshot();
    }

    return this->snapshot_;
}

ChannelPtr ChannelView::channel() const
{
    assert(this->channel_ != nullptr);

    return this->channel_;
}

ChannelPtr ChannelView::underlyingChannel() const
{
    return this->underlyingChannel_;
}

bool ChannelView::showScrollbarHighlights() const
{
    return this->channel_->getType() != Channel::Type::TwitchMentions;
}

void ChannelView::refreshMessageColors()
{
    this->messageColors_.applyTheme(getTheme(), this->isOverlay_,
                                    getSettings()->overlayBackgroundOpacity);
    this->activityMessageColors_ = this->messageColors_;
    this->activityMessageColors_.systemText =
        blendColorTowards(this->activityMessageColors_.regularText,
                          this->activityMessageColors_.channelBackground, 0.10);
}

bool ChannelView::isActivityPaneView() const
{
    return this->split_ != nullptr && this->split_->isActivityPane();
}

bool ChannelView::shouldRefreshActivityTime() const
{
    return this->isVisible() && this->isActivityPaneView() &&
           this->resolvedActivityTimeDisplayMode() ==
               ActivityTimeDisplayMode::Relative;
}

void ChannelView::updateActivityTimeRefreshTimer()
{
    if (this->shouldRefreshActivityTime())
    {
        if (!this->activityTimeRefreshTimer_.isActive())
        {
            this->activityTimeRefreshTimer_.start();
        }
        return;
    }

    this->activityTimeRefreshTimer_.stop();
}

const MessageColors &ChannelView::effectiveMessageColors() const
{
    return this->isActivityPaneView() ? this->activityMessageColors_
                                      : this->messageColors_;
}

float ChannelView::activityMessageScale() const
{
    const auto multiplier = this->isActivityPaneView() && this->split_ != nullptr
                                ? static_cast<float>(
                                      this->split_->activityMessageScale())
                                : 1.0F;
    return this->scale() * multiplier;
}

float ChannelView::activityMessageImageScale() const
{
    return this->activityMessageScale() *
           static_cast<float>(this->devicePixelRatio());
}

PlatformIndicatorMode ChannelView::resolvedPlatformIndicatorMode() const
{
    if (this->split_ != nullptr)
    {
        return this->split_->platformIndicatorMode();
    }

    return getSettings()->mergedPlatformIndicatorMode.getEnum();
}

ActivityTimeDisplayMode ChannelView::resolvedActivityTimeDisplayMode() const
{
    if (this->split_ != nullptr)
    {
        return this->split_->activityTimeDisplayMode();
    }

    return ActivityTimeDisplayMode::Relative;
}

ScrollbarHighlight ChannelView::scrollbarHighlightForMessage(
    const MessagePtr &message) const
{
    return message->getScrollBarHighlight(this->resolvedPlatformIndicatorMode(),
                                          this->isActivityPaneView());
}

std::optional<MessagePtr> ChannelView::transformActivityMessage(
    const MessagePtr &message, int &pendingGiftRecipients,
    bool &suppressNextAnnouncementMessage) const
{
    if (!this->isActivityPaneView())
    {
        return message;
    }

    if (message->flags.has(MessageFlag::RecentMessage))
    {
        return std::nullopt;
    }

    if (suppressNextAnnouncementMessage)
    {
        suppressNextAnnouncementMessage = false;
        if (isActivityTwitchAnnouncementFollowupMessage(*message))
        {
            return std::nullopt;
        }
    }

    if (isActivityTwitchAnnouncementHeaderMessage(*message))
    {
        suppressNextAnnouncementMessage = true;
        return std::nullopt;
    }

    const bool collapseGiftedSubscriptions =
        this->split_ == nullptr ||
        this->split_->collapseGiftedSubscriptions();
    if (const auto recipientCount = getActivityGiftBombRecipientCount(*message))
    {
        if (!shouldShowMessageInActivityPane(
                *message,
                this->split_ ? this->split_->twitchActivityMinimumBits() : 100,
                this->split_ ? this->split_->kickActivityMinimumKicks() : 100,
                this->split_ ? this->split_->tiktokActivityMinimumDiamonds()
                             : 0))
        {
            return std::nullopt;
        }

        pendingGiftRecipients = std::max(0, *recipientCount);
        if (!collapseGiftedSubscriptions)
        {
            if (message->giftedSubscriptionRecipientCount > 0)
            {
                auto expanded = message->clone();
                expanded->flags.unset(MessageFlag::Collapsed);
                return prepareActivityMessage(
                    expanded, this->resolvedActivityTimeDisplayMode());
            }

            // Twitch and YouTube gift bombs are followed by one recipient
            // message per gifted subscription. Hide the summary so the
            // recipients themselves become the activity entries.
            return std::nullopt;
        }
        return makeActivityCompactMessage(message,
                                          compactActivityGiftBombText(*message),
                                          this->resolvedActivityTimeDisplayMode());
    }

    if (isActivityGiftRecipientMessage(*message))
    {
        if (pendingGiftRecipients > 0)
        {
            pendingGiftRecipients--;
        }
        if (collapseGiftedSubscriptions)
        {
            return std::nullopt;
        }
        return prepareActivityMessage(
            message, this->resolvedActivityTimeDisplayMode());
    }

    if (!shouldShowMessageInActivityPane(
            *message,
            this->split_ ? this->split_->twitchActivityMinimumBits() : 100,
            this->split_ ? this->split_->kickActivityMinimumKicks() : 100,
            this->split_ ? this->split_->tiktokActivityMinimumDiamonds() : 0))
    {
        return std::nullopt;
    }

    return prepareActivityMessage(message,
                                  this->resolvedActivityTimeDisplayMode());
}

bool ChannelView::shouldDecorateStreamDatabaseMessage() const
{
    if (this->isActivityPaneView())
    {
        return false;
    }

    const auto channel =
        this->underlyingChannel_ != nullptr ? this->underlyingChannel_
                                            : this->channel_;
    return channel != nullptr && channel->getType() == Channel::Type::Twitch &&
           channel->getName().compare(QStringLiteral("streamdatabase"),
                                      Qt::CaseInsensitive) == 0;
}

MessagePtr ChannelView::decorateStreamDatabaseMessage(
    const MessagePtr &message) const
{
    if (!this->shouldDecorateStreamDatabaseMessage() || !message)
    {
        return message;
    }

    const bool needsAccent = !message->platformAccentColor &&
                             !message->flags.has(MessageFlag::DoNotLog);
    const bool needsBadge = !message->flags.has(MessageFlag::System) &&
                            !hasPlatformBadgeElement(*message);
    if (!needsAccent && !needsBadge)
    {
        return message;
    }

    auto copy = message->clone();
    if (needsAccent)
    {
        copy->platformAccentColor = defaultActivityPlatformAccent(copy->platform);
    }
    if (needsBadge)
    {
        insertActivityPlatformBadge(*copy);
    }
    return copy;
}

bool ChannelView::tryMergeActivityGiftMessage(const MessagePtr &message)
{
    if (this->split_ != nullptr &&
        !this->split_->collapseGiftedSubscriptions())
    {
        return false;
    }

    const auto currentSummary = activityCompactGiftSummary(*message);
    if (!currentSummary)
    {
        return false;
    }

    const auto lastLayout = this->messages_.last();
    if (!lastLayout)
    {
        return false;
    }

    const auto previousMessage = (*lastLayout)->getMessagePtr();
    const auto previousSummary = activityCompactGiftSummary(*previousMessage);
    if (!previousSummary ||
        !activityGiftSummariesCanMerge(*previousMessage, *previousSummary,
                                       *message, *currentSummary))
    {
        return false;
    }

    const auto mergedCount = previousSummary->count + currentSummary->count;
    auto mergedMessage = makeActivityCompactMessage(
        previousMessage,
        activityMergedGiftSummaryText(*previousSummary, mergedCount),
        this->resolvedActivityTimeDisplayMode());
    auto mergedLayout = std::make_shared<MessageLayout>(mergedMessage);

    if ((*lastLayout)->flags.has(MessageLayoutFlag::AlternateBackground))
    {
        mergedLayout->flags.set(MessageLayoutFlag::AlternateBackground);
    }
    if ((*lastLayout)->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        mergedLayout->flags.set(MessageLayoutFlag::IgnoreHighlights);
    }

    const auto index = this->messages_.size() - 1;
    if (this->showScrollbarHighlights())
    {
        this->scrollBar_->replaceHighlight(
            index, this->scrollbarHighlightForMessage(mergedMessage));
    }
    this->messages_.replaceItem(index, mergedLayout);
    return true;
}

void ChannelView::rebuildActivityMessages()
{
    this->clearMessageLayoutShiftAnimations();
    this->clearMessageArrivalAnimations();
    auto snapshot = this->channel_->getMessageSnapshot();

    this->messages_.clear();
    this->scrollBar_->clearHighlights();
    this->scrollBar_->resetBounds();
    this->scrollBar_->setMaximum(0);
    this->scrollBar_->setMinimum(0);
    this->lastMessageHasAlternateBackground_ = false;
    this->lastMessageHasAlternateBackgroundReverse_ = true;

    int pendingGiftRecipients = 0;
    bool suppressNextAnnouncementMessage = false;
    size_t nMessagesAdded = 0;
    for (const auto &msg : snapshot)
    {
        const auto transformed = this->transformActivityMessage(
            msg, pendingGiftRecipients, suppressNextAnnouncementMessage);
        if (!transformed)
        {
            continue;
        }

        if (this->tryMergeActivityGiftMessage(*transformed))
        {
            continue;
        }

        auto messageLayout = std::make_shared<MessageLayout>(*transformed);

        if (this->lastMessageHasAlternateBackground_)
        {
            messageLayout->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        this->lastMessageHasAlternateBackground_ =
            !this->lastMessageHasAlternateBackground_;

        if (this->channel_->shouldIgnoreHighlights())
        {
            messageLayout->flags.set(MessageLayoutFlag::IgnoreHighlights);
        }

        this->messages_.pushBack(messageLayout);
        nMessagesAdded++;
        if (this->showScrollbarHighlights())
        {
            this->scrollBar_->addHighlight(
                this->scrollbarHighlightForMessage(*transformed));
        }
    }

    this->activityGiftRecipientsToSuppress_ = pendingGiftRecipients;
    this->activitySuppressNextAnnouncementMessage_ =
        suppressNextAnnouncementMessage;
    this->scrollBar_->setMaximum(
        static_cast<qreal>(std::min(nMessagesAdded, this->messages_.limit())));
    this->queueLayout();
}

void ChannelView::enqueueOrAddProxyMessage(
    const MessagePtr &message, std::optional<MessageFlags> overridingFlags,
    bool emitAddedToChannel)
{
    if (!this->shouldQueueSlowChatMessages())
    {
        this->appendProxyMessage(message, overridingFlags, emitAddedToChannel);
        return;
    }

    const auto priority = slowChatQueuePriority(message);
    if (priority == SlowChatQueuePriority::Moderator)
    {
        this->appendProxyMessage(message, overridingFlags, emitAddedToChannel);
        return;
    }

    const auto now = SteadyClock::now();
    if (this->pendingSlowChatMessages_.empty() &&
        (!this->nextSlowChatMessageAt_ || now >= *this->nextSlowChatMessageAt_))
    {
        const auto rate = std::max<qreal>(
            0.01, this->split_->slowerChatMessagesPerSecond());
        this->nextSlowChatMessageAt_ =
            now + std::chrono::milliseconds(static_cast<int>(
                      std::round(1000.0 / static_cast<double>(rate))));
        this->appendProxyMessage(message, overridingFlags, emitAddedToChannel);
        return;
    }

    PendingSlowChatMessage pending{message, overridingFlags, emitAddedToChannel};
    if (priority == SlowChatQueuePriority::Vip)
    {
        this->pendingSlowChatMessages_.push_front(std::move(pending));
    }
    else
    {
        this->pendingSlowChatMessages_.push_back(std::move(pending));
    }
    this->notifySlowChatQueueCountChanged();
    this->scheduleSlowChatDrain();
}

void ChannelView::appendProxyMessage(const MessagePtr &message,
                                     std::optional<MessageFlags> overridingFlags,
                                     bool emitAddedToChannel)
{
    auto proxyMessage = this->decorateStreamDatabaseMessage(message);
    this->channel_->addMessage(proxyMessage, MessageContext::Repost,
                               overridingFlags);
    if (emitAddedToChannel)
    {
        auto emittedMessage = proxyMessage;
        this->messageAddedToChannel(emittedMessage);
    }
}

bool ChannelView::shouldQueueSlowChatMessages() const
{
    return this->split_ != nullptr && !this->isActivityPaneView() &&
           this->split_->slowerChatEnabled();
}

bool ChannelView::shouldAnimateMessageAnimations() const
{
    return this->split_ != nullptr && this->context_ != Context::Search &&
           !this->isActivityPaneView() &&
           getSettings()->messageAnimations &&
           this->split_->slowerChatMessageAnimations();
}

int ChannelView::pendingSlowChatMessageCount() const
{
    return static_cast<int>(this->pendingSlowChatMessages_.size());
}

void ChannelView::notifySlowChatQueueCountChanged()
{
    const int count = this->pendingSlowChatMessageCount();
    if (count == this->lastSlowChatQueueCount_)
    {
        return;
    }

    this->lastSlowChatQueueCount_ = count;
    Q_EMIT this->slowChatQueueCountChanged(count);
}

void ChannelView::drainSlowChatQueue()
{
    if (!this->shouldQueueSlowChatMessages())
    {
        this->flushSlowChatQueue();
        return;
    }

    if (this->pendingSlowChatMessages_.empty())
    {
        this->slowChatTimer_.stop();
        return;
    }

    const auto now = SteadyClock::now();
    if (this->nextSlowChatMessageAt_ && now < *this->nextSlowChatMessageAt_)
    {
        this->scheduleSlowChatDrain();
        return;
    }

    auto nextMessage = this->pendingSlowChatMessages_.front();
    this->pendingSlowChatMessages_.pop_front();
    this->notifySlowChatQueueCountChanged();

    const auto rate =
        std::max<qreal>(0.01, this->split_->slowerChatMessagesPerSecond());
    this->nextSlowChatMessageAt_ =
        now + std::chrono::milliseconds(static_cast<int>(
                  std::round(1000.0 / static_cast<double>(rate))));

    this->appendProxyMessage(nextMessage.message, nextMessage.overridingFlags,
                             nextMessage.emitAddedToChannel);

    if (this->pendingSlowChatMessages_.empty())
    {
        this->slowChatTimer_.stop();
        return;
    }

    this->scheduleSlowChatDrain();
}

void ChannelView::scheduleSlowChatDrain()
{
    if (this->pendingSlowChatMessages_.empty())
    {
        this->slowChatTimer_.stop();
        return;
    }

    if (!this->shouldQueueSlowChatMessages())
    {
        this->flushSlowChatQueue();
        return;
    }

    const auto now = SteadyClock::now();
    const auto delay = this->nextSlowChatMessageAt_
                           ? std::max(std::chrono::duration_cast<
                                          std::chrono::milliseconds>(
                                          *this->nextSlowChatMessageAt_ - now),
                                      std::chrono::milliseconds(0))
                           : std::chrono::milliseconds(0);

    this->slowChatTimer_.start(static_cast<int>(delay.count()));
}

void ChannelView::flushSlowChatQueue()
{
    this->slowChatTimer_.stop();
    while (!this->pendingSlowChatMessages_.empty())
    {
        auto nextMessage = this->pendingSlowChatMessages_.front();
        this->pendingSlowChatMessages_.pop_front();
        this->appendProxyMessage(nextMessage.message, nextMessage.overridingFlags,
                                 nextMessage.emitAddedToChannel);
    }
    this->nextSlowChatMessageAt_.reset();
    this->notifySlowChatQueueCountChanged();
}

bool ChannelView::updatePendingSlowChatMessage(const MessagePtr &prev,
                                               const MessagePtr &replacement)
{
    for (auto it = this->pendingSlowChatMessages_.begin();
         it != this->pendingSlowChatMessages_.end(); ++it)
    {
        if (it->message != prev &&
            (prev->id.isEmpty() || it->message->id != prev->id))
        {
            continue;
        }

        if (replacement)
        {
            it->message = replacement;
        }
        else
        {
            this->pendingSlowChatMessages_.erase(it);
            this->notifySlowChatQueueCountChanged();
        }
        return true;
    }

    return false;
}

void ChannelView::clearMessageLayoutShiftAnimations()
{
    this->messageLayoutShiftAnimationTimer_.stop();
    this->messageLayoutShiftAnimations_.clear();
}

void ChannelView::startOrUpdateMessageLayoutShiftAnimation(
    const MessageLayoutPtr &layout, qreal fromY, qreal toY)
{
    if (qFuzzyCompare(fromY + 1.0, toY + 1.0))
    {
        return;
    }

    const auto now = SteadyClock::now();
    for (auto &animation : this->messageLayoutShiftAnimations_)
    {
        if (animation.layout != layout)
        {
            continue;
        }

        const auto elapsed = now - animation.startedAt;
        if (elapsed < MESSAGE_LAYOUT_SHIFT_ANIMATION_DURATION)
        {
            const auto progress = static_cast<qreal>(
                                      std::chrono::duration_cast<
                                          std::chrono::milliseconds>(elapsed)
                                          .count()) /
                                  static_cast<qreal>(
                                      MESSAGE_LAYOUT_SHIFT_ANIMATION_DURATION
                                          .count());
            const auto eased = messageLayoutShiftProgress(progress);
            fromY = animation.toY + ((animation.fromY - animation.toY) *
                                     (1.0 - eased));
        }

        animation.fromY = fromY;
        animation.toY = toY;
        animation.startedAt = now;
        if (!this->messageLayoutShiftAnimationTimer_.isActive())
        {
            this->messageLayoutShiftAnimationTimer_.start();
        }
        return;
    }

    this->messageLayoutShiftAnimations_.push_back(
        MessageLayoutShiftAnimation{layout, fromY, toY, now});
    if (!this->messageLayoutShiftAnimationTimer_.isActive())
    {
        this->messageLayoutShiftAnimationTimer_.start();
    }
}

std::optional<qreal> ChannelView::messageLayoutShiftAnimationY(
    const MessageLayoutPtr &layout) const
{
    const auto now = SteadyClock::now();
    for (const auto &animation : this->messageLayoutShiftAnimations_)
    {
        if (animation.layout != layout)
        {
            continue;
        }

        const auto elapsed = now - animation.startedAt;
        if (elapsed >= MESSAGE_LAYOUT_SHIFT_ANIMATION_DURATION)
        {
            return std::nullopt;
        }

        const auto progress = static_cast<qreal>(
                                  std::chrono::duration_cast<
                                      std::chrono::milliseconds>(elapsed)
                                      .count()) /
                              static_cast<qreal>(
                                  MESSAGE_LAYOUT_SHIFT_ANIMATION_DURATION
                                      .count());
        const auto eased = messageLayoutShiftProgress(progress);
        return animation.toY +
               ((animation.fromY - animation.toY) * (1.0 - eased));
    }

    return std::nullopt;
}

void ChannelView::clearMessageArrivalAnimations()
{
    this->messageArrivalAnimationTimer_.stop();
    this->messageArrivalAnimations_.clear();
}

void ChannelView::addMessageArrivalAnimation(const MessageLayoutPtr &layout)
{
    // Keep a single active arrival animation so fast queue drains don't
    // visually compress multiple entering messages into each other.
    this->messageArrivalAnimations_.clear();
    this->messageArrivalAnimations_.push_back(
        MessageArrivalAnimation{layout, SteadyClock::now()});
    if (!this->messageArrivalAnimationTimer_.isActive())
    {
        this->messageArrivalAnimationTimer_.start();
    }
}

std::optional<qreal> ChannelView::messageArrivalAnimationProgress(
    const MessageLayoutPtr &layout) const
{
    const auto now = SteadyClock::now();
    for (const auto &animation : this->messageArrivalAnimations_)
    {
        if (animation.layout != layout)
        {
            continue;
        }

        const auto elapsed = now - animation.startedAt;
        if (elapsed >= SLOW_CHAT_MESSAGE_ANIMATION_DURATION)
        {
            return std::nullopt;
        }

        const auto progress = static_cast<qreal>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()) /
                              static_cast<qreal>(
                                  SLOW_CHAT_MESSAGE_ANIMATION_DURATION.count());
        return std::clamp(progress, 0.0, 1.0);
    }

    return std::nullopt;
}

void ChannelView::setChannel(const ChannelPtr &underlyingChannel)
{
    /// Clear connections from the last channel
    this->channelConnections_.clear();
    this->activityGiftRecipientsToSuppress_ = 0;
    this->activitySuppressNextAnnouncementMessage_ = false;

    this->clearMessages();
    this->scrollBar_->clearHighlights();

    /// make copy of channel and expose
    this->channel_ = std::make_unique<Channel>(underlyingChannel->getName(),
                                               underlyingChannel->getType());

    //
    // Proxy channel connections
    // Use a proxy channel to keep filtered messages past the time they are removed from their origin channel
    //

    this->channelConnections_.managedConnect(
        underlyingChannel->messageAppended,
        [this](MessagePtr &message,
               std::optional<MessageFlags> overridingFlags) {
            auto copiedMessage = message;
            runOnChannelViewThread(
                this, [copiedMessage, overridingFlags](ChannelView &view) {
                    if (view.shouldIncludeMessage(copiedMessage))
                    {
                        if (view.channel_->lastDate_ != QDate::currentDate())
                        {
                            // Day change message
                            view.channel_->lastDate_ = QDate::currentDate();
                            auto msg = makeSystemMessage(
                                QLocale().toString(QDate::currentDate(),
                                                   QLocale::LongFormat),
                                QTime(0, 0));
                            msg->flags.set(MessageFlag::DoNotLog);
                            msg->flags.set(
                                MessageFlag::DoNotTriggerNotification);
                            view.enqueueOrAddProxyMessage(msg);
                        }
                        view.enqueueOrAddProxyMessage(copiedMessage,
                                                      overridingFlags, true);
                    }
                });
        });

    this->channelConnections_.managedConnect(
        underlyingChannel->messagesAddedAtStart,
        [this](std::vector<MessagePtr> &messages) {
            auto copiedMessages = messages;
            runOnChannelViewThread(
                this, [copiedMessages = std::move(copiedMessages)](
                          ChannelView &view) {
                    std::vector<MessagePtr> filtered;
                    filtered.reserve(copiedMessages.size());
                    for (const auto &msg : copiedMessages)
                    {
                        if (view.shouldIncludeMessage(msg))
                        {
                            filtered.push_back(
                                view.decorateStreamDatabaseMessage(msg));
                        }
                    }

                    if (!filtered.empty())
                    {
                        view.channel_->addMessagesAtStart(filtered);
                    }
                });
        });

    this->channelConnections_.managedConnect(
        underlyingChannel->messageReplaced,
        [this](auto index, const auto &prev, const auto &replacement) {
            runOnChannelViewThread(
                this, [index, prev, replacement](ChannelView &view) {
                    const bool shouldIncludeReplacement =
                        view.shouldIncludeMessage(replacement);
                    auto proxyReplacement =
                        shouldIncludeReplacement
                            ? view.decorateStreamDatabaseMessage(replacement)
                            : MessagePtr{};

                    if (view.updatePendingSlowChatMessage(prev,
                                                          proxyReplacement))
                    {
                        return;
                    }

                    if (shouldIncludeReplacement)
                    {
                        auto proxyPrev = prev;
                        if (view.shouldDecorateStreamDatabaseMessage() &&
                            prev != nullptr && !prev->id.isEmpty())
                        {
                            if (auto existing =
                                    view.channel_->findMessageByID(prev->id))
                            {
                                proxyPrev = existing;
                            }
                        }
                        view.channel_->replaceMessage(index, proxyPrev,
                                                      proxyReplacement);
                    }
                });
        });

    this->channelConnections_.managedConnect(
        Channel::messageRemoved,
        [this](Channel *source, size_t, const MessagePtr &message) {
            if (source != this->underlyingChannel_.get())
            {
                return;
            }
            runOnChannelViewThread(
                this, [message](ChannelView &view) {
                    auto proxyMessage = message;
                    if (message != nullptr && !message->id.isEmpty())
                    {
                        if (auto existing =
                                view.channel_->findMessageByID(message->id))
                        {
                            proxyMessage = existing;
                        }
                    }
                    view.channel_->removeMessage(proxyMessage);
                });
        });

    this->channelConnections_.managedConnect(
        underlyingChannel->filledInMessages, [this](const auto &messages) {
            auto copiedMessages = messages;
            runOnChannelViewThread(
                this, [copiedMessages = std::move(copiedMessages)](
                          ChannelView &view) {
                    std::vector<MessagePtr> filtered;
                    filtered.reserve(copiedMessages.size());
                    for (const auto &msg : copiedMessages)
                    {
                        if (view.shouldIncludeMessage(msg))
                        {
                            filtered.push_back(
                                view.decorateStreamDatabaseMessage(msg));
                        }
                    }
                    view.channel_->fillInMissingMessages(filtered);
                });
        });

    this->channelConnections_.managedConnect(underlyingChannel->messagesCleared,
                                             [this]() {
                                                 runOnChannelViewThread(
                                                     this,
                                                     [](ChannelView &view) {
                                                         view.clearMessages();
                                                     });
                                             });

    // Copy over messages from the backing channel to the filtered one
    // and the ui.
    auto snapshot = underlyingChannel->getMessageSnapshot();

    size_t nMessagesAdded = 0;
    for (const auto &msg : snapshot)
    {
        if (!this->shouldIncludeMessage(msg))
        {
            continue;
        }

        auto proxyMessage = this->decorateStreamDatabaseMessage(msg);
        auto messageLayout = std::make_shared<MessageLayout>(proxyMessage);

        if (this->lastMessageHasAlternateBackground_)
        {
            messageLayout->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        this->lastMessageHasAlternateBackground_ =
            !this->lastMessageHasAlternateBackground_;

        if (underlyingChannel->shouldIgnoreHighlights())
        {
            messageLayout->flags.set(MessageLayoutFlag::IgnoreHighlights);
        }

        this->messages_.pushBack(messageLayout);

        this->channel_->addMessage(proxyMessage, MessageContext::Repost);

        nMessagesAdded++;
        if (this->showScrollbarHighlights())
        {
            this->scrollBar_->addHighlight(
                this->scrollbarHighlightForMessage(proxyMessage));
        }
    }

    this->scrollBar_->setMaximum(
        static_cast<qreal>(std::min(nMessagesAdded, this->messages_.limit())));

    if (this->isActivityPaneView())
    {
        this->rebuildActivityMessages();
    }

    //
    // Standard channel connections
    //

    // on new message
    this->channelConnections_.managedConnect(
        this->channel_->messageAppended,
        [this](MessagePtr &message,
               std::optional<MessageFlags> overridingFlags) {
            this->messageAppended(message, overridingFlags);
        });

    this->channelConnections_.managedConnect(
        this->channel_->messagesAddedAtStart,
        [this](std::vector<MessagePtr> &messages) {
            this->messageAddedAtStart(messages);
        });

    // on message replaced
    this->channelConnections_.managedConnect(
        this->channel_->messageReplaced,
        [this](size_t index, const MessagePtr &prev,
               const MessagePtr &replacement) {
            this->messageReplaced(index, prev, replacement);
        });

    this->channelConnections_.managedConnect(
        Channel::messageRemoved,
        [this](Channel *source, size_t index, const MessagePtr &message) {
            if (source != this->channel_.get())
            {
                return;
            }
            this->messageRemoved(index, message);
        });

    // on messages filled in
    this->channelConnections_.managedConnect(this->channel_->filledInMessages,
                                             [this](const auto &) {
                                                 this->messagesUpdated();
                                             });

    this->underlyingChannel_ = underlyingChannel;

    this->updateID();

    this->queueLayout();
    if (!this->isVisible() && !this->scrollBar_->isVisible())
    {
        // If we're not visible and the scrollbar is not (yet) visible,
        // we need to make sure that it's at the bottom when this view is laid
        // out later.
        this->scrollBar_->scrollToBottom();
    }
    this->queueUpdate();
    this->updateActivityTimeRefreshTimer();

    // Notifications
    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(underlyingChannel.get());
    if (twitchChannel != nullptr)
    {
        this->channelConnections_.managedConnect(
            twitchChannel->streamStatusChanged, [this]() {
                runOnChannelViewThread(this, [](ChannelView &view) {
                    view.liveStatusChanged.invoke();
                });
            });
    }
    else if (auto *kickChannel =
                 dynamic_cast<KickChannel *>(underlyingChannel.get()))
    {
        this->channelConnections_.managedConnect(
            kickChannel->liveStatusChanged, [this] {
                runOnChannelViewThread(this, [](ChannelView &view) {
                    view.liveStatusChanged.invoke();
                });
            });
    }
    else if (auto *mergedChannel =
                 dynamic_cast<MergedChannel *>(underlyingChannel.get()))
    {
        this->channelConnections_.managedConnect(
            mergedChannel->streamStatusChanged, [this] {
                runOnChannelViewThread(this, [](ChannelView &view) {
                    view.liveStatusChanged.invoke();
                });
            });
    }
}

void ChannelView::refreshMessages()
{
    if (!this->underlyingChannel_)
    {
        return;
    }

    this->setChannel(this->underlyingChannel_);
}

void ChannelView::refreshSlowerChatSettings()
{
    if (!this->shouldAnimateMessageAnimations())
    {
        this->clearMessageLayoutShiftAnimations();
    }

    if (!this->shouldAnimateMessageAnimations())
    {
        this->clearMessageArrivalAnimations();
    }

    if (!this->shouldQueueSlowChatMessages())
    {
        this->flushSlowChatQueue();
        return;
    }

    if (!this->pendingSlowChatMessages_.empty())
    {
        this->scheduleSlowChatDrain();
    }
}

void ChannelView::setFilters(const QList<QUuid> &ids)
{
    this->channelFilters_ = std::make_shared<FilterSet>(ids);

    this->updateID();
}

QList<QUuid> ChannelView::getFilterIds() const
{
    if (!this->channelFilters_)
    {
        return {};
    }

    return this->channelFilters_->filterIds();
}

FilterSetPtr ChannelView::getFilterSet() const
{
    return this->channelFilters_;
}

bool ChannelView::shouldIncludeMessage(const MessagePtr &m) const
{
    if (this->split_ && this->split_->filterActivity() &&
        !this->split_->isActivityPane())
    {
        if (isActivityTikTokGiftMessage(*m))
        {
            if (shouldShowTikTokGiftInActivityPane(
                    *m, this->split_->tiktokActivityMinimumDiamonds()))
            {
                return false;
            }
        }
        else if (isActivityTwitchBitsMessage(*m))
        {
            if (shouldShowTwitchBitsInActivityPane(
                    *m, this->split_->twitchActivityMinimumBits()))
            {
                return false;
            }
        }
        else if (isActivityKickKicksGiftMessage(*m))
        {
            if (shouldShowKickKicksGiftInActivityPane(
                    *m, this->split_->kickActivityMinimumKicks()))
            {
                return false;
            }
        }
        else if (isActivityAlertMessage(*m))
        {
            return false;
        }
    }

    if (this->channelFilters_)
    {
        if (getSettings()->excludeUserMessagesFromFilter &&
            getApp()->getAccounts()->twitch.getCurrent()->getUserName().compare(
                m->loginName, Qt::CaseInsensitive) == 0)
        {
            return true;
        }

        return this->channelFilters_->filter(m, this->underlyingChannel_);
    }

    return true;
}

ChannelPtr ChannelView::sourceChannel() const
{
    return this->sourceChannel_;
}

void ChannelView::setSourceChannel(ChannelPtr sourceChannel)
{
    this->sourceChannel_ = std::move(sourceChannel);
}

bool ChannelView::hasSourceChannel() const
{
    return this->sourceChannel_ != nullptr;
}

void ChannelView::messageAppended(MessagePtr &message,
                                  std::optional<MessageFlags> overridingFlags)
{
    MessagePtr visibleMessage = message;
    if (this->isActivityPaneView())
    {
        auto transformed = this->transformActivityMessage(
            message, this->activityGiftRecipientsToSuppress_,
            this->activitySuppressNextAnnouncementMessage_);
        if (!transformed)
        {
            return;
        }
        visibleMessage = *transformed;

        if (this->tryMergeActivityGiftMessage(visibleMessage))
        {
            this->queueLayout();
            return;
        }
    }

    auto *messageFlags = &message->flags;
    if (overridingFlags)
    {
        messageFlags = &*overridingFlags;
    }

    auto messageRef = std::make_shared<MessageLayout>(visibleMessage);

    if (this->lastMessageHasAlternateBackground_)
    {
        messageRef->flags.set(MessageLayoutFlag::AlternateBackground);
    }
    if (this->channel_->shouldIgnoreHighlights())
    {
        messageRef->flags.set(MessageLayoutFlag::IgnoreHighlights);
    }
    this->lastMessageHasAlternateBackground_ =
        !this->lastMessageHasAlternateBackground_;

    if (this->shouldAnimateMessageAnimations() && this->isVisible() &&
        (this->showingLatestMessages_ || !this->showScrollBar_))
    {
        this->addMessageArrivalAnimation(messageRef);
    }

    if (this->paused())
    {
        this->pauseScrollMaximumOffset_++;
    }
    else
    {
        this->scrollBar_->offsetMaximum(1);
    }

    if (this->messages_.pushBack(messageRef))
    {
        if (this->paused())
        {
            this->pauseScrollMinimumOffset_++;
            this->pauseSelectionOffset_++;
        }
        else
        {
            this->scrollBar_->offsetMinimum(1);
            if (this->showingLatestMessages_ && !this->isVisible())
            {
                this->scrollBar_->scrollToBottom(false);
            }
            this->selection_.shiftMessageIndex(1);
            this->doubleClickSelection_.shiftMessageIndex(1);
        }
    }

    if (!messageFlags->has(MessageFlag::DoNotTriggerNotification))
    {
        if ((messageFlags->has(MessageFlag::Highlighted) &&
             messageFlags->has(MessageFlag::ShowInMentions) &&
             !messageFlags->has(MessageFlag::Subscription) &&
             (getSettings()->highlightMentions ||
              this->channel_->getType() != Channel::Type::TwitchMentions)) ||
            (this->channel_->getType() == Channel::Type::TwitchAutomod &&
             getSettings()->enableAutomodHighlight))
        {
            this->tabHighlightRequested.invoke(HighlightState::Highlighted);
        }
        else
        {
            this->tabHighlightRequested.invoke(HighlightState::NewMessage);
        }
    }

    if (this->showScrollbarHighlights())
    {
        this->scrollBar_->addHighlight(
            this->scrollbarHighlightForMessage(visibleMessage));
    }

    this->queueLayout();
}

void ChannelView::messageAddedAtStart(std::vector<MessagePtr> &messages)
{
    if (this->isActivityPaneView())
    {
        this->rebuildActivityMessages();
        return;
    }

    std::vector<MessageLayoutPtr> messageRefs;
    messageRefs.resize(messages.size());

    /// Create message layouts
    for (size_t i = 0; i < messages.size(); i++)
    {
        auto message = messages.at(i);
        auto layout = std::make_shared<MessageLayout>(message);

        // alternate color
        if (!this->lastMessageHasAlternateBackgroundReverse_)
        {
            layout->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        this->lastMessageHasAlternateBackgroundReverse_ =
            !this->lastMessageHasAlternateBackgroundReverse_;

        messageRefs.at(i) = std::move(layout);
    }

    /// Add the messages at the start
    auto addedMessages = this->messages_.pushFront(messageRefs);
    if (!addedMessages.empty())
    {
        if (this->scrollBar_->isAtBottom())
        {
            this->scrollBar_->scrollToBottom();
        }
        else
        {
            this->scrollBar_->offset(qreal(addedMessages.size()));
        }
        this->scrollBar_->offsetMaximum(qreal(addedMessages.size()));
    }

    if (this->showScrollbarHighlights())
    {
        std::vector<ScrollbarHighlight> highlights;
        highlights.reserve(messages.size());
        for (const auto &message : messages)
        {
            highlights.push_back(this->scrollbarHighlightForMessage(message));
        }

        this->scrollBar_->addHighlightsAtStart(highlights);
    }

    this->queueLayout();
}

void ChannelView::messageReplaced(size_t hint, const MessagePtr &prev,
                                  const MessagePtr &replacement)
{
    if (this->isActivityPaneView())
    {
        (void)hint;
        (void)prev;
        (void)replacement;
        this->rebuildActivityMessages();
        return;
    }

    auto optItem = this->messages_.find(hint, [&](const auto &it) {
        return it->getMessagePtr() == prev;
    });
    if (!optItem)
    {
        return;
    }
    const auto &[index, oldItem] = *optItem;

    auto newItem = std::make_shared<MessageLayout>(replacement);

    if (oldItem->flags.has(MessageLayoutFlag::AlternateBackground))
    {
        newItem->flags.set(MessageLayoutFlag::AlternateBackground);
    }

    this->scrollBar_->replaceHighlight(
        index, this->scrollbarHighlightForMessage(replacement));

    this->messages_.replaceItem(index, newItem);
    this->queueLayout();
}

void ChannelView::messageRemoved(size_t hint, const MessagePtr &message)
{
    if (this->isActivityPaneView())
    {
        this->rebuildActivityMessages();
        return;
    }

    auto optItem = this->messages_.find(hint, [&](const auto &item) {
        return item->getMessagePtr() == message;
    });
    if (!optItem)
    {
        return;
    }

    const auto index = optItem->first;
    const bool wasAtBottom = this->scrollBar_->isAtBottom();
    const auto relativeCurrent = this->scrollBar_->getRelativeCurrentValue();

    if (!this->messages_.removeItem(index))
    {
        return;
    }
    this->scrollBar_->removeHighlight(index);
    this->scrollBar_->offsetMaximum(-1);

    if (wasAtBottom)
    {
        this->scrollBar_->scrollToBottom();
    }
    else if (static_cast<qreal>(index) < relativeCurrent)
    {
        this->scrollBar_->offset(-1);
    }

    bool alternate = false;
    for (const auto &layout : this->messages_.getSnapshot())
    {
        layout->flags.set(MessageLayoutFlag::AlternateBackground, alternate);
        alternate = !alternate;
    }
    this->lastMessageHasAlternateBackground_ = alternate;
    this->lastMessageHasAlternateBackgroundReverse_ = true;
    this->queueLayout();
}

void ChannelView::messagesUpdated()
{
    if (this->isActivityPaneView())
    {
        this->rebuildActivityMessages();
        return;
    }

    auto snapshot = this->channel_->getMessageSnapshot();

    this->clearMessageArrivalAnimations();
    this->messages_.clear();
    this->scrollBar_->clearHighlights();
    this->scrollBar_->resetBounds();
    this->scrollBar_->setMaximum(qreal(snapshot.size()));
    this->scrollBar_->setMinimum(0);
    this->lastMessageHasAlternateBackground_ = false;
    this->lastMessageHasAlternateBackgroundReverse_ = true;

    for (const auto &msg : snapshot)
    {
        auto messageLayout = std::make_shared<MessageLayout>(msg);

        if (this->lastMessageHasAlternateBackground_)
        {
            messageLayout->flags.set(MessageLayoutFlag::AlternateBackground);
        }
        this->lastMessageHasAlternateBackground_ =
            !this->lastMessageHasAlternateBackground_;

        if (this->channel_->shouldIgnoreHighlights())
        {
            messageLayout->flags.set(MessageLayoutFlag::IgnoreHighlights);
        }

        this->messages_.pushBack(messageLayout);
        if (this->showScrollbarHighlights())
        {
            this->scrollBar_->addHighlight(
                this->scrollbarHighlightForMessage(msg));
        }
    }

    this->queueLayout();
}

void ChannelView::updateLastReadMessage()
{
    if (auto lastMessage = this->messages_.last())
    {
        this->lastReadMessage_ = *lastMessage;
    }

    this->update();
}

void ChannelView::resizeEvent(QResizeEvent * /*event*/)
{
    this->scrollBar_->setGeometry(this->width() - this->scrollBar_->width(), 0,
                                  this->scrollBar_->width(), this->height());

    this->goToBottom_->setGeometry(0, this->height() - int(this->scale() * 26),
                                   this->width(), int(this->scale() * 26));

    this->scrollBar_->raise();

    this->queueLayout();

    this->update();
}

void ChannelView::setSelection(const Selection &newSelection)
{
    if (this->selection_ != newSelection)
    {
        this->selection_ = newSelection;
        this->selectionChanged.invoke();
        this->update();
    }
}

void ChannelView::setSelection(const SelectionItem &start,
                               const SelectionItem &end)
{
    this->setSelection({start, end});
}

MessageElementFlags ChannelView::getFlags() const
{
    auto *app = getApp();

    if (this->overrideFlags_)
    {
        return *this->overrideFlags_;
    }

    MessageElementFlags flags = app->getWindows()->getWordFlags();

    auto *split = dynamic_cast<Split *>(this->parentWidget());

    if (split == nullptr)
    {
        auto *searchPopup = dynamic_cast<SearchPopup *>(this->parentWidget());
        if (searchPopup != nullptr)
        {
            split = dynamic_cast<Split *>(searchPopup->parentWidget());
        }
    }

    const auto platformIndicatorMode =
        split != nullptr ? split->platformIndicatorMode()
                         : this->resolvedPlatformIndicatorMode();

    if (split != nullptr)
    {
        if (split->getModerationMode())
        {
            flags.set(MessageElementFlag::ModeratorTools);
        }
        if (this->underlyingChannel_ ==
                getApp()->getTwitch()->getMentionsChannel() ||
            this->underlyingChannel_ ==
                getApp()->getTwitch()->getLiveChannel() ||
            this->underlyingChannel_ ==
                getApp()->getTwitch()->getAutomodChannel())
        {
            flags.set(MessageElementFlag::ChannelName);
            flags.unset(MessageElementFlag::ChannelPointReward);
        }
    }

    if (this->sourceChannel_ == getApp()->getTwitch()->getMentionsChannel() ||
        this->sourceChannel_ == getApp()->getTwitch()->getAutomodChannel())
    {
        flags.set(MessageElementFlag::ChannelName);
    }

    if (this->context_ == Context::ReplyThread ||
        getSettings()->hideReplyContext)
    {
        // Don't show inline replies within the ReplyThreadPopup
        // or if they're hidden
        flags.unset(MessageElementFlag::RepliedMessage);
    }

    if (!this->canReplyToMessages())
    {
        flags.unset(MessageElementFlag::ReplyButton);
    }

    flags.set(MessageElementFlag::PlatformBadge,
              mergedPlatformIndicatorShowsBadge(platformIndicatorMode));

    return flags;
}

bool ChannelView::scrollToMessage(const MessagePtr &message)
{
    return this->scrollToMessage(message, false);
}

bool ChannelView::scrollToMessage(const MessagePtr &message, bool startReply)
{
    if (!this->mayContainMessage(message))
    {
        return false;
    }

    auto &messagesSnapshot = this->getMessagesSnapshot();
    if (messagesSnapshot.size() == 0)
    {
        return false;
    }

    // TODO: Figure out if we can somehow binary-search here.
    //       Currently, a message only sometimes stores a QDateTime,
    //       but always a QTime (inaccurate on midnight).
    //
    // We're searching from the bottom since it's more likely for a user
    // wanting to go to a message that recently scrolled out of view.
    size_t messageIdx = messagesSnapshot.size() - 1;
    for (; messageIdx < SIZE_MAX; messageIdx--)
    {
        if (isSameChatMessage(messagesSnapshot[messageIdx]->getMessagePtr(),
                              message))
        {
            break;
        }
    }

    if (messageIdx == SIZE_MAX)
    {
        return false;
    }

    this->scrollToMessageLayout(messagesSnapshot[messageIdx].get(), messageIdx);
    if (this->split_)
    {
        getApp()->getWindows()->select(this->split_);
    }
    if (startReply)
    {
        this->setInputReply(messagesSnapshot[messageIdx]->getMessagePtr());
    }
    return true;
}

bool ChannelView::scrollToMessageId(const QString &messageId)
{
    auto &messagesSnapshot = this->getMessagesSnapshot();
    if (messagesSnapshot.size() == 0)
    {
        return false;
    }

    // We're searching from the bottom since it's more likely for a user
    // wanting to go to a message that recently scrolled out of view.
    size_t messageIdx = messagesSnapshot.size() - 1;
    for (; messageIdx < SIZE_MAX; messageIdx--)
    {
        if (messagesSnapshot[messageIdx]->getMessagePtr()->id == messageId)
        {
            break;
        }
    }

    if (messageIdx == SIZE_MAX)
    {
        return false;
    }

    this->scrollToMessageLayout(messagesSnapshot[messageIdx].get(), messageIdx);
    if (this->split_)
    {
        getApp()->getWindows()->select(this->split_);
    }
    return true;
}

void ChannelView::scrollToMessageLayout(MessageLayout *layout,
                                        size_t messageIdx)
{
    this->highlightedMessage_ = layout;
    this->highlightAnimation_.setCurrentTime(0);
    this->highlightAnimation_.start(QAbstractAnimation::KeepWhenStopped);

    if (this->showScrollBar_)
    {
        this->getScrollBar().setDesiredValue(this->scrollBar_->getMinimum() +
                                             qreal(messageIdx));
    }
}

void ChannelView::paintEvent(QPaintEvent *event)
{
    //    BenchmarkGuard benchmark("paint");

    QPainter painter(this);

    painter.fillRect(this->rect(), this->messageColors_.channelBackground);

    // draw messages
    this->drawMessages(painter, event->rect());

    // draw paused sign
    if (this->paused())
    {
        auto baseSize = 20;
        auto scale = this->scale();
        auto indicatorSize = baseSize * scale;
        auto color = QColor(180, 180, 180, 255);
        auto brush = QBrush(color);

        const auto pausedY = indicatorSize / 4;
        const auto pausedX = 5 * scale;

        QFont font = painter.font();
        font.setPixelSize(indicatorSize);
        painter.setFont(font);

        const QString text = "Paused";
        const QFontMetrics metrics(font);
        const auto textWidth = metrics.horizontalAdvance(text);
        const auto textX = pausedX * 3 + 10 * scale;

        painter.fillRect(QRectF(0, 0, pausedX + textX + textWidth,
                                indicatorSize / 2 + indicatorSize),
                         QBrush(QColor(0, 0, 0, 200), Qt::SolidPattern));

        painter.fillRect(
            QRectF(pausedX, pausedY, indicatorSize / 4, indicatorSize), brush);
        painter.fillRect(
            QRectF(pausedX * 3, pausedY, indicatorSize / 4, indicatorSize),
            brush);

        painter.setPen(color);
        painter.drawText(QRectF(textX, pausedY, textWidth, indicatorSize),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

// if overlays is false then it draws the message, if true then it draws things
// such as the grey overlay when a message is disabled
void ChannelView::drawMessages(QPainter &painter, const QRect &area)
{
    auto &messagesSnapshot = this->getMessagesSnapshot();

    const auto relativeCurrentValue = this->scrollBar_->getRelativeCurrentValue();
    const auto start = size_t(relativeCurrentValue);

    if (start >= messagesSnapshot.size())
    {
        return;
    }

    MessageLayout *end = nullptr;
    bool showLastMessageIndicator = getSettings()->showLastMessageIndicator;
    struct ActiveArrivalAnimation {
        size_t messageIndex;
        qreal offset;
    };
    std::vector<ActiveArrivalAnimation> activeArrivalAnimations;
    activeArrivalAnimations.reserve(this->messageArrivalAnimations_.size());
    qreal totalStackAnimationOffset = 0.0;
    const auto now = SteadyClock::now();
    for (const auto &animation : this->messageArrivalAnimations_)
    {
        const auto it = std::find(messagesSnapshot.rbegin(),
                                  messagesSnapshot.rend(), animation.layout);
        if (it == messagesSnapshot.rend())
        {
            continue;
        }

        const auto elapsed = now - animation.startedAt;
        if (elapsed >= SLOW_CHAT_MESSAGE_ANIMATION_DURATION)
        {
            continue;
        }

        const auto progress = static_cast<qreal>(
                                  std::chrono::duration_cast<
                                      std::chrono::milliseconds>(elapsed)
                                      .count()) /
                              static_cast<qreal>(
                                  SLOW_CHAT_MESSAGE_ANIMATION_DURATION.count());
        const auto clampedProgress = std::clamp(progress, 0.0, 1.0);
        const auto offset = slowChatArrivalOffset(
            clampedProgress, static_cast<qreal>((*it)->getHeight()));
        const auto messageIndex =
            static_cast<size_t>(std::distance(it, messagesSnapshot.rend()) - 1);
        activeArrivalAnimations.push_back(
            {
                .messageIndex = messageIndex,
                .offset = offset,
            });
        totalStackAnimationOffset += offset;
    }
    std::sort(activeArrivalAnimations.begin(), activeArrivalAnimations.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.messageIndex < rhs.messageIndex;
              });
    auto activeArrivalIt = activeArrivalAnimations.begin();
    const auto lowestActiveArrivalIndex =
        activeArrivalAnimations.empty()
            ? std::optional<size_t>()
            : std::make_optional(activeArrivalAnimations.back().messageIndex);
    const bool shortLiveChatArrivalMode =
        this->showingLatestMessages_ && !this->showScrollBar_;

    size_t renderStart = start;
    qreal renderStartOffsetBudget =
        shortLiveChatArrivalMode ? 0.0 : totalStackAnimationOffset;
    while (renderStart > 0 && renderStartOffsetBudget > 0.0)
    {
        --renderStart;
        renderStartOffsetBudget -=
            static_cast<qreal>(messagesSnapshot[renderStart]->getHeight());
    }

    auto initialY = -static_cast<int>(
        messagesSnapshot[start]->getHeight() *
        (fmod(relativeCurrentValue, 1)));
    for (size_t i = renderStart; i < start; ++i)
    {
        initialY -= messagesSnapshot[i]->getHeight();
    }

    MessagePaintContext ctx = {
        .painter = painter,
        .selection = this->selection_,
        .colorProvider = ColorProvider::instance(),
        .messageColors = this->effectiveMessageColors(),
        .preferences = this->messagePreferences_,

        .canvasWidth = this->width(),
        .isWindowFocused = this->window() == QApplication::activeWindow(),
        .isMentions = this->underlyingChannel_ ==
                      getApp()->getTwitch()->getMentionsChannel(),
        .forceFlatEventHighlights = this->isActivityPaneView(),
        .platformIndicatorMode = this->resolvedPlatformIndicatorMode(),

        .y = initialY,
        .messageIndex = renderStart,
        .isLastReadMessage = false,

    };

    // using QRect here, because we can only request updates with a rect
    QRect animationArea;
    auto areaContainsY = [&area](auto y) {
        return y >= area.y() && y < area.y() + area.height();
    };

    size_t renderEnd = renderStart;
    for (; ctx.messageIndex < messagesSnapshot.size(); ++ctx.messageIndex)
    {
        MessageLayout *layout = messagesSnapshot[ctx.messageIndex].get();

        if (showLastMessageIndicator)
        {
            ctx.isLastReadMessage = this->lastReadMessage_.get() == layout;
        }
        else
        {
            ctx.isLastReadMessage = false;
        }

        const auto animationProgress =
            this->messageArrivalAnimationProgress(
                messagesSnapshot[ctx.messageIndex]);
        while (activeArrivalIt != activeArrivalAnimations.end() &&
               activeArrivalIt->messageIndex < ctx.messageIndex)
        {
            totalStackAnimationOffset -= activeArrivalIt->offset;
            ++activeArrivalIt;
        }

        const bool hasActiveArrival =
            activeArrivalIt != activeArrivalAnimations.end() &&
            activeArrivalIt->messageIndex == ctx.messageIndex;
        const qreal currentArrivalOffset =
            hasActiveArrival ? activeArrivalIt->offset : 0.0;

        qreal renderY = ctx.y;
        renderY += shortLiveChatArrivalMode ? currentArrivalOffset
                                            : totalStackAnimationOffset;
        if (const auto animatedY =
                this->messageLayoutShiftAnimationY(
                    messagesSnapshot[ctx.messageIndex]))
        {
            renderY = *animatedY;
        }

        if (areaContainsY(renderY) ||
            areaContainsY(renderY + layout->getHeight()) ||
            (renderY < area.y() && layout->getHeight() > area.height()))
        {
            auto animatedCtx = ctx;
            animatedCtx.y = static_cast<int>(std::round(renderY));
            MessagePaintResult paintResult;
            const bool shouldFadeArrival =
                animationProgress && lowestActiveArrivalIndex &&
                *lowestActiveArrivalIndex == ctx.messageIndex;
            if (shouldFadeArrival)
            {
                painter.save();
                painter.setOpacity(
                    painter.opacity() *
                    slowChatArrivalOpacity(animationProgress.value()));
                paintResult = layout->paint(animatedCtx);
                painter.restore();
            }
            else
            {
                paintResult = layout->paint(animatedCtx);
            }

            if (paintResult.hasAnimatedElements)
            {
                if (animationArea.isNull())
                {
                    animationArea = QRect{
                        0,
                        animatedCtx.y,
                        layout->getWidth(),
                        layout->getHeight(),
                    };
                }
                else
                {
                    animationArea.setBottom((animatedCtx.y + layout->getHeight()));
                    animationArea.setWidth(
                        std::max(layout->getWidth(), animationArea.width()));
                }
            }

            if (this->highlightedMessage_ == layout)
            {
            painter.fillRect(
                    QRect{
                        0,
                        animatedCtx.y,
                        layout->getWidth(),
                        layout->getHeight(),
                    },
                    this->highlightAnimation_.currentValue().value<QColor>());
                if (this->highlightAnimation_.state() ==
                    QVariantAnimation::Stopped)
            {
                this->highlightedMessage_ = nullptr;
                }
            }
        }

        if (hasActiveArrival)
        {
            totalStackAnimationOffset -= activeArrivalIt->offset;
            ++activeArrivalIt;
        }

        ctx.y += layout->getHeight();

        end = layout;
        renderEnd = ctx.messageIndex;
        if (ctx.y > this->height())
        {
            break;
        }
    }

    // Only update on a full repaint as some messages with animated elements
    // might get left out in partial repaints.
    // This happens for example when hovering over the go-to-bottom button.
    if (this->height() <= area.height())
    {
        this->animationArea_ = animationArea;
    }
#ifdef FOURTF
    else
    {
        // shows the updated area on partial repaints
        painter.setPen(Qt::red);
        painter.drawRect(area.x(), area.y(), area.width() - 1,
                         area.height() - 1);
    }
#endif

    if (end == nullptr)
    {
        return;
    }

    // Keep nearby row pixmaps alive so smooth scrolling does not recreate
    // buffers for the same messages on adjacent frames.
    auto retainedBufferStart = renderStart;
    auto retainedHeight = 0;
    const auto retainedHeightLimit =
        this->height() * MESSAGE_BUFFER_RETAIN_VIEWPORTS;
    while (retainedBufferStart > 0 && retainedHeight < retainedHeightLimit)
    {
        --retainedBufferStart;
        retainedHeight +=
            std::max(1, messagesSnapshot[retainedBufferStart]->getHeight());
    }

    auto retainedBufferEnd = renderEnd;
    retainedHeight = 0;
    while (retainedBufferEnd + 1 < messagesSnapshot.size() &&
           retainedHeight < retainedHeightLimit)
    {
        ++retainedBufferEnd;
        retainedHeight +=
            std::max(1, messagesSnapshot[retainedBufferEnd]->getHeight());
    }

    for (size_t i = retainedBufferStart; i <= retainedBufferEnd; ++i)
    {
        auto it = this->messagesWithBuffers_.find(messagesSnapshot[i]);
        if (it != this->messagesWithBuffers_.end())
        {
            this->messagesWithBuffers_.erase(it);
        }
    }

    for (const std::shared_ptr<MessageLayout> &item : this->messagesWithBuffers_)
    {
        item->deleteBuffer();
    }

    this->messagesWithBuffers_.clear();

    for (size_t i = retainedBufferStart; i <= retainedBufferEnd; ++i)
    {
        const std::shared_ptr<MessageLayout> &layout = messagesSnapshot[i];

        this->messagesWithBuffers_.insert(layout);
    }
}

void ChannelView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0)
    {
        // Ignore any scrolls where no vertical scrolling has taken place
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier))
    {
        // Ignore any scrolls where ctrl is held down - it is used for zoom
        event->ignore();
        return;
    }

    if (this->scrollBar_->isVisible())
    {
        float mouseMultiplier = getSettings()->mouseScrollMultiplier;

        // This ensures snapshot won't be indexed out of bounds when scrolling really fast
        auto &snapshot = this->getMessagesSnapshot();
        if (snapshot.empty())
        {
            event->accept();
            return;
        }

        qreal desired = std::max<qreal>(0, this->scrollBar_->getDesiredValue());
        qreal delta = event->angleDelta().y() * qreal(1.5) * mouseMultiplier;

        int snapshotLength = int(snapshot.size());
        int i = std::clamp<int>(
            int(desired - this->scrollBar_->getMinimum()), 0,
            snapshotLength - 1);

        if (delta > 0)
        {
            qreal scrollFactor = fmod(desired, 1);
            qreal currentScrollLeft = std::max<qreal>(
                0.01, int(scrollFactor * snapshot[i]->getHeight()));

            for (; i >= 0; i--)
            {
                if (delta < currentScrollLeft)
                {
                    desired -= scrollFactor * (delta / currentScrollLeft);
                    break;
                }
                else
                {
                    delta -= currentScrollLeft;
                    desired -= scrollFactor;
                }

                if (i == 0)
                {
                    desired = 0;
                }
                else
                {
                    snapshot[i - 1]->layout(
                        {
                            .messageColors = this->effectiveMessageColors(),
                            .flags = this->getFlags(),
                            .width = this->getLayoutWidth(),
                            .scale = this->activityMessageScale(),
                            .imageScale = this->activityMessageImageScale(),
                        },
                        false);
                    scrollFactor = 1;
                    currentScrollLeft = snapshot[i - 1]->getHeight();
                }
            }
        }
        else
        {
            delta = -delta;
            qreal scrollFactor = 1 - fmod(desired, 1);
            qreal currentScrollLeft = std::max<qreal>(
                0.01, int(scrollFactor * snapshot[i]->getHeight()));

            for (; i < snapshotLength; i++)
            {
                if (delta < currentScrollLeft)
                {
                    desired +=
                        scrollFactor * (qreal(delta) / currentScrollLeft);
                    break;
                }
                else
                {
                    delta -= currentScrollLeft;
                    desired += scrollFactor;
                }

                if (i == snapshotLength - 1)
                {
                    desired = snapshot.size();
                }
                else
                {
                    snapshot[i + 1]->layout(
                        {
                            .messageColors = this->effectiveMessageColors(),
                            .flags = this->getFlags(),
                            .width = this->getLayoutWidth(),
                            .scale = this->activityMessageScale(),
                            .imageScale = this->activityMessageImageScale(),
                        },
                        false);

                    scrollFactor = 1;
                    currentScrollLeft = snapshot[i + 1]->getHeight();
                }
            }
        }

        this->scrollBar_->setDesiredValue(desired, true);
    }
}

void ChannelView::enterEvent(QEnterEvent * /*event*/)
{
}

void ChannelView::leaveEvent(QEvent * /*event*/)
{
    this->tooltipWidget_->hide();

    this->unpause(PauseReason::Mouse);
}

bool ChannelView::event(QEvent *event)
{
    if (event->type() == QEvent::Gesture)
    {
        if (const auto *gestureEvent = dynamic_cast<QGestureEvent *>(event))
        {
            return this->gestureEvent(gestureEvent);
        }
    }

    return BaseWidget::event(event);
}

bool ChannelView::gestureEvent(const QGestureEvent *event)
{
    if (QGesture *pan = event->gesture(Qt::PanGesture))
    {
        if (const auto *gesture = dynamic_cast<QPanGesture *>(pan))
        {
            switch (gesture->state())
            {
                case Qt::GestureStarted: {
                    this->isPanning_ = true;
                    // Remove any selections and hide tooltip while panning
                    this->clearSelection();
                    this->tooltipWidget_->hide();
                    if (this->isScrolling_)
                    {
                        this->disableScrolling();
                    }
                }
                break;

                case Qt::GestureUpdated: {
                    if (this->scrollBar_->isVisible())
                    {
                        this->scrollBar_->offset(-gesture->delta().y() * 0.1);
                    }
                }
                break;

                case Qt::GestureFinished:
                case Qt::GestureCanceled:
                default: {
                    this->clearSelection();
                    this->isPanning_ = false;
                }
                break;
            }

            return true;
        }
    }

    return false;
}

void ChannelView::mouseMoveEvent(QMouseEvent *event)
{
    if (this->isPanning_)
    {
        // Don't do any text selection, hovering, etc while panning
        return;
    }

    /// Pause on hover
    if (float pauseTime = getSettings()->pauseOnHoverDuration;
        pauseTime > 0.001F)
    {
        this->pause(PauseReason::Mouse,
                    static_cast<uint32_t>(pauseTime * 1000.F));
    }
    else if (pauseTime < -0.5F)
    {
        this->pause(PauseReason::Mouse);
    }

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    // no message under cursor
    if (!this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex))
    {
        this->setCursor(Qt::ArrowCursor);
        this->tooltipWidget_->hide();
        return;
    }

    if (this->isScrolling_)
    {
        this->currentMousePosition_ = event->globalPosition();
    }

    // check for word underneath cursor
    const MessageLayoutElement *hoverLayoutElement =
        layout->getElementAt(relativePos);

    // selecting single characters
    if (this->isLeftMouseDown_)
    {
        auto index = layout->getSelectionIndex(relativePos);
        this->setSelection(this->selection_.start,
                           SelectionItem(messageIndex, index));
    }

    // selecting whole words
    if (this->isDoubleClick_ && hoverLayoutElement)
    {
        auto [wordStart, wordEnd] =
            layout->getWordBounds(hoverLayoutElement, relativePos);
        auto hoveredWord = Selection{SelectionItem(messageIndex, wordStart),
                                     SelectionItem(messageIndex, wordEnd)};
        // combined selection spanning from initially selected word to hoveredWord
        auto selectUnion = this->doubleClickSelection_ | hoveredWord;

        this->setSelection(selectUnion);
    }

    // message under cursor is collapsed
    if (layout->flags.has(MessageLayoutFlag::Collapsed))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->tooltipWidget_->hide();
        return;
    }

    if (hoverLayoutElement == nullptr)
    {
        this->setCursor(Qt::ArrowCursor);
        this->tooltipWidget_->hide();
        return;
    }

    auto *element = &hoverLayoutElement->getCreator();
    bool isLinkValid = hoverLayoutElement->getLink().isValid();
    const auto *emoteElement = dynamic_cast<const EmoteElement *>(element);
    const auto *layeredEmoteElement =
        dynamic_cast<const LayeredEmoteElement *>(element);
    bool isNotEmote = emoteElement == nullptr && layeredEmoteElement == nullptr;

    if (element->getTooltip().isEmpty() ||
        (isLinkValid && isNotEmote && !getSettings()->linkInfoTooltip))
    {
        this->tooltipWidget_->hide();
    }
    else
    {
        const auto *badgeElement = dynamic_cast<const BadgeElement *>(element);

        if (badgeElement || emoteElement || layeredEmoteElement)
        {
            auto showThumbnailSetting =
                getSettings()->emotesTooltipPreview.getEnum();

            bool showThumbnail =
                showThumbnailSetting == ThumbnailPreviewMode::AlwaysShow ||
                (showThumbnailSetting == ThumbnailPreviewMode::ShowOnShift &&
                 event->modifiers() == Qt::ShiftModifier);

            if (emoteElement)
            {
                auto scale = getSettings()->emoteTooltipScale.getEnum();
                this->tooltipWidget_->setOne(TooltipEntry::scaled(
                    showThumbnail
                        ? emoteElement->getEmote()->images.getImage(3.0)
                        : nullptr,
                    element->getTooltip(), getTooltipScale(scale)));
            }
            else if (layeredEmoteElement)
            {
                const auto &layeredEmotes = layeredEmoteElement->getEmotes();
                // Should never be empty but ensure it
                if (!layeredEmotes.empty())
                {
                    std::vector<TooltipEntry> entries;
                    entries.reserve(layeredEmotes.size());

                    const auto &emoteTooltips =
                        layeredEmoteElement->getEmoteTooltips();

                    // Someone performing some tomfoolery could put an emote with tens,
                    // if not hundreds of zero-width emotes on a single emote. If the
                    // tooltip may take up more than three rows, truncate everything else.
                    bool truncating = false;
                    size_t upperLimit = layeredEmotes.size();
                    if (layeredEmotes.size() > TOOLTIP_EMOTE_ENTRIES_LIMIT)
                    {
                        upperLimit = TOOLTIP_EMOTE_ENTRIES_LIMIT - 1;
                        truncating = true;
                    }

                    for (size_t i = 0; i < upperLimit; ++i)
                    {
                        const auto &emote = layeredEmotes[i].ptr;
                        if (i == 0)
                        {
                            // First entry gets a large image and full description
                            auto scale =
                                getSettings()->emoteTooltipScale.getEnum();
                            entries.push_back(TooltipEntry::scaled(
                                showThumbnail ? emote->images.getImage(3.0)
                                              : nullptr,
                                emoteTooltips[i], getTooltipScale(scale)));
                        }
                        else
                        {
                            // Every other entry gets a small image and just the emote name
                            auto scale =
                                getSettings()->emoteTooltipScale.getEnum();
                            entries.push_back(TooltipEntry::scaled(
                                showThumbnail ? emote->images.getImage(1.0)
                                              : nullptr,
                                emote->name.string, getTooltipScale(scale)));
                        }
                    }

                    if (truncating)
                    {
                        entries.push_back({nullptr, "..."});
                    }

                    auto style = layeredEmotes.size() > 2
                                     ? TooltipStyle::Grid
                                     : TooltipStyle::Vertical;
                    this->tooltipWidget_->set(entries, style);
                }
            }
            else if (badgeElement)
            {
                auto scale = getSettings()->emoteTooltipScale.getEnum();
                this->tooltipWidget_->setOne(TooltipEntry::scaled(
                    showThumbnail
                        ? badgeElement->getEmote()->images.getImage(3.0)
                        : nullptr,
                    element->getTooltip(), getTooltipScale(scale)));
            }
        }
        else if (auto *linkElement = dynamic_cast<LinkElement *>(element))
        {
            auto thumbnailSize = getSettings()->thumbnailSize;
            if (linkElement)
            {
                if (linkElement->linkInfo()->isPending())
                {
                    getApp()->getLinkResolver()->resolve(
                        linkElement->linkInfo());
                }
                this->setLinkInfoTooltip(linkElement->linkInfo());
            }
        }
        else
        {
            this->tooltipWidget_->setOne(TooltipEntry{
                .image = nullptr,
                .text = element->getTooltip(),
            });
        }

        this->tooltipWidget_->moveTo(
            event->globalPosition().toPoint() + QPoint(16, 16),
            widgets::BoundsChecking::CursorPosition);
        this->tooltipWidget_->setWordWrap(isLinkValid);
        this->tooltipWidget_->show();
    }

    // check if word has a link
    if (isLinkValid)
    {
        this->setCursor(Qt::PointingHandCursor);
    }
    else
    {
        this->setCursor(Qt::ArrowCursor);
    }
}

void ChannelView::mousePressEvent(QMouseEvent *event)
{
    this->mouseDown.invoke(event);

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    if (!this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex))
    {
        this->setCursor(Qt::ArrowCursor);
        auto &messagesSnapshot = this->getMessagesSnapshot();
        if (messagesSnapshot.size() == 0)
        {
            return;
        }

        // Start selection at the last message at its last index
        if (event->button() == Qt::LeftButton)
        {
            auto lastMessageIndex = messagesSnapshot.size() - 1;
            auto lastMessage = messagesSnapshot[lastMessageIndex];
            auto lastCharacterIndex = lastMessage->getLastCharacterIndex();

            SelectionItem selectionItem(lastMessageIndex, lastCharacterIndex);
            this->setSelection(selectionItem, selectionItem);
        }
        return;
    }

    // check if message is collapsed
    switch (event->button())
    {
        case Qt::LeftButton: {
            if (this->isScrolling_)
            {
                this->disableScrolling();
            }

            this->lastLeftPressPosition_ = event->globalPosition();
            this->isLeftMouseDown_ = true;

            if (layout->flags.has(MessageLayoutFlag::Collapsed))
            {
                return;
            }

            if (getSettings()->linksDoubleClickOnly.getValue())
            {
                this->pause(PauseReason::DoubleClick, 200);
            }

            int index = layout->getSelectionIndex(relativePos);
            auto selectionItem = SelectionItem(messageIndex, index);
            this->setSelection(selectionItem, selectionItem);
        }
        break;

        case Qt::RightButton: {
            if (this->isScrolling_)
            {
                this->disableScrolling();
            }

            this->lastRightPressPosition_ = event->globalPosition();
            this->isRightMouseDown_ = true;
        }
        break;

        case Qt::MiddleButton: {
            const MessageLayoutElement *hoverLayoutElement =
                layout->getElementAt(relativePos);

            if (hoverLayoutElement != nullptr &&
                hoverLayoutElement->getLink().isUrl() &&
                this->isScrolling_ == false)
            {
                break;
            }
            else
            {
                if (this->isScrolling_)
                {
                    this->disableScrolling();
                }
                else if (hoverLayoutElement != nullptr &&
                         hoverLayoutElement->getFlags().has(
                             MessageElementFlag::Username))
                {
                    break;
                }
                else if (this->scrollBar_->isVisible())
                {
                    this->enableScrolling(event->globalPosition());
                }
            }
        }
        break;

        default:;
    }

    this->update();
}

void ChannelView::mouseReleaseEvent(QMouseEvent *event)
{
    // find message
    this->queueLayout();

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    bool foundElement =
        this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex);

    // check if mouse was pressed
    if (event->button() == Qt::LeftButton)
    {
        if (this->isDoubleClick_)
        {
            this->isDoubleClick_ = false;
            // Was actually not a wanted triple-click
            if (std::abs(distanceBetweenPoints(this->lastDoubleClickPosition_,
                                               event->globalPosition())) > 10.F)
            {
                this->clickTimer_.stop();
                return;
            }
        }
        else if (this->isLeftMouseDown_)
        {
            this->isLeftMouseDown_ = false;

            if (std::abs(distanceBetweenPoints(this->lastLeftPressPosition_,
                                               event->globalPosition())) > 15.F)
            {
                return;
            }

            // Triple-clicking a message selects the whole message
            if (foundElement && this->clickTimer_.isActive() &&
                (std::abs(distanceBetweenPoints(this->lastDoubleClickPosition_,
                                                event->globalPosition())) <
                 10.F))
            {
                this->selectWholeMessage(layout.get(), messageIndex);
                return;
            }
        }
        else
        {
            return;
        }
    }
    else if (event->button() == Qt::RightButton)
    {
        if (this->isRightMouseDown_)
        {
            this->isRightMouseDown_ = false;

            if (std::abs(distanceBetweenPoints(this->lastRightPressPosition_,
                                               event->globalPosition())) > 15.F)
            {
                return;
            }
        }
        else
        {
            return;
        }
    }
    else if (event->button() == Qt::MiddleButton)
    {
        if (this->isScrolling_ && this->scrollBar_->isVisible())
        {
            if (event->globalPosition() == this->lastMiddlePressPosition_)
            {
                this->enableScrolling(event->globalPosition());
            }
            else
            {
                this->disableScrolling();
            }

            return;
        }

        if (foundElement)
        {
            const MessageLayoutElement *hoverLayoutElement =
                layout->getElementAt(relativePos);

            if (hoverLayoutElement == nullptr)
            {
                return;
            }
            if (hoverLayoutElement->getFlags().has(
                    MessageElementFlag::Username))
            {
                const auto userName = hoverLayoutElement->getLink().value;
                const auto type = this->hasSourceChannel()
                                      ? this->sourceChannel_->getType()
                                      : this->channel_->getType();
                switch (type)
                {
                    case Channel::Type::TwitchWhispers:
                    case Channel::Type::TwitchLive:
                        QDesktopServices::openUrl(
                            QUrl(u"https://www.twitch.tv/" % userName));
                        break;
                    case Channel::Type::TwitchMentions:
                        openTwitchUsercard(layout->getMessage()->channelName,
                                           userName);
                        break;
                    default:
                        openTwitchUsercard(this->channel_->getName(), userName);
                        break;
                }

                return;
            }
            if (hoverLayoutElement->getLink().isUrl() == false)
            {
                return;
            }
        }
    }
    else
    {
        // not left or right button
        return;
    }

    // no message found
    if (!foundElement)
    {
        // No message at clicked position
        return;
    }

    // message under cursor is collapsed
    if (layout->flags.has(MessageLayoutFlag::Collapsed))
    {
        layout->flags.set(MessageLayoutFlag::Expanded);
        layout->flags.set(MessageLayoutFlag::RequiresLayout);

        this->queueLayout();
        return;
    }

    const MessageLayoutElement *hoverLayoutElement =
        layout->getElementAt(relativePos);

    // handle the click
    this->handleMouseClick(event, hoverLayoutElement, layout);

    this->update();
}

void ChannelView::handleMouseClick(QMouseEvent *event,
                                   const MessageLayoutElement *hoveredElement,
                                   MessageLayoutPtr layout)
{
    switch (event->button())
    {
        case Qt::LeftButton: {
            if (hoveredElement == nullptr)
            {
                return;
            }

            const auto &link = hoveredElement->getLink();
            if (!getSettings()->linksDoubleClickOnly)
            {
                this->handleLinkClick(event, link, layout.get());
            }

            // Invoke to signal from EmotePopup.
            if (link.type == Link::InsertText)
            {
                this->linkClicked.invoke(link);

                if (this->context_ == Context::None)
                {
                    auto *split = dynamic_cast<Split *>(this->parentWidget());
                    if (split)
                    {
                        split->insertTextToInput(link.value);
                    }
                }
            }
        }
        break;
        case Qt::RightButton: {
            // insert user mention to input, only in default context
            if ((this->context_ == Context::None) &&
                (hoveredElement != nullptr))
            {
                auto *split = dynamic_cast<Split *>(this->parentWidget());
                auto insertText = [=](QString text) {
                    if (split)
                    {
                        split->insertTextToInput(text);
                    }
                };
                const auto &link = hoveredElement->getLink();

                if (link.type == Link::UserInfo)
                {
                    // This is terrible because it FPs on messages where the
                    // user mentions themselves
                    bool canReply =
                        QString::compare(link.value,
                                         layout->getMessage()->loginName,
                                         Qt::CaseInsensitive) == 0;
                    UsernameRightClickBehavior action =
                        UsernameRightClickBehavior::Mention;
                    if (canReply)
                    {
                        Qt::KeyboardModifier userSpecifiedModifier =
                            getSettings()->usernameRightClickModifier;

                        if (userSpecifiedModifier ==
                            Qt::KeyboardModifier::NoModifier)
                        {
                            qCWarning(chatterinoCommon)
                                << "sanity check failed: "
                                   "invalid settings detected "
                                   "Settings::usernameRightClickModifier is "
                                   "NoModifier, which should never happen";
                            return;
                        }

                        Qt::KeyboardModifiers modifiers{userSpecifiedModifier};
                        auto isModifierHeld = event->modifiers() == modifiers;

                        if (isModifierHeld)
                        {
                            action = getSettings()
                                         ->usernameRightClickModifierBehavior;
                        }
                        else
                        {
                            action = getSettings()->usernameRightClickBehavior;
                        }
                    }
                    switch (action)
                    {
                        case UsernameRightClickBehavior::Mention: {
                            if (split == nullptr)
                            {
                                return;
                            }

                            if (link.value.startsWith("id:"))
                            {
                                return;
                            }

                            // Insert @username into split input
                            const bool commaMention =
                                getSettings()->mentionUsersWithComma;
                            const bool isFirstWord =
                                split->getInput().isEditFirstWord();
                            auto userMention = formatUserMention(
                                link.value, isFirstWord, commaMention);
                            insertText("@" + userMention + " ");
                        }
                        break;

                        case UsernameRightClickBehavior::Reply: {
                            // Start a new reply if matching user's settings
                            this->setInputReply(layout->getMessagePtr());
                        }
                        break;

                        case UsernameRightClickBehavior::Ignore:
                            break;

                        default: {
                            qCWarning(chatterinoCommon)
                                << "unhandled or corrupted "
                                   "UsernameRightClickBehavior value in "
                                   "ChannelView::handleMouseClick:"
                                << action;
                        }
                        break;  // unreachable
                    }

                    return;
                }

                if (link.type == Link::UserWhisper)
                {
                    insertText("/w " + link.value + " ");
                    return;
                }
            }

            this->addContextMenuItems(hoveredElement, layout, event);
        }
        break;
        case Qt::MiddleButton: {
            if (hoveredElement == nullptr)
            {
                return;
            }

            const auto &link = hoveredElement->getLink();
            if (!getSettings()->linksDoubleClickOnly)
            {
                this->handleLinkClick(event, link, layout.get());
            }
        }
        break;
        default:;
    }
}

void ChannelView::addContextMenuItems(
    const MessageLayoutElement *hoveredElement, MessageLayoutPtr layout,
    QMouseEvent *event)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // Add image options if the element clicked contains an image (e.g. a badge or an emote)
    addImageContextMenuItems(menu, hoveredElement, this, this->channel());

    // Add link options if the element clicked contains a link
    addLinkContextMenuItems(menu, hoveredElement);

    // Add message options
    this->addMessageContextMenuItems(menu, layout);

    // Add Twitch-specific link options if the element clicked contains a link detected as a Twitch username
    this->addTwitchLinkContextMenuItems(menu, hoveredElement);

    // Add hidden options (e.g. copy message ID) if the user held down Shift
    addHiddenContextMenuItems(menu, hoveredElement, layout, event);

    // Add executable command options
    this->addCommandExecutionContextMenuItems(menu, layout);

    menu->popup(QCursor::pos());
    menu->raise();
}

void ChannelView::addMessageContextMenuItems(QMenu *menu,
                                             const MessageLayoutPtr &layout)
{
    // Copy actions
    if (!this->selection_.isEmpty())
    {
        menu->addAction("&Copy selection", [this] {
            crossPlatformCopy(this->getSelectedText());
        });
    }

    menu->addAction("Copy &message", [layout] {
        QString copyString;
        layout->addSelectionText(copyString, 0, INT_MAX,
                                 CopyMode::OnlyTextAndEmotes);

        crossPlatformCopy(copyString);
    });

    menu->addAction("Copy &full message", [layout] {
        QString copyString;
        layout->addSelectionText(copyString, 0, INT_MAX,
                                 CopyMode::EverythingButReplies);

        crossPlatformCopy(copyString);
    });

    // Only display reply option where it makes sense
    auto replyMessage = layout->getMessagePtr();
    if (this->canReplyToMessage(replyMessage))
    {
        switch (replyMessage->isReplyable())
        {
            case Message::ReplyStatus::Replyable: {
                menu->addAction("&Reply to message", [this, replyMessage] {
                    this->setInputReply(replyMessage);
                });
                break;
            }

            case Message::ReplyStatus::NotReplyable: {
                const auto &replyAction =
                    menu->addAction("&Reply to message", [this, replyMessage] {
                        this->setInputReply(replyMessage);
                    });
                replyAction->setEnabled(false);
                break;
            }

            case Message::ReplyStatus::ReplyableWithThread: {
                menu->addAction("&Reply to message", [this, replyMessage] {
                    this->setInputReply(replyMessage);
                });
                menu->addAction(
                    "Reply to &original thread", [this, replyMessage] {
                        this->setInputReply(replyMessage->replyThread->root());
                    });
                break;
            }

            case Message::ReplyStatus::NotReplyableWithThread: {
                const auto &replyAction =
                    menu->addAction("&Reply to message", [this, replyMessage] {
                        this->setInputReply(replyMessage);
                    });
                replyAction->setEnabled(false);

                menu->addAction(
                    "Reply to &original thread", [this, replyMessage] {
                        this->setInputReply(replyMessage->replyThread->root());
                    });
                break;
            }

            case Message::ReplyStatus::NotReplyableDueToThread: {
                const auto &replyAction =
                    menu->addAction("&Reply to message", [this, replyMessage] {
                        this->setInputReply(replyMessage);
                    });
                replyAction->setEnabled(false);

                const auto &replyThreadAction = menu->addAction(
                    "Reply to &original thread", [this, replyMessage] {
                        this->setInputReply(replyMessage->replyThread->root());
                    });

                replyThreadAction->setEnabled(false);
                break;
            }
        }

        if (auto threadMessage = layout->getMessagePtr();
            threadMessage->replyThread != nullptr)
        {
            menu->addAction("View &thread", [this, threadMessage] {
                this->showReplyThreadPopup(threadMessage);
            });
        }
    }

    // Add search action when text is selected and search feature is enabled
    if (!this->selection_.isEmpty() && getSettings()->searchEnabled.getValue())
    {
        QString searchURL = getSettings()->searchEngineUrl.getValue();
        QString searchName = getSettings()->searchEngineName.getValue();

        if (!searchURL.isEmpty())
        {
            QString actionText =
                searchName.isEmpty() ? "&Search" : "&Search with " + searchName;

            if (getSettings()->searchIncognito && supportsIncognitoLinks())
            {
                actionText += " in private mode";
            }

            menu->addAction(actionText, [this, searchURL] {
                QString query = this->getSelectedText().trimmed();
                QString encodedQuery = QUrl::toPercentEncoding(query);
                QString url = searchURL + encodedQuery;

                if (getSettings()->searchIncognito && supportsIncognitoLinks())
                {
                    openLinkIncognito(url);
                }
                else
                {
                    QDesktopServices::openUrl(QUrl(url));
                }
            });
        }
    }

    const auto message = layout->getMessage();
    const auto moderationChannel = this->hasSourceChannel()
                                       ? this->sourceChannel_
                                       : this->underlyingChannel_;
    auto *mergedModerationChannel =
        moderationChannel
            ? dynamic_cast<MergedChannel *>(moderationChannel.get())
            : nullptr;
    const bool canModerateYouTube =
        message->platform == MessagePlatform::YouTube &&
        mergedModerationChannel != nullptr &&
        mergedModerationChannel->youtubeLiveChat() != nullptr &&
        mergedModerationChannel->youtubeLiveChat()->hasModeratorPrivileges();
    const bool canModerateMessage =
        message->platform == MessagePlatform::YouTube
            ? canModerateYouTube
            : moderationChannel && moderationChannel->hasModRights();

    if (!message->id.isEmpty() && canModerateMessage)
    {
        menu->addSeparator();
        menu->addAction(
            "&Delete message",
            [id = message->id, platform = message->platform,
             moderationChannel] {
                auto *twitchChannel = dynamic_cast<TwitchChannel *>(
                    moderationChannel.get());
                if (twitchChannel)
                {
                    twitchChannel->deleteMessagesAs(
                        id, getApp()->getAccounts()->twitch.getCurrent().get());
                }
                else if (auto *kc = dynamic_cast<KickChannel *>(
                             moderationChannel.get()))
                {
                    kc->deleteMessage(id);
                }
                else if (auto *mergedChannel = dynamic_cast<MergedChannel *>(
                             moderationChannel.get()))
                {
                    if (platform == MessagePlatform::YouTube &&
                        mergedChannel->youtubeLiveChat() != nullptr)
                    {
                        mergedChannel->youtubeLiveChat()->deleteMessage(id);
                    }
                    else if (platform == MessagePlatform::Kick)
                    {
                        auto *kickChannel = dynamic_cast<KickChannel *>(
                            mergedChannel->kickChannel().get());
                        if (kickChannel != nullptr)
                        {
                            kickChannel->deleteMessage(id);
                        }
                    }
                    else
                    {
                        auto *mergedTwitchChannel =
                            dynamic_cast<TwitchChannel *>(
                                mergedChannel->twitchChannel().get());
                        if (mergedTwitchChannel != nullptr)
                        {
                            mergedTwitchChannel->deleteMessagesAs(
                                id, getApp()
                                        ->getAccounts()
                                        ->twitch.getCurrent()
                                        .get());
                        }
                    }
                }
            });

        auto messagePtr = layout->getMessagePtr();
        if (commands::canPinChatMessage(moderationChannel, messagePtr))
        {
            menu->addAction("&Pin message", [moderationChannel, messagePtr] {
                commands::pinChatMessage(moderationChannel, messagePtr, 1200);
            });
        }
    }

    bool isSearch = this->context_ == Context::Search;
    bool isReplyOrUserCard = (this->context_ == Context::ReplyThread ||
                              this->context_ == Context::UserCard) &&
                             this->split_ != nullptr;
    bool isMentions =
        this->channel()->getType() == Channel::Type::TwitchMentions;
    bool isAutomod = this->channel()->getType() == Channel::Type::TwitchAutomod;
    if (isSearch || isMentions || isReplyOrUserCard || isAutomod)
    {
        auto messagePtr = layout->getMessagePtr();
        menu->addAction("&Go to message", [this, messagePtr, isSearch,
                                           isMentions, isReplyOrUserCard,
                                           isAutomod] {
            if (isSearch)
            {
                if (const auto &search =
                        dynamic_cast<SearchPopup *>(this->parentWidget()))
                {
                    search->goToMessage(messagePtr);
                }
            }
            else if (isMentions || isAutomod)
            {
                getApp()->getWindows()->scrollToMessage(messagePtr);
            }
            else if (isReplyOrUserCard)
            {
                // If the thread is in the mentions or automod channel,
                // we need to find the original split.
                const auto type = this->split_->getChannel()->getType();
                if (type == Channel::Type::TwitchMentions ||
                    type == Channel::Type::TwitchAutomod)
                {
                    getApp()->getWindows()->scrollToMessage(messagePtr);
                }
                else
                {
                    this->split_->getChannelView().scrollToMessage(messagePtr);
                }
            }
        });
    }
}

void ChannelView::addTwitchLinkContextMenuItems(
    QMenu *menu, const MessageLayoutElement *hoveredElement)
{
    if (hoveredElement == nullptr)
    {
        return;
    }

    const auto &link = hoveredElement->getLink();

    if (link.type != Link::Url)
    {
        return;
    }

    static QRegularExpression twitchChannelRegex(
        R"(^(?:https?:\/\/)?(?:www\.|go\.)?twitch\.tv\/(?:popout\/)?(?<username>[a-z0-9_]{3,}))",
        QRegularExpression::CaseInsensitiveOption);
    static QSet<QString> ignoredUsernames{
        "directory",      //
        "downloads",      //
        "drops",          //
        "friends",        //
        "inventory",      //
        "jobs",           //
        "login",          //
        "messages",       //
        "payments",       //
        "profile",        //
        "security",       //
        "settings",       //
        "signup",         //
        "subscriptions",  //
        "turbo",          //
        "videos",         //
        "wallet",         //
    };

    auto twitchMatch = twitchChannelRegex.match(link.value);
    auto twitchUsername = twitchMatch.captured("username");
    if (!twitchUsername.isEmpty() && !ignoredUsernames.contains(twitchUsername))
    {
        menu->addSeparator();
        menu->addAction("&Open in new split", [twitchUsername, this] {
            this->openChannelIn.invoke(twitchUsername,
                                       FromTwitchLinkOpenChannelIn::Split);
        });
        menu->addAction("Open in new &tab", [twitchUsername, this] {
            this->openChannelIn.invoke(twitchUsername,
                                       FromTwitchLinkOpenChannelIn::Tab);
        });

        menu->addSeparator();
        menu->addAction("Open player in &browser", [twitchUsername, this] {
            this->openChannelIn.invoke(
                twitchUsername, FromTwitchLinkOpenChannelIn::BrowserPlayer);
        });
        menu->addAction("Open in &streamlink", [twitchUsername, this] {
            this->openChannelIn.invoke(twitchUsername,
                                       FromTwitchLinkOpenChannelIn::Streamlink);
        });

        if (!getSettings()->customURIScheme.getValue().isEmpty())
        {
            menu->addAction("Open in custom &player", [twitchUsername, this] {
                this->openChannelIn.invoke(
                    twitchUsername, FromTwitchLinkOpenChannelIn::CustomPlayer);
            });
        }
    }
}

void ChannelView::addCommandExecutionContextMenuItems(
    QMenu *menu, const MessageLayoutPtr &layout)
{
    /* Get commands to be displayed in context menu;
     * only those that had the showInMsgContextMenu check box marked in the Commands page */
    std::vector<Command> cmds;
    for (const auto &cmd : getApp()->getCommands()->items)
    {
        if (cmd.showInMsgContextMenu)
        {
            cmds.push_back(cmd);
        }
    }

    if (cmds.empty())
    {
        return;
    }

    menu->addSeparator();
    auto *executeAction = menu->addAction("&Execute command");
    auto *cmdMenu = new QMenu(menu);
    executeAction->setMenu(cmdMenu);

    for (auto &cmd : cmds)
    {
        QString inputText = this->selection_.isEmpty()
                                ? layout->getMessage()->messageText
                                : this->getSelectedText();

        inputText.push_front(cmd.name + " ");

        cmdMenu->addAction(cmd.name, [this, layout, cmd, inputText] {
            ChannelPtr channel;

            /* Search popups and user message history's underlyingChannels aren't of type TwitchChannel, but
             * we would still like to execute commands from them. Use their source channel instead if applicable. */
            if (this->hasSourceChannel())
            {
                channel = this->sourceChannel();
            }
            else
            {
                channel = this->underlyingChannel_;
            }
            channel = commandChannelForMessage(
                channel, layout->getMessage()->platform);
            if (!channel)
            {
                return;
            }
            auto *split = dynamic_cast<Split *>(this->parentWidget());
            QString userText;
            if (split)
            {
                userText = split->getInput().getInputText();
            }

            // Execute command through right-clicking a message -> Execute command
            QString value = getApp()->getCommands()->execCustomCommand(
                inputText.split(' '), cmd, true, channel, layout->getMessage(),
                {
                    {"input.text", userText},
                });

            value = getApp()->getCommands()->execCommand(
                value, channel, false, layout->getMessage());

            channel->sendMessage(value);
        });
    }
}

void ChannelView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }

    std::shared_ptr<MessageLayout> layout;
    QPointF relativePos;
    int messageIndex;

    if (!this->tryGetMessageAt(event->pos(), layout, relativePos, messageIndex))
    {
        return;
    }

    this->isDoubleClick_ = true;
    this->lastDoubleClickPosition_ = event->globalPosition();
    this->clickTimer_.start();

    // message under cursor is collapsed
    if (layout->flags.has(MessageLayoutFlag::Collapsed))
    {
        return;
    }

    const MessageLayoutElement *hoverLayoutElement =
        layout->getElementAt(relativePos);

    if (hoverLayoutElement == nullptr)
    {
        // XXX: this is duplicate work
        auto idx = layout->getSelectionIndex(relativePos);
        SelectionItem item(messageIndex, idx);
        this->doubleClickSelection_ = {item, item};
        return;
    }

    auto [wordStart, wordEnd] =
        layout->getWordBounds(hoverLayoutElement, relativePos);

    this->doubleClickSelection_ = {SelectionItem(messageIndex, wordStart),
                                   SelectionItem(messageIndex, wordEnd)};
    this->setSelection(this->doubleClickSelection_);

    if (getSettings()->linksDoubleClickOnly)
    {
        const auto &link = hoverLayoutElement->getLink();
        this->handleLinkClick(event, link, layout.get());
    }
}

void ChannelView::hideEvent(QHideEvent * /*event*/)
{
    this->activityTimeRefreshTimer_.stop();

    for (const auto &layout : this->messagesWithBuffers_)
    {
        layout->deleteBuffer();
    }

    this->messagesWithBuffers_.clear();
}

void ChannelView::showUserInfoPopup(const QString &userName,
                                    MessagePlatform platform,
                                    const QString &alternativePopoutChannel,
                                    const QString &platformUserID)
{
    if (!this->split_)
    {
        qCWarning(chatterinoCommon)
            << "Tried to show user info for" << userName
            << "but the channel view doesn't belong to a split.";
        return;
    }

    auto *userPopup = new UserInfoPopup(false, this->split_);

    auto openingChannel = this->hasSourceChannel() ? this->sourceChannel_
                                                   : this->underlyingChannel_;
    ChannelPtr contextChannel;
    auto *mergedChannel =
        openingChannel ? dynamic_cast<MergedChannel *>(openingChannel.get())
                       : nullptr;
    if (platform == MessagePlatform::Kick)
    {
        if (mergedChannel != nullptr)
        {
            contextChannel = mergedChannel->kickChannel();
        }
        if (!contextChannel && openingChannel && openingChannel->isKickChannel())
        {
            contextChannel = openingChannel;
        }
        if (!contextChannel)
        {
            contextChannel = getApp()->getKickChatServer()->findBySlug(
                alternativePopoutChannel);
        }
        if (!contextChannel)
        {
            contextChannel = Channel::getEmpty();
        }
    }
    else if (platform == MessagePlatform::YouTube ||
             platform == MessagePlatform::TikTok)
    {
        contextChannel = Channel::getEmpty();
    }
    else
    {
        if (mergedChannel != nullptr)
        {
            contextChannel = mergedChannel->twitchChannel();
        }
        if (!contextChannel && openingChannel &&
            openingChannel->isTwitchChannel())
        {
            contextChannel = openingChannel;
        }
        if (!contextChannel)
        {
            contextChannel = getApp()->getTwitch()->getChannelOrEmpty(
                alternativePopoutChannel);
        }
    }
    userPopup->setData(userName, contextChannel, openingChannel, platform,
                       platformUserID);

    QPoint offset(userPopup->width() / 3, userPopup->height() / 5);
    userPopup->moveTo(QCursor::pos() - offset,
                      widgets::BoundsChecking::CursorPosition);
    userPopup->show();
}

bool ChannelView::mayContainMessage(const MessagePtr &message)
{
    switch (this->channel()->getType())
    {
        case Channel::Type::Direct:
        case Channel::Type::Twitch:
        case Channel::Type::TwitchWatching:
            // XXX: system messages may not have the channel set
            return message->flags.has(MessageFlag::System) ||
                   this->channel()->getName() == message->channelName;
        case Channel::Type::TwitchWhispers:
            return message->flags.has(MessageFlag::Whisper);
        case Channel::Type::TwitchMentions:
            return message->flags.has(MessageFlag::Highlighted);
        case Channel::Type::TwitchLive:
            return message->flags.has(MessageFlag::System);
        case Channel::Type::TwitchAutomod:
            return message->flags.has(MessageFlag::AutoMod);
        case Channel::Type::TwitchEnd:  // TODO: not used?
        case Channel::Type::None:       // Unspecific
        case Channel::Type::Misc:       // Unspecific
            return true;
        default:
            return true;  // unreachable
    }
}

void ChannelView::handleLinkClick(QMouseEvent *event, const Link &link,
                                  MessageLayout *layout)
{
    if (event->button() != Qt::LeftButton &&
        event->button() != Qt::MiddleButton)
    {
        return;
    }

    switch (link.type)
    {
        case Link::UserWhisper:
        case Link::UserInfo: {
            auto user = link.value;
            this->showUserInfoPopup(user, layout->getMessage()->platform,
                                    layout->getMessage()->channelName,
                                    layout->getMessage()->userID);
        }
        break;

        case Link::Url: {
            if (getSettings()->openLinksIncognito && supportsIncognitoLinks())
            {
                openLinkIncognito(link.value);
            }
            else
            {
                QDesktopServices::openUrl(QUrl(link.value));
            }
        }
        break;

        case Link::UserAction: {
            QString value = link.value;

            ChannelPtr channel = this->underlyingChannel_;
            auto *searchPopup =
                dynamic_cast<SearchPopup *>(this->parentWidget());
            if (searchPopup != nullptr)
            {
                auto *split =
                    dynamic_cast<Split *>(searchPopup->parentWidget());
                if (split != nullptr)
                {
                    channel = split->getChannel();
                }
            }

            channel = commandChannelForMessage(
                channel, layout->getMessage()->platform);
            if (!channel)
            {
                break;
            }

            // Execute command clicking a moderator button
            value = getApp()->getCommands()->execCustomCommand(
                QStringList(), Command{"(modaction)", value}, true, channel,
                layout->getMessage());

            value = getApp()->getCommands()->execCommand(
                value, channel, false, layout->getMessage());

            channel->sendMessage(value);
        }
        break;

        case Link::AutoModAllow: {
            getApp()->getAccounts()->twitch.getCurrent()->autoModAllow(
                link.value, this->channel());
        }
        break;

        case Link::AutoModDeny: {
            getApp()->getAccounts()->twitch.getCurrent()->autoModDeny(
                link.value, this->channel());
        }
        break;

        case Link::OpenAccountsPage: {
            SettingsDialog::showDialog(this,
                                       SettingsDialogPreference::Accounts);
        }
        break;
        case Link::JumpToChannel: {
            // Get all currently open pages
            QList<SplitContainer *> openPages;

            auto &nb = getApp()->getWindows()->getMainWindow().getNotebook();
            for (int i = 0; i < nb.getPageCount(); ++i)
            {
                openPages.push_back(
                    static_cast<SplitContainer *>(nb.getPageAt(i)));
            }
            QStringView searchName = link.value;
            bool searchKickChannel = link.value.startsWith(u":kick:");
            if (searchKickChannel)
            {
                searchName = searchName.sliced(sizeof(":kick:") - 1);
            }

            for (auto *page : openPages)
            {
                auto splits = page->getSplits();

                // Search for channel matching link in page/split container
                // TODO(zneix): Consider opening a channel if it's closed (?)
                auto it = std::ranges::find_if(
                    splits, [searchName, searchKickChannel](Split *split) {
                        return split->getChannel()->getName() == searchName &&
                               split->getChannel()->isKickChannel() ==
                                   searchKickChannel;
                    });

                if (it != splits.end())
                {
                    // Select SplitContainer and Split itself where mention message was sent
                    // TODO(zneix): Try exploring ways of scrolling to a certain message as well
                    nb.select(page);

                    Split *split = *it;
                    page->setSelected(split);
                    break;
                }
            }
        }
        break;
        case Link::CopyToClipboard: {
            crossPlatformCopy(link.value);
        }
        break;
        case Link::Reconnect: {
            this->underlyingChannel_.get()->reconnect();
        }
        break;
        case Link::ReplyToMessage: {
            if (this->canReplyToMessage(layout->getMessagePtr()) &&
                layout->getMessagePtr()->isReplyable() !=
                Message::ReplyStatus::NotReplyable)
            {
                this->setInputReply(layout->getMessagePtr());
            }
        }
        break;
        case Link::ViewThread: {
            this->showReplyThreadPopup(layout->getMessagePtr());
        }
        break;
        case Link::JumpToMessage: {
            if (this->context_ == Context::Search)
            {
                auto *search =
                    dynamic_cast<SearchPopup *>(this->parentWidget());
                if (search != nullptr)
                {
                    search->goToMessageId(link.value);
                }
                return;
            }

            this->scrollToMessageId(link.value);
        }
        break;

        default:;
    }
}

bool ChannelView::tryGetMessageAt(QPointF p,
                                  std::shared_ptr<MessageLayout> &_message,
                                  QPointF &relativePos, int &index)
{
    auto &messagesSnapshot = this->getMessagesSnapshot();

    const auto start = size_t(this->scrollBar_->getRelativeCurrentValue());

    if (start >= messagesSnapshot.size())
    {
        return false;
    }

    qreal y = -(messagesSnapshot[start]->getHeight() *
                (fmod(this->scrollBar_->getRelativeCurrentValue(), 1)));

    for (size_t i = start; i < messagesSnapshot.size(); ++i)
    {
        auto message = messagesSnapshot[i];

        if (p.y() < y + message->getHeight())
        {
            relativePos = QPointF(p.x(), p.y() - y);
            _message = message;
            index = i;
            return true;
        }

        y += message->getHeight();
    }

    return false;
}

int ChannelView::getLayoutWidth() const
{
    if (this->scrollBar_->isVisible())
    {
        return int(this->width() - SCROLLBAR_PADDING * this->scale());
    }

    return this->width();
}

void ChannelView::selectWholeMessage(MessageLayout *layout, int &messageIndex)
{
    SelectionItem msgStart(messageIndex,
                           layout->getFirstMessageCharacterIndex());
    SelectionItem msgEnd(messageIndex, layout->getLastCharacterIndex());
    this->setSelection(msgStart, msgEnd);
}

void ChannelView::enableScrolling(const QPointF &scrollStart)
{
    this->isScrolling_ = true;
    this->lastMiddlePressPosition_ = scrollStart;
    // The line below prevents a sudden jerk at the beginning
    this->currentMousePosition_ = scrollStart;

    this->scrollTimer_.start();

    if (!QGuiApplication::overrideCursor())
    {
        QGuiApplication::setOverrideCursor(this->cursors_.neutral);
    }
}

void ChannelView::disableScrolling()
{
    this->isScrolling_ = false;
    this->scrollTimer_.stop();
    QGuiApplication::restoreOverrideCursor();
}

void ChannelView::scrollUpdateRequested()
{
    const qreal dpi = this->devicePixelRatioF();
    const qreal delta = dpi * (this->currentMousePosition_.y() -
                               this->lastMiddlePressPosition_.y());
    const int cursorHeight =
        std::max(16, this->cursors_.neutral.pixmap().height());

    if (fabs(delta) <= cursorHeight * dpi)
    {
        /*
         * If within an area close to the initial position, don't do any
         * scrolling at all.
         */
        QGuiApplication::changeOverrideCursor(this->cursors_.neutral);
        return;
    }

    qreal offset;
    if (delta > 0)
    {
        QGuiApplication::changeOverrideCursor(this->cursors_.down);
        offset = delta - cursorHeight;
    }
    else
    {
        QGuiApplication::changeOverrideCursor(this->cursors_.up);
        offset = delta + cursorHeight;
    }

    // "Good" feeling multiplier found by trial-and-error
    const qreal multiplier(0.02);
    this->scrollBar_->offset(multiplier * offset);
}

void ChannelView::setInputReply(const MessagePtr &message)
{
    assertInGuiThread();

    if (message == nullptr || this->split_ == nullptr)
    {
        return;
    }

    const auto channelType = this->channel()->getType();
    if (channelType == Channel::Type::TwitchMentions ||
        channelType == Channel::Type::TwitchAutomod)
    {
        getApp()->getWindows()->scrollToMessage(message, true);
        return;
    }

    auto replyChannel = this->underlyingChannel_;
    if (auto *merged = dynamic_cast<MergedChannel *>(replyChannel.get()))
    {
        switch (message->platform)
        {
            case MessagePlatform::Kick:
                replyChannel = merged->kickChannel();
                break;
            case MessagePlatform::AnyOrTwitch:
                replyChannel = merged->twitchChannel();
                break;
            default:
                qCWarning(chatterinoCommon)
                    << "Failed to create reply thread for unsupported merged "
                       "platform"
                    << qmagicenum::enumNameString(message->platform);
                return;
        }
    }

    if (!message->replyThread)
    {
        // Message did not already have a thread attached, try to find or create one
        auto *tc = dynamic_cast<TwitchChannel *>(replyChannel.get());
        auto *kc = dynamic_cast<KickChannel *>(replyChannel.get());

        if (tc)
        {
            tc->getOrCreateThread(message);
        }
        else if (kc)
        {
            kc->getOrCreateThread(message->id);
        }
        else
        {
            qCWarning(chatterinoCommon) << "Failed to create new reply thread";
            // Unable to create new reply thread.
            // TODO(dnsge): Should probably notify user?
            return;
        }
    }

    this->split_->setInputReply(message);
}

void ChannelView::showReplyThreadPopup(const MessagePtr &message)
{
    if (message == nullptr || message->replyThread == nullptr)
    {
        return;
    }

    if (!this->split_)
    {
        qCWarning(chatterinoCommon)
            << "Tried to show reply thread popup but the "
               "channel view doesn't belong to a split.";
        return;
    }

    auto *popup =
        new ReplyThreadPopup(getSettings()->autoCloseThreadPopup, this->split_);

    popup->setThread(message->replyThread);

    QPoint offset(int(150 * this->scale()), int(70 * this->scale()));
    popup->showAndMoveTo(QCursor::pos() - offset,
                         widgets::BoundsChecking::CursorPosition);
    popup->giveFocus(Qt::MouseFocusReason);
}

ChannelView::Context ChannelView::getContext() const
{
    return this->context_;
}

bool ChannelView::canReplyToMessages() const
{
    if (this->context_ == ChannelView::Context::ReplyThread ||
        this->context_ == ChannelView::Context::Search)
    {
        return false;
    }

    assert(this->channel_ != nullptr);
    auto channel =
        this->underlyingChannel_ != nullptr ? this->underlyingChannel_
                                            : this->channel_;

    if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
    {
        return merged->twitchChannel() != nullptr ||
               merged->kickChannel() != nullptr;
    }

    if (!channel->isTwitchOrKickChannel())
    {
        return false;
    }

    if (channel->getType() == Channel::Type::TwitchWhispers ||
        channel->getType() == Channel::Type::TwitchLive)
    {
        return false;
    }

    return true;
}

bool ChannelView::canReplyToMessage(const MessagePtr &message) const
{
    if (!this->canReplyToMessages() || message == nullptr)
    {
        return false;
    }

    auto channel =
        this->underlyingChannel_ != nullptr ? this->underlyingChannel_
                                            : this->channel_;
    const auto *merged = dynamic_cast<MergedChannel *>(channel.get());
    if (merged == nullptr)
    {
        return true;
    }

    switch (message->platform)
    {
        case MessagePlatform::Kick:
            return merged->kickChannel() != nullptr;
        case MessagePlatform::AnyOrTwitch:
            return merged->twitchChannel() != nullptr;
        default:
            return false;
    }
}

void ChannelView::setLinkInfoTooltip(LinkInfo *info)
{
    assert(info);

    auto thumbnailSize = getSettings()->thumbnailSize;

    ImagePtr thumbnail;
    if (info->hasThumbnail() && thumbnailSize > 0)
    {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideLinkThumbnails)
        {
            thumbnail = Image::fromResourcePixmap(getResources().streamerMode);
        }
        else
        {
            thumbnail = info->thumbnail();
        }
    }

    this->tooltipWidget_->setOne({
        .image = thumbnail,
        .text = info->tooltip(),
        .customWidth = thumbnailSize,
        .customHeight = thumbnailSize,
    });

    if (info->isLoaded())
    {
        this->pendingLinkInfo_.clear();
        return;  // Either resolved or errored (can't change anymore)
    }

    // listen to changes

    if (this->pendingLinkInfo_.data() == info)
    {
        return;  // same info - already registered
    }

    if (this->pendingLinkInfo_)
    {
        QObject::disconnect(this->pendingLinkInfo_.data(),
                            &LinkInfo::stateChanged, this, nullptr);
    }
    QObject::connect(info, &LinkInfo::stateChanged, this,
                     &ChannelView::pendingLinkInfoStateChanged);
    this->pendingLinkInfo_ = info;
}

void ChannelView::pendingLinkInfoStateChanged()
{
    if (!this->pendingLinkInfo_)
    {
        return;
    }
    this->setLinkInfoTooltip(this->pendingLinkInfo_.data());
    this->tooltipWidget_->applyLastBoundsCheck();
}

void ChannelView::updateID()
{
    if (!this->underlyingChannel_)
    {
        // cannot update
        return;
    }

    std::size_t seed = 0;
    auto first = qHash(this->underlyingChannel_->getName());
    auto second = qHash(this->getFilterIds());

    boost::hash_combine(seed, first);
    boost::hash_combine(seed, second);
    boost::hash_combine(seed, this->underlyingChannel_->getType());
    boost::hash_combine(seed, this->split_ != nullptr &&
                                  this->split_->filterActivity() &&
                                  !this->split_->isActivityPane());
    boost::hash_combine(seed, this->split_ != nullptr
                                  ? this->split_->tiktokActivityMinimumDiamonds()
                                  : 0U);
    boost::hash_combine(seed, this->split_ != nullptr
                                  ? this->split_->twitchActivityMinimumBits()
                                  : 100U);
    boost::hash_combine(seed, this->split_ != nullptr
                                  ? this->split_->kickActivityMinimumKicks()
                                  : 100U);

    this->id_ = seed;
}

ChannelView::ChannelViewID ChannelView::getID() const
{
    return this->id_;
}

}  // namespace chatterino
