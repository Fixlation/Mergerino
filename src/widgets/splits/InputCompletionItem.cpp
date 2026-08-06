// SPDX-FileCopyrightText: 2021 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/InputCompletionItem.hpp"

#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"

#include <QSvgRenderer>

#include <algorithm>
#include <utility>

namespace chatterino {

namespace {

constexpr int PLATFORM_ICON_SIZE = 16;
constexpr int PLATFORM_ICON_GAP = 5;
constexpr int ITEM_MARGIN = 4;

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral(":/platforms/kick.svg");
        case MessagePlatform::YouTube:
            return QStringLiteral(":/platforms/youtube.svg");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral(":/platforms/twitch.svg");
    }
}

}  // namespace

InputCompletionItem::InputCompletionItem(
    const EmotePtr &emote, const QString &text, ActionCallback action,
    std::vector<MessagePlatform> platforms,
    PlatformActionCallback platformAction)
    : emote_(emote)
    , text_(text)
    , action_(std::move(action))
    , platforms_(std::move(platforms))
    , platformAction_(std::move(platformAction))
{
}

QString InputCompletionItem::actionText() const
{
    return this->emote_ ? this->emote_->name.string : this->text_;
}

void InputCompletionItem::action()
{
    if (this->platformAction_ && this->platforms_.size() == 1)
    {
        this->platformAction_(this->actionText(), this->platforms_.front());
        return;
    }

    if (this->action_)
    {
        this->action_(this->actionText());
    }
}

std::vector<QRect> InputCompletionItem::platformIconRects(
    const QRect &rect) const
{
    std::vector<QRect> result;
    result.reserve(this->platforms_.size());
    if (this->platforms_.empty())
    {
        return result;
    }

    const int totalWidth =
        static_cast<int>(this->platforms_.size()) * PLATFORM_ICON_SIZE +
        (static_cast<int>(this->platforms_.size()) - 1) * PLATFORM_ICON_GAP;
    int x = rect.right() - ITEM_MARGIN - totalWidth + 1;
    const int y = rect.top() + (rect.height() - PLATFORM_ICON_SIZE) / 2;
    for (size_t i = 0; i < this->platforms_.size(); ++i)
    {
        result.emplace_back(x, y, PLATFORM_ICON_SIZE, PLATFORM_ICON_SIZE);
        x += PLATFORM_ICON_SIZE + PLATFORM_ICON_GAP;
    }
    return result;
}

void InputCompletionItem::actionAt(const QPoint &position, const QRect &rect)
{
    if (this->platformAction_)
    {
        const auto iconRects = this->platformIconRects(rect);
        for (size_t i = 0; i < iconRects.size(); ++i)
        {
            if (iconRects[i].adjusted(-2, -3, 2, 3).contains(position))
            {
                this->platformAction_(this->actionText(), this->platforms_[i]);
                return;
            }
        }
    }

    this->action();
}

void InputCompletionItem::paint(QPainter *painter, const QRect &rect) const
{
    auto margin = ITEM_MARGIN;
    QRect textRect;
    if (this->emote_)
    {
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->setRenderHint(QPainter::Antialiasing);

        auto imageHeight = ICON_SIZE.height() - margin * 2;

        QRect iconRect{
            rect.topLeft() + QPoint{margin, margin},
            QSize{imageHeight, imageHeight},
        };

        if (auto image = this->emote_->images.getImage(2))
        {
            if (auto pixmap = image->pixmapOrLoad())
            {
                if (image->height() != 0)
                {
                    auto aspectRatio =
                        double(image->width()) / double(image->height());

                    iconRect = {
                        rect.topLeft() + QPoint{margin, margin},
                        QSize(int(imageHeight * aspectRatio), imageHeight)};
                    painter->drawPixmap(iconRect, *pixmap);
                }
            }
        }

        textRect =
            QRect(iconRect.topRight() + QPoint{margin, 0},
                  QSize(rect.width() - iconRect.width(), iconRect.height()));
    }
    else
    {
        textRect = QRect(rect.topLeft() + QPoint{margin, 0},
                         QSize(rect.width(), rect.height()));
    }

    const auto iconRects = this->platformIconRects(rect);
    if (!iconRects.empty())
    {
        const int reservedWidth =
            rect.right() - iconRects.front().left() + 1 + ITEM_MARGIN;
        textRect.setWidth(std::max(0, textRect.width() - reservedWidth));

        for (size_t i = 0; i < iconRects.size(); ++i)
        {
            QSvgRenderer renderer(platformIconPath(this->platforms_[i]));
            renderer.setAspectRatioMode(Qt::KeepAspectRatio);
            renderer.render(painter, iconRects[i]);
        }
    }

    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, this->text_);
}

QSize InputCompletionItem::sizeHint(const QRect &rect) const
{
    return QSize(rect.width(), ICON_SIZE.height());
}

}  // namespace chatterino
