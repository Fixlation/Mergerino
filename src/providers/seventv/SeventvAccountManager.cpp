// SPDX-License-Identifier: MIT

#include "providers/seventv/SeventvAccountManager.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvBrowserAuth.hpp"
#include "singletons/Settings.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace {

using namespace chatterino;

const QString GQL_URL = QStringLiteral("https://7tv.io/v3/gql");
const QString GQL_V4_URL = QStringLiteral("https://7tv.io/v4/gql");
constexpr qint64 SEVENTV_EDITOR_MODIFY_EMOTES = 1LL << 0;
constexpr auto SEVENTV_BUSY_TIMEOUT_MS = 30000;

const QString COSMETIC_INVENTORY_V4_QUERY = QStringLiteral(R"gql(
query MergerinoCosmeticInventoryV4 {
  users {
    me {
      inventory {
        paints {
          to {
            paint {
              id
              name
              data {
                layers {
                  id
                  opacity
                  ty {
                    __typename
                    ... on PaintLayerTypeSingleColor {
                      color { hex }
                    }
                    ... on PaintLayerTypeLinearGradient {
                      angle
                      repeating
                      stops { at color { hex } }
                    }
                    ... on PaintLayerTypeRadialGradient {
                      repeating
                      shape
                      stops { at color { hex } }
                    }
                    ... on PaintLayerTypeImage {
                      images {
                        url
                        mime
                        scale
                        width
                        height
                        frameCount
                      }
                    }
                  }
                }
                shadows {
                  color { hex }
                  offsetX
                  offsetY
                  blur
                }
              }
            }
          }
        }
        badges {
          to {
            badge {
              id
              name
              images {
                url
                mime
                scale
                width
                height
                frameCount
              }
            }
          }
        }
      }
    }
  }
}
)gql");

const QString ACTOR_QUERY = QStringLiteral(R"gql(
query MergerinoActor {
  actor {
    id username display_name
    style { paint_id badge_id }
    cosmetics { id kind selected }
    connections { platform id username display_name emote_set_id }
    editor_of {
      id permissions
      user {
        id username display_name
        connections { platform id username display_name emote_set_id }
      }
    }
  }
}
)gql");

const QString UPDATE_COSMETIC_MUTATION = QStringLiteral(R"gql(
mutation MergerinoUpdateCosmetic($id: ObjectID!, $update: UserCosmeticUpdate!) {
  user(id: $id) { cosmetics(update: $update) }
}
)gql");

const QString CHANGE_EMOTE_MUTATION = QStringLiteral(R"gql(
mutation MergerinoChangeEmote($id: ObjectID!, $action: ListItemAction!, $emote_id: ObjectID!, $name: String) {
  emoteSet(id: $id) {
    id
    emotes(id: $emote_id, action: $action, name: $name) { id name }
  }
}
)gql");

const QString SEARCH_EMOTES_QUERY = QStringLiteral(R"gql(
query MergerinoSearchEmotes($query: String!, $page: Int, $limit: Int) {
  emotes(query: $query, page: $page, limit: $limit,
         sort: { value: "POPULARITY", order: DESCENDING }) {
    items {
      id name flags
      host { url files { name format width height } }
      owner { id username display_name }
    }
  }
}
)gql");

bool validateJwt(const QString &token, QString *error)
{
    const auto parts = token.trimmed().split('.');
    if (parts.size() != 3)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("7TV returned an invalid session.");
        }
        return false;
    }

    auto encoded = parts.at(1).toLatin1();
    while (encoded.size() % 4 != 0)
    {
        encoded.append('=');
    }
    const auto payload = QJsonDocument::fromJson(
        QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding));
    const auto root = payload.object();
    const auto expiresAt =
        root.value(QStringLiteral("exp")).toVariant().toLongLong();
    if (root.isEmpty() ||
        root.value(QStringLiteral("iss")).toString() !=
            QStringLiteral("7tv.io") ||
        expiresAt <= QDateTime::currentSecsSinceEpoch())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("The 7TV session is invalid or expired.");
        }
        return false;
    }
    if (root.value(QStringLiteral("sub")).toString().trimmed().isEmpty())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("The 7TV session has no account identity.");
        }
        return false;
    }
    return true;
}

QString graphQLError(const QJsonObject &root)
{
    QStringList messages;
    for (const auto &value : root.value(QStringLiteral("errors")).toArray())
    {
        const auto message =
            value.toObject().value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
        {
            messages.append(message);
        }
    }
    return messages.join(QStringLiteral("; "));
}

SeventvOwnedCosmetic parseCosmetic(const QJsonObject &object,
                                   const QString &kind)
{
    auto name = object.value(QStringLiteral("name"))
                    .toString()
                    .normalized(QString::NormalizationForm_C)
                    .trimmed();
    qsizetype visibleStart = 0;
    while (visibleStart < name.size())
    {
        const auto character = name.at(visibleStart);
        const auto category = character.category();
        const bool invisible =
            character.isSpace() || category == QChar::Other_Control ||
            category == QChar::Other_Format ||
            category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing ||
            category == QChar::Separator_Space ||
            category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph;
        if (!invisible)
        {
            break;
        }
        ++visibleStart;
    }
    name = name.sliced(visibleStart).trimmed();

    return {
        .id = object.value(QStringLiteral("id")).toString(),
        .name = std::move(name),
        .kind = kind,
        .data = object,
    };
}

void sortCosmetics(std::vector<SeventvOwnedCosmetic> &cosmetics)
{
    std::ranges::sort(cosmetics, [](const auto &left, const auto &right) {
        const auto byName =
            left.name.compare(right.name, Qt::CaseInsensitive);
        return byName == 0 ? left.id < right.id : byName < 0;
    });
}

}  // namespace

