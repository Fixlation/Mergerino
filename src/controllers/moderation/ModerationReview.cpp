// SPDX-License-Identifier: MIT
#include "controllers/moderation/ModerationReview.hpp"

#include "messages/Emote.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/TwitchBadge.hpp"

#include <QCryptographicHash>
#include <QRegularExpression>

#include <algorithm>
#include <limits>
#include <utility>

namespace chatterino::moderation {
namespace {
constexpr int MAX_AGE_LOOKUPS = 500;
constexpr int MAX_OFFENDERS = 2000;

/// Duplicates compare without case, accents, punctuation or the invisible
/// characters people add to get around duplicate message filters.
QString repeatText(const QString &text)
{
    QString result;
    for (const auto ch : cleanForMatching(text))
        result.append(ch.isLetterOrNumber() ? ch : QChar(u' '));
    return result.simplified();
}

qint64 strength(RuleAction action, int timeout)
{
    switch (action)
    {
        case RuleAction::Delete:
            return 1;
        case RuleAction::Warn:
            return 2;
        case RuleAction::Timeout:
            return 10 + timeout;
        case RuleAction::Ban:
            return std::numeric_limits<qint64>::max();
        default:
            return 0;
    }
}

QString hostOf(const QString &link)
{
    static const QRegularExpression host(
        R"(^(?:https?://)?(?:www\.)?([^/\s?#:]+))",
        QRegularExpression::CaseInsensitiveOption);
    return host.match(link).captured(1).toCaseFolded();
}

bool domainMatches(const QString &host, const QString &domain)
{
    auto wanted = domain.trimmed().toCaseFolded();
    if (wanted.startsWith("www."))
        wanted.remove(0, 4);
    return !wanted.isEmpty() &&
           (host == wanted || host.endsWith('.' + wanted));
}

bool artCharacter(QChar ch)
{
    const auto code = ch.unicode();
    return (code >= 0x2500 && code <= 0x25FF) ||
           (code >= 0x2800 && code <= 0x28FF);
}
}  // namespace

ReviewInput ReviewInput::fromMessage(const Message &message)
{
    ReviewInput result;
    result.platform = message.platform;
    result.userID = message.userID;
    result.login = message.loginName;
    result.displayName = message.displayName;
    result.color = message.usernameColor;
    result.messageID = message.id;
    result.text = message.messageText;
    result.timestamp = message.serverReceivedTime;
    result.deleted = message.flags.has(MessageFlag::Disabled);
    result.firstMessage = message.flags.has(MessageFlag::FirstMessage);
    result.platformFlagged = message.flags.hasAny(
        {MessageFlag::AutoModOffendingMessage, MessageFlag::RestrictedMessage,
         MessageFlag::MonitoredMessage});
    result.system = message.flags.hasAny(
        {MessageFlag::System, MessageFlag::Whisper,
         MessageFlag::ModerationAction, MessageFlag::Subscription,
         MessageFlag::TikTokJoinMessage});
    if (result.platformFlagged &&
        message.flags.has(MessageFlag::AutoModOffendingMessage))
    {
        result.system = false;
    }
    if (message.flags.has(MessageFlag::Timeout) &&
        !message.flags.has(MessageFlag::ClearChat) &&
        !message.timeoutUser.startsWith("msg:"))
    {
        result.timeoutTarget = message.timeoutUser.trimmed();
    }
    for (const auto &badge : message.twitchBadges)
    {
        if (badge.key_ == "moderator" || badge.key_ == "broadcaster" ||
            badge.key_ == "vip" || badge.key_ == "lead_moderator")
        {
            result.trusted = true;
        }
        if (badge.key_ == "subscriber" || badge.key_ == "founder")
        {
            result.subscriber = true;
        }
    }
    for (const auto &element : message.elements)
    {
        const auto flags = element->getFlags();
        if (flags.hasAny({MessageElementFlag::BadgeChannelAuthority,
                          MessageElementFlag::BadgeGlobalAuthority}))
        {
            result.trusted = true;
        }
        if (flags.has(MessageElementFlag::BadgeSubscription))
        {
            result.subscriber = true;
        }
        if (const auto *emote =
                dynamic_cast<const EmoteElement *>(element.get()))
        {
            ++result.emoteCount;
            if (result.emotes.size() < 100 && emote->getEmote())
                result.emotes.append(emote->getEmote()->name.string);
        }
    }
    return result;
}

QString ModerationReview::platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch:
            return "Twitch";
        case MessagePlatform::Kick:
            return "Kick";
        case MessagePlatform::YouTube:
            return "YouTube";
        case MessagePlatform::TikTok:
            return "TikTok";
    }
    return {};
}

