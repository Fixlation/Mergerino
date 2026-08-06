// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PinnedMessageBar.hpp"

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "controllers/commands/builtin/PinMessage.hpp"
#include "messages/MessageElement.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "util/IncognitoBrowser.hpp"
#include "widgets/TooltipWidget.hpp"

#include <QAction>
#include <QCursor>
#include <QDesktopServices>
#include <QEvent>
#include <QFontMetrics>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QTextLayout>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace chatterino {
namespace {

constexpr auto OPEN_MENU_ROW_PROPERTY = "mergerinoPinnedOpenMenuRow";
constexpr auto SUPPRESS_MENU_ROW_PROPERTY =
    "mergerinoPinnedSuppressMenuRow";

int menuRowProperty(const QObject *object, const char *name)
{
    const auto value = object->property(name);
    return value.isValid() ? value.toInt() : -1;
}

int scaled(float scale, int value)
{
    return std::max(1, static_cast<int>(std::lround(scale * value)));
}

QString platformName(PinnedChatMessage::Platform platform)
{
    return platform == PinnedChatMessage::Platform::Kick
               ? QStringLiteral("Kick")
               : QStringLiteral("Twitch");
}

QColor platformAccent(PinnedChatMessage::Platform platform)
{
    return platform == PinnedChatMessage::Platform::Kick
               ? QColor(QStringLiteral("#53FC18"))
               : QColor(QStringLiteral("#A970FF"));
}

QString pinnedMessageKey(const ChannelPtr &channel,
                         const PinnedChatMessage &message)
{
    return platformName(message.platform) + QLatin1Char(':') +
           (channel ? channel->getName() : QString{}) + QLatin1Char(':') +
           message.messageID;
}

struct PinnedMessageLink {
    int start = 0;
    int length = 0;
    QString url;
};

std::vector<PinnedMessageLink> findPinnedMessageLinks(const QString &message)
{
    static const QRegularExpression tokenPattern(QStringLiteral("\\S+"));
    std::vector<PinnedMessageLink> links;
    auto matches = tokenPattern.globalMatch(message);
    const QStringView messageView{message};
    while (matches.hasNext())
    {
        const auto match = matches.next();
        const auto token =
            messageView.sliced(match.capturedStart(), match.capturedLength());
        const auto parsed = linkparser::parse(token);
        if (!parsed)
        {
            continue;
        }

        auto url = parsed->link.toString();
        if (parsed->protocol.isNull())
        {
            url.prepend(QStringLiteral("http://"));
        }
        links.push_back({
            static_cast<int>(match.capturedStart() +
                             parsed->prefix(token).size()),
            static_cast<int>(parsed->link.size()),
            std::move(url),
        });
    }
    return links;
}

bool hostMatches(const QString &host, const QString &domain)
{
    return host == domain || host.endsWith(QLatin1Char('.') + domain);
}

QString pinnedLinkTooltip(const QString &url, const LinkInfo *info)
{
    const QUrl parsed(url);
    const auto host = parsed.host().toLower();
    const bool isDiscord = hostMatches(host, QStringLiteral("discord.gg")) ||
                           hostMatches(host, QStringLiteral("discord.com"));

    QString title;
    QString subtitle;
    if (info != nullptr && info->isResolved())
    {
        title = info->previewTitle().trimmed();
        subtitle = info->previewSubtitle().trimmed();
    }

    if (isDiscord)
    {
        title = title.isEmpty() ? QStringLiteral("Join Discord server")
                                : QStringLiteral("Join %1").arg(title);
    }
    else if (title.isEmpty())
    {
        if (hostMatches(host, QStringLiteral("youtube.com")) ||
            hostMatches(host, QStringLiteral("youtu.be")))
        {
            title = QStringLiteral("Open on YouTube");
        }
        else if (hostMatches(host, QStringLiteral("twitch.tv")))
        {
            title = QStringLiteral("Open on Twitch");
        }
        else if (hostMatches(host, QStringLiteral("kick.com")))
        {
            title = QStringLiteral("Open on Kick");
        }
        else if (hostMatches(host, QStringLiteral("x.com")) ||
                 hostMatches(host, QStringLiteral("twitter.com")))
        {
            title = QStringLiteral("Open on X");
        }
        else if (hostMatches(host, QStringLiteral("reddit.com")) ||
                 hostMatches(host, QStringLiteral("redd.it")))
        {
            title = QStringLiteral("Open on Reddit");
        }
        else
        {
            title = host.isEmpty() ? QStringLiteral("Open link")
                                   : QStringLiteral("Open %1").arg(host);
        }
    }

    QString tooltip = title;
    if (!subtitle.isEmpty())
    {
        tooltip += QLatin1Char('\n') + subtitle;
    }
    tooltip += QLatin1Char('\n') + url;
    return tooltip;
}

void appendLinkFormats(QList<QTextLayout::FormatRange> &formats,
                       const std::vector<PinnedMessageLink> &links,
                       int textOffset, int visibleMessageLength,
                       const QColor &linkColor)
{
    for (const auto &link : links)
    {
        const int visibleEnd =
            std::min(link.start + link.length, visibleMessageLength);
        if (visibleEnd <= link.start)
        {
            continue;
        }

        QTextLayout::FormatRange format;
        format.start = textOffset + link.start;
        format.length = visibleEnd - link.start;
        format.format.setForeground(linkColor);
        formats.push_back(std::move(format));
    }
}

void appendLinkHitboxes(
    std::vector<std::pair<QRectF, QString>> &hitboxes,
    const QTextLayout &layout, const QPointF &origin, const QRectF &clip,
    const std::vector<PinnedMessageLink> &links, int textOffset,
    int visibleMessageLength)
{
    for (const auto &link : links)
    {
        const int linkStart = textOffset + link.start;
        const int linkEnd =
            textOffset +
            std::min(link.start + link.length, visibleMessageLength);
        if (linkEnd <= linkStart)
        {
            continue;
        }

        for (int lineIndex = 0; lineIndex < layout.lineCount(); ++lineIndex)
        {
            const auto line = layout.lineAt(lineIndex);
            const int lineStart = line.textStart();
            const int lineEnd = lineStart + line.textLength();
            const int fragmentStart = std::max(linkStart, lineStart);
            const int fragmentEnd = std::min(linkEnd, lineEnd);
            if (fragmentEnd <= fragmentStart)
            {
                continue;
            }

            qreal left = line.cursorToX(fragmentStart);
            qreal right = line.cursorToX(fragmentEnd);
            if (right < left)
            {
                std::swap(left, right);
            }
            const auto linePosition = line.position();
            const QRectF fragment(
                origin.x() + linePosition.x() + left,
                origin.y() + linePosition.y(), right - left, line.height());
            const auto visibleFragment = fragment.intersected(clip);
            if (!visibleFragment.isEmpty())
            {
                hitboxes.emplace_back(visibleFragment, link.url);
            }
        }
    }
}

}  // namespace

