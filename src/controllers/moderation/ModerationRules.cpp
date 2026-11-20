// SPDX-License-Identifier: MIT
#include "controllers/moderation/ModerationRules.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace chatterino::moderation {
namespace {
constexpr int MAX_LISTS = 32;
constexpr int MAX_TERMS = 10000;
constexpr int MAX_TERM_LENGTH = 100;
constexpr int MAX_ALLOWED = 2000;
constexpr int MAX_PATTERNS = 200;
constexpr int MAX_DOMAINS = 200;
constexpr int MAX_TIMEOUT = 1209600;

const std::pair<RuleAction, QString> ACTION_KEYS[] = {
    {RuleAction::Flag, "flag"},       {RuleAction::Delete, "delete"},
    {RuleAction::Warn, "warn"},       {RuleAction::Timeout, "timeout"},
    {RuleAction::Ban, "ban"},         {RuleAction::Escalate, "escalate"},
};

QString actionKey(RuleAction action)
{
    for (const auto &[value, key] : ACTION_KEYS)
        if (value == action)
            return key;
    return "flag";
}

RuleAction actionFromKey(const QString &key)
{
    for (const auto &[value, name] : ACTION_KEYS)
        if (name == key)
            return value;
    return RuleAction::Flag;
}

QStringList boundedList(const QStringList &values, int count, int length)
{
    QStringList result;
    QSet<QString> seen;
    for (auto value : values)
    {
        value = value.trimmed().left(length);
        const auto folded = value.toCaseFolded();
        if (value.isEmpty() || seen.contains(folded))
            continue;
        seen.insert(folded);
        result.push_back(value);
        if (result.size() >= count)
            break;
    }
    return result;
}

QStringList stringList(const QJsonValue &value, int limit)
{
    QStringList result;
    for (const auto &entry : value.toArray())
    {
        if (entry.isString())
            result.push_back(entry.toString());
        if (result.size() >= limit)
            break;
    }
    return result;
}

QJsonObject baseJson(const RuleBase &rule)
{
    return {{"enabled", rule.enabled},
            {"skipSubscribers", rule.skipSubscribers},
            {"action", actionKey(rule.action)},
            {"timeout", rule.timeout}};
}

void readBase(RuleBase &rule, const QJsonObject &json)
{
    rule.enabled = json.value("enabled").toBool(rule.enabled);
    rule.skipSubscribers =
        json.value("skipSubscribers").toBool(rule.skipSubscribers);
    if (json.contains("action"))
        rule.action = actionFromKey(json.value("action").toString());
    rule.timeout = json.value("timeout").toInt(rule.timeout);
}

QJsonObject windowJson(const WindowRule &rule)
{
    auto json = baseJson(rule);
    json.insert("count", rule.count);
    json.insert("window", rule.window);
    return json;
}

void readWindow(WindowRule &rule, const QJsonObject &json)
{
    readBase(rule, json);
    rule.count = json.value("count").toInt(rule.count);
    rule.window = json.value("window").toInt(rule.window);
}

void clampBase(RuleBase &rule)
{
    rule.timeout = std::clamp(rule.timeout, 1, MAX_TIMEOUT);
}

void clampWindow(WindowRule &rule, int minCount, int maxCount, int maxWindow)
{
    clampBase(rule);
    rule.count = std::clamp(rule.count, minCount, maxCount);
    rule.window = std::clamp(rule.window, 2, maxWindow);
}

bool isInvisible(char32_t ch)
{
    return ch == 0x00AD || ch == 0x034F || ch == 0x061C || ch == 0x115F ||
           ch == 0x1160 || ch == 0x17B4 || ch == 0x17B5 || ch == 0x180E ||
           (ch >= 0x200B && ch <= 0x200F) || (ch >= 0x202A && ch <= 0x202E) ||
           (ch >= 0x2060 && ch <= 0x206F) || ch == 0x3164 || ch == 0xFEFF ||
           ch == 0xFFA0 || (ch >= 0xFE00 && ch <= 0xFE0F) ||
           (ch >= 0xE0000 && ch <= 0xE007F) || (ch >= 0x1D173 && ch <= 0x1D17A);
}

QChar leetLetter(QChar ch)
{
    switch (ch.unicode())
    {
        case u'0':
            return u'o';
        case u'1':
        case u'!':
        case u'|':
            return u'i';
        case u'3':
            return u'e';
        case u'4':
        case u'@':
            return u'a';
        case u'5':
        case u'$':
            return u's';
        case u'7':
        case u'+':
            return u't';
        case u'8':
            return u'b';
        case u'9':
            return u'g';
        default:
            return {};
    }
}

bool stretched(const QString &word)
{
    for (qsizetype i = 2; i < word.size(); ++i)
        if (word[i] == word[i - 1] && word[i] == word[i - 2])
            return true;
    return false;
}

/// Values for the fields presets control, in one comparable object.
QJsonObject presetFields(const ReviewRules &rules)
{
    const auto window = [](const WindowRule &rule) {
        return QJsonArray{rule.enabled, rule.count, rule.window};
    };
    return {
        {"bursts", window(rules.bursts)},
        {"repeats", window(rules.repeats)},
        {"links", window(rules.links)},
        {"coordinated", window(rules.coordinated)},
        {"platform", rules.platformAlerts.enabled},
        {"caps", QJsonArray{rules.caps.enabled, rules.caps.percent,
                            rules.caps.minLength}},
        {"symbols", QJsonArray{rules.symbols.enabled, rules.symbols.percent,
                               rules.symbols.minLength}},
        {"emotes", QJsonArray{rules.emotes.enabled, rules.emotes.limit}},
        {"walls", QJsonArray{rules.walls.enabled, rules.walls.length,
                             rules.walls.artPercent}},
        {"mentions", QJsonArray{rules.mentions.enabled, rules.mentions.limit}},
        {"unusual", rules.unusual.enabled},
        {"first", rules.firstMessages.enabled},
        {"age", QJsonArray{rules.newAccounts.enabled, rules.newAccounts.limit}},
        {"returning", rules.returning.enabled},
    };
}
}  // namespace

