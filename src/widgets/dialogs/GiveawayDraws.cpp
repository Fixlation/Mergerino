// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/GiveawayDraws.hpp"

#include "messages/Emote.hpp"
#include "messages/Image.hpp"

#include <QEasingCurve>
#include <QFontMetricsF>
#include <QIcon>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QtMath>
#include <QVariantAnimation>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace chatterino {

namespace {

constexpr int REEL_CARDS = 72;
constexpr int REEL_WINNER_SLOT = 62;
constexpr double REEL_CARD_WIDTH = 104;
constexpr double REEL_PITCH = 110;
constexpr int REEL_SPIN_MS = 6000;
constexpr int REEL_SETTLE_MS = 450;
constexpr int WHEEL_SPIN_MS = 7000;
constexpr int WHEEL_RESULT_MS = 4000;
constexpr double WHEEL_MIN_WEDGE = 2.5;
constexpr int WHEEL_READOUT_HEIGHT = 22;

const QColor MARKER_GOLD(0xe4, 0xae, 0x39);

QColor mixColors(const QColor &from, const QColor &to, double amount)
{
    const auto mix = [amount](int a, int b) {
        return static_cast<int>(std::lround(a + (b - a) * amount));
    };
    return {mix(from.red(), to.red()), mix(from.green(), to.green()),
            mix(from.blue(), to.blue()), mix(from.alpha(), to.alpha())};
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

QColor platformColor(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return {0x53, 0xc8, 0x3a};
        case MessagePlatform::YouTube:
            return {0xeb, 0x4b, 0x4b};
        case MessagePlatform::TikTok:
            return {0x25, 0xc7, 0xd9};
        case MessagePlatform::AnyOrTwitch:
        default:
            return {0x4b, 0x69, 0xff};
    }
}

// Card stripe: the entrant's platform, or gold for boosted entrants
QColor cardStripe(const GiveawayDrawEntry &entry)
{
    return entry.weight > 1 ? MARKER_GOLD : platformColor(entry.platform);
}

const std::array<QColor, 8> &wheelPalette()
{
    static const std::array<QColor, 8> palette{
        QColor(0xe6, 0xb4, 0x50), QColor(0x4e, 0xa8, 0xde),
        QColor(0x3f, 0xbf, 0xa6), QColor(0xef, 0x6f, 0x5e),
        QColor(0x8c, 0xc1, 0x52), QColor(0xf3, 0x9c, 0x4a),
        QColor(0x6c, 0x8e, 0xad), QColor(0xd9, 0xc3, 0x8c),
    };
    return palette;
}

double easeOut(double progress)
{
    return QEasingCurve(QEasingCurve::OutQuart).valueForProgress(progress);
}

}  // namespace

QString giveawayPlatformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral(":/platforms/kick.svg");
        case MessagePlatform::YouTube:
            return QStringLiteral(":/platforms/youtube.svg");
        case MessagePlatform::TikTok:
            return QStringLiteral(":/platforms/tiktok.svg");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral(":/platforms/twitch.svg");
    }
}

int pickWeightedGiveawayEntry(const std::vector<GiveawayDrawEntry> &entries)
{
    quint64 total = 0;
    for (const auto &entry : entries)
    {
        total += static_cast<quint64>(std::max(1, entry.weight));
    }
    if (total == 0)
    {
        return -1;
    }

    // Reject the biased tail so every ticket is equally likely
    const auto rejectionThreshold =
        (std::numeric_limits<quint64>::max() - total + 1) % total;
    quint64 randomValue = 0;
    do
    {
        randomValue = QRandomGenerator::global()->generate64();
    } while (randomValue < rejectionThreshold);
    auto ticket = randomValue % total;

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto weight =
            static_cast<quint64>(std::max(1, entries[i].weight));
        if (ticket < weight)
        {
            return static_cast<int>(i);
        }
        ticket -= weight;
    }
    return static_cast<int>(entries.size()) - 1;
}

// ---------------------------------------------------------------- reel

GiveawayReel::GiveawayReel(QWidget *parent)
    : QWidget(parent)
    , spin_(new QVariantAnimation(this))
    , settle_(new QVariantAnimation(this))
{
    this->setObjectName(QStringLiteral("GiveawayReel"));
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    this->spin_->setDuration(REEL_SPIN_MS);
    this->spin_->setStartValue(0.0);
    this->spin_->setEndValue(1.0);
    QObject::connect(this->spin_, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         this->offset_ = this->travelFor(value.toDouble());
                         this->update();
                     });
    QObject::connect(this->spin_, &QVariantAnimation::finished, this, [this] {
        if (!this->spinning_)
        {
            return;
        }
        this->settle_->setStartValue(this->landingOffset_);
        this->settle_->setEndValue(this->restingOffset_);
        this->settle_->start();
    });

    this->settle_->setDuration(REEL_SETTLE_MS);
    this->settle_->setEasingCurve(QEasingCurve::InOutCubic);
    QObject::connect(this->settle_, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         this->offset_ = value.toDouble();
                         this->update();
                     });
    QObject::connect(this->settle_, &QVariantAnimation::finished, this,
                     [this] {
                         if (this->spinning_)
                         {
                             this->land();
                         }
                     });
}

GiveawayReel::~GiveawayReel() = default;

void GiveawayReel::setColors(const GiveawayDrawColors &colors)
{
    this->colors_ = colors;
    this->update();
}

