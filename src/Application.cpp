// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Application.hpp"

#include "common/Args.hpp"
#include "common/Channel.hpp"
#include "common/Modes.hpp"
#include "common/Version.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/ignores/IgnoreController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "controllers/sound/ISoundController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "providers/bttv/BttvBadges.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/eventsub/Controller.hpp"
#include "providers/twitch/TwitchBadges.hpp"
#include "singletons/ImageUploader.hpp"
#include "singletons/NativeMessaging.hpp"
#ifdef CHATTERINO_HAVE_PLUGINS
#    include "controllers/plugins/PluginController.hpp"
#endif
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/sound/MiniaudioBackend.hpp"
#include "controllers/sound/NullBackend.hpp"
#include "controllers/twitch/LiveController.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "debug/AssertInGuiThread.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/chatterino/ChatterinoBadges.hpp"
#include "providers/ffz/FfzBadges.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/PubSubMessages.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchUsers.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/helper/LoggingChannel.hpp"
#include "singletons/Logging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/Toasts.hpp"
#include "singletons/Updates.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/ChatterinoImport.hpp"
#ifdef MERGERINO_ENABLE_EXTENSION_BRIDGE
#include "util/MergerinoExtensionBridge.hpp"
#endif
#include "util/ObsBrowserDockServer.hpp"
#include "util/PostToThread.hpp"
#include "util/WidgetHelpers.hpp"
#include "widgets/dialogs/UpdateDialog.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/Window.hpp"

#include <miniaudio.h>
#ifdef USEWINSDK
#    include <Windows.h>
#endif
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <tuple>

