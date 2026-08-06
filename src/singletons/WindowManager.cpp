// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/WindowManager.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/QLogging.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/CombinePath.hpp"
#include "util/FilesystemHelpers.hpp"
#include "util/QMagicEnum.hpp"
#include "util/SignalListener.hpp"
#include "widgets/AccountSwitchPopup.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/FramelessEmbedWindow.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/OverlayWindow.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/Window.hpp"

#include <pajlada/settings/backup.hpp>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSaveFile>
#include <QScreen>
#include <QTextStream>

#include <chrono>
#include <optional>
#include <vector>

namespace {

std::optional<bool> &shouldMoveOutOfBoundsWindow()
{
    static std::optional<bool> x;
    return x;
}

void closeWindowsRecursive(QWidget *window)
{
    if (window->isWindow() && window->isVisible())
    {
        window->close();
    }

    for (auto *child : window->children())
    {
        if (child->isWidgetType())
        {
            // We check if it's a widget above
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            closeWindowsRecursive(static_cast<QWidget *>(child));
        }
    }
}

}  // namespace

namespace chatterino {

const QString WindowManager::WINDOW_LAYOUT_FILENAME(
    QStringLiteral("window-layout.json"));

using SplitNode = SplitContainer::Node;

namespace {

QString platformIndicatorModeName(PlatformIndicatorMode mode)
{
    switch (mode)
    {
        case PlatformIndicatorMode::None:
            return QStringLiteral("none");
        case PlatformIndicatorMode::LineColor:
            return QStringLiteral("linecolor");
        case PlatformIndicatorMode::Badge:
            return QStringLiteral("badge");
        case PlatformIndicatorMode::Both:
            return QStringLiteral("both");
    }

    return QStringLiteral("linecolor");
}

QString messagePlatformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral("kick");
        case MessagePlatform::YouTube:
            return QStringLiteral("youtube");
        case MessagePlatform::TikTok:
            return QStringLiteral("tiktok");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral("twitch");
    }
}

QJsonArray messagePlatformArray(const std::vector<MessagePlatform> &platforms)
{
    QJsonArray array;
    for (const auto platform : platforms)
    {
        array.append(messagePlatformName(platform));
    }
    return array;
}

void layoutTrace(const QString &message)
{
    QFile file(QStringLiteral("mergerino-layout-trace.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return;
    }

    QTextStream stream(&file);
    stream << message << '\n';
}

}  // namespace

void WindowManager::showSettingsDialog(QWidget *parent,
                                       SettingsDialogPreference preference)
{
    if (this->appArgs.dontSaveSettings)
    {
        QMessageBox::critical(parent, "Mergerino - Editing Settings Forbidden",
                              "Settings cannot be edited when running with\n"
                              "commandline arguments such as '-c'.");
    }
    else
    {
        QTimer::singleShot(80, [parent, preference] {
            SettingsDialog::showDialog(parent, preference);
        });
    }
}

void WindowManager::showAccountSelectPopup(QPoint point)
{
    static auto *w = new AccountSwitchPopup;
    constexpr int toggleDebounceMs = 150;

    if (w->isVisible())
    {
        w->hide();
        return;
    }
    if (w->wasHiddenRecently(toggleDebounceMs))
    {
        return;
    }

    w->refresh();

    w->moveTo(point - QPoint(30, 0), widgets::BoundsChecking::CursorPosition);
    w->show();
    w->setFocus();
}

ProviderId WindowManager::activeAccountProvider() const
{
    return this->activeAccountProvider_;
}

void WindowManager::setActiveAccountProvider(ProviderId provider)
{
    if (this->activeAccountProvider_ == provider)
    {
        return;
    }

    this->activeAccountProvider_ = provider;
    this->activeAccountProviderChanged.invoke(provider);
}

