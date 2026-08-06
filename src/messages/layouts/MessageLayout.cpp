// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/layouts/MessageLayout.hpp"

#include "Application.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/layouts/MessageLayoutElement.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Selection.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/links/LinkInfo.hpp"
#include "providers/links/LinkResolver.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"

#include <QApplication>
#include <QDebug>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QTextLayout>
#include <QTextOption>
#include <QtGlobal>
#include <QThread>

#include <algorithm>

namespace chatterino {

namespace {

QColor blendColors(const QColor &base, const QColor &apply)
{
    const qreal &alpha = apply.alphaF();
    QColor result;
    result.setRgbF(base.redF() * (1 - alpha) + apply.redF() * alpha,
                   base.greenF() * (1 - alpha) + apply.greenF() * alpha,
                   base.blueF() * (1 - alpha) + apply.blueF() * alpha);
    return result;
}

QStringList wrapPreviewText(const QString &input, const QFont &font,
                            qreal width, int maxLines)
{
    const auto text = input.simplified();
    if (text.isEmpty() || width <= 0 || maxLines <= 0)
    {
        return {};
    }

    QTextLayout textLayout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    textLayout.setTextOption(option);

    const QFontMetricsF metrics(font);
    QStringList lines;
    textLayout.beginLayout();
    for (int index = 0; index < maxLines; ++index)
    {
        auto line = textLayout.createLine();
        if (!line.isValid())
        {
            break;
        }
        line.setLineWidth(width);

        const auto start = line.textStart();
        const auto length = line.textLength();
        const bool isLastVisibleLine = index == maxLines - 1;
        if (isLastVisibleLine && start + length < text.size())
        {
            lines.append(metrics.elidedText(text.mid(start).simplified(),
                                            Qt::ElideRight, qFloor(width)));
            break;
        }
        lines.append(text.mid(start, length).trimmed());
    }
    textLayout.endLayout();
    return lines;
}

struct LinkPreviewCardLayout {
    QSizeF size;
    qreal contentLeft = 0;
    qreal contentWidth = 0;
    qreal siteTop = -1;
    qreal titleTop = -1;
    qreal subtitleTop = -1;
    qreal imageTop = -1;
    qreal imageHeight = 0;
    QFont siteFont;
    QFont titleFont;
    QFont subtitleFont;
    QStringList siteLines;
    QStringList titleLines;
    QStringList subtitleLines;
};

LinkPreviewCardLayout makeLinkPreviewCardLayout(
    const QString &title, const QString &subtitle, const QString &siteName,
    const ImagePtr &thumbnail, float scale, qreal width)
{
    LinkPreviewCardLayout layout;
    layout.siteFont =
        getApp()->getFonts()->getFont(FontStyle::ChatSmall, scale);
    layout.titleFont =
        getApp()->getFonts()->getFont(FontStyle::ChatMediumBold, scale);
    layout.subtitleFont =
        getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, scale);
    layout.subtitleFont.setPointSizeF(
        layout.subtitleFont.pointSizeF() * 0.95);

    const qreal accentWidth = 4 * scale;
    const qreal horizontalPadding = 12 * scale;
    layout.contentLeft = accentWidth + horizontalPadding;
    layout.contentWidth =
        std::max<qreal>(0, width - layout.contentLeft - horizontalPadding);

    layout.siteLines =
        wrapPreviewText(siteName, layout.siteFont, layout.contentWidth, 1);
    layout.titleLines =
        wrapPreviewText(title, layout.titleFont, layout.contentWidth, 2);
    layout.subtitleLines = wrapPreviewText(
        subtitle, layout.subtitleFont, layout.contentWidth, 3);

    const QFontMetricsF siteMetrics(layout.siteFont);
    const QFontMetricsF titleMetrics(layout.titleFont);
    const QFontMetricsF subtitleMetrics(layout.subtitleFont);

    qreal y = 10 * scale;
    if (!layout.siteLines.isEmpty())
    {
        layout.siteTop = y;
        y += siteMetrics.lineSpacing() * layout.siteLines.size();
    }
    if (!layout.titleLines.isEmpty())
    {
        if (layout.siteTop >= 0)
        {
            y += 3 * scale;
        }
        layout.titleTop = y;
        y += titleMetrics.lineSpacing() * layout.titleLines.size();
    }
    if (!layout.subtitleLines.isEmpty())
    {
        if (layout.siteTop >= 0 || layout.titleTop >= 0)
        {
            y += 6 * scale;
        }
        layout.subtitleTop = y;
        y += subtitleMetrics.lineSpacing() * layout.subtitleLines.size();
    }

    if (thumbnail && !thumbnail->isEmpty())
    {
        y += 10 * scale;
        layout.imageTop = y;
        const auto sourceSize = thumbnail->size();
        const qreal sourceRatio =
            sourceSize.width() > 0 && sourceSize.height() > 0
                ? sourceSize.width() / sourceSize.height()
                : 16.0 / 9.0;
        layout.imageHeight = std::clamp(
            layout.contentWidth / std::max<qreal>(sourceRatio, 0.01),
            qreal{110} * scale, qreal{280} * scale);
        y += layout.imageHeight;
    }

