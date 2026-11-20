// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/Split.hpp"
#include "widgets/scenes/ThemeSceneHost.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/tiktok/TikTokLiveChat.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/CustomPlayer.hpp"
#include "util/StreamLink.hpp"
#include "widgets/ChatterListWidget.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/dialogs/QualityPopup.hpp"
#include "widgets/dialogs/SelectChannelDialog.hpp"
#include "widgets/dialogs/SelectChannelFiltersDialog.hpp"
#include "widgets/dialogs/SplitSettingsDialog.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/DebugPopup.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/helper/SearchPopup.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/OverlayWindow.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/DraggedSplit.hpp"
#include "widgets/splits/ModView.hpp"
#include "widgets/splits/PriorityChat.hpp"
#include "widgets/splits/PinnedMessageBar.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/splits/SplitOverlay.hpp"
#include "widgets/splits/StreamDatabaseBadgeBar.hpp"
#include "widgets/splits/TwitchPollsAndPredictionsBar.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDrag>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMovie>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace chatterino {
namespace {
// A layout-only chat column keeps ChannelView parented to Split. Several chat
// input, rendering and focus paths deliberately depend on that relationship.
constexpr qreal ALERTS_PRIMARY_RATIO = 0.7;
constexpr qreal ALERTS_SECONDARY_RATIO = 0.3;
constexpr qreal MIN_ACTIVITY_MESSAGE_SCALE = 0.75;
constexpr qreal MAX_ACTIVITY_MESSAGE_SCALE = 1.1;
constexpr qreal MIN_SLOWER_CHAT_MESSAGES_PER_SECOND = 0.25;
constexpr qreal MAX_SLOWER_CHAT_MESSAGES_PER_SECOND = 20.0;
constexpr auto STREAMDATABASE_LOGIN = "streamdatabase";

bool isTwitchSpecialChannelType(Channel::Type type)
{
    switch (type)
    {
        case Channel::Type::TwitchWhispers:
        case Channel::Type::TwitchWatching:
        case Channel::Type::TwitchMentions:
        case Channel::Type::TwitchLive:
        case Channel::Type::TwitchAutomod:
            return true;
        default:
            return false;
    }
}

PlatformIndicatorMode defaultPlatformIndicatorMode(bool isActivityPane)
{
    if (isActivityPane)
    {
        return PlatformIndicatorMode::LineColor;
    }

    return getSettings()->mergedPlatformIndicatorMode.getEnum();
}

PlatformIndicatorMode normalizedPlatformIndicatorMode(
    PlatformIndicatorMode value, bool isActivityPane)
{
    switch (value)
    {
        case PlatformIndicatorMode::None:
        case PlatformIndicatorMode::LineColor:
        case PlatformIndicatorMode::Badge:
        case PlatformIndicatorMode::Both:
            return value;
    }

    return defaultPlatformIndicatorMode(isActivityPane);
}

bool mergedSplitHasTikTokEnabled(const Split *split)
{
    if (!split)
    {
        return false;
    }

    auto *merged = dynamic_cast<MergedChannel *>(split->getChannel().get());
    return merged != nullptr && merged->config().tiktokEnabled;
}

bool splitHasTwitchActivity(const Split *split)
{
    if (!split)
    {
        return false;
    }

    auto *merged = dynamic_cast<MergedChannel *>(split->getChannel().get());
    if (merged != nullptr)
    {
        return merged->config().twitchEnabled;
    }

    const auto channel = split->getChannel();
    return channel != nullptr && channel->getType() == Channel::Type::Twitch;
}

const TwitchChannel *streamDatabaseTwitchChannel(Channel *channel)
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        return twitch;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<TwitchChannel *>(merged->twitchChannel().get());
    }

    return nullptr;
}

bool splitIsStreamDatabase(const Split *split)
{
    if (split == nullptr)
    {
        return false;
    }

    auto *twitch = streamDatabaseTwitchChannel(split->getChannel().get());
    return twitch != nullptr &&
           twitch->getName().compare(QString::fromLatin1(STREAMDATABASE_LOGIN),
                                     Qt::CaseInsensitive) == 0;
}

bool splitHasKickActivity(const Split *split)
{
    if (!split)
    {
        return false;
    }

    auto *merged = dynamic_cast<MergedChannel *>(split->getChannel().get());
    if (merged != nullptr)
    {
        return merged->config().kickEnabled;
    }

    const auto channel = split->getChannel();
    return channel != nullptr && channel->getType() == Channel::Type::Kick;
}

const TwitchChannel *viewerListTwitchChannel(Channel *channel)
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        return twitch;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<TwitchChannel *>(merged->twitchChannel().get());
    }

    return nullptr;
}

TwitchChannel *roomModeTwitchChannel(Channel *channel)
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        return twitch;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<TwitchChannel *>(merged->twitchChannel().get());
    }

    return nullptr;
}

KickChannel *roomModeKickChannel(Channel *channel)
{
    if (auto *kick = dynamic_cast<KickChannel *>(channel))
    {
        return kick;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<KickChannel *>(merged->kickChannel().get());
    }

    return nullptr;
}