QString ModerationReview::keyFor(const ReviewInput &input)
{
    if (input.login.trimmed().isEmpty() && input.userID.trimmed().isEmpty())
    {
        return {};
    }
    return platformName(input.platform).toLower() + ':' +
           (input.userID.isEmpty()
                ? QStringLiteral("name:") + input.login.trimmed().toCaseFolded()
                : input.userID.trimmed().left(200));
}

QString ModerationReview::notesKey(const ReviewUser &user)
{
    if (user.platform == MessagePlatform::AnyOrTwitch && !user.userID.isEmpty())
    {
        return user.userID;
    }
    return user.key;
}

SimulationResult ModerationReview::simulate(
    const ReviewRules &rules, const std::vector<ReviewInput> &inputs)
{
    SimulationResult result;
    ModerationReview review;
    review.setRules(rules);
    QDateTime start;
    qint64 now = 0;
    for (const auto &input : inputs)
    {
        if (input.timestamp.isValid())
        {
            if (!start.isValid())
                start = input.timestamp;
            now = std::max(now, start.msecsTo(input.timestamp));
        }
        if (!input.system && !input.text.trimmed().isEmpty())
            ++result.messages;
        review.ingest(input, now);
        result.actions += int(review.takeTriggers().size());
    }
    std::vector<const ReviewUser *> flagged;
    for (const auto &state : review.users_)
        if (state.user.pending)
            flagged.push_back(&state.user);
    std::sort(flagged.begin(), flagged.end(), [](auto *a, auto *b) {
        return a->severity != b->severity ? a->severity > b->severity
                                          : a->markedAt > b->markedAt;
    });
    result.flagged = int(flagged.size());
    for (const auto *user : flagged)
    {
        if (result.examples.size() >= 6)
            break;
        QStringList titles;
        for (const auto &code : user->reasons.keys())
            titles.append(ruleTitle(code));
        result.examples.append(user->displayName + ": " + titles.join(", "));
    }
    return result;
}

void ModerationReview::setRules(ReviewRules rules)
{
    rules.normalize();
    this->rules_ = std::move(rules);
    this->matchers_.clear();
    for (const auto &list : this->rules_.lists)
        this->matchers_.emplace_back(list);
    this->allowed_.clear();
    for (const auto &line : this->rules_.allowList)
        if (auto entry = AllowEntry::parse(line))
            this->allowed_.push_back(*entry);
    this->namePatterns_.clear();
    for (const auto &pattern : this->rules_.names.patterns)
        this->namePatterns_.emplace_back(
            QRegularExpression::wildcardToRegularExpression(
                pattern, QRegularExpression::NonPathWildcardConversion),
            QRegularExpression::CaseInsensitiveOption);
    // Changing thresholds starts a new observation window; past messages must
    // not suddenly become a burst under newly enabled rules.
    for (auto &state : this->users_)
    {
        state.arrivals.clear();
        state.repetitions.clear();
        state.links.clear();
        state.streak = 0;
        state.user.namesChecked = false;
    }
    this->groups_.clear();
    this->candidates_.clear();
    ++this->revision_;
}

const ReviewRules &ModerationReview::rules() const
{
    return this->rules_;
}

void ModerationReview::changed(ReviewUser &user)
{
    user.revision = ++this->revision_;
}