    y += 12 * scale;
    layout.size = {width, std::max<qreal>(y, 56 * scale)};
    return layout;
}

void drawPreviewLines(QPainter &painter, const QStringList &lines,
                      const QFont &font, const QColor &color, qreal x,
                      qreal top)
{
    if (lines.isEmpty() || top < 0)
    {
        return;
    }

    const QFontMetricsF metrics(font);
    painter.setFont(font);
    painter.setPen(color);
    qreal baseline = top + metrics.ascent();
    for (const auto &line : lines)
    {
        painter.drawText(QPointF(x, baseline), line);
        baseline += metrics.lineSpacing();
    }
}

class LinkPreviewLayoutElement final : public MessageLayoutElement
{
public:
    LinkPreviewLayoutElement(MessageElement &creator, QString title,
                             QString subtitle, QString siteName,
                             ImagePtr thumbnail, QColor accentColor,
                             float scale, QSizeF size)
        : MessageLayoutElement(creator, size)
        , title_(std::move(title))
        , subtitle_(std::move(subtitle))
        , siteName_(std::move(siteName))
        , thumbnail_(std::move(thumbnail))
        , accentColor_(std::move(accentColor))
        , scale_(scale)
    {
        this->setTrailingSpace(false);
    }

    static QSizeF calculateSize(const QString &title, const QString &subtitle,
                                const QString &siteName,
                                const ImagePtr &thumbnail, float scale,
                                qreal width)
    {
        auto size = makeLinkPreviewCardLayout(
                        title, subtitle, siteName, thumbnail, scale, width)
                        .size;
        size.rheight() += 6 * scale;
        return size;
    }

    void addCopyTextToString(QString &, uint32_t, uint32_t) const override
    {
    }

    size_t getSelectionIndexCount() const override
    {
        return 0;
    }

    void paint(QPainter &painter,
               const MessageColors &messageColors) override
    {
        const auto rect = this->cardRect();
        painter.setRenderHint(QPainter::Antialiasing, true);

        auto background = messageColors.regularText;
        background.setAlpha(22);
        auto border = messageColors.regularText;
        border.setAlpha(36);
        painter.setPen(QPen(border, std::max<qreal>(1.0, this->scale_)));
        painter.setBrush(background);
        painter.drawRoundedRect(rect, 5 * this->scale_, 5 * this->scale_);

        painter.save();
        QPainterPath cardClip;
        cardClip.addRoundedRect(rect, 5 * this->scale_, 5 * this->scale_);
        painter.setClipPath(cardClip);
        auto accent = this->accentColor_.isValid()
                          ? this->accentColor_
                          : messageColors.linkText;
        accent.setAlpha(220);
        painter.fillRect(QRectF(rect.left(), rect.top(), 4 * this->scale_,
                               rect.height()),
                         accent);
        painter.restore();

        const auto layout = makeLinkPreviewCardLayout(
            this->title_, this->subtitle_, this->siteName_, this->thumbnail_,
            this->scale_, rect.width());

        auto secondary = messageColors.regularText;
        secondary.setAlpha(178);
        drawPreviewLines(painter, layout.siteLines, layout.siteFont, secondary,
                         rect.left() + layout.contentLeft,
                         rect.top() + layout.siteTop);
        drawPreviewLines(painter, layout.titleLines, layout.titleFont,
                         messageColors.linkText,
                         rect.left() + layout.contentLeft,
                         rect.top() + layout.titleTop);
        drawPreviewLines(painter, layout.subtitleLines, layout.subtitleFont,
                         messageColors.regularText,
                         rect.left() + layout.contentLeft,
                         rect.top() + layout.subtitleTop);

        const auto thumbRect = this->thumbnailRect(rect);
        if (!thumbRect.isEmpty() && this->thumbnail_)
        {
            const auto pixmap = this->thumbnail_->pixmapOrLoad();
            if (pixmap && !this->thumbnail_->animated())
            {
                this->drawThumbnail(painter, *pixmap, thumbRect);
            }
        }
    }

    bool paintAnimated(QPainter &painter, qreal yOffset) override
    {
        if (!this->thumbnail_ || !this->thumbnail_->animated())
        {
            return false;
        }

        const auto pixmap = this->thumbnail_->pixmapOrLoad();
        if (!pixmap)
        {
            return false;
        }

        auto thumbRect = this->thumbnailRect(this->cardRect());
        if (thumbRect.isEmpty())
        {
            return false;
        }
        thumbRect.translate(0, yOffset);
        this->drawThumbnail(painter, *pixmap, thumbRect);
        return true;
    }

    int getMouseOverIndex(QPointF) const override
    {
        return 0;
    }

    qreal getXFromIndex(size_t index) override
    {
        return index == 0 ? this->getRect().left()
                          : this->getRect().right();
    }

private:
    QRectF cardRect() const
    {
        auto rect = this->getRect();
        rect.setTop(rect.top() + 6 * this->scale_);
        return rect;
    }