ReviewRules::ReviewRules()
{
    this->platformAlerts.enabled = true;
    this->escalation = {{
        {RuleAction::Delete, 60},
        {RuleAction::Timeout, 60},
        {RuleAction::Timeout, 600},
        {RuleAction::Flag, 600},
    }};
    this->applyPreset(RulePreset::Standard);
}

void ReviewRules::applyPreset(RulePreset preset)
{
    const auto set = [](WindowRule &rule, bool enabled, int count, int window) {
        rule.enabled = enabled;
        rule.count = count;
        rule.window = window;
    };
    const auto share = [](ShareRule &rule, bool enabled, int percent,
                          int minLength) {
        rule.enabled = enabled;
        rule.percent = percent;
        rule.minLength = minLength;
    };
    const auto limit = [](LimitRule &rule, bool enabled, int value) {
        rule.enabled = enabled;
        rule.limit = value;
    };
    const auto wall = [this](bool enabled, int length, int artPercent) {
        this->walls.enabled = enabled;
        this->walls.length = length;
        this->walls.artPercent = artPercent;
    };
    this->platformAlerts.enabled = true;
    switch (preset)
    {
        case RulePreset::Relaxed:
            set(this->bursts, true, 12, 10);
            set(this->repeats, true, 6, 30);
            set(this->links, true, 5, 30);
            set(this->coordinated, false, 4, 15);
            share(this->caps, false, 80, 20);
            share(this->symbols, false, 60, 20);
            limit(this->emotes, false, 20);
            wall(false, 500, 50);
            limit(this->mentions, false, 8);
            this->unusual.enabled = false;
            this->firstMessages.enabled = false;
            limit(this->newAccounts, false, 3);
            this->returning.enabled = false;
            break;
        case RulePreset::Standard:
            set(this->bursts, true, 8, 10);
            set(this->repeats, true, 4, 20);
            set(this->links, true, 3, 30);
            set(this->coordinated, false, 4, 15);
            share(this->caps, false, 70, 15);
            share(this->symbols, false, 50, 15);
            limit(this->emotes, false, 12);
            wall(false, 350, 40);
            limit(this->mentions, false, 5);
            this->unusual.enabled = false;
            this->firstMessages.enabled = false;
            limit(this->newAccounts, false, 7);
            this->returning.enabled = false;
            break;
        case RulePreset::Strict:
            set(this->bursts, true, 6, 10);
            set(this->repeats, true, 3, 30);
            set(this->links, true, 2, 60);
            set(this->coordinated, true, 3, 15);
            share(this->caps, true, 70, 15);
            share(this->symbols, true, 50, 15);
            limit(this->emotes, true, 12);
            wall(true, 300, 40);
            limit(this->mentions, true, 4);
            this->unusual.enabled = true;
            this->firstMessages.enabled = false;
            limit(this->newAccounts, true, 7);
            this->returning.enabled = true;
            break;
        case RulePreset::Raid:
            set(this->bursts, true, 4, 10);
            set(this->repeats, true, 2, 30);
            set(this->links, true, 1, 60);
            set(this->coordinated, true, 3, 20);
            share(this->caps, true, 60, 12);
            share(this->symbols, true, 40, 12);
            limit(this->emotes, true, 10);
            wall(true, 250, 30);
            limit(this->mentions, true, 3);
            this->unusual.enabled = true;
            this->firstMessages.enabled = true;
            limit(this->newAccounts, true, 30);
            this->returning.enabled = true;
            break;
    }
}