QStringList normalizedFilterIds(const QList<QUuid> &ids)
{
    QStringList out;
    out.reserve(ids.size());
    for (const auto &id : ids)
    {
        out.append(id.toString(QUuid::WithoutBraces));
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

QJsonObject encodeChannelSignature(IndirectChannel channel)
{
    return Split::activityChannelSignature(std::move(channel));
}

const auto ALERTS_FILTER_TEXT = QStringLiteral(
    "flags.sub_message || flags.elevated_message || flags.cheer_message || "
    "flags.watch_streak || flags.raid_message");
const auto LEGACY_ALERTS_FILTER_TEXT = QStringLiteral(
    "flags.sub_message || flags.elevated_message || flags.cheer_message");

std::optional<QUuid> findAlertsFilterId()
{
    const auto filters = getSettings()->filterRecords.readOnly();
    for (const auto &filter : *filters)
    {
        if (filter && (filter->getFilter() == ALERTS_FILTER_TEXT ||
                       filter->getFilter() == LEGACY_ALERTS_FILTER_TEXT))
        {
            return filter->getId();
        }
    }

    return std::nullopt;
}

std::optional<QUuid> ensureAlertsFilter()
{
    static const auto filterName = QStringLiteral("Mergerino events");
    // Upgrade only unchanged filters generated by Mergerino, preserving IDs
    // referenced by saved panes. Leave user-customized filters alone.
    std::vector<std::pair<int, FilterRecordPtr>> migrations;
    {
        const auto filters = getSettings()->filterRecords.readOnly();
        for (int i = static_cast<int>(filters->size()) - 1; i >= 0; --i)
        {
            const auto &filter = (*filters)[i];
            if (filter &&
                filter->getName() == QStringLiteral("Mergerino donations + subs") &&
                filter->getFilter() == LEGACY_ALERTS_FILTER_TEXT)
            {
                auto migrated = std::make_shared<FilterRecord>(
                    filterName, ALERTS_FILTER_TEXT, filter->getId());
                if (migrated->valid())
                {
                    migrations.emplace_back(i, std::move(migrated));
                }
            }
        }
    }
    for (const auto &[index, migrated] : migrations)
    {
        getSettings()->filterRecords.removeAt(index);
        getSettings()->filterRecords.append(migrated);
    }
    if (!migrations.empty())
    {
        // Synchronize serialized records before saving the migrated filters.
        getSettings()->filterRecords.delayedItemsChanged.invoke();
        (void)getSettings()->requestSave();
    }
    if (const auto existing = findAlertsFilterId())
    {
        return existing;
    }

    auto filter = std::make_shared<FilterRecord>(filterName, ALERTS_FILTER_TEXT);
    if (!filter->valid())
    {
        return std::nullopt;
    }

    getSettings()->filterRecords.append(filter);
    getSettings()->filterRecords.delayedItemsChanged.invoke();
    (void)getSettings()->requestSave();
    return filter->getId();
}

bool splitMatchesChannelAndFilters(Split *split,
                                   const QJsonObject &channelSignature,
                                   const QList<QUuid> &filterIds)
{
    if (!split)
    {
        return false;
    }

    return encodeChannelSignature(split->getIndirectChannel()) ==
               channelSignature &&
           normalizedFilterIds(split->getFilters()) ==
               normalizedFilterIds(filterIds);
}

std::vector<Split *> findLinkedActivityPanes(Split *source)
{
    std::vector<Split *> panes;
    if (!source || source->isUtilityPane())
    {
        return panes;
    }

    auto *container = dynamic_cast<SplitContainer *>(source->parentWidget());
    if (!container)
    {
        return panes;
    }
    for (auto *split : container->getSplits())
    {
        if (split != source && split->isActivityPane() &&
            split->activityOwner() == source)
        {
            panes.push_back(split);
        }
    }
    return panes;
}

Split *findLinkedActivityPane(Split *source)
{
    const auto panes = findLinkedActivityPanes(source);
    return panes.empty() ? nullptr : panes.front();
}

Split *findActivityOwnerSplit(Split *activitySplit)
{
    return activitySplit ? activitySplit->activityOwner() : nullptr;
}

void refreshActivityIcons(SplitContainer *container)
{
    if (!container)
    {
        return;
    }

    for (auto *split : container->getSplits())
    {
        split->updateHeaderIcons();
    }
}

bool areEquivalentIndirectChannels(const IndirectChannel &lhs,
                                  const IndirectChannel &rhs)
{
    if (lhs.getType() != rhs.getType())
    {
        return false;
    }

    const auto left = lhs.get();
    const auto right = rhs.get();
    if (left == right)
    {
        return true;
    }

    if (!left || !right)
    {
        return left == right;
    }

    if (lhs.getType() == Channel::Type::Merged)
    {
        const auto leftMerged = std::dynamic_pointer_cast<MergedChannel>(left);
        const auto rightMerged = std::dynamic_pointer_cast<MergedChannel>(right);
        return leftMerged != nullptr && rightMerged != nullptr &&
               leftMerged->config() == rightMerged->config();
    }

    return left->getName() == right->getName();
}

void syncLinkedActivityPane(Split *ownerSplit, Split *activitySplit,
                            bool enabled)
{
    auto *container =
        dynamic_cast<SplitContainer *>(ownerSplit->parentWidget());
    if (!container)
    {
        return;
    }

    if (!enabled)
    {
        ownerSplit->setFilterActivity(false);
        auto panes = findLinkedActivityPanes(ownerSplit);
        if (activitySplit && activitySplit->parentWidget() == container &&
            std::find(panes.begin(), panes.end(), activitySplit) == panes.end())
        {
            panes.push_back(activitySplit);
        }
        for (auto *pane : panes)
            container->deleteSplit(pane);
        refreshActivityIcons(container);
        return;
    }

    if (activitySplit)
    {
        const auto activityPlatformIndicatorMode =
            activitySplit->platformIndicatorMode();
        const auto activityTimeDisplayMode =
            activitySplit->activityTimeDisplayMode();
        const auto activityMessageScale = activitySplit->activityMessageScale();
        const auto collapseGiftedSubscriptions =
            activitySplit->collapseGiftedSubscriptions();
        const auto showRaids = activitySplit->showRaids();
        const auto showWatchStreaks = activitySplit->showWatchStreaks();
        const auto tiktokActivityMinimumDiamonds =
            activitySplit->tiktokActivityMinimumDiamonds();
        const auto twitchActivityMinimumBits =
            activitySplit->twitchActivityMinimumBits();
        const auto kickActivityMinimumKicks =
            activitySplit->kickActivityMinimumKicks();

        activitySplit->setFilterActivity(false);
        activitySplit->setChannel(ownerSplit->getIndirectChannel());
        activitySplit->setFilters(ownerSplit->getFilters());
        activitySplit->setInputEnabled(false);
        activitySplit->setActivityMessageScale(activityMessageScale);
        activitySplit->setCollapseGiftedSubscriptions(
            collapseGiftedSubscriptions);
        activitySplit->setShowRaids(showRaids);
        activitySplit->setShowWatchStreaks(showWatchStreaks);
        activitySplit->setActivityTimeDisplayMode(activityTimeDisplayMode);
        activitySplit->setTwitchActivityMinimumBits(twitchActivityMinimumBits);
        activitySplit->setKickActivityMinimumKicks(kickActivityMinimumKicks);
        activitySplit->setTikTokActivityMinimumDiamonds(
            tiktokActivityMinimumDiamonds);
        activitySplit->setPlatformIndicatorMode(activityPlatformIndicatorMode);
        ownerSplit->setTwitchActivityMinimumBits(twitchActivityMinimumBits);
        ownerSplit->setKickActivityMinimumKicks(kickActivityMinimumKicks);
        ownerSplit->setTikTokActivityMinimumDiamonds(
            tiktokActivityMinimumDiamonds);
        ownerSplit->setCollapseGiftedSubscriptions(collapseGiftedSubscriptions);
        ownerSplit->setShowRaids(showRaids);
        ownerSplit->setShowWatchStreaks(showWatchStreaks);
        ownerSplit->setFilterActivity(true);
        refreshActivityIcons(container);
        return;
    }

    ownerSplit->openAlertsPane();
}

void showTutorialVideo(QWidget *parent, const QString &source,
                       const QString &title, const QString &description)
{
    auto *window = new BasePopup(
        {
            BaseWindow::EnableCustomFrame,
            BaseWindow::BoundsCheckOnShow,
        },
        parent);
    window->setWindowTitle("Mergerino - " + title);
    window->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(description));
    auto *label = new QLabel(window);
    layout->addWidget(label);
    auto *movie = new QMovie(label);
    movie->setFileName(source);
    label->setMovie(movie);
    movie->start();
    window->getLayoutContainer()->setLayout(layout);
    window->show();
}

void splitCtorTrace(const QString &message)
{
    QFile file(QStringLiteral("mergerino-layout-trace.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return;
    }

    QTextStream stream(&file);
    stream << message << '\n';
}

IndirectChannel createInitialSplitChannel()
{
    splitCtorTrace(QStringLiteral("Split init before Channel::getEmpty"));
    auto channel = Channel::getEmpty();
    splitCtorTrace(QStringLiteral("Split init after Channel::getEmpty"));
    return channel;
}

PlatformIndicatorMode createInitialPlatformIndicatorMode()
{
    splitCtorTrace(
        QStringLiteral("Split init before defaultPlatformIndicatorMode"));
    auto mode = defaultPlatformIndicatorMode(false);
    splitCtorTrace(
        QStringLiteral("Split init after defaultPlatformIndicatorMode"));
    return mode;
}

QVBoxLayout *createSplitVBoxLayout(Split *split)
{
    splitCtorTrace(QStringLiteral("Split init before QVBoxLayout"));
    auto *layout = new QVBoxLayout(split);
    splitCtorTrace(QStringLiteral("Split init after QVBoxLayout"));
    return layout;
}

SplitHeader *createSplitHeader(Split *split)
{
    splitCtorTrace(QStringLiteral("Split init before SplitHeader"));
    auto *header = new SplitHeader(split);
    splitCtorTrace(QStringLiteral("Split init after SplitHeader"));
    return header;
}

TwitchPollsAndPredictionsBar *createSplitPollsBar(Split *split)
{
    splitCtorTrace(
        QStringLiteral("Split init before TwitchPollsAndPredictionsBar"));
    auto *bar = new TwitchPollsAndPredictionsBar(split);
    splitCtorTrace(
        QStringLiteral("Split init after TwitchPollsAndPredictionsBar"));
    return bar;
}

ChannelView *createSplitChannelView(Split *split)
{
    splitCtorTrace(QStringLiteral("Split init before scrollbackSplitLimit"));
    auto scrollbackLimit = getSettings()->scrollbackSplitLimit;
    splitCtorTrace(QStringLiteral("Split init after scrollbackSplitLimit"));
    splitCtorTrace(QStringLiteral("Split init before ChannelView"));
    auto *view = new ChannelView(split, split, ChannelView::Context::None,
                                 scrollbackLimit);
    splitCtorTrace(QStringLiteral("Split init after ChannelView"));
    return view;
}

SplitInput *createSplitInput(Split *split)
{
    splitCtorTrace(QStringLiteral("Split init before SplitInput"));
    auto *input = new SplitInput(split);
    splitCtorTrace(QStringLiteral("Split init after SplitInput"));
    return input;
}

SplitOverlay *createSplitOverlay(Split *split)
{
    splitCtorTrace(QStringLiteral("Split init before SplitOverlay"));
    auto *overlay = new SplitOverlay(split);
    splitCtorTrace(QStringLiteral("Split init after SplitOverlay"));
    return overlay;
}
}  // namespace

pajlada::Signals::Signal<Qt::KeyboardModifiers> Split::modifierStatusChanged;
Qt::KeyboardModifiers Split::modifierStatus = Qt::NoModifier;

Split::Split(QWidget *parent)
    : BaseWidget(parent)
    , channel_(createInitialSplitChannel())
    , priorityChatId_(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , activityTimeDisplayMode_(ActivityTimeDisplayMode::Relative)
    , platformIndicatorMode_(createInitialPlatformIndicatorMode())
    , vbox_(createSplitVBoxLayout(this))
    , header_(createSplitHeader(this))
    , twitchPollsAndPredictionsBar_(createSplitPollsBar(this))
    , pinnedMessageBar_(new PinnedMessageBar(this))
    , view_(createSplitChannelView(this))
    , input_(createSplitInput(this))
    , overlay_(createSplitOverlay(this))
{
    splitCtorTrace(QStringLiteral("Split constructor body start"));
    this->modViewId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    this->setMouseTracking(true);
    this->view_->setPausable(true);
    this->view_->setFocusProxy(this->input_->ui_.textEdit);
    this->setFocusProxy(this->input_->ui_.textEdit);
    this->view_->installEventFilter(this);

    this->vbox_->setSpacing(0);
    this->vbox_->setContentsMargins(1, 1, 1, 1);

    auto *streamDatabaseBadgeBar = new StreamDatabaseBadgeBar(this);
    this->streamDatabaseBadgeBar_ = streamDatabaseBadgeBar;
    streamDatabaseBadgeBar->setBadgeFeedVisible(
        this->inputEnabled_ && this->streamDatabaseBadgeFeedVisible_);

    this->vbox_->addWidget(this->header_);
    this->chatRow_ = new QHBoxLayout;
    this->chatRow_->setSpacing(0);
    auto *chatColumn = new QVBoxLayout;
    chatColumn->setSpacing(0);
    chatColumn->addWidget(streamDatabaseBadgeBar);
    chatColumn->addWidget(this->twitchPollsAndPredictionsBar_);
    chatColumn->addWidget(this->pinnedMessageBar_);
    chatColumn->addWidget(this->view_, 1);
    chatColumn->addWidget(this->input_);
    this->chatRow_->addLayout(chatColumn, 1);
    this->vbox_->addLayout(this->chatRow_, 1);
    this->pinnedMessageBar_->setChannel(this->getChannel());
    this->pinnedMessageBar_->setDisplayEnabled(
        this->inputEnabled_ && !disablePinnedMessagesSetting().getValue());
    std::ignore = this->twitchPollsAndPredictionsBar_->pollClicked.connect(
        [this] {
            if (!this->isActivityPane())
            {
                this->input_->updatePollPredictionButtons();
                this->input_->openTwitchPollDialog();
            }
        });
    std::ignore =
        this->twitchPollsAndPredictionsBar_->kickPollClicked.connect([this] {
            if (!this->isActivityPane())
            {
                this->input_->updatePollPredictionButtons();
                this->input_->openKickPollDialog();
            }
        });
    std::ignore =
        this->twitchPollsAndPredictionsBar_->predictionClicked.connect([this] {
            if (!this->isActivityPane())
            {
                this->input_->updatePollPredictionButtons();
                this->input_->openTwitchPredictionDialog();
            }
        });
    std::ignore =
        this->twitchPollsAndPredictionsBar_->kickPredictionClicked.connect(
            [this] {
                if (!this->isActivityPane())
                {
                    this->input_->updatePollPredictionButtons();
                    this->input_->openKickPredictionDialog();
                }
            });
    std::ignore =
        this->twitchPollsAndPredictionsBar_->predictionStateChanged.connect(
            [this] {
                if (!this->isActivityPane())
                {
                    this->input_->updatePollPredictionButtons();
                }
            });

    this->input_->ui_.textEdit->installEventFilter(parent);

    // update placeholder text on Twitch account change and channel change
    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect(
            [this, streamDatabaseBadgeBar] {
                streamDatabaseBadgeBar->setChannel(this->getChannel());
                this->pinnedMessageBar_->setChannel(this->getChannel());
                this->input_->updatePlatformSelector();
                this->updateInputPlaceholder();
                this->refreshPriorityChatAccess();
            }));
    this->signalHolder_.managedConnect(this->channelChanged,
                                       [this, streamDatabaseBadgeBar] {
        streamDatabaseBadgeBar->setChannel(this->getChannel());
        this->pinnedMessageBar_->setChannel(this->getChannel());
        this->input_->updatePlatformSelector();
        this->updateInputPlaceholder();
        this->refreshYouTubeDiscoveryForeground();
        if (this->modView_)
        {
            this->modView_->setChannel(this->getChannel());
        }
        if (this->priorityChat_)
        {
            this->priorityChat_->setChannel(this->getChannel());
            if (this->isPriorityChatPane())
                this->priorityChat_->setMonitoring(this->canUsePriorityChat());
        }
    });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged, [this] {
            this->pinnedMessageBar_->setChannel(this->getChannel());
            this->input_->updatePlatformSelector();
            this->updateInputPlaceholder();
            this->refreshPriorityChatAccess();
        });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->youtube.currentUserChanged, [this] {
            this->input_->updatePlatformSelector();
            this->updateInputPlaceholder();
            this->refreshPriorityChatAccess();
        });
    // whether a click can leave anonymous mode depends on the saved accounts
    for (auto *userListUpdated :
         {&getApp()->getAccounts()->twitch.userListUpdated,
          &getApp()->getAccounts()->kick.userListUpdated,
          &getApp()->getAccounts()->youtube.userListUpdated})
    {
        this->signalHolder_.managedConnect(*userListUpdated, [this] {
            this->updateInputPlaceholder();
        });
    }
    this->signalHolder_.managedConnect(this->input_->sendPlatformChanged,
                                       [this] {
                                           this->updateInputPlaceholder();
                                           getApp()->getWindows()->queueSave();
                                       });
    disablePinnedMessagesSetting().connect(
        [this](const bool &) {
            this->pinnedMessageBar_->setDisplayEnabled(
                this->inputEnabled_ &&
                !disablePinnedMessagesSetting().getValue());
        },
        this->signalHolder_);
    this->updateInputPlaceholder();

    // clear SplitInput selection when selecting in ChannelView
    // this connection can be ignored since the ChannelView is owned by this Split
    std::ignore = this->view_->selectionChanged.connect([this]() {
        if (this->input_->hasSelection())
        {
            this->input_->clearSelection();
        }
    });

    // clear ChannelView selection when selecting in SplitInput
    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->selectionChanged.connect([this]() {
        if (this->view_->hasSelection())
        {
            this->view_->clearSelection();
        }
    });

    // this connection can be ignored since the ChannelView is owned by this Split
    std::ignore = this->view_->openChannelIn.connect(
        [this](QString twitchChannel, FromTwitchLinkOpenChannelIn openIn) {
            ChannelPtr channel =
                getApp()->getTwitch()->getOrAddChannel(twitchChannel);
            switch (openIn)
            {
                case FromTwitchLinkOpenChannelIn::Split:
                    this->openSplitRequested.invoke(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::Tab:
                    this->joinChannelInNewTab(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::BrowserPlayer:
                    this->openChannelInBrowserPlayer(channel);
                    break;
                case FromTwitchLinkOpenChannelIn::Streamlink:
                    this->openChannelInStreamlink(twitchChannel);
                    break;
                case FromTwitchLinkOpenChannelIn::CustomPlayer:
                    this->openChannelInCustomPlayer(twitchChannel);
                default:
                    qCWarning(chatterinoWidget)
                        << "Unhandled \"FromTwitchLinkOpenChannelIn\" enum "
                           "value: "
                        << static_cast<int>(openIn);
            }
        });

    this->header_->setSlowChatQueueIndicatorReady(true);
    QObject::connect(this->view_, &ChannelView::slowChatQueueCountChanged, this,
                     [this](int) {
                         this->header_->updateIcons();
                     });

    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->textChanged.connect(
        [this](const QString & /* newText */) {
            this->updateInputVisibility();
        });

    getSettings()->showEmptyInput.connect(
        [this](const bool & /* showEmptyInput */) {
            this->updateInputVisibility();
        },
        this->signalHolder_);

    this->updateInputVisibility();

    this->header_->updateIcons();
    this->overlay_->hide();

    this->setSizePolicy(QSizePolicy::MinimumExpanding,
                        QSizePolicy::MinimumExpanding);

    // update moderation button when items changed
    this->signalHolder_.managedConnect(
        getSettings()->moderationActions.delayedItemsChanged, [this] {
            this->refreshModerationMode();
        });

    this->signalHolder_.managedConnect(modifierStatusChanged, [this](
                                                                  Qt::KeyboardModifiers
                                                                      status) {
        if ((status ==
             SHOW_SPLIT_OVERLAY_MODIFIERS /*|| status == showAddSplitRegions*/) &&
            this->isMouseOver_)
        {
            this->overlay_->show();
        }
        else
        {
            this->overlay_->hide();
        }

        if (getSettings()->pauseChatModifier.getEnum() != Qt::NoModifier &&
            status == getSettings()->pauseChatModifier.getEnum())
        {
            this->view_->pause(PauseReason::KeyboardModifier);
        }
        else
        {
            this->view_->unpause(PauseReason::KeyboardModifier);
        }
    });

    this->signalHolder_.managedConnect(this->input_->ui_.textEdit->focused,
                                       [this] {
                                           // Forward textEdit's focused event
                                           this->focused.invoke();
                                           this->updateYouTubeDiscoveryForeground(
                                               true);
                                       });
    this->signalHolder_.managedConnect(this->input_->ui_.textEdit->focusLost,
                                       [this] {
                                           // Forward textEdit's focusLost event
                                           this->focusLost.invoke();
                                           this->updateYouTubeDiscoveryForeground(
                                               false);
                                       });

    // this connection can be ignored since the SplitInput is owned by this Split
    std::ignore = this->input_->ui_.textEdit->imagePasted.connect(
        [this](const QMimeData *original) {
            if (!getSettings()->imageUploaderEnabled)
            {
                return;
            }

            auto channel = this->getChannel();
            auto *imageUploader = getApp()->getImageUploader();

            auto [images, imageProcessError] =
                imageUploader->getImages(original);
            if (images.empty())
            {
                channel->addSystemMessage(
                    QString(
                        "An error occurred trying to process your image: %1")
                        .arg(imageProcessError));
                return;
            }

            if (getSettings()->askOnImageUpload.getValue())
            {
                QMessageBox msgBox(this->window());
                msgBox.setWindowTitle("Mergerino");
                msgBox.setText("Image upload");
                msgBox.setInformativeText(
                    "You are uploading an image to a 3rd party service not in "
                    "control of the Mergerino team. You may not be able to "
                    "remove the image from the site. Are you okay with this?");
                auto *cancel = msgBox.addButton(QMessageBox::Cancel);
                auto *yes = msgBox.addButton(QMessageBox::Yes);
                auto *yesDontAskAgain = msgBox.addButton("Yes, don't ask again",
                                                         QMessageBox::YesRole);

                msgBox.setDefaultButton(QMessageBox::Yes);

                msgBox.exec();

                auto *clickedButton = msgBox.clickedButton();
                if (clickedButton == yesDontAskAgain)
                {
                    getSettings()->askOnImageUpload.setValue(false);
                }
                else if (clickedButton == yes)
                {
                    // Continue with image upload
                }
                else if (clickedButton == cancel)
                {
                    // Not continuing with image upload
                    return;
                }
                else
                {
                    // An unknown "button" was pressed - handle it as if cancel was pressed
                    // cancel is already handled as the "escape" option, so this should never happen
                    qCWarning(chatterinoImageuploader)
                        << "Unhandled button pressed:" << clickedButton;
                    return;
                }
            }

            QPointer<ResizingTextEdit> edit = this->input_->ui_.textEdit;
            imageUploader->upload(std::move(images), channel, edit);
        });

    getSettings()->imageUploaderEnabled.connect(
        [this](const bool &val) {
            this->setAcceptDrops(val);
        },
        this->signalHolder_);
    this->addShortcuts();
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });
}