PinnedMessageBar::PinnedMessageBar(QWidget *parent)
    : BaseWidget(parent)
    , tooltipWidget_(new TooltipWidget(this))
    , rowExpansionAnimation_(this)
{
    this->setMouseTracking(true);
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    this->setVisible(false);
    this->setProperty(OPEN_MENU_ROW_PROPERTY, -1);
    this->setProperty(SUPPRESS_MENU_ROW_PROPERTY, -1);

    this->displayTimer_.setInterval(1000);
    QObject::connect(&this->displayTimer_, &QTimer::timeout, this, [this] {
        std::vector<ChannelPtr> expired;
        const auto now = QDateTime::currentDateTimeUtc();
        for (const auto &row : this->rows_)
        {
            if (row.message.endsAt && *row.message.endsAt <= now)
            {
                expired.push_back(row.channel);
            }
        }
        for (const auto &channel : expired)
        {
            channel->setPinnedMessage(std::nullopt);
        }
        this->update();
    });
    this->displayTimer_.start();

    this->refreshTimer_.setInterval(15'000);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this,
                     [this] {
                         this->refreshTwitchState();
                     });
    this->refreshTimer_.start();

    this->rowExpansionAnimation_.setDuration(180);
    this->rowExpansionAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(
        &this->rowExpansionAnimation_, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            this->animatedRowHeight_ = value.toInt();
            this->update();
        });
    QObject::connect(
        &this->rowExpansionAnimation_, &QVariantAnimation::finished, this,
        [this] {
            if (!this->animationTargetExpanded_)
            {
                this->expandedMessageKeys_.remove(this->animatedMessageKey_);
            }
            this->animatedMessageKey_.clear();
            this->animatedRowHeight_ = 0;
            this->updateFixedHeight();
            this->update();
        });
}

QSize PinnedMessageBar::sizeHint() const
{
    int height = 0;
    for (const auto &row : this->rows_)
    {
        height += this->rowHeight(row);
    }
    return {scaled(this->scale(), 320), height};
}

int PinnedMessageBar::collapsedRowHeight() const
{
    return scaled(this->scale(), 42);
}

QString PinnedMessageBar::bodyText(const Row &row) const
{
    const auto author = row.message.senderDisplayName.isEmpty()
                            ? row.message.senderLogin
                            : row.message.senderDisplayName;
    return author.isEmpty()
               ? row.message.messageText
               : author + QStringLiteral(": ") + row.message.messageText;
}

bool PinnedMessageBar::rowExpanded(const Row &row) const
{
    return this->expandedMessageKeys_.contains(
        pinnedMessageKey(row.channel, row.message));
}