std::optional<RulePreset> ReviewRules::matchingPreset() const
{
    const auto current = presetFields(*this);
    for (const auto preset : {RulePreset::Relaxed, RulePreset::Standard,
                              RulePreset::Strict, RulePreset::Raid})
    {
        auto candidate = *this;
        candidate.applyPreset(preset);
        if (presetFields(candidate) == current)
            return preset;
    }
    return std::nullopt;
}

void ReviewRules::normalize()
{
    clampWindow(this->bursts, 2, 100, 120);
    clampWindow(this->repeats, 2, 30, 300);
    clampWindow(this->links, 1, 30, 300);
    clampWindow(this->coordinated, 3, 30, 120);
    this->links.allowed = boundedList(this->links.allowed, MAX_DOMAINS, 100);
    this->links.blocked = boundedList(this->links.blocked, MAX_DOMAINS, 100);
    for (auto *rule : {&this->caps, &this->symbols})
    {
        clampBase(*rule);
        rule->percent = std::clamp(rule->percent, 20, 100);
        rule->minLength = std::clamp(rule->minLength, 5, 500);
    }
    clampBase(this->emotes);
    this->emotes.limit = std::clamp(this->emotes.limit, 2, 100);
    clampBase(this->walls);
    this->walls.length = std::clamp(this->walls.length, 100, 2000);
    this->walls.artPercent = std::clamp(this->walls.artPercent, 10, 100);
    clampBase(this->mentions);
    this->mentions.limit = std::clamp(this->mentions.limit, 2, 30);
    clampBase(this->newAccounts);
    this->newAccounts.limit = std::clamp(this->newAccounts.limit, 1, 365);
    for (auto *rule : {&this->platformAlerts, &this->unusual,
                       &this->firstMessages, &this->returning})
        clampBase(*rule);
    clampBase(this->names);
    this->names.patterns =
        boundedList(this->names.patterns, MAX_PATTERNS, MAX_TERM_LENGTH);
    if (this->lists.size() > MAX_LISTS)
        this->lists.resize(MAX_LISTS);
    for (auto &list : this->lists)
    {
        clampBase(list);
        list.name = list.name.trimmed().left(60);
        if (list.name.isEmpty())
            list.name = "Word list";
        list.terms = boundedList(list.terms, MAX_TERMS, MAX_TERM_LENGTH);
        list.source = list.source.trimmed().left(500);
    }
    QStringList allowed;
    for (const auto &line : this->allowList)
    {
        if (allowed.size() >= MAX_ALLOWED)
            break;
        if (auto entry = AllowEntry::parse(line))
            allowed.append(entry->toString());
    }
    this->allowList = boundedList(allowed, MAX_ALLOWED, 200);
    for (auto &step : this->escalation)
    {
        if (step.action == RuleAction::Escalate)
            step.action = RuleAction::Flag;
        step.timeout = std::clamp(step.timeout, 1, MAX_TIMEOUT);
    }
    auto &lockdown = this->lockdown;
    lockdown.followersMinutes = std::clamp(lockdown.followersMinutes, 0, 129600);
    lockdown.slowSeconds = std::clamp(lockdown.slowSeconds, 3, 120);
}

