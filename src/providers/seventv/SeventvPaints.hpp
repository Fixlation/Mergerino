#pragma once

#include "providers/seventv/paints/Paint.hpp"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace chatterino {

namespace seventv::eventapi {
struct TwitchUser;
struct KickUser;
using User = std::variant<TwitchUser, KickUser>;
}  // namespace seventv::eventapi

std::shared_ptr<Paint> makeSeventvPaintFromGraphQL(
    const QJsonObject &paintJson);
QString seventvPaintAnimationUrl(const std::shared_ptr<Paint> &paint);
QJsonArray seventvPaintBrowserLayers(const std::shared_ptr<Paint> &paint,
                                     const QColor &userColor);

class SeventvPaints
{
public:
    SeventvPaints();

    void addPaint(const QJsonObject &paintJson);
    void addPaintFromGraphQL(const QJsonObject &paintJson);
    void removePaint(const QString &paintID);
    void assignPaintToUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users,
                             std::optional<QColor> styleColor = std::nullopt);
    void clearPaintFromUsers(const QString &paintID,
                             std::span<const seventv::eventapi::User> users);

    std::shared_ptr<Paint> getPaint(const QString &userName, bool kick) const;
    std::optional<QColor> getUserStyleColor(const QString &userName,
                                            bool kick) const;

private:
    void addParsedPaint(const QString &paintID, std::shared_ptr<Paint> paint);
    void ensurePaintLoaded(const QString &paintID);

    // Mutex for both `paintMap_` and `knownPaints_`
    mutable std::shared_mutex mutex_;

    // user-name => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> kickPaintMap_;
    // user-name => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> twitchPaintMap_;
    // user-name => 7TV style color used as the base color for paint-only shadows
    std::unordered_map<QString, QColor> kickPaintUserColors_;
    // user-name => 7TV style color used as the base color for paint-only shadows
    std::unordered_map<QString, QColor> twitchPaintUserColors_;
    // paint-id => paint
    std::unordered_map<QString, std::shared_ptr<Paint>> knownPaints_;
    // user-name => pending paint-id
    std::unordered_map<QString, QString> pendingKickPaintAssignments_;
    // user-name => pending paint-id
    std::unordered_map<QString, QString> pendingTwitchPaintAssignments_;
    std::unordered_set<QString> requestedPaintIDs_;
};

}  // namespace chatterino