    QRectF thumbnailRect(const QRectF &cardRect) const
    {
        if (!this->thumbnail_ || this->thumbnail_->isEmpty())
        {
            return {};
        }

        const auto layout = makeLinkPreviewCardLayout(
            this->title_, this->subtitle_, this->siteName_, this->thumbnail_,
            this->scale_, cardRect.width());
        if (layout.imageTop < 0 || layout.imageHeight <= 0)
        {
            return {};
        }
        return {cardRect.left() + layout.contentLeft,
                cardRect.top() + layout.imageTop, layout.contentWidth,
                layout.imageHeight};
    }

    void drawThumbnail(QPainter &painter, const QPixmap &pixmap,
                       const QRectF &target) const
    {
        if (pixmap.isNull() || target.isEmpty())
        {
            return;
        }

        QRectF source(0, 0, pixmap.width(), pixmap.height());
        const qreal sourceRatio = source.width() / source.height();
        const qreal targetRatio = target.width() / target.height();
        if (sourceRatio > targetRatio)
        {
            const qreal width = source.height() * targetRatio;
            source.setLeft((source.width() - width) / 2);
            source.setWidth(width);
        }
        else
        {
            const qreal height = source.width() / targetRatio;
            source.setTop((source.height() - height) / 2);
            source.setHeight(height);
        }

        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QPainterPath clip;
        clip.addRoundedRect(target, 4 * this->scale_, 4 * this->scale_);
        painter.setClipPath(clip);
        painter.drawPixmap(target, pixmap, source);
        painter.restore();
    }

    QString title_;
    QString subtitle_;
    QString siteName_;
    ImagePtr thumbnail_;
    QColor accentColor_;
    float scale_;
};

QColor defaultPlatformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64);
        case MessagePlatform::TikTok:
            return QColor(37, 244, 238);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255);
    }
}

QColor fallbackPlatformHighlightColor(const Message &message,
                                      const QColor &highlightColor)
{
    auto color =
        message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
    int alpha = 72;
    if (highlightColor.isValid())
    {
        alpha = highlightColor.alpha();
    }
    else if (message.platformAccentColor &&
             message.platformAccentColor->alpha() > 0)
    {
        alpha = std::max(message.platformAccentColor->alpha(), alpha);
    }

    color.setAlpha(alpha);
    return color;
}

QColor activityPlatformHighlightColor(const Message &message)
{
    const auto accent = defaultPlatformAccent(message.platform);
    const auto hsv = accent.toHsv();
    const int hue = hsv.hsvHue() >= 0 ? hsv.hsvHue() : 0;
    const int saturation =
        std::clamp(std::max(hsv.hsvSaturation(), 150) - 10, 0, 180);
    const int value = std::clamp(std::min(220, std::max(hsv.value(), 198)), 0,
                                 220);
    constexpr int alpha = 102;

    QColor popped;
    popped.setHsv(hue, saturation, value, alpha);
    return popped;
}

bool isPlatformAlertMessage(const Message &message);

QColor automaticEventHighlightColor(const Message &message,
                                    const QColor &highlightColor,
                                    const MessagePaintContext &ctx)
{
    const bool isAlert = isPlatformAlertMessage(message);
    const auto style =
        isAlert ? platformAlertHighlightStyleSetting().getEnum()
                : getSettings()->platformEventHighlightStyle.getEnum();
    if (style == PlatformEventHighlightStyle::None)
    {
        return {};
    }

    if (ctx.forceFlatEventHighlights)
    {
        if (!mergedPlatformIndicatorShowsLineColor(ctx.platformIndicatorMode))
        {
            return {};
        }

        return activityPlatformHighlightColor(message);
    }

    if (style == PlatformEventHighlightStyle::CustomColor)
    {
        QColor custom(
            isAlert ? platformAlertHighlightCustomColorSetting().getValue()
                    : getSettings()
                          ->platformEventHighlightCustomColor.getValue());
        if (custom.isValid())
        {
            return custom;
        }
    }

    if ((message.flags.has(MessageFlag::FirstMessage) ||
         message.flags.has(MessageFlag::FirstMessageSession)) &&
        message.platform == MessagePlatform::AnyOrTwitch &&
        highlightColor.isValid())
    {
        return highlightColor;
    }

    if (message.flags.has(MessageFlag::Subscription) &&
        message.platform == MessagePlatform::AnyOrTwitch &&
        highlightColor.isValid())
    {
        return highlightColor;
    }

    return fallbackPlatformHighlightColor(message, highlightColor);
}

bool automaticEventHighlightUsesGradient(const Message &message,
                                         const MessagePaintContext &ctx)
{
    if (ctx.forceFlatEventHighlights)
    {
        return false;
    }

    const auto style =
        isPlatformAlertMessage(message)
            ? platformAlertHighlightStyleSetting().getEnum()
            : getSettings()->platformEventHighlightStyle.getEnum();
    return style == PlatformEventHighlightStyle::Gradient;
}