void GiveawayReel::spin(const std::vector<GiveawayDrawEntry> &entries,
                        int winner)
{
    this->stop();
    if (winner < 0 || winner >= static_cast<int>(entries.size()))
    {
        return;
    }

    this->cards_.clear();
    this->cards_.reserve(REEL_CARDS);
    for (int i = 0; i < REEL_CARDS; ++i)
    {
        const auto pick = i == REEL_WINNER_SLOT
                              ? winner
                              : pickWeightedGiveawayEntry(entries);
        this->cards_.push_back({entries[static_cast<size_t>(pick)]});
    }
    this->winnerSlot_ = REEL_WINNER_SLOT;

    // Come to rest somewhere inside the winning card, then ease onto its
    // centre like the real thing
    const auto landing =
        0.1 + 0.8 * QRandomGenerator::global()->generateDouble();
    this->landingOffset_ =
        this->winnerSlot_ * REEL_PITCH + landing * REEL_CARD_WIDTH;
    this->restingOffset_ =
        this->winnerSlot_ * REEL_PITCH + REEL_CARD_WIDTH / 2;
    this->offset_ = this->travelFor(0);
    this->spinning_ = true;
    this->landed_ = false;
    this->spin_->start();
    this->update();
}

double GiveawayReel::travelFor(double progress) const
{
    const double start = 2.5 * REEL_PITCH;
    return start + (this->landingOffset_ - start) * easeOut(progress);
}

void GiveawayReel::finish()
{
    if (!this->spinning_)
    {
        return;
    }
    this->spin_->stop();
    this->settle_->stop();
    this->offset_ = this->restingOffset_;
    this->land();
}

void GiveawayReel::land()
{
    this->spinning_ = false;
    this->landed_ = true;
    this->update();
    if (this->onLanded)
    {
        this->onLanded();
    }
}

void GiveawayReel::stop()
{
    this->spinning_ = false;
    this->landed_ = false;
    this->spin_->stop();
    this->settle_->stop();
    this->update();
}

const QPixmap &GiveawayReel::platformIcon(MessagePlatform platform)
{
    const auto ratio = this->devicePixelRatioF();
    if (!qFuzzyCompare(this->platformIconRatio_, ratio))
    {
        this->platformIcons_ = {};
        this->platformIconRatio_ = ratio;
    }
    const auto index = std::min<size_t>(static_cast<size_t>(platform),
                                        this->platformIcons_.size() - 1);
    auto &icon = this->platformIcons_[index];
    if (icon.isNull())
    {
        icon = QIcon(giveawayPlatformIconPath(platform))
                   .pixmap(QSize(16, 16), ratio);
    }
    return icon;
}

bool GiveawayReel::spinning() const
{
    return this->spinning_;
}

bool GiveawayReel::landed() const
{
    return this->landed_;
}