WindowManager::WindowManager(const Args &appArgs_, const Paths &paths,
                             Settings &settings, Theme &themes_, Fonts &fonts)
    : themes(themes_)
    , appArgs(appArgs_)
    , windowLayoutFilePath(combinePath(paths.settingsDirectory,
                                       WindowManager::WINDOW_LAYOUT_FILENAME))
    , updateWordTypeMaskListener([this] {
        this->updateWordTypeMask();
    })
    , forceLayoutChannelViewsListener([this] {
        this->forceLayoutChannelViews();
    })
    , layoutChannelViewsListener([this] {
        this->layoutChannelViews();
    })
    , invalidateChannelViewBuffersListener([this] {
        this->invalidateChannelViewBuffers();
    })
    , reloadChannelViewsListener([this] {
        this->reloadChannelViews();
    })
    , repaintVisibleChatWidgetsListener([this] {
        this->repaintVisibleChatWidgets();
    })
{
    qCDebug(chatterinoWindowmanager) << "init WindowManager";

    this->updateWordTypeMaskListener.add(settings.showTimestamps);
    this->updateWordTypeMaskListener.add(settings.showBadgesGlobalAuthority);
    this->updateWordTypeMaskListener.add(settings.showBadgesPredictions);
    this->updateWordTypeMaskListener.add(settings.showBadgesChannelAuthority);
    this->updateWordTypeMaskListener.add(settings.showBadgesSubscription);
    this->updateWordTypeMaskListener.add(settings.showBadgesVanity);
    this->updateWordTypeMaskListener.add(settings.showBadgesChatterino);
    this->updateWordTypeMaskListener.add(settings.showBadgesFfz);
    this->updateWordTypeMaskListener.add(settings.showBadgesBttv);
    this->updateWordTypeMaskListener.add(settings.showBadgesSevenTV);
    this->updateWordTypeMaskListener.add(settings.showKickLevelBadges);
    this->updateWordTypeMaskListener.add(settings.enableEmoteImages);
    this->updateWordTypeMaskListener.add(settings.lowercaseDomains);
    this->updateWordTypeMaskListener.add(settings.showReplyButton);
    this->updateWordTypeMaskListener.add(settings.mergedPlatformIndicatorMode);

    this->forceLayoutChannelViewsListener.add(
        settings.moderationActions.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.highlightedMessages.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.highlightedUsers.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.highlightedBadges.delayedItemsChanged);
    this->forceLayoutChannelViewsListener.add(
        settings.removeSpacesBetweenEmotes);
    this->forceLayoutChannelViewsListener.add(settings.emoteScale);
    this->forceLayoutChannelViewsListener.add(settings.timestampFormat);
    this->forceLayoutChannelViewsListener.add(settings.collpseMessagesMinLines);
    this->forceLayoutChannelViewsListener.add(settings.enableRedeemedHighlight);
    this->forceLayoutChannelViewsListener.add(settings.enableFirstMessageHighlight);
    this->forceLayoutChannelViewsListener.add(
        settings.enableFirstMessageSessionHighlight);
    this->forceLayoutChannelViewsListener.add(
        settings.enableElevatedMessageHighlight);
    this->forceLayoutChannelViewsListener.add(settings.enableSubHighlight);
    this->forceLayoutChannelViewsListener.add(
        settings.enableWatchStreakHighlight);
    this->forceLayoutChannelViewsListener.add(settings.enableAutomodHighlight);
    this->forceLayoutChannelViewsListener.add(settings.colorUsernames);
    this->forceLayoutChannelViewsListener.add(settings.boldUsernames);
    this->forceLayoutChannelViewsListener.add(
        settings.showBlockedTermAutomodMessages);
    this->forceLayoutChannelViewsListener.add(settings.hideModerated);
    this->forceLayoutChannelViewsListener.add(linkPreviewModeSetting());
    this->forceLayoutChannelViewsListener.add(
        settings.streamerModeHideModActions);
    this->forceLayoutChannelViewsListener.add(
        settings.streamerModeHideRestrictedUsers);
    this->forceLayoutChannelViewsListener.add(fonts.fontChanged);

    this->layoutChannelViewsListener.add(settings.timestampFormat);

    this->invalidateChannelViewBuffersListener.add(settings.alternateMessages);
    this->invalidateChannelViewBuffersListener.add(settings.separateMessages);
    this->invalidateChannelViewBuffersListener.add(settings.fadeMessageHistory);
    this->invalidateChannelViewBuffersListener.add(
        settings.mergedPlatformIndicatorMode);

    this->reloadChannelViewsListener.add(settings.platformEventHighlightStyle);
    this->reloadChannelViewsListener.add(
        settings.platformEventHighlightCustomColor);
    this->reloadChannelViewsListener.add(platformAlertHighlightStyleSetting());
    this->reloadChannelViewsListener.add(
        platformAlertHighlightCustomColorSetting());

    settings.messageAnimations.connect(
        [this] {
            for (auto *window : this->windows_)
            {
                window->getNotebook().forEachSplit([](Split *split) {
                    split->getChannelView().refreshSlowerChatSettings();
                });
            }
        },
        this->signalHolder,
        false);

    this->repaintVisibleChatWidgetsListener.add(
        this->themes.repaintVisibleChatWidgets_);

    this->saveTimer = new QTimer;

    this->saveTimer->setSingleShot(true);

    QObject::connect(this->saveTimer, &QTimer::timeout, [] {
        getApp()->getWindows()->save();
    });

    this->updateWordTypeMask();
}

WindowManager::~WindowManager() = default;

MessageElementFlags WindowManager::getWordFlags()
{
    return this->wordFlags_;
}