void Split::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"delete",
         [this](const std::vector<QString> &) -> QString {
             this->deleteFromContainer();
             return "";
         }},
        {"changeChannel",
         [this](const std::vector<QString> &) -> QString {
             this->changeChannel();
             return "";
         }},
        {"showSearch",
         [this](const std::vector<QString> &) -> QString {
             this->showSearch(true);
             return "";
         }},
        {"showGlobalSearch",
         [this](const std::vector<QString> &) -> QString {
             this->showSearch(false);
             return "";
         }},
        {"reconnect",
         [this](const std::vector<QString> &) -> QString {
             this->reconnect();
             return "";
         }},
        {"debug",
         [](const std::vector<QString> &) -> QString {
             auto *popup = new DebugPopup;
             popup->setAttribute(Qt::WA_DeleteOnClose);
             popup->setWindowTitle("Mergerino - Debug popup");
             popup->show();
             return "";
         }},
        {"focus",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 return "focus action requires only one argument: the "
                        "focus direction Use \"up\", \"above\", \"down\", "
                        "\"below\", \"left\" or \"right\".";
             }
             const auto &direction = arguments.at(0);
             if (direction == "up" || direction == "above")
             {
                 this->actionRequested.invoke(Action::SelectSplitAbove);
             }
             else if (direction == "down" || direction == "below")
             {
                 this->actionRequested.invoke(Action::SelectSplitBelow);
             }
             else if (direction == "left")
             {
                 this->actionRequested.invoke(Action::SelectSplitLeft);
             }
             else if (direction == "right")
             {
                 this->actionRequested.invoke(Action::SelectSplitRight);
             }
             else
             {
                 return "focus in unknown direction. Use \"up\", "
                        "\"above\", \"down\", \"below\", \"left\" or "
                        "\"right\".";
             }
             return "";
         }},
        {"scrollToBottom",
         [this](const std::vector<QString> &) -> QString {
             this->getChannelView().getScrollBar().scrollToBottom(
                 getSettings()->enableSmoothScrollingNewMessages.getValue());
             return "";
         }},
        {"scrollToTop",
         [this](const std::vector<QString> &) -> QString {
             this->getChannelView().getScrollBar().scrollToTop(
                 getSettings()->enableSmoothScrollingNewMessages.getValue());
             return "";
         }},
        {"scrollPage",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "scrollPage hotkey called without arguments!";
                 return "scrollPage hotkey called without arguments!";
             }
             const auto &direction = arguments.at(0);

             auto &scrollbar = this->getChannelView().getScrollBar();
             if (direction == "up")
             {
                 scrollbar.offset(-scrollbar.getPageSize());
             }
             else if (direction == "down")
             {
                 scrollbar.offset(scrollbar.getPageSize());
             }
             else
             {
                 qCWarning(chatterinoHotkeys) << "Unknown scroll direction";
             }
             return "";
         }},
        {"pickFilters",
         [this](const std::vector<QString> &) -> QString {
             this->setFiltersDialog();
             return "";
         }},
        {"openInBrowser",
         [this](const std::vector<QString> &) -> QString {
             if (this->getChannel()->getType() == Channel::Type::TwitchWhispers)
             {
                 this->openWhispersInBrowser();
             }
             else
             {
                 this->openInBrowser();
             }

             return "";
         }},
        {"openInStreamlink",
         [this](const std::vector<QString> &) -> QString {
             this->openInStreamlink();
             return "";
         }},
        {"openInCustomPlayer",
         [this](const std::vector<QString> &) -> QString {
             this->openWithCustomScheme();
             return "";
         }},
        {"openPlayerInBrowser",
         [this](const std::vector<QString> &) -> QString {
             this->openBrowserPlayer();
             return "";
         }},
        {"openModView",
         [this](const std::vector<QString> &) -> QString {
             this->openModView();
             return "";
         }},
        {"createClip",
         [this](const std::vector<QString> &) -> QString {
             // Alt+X: create clip LUL
             if (const auto type = this->getChannel()->getType();
                 type != Channel::Type::Twitch &&
                 type != Channel::Type::TwitchWatching)
             {
                 return "Cannot create clips in a non-Twitch channel.";
             }

             auto *twitchChannel =
                 dynamic_cast<TwitchChannel *>(this->getChannel().get());

             twitchChannel->createClip({}, {});
             return "";
         }},
        {"reloadEmotes",
         [this](const std::vector<QString> &arguments) -> QString {
             auto reloadChannel = true;
             auto reloadSubscriber = true;
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "channel")
                 {
                     reloadSubscriber = false;
                 }
                 else if (arg == "subscriber")
                 {
                     reloadChannel = false;
                 }
             }

             if (reloadChannel)
             {
                 this->header_->reloadChannelEmotes();
             }
             if (reloadSubscriber)
             {
                 this->header_->reloadSubscriberEmotes();
             }
             return "";
         }},
        {"setModerationMode",
         [this](const std::vector<QString> &arguments) -> QString {
             if (!this->getChannel()->isTwitchOrKickChannel() &&
                 !this->getChannel()->isMergedChannel())
             {
                 return "Cannot set moderation mode in this channel.";
             }
             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             switch (mode)
             {
                 case 0:
                     this->setModerationMode(false);
                     break;
                 case 1:
                     this->setModerationMode(true);
                     break;
                 default:
                     this->setModerationMode(!this->getModerationMode());
             }

             return "";
         }},
        {"openViewerList",
         [this](const std::vector<QString> &) -> QString {
             this->openChatterList();
             return "";
         }},
        {"clearMessages",
         [this](const std::vector<QString> &) -> QString {
             this->clear();
             return "";
         }},
        {"runCommand",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.empty())
             {
                 qCWarning(chatterinoHotkeys)
                     << "runCommand hotkey called without arguments!";
                 return "runCommand hotkey called without arguments!";
             }
             QString requestedText = QString(arguments[0]).replace('\n', ' ');

             QString inputText = this->getInput().getInputText();
             QString message = getApp()->getCommands()->execCustomCommand(
                 requestedText.split(' '), Command{"(hotkey)", requestedText},
                 true, this->getChannel(), nullptr,
                 {
                     {"input.text", inputText},
                 });

             message = getApp()->getCommands()->execCommand(
                 message, this->getChannel(), false);
             this->getChannel()->sendMessage(message);
             return "";
         }},
        {"setChannelNotification",
         [this](const std::vector<QString> &arguments) -> QString {
             if (!this->getChannel()->isTwitchChannel())
             {
                 return "Cannot set channel notifications for a non-Twitch "
                        "channel.";
             }
             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             auto *notifications = getApp()->getNotifications();
             const QString channelName = this->getChannel()->getName();
             switch (mode)
             {
                 case 0:
                     notifications->removeChannelNotification(channelName,
                                                              Platform::Twitch);
                     break;
                 case 1:
                     notifications->addChannelNotification(channelName,
                                                           Platform::Twitch);
                     break;
                 default:
                     notifications->updateChannelNotification(channelName,
                                                              Platform::Twitch);
             }
             return "";
         }},
        {"popupOverlay",
         [this](const auto &) -> QString {
             this->showOverlayWindow();
             return {};
         }},
        {"toggleOverlayInertia",
         [this](const auto &args) -> QString {
             if (args.empty())
             {
                 return "No arguments provided to toggleOverlayInertia "
                        "(expected one)";
             }
             const auto &arg = args.front();

             if (arg == "this")
             {
                 if (this->overlayWindow_)
                 {
                     this->overlayWindow_->toggleInertia();
                 }
                 return {};
             }
             if (arg == "thisOrAll")
             {
                 if (this->overlayWindow_)
                 {
                     this->overlayWindow_->toggleInertia();
                 }
                 else
                 {
                     getApp()->getWindows()->toggleAllOverlayInertia();
                 }
                 return {};
             }
             if (arg == "all")
             {
                 getApp()->getWindows()->toggleAllOverlayInertia();
                 return {};
             }
             return {};
         }},
        {"setHighlightSounds",
         [this](const std::vector<QString> &arguments) -> QString {
             if (!this->getChannel()->isTwitchChannel())
             {
                 return "Cannot set highlight sounds in a non-Twitch "
                        "channel.";
             }

             auto mode = 2;
             // 0 is off
             // 1 is on
             // 2 is toggle
             if (!arguments.empty())
             {
                 const auto &arg = arguments.at(0);
                 if (arg == "off")
                 {
                     mode = 0;
                 }
                 else if (arg == "on")
                 {
                     mode = 1;
                 }
             }

             const QString channel = this->getChannel()->getName();

             switch (mode)
             {
                 case 0:
                     getSettings()->mute(channel);
                     break;
                 case 1:
                     getSettings()->unmute(channel);
                     break;
                 default:
                     getSettings()->toggleMutedChannel(channel);
             }
             return "";
         }},
        {"openSubscriptionPage",
         [this](const auto &) -> QString {
             if (!this->getChannel()->isTwitchChannel())
             {
                 return "Cannot subscribe to a non-Twitch "
                        "channel.";
             }

             this->openSubPage();
             return "";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::Split, actions, this);
}

Split::~Split()
{
    if (this->modView_) this->modView_->setMonitoring(false);
    if (this->priorityChat_) this->priorityChat_->setMonitoring(false);
    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->channelIDChangedConnection_.disconnect();
    this->indirectChannelChangedConnection_.disconnect();
}

ChannelView &Split::getChannelView()
{
    return *this->view_;
}

SplitInput &Split::getInput()
{
    if (this->isPriorityChatPane())
        if (auto *owner = this->priorityChatOwner()) return owner->getInput();
    if (this->isModViewPane())
        if (auto *owner = this->modViewOwner())
            return owner->getInput();
    return *this->input_;
}

bool Split::inputEnabled() const
{
    return this->inputEnabled_;
}

bool Split::isActivityPane() const
{
    return !this->inputEnabled_ && !this->priorityChatPane_ &&
           !this->modViewPane_;
}

bool Split::hasLinkedActivityPane()
{
    return findLinkedActivityPane(this) != nullptr;
}

QJsonObject Split::activityChannelSignature(IndirectChannel channel)
{
    QJsonObject obj;
    WindowManager::encodeChannel(channel, obj);
    // Presentation preferences and asynchronously populated Kick IDs are not
    // chat identity. Including them strands otherwise identical saved panes.
    obj.remove("tiktokShowJoinMessages");
    if (auto *merged = dynamic_cast<MergedChannel *>(channel.get().get()))
    {
        const auto &config = merged->config();
        if (config.twitchEnabled || config.kickEnabled ||
            config.youtubeEnabled || config.tiktokEnabled)
            obj.remove("name");
        obj.insert(
            "twitchChannel",
            config.twitchEnabled
                ? config.effectiveTwitchChannelName().trimmed().toCaseFolded()
                : QString());
        obj.insert(
            "kickChannel",
            config.kickEnabled
                ? config.effectiveKickChannelName().trimmed().toCaseFolded()
                : QString());
        obj.insert("youtubeStreamUrl", config.youtubeEnabled
                                           ? config.youtubeStreamUrl.trimmed()
                                           : QString());
        auto tiktokSource =
            TikTokLiveChat::normalizeSource(config.tiktokSource);
        if (tiktokSource.isEmpty())
            tiktokSource = config.tiktokSource.trimmed().toCaseFolded();
        obj.insert("tiktokSource",
                   config.tiktokEnabled ? tiktokSource : QString());
    }
    else if (channel.getType() == Channel::Type::Kick ||
             channel.getType() == Channel::Type::Twitch)
    {
        obj.remove("roomID");
        obj.remove("userID");
        obj.remove("channelID");
        obj.insert("name", channel.get()->getName().toCaseFolded());
    }
    return obj;
}

