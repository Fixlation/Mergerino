// SPDX-License-Identifier: MIT
#include "controllers/moderation/ModerationProfiles.hpp"

#include <QJsonArray>
#include <QUuid>

#include <algorithm>

namespace chatterino::moderation {
namespace {
constexpr auto DOCUMENT_KIND = "mergerino-modview-profiles";
}  // namespace

ModerationProfiles::ModerationProfiles(const QJsonObject &root)
{
    for (const auto &value : root.value("profiles").toArray())
    {
        const auto object = value.toObject();
        const auto id = object.value("id").toString().trimmed().left(64);
        if (id.isEmpty() || this->find(id) ||
            this->profiles_.size() >= MAX_PROFILES)
            continue;
        this->profiles_.push_back(
            {id, object.value("name").toString().trimmed().left(60),
             ReviewRules::fromJson(object.value("rules").toObject())});
    }
    if (root.contains("chats"))
        this->chats_ = root.value("chats").toObject();
    else
        // Chats used to be stored at the top level, keyed by their scope
        for (auto it = root.begin(); it != root.end(); ++it)
            if (it.key() != "profiles" && it.value().isObject())
                this->chats_.insert(it.key(), it.value());
    this->ensureDefault();
}

ModerationProfile &ModerationProfiles::ensureDefault()
{
    auto it = std::find_if(this->profiles_.begin(), this->profiles_.end(),
                           [](const auto &profile) {
                               return profile.id == DEFAULT_ID;
                           });
    if (it == this->profiles_.end())
    {
        this->profiles_.insert(this->profiles_.begin(),
                               {DEFAULT_ID, "Default", ReviewRules()});
        return this->profiles_.front();
    }
    if (it->name.isEmpty())
        it->name = "Default";
    return *it;
}

QJsonObject ModerationProfiles::toJson() const
{
    QJsonArray profiles;
    for (const auto &profile : this->profiles_)
        profiles.append(QJsonObject{{"id", profile.id},
                                    {"name", profile.name},
                                    {"rules", profile.rules.toJson()}});
    return {{"version", 2}, {"profiles", profiles}, {"chats", this->chats_}};
}

const std::vector<ModerationProfile> &ModerationProfiles::profiles() const
{
    return this->profiles_;
}

const ModerationProfile *ModerationProfiles::find(const QString &id) const
{
    for (const auto &profile : this->profiles_)
        if (profile.id == id)
            return &profile;
    return nullptr;
}

QString ModerationProfiles::uniqueName(const QString &name) const
{
    const auto base = name.trimmed().isEmpty() ? QString("Profile")
                                               : name.trimmed().left(50);
    const auto taken = [this](const QString &candidate) {
        return std::any_of(this->profiles_.begin(), this->profiles_.end(),
                           [&](const auto &profile) {
                               return profile.name.compare(
                                          candidate, Qt::CaseInsensitive) == 0;
                           });
    };
    auto candidate = base;
    for (int i = 2; taken(candidate); ++i)
        candidate = QString("%1 (%2)").arg(base).arg(i);
    return candidate;
}

QString ModerationProfiles::create(const QString &name,
                                   const ReviewRules &rules)
{
    if (this->profiles_.size() >= MAX_PROFILES)
        return {};
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto copy = rules;
    copy.normalize();
    this->profiles_.push_back({id, this->uniqueName(name), copy});
    return id;
}

bool ModerationProfiles::rename(const QString &id, const QString &name)
{
    for (auto &profile : this->profiles_)
        if (profile.id == id)
        {
            if (name.trimmed().isEmpty())
                return false;
            if (profile.name.compare(name.trimmed(), Qt::CaseInsensitive) != 0)
                profile.name = this->uniqueName(name);
            return true;
        }
    return false;
}

bool ModerationProfiles::remove(const QString &id)
{
    if (id == DEFAULT_ID)
        return false;
    const auto before = this->profiles_.size();
    std::erase_if(this->profiles_, [&](const auto &profile) {
        return profile.id == id;
    });
    if (before == this->profiles_.size())
        return false;
    for (auto it = this->chats_.begin(); it != this->chats_.end(); ++it)
    {
        auto chat = it.value().toObject();
        if (chat.value("profile").toString() == id)
        {
            chat.insert("profile", DEFAULT_ID);
            it.value() = chat;
        }
    }
    return true;
}

void ModerationProfiles::setRules(const QString &id, const ReviewRules &rules)
{
    for (auto &profile : this->profiles_)
        if (profile.id == id)
        {
            profile.rules = rules;
            profile.rules.normalize();
        }
}

QJsonObject ModerationProfiles::chat(const QString &scope) const
{
    return this->chats_.value(scope).toObject();
}

void ModerationProfiles::setChat(const QString &scope, const QJsonObject &chat)
{
    if (!scope.isEmpty())
        this->chats_.insert(scope, chat);
}

QString ModerationProfiles::profileFor(const QString &scope,
                                       const QString &chatName)
{
    auto chat = this->chat(scope);
    auto id = chat.value("profile").toString();
    if (!id.isEmpty() && this->find(id))
        return id;
    id = DEFAULT_ID;
    if (chat.contains("rules"))
    {
        const auto old = chat.value("rules").toObject();
        const auto rules = ReviewRules::fromJson(old);
        if (rules.toJson() != ReviewRules().toJson())
        {
            const auto created = this->create(
                chatName.isEmpty() ? QString("Imported rules") : chatName,
                rules);
            if (!created.isEmpty())
                id = created;
        }
        if (!chat.contains("inlineActions"))
            chat.insert("inlineActions",
                        old.value("inlineActions").toBool(true));
        chat.remove("rules");
    }
    if (!scope.isEmpty())
    {
        chat.insert("profile", id);
        this->chats_.insert(scope, chat);
    }
    return id;
}

void ModerationProfiles::setProfileFor(const QString &scope, const QString &id)
{
    if (scope.isEmpty() || !this->find(id))
        return;
    auto chat = this->chat(scope);
    chat.insert("profile", id);
    this->chats_.insert(scope, chat);
}

int ModerationProfiles::usage(const QString &id) const
{
    int count = 0;
    for (const auto &value : this->chats_)
        count += value.toObject().value("profile").toString() == id ? 1 : 0;
    return count;
}

QJsonObject ModerationProfiles::exportDocument(
    const std::vector<ModerationProfile> &profiles)
{
    QJsonArray array;
    for (const auto &profile : profiles)
        array.append(QJsonObject{{"name", profile.name},
                                 {"rules", profile.rules.toJson()}});
    return {{"kind", DOCUMENT_KIND}, {"version", 2}, {"profiles", array}};
}

std::vector<ModerationProfile> ModerationProfiles::importDocument(
    const QJsonObject &document, const QString &fallbackName)
{
    std::vector<ModerationProfile> result;
    const auto read = [&](const QJsonObject &object, const QString &name) {
        if (result.size() < MAX_PROFILES)
            result.push_back({{}, name.trimmed().left(60),
                              ReviewRules::fromJson(object)});
    };
    if (document.value("profiles").isArray())
    {
        for (const auto &value : document.value("profiles").toArray())
        {
            const auto object = value.toObject();
            if (object.value("rules").isObject())
                read(object.value("rules").toObject(),
                     object.value("name").toString(fallbackName));
        }
    }
    else if (document.value("rules").isObject())
        read(document.value("rules").toObject(),
             document.value("name").toString(fallbackName));
    else if (document.contains("bursts") || document.contains("lists"))
        read(document, fallbackName);
    for (auto &profile : result)
        if (profile.name.isEmpty())
            profile.name = fallbackName.isEmpty() ? "Imported" : fallbackName;
    return result;
}

}  // namespace chatterino::moderation