namespace chatterino {

SeventvAccountManager &SeventvAccountManager::instance()
{
    static SeventvAccountManager manager;
    return manager;
}

const QString &SeventvAccountManager::inheritValue()
{
    static const QString value = QStringLiteral("__TVERINO_DEFAULT__");
    return value;
}

const QString &SeventvAccountManager::twitchPlatform()
{
    static const QString value = QStringLiteral("TWITCH");
    return value;
}

const QString &SeventvAccountManager::kickPlatform()
{
    static const QString value = QStringLiteral("KICK");
    return value;
}

const QString &SeventvAccountManager::bothPlatforms()
{
    static const QString value = QStringLiteral("BOTH");
    return value;
}

namespace {

QString normalizedCosmeticPlatform(QString platform)
{
    platform = platform.trimmed().toUpper();
    if (platform == SeventvAccountManager::kickPlatform() ||
        platform == SeventvAccountManager::bothPlatforms())
    {
        return platform;
    }
    return SeventvAccountManager::twitchPlatform();
}

bool cosmeticChannelMatches(const SeventvChannelCosmeticOverride &entry,
                            const QString &channelID,
                            const QString &channelLogin)
{
    const auto normalizedLogin = channelLogin.trimmed();
    if (!normalizedLogin.isEmpty() && !entry.channelLogin.trimmed().isEmpty() &&
        entry.channelLogin.compare(normalizedLogin, Qt::CaseInsensitive) == 0)
    {
        return true;
    }
    return !channelID.trimmed().isEmpty() && entry.channelID == channelID;
}

}  // namespace

SeventvAccountManager::SeventvAccountManager()
{
    this->loadOverrides();
}

bool SeventvAccountManager::isLoggedIn() const
{
    return !getSettings()->sevenTVAccountToken.getValue().trimmed().isEmpty();
}

bool SeventvAccountManager::isBusy() const
{
    return this->busy_;
}

QString SeventvAccountManager::userID() const
{
    return getSettings()->sevenTVAccountUserID.getValue();
}

QString SeventvAccountManager::username() const
{
    return getSettings()->sevenTVAccountUsername.getValue();
}

QString SeventvAccountManager::displayName() const
{
    const auto display =
        getSettings()->sevenTVAccountDisplayName.getValue().trimmed();
    return display.isEmpty() ? this->username() : display;
}

QString SeventvAccountManager::emoteSetID() const
{
    return getSettings()->sevenTVAccountEmoteSetID.getValue();
}

QString SeventvAccountManager::defaultPaintID(const QString &platform) const
{
    if (normalizedCosmeticPlatform(platform) == kickPlatform())
    {
        return this->kickDefaultPaintID_;
    }
    return getSettings()->sevenTVDefaultPaintID.getValue();
}

QString SeventvAccountManager::defaultBadgeID(const QString &platform) const
{
    if (normalizedCosmeticPlatform(platform) == kickPlatform())
    {
        return this->kickDefaultBadgeID_;
    }
    return getSettings()->sevenTVDefaultBadgeID.getValue();
}

QString SeventvAccountManager::activePaintID() const
{
    return this->activePaintID_;
}

QString SeventvAccountManager::activeBadgeID() const
{
    return this->activeBadgeID_;
}

bool SeventvAccountManager::editorChannelsLoaded() const
{
    return this->editorChannelsLoaded_;
}

const std::vector<SeventvOwnedCosmetic> &SeventvAccountManager::paints() const
{
    return this->paints_;
}

const std::vector<SeventvOwnedCosmetic> &SeventvAccountManager::badges() const
{
    return this->badges_;
}

const std::vector<SeventvManagedEmote> &SeventvAccountManager::emotes() const
{
    return this->emotes_;
}

const std::vector<SeventvManagedEmote> &
SeventvAccountManager::searchResults() const
{
    return this->searchResults_;
}

const std::vector<SeventvEditorChannel> &
SeventvAccountManager::editorChannels() const
{
    return this->editorChannels_;
}

const std::vector<SeventvChannelCosmeticOverride> &
    SeventvAccountManager::channelOverrides() const
{
    return this->channelOverrides_;
}

void SeventvAccountManager::beginSignIn()
{
    this->feedback.invoke(
        QStringLiteral("Connecting to 7TV through your default browser…"),
        false);
    SeventvBrowserAuth::instance().start(
        [this](const QString &token) {
            QString error;
            if (!this->acceptSessionToken(token, &error))
                this->feedback.invoke(error, true);
        },
        [this](const QString &error) {
            this->feedback.invoke(error, true);
            this->stateChanged.invoke();
        });
    this->stateChanged.invoke();
}

bool SeventvAccountManager::acceptSessionToken(const QString &token,
                                               QString *error)
{
    if (this->busy_)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("7TV is currently refreshing. Try again.");
        }
        return false;
    }
    const auto normalized = token.trimmed();
    if (!validateJwt(normalized, error))
    {
        return false;
    }
    if (getSettings()->sevenTVAccountToken.getValue() != normalized)
    {
        this->clearRuntimeState();
    }
    this->signInPending_ = true;
    this->authRequestID_.clear();
    getSettings()->sevenTVAccountToken = normalized;
    this->markChanged();
    this->stateChanged.invoke();
    this->refresh();
    return true;
}

void SeventvAccountManager::logout()
{
    if (this->busy_)
    {
        this->feedback.invoke(
            QStringLiteral("Wait for the current 7TV action to finish."), true);
        return;
    }
    this->signInPending_ = false;
    getSettings()->sevenTVAccountToken = QString{};
    getSettings()->sevenTVAccountUserID = QString{};
    getSettings()->sevenTVAccountUsername = QString{};
    getSettings()->sevenTVAccountDisplayName = QString{};
    getSettings()->sevenTVAccountEmoteSetID = QString{};
    this->authRequestID_.clear();
    this->clearRuntimeState();
    this->markChanged();
    this->stateChanged.invoke();
    this->inventoryChanged.invoke();
}

void SeventvAccountManager::refresh()
{
    if (!this->isLoggedIn())
    {
        this->feedback.invoke(QStringLiteral("Sign in to 7TV first."), true);
        return;
    }
    if (this->busy_)
    {
        return;
    }
    this->setBusy(true);
    this->loadActor();
}

void SeventvAccountManager::setDefaults(const QString &platform,
                                        const QString &paintID,
                                        const QString &badgeID)
{
    const auto normalizedPlatform = normalizedCosmeticPlatform(platform);
    if (normalizedPlatform != kickPlatform())
    {
        getSettings()->sevenTVDefaultPaintID = paintID.trimmed();
        getSettings()->sevenTVDefaultBadgeID = badgeID.trimmed();
    }
    if (normalizedPlatform != twitchPlatform())
    {
        this->kickDefaultPaintID_ = paintID.trimmed();
        this->kickDefaultBadgeID_ = badgeID.trimmed();
    }
    this->lastChannelApplyKey_.clear();
    this->saveOverrides();
    this->markChanged();
    this->stateChanged.invoke();
}

void SeventvAccountManager::setChannelOverride(
    const SeventvChannelCosmeticOverride &override)
{
    auto normalized = override;
    normalized.channelID = normalized.channelID.trimmed();
    normalized.channelLogin = normalized.channelLogin.trimmed().toLower();
    normalized.platform = normalizedCosmeticPlatform(normalized.platform);
    if (normalized.channelID.isEmpty() && normalized.channelLogin.isEmpty())
    {
        return;
    }
    const auto it = std::ranges::find_if(
        this->channelOverrides_, [&](const auto &entry) {
            return normalizedCosmeticPlatform(entry.platform) ==
                       normalized.platform &&
                   cosmeticChannelMatches(entry, normalized.channelID,
                                          normalized.channelLogin);
        });
    if (it == this->channelOverrides_.end())
    {
        this->channelOverrides_.push_back(normalized);
    }
    else
    {
        if (it->channelID == normalized.channelID &&
            it->channelLogin == normalized.channelLogin &&
            it->channelDisplayName == normalized.channelDisplayName &&
            it->platform == normalized.platform &&
            it->paintID == normalized.paintID &&
            it->badgeID == normalized.badgeID)
        {
            return;
        }
        *it = normalized;
    }
    this->lastChannelApplyKey_.clear();
    this->saveOverrides();
    this->markChanged();
    this->stateChanged.invoke();
}

