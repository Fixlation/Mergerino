// SPDX-License-Identifier: MIT
#pragma once

#include "controllers/moderation/ModerationRules.hpp"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace chatterino::moderation {

struct ModerationProfile {
    QString id;
    QString name;
    ReviewRules rules;
};

/// Named rule profiles shared between chats, and what each chat uses. Stored
/// as one JSON document in the settings.
class ModerationProfiles
{
public:
    static constexpr auto DEFAULT_ID = "default";
    static constexpr int MAX_PROFILES = 50;

    explicit ModerationProfiles(const QJsonObject &root = {});
    QJsonObject toJson() const;

    const std::vector<ModerationProfile> &profiles() const;
    const ModerationProfile *find(const QString &id) const;
    /// Adds a profile with a name that isn't taken yet and returns its id.
    QString create(const QString &name, const ReviewRules &rules);
    bool rename(const QString &id, const QString &name);
    /// The default profile can't be removed. Chats using a removed profile
    /// go back to the default.
    bool remove(const QString &id);
    void setRules(const QString &id, const ReviewRules &rules);
    QString uniqueName(const QString &name) const;

    /// Per-chat settings: profile, platforms, tracked users and pane width.
    QJsonObject chat(const QString &scope) const;
    void setChat(const QString &scope, const QJsonObject &chat);
    /// The profile a chat uses. Rules saved by older versions become a
    /// profile named after the chat, unless they were never changed.
    QString profileFor(const QString &scope, const QString &chatName);
    void setProfileFor(const QString &scope, const QString &id);
    int usage(const QString &id) const;

    /// A file holding the given profiles, for sharing.
    static QJsonObject exportDocument(
        const std::vector<ModerationProfile> &profiles);
    /// Reads an exported file, a single profile, or bare rules.
    static std::vector<ModerationProfile> importDocument(
        const QJsonObject &document, const QString &fallbackName);

private:
    ModerationProfile &ensureDefault();

    std::vector<ModerationProfile> profiles_;
    QJsonObject chats_;
};

}  // namespace chatterino::moderation
