// SPDX-License-Identifier: MIT
#pragma once

#include "controllers/moderation/ModerationRules.hpp"
#include "messages/Message.hpp"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>

#include <deque>
#include <list>

namespace chatterino::moderation {

struct ReviewInput {
    MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    QString userID;
    QString login;
    QString displayName;
    QColor color;
    QString messageID;
    QString text;
    QDateTime timestamp;
    bool trusted = false;
    bool subscriber = false;
    bool firstMessage = false;
    bool own = false;
    bool system = false;
    bool deleted = false;
    bool platformFlagged = false;
    int emoteCount = 0;
    /// Emote names, so they don't count as shouting.
    QStringList emotes;
    /// Login of the user a timeout message is about.
    QString timeoutTarget;

    static ReviewInput fromMessage(const Message &message);
};

struct ReviewHistory {
    quint64 sequence = 0;
    QString messageID;
    QString text;
    QDateTime timestamp;
    bool deleted = false;
};

struct ReviewUser {
    QString key;
    MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    QString userID;
    QString login;
    QString displayName;
    QColor color;
    bool tracked = false;
    bool pending = false;
    bool trusted = false;
    bool subscriber = false;
    qint64 lastSeen = 0;
    qint64 markedAt = 0;
    qint64 dismissedAt = -1;
    quint64 revision = 0;
    quint64 pendingSequence = 0;
    /// Sum of the weights of the current reasons.
    int severity = 0;
    /// Automatic actions taken this session, for escalation.
    int offences = 0;
    qint64 lastActionAt = -100000;
    qint64 lastActionStrength = 0;
    QDateTime accountCreated;
    bool namesChecked = false;
    bool ageRequested = false;
    QMap<QString, QString> reasons;
    std::deque<ReviewHistory> history;
};

/// An automatic action a rule asks for. The review never sends it itself.
struct ReviewTrigger {
    QString key;
    QString code;
    QString rule;
    RuleAction action = RuleAction::Flag;
    int timeout = 0;
    QString messageID;
};

struct SimulationResult {
    int messages = 0;
    int flagged = 0;
    int actions = 0;
    /// "name: reason" for the first few flagged users.
    QStringList examples;
};

// GUI-independent review state. It never performs moderation actions and holds
// only bounded text/identity data, rather than retaining full chat messages.
class ModerationReview
{
public:
    ModerationReview() = default;
    ModerationReview(const ModerationReview &) = delete;
    ModerationReview &operator=(const ModerationReview &) = delete;
    ModerationReview(ModerationReview &&) = default;
    ModerationReview &operator=(ModerationReview &&) = default;
    static constexpr int MAX_USERS = 500;
    static constexpr int MAX_TRACKED = 200;
    static constexpr int MAX_PENDING = 300;
    static constexpr int MAX_HISTORY = 6000;
    static constexpr int MAX_USER_HISTORY = 24;
    static constexpr int MAX_TEXT = 1000;
    static constexpr int MAX_TRIGGERS = 200;
    static constexpr qint64 DISMISS_COOLDOWN_MS = 30000;
    /// Messages already sent before an action lands mustn't repeat it.
    static constexpr qint64 ACTION_COOLDOWN_MS = 5000;

    static QString keyFor(const ReviewInput &input);
    static QString platformName(MessagePlatform platform);
    static QString notesKey(const ReviewUser &user);
    /// Runs the rules over past messages without keeping or sending anything.
    static SimulationResult simulate(const ReviewRules &rules,
                                     const std::vector<ReviewInput> &inputs);

    void setRules(ReviewRules rules);
    const ReviewRules &rules() const;
    QString ingest(ReviewInput input, qint64 now, bool evaluate = true);
    void updateMessage(const ReviewInput &input, bool removed = false);
    bool track(const QString &key, bool tracked);
    bool mark(const QString &key, qint64 now);
    bool dismiss(const QString &key, qint64 now);
    void dismissAll(qint64 now);
    const ReviewUser *find(const QString &key) const;
    QStringList keys() const;
    int pendingCount() const;
    int trackedCount() const;
    quint64 revision() const;
    QJsonArray trackedJson() const;
    void restoreTracked(const QJsonArray &json);

    std::vector<ReviewTrigger> takeTriggers();
    /// Keys of Twitch users whose account age is needed for the new account
    /// rule.
    QStringList takeAgeLookups();
    void applyAccountCreated(const QString &key, const QDateTime &created,
                             qint64 now);
    /// Remembers someone who was timed out or banned, for the returning
    /// offenders rule.
    void noteActioned(const QString &key);

private:
    struct State {
        ReviewUser user;
        std::deque<qint64> arrivals;
        std::deque<std::pair<qint64, QByteArray>> repetitions;
        std::deque<std::pair<qint64, int>> links;
        QByteArray lastDigest;
        int streak = 0;
        std::list<QString>::iterator lru;
    };
    struct GroupEntry {
        qint64 time;
        QString key;
    };
    struct Group {
        std::deque<GroupEntry> entries;
        qint64 lastSeen = 0;
    };
    struct Candidate {
        QString code;
        QString rule;
        RuleAction action;
        int timeout;
    };
    State *ensureUser(const ReviewInput &input, qint64 now);
    void changed(ReviewUser &user);
    void flag(State &state, const QString &code, const QString &reason,
              qint64 now);
    /// Flags the user and records the rule's action, unless the rule skips
    /// subscribers and they are one.
    void hit(State &state, const QString &code, const QString &reason,
             const RuleBase &rule, qint64 now, const QString &title = {});
    void resolveCandidates(qint64 now);
    bool excluded(const ReviewInput &input) const;
    void evaluate(State &state, const ReviewInput &input, qint64 now);
    void recountSeverity(ReviewUser &user);

    ReviewRules rules_;
    std::vector<TermMatcher> matchers_;
    std::vector<AllowEntry> allowed_;
    std::vector<QRegularExpression> namePatterns_;
    QHash<QString, State> users_;
    std::list<QString> lru_;
    std::deque<std::pair<QString, quint64>> pendingOrder_;
    std::deque<std::pair<QString, quint64>> historyOrder_;
    QHash<QByteArray, Group> groups_;
    QSet<QString> seenIDs_;
    std::deque<QString> seenOrder_;
    QSet<QString> offenders_;
    QHash<QString, std::vector<Candidate>> candidates_;
    std::vector<ReviewTrigger> triggers_;
    QStringList ageLookups_;
    int pending_ = 0;
    int tracked_ = 0;
    quint64 sequence_ = 0;
    quint64 revision_ = 0;
};

}  // namespace chatterino::moderation