void GiveawayReel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && this->landed_ &&
        this->onDismissed)
    {
        this->onDismissed();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GiveawayReel::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF track = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath trackPath;
    trackPath.addRoundedRect(track, 6, 6);
    painter.fillPath(trackPath, this->colors_.background);
    painter.save();
    painter.setClipPath(trackPath);

    const double center = this->width() / 2.0;
    const double cardTop = 11;
    const double cardHeight = this->height() - 2 * cardTop;
    const auto cardColor =
        mixColors(this->colors_.panel, this->colors_.text, 0.07);
    auto nameFont = this->font();
    nameFont.setBold(true);
    auto smallFont = this->font();
    smallFont.setPointSizeF(std::max(6.0, smallFont.pointSizeF() * 0.85));
    const QFontMetricsF nameMetrics(nameFont);

    const int firstVisible = std::max(
        0, static_cast<int>(std::floor((this->offset_ - center) / REEL_PITCH)));
    const int lastVisible =
        std::min(static_cast<int>(this->cards_.size()) - 1,
                 static_cast<int>(std::ceil((this->offset_ + center) /
                                            REEL_PITCH)));
    for (int i = firstVisible; i <= lastVisible; ++i)
    {
        const auto &card = this->cards_[static_cast<size_t>(i)];
        const QRectF rect(i * REEL_PITCH - this->offset_ + center, cardTop,
                          REEL_CARD_WIDTH, cardHeight);
        const bool isWinner = this->landed_ && i == this->winnerSlot_;
        painter.setOpacity(this->landed_ && !isWinner ? 0.4 : 1.0);

        QPainterPath cardPath;
        cardPath.addRoundedRect(rect, 4, 4);
        painter.fillPath(cardPath, cardColor);

        const auto stripe = cardStripe(card.entry);
        painter.save();
        painter.setClipPath(cardPath);
        QLinearGradient glow(rect.bottomLeft(),
                             rect.bottomLeft() - QPointF(0, rect.height() * 0.6));
        glow.setColorAt(0, withAlpha(stripe, isWinner ? 110 : 70));
        glow.setColorAt(1, withAlpha(stripe, 0));
        painter.fillRect(rect, glow);
        painter.fillRect(QRectF(rect.left(), rect.bottom() - 4, rect.width(), 4),
                         stripe);
        painter.restore();

        painter.drawPixmap(QPointF(rect.left() + 8, rect.top() + 8),
                           this->platformIcon(card.entry.platform));
        if (card.entry.weight > 1)
        {
            painter.setFont(smallFont);
            painter.setPen(this->colors_.muted);
            painter.drawText(rect.adjusted(0, 6, -8, 0),
                             Qt::AlignRight | Qt::AlignTop,
                             QStringLiteral("×%1").arg(card.entry.weight));
        }

        painter.setFont(nameFont);
        painter.setPen(this->colors_.text);
        const auto name = nameMetrics.elidedText(card.entry.name,
                                                 Qt::ElideRight,
                                                 rect.width() - 14);
        painter.drawText(rect.adjusted(7, 0, -7, -8), Qt::AlignCenter, name);

        if (isWinner)
        {
            painter.setOpacity(1.0);
            painter.setPen(QPen(MARKER_GOLD, 2));
            painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 4, 4);
        }
    }
    painter.setOpacity(1.0);

    // Fade the cards out towards both edges
    const double fade = std::min(64.0, this->width() / 5.0);
    QLinearGradient left(track.left(), 0, track.left() + fade, 0);
    left.setColorAt(0, this->colors_.background);
    left.setColorAt(1, withAlpha(this->colors_.background, 0));
    painter.fillRect(QRectF(track.left(), 0, fade, this->height()), left);
    QLinearGradient right(track.right(), 0, track.right() - fade, 0);
    right.setColorAt(0, this->colors_.background);
    right.setColorAt(1, withAlpha(this->colors_.background, 0));
    painter.fillRect(QRectF(track.right() - fade, 0, fade, this->height()),
                     right);

    // Marker
    if (!this->cards_.empty())
    {
        painter.fillRect(QRectF(center - 1, 0, 2, this->height()),
                         MARKER_GOLD);
        QPainterPath arrows;
        arrows.moveTo(center - 7, 0);
        arrows.lineTo(center + 7, 0);
        arrows.lineTo(center, 8);
        arrows.closeSubpath();
        arrows.moveTo(center - 7, this->height());
        arrows.lineTo(center + 7, this->height());
        arrows.lineTo(center, this->height() - 8);
        arrows.closeSubpath();
        painter.fillPath(arrows, MARKER_GOLD);
    }
    painter.restore();

    painter.setPen(QPen(this->colors_.border, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(trackPath);
}

// ---------------------------------------------------------------- wheel

GiveawayWheel::GiveawayWheel(QWidget *parent)
    : QWidget(parent)
    , spin_(new QVariantAnimation(this))
{
    this->setObjectName(QStringLiteral("GiveawayWheel"));
    this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    this->spin_->setDuration(WHEEL_SPIN_MS);
    this->spin_->setStartValue(0.0);
    this->spin_->setEndValue(1.0);
    QObject::connect(
        this->spin_, &QVariantAnimation::valueChanged, this,
        [this](const QVariant &value) {
            this->rotation_ = this->spinFrom_ + (this->spinTo_ - this->spinFrom_) *
                                                    easeOut(value.toDouble());
            const auto slice = this->sliceAt(-this->rotation_);
            if (slice != this->lastSlice_)
            {
                this->lastSlice_ = slice;
                this->pointerKick_ = std::min(this->pointerKick_, -16.0);
                if (!this->pointerTimer_.isActive())
                {
                    this->pointerTimer_.start();
                }
            }
            this->update();
        });
    QObject::connect(this->spin_, &QVariantAnimation::finished, this, [this] {
        if (this->spinning_)
        {
            this->finish();
        }
    });

    this->resultTimer_.setSingleShot(true);
    this->resultTimer_.setInterval(WHEEL_RESULT_MS);
    QObject::connect(&this->resultTimer_, &QTimer::timeout, this, [this] {
        this->showingResult_ = false;
        this->applyPendingEntries();
        this->update();
    });

    this->pointerTimer_.setInterval(16);
    QObject::connect(&this->pointerTimer_, &QTimer::timeout, this, [this] {
        this->pointerKick_ *= 0.72;
        if (std::abs(this->pointerKick_) < 0.3)
        {
            this->pointerKick_ = 0;
            this->pointerTimer_.stop();
        }
        this->update();
    });
}

GiveawayWheel::~GiveawayWheel() = default;

QSize GiveawayWheel::sizeHint() const
{
    return {220, 228};
}

void GiveawayWheel::setColors(const GiveawayDrawColors &colors)
{
    this->colors_ = colors;
    this->rebuildFace();
    this->update();
}

void GiveawayWheel::setEntries(std::vector<GiveawayDrawEntry> entries)
{
    if (this->spinning_ || this->showingResult_)
    {
        this->pendingEntries_ = std::move(entries);
        this->hasPendingEntries_ = true;
        return;
    }
    if (entries == this->entries_ && !this->hasPendingEntries_)
    {
        return;
    }
    this->entries_ = std::move(entries);
    this->hasPendingEntries_ = false;
    this->pendingEntries_.clear();
    this->winner_ = -1;

    this->sliceStarts_.clear();
    this->sliceStarts_.reserve(this->entries_.size());
    this->totalWeight_ = 0;
    for (const auto &entry : this->entries_)
    {
        this->totalWeight_ += std::max(1, entry.weight);
    }
    double angle = 0;
    for (const auto &entry : this->entries_)
    {
        this->sliceStarts_.push_back(angle);
        angle += 360.0 * std::max(1, entry.weight) / this->totalWeight_;
    }
    this->rebuildFace();
    this->update();
}

const std::vector<GiveawayDrawEntry> &GiveawayWheel::entries() const
{
    return this->entries_;
}

void GiveawayWheel::applyPendingEntries()
{
    if (this->hasPendingEntries_)
    {
        auto entries = std::move(this->pendingEntries_);
        this->hasPendingEntries_ = false;
        this->setEntries(std::move(entries));
    }
}

QRectF GiveawayWheel::wheelRect() const
{
    const double size =
        std::min<double>(this->width() - 4,
                         this->height() - WHEEL_READOUT_HEIGHT - 10);
    return {(this->width() - size) / 2.0, 8, size, size};
}

int GiveawayWheel::sliceAt(double angle) const
{
    if (this->sliceStarts_.empty())
    {
        return -1;
    }
    angle = std::fmod(angle, 360.0);
    if (angle < 0)
    {
        angle += 360.0;
    }
    const auto it = std::upper_bound(this->sliceStarts_.begin(),
                                     this->sliceStarts_.end(), angle);
    return static_cast<int>(std::distance(this->sliceStarts_.begin(), it)) -
           1;
}

void GiveawayWheel::rebuildFace()
{
    const auto rect = this->wheelRect();
    const auto dpr = this->devicePixelRatioF();
    const auto pixels = static_cast<int>(std::ceil(rect.width() * dpr));
    if (pixels <= 0)
    {
        this->face_ = {};
        return;
    }
    this->face_ = QPixmap(pixels, pixels);
    this->face_.setDevicePixelRatio(dpr);
    this->face_.fill(Qt::transparent);

    QPainter painter(&this->face_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRectF disc(0, 0, rect.width(), rect.width());
    const auto center = disc.center();
    const auto radius = disc.width() / 2.0;

    if (this->entries_.empty())
    {
        painter.setPen(QPen(this->colors_.border, 2, Qt::DashLine));
        painter.setBrush(this->colors_.background);
        painter.drawEllipse(disc.adjusted(2, 2, -2, -2));
        return;
    }

    // Thin slices are merged into wedges of at least WHEEL_MIN_WEDGE degrees
    // so huge giveaways stay cheap to draw and readable
    struct Wedge {
        double start;
        double span;
    };
    std::vector<Wedge> wedges;
    double wedgeStart = 0;
    for (size_t i = 0; i < this->entries_.size(); ++i)
    {
        const auto end = i + 1 < this->sliceStarts_.size()
                             ? this->sliceStarts_[i + 1]
                             : 360.0;
        if (end - wedgeStart >= WHEEL_MIN_WEDGE || i + 1 == this->entries_.size())
        {
            wedges.push_back({wedgeStart, end - wedgeStart});
            wedgeStart = end;
        }
    }

    const auto &palette = wheelPalette();
    const auto paletteSize = palette.size();
    for (size_t i = 0; i < wedges.size(); ++i)
    {
        auto index = i % paletteSize;
        if (wedges.size() > 1 && i + 1 == wedges.size() && index == 0)
        {
            index = 1;  // never match the first wedge across the seam
        }
        QPainterPath path;
        path.moveTo(center);
        path.arcTo(disc, 90.0 - wedges[i].start, -wedges[i].span);
        path.closeSubpath();
        painter.fillPath(path, palette[index]);
    }

    if (wedges.size() > 1 && wedges.size() <= 150)
    {
        painter.setPen(QPen(withAlpha(this->colors_.background, 150), 1));
        for (const auto &wedge : wedges)
        {
            const auto radians = qDegreesToRadians(wedge.start - 90.0);
            painter.drawLine(center, center + QPointF(std::cos(radians) * radius,
                                                      std::sin(radians) * radius));
        }
    }

    // Names along the radius when every slice is wide enough to hold one
    const auto smallest =
        360.0 / this->totalWeight_ *
        std::max(1, std::ranges::min_element(this->entries_, {},
                                             &GiveawayDrawEntry::weight)
                        ->weight);
    if (this->entries_.size() <= 40 && smallest >= 7.0)
    {
        auto font = this->font();
        font.setBold(true);
        const auto arcHeight =
            radius * 0.72 * qDegreesToRadians(smallest) * 0.8;
        font.setPixelSize(static_cast<int>(std::clamp(arcHeight, 7.0, 12.0)));
        painter.setFont(font);
        painter.setPen(QColor(0x1b, 0x1b, 0x1f));
        const QFontMetricsF metrics(font);
        const double inner = radius * 0.26;
        const double outer = radius - 9;
        for (size_t i = 0; i < this->entries_.size(); ++i)
        {
            const auto span = (i + 1 < this->sliceStarts_.size()
                                   ? this->sliceStarts_[i + 1]
                                   : 360.0) -
                              this->sliceStarts_[i];
            const auto middle = this->sliceStarts_[i] + span / 2.0;
            painter.save();
            painter.translate(center);
            painter.rotate(middle - 90.0);
            const auto text = metrics.elidedText(this->entries_[i].name,
                                                 Qt::ElideRight, outer - inner);
            painter.drawText(QRectF(inner, -metrics.height() / 2.0,
                                    outer - inner, metrics.height()),
                             Qt::AlignRight | Qt::AlignVCenter, text);
            painter.restore();
        }
    }
}

void GiveawayWheel::spin(int winner)
{
    this->applyPendingEntries();
    if (winner < 0 || winner >= static_cast<int>(this->entries_.size()))
    {
        return;
    }
    this->resultTimer_.stop();
    this->showingResult_ = false;
    this->winner_ = winner;

    const auto start = this->sliceStarts_[static_cast<size_t>(winner)];
    const auto end = static_cast<size_t>(winner) + 1 < this->sliceStarts_.size()
                         ? this->sliceStarts_[static_cast<size_t>(winner) + 1]
                         : 360.0;
    const auto target =
        start + (end - start) *
                    (0.15 + 0.7 * QRandomGenerator::global()->generateDouble());

    // The pointer sits at the top, so the wheel has to end up rotated by
    // -target; add a few full turns for show
    auto delta = std::fmod(-target - this->rotation_, 360.0);
    if (delta < 0)
    {
        delta += 360.0;
    }
    this->rotation_ = std::fmod(this->rotation_, 360.0);
    this->spinFrom_ = this->rotation_;
    this->spinTo_ = this->rotation_ + 5 * 360.0 + delta;
    this->lastSlice_ = this->sliceAt(-this->rotation_);
    this->spinning_ = true;
    this->spin_->start();
}

void GiveawayWheel::finish()
{
    if (!this->spinning_)
    {
        return;
    }
    this->spin_->stop();
    this->rotation_ = this->spinTo_;
    this->spinning_ = false;
    this->showingResult_ = true;
    this->resultTimer_.start();
    this->update();
    if (this->onLanded)
    {
        this->onLanded();
    }
}

void GiveawayWheel::stop()
{
    this->spin_->stop();
    this->resultTimer_.stop();
    this->spinning_ = false;
    this->showingResult_ = false;
    this->winner_ = -1;
    this->applyPendingEntries();
    this->update();
}

bool GiveawayWheel::spinning() const
{
    return this->spinning_;
}

void GiveawayWheel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    this->rebuildFace();
}

void GiveawayWheel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && this->showingResult_)
    {
        this->resultTimer_.stop();
        this->showingResult_ = false;
        this->applyPendingEntries();
        this->update();
        return;
    }
    QWidget::mousePressEvent(event);
}