void WindowManager::updateWordTypeMask()
{
    using MEF = MessageElementFlag;
    auto *settings = getSettings();

    // text
    auto flags = MessageElementFlags(MEF::Text);

    // timestamp
    if (settings->showTimestamps)
    {
        flags.set(MEF::Timestamp);
    }

    // emotes
    if (settings->enableEmoteImages)
    {
        flags.set(MEF::EmoteImage);
    }
    flags.set(MEF::EmoteText);
    flags.set(MEF::EmojiText);

    // bits
    flags.set(MEF::BitsAmount);
    flags.set(settings->animateEmotes ? MEF::BitsAnimated : MEF::BitsStatic);

    // badges
    flags.set(MEF::BadgeSharedChannel);
    flags.set(settings->showBadgesGlobalAuthority ? MEF::BadgeGlobalAuthority
                                                  : MEF::None);
    flags.set(settings->showBadgesPredictions ? MEF::BadgePredictions
                                              : MEF::None);
    flags.set(settings->showBadgesChannelAuthority ? MEF::BadgeChannelAuthority
                                                   : MEF::None);
    flags.set(settings->showBadgesSubscription ? MEF::BadgeSubscription
                                               : MEF::None);
    flags.set(settings->showBadgesVanity ? MEF::BadgeVanity : MEF::None);
    flags.set(settings->showBadgesChatterino ? MEF::BadgeChatterino
                                             : MEF::None);
    flags.set(settings->showBadgesFfz ? MEF::BadgeFfz : MEF::None);
    flags.set(settings->showBadgesBttv ? MEF::BadgeBttv : MEF::None);
    flags.set(settings->showBadgesSevenTV ? MEF::BadgeSevenTV : MEF::None);
    flags.set(settings->showKickLevelBadges ? MEF::BadgeKickLevel : MEF::None);

    // username
    flags.set(MEF::Username);

    // replies
    flags.set(MEF::RepliedMessage);
    flags.set(settings->showReplyButton ? MEF::ReplyButton : MEF::None);

    // misc
    flags.set(MEF::AlwaysShow);
    flags.set(MEF::Collapsed);
    flags.set(MEF::LowercaseLinks, settings->lowercaseDomains);
    flags.set(MEF::ChannelPointReward);
    flags.set(
        MEF::PlatformBadge,
        mergedPlatformIndicatorShowsBadge(
            settings->mergedPlatformIndicatorMode));

    // update flags
    MessageElementFlags newFlags = static_cast<MessageElementFlags>(flags);

    if (newFlags != this->wordFlags_)
    {
        this->wordFlags_ = newFlags;

        this->wordFlagsChanged.invoke();
    }
}

void WindowManager::layoutChannelViews(Channel *channel)
{
    this->layoutRequested.invoke(channel);
}

void WindowManager::forceLayoutChannelViews()
{
    this->incGeneration();
    this->layoutChannelViews(nullptr);
}

void WindowManager::invalidateChannelViewBuffers(Channel *channel)
{
    this->invalidateBuffersRequested.invoke(channel);
}

void WindowManager::reloadChannelViews(Channel *channel)
{
    this->reloadMessagesRequested.invoke(channel);
}

void WindowManager::repaintVisibleChatWidgets(Channel *channel)
{
    this->layoutRequested.invoke(channel);
}

void WindowManager::repaintGifEmotes()
{
    this->gifRepaintRequested.invoke();
}

// void WindowManager::updateAll()
//{
//    if (this->mainWindow != nullptr) {
//        this->mainWindow->update();
//    }
//}

Window &WindowManager::getMainWindow()
{
    assertInGuiThread();

    return *this->mainWindow_;
}

Window *WindowManager::getLastSelectedWindow() const
{
    assertInGuiThread();
    if (this->selectedWindow_ == nullptr)
    {
        return this->mainWindow_;
    }

    return this->selectedWindow_;
}

Window &WindowManager::createWindow(WindowType type, bool show, QWidget *parent)
{
    assertInGuiThread();

    auto *const realParent = [this, type, parent]() -> QWidget * {
        (void)this;
        (void)type;

        if (parent)
        {
            // If a parent is explicitly specified, we use that immediately.
            return parent;
        }

        // FIXME: On Windows, parenting popup windows causes unwanted behavior (see
        //        https://github.com/Chatterino/chatterino2/issues/4179 for discussion). Ideally, we
        //        would use a different solution rather than relying on OS-specific code but this is
        //        the low-effort fix for now.
#ifndef Q_OS_WIN
        if (type == WindowType::Popup)
        {
            // On some window managers, popup windows require a parent to behave correctly. See
            // https://github.com/Chatterino/chatterino2/pull/1843 for additional context.
            return &(this->getMainWindow());
        }
#endif

        // If no parent is set and something other than a popup window is being created, we fall
        // back to the default behavior of no parent.
        return nullptr;
    }();

    auto *window = new Window(type, realParent);

    this->windows_.push_back(window);
    if (show)
    {
        window->show();
    }

    if (type != WindowType::Main)
    {
        window->setAttribute(Qt::WA_DeleteOnClose);

        QObject::connect(window, &QWidget::destroyed, this, [this, window] {
            for (auto it = this->windows_.begin(); it != this->windows_.end();
                 it++)
            {
                if (*it == window)
                {
                    this->windows_.erase(it);
                    break;
                }
            }
        });
    }

    return *window;
}

