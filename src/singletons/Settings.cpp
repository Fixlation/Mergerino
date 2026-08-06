// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Settings.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/highlights/HighlightBadge.hpp"
#include "controllers/highlights/HighlightBlacklistUser.hpp"
#include "controllers/highlights/HighlightPhrase.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "controllers/moderationactions/ModerationAction.hpp"
#include "controllers/nicknames/Nickname.hpp"
#include "debug/Benchmark.hpp"
#include "pajlada/settings/signalargs.hpp"
#include "util/Backup.hpp"
#include "util/WindowsHelper.hpp"

#include <pajlada/signals/scoped-connection.hpp>

#include <string_view>

namespace {

using namespace chatterino;
using namespace Qt::Literals;

template <typename T>
void initializeSignalVector(pajlada::Signals::SignalHolder &signalHolder,
                            ChatterinoSetting<std::vector<T>> &setting,
                            SignalVector<T> &vec)
{
    // Fill the SignalVector up with initial values
    for (auto &&item : setting.getValue())
    {
        vec.append(item);
    }

    // Set up a signal to
    signalHolder.managedConnect(vec.delayedItemsChanged, [&] {
        setting.setValue(vec.raw());
    });
}

void migrateTimeoutButtons(Settings &settings)
{
    auto timeouts = settings.timeoutButtons.getValue();
    bool changed = false;

    for (auto &timeout : timeouts)
    {
        if (timeout.first == "m" && timeout.second == 5)
        {
            timeout.second = 10;
            changed = true;
        }
    }

    if (changed)
    {
        settings.timeoutButtons.setValue(timeouts);
    }
}

}  // namespace