QJsonObject ReviewRules::toJson() const
{
    auto repeats = windowJson(this->repeats);
    repeats.insert("inARow", this->repeats.inARow);
    auto links = windowJson(this->links);
    links.insert("allowed", QJsonArray::fromStringList(this->links.allowed));
    links.insert("blocked", QJsonArray::fromStringList(this->links.blocked));
    const auto share = [](const ShareRule &rule) {
        auto json = baseJson(rule);
        json.insert("percent", rule.percent);
        json.insert("minLength", rule.minLength);
        return json;
    };
    const auto limit = [](const LimitRule &rule) {
        auto json = baseJson(rule);
        json.insert("limit", rule.limit);
        return json;
    };
    auto walls = baseJson(this->walls);
    walls.insert("length", this->walls.length);
    walls.insert("artPercent", this->walls.artPercent);
    auto names = baseJson(this->names);
    names.insert("patterns", QJsonArray::fromStringList(this->names.patterns));
    QJsonArray lists;
    for (const auto &list : this->lists)
    {
        auto json = baseJson(list);
        json.insert("name", list.name);
        json.insert("wholeWords", list.wholeWords);
        json.insert("terms", QJsonArray::fromStringList(list.terms));
        json.insert("source", list.source);
        lists.append(json);
    }
    QJsonArray escalation;
    for (const auto &step : this->escalation)
        escalation.append(QJsonObject{{"action", actionKey(step.action)},
                                      {"timeout", step.timeout}});
    const auto &lockdown = this->lockdown;
    return {
        {"version", 2},
        {"enabled", this->enabled},
        {"automaticActions", this->automaticActions},
        {"excludeTrusted", this->excludeTrusted},
        {"bursts", windowJson(this->bursts)},
        {"repeats", repeats},
        {"links", links},
        {"coordinated", windowJson(this->coordinated)},
        {"platformAlerts", baseJson(this->platformAlerts)},
        {"caps", share(this->caps)},
        {"symbols", share(this->symbols)},
        {"emotes", limit(this->emotes)},
        {"walls", walls},
        {"mentions", limit(this->mentions)},
        {"unusual", baseJson(this->unusual)},
        {"firstMessages", baseJson(this->firstMessages)},
        {"newAccounts", limit(this->newAccounts)},
        {"names", names},
        {"returning", baseJson(this->returning)},
        {"lists", lists},
        {"allowList", QJsonArray::fromStringList(this->allowList)},
        {"escalation", escalation},
        {"lockdown",
         QJsonObject{{"followersOnly", lockdown.followersOnly},
                     {"followersMinutes", lockdown.followersMinutes},
                     {"slowMode", lockdown.slowMode},
                     {"slowSeconds", lockdown.slowSeconds},
                     {"emoteOnly", lockdown.emoteOnly},
                     {"subscribersOnly", lockdown.subscribersOnly},
                     {"shieldMode", lockdown.shieldMode},
                     {"raidRules", lockdown.raidRules}}},
    };
}