void SeventvAccountManager::changeChannelOverridePlatform(
    const SeventvChannelCosmeticOverride &override, const QString &platform)
{
    const auto sourcePlatform = normalizedCosmeticPlatform(override.platform);
    const auto targetPlatform = normalizedCosmeticPlatform(platform);
    if (sourcePlatform == targetPlatform)
    {
        return;
    }

    const auto source = std::ranges::find_if(
        this->channelOverrides_, [&](const auto &entry) {
            return normalizedCosmeticPlatform(entry.platform) ==
                       sourcePlatform &&
                   cosmeticChannelMatches(entry, override.channelID,
                                          override.channelLogin);
        });
    if (source == this->channelOverrides_.end())
    {
        return;
    }

    auto moved = *source;
    moved.platform = targetPlatform;
    std::erase_if(this->channelOverrides_, [&](const auto &entry) {
        const auto entryPlatform = normalizedCosmeticPlatform(entry.platform);
        return (entryPlatform == sourcePlatform ||
                entryPlatform == targetPlatform) &&
               cosmeticChannelMatches(entry, override.channelID,
                                      override.channelLogin);
    });
    this->channelOverrides_.push_back(std::move(moved));
    this->lastChannelApplyKey_.clear();
    this->saveOverrides();
    this->markChanged();
    this->stateChanged.invoke();
}

void SeventvAccountManager::removeChannelOverride(const QString &channelID,
                                                  const QString &platform)
{
    const auto normalizedPlatform = normalizedCosmeticPlatform(platform);
    const auto before = this->channelOverrides_.size();
    std::erase_if(this->channelOverrides_, [&](const auto &entry) {
        return entry.channelID == channelID &&
               normalizedCosmeticPlatform(entry.platform) ==
                   normalizedPlatform;
    });
    if (before == this->channelOverrides_.size())
    {
        return;
    }
    this->lastChannelApplyKey_.clear();
    this->saveOverrides();
    this->markChanged();
    this->stateChanged.invoke();
}

SeventvCosmeticSelection SeventvAccountManager::resolveSelection(
    const QString &channelID, const QString &channelLogin,
    const QString &platform) const
{
    const auto normalizedPlatform = normalizedCosmeticPlatform(platform);
    const auto defaultPlatform = normalizedPlatform == bothPlatforms()
                                     ? twitchPlatform()
                                     : normalizedPlatform;
    SeventvCosmeticSelection selection{
        .paintID = this->defaultPaintID(defaultPlatform),
        .badgeID = this->defaultBadgeID(defaultPlatform),
    };
    auto findForPlatform = [&](const QString &wantedPlatform) {
        return std::ranges::find_if(
            this->channelOverrides_, [&](const auto &entry) {
                return normalizedCosmeticPlatform(entry.platform) ==
                           wantedPlatform &&
                       cosmeticChannelMatches(entry, channelID, channelLogin);
            });
    };
    auto it = findForPlatform(normalizedPlatform);
    if (it == this->channelOverrides_.end() &&
        normalizedPlatform != bothPlatforms())
    {
        it = findForPlatform(bothPlatforms());
    }
    if (it == this->channelOverrides_.end())
    {
        return selection;
    }
    selection.paintID =
        it->paintID == inheritValue() ? selection.paintID : it->paintID;
    selection.badgeID =
        it->badgeID == inheritValue() ? selection.badgeID : it->badgeID;
    return selection;
}

void SeventvAccountManager::applySelection(
    const SeventvCosmeticSelection &selection, SuccessCallback onSuccess,
    ErrorCallback onError)
{
    if (!this->isLoggedIn() || this->userID().isEmpty())
    {
        if (onError)
        {
            onError(QStringLiteral("7TV account is not ready."));
        }
        return;
    }

    this->setBusy(true);
    this->mutateCosmetic(
        QStringLiteral("PAINT"), this->activePaintID_, selection.paintID,
        [this, selection, onSuccess, onError] {
            this->activePaintID_ = selection.paintID;
            this->mutateCosmetic(
                QStringLiteral("BADGE"), this->activeBadgeID_,
                selection.badgeID,
                [this, selection, onSuccess] {
                    this->activeBadgeID_ = selection.badgeID;
                    this->setBusy(false);
                    this->stateChanged.invoke();
                    if (onSuccess)
                    {
                        onSuccess();
                    }
                },
                [this, onError](const QString &error) {
                    this->setBusy(false);
                    if (onError)
                    {
                        onError(error);
                    }
                    this->feedback.invoke(error, true);
                });
        },
        [this, onError](const QString &error) {
            this->setBusy(false);
            if (onError)
            {
                onError(error);
            }
            this->feedback.invoke(error, true);
        });
}

void SeventvAccountManager::ensureChannelCosmetics(
    const QString &platform, const QString &channelID,
    const QString &channelLogin,
    const QString &channelDisplayName)
{
    if ((channelID.isEmpty() && channelLogin.trimmed().isEmpty()) ||
        !this->isLoggedIn())
    {
        return;
    }
    const auto normalizedPlatform = normalizedCosmeticPlatform(platform);
    if (!this->activeStateKnown_)
    {
        this->pendingPlatform_ = normalizedPlatform;
        this->pendingChannelID_ = channelID;
        this->pendingChannelLogin_ = channelLogin;
        this->pendingChannelDisplayName_ = channelDisplayName;
        if (!this->busy_)
        {
            this->refresh();
        }
        return;
    }
    if (this->busy_)
    {
        return;
    }
    const auto selection =
        this->resolveSelection(channelID, channelLogin, normalizedPlatform);
    const auto key = QStringLiteral("%1:%2:%3:%4")
                         .arg(normalizedPlatform, channelID,
                              selection.paintID, selection.badgeID);
    if (key == this->lastChannelApplyKey_ &&
        this->activePaintID_ == selection.paintID &&
        this->activeBadgeID_ == selection.badgeID)
    {
        return;
    }
    this->lastChannelApplyKey_ = key;
    this->applySelection(
        selection,
        [this, normalizedPlatform, channelID, channelLogin,
         channelDisplayName] {
            if (normalizedPlatform != kickPlatform())
            {
                if (auto *app = tryGetApp())
                {
                    app->getSeventvAPI()->updatePresence(
                        channelID, this->userID(), [] {}, [](const auto &) {});
                }
            }
            this->feedback.invoke(
                QStringLiteral("7TV cosmetics set for #%1 on %2.")
                    .arg(channelDisplayName.isEmpty() ? channelLogin
                                                      : channelDisplayName,
                         normalizedPlatform == bothPlatforms()
                             ? QStringLiteral("Twitch and Kick")
                             : (normalizedPlatform == kickPlatform()
                                    ? QStringLiteral("Kick")
                                    : QStringLiteral("Twitch"))),
                false);
        },
        [this](const QString &) {
            this->lastChannelApplyKey_.clear();
        });
}

