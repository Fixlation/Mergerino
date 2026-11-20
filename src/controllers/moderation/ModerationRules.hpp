// SPDX-License-Identifier: MIT
#pragma once

#include "messages/Message.hpp"

#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <array>
#include <optional>
#include <vector>

namespace chatterino::moderation {

/// What happens when a rule matches. Everything except Flag is an automatic
/// moderation action, sent only while automatic actions are allowed.
enum class RuleAction : std::uint8_t {
    Flag,
    Delete,
    Warn,
    Timeout,
    Ban,
    /// Uses the escalation ladder, based on how often the user was actioned.
    Escalate,
};

struct RuleBase {
    bool enabled = false;
    bool skipSubscribers = false;
    RuleAction action = RuleAction::Flag;
    /// Timeout length in seconds when the action is a timeout.
    int timeout = 600;
};

/// "count" events within "window" seconds.
struct WindowRule : RuleBase {
    int count = 0;
    int window = 0;
};

struct RepeatRule : WindowRule {
    /// Count identical messages in a row instead of within a time window.
    bool inARow = false;
};

struct LinkRule : WindowRule {
    /// Domains that never count as links, like clips.twitch.tv.
    QStringList allowed;
    /// Domains that are flagged immediately.
    QStringList blocked;
};

/// "percent" of a message of at least "minLength" characters.
struct ShareRule : RuleBase {
    int percent = 0;
    int minLength = 0;
};

struct LimitRule : RuleBase {
    int limit = 0;
};

struct WallRule : RuleBase {
    int length = 0;
    int artPercent = 0;
};

struct PatternRule : RuleBase {
    /// Wildcard patterns, * matching anything.
    QStringList patterns;
};

struct WordList : RuleBase {
    QString name;
    bool wholeWords = true;
    QStringList terms;
    /// Where the list was imported from, so it can be refreshed.
    QString source;
};

struct EscalationStep {
    RuleAction action = RuleAction::Flag;
    int timeout = 600;
};

/// Chat settings applied by "Lock down chat". Only Twitch lets apps change
/// these.
struct LockdownSettings {
    bool followersOnly = true;
    int followersMinutes = 10;
    bool slowMode = true;
    int slowSeconds = 30;
    bool emoteOnly = false;
    bool subscribersOnly = false;
    bool shieldMode = false;
    /// Use the Raid preset's rule thresholds while locked down.
    bool raidRules = true;
};

enum class RulePreset : std::uint8_t {
    Relaxed,
    Standard,
    Strict,
    Raid,
};

/// A complete set of review rules. Profiles store one of these.
struct ReviewRules {
    ReviewRules();

    bool enabled = true;
    bool automaticActions = true;
    bool excludeTrusted = true;

    WindowRule bursts;
    RepeatRule repeats;
    LinkRule links;
    WindowRule coordinated;
    RuleBase platformAlerts;
    ShareRule caps;
    ShareRule symbols;
    LimitRule emotes;
    WallRule walls;
    LimitRule mentions;
    RuleBase unusual;
    RuleBase firstMessages;
    /// Accounts younger than "limit" days. Twitch only.
    LimitRule newAccounts;
    PatternRule names;
    RuleBase returning;
    std::vector<WordList> lists;
    /// Entries are "[platform:]name[ #id]".
    QStringList allowList;
    std::array<EscalationStep, 4> escalation;
    LockdownSettings lockdown;

    void normalize();
    QJsonObject toJson() const;
    /// Reads current profiles and the flat format older versions saved.
    static ReviewRules fromJson(const QJsonObject &json);

    /// Presets only change which detectors run and how sensitive they are.
    /// Word lists, actions, exceptions and escalation are left alone.
    void applyPreset(RulePreset preset);
    std::optional<RulePreset> matchingPreset() const;
};

QString presetName(RulePreset preset);
/// Short label, e.g. "Timeout 10m".
QString actionLabel(RuleAction action, int timeout = 0);
QString durationLabel(int seconds);
QString ruleTitle(const QString &code);
/// Higher numbers sort first in the review queue.
int ruleWeight(const QString &code);
bool actionSupported(RuleAction action, MessagePlatform platform);

/// One entry per line. Blank lines and lines starting with # are skipped. A
/// JSON array of strings, or an object with such an array, also works, as do
/// arrays of {"match": "a|b", "tags": [...]} entries, optionally limited to
/// some tags.
QStringList parseTermList(const QString &text, const QStringList &tags = {});

/// A list someone can import from a public URL.
struct SuggestedList {
    QString title;
    QString name;
    /// The URL, with "#tags=a,b" to keep only entries with those tags.
    QString source;
};
const std::vector<SuggestedList> &suggestedLists();
/// Splits a list source into the URL to fetch and the tags to keep.
std::pair<QString, QStringList> splitListSource(const QString &source);

struct AllowEntry {
    std::optional<MessagePlatform> platform;
    QString name;
    QString id;

    static std::optional<AllowEntry> parse(const QString &line);
    QString toString() const;
};

std::optional<MessagePlatform> platformFromName(const QString &name);
QString platformKey(MessagePlatform platform);

/// Folds case, width and accents, and removes invisible characters.
QString cleanForMatching(const QString &text);
/// Zero-width and other invisible characters.
int hiddenCharacterCount(const QString &text);
/// The most combining marks stacked on one character, as in zalgo text.
int markStackLength(const QString &text);

/// Matches text against a word list. It catches common ways of dodging a
/// filter: look-alike characters, l33t spelling, spaced or dotted letters and
/// stretched words.
class TermMatcher
{
public:
    explicit TermMatcher(const WordList &list = {});

    /// Returns the configured term that matched, or an empty string.
    QString match(const QString &text) const;
    bool empty() const;

private:
    struct Affix {
        QString text;
        QString term;
    };

    bool wholeWords_ = true;
    QHash<QString, QString> exact_;
    QHash<QString, QString> stretched_;
    std::vector<Affix> prefixes_;
    std::vector<Affix> suffixes_;
    std::vector<Affix> fragments_;
    std::vector<Affix> phrases_;
    std::vector<Affix> literals_;
    /// Words with a * in the middle, like "bi*tch".
    std::vector<std::pair<QRegularExpression, QString>> patterns_;
};

/// Splits text into normalized words for TermMatcher.
QStringList matchingWords(const QString &text);
QString normalizeWord(const QString &word);
QString collapseRepeats(const QString &word);

}  // namespace chatterino::moderation