int PinnedMessageBar::expandedBodyHeight(const Row &row) const
{
    const int accentWidth = scaled(this->scale(), 3);
    const int left = accentWidth + scaled(this->scale(), 10);
    const int right = scaled(this->scale(), 8);
    const int menuSize = scaled(this->scale(), 26);
    const int contentRight =
        this->width() - right - menuSize - scaled(this->scale(), 6);
    const int bodyWidth = std::max(1, contentRight - left);

    QFont normalFont = this->font();
    QFont authorFont = normalFont;
    authorFont.setWeight(QFont::Bold);
    QTextLayout layout(this->bodyText(row), normalFont);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    const auto author = row.message.senderDisplayName.isEmpty()
                            ? row.message.senderLogin
                            : row.message.senderDisplayName;
    if (!author.isEmpty())
    {
        QTextLayout::FormatRange format;
        format.start = 0;
        format.length = author.size() + 2;
        format.format.setFont(authorFont);
        layout.setFormats({format});
    }

    qreal height = 0;
    layout.beginLayout();
    while (true)
    {
        auto line = layout.createLine();
        if (!line.isValid())
        {
            break;
        }
        line.setLineWidth(bodyWidth);
        line.setPosition(QPointF(0, height));
        height += line.height();
    }
    layout.endLayout();
    return std::max(QFontMetrics(normalFont).height(),
                    static_cast<int>(std::ceil(height)));
}

int PinnedMessageBar::expandedRowHeight(const Row &row) const
{
    const int expanded = scaled(this->scale(), 4) +
                         scaled(this->scale(), 16) +
                         this->expandedBodyHeight(row) +
                         scaled(this->scale(), 6);
    return std::max(this->collapsedRowHeight(), expanded);
}

int PinnedMessageBar::rowHeight(const Row &row) const
{
    const auto key = pinnedMessageKey(row.channel, row.message);
    if (this->rowExpansionAnimation_.state() ==
            QAbstractAnimation::Running &&
        key == this->animatedMessageKey_)
    {
        return this->animatedRowHeight_;
    }
    return this->rowExpanded(row) ? this->expandedRowHeight(row)
                                  : this->collapsedRowHeight();
}

void PinnedMessageBar::animateRowExpansion(const Row &row, bool expand)
{
    const auto key = pinnedMessageKey(row.channel, row.message);
    int startHeight = this->rowHeight(row);

    if (!this->animatedMessageKey_.isEmpty() &&
        this->animatedMessageKey_ != key)
    {
        if (!this->animationTargetExpanded_)
        {
            this->expandedMessageKeys_.remove(this->animatedMessageKey_);
        }
        this->rowExpansionAnimation_.stop();
        this->animatedMessageKey_.clear();
        this->animatedRowHeight_ = 0;
        startHeight = this->rowExpanded(row) ? this->expandedRowHeight(row)
                                             : this->collapsedRowHeight();
    }
    else
    {
        this->rowExpansionAnimation_.stop();
    }

    // Keep a collapsing row logically expanded until the finished handler
    // removes it, preserving the text layout and baseline for every frame.
    if (expand)
    {
        this->expandedMessageKeys_.insert(key);
    }
    this->animatedMessageKey_ = key;
    this->animatedRowHeight_ = startHeight;
    this->animationTargetExpanded_ = expand;
    this->rowExpansionAnimation_.setStartValue(startHeight);
    this->rowExpansionAnimation_.setEndValue(
        expand ? this->expandedRowHeight(row) : this->collapsedRowHeight());

    // Reserve the final height before expanding. During collapse, keep the
    // current height until the animation finishes so the text is never painted
    // inside prematurely collapsed geometry.
    if (expand)
    {
        this->updateFixedHeight();
    }
    this->rowExpansionAnimation_.start();
}

void PinnedMessageBar::updateFixedHeight()
{
    const bool visible = this->displayEnabled_ && !this->rows_.empty();
    this->setVisible(visible);
    this->setFixedHeight(visible ? this->sizeHint().height() : 0);
    this->updateGeometry();
}

void PinnedMessageBar::setChannel(const ChannelPtr &channel)
{
    if (this->channel_ == channel)
    {
        this->rebuildRows();
        this->refreshTwitchState();
        return;
    }

    this->channel_ = channel;
    this->rebuildObservedChannels();
    this->refreshTwitchState();
}

void PinnedMessageBar::setDisplayEnabled(bool enabled)
{
    if (this->displayEnabled_ == enabled)
    {
        this->updateFixedHeight();
        return;
    }

    this->displayEnabled_ = enabled;
    this->updateFixedHeight();
    if (enabled)
    {
        this->refreshTwitchState();
    }
}

void PinnedMessageBar::setMultiplePlatformsSelected(
    bool multiplePlatformsSelected)
{
    if (this->multiplePlatformsSelected_ == multiplePlatformsSelected)
    {
        return;
    }

    this->multiplePlatformsSelected_ = multiplePlatformsSelected;
    this->update();
}

bool PinnedMessageBar::hasHiddenPinnedMessage() const
{
    for (const auto &observed : this->observedChannels_)
    {
        if (observed && observed->pinnedMessage() &&
            this->hiddenMessageKeys_.contains(
                pinnedMessageKey(observed, *observed->pinnedMessage())))
        {
            return true;
        }
    }
    return false;
}