void GiveawayWheel::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const auto rect = this->wheelRect();
    const auto center = rect.center();
    const auto radius = rect.width() / 2.0;

    painter.save();
    painter.translate(center);
    painter.rotate(this->rotation_);
    painter.drawPixmap(QPointF(-radius, -radius), this->face_);
    painter.restore();

    const bool showResult = this->showingResult_ && this->winner_ >= 0 &&
                            this->winner_ < static_cast<int>(this->entries_.size());
    if (showResult)
    {
        // Dim everything but the winning slice
        const auto winner = static_cast<size_t>(this->winner_);
        const auto start = this->sliceStarts_[winner];
        const auto end = winner + 1 < this->sliceStarts_.size()
                             ? this->sliceStarts_[winner + 1]
                             : 360.0;
        QPainterPath disc;
        disc.addEllipse(rect);
        QPainterPath slice;
        slice.moveTo(center);
        slice.arcTo(rect, 90.0 - (start + this->rotation_), -(end - start));
        slice.closeSubpath();
        painter.fillPath(disc.subtracted(slice),
                         withAlpha(this->colors_.background, 150));
        painter.setPen(QPen(MARKER_GOLD, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(slice);
    }

    // Rim and hub
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(this->colors_.border, 2));
    painter.drawEllipse(rect.adjusted(1, 1, -1, -1));
    const auto hubRadius = radius * 0.17;
    painter.setPen(QPen(this->colors_.border, 2));
    painter.setBrush(this->colors_.panel);
    painter.drawEllipse(center, hubRadius, hubRadius);
    painter.setBrush(MARKER_GOLD);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(center, hubRadius * 0.3, hubRadius * 0.3);

    // Pointer, kicked back a little whenever a slice boundary passes it
    painter.save();
    painter.translate(center.x(), rect.top() - 6);
    painter.rotate(this->pointerKick_);
    QPainterPath pointer;
    pointer.moveTo(-8, 0);
    pointer.lineTo(8, 0);
    pointer.lineTo(0, 17);
    pointer.closeSubpath();
    painter.setPen(QPen(mixColors(MARKER_GOLD, QColor(0, 0, 0), 0.45), 1));
    painter.setBrush(MARKER_GOLD);
    painter.drawPath(pointer);
    painter.restore();

    // Readout underneath: the slice under the pointer, the winner, or a count
    QString readout;
    QColor readoutColor = this->colors_.muted;
    auto font = this->font();
    if (this->entries_.empty())
    {
        readout = QStringLiteral("Waiting for entrants");
    }
    else if (this->spinning_ || showResult)
    {
        const auto slice = showResult ? this->winner_
                                      : this->sliceAt(-this->rotation_);
        if (slice >= 0)
        {
            readout = this->entries_[static_cast<size_t>(slice)].name;
        }
        readoutColor = showResult ? MARKER_GOLD : this->colors_.text;
        font.setBold(true);
    }
    else
    {
        readout = QStringLiteral("%1 on the wheel").arg(this->entries_.size());
    }
    painter.setFont(font);
    painter.setPen(readoutColor);
    const QRectF readoutRect(0, this->height() - WHEEL_READOUT_HEIGHT,
                             this->width(), WHEEL_READOUT_HEIGHT);
    painter.drawText(readoutRect, Qt::AlignCenter,
                     QFontMetricsF(font).elidedText(readout, Qt::ElideRight,
                                                    readoutRect.width() - 8));
}

