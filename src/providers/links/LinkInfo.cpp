// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/links/LinkInfo.hpp"

#include "debug/AssertInGuiThread.hpp"

#include <QHash>
#include <QString>

namespace chatterino {

namespace {

struct PreviewMetadata {
    QString title;
    QString subtitle;
    QString siteName;
    QColor accentColor;
};

QHash<const LinkInfo *, PreviewMetadata> &previewMetadata()
{
    static auto *metadata =
        new QHash<const LinkInfo *, PreviewMetadata>;
    return *metadata;
}

}  // namespace

LinkInfo::LinkInfo(QString url)
    : QObject(nullptr)
    , originalUrl_(url)
    , url_(std::move(url))
    , tooltip_(this->url_)
{
}

LinkInfo::~LinkInfo()
{
    previewMetadata().remove(this);
}

LinkInfo::State LinkInfo::state() const
{
    return this->state_;
}

QString LinkInfo::url() const
{
    return this->url_;
}

QString LinkInfo::originalUrl() const
{
    return this->originalUrl_;
}

bool LinkInfo::isPending() const
{
    return this->state_ == State::Created;
}

bool LinkInfo::isLoading() const
{
    return this->state_ == State::Loading;
}

bool LinkInfo::isLoaded() const
{
    return this->state_ > State::Loading;
}

bool LinkInfo::isResolved() const
{
    return this->state_ == State::Resolved;
}

bool LinkInfo::hasError() const
{
    return this->state_ == State::Errored;
}

bool LinkInfo::hasThumbnail() const
{
    return this->thumbnail_ && !this->thumbnail_->url().string.isEmpty();
}

bool LinkInfo::hasPreview() const
{
    const auto it = previewMetadata().constFind(this);
    return it != previewMetadata().cend() && !it->title.isEmpty();
}

QString LinkInfo::previewTitle() const
{
    return previewMetadata().value(this).title;
}

QString LinkInfo::previewSubtitle() const
{
    return previewMetadata().value(this).subtitle;
}

QString LinkInfo::previewSiteName() const
{
    return previewMetadata().value(this).siteName;
}

QColor LinkInfo::previewAccentColor() const
{
    return previewMetadata().value(this).accentColor;
}

QString LinkInfo::tooltip() const
{
    return this->tooltip_;
}

ImagePtr LinkInfo::thumbnail() const
{
    return this->thumbnail_;
}

void LinkInfo::setState(State state)
{
    assertInGuiThread();
    assert(state >= this->state_);

    if (this->state_ == state)
    {
        return;
    }

    this->state_ = state;
    this->stateChanged(state);
}

void LinkInfo::setResolvedUrl(QString resolvedUrl)
{
    assertInGuiThread();
    this->url_ = std::move(resolvedUrl);
}

void LinkInfo::setTooltip(QString tooltip)
{
    assertInGuiThread();
    this->tooltip_ = std::move(tooltip);
}

void LinkInfo::setThumbnail(ImagePtr thumbnail)
{
    assertInGuiThread();
    this->thumbnail_ = std::move(thumbnail);
}

void LinkInfo::setPreview(QString title, QString subtitle, QString siteName,
                          QColor accentColor)
{
    assertInGuiThread();
    previewMetadata().insert(
        this, {.title = std::move(title),
               .subtitle = std::move(subtitle),
               .siteName = std::move(siteName),
               .accentColor = std::move(accentColor)});
}

}  // namespace chatterino