void SeventvAccountManager::searchEmotes(const QString &query)
{
    const auto normalized = query.trimmed();
    if (normalized.size() < 2)
    {
        this->searchResults_.clear();
        this->searchResultsChanged.invoke();
        return;
    }

    this->requestGraphQL(
        SEARCH_EMOTES_QUERY,
        QJsonObject{
            {QStringLiteral("query"), normalized},
            {QStringLiteral("page"), 1},
            {QStringLiteral("limit"), 30},
        },
        [this](const QJsonObject &data) {
            this->searchResults_.clear();
            const auto items = data.value(QStringLiteral("emotes"))
                                   .toObject()
                                   .value(QStringLiteral("items"))
                                   .toArray();
            for (const auto &value : items)
            {
                const auto object = value.toObject();
                this->searchResults_.push_back({
                    .id = object.value(QStringLiteral("id")).toString(),
                    .name = object.value(QStringLiteral("name")).toString(),
                    .data = object,
                });
            }
            this->searchResultsChanged.invoke();
        },
        [this](const QString &error) {
            this->feedback.invoke(error, true);
        });
}

void SeventvAccountManager::addEmote(const QString &emoteID,
                                     const QString &name)
{
    this->changeEmote(QStringLiteral("ADD"), emoteID, name);
}

void SeventvAccountManager::addEmoteToEditorChannel(
    const SeventvEditorChannel &channel, const QString &emoteID,
    const QString &name, SuccessCallback onSuccess, ErrorCallback onError)
{
    this->changeEmoteInEditorChannel(
        channel, QStringLiteral("ADD"), emoteID, name, std::move(onSuccess),
        std::move(onError));
}

void SeventvAccountManager::changeEmoteInEditorChannel(
    const SeventvEditorChannel &channel, const QString &action,
    const QString &emoteID, const QString &name, SuccessCallback onSuccess,
    ErrorCallback onError)
{
    if (this->busy_)
    {
        if (onError)
        {
            onError(
                QStringLiteral("Wait for the current 7TV action to finish."));
        }
        return;
    }

    const auto current = std::ranges::find(this->editorChannels_, channel);
    if (!this->editorChannelsLoaded_ || current == this->editorChannels_.end())
    {
        if (onError)
        {
            onError(QStringLiteral(
                "That channel is no longer in your editable 7TV channels. "
                "Refresh your 7TV account and try again."));
        }
        return;
    }

    const auto normalizedName = name.trimmed();
    const bool requiresName = action != QStringLiteral("REMOVE");
    if (emoteID.trimmed().isEmpty() ||
        (requiresName && normalizedName.isEmpty()))
    {
        if (onError)
        {
            onError(requiresName
                        ? QStringLiteral("The 7TV emote and name are required.")
                        : QStringLiteral("The 7TV emote is required."));
        }
        return;
    }

    this->setBusy(true);
    this->requestEmoteChange(
        current->emoteSetID, action, emoteID, normalizedName,
        [this, onSuccess = std::move(onSuccess)] {
            this->setBusy(false);
            if (onSuccess)
            {
                onSuccess();
            }
        },
        [this, onError = std::move(onError)](const QString &error) {
            this->setBusy(false);
            this->feedback.invoke(error, true);
            if (onError)
            {
                onError(error);
            }
        });
}

void SeventvAccountManager::loadEditorChannelEmotes(
    const SeventvEditorChannel &channel, EmotesCallback onSuccess,
    ErrorCallback onError)
{
    const auto current = std::ranges::find(this->editorChannels_, channel);
    if (!this->editorChannelsLoaded_ ||
        current == this->editorChannels_.end())
    {
        if (onError)
        {
            onError(QStringLiteral(
                "That channel is no longer in your editable 7TV channels. "
                "Refresh your 7TV account and try again."));
        }
        return;
    }

    getApp()->getSeventvAPI()->getEmoteSet(
        current->emoteSetID,
        [onSuccess = std::move(onSuccess)](const QJsonObject &set) {
            std::vector<SeventvManagedEmote> emotes;
            for (const auto &value :
                 set.value(QStringLiteral("emotes")).toArray())
            {
                const auto object = value.toObject();
                auto data = object.value(QStringLiteral("data")).toObject();
                if (data.isEmpty())
                {
                    data = object;
                }
                emotes.push_back({
                    .id = object.value(QStringLiteral("id")).toString(),
                    .name = object.value(QStringLiteral("name")).toString(),
                    .data = std::move(data),
                });
            }
            std::ranges::sort(
                emotes, [](const auto &left, const auto &right) {
                    return left.name.compare(right.name,
                                             Qt::CaseInsensitive) < 0;
                });
            if (onSuccess)
            {
                onSuccess(std::move(emotes));
            }
        },
        [onError = std::move(onError)](const NetworkResult &result) {
            if (onError)
            {
                onError(QStringLiteral("Unable to load the 7TV emote set: %1")
                            .arg(result.formatError()));
            }
        });
}

void SeventvAccountManager::removeEmoteFromEditorChannel(
    const SeventvEditorChannel &channel, const QString &emoteID,
    SuccessCallback onSuccess, ErrorCallback onError)
{
    this->changeEmoteInEditorChannel(
        channel, QStringLiteral("REMOVE"), emoteID, {}, std::move(onSuccess),
        std::move(onError));
}

void SeventvAccountManager::renameEmoteInEditorChannel(
    const SeventvEditorChannel &channel, const QString &emoteID,
    const QString &name, SuccessCallback onSuccess, ErrorCallback onError)
{
    this->changeEmoteInEditorChannel(
        channel, QStringLiteral("UPDATE"), emoteID, name,
        std::move(onSuccess), std::move(onError));
}

void SeventvAccountManager::removeEmote(const QString &emoteID)
{
    this->changeEmote(QStringLiteral("REMOVE"), emoteID, {});
}

void SeventvAccountManager::renameEmote(const QString &emoteID,
                                        const QString &name)
{
    this->changeEmote(QStringLiteral("UPDATE"), emoteID, name);
}