bool shouldHideBlockedTermAutomodMessages()
{
    return getSettings()
               ->showBlockedTermAutomodMessages.getValueCopy()
               .compare(QStringLiteral("Never"), Qt::CaseInsensitive) == 0;
}

bool isPlatformAlertMessage(const Message &message)
{
    return message.flags.has(MessageFlag::ModerationAction) &&
           !message.flags.has(MessageFlag::AutoMod) &&
           !message.flags.has(MessageFlag::LowTrustUsers);
}

bool automaticEventIncludesUserMessage(const Message &message)
{
    for (const auto &element : message.elements)
    {
        if (dynamic_cast<const MentionElement *>(element.get()) != nullptr)
        {
            continue;
        }

        if (const auto *text = dynamic_cast<const TextElement *>(element.get()))
        {
            if (text->color().type() != MessageColor::System)
            {
                return true;
            }
        }
        else if (const auto *singleLine =
                     dynamic_cast<const SingleLineTextElement *>(
                         element.get()))
        {
            if (singleLine->color().type() != MessageColor::System)
            {
                return true;
            }
        }
    }

    return false;
}

bool usesAutomaticEventOverlay(const Message &message,
                               const MessagePreferences &preferences,
                               bool includeActivityCheerOverlay)
{
    if (message.flags.has(MessageFlag::ElevatedMessage) &&
        preferences.enableElevatedMessageHighlight)
    {
        return true;
    }

    if ((message.flags.has(MessageFlag::FirstMessage) &&
         preferences.enableFirstMessageHighlight) ||
        (message.flags.has(MessageFlag::FirstMessageSession) &&
         preferences.enableFirstMessageSessionHighlight))
    {
        return true;
    }

    if (message.flags.has(MessageFlag::WatchStreak) &&
        preferences.enableWatchStreakHighlight)
    {
        return true;
    }

    if (message.flags.has(MessageFlag::Subscription) &&
        preferences.enableSubHighlight)
    {
        return true;
    }

    if ((message.flags.has(MessageFlag::RedeemedHighlight) ||
         message.flags.has(MessageFlag::RedeemedChannelPointReward)) &&
        preferences.enableRedeemedHighlight)
    {
        return true;
    }

    if (includeActivityCheerOverlay &&
        message.flags.has(MessageFlag::CheerMessage))
    {
        return true;
    }

    return isPlatformAlertMessage(message);
}

bool hasEnabledFirstMessageHighlight(const Message &message,
                                     const MessagePreferences &preferences)
{
    return (message.flags.has(MessageFlag::FirstMessage) &&
            preferences.enableFirstMessageHighlight) ||
           (message.flags.has(MessageFlag::FirstMessageSession) &&
            preferences.enableFirstMessageSessionHighlight);
}

QColor brightenGradientColor(const QColor &color)
{
    auto brighter = color.lighter(130);
    brighter.setAlpha(std::min(255, qRound(color.alpha() * 1.25)));
    return brighter;
}

bool applyAutomaticEventOverlay(const Message &message, const QColor &baseColor,
                                bool brightenGradient,
                                const MessagePaintContext &ctx,
                                QColor &gradientOverlayColor,
                                QColor &solidOverlayColor)
{
    auto resolvedColor =
        automaticEventHighlightColor(message, baseColor, ctx);
    if (!resolvedColor.isValid())
    {
        return false;
    }

    if (automaticEventHighlightUsesGradient(message, ctx))
    {
        gradientOverlayColor =
            brightenGradient ? brightenGradientColor(resolvedColor)
                             : resolvedColor;
    }
    else
    {
        solidOverlayColor = resolvedColor;
    }

    return true;
}

QColor firstMessageGradientLeadInColor(const QColor &baseColor,
                                       const QColor &gradientColor)
{
    if (!baseColor.isValid())
    {
        return {};
    }

    auto leadIn = baseColor;
    leadIn.setAlpha(gradientColor.alpha());
    return leadIn;
}

void fillLeadingFade(QPainter &painter, const QRect &rect,
                     const QColor &leadingColor,
                     const QColor &leadInColor = {})
{
    if (!leadingColor.isValid() || leadingColor.alpha() == 0)
    {
        return;
    }

    QLinearGradient gradient(rect.left(), 0, rect.right(), 0);
    QColor transparent(leadingColor);
    transparent.setAlpha(0);

    if (leadInColor.isValid() && leadInColor.alpha() > 0)
    {
        gradient.setColorAt(0.0, leadInColor);
        gradient.setColorAt(0.1, leadingColor);
    }
    else
    {
        gradient.setColorAt(0.0, leadingColor);
    }

    gradient.setColorAt(0.5, transparent);
    gradient.setColorAt(1.0, transparent);
    painter.fillRect(rect, gradient);
}
}  // namespace

MessageLayout::MessageLayout(MessagePtr message)
    : message_(std::move(message))
{
    DebugCount::increase(DebugObject::MessageLayout);
}

