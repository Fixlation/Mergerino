// SPDX-License-Identifier: MIT
#pragma once

#include "messages/Message.hpp"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace chatterino::priority {

enum class Choice { Automatic, Include, Exclude };

struct Rules {
    bool subscriptions = false;
    int subscriptionMonths = 0;
    bool activeOnly = false;
    bool follows = false;
    int followMonths = 0;
    bool matchAll = false;
    bool moderators = false;
    bool vips = false;
    bool bots = false;
    QStringList botAccounts;

    bool configured() const;
    QJsonObject toJson() const;
    static Rules fromJson(const QJsonObject &json);
};

struct Person {
    MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    QString id;
    QString login;
    QString name;
    std::optional<int> months;
    std::optional<bool> active;
    QDateTime activeAt;
    QDateTime followedAt;
    bool followingKnown = false;
    QDateTime lastSeen;
    qint64 subAttempt = 0;
    qint64 followAttempt = 0;
    qint64 kickAttempt = 0;
    bool botBadge = false;
};

struct ManualPerson {
    Choice choice = Choice::Automatic;
    QString name;
    QString login;
    MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    QString id;
};

/// A badge establishes a lower bound, never an inferred exact duration.
std::optional<int> youtubeBadgeMonths(const QString &label);
QString platformName(MessagePlatform platform);
QString personKey(MessagePlatform platform, const QString &id,
                  const QString &login = {});
bool isChatMessage(const Message &message);
bool isModerator(const Message &message);
bool isVip(const Message &message);
QStringList parseBotAccounts(const QString &text);

class Model
{
public:
    static constexpr int MAX_PEOPLE = 2000;
    QString title = QStringLiteral("Priority Chat");
    int width = 350;
    std::array<Rules, 4> rules;
    QHash<QString, Person> people;
    QHash<QString, ManualPerson> manual;

    bool observe(const Message &message);
    bool isBot(const Message &message) const;
    bool isBot(const Person &person) const;
    bool matches(const Message &message, const QDateTime &now) const;
    bool qualifies(const Person &person, const QDateTime &now) const;
    void setChoice(const Person &person, Choice choice);
    Choice choice(const QString &key) const;
    QJsonObject toJson() const;
    void load(const QJsonObject &json);

private:
    void boundPeople();
};
}  // namespace chatterino::priority