Window &WindowManager::openInPopup(ChannelPtr channel)
{
    auto &popup = this->createWindow(WindowType::Popup, true);
    auto *split =
        popup.getNotebook().getOrAddSelectedPage()->appendNewSplit(false);
    split->setChannel(channel);

    return popup;
}

void WindowManager::select(Split *split)
{
    this->selectSplit.invoke(split);
}

void WindowManager::select(SplitContainer *container)
{
    this->selectSplitContainer.invoke(container);
}

void WindowManager::scrollToMessage(const MessagePtr &message)
{
    this->scrollToMessage(message, false);
}

void WindowManager::scrollToMessage(const MessagePtr &message, bool startReply)
{
    this->scrollToMessageSignal.invoke(message, startReply);
}

QRect WindowManager::emotePopupBounds() const
{
    return this->emotePopupBounds_;
}

void WindowManager::setEmotePopupBounds(QRect bounds)
{
    if (this->emotePopupBounds_ != bounds)
    {
        this->emotePopupBounds_ = bounds;
        this->queueSave();
    }
}

void WindowManager::initialize()
{
    assertInGuiThread();
    layoutTrace(QStringLiteral("WindowManager::initialize start"));

    {
        WindowLayout windowLayout;

        if (this->appArgs.customChannelLayout)
        {
            windowLayout = this->appArgs.customChannelLayout.value();
        }
        else
        {
            layoutTrace(QStringLiteral("before loadWindowLayoutFromFile"));
            windowLayout = this->loadWindowLayoutFromFile();
            layoutTrace(QStringLiteral("after loadWindowLayoutFromFile"));
        }

        auto desired = this->appArgs.activateChannel;
        if (desired)
        {
            windowLayout.activateOrAddChannel(desired->provider, desired->name);
        }

        this->emotePopupBounds_ = windowLayout.emotePopupBounds_;

        layoutTrace(QStringLiteral("before applyWindowLayout"));
        this->applyWindowLayout(windowLayout);
        layoutTrace(QStringLiteral("after applyWindowLayout"));
    }

    if (this->appArgs.isFramelessEmbed)
    {
        this->framelessEmbedWindow_.reset(new FramelessEmbedWindow);
        this->framelessEmbedWindow_->show();
    }

    // No main window has been created from loading, create an empty one
    if (this->mainWindow_ == nullptr)
    {
        layoutTrace(QStringLiteral("creating fallback main window"));
        this->mainWindow_ = &this->createWindow(WindowType::Main);
        this->mainWindow_->getNotebook().addPage(true);

        // TODO: don't create main window if it's a frameless embed
        if (this->appArgs.isFramelessEmbed)
        {
            this->mainWindow_->hide();
        }
    }

    layoutTrace(QStringLiteral("WindowManager::initialize end"));
}

void WindowManager::save()
{
    if (this->appArgs.dontSaveSettings)
    {
        return;
    }

    if (this->shuttingDown_)
    {
        qCDebug(chatterinoWindowmanager) << "Skipping save (shutting down)";
        return;
    }

    qCDebug(chatterinoWindowmanager) << "Saving";
    assertInGuiThread();
    QJsonDocument document(this->currentWindowLayoutJson());
    // save file
    std::error_code ec;
    pajlada::Settings::Backup::saveWithBackup(
        qStringToStdPath(this->windowLayoutFilePath),
        {.enabled = true, .numSlots = 9},
        [&](const auto &path, auto &ec) {
            QSaveFile file(stdPathToQString(path));
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                ec = std::make_error_code(std::errc::io_error);
                return;
            }

            file.write(document.toJson(QJsonDocument::Indented));
            if (!file.commit() || file.error() != QFile::NoError)
            {
                ec = std::make_error_code(std::errc::io_error);
            }
        },
        ec);

    if (ec)
    {
        // TODO(Qt 6.5): drop fromStdString
        qCWarning(chatterinoWindowmanager)
            << "Failed to save windowlayout"
            << QString::fromStdString(ec.message());
    }
}