void PinnedMessageBar::unhidePinnedMessages()
{
    if (this->hiddenMessageKeys_.isEmpty())
    {
        return;
    }

    this->hiddenMessageKeys_.clear();
    this->rebuildRows();
}

void PinnedMessageBar::rebuildObservedChannels()
{
    this->channelSignals_.clear();
    this->observedChannels_.clear();

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(this->channel_))
    {
        if (merged->twitchChannel())
        {
            this->observedChannels_.push_back(merged->twitchChannel());
        }
        if (merged->kickChannel())
        {
            this->observedChannels_.push_back(merged->kickChannel());
        }
    }
    else if (this->channel_ && this->channel_->isTwitchOrKickChannel())
    {
        this->observedChannels_.push_back(this->channel_);
    }

    for (const auto &observed : this->observedChannels_)
    {
        this->channelSignals_.managedConnect(
            observed->pinnedMessageChanged,
            [this](const std::optional<PinnedChatMessage> &) {
                this->rebuildRows();
            });
    }
    this->rebuildRows();
}

void PinnedMessageBar::rebuildRows()
{
    if (this->hoveredLinkInfo_)
    {
        QObject::disconnect(this->hoveredLinkInfo_.data(),
                            &LinkInfo::stateChanged, this, nullptr);
        this->hoveredLinkInfo_.clear();
    }
    this->hoveredLinkUrl_.clear();
    this->tooltipWidget_->hide();
    this->rows_.clear();
    QSet<QString> activeMessageKeys;
    for (const auto &observed : this->observedChannels_)
    {
        if (observed && observed->pinnedMessage())
        {
            const auto &message = *observed->pinnedMessage();
            const auto key = pinnedMessageKey(observed, message);
            if (!this->hiddenMessageKeys_.contains(key))
            {
                activeMessageKeys.insert(key);
                this->rows_.push_back(
                    {observed, message, {}, {}, {}, false});
            }
        }
    }
    this->expandedMessageKeys_.intersect(activeMessageKeys);
    if (!this->animatedMessageKey_.isEmpty() &&
        !activeMessageKeys.contains(this->animatedMessageKey_))
    {
        this->rowExpansionAnimation_.stop();
        this->animatedMessageKey_.clear();
        this->animatedRowHeight_ = 0;
    }
    this->hoveredMenuRow_ = -1;
    this->updateFixedHeight();
    this->update();
}

void PinnedMessageBar::refreshTwitchState()
{
    for (const auto &observed : this->observedChannels_)
    {
        if (observed && observed->isTwitchChannel())
        {
            commands::refreshPinnedChatMessage(observed);
        }
    }
}

QString PinnedMessageBar::remainingText(const Row &row) const
{
    const auto &message = row.message;
    if (!message.endsAt)
    {
        return row.channel && row.channel->isLive()
                   ? QStringLiteral("Until stream ends")
                   : QStringLiteral("Until stream starts");
    }

    const auto seconds = std::max<qint64>(
        0, QDateTime::currentDateTimeUtc().secsTo(*message.endsAt));
    const auto minutes = seconds / 60;
    const auto remainder = seconds % 60;
    if (minutes > 0)
    {
        return QStringLiteral("%1m %2s left").arg(minutes).arg(remainder);
    }
    return QStringLiteral("%1s left").arg(remainder);
}

int PinnedMessageBar::rowAt(const QPoint &position) const
{
    for (int i = 0; i < static_cast<int>(this->rows_.size()); ++i)
    {
        if (this->rows_[static_cast<size_t>(i)].rect.contains(position))
        {
            return i;
        }
    }
    return -1;
}