ModerationReview::State *ModerationReview::ensureUser(const ReviewInput &input,
                                                      qint64 now)
{
    const auto key = keyFor(input);
    if (key.isEmpty())
    {
        return nullptr;
    }
    auto it = this->users_.find(key);
    if (it == this->users_.end())
    {
        if (this->users_.size() >= MAX_USERS)
        {
            auto oldest = this->lru_.begin();
            while (oldest != this->lru_.end() &&
                   this->users_[*oldest].user.tracked)
            {
                ++oldest;
            }
            if (oldest == this->lru_.end())
            {
                return nullptr;
            }
            auto removed = this->users_.find(*oldest);
            this->pending_ -= removed->user.pending ? 1 : 0;
            this->users_.erase(removed);
            this->lru_.erase(oldest);
        }
        this->lru_.push_back(key);
        State state;
        state.user.key = key;
        state.user.platform = input.platform;
        state.lru = std::prev(this->lru_.end());
        it = this->users_.insert(key, std::move(state));
    }
    this->lru_.splice(this->lru_.end(), this->lru_, it->lru);
    it->user.userID = input.userID.left(200);
    it->user.login = input.login.left(100);
    it->user.displayName =
        (input.displayName.isEmpty() ? input.login : input.displayName)
            .left(100);
    if (input.color.isValid())
    {
        it->user.color = input.color;
    }
    it->user.lastSeen = now;
    it->user.trusted = input.trusted;
    it->user.subscriber = input.subscriber;
    return &*it;
}

bool ModerationReview::excluded(const ReviewInput &input) const
{
    if (input.own || (this->rules_.excludeTrusted && input.trusted))
    {
        return true;
    }
    const auto login = input.login.trimmed().toCaseFolded();
    return std::any_of(
        this->allowed_.begin(), this->allowed_.end(), [&](const auto &entry) {
            if (entry.platform && *entry.platform != input.platform)
                return false;
            if (!entry.id.isEmpty())
                return entry.id == input.userID;
            return entry.name == login;
        });
}

void ModerationReview::recountSeverity(ReviewUser &user)
{
    user.severity = 0;
    for (const auto &code : user.reasons.keys())
        user.severity += ruleWeight(code);
}

void ModerationReview::flag(State &state, const QString &code,
                            const QString &reason, qint64 now)
{
    auto &user = state.user;
    if (user.dismissedAt >= 0 && now - user.dismissedAt < DISMISS_COOLDOWN_MS)
    {
        return;
    }
    user.reasons.insert(code, reason);
    this->recountSeverity(user);
    if (!user.pending)
    {
        user.pending = true;
        user.markedAt = now;
        ++this->pending_;
        user.pendingSequence = ++this->sequence_;
        this->pendingOrder_.emplace_back(user.key, user.pendingSequence);
    }
    // Also bound stale queue references after manual dismissal/re-marking.
    while (this->pending_ > MAX_PENDING ||
           this->pendingOrder_.size() > MAX_USERS * 2)
    {
        const auto [oldest, sequence] = this->pendingOrder_.front();
        this->pendingOrder_.pop_front();
        auto it = this->users_.find(oldest);
        if (it != this->users_.end() && it->user.pending &&
            it->user.pendingSequence == sequence && oldest != user.key)
        {
            it->user.pending = false;
            it->user.reasons.clear();
            it->user.severity = 0;
            --this->pending_;
            this->changed(it->user);
        }
    }
    this->changed(user);
}

void ModerationReview::hit(State &state, const QString &code,
                           const QString &reason, const RuleBase &rule,
                           qint64 now, const QString &title)
{
    if (rule.skipSubscribers && state.user.subscriber)
        return;
    this->flag(state, code, reason, now);
    if (this->rules_.automaticActions && rule.action != RuleAction::Flag)
        this->candidates_[state.user.key].push_back(
            {code, title.isEmpty() ? ruleTitle(code) : title, rule.action,
             rule.timeout});
}

void ModerationReview::resolveCandidates(qint64 now)
{
    for (auto it = this->candidates_.begin(); it != this->candidates_.end();
         ++it)
    {
        auto user = this->users_.find(it.key());
        if (user == this->users_.end())
            continue;
        auto &target = user->user;
        const auto &step =
            this->rules_.escalation[std::min<size_t>(
                target.offences, this->rules_.escalation.size() - 1)];
        std::optional<ReviewTrigger> best;
        qint64 bestStrength = 0;
        for (const auto &candidate : *it)
        {
            const bool escalate = candidate.action == RuleAction::Escalate;
            const auto action = escalate ? step.action : candidate.action;
            const auto timeout = escalate ? step.timeout : candidate.timeout;
            const auto power = strength(action, timeout);
            if (action == RuleAction::Flag || power <= bestStrength)
                continue;
            bestStrength = power;
            best = ReviewTrigger{target.key, candidate.code, candidate.rule,
                                 action,     timeout,        {}};
        }
        if (!best || (now - target.lastActionAt < ACTION_COOLDOWN_MS &&
                      bestStrength <= target.lastActionStrength))
            continue;
        target.lastActionAt = now;
        target.lastActionStrength = bestStrength;
        ++target.offences;
        for (auto message = target.history.rbegin();
             message != target.history.rend(); ++message)
            if (!message->deleted && !message->messageID.isEmpty())
            {
                best->messageID = message->messageID;
                break;
            }
        this->triggers_.push_back(*best);
        if (this->triggers_.size() > MAX_TRIGGERS)
            this->triggers_.erase(this->triggers_.begin());
    }
    this->candidates_.clear();
}