QJsonObject WindowManager::currentWindowLayoutJson() const
{
    assertInGuiThread();
    // "serialize"
    QJsonArray windowArr;
    for (Window *window : this->windows_)
    {
        QJsonObject windowObj;

        // window type
        switch (window->getType())
        {
            case WindowType::Main:
                windowObj.insert("type", "main");
                break;

            case WindowType::Popup:
                windowObj.insert("type", "popup");
                break;

            case WindowType::Attached:;
        }

        if (window->isMaximized())
        {
            windowObj.insert("state", "maximized");
        }
        else if (window->isMinimized())
        {
            windowObj.insert("state", "minimized");
        }

        // window geometry
        auto rect = window->getBounds();

        windowObj.insert("x", rect.x());
        windowObj.insert("y", rect.y());
        windowObj.insert("width", rect.width());
        windowObj.insert("height", rect.height());

        windowObj["emotePopup"] = QJsonObject{
            {"x", this->emotePopupBounds_.x()},
            {"y", this->emotePopupBounds_.y()},
            {"width", this->emotePopupBounds_.width()},
            {"height", this->emotePopupBounds_.height()},
        };

        auto &notebook = window->getNotebook();

        QJsonArray tabFoldersArr;
        for (const auto &folder : notebook.getTabFolderStates())
        {
            QJsonObject folderObj;
            folderObj.insert("id", folder.id);
            folderObj.insert("title", folder.title);
            folderObj.insert("expanded", folder.expanded);
            tabFoldersArr.append(folderObj);
        }
        if (!tabFoldersArr.isEmpty())
        {
            windowObj.insert("tabFolders", tabFoldersArr);
        }

        // window tabs
        QJsonArray tabsArr;

        for (int tabIndex = 0; tabIndex < notebook.getPageCount(); tabIndex++)
        {
            QJsonObject tabObj;
            SplitContainer *tab = dynamic_cast<SplitContainer *>(
                notebook.getPageAt(tabIndex));
            assert(tab != nullptr);

            bool isSelected = notebook.getSelectedPage() == tab;
            WindowManager::encodeTab(tab, isSelected, tabObj);
            const auto folderId = notebook.folderIdOfPage(tab);
            if (!folderId.isEmpty())
            {
                tabObj.insert("folderId", folderId);
            }
            tabsArr.append(tabObj);
        }

        windowObj.insert("tabs", tabsArr);
        windowArr.append(windowObj);
    }

    QJsonObject obj;
    obj.insert("windows", windowArr);
    return obj;
}

void WindowManager::sendAlert()
{
    int flashDuration = 2500;
    if (getSettings()->longAlerts)
    {
        flashDuration = 0;
    }
    QApplication::alert(this->getMainWindow().window(), flashDuration);
}

void WindowManager::queueSave()
{
    using namespace std::chrono_literals;

    this->saveTimer->start(10s);
}

void WindowManager::toggleAllOverlayInertia()
{
    // check if any window is not inert
    bool anyNonInert = false;
    for (auto *window : this->windows_)
    {
        if (anyNonInert)
        {
            break;
        }
        window->getNotebook().forEachSplit([&](auto *split) {
            auto *overlay = split->overlayWindow();
            if (overlay)
            {
                anyNonInert = anyNonInert || !overlay->isInert();
            }
        });
    }

    for (auto *window : this->windows_)
    {
        window->getNotebook().forEachSplit([&](auto *split) {
            auto *overlay = split->overlayWindow();
            if (overlay)
            {
                overlay->setInert(anyNonInert);
            }
        });
    }
}

std::set<QString> WindowManager::getVisibleChannelNames() const
{
    std::set<QString> visible;
    for (auto *window : this->windows_)
    {
        auto *page = window->getNotebook().getSelectedPage();
        if (!page)
        {
            continue;
        }

        for (auto *split : page->getSplits())
        {
            auto channel = split->getChannel();
            visible.emplace(channel->getName());

            if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
            {
                if (auto twitchChannel = merged->twitchChannel())
                {
                    visible.emplace(twitchChannel->getName());
                }
            }
        }
    }

    return visible;
}

void WindowManager::encodeTab(SplitContainer *tab, bool isSelected,
                              QJsonObject &obj)
{
    // custom tab title
    if (tab->getTab()->hasCustomTitle())
    {
        obj.insert("title", tab->getTab()->getCustomTitle());
    }

    // selected
    if (isSelected)
    {
        obj.insert("selected", true);
    }

    // highlighting on new messages
    obj.insert("highlightsEnabled", tab->getTab()->hasHighlightsEnabled());

    // splits
    QJsonObject splits;

    WindowManager::encodeNodeRecursively(tab->getBaseNode(), splits);

    obj.insert("splits2", splits);
}