Split *Split::activityOwner()
{
    if (!this->isActivityPane())
        return this;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
        return nullptr;
    if (this->activityOwner_ &&
        this->activityOwner_->parentWidget() == container &&
        !this->activityOwner_->isActivityPane() &&
        !this->activityOwner_->isPriorityChatPane())
        return this->activityOwner_;
    this->activityOwner_.clear();
    if (this->getChannel()->isEmpty())
        return nullptr;
    const auto alertFilterId = findAlertsFilterId();
    if (!alertFilterId)
        return nullptr;
    auto filters = this->getFilters();
    filters.removeAll(*alertFilterId);
    const auto signature = encodeChannelSignature(this->getIndirectChannel());
    Split *onlyChannelOwner = nullptr;
    int matchingChannels = 0;
    for (auto *candidate : container->getSplits())
    {
        if (candidate == this || candidate->isUtilityPane() ||
            encodeChannelSignature(candidate->getIndirectChannel()) !=
                signature)
            continue;
        ++matchingChannels;
        onlyChannelOwner = candidate;
        if (splitMatchesChannelAndFilters(candidate, signature, filters))
        {
            this->activityOwner_ = candidate;
            return candidate;
        }
    }
    // A pane may have additional filters of its own. Restore its unique chat
    // owner without confusing it with another filtered view of that chat.
    if (matchingChannels == 1)
    {
        this->activityOwner_ = onlyChannelOwner;
        return onlyChannelOwner;
    }
    return nullptr;
}

void Split::normalizeLinkedActivityPanes()
{
    if (this->isUtilityPane())
        return;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
        return;
    const auto panes = findLinkedActivityPanes(this);
    // Old toggle failures could save several copies. Keep the first pane and
    // its preferences, then use that same live relationship for icon state.
    for (size_t i = 1; i < panes.size(); ++i)
        container->deleteSplit(panes[i]);
    if (!panes.empty())
    {
        // Route the main chat using the visible pane's saved event choices.
        this->setShowRaids(panes.front()->showRaids());
        this->setShowWatchStreaks(panes.front()->showWatchStreaks());
    }
    refreshActivityIcons(container);
}

bool Split::filterActivity() const
{
    return this->filterActivity_;
}

bool Split::filterActivityExplicit() const
{
    return this->filterActivityExplicit_;
}

bool Split::viewerCountEnabled() const
{
    return this->viewerCountEnabledOverride_.value_or(
        getSettings()->headerViewerCount.getValue());
}

std::optional<bool> Split::viewerCountEnabledOverride() const
{
    return this->viewerCountEnabledOverride_;
}

QString Split::activityPaneTitle() const
{
    return QStringLiteral("Activity");
}

qreal Split::activityMessageScale() const
{
    return this->activityMessageScale_;
}

bool Split::collapseGiftedSubscriptions() const
{
    return this->collapseGiftedSubscriptions_;
}

bool Split::showRaids() const
{
    return this->showRaids_;
}

bool Split::showWatchStreaks() const
{
    return this->showWatchStreaks_;
}

ActivityTimeDisplayMode Split::activityTimeDisplayMode() const
{
    return this->activityTimeDisplayMode_;
}

bool Split::slowerChatEnabled() const
{
    return this->slowerChatEnabled_;
}

qreal Split::slowerChatMessagesPerSecond() const
{
    return this->slowerChatMessagesPerSecond_;
}

bool Split::slowerChatMessageAnimations() const
{
    return this->slowerChatMessageAnimations_;
}

bool Split::streamDatabaseBadgeFeedVisible() const
{
    return this->streamDatabaseBadgeFeedVisible_;
}

bool Split::titleSettingsButtonVisible() const
{
    return this->titleSettingsButtonVisible_;
}

bool Split::chatModeIndicatorVisible() const
{
    return this->chatModeIndicatorVisible_;
}

PlatformIndicatorMode Split::platformIndicatorMode() const
{
    return normalizedPlatformIndicatorMode(this->platformIndicatorMode_,
                                           this->isActivityPane());
}

uint32_t Split::twitchActivityMinimumBits() const
{
    return this->twitchActivityMinimumBits_;
}

uint32_t Split::kickActivityMinimumKicks() const
{
    return this->kickActivityMinimumKicks_;
}

uint32_t Split::tiktokActivityMinimumDiamonds() const
{
    return this->tiktokActivityMinimumDiamonds_;
}

void Split::updateYouTubeDiscoveryForeground(bool foreground)
{
    if (auto *merged = dynamic_cast<MergedChannel *>(this->getChannel().get()))
    {
        merged->setYouTubeDiscoveryForeground(foreground);
    }
}

void Split::refreshYouTubeDiscoveryForeground()
{
    auto *focusWidget = QApplication::focusWidget();
    this->updateYouTubeDiscoveryForeground(
        focusWidget != nullptr &&
        (focusWidget == this || this->isAncestorOf(focusWidget)));
}

bool Split::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->view_)
    {
        if (event->type() == QEvent::FocusIn)
        {
            this->focused.invoke();
            this->updateYouTubeDiscoveryForeground(true);
        }
        else if (event->type() == QEvent::FocusOut)
        {
            this->focusLost.invoke();
            this->updateYouTubeDiscoveryForeground(false);
        }
    }

    return BaseWidget::eventFilter(watched, event);
}

void Split::setInputEnabled(bool enabled)
{
    enabled = enabled && !this->priorityChatPane_ && !this->modViewPane_;
    auto updateStreamDatabaseBadgeFeedVisibility = [this, enabled] {
        if (this->streamDatabaseBadgeBar_ != nullptr)
        {
            this->streamDatabaseBadgeBar_->setBadgeFeedVisible(
                enabled && this->streamDatabaseBadgeFeedVisible_);
        }
    };

    this->pinnedMessageBar_->setDisplayEnabled(
        enabled && !disablePinnedMessagesSetting().getValue());
    this->twitchPollsAndPredictionsBar_->setChannel(
        enabled ? this->getChannel() : Channel::getEmpty());

    if (this->inputEnabled_ == enabled)
    {
        updateStreamDatabaseBadgeFeedVisibility();
        this->header_->updateRoomModes();
        return;
    }

    // Remove linked panes while this is still a chat owner.
    if (!enabled && !this->priorityChatPane_ && !this->modViewPane_)
    {
        this->setModerationMode(false);
        this->setPriorityChatEnabled(false);
    }
    this->inputEnabled_ = enabled;
    if (enabled) this->activityOwner_.clear();
    if (!enabled && !this->isModViewPane())
        this->setModerationMode(false);
    updateStreamDatabaseBadgeFeedVisibility();

    if (!enabled && !this->priorityChatPane_ && !this->modViewPane_)
    {
        auto filters = this->getFilters();
        if (const auto alertFilterId = ensureAlertsFilter())
        {
            if (!filters.contains(*alertFilterId))
            {
                filters.append(*alertFilterId);
                this->view_->setFilters(filters);
            }
        }
    }

    getApp()->getWindows()->queueSave();
    this->updateInputVisibility();
    this->header_->updateChannelText();
    this->header_->updateRoomModes();
    this->view_->refreshMessages();
    this->view_->refreshSlowerChatSettings();
    this->actionRequested.invoke(Action::RefreshTab);
}

void Split::setActivityMessageScale(qreal value)
{
    const auto clamped =
        std::clamp(value, MIN_ACTIVITY_MESSAGE_SCALE, MAX_ACTIVITY_MESSAGE_SCALE);
    if (qFuzzyCompare(this->activityMessageScale_, clamped))
    {
        return;
    }

    this->activityMessageScale_ = clamped;
    this->view_->invalidateBuffers();
    getApp()->getWindows()->queueSave();
}

void Split::setCollapseGiftedSubscriptions(bool value)
{
    if (this->collapseGiftedSubscriptions_ == value)
    {
        return;
    }

    this->collapseGiftedSubscriptions_ = value;
    this->view_->refreshMessages();
    getApp()->getWindows()->queueSave();
}

void Split::setShowRaids(bool value)
{
    if (this->showRaids_ == value)
    {
        return;
    }

    this->showRaids_ = value;
    this->view_->refreshMessages();
    getApp()->getWindows()->queueSave();
}

void Split::setShowWatchStreaks(bool value)
{
    if (this->showWatchStreaks_ == value)
    {
        return;
    }

    this->showWatchStreaks_ = value;
    this->view_->refreshMessages();
    getApp()->getWindows()->queueSave();
}

bool Split::hasHiddenPinnedMessage() const
{
    return this->pinnedMessageBar_->hasHiddenPinnedMessage();
}

void Split::unhidePinnedMessages()
{
    this->pinnedMessageBar_->unhidePinnedMessages();
}

void Split::setActivityTimeDisplayMode(ActivityTimeDisplayMode value)
{
    if (this->activityTimeDisplayMode_ == value)
    {
        return;
    }

    this->activityTimeDisplayMode_ = value;
    this->view_->refreshActivityTimeDisplayMode();
    getApp()->getWindows()->queueSave();
}

void Split::setSlowerChatEnabled(bool value)
{
    if (this->slowerChatEnabled_ == value)
    {
        return;
    }

    this->slowerChatEnabled_ = value;
    this->view_->refreshSlowerChatSettings();
    getApp()->getWindows()->queueSave();
}

void Split::setSlowerChatMessagesPerSecond(qreal value)
{
    const auto clamped = std::clamp(value, MIN_SLOWER_CHAT_MESSAGES_PER_SECOND,
                                    MAX_SLOWER_CHAT_MESSAGES_PER_SECOND);
    if (qFuzzyCompare(this->slowerChatMessagesPerSecond_, clamped))
    {
        return;
    }

    this->slowerChatMessagesPerSecond_ = clamped;
    this->view_->refreshSlowerChatSettings();
    getApp()->getWindows()->queueSave();
}

void Split::setSlowerChatMessageAnimations(bool value)
{
    if (this->slowerChatMessageAnimations_ == value)
    {
        return;
    }

    this->slowerChatMessageAnimations_ = value;
    this->view_->refreshSlowerChatSettings();
    getApp()->getWindows()->queueSave();
}

void Split::setStreamDatabaseBadgeFeedVisible(bool value)
{
    const bool effectiveValue = this->inputEnabled_ && value;
    if (this->streamDatabaseBadgeFeedVisible_ == value)
    {
        if (this->streamDatabaseBadgeBar_ != nullptr)
        {
            this->streamDatabaseBadgeBar_->setBadgeFeedVisible(effectiveValue);
        }
        return;
    }

    this->streamDatabaseBadgeFeedVisible_ = value;
    if (this->streamDatabaseBadgeBar_ != nullptr)
    {
        this->streamDatabaseBadgeBar_->setBadgeFeedVisible(effectiveValue);
    }
    getApp()->getWindows()->queueSave();
}

void Split::setTitleSettingsButtonVisible(bool value)
{
    if (this->titleSettingsButtonVisible_ == value)
    {
        return;
    }

    this->titleSettingsButtonVisible_ = value;
    getApp()->getWindows()->queueSave();
}

void Split::setChatModeIndicatorVisible(bool value)
{
    if (this->chatModeIndicatorVisible_ == value)
    {
        return;
    }

    this->chatModeIndicatorVisible_ = value;
    this->header_->updateRoomModes();
    getApp()->getWindows()->queueSave();
}

void Split::setPlatformIndicatorMode(PlatformIndicatorMode value)
{
    value = normalizedPlatformIndicatorMode(value, this->isActivityPane());
    if (this->platformIndicatorMode_ == value)
    {
        return;
    }

    this->platformIndicatorMode_ = value;
    this->view_->refreshPlatformIndicatorMode();
    getApp()->getWindows()->queueSave();
}

void Split::setTwitchActivityMinimumBits(uint32_t value)
{
    if (this->twitchActivityMinimumBits_ == value)
    {
        return;
    }

    this->twitchActivityMinimumBits_ = value;
    this->view_->refreshMessages();
    getApp()->getWindows()->queueSave();
}

void Split::setKickActivityMinimumKicks(uint32_t value)
{
    if (this->kickActivityMinimumKicks_ == value)
    {
        return;
    }

    this->kickActivityMinimumKicks_ = value;
    this->view_->refreshMessages();
    getApp()->getWindows()->queueSave();
}

void Split::setTikTokActivityMinimumDiamonds(uint32_t value)
{
    if (this->tiktokActivityMinimumDiamonds_ == value)
    {
        return;
    }

    this->tiktokActivityMinimumDiamonds_ = value;
    this->view_->refreshMessages();
    getApp()->getWindows()->queueSave();
}

void Split::setViewerCountEnabled(bool value)
{
    this->setViewerCountEnabledOverride(value);
}

void Split::setViewerCountEnabledOverride(std::optional<bool> value)
{
    if (this->viewerCountEnabledOverride_ == value)
    {
        return;
    }

    this->viewerCountEnabledOverride_ = value;
    this->header_->updateChannelText();
    getApp()->getWindows()->queueSave();
}

void Split::setFilterActivity(bool value, bool explicitPreference)
{
    const bool valueChanged = this->filterActivity_ != value;
    const bool explicitChanged =
        this->filterActivityExplicit_ != explicitPreference;
    if (!valueChanged && !explicitChanged)
    {
        return;
    }

    this->filterActivity_ = value;
    this->filterActivityExplicit_ = explicitPreference;
    if (valueChanged)
    {
        this->view_->refreshMessages();
    }
    getApp()->getWindows()->queueSave();
}