void PinnedMessageBar::mousePressEvent(QMouseEvent *event)
{
    const int rowIndex = this->rowAt(event->pos());
    if (rowIndex < 0)
    {
        BaseWidget::mousePressEvent(event);
        return;
    }

    auto &row = this->rows_[static_cast<size_t>(rowIndex)];
    const auto hoveredLink = std::ranges::find_if(
        row.linkHitboxes, [position = event->position()](const auto &link) {
            return link.first.contains(position);
        });
    if (event->button() == Qt::RightButton &&
        hoveredLink != row.linkHitboxes.end())
    {
        this->showLinkMenu(hoveredLink->second,
                           event->globalPosition().toPoint());
        event->accept();
        return;
    }
    const bool menuButtonClick =
        event->button() == Qt::LeftButton &&
        row.menuRect.contains(event->pos());
    if (menuButtonClick &&
        menuRowProperty(this, SUPPRESS_MENU_ROW_PROPERTY) == rowIndex)
    {
        this->setProperty(SUPPRESS_MENU_ROW_PROPERTY, -1);
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton || menuButtonClick)
    {
        this->showMenu(rowIndex, event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton)
    {
        if (hoveredLink != row.linkHitboxes.end())
        {
            QDesktopServices::openUrl(QUrl(hoveredLink->second));
            event->accept();
            return;
        }
        if (row.bodyElided || this->rowExpanded(row))
        {
            const auto key = pinnedMessageKey(row.channel, row.message);
            const bool currentlyExpanding =
                this->rowExpansionAnimation_.state() ==
                    QAbstractAnimation::Running &&
                this->animatedMessageKey_ == key
                    ? this->animationTargetExpanded_
                    : this->rowExpanded(row);
            this->animateRowExpansion(row, !currentlyExpanding);
            event->accept();
            return;
        }
        auto message = row.channel->findMessageByID(row.message.messageID);
        if (message)
        {
            getApp()->getWindows()->scrollToMessage(message);
        }
        event->accept();
        return;
    }
    BaseWidget::mousePressEvent(event);
}

void PinnedMessageBar::mouseMoveEvent(QMouseEvent *event)
{
    int hovered = -1;
    for (int i = 0; i < static_cast<int>(this->rows_.size()); ++i)
    {
        if (this->rows_[static_cast<size_t>(i)].menuRect.contains(event->pos()))
        {
            hovered = i;
            break;
        }
    }
    const int hoveredRow = this->rowAt(event->pos());
    QString hoveredLinkUrl;
    if (hoveredRow >= 0)
    {
        const auto &row = this->rows_[static_cast<size_t>(hoveredRow)];
        const auto hoveredLink = std::ranges::find_if(
            row.linkHitboxes, [position = event->position()](const auto &link) {
                return link.first.contains(position);
            });
        if (hoveredLink != row.linkHitboxes.end())
        {
            hoveredLinkUrl = hoveredLink->second;
        }
    }
    this->hoveredLinkUrl_ = hoveredLinkUrl;
    if (this->hoveredLinkUrl_.isEmpty())
    {
        if (this->hoveredLinkInfo_)
        {
            QObject::disconnect(this->hoveredLinkInfo_.data(),
                                &LinkInfo::stateChanged, this, nullptr);
            this->hoveredLinkInfo_.clear();
        }
        this->tooltipWidget_->hide();
    }
    else
    {
        this->showLinkTooltip(this->hoveredLinkUrl_,
                              event->globalPosition().toPoint());
    }
    const bool expandable =
        hoveredRow >= 0 &&
        (this->rows_[static_cast<size_t>(hoveredRow)].bodyElided ||
         this->rowExpanded(this->rows_[static_cast<size_t>(hoveredRow)]));
    this->setCursor(hovered >= 0 || !hoveredLinkUrl.isEmpty() || expandable
                        ? Qt::PointingHandCursor
                        : Qt::ArrowCursor);
    if (hovered != this->hoveredMenuRow_)
    {
        this->hoveredMenuRow_ = hovered;
        this->update();
    }
    BaseWidget::mouseMoveEvent(event);
}

void PinnedMessageBar::leaveEvent(QEvent *event)
{
    this->hoveredMenuRow_ = -1;
    this->hoveredLinkUrl_.clear();
    if (this->hoveredLinkInfo_)
    {
        QObject::disconnect(this->hoveredLinkInfo_.data(),
                            &LinkInfo::stateChanged, this, nullptr);
        this->hoveredLinkInfo_.clear();
    }
    this->tooltipWidget_->hide();
    this->unsetCursor();
    this->update();
    BaseWidget::leaveEvent(event);
}

void PinnedMessageBar::showLinkMenu(const QString &url,
                                    const QPoint &globalPosition)
{
    this->tooltipWidget_->hide();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Open link"), [url] {
        QDesktopServices::openUrl(QUrl(url));
    });
    if (supportsIncognitoLinks())
    {
        menu.addAction(QStringLiteral("Open in incognito"), [url] {
            openLinkIncognito(url);
        });
    }
    menu.addAction(QStringLiteral("Copy link"), [url] {
        crossPlatformCopy(url);
    });
    menu.exec(globalPosition);
}

void PinnedMessageBar::showLinkTooltip(const QString &url,
                                       const QPoint &globalPosition)
{
    LinkInfo *info = nullptr;
    for (const auto &row : this->rows_)
    {
        if (!row.channel)
        {
            continue;
        }
        const auto message =
            row.channel->findMessageByID(row.message.messageID);
        if (!message)
        {
            continue;
        }
        for (const auto &element : message->elements)
        {
            auto *linkElement = dynamic_cast<LinkElement *>(element.get());
            if (linkElement == nullptr)
            {
                continue;
            }
            auto *candidate = linkElement->linkInfo();
            if (candidate->originalUrl() == url || candidate->url() == url)
            {
                info = candidate;
                break;
            }
        }
        if (info != nullptr)
        {
            break;
        }
    }

    if (this->hoveredLinkInfo_.data() != info)
    {
        if (this->hoveredLinkInfo_)
        {
            QObject::disconnect(this->hoveredLinkInfo_.data(),
                                &LinkInfo::stateChanged, this, nullptr);
        }
        this->hoveredLinkInfo_ = info;
        if (info != nullptr && !info->isLoaded())
        {
            QObject::connect(
                info, &LinkInfo::stateChanged, this,
                [this, url](LinkInfo::State) {
                    if (this->hoveredLinkUrl_ == url)
                    {
                        this->showLinkTooltip(url, QCursor::pos());
                    }
                });
        }
    }

    this->tooltipWidget_->setOne({
        .image = nullptr,
        .text = pinnedLinkTooltip(url, info),
    });
    this->tooltipWidget_->moveTo(
        globalPosition + QPoint(16, 16),
        widgets::BoundsChecking::CursorPosition);
    this->tooltipWidget_->setWordWrap(true);
    this->tooltipWidget_->show();
}