void WindowManager::encodeNodeRecursively(SplitNode *node, QJsonObject &obj)
{
    switch (node->getType())
    {
        case SplitNode::Type::Split: {
            obj.insert("type", "split");
            obj.insert("moderationMode", node->getSplit()->getModerationMode());
            obj.insert("activityPane", node->getSplit()->isActivityPane());
            obj.insert("inputEnabled", node->getSplit()->inputEnabled());
            obj.insert("filterActivity", node->getSplit()->filterActivity());
            obj.insert("filterActivityExplicit",
                       node->getSplit()->filterActivityExplicit());
            obj.insert("activityMessageScale",
                       node->getSplit()->activityMessageScale());
            obj.insert("collapseGiftedSubscriptions",
                       node->getSplit()->collapseGiftedSubscriptions());
            obj.insert("activityTimeDisplayMode",
                       qmagicenum::enumNameString(
                           node->getSplit()->activityTimeDisplayMode())
                           .toLower());
            obj.insert("slowerChatEnabled",
                       node->getSplit()->slowerChatEnabled());
            obj.insert("slowerChatMessagesPerSecond",
                       node->getSplit()->slowerChatMessagesPerSecond());
            obj.insert("slowerChatMessageAnimations",
                       node->getSplit()->slowerChatMessageAnimations());
            obj.insert("streamDatabaseBadgeFeedVisible",
                       node->getSplit()->streamDatabaseBadgeFeedVisible());
            obj.insert("titleSettingsButtonVisible",
                       node->getSplit()->titleSettingsButtonVisible());
            obj.insert("chatModeIndicatorVisible",
                       node->getSplit()->chatModeIndicatorVisible());
            if (const auto viewerCountEnabled =
                    node->getSplit()->viewerCountEnabledOverride())
            {
                obj.insert("viewerCountEnabled", *viewerCountEnabled);
            }
            obj.insert("twitchActivityMinimumBits",
                       static_cast<qint64>(
                           node->getSplit()->twitchActivityMinimumBits()));
            obj.insert("kickActivityMinimumKicks",
                       static_cast<qint64>(
                           node->getSplit()->kickActivityMinimumKicks()));
            obj.insert("tiktokActivityMinimumDiamonds",
                       static_cast<qint64>(
                           node->getSplit()->tiktokActivityMinimumDiamonds()));
            obj.insert("platformIndicatorMode",
                       platformIndicatorModeName(
                           node->getSplit()->platformIndicatorMode()));
            const auto sendPlatformSelection =
                node->getSplit()->getInput().sendPlatformSelection();
            obj.insert("selectedSendPlatform",
                       messagePlatformName(
                           sendPlatformSelection.selectedPlatform));
            obj.insert("selectedSendAllPlatforms",
                       sendPlatformSelection.allPlatforms);
            obj.insert("customSelectedSendPlatforms",
                       messagePlatformArray(
                           sendPlatformSelection.customPlatforms));
            obj.insert("enabledSendPlatforms",
                       messagePlatformArray(
                           sendPlatformSelection.enabledPlatforms));

            QJsonObject split;
            WindowManager::encodeChannel(node->getSplit()->getIndirectChannel(),
                                         split);
            obj.insert("data", split);

            QJsonArray filters;
            WindowManager::encodeFilters(node->getSplit(), filters);
            obj.insert("filters", filters);

            auto spellOverride = node->getSplit()->checkSpellingOverride();
            if (spellOverride)
            {
                obj["checkSpelling"] = *spellOverride;
            }
        }
        break;
        case SplitNode::Type::HorizontalContainer:
        case SplitNode::Type::VerticalContainer: {
            obj.insert("type",
                       node->getType() == SplitNode::Type::HorizontalContainer
                           ? "horizontal"
                           : "vertical");

            QJsonArray itemsArr;
            for (const auto &n : node->getChildren())
            {
                QJsonObject subObj;
                WindowManager::encodeNodeRecursively(n.get(), subObj);
                itemsArr.append(subObj);
            }
            obj.insert("items", itemsArr);
        }
        break;

        default:
            break;
    }

    obj.insert("flexh", node->getHorizontalFlex());
    obj.insert("flexv", node->getVerticalFlex());
}

void WindowManager::encodeChannel(IndirectChannel channel, QJsonObject &obj)
{
    assertInGuiThread();

    switch (channel.getType())
    {
        case Channel::Type::Twitch: {
            obj.insert("type", "twitch");
            obj.insert("name", channel.get()->getName());
        }
        break;
        case Channel::Type::TwitchAutomod: {
            obj.insert("type", "automod");
        }
        break;
        case Channel::Type::TwitchMentions: {
            obj.insert("type", "mentions");
        }
        break;
        case Channel::Type::TwitchWatching: {
            obj.insert("type", "watching");
        }
        break;
        case Channel::Type::TwitchWhispers: {
            obj.insert("type", "whispers");
        }
        break;
        case Channel::Type::TwitchLive: {
            obj.insert("type", "live");
        }
        break;
        case Channel::Type::Misc: {
            obj.insert("type", "misc");
            obj.insert("name", channel.get()->getName());
        }
        break;
        case Channel::Type::Kick: {
            obj.insert("type", "kick");
            obj.insert("name", channel.get()->getName());
            auto *kc = dynamic_cast<KickChannel *>(channel.get().get());
            if (kc)
            {
                obj.insert("roomID", static_cast<qint64>(kc->roomID()));
                obj.insert("userID", static_cast<qint64>(kc->userID()));
                obj.insert("channelID", static_cast<qint64>(kc->channelID()));
            }
        }
        break;
        case Channel::Type::Merged: {
            auto *merged = dynamic_cast<MergedChannel *>(channel.get().get());
            if (!merged)
            {
                break;
            }

            const auto &config = merged->config();
            obj.insert("type", "merged");
            obj.insert("name", config.tabName);
            obj.insert("twitchEnabled", config.twitchEnabled);
            obj.insert("twitchChannel", config.twitchChannelName);
            obj.insert("kickEnabled", config.kickEnabled);
            obj.insert("kickChannel", config.kickChannelName);
            obj.insert("youtubeEnabled", config.youtubeEnabled);
            obj.insert("youtubeStreamUrl", config.youtubeStreamUrl);
            obj.insert("tiktokEnabled", config.tiktokEnabled);
            obj.insert("tiktokSource", config.tiktokSource);
            obj.insert("tiktokShowJoinMessages",
                       config.tiktokShowJoinMessages);
        }
        break;

        default:
            break;
    }
}