namespace {

using namespace chatterino;

const QString BTTV_LIVE_UPDATES_URL = "wss://sockets.betterttv.net/ws";
const QString SEVENTV_EVENTAPI_URL = "wss://events.7tv.io/v3";

std::atomic<bool> STOPPED{false};
std::atomic<bool> ABOUT_TO_QUIT{false};

#ifdef USEWINSDK
void activateWindowAfterImport(Window &window)
{
    if (window.windowState().testFlag(Qt::WindowMinimized))
    {
        window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
    }

    window.show();
    window.raise();
    window.activateWindow();
    window.setFocus(Qt::ActiveWindowFocusReason);

    if (auto *handle = window.windowHandle())
    {
        handle->requestActivate();
    }

    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    if (hwnd != nullptr)
    {
        if (IsIconic(hwnd))
        {
            ShowWindow(hwnd, SW_RESTORE);
        }
        else
        {
            ShowWindow(hwnd, SW_SHOW);
        }
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
}

void appStartupTrace(const QString &message);

bool stageChatterinoImportAndRestart(QWidget *parent, bool autorun)
{
    const auto reply = QMessageBox::question(
        parent, "Import from Chatterino",
        "Import Chatterino settings, ping alerts, commands, user data, and "
        "accounts, and channel tabs into Mergerino?\n\nMergerino will "
        "restart. Current Mergerino settings files will be backed up before "
        "they are replaced.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply != QMessageBox::Yes)
    {
        return false;
    }

    auto *settings = getSettings();
    chatterino_import::ImportOptions options;
    options.startupPromptAcknowledged = true;
    options.autorun = autorun;
    options.currentVersion = CHATTERINO_VERSION;
    options.mergedPlatformIndicatorMode =
        settings->mergedPlatformIndicatorMode.getValue();
    options.platformEventHighlightStyle =
        settings->platformEventHighlightStyle.getValue();
    options.platformEventHighlightCustomColor =
        settings->platformEventHighlightCustomColor.getValue();
    options.platformAlertHighlightStyle =
        platformAlertHighlightStyleSetting().getValue();
    options.platformAlertHighlightCustomColor =
        platformAlertHighlightCustomColorSetting().getValue();

    auto stage =
        chatterino_import::stageImportFromDefaultSource(getApp()->getPaths(),
                                                        options);
    if (!stage)
    {
        QMessageBox::critical(parent, "Chatterino import failed",
                              "Mergerino could not stage the Chatterino "
                              "import:\n\n" +
                                  stage.error());
        return false;
    }

    settings->startupPromptAcknowledged = true;
    settings->autorun = autorun;
    settings->requestSave();

    if (!chatterino_import::restartApplication())
    {
        QMessageBox::critical(
            parent, "Restart failed",
            "Mergerino staged the Chatterino import, but could not restart "
            "itself. Close and reopen Mergerino to finish the import.");
        return false;
    }

    QApplication::quit();
    return true;
}

bool maybePromptForStartup(QWidget *parent)
{
    appStartupTrace(QStringLiteral("maybePromptForStartup start"));
    auto *settings = getSettings();
    if (settings->startupPromptAcknowledged)
    {
        appStartupTrace(QStringLiteral("maybePromptForStartup acknowledged"));
        return false;
    }

    if (settings->autorun)
    {
        appStartupTrace(QStringLiteral("maybePromptForStartup autorun"));
        settings->startupPromptAcknowledged = true;
        settings->requestSave();
        return false;
    }

    appStartupTrace(QStringLiteral("maybePromptForStartup before dialog"));
    QDialog dialog(parent);
    dialog.setWindowTitle("Mergerino quick setup");
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto *title = new QLabel("Set up Mergerino");
    auto font = title->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);

    auto *description = new QLabel(
        "Bring over your Chatterino setup now, or continue with a fresh "
        "Mergerino profile.");
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *startWithWindows = new QCheckBox("Start Mergerino with Windows");
    startWithWindows->setChecked(false);
    layout->addWidget(startWithWindows);

    bool restartStarted = false;
    if (chatterino_import::defaultSourceSettingsDirectoryExists())
    {
        appStartupTrace(QStringLiteral("maybePromptForStartup found Chatterino source"));
        auto *importBox = new QHBoxLayout;
        auto *importText = new QLabel(
            "Found Chatterino settings. Import settings, ping alerts, "
            "commands, user data, accounts, and channel tabs.");
        importText->setWordWrap(true);
        auto *importButton = new QPushButton("Import from Chatterino");
        importBox->addWidget(importText, 1);
        importBox->addWidget(importButton);
        layout->addLayout(importBox);

        QObject::connect(importButton, &QPushButton::clicked, &dialog, [&] {
            restartStarted = stageChatterinoImportAndRestart(
                &dialog, startWithWindows->isChecked());
            if (restartStarted)
            {
                dialog.accept();
            }
        });
    }

    auto *buttonBox = new QDialogButtonBox(Qt::Horizontal, &dialog);
    auto *continueButton =
        buttonBox->addButton("Continue", QDialogButtonBox::AcceptRole);
    buttonBox->addButton("Skip", QDialogButtonBox::RejectRole);
    continueButton->setDefault(true);
    layout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, [&] {
        settings->startupPromptAcknowledged = true;
        settings->autorun = startWithWindows->isChecked();
        settings->requestSave();
        dialog.accept();
    });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, [&] {
        settings->startupPromptAcknowledged = true;
        settings->requestSave();
        dialog.reject();
    });

    appStartupTrace(QStringLiteral("maybePromptForStartup before exec"));
    if (dialog.exec() == QDialog::Rejected && !restartStarted)
    {
        appStartupTrace(QStringLiteral("maybePromptForStartup rejected"));
        settings->startupPromptAcknowledged = true;
        settings->requestSave();
    }
    appStartupTrace(QStringLiteral("maybePromptForStartup after exec"));

    return restartStarted;
}
#endif

ISoundController *makeSoundController(Settings &settings)
{
    SoundBackend soundBackend = settings.soundBackend;
    switch (soundBackend)
    {
        case SoundBackend::Miniaudio: {
            return new MiniaudioBackend(settings.soundMiniaudioKeepEngineAlive);
        }
        break;

        case SoundBackend::Null: {
            return new NullBackend();
        }
        break;

        default: {
            return new MiniaudioBackend(settings.soundMiniaudioKeepEngineAlive);
        }
        break;
    }
}

BttvLiveUpdates *makeBttvLiveUpdates(Settings &settings)
{
    bool enabled =
        settings.enableBTTVLiveUpdates &&
        (settings.enableBTTVChannelEmotes || settings.showBadgesBttv);

    if (enabled)
    {
        return new BttvLiveUpdates(BTTV_LIVE_UPDATES_URL);
    }

    return nullptr;
}