void PinnedMessageBar::showMenu(int rowIndex,
                                const QPoint &globalPosition)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(this->rows_.size()))
    {
        return;
    }

    const auto row = this->rows_[static_cast<size_t>(rowIndex)];
    QMenu menu(this);
    const auto titleText = QStringLiteral("Pinned");
    auto *title = menu.addAction(titleText);
    title->setEnabled(false);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Hide"), [this, row] {
        this->hiddenMessageKeys_.insert(
            pinnedMessageKey(row.channel, row.message));
        this->rebuildRows();
    });

    const bool canManage = row.channel->hasModRights();
    if (canManage)
    {
        menu.addSeparator();
    }

    if (canManage &&
        row.message.platform == PinnedChatMessage::Platform::Twitch)
    {
        auto *durationMenu =
            menu.addMenu(QStringLiteral("Keep pinned for"));
        const auto addDuration = [durationMenu, channel = row.channel](
                                     const QString &label, int minutes) {
            durationMenu->addAction(label, [channel, minutes] {
                commands::updatePinnedChatMessageDuration(channel,
                                                          minutes * 60);
            });
        };
        addDuration(QStringLiteral("5 minutes"), 5);
        addDuration(QStringLiteral("10 minutes"), 10);
        addDuration(QStringLiteral("20 minutes"), 20);
        addDuration(QStringLiteral("30 minutes"), 30);
        durationMenu->addSeparator();
        durationMenu->addAction(
            QStringLiteral("Custom duration..."),
            [this, channel = row.channel, message = row.message] {
                int initialMinutes = 20;
                if (message.endsAt)
                {
                    initialMinutes = std::clamp(
                        static_cast<int>(
                            std::ceil(QDateTime::currentDateTimeUtc().secsTo(
                                          *message.endsAt) /
                                      60.0)),
                        1, 30);
                }
                bool accepted = false;
                const int minutes = QInputDialog::getInt(
                    this, QStringLiteral("Pinned message duration"),
                    QStringLiteral("Keep pinned for how many minutes?"),
                    initialMinutes, 1, 30, 1, &accepted);
                if (accepted)
                {
                    commands::updatePinnedChatMessageDuration(channel,
                                                              minutes * 60);
                }
            });
        const auto streamDurationLabel =
            row.channel && row.channel->isLive()
                ? QStringLiteral("Until stream ends")
                : QStringLiteral("Until stream starts");
        durationMenu->addAction(
            streamDurationLabel, [channel = row.channel] {
                commands::updatePinnedChatMessageDuration(channel,
                                                          std::nullopt);
            });
        menu.addSeparator();
    }
    if (canManage)
    {
        menu.addAction(QStringLiteral("Unpin message"),
                       [channel = row.channel] {
                           commands::unpinChatMessage(channel);
                       });
    }
    this->setProperty(OPEN_MENU_ROW_PROPERTY, rowIndex);
    this->update();
    const auto menuRect = row.menuRect;
    QObject::connect(&menu, &QMenu::aboutToHide, this,
                     [this, rowIndex, menuRect] {
                         if (menuRect.contains(
                                 this->mapFromGlobal(QCursor::pos())))
                         {
                             this->setProperty(SUPPRESS_MENU_ROW_PROPERTY,
                                               rowIndex);
                             QTimer::singleShot(250, this, [this, rowIndex] {
                                 if (menuRowProperty(
                                         this, SUPPRESS_MENU_ROW_PROPERTY) ==
                                     rowIndex)
                                 {
                                     this->setProperty(
                                         SUPPRESS_MENU_ROW_PROPERTY, -1);
                                 }
                             });
                         }
                     });
    const auto requestedPosition = this->mapFromGlobal(globalPosition);
    const auto menuPosition = menuRect.contains(requestedPosition)
                                  ? this->mapToGlobal(menuRect.bottomRight())
                                  : globalPosition;
    menu.exec(menuPosition);
    if (menuRowProperty(this, OPEN_MENU_ROW_PROPERTY) == rowIndex)
    {
        this->setProperty(OPEN_MENU_ROW_PROPERTY, -1);
    }
    this->update();
}