QString ModerationReview::ingest(ReviewInput input, qint64 now, bool evaluate)
{
    if (input.system && !input.timeoutTarget.isEmpty() &&
        this->offenders_.size() < MAX_OFFENDERS)
    {
        this->offenders_.insert(platformName(input.platform).toLower() +
                                ":name:" + input.timeoutTarget.toCaseFolded());
    }
    if (input.system || input.text.trimmed().isEmpty())
    {
        return {};
    }
    input.userID = input.userID.trimmed().left(200);
    input.login = input.login.trimmed().left(100);
    input.text = input.text.left(MAX_TEXT);
    input.messageID = input.messageID.left(300);
    const auto key = keyFor(input);
    if (key.isEmpty())
        return {};
    if (!input.messageID.isEmpty())
    {
        const auto id = platformName(input.platform) + ':' + input.messageID;
        if (this->seenIDs_.contains(id))
            return key;
        this->seenIDs_.insert(id);
        this->seenOrder_.push_back(id);
        while (this->seenOrder_.size() > MAX_HISTORY)
        {
            this->seenIDs_.remove(this->seenOrder_.front());
            this->seenOrder_.pop_front();
        }
    }
    auto *state = this->ensureUser(input, now);
    if (!state)
        return {};
    auto &user = state->user;
    user.history.push_back({++this->sequence_, input.messageID, input.text,
                            input.timestamp, input.deleted});
    this->historyOrder_.emplace_back(key, this->sequence_);
    while (user.history.size() > MAX_USER_HISTORY)
        user.history.pop_front();
    while (this->historyOrder_.size() > MAX_HISTORY)
    {
        const auto [oldKey, sequence] = this->historyOrder_.front();
        this->historyOrder_.pop_front();
        auto old = this->users_.find(oldKey);
        if (old != this->users_.end() && !old->user.history.empty() &&
            old->user.history.front().sequence == sequence)
        {
            old->user.history.pop_front();
            this->changed(old->user);
        }
    }
    this->changed(user);
    if (!evaluate || !this->rules_.enabled || input.deleted ||
        this->excluded(input))
    {
        return key;
    }
    this->evaluate(*state, input, now);
    this->resolveCandidates(now);
    return key;
}