QJsonObject SeventvAccountManager::exportSyncState() const
{
    QJsonArray overrides;
    for (const auto &entry : this->channelOverrides_)
    {
        overrides.append(QJsonObject{
            {QStringLiteral("channelID"), entry.channelID},
            {QStringLiteral("channelLogin"), entry.channelLogin},
            {QStringLiteral("channelDisplayName"), entry.channelDisplayName},
            {QStringLiteral("platform"),
             normalizedCosmeticPlatform(entry.platform)},
            {QStringLiteral("paintID"), entry.paintID},
            {QStringLiteral("badgeID"), entry.badgeID},
        });
    }
    return QJsonObject{
        {QStringLiteral("token"),
         getSettings()->sevenTVAccountToken.getValue()},
        {QStringLiteral("userID"), this->userID()},
        {QStringLiteral("username"), this->username()},
        {QStringLiteral("displayName"), this->displayName()},
        {QStringLiteral("authRequestID"), this->authRequestID_},
        {QStringLiteral("defaultPaintID"),
         this->defaultPaintID(twitchPlatform())},
        {QStringLiteral("defaultBadgeID"),
         this->defaultBadgeID(twitchPlatform())},
        {QStringLiteral("kickDefaultPaintID"),
         this->defaultPaintID(kickPlatform())},
        {QStringLiteral("kickDefaultBadgeID"),
         this->defaultBadgeID(kickPlatform())},
        {QStringLiteral("channelOverrides"), overrides},
        {QStringLiteral("updatedAt"),
         static_cast<qint64>(
             getSettings()->sevenTVAccountUpdatedAt.getValue())},
    };
}

bool SeventvAccountManager::importSyncState(const QJsonObject &state,
                                            QString *error)
{
    const auto incomingUpdatedAt =
        state.value(QStringLiteral("updatedAt")).toVariant().toULongLong();
    const auto currentUpdatedAt =
        getSettings()->sevenTVAccountUpdatedAt.getValue();
    if (currentUpdatedAt > 0 && incomingUpdatedAt <= currentUpdatedAt)
    {
        return true;
    }

    if (this->busy_)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("7TV is currently refreshing. Try again.");
        }
        return false;
    }
    const auto token = state.value(QStringLiteral("token")).toString().trimmed();
    if (!token.isEmpty() && !validateJwt(token, error))
    {
        return false;
    }

    std::vector<SeventvChannelCosmeticOverride> overrides;
    const auto rawOverrides =
        state.value(QStringLiteral("channelOverrides")).toArray();
    if (rawOverrides.size() > 500)
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("Too many 7TV channel presets.");
        }
        return false;
    }
    for (const auto &value : rawOverrides)
    {
        const auto object = value.toObject();
        SeventvChannelCosmeticOverride entry{
            .channelID =
                object.value(QStringLiteral("channelID")).toString().trimmed(),
            .channelLogin =
                object.value(QStringLiteral("channelLogin"))
                    .toString()
                    .trimmed()
                    .toLower(),
            .channelDisplayName =
                object.value(QStringLiteral("channelDisplayName"))
                    .toString()
                    .trimmed(),
            .platform = normalizedCosmeticPlatform(
                object.value(QStringLiteral("platform")).toString()),
            .paintID =
                object.value(QStringLiteral("paintID")).toString().trimmed(),
            .badgeID =
                object.value(QStringLiteral("badgeID")).toString().trimmed(),
        };
        if (!entry.channelID.isEmpty())
        {
            overrides.push_back(std::move(entry));
        }
    }

    getSettings()->sevenTVAccountToken = token;
    getSettings()->sevenTVAccountUserID =
        state.value(QStringLiteral("userID")).toString().trimmed();
    getSettings()->sevenTVAccountUsername =
        state.value(QStringLiteral("username")).toString().trimmed();
    getSettings()->sevenTVAccountDisplayName =
        state.value(QStringLiteral("displayName")).toString().trimmed();
    getSettings()->sevenTVDefaultPaintID =
        state.value(QStringLiteral("defaultPaintID")).toString().trimmed();
    getSettings()->sevenTVDefaultBadgeID =
        state.value(QStringLiteral("defaultBadgeID")).toString().trimmed();
    this->kickDefaultPaintID_ =
        state.contains(QStringLiteral("kickDefaultPaintID"))
            ? state.value(QStringLiteral("kickDefaultPaintID"))
                  .toString()
                  .trimmed()
            : getSettings()->sevenTVDefaultPaintID.getValue();
    this->kickDefaultBadgeID_ =
        state.contains(QStringLiteral("kickDefaultBadgeID"))
            ? state.value(QStringLiteral("kickDefaultBadgeID"))
                  .toString()
                  .trimmed()
            : getSettings()->sevenTVDefaultBadgeID.getValue();
    this->channelOverrides_ = std::move(overrides);
    this->saveOverrides();
    getSettings()->sevenTVAccountUpdatedAt = incomingUpdatedAt;
    std::ignore = getSettings()->requestSave();
    this->lastChannelApplyKey_.clear();
    this->stateChanged.invoke();
    if (!token.isEmpty())
    {
        this->authRequestID_.clear();
        this->refresh();
    }
    else
    {
        this->clearRuntimeState();
        this->inventoryChanged.invoke();
    }
    return true;
}

void SeventvAccountManager::requestGraphQL(
    const QString &query, const QJsonObject &variables, JsonCallback onSuccess,
    ErrorCallback onError)
{
    const auto token =
        getSettings()->sevenTVAccountToken.getValue().trimmed();
    QJsonObject payload{
        {QStringLiteral("query"), query},
        {QStringLiteral("variables"), variables},
    };
    NetworkRequest(GQL_URL, NetworkRequestType::Post)
        .header("Authorization", QByteArrayLiteral("Bearer ") + token.toUtf8())
        .json(payload)
        .timeout(20000)
        .onSuccess([onSuccess = std::move(onSuccess), onError](
                       const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto error = graphQLError(root);
            if (!error.isEmpty())
            {
                onError(error);
                return;
            }
            onSuccess(root.value(QStringLiteral("data")).toObject());
        })
        .onError([onError = std::move(onError)](
                     const NetworkResult &result) {
            onError(QStringLiteral("7TV request failed: %1")
                        .arg(result.formatError()));
        })
        .execute();
}