// ---------------------------------------------------------------- emote tile

namespace {

constexpr double TILE_EMOTE_SIZE = 30;
constexpr double TILE_PITCH = 36;
// pixels per second the hidden reel scrolls at
constexpr double TILE_SPEED = 120;
constexpr int TILE_LANDING_SLOTS = 4;

// Three passes of a box blur come close to a gaussian and are cheap at
// emote sizes. Edges blur into transparency instead of being clamped hard.
QImage softBlur(const QImage &source, int radius)
{
    const int pad = radius * 3;
    QImage image(source.width() + 2 * pad, source.height() + 2 * pad,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.drawImage(pad, pad, source);
    }

    const int width = image.width();
    const int height = image.height();
    std::vector<QRgb> line(static_cast<size_t>(std::max(width, height)));
    const auto blurLine = [radius, &line](auto get, auto set, int length) {
        for (int i = 0; i < length; ++i)
        {
            line[static_cast<size_t>(i)] = get(i);
        }
        for (int i = 0; i < length; ++i)
        {
            int alpha = 0;
            int red = 0;
            int green = 0;
            int blue = 0;
            for (int k = -radius; k <= radius; ++k)
            {
                const auto index = i + k;
                if (index < 0 || index >= length)
                {
                    continue;  // transparent outside
                }
                const auto pixel = line[static_cast<size_t>(index)];
                alpha += qAlpha(pixel);
                red += qRed(pixel);
                green += qGreen(pixel);
                blue += qBlue(pixel);
            }
            const int count = 2 * radius + 1;
            set(i, qRgba(red / count, green / count, blue / count,
                         alpha / count));
        }
    };
    for (int pass = 0; pass < 3; ++pass)
    {
        for (int y = 0; y < height; ++y)
        {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            blurLine(
                [row](int x) {
                    return row[x];
                },
                [row](int x, QRgb pixel) {
                    row[x] = pixel;
                },
                width);
        }
        for (int x = 0; x < width; ++x)
        {
            blurLine(
                [&image, x](int y) {
                    return reinterpret_cast<const QRgb *>(
                        image.constScanLine(y))[x];
                },
                [&image, x](int y, QRgb pixel) {
                    reinterpret_cast<QRgb *>(image.scanLine(y))[x] = pixel;
                },
                height);
        }
    }
    return image;
}

}  // namespace