MessageLayout::~MessageLayout()
{
    DebugCount::decrease(DebugObject::MessageLayout);
}

const Message *MessageLayout::getMessage()
{
    return this->message_.get();
}

const MessagePtr &MessageLayout::getMessagePtr() const
{
    return this->message_;
}

// Height
int MessageLayout::getHeight() const
{
    return static_cast<int>(this->container_.getHeight());
}

int MessageLayout::getWidth() const
{
    return static_cast<int>(this->container_.getWidth());
}

void MessageLayout::paintBackground(QPainter &painter, const QRect &rect,
                                    const MessagePaintContext &ctx) const
{
    if (rect.isEmpty())
    {
        return;
    }

    QColor backgroundColor = [&] {
        if (this->message_->flags.has(MessageFlag::WatchStreak) &&
            ctx.preferences.enableWatchStreakHighlight)
        {
            return ctx.messageColors.regularBg;
        }

        if (ctx.preferences.alternateMessages &&
            this->flags.has(MessageLayoutFlag::AlternateBackground))
        {
            return ctx.messageColors.alternateBg;
        }

        return ctx.messageColors.regularBg;
    }();
    QColor solidOverlayColor;
    QColor gradientOverlayColor;
    QColor gradientLeadInColor;

    const bool isWatchStreakEvent =
        this->message_->flags.has(MessageFlag::WatchStreak) &&
        ctx.preferences.enableWatchStreakHighlight;

    bool suppressMergedPlatformTint =
        usesAutomaticEventOverlay(*this->message_, ctx.preferences,
                                  ctx.forceFlatEventHighlights) &&
        ((ctx.forceFlatEventHighlights &&
          mergedPlatformIndicatorShowsLineColor(ctx.platformIndicatorMode)) ||
         (automaticEventHighlightUsesGradient(*this->message_, ctx) &&
          (isWatchStreakEvent ||
           !automaticEventIncludesUserMessage(*this->message_))));

    if (this->message_->platformAccentColor &&
        mergedPlatformIndicatorShowsLineColor(ctx.platformIndicatorMode) &&
        !suppressMergedPlatformTint)
    {
        backgroundColor = blendColors(backgroundColor,
                                      *this->message_->platformAccentColor);
    }

    if (this->message_->flags.has(MessageFlag::ElevatedMessage) &&
        ctx.preferences.enableElevatedMessageHighlight)
    {
        applyAutomaticEventOverlay(*this->message_,
                                   *ctx.colorProvider.color(
                                       ColorType::ElevatedMessageHighlight),
                                   false, ctx, gradientOverlayColor,
                                   solidOverlayColor);
    }

    else if (hasEnabledFirstMessageHighlight(*this->message_,
                                             ctx.preferences))
    {
        auto firstMessageBaseColor =
            *ctx.colorProvider.color(ColorType::FirstMessageHighlight);
        applyAutomaticEventOverlay(*this->message_, firstMessageBaseColor,
                                   false, ctx, gradientOverlayColor,
                                   solidOverlayColor);
        if (gradientOverlayColor.isValid())
        {
            gradientLeadInColor = firstMessageGradientLeadInColor(
                firstMessageBaseColor, gradientOverlayColor);
        }
    }
    else if (this->message_->flags.has(MessageFlag::WatchStreak) &&
             ctx.preferences.enableWatchStreakHighlight)
    {
        applyAutomaticEventOverlay(
            *this->message_, *ctx.colorProvider.color(ColorType::WatchStreak),
            false, ctx, gradientOverlayColor, solidOverlayColor);
    }
    else if (ctx.forceFlatEventHighlights &&
             this->message_->flags.has(MessageFlag::CheerMessage))
    {
        applyAutomaticEventOverlay(*this->message_, {}, false, ctx,
                                   gradientOverlayColor, solidOverlayColor);
    }
    else if ((this->message_->flags.has(MessageFlag::Highlighted) ||
              this->message_->flags.has(MessageFlag::HighlightedWhisper)) &&
             !this->flags.has(MessageLayoutFlag::IgnoreHighlights))
    {
        assert(this->message_->highlightColor);
        solidOverlayColor = this->message_->highlightColor
                                ? *this->message_->highlightColor
                                : QColor{};
    }
    else if (this->message_->flags.has(MessageFlag::Subscription) &&
             ctx.preferences.enableSubHighlight)
    {
        applyAutomaticEventOverlay(
            *this->message_, *ctx.colorProvider.color(ColorType::Subscription),
            false, ctx, gradientOverlayColor, solidOverlayColor);
    }
    else if ((this->message_->flags.has(MessageFlag::RedeemedHighlight) ||
              this->message_->flags.has(
                  MessageFlag::RedeemedChannelPointReward)) &&
             ctx.preferences.enableRedeemedHighlight)
    {
        applyAutomaticEventOverlay(
            *this->message_,
            *ctx.colorProvider.color(ColorType::RedeemedHighlight), false, ctx,
            gradientOverlayColor, solidOverlayColor);
    }
    else if (isPlatformAlertMessage(*this->message_))
    {
        applyAutomaticEventOverlay(*this->message_, {}, true, ctx,
                                   gradientOverlayColor, solidOverlayColor);
    }
    else if (this->message_->flags.has(MessageFlag::AutoMod) ||
             this->message_->flags.has(MessageFlag::LowTrustUsers))
    {
        if (ctx.preferences.enableAutomodHighlight &&
            (this->message_->flags.has(MessageFlag::AutoModOffendingMessage) ||
             this->message_->flags.has(
                 MessageFlag::AutoModOffendingMessageHeader)))
        {
            solidOverlayColor =
                *ctx.colorProvider.color(ColorType::AutomodHighlight);
        }
        else
        {
            backgroundColor = QColor("#404040");
        }
    }
    else if (this->message_->flags.has(MessageFlag::Debug))
    {
        backgroundColor = QColor("#4A273D");
    }

    painter.fillRect(rect, backgroundColor);
    if (gradientOverlayColor.isValid())
    {
        fillLeadingFade(painter, rect, gradientOverlayColor,
                        gradientLeadInColor);
    }
    else if (solidOverlayColor.isValid())
    {
        painter.fillRect(rect, solidOverlayColor);
    }
}