ReviewRules ReviewRules::fromJson(const QJsonObject &json)
{
    ReviewRules result;
    result.enabled = json.value("enabled").toBool(result.enabled);
    result.excludeTrusted =
        json.value("excludeTrusted").toBool(result.excludeTrusted);

    if (json.value("bursts").isBool())
    {
        // Flat format saved before profiles existed
        const auto flag = [&](const char *key, bool fallback) {
            return json.value(key).toBool(fallback);
        };
        const auto number = [&](const char *key, int fallback) {
            return json.value(key).toInt(fallback);
        };
        result.bursts.enabled = flag("bursts", true);
        result.bursts.count = number("burstCount", result.bursts.count);
        result.bursts.window = number("burstSeconds", result.bursts.window);
        result.repeats.enabled = flag("repeats", true);
        result.repeats.count = number("repeatCount", result.repeats.count);
        result.repeats.window = number("repeatSeconds", result.repeats.window);
        result.links.enabled = flag("links", true);
        result.links.count = number("linkCount", result.links.count);
        result.links.window = number("linkSeconds", result.links.window);
        result.coordinated.enabled = flag("coordinated", false);
        result.coordinated.count =
            number("coordinatedCount", result.coordinated.count);
        result.coordinated.window =
            number("coordinatedSeconds", result.coordinated.window);
        result.platformAlerts.enabled = flag("platformAlerts", true);
        const auto terms = stringList(json.value("terms"), MAX_TERMS);
        if (!terms.isEmpty() || flag("termsEnabled", false))
        {
            WordList list;
            list.name = "Terms";
            list.enabled = flag("termsEnabled", false);
            list.wholeWords = flag("wholeWords", true);
            list.terms = terms;
            result.lists.push_back(list);
        }
        result.allowList = stringList(json.value("excludedUsers"), MAX_ALLOWED);
        result.normalize();
        return result;
    }

    result.automaticActions =
        json.value("automaticActions").toBool(result.automaticActions);
    const auto object = [&](const char *key) {
        return json.value(key).toObject();
    };
    readWindow(result.bursts, object("bursts"));
    readWindow(result.repeats, object("repeats"));
    result.repeats.inARow =
        object("repeats").value("inARow").toBool(result.repeats.inARow);
    readWindow(result.links, object("links"));
    result.links.allowed =
        stringList(object("links").value("allowed"), MAX_DOMAINS);
    result.links.blocked =
        stringList(object("links").value("blocked"), MAX_DOMAINS);
    readWindow(result.coordinated, object("coordinated"));
    readBase(result.platformAlerts, object("platformAlerts"));
    for (auto [rule, key] : {std::pair{&result.caps, "caps"},
                             std::pair{&result.symbols, "symbols"}})
    {
        const auto value = object(key);
        readBase(*rule, value);
        rule->percent = value.value("percent").toInt(rule->percent);
        rule->minLength = value.value("minLength").toInt(rule->minLength);
    }
    for (auto [rule, key] : {std::pair{&result.emotes, "emotes"},
                             std::pair{&result.mentions, "mentions"},
                             std::pair{&result.newAccounts, "newAccounts"}})
    {
        const auto value = object(key);
        readBase(*rule, value);
        rule->limit = value.value("limit").toInt(rule->limit);
    }
    readBase(result.walls, object("walls"));
    result.walls.length =
        object("walls").value("length").toInt(result.walls.length);
    result.walls.artPercent =
        object("walls").value("artPercent").toInt(result.walls.artPercent);
    readBase(result.unusual, object("unusual"));
    readBase(result.firstMessages, object("firstMessages"));
    readBase(result.returning, object("returning"));
    readBase(result.names, object("names"));
    result.names.patterns =
        stringList(object("names").value("patterns"), MAX_PATTERNS);
    for (const auto &value : json.value("lists").toArray())
    {
        if (result.lists.size() >= MAX_LISTS)
            break;
        const auto entry = value.toObject();
        WordList list;
        readBase(list, entry);
        list.name = entry.value("name").toString();
        list.wholeWords = entry.value("wholeWords").toBool(true);
        list.terms = stringList(entry.value("terms"), MAX_TERMS);
        list.source = entry.value("source").toString();
        result.lists.push_back(list);
    }
    result.allowList = stringList(json.value("allowList"), MAX_ALLOWED);
    const auto escalation = json.value("escalation").toArray();
    for (qsizetype i = 0;
         i < std::min<qsizetype>(escalation.size(), result.escalation.size());
         ++i)
    {
        const auto step = escalation[i].toObject();
        result.escalation[i] = {actionFromKey(step.value("action").toString()),
                                step.value("timeout").toInt(600)};
    }
    const auto lockdown = object("lockdown");
    auto &locked = result.lockdown;
    locked.followersOnly =
        lockdown.value("followersOnly").toBool(locked.followersOnly);
    locked.followersMinutes =
        lockdown.value("followersMinutes").toInt(locked.followersMinutes);
    locked.slowMode = lockdown.value("slowMode").toBool(locked.slowMode);
    locked.slowSeconds =
        lockdown.value("slowSeconds").toInt(locked.slowSeconds);
    locked.emoteOnly = lockdown.value("emoteOnly").toBool(locked.emoteOnly);
    locked.subscribersOnly =
        lockdown.value("subscribersOnly").toBool(locked.subscribersOnly);
    locked.shieldMode = lockdown.value("shieldMode").toBool(locked.shieldMode);
    locked.raidRules = lockdown.value("raidRules").toBool(locked.raidRules);
    result.normalize();
    return result;
}

QString presetName(RulePreset preset)
{
    switch (preset)
    {
        case RulePreset::Relaxed:
            return "Relaxed";
        case RulePreset::Standard:
            return "Standard";
        case RulePreset::Strict:
            return "Strict";
        case RulePreset::Raid:
            return "Raid";
    }
    return {};
}

QString durationLabel(int seconds)
{
    for (const auto &[unit, length] :
         {std::pair{"w", 604800}, std::pair{"d", 86400}, std::pair{"h", 3600},
          std::pair{"m", 60}})
        if (seconds >= length && seconds % length == 0)
            return QString::number(seconds / length) + unit;
    return QString::number(seconds) + "s";
}