void Split::updateInputVisibility()
{
    if (this->isModViewPane())
    {
        this->input_->setEnabled(false);
        this->input_->hide();
        this->setFocusProxy(this->modView_.data());
        return;
    }
    const auto channel = this->getChannel();
    const bool suppressInputForChannel =
        channel != nullptr &&
        channel->getType() == Channel::Type::TwitchMentions;

    if (!this->inputEnabled_ || suppressInputForChannel)
    {
        this->input_->setEnabled(false);
        this->input_->hide();
        this->view_->setFocusProxy(nullptr);
        this->setFocusProxy(this->view_);
        if (this->input_->ui_.textEdit->hasFocus())
        {
            this->view_->setFocus(Qt::OtherFocusReason);
        }
        return;
    }

    this->input_->setEnabled(true);
    this->view_->setFocusProxy(this->input_->ui_.textEdit);
    this->setFocusProxy(this->input_->ui_.textEdit);

    if (getSettings()->showEmptyInput || !this->input_->getInputText().isEmpty())
    {
        this->input_->show();
    }
    else
    {
        this->input_->hide();
    }
}

void Split::updateInputPlaceholder()
{
    this->input_->updatePlatformSelector();
    this->pinnedMessageBar_->setMultiplePlatformsSelected(
        this->input_->selectedSendPlatforms().size() > 1);

    const auto messagePlaceholder = [this] {
        const auto channel = this->getChannel();
        if (channel == nullptr)
        {
            return QStringLiteral("Send a message");
        }
        const auto name = channel->getDisplayName().trimmed();
        return name.isEmpty()
                   ? QStringLiteral("Send a message")
                   : QStringLiteral("Send a message in %1").arg(name);
    };

    // "Twitch", "Twitch and Kick", "Twitch, Kick and YouTube"
    const auto listPlatforms = [](const QStringList &names) {
        if (names.size() < 2)
        {
            return names.value(0);
        }
        return names.mid(0, names.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") +
               names.last();
    };
    // Nothing can be sent: the box becomes the anonymous notice. Clicking it
    // switches to the saved account where there's only one to pick.
    const auto showAnonymous = [this, &listPlatforms](
                                   const QStringList &anonymousOn) {
        this->input_->setAnonymousState(anonymousOn);
        const auto &switchable = this->input_->switchableAnonymousPlatforms();
        auto text = QStringLiteral("Anonymous mode on %1")
                        .arg(listPlatforms(anonymousOn));
        if (switchable == anonymousOn)
        {
            text += QStringLiteral(" · click to disable");
        }
        else if (!switchable.isEmpty())
        {
            text += QStringLiteral(" · click to disable on %1")
                        .arg(listPlatforms(switchable));
        }
        this->input_->setPlaceholderText(text);
    };

    if (auto *merged = dynamic_cast<MergedChannel *>(this->getChannel().get()))
    {
        auto *accounts = getApp()->getAccounts();
        const auto &config = merged->config();
        // Platforms in this chat that take messages but aren't logged in
        QStringList anonymousOn;
        if (config.twitchEnabled && !accounts->twitch.isLoggedIn())
        {
            anonymousOn.append(QStringLiteral("Twitch"));
        }
        if (config.kickEnabled && !accounts->kick.isLoggedIn())
        {
            anonymousOn.append(QStringLiteral("Kick"));
        }
        if (config.youtubeEnabled && merged->youtubeLiveChat() &&
            merged->youtubeLiveChat()->isLive() &&
            !accounts->youtube.isLoggedIn())
        {
            anonymousOn.append(QStringLiteral("YouTube"));
        }

        if (!this->input_->selectedSendPlatforms().empty())
        {
            // Logged in wherever this message goes, so the box is as usual
            this->input_->setAnonymousState({});
            this->input_->setPlaceholderText(messagePlaceholder());
            return;
        }

        // Replies only go to the replied message's platform
        if (const auto reply = this->input_->replySendPlatform())
        {
            const auto replyOn = *reply == MessagePlatform::Kick
                                     ? QStringLiteral("Kick")
                                 : *reply == MessagePlatform::YouTube
                                     ? QStringLiteral("YouTube")
                                     : QStringLiteral("Twitch");
            if (anonymousOn.contains(replyOn))
            {
                showAnonymous({replyOn});
                return;
            }
        }

        if (anonymousOn.isEmpty())
        {
            this->input_->setAnonymousState({});
            this->input_->setPlaceholderText(QString{});
            return;
        }
        showAnonymous(anonymousOn);
        return;
    }

    if (this->getChannel()->isKickChannel())
    {
        if (getApp()->getAccounts()->kick.current()->isAnonymous())
        {
            showAnonymous({QStringLiteral("Kick")});
            return;
        }
        this->input_->setAnonymousState({});
        this->input_->setPlaceholderText(messagePlaceholder());
        return;
    }

    if (!this->getChannel()->isTwitchChannel())
    {
        this->input_->setAnonymousState({});
        return;
    }

    if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
    {
        showAnonymous({QStringLiteral("Twitch")});
        return;
    }
    this->input_->setAnonymousState({});
    this->input_->setPlaceholderText(messagePlaceholder());
}

void Split::joinChannelInNewTab(const ChannelPtr &channel)
{
    auto &nb = getApp()->getWindows()->getMainWindow().getNotebook();
    SplitContainer *container = nb.addPage(true);

    auto *split = new Split(container);
    split->setChannel(channel);
    container->insertSplit(split);
}

void Split::refreshModerationMode()
{
    this->header_->updateIcons();
    this->view_->queueLayout();
}

void Split::openChannelInBrowserPlayer(ChannelPtr channel)
{
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(
            QUrl(TWITCH_PLAYER_URL.arg(twitchChannel->getName())));
    }
}

void Split::openChannelInStreamlink(const QString channelName)
{
    try
    {
        openStreamlinkForChannel(channelName);
    }
    catch (const Exception &ex)
    {
        qCWarning(chatterinoWidget)
            << "Error in doOpenStreamlink:" << ex.what();
    }
}

void Split::openChannelInCustomPlayer(const QString channelName)
{
    openInCustomPlayer(channelName);
}

IndirectChannel Split::getIndirectChannel()
{
    return this->channel_;
}

ChannelPtr Split::getChannel() const
{
    return this->channel_.get();
}

void Split::setChannel(IndirectChannel newChannel)
{
    const auto activityPanes = findLinkedActivityPanes(this);
    const auto priorityPanes = this->linkedPriorityChatPanes();
    const auto modPanes = this->linkedModViewPanes();
    this->channel_ = newChannel;

    this->view_->setChannel(this->isModViewPane() ? Channel::getEmpty()
                                                  : newChannel.get());
    this->twitchPollsAndPredictionsBar_->setChannel(
        this->inputEnabled_ ? newChannel.get() : Channel::getEmpty());

    this->usermodeChangedConnection_.disconnect();
    this->roomModeChangedConnection_.disconnect();
    this->channelIDChangedConnection_.disconnect();
    this->indirectChannelChangedConnection_.disconnect();
    this->channelSignalHolder_.clear();
    this->input_->clearSendWaitStatuses();

    auto *tc = roomModeTwitchChannel(newChannel.get().get());
    auto *kc = roomModeKickChannel(newChannel.get().get());
    auto *mergedChannel =
        dynamic_cast<MergedChannel *>(newChannel.get().get());
    auto *yc = mergedChannel ? mergedChannel->youtubeLiveChat() : nullptr;

    if (tc != nullptr)
    {
        this->channelSignalHolder_.managedConnect(tc->userStateChanged, [this] {
            this->refreshPriorityChatAccess();
            this->header_->updateRoomModes();
            this->input_->updatePollPredictionButtons();
        });

        this->channelSignalHolder_.managedConnect(tc->roomModesChanged, [this] {
            this->header_->updateRoomModes();
        });

        this->channelSignalHolder_.managedConnect(
            tc->sendWaitUpdate, [this](int secondsRemaining) {
                this->getInput().setSendWaitStatus(
                    MessagePlatform::AnyOrTwitch, secondsRemaining);
            });
    }
    if (kc != nullptr)
    {
        this->channelSignalHolder_.managedConnect(kc->userStateChanged, [this] {
            this->refreshPriorityChatAccess();
            this->header_->updateRoomModes();
            this->input_->updatePollPredictionButtons();
        });

        this->channelSignalHolder_.managedConnect(kc->roomModesChanged, [this] {
            this->header_->updateRoomModes();
        });

        this->channelSignalHolder_.managedConnect(
            kc->sendWaitUpdate, [this](int secondsRemaining) {
                this->getInput().setSendWaitStatus(MessagePlatform::Kick,
                                                   secondsRemaining);
            });
    }
    if (yc != nullptr)
    {
        this->channelSignalHolder_.managedConnect(
            yc->moderationStatusChanged, [this] {
                this->refreshPriorityChatAccess();
            });
        this->channelSignalHolder_.managedConnect(
            yc->sendWaitUpdate, [this](int secondsRemaining) {
                this->getInput().setSendWaitStatus(MessagePlatform::YouTube,
                                                   secondsRemaining);
            });
    }

    this->indirectChannelChangedConnection_ =
        newChannel.getChannelChanged().connect([this] {
            QTimer::singleShot(0, this, [this] {
                this->setChannel(this->channel_);
            });
        });

    this->header_->updateIcons();
    this->header_->updateChannelText();
    this->header_->updateRoomModes();

    this->channelSignalHolder_.managedConnect(
        this->channel_.get()->displayNameChanged, [this] {
            this->actionRequested.invoke(Action::RefreshTab);
        });
    if (auto *merged = dynamic_cast<MergedChannel *>(this->channel_.get().get()))
    {
        this->channelIDChangedConnection_ =
            merged->streamStatusChanged.connect([this] {
                this->input_->updatePlatformSelector(true);
                this->updateInputPlaceholder();
            });
    }
    this->input_->applyActiveAccountProviderDefault();
    this->updateInputVisibility();

    QObject::connect(
        this->view_, &ChannelView::messageAddedToChannel, this,
        [this](MessagePtr &message) {
            if (!getSettings()->pulseTextInputOnSelfMessage)
            {
                return;
            }
            auto user = getApp()->getAccounts()->twitch.getCurrent();
            if (!user->isAnon() && message->userID == user->getUserId())
            {
                // A message from yourself was just received in this split
                this->input_->triggerSelfMessageReceived();
            }
        });

    this->channelChanged.invoke();
    for (auto *pane : activityPanes) pane->setChannel(newChannel);
    for (auto *pane : priorityPanes) pane->setChannel(newChannel);
    for (auto *pane : modPanes)
        pane->setChannel(newChannel);
    if (this->isModViewPane())
        QTimer::singleShot(0, this, [this] {
            this->normalizeModViewPanes();
        });
    this->refreshPriorityChatAccess();
    refreshActivityIcons(dynamic_cast<SplitContainer *>(this->parentWidget()));
    this->actionRequested.invoke(Action::RefreshTab);

    // Queue up save because: Split channel changed
    getApp()->getWindows()->queueSave();
}