// Layout
// return true if redraw is required
bool MessageLayout::layout(const MessageLayoutContext &ctx,
                           bool shouldInvalidateBuffer)
{
    //    BenchmarkGuard benchmark("MessageLayout::layout()");

    bool layoutRequired = false;

    // check if width changed
    bool widthChanged = ctx.width != this->currentLayoutWidth_;
    layoutRequired |= widthChanged;
    this->currentLayoutWidth_ = ctx.width;

    // check if layout state changed
    const auto layoutGeneration = getApp()->getWindows()->getGeneration();
    if (this->layoutState_ != layoutGeneration)
    {
        layoutRequired = true;
        this->flags.set(MessageLayoutFlag::RequiresBufferUpdate);
        this->layoutState_ = layoutGeneration;
    }

    // check if work mask changed
    layoutRequired |= this->currentWordFlags_ != ctx.flags;
    this->currentWordFlags_ = ctx.flags;  // getSettings()->getWordTypeMask();

    // check if layout was requested manually
    layoutRequired |= this->flags.has(MessageLayoutFlag::RequiresLayout);
    this->flags.unset(MessageLayoutFlag::RequiresLayout);

    // check if dpi changed
    layoutRequired |= this->scale_ != ctx.scale;
    this->scale_ = ctx.scale;
    layoutRequired |= this->imageScale_ != ctx.imageScale;
    this->imageScale_ = ctx.imageScale;

    if (!layoutRequired)
    {
        if (shouldInvalidateBuffer)
        {
            this->invalidateBuffer();
            return true;
        }
        return false;
    }

    qreal oldHeight = this->container_.getHeight();
    this->actuallyLayout(ctx);
    if (widthChanged || this->container_.getHeight() != oldHeight)
    {
        this->deleteBuffer();
    }
    this->invalidateBuffer();

    return true;
}