void SeventvAccountManager::requestGraphQLV4(
    const QString &query, const QJsonObject &variables, JsonCallback onSuccess,
    ErrorCallback onError)
{
    const auto token =
        getSettings()->sevenTVAccountToken.getValue().trimmed();
    QJsonObject payload{
        {QStringLiteral("query"), query},
        {QStringLiteral("variables"), variables},
    };
    NetworkRequest(GQL_V4_URL, NetworkRequestType::Post)
        .header("Authorization", QByteArrayLiteral("Bearer ") + token.toUtf8())
        .json(payload)
        .timeout(20000)
        .onSuccess([onSuccess = std::move(onSuccess), onError](
                       const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto error = graphQLError(root);
            if (!error.isEmpty())
            {
                onError(error);
                return;
            }
            onSuccess(root.value(QStringLiteral("data")).toObject());
        })
        .onError([onError = std::move(onError)](
                     const NetworkResult &result) {
            onError(QStringLiteral("7TV request failed: %1")
                        .arg(result.formatError()));
        })
        .execute();
}

void SeventvAccountManager::loadActor()
{
    this->editorChannelsLoaded_ = false;
    this->requestGraphQL(
        ACTOR_QUERY, {},
        [this](const QJsonObject &data) {
            const auto actor = data.value(QStringLiteral("actor")).toObject();
            const auto id = actor.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
            {
                this->handleActorLoadFailure(
                    QStringLiteral("7TV did not return an account."));
                return;
            }

            this->signInPending_ = false;
            getSettings()->sevenTVAccountUserID = id;
            getSettings()->sevenTVAccountUsername =
                actor.value(QStringLiteral("username")).toString();
            getSettings()->sevenTVAccountDisplayName =
                actor.value(QStringLiteral("display_name")).toString();

            std::vector<SeventvEditorChannel> editorChannels;
            const auto appendConnections = [&editorChannels](
                                               const QString &userID,
                                               const QString &fallbackUsername,
                                               const QString &fallbackDisplayName,
                                               const QJsonArray &connections) {
                for (const auto &value : connections)
                {
                    const auto connection = value.toObject();
                    SeventvEditorChannel channel{
                        .userID = userID,
                        .platform =
                            connection.value(QStringLiteral("platform"))
                                .toString()
                                .toUpper(),
                        .connectionID =
                            connection.value(QStringLiteral("id")).toString(),
                        .username =
                            connection.value(QStringLiteral("username"))
                                .toString(),
                        .displayName =
                            connection.value(QStringLiteral("display_name"))
                                .toString(),
                        .emoteSetID =
                            connection.value(QStringLiteral("emote_set_id"))
                                .toString(),
                    };
                    if (channel.username.isEmpty())
                    {
                        channel.username = fallbackUsername;
                    }
                    if (channel.displayName.isEmpty())
                    {
                        channel.displayName = fallbackDisplayName;
                    }
                    if (channel.emoteSetID.isEmpty())
                    {
                        continue;
                    }

                    const auto duplicate = std::ranges::find_if(
                        editorChannels, [&channel](const auto &existing) {
                            return existing.platform == channel.platform &&
                                   existing.connectionID ==
                                       channel.connectionID &&
                                   existing.emoteSetID == channel.emoteSetID;
                        });
                    if (duplicate == editorChannels.end())
                    {
                        editorChannels.emplace_back(std::move(channel));
                    }
                }
            };

            appendConnections(
                id, actor.value(QStringLiteral("username")).toString(),
                actor.value(QStringLiteral("display_name")).toString(),
                actor.value(QStringLiteral("connections")).toArray());
            for (const auto &value :
                 actor.value(QStringLiteral("editor_of")).toArray())
            {
                const auto editor = value.toObject();
                const auto permissions =
                    editor.value(QStringLiteral("permissions")).toInteger();
                if ((permissions & SEVENTV_EDITOR_MODIFY_EMOTES) == 0)
                {
                    continue;
                }

                const auto user =
                    editor.value(QStringLiteral("user")).toObject();
                auto editorUserID =
                    user.value(QStringLiteral("id")).toString();
                if (editorUserID.isEmpty())
                {
                    editorUserID =
                        editor.value(QStringLiteral("id")).toString();
                }
                appendConnections(
                    editorUserID,
                    user.value(QStringLiteral("username")).toString(),
                    user.value(QStringLiteral("display_name")).toString(),
                    user.value(QStringLiteral("connections")).toArray());
            }

            const auto platformRank = [](const QString &platform) {
                if (platform == QStringLiteral("TWITCH"))
                {
                    return 0;
                }
                if (platform == QStringLiteral("KICK"))
                {
                    return 1;
                }
                if (platform == QStringLiteral("YOUTUBE"))
                {
                    return 2;
                }
                return 3;
            };
            std::ranges::sort(editorChannels, [&](const auto &left,
                                                  const auto &right) {
                const auto leftRank = platformRank(left.platform);
                const auto rightRank = platformRank(right.platform);
                if (leftRank != rightRank)
                {
                    return leftRank < rightRank;
                }
                const auto leftName = left.displayName.isEmpty()
                                          ? left.username
                                          : left.displayName;
                const auto rightName = right.displayName.isEmpty()
                                           ? right.username
                                           : right.displayName;
                return leftName.compare(rightName, Qt::CaseInsensitive) < 0;
            });
            this->editorChannels_ = std::move(editorChannels);
            this->editorChannelsLoaded_ = true;

            const auto style = actor.value(QStringLiteral("style")).toObject();
            this->activePaintID_ =
                style.value(QStringLiteral("paint_id")).toString();
            this->activeBadgeID_ =
                style.value(QStringLiteral("badge_id")).toString();
            this->activeStateKnown_ = true;

            QStringList cosmeticIDs;
            for (const auto &value :
                 actor.value(QStringLiteral("cosmetics")).toArray())
            {
                const auto cosmetic = value.toObject();
                const auto cosmeticID =
                    cosmetic.value(QStringLiteral("id")).toString();
                if (!cosmeticID.isEmpty())
                {
                    cosmeticIDs.append(cosmeticID);
                }
                if (!cosmetic.value(QStringLiteral("selected")).toBool())
                {
                    continue;
                }
                const auto kind =
                    cosmetic.value(QStringLiteral("kind")).toString();
                if (kind == QStringLiteral("PAINT"))
                {
                    this->activePaintID_ = cosmeticID;
                }
                else if (kind == QStringLiteral("BADGE"))
                {
                    this->activeBadgeID_ = cosmeticID;
                }
            }

            QString selectedEmoteSet;
            for (const auto &value :
                 actor.value(QStringLiteral("connections")).toArray())
            {
                const auto connection = value.toObject();
                const auto setID =
                    connection.value(QStringLiteral("emote_set_id")).toString();
                if (setID.isEmpty())
                {
                    continue;
                }
                if (selectedEmoteSet.isEmpty())
                {
                    selectedEmoteSet = setID;
                }
                if (connection.value(QStringLiteral("platform")).toString() ==
                    QStringLiteral("TWITCH"))
                {
                    selectedEmoteSet = setID;
                    break;
                }
            }
            getSettings()->sevenTVAccountEmoteSetID = selectedEmoteSet;
            std::ignore = getSettings()->requestSave();
            this->stateChanged.invoke();
            this->loadCosmetics(cosmeticIDs);
        },
        [this](const QString &error) {
            this->handleActorLoadFailure(error);
        });
}