SeventvEventAPI *makeSeventvEventAPI(Settings &settings)
{
    bool enabled = settings.enableSevenTVEventAPI;

    if (enabled)
    {
        return new SeventvEventAPI(SEVENTV_EVENTAPI_URL %
                                   "?app=Mergerino&version=" %
                                   Version::instance().version());
    }

    return nullptr;
}

eventsub::IController *makeEventSubController(Settings &settings)
{
    bool enabled = settings.enableExperimentalEventSub;

    if (enabled)
    {
        return new eventsub::Controller();
    }

    return new eventsub::DummyController();
}

const QString TWITCH_PUBSUB_URL = "wss://pubsub-edge.twitch.tv";

void appStartupTrace(const QString &message)
{
    QFile file(QStringLiteral("mergerino-layout-trace.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return;
    }

    QTextStream stream(&file);
    stream << message << '\n';
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
IApplication *INSTANCE = nullptr;

}  // namespace

namespace chatterino {

IApplication::IApplication()
{
    INSTANCE = this;
}

IApplication::~IApplication()
{
    INSTANCE = nullptr;
}

// this class is responsible for handling the workflow of Chatterino
// It will create the instances of the major classes, and connect their signals
// to each other

Application::Application(Settings &_settings, const Paths &paths,
                         const Args &_args, Updates &_updates)
    : paths_(paths)
    , args_(_args)
    , themes(new Theme(paths))
    , fonts(new Fonts(_settings))
    , logging(new Logging(_settings))
    , emotes(new EmoteController)
    , accounts(new AccountController)
    , eventSub(makeEventSubController(_settings))
    , hotkeys(new HotkeyController)
    , windows(new WindowManager(_args, paths, _settings, *this->themes,
                                *this->fonts))
    , toasts(new Toasts)
    , imageUploader(new ImageUploader)
    , seventvAPI(new SeventvAPI)
    , crashHandler(new CrashHandler(paths))

    , commands(new CommandController(paths))
    , notifications(new NotificationController)
    , highlights(new HighlightController(_settings, this->accounts.get()))
    , twitch(new TwitchIrcServer)
    , ffzBadges(new FfzBadges)
    , bttvBadges(new BttvBadges)
    , seventvBadges(new SeventvBadges)
    , seventvPaints(new SeventvPaints)
    , seventvPersonalEmotes(new SeventvPersonalEmotes)
    , userData(new UserDataController(paths))
    , sound(makeSoundController(_settings))
    , twitchLiveController(new TwitchLiveController)
    , twitchPubSub(new PubSub(TWITCH_PUBSUB_URL))
    , twitchBadges(new TwitchBadges)
    , chatterinoBadges(new ChatterinoBadges)
    , bttvEmotes(new BttvEmotes)
    , bttvLiveUpdates(makeBttvLiveUpdates(_settings))
    , ffzEmotes(new FfzEmotes)
    , seventvEmotes(new SeventvEmotes)
    , seventvEventAPI(makeSeventvEventAPI(_settings))
    , linkResolver(new LinkResolver)
    , streamerMode(new StreamerMode)
    , twitchUsers(new TwitchUsers)
    , spellChecker(new SpellChecker)
    , kickChatServer(new KickChatServer)
#ifdef MERGERINO_ENABLE_EXTENSION_BRIDGE
    , mergerinoExtensionBridge(new MergerinoExtensionBridge)
#endif
    , obsBrowserDockServer(new ObsBrowserDockServer)
#ifdef CHATTERINO_HAVE_PLUGINS
    , plugins(new PluginController(paths))
#endif
    , nmServer(new NativeMessagingServer())
    , updates(_updates)
{
}

Application::~Application()
{
    // we do this early to ensure getApp isn't used in any dtors
    INSTANCE = nullptr;
}

void Application::initialize(Settings &settings, const Paths &paths)
{
    assert(!this->initialized);
    appStartupTrace(QStringLiteral("Application::initialize start"));

    if (!this->args_.isFramelessEmbed)
    {
        const auto previousVersion = settings.currentVersion.getValue();
        const auto pendingPostUpdateVersion =
            settings.pendingPostUpdateVersion.getValue();
        const bool pendingPostUpdateForThisVersion =
            !pendingPostUpdateVersion.isEmpty() &&
            pendingPostUpdateVersion == CHATTERINO_VERSION &&
            previousVersion != CHATTERINO_VERSION;
        // Versions before pendingPostUpdateVersion existed only leave
        // currentVersion behind after the updater restarts.
        const bool legacyPostUpdateForThisVersion =
            pendingPostUpdateVersion.isEmpty() && !previousVersion.isEmpty() &&
            Updates::isDowngradeOf(previousVersion, CHATTERINO_VERSION);

        if (pendingPostUpdateForThisVersion)
        {
            this->previousVersionForPatchNotes_ =
                previousVersion.isEmpty() ? pendingPostUpdateVersion
                                          : previousVersion;
        }
        else if (legacyPostUpdateForThisVersion)
        {
            this->previousVersionForPatchNotes_ = previousVersion;
        }
        else if (!pendingPostUpdateVersion.isEmpty())
        {
            // Only an explicit updater handoff should show post-update notes.
            settings.pendingPostUpdateVersion = "";
        }
        settings.currentVersion.setValue(CHATTERINO_VERSION);
    }
    appStartupTrace(QStringLiteral("Application before emotes initialize"));
    this->emotes->initialize();
    appStartupTrace(QStringLiteral("Application after emotes initialize"));

    appStartupTrace(QStringLiteral("Application before accounts load"));
    this->accounts->load();
    appStartupTrace(QStringLiteral("Application after accounts load"));

    appStartupTrace(QStringLiteral("Application before windows initialize"));
    this->windows->initialize();
    appStartupTrace(QStringLiteral("Application after windows initialize"));

    appStartupTrace(QStringLiteral("Application before ffzBadges load"));
    this->ffzBadges->load();
    appStartupTrace(QStringLiteral("Application after ffzBadges load"));

    // Load global emotes
    appStartupTrace(QStringLiteral("Application before global emotes"));
    this->bttvEmotes->loadEmotes();
    this->ffzEmotes->loadEmotes();
    this->seventvEmotes->loadGlobalEmotes();
    appStartupTrace(QStringLiteral("Application after global emotes"));

    appStartupTrace(QStringLiteral("Application before chat servers initialize"));
    this->twitch->initialize();
    this->kickChatServer->initialize();
    appStartupTrace(QStringLiteral("Application after chat servers initialize"));

    // Load live status
    appStartupTrace(QStringLiteral("Application before notifications initialize"));
    this->notifications->initialize();
    appStartupTrace(QStringLiteral("Application after notifications initialize"));

    // XXX: Loading Twitch badges after Helix has been initialized, which only happens after
    // the AccountController initialize has been called
    appStartupTrace(QStringLiteral("Application before twitchBadges load"));
    this->twitchBadges->loadTwitchBadges();
    appStartupTrace(QStringLiteral("Application after twitchBadges load"));

#ifdef CHATTERINO_HAVE_PLUGINS
    appStartupTrace(QStringLiteral("Application before plugins initialize"));
    this->plugins->initialize(settings);
    appStartupTrace(QStringLiteral("Application after plugins initialize"));
#endif

    // Show crash message.
    // On Windows, the crash message was already shown.
#ifndef Q_OS_WIN
    if (!this->args_.isFramelessEmbed && this->args_.crashRecovery)
    {
        if (auto *selected =
                this->windows->getMainWindow().getNotebook().getSelectedPage())
        {
            if (auto *container = dynamic_cast<SplitContainer *>(selected))
            {
                for (auto &&split : container->getSplits())
                {
                    if (auto channel = split->getChannel(); !channel->isEmpty())
                    {
                        channel->addSystemMessage(
                            "Mergerino unexpectedly crashed and restarted. "
                            "You can disable automatic restarts in the "
                            "settings.");
                    }
                }
            }
        }
    }
#endif

    if (!this->args_.isFramelessEmbed)
    {
        appStartupTrace(QStringLiteral("Application before initNm"));
        this->initNm(paths);
        appStartupTrace(QStringLiteral("Application after initNm"));
    }

    appStartupTrace(QStringLiteral("Application before initEventAPIs"));
    this->twitch->initEventAPIs(this->bttvLiveUpdates.get(),
                                this->seventvEventAPI.get());
    appStartupTrace(QStringLiteral("Application after initEventAPIs"));

    appStartupTrace(QStringLiteral("Application before streamerMode start"));
    this->streamerMode->start();
    appStartupTrace(QStringLiteral("Application after streamerMode start"));

    this->initialized = true;
    appStartupTrace(QStringLiteral("Application::initialize end"));
}

int Application::run()
{
    assert(this->initialized);
    appStartupTrace(QStringLiteral("Application::run start"));

    if (!this->args_.isFramelessEmbed)
    {
        appStartupTrace(QStringLiteral("Application::run before mainWindow"));
        auto &mainWindow = this->windows->getMainWindow();
        appStartupTrace(QStringLiteral("Application::run after mainWindow"));
        appStartupTrace(QStringLiteral("Application::run before mainWindow.show"));
        mainWindow.show();
        appStartupTrace(QStringLiteral("Application::run after mainWindow.show"));

#ifdef USEWINSDK
        if (this->args_.activateAfterImport)
        {
            QTimer::singleShot(0, &mainWindow, [&mainWindow] {
                activateWindowAfterImport(mainWindow);
            });
            QTimer::singleShot(250, &mainWindow, [&mainWindow] {
                activateWindowAfterImport(mainWindow);
            });
        }
#endif

        const auto markPostUpdateShown = [] {
            auto *settings = getSettings();
            if (settings->pendingPostUpdateVersion.getValue() ==
                CHATTERINO_VERSION)
            {
                settings->pendingPostUpdateVersion = "";
            }
            settings->currentVersion.setValue(CHATTERINO_VERSION);
            settings->requestSave();
        };

        const auto showPostUpdateDialog = [&mainWindow, markPostUpdateShown] {
            markPostUpdateShown();

            auto *dialog =
                new PostUpdateDialog(CHATTERINO_DISPLAY_VERSION, &mainWindow);
            const auto position = mainWindow.mapToGlobal(QPoint{
                (mainWindow.width() - dialog->width()) / 2,
                48,
            });
            widgets::showAndMoveWindowTo(
                dialog, position, widgets::BoundsChecking::DesiredPosition);
            dialog->raise();
            dialog->activateWindow();
        };

        const auto showStartupDialogs = [this, &mainWindow,
                                         showPostUpdateDialog] {
            appStartupTrace(QStringLiteral("showStartupDialogs start"));
#ifdef USEWINSDK
            appStartupTrace(QStringLiteral("showStartupDialogs before maybePromptForStartup"));
            if (maybePromptForStartup(&mainWindow))
            {
                appStartupTrace(QStringLiteral("showStartupDialogs startup prompt restarted"));
                return;
            }
            appStartupTrace(QStringLiteral("showStartupDialogs after maybePromptForStartup"));
#endif

            const bool showPostUpdate =
                !this->previousVersionForPatchNotes_.isEmpty();
            appStartupTrace(
                QStringLiteral("showStartupDialogs postUpdate=%1")
                    .arg(showPostUpdate ? 1 : 0));

            if (showPostUpdate)
            {
                appStartupTrace(QStringLiteral("showStartupDialogs before post update dialog"));
                showPostUpdateDialog();
                appStartupTrace(QStringLiteral("showStartupDialogs after post update dialog"));
            }
            appStartupTrace(QStringLiteral("showStartupDialogs end"));
        };

        QTimer::singleShot(0, &mainWindow, showStartupDialogs);
        appStartupTrace(QStringLiteral("Application::run after startup dialog timer"));
        QTimer::singleShot(3000, &mainWindow, [] {
            getApp()->getUpdates().checkForUpdates();
        });
        appStartupTrace(QStringLiteral("Application::run after updates timer"));
    }

    appStartupTrace(QStringLiteral("Application::run before BTTV connect"));
    getSettings()->enableBTTVChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllBTTVChannelEmotes();
        },
        this->settingConnections_,
        false);
    appStartupTrace(QStringLiteral("Application::run after BTTV connect"));
    appStartupTrace(QStringLiteral("Application::run before FFZ connect"));
    getSettings()->enableFFZChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllFFZChannelEmotes();
        },
        this->settingConnections_,
        false);
    appStartupTrace(QStringLiteral("Application::run after FFZ connect"));
    appStartupTrace(QStringLiteral("Application::run before SevenTV connect"));
    getSettings()->enableSevenTVChannelEmotes.connect(
        [this] {
            this->twitch->reloadAllSevenTVChannelEmotes();
        },
        this->settingConnections_,
        false);
    appStartupTrace(QStringLiteral("Application::run after SevenTV connect"));

    appStartupTrace(QStringLiteral("Application::run before twitch connect timer"));
    QTimer::singleShot(0, [this] {
        appStartupTrace(QStringLiteral("Application::run twitch connect timer start"));
        this->twitch->connect();
        appStartupTrace(QStringLiteral("Application::run twitch connect timer end"));
    });
    appStartupTrace(QStringLiteral("Application::run after twitch connect timer"));

    appStartupTrace(QStringLiteral("Application::run before QApplication::exec"));
    return QApplication::exec();
}