void ModerationReview::evaluate(State &state, const ReviewInput &input,
                                qint64 now)
{
    const auto &rules = this->rules_;
    const auto &key = state.user.key;
    const auto normalized = repeatText(input.text);
    // Observation windows need equality only. Keep a small digest instead of
    // another copy of every message's text for each repetition/group window.
    const auto digest =
        QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256);

    if (rules.bursts.enabled)
    {
        auto &window = state.arrivals;
        while (!window.empty() &&
               now - window.front() > rules.bursts.window * 1000)
            window.pop_front();
        window.push_back(now);
        while (window.size() > 101)
            window.pop_front();
        if (window.size() >= size_t(rules.bursts.count))
            this->hit(state, "burst",
                      QString("%1 messages within %2s")
                          .arg(window.size())
                          .arg(rules.bursts.window),
                      rules.bursts, now);
    }

    state.streak = digest == state.lastDigest ? state.streak + 1 : 1;
    state.lastDigest = digest;
    if (rules.repeats.enabled && normalized.size() >= 3)
    {
        if (rules.repeats.inARow)
        {
            if (state.streak >= rules.repeats.count)
                this->hit(state, "repeat",
                          QString("Same message %1 times in a row")
                              .arg(state.streak),
                          rules.repeats, now);
        }
        else
        {
            auto &window = state.repetitions;
            while (!window.empty() &&
                   now - window.front().first > rules.repeats.window * 1000)
                window.pop_front();
            window.emplace_back(now, digest);
            while (window.size() > 100)
                window.pop_front();
            const auto count = std::count_if(
                window.begin(), window.end(), [&](const auto &entry) {
                    return entry.second == digest;
                });
            if (count >= rules.repeats.count)
                this->hit(state, "repeat",
                          QString("Same message %1 times within %2s")
                              .arg(count)
                              .arg(rules.repeats.window),
                          rules.repeats, now);
        }
    }

    if (rules.links.enabled)
    {
        static const QRegularExpression links(
            R"(\b(?:https?://|www\.)\S+)",
            QRegularExpression::CaseInsensitiveOption);
        const auto folded = input.text.toCaseFolded();
        for (const auto &domain : rules.links.blocked)
        {
            auto wanted = domain.toCaseFolded();
            if (wanted.startsWith("www."))
                wanted.remove(0, 4);
            if (!wanted.isEmpty() && folded.contains(wanted))
            {
                this->hit(state, "blockedlink",
                          QString("Posted a blocked link (%1)").arg(domain),
                          rules.links, now);
                break;
            }
        }
        int count = 0;
        auto matches = links.globalMatch(input.text);
        while (matches.hasNext() && count < 30)
        {
            const auto host = hostOf(matches.next().captured());
            if (std::none_of(rules.links.allowed.begin(),
                             rules.links.allowed.end(), [&](const auto &d) {
                                 return domainMatches(host, d);
                             }))
                ++count;
        }
        auto &window = state.links;
        while (!window.empty() &&
               now - window.front().first > rules.links.window * 1000)
            window.pop_front();
        if (count)
            window.emplace_back(now, count);
        while (window.size() > 100)
            window.pop_front();
        count = 0;
        for (const auto &entry : window)
            count += entry.second;
        if (count >= rules.links.count)
            this->hit(state, "links",
                      QString("%1 links within %2s")
                          .arg(count)
                          .arg(rules.links.window),
                      rules.links, now);
    }

    for (size_t i = 0; i < rules.lists.size() && i < this->matchers_.size();
         ++i)
    {
        const auto &list = rules.lists[i];
        if (!list.enabled)
            continue;
        const auto term = this->matchers_[i].match(input.text);
        if (!term.isEmpty())
            this->hit(state, "list:" + list.name,
                      QString("Matched \"%1\" (%2)").arg(term, list.name),
                      list, now, list.name);
    }

    if (rules.platformAlerts.enabled && input.platformFlagged)
        this->hit(state, "platform", "Flagged by platform moderation",
                  rules.platformAlerts, now);

    // Shouting and symbol counts leave emote names out
    QString words;
    for (const auto &word : input.text.split(' ', Qt::SkipEmptyParts))
        if (!input.emotes.contains(word))
            words += word + ' ';
    if (rules.caps.enabled)
    {
        int letters = 0;
        int upper = 0;
        for (const auto ch : words)
            if (ch.isLetter())
            {
                ++letters;
                upper += ch.isUpper() ? 1 : 0;
            }
        if (letters >= rules.caps.minLength &&
            upper * 100 >= rules.caps.percent * letters)
            this->hit(state, "caps",
                      QString("%1% capital letters").arg(upper * 100 / letters),
                      rules.caps, now);
    }
    if (rules.symbols.enabled)
    {
        int visible = 0;
        int symbols = 0;
        for (const auto ch : words)
            if (!ch.isSpace())
            {
                ++visible;
                symbols += ch.isLetterOrNumber() ? 0 : 1;
            }
        if (visible >= rules.symbols.minLength &&
            symbols * 100 >= rules.symbols.percent * visible)
            this->hit(state, "symbols",
                      QString("%1% symbols").arg(symbols * 100 / visible),
                      rules.symbols, now);
    }
    if (rules.emotes.enabled && input.emoteCount >= rules.emotes.limit)
        this->hit(state, "emotes",
                  QString("%1 emotes in one message").arg(input.emoteCount),
                  rules.emotes, now);
    if (rules.walls.enabled)
    {
        const auto art = std::count_if(input.text.begin(), input.text.end(),
                                       artCharacter);
        const auto visible =
            std::count_if(input.text.begin(), input.text.end(), [](QChar ch) {
                return !ch.isSpace();
            });
        if (input.text.size() >= rules.walls.length)
            this->hit(state, "wall",
                      QString("%1 characters in one message")
                          .arg(input.text.size()),
                      rules.walls, now);
        else if (art >= 20 && art * 100 >= rules.walls.artPercent * visible)
            this->hit(state, "wall", "Looks like ASCII art", rules.walls, now);
    }
    if (rules.mentions.enabled)
    {
        static const QRegularExpression mention(R"((?:^|\s)@(\w+))");
        QSet<QString> mentioned;
        for (auto it = mention.globalMatch(input.text); it.hasNext();)
            mentioned.insert(it.next().captured(1).toCaseFolded());
        if (mentioned.size() >= rules.mentions.limit)
            this->hit(state, "mentions",
                      QString("Mentioned %1 people").arg(mentioned.size()),
                      rules.mentions, now);
    }
    if (rules.unusual.enabled &&
        (markStackLength(input.text) >= 3 ||
         hiddenCharacterCount(input.text) >= 3))
        this->hit(state, "unusual", "Zalgo text or hidden characters",
                  rules.unusual, now);
    if (rules.firstMessages.enabled && input.firstMessage)
        this->hit(state, "first", "First message in this chat",
                  rules.firstMessages, now);

    auto &user = state.user;
    if (rules.names.enabled && !user.namesChecked)
    {
        user.namesChecked = true;
        for (size_t i = 0; i < this->namePatterns_.size(); ++i)
        {
            const auto &pattern = this->namePatterns_[i];
            if (pattern.match(user.login).hasMatch() ||
                pattern.match(user.displayName).hasMatch())
            {
                this->hit(state, "name",
                          QString("Name matches \"%1\"")
                              .arg(rules.names.patterns.value(qsizetype(i))),
                          rules.names, now);
                break;
            }
        }
    }
    if (rules.newAccounts.enabled &&
        user.platform == MessagePlatform::AnyOrTwitch &&
        !user.userID.isEmpty() && !user.ageRequested &&
        !user.accountCreated.isValid() &&
        this->ageLookups_.size() < MAX_AGE_LOOKUPS)
    {
        user.ageRequested = true;
        this->ageLookups_.append(key);
    }
    if (rules.returning.enabled)
    {
        const auto byName = platformName(user.platform).toLower() + ":name:" +
                            user.login.toCaseFolded();
        // Both forms are removed so the user is flagged once per return
        const bool byKey = this->offenders_.remove(key);
        const bool named = this->offenders_.remove(byName);
        if (byKey || named)
            this->hit(state, "returning",
                      "Talking again after a timeout or ban", rules.returning,
                      now);
    }

    if (rules.coordinated.enabled && normalized.size() >= 12 &&
        normalized.contains(' '))
    {
        if (!this->groups_.contains(digest) && this->groups_.size() >= 500)
        {
            auto oldest =
                std::min_element(this->groups_.begin(), this->groups_.end(),
                                 [](const Group &a, const Group &b) {
                                     return a.lastSeen < b.lastSeen;
                                 });
            this->groups_.erase(oldest);
        }
        auto &group = this->groups_[digest];
        group.lastSeen = now;
        while (!group.entries.empty() &&
               now - group.entries.front().time >
                   rules.coordinated.window * 1000)
            group.entries.pop_front();
        group.entries.push_back({now, key});
        while (group.entries.size() > 100)
            group.entries.pop_front();
        QSet<QString> unique;
        for (const auto &entry : group.entries)
            unique.insert(entry.key);
        if (unique.size() >= rules.coordinated.count)
        {
            for (const auto &target : unique)
            {
                auto it = this->users_.find(target);
                if (it != this->users_.end())
                    this->hit(*it, "coordinated",
                              QString("Same text from %1 accounts within %2s")
                                  .arg(unique.size())
                                  .arg(rules.coordinated.window),
                              rules.coordinated, now);
            }
        }
    }
}