void SeventvAccountManager::handleActorLoadFailure(const QString &error)
{
    const bool failedSignIn = std::exchange(this->signInPending_, false);
    if (failedSignIn)
    {
        getSettings()->sevenTVAccountToken = QString{};
        getSettings()->sevenTVAccountUserID = QString{};
        getSettings()->sevenTVAccountUsername = QString{};
        getSettings()->sevenTVAccountDisplayName = QString{};
        getSettings()->sevenTVAccountEmoteSetID = QString{};
        this->authRequestID_.clear();
        this->clearRuntimeState();
        this->markChanged();
        this->stateChanged.invoke();
        this->inventoryChanged.invoke();
        this->feedback.invoke(
            QStringLiteral("7TV sign-in failed: %1").arg(error), true);
        return;
    }

    this->activeStateKnown_ = false;
    this->editorChannelsLoaded_ = false;
    this->editorChannels_.clear();
    this->setBusy(false);
    this->feedback.invoke(error, true);
}

void SeventvAccountManager::loadCosmetics(const QStringList &ids)
{
    this->requestGraphQLV4(
        COSMETIC_INVENTORY_V4_QUERY, {},
        [this, ids](const QJsonObject &data) {
            const auto inventory = data.value(QStringLiteral("users"))
                                       .toObject()
                                       .value(QStringLiteral("me"))
                                       .toObject()
                                       .value(QStringLiteral("inventory"))
                                       .toObject();
            if (inventory.isEmpty())
            {
                this->loadLegacyCosmetics(ids);
                return;
            }

            this->paints_.clear();
            this->badges_.clear();
            QSet<QString> seenPaints;
            QSet<QString> seenBadges;
            for (const auto &value :
                 inventory.value(QStringLiteral("paints")).toArray())
            {
                const auto paint = value.toObject()
                                       .value(QStringLiteral("to"))
                                       .toObject()
                                       .value(QStringLiteral("paint"))
                                       .toObject();
                const auto id = paint.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || seenPaints.contains(id))
                {
                    continue;
                }
                seenPaints.insert(id);
                this->paints_.push_back(
                    parseCosmetic(paint, QStringLiteral("PAINT")));
            }
            for (const auto &value :
                 inventory.value(QStringLiteral("badges")).toArray())
            {
                const auto badge = value.toObject()
                                       .value(QStringLiteral("to"))
                                       .toObject()
                                       .value(QStringLiteral("badge"))
                                       .toObject();
                const auto id = badge.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || seenBadges.contains(id))
                {
                    continue;
                }
                seenBadges.insert(id);
                this->badges_.push_back(
                    parseCosmetic(badge, QStringLiteral("BADGE")));
            }
            sortCosmetics(this->paints_);
            sortCosmetics(this->badges_);
            this->inventoryChanged.invoke();
            this->loadEmotes();
        },
        [this, ids](const QString &) {
            this->loadLegacyCosmetics(ids);
        });
}

void SeventvAccountManager::loadLegacyCosmetics(const QStringList &ids)
{
    if (ids.isEmpty())
    {
        this->paints_.clear();
        this->badges_.clear();
        this->inventoryChanged.invoke();
        this->loadEmotes();
        return;
    }

    getApp()->getSeventvAPI()->getCosmeticsByIDs(
        ids,
        [this](const QJsonObject &inventory) {
            this->paints_.clear();
            this->badges_.clear();
            for (const auto &value :
                 inventory.value(QStringLiteral("paints")).toArray())
            {
                this->paints_.push_back(
                    parseCosmetic(value.toObject(), QStringLiteral("PAINT")));
            }
            for (const auto &value :
                 inventory.value(QStringLiteral("badges")).toArray())
            {
                this->badges_.push_back(
                    parseCosmetic(value.toObject(), QStringLiteral("BADGE")));
            }
            sortCosmetics(this->paints_);
            sortCosmetics(this->badges_);
            this->inventoryChanged.invoke();
            this->loadEmotes();
        },
        [this](const NetworkResult &result) {
            this->setBusy(false);
            this->feedback.invoke(
                QStringLiteral("Unable to load 7TV cosmetics: %1")
                    .arg(result.formatError()),
                true);
            this->flushPendingChannelCosmetics();
        });
}

void SeventvAccountManager::loadEmotes()
{
    const auto setID = this->emoteSetID();
    if (setID.isEmpty())
    {
        this->emotes_.clear();
        this->setBusy(false);
        this->inventoryChanged.invoke();
        this->flushPendingChannelCosmetics();
        return;
    }
    getApp()->getSeventvAPI()->getEmoteSet(
        setID,
        [this](const QJsonObject &set) {
            this->emotes_.clear();
            for (const auto &value :
                 set.value(QStringLiteral("emotes")).toArray())
            {
                const auto object = value.toObject();
                this->emotes_.push_back({
                    .id = object.value(QStringLiteral("id")).toString(),
                    .name = object.value(QStringLiteral("name")).toString(),
                    .data = object,
                });
            }
            std::ranges::sort(
                this->emotes_, [](const auto &left, const auto &right) {
                    return left.name.compare(right.name,
                                             Qt::CaseInsensitive) < 0;
                });
            this->setBusy(false);
            this->inventoryChanged.invoke();
            this->feedback.invoke(QStringLiteral("7TV account refreshed."),
                                  false);
            this->flushPendingChannelCosmetics();
        },
        [this](const NetworkResult &result) {
            this->setBusy(false);
            this->feedback.invoke(
                QStringLiteral("Unable to load the 7TV emote set: %1")
                    .arg(result.formatError()),
                true);
            this->flushPendingChannelCosmetics();
        });
}

void SeventvAccountManager::mutateCosmetic(
    const QString &kind, const QString &currentID, const QString &desiredID,
    SuccessCallback onSuccess, ErrorCallback onError)
{
    if (currentID == desiredID)
    {
        onSuccess();
        return;
    }
    const auto cosmeticID = desiredID.isEmpty() ? currentID : desiredID;
    if (cosmeticID.isEmpty())
    {
        onSuccess();
        return;
    }
    this->requestGraphQL(
        UPDATE_COSMETIC_MUTATION,
        QJsonObject{
            {QStringLiteral("id"), this->userID()},
            {QStringLiteral("update"),
             QJsonObject{
                 {QStringLiteral("id"), cosmeticID},
                 {QStringLiteral("kind"), kind},
                 {QStringLiteral("selected"), !desiredID.isEmpty()},
             }},
        },
        [onSuccess = std::move(onSuccess)](const QJsonObject &) {
            onSuccess();
        },
        std::move(onError));
}