Theme *Application::getThemes()
{
    assertInGuiThread();
    assert(this->themes);

    return this->themes.get();
}

Fonts *Application::getFonts()
{
    assertInGuiThread();
    assert(this->fonts);

    return this->fonts.get();
}

EmoteController *Application::getEmotes()
{
    assertInGuiThread();
    assert(this->emotes);

    return this->emotes.get();
}

AccountController *Application::getAccounts()
{
    assertInGuiThread();
    assert(this->accounts);

    return this->accounts.get();
}

HotkeyController *Application::getHotkeys()
{
    assertInGuiThread();
    assert(this->hotkeys);

    return this->hotkeys.get();
}

WindowManager *Application::getWindows()
{
    assertInGuiThread();
    assert(this->windows);

    return this->windows.get();
}

Toasts *Application::getToasts()
{
    assertInGuiThread();
    assert(this->toasts);

    return this->toasts.get();
}

CrashHandler *Application::getCrashHandler()
{
    assertInGuiThread();
    assert(this->crashHandler);

    return this->crashHandler.get();
}

CommandController *Application::getCommands()
{
    assertInGuiThread();
    assert(this->commands);

    return this->commands.get();
}

NotificationController *Application::getNotifications()
{
    assertInGuiThread();
    assert(this->notifications);

    return this->notifications.get();
}