void MessageLayout::actuallyLayout(const MessageLayoutContext &ctx)
{
#ifdef FOURTF
    this->layoutCount_++;
#endif

    auto messageFlags = this->message_->flags;

    if (this->flags.has(MessageLayoutFlag::Expanded) ||
        (ctx.flags.has(MessageElementFlag::ModeratorTools) &&
         !this->message_->flags.has(MessageFlag::Disabled)))
    {
        messageFlags.unset(MessageFlag::Collapsed);
    }

    bool hideModerated = getSettings()->hideModerated;
    bool hideModerationActions = getSettings()->hideModerationActions;
    bool hideBlockedTermAutomodMessages =
        shouldHideBlockedTermAutomodMessages();
    bool hideSimilar = getSettings()->hideSimilar;
    bool hideReplies = !ctx.flags.has(MessageElementFlag::RepliedMessage);

    this->container_.beginLayout(ctx.width, this->scale_, this->imageScale_,
                                 messageFlags);

    const bool showLinkPreviews =
        linkPreviewModeSetting().getEnum() != LinkPreviewMode::Disabled &&
        !shouldSuppressLinkPreview(*this->message_);
    std::vector<LinkElement *> previewLinks;
    QSet<QString> seenPreviewUrls;

    for (const auto &element : this->message_->elements)
    {
        if (hideModerated && this->message_->flags.has(MessageFlag::Disabled))
        {
            continue;
        }

        if (hideBlockedTermAutomodMessages &&
            this->message_->flags.has(MessageFlag::AutoModBlockedTerm))
        {
            // NOTE: This hides the message but it will make the message re-appear if moderation message hiding is no longer active, and the layout is re-laid-out.
            // This is only the case for the moderation messages that don't get filtered during creation.
            // We should decide which is the correct method & apply that everywhere
            continue;
        }

        if (this->message_->flags.has(MessageFlag::RestrictedMessage))
        {
            if (getApp()->getStreamerMode()->shouldHideRestrictedUsers())
            {
                // Message is being hidden because the source is a
                // restricted user
                continue;
            }
        }

        if (this->message_->flags.has(MessageFlag::ModerationAction))
        {
            if (hideModerationActions ||
                getApp()->getStreamerMode()->shouldHideModActions())
            {
                // Message is being hidden because we consider the message
                // a moderation action (something a streamer is unlikely to
                // want to share if they briefly show their chat on stream)
                continue;
            }
        }

        if (hideSimilar && this->message_->flags.has(MessageFlag::Similar))
        {
            continue;
        }

        if (hideReplies &&
            element->getFlags().has(MessageElementFlag::RepliedMessage))
        {
            continue;
        }

        element->addToContainer(this->container_, ctx);

        if (showLinkPreviews)
        {
            auto *linkElement = dynamic_cast<LinkElement *>(element.get());
            if (linkElement)
            {
                auto *linkInfo = linkElement->linkInfo();
                if (!shouldShowLinkPreview(linkInfo->originalUrl()))
                {
                    continue;
                }
                if (linkInfo->isPending())
                {
                    getApp()->getLinkResolver()->resolve(linkInfo);
                }

                const auto previewKey = linkInfo->originalUrl();
                if (linkInfo->hasPreview() &&
                    !seenPreviewUrls.contains(previewKey))
                {
                    seenPreviewUrls.insert(previewKey);
                    previewLinks.push_back(linkElement);
                }
            }
        }
    }

    const qreal previewWidth =
        std::min<qreal>(300 * this->scale_,
                        ctx.width - (32 * this->scale_));
    if (previewWidth >= 220 * this->scale_)
    {
        for (auto *linkElement : previewLinks)
        {
            if (!this->container_.atStartOfLine())
            {
                this->container_.breakLine();
            }

            auto *linkInfo = linkElement->linkInfo();
            ImagePtr thumbnail =
                linkInfo->hasThumbnail() ? linkInfo->thumbnail() : nullptr;

            const auto previewSize = LinkPreviewLayoutElement::calculateSize(
                linkInfo->previewTitle(), linkInfo->previewSubtitle(),
                linkInfo->previewSiteName(), thumbnail, this->scale_,
                previewWidth);
            this->container_.addElementNoLineBreak(new LinkPreviewLayoutElement(
                *linkElement, linkInfo->previewTitle(),
                linkInfo->previewSubtitle(), linkInfo->previewSiteName(),
                std::move(thumbnail), linkInfo->previewAccentColor(),
                this->scale_, previewSize));
            this->container_.breakLine();
        }
    }

    if (this->height_ != this->container_.getHeight())
    {
        this->deleteBuffer();
    }

    this->container_.endLayout();
    this->height_ = this->container_.getHeight();

    // collapsed state
    this->flags.unset(MessageLayoutFlag::Collapsed);
    if (this->container_.isCollapsed())
    {
        this->flags.set(MessageLayoutFlag::Collapsed);
    }
}

// Painting
MessagePaintResult MessageLayout::paint(const MessagePaintContext &ctx)
{
    MessagePaintResult result;

    QPixmap *pixmap = this->ensureBuffer(ctx.painter, ctx.canvasWidth,
                                         ctx.messageColors.hasTransparency);

    if (!this->bufferValid_)
    {
        if (ctx.messageColors.hasTransparency)
        {
            pixmap->fill(Qt::transparent);
        }
        this->updateBuffer(pixmap, ctx);
    }

    // draw on buffer
    ctx.painter.drawPixmap(QPoint{0, ctx.y}, *pixmap);

    // draw gif emotes
    result.hasAnimatedElements =
        this->container_.paintAnimatedElements(ctx.painter, ctx.y);

    // draw disabled
    if (this->message_->flags.has(MessageFlag::Disabled))
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (this->message_->flags.has(MessageFlag::RecentMessage) &&
        ctx.preferences.fadeMessageHistory)
    {
        ctx.painter.fillRect(
            QRect{
                0,
                ctx.y,
                pixmap->width(),
                pixmap->height(),
            },
            ctx.messageColors.disabled);
    }

    if (!ctx.isMentions &&
        (this->message_->flags.has(MessageFlag::RedeemedChannelPointReward) ||
         this->message_->flags.has(MessageFlag::RedeemedHighlight)) &&
        ctx.preferences.enableRedeemedHighlight)
    {
        auto redeemedStripeColor = automaticEventHighlightColor(
            *this->message_,
            *ColorProvider::instance().color(ColorType::RedeemedHighlight),
            ctx);
        if (redeemedStripeColor.isValid())
        {
            ctx.painter.fillRect(
                QRect{
                    0,
                    ctx.y,
                    static_cast<int>(this->scale_ * 4),
                    pixmap->height(),
                },
                redeemedStripeColor);
        }
    }

    // draw selection
    if (!ctx.selection.isEmpty())
    {
        this->container_.paintSelection(ctx.painter, ctx.messageIndex,
                                        ctx.selection, ctx.y);
    }

    // draw message seperation line
    if (ctx.preferences.separateMessages)
    {
        ctx.painter.fillRect(
            QRectF{
                0.0,
                static_cast<qreal>(ctx.y),
                this->container_.getWidth() + 64,
                1.0,
            },
            ctx.messageColors.messageSeperator);
    }

    // draw last read message line
    if (ctx.isLastReadMessage)
    {
        QColor color;
        if (ctx.preferences.lastMessageColor.isValid())
        {
            color = ctx.preferences.lastMessageColor;
        }
        else
        {
            color = ctx.isWindowFocused
                        ? ctx.messageColors.focusedLastMessageLine
                        : ctx.messageColors.unfocusedLastMessageLine;
        }

        QBrush brush(color, ctx.preferences.lastMessagePattern);

        ctx.painter.fillRect(
            QRectF{
                0,
                ctx.y + this->container_.getHeight() - 1,
                static_cast<qreal>(pixmap->width()),
                1,
            },
            brush);
    }

    this->bufferValid_ = true;

    return result;
}