GiveawayEmoteTile::GiveawayEmoteTile(QWidget *parent)
    : QWidget(parent)
{
    this->setObjectName(QStringLiteral("GiveawayEmoteTile"));
    this->setFixedSize(this->sizeHint());
    this->clock_.start();
    this->frameTimer_.setInterval(33);
    QObject::connect(&this->frameTimer_, &QTimer::timeout, this, [this] {
        this->tick();
    });
}

QSize GiveawayEmoteTile::sizeHint() const
{
    return {44, 44};
}

void GiveawayEmoteTile::setColors(const GiveawayDrawColors &colors)
{
    this->colors_ = colors;
    this->update();
}

void GiveawayEmoteTile::setEmote(EmotePtr emote)
{
    if (emote == this->emote_)
    {
        return;
    }
    this->emote_ = std::move(emote);
    this->landing_ = false;
    this->updateTimer();
    this->update();
}

void GiveawayEmoteTile::setDecoys(std::vector<EmotePtr> decoys)
{
    this->decoys_ = std::move(decoys);
    this->blurCache_.clear();
    this->updateTimer();
    this->update();
}

bool GiveawayEmoteTile::revealed() const
{
    return this->revealed_;
}

bool GiveawayEmoteTile::landing() const
{
    return this->landing_;
}

bool GiveawayEmoteTile::spinning() const
{
    return !this->revealed_ && this->emote_ != nullptr &&
           !this->decoys_.empty();
}

void GiveawayEmoteTile::setRevealed(bool revealed, bool animated)
{
    if (revealed && this->landing_)
    {
        return;  // already on its way to the target
    }
    if (revealed && animated && this->spinning() && this->isVisible())
    {
        // Slow down over a few more slots and stop on the target, starting
        // at the cruising speed so there is no jolt
        this->landingSlot_ =
            static_cast<qint64>(std::ceil(this->scroll_ / TILE_PITCH)) +
            TILE_LANDING_SLOTS;
        this->landingFrom_ = this->scroll_;
        this->landingTo_ = static_cast<double>(this->landingSlot_) * TILE_PITCH;
        this->landingDuration_ = std::max<qint64>(
            1, static_cast<qint64>(2000.0 *
                                   (this->landingTo_ - this->landingFrom_) /
                                   TILE_SPEED));
        this->landingStart_ = this->clock_.elapsed();
        this->landing_ = true;
    }
    else
    {
        this->landing_ = false;
        this->revealed_ = revealed;
    }
    this->updateTimer();
    this->update();
}

void GiveawayEmoteTile::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    this->updateTimer();
}

void GiveawayEmoteTile::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    this->frameTimer_.stop();
}