HighlightController *Application::getHighlights()
{
    assertInGuiThread();
    assert(this->highlights);

    return this->highlights.get();
}

FfzBadges *Application::getFfzBadges()
{
    assertInGuiThread();
    assert(this->ffzBadges);

    return this->ffzBadges.get();
}

BttvBadges *Application::getBttvBadges()
{
    // BttvBadges handles its own locks, so we don't need to assert that this is called in the GUI thread
    assert(this->bttvBadges);

    return this->bttvBadges.get();
}

SeventvBadges *Application::getSeventvBadges()
{
    // SeventvBadges handles its own locks, so we don't need to assert that this is called in the GUI thread
    assert(this->seventvBadges);

    return this->seventvBadges.get();
}

IUserDataController *Application::getUserData()
{
    assertInGuiThread();

    return this->userData.get();
}

ISoundController *Application::getSound()
{
    assertInGuiThread();

    return this->sound.get();
}

ITwitchLiveController *Application::getTwitchLiveController()
{
    assertInGuiThread();
    assert(this->twitchLiveController);

    return this->twitchLiveController.get();
}

TwitchBadges *Application::getTwitchBadges()
{
    assertInGuiThread();
    assert(this->twitchBadges);

    return this->twitchBadges.get();
}

IChatterinoBadges *Application::getChatterinoBadges()
{
    assertInGuiThread();
    assert(this->chatterinoBadges);

    return this->chatterinoBadges.get();
}