void ModerationReview::updateMessage(const ReviewInput &input, bool removed)
{
    auto it = this->users_.find(keyFor(input));
    if (it == this->users_.end() || input.messageID.isEmpty())
        return;
    for (auto &message : it->user.history)
    {
        if (message.messageID == input.messageID)
        {
            message.text = input.text.left(MAX_TEXT);
            message.deleted = removed || input.deleted;
            this->changed(it->user);
        }
    }
}

bool ModerationReview::track(const QString &key, bool tracked)
{
    auto it = this->users_.find(key);
    if (it == this->users_.end() ||
        (tracked && !it->user.tracked && this->tracked_ >= MAX_TRACKED))
        return false;
    if (it->user.tracked != tracked)
    {
        it->user.tracked = tracked;
        this->tracked_ += tracked ? 1 : -1;
        this->changed(it->user);
    }
    return true;
}

bool ModerationReview::mark(const QString &key, qint64 now)
{
    auto it = this->users_.find(key);
    if (it == this->users_.end())
        return false;
    it->user.dismissedAt = -1;
    this->flag(*it, "manual", "Manually marked for review", now);
    return true;
}

bool ModerationReview::dismiss(const QString &key, qint64 now)
{
    auto it = this->users_.find(key);
    if (it == this->users_.end())
        return false;
    if (it->user.pending)
        --this->pending_;
    it->user.pending = false;
    it->user.dismissedAt = now;
    it->user.reasons.clear();
    it->user.severity = 0;
    this->changed(it->user);
    return true;
}