void GiveawayEmoteTile::tick()
{
    const auto now = this->clock_.elapsed();
    const auto delta = static_cast<double>(now - this->lastTick_) / 1000.0;
    this->lastTick_ = now;
    if (this->landing_)
    {
        const auto progress = std::min(
            1.0, static_cast<double>(now - this->landingStart_) /
                     static_cast<double>(this->landingDuration_));
        const auto eased = 1.0 - (1.0 - progress) * (1.0 - progress);
        this->scroll_ = this->landingFrom_ +
                        (this->landingTo_ - this->landingFrom_) * eased;
        if (progress >= 1.0)
        {
            this->landing_ = false;
            this->revealed_ = true;
        }
    }
    else if (this->spinning())
    {
        // a long pause (hidden window) should not make the reel jump
        this->scroll_ += TILE_SPEED * std::min(delta, 0.1);
    }
    this->update();
    this->updateTimer();
}

void GiveawayEmoteTile::updateTimer()
{
    bool wanted = false;
    if (this->isVisible())
    {
        if (this->landing_ || this->spinning())
        {
            wanted = true;
        }
        else if (this->revealed_ && this->emote_ != nullptr)
        {
            const auto &image = this->emote_->images.getImage(3);
            wanted = image != nullptr && !image->isEmpty() &&
                     (!image->loaded() || image->animated());
        }
    }
    if (wanted && !this->frameTimer_.isActive())
    {
        this->lastTick_ = this->clock_.elapsed();
        this->frameTimer_.start();
    }
    else if (!wanted)
    {
        this->frameTimer_.stop();
    }
}

QPixmap GiveawayEmoteTile::blurred(const EmotePtr &emote)
{
    const auto &image = emote->images.getImageOrLoaded(2);
    if (image == nullptr || image->isEmpty())
    {
        return {};
    }
    if (const auto cached = this->blurCache_.constFind(image.get());
        cached != this->blurCache_.cend())
    {
        return cached.value();
    }
    const auto pixmap = image->pixmapOrLoad();
    if (!pixmap || pixmap->isNull())
    {
        return {};  // still loading, try again next frame
    }

    const auto ratio = this->devicePixelRatioF();
    const auto side = static_cast<int>(std::ceil(TILE_EMOTE_SIZE * ratio));
    const auto fitted = pixmap->toImage()
                            .scaled(side, side, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation)
                            .convertToFormat(
                                QImage::Format_ARGB32_Premultiplied);
    auto result = QPixmap::fromImage(
        softBlur(fitted, std::max(2, static_cast<int>(std::lround(2.5 * ratio)))));
    result.setDevicePixelRatio(ratio);
    this->blurCache_.insert(image.get(), result);
    return result;
}

void GiveawayEmoteTile::paintReel(QPainter &painter, const QRectF &frame)
{
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(frame.adjusted(1, 1, -1, -1), 4, 4);
    painter.setClipPath(clip);

    const auto center = frame.center();
    const auto count = static_cast<qint64>(this->decoys_.size());
    const auto first = static_cast<qint64>(
        std::floor((this->scroll_ - frame.height()) / TILE_PITCH));
    const auto last = static_cast<qint64>(
        std::ceil((this->scroll_ + frame.height()) / TILE_PITCH));
    const auto landingProgress =
        this->landing_
            ? std::min(1.0, static_cast<double>(this->clock_.elapsed() -
                                                this->landingStart_) /
                                static_cast<double>(this->landingDuration_))
            : 0.0;

    for (auto slot = first; slot <= last; ++slot)
    {
        const auto y = center.y() + static_cast<double>(slot) * TILE_PITCH -
                       this->scroll_;
        const QRectF box(center.x() - TILE_EMOTE_SIZE / 2,
                         y - TILE_EMOTE_SIZE / 2, TILE_EMOTE_SIZE,
                         TILE_EMOTE_SIZE);
        const bool isTarget = this->landing_ && slot == this->landingSlot_;
        const auto &emote =
            isTarget ? this->emote_
                     : this->decoys_[static_cast<size_t>(
                           ((slot % count) + count) % count)];

        auto soft = this->blurred(emote);
        // the target sharpens as the reel comes to rest
        const auto sharpness =
            isTarget ? std::clamp((landingProgress - 0.55) / 0.45, 0.0, 1.0)
                     : 0.0;
        if (soft.isNull())
        {
            painter.setOpacity(1.0 - sharpness);
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(this->colors_.muted, 60));
            painter.drawRoundedRect(box.adjusted(3, 3, -3, -3), 6, 6);
        }
        else
        {
            const auto size = QSizeF(soft.size()) / soft.devicePixelRatio();
            painter.setOpacity(1.0 - sharpness);
            painter.drawPixmap(
                QRectF(box.center().x() - size.width() / 2,
                       box.center().y() - size.height() / 2, size.width(),
                       size.height()),
                soft, QRectF(soft.rect()));
        }
        if (sharpness > 0)
        {
            const auto &image = emote->images.getImageOrLoaded(3);
            if (auto pixmap = image != nullptr ? image->pixmapOrLoad()
                                               : std::nullopt;
                pixmap && !pixmap->isNull())
            {
                painter.setOpacity(sharpness);
                const auto size =
                    QSizeF(pixmap->size()).scaled(box.size(), Qt::KeepAspectRatio);
                painter.drawPixmap(
                    QRectF(box.center().x() - size.width() / 2,
                           box.center().y() - size.height() / 2, size.width(),
                           size.height()),
                    *pixmap, QRectF(pixmap->rect()));
            }
        }
    }
    painter.setOpacity(1.0);

    // Fade the reel out at the top and bottom edges
    const double fade = 12;
    QLinearGradient top(0, frame.top(), 0, frame.top() + fade);
    top.setColorAt(0, this->colors_.background);
    top.setColorAt(1, withAlpha(this->colors_.background, 0));
    painter.fillRect(QRectF(frame.left(), frame.top(), frame.width(), fade),
                     top);
    QLinearGradient bottom(0, frame.bottom(), 0, frame.bottom() - fade);
    bottom.setColorAt(0, this->colors_.background);
    bottom.setColorAt(1, withAlpha(this->colors_.background, 0));
    painter.fillRect(
        QRectF(frame.left(), frame.bottom() - fade, frame.width(), fade),
        bottom);
    painter.restore();
}