QString actionLabel(RuleAction action, int timeout)
{
    switch (action)
    {
        case RuleAction::Flag:
            return "Flag for review";
        case RuleAction::Delete:
            return "Delete message";
        case RuleAction::Warn:
            return "Warn";
        case RuleAction::Timeout:
            return timeout > 0 ? "Timeout " + durationLabel(timeout)
                               : QString("Timeout");
        case RuleAction::Ban:
            return "Ban";
        case RuleAction::Escalate:
            return "Escalate";
    }
    return {};
}

QString ruleTitle(const QString &code)
{
    static const QHash<QString, QString> titles{
        {"burst", "Message bursts"},
        {"repeat", "Repeated messages"},
        {"links", "Frequent links"},
        {"blockedlink", "Blocked links"},
        {"coordinated", "Same text, many users"},
        {"platform", "Platform flags"},
        {"caps", "Caps spam"},
        {"symbols", "Symbol spam"},
        {"emotes", "Emote spam"},
        {"wall", "Walls of text"},
        {"mentions", "Mention spam"},
        {"unusual", "Unusual characters"},
        {"first", "First-time chatters"},
        {"newaccount", "New accounts"},
        {"name", "Username patterns"},
        {"returning", "Returning offenders"},
        {"manual", "Flagged by you"},
    };
    if (code.startsWith("list:"))
        return code.mid(5);
    return titles.value(code, code);
}

int ruleWeight(const QString &code)
{
    if (code.startsWith("list:") || code == "platform" ||
        code == "blockedlink")
        return 3;
    if (code == "coordinated" || code == "returning" || code == "newaccount" ||
        code == "name" || code == "manual")
        return 2;
    return 1;
}

bool actionSupported(RuleAction action, MessagePlatform platform)
{
    if (action == RuleAction::Flag || action == RuleAction::Escalate)
        return true;
    if (platform == MessagePlatform::TikTok)
        return false;
    if (action == RuleAction::Warn)
        return platform == MessagePlatform::AnyOrTwitch;
    return true;
}

QStringList parseTermList(const QString &text, const QStringList &tags)
{
    const auto trimmed = text.trimmed();
    QStringList terms;
    if (trimmed.startsWith('[') || trimmed.startsWith('{'))
    {
        const auto document = QJsonDocument::fromJson(trimmed.toUtf8());
        const auto wanted = [&](const QJsonObject &entry) {
            if (tags.isEmpty())
                return true;
            for (const auto &tag : entry.value("tags").toArray())
                if (tags.contains(tag.toString(), Qt::CaseInsensitive))
                    return true;
            return false;
        };
        const auto readArray = [&](const QJsonArray &array) {
            for (const auto &value : array)
            {
                if (value.isString() && tags.isEmpty())
                    terms.append(value.toString());
                // Lists like {"match": "word|variant", "tags": [...]}
                else if (value.isObject() && wanted(value.toObject()))
                    for (const auto *key : {"match", "word", "term"})
                        if (value.toObject().value(key).isString())
                        {
                            terms.append(value.toObject()
                                             .value(key)
                                             .toString()
                                             .split('|', Qt::SkipEmptyParts));
                            break;
                        }
            }
        };
        if (document.isArray())
            readArray(document.array());
        else if (document.isObject())
            for (const auto &value : document.object())
                if (value.isArray())
                {
                    readArray(value.toArray());
                    break;
                }
        if (!document.isNull())
            return boundedList(terms, MAX_TERMS, MAX_TERM_LENGTH);
    }
    for (auto line : text.split('\n'))
    {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        terms.append(line);
    }
    return boundedList(terms, MAX_TERMS, MAX_TERM_LENGTH);
}

const std::vector<SuggestedList> &suggestedLists()
{
    static const QString profanity =
        "https://raw.githubusercontent.com/dsojevic/profanity-list/main/"
        "en.json";
    static const std::vector<SuggestedList> lists{
        {"Slurs: racial and LGBTQ (dsojevic/profanity-list)", "Slurs",
         profanity + "#tags=racial,lgbtq"},
        {"General profanity (dsojevic/profanity-list)", "Profanity",
         profanity + "#tags=general"},
        {"Broad English bad-word list (LDNOOBW)", "Bad words",
         "https://raw.githubusercontent.com/LDNOOBW/"
         "List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words/master/en"},
    };
    return lists;
}