bool Split::isModViewPane() const
{
    return this->modViewPane_;
}
bool Split::isUtilityPane() const
{
    return this->modViewPane_ || this->priorityChatPane_ ||
           this->isActivityPane();
}
bool Split::modViewStandalone() const
{
    return this->modViewStandalone_;
}
QString Split::modViewId() const
{
    return this->modViewId_;
}
QString Split::modViewOwnerId() const
{
    return this->modViewOwnerId_;
}
bool Split::moderationModeRequested() const
{
    return !this->isUtilityPane() && this->moderationMode_;
}
Split *Split::modViewOwner()
{
    if (!this->isModViewPane() || this->modViewStandalone_)
        return nullptr;
    if (this->modViewOwner_)
        return this->modViewOwner_;
    if (auto *container = dynamic_cast<SplitContainer *>(this->parentWidget()))
        for (auto *split : container->getSplits())
            if (!split->isUtilityPane() &&
                split->modViewId_ == this->modViewOwnerId_)
            {
                this->modViewOwner_ = split;
                return split;
            }
    return nullptr;
}
std::vector<Split *> Split::linkedModViewPanes() const
{
    std::vector<Split *> panes;
    if (this->isUtilityPane())
        return panes;
    if (auto *container = dynamic_cast<SplitContainer *>(this->parentWidget()))
        for (auto *split : container->getSplits())
            if (split != this && split->isModViewPane() &&
                !split->modViewStandalone_ &&
                split->modViewOwnerId_ == this->modViewId_)
                panes.push_back(split);
    return panes;
}
void Split::restoreModViewState(bool pane, const QString &id,
                                const QString &ownerId, bool standalone)
{
    if (!id.isEmpty())
        this->modViewId_ = id;
    this->modViewPane_ = pane;
    this->modViewOwnerId_ = ownerId;
    this->modViewStandalone_ = standalone;
    if (!pane)
        return;
    this->priorityChatPane_ = false;
    this->moderationMode_ = false;
    this->priorityChatEnabled_ = false;
    this->setInputEnabled(false);
    this->view_->setChannel(Channel::getEmpty());
    this->view_->hide();
    this->setTitleSettingsButtonVisible(true);
}
void Split::attachModView(ModView *view)
{
    if (!this->isModViewPane() || !view)
        return;
    if (this->modView_ && this->modView_ != view &&
        this->modView_->parentWidget() == this)
    {
        this->modView_->setMonitoring(false);
        this->modView_->deleteLater();
    }
    this->modView_ = view;
    view->split_ = this;
    view->setParent(this);
    view->setMinimumWidth(0);
    view->setMaximumWidth(QWIDGETSIZE_MAX);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (this->chatRow_->indexOf(view) < 0)
        this->chatRow_->addWidget(view, 1);
    // A Mod View split contains only its panel beneath the native header.
    if (this->chatRow_->count() > 0 && this->chatRow_->itemAt(0)->layout())
        delete this->chatRow_->takeAt(0);
    this->streamDatabaseBadgeBar_->hide();
    this->twitchPollsAndPredictionsBar_->hide();
    this->pinnedMessageBar_->hide();
    this->input_->hide();
    this->view_->hide();
    this->setFocusProxy(view);
    view->setChannel(this->getChannel());
    view->setMonitoring(true);
    view->show();
    this->header_->updateChannelText();
    this->header_->updateIcons();
}
void Split::ensureModView()
{
    if (this->modView_)
        return;
    auto *view = new ModView(this, this);
    this->modView_ = view;
    view->setChannel(this->getChannel());
    view->hide();
    std::ignore = view->closeRequested.connect([view] {
        if (!view->split_)
            return;
        if (view->split_->isModViewPane())
            view->split_->closeModViewPane();
        else
            view->split_->setModerationMode(false);
    });
    std::ignore = view->popOutRequested.connect([view] {
        if (view->split_)
            view->split_->toggleModViewPopOut();
    });
    std::ignore = view->inlineActionsChanged.connect([view] {
        if (!view->split_)
            return;
        auto *split = view->split_.data();
        if (auto *owner = split->modViewOwner())
            owner->refreshModerationMode();
        else
            split->refreshModerationMode();
    });
}
void Split::setModerationMode(bool value)
{
    if (this->isModViewPane())
        return;
    value = value && !this->isUtilityPane();
    this->moderationMode_ = value;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    const auto splits =
        container ? container->getSplits() : std::vector<Split *>{};
    if (!container ||
        std::find(splits.begin(), splits.end(), this) == splits.end())
    {
        if (value)
            QTimer::singleShot(0, this, [this] {
                this->normalizeModViewPanes();
            });
        this->refreshModerationMode();
        return;
    }
    if (value)
    {
        this->ensureModView();
        auto panes = this->linkedModViewPanes();
        if (panes.empty())
        {
            auto *pane = container->cloneSplit(
                this, this->getFilters(), SplitDirection::Right,
                ALERTS_PRIMARY_RATIO, ALERTS_SECONDARY_RATIO, false);
            if (pane)
            {
                pane->restoreModViewState(true, {}, this->modViewId_, false);
                pane->modViewOwner_ = this;
                panes.push_back(pane);
            }
        }
        if (!panes.empty())
            panes.front()->attachModView(this->modView_);
    }
    else
    {
        for (;;)
        {
            const auto panes = this->linkedModViewPanes();
            if (panes.empty())
                break;
            container->deleteSplit(panes.front());
        }
        if (this->modView_)
        {
            this->modView_->setMonitoring(false);
            this->modView_->hide();
        }
    }
    this->refreshModerationMode();
    getApp()->getWindows()->queueSave();
}
bool Split::getModerationMode() const
{
    if (this->isUtilityPane())
        return false;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    const auto splits =
        container ? container->getSplits() : std::vector<Split *>{};
    return !container ||
                   std::find(splits.begin(), splits.end(), this) == splits.end()
               ? this->moderationMode_
               : !this->linkedModViewPanes().empty();
}
bool Split::showInlineModerationActions() const
{
    return this->getModerationMode() && this->modView_ &&
           this->modView_->inlineActionsEnabled();
}
void Split::normalizeModViewPanes()
{
    if (this->isModViewPane())
    {
        if (auto *owner = this->modViewOwner())
            owner->normalizeModViewPanes();
        else
        {
            this->modViewStandalone_ = true;
            this->ensureModView();
            this->attachModView(this->modView_);
        }
        return;
    }
    if (this->isUtilityPane())
        return;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
        return;
    const auto splits = container->getSplits();
    if (std::find(splits.begin(), splits.end(), this) == splits.end())
        return;
    const auto panes = this->linkedModViewPanes();
    for (size_t i = 1; i < panes.size(); ++i)
        container->deleteSplit(panes[i]);
    if (!panes.empty() || this->moderationMode_)
        this->setModerationMode(true);
}
void Split::returnModViewToOwner()
{
    auto *owner = this->modViewOwner();
    if (!owner)
        return;
    if (this->modView_ && owner->modView_ == this->modView_)
    {
        auto *view = this->modView_.data();
        view->setMonitoring(false);
        view->hide();
        this->chatRow_->removeWidget(view);
        view->setParent(owner);
        view->split_ = owner;
        this->modView_.clear();
    }
    owner->moderationMode_ = false;
    owner->refreshModerationMode();
}
void Split::unlinkModViewPane()
{
    if (!this->isModViewPane() || this->modViewStandalone_)
        return;
    if (auto *owner = this->modViewOwner())
    {
        if (owner->modView_ == this->modView_)
            owner->modView_.clear();
        owner->moderationMode_ = false;
        owner->refreshModerationMode();
    }
    this->modViewStandalone_ = true;
    this->modViewOwner_.clear();
    this->modViewOwnerId_.clear();
}
void Split::closeModViewPane()
{
    if (auto *container = dynamic_cast<SplitContainer *>(this->parentWidget()))
        container->deleteSplit(this);
}
void Split::openModView()
{
    if (!this->isUtilityPane())
        this->setModerationMode(true);
}
void Split::inspectInModView(const MessagePtr &message)
{
    if (!message || this->isUtilityPane())
        return;
    this->openModView();
    if (this->modView_)
        this->modView_->inspectMessage(message);
}
void Split::toggleModViewPopOut()
{
    if (!this->isModViewPane())
    {
        const auto panes = this->linkedModViewPanes();
        if (!panes.empty())
            panes.front()->toggleModViewPopOut();
        return;
    }
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
        return;
    Window &window = getApp()->getWindows()->createWindow(WindowType::Popup);
    auto *target = window.getNotebook().getOrAddSelectedPage();
    container->releaseSplit(this);
    target->insertSplit(this);
    window.show();
}

bool Split::canUsePriorityChat() const
{
    if (this->isActivityPane() || this->isModViewPane())
        return false;
    const auto channel = this->getChannel();
    return channel &&
           (channel->isTwitchOrKickChannel() || channel->isMergedChannel());
}

bool Split::priorityChatRequested() const
{
    return !this->isUtilityPane() && this->priorityChatEnabled_;
}

void Split::refreshPriorityChatAccess()
{
    this->header_->updateIcons();
    if (this->isPriorityChatPane() && this->priorityChat_)
        this->priorityChat_->setMonitoring(this->canUsePriorityChat());
    // Role/account signals can arrive while layouts or sibling panes change.
    QTimer::singleShot(0, this, [this] {
        auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
        if (!container)
            return;
        const auto splits = container->getSplits();
        if (std::find(splits.begin(), splits.end(), this) == splits.end())
            return;
        if (!this->isPriorityChatPane())
            this->normalizePriorityChatPanes();
        else if (!this->canUsePriorityChat())
        {
            if (auto *owner = this->priorityChatOwner();
                owner && !this->priorityChatStandalone_)
                owner->normalizePriorityChatPanes();
            else
                container->deleteSplit(this);
        }
    });
}

bool Split::priorityChatEnabled() const
{
    if (this->isPriorityChatPane() || this->isActivityPane() ||
        !this->canUsePriorityChat())
        return false;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    const auto splits =
        container ? container->getSplits() : std::vector<Split *>{};
    if (!container ||
        std::find(splits.begin(), splits.end(), this) == splits.end())
        return this->priorityChatEnabled_;
    return !this->linkedPriorityChatPanes().empty();
}

bool Split::isPriorityChatPane() const
{
    return this->priorityChatPane_;
}
bool Split::priorityChatStandalone() const
{
    return this->priorityChatStandalone_;
}
QString Split::priorityChatId() const
{
    return this->priorityChatId_;
}
QString Split::priorityChatOwnerId() const
{
    return this->priorityChatOwnerId_;
}
QString Split::priorityChatTitle() const
{
    return this->priorityChat_ ? this->priorityChat_->title()
                               : QStringLiteral("Priority Chat");
}

Split *Split::priorityChatOwner()
{
    if (!this->priorityChatPane_)
        return this;
    if (this->priorityChatOwner_)
        return this->priorityChatOwner_;
    if (this->priorityChatOwnerId_.isEmpty())
        return nullptr;
    if (auto *container = dynamic_cast<SplitContainer *>(this->parentWidget()))
        for (auto *split : container->getSplits())
            if (!split->isUtilityPane() &&
                split->priorityChatId_ == this->priorityChatOwnerId_)
            {
                this->priorityChatOwner_ = split;
                return split;
            }
    for (auto *widget : QApplication::allWidgets())
        if (auto *split = qobject_cast<Split *>(widget);
            split && split != this && !split->isPriorityChatPane() &&
            !split->isActivityPane() &&
            split->priorityChatId_ == this->priorityChatOwnerId_ &&
            dynamic_cast<SplitContainer *>(split->parentWidget()))
        {
            this->priorityChatOwner_ = split;
            return split;
        }
    return nullptr;
}

std::vector<Split *> Split::linkedPriorityChatPanes() const
{
    std::vector<Split *> panes;
    if (this->isPriorityChatPane() || this->isActivityPane())
        return panes;
    if (auto *container = dynamic_cast<SplitContainer *>(this->parentWidget()))
        for (auto *split : container->getSplits())
            if (split != this && split->isPriorityChatPane() &&
                !split->priorityChatStandalone_ &&
                split->priorityChatOwnerId_ == this->priorityChatId_)
                panes.push_back(split);
    return panes;
}

void Split::restorePriorityChatState(bool pane, const QString &id,
                                     const QString &ownerId, bool standalone)
{
    if (!id.isEmpty())
        this->priorityChatId_ = id;
    this->priorityChatPane_ = pane;
    this->priorityChatStandalone_ = standalone;
    this->priorityChatOwnerId_ = ownerId;
    if (pane)
    {
        this->priorityChatEnabled_ = false;
        this->setModerationMode(false);
        this->setInputEnabled(false);
        this->setFilterActivity(false);
        this->setSlowerChatEnabled(false);
        this->ensurePriorityChat();
        this->priorityChat_->setMonitoring(this->canUsePriorityChat());
    }
}

void Split::normalizePriorityChatPanes()
{
    if (this->isUtilityPane())
        return;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
        return;
    const auto splits = container->getSplits();
    if (std::find(splits.begin(), splits.end(), this) == splits.end())
        return;
    const auto panes = this->linkedPriorityChatPanes();
    if (!this->canUsePriorityChat())
    {
        const bool requested = this->priorityChatEnabled_ || !panes.empty();
        this->setPriorityChatEnabled(false);
        // Keep restored activation while the source channel is unavailable.
        this->priorityChatEnabled_ = requested;
        return;
    }
    for (size_t i = 1; i < panes.size(); ++i)
        container->deleteSplit(panes[i]);
    if (!panes.empty())
        this->priorityChatEnabled_ = true;
    else if (this->priorityChatEnabled_)
        this->setPriorityChatEnabled(true);
    this->updateHeaderIcons();
}

void Split::ensurePriorityChat()
{
    if (this->priorityChat_)
        return;
    this->priorityChat_ = new PriorityChat(
        this, this, this->priorityChatPane_ ? this->view_ : nullptr);
    this->priorityChat_->hide();
    this->signalHolder_.managedConnect(
        this->priorityChat_->titleChanged, [this] {
            if (this->priorityChatPane_)
            {
                this->header_->updateChannelText();
                this->actionRequested.invoke(Action::RefreshTab);
            }
        });
    this->priorityChat_->setChannel(this->getChannel());
}

void Split::setPriorityChatEnabled(bool enabled)
{
    if (this->isPriorityChatPane() || this->isModViewPane())
        return;
    enabled = enabled && !this->isActivityPane();
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    const auto splits =
        container ? container->getSplits() : std::vector<Split *>{};
    if (!container ||
        std::find(splits.begin(), splits.end(), this) == splits.end())
    {
        this->priorityChatEnabled_ = enabled;
        if (enabled)
            QTimer::singleShot(0, this, [this] {
                this->normalizePriorityChatPanes();
            });
        return;
    }
    if (enabled && !this->canUsePriorityChat())
        return;
    this->priorityChatEnabled_ = enabled;
    const auto panes = this->linkedPriorityChatPanes();
    if (!enabled)
    {
        // releaseSplit also updates the owner; re-read after each removal.
        for (;;)
        {
            const auto remaining = this->linkedPriorityChatPanes();
            if (remaining.empty())
                break;
            container->deleteSplit(remaining.front());
        }
    }
    else if (panes.empty())
    {
        auto *pane = container->cloneSplit(
            this, this->getFilters(), SplitDirection::Right,
            ALERTS_PRIMARY_RATIO, ALERTS_SECONDARY_RATIO, false);
        if (pane)
        {
            pane->restorePriorityChatState(true, {}, this->priorityChatId_,
                                           false);
            pane->priorityChatOwner_ = this;
            pane->setTitleSettingsButtonVisible(true);
        }
    }
    this->header_->updateIcons();
    getApp()->getWindows()->queueSave();
}

void Split::setPriorityChatUserChoice(const MessagePtr &message,
                                      priority::Choice choice)
{
    if (this->isActivityPane() || !this->canUsePriorityChat())
        return;
    this->ensurePriorityChat();
    this->priorityChat_->setUserChoice(message, choice);
    if (choice == priority::Choice::Include && !this->isPriorityChatPane())
        this->setPriorityChatEnabled(true);
}

priority::Choice Split::priorityChatUserChoice(const Message &message)
{
    if (!this->canUsePriorityChat())
        return priority::Choice::Automatic;
    this->ensurePriorityChat();
    return this->priorityChat_->userChoice(message);
}

bool Split::isPriorityChatUserIncluded(const Message &message)
{
    if (!this->canUsePriorityChat())
        return false;
    const auto panes = this->linkedPriorityChatPanes();
    auto *split = panes.empty() ? this : panes.front();
    split->ensurePriorityChat();
    return split->priorityChat_->includesUser(message);
}

