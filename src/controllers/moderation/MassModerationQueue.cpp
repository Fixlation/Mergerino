// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "controllers/moderation/MassModerationQueue.hpp"

#include <QPointer>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace chatterino {

MassModerationList parseMassModerationList(const QString &text, bool kick)
{
    MassModerationList result;
    QSet<QString> seen;
    const QRegularExpression separators(QStringLiteral("[,;\\r\\n\\t]+"));
    const QRegularExpression username(kick ? QStringLiteral("^[a-zA-Z0-9_-]+$")
                                           : QStringLiteral("^[a-zA-Z0-9_]+$"));
    for (auto item : text.split(separators, Qt::SkipEmptyParts))
    {
        item = item.trimmed();
        // Accept both a quoted comma-separated list and individually quoted names.
        while (item.startsWith('"') || item.startsWith('\''))
        {
            item.remove(0, 1);
        }
        while (item.endsWith('"') || item.endsWith('\''))
        {
            item.chop(1);
        }
        item = item.trimmed();
        if (item.startsWith('@'))
        {
            item.remove(0, 1);
        }
        if (item.isEmpty())
        {
            continue;
        }
        if (!username.match(item).hasMatch())
        {
            result.invalid.append(item);
            // Formatting must never silently discard invalid entries.
            result.formatted.append(item);
            continue;
        }
        auto name = item.toLower();
        if (seen.contains(name))
        {
            ++result.duplicates;
            continue;
        }
        seen.insert(name);
        result.names.append(name);
        result.formatted.append(name);
    }
    return result;
}

MassModerationQueue::MassModerationQueue(Action action, QObject *parent)
    : QObject(parent)
    , action_(std::move(action))
{
    this->timer_.setSingleShot(true);
    this->timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
        this->dispatch();
    });
}

bool MassModerationQueue::start(QStringList names, int intervalMs)
{
    if (this->state_ != State::Idle || names.isEmpty() || intervalMs < 1)
    {
        return false;
    }
    this->names_ = std::move(names);
    this->intervalMs_ = intervalMs;
    this->state_ = State::Running;
    this->timer_.start(0);
    this->notify();
    return true;
}

void MassModerationQueue::pause()
{
    if (this->state_ == State::Running)
    {
        this->state_ = State::Paused;
        this->timer_.stop();
        this->notify();
    }
}

void MassModerationQueue::resume()
{
    if (this->state_ == State::Paused)
    {
        this->state_ = State::Running;
        if (!this->inFlight_)
        {
            this->timer_.start(this->intervalMs_);
        }
        this->notify();
    }
}

void MassModerationQueue::stop()
{
    if (this->state_ == State::Running || this->state_ == State::Paused)
    {
        this->state_ = State::Stopped;
        this->timer_.stop();
        this->notify();
    }
}

void MassModerationQueue::setInterval(int intervalMs)
{
    this->intervalMs_ = std::max(1, intervalMs);
    if (this->timer_.isActive() && this->state_ == State::Running)
    {
        this->timer_.start(this->intervalMs_);
    }
}

void MassModerationQueue::dispatch()
{
    if (this->state_ != State::Running || this->inFlight_)
    {
        return;
    }
    this->inFlight_ = true;
    this->notify();
    QPointer<MassModerationQueue> self(this);
    const auto name = this->names_.at(this->next_);
    const auto requestID = ++this->requestID_;
    this->action_(
        name,
        [self, requestID] {
            return self && self->state_ == State::Running && self->inFlight_ &&
                   self->requestID_ == requestID;
        },
        [self, name, requestID](Result result) {
            if (!self)
            {
                return;
            }
            // API callbacks may originate outside the UI thread.
            QTimer::singleShot(
                0, self.data(),
                [self, name, requestID, result = std::move(result)]() mutable {
                    if (self)
                    {
                        self->complete(requestID, name, std::move(result));
                    }
                });
        });
}

void MassModerationQueue::complete(int requestID, const QString &name,
                                   Result result)
{
    if (!this->inFlight_ || requestID != this->requestID_)
    {
        return;
    }
    this->inFlight_ = false;
    if (result.outcome == Outcome::Success || result.outcome == Outcome::Failed)
    {
        ++this->next_;
        result.outcome == Outcome::Success ? ++this->succeeded_
                                           : ++this->failed_;
    }
    else if (result.outcome == Outcome::RateLimited &&
             this->state_ != State::Stopped)
    {
        this->state_ = State::Paused;
    }
    else if (result.outcome == Outcome::Fatal)
    {
        this->state_ = State::Stopped;
    }

    if (this->reported && result.outcome != Outcome::Cancelled)
    {
        this->reported(name, result);
    }
    if (this->next_ == this->names_.size() && this->state_ != State::Stopped)
    {
        this->state_ = State::Finished;
    }
    if (this->state_ == State::Running)
    {
        this->timer_.start(this->intervalMs_);
    }
    this->notify();
}

void MassModerationQueue::notify()
{
    if (this->changed)
    {
        this->changed();
    }
}

MassModerationQueue::State MassModerationQueue::state() const
{
    return this->state_;
}
int MassModerationQueue::total() const
{
    return this->names_.size();
}
int MassModerationQueue::completed() const
{
    return this->next_;
}
int MassModerationQueue::succeeded() const
{
    return this->succeeded_;
}
int MassModerationQueue::failed() const
{
    return this->failed_;
}
bool MassModerationQueue::inFlight() const
{
    return this->inFlight_;
}

}  // namespace chatterino