std::pair<QString, QStringList> splitListSource(const QString &source)
{
    const auto marker = source.indexOf("#tags=");
    if (marker < 0)
        return {source.trimmed(), {}};
    return {source.left(marker).trimmed(),
            source.mid(marker + 6).split(',', Qt::SkipEmptyParts)};
}

std::optional<MessagePlatform> platformFromName(const QString &name)
{
    const auto key = name.trimmed().toCaseFolded();
    if (key == "twitch")
        return MessagePlatform::AnyOrTwitch;
    if (key == "kick")
        return MessagePlatform::Kick;
    if (key == "youtube")
        return MessagePlatform::YouTube;
    if (key == "tiktok")
        return MessagePlatform::TikTok;
    return std::nullopt;
}

QString platformKey(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return "kick";
        case MessagePlatform::YouTube:
            return "youtube";
        case MessagePlatform::TikTok:
            return "tiktok";
        case MessagePlatform::AnyOrTwitch:
        default:
            return "twitch";
    }
}

std::optional<AllowEntry> AllowEntry::parse(const QString &line)
{
    static const QRegularExpression pattern(
        R"(^(?:([a-z]+):)?@?([^\s#:]*)\s*(?:#\s*(\S+))?$)",
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(line.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    AllowEntry entry;
    if (match.hasCaptured(1) && !match.captured(1).isEmpty())
    {
        entry.platform = platformFromName(match.captured(1));
        if (!entry.platform)
            return std::nullopt;
    }
    entry.name = match.captured(2).toCaseFolded().left(100);
    entry.id = match.captured(3).left(200);
    if (entry.name.isEmpty() && entry.id.isEmpty())
        return std::nullopt;
    return entry;
}

QString AllowEntry::toString() const
{
    QString result;
    if (this->platform)
        result = platformKey(*this->platform) + ':';
    result += this->name;
    if (!this->id.isEmpty())
        result += (this->name.isEmpty() ? "#" : " #") + this->id;
    return result;
}

QString cleanForMatching(const QString &text)
{
    const auto decomposed =
        text.normalized(QString::NormalizationForm_KD).toUcs4();
    QList<char32_t> kept;
    kept.reserve(decomposed.size());
    for (const auto ch : decomposed)
    {
        const auto category = QChar::category(ch);
        if (category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_Enclosing ||
            category == QChar::Mark_SpacingCombining || isInvisible(ch))
            continue;
        kept.append(ch);
    }
    return QString::fromUcs4(kept.constData(), kept.size()).toCaseFolded();
}

int hiddenCharacterCount(const QString &text)
{
    const auto characters = text.toUcs4();
    return int(std::count_if(characters.begin(), characters.end(),
                             [](char32_t ch) {
                                 return isInvisible(ch);
                             }));
}

int markStackLength(const QString &text)
{
    int longest = 0;
    int current = 0;
    for (const auto ch : text.toUcs4())
    {
        const auto category = QChar::category(ch);
        if (category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_Enclosing)
            longest = std::max(longest, ++current);
        else
            current = 0;
    }
    return longest;
}

QString normalizeWord(const QString &token)
{
    // Edges keep only what can start or end a disguised word: "$lut", "h3ll0"
    const auto edge = [](QChar ch) {
        return ch.isLetterOrNumber() || ch == u'$' || ch == u'@';
    };
    qsizetype start = 0;
    qsizetype end = token.size();
    while (start < end && !edge(token[start]))
        ++start;
    while (end > start && !edge(token[end - 1]))
        --end;
    auto core = QStringView(token).mid(start, end - start);
    if (core.startsWith(u'@'))
        core = core.mid(1);  // A mention
    const bool hasLetter = std::any_of(core.begin(), core.end(), [](QChar ch) {
        return ch.isLetter();
    });
    QString result;
    result.reserve(core.size());
    for (const auto ch : core)
    {
        if (ch.isLetter())
            result.append(ch);
        else if (hasLetter && !leetLetter(ch).isNull())
            result.append(leetLetter(ch));
        else if (ch.isDigit())
            result.append(ch);
    }
    return result;
}

QString collapseRepeats(const QString &word)
{
    QString result;
    result.reserve(word.size());
    for (const auto ch : word)
        if (result.isEmpty() || result.back() != ch)
            result.append(ch);
    return result;
}

QStringList matchingWords(const QString &text)
{
    static const QRegularExpression space(R"(\s+)");
    QStringList words;
    QStringList letters;
    // "f u c k" is one word written with spaces
    const auto flush = [&] {
        if (letters.size() >= 3)
            words.append(letters.join(QString()));
        else
            words.append(letters);
        letters.clear();
    };
    for (const auto &token :
         cleanForMatching(text).split(space, Qt::SkipEmptyParts))
    {
        const auto word = normalizeWord(token);
        if (word.isEmpty())
            continue;
        if (word.size() == 1)
        {
            letters.append(word);
            continue;
        }
        flush();
        words.append(word);
    }
    flush();
    return words;
}

TermMatcher::TermMatcher(const WordList &list)
    : wholeWords_(list.wholeWords)
{
    static const QRegularExpression literal(R"([./:\\])");
    static const QRegularExpression space(R"(\s)");
    for (const auto &term : list.terms)
    {
        const auto clean = cleanForMatching(term.trimmed());
        if (clean.isEmpty())
            continue;
        if (clean.contains(space))
        {
            const auto words = matchingWords(clean);
            if (!words.isEmpty())
                this->phrases_.push_back({' ' + words.join(' ') + ' ', term});
            continue;
        }
        if (clean.contains(literal))
        {
            this->literals_.push_back({clean, term});
            continue;
        }
        const bool leading = clean.startsWith('*');
        const bool trailing = clean.endsWith('*');
        auto core = clean;
        while (core.startsWith('*'))
            core.remove(0, 1);
        while (core.endsWith('*'))
            core.chop(1);
        if (core.contains('*'))
        {
            QStringList parts;
            for (const auto &part : core.split('*', Qt::SkipEmptyParts))
                parts.append(QRegularExpression::escape(normalizeWord(part)));
            this->patterns_.emplace_back(
                QRegularExpression((leading ? "" : "^") + parts.join(".*") +
                                   (trailing ? "" : "$")),
                term);
            continue;
        }
        const auto word = normalizeWord(core);
        if (word.isEmpty())
            continue;
        if ((leading && trailing) || (!leading && !trailing && !list.wholeWords))
            this->fragments_.push_back({word, term});
        else if (trailing)
            this->prefixes_.push_back({word, term});
        else if (leading)
            this->suffixes_.push_back({word, term});
        else
        {
            this->exact_.insert(word, term);
            this->stretched_.insert(collapseRepeats(word), term);
        }
    }
}

bool TermMatcher::empty() const
{
    return this->exact_.isEmpty() && this->prefixes_.empty() &&
           this->suffixes_.empty() && this->fragments_.empty() &&
           this->phrases_.empty() && this->literals_.empty() &&
           this->patterns_.empty();
}

QString TermMatcher::match(const QString &text) const
{
    if (this->empty())
        return {};
    const auto words = matchingWords(text);
    for (const auto &word : words)
    {
        if (auto it = this->exact_.constFind(word); it != this->exact_.cend())
            return *it;
        // Stretched words only match when actually stretched, so a real word
        // that happens to collapse to a listed one ("Niger") doesn't match.
        const bool isStretched = stretched(word);
        const auto collapsed =
            isStretched ? collapseRepeats(word) : QString();
        if (isStretched)
            if (auto it = this->stretched_.constFind(collapsed);
                it != this->stretched_.cend())
                return *it;
        for (const auto &prefix : this->prefixes_)
            if (word.startsWith(prefix.text))
                return prefix.term;
        for (const auto &suffix : this->suffixes_)
            if (word.endsWith(suffix.text))
                return suffix.term;
        for (const auto &fragment : this->fragments_)
            if (word.contains(fragment.text) ||
                (isStretched &&
                 collapsed.contains(collapseRepeats(fragment.text))))
                return fragment.term;
        for (const auto &[pattern, term] : this->patterns_)
            if (pattern.match(word).hasMatch())
                return term;
    }
    if (!this->phrases_.empty())
    {
        const auto joined = ' ' + words.join(' ') + ' ';
        for (const auto &phrase : this->phrases_)
            if (joined.contains(phrase.text))
                return phrase.term;
    }
    if (!this->literals_.empty())
    {
        const auto clean = cleanForMatching(text);
        for (const auto &literal : this->literals_)
            if (clean.contains(literal.text))
                return literal.term;
    }
    return {};
}

}  // namespace chatterino::moderation