std::optional<bool> Split::checkSpellingOverride() const
{
    return this->input_->checkSpellingOverride();
}

void Split::setCheckSpellingOverride(std::optional<bool> override)
{
    this->input_->setCheckSpellingOverride(override);
}

void Split::insertTextToInput(const QString &text)
{
    if (this->isModViewPane())
    {
        if (auto *owner = this->modViewOwner())
            owner->insertTextToInput(text);
        return;
    }
    if (this->isPriorityChatPane())
    {
        if (auto *owner = this->priorityChatOwner()) owner->insertTextToInput(text);
        return;
    }
    this->input_->insertText(text);
}

void Split::showChangeChannelPopup(const char *dialogTitle, bool empty,
                                   std::function<void(bool)> callback)
{
    if (!this->selectChannelDialog_.isNull())
    {
        this->selectChannelDialog_->raise();

        return;
    }

    QPointer<Split> activityOwnerSplit =
        this->isActivityPane() ? findActivityOwnerSplit(this) : this;
    QPointer<Split> linkedActivityPane =
        activityOwnerSplit ? findLinkedActivityPane(activityOwnerSplit) : nullptr;
    const bool showSpecialPage =
        empty ||
        (activityOwnerSplit &&
         (activityOwnerSplit->getChannel()->isEmpty() ||
          isTwitchSpecialChannelType(
              activityOwnerSplit->getIndirectChannel().getType())));

    auto *dialog = new SelectChannelDialog(showSpecialPage, empty, this);
    if (!empty)
    {
        dialog->setSelectedChannel(activityOwnerSplit
                                       ? activityOwnerSplit->getIndirectChannel()
                                       : this->getIndirectChannel());
    }
    else
    {
        dialog->setSelectedChannel({});
    }
    dialog->setActivityPaneEnabled(linkedActivityPane != nullptr);
    dialog->setFilterActivity(activityOwnerSplit
                                  ? activityOwnerSplit->filterActivity()
                                  : this->filterActivity());
    if (!empty)
    {
        dialog->setPlatformIndicatorMode(activityOwnerSplit
                                             ? activityOwnerSplit
                                                   ->platformIndicatorMode()
                                             : this->platformIndicatorMode());
    }
    dialog->setSlowerChatEnabled(activityOwnerSplit
                                     ? activityOwnerSplit->slowerChatEnabled()
                                     : this->slowerChatEnabled());
    dialog->setSlowerChatMessagesPerSecond(
        activityOwnerSplit ? activityOwnerSplit->slowerChatMessagesPerSecond()
                           : this->slowerChatMessagesPerSecond());
    dialog->setViewerCountEnabled(activityOwnerSplit
                                      ? activityOwnerSplit->viewerCountEnabled()
                                      : this->viewerCountEnabled());
    dialog->setStreamDatabaseBadgeFeedVisible(
        activityOwnerSplit ? activityOwnerSplit->streamDatabaseBadgeFeedVisible()
                           : this->streamDatabaseBadgeFeedVisible());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(dialogTitle);
    dialog->show();
    // We can safely ignore this signal connection since the dialog will be closed before
    // this Split is closed
    std::ignore = dialog->closed.connect([=, this] {
        const bool acceptedChanges = dialog->hasSeletedChannel();
        const bool selectedFolder = acceptedChanges && dialog->selectedFolder();

        if (selectedFolder)
        {
            auto *window = dynamic_cast<Window *>(this->window());
            callback(false);
            if (window != nullptr)
            {
                window->getNotebook().showAddFolderDialog();
            }
            return;
        }

        if (!acceptedChanges || !activityOwnerSplit)
        {
            callback(acceptedChanges);
            return;
        }

        const auto selectedChannel = dialog->getSelectedChannel();
        const auto platformIndicatorMode = dialog->platformIndicatorMode();
        const auto filterActivity = dialog->filterActivity();
        const auto slowerChatEnabled = dialog->slowerChatEnabled();
        const auto slowerChatMessagesPerSecond =
            dialog->slowerChatMessagesPerSecond();
        const auto viewerCountEnabled = dialog->viewerCountEnabled();
        const auto streamDatabaseBadgeFeedVisible =
            dialog->streamDatabaseBadgeFeedVisible();
        const auto activityPaneEnabled = dialog->activityPaneEnabled();

        const bool didChangeChannel = !areEquivalentIndirectChannels(
            activityOwnerSplit->getIndirectChannel(), selectedChannel);
        if (didChangeChannel)
        {
            activityOwnerSplit->setChannel(selectedChannel);
        }
        activityOwnerSplit->setPlatformIndicatorMode(platformIndicatorMode);
        activityOwnerSplit->setFilterActivity(filterActivity, true);
        activityOwnerSplit->setSlowerChatEnabled(slowerChatEnabled);
        activityOwnerSplit->setSlowerChatMessagesPerSecond(
            slowerChatMessagesPerSecond);
        activityOwnerSplit->setViewerCountEnabled(viewerCountEnabled);
        if (splitIsStreamDatabase(activityOwnerSplit))
        {
            activityOwnerSplit->setStreamDatabaseBadgeFeedVisible(
                streamDatabaseBadgeFeedVisible);
        }

        const bool didToggleActivityPane =
            (linkedActivityPane != nullptr) != activityPaneEnabled;

        callback(true);

        if (activityOwnerSplit && (didChangeChannel || didToggleActivityPane))
        {
            syncLinkedActivityPane(activityOwnerSplit, linkedActivityPane,
                                   activityPaneEnabled);
        }
    });
    this->selectChannelDialog_ = dialog;
}

void Split::showSettingsDialog()
{
    if (this->isModViewPane())
    {
        this->normalizeModViewPanes();
        if (this->modView_)
            this->modView_->showRules();
        return;
    }
    if (this->isPriorityChatPane())
    {
        if (!this->canUsePriorityChat())
            return;
        this->ensurePriorityChat();
        this->priorityChat_->showSettings();
        return;
    }
    if (!this->splitSettingsDialog_.isNull())
    {
        this->splitSettingsDialog_->raise();
        return;
    }

    auto *settingsOwnerSplit =
        this->isActivityPane() ? findActivityOwnerSplit(this) : this;
    const bool showTwitchActivityMinimum =
        this->isActivityPane() && splitHasTwitchActivity(settingsOwnerSplit);
    const bool showKickActivityMinimum =
        this->isActivityPane() && splitHasKickActivity(settingsOwnerSplit);
    const bool showTikTokActivityMinimum =
        this->isActivityPane() &&
        mergedSplitHasTikTokEnabled(settingsOwnerSplit);
    auto *dialog = new SplitSettingsDialog(
        this->isActivityPane(), showTwitchActivityMinimum,
        showKickActivityMinimum, showTikTokActivityMinimum,
        !this->isActivityPane() && splitIsStreamDatabase(settingsOwnerSplit),
        this);
    dialog->setPlatformIndicatorMode(this->platformIndicatorMode());
    dialog->setFilterActivity(this->filterActivity());
    dialog->setActivityMessageScale(this->activityMessageScale());
    dialog->setCollapseGiftedSubscriptions(
        this->collapseGiftedSubscriptions());
    dialog->setShowRaids(this->showRaids());
    dialog->setShowWatchStreaks(this->showWatchStreaks());
    dialog->setActivityTimeDisplayMode(this->activityTimeDisplayMode());
    dialog->setSlowerChatEnabled(this->slowerChatEnabled());
    dialog->setSlowerChatMessagesPerSecond(
        this->slowerChatMessagesPerSecond());
    dialog->setViewerCountEnabled(this->viewerCountEnabled());
    dialog->setStreamDatabaseBadgeFeedVisible(
        this->streamDatabaseBadgeFeedVisible());
    const auto activityMinimum =
        showTwitchActivityMinimum
            ? this->twitchActivityMinimumBits()
            : (showKickActivityMinimum
                   ? this->kickActivityMinimumKicks()
                   : this->tiktokActivityMinimumDiamonds());
    dialog->setActivityMinimum(activityMinimum);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(this->isActivityPane() ? this->activityPaneTitle()
                                                  : "Split settings");
    dialog->show();

    std::ignore = dialog->closed.connect([this, dialog] {
        if (!dialog->hasAcceptedChanges())
        {
            return;
        }

        const auto platformIndicatorMode = dialog->platformIndicatorMode();
        const auto activityMessageScale = dialog->activityMessageScale();
        const auto collapseGiftedSubscriptions =
            dialog->collapseGiftedSubscriptions();
        const auto showRaids = dialog->showRaids();
        const auto showWatchStreaks = dialog->showWatchStreaks();
        const auto activityTimeDisplayMode =
            dialog->activityTimeDisplayMode();
        const auto activityMinimum = dialog->activityMinimum();
        const auto filterActivity = dialog->filterActivity();
        const auto slowerChatEnabled = dialog->slowerChatEnabled();
        const auto slowerChatMessagesPerSecond =
            dialog->slowerChatMessagesPerSecond();
        const auto viewerCountEnabled = dialog->viewerCountEnabled();
        const auto streamDatabaseBadgeFeedVisible =
            dialog->streamDatabaseBadgeFeedVisible();

        this->setPlatformIndicatorMode(platformIndicatorMode);
        if (this->isActivityPane())
        {
            this->setActivityMessageScale(activityMessageScale);
            this->setCollapseGiftedSubscriptions(
                collapseGiftedSubscriptions);
            this->setShowRaids(showRaids);
            this->setShowWatchStreaks(showWatchStreaks);
            this->setActivityTimeDisplayMode(activityTimeDisplayMode);
            this->setTwitchActivityMinimumBits(activityMinimum);
            this->setKickActivityMinimumKicks(activityMinimum);
            this->setTikTokActivityMinimumDiamonds(activityMinimum);
            if (auto *ownerSplit = findActivityOwnerSplit(this))
            {
                ownerSplit->setCollapseGiftedSubscriptions(
                    collapseGiftedSubscriptions);
                ownerSplit->setShowRaids(showRaids);
                ownerSplit->setShowWatchStreaks(showWatchStreaks);
                ownerSplit->setTwitchActivityMinimumBits(
                    activityMinimum);
                ownerSplit->setKickActivityMinimumKicks(
                    activityMinimum);
                ownerSplit->setTikTokActivityMinimumDiamonds(
                    activityMinimum);
            }
            return;
        }

        this->setFilterActivity(filterActivity, true);
        this->setSlowerChatEnabled(slowerChatEnabled);
        this->setSlowerChatMessagesPerSecond(slowerChatMessagesPerSecond);
        this->setViewerCountEnabled(viewerCountEnabled);
        if (splitIsStreamDatabase(this))
        {
            this->setStreamDatabaseBadgeFeedVisible(
                streamDatabaseBadgeFeedVisible);
        }
    });

    this->splitSettingsDialog_ = dialog;
}

void Split::updateGifEmotes()
{
    this->view_->queueUpdate();
}

void Split::updateLastReadMessage()
{
    this->view_->updateLastReadMessage();
}

void Split::updateHeaderIcons()
{
    this->header_->updateIcons();
}

void Split::paintEvent(QPaintEvent *)
{
    // color the background of the chat
    if (this->theme->effect().isScene() &&
        scenes::ThemeSceneHost::instance().showsScene(this))
    {
        // The window paints the scene; gaps between panels show it
        return;
    }

    QPainter painter(this);

    painter.fillRect(this->rect(), this->theme->splits.background);
}