namespace chatterino {

std::vector<std::weak_ptr<pajlada::Settings::SettingData>> _settings;

void _actuallyRegisterSetting(
    std::weak_ptr<pajlada::Settings::SettingData> setting)
{
    _settings.push_back(std::move(setting));
}

bool Settings::isHighlightedUser(const QString &username)
{
    auto items = this->highlightedUsers.readOnly();

    for (const auto &highlightedUser : *items)
    {
        if (highlightedUser.isMatch(username))
        {
            return true;
        }
    }

    return false;
}

bool Settings::isBlacklistedUser(const QString &username)
{
    auto items = this->blacklistedUsers.readOnly();

    for (const auto &blacklistedUser : *items)
    {
        if (blacklistedUser.isMatch(username))
        {
            return true;
        }
    }

    return false;
}

bool Settings::isMutedChannel(const QString &channelName)
{
    auto items = this->mutedChannels.readOnly();

    for (const auto &channel : *items)
    {
        if (channelName.toLower() == channel.toLower())
        {
            return true;
        }
    }
    return false;
}

std::optional<QString> Settings::matchNickname(const QString &usernameText)
{
    auto nicknames = this->nicknames.readOnly();

    for (const auto &nickname : *nicknames)
    {
        if (auto nicknameText = nickname.match(usernameText))
        {
            return nicknameText;
        }
    }

    return std::nullopt;
}

void Settings::mute(const QString &channelName)
{
    if (!this->isMutedChannel(channelName))
    {
        this->mutedChannels.append(channelName);
    }
}

void Settings::unmute(const QString &channelName)
{
    for (std::vector<int>::size_type i = 0;
         i != this->mutedChannels.raw().size(); i++)
    {
        if (this->mutedChannels.raw()[i].toLower() == channelName.toLower())
        {
            this->mutedChannels.removeAt(i);
            i--;
        }
    }
}

bool Settings::toggleMutedChannel(const QString &channelName)
{
    if (this->isMutedChannel(channelName))
    {
        this->unmute(channelName);
        return false;
    }
    else
    {
        this->mutedChannels.append(channelName);
        return true;
    }
}

Settings *Settings::instance_ = nullptr;

EnumStringSetting<PlatformEventHighlightStyle> &
platformAlertHighlightStyleSetting()
{
    static auto *setting =
        new EnumStringSetting<PlatformEventHighlightStyle>{
            "/appearance/messages/platformAlertHighlightStyle",
            PlatformEventHighlightStyle::Gradient,
        };
    return *setting;
}

QStringSetting &platformAlertHighlightCustomColorSetting()
{
    static auto *setting = new QStringSetting{
        "/appearance/messages/platformAlertHighlightCustomColor",
        "#5a9146ff",
    };
    return *setting;
}

namespace {

BoolSetting &platformHighlightStylesSplitMigratedSetting()
{
    static auto *setting = new BoolSetting{
        "/appearance/messages/platformHighlightStylesSplitMigrated", false};
    return *setting;
}

}  // namespace

EnumStringSetting<SplitHeaderViewerCountMode> &headerViewerCountModeSetting()
{
    static auto *setting = new EnumStringSetting<SplitHeaderViewerCountMode>{
        "/appearance/splitheader/viewerCountMode",
        SplitHeaderViewerCountMode::Total,
    };
    return *setting;
}

bool isExternallyCommittedSevenTVSetting(std::string_view path)
{
    return path == "/accounts/seventv/token" ||
           path == "/accounts/seventv/userId" ||
           path == "/accounts/seventv/username" ||
           path == "/accounts/seventv/displayName" ||
           path == "/accounts/seventv/emoteSetId" ||
           path == "/accounts/seventv/defaultPaintId" ||
           path == "/accounts/seventv/defaultBadgeId" ||
           path == "/accounts/seventv/channelCosmetics" ||
           path == "/accounts/seventv/updatedAt";
}

EnumStringSetting<EmoteBarMode> &emoteBarModeSetting()
{
    static auto *setting = new EnumStringSetting<EmoteBarMode>{
        "/emotes/bar/mode",
        EmoteBarMode::Combined,
    };
    return *setting;
}

EnumStringSetting<EmoteBarScope> &emoteBarScopeSetting()
{
    static auto *setting = new EnumStringSetting<EmoteBarScope>{
        "/emotes/bar/scope",
        EmoteBarScope::SevenTV,
    };
    return *setting;
}

IntSetting &emoteBarMaxEmotesSetting()
{
    static auto *setting =
        new IntSetting{"/emotes/bar/maxEmotes", 6};
    return *setting;
}

QStringSetting &emoteBarHistoryJsonSetting()
{
    static auto *setting = new QStringSetting{
        "/emotes/bar/history",
        R"({"version":1,"channels":[]})",
    };
    return *setting;
}

BoolSetting &emoteBarIntroductionDismissedSetting()
{
    static auto *setting =
        new BoolSetting{"/emotes/bar/introductionDismissed", false};
    return *setting;
}

BoolSetting &disablePinnedMessagesSetting()
{
    static auto *setting =
        new BoolSetting{"/appearance/messages/disablePinnedMessages", false};
    return *setting;
}

BoolSetting &showSeventvChatButtonSetting()
{
    static auto *setting =
        new BoolSetting{"/appearance/chatInput/showSeventvButton", true};
    return *setting;
}

BoolSetting &showPredictionChatButtonSetting()
{
    static auto *setting =
        new BoolSetting{"/appearance/chatInput/showPredictionButton", true};
    return *setting;
}

BoolSetting &showPollChatButtonSetting()
{
    static auto *setting =
        new BoolSetting{"/appearance/chatInput/showPollButton", true};
    return *setting;
}

BoolSetting &showGiveawayChatButtonSetting()
{
    static auto *setting =
        new BoolSetting{"/appearance/chatInput/showGiveawayButton", true};
    return *setting;
}

BoolSetting &obsOverlayMessageAnimationsSetting()
{
    static auto *setting =
        new BoolSetting{"/obs/overlay/messageAnimations", true};
    return *setting;
}

EnumStringSetting<LinkPreviewMode> &linkPreviewModeSetting()
{
    static auto *setting = new EnumStringSetting<LinkPreviewMode>{
        "/links/linkPreviewMode",
        LinkPreviewMode::Disabled,
    };
    return *setting;
}

EnumStringSetting<SeventvAddEmoteTargetScope> &
seventvAddEmoteTargetScopeSetting()
{
    static auto *setting =
        new EnumStringSetting<SeventvAddEmoteTargetScope>{
            "/accounts/seventv/addEmoteTargetScope",
            SeventvAddEmoteTargetScope::OpenTabs,
        };
    return *setting;
}

Settings::Settings(const Args &args, const QString &settingsDirectory,
                   bool isTest)
    : prevInstance_(Settings::instance_)
    , disableSaving(args.dontSaveSettings)
{
    QString settingsPath = settingsDirectory + "/settings.json";
    (void)headerViewerCountModeSetting();
    (void)platformAlertHighlightStyleSetting();
    (void)platformAlertHighlightCustomColorSetting();
    (void)platformHighlightStylesSplitMigratedSetting();
    (void)emoteBarModeSetting();
    (void)emoteBarScopeSetting();
    (void)emoteBarMaxEmotesSetting();
    (void)emoteBarHistoryJsonSetting();
    (void)emoteBarIntroductionDismissedSetting();
    (void)disablePinnedMessagesSetting();
    (void)showSeventvChatButtonSetting();
    (void)showPredictionChatButtonSetting();
    (void)showPollChatButtonSetting();
    (void)showGiveawayChatButtonSetting();
    (void)obsOverlayMessageAnimationsSetting();
    (void)linkPreviewModeSetting();
    (void)seventvAddEmoteTargetScopeSetting();

    // get global instance of the settings library
    auto settingsInstance = pajlada::Settings::SettingManager::getInstance();

    if (isTest)
    {
        settingsInstance->load(qPrintable(settingsPath));
    }
    else
    {
        backup::loadWithBackups(
            backup::FileData{
                .fileName = u"settings.json"_s,
                .directory = settingsDirectory,
                .fileKind = u"Settings"_s,
                .fileDescription =
                    u"This file contains the main application settings such as accounts and hotkeys."_s,
            },
            [&]() -> ExpectedStr<void> {
                using LoadError = pajlada::Settings::SettingManager::LoadError;
                auto err = settingsInstance->load(qPrintable(settingsPath));
                switch (err)
                {
                    case LoadError::NoError:
                        return {};  // ok
                    case LoadError::CannotOpenFile:
                        return makeUnexpected(u"Failed to open '" %
                                              settingsPath % '\'');
                    case LoadError::FileHandleError:
                        return makeUnexpected("File handle error");
                    case LoadError::FileReadError:
                        return makeUnexpected("Failed to read file");
                    case LoadError::FileSeekError:
                        return makeUnexpected("Failed to seek in file");
                    case LoadError::JSONParseError:
                        return makeUnexpected("File contained malformed JSON");
                }
                assert(false);
                return makeUnexpected("Unknown error");
            });
    }

    if (isTest)
    {
        this->mergedPlatformIndicatorMode = "badge";
    }

    if (!platformHighlightStylesSplitMigratedSetting().getValue())
    {
        platformAlertHighlightStyleSetting() =
            this->platformEventHighlightStyle.getValue();
        platformAlertHighlightCustomColorSetting() =
            this->platformEventHighlightCustomColor.getValue();
        platformHighlightStylesSplitMigratedSetting() = true;
    }

    settingsInstance->setBackupEnabled(true);
    settingsInstance->setBackupSlots(9);
    settingsInstance->saveMethod = static_cast<
        pajlada::Settings::SettingManager::SaveMethod>(
        static_cast<uint64_t>(
            pajlada::Settings::SettingManager::SaveMethod::SaveManually) |
        static_cast<uint64_t>(
            pajlada::Settings::SettingManager::SaveMethod::OnlySaveIfChanged));

    migrateTimeoutButtons(*this);

    initializeSignalVector(this->signalHolder, this->highlightedMessagesSetting,
                           this->highlightedMessages);
    initializeSignalVector(this->signalHolder, this->highlightedUsersSetting,
                           this->highlightedUsers);
    initializeSignalVector(this->signalHolder, this->highlightedBadgesSetting,
                           this->highlightedBadges);
    initializeSignalVector(this->signalHolder, this->blacklistedUsersSetting,
                           this->blacklistedUsers);
    initializeSignalVector(this->signalHolder, this->ignoredMessagesSetting,
                           this->ignoredMessages);
    initializeSignalVector(this->signalHolder, this->mutedChannelsSetting,
                           this->mutedChannels);
    initializeSignalVector(this->signalHolder, this->filterRecordsSetting,
                           this->filterRecords);
    initializeSignalVector(this->signalHolder, this->nicknamesSetting,
                           this->nicknames);
    initializeSignalVector(this->signalHolder, this->moderationActionsSetting,
                           this->moderationActions);
    initializeSignalVector(this->signalHolder, this->loggedChannelsSetting,
                           this->loggedChannels);
    initializeSignalVector(this->signalHolder, this->loggedUsersSetting,
                           this->loggedUsers);

    instance_ = this;

#ifdef USEWINSDK
    this->autorun = isRegisteredForStartup();
    this->autorun.connect(
        [](bool autorun) {
            setRegisteredForStartup(autorun);
        },
        false);
#endif

    // migration for `/emotes/showUnlistedEmotes` -> `/emotes/showUnlistedSevenTVEmotes`
    if (this->showUnlistedEmotesDontUse && !this->showUnlistedSevenTVEmotes)
    {
        this->showUnlistedSevenTVEmotes.setValue(true);
        // reset to default, so it doesn't appear in the config
        this->showUnlistedEmotesDontUse.remove();
    }
}

Settings::~Settings()
{
    Settings::instance_ = this->prevInstance_;
}

pajlada::Settings::SettingManager::SaveResult Settings::requestSave() const
{
    if (this->disableSaving)
    {
        return pajlada::Settings::SettingManager::SaveResult::Skipped;
    }

    return pajlada::Settings::SettingManager::gSave();
}

void Settings::saveSnapshot()
{
    BenchmarkGuard benchmark("Settings::saveSnapshot");

    rapidjson::Document *d = new rapidjson::Document(rapidjson::kObjectType);
    rapidjson::Document::AllocatorType &a = d->GetAllocator();

    for (const auto &weakSetting : _settings)
    {
        auto setting = weakSetting.lock();
        if (!setting)
        {
            continue;
        }

        rapidjson::Value key(setting->getPath().c_str(), a);
        auto *curVal = setting->unmarshalJSON();
        if (curVal == nullptr)
        {
            continue;
        }

        rapidjson::Value val;
        val.CopyFrom(*curVal, a);
        d->AddMember(key.Move(), val.Move(), a);
    }

    // log("Snapshot state: {}", rj::stringify(*d));

    this->snapshot_.reset(d);
}

void Settings::restoreSnapshot()
{
    if (!this->snapshot_)
    {
        return;
    }

    BenchmarkGuard benchmark("Settings::restoreSnapshot");

    const auto &snapshot = *(this->snapshot_.get());

    if (!snapshot.IsObject())
    {
        return;
    }

    // The 7TV account dialog commits independently of the parent Settings
    // dialog. Preserve its live state across a Settings cancellation even if
    // snapshot iteration or registration changes cause the path filter below
    // to miss one of these settings.
    const auto sevenTVAccountToken = this->sevenTVAccountToken.getValue();
    const auto sevenTVAccountUserID = this->sevenTVAccountUserID.getValue();
    const auto sevenTVAccountUsername =
        this->sevenTVAccountUsername.getValue();
    const auto sevenTVAccountDisplayName =
        this->sevenTVAccountDisplayName.getValue();
    const auto sevenTVAccountEmoteSetID =
        this->sevenTVAccountEmoteSetID.getValue();
    const auto sevenTVDefaultPaintID = this->sevenTVDefaultPaintID.getValue();
    const auto sevenTVDefaultBadgeID = this->sevenTVDefaultBadgeID.getValue();
    const auto sevenTVChannelCosmeticsJson =
        this->sevenTVChannelCosmeticsJson.getValue();
    const auto sevenTVAccountUpdatedAt =
        this->sevenTVAccountUpdatedAt.getValue();

    for (const auto &weakSetting : _settings)
    {
        auto setting = weakSetting.lock();
        if (!setting)
        {
            continue;
        }

        const char *path = setting->getPath().c_str();

        // The 7TV account dialog commits these values immediately and lives
        // outside the parent Settings dialog. Cancelling Settings must not
        // roll a completed sign-in, sign-out, or cosmetic change back to the
        // snapshot captured before the account dialog was opened.
        if (isExternallyCommittedSevenTVSetting(path))
        {
            continue;
        }

        if (!snapshot.HasMember(path))
        {
            continue;
        }

        pajlada::Settings::SignalArgs args;
        args.compareBeforeSet = true;

        setting->marshalJSON(snapshot[path], std::move(args));
    }

    this->sevenTVAccountToken = sevenTVAccountToken;
    this->sevenTVAccountUserID = sevenTVAccountUserID;
    this->sevenTVAccountUsername = sevenTVAccountUsername;
    this->sevenTVAccountDisplayName = sevenTVAccountDisplayName;
    this->sevenTVAccountEmoteSetID = sevenTVAccountEmoteSetID;
    this->sevenTVDefaultPaintID = sevenTVDefaultPaintID;
    this->sevenTVDefaultBadgeID = sevenTVDefaultBadgeID;
    this->sevenTVChannelCosmeticsJson = sevenTVChannelCosmeticsJson;
    this->sevenTVAccountUpdatedAt = sevenTVAccountUpdatedAt;
    std::ignore = this->requestSave();
}

void Settings::disableSave()
{
    this->disableSaving = true;
}

bool Settings::shouldSendHelixChat() const
{
    switch (this->chatSendProtocol.getEnum())
    {
        case ChatSendProtocol::Helix:
            return true;
        case ChatSendProtocol::Default:
        case ChatSendProtocol::IRC:
            return false;
        default:
            assert(false && "Invalid chat protocol value");
            return false;
    }
}

float Settings::getClampedUiScale() const
{
    return std::clamp(this->uiScale.getValue(), 0.2F, 10.F);
}

void Settings::setClampedUiScale(float value)
{
    this->uiScale.setValue(std::clamp(value, 0.2F, 10.F));
}

float Settings::getClampedOverlayScale() const
{
    return std::clamp(this->overlayScaleFactor.getValue(), 0.2F, 10.F);
}

void Settings::setClampedOverlayScale(float value)
{
    this->overlayScaleFactor.setValue(std::clamp(value, 0.2F, 10.F));
}

Settings &Settings::instance()
{
    assert(instance_ != nullptr);

    return *instance_;
}

Settings *getSettings()
{
    return &Settings::instance();
}

}  // namespace chatterino
