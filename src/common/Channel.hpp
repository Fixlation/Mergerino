// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/enums/MessageContext.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "messages/LimitedQueue.hpp"
#include "messages/MessageFlag.hpp"
#include "messages/MessageSink.hpp"

#include <magic_enum/magic_enum.hpp>
#include <pajlada/signals/signal.hpp>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>

namespace chatterino {

struct Message;
using MessagePtr = std::shared_ptr<const Message>;
using MessagePtrMut = std::shared_ptr<Message>;

class EmoteMap;

struct PinnedChatMessage {
    enum class Platform : std::uint8_t {
        Twitch,
        Kick,
    };

    Platform platform = Platform::Twitch;
    QString messageID;
    QString messageText;
    QString senderUserID;
    QString senderLogin;
    QString senderDisplayName;
    QString pinnedByDisplayName;
    QDateTime startsAt;
    std::optional<QDateTime> endsAt;
};

class Channel : public std::enable_shared_from_this<Channel>, public MessageSink
{
public:
    // This is for Lua. See scripts/make_luals_meta.py
    /**
     * @exposeenum c2.ChannelType
     */
    enum class Type : std::uint8_t {
        /// This channel may not be backed by a real channel.
        ///
        /// Messages sent to this channel are not logged.
        None,
        /// Direct
        Direct,
        /// Twitch
        Twitch,
        /// TwitchWhispers
        TwitchWhispers,
        /// TwitchWatching
        TwitchWatching,
        /// TwitchMentions
        TwitchMentions,
        /// TwitchLive
        TwitchLive,
        /// TwitchAutomod
        TwitchAutomod,
        /// TwitchEnd
        TwitchEnd,
        Kick,
        Merged,
        /// Misc
        Misc,
    };

    explicit Channel(const QString &name, Type type);
    ~Channel() override;

    // SIGNALS
    pajlada::Signals::Signal<MessagePtr &, std::optional<MessageFlags>>
        messageAppended;
    pajlada::Signals::Signal<std::vector<MessagePtr> &> messagesAddedAtStart;
    /// (index, prev-message, replacement)
    pajlada::Signals::Signal<size_t, const MessagePtr &, const MessagePtr &>
        messageReplaced;
    // Static so adding targeted message removal does not alter Channel's
    // object layout in incremental development builds.
    static pajlada::Signals::Signal<Channel *, size_t, const MessagePtr &>
        messageRemoved;
    // Static so flag-only message updates can propagate without altering
    // Channel's object layout in incremental development builds.
    static pajlada::Signals::Signal<Channel *, const MessagePtr &>
        messageFlagsChanged;
    /// Invoked when some number of messages were filled in using time received
    pajlada::Signals::Signal<const std::vector<MessagePtr> &> filledInMessages;
    pajlada::Signals::NoArgSignal displayNameChanged;
    pajlada::Signals::NoArgSignal messagesCleared;
    pajlada::Signals::Signal<const std::optional<PinnedChatMessage> &>
        pinnedMessageChanged;

    Type getType() const;
    const QString &getName() const;
    virtual const QString &getDisplayName() const;
    virtual const QString &getLocalizedName() const;
    virtual bool isMergedChannel() const;
    bool isTwitchChannel() const;
    bool isKickChannel() const;
    bool isTwitchOrKickChannel() const;
    virtual bool isEmpty() const;

    std::vector<MessagePtr> getMessageSnapshot() const;
    std::vector<MessagePtr> getMessageSnapshot(size_t nItems) const;

    /// Essentially the same as #getMessageSnapshot(size_t), but the returned
    /// vector holds `std::shared_ptr<Message>`. This should only be used in
    /// plugins, because they take messages as `Message` but check that they're
    /// frozen.
    std::vector<MessagePtrMut> getMessageSnapshotMut(size_t nItems) const;

    /// Returns the last message (the one at the bottom). If the channel has no
    /// messages, this will return an empty shared pointer.
    MessagePtr getLastMessage() const;