void WindowManager::encodeFilters(Split *split, QJsonArray &arr)
{
    assertInGuiThread();

    auto filters = split->getFilters();
    for (const auto &f : filters)
    {
        arr.append(f.toString(QUuid::WithoutBraces));
    }
}

IndirectChannel WindowManager::decodeChannel(const SplitDescriptor &descriptor)
{
    assertInGuiThread();

    if (descriptor.type_ == "twitch")
    {
        return getApp()->getTwitch()->getOrAddChannel(descriptor.channelName_);
    }
    else if (descriptor.type_ == "mentions")
    {
        return getApp()->getTwitch()->getMentionsChannel();
    }
    else if (descriptor.type_ == "watching")
    {
        return getApp()->getTwitch()->getWatchingChannel();
    }
    else if (descriptor.type_ == "whispers")
    {
        return getApp()->getTwitch()->getWhispersChannel();
    }
    else if (descriptor.type_ == "live")
    {
        return getApp()->getTwitch()->getLiveChannel();
    }
    else if (descriptor.type_ == "automod")
    {
        return getApp()->getTwitch()->getAutomodChannel();
    }
    else if (descriptor.type_ == "misc")
    {
        return getApp()->getTwitch()->getChannelOrEmpty(
            descriptor.channelName_);
    }
    else if (descriptor.type_ == "kick")
    {
        return getApp()->getKickChatServer()->getOrCreate(
            descriptor.channelName_, KickChannel::UserInit{
                                         .roomID = descriptor.kickRoomID,
                                         .userID = descriptor.kickUserID,
                                         .channelID = descriptor.kickChannelID,
                                     });
    }
    else if (descriptor.type_ == "merged")
    {
        return IndirectChannel(std::make_shared<MergedChannel>(MergedChannelConfig{
            .tabName = descriptor.channelName_,
            .twitchEnabled = descriptor.mergedTwitchEnabled,
            .twitchChannelName = descriptor.mergedTwitchChannelName_,
            .kickEnabled = descriptor.mergedKickEnabled,
            .kickChannelName = descriptor.mergedKickChannelName_,
            .youtubeEnabled = descriptor.mergedYoutubeEnabled,
            .youtubeStreamUrl = descriptor.mergedYoutubeStreamUrl_,
            .tiktokEnabled = descriptor.mergedTikTokEnabled,
            .tiktokSource = descriptor.mergedTikTokSource_,
            .tiktokShowJoinMessages =
                descriptor.mergedTikTokShowJoinMessages,
        }));
    }

    return Channel::getEmpty();
}

void WindowManager::closeAll()
{
    assertInGuiThread();

    qCDebug(chatterinoWindowmanager) << "Shutting down (closing windows)";
    this->shuttingDown_ = true;

    for (Window *window : this->windows_)
    {
        closeWindowsRecursive(window);
    }
}

int WindowManager::getGeneration() const
{
    return this->generation_;
}

void WindowManager::incGeneration()
{
    this->generation_++;
}

WindowLayout WindowManager::loadWindowLayoutFromFile() const
{
    return WindowLayout::loadFromFile(this->windowLayoutFilePath);
}