void SeventvAccountManager::changeEmote(const QString &action,
                                        const QString &emoteID,
                                        const QString &name)
{
    if (this->emoteSetID().isEmpty())
    {
        this->feedback.invoke(
            QStringLiteral("No editable 7TV emote set was found."), true);
        return;
    }
    this->setBusy(true);
    this->requestEmoteChange(
        this->emoteSetID(), action, emoteID, name,
        [this] {
            this->loadEmotes();
        },
        [this](const QString &error) {
            this->setBusy(false);
            this->feedback.invoke(error, true);
        });
}

void SeventvAccountManager::requestEmoteChange(
    const QString &emoteSetID, const QString &action, const QString &emoteID,
    const QString &name, SuccessCallback onSuccess, ErrorCallback onError)
{
    QJsonObject variables{
        {QStringLiteral("id"), emoteSetID},
        {QStringLiteral("action"), action},
        {QStringLiteral("emote_id"), emoteID},
    };
    if (!name.trimmed().isEmpty())
    {
        variables.insert(QStringLiteral("name"), name.trimmed());
    }
    this->requestGraphQL(
        CHANGE_EMOTE_MUTATION, variables,
        [onSuccess = std::move(onSuccess)](const QJsonObject &) {
            if (onSuccess)
            {
                onSuccess();
            }
        },
        std::move(onError));
}

void SeventvAccountManager::setBusy(bool busy)
{
    if (this->busy_ == busy)
    {
        return;
    }
    this->busy_ = busy;
    const auto generation = ++this->busyGeneration_;
    this->busyChanged.invoke(busy);
    if (!busy)
    {
        return;
    }

    QTimer::singleShot(
        SEVENTV_BUSY_TIMEOUT_MS, QCoreApplication::instance(),
        [this, generation] {
            if (!this->busy_ || this->busyGeneration_ != generation)
            {
                return;
            }
            this->lastChannelApplyKey_.clear();
            this->setBusy(false);
            this->feedback.invoke(
                QStringLiteral("The 7TV action timed out. Try Refresh."), true);
        });
}

void SeventvAccountManager::markChanged()
{
    const auto previous = getSettings()->sevenTVAccountUpdatedAt.getValue();
    const auto now =
        static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
    getSettings()->sevenTVAccountUpdatedAt = std::max(now, previous + 1);
    std::ignore = getSettings()->requestSave();
}

void SeventvAccountManager::loadOverrides()
{
    this->channelOverrides_.clear();
    const auto document = QJsonDocument::fromJson(
        getSettings()->sevenTVChannelCosmeticsJson.getValue().toUtf8());
    QJsonArray storedOverrides;
    if (document.isObject())
    {
        const auto root = document.object();
        const auto kickDefaults =
            root.value(QStringLiteral("kickDefaults")).toObject();
        this->kickDefaultPaintID_ =
            kickDefaults.contains(QStringLiteral("paintID"))
                ? kickDefaults.value(QStringLiteral("paintID")).toString()
                : getSettings()->sevenTVDefaultPaintID.getValue();
        this->kickDefaultBadgeID_ =
            kickDefaults.contains(QStringLiteral("badgeID"))
                ? kickDefaults.value(QStringLiteral("badgeID")).toString()
                : getSettings()->sevenTVDefaultBadgeID.getValue();
        storedOverrides =
            root.value(QStringLiteral("overrides")).toArray();
    }
    else
    {
        // Version 1 stored a plain array and only supported Twitch.
        this->kickDefaultPaintID_ =
            getSettings()->sevenTVDefaultPaintID.getValue();
        this->kickDefaultBadgeID_ =
            getSettings()->sevenTVDefaultBadgeID.getValue();
        storedOverrides = document.array();
    }
    for (const auto &value : storedOverrides)
    {
        const auto object = value.toObject();
        const auto channelID =
            object.value(QStringLiteral("channelID")).toString();
        if (channelID.isEmpty())
        {
            continue;
        }
        this->channelOverrides_.push_back({
            .channelID = channelID,
            .channelLogin =
                object.value(QStringLiteral("channelLogin")).toString(),
            .channelDisplayName =
                object.value(QStringLiteral("channelDisplayName")).toString(),
            .platform = normalizedCosmeticPlatform(
                object.value(QStringLiteral("platform")).toString()),
            .paintID = object.value(QStringLiteral("paintID")).toString(),
            .badgeID = object.value(QStringLiteral("badgeID")).toString(),
        });
    }
}

void SeventvAccountManager::saveOverrides()
{
    QJsonArray overrides;
    for (const auto &entry : this->channelOverrides_)
    {
        overrides.append(QJsonObject{
            {QStringLiteral("channelID"), entry.channelID},
            {QStringLiteral("channelLogin"), entry.channelLogin},
            {QStringLiteral("channelDisplayName"), entry.channelDisplayName},
            {QStringLiteral("platform"),
             normalizedCosmeticPlatform(entry.platform)},
            {QStringLiteral("paintID"), entry.paintID},
            {QStringLiteral("badgeID"), entry.badgeID},
        });
    }
    const QJsonObject root{
        {QStringLiteral("version"), 2},
        {QStringLiteral("kickDefaults"),
         QJsonObject{{QStringLiteral("paintID"), this->kickDefaultPaintID_},
                     {QStringLiteral("badgeID"),
                      this->kickDefaultBadgeID_}}},
        {QStringLiteral("overrides"), overrides},
    };
    getSettings()->sevenTVChannelCosmeticsJson = QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void SeventvAccountManager::clearRuntimeState()
{
    this->activeStateKnown_ = false;
    this->editorChannelsLoaded_ = false;
    this->activePaintID_.clear();
    this->activeBadgeID_.clear();
    this->lastChannelApplyKey_.clear();
    this->pendingChannelID_.clear();
    this->pendingChannelLogin_.clear();
    this->pendingChannelDisplayName_.clear();
    this->pendingPlatform_.clear();
    this->paints_.clear();
    this->badges_.clear();
    this->emotes_.clear();
    this->searchResults_.clear();
    this->editorChannels_.clear();
    this->setBusy(false);
}

void SeventvAccountManager::flushPendingChannelCosmetics()
{
    if (this->busy_ || !this->activeStateKnown_ ||
        this->pendingChannelID_.isEmpty())
    {
        return;
    }

    const auto platform = std::exchange(this->pendingPlatform_, {});
    const auto channelID = std::exchange(this->pendingChannelID_, {});
    const auto channelLogin = std::exchange(this->pendingChannelLogin_, {});
    const auto channelDisplayName =
        std::exchange(this->pendingChannelDisplayName_, {});
    this->ensureChannelCosmetics(platform, channelID, channelLogin,
                                 channelDisplayName);
}

}  // namespace chatterino