    // MESSAGES
    // overridingFlags can be filled in with flags that should be used instead
    // of the message's flags. This is useful in case a flag is specific to a
    // type of split
    void addMessage(
        MessagePtr message, MessageContext context,
        std::optional<MessageFlags> overridingFlags = std::nullopt) final;
    void addMessagesAtStart(const std::vector<MessagePtr> &messages_);

    void addSystemMessage(const QString &contents);

    /// Inserts the given messages in order by Message::serverReceivedTime.
    void fillInMissingMessages(const std::vector<MessagePtr> &messages);

    void addOrReplaceTimeout(MessagePtr message, const QDateTime &now) final;
    void addOrReplaceClearChat(MessagePtr message, const QDateTime &now) final;
    void disableAllMessages() final;
    void replaceMessage(const MessagePtr &message,
                        const MessagePtr &replacement);
    void replaceMessage(size_t index, const MessagePtr &replacement);
    void replaceMessage(size_t hint, const MessagePtr &message,
                        const MessagePtr &replacement);
    void removeMessage(const MessagePtr &message);
    MessagePtr disableMessage(const QString &messageID,
                              MessageFlags additionalFlags = {});

    /// Removes all messages from this channel and invokes #messagesCleared
    void clearMessages();

    const std::optional<PinnedChatMessage> &pinnedMessage() const;
    void setPinnedMessage(std::optional<PinnedChatMessage> message);

    MessagePtr findMessageByID(QStringView messageID) final;

    bool hasMessages() const;

    size_t countMessages() const;

    void applySimilarityFilters(const MessagePtr &message) const final;

    MessageSinkTraits sinkTraits() const final;

    // CHANNEL INFO
    virtual bool canSendMessage() const;
    virtual bool isWritable() const;  // whether split input will be usable
    virtual void sendMessage(const QString &message);
    virtual bool isMod() const;
    virtual bool isBroadcaster() const;
    virtual bool hasModRights() const;
    virtual bool hasHighRateLimit() const;
    virtual bool isLive() const;
    virtual bool isRerun() const;
    virtual bool shouldIgnoreHighlights() const;
    virtual bool canReconnect() const;
    virtual void reconnect();
    virtual QString getCurrentStreamID() const;

    static std::shared_ptr<Channel> getEmpty();

    /// Update the user's last message and insert the personal emotes if necessary.
    void upsertPersonalSeventvEmotes(
        const QString &userLogin,
        const std::shared_ptr<const EmoteMap> &emoteMap);

    TabCompletionModel *completionModel;
    QDate lastDate_;

protected:
    virtual void onConnected();
    virtual void messageRemovedFromStart(const MessagePtr &msg);
    virtual QString getCurrentStreamIDForMessage(const Message &message) const;
    QString platform_;

private:
    bool canRecurse() const noexcept;
    void applySessionFirstMessageHighlight(const MessagePtr &message,
                                           MessageContext context);

    const QString name_;
    LimitedQueue<MessagePtr> messages_;
    Type type_;
    bool anythingLogged_ = false;
    std::optional<PinnedChatMessage> pinnedMessage_;
    QHash<QString, QSet<QString>> sessionFirstMessageUsersByStream_;
    QStringList sessionFirstMessageStreamOrder_;

    /// Recursion count for message signals.
    ///
    /// This is intended to prevent _trivial_ infinite recursion of signals
    /// (e.g. unconditionally adding a message in `messageAppended`). It is not
    /// intended to prevent all infinite recursion. That will still crash the
    /// program.
    uint8_t recursionCount_ = 0;

    QTimer clearCompletionModelTimer_;
};

using ChannelPtr = std::shared_ptr<Channel>;

class IndirectChannel
{
    struct Data {
        ChannelPtr channel;
        Channel::Type type;
        pajlada::Signals::NoArgSignal changed;

        Data(ChannelPtr channel, Channel::Type type);
    };

public:
    IndirectChannel(ChannelPtr channel,
                    Channel::Type type = Channel::Type::Direct);

    ChannelPtr get() const;
    void reset(ChannelPtr channel);
    pajlada::Signals::NoArgSignal &getChannelChanged();
    Channel::Type getType() const;

private:
    std::shared_ptr<Data> data_;
};

}  // namespace chatterino