void WindowManager::applyWindowLayout(const WindowLayout &layout)
{
    layoutTrace(QStringLiteral("applyWindowLayout start windows=%1")
                    .arg(layout.windows_.size()));
    if (this->appArgs.dontLoadMainWindow)
    {
        layoutTrace(QStringLiteral("applyWindowLayout skipped dontLoadMainWindow"));
        return;
    }

    // Set emote popup position
    this->emotePopupBounds_ = layout.emotePopupBounds_;

    int windowIndex = 0;
    for (const auto &windowData : layout.windows_)
    {
        auto type = windowData.type_;

        layoutTrace(QStringLiteral("window %1 before create type=%2 tabs=%3")
                        .arg(windowIndex)
                        .arg(type == WindowType::Main ? QStringLiteral("main")
                                                      : QStringLiteral("popup"))
                        .arg(windowData.tabs_.size()));
        Window &window = this->createWindow(type, false);
        layoutTrace(QStringLiteral("window %1 after create").arg(windowIndex));

        if (type == WindowType::Main)
        {
            assert(this->mainWindow_ == nullptr);

            this->mainWindow_ = &window;
        }

        // get geometry
        {
            layoutTrace(QStringLiteral("window %1 before geometry").arg(windowIndex));
            // out of bounds windows
            auto screens = QApplication::screens();
            bool outOfBounds =
                !qEnvironmentVariableIsSet("I3SOCK") &&
                std::none_of(screens.begin(), screens.end(),
                             [&](QScreen *screen) {
                                 return screen->availableGeometry().intersects(
                                     windowData.geometry_);
                             });

            // ask if move into bounds
            auto &&should = shouldMoveOutOfBoundsWindow();
            if (outOfBounds && !should)
            {
                should =
                    QMessageBox(QMessageBox::Icon::Warning,
                                "Windows out of bounds",
                                "Some windows were detected out of bounds. "
                                "Should they be moved into bounds?",
                                QMessageBox::Yes | QMessageBox::No)
                        .exec() == QMessageBox::Yes;
            }

            if ((!outOfBounds || !should.value()) &&
                windowData.geometry_.x() != -1 &&
                windowData.geometry_.y() != -1 &&
                windowData.geometry_.width() != -1 &&
                windowData.geometry_.height() != -1)
            {
                // Have to offset x by one because qt moves the window 1px too
                // far to the left:w

                window.setInitialBounds(
                    {
                        windowData.geometry_.x(),
                        windowData.geometry_.y(),
                        windowData.geometry_.width(),
                        windowData.geometry_.height(),
                    },
                    widgets::BoundsChecking::Off);
            }
            layoutTrace(QStringLiteral("window %1 after geometry").arg(windowIndex));
        }

        for (const auto &folder : windowData.tabFolders_)
        {
            layoutTrace(QStringLiteral("window %1 before restore folder").arg(windowIndex));
            window.getNotebook().restoreTabFolder(folder.id_, folder.title_,
                                                  folder.expanded_);
            layoutTrace(QStringLiteral("window %1 after restore folder").arg(windowIndex));
        }

        // open tabs
        int tabIndex = 0;
        for (const auto &tab : windowData.tabs_)
        {
            layoutTrace(QStringLiteral("window %1 tab %2 before addPage selected=%3 hasRoot=%4")
                            .arg(windowIndex)
                            .arg(tabIndex)
                            .arg(tab.selected_)
                            .arg(tab.rootNode_.has_value()));
            SplitContainer *page = window.getNotebook().addPage(false);
            layoutTrace(QStringLiteral("window %1 tab %2 after addPage")
                            .arg(windowIndex)
                            .arg(tabIndex));

            // set custom title
            if (!tab.customTitle_.isEmpty())
            {
                page->getTab()->setCustomTitle(tab.customTitle_);
            }

            if (!tab.folderId_.isEmpty())
            {
                window.getNotebook().setPageFolder(page, tab.folderId_, false);
            }

            // selected
            if (tab.selected_)
            {
                layoutTrace(QStringLiteral("window %1 tab %2 before select")
                                .arg(windowIndex)
                                .arg(tabIndex));
                window.getNotebook().select(page);
                layoutTrace(QStringLiteral("window %1 tab %2 after select")
                                .arg(windowIndex)
                                .arg(tabIndex));
            }

            // highlighting on new messages
            page->getTab()->setHighlightsEnabled(tab.highlightsEnabled_);

            if (tab.rootNode_)
            {
                layoutTrace(QStringLiteral("window %1 tab %2 before applyFromDescriptor")
                                .arg(windowIndex)
                                .arg(tabIndex));
                page->applyFromDescriptor(*tab.rootNode_);
                layoutTrace(QStringLiteral("window %1 tab %2 after applyFromDescriptor")
                                .arg(windowIndex)
                                .arg(tabIndex));
            }
            ++tabIndex;
        }
        layoutTrace(QStringLiteral("window %1 before show").arg(windowIndex));
        window.show();
        layoutTrace(QStringLiteral("window %1 after show").arg(windowIndex));

        // Set window state
        layoutTrace(QStringLiteral("window %1 before state").arg(windowIndex));
        switch (windowData.state_)
        {
            case WindowDescriptor::State::Minimized: {
                window.setWindowState(Qt::WindowMinimized);
            }
            break;

            case WindowDescriptor::State::Maximized: {
                window.setWindowState(Qt::WindowMaximized);
            }
            break;

            case WindowDescriptor::State::None:
                break;
        }
        layoutTrace(QStringLiteral("window %1 after state").arg(windowIndex));
        ++windowIndex;
    }
    layoutTrace(QStringLiteral("applyWindowLayout end"));
}

}  // namespace chatterino