void ModerationReview::dismissAll(qint64 now)
{
    for (const auto &key : this->keys())
        if (this->users_[key].user.pending)
            this->dismiss(key, now);
    this->pendingOrder_.clear();
}

const ReviewUser *ModerationReview::find(const QString &key) const
{
    auto it = this->users_.constFind(key);
    return it == this->users_.cend() ? nullptr : &it->user;
}
QStringList ModerationReview::keys() const
{
    return this->users_.keys();
}
int ModerationReview::pendingCount() const
{
    return this->pending_;
}
int ModerationReview::trackedCount() const
{
    return this->tracked_;
}
quint64 ModerationReview::revision() const
{
    return this->revision_;
}

std::vector<ReviewTrigger> ModerationReview::takeTriggers()
{
    return std::exchange(this->triggers_, {});
}

QStringList ModerationReview::takeAgeLookups()
{
    return std::exchange(this->ageLookups_, {});
}

void ModerationReview::applyAccountCreated(const QString &key,
                                           const QDateTime &created,
                                           qint64 now)
{
    auto it = this->users_.find(key);
    if (it == this->users_.end() || !created.isValid())
        return;
    it->user.accountCreated = created;
    const auto &rule = this->rules_.newAccounts;
    const auto days = created.daysTo(QDateTime::currentDateTimeUtc());
    if (rule.enabled && this->rules_.enabled && days < rule.limit)
    {
        this->hit(*it, "newaccount",
                  days < 1 ? QString("Account created today")
                           : QString("Account created %1 day%2 ago")
                                 .arg(days)
                                 .arg(days == 1 ? "" : "s"),
                  rule, now);
        this->resolveCandidates(now);
    }
    this->changed(it->user);
}

void ModerationReview::noteActioned(const QString &key)
{
    if (this->offenders_.size() < MAX_OFFENDERS)
        this->offenders_.insert(key);
}

QJsonArray ModerationReview::trackedJson() const
{
    QJsonArray result;
    for (const auto &state : this->users_)
    {
        const auto &user = state.user;
        if (user.tracked)
            result.append(QJsonObject{{"platform", int(user.platform)},
                                      {"id", user.userID},
                                      {"login", user.login},
                                      {"name", user.displayName}});
    }
    return result;
}

void ModerationReview::restoreTracked(const QJsonArray &json)
{
    int count = 0;
    for (const auto &value : json)
    {
        if (++count > MAX_TRACKED)
            break;
        const auto obj = value.toObject();
        const auto platform = obj.value("platform").toInt(-1);
        if (platform < 0 || platform > int(MessagePlatform::TikTok))
            continue;
        ReviewInput input;
        input.platform = MessagePlatform(platform);
        input.userID = obj.value("id").toString().trimmed().left(200);
        input.login = obj.value("login").toString().trimmed().left(100);
        input.displayName = obj.value("name").toString().left(100);
        if (auto *state = this->ensureUser(input, 0))
            this->track(state->user.key, true);
    }
}

}  // namespace chatterino::moderation
