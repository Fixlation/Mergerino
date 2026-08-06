// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>
#include <pajlada/signals/signal.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace chatterino {

struct SeventvOwnedCosmetic {
    QString id;
    QString name;
    QString kind;
    QJsonObject data;
};

struct SeventvManagedEmote {
    QString id;
    QString name;
    QJsonObject data;
};

struct SeventvEditorChannel {
    QString userID;
    QString platform;
    QString connectionID;
    QString username;
    QString displayName;
    QString emoteSetID;

    bool operator==(const SeventvEditorChannel &) const = default;
};

struct SeventvChannelCosmeticOverride {
    QString channelID;
    QString channelLogin;
    QString channelDisplayName;
    QString platform;
    QString paintID;
    QString badgeID;
};

struct SeventvCosmeticSelection {
    QString paintID;
    QString badgeID;
};

class SeventvAccountManager final
{
public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const QString &)>;
    using EmotesCallback =
        std::function<void(std::vector<SeventvManagedEmote>)>;

    static SeventvAccountManager &instance();
    static const QString &inheritValue();
    static const QString &twitchPlatform();
    static const QString &kickPlatform();
    static const QString &bothPlatforms();

    bool isLoggedIn() const;
    bool isBusy() const;
    QString userID() const;
    QString username() const;
    QString displayName() const;
    QString emoteSetID() const;
    QString defaultPaintID(const QString &platform) const;
    QString defaultBadgeID(const QString &platform) const;
    QString activePaintID() const;
    QString activeBadgeID() const;
    bool editorChannelsLoaded() const;

    const std::vector<SeventvOwnedCosmetic> &paints() const;
    const std::vector<SeventvOwnedCosmetic> &badges() const;
    const std::vector<SeventvManagedEmote> &emotes() const;
    const std::vector<SeventvManagedEmote> &searchResults() const;
    const std::vector<SeventvEditorChannel> &editorChannels() const;
    const std::vector<SeventvChannelCosmeticOverride> &channelOverrides() const;

    void beginSignIn();
    bool acceptSessionToken(const QString &token, QString *error = nullptr);
    void logout();
    void refresh();

    void setDefaults(const QString &platform, const QString &paintID,
                     const QString &badgeID);
    void setChannelOverride(const SeventvChannelCosmeticOverride &override);
    void changeChannelOverridePlatform(
        const SeventvChannelCosmeticOverride &override,
        const QString &platform);
    void removeChannelOverride(const QString &channelID,
                               const QString &platform);
    SeventvCosmeticSelection resolveSelection(
        const QString &channelID, const QString &channelLogin,
        const QString &platform) const;

    void applySelection(const SeventvCosmeticSelection &selection,
                        SuccessCallback onSuccess = {},
                        ErrorCallback onError = {});
    void ensureChannelCosmetics(const QString &platform,
                                const QString &channelID,
                                const QString &channelLogin,
                                const QString &channelDisplayName);

    void searchEmotes(const QString &query);
    void addEmote(const QString &emoteID, const QString &name = {});
    void addEmoteToEditorChannel(const SeventvEditorChannel &channel,
                                 const QString &emoteID, const QString &name,
                                 SuccessCallback onSuccess,
                                 ErrorCallback onError);
    void loadEditorChannelEmotes(const SeventvEditorChannel &channel,
                                 EmotesCallback onSuccess,
                                 ErrorCallback onError);
    void removeEmoteFromEditorChannel(const SeventvEditorChannel &channel,
                                      const QString &emoteID,
                                      SuccessCallback onSuccess,
                                      ErrorCallback onError);
    void renameEmoteInEditorChannel(const SeventvEditorChannel &channel,
                                    const QString &emoteID,
                                    const QString &name,
                                    SuccessCallback onSuccess,
                                    ErrorCallback onError);
    void removeEmote(const QString &emoteID);
    void renameEmote(const QString &emoteID, const QString &name);

    QJsonObject exportSyncState() const;
    bool importSyncState(const QJsonObject &state, QString *error = nullptr);

    pajlada::Signals::NoArgSignal stateChanged;
    pajlada::Signals::NoArgSignal inventoryChanged;
    pajlada::Signals::NoArgSignal searchResultsChanged;
    pajlada::Signals::Signal<bool> busyChanged;
    pajlada::Signals::Signal<const QString &, bool> feedback;

private:
    SeventvAccountManager();

    using JsonCallback = std::function<void(const QJsonObject &)>;

    void requestGraphQL(const QString &query, const QJsonObject &variables,
                        JsonCallback onSuccess, ErrorCallback onError);
    void requestGraphQLV4(const QString &query, const QJsonObject &variables,
                          JsonCallback onSuccess, ErrorCallback onError);
    void loadActor();
    void loadCosmetics(const QStringList &ids);
    void loadLegacyCosmetics(const QStringList &ids);
    void loadEmotes();
    void handleActorLoadFailure(const QString &error);
    void mutateCosmetic(const QString &kind, const QString &currentID,
                        const QString &desiredID, SuccessCallback onSuccess,
                        ErrorCallback onError);
    void changeEmote(const QString &action, const QString &emoteID,
                     const QString &name);
    void changeEmoteInEditorChannel(const SeventvEditorChannel &channel,
                                    const QString &action,
                                    const QString &emoteID,
                                    const QString &name,
                                    SuccessCallback onSuccess,
                                    ErrorCallback onError);
    void requestEmoteChange(const QString &emoteSetID, const QString &action,
                            const QString &emoteID, const QString &name,
                            SuccessCallback onSuccess, ErrorCallback onError);
    void setBusy(bool busy);
    void markChanged();
    void loadOverrides();
    void saveOverrides();
    void clearRuntimeState();
    void flushPendingChannelCosmetics();

    bool busy_ = false;
    std::uint64_t busyGeneration_ = 0;
    bool signInPending_ = false;
    bool activeStateKnown_ = false;
    bool editorChannelsLoaded_ = false;
    QString activePaintID_;
    QString activeBadgeID_;
    QString lastChannelApplyKey_;
    QString pendingChannelID_;
    QString pendingChannelLogin_;
    QString pendingChannelDisplayName_;
    QString pendingPlatform_;
    QString kickDefaultPaintID_;
    QString kickDefaultBadgeID_;
    QString authRequestID_;
    std::vector<SeventvOwnedCosmetic> paints_;
    std::vector<SeventvOwnedCosmetic> badges_;
    std::vector<SeventvManagedEmote> emotes_;
    std::vector<SeventvManagedEmote> searchResults_;
    std::vector<SeventvEditorChannel> editorChannels_;
    std::vector<SeventvChannelCosmeticOverride> channelOverrides_;
};

}  // namespace chatterino
