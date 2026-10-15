// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace chatterino {

struct MassModerationList {
    QStringList names;
    QStringList invalid;
    QStringList formatted;
    int duplicates = 0;
};

MassModerationList parseMassModerationList(const QString &text, bool kick);

// The executor must check canAct after resolving a name, immediately before
// sending the moderation request. Pause/Stop can happen during that lookup.
class MassModerationQueue final : public QObject
{
public:
    enum class State { Idle, Running, Paused, Stopped, Finished };
    enum class Outcome { Success, Failed, Cancelled, RateLimited, Fatal };
    struct Result {
        Outcome outcome;
        QString message;
    };
    using Guard = std::function<bool()>;
    using Completion = std::function<void(Result)>;
    using Action = std::function<void(const QString &, Guard, Completion)>;

    explicit MassModerationQueue(Action action, QObject *parent = nullptr);
    bool start(QStringList names, int intervalMs);
    void pause();
    void resume();
    void stop();
    void setInterval(int intervalMs);

    State state() const;
    int total() const;
    int completed() const;
    int succeeded() const;
    int failed() const;
    bool inFlight() const;

    std::function<void()> changed;
    std::function<void(const QString &, const Result &)> reported;

private:
    void dispatch();
    void complete(int requestID, const QString &name, Result result);
    void notify();

    Action action_;
    QTimer timer_;
    QStringList names_;
    State state_ = State::Idle;
    int intervalMs_ = 1000;
    int next_ = 0;
    int succeeded_ = 0;
    int failed_ = 0;
    int requestID_ = 0;
    bool inFlight_ = false;
};

}  // namespace chatterino