void Split::mouseMoveEvent(QMouseEvent *event)
{
    (void)event;

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::keyPressEvent(QKeyEvent *event)
{
    (void)event;

    this->view_->unsetCursor();
    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::keyReleaseEvent(QKeyEvent *event)
{
    (void)event;

    this->view_->unsetCursor();
    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::resizeEvent(QResizeEvent *event)
{
    // Queue up save because: Split resized
    getApp()->getWindows()->queueSave();

    BaseWidget::resizeEvent(event);

    this->overlay_->setGeometry(this->rect());
}

void Split::enterEvent(QEnterEvent * /*event*/)
{
    this->isMouseOver_ = true;

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());

    if (modifierStatus ==
        SHOW_SPLIT_OVERLAY_MODIFIERS /*|| modifierStatus == showAddSplitRegions*/)
    {
        this->overlay_->show();
    }

    this->actionRequested.invoke(Action::ResetMouseStatus);
}

void Split::leaveEvent(QEvent *event)
{
    (void)event;

    this->isMouseOver_ = false;

    this->overlay_->hide();

    this->handleModifiers(QGuiApplication::queryKeyboardModifiers());
}

void Split::handleModifiers(Qt::KeyboardModifiers modifiers)
{
    if (modifierStatus != modifiers)
    {
        modifierStatus = modifiers;
        modifierStatusChanged.invoke(modifiers);
    }
}

void Split::setIsTopRightSplit(bool value)
{
    this->isTopRightSplit_ = value;
    this->header_->setAddButtonVisible(value);
}

/// Slots
void Split::addSibling()
{
    this->actionRequested.invoke(Action::AppendNewSplit);
}

void Split::deleteFromContainer()
{
    this->actionRequested.invoke(Action::Delete);
}

void Split::changeChannel()
{
    this->showChangeChannelPopup(
        "Tab settings", false, [this](bool didSelectChannel) {
            if (!didSelectChannel)
            {
                return;
            }

            // After changing channel (i.e. pressing OK in the channel switcher), close all open Viewer Lists
            // We could consider updating the viewer list with the new channel
            for (const auto &w : this->findChildren<ChatterListWidget *>())
            {
                w->close();
            }
        });
}

void Split::openAlertsPane()
{
    if (this->isPriorityChatPane() || this->isModViewPane())
        return;
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
    {
        return;
    }

    if (this->isActivityPane())
    {
        if (auto *ownerSplit = findActivityOwnerSplit(this))
        {
            ownerSplit->openAlertsPane();
            return;
        }
        container->deleteSplit(this);
        for (auto *split : container->getSplits())
        {
            split->header_->updateIcons();
        }
        return;
    }

    const auto linkedPanes = findLinkedActivityPanes(this);
    if (!linkedPanes.empty())
    {
        this->setFilterActivity(false);
        for (auto *pane : linkedPanes)
            container->deleteSplit(pane);
        refreshActivityIcons(container);
        return;
    }
    if (this->getChannel()->isEmpty())
        return;
    const auto alertFilterId = ensureAlertsFilter();
    if (!alertFilterId)
        return;
    auto filterIds = this->getFilters();
    if (!filterIds.contains(*alertFilterId))
        filterIds.append(*alertFilterId);

    auto *alertsSplit = container->cloneSplit(
        this, filterIds, SplitDirection::Right, ALERTS_PRIMARY_RATIO,
        ALERTS_SECONDARY_RATIO, false);
    if (!alertsSplit)
    {
        return;
    }

    alertsSplit->setInputEnabled(false);
    alertsSplit->activityOwner_ = this;
    alertsSplit->setFilterActivity(false);
    alertsSplit->setSlowerChatEnabled(false);
    alertsSplit->setPlatformIndicatorMode(defaultPlatformIndicatorMode(true));
    this->setFilterActivity(true);
    container->setSelected(alertsSplit);
    alertsSplit->setFocus(Qt::OtherFocusReason);
    for (auto *split : container->getSplits())
    {
        split->header_->updateIcons();
    }
}

void Split::explainMoving()
{
    showTutorialVideo(this, ":/examples/moving.gif", "Moving",
                      "Hold <Ctrl+Alt> to move splits.\n\nExample:");
}

void Split::explainSplitting()
{
    showTutorialVideo(this, ":/examples/splitting.gif", "Splitting",
                      "Hold <Ctrl+Alt> to add new splits.\n\nExample:");
}

void Split::popup()
{
    if (this->isModViewPane())
    {
        this->toggleModViewPopOut();
        return;
    }
    auto *app = getApp();
    Window &window = app->getWindows()->createWindow(WindowType::Popup);

    auto *split = new Split(window.getNotebook().getOrAddSelectedPage());

    if (this->isPriorityChatPane())
        split->restorePriorityChatState(true, {}, this->priorityChatOwnerId_, true);
    split->setChannel(this->getIndirectChannel());
    split->setModerationMode(this->getModerationMode());
    split->setPriorityChatEnabled(this->priorityChatRequested());
    split->setFilters(this->getFilters());
    split->setInputEnabled(this->inputEnabled());
    split->setFilterActivity(this->filterActivity(),
                             this->filterActivityExplicit());
    split->setActivityMessageScale(this->activityMessageScale());
    split->setCollapseGiftedSubscriptions(
        this->collapseGiftedSubscriptions());
    split->setShowRaids(this->showRaids());
    split->setShowWatchStreaks(this->showWatchStreaks());
    split->setActivityTimeDisplayMode(this->activityTimeDisplayMode());
    split->setSlowerChatEnabled(this->slowerChatEnabled());
    split->setSlowerChatMessagesPerSecond(
        this->slowerChatMessagesPerSecond());
    split->setSlowerChatMessageAnimations(
        this->slowerChatMessageAnimations());
    split->setViewerCountEnabledOverride(this->viewerCountEnabledOverride());
    split->setStreamDatabaseBadgeFeedVisible(
        this->streamDatabaseBadgeFeedVisible());
    split->setTitleSettingsButtonVisible(this->titleSettingsButtonVisible());
    split->setChatModeIndicatorVisible(this->chatModeIndicatorVisible());
    split->setTwitchActivityMinimumBits(this->twitchActivityMinimumBits());
    split->setKickActivityMinimumKicks(this->kickActivityMinimumKicks());
    split->setTikTokActivityMinimumDiamonds(
        this->tiktokActivityMinimumDiamonds());
    split->setPlatformIndicatorMode(this->platformIndicatorMode());

    window.getNotebook().getOrAddSelectedPage()->insertSplit(split);
    window.show();
}

OverlayWindow *Split::overlayWindow()
{
    return this->overlayWindow_.data();
}

void Split::showOverlayWindow()
{
    if (!this->overlayWindow_)
    {
        this->overlayWindow_ =
            new OverlayWindow(this->getIndirectChannel(), this->getFilters());
    }
    this->overlayWindow_->show();
}

void Split::clear()
{
    this->view_->clearMessages();
}

void Split::openInBrowser()
{
    auto channel = this->getChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://www.twitch.tv/" +
                                  twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://kick.com/" + kc->slug());
    }
    else if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
    {
        auto browserUrls = merged->liveStreamBrowserUrls();
        if (browserUrls.empty())
        {
            browserUrls = merged->channelBrowserUrls();
        }

        if (!browserUrls.empty())
        {
            QDesktopServices::openUrl(browserUrls.front().url);
        }
    }
}

void Split::openWhispersInBrowser()
{
    auto userName = getApp()->getAccounts()->twitch.getCurrent()->getUserName();
    QDesktopServices::openUrl("https://www.twitch.tv/popout/moderator/" +
                              userName + "/whispers");
}

void Split::openBrowserPlayer()
{
    this->openChannelInBrowserPlayer(this->getChannel());
}

void Split::openModViewInBrowser()
{
    auto channel = this->getChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://www.twitch.tv/moderator/" +
                                  twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        QDesktopServices::openUrl("https://dashboard.kick.com/moderator/" +
                                  kc->slug());
    }
}

void Split::openInStreamlink()
{
    if (auto *merged = dynamic_cast<MergedChannel *>(this->getChannel().get()))
    {
        QList<StreamlinkSource> sources;
        const auto liveUrls = merged->liveStreamBrowserUrls();
        for (auto source : merged->channelBrowserUrls())
        {
            for (const auto &live : liveUrls)
            {
                if (live.platformName == source.platformName)
                {
                    source.url = live.url;
                    break;
                }
            }
            if (source.platformName == "TikTok" &&
                !source.url.path().endsWith("/live"))
            {
                source.url.setPath(source.url.path() + "/live");
            }
            sources.append({source.platformName, source.url.toString()});
        }
        QualityPopup::showDialog(sources);
        return;
    }
    auto *kc = dynamic_cast<KickChannel *>(this->getChannel().get());
    if (kc)
    {
        openStreamlinkForChannel(kc->slug(), u"kick.com/");
        return;
    }
    this->openChannelInStreamlink(this->getChannel()->getName());
}

void Split::openWithCustomScheme()
{
    auto *const channel = this->getChannel().get();
    if (auto *const twitchChannel = dynamic_cast<TwitchChannel *>(channel))
    {
        this->openChannelInCustomPlayer(twitchChannel->getName());
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel))
    {
        openInCustomPlayer(kc->slug(), u"https://kick.com/");
    }
}

void Split::openChatterList()
{
    if (this->isActivityPane())
    {
        qCWarning(chatterinoWidget)
            << "Viewer list opened from an activity pane";
        return;
    }

    auto channel = this->getChannel();
    if (!channel)
    {
        qCWarning(chatterinoWidget)
            << "Viewer list opened when no channel was defined";
        return;
    }

    auto *twitchChannel = viewerListTwitchChannel(channel.get());
    if (twitchChannel == nullptr)
    {
        qCWarning(chatterinoWidget)
            << "Viewer list opened without a Twitch channel";
        return;
    }

    const auto chatterListWidth = static_cast<int>(this->width() * 0.5);
    const auto chatterListHeight =
        this->height() - this->header_->height() - this->input_->height();

    auto *chatterDock = new ChatterListWidget(twitchChannel, this);

    QObject::connect(chatterDock, &ChatterListWidget::userClicked,
                     [this](const QString &userLogin) {
                         this->view_->showUserInfoPopup(
                             userLogin, MessagePlatform::AnyOrTwitch);
                     });
    QObject::connect(chatterDock, &QObject::destroyed, this, [this] {
        this->header_->updateIcons();
    });

    chatterDock->resize(chatterListWidth, chatterListHeight);
    widgets::showAndMoveWindowTo(
        chatterDock, this->mapToGlobal(QPoint{0, this->header_->height()}),
        widgets::BoundsChecking::CursorPosition);
    this->header_->updateIcons();
}

void Split::openSubPage()
{
    ChannelPtr channel = this->getChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        QDesktopServices::openUrl(twitchChannel->subscriptionUrl());
    }
}

void Split::setFiltersDialog()
{
    QList<QUuid> hiddenFilters;
    if (const auto alertFilterId = findAlertsFilterId())
    {
        hiddenFilters.append(*alertFilterId);
    }

    SelectChannelFiltersDialog d(this->getFilters(), hiddenFilters, this);
    d.setWindowTitle("Select filters");

    if (d.exec() == QDialog::Accepted)
    {
        this->setFilters(d.getSelection());
    }
}

void Split::setFilters(const QList<QUuid> ids)
{
    const auto activityPanes = findLinkedActivityPanes(this);
    const auto priorityPanes = this->linkedPriorityChatPanes();
    auto filterIds = ids;
    if (this->isActivityPane())
    {
        if (const auto alertFilterId = ensureAlertsFilter())
        {
            if (!filterIds.contains(*alertFilterId))
            {
                filterIds.append(*alertFilterId);
            }
        }
    }

    this->view_->setFilters(filterIds);
    if (this->priorityChat_) this->priorityChat_->refreshFilters();
    this->header_->updateChannelText();
    for (auto *pane : activityPanes)
        pane->setFilters(ids);
    for (auto *pane : priorityPanes) pane->setFilters(ids);
    refreshActivityIcons(dynamic_cast<SplitContainer *>(this->parentWidget()));
}

QList<QUuid> Split::getFilters() const
{
    return this->view_->getFilterIds();
}

void Split::showSearch(bool singleChannel)
{
    auto *popup = new SearchPopup(this, this);
    popup->setAttribute(Qt::WA_DeleteOnClose);

    if (singleChannel)
    {
        popup->addChannel(this->getChannelView());
        popup->show();
        return;
    }

    // Pass every ChannelView for every Split across the app to the search popup
    auto &notebook = getApp()->getWindows()->getMainWindow().getNotebook();
    for (int i = 0; i < notebook.getPageCount(); ++i)
    {
        auto *container = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
        if (container == nullptr)
        {
            continue;
        }

        for (auto *split : container->getSplits())
        {
            if (split == nullptr)
            {
                continue;
            }

            if (split->channel_.getType() != Channel::Type::TwitchAutomod)
            {
                popup->addChannel(split->getChannelView());
            }
        }
    }

    popup->show();
}

void Split::reconnect()
{
    this->getChannel()->reconnect();
}

void Split::dragEnterEvent(QDragEnterEvent *event)
{
    if (getSettings()->imageUploaderEnabled &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
    {
        event->acceptProposedAction();
    }
    else
    {
        BaseWidget::dragEnterEvent(event);
    }
}

void Split::dropEvent(QDropEvent *event)
{
    if (getSettings()->imageUploaderEnabled &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
    {
        this->input_->ui_.textEdit->imagePasted.invoke(event->mimeData());
    }
    else
    {
        BaseWidget::dropEvent(event);
    }
}

void Split::drag()
{
    auto *container = dynamic_cast<SplitContainer *>(this->parentWidget());
    if (!container)
    {
        qCWarning(chatterinoWidget) << "Attempted to initiate split drag "
                                       "without a container parent";
        return;
    }

    startDraggingSplit();

    auto originalLocation = container->releaseSplit(this);
    auto *drag = new QDrag(this);
    auto *mimeData = new QMimeData;

    mimeData->setData("chatterino/split", "xD");
    drag->setMimeData(mimeData);

    // drag->exec is a blocking action
    auto dragRes = drag->exec(Qt::MoveAction);
    if (dragRes != Qt::MoveAction || drag->target() == nullptr)
    {
        // The split wasn't dropped in a valid spot, return it to its original position
        container->insertSplit(this, {.position = originalLocation});
    }

    stopDraggingSplit();
}

void Split::setInputReply(const MessagePtr &reply)
{
    if (this->isModViewPane())
    {
        if (auto *owner = this->modViewOwner())
            owner->setInputReply(reply);
        return;
    }
    if (this->isPriorityChatPane())
    {
        if (auto *owner = this->priorityChatOwner()) owner->setInputReply(reply);
        return;
    }
    this->input_->setReply(reply);
}

void Split::unpause()
{
    this->view_->unpause(PauseReason::KeyboardModifier);
    this->view_->unpause(PauseReason::DoubleClick);
    // Mouse intentionally left out, we may still have the mouse over the split
}

}  // namespace chatterino

QDebug operator<<(QDebug dbg, const chatterino::Split &split)
{
    auto channel = split.getChannel();
    if (channel)
    {
        dbg.nospace() << "Split(" << (void *)&split
                      << ", channel:" << channel->getName() << ")";
    }
    else
    {
        dbg.nospace() << "Split(" << (void *)&split << ", no channel)";
    }

    return dbg;
}

QDebug operator<<(QDebug dbg, const chatterino::Split *split)
{
    if (split != nullptr)
    {
        return operator<<(dbg, *split);
    }

    dbg.nospace() << "Split(nullptr)";

    return dbg;
}