QPixmap *MessageLayout::ensureBuffer(QPainter &painter, qreal width, bool clear)
{
    if (this->buffer_ != nullptr)
    {
        return this->buffer_.get();
    }

    // Create new buffer
    this->buffer_ = std::make_unique<QPixmap>(
        static_cast<int>(width * painter.device()->devicePixelRatioF()),
        static_cast<int>(this->container_.getHeight() *
                         painter.device()->devicePixelRatioF()));
    this->buffer_->setDevicePixelRatio(painter.device()->devicePixelRatioF());

    if (clear)
    {
        this->buffer_->fill(Qt::transparent);
    }

    this->bufferValid_ = false;
    DebugCount::increase(DebugObject::MessageDrawingBuffer);
    return this->buffer_.get();
}

void MessageLayout::updateBuffer(QPixmap *buffer,
                                 const MessagePaintContext &ctx)
{
    if (buffer->isNull())
    {
        return;
    }

    QPainter painter(buffer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    this->paintBackground(painter, buffer->rect(), ctx);

    // draw message
    this->container_.paintElements(painter, ctx);

#ifdef FOURTF
    // debug
    painter.setPen(QColor(255, 0, 0));
    painter.drawRect(buffer->rect().x(), buffer->rect().y(),
                     buffer->rect().width() - 1, buffer->rect().height() - 1);

    QTextOption option;
    option.setAlignment(Qt::AlignRight | Qt::AlignTop);

    painter.drawText(QRectF(1, 1, this->container_.getWidth() - 3, 1000),
                     QString::number(this->layoutCount_) + ", " +
                         QString::number(++this->bufferUpdatedCount_),
                     option);
#endif
}

void MessageLayout::invalidateBuffer()
{
    this->bufferValid_ = false;
}

void MessageLayout::deleteBuffer()
{
    if (this->buffer_ != nullptr)
    {
        DebugCount::decrease(DebugObject::MessageDrawingBuffer);

        this->buffer_ = nullptr;
    }
}

void MessageLayout::deleteCache()
{
    this->deleteBuffer();

#ifdef XD
    this->container_.clear();
#endif
}

// Elements
//    assert(QThread::currentThread() == QApplication::instance()->thread());

// returns nullptr if none was found

// fourtf: this should return a MessageLayoutItem
const MessageLayoutElement *MessageLayout::getElementAt(QPointF point) const
{
    // go through all words and return the first one that contains the point.
    return this->container_.getElementAt(point);
}

std::pair<int, int> MessageLayout::getWordBounds(
    const MessageLayoutElement *hoveredElement, QPointF relativePos) const
{
    // An element with wordId != -1 can be multiline, so we need to check all
    // elements in the container
    if (hoveredElement->getWordId() != -1)
    {
        return this->container_.getWordBounds(hoveredElement);
    }

    const auto wordStart = this->getSelectionIndex(relativePos) -
                           hoveredElement->getMouseOverIndex(relativePos);
    const auto selectionLength = hoveredElement->getSelectionIndexCount();
    const auto length = hoveredElement->hasTrailingSpace() ? selectionLength - 1
                                                           : selectionLength;

    return {wordStart, wordStart + length};
}

size_t MessageLayout::getLastCharacterIndex() const
{
    return this->container_.getLastCharacterIndex();
}

size_t MessageLayout::getFirstMessageCharacterIndex() const
{
    return this->container_.getFirstMessageCharacterIndex();
}

size_t MessageLayout::getSelectionIndex(QPointF position) const
{
    return this->container_.getSelectionIndex(position);
}

void MessageLayout::addSelectionText(QString &str, uint32_t from, uint32_t to,
                                     CopyMode copymode)
{
    this->container_.addSelectionText(str, from, to, copymode);
}

}  // namespace chatterino