void GiveawayEmoteTile::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF frame = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(this->colors_.border, 1));
    painter.setBrush(this->colors_.background);
    painter.drawRoundedRect(frame, 5, 5);

    if (this->landing_ || this->spinning())
    {
        this->paintReel(painter, frame);
        return;
    }

    if (this->emote_ != nullptr && this->revealed_)
    {
        const auto &image = this->emote_->images.getImageOrLoaded(3);
        if (image != nullptr)
        {
            if (auto pixmap = image->pixmapOrLoad(); pixmap && !pixmap->isNull())
            {
                const auto center = frame.center();
                const auto size = QSizeF(pixmap->size())
                                      .scaled(QSizeF(TILE_EMOTE_SIZE,
                                                     TILE_EMOTE_SIZE),
                                              Qt::KeepAspectRatio);
                const QRectF target(center.x() - size.width() / 2,
                                    center.y() - size.height() / 2,
                                    size.width(), size.height());
                painter.drawPixmap(target, *pixmap, QRectF(pixmap->rect()));
                return;
            }
        }
    }

    auto font = this->font();
    font.setBold(true);
    font.setPixelSize(20);
    painter.setFont(font);
    painter.setPen(this->colors_.muted);
    painter.drawText(frame, Qt::AlignCenter,
                     this->emote_ != nullptr ? QStringLiteral("?")
                                             : QStringLiteral("–"));
}

// ---------------------------------------------------------------- secret text

namespace {

constexpr int SECRET_LENGTH = 8;
const QString SECRET_GLYPHS = QStringLiteral(
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789");

QChar randomSecretGlyph()
{
    return SECRET_GLYPHS.at(static_cast<qsizetype>(
        QRandomGenerator::global()->bounded(
            static_cast<quint32>(SECRET_GLYPHS.size()))));
}

}  // namespace

GiveawaySecretLineEdit::GiveawaySecretLineEdit(QWidget *parent)
    : QLineEdit(parent)
{
    for (int i = 0; i < SECRET_LENGTH; ++i)
    {
        this->glyphs_.append(randomSecretGlyph());
    }
    this->scrambleTimer_.setInterval(80);
    QObject::connect(&this->scrambleTimer_, &QTimer::timeout, this, [this] {
        // re-roll a couple of characters at a time so it shimmers
        for (int i = 0; i < 2; ++i)
        {
            const auto index = static_cast<qsizetype>(
                QRandomGenerator::global()->bounded(SECRET_LENGTH));
            this->glyphs_[index] = randomSecretGlyph();
        }
        this->update();
    });
    QObject::connect(this, &QLineEdit::textChanged, this, [this] {
        this->updateTimer();
    });
}

void GiveawaySecretLineEdit::setSecret(bool secret)
{
    this->secret_ = secret;
    this->setEchoMode(secret ? QLineEdit::NoEcho : QLineEdit::Normal);
    this->updateTimer();
    this->update();
}

bool GiveawaySecretLineEdit::secret() const
{
    return this->secret_;
}

void GiveawaySecretLineEdit::showEvent(QShowEvent *event)
{
    QLineEdit::showEvent(event);
    this->updateTimer();
}

void GiveawaySecretLineEdit::hideEvent(QHideEvent *event)
{
    QLineEdit::hideEvent(event);
    this->scrambleTimer_.stop();
}

void GiveawaySecretLineEdit::updateTimer()
{
    const bool wanted =
        this->secret_ && this->isVisible() && !this->text().isEmpty();
    if (wanted && !this->scrambleTimer_.isActive())
    {
        this->scrambleTimer_.start();
    }
    else if (!wanted)
    {
        this->scrambleTimer_.stop();
    }
}

void GiveawaySecretLineEdit::paintEvent(QPaintEvent *event)
{
    QLineEdit::paintEvent(event);
    if (!this->secret_ || this->text().isEmpty())
    {
        return;
    }

    QStyleOptionFrame option;
    this->initStyleOption(&option);
    const auto area =
        this->style()
            ->subElementRect(QStyle::SE_LineEditContents, &option, this)
            .marginsRemoved(this->textMargins());

    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(this->font());
    const QFontMetricsF metrics(this->font());
    const auto advance = metrics.horizontalAdvance(QLatin1Char('0')) + 1.5;
    auto color = this->palette().color(QPalette::PlaceholderText);
    for (int i = 0; i < this->glyphs_.size(); ++i)
    {
        // a slight, stable variation in strength per character
        color.setAlpha(150 + (this->glyphs_[i].unicode() * 37 + i * 53) % 90);
        painter.setPen(color);
        const QRectF cell(area.left() + 2 + i * advance, area.top(), advance,
                          area.height());
        painter.drawText(cell, Qt::AlignCenter, QString(this->glyphs_[i]));
    }
}

}  // namespace chatterino