ImageUploader *Application::getImageUploader()
{
    assertInGuiThread();
    assert(this->imageUploader);

    return this->imageUploader.get();
}

SeventvAPI *Application::getSeventvAPI()
{
    assertInGuiThread();
    assert(this->seventvAPI);

    return this->seventvAPI.get();
}

#ifdef CHATTERINO_HAVE_PLUGINS
PluginController *Application::getPlugins()
{
    assertInGuiThread();
    assert(this->plugins);

    return this->plugins.get();
}
#endif

Updates &Application::getUpdates()
{
    assertInGuiThread();

    return this->updates;
}

ITwitchIrcServer *Application::getTwitch()
{
    return this->twitch.get();
}

PubSub *Application::getTwitchPubSub()
{
    assertInGuiThread();

    return this->twitchPubSub.get();
}

ILogging *Application::getChatLogger()
{
    assertInGuiThread();
    assert(this->logging);

    return this->logging.get();
}

ILinkResolver *Application::getLinkResolver()
{
    assertInGuiThread();

    return this->linkResolver.get();
}

IStreamerMode *Application::getStreamerMode()
{
    return this->streamerMode.get();
}

ITwitchUsers *Application::getTwitchUsers()
{
    assertInGuiThread();
    assert(this->twitchUsers);

    return this->twitchUsers.get();
}