void PinnedMessageBar::paintEvent(QPaintEvent *)
{
    if (this->rows_.empty())
    {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor surface = this->theme->messages.backgrounds.alternate;
    if (surface == this->theme->messages.backgrounds.regular)
    {
        surface = this->theme->splits.input.background;
    }
    QColor separator = this->theme->splits.messageSeperator;
    if (!separator.isValid())
    {
        separator = this->theme->splits.header.border;
    }
    const QColor text = this->theme->messages.textColors.regular;
    const QColor linkColor = this->theme->messages.textColors.link;
    QColor muted = text;
    muted.setAlphaF(0.68);
    QColor remainingColor = text;
    remainingColor.setAlphaF(0.30);

    painter.fillRect(this->rect(), surface);
    const int accentWidth = scaled(this->scale(), 3);
    const int left = accentWidth + scaled(this->scale(), 10);
    const int right = scaled(this->scale(), 8);
    const int menuSize = scaled(this->scale(), 26);
    const int headerTop = scaled(this->scale(), 4);
    const int lineHeight = scaled(this->scale(), 16);

    QFont normalFont = this->font();
    QFont headerFont = normalFont;
    headerFont.setBold(true);
    QFont authorFont = normalFont;
    authorFont.setWeight(QFont::Bold);
    QFont remainingFont = normalFont;
    if (remainingFont.pointSizeF() > 0)
    {
        remainingFont.setPointSizeF(
            std::max<qreal>(7.0, remainingFont.pointSizeF() - 1.0));
    }
    else if (remainingFont.pixelSize() > 0)
    {
        remainingFont.setPixelSize(
            std::max(7, remainingFont.pixelSize() - 1));
    }
    QFontMetrics normalMetrics(normalFont);
    QFontMetrics headerMetrics(headerFont);
    QFontMetrics authorMetrics(authorFont);
    QFontMetrics remainingMetrics(remainingFont);

    int top = 0;
    for (int i = 0; i < static_cast<int>(this->rows_.size()); ++i)
    {
        auto &row = this->rows_[static_cast<size_t>(i)];
        const int height = this->rowHeight(row);
        row.rect = QRect(0, top, this->width(), height);
        row.menuRect = QRect(this->width() - right - menuSize,
                             top + (this->collapsedRowHeight() - menuSize) / 2,
                             menuSize,
                             menuSize);
        row.linkHitboxes.clear();

        const QColor accent = platformAccent(row.message.platform);
        QColor rowTint = accent;
        rowTint.setAlphaF(this->theme->isLightTheme() ? 0.045 : 0.075);
        painter.fillRect(row.rect, rowTint);
        painter.fillRect(QRect(0, top, accentWidth, height), accent);

        if (i > 0)
        {
            painter.setPen(separator);
            painter.drawLine(0, top, this->width(), top);
        }

        const bool menuActive =
            this->hoveredMenuRow_ == i ||
            menuRowProperty(this, OPEN_MENU_ROW_PROPERTY) == i;
        if (menuActive)
        {
            QColor menuHover = text;
            menuHover.setAlphaF(this->theme->isLightTheme() ? 0.08 : 0.12);
            painter.setPen(Qt::NoPen);
            painter.setBrush(menuHover);
            const auto radius = scaled(this->scale(), 5);
            painter.drawRoundedRect(row.menuRect, radius, radius);
        }

        const auto remaining = this->remainingText(row);
        const int remainingWidth =
            remainingMetrics.horizontalAdvance(remaining);
        const int contentRight = row.menuRect.left() - scaled(this->scale(), 6);
        QRect headerRect(left, top + headerTop,
                         std::max(0, contentRight - left - remainingWidth -
                                         scaled(this->scale(), 10)),
                         lineHeight);
        QRect remainingRect(contentRight - remainingWidth +
                                scaled(this->scale(), 2),
                             top + headerTop - scaled(this->scale(), 1),
                             remainingWidth, lineHeight);

        painter.setFont(headerFont);
        painter.setPen(accent);
        const auto header = QStringLiteral("Pinned");
        painter.drawText(headerRect, Qt::AlignLeft | Qt::AlignVCenter,
                         headerMetrics.elidedText(header, Qt::ElideRight,
                                                  headerRect.width()));
        painter.setFont(remainingFont);
        painter.setPen(remainingColor);
        painter.drawText(remainingRect, Qt::AlignRight | Qt::AlignVCenter,
                         remaining);

        const auto author = row.message.senderDisplayName.isEmpty()
                                ? row.message.senderLogin
                                : row.message.senderDisplayName;
        QRect bodyRect(left, top + headerTop + lineHeight,
                       std::max(0, contentRight - left),
                       height - headerTop - lineHeight -
                           scaled(this->scale(), 3));
        const auto authorText =
            author.isEmpty() ? QString{} : author + QStringLiteral(": ");
        const auto links = findPinnedMessageLinks(row.message.messageText);
        row.bodyElided = author.isEmpty()
                             ? normalMetrics.horizontalAdvance(
                                   row.message.messageText) > bodyRect.width()
                             : authorMetrics.horizontalAdvance(authorText) >
                                       std::max(0, bodyRect.width() / 2) ||
                                   authorMetrics.horizontalAdvance(authorText) +
                                           normalMetrics.horizontalAdvance(
                                               row.message.messageText) >
                                       bodyRect.width();
        painter.setPen(text);
        if (this->rowExpanded(row))
        {
            QTextLayout layout(this->bodyText(row), normalFont);
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout.setTextOption(option);
            QList<QTextLayout::FormatRange> formats;
            if (!author.isEmpty())
            {
                QTextLayout::FormatRange format;
                format.start = 0;
                format.length = authorText.size();
                format.format.setFont(authorFont);
                formats.push_back(std::move(format));
            }
            appendLinkFormats(formats, links, authorText.size(),
                              row.message.messageText.size(), linkColor);
            layout.setFormats(formats);
            qreal lineTop = 0;
            layout.beginLayout();
            while (true)
            {
                auto line = layout.createLine();
                if (!line.isValid())
                {
                    break;
                }
                line.setLineWidth(bodyRect.width());
                line.setPosition(QPointF(0, lineTop));
                lineTop += line.height();
            }
            layout.endLayout();
            const int collapsedBodyHeight =
                this->collapsedRowHeight() - headerTop - lineHeight -
                scaled(this->scale(), 3);
            const qreal firstLineOffset = std::max<qreal>(
                0.0, (collapsedBodyHeight - normalMetrics.height()) / 2.0);
            painter.save();
            painter.setClipRect(bodyRect);
            const QPointF layoutOrigin(bodyRect.left(),
                                       bodyRect.top() + firstLineOffset);
            layout.draw(&painter, layoutOrigin);
            painter.restore();
            appendLinkHitboxes(row.linkHitboxes, layout, layoutOrigin, bodyRect,
                               links, authorText.size(),
                               row.message.messageText.size());
        }
        else
        {
            QRect messageRect = bodyRect;
            if (!author.isEmpty())
            {
                const int authorWidth = std::min(
                    authorMetrics.horizontalAdvance(authorText),
                    std::max(0, bodyRect.width() / 2));
                const auto visibleAuthor = authorMetrics.elidedText(
                    authorText, Qt::ElideRight, authorWidth);
                const int usedAuthorWidth =
                    authorMetrics.horizontalAdvance(visibleAuthor);
                painter.setFont(authorFont);
                painter.drawText(
                    QRect(bodyRect.left(), bodyRect.top(), authorWidth,
                          bodyRect.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, visibleAuthor);
                messageRect.adjust(usedAuthorWidth, 0, 0, 0);
            }

            const auto visibleMessage = normalMetrics.elidedText(
                row.message.messageText, Qt::ElideRight, messageRect.width());
            const int visibleMessageLength =
                visibleMessage == row.message.messageText
                    ? static_cast<int>(row.message.messageText.size())
                    : std::max(0, static_cast<int>(visibleMessage.size()) - 1);
            QTextLayout layout(visibleMessage, normalFont);
            QTextOption option;
            option.setWrapMode(QTextOption::NoWrap);
            layout.setTextOption(option);
            QList<QTextLayout::FormatRange> formats;
            appendLinkFormats(formats, links, 0, visibleMessageLength,
                              linkColor);
            layout.setFormats(formats);
            layout.beginLayout();
            auto line = layout.createLine();
            if (line.isValid())
            {
                line.setPosition(QPointF(0, 0));
            }
            layout.endLayout();
            const qreal renderedLineHeight =
                line.isValid() ? line.height() : normalMetrics.height();
            const QPointF layoutOrigin(
                messageRect.left(),
                messageRect.top() +
                    std::max<qreal>(
                        0.0,
                        (messageRect.height() - renderedLineHeight) / 2.0));
            painter.save();
            painter.setClipRect(messageRect);
            layout.draw(&painter, layoutOrigin);
            painter.restore();
            appendLinkHitboxes(row.linkHitboxes, layout, layoutOrigin,
                               messageRect, links, 0, visibleMessageLength);
        }

        QColor dotColor = menuActive ? text : muted;
        dotColor.setAlphaF(menuActive ? 0.92 : 0.72);
        painter.setPen(Qt::NoPen);
        painter.setBrush(dotColor);
        const qreal dotRadius = std::max<qreal>(1.2, this->scale() * 1.25);
        const qreal dotSpacing = this->scale() * 4.5;
        const QPointF center =
            row.menuRect.center() + QPointF(1.0, 1.0);
        for (int dot = -1; dot <= 1; ++dot)
        {
            painter.drawEllipse(
                QPointF(center.x() + dot * dotSpacing, center.y()),
                dotRadius, dotRadius);
        }
        top += height;
    }
}

void PinnedMessageBar::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);
    this->updateFixedHeight();
    this->update();
}

void PinnedMessageBar::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->update();
}

}  // namespace chatterino