BttvEmotes *Application::getBttvEmotes()
{
    assertInGuiThread();
    assert(this->bttvEmotes);

    return this->bttvEmotes.get();
}

BttvLiveUpdates *Application::getBttvLiveUpdates()
{
    assertInGuiThread();
    // bttvLiveUpdates may be nullptr if it's not enabled

    return this->bttvLiveUpdates.get();
}

FfzEmotes *Application::getFfzEmotes()
{
    assertInGuiThread();
    assert(this->ffzEmotes);

    return this->ffzEmotes.get();
}

SeventvEmotes *Application::getSeventvEmotes()
{
    assertInGuiThread();
    assert(this->seventvEmotes);

    return this->seventvEmotes.get();
}

SeventvPersonalEmotes *Application::getSeventvPersonalEmotes()
{
    assert(this->seventvPersonalEmotes);

    return this->seventvPersonalEmotes.get();
}

SeventvPaints *Application::getSeventvPaints()
{
    assert(this->seventvPaints);

    return this->seventvPaints.get();
}

SeventvEventAPI *Application::getSeventvEventAPI()
{
    assertInGuiThread();
    // seventvEventAPI may be nullptr if it's not enabled

    return this->seventvEventAPI.get();
}

eventsub::IController *Application::getEventSub()
{
    assert(this->eventSub);

    return this->eventSub.get();
}

SpellChecker *Application::getSpellChecker()
{
    assertInGuiThread();
    assert(this->spellChecker);

    return this->spellChecker.get();
}

KickChatServer *Application::getKickChatServer()
{
    assertInGuiThread();
    assert(this->kickChatServer);

    return this->kickChatServer.get();
}

void Application::aboutToQuit()
{
    ABOUT_TO_QUIT.store(true);

    this->eventSub->setQuitting();

    this->twitch->aboutToQuit();

    this->hotkeys->save();
    this->windows->save();

    this->windows->closeAll();
}

void Application::stop()
{
#ifdef CHATTERINO_HAVE_PLUGINS
    this->plugins.reset();
#endif
    this->twitchUsers.reset();
    this->streamerMode.reset();
    this->linkResolver.reset();
    this->seventvEventAPI.reset();
    this->seventvEmotes.reset();
    this->ffzEmotes.reset();
    this->bttvLiveUpdates.reset();
    this->bttvEmotes.reset();
    this->chatterinoBadges.reset();
    this->twitchBadges.reset();
    this->twitchPubSub.reset();
    this->twitchLiveController.reset();
    this->sound.reset();
    this->userData.reset();
    this->seventvBadges.reset();
    this->ffzBadges.reset();
    this->twitch.reset();
    this->highlights.reset();
    this->notifications.reset();
    this->commands.reset();
    this->crashHandler.reset();
    this->seventvAPI.reset();
    this->imageUploader.reset();
    this->toasts.reset();
#ifdef MERGERINO_ENABLE_EXTENSION_BRIDGE
    this->mergerinoExtensionBridge.reset();
#endif
    this->windows.reset();
    this->hotkeys.reset();
    this->eventSub.reset();
    this->accounts.reset();
    this->emotes.reset();
    this->logging.reset();
    this->fonts.reset();
    this->themes.reset();
    this->spellChecker.reset();

    STOPPED.store(true);
}

void Application::initNm(const Paths &paths)
{
    (void)paths;

#if defined QT_NO_DEBUG || defined CHATTERINO_DEBUG_NM
    registerNmHost(paths);
    this->nmServer->start();
#endif
}

IApplication *getApp()
{
    assert(INSTANCE != nullptr);
    assert(STOPPED.load() == false);

    return INSTANCE;
}

IApplication *tryGetApp()
{
    return INSTANCE;
}

bool isAppAboutToQuit()
{
    return ABOUT_TO_QUIT.load();
}

}  // namespace chatterino
