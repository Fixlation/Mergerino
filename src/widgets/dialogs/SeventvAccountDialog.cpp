// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SeventvAccountDialog.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "providers/seventv/SeventvAccountManager.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QEasingCurve>
#include <QFormLayout>
#include <QFontMetricsF>
#include <QFrame>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>

namespace {

using namespace chatterino;

constexpr int CosmeticDataRole = Qt::UserRole + 1;
constexpr int CosmeticKindRole = Qt::UserRole + 2;
constexpr int EmoteDataRole = Qt::UserRole + 3;
constexpr int CosmeticRowHeight = 39;
constexpr int CosmeticVisibleRows = 8;
constexpr int CosmeticSearchHeight = 39;
constexpr int EditorChannelVisibleRows = 12;
const QString MixedCosmeticValue = QStringLiteral("__MERGERINO_MIXED__");

QString nextCosmeticPlatform(const QString &platform)
{
    if (platform == SeventvAccountManager::twitchPlatform())
    {
        return SeventvAccountManager::kickPlatform();
    }
    if (platform == SeventvAccountManager::kickPlatform())
    {
        return SeventvAccountManager::bothPlatforms();
    }
    return SeventvAccountManager::twitchPlatform();
}

QString cosmeticPlatformLabel(const QString &platform)
{
    if (platform == SeventvAccountManager::kickPlatform())
    {
        return QStringLiteral("Kick");
    }
    if (platform == SeventvAccountManager::bothPlatforms())
    {
        return QStringLiteral("Both");
    }
    return QStringLiteral("Twitch");
}

QString normalizedImageUrl(QString url)
{
    if (url.startsWith(QStringLiteral("//")))
    {
        url.prepend(QStringLiteral("https:"));
    }
    return url;
}

QString cosmeticDisplayName(const SeventvOwnedCosmetic &cosmetic)
{
    return cosmetic.name.isEmpty() ? cosmetic.id : cosmetic.name;
}

bool isSubscriberBadge(const SeventvOwnedCosmetic &cosmetic)
{
    const auto name = cosmeticDisplayName(cosmetic).toLower();
    return name.contains(QStringLiteral("subscriber")) ||
           name.startsWith(QStringLiteral("sub ")) ||
           name.contains(QStringLiteral(" sub ")) ||
           name.endsWith(QStringLiteral(" sub"));
}

int subscriberBadgeMilestone(const SeventvOwnedCosmetic &cosmetic)
{
    const auto name = cosmeticDisplayName(cosmetic).toLower();
    if (name.contains(QStringLiteral("founder")))
    {
        return -1;
    }

    static const QRegularExpression durationPattern(
        QStringLiteral(R"((\d+)\s*(year|month))"));
    int months = 0;
    bool foundDuration = false;
    auto matches = durationPattern.globalMatch(name);
    while (matches.hasNext())
    {
        const auto match = matches.next();
        const auto amount = match.captured(1).toInt();
        months += match.captured(2) == QStringLiteral("year") ? amount * 12
                                                               : amount;
        foundDuration = true;
    }
    return foundDuration ? months : std::numeric_limits<int>::max();
}

std::vector<SeventvOwnedCosmetic> sortedCosmetics(
    const std::vector<SeventvOwnedCosmetic> &items, const QString &kind)
{
    auto sorted = items;
    std::ranges::stable_sort(
        sorted, [&kind](const auto &left, const auto &right) {
            if (kind == QStringLiteral("BADGE"))
            {
                const bool leftSubscriber = isSubscriberBadge(left);
                const bool rightSubscriber = isSubscriberBadge(right);
                if (leftSubscriber != rightSubscriber)
                {
                    return leftSubscriber;
                }
                if (leftSubscriber)
                {
                    const int leftMilestone = subscriberBadgeMilestone(left);
                    const int rightMilestone = subscriberBadgeMilestone(right);
                    if (leftMilestone != rightMilestone)
                    {
                        return leftMilestone < rightMilestone;
                    }
                }
            }
            return QString::localeAwareCompare(cosmeticDisplayName(left),
                                               cosmeticDisplayName(right)) < 0;
        });
    return sorted;
}

ImagePtr badgeImageFromCosmetic(const QJsonObject &cosmetic)
{
    const bool animate = getSettings()->animateSevenTVBadges;
    const auto images = cosmetic.value(QStringLiteral("images")).toArray();
    QJsonObject bestImage;
    int bestScore = std::numeric_limits<int>::max();
    for (const auto &value : images)
    {
        const auto image = value.toObject();
        const auto url = image.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
        {
            continue;
        }

        const auto mime =
            image.value(QStringLiteral("mime")).toString().toLower();
        const auto scale =
            std::max(1, image.value(QStringLiteral("scale")).toInt(1));
        const auto animated =
            image.value(QStringLiteral("frameCount")).toInt(1) > 1;
        const int animationPenalty = animated == animate ? 0 : 1000;
        const int formatPenalty =
            mime.contains(QStringLiteral("webp"))
                ? 0
                : (mime.contains(QStringLiteral("gif")) ? 20 : 40);
        const int score =
            animationPenalty + formatPenalty + (scale - 1) * 100;
        if (score < bestScore)
        {
            bestScore = score;
            bestImage = image;
        }
    }
    if (!bestImage.isEmpty())
    {
        const auto url = normalizedImageUrl(
            bestImage.value(QStringLiteral("url")).toString());
        const auto expectedSize =
            QSize(bestImage.value(QStringLiteral("width")).toInt(18),
                  bestImage.value(QStringLiteral("height")).toInt(18));
        return Image::fromUrl({url}, 1.0, expectedSize);
    }

    const auto host = cosmetic.value(QStringLiteral("host")).toObject();
    const auto files = host.value(QStringLiteral("files")).toArray();
    QJsonObject bestFile;
    int bestWidth = std::numeric_limits<int>::max();
    for (const auto &value : files)
    {
        const auto file = value.toObject();
        if (file.value(QStringLiteral("format")).toString().toUpper() !=
            QStringLiteral("WEBP"))
        {
            continue;
        }
        const auto width = file.value(QStringLiteral("width")).toInt();
        if (width > 0 && width < bestWidth)
        {
            bestWidth = width;
            bestFile = file;
        }
    }
    if (bestFile.isEmpty() && !files.isEmpty())
    {
        bestFile = files.first().toObject();
    }
    if (bestFile.isEmpty())
    {
        return nullptr;
    }

    const auto staticName =
        bestFile.value(QStringLiteral("static_name")).toString();
    const auto fileName = !animate && !staticName.isEmpty()
                              ? staticName
                              : bestFile.value(QStringLiteral("name"))
                                    .toString();
    auto baseUrl =
        normalizedImageUrl(host.value(QStringLiteral("url")).toString());
    if (!baseUrl.endsWith('/'))
    {
        baseUrl.append('/');
    }
    return Image::fromUrl(
        {baseUrl + fileName}, 1.0,
        QSize(bestFile.value(QStringLiteral("width")).toInt(18),
              bestFile.value(QStringLiteral("height")).toInt(18)));
}

class CosmeticListView final : public QListView
{
public:
    explicit CosmeticListView(QWidget *parent = nullptr)
        : QListView(parent)
        , search_(new QLineEdit(this))
    {
        this->search_->setPlaceholderText(QStringLiteral("Search cosmetics"));
        this->search_->setClearButtonEnabled(true);
        this->search_->setFixedHeight(29);
        this->search_->setStyleSheet(QStringLiteral(
            "QLineEdit { background: palette(base); border: 1px solid "
            "palette(mid); border-radius: 5px; color: palette(text); "
            "padding: 3px 8px; selection-background-color: palette(highlight); }"));
        this->setViewportMargins(0, CosmeticSearchHeight, 0, 0);
        QObject::connect(this->search_, &QLineEdit::textChanged, this,
                         [this](const QString &text) {
                             this->filter(text);
                         });
    }

    void resetSearch()
    {
        this->search_->clear();
        this->filter({});
    }

    void focusSearch()
    {
        this->search_->setFocus(Qt::PopupFocusReason);
    }

    QLineEdit *searchEdit() const
    {
        return this->search_;
    }

    int visibleRowCount() const
    {
        if (this->model() == nullptr)
        {
            return 0;
        }

        int count = 0;
        for (int row = 0; row < this->model()->rowCount(); ++row)
        {
            count += !this->isRowHidden(row);
        }
        return count;
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QListView::resizeEvent(event);
        this->search_->setGeometry(
            6, 5, std::max(20, this->viewport()->width() - 12), 29);
        this->search_->raise();
    }

private:
    void filter(const QString &text)
    {
        if (this->model() == nullptr)
        {
            return;
        }
        const auto needle = text.trimmed();
        for (int row = 0; row < this->model()->rowCount(); ++row)
        {
            const auto label = this->model()
                                   ->index(row, 0, this->rootIndex())
                                   .data(Qt::DisplayRole)
                                   .toString();
            this->setRowHidden(
                row, !needle.isEmpty() &&
                         !label.contains(needle, Qt::CaseInsensitive));
        }
    }

    QLineEdit *search_;
};

class CosmeticItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(CosmeticRowHeight);
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const bool popupRow = qobject_cast<const QListView *>(option.widget);
        if (popupRow)
        {
            auto rowRect = option.rect.adjusted(3, 2, -3, -2);
            QColor rowColor = option.palette.color(QPalette::AlternateBase);
            if (option.state & QStyle::State_Selected)
            {
                rowColor = option.palette.color(QPalette::Highlight);
            }
            else if (option.state & QStyle::State_MouseOver)
            {
                rowColor = option.palette.color(QPalette::Button);
            }
            painter->setPen(Qt::NoPen);
            painter->setBrush(rowColor);
            painter->drawRoundedRect(rowRect, 5, 5);
        }

        auto content = option.rect.adjusted(popupRow ? 10 : 7, 0,
                                            popupRow ? -10 : -7, 0);
        if (option.state & QStyle::State_Selected)
        {
            content.adjust(0, 0, -24, 0);
        }

        const auto kind = index.data(CosmeticKindRole).toString();
        const bool hasCosmetic =
            !index.data(CosmeticDataRole).toByteArray().isEmpty();
        if (hasCosmetic && (kind == QStringLiteral("PAINT") ||
                            kind == QStringLiteral("BADGE")))
        {
            content.translate(-2, 0);
        }
        auto font = option.font;
        font.setWeight(QFont::DemiBold);
        const QFontMetricsF metrics(font);
        const auto label = index.data(Qt::DisplayRole).toString();

        if (kind == QStringLiteral("BADGE") && hasCosmetic)
        {
            const auto id = index.data(Qt::UserRole).toString();
            auto image = this->badgeCache_.value(id);
            if (image == nullptr)
            {
                const auto document = QJsonDocument::fromJson(
                    index.data(CosmeticDataRole).toByteArray());
                image = badgeImageFromCosmetic(document.object());
                this->badgeCache_.insert(id, image);
            }

            constexpr int BadgeSize = 20;
            const QRect badgeSlot(content.left(),
                                  content.center().y() - BadgeSize / 2,
                                  BadgeSize, BadgeSize);
            if (image != nullptr)
            {
                if (const auto pixmap = image->pixmapOrLoad())
                {
                    auto displaySize = pixmap->size();
                    displaySize.scale(BadgeSize, BadgeSize,
                                      Qt::KeepAspectRatio);
                    const QRect target(
                        badgeSlot.center().x() - displaySize.width() / 2,
                        badgeSlot.center().y() - displaySize.height() / 2,
                        displaySize.width(), displaySize.height());
                    painter->drawPixmap(target, *pixmap, pixmap->rect());
                }
            }
            content.setLeft(badgeSlot.right() + 8);
        }

        auto textRect = QRectF(content);
        if (hasCosmetic && kind == QStringLiteral("PAINT"))
        {
            textRect.translate(0.0, 0.5);
        }
        else if (hasCosmetic && kind == QStringLiteral("BADGE"))
        {
            textRect.translate(0.0, -0.5);
        }
        const auto text = metrics.elidedText(
            label, Qt::ElideRight, std::max(1, content.width()));

        bool drewPaint = false;
        if (kind == QStringLiteral("PAINT") && hasCosmetic)
        {
            const auto id = index.data(Qt::UserRole).toString();
            auto paint = this->paintCache_.value(id);
            if (paint == nullptr)
            {
                const auto document = QJsonDocument::fromJson(
                    index.data(CosmeticDataRole).toByteArray());
                paint = makeSeventvPaintFromGraphQL(document.object());
                this->paintCache_.insert(id, paint);
            }
            if (paint != nullptr)
            {
                const QSizeF textSize(metrics.horizontalAdvance(text) + 4,
                                      metrics.height() + 4);
                const auto dpr = option.widget == nullptr
                                     ? 1.0F
                                     : option.widget->devicePixelRatioF();
                const auto padding = paint->pixmapPadding(1.0F, dpr);
                const auto pixmap = paint->getPixmap(
                    text, font, option.palette.color(QPalette::Text), textSize,
                    1.0F, dpr);
                const QPointF position(
                    content.left() - padding.left(),
                    content.center().y() - (textSize.height() / 2.0) -
                        padding.top() + 0.5);
                auto paintClip = content;
                paintClip.setLeft(option.rect.left() + 3);
                painter->setClipRect(paintClip);
                painter->drawPixmap(position, pixmap);
                painter->setClipping(false);
                drewPaint = true;
            }
        }

        if (!drewPaint)
        {
            painter->setFont(font);
            const auto isEmptyChoice = index.data(Qt::UserRole).toString().isEmpty();
            painter->setPen(isEmptyChoice
                                ? option.palette.color(QPalette::PlaceholderText)
                                : option.palette.color(QPalette::Text));
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
        }

        if (option.state & QStyle::State_Selected)
        {
            auto markerRect = option.rect.adjusted(0, 0, -10, 0);
            painter->setFont(option.font);
            painter->setPen(option.palette.color(QPalette::Text));
            painter->drawText(markerRect, Qt::AlignRight | Qt::AlignVCenter,
                              QStringLiteral("✓"));
        }
        painter->restore();
    }

private:
    mutable QHash<QString, std::shared_ptr<Paint>> paintCache_;
    mutable QHash<QString, ImagePtr> badgeCache_;
};

class CosmeticPlatformCycleButton final : public QPushButton
{
public:
    using ChangedCallback =
        std::function<void(const QString &, const QString &)>;

    explicit CosmeticPlatformCycleButton(QWidget *parent = nullptr);
    void setPlatform(const QString &platform);
    void setChangedCallback(ChangedCallback callback);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void renderIcon(QPainter &painter, const QIcon &icon,
                    const QRectF &bounds, qreal opacity);
    void renderPlus(QPainter &painter, const QPointF &center, qreal opacity);
    void renderPlatform(QPainter &painter, const QString &platform,
                        const QRectF &bounds, qreal opacity, qreal spread);
    void updateTooltip();

    QIcon twitchIcon_;
    QIcon kickIcon_;
    QVariantAnimation animation_;
    QString platform_ = SeventvAccountManager::twitchPlatform();
    QString previousPlatform_ = SeventvAccountManager::twitchPlatform();
    qreal animationProgress_ = 1.0;
    ChangedCallback changedCallback_;
};

CosmeticPlatformCycleButton::CosmeticPlatformCycleButton(QWidget *parent)
    : QPushButton(parent)
    , twitchIcon_(QStringLiteral(":/platforms/twitch.svg"))
    , kickIcon_(QStringLiteral(":/platforms/kick.svg"))
    , animation_(this)
{
    this->setObjectName(QStringLiteral("CosmeticPlatformCycleButton"));
    this->setCursor(Qt::PointingHandCursor);
    this->setFixedSize(68, 29);
    this->setFocusPolicy(Qt::NoFocus);
    this->animation_.setDuration(170);
    this->animation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&this->animation_, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         this->animationProgress_ = value.toReal();
                         this->update();
                     });
    QObject::connect(&this->animation_, &QVariantAnimation::finished, this,
                     [this] {
                         this->animationProgress_ = 1.0;
                         this->update();
                         if (this->changedCallback_)
                         {
                             this->changedCallback_(this->previousPlatform_,
                                                    this->platform_);
                         }
                     });
    QObject::connect(this, &QPushButton::clicked, this, [this] {
        if (this->animation_.state() == QAbstractAnimation::Running)
        {
            return;
        }
        this->previousPlatform_ = this->platform_;
        this->platform_ = nextCosmeticPlatform(this->platform_);
        this->updateTooltip();
        this->animationProgress_ = 0.0;
        this->animation_.setStartValue(0.0);
        this->animation_.setEndValue(1.0);
        this->animation_.start();
    });
    this->updateTooltip();
}

void CosmeticPlatformCycleButton::setPlatform(const QString &platform)
{
    this->animation_.stop();
    this->platform_ = platform;
    this->previousPlatform_ = platform;
    this->animationProgress_ = 1.0;
    this->updateTooltip();
    this->update();
}

void CosmeticPlatformCycleButton::setChangedCallback(ChangedCallback callback)
{
    this->changedCallback_ = std::move(callback);
}

void CosmeticPlatformCycleButton::paintEvent(QPaintEvent *)
{
    QStylePainter painter(this);
    QStyleOptionButton option;
    this->initStyleOption(&option);
    option.state &= ~(QStyle::State_HasFocus | QStyle::State_On |
                      QStyle::State_Selected);
    option.text.clear();
    option.icon = {};
    painter.drawControl(QStyle::CE_PushButton, option);

    const QSizeF iconSize(17, 17);
    const QRectF bounds(
        QPointF((this->width() - iconSize.width()) / 2.0,
                (this->height() - iconSize.height()) / 2.0),
        iconSize);
    painter.save();
    painter.setClipRect(this->rect());
    if (this->animationProgress_ >= 1.0)
    {
        this->renderPlatform(painter, this->platform_, bounds, 1.0, 1.0);
    }
    else if (this->previousPlatform_ !=
                 SeventvAccountManager::bothPlatforms() &&
             this->platform_ != SeventvAccountManager::bothPlatforms())
    {
        const auto slide = bounds.width() * this->animationProgress_;
        this->renderPlatform(
            painter, this->previousPlatform_, bounds.translated(slide, 0),
            1.0 - this->animationProgress_, 1.0);
        this->renderPlatform(
            painter, this->platform_,
            bounds.translated(slide - bounds.width(), 0),
            this->animationProgress_, 1.0);
    }
    else
    {
        this->renderPlatform(painter, this->previousPlatform_, bounds,
                             1.0 - this->animationProgress_,
                             1.0 - this->animationProgress_);
        this->renderPlatform(painter, this->platform_, bounds,
                             this->animationProgress_,
                             this->animationProgress_);
    }
    painter.restore();
}

void CosmeticPlatformCycleButton::renderIcon(
    QPainter &painter, const QIcon &icon, const QRectF &bounds, qreal opacity)
{
    if (opacity <= 0.0)
    {
        return;
    }
    const auto pixmap =
        icon.pixmap(bounds.size().toSize(), QIcon::Normal, QIcon::Off);
    painter.save();
    painter.setOpacity(opacity);
    painter.drawPixmap(bounds, pixmap, QRectF(pixmap.rect()));
    painter.restore();
}

void CosmeticPlatformCycleButton::renderPlus(
    QPainter &painter, const QPointF &center, qreal opacity)
{
    painter.save();
    painter.setOpacity(opacity);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(this->palette().color(QPalette::ButtonText), 1.2,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(center.x() - 2.1, center.y()),
                     QPointF(center.x() + 2.1, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - 2.1),
                     QPointF(center.x(), center.y() + 2.1));
    painter.restore();
}

void CosmeticPlatformCycleButton::renderPlatform(
    QPainter &painter, const QString &platform, const QRectF &bounds,
    qreal opacity, qreal spread)
{
    if (platform != SeventvAccountManager::bothPlatforms())
    {
        this->renderIcon(
            painter,
            platform == SeventvAccountManager::kickPlatform()
                ? this->kickIcon_
                : this->twitchIcon_,
            bounds, opacity);
        return;
    }

    const auto center = bounds.center();
    const qreal distance = 15.0 * spread;
    this->renderIcon(painter, this->twitchIcon_,
                     bounds.translated(-distance, 0), opacity);
    this->renderPlus(painter, center, opacity * spread);
    this->renderIcon(painter, this->kickIcon_,
                     bounds.translated(distance, 0), opacity);
}

void CosmeticPlatformCycleButton::updateTooltip()
{
    const auto next = nextCosmeticPlatform(this->platform_);
    this->setToolTip(
        QStringLiteral("%1 preset. Click to switch to %2.")
            .arg(cosmeticPlatformLabel(this->platform_),
                 cosmeticPlatformLabel(next)));
    this->setAccessibleName(
        QStringLiteral("%1 platform preset")
            .arg(cosmeticPlatformLabel(this->platform_)));
}

ImagePtr emoteImageFromData(const QJsonObject &data)
{
    auto images =
        SeventvEmotes::createImageSet(data, !getSettings()->animateEmotes);
    auto image = images.getImage2();
    if (image == nullptr || image->isEmpty())
    {
        image = images.getImage1();
    }
    return image;
}

class EmoteItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(38);
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.column() != 0 ||
            index.data(EmoteDataRole).toByteArray().isEmpty())
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        auto background = option;
        this->initStyleOption(&background, index);
        const auto label = background.text;
        background.text.clear();
        const auto *style = option.widget == nullptr
                                ? QApplication::style()
                                : option.widget->style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter,
                           option.widget);

        const auto id = index.data(Qt::UserRole).toString();
        auto image = this->imageCache_.value(id);
        if (image == nullptr)
        {
            const auto document = QJsonDocument::fromJson(
                index.data(EmoteDataRole).toByteArray());
            image = emoteImageFromData(document.object());
            this->imageCache_.insert(id, image);
        }

        constexpr int ImageSize = 28;
        const QRect imageSlot(option.rect.left() + 6,
                              option.rect.center().y() - ImageSize / 2,
                              ImageSize, ImageSize);
        if (image != nullptr)
        {
            if (const auto pixmap = image->pixmapOrLoad())
            {
                auto displaySize = pixmap->size();
                displaySize.scale(ImageSize, ImageSize, Qt::KeepAspectRatio);
                const QRect target(
                    imageSlot.center().x() - displaySize.width() / 2,
                    imageSlot.center().y() - displaySize.height() / 2,
                    displaySize.width(), displaySize.height());
                painter->drawPixmap(target, *pixmap, pixmap->rect());
            }
        }

        painter->save();
        painter->setPen(option.palette.color(QPalette::Text));
        const auto textRect =
            option.rect.adjusted(ImageSize + 13, 0, -6, 0);
        painter->drawText(
            textRect, Qt::AlignLeft | Qt::AlignVCenter,
            option.fontMetrics.elidedText(label, Qt::ElideRight,
                                          textRect.width()));
        painter->restore();
    }

private:
    mutable QHash<QString, ImagePtr> imageCache_;
};

QWidget *centeredTableCell(QWidget *content, QTableWidget *table,
                           int left = 4, int top = 4, int right = 4,
                           int bottom = 4)
{
    auto *container = new QWidget(table);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(left, top, right, bottom);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(content);
    return container;
}

void configureTableActionButton(QPushButton *button)
{
    button->setObjectName(QStringLiteral("SeventvTableActionButton"));
    button->setFixedSize(88, 30);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

class CosmeticComboBoxStyle final : public QProxyStyle
{
public:
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override
    {
        if (hint == QStyle::SH_ComboBox_Popup)
        {
            return 0;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

class CosmeticComboBox final : public QComboBox
{
public:
    explicit CosmeticComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        auto *popupStyle = new CosmeticComboBoxStyle;
        popupStyle->setParent(this);
        this->setStyle(popupStyle);

        auto *list = new CosmeticListView(this);
        this->list_ = list;
        list->setItemDelegate(new CosmeticItemDelegate(list));
        list->setUniformItemSizes(true);
        list->setSpacing(1);
        list->setAutoScroll(false);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setTextElideMode(Qt::ElideRight);
        list->setStyleSheet(QStringLiteral(
            "QListView { background: palette(base); border: 1px solid palette(mid); "
            "border-radius: 6px; padding: 0; outline: 0; }"
            "QListView::item { background: transparent; padding: 0; }"
            "QScrollBar:vertical { background: palette(base); width: 10px; "
            "margin: 3px 1px 3px 0; }"
            "QScrollBar::handle:vertical { background: palette(mid); "
            "border-radius: 4px; min-height: 30px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { "
            "height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { "
            "background: transparent; }"));
        this->setView(list);
        this->setItemDelegate(new CosmeticItemDelegate(this));
        this->setMaxVisibleItems(CosmeticVisibleRows);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->setFixedHeight(29);

        QObject::connect(list->searchEdit(), &QLineEdit::textChanged, this,
                         [this] {
                             QTimer::singleShot(0, this, [this] {
                                 this->placePopupBelow();
                             });
                         });

        this->previewTimer_.setInterval(50);
        QObject::connect(&this->previewTimer_, &QTimer::timeout, this, [this] {
            const auto popupOpen =
                this->view() != nullptr && this->view()->isVisible();
            auto *topLevel = this->window();
            if (!this->isVisible() ||
                (!popupOpen &&
                 (topLevel == nullptr || !topLevel->isActiveWindow())))
            {
                return;
            }

            this->update();
            if (auto *viewport = this->view()->viewport())
            {
                viewport->update();
            }
        });
        this->previewTimer_.start();
    }

    void applyPopupTheme()
    {
        const auto themedPalette = this->palette();
        this->list_->setPalette(themedPalette);
        this->list_->viewport()->setPalette(themedPalette);
        this->list_->searchEdit()->setPalette(themedPalette);
        auto *popup = this->list_->window();
        if (popup != nullptr && popup != this->window())
        {
            popup->setPalette(themedPalette);
        }
        this->list_->viewport()->update();
        this->list_->searchEdit()->update();
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }

    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox comboOption;
        this->initStyleOption(&comboOption);
        comboOption.currentText.clear();
        comboOption.currentIcon = {};
        painter.drawComplexControl(QStyle::CC_ComboBox, comboOption);

        if (this->currentIndex() < 0)
        {
            return;
        }

        auto itemOption = QStyleOptionViewItem{};
        itemOption.initFrom(this);
        itemOption.rect = this->style()
                              ->subControlRect(QStyle::CC_ComboBox,
                                               &comboOption,
                                               QStyle::SC_ComboBoxEditField,
                                               this)
                              .adjusted(2, 0, -2, 0);
        itemOption.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        this->itemDelegate()->paint(
            &painter, itemOption,
            this->model()->index(this->currentIndex(), this->modelColumn(),
                                 this->rootModelIndex()));
    }

    void showPopup() override
    {
        this->list_->resetSearch();
        this->applyPopupTheme();
        QComboBox::showPopup();
        this->applyPopupTheme();
        this->placePopupBelow();
        QTimer::singleShot(0, this, [this] {
            this->applyPopupTheme();
            this->placePopupBelow();
            this->list_->focusSearch();
        });
    }

    void hidePopup() override
    {
        QComboBox::hidePopup();
        this->update();
    }

private:
    void placePopupBelow()
    {
        if (this->list_ == nullptr || !this->list_->isVisible())
        {
            return;
        }

        auto *popup = this->list_->window();
        if (popup == nullptr || popup == this->window())
        {
            return;
        }

        const auto anchor = this->mapToGlobal(QPoint(0, this->height()));
        auto *screen = QGuiApplication::screenAt(anchor);
        if (screen == nullptr)
        {
            screen = this->screen();
        }
        if (screen == nullptr)
        {
            return;
        }

        const auto available = screen->availableGeometry();
        const int rows = std::clamp(this->list_->visibleRowCount(), 1,
                                    CosmeticVisibleRows);
        const int popupChrome =
            std::max(0, popup->height() - this->list_->height());
        const int desiredHeight = CosmeticSearchHeight +
                                  rows * CosmeticRowHeight + popupChrome + 2;
        const int availableBelow =
            std::max(1, available.bottom() - anchor.y() + 1);
        const int popupHeight = std::min(desiredHeight, availableBelow);
        const int popupWidth = std::min(this->width(), available.width());
        const int popupX = std::clamp(anchor.x(), available.left(),
                                      available.right() - popupWidth + 1);

        popup->setGeometry(popupX, anchor.y(), popupWidth, popupHeight);
    }

    QTimer previewTimer_;
    CosmeticListView *list_ = nullptr;
};

class EditorChannelComboBox final : public QComboBox
{
public:
    explicit EditorChannelComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        auto *list = new QListView(this);
        this->list_ = list;
        list->setUniformItemSizes(true);
        list->setAutoScroll(false);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setTextElideMode(Qt::ElideRight);
        list->setStyleSheet(QStringLiteral(
            "QListView { background:palette(base); border:1px solid palette(mid); "
            "padding:0; outline:0; }"
            "QListView::item { min-height: 22px; padding: 1px 6px; }"
            "QScrollBar:vertical { background:palette(base); width:10px; }"
            "QScrollBar::handle:vertical { background:palette(mid); }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { "
            "height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { "
            "background: transparent; }"));
        this->setView(list);
        this->setMaxVisibleItems(EditorChannelVisibleRows);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->setFixedHeight(29);
    }

private:
    QListView *list_ = nullptr;
};

void addCosmeticChoice(QComboBox *combo, const QString &label,
                       const QString &id, const QString &kind,
                       const QJsonObject &data = {})
{
    combo->addItem(label, id);
    const auto index = combo->count() - 1;
    combo->setItemData(index, kind, CosmeticKindRole);
    if (!data.isEmpty())
    {
        combo->setItemData(
            index, QJsonDocument(data).toJson(QJsonDocument::Compact),
            CosmeticDataRole);
    }
}

QPixmap seventvLogoFor(const QWidget *widget, const QSize &size)
{
    const bool dark =
        widget->palette().color(QPalette::Window).lightness() < 128;
    return QIcon(dark ? QStringLiteral(":/buttons/seventv.svg")
                      : QStringLiteral(":/buttons/seventvDark.svg"))
        .pixmap(size);
}

QString editorChannelName(const SeventvEditorChannel &target)
{
    const auto name = target.displayName.isEmpty() ? target.username
                                                    : target.displayName;
    return name.isEmpty() ? target.connectionID : name;
}

QString editorChannelLabel(const SeventvEditorChannel &target)
{
    return editorChannelName(target);
}

QString editorChannelGroupKey(const SeventvEditorChannel &target)
{
    auto key = target.userID.trimmed().toLower();
    if (key.isEmpty())
    {
        key = target.username.trimmed().toLower();
    }
    if (key.isEmpty())
    {
        key = target.displayName.trimmed().toLower();
    }
    if (key.isEmpty())
    {
        key = target.connectionID.trimmed().toLower();
    }
    return key;
}

int editorPlatformPriority(const SeventvEditorChannel &target)
{
    if (target.platform.compare(QStringLiteral("TWITCH"),
                                Qt::CaseInsensitive) == 0)
    {
        return 0;
    }
    if (target.platform.compare(QStringLiteral("KICK"),
                                Qt::CaseInsensitive) == 0)
    {
        return 1;
    }
    return 2;
}

std::vector<SeventvEditorChannel> dialogEditorTargets()
{
    const auto &allTargets =
        SeventvAccountManager::instance().editorChannels();
    return {allTargets.begin(), allTargets.end()};
}

}  // namespace

namespace chatterino {

void SeventvAccountDialog::showDialog(QWidget *parent,
                                      const QString &channelID,
                                      const QString &channelLogin,
                                      const QString &channelDisplayName)
{
    auto *dialog = new SeventvAccountDialog(
        parent, channelID, channelLogin, channelDisplayName);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

SeventvAccountDialog::SeventvAccountDialog(QWidget *parent, QString channelID,
                                           QString channelLogin,
                                           QString channelDisplayName)
    : QDialog(parent)
    , contextChannelID_(std::move(channelID))
    , contextChannelLogin_(std::move(channelLogin))
    , contextChannelDisplayName_(std::move(channelDisplayName))
{
    this->setWindowFlag(Qt::WindowMinimizeButtonHint, false);
    this->setWindowFlag(Qt::WindowMaximizeButtonHint, false);
    this->setWindowFlag(Qt::WindowCloseButtonHint, true);
    this->setObjectName(QStringLiteral("SeventvAccountDialog"));
    this->setWindowTitle(QStringLiteral("7TV account & channel cosmetics"));
    this->resize(860, 650);
    this->setMinimumSize(700, 500);
    this->setAttribute(Qt::WA_StyledBackground);
    this->applyTheme();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto *accountFrame = new QFrame(this);
    accountFrame->setObjectName(QStringLiteral("SeventvAccountHeader"));
    accountFrame->setFrameShape(QFrame::StyledPanel);
    auto *accountLayout = new QHBoxLayout(accountFrame);
    accountLayout->setContentsMargins(12, 10, 12, 10);
    accountLayout->setSpacing(8);

    auto *logo = new QLabel(accountFrame);
    logo->setPixmap(seventvLogoFor(logo, QSize(42, 30)));
    logo->setFixedSize(48, 34);
    logo->setAlignment(Qt::AlignCenter);
    logo->setToolTip(QStringLiteral("7TV"));
    accountLayout->addWidget(logo);
    this->accountStatus_ = new QLabel(accountFrame);
    accountLayout->addWidget(this->accountStatus_, 1);
    this->signInButton_ =
        new QPushButton(QStringLiteral("Connect account"), accountFrame);
    this->signInButton_->setIcon(
        QIcon(QStringLiteral(":/buttons/seventv.svg")));
    this->refreshButton_ =
        new QPushButton(QStringLiteral("Refresh"), accountFrame);
    this->logoutButton_ =
        new QPushButton(QStringLiteral("Sign out"), accountFrame);
    accountLayout->addWidget(this->signInButton_);
    accountLayout->addWidget(this->refreshButton_);
    accountLayout->addWidget(this->logoutButton_);
    root->addWidget(accountFrame);

    this->feedback_ = new QLabel(this);
    this->feedback_->setWordWrap(true);
    this->feedback_->hide();
    root->addWidget(this->feedback_);

    this->tabs_ = new QTabWidget(this);
    root->addWidget(this->tabs_, 1);

    auto *cosmeticsTab = new QWidget(this->tabs_);
    auto *cosmeticsLayout = new QVBoxLayout(cosmeticsTab);
    cosmeticsLayout->setContentsMargins(12, 12, 12, 12);
    cosmeticsLayout->setSpacing(10);
    this->defaultsDescription_ = new QLabel(cosmeticsTab);
    this->defaultsDescription_->setWordWrap(true);
    this->defaultsDescription_->setText(QStringLiteral(
        "Defaults are used when a channel has no matching platform preset. "
        "Selecting None removes that cosmetic."));
    cosmeticsLayout->addWidget(this->defaultsDescription_);

    auto *defaultsRow = new QHBoxLayout;
    defaultsRow->addWidget(new QLabel(QStringLiteral("Paint:"), cosmeticsTab));
    this->defaultPaint_ = new CosmeticComboBox(cosmeticsTab);
    defaultsRow->addWidget(this->defaultPaint_, 1);
    defaultsRow->addWidget(new QLabel(QStringLiteral("Badge:"), cosmeticsTab));
    this->defaultBadge_ = new CosmeticComboBox(cosmeticsTab);
    defaultsRow->addWidget(this->defaultBadge_, 1);
    this->applyDefaultsButton_ =
        new QPushButton(QStringLiteral("Save && apply"), cosmeticsTab);
    defaultsRow->addWidget(this->applyDefaultsButton_);
    cosmeticsLayout->addLayout(defaultsRow);

    auto *channelAddRow = new QHBoxLayout;
    this->channelInput_ = new QLineEdit(cosmeticsTab);
    this->channelInput_->setPlaceholderText(
        QStringLiteral("Channel name"));
    if (!this->contextChannelLogin_.isEmpty())
    {
        this->channelInput_->setText(this->contextChannelLogin_);
        this->channelInput_->selectAll();
    }
    this->addChannelButton_ =
        new QPushButton(QStringLiteral("Add channel preset"), cosmeticsTab);
    channelAddRow->addWidget(this->channelInput_, 1);
    channelAddRow->addWidget(this->addChannelButton_);
    cosmeticsLayout->addLayout(channelAddRow);

    this->channelTable_ = new QTableWidget(0, 5, cosmeticsTab);
    this->channelTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Platform"), QStringLiteral("Channel"),
         QStringLiteral("Paint"), QStringLiteral("Badge"), QString{}});
    this->channelTable_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Fixed);
    this->channelTable_->setColumnWidth(0, 92);
    this->channelTable_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    this->channelTable_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    this->channelTable_->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch);
    this->channelTable_->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Fixed);
    this->channelTable_->setColumnWidth(4, 112);
    this->channelTable_->verticalHeader()->hide();
    this->channelTable_->verticalHeader()->setDefaultSectionSize(35);
    this->channelTable_->setSelectionMode(QAbstractItemView::NoSelection);
    this->channelTable_->setFocusPolicy(Qt::NoFocus);
    cosmeticsLayout->addWidget(this->channelTable_, 1);
    this->tabs_->addTab(cosmeticsTab, QStringLiteral("Cosmetics"));

    auto *emotesTab = new QWidget(this->tabs_);
    auto *emotesLayout = new QVBoxLayout(emotesTab);
    emotesLayout->setContentsMargins(12, 12, 12, 12);
    auto *channelRow = new QHBoxLayout;
    channelRow->setSpacing(6);
    channelRow->addWidget(new QLabel(QStringLiteral("Channel:"), emotesTab));
    this->emoteChannel_ = new EditorChannelComboBox(emotesTab);
    this->emoteChannel_->setToolTip(QStringLiteral(
        "Select the editable 7TV channel whose emote set you want to manage."));
    channelRow->addWidget(this->emoteChannel_, 1);
    const auto addPlatformIndicator =
        [this, emotesTab, channelRow](const QString &platform,
                                     const QString &name) {
            auto *indicator = new QLabel(emotesTab);
            indicator->setProperty("platform", platform);
            indicator->setProperty("platformName", name);
            indicator->setAlignment(Qt::AlignCenter);
            indicator->setFixedHeight(24);
            this->emotePlatformIndicators_.push_back(indicator);
            channelRow->addWidget(indicator);
        };
    addPlatformIndicator(QStringLiteral("TWITCH"), QStringLiteral("Twitch"));
    addPlatformIndicator(QStringLiteral("KICK"), QStringLiteral("Kick"));
    addPlatformIndicator(QStringLiteral("YOUTUBE"), QStringLiteral("YouTube"));
    emotesLayout->addLayout(channelRow);
    QObject::connect(this->emoteChannel_, &QComboBox::currentIndexChanged,
                     this, [this] {
                         this->rebuildEmotePlatformIndicators();
                         this->loadSelectedEmoteChannel();
                     });

    auto *splitter = new QSplitter(Qt::Horizontal, emotesTab);

    auto *selectedPanel = new QFrame(splitter);
    selectedPanel->setObjectName(QStringLiteral("SeventvEmotePanel"));
    selectedPanel->setAttribute(Qt::WA_StyledBackground);
    auto *selectedLayout = new QVBoxLayout(selectedPanel);
    selectedLayout->setContentsMargins(8, 8, 8, 8);
    selectedLayout->setSpacing(7);
    this->emoteSetTitle_ =
        new QLabel(QStringLiteral("Emotes in channel"), selectedPanel);
    this->emoteSetTitle_->setObjectName(
        QStringLiteral("SeventvEmotePanelTitle"));
    selectedLayout->addWidget(this->emoteSetTitle_);
    this->emoteFilter_ = new QLineEdit(selectedPanel);
    this->emoteFilter_->setPlaceholderText(
        QStringLiteral("Search emotes in this channel"));
    selectedLayout->addWidget(this->emoteFilter_);
    this->emoteTable_ = new QTableWidget(0, 3, selectedPanel);
    selectedLayout->addWidget(this->emoteTable_, 1);

    auto *publicPanel = new QFrame(splitter);
    publicPanel->setObjectName(QStringLiteral("SeventvEmotePanel"));
    publicPanel->setAttribute(Qt::WA_StyledBackground);
    auto *publicLayout = new QVBoxLayout(publicPanel);
    publicLayout->setContentsMargins(8, 8, 8, 8);
    publicLayout->setSpacing(7);
    auto *publicTitle =
        new QLabel(QStringLiteral("Public emotes"), publicPanel);
    publicTitle->setObjectName(QStringLiteral("SeventvEmotePanelTitle"));
    publicLayout->addWidget(publicTitle);
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    this->emoteSearch_ = new QLineEdit(publicPanel);
    this->emoteSearch_->setPlaceholderText(
        QStringLiteral("Search public 7TV emotes"));
    this->emoteSearchButton_ =
        new QPushButton(QStringLiteral("Search"), publicPanel);
    searchRow->addWidget(this->emoteSearch_, 1);
    searchRow->addWidget(this->emoteSearchButton_);
    publicLayout->addLayout(searchRow);
    this->searchTable_ = new QTableWidget(0, 2, publicPanel);
    publicLayout->addWidget(this->searchTable_, 1);

    for (auto *table : {this->emoteTable_, this->searchTable_})
    {
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(38);
        table->horizontalHeader()->hide();
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setAlternatingRowColors(true);
        table->setItemDelegateForColumn(0, new EmoteItemDelegate(table));
        table->horizontalHeader()->setStretchLastSection(false);
        table->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::Stretch);
    }
    this->emoteTable_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Fixed);
    this->emoteTable_->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Fixed);
    this->searchTable_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Fixed);
    this->emoteTable_->setColumnWidth(1, 104);
    this->emoteTable_->setColumnWidth(2, 104);
    this->searchTable_->setColumnWidth(1, 104);
    splitter->addWidget(selectedPanel);
    splitter->addWidget(publicPanel);
    splitter->setSizes({470, 330});
    emotesLayout->addWidget(splitter, 1);
    auto *emotePreviewTimer = new QTimer(this);
    emotePreviewTimer->setInterval(50);
    QObject::connect(emotePreviewTimer, &QTimer::timeout, this, [this] {
        if (this->emoteTable_->isVisible())
        {
            this->emoteTable_->viewport()->update();
            this->searchTable_->viewport()->update();
        }
    });
    emotePreviewTimer->start();
    this->tabs_->addTab(emotesTab, QStringLiteral("Emote set"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::close);
    root->addWidget(buttons);

    auto &manager = SeventvAccountManager::instance();
    QObject::connect(this->signInButton_, &QPushButton::clicked, this,
                     [&manager] {
                         manager.beginSignIn();
                     });
    QObject::connect(this->refreshButton_, &QPushButton::clicked, this,
                     [&manager] {
                         manager.refresh();
                     });
    QObject::connect(this->logoutButton_, &QPushButton::clicked, this,
                     [&manager] {
                         manager.logout();
                     });
    const auto updateDefaultsButton = [this](int) {
        this->applyDefaultsButton_->setEnabled(
            this->defaultPaint_->currentData().toString() !=
                MixedCosmeticValue &&
            this->defaultBadge_->currentData().toString() !=
                MixedCosmeticValue);
    };
    QObject::connect(this->defaultPaint_, &QComboBox::currentIndexChanged,
                     this, updateDefaultsButton);
    QObject::connect(this->defaultBadge_, &QComboBox::currentIndexChanged,
                     this, updateDefaultsButton);
    QObject::connect(this->applyDefaultsButton_, &QPushButton::clicked, this,
                     [this, &manager] {
                         const auto paintID =
                             this->defaultPaint_->currentData().toString();
                         const auto badgeID =
                             this->defaultBadge_->currentData().toString();
                         manager.setDefaults(
                             SeventvAccountManager::bothPlatforms(), paintID,
                             badgeID);
                         manager.applySelection(
                             {paintID, badgeID},
                             [this] {
                                 this->setFeedback(
                                     QStringLiteral(
                                         "Defaults saved for Twitch and Kick "
                                         "and applied."),
                                     false);
                             });
                     });
    QObject::connect(this->addChannelButton_, &QPushButton::clicked, this,
                     [this] {
                         this->addChannelByLogin();
                     });
    QObject::connect(this->channelInput_, &QLineEdit::returnPressed, this,
                     [this] {
                         this->addChannelByLogin();
                     });
    QObject::connect(this->emoteFilter_, &QLineEdit::textChanged, this,
                     [this] {
                         this->rebuildEmoteRows();
                     });
    QObject::connect(this->emoteSearchButton_, &QPushButton::clicked, this,
                     [this, &manager] {
                         manager.searchEmotes(this->emoteSearch_->text());
                     });
    QObject::connect(this->emoteSearch_, &QLineEdit::returnPressed, this,
                     [this, &manager] {
                         manager.searchEmotes(this->emoteSearch_->text());
                     });
    this->signalHolder_.managedConnect(manager.stateChanged, [this] {
        this->updateAccountState();
        QTimer::singleShot(0, this, [this] {
            this->rebuildChannelRows();
            this->rebuildEmoteChannels();
        });
    });
    this->signalHolder_.managedConnect(manager.inventoryChanged, [this] {
        this->rebuildInventory();
    });
    this->signalHolder_.managedConnect(manager.searchResultsChanged, [this] {
        this->rebuildSearchRows();
    });
    this->signalHolder_.managedConnect(manager.busyChanged, [this](bool) {
        this->updateAccountState();
    });
    this->signalHolder_.managedConnect(
        manager.feedback, [this](const QString &message, bool error) {
            this->setFeedback(message, error);
        });

    this->signalHolder_.managedConnect(getTheme()->updated, [this] {
        this->applyTheme();
    });

    this->updateAccountState();
    this->rebuildInventory();
    this->rebuildChannelRows();
    this->rebuildEmoteChannels();
    if (manager.isLoggedIn() && manager.paints().empty() && !manager.isBusy())
    {
        QTimer::singleShot(0, this, [&manager] {
            manager.refresh();
        });
    }
}

void SeventvAccountDialog::applyTheme()
{
    const auto *theme = getTheme();
    if (theme == nullptr)
    {
        return;
    }

    const bool light = theme->isLightTheme();
    const auto colorOr = [](const QColor &color, const QColor &fallback) {
        return color.isValid() ? color : fallback;
    };
    const auto background =
        colorOr(theme->window.background,
                light ? QColor(QStringLiteral("#f4f4f6"))
                      : QColor(QStringLiteral("#18181b")));
    const auto panel =
        colorOr(theme->splits.header.background,
                light ? QColor(QStringLiteral("#ffffff"))
                      : QColor(QStringLiteral("#202024")));
    const auto field =
        colorOr(theme->splits.input.background,
                light ? QColor(QStringLiteral("#ffffff"))
                      : QColor(QStringLiteral("#121214")));
    const auto text =
        colorOr(theme->window.text,
                light ? QColor(QStringLiteral("#202024"))
                      : QColor(QStringLiteral("#efeff1")));
    const auto muted =
        colorOr(theme->tabs.regular.text,
                light ? QColor(QStringLiteral("#62626c"))
                      : QColor(QStringLiteral("#adadb8")));
    const auto border =
        colorOr(theme->splits.header.border,
                light ? QColor(QStringLiteral("#cfd0d6"))
                      : QColor(QStringLiteral("#34343b")));
    const auto hover =
        colorOr(theme->tabs.regular.backgrounds.hover,
                light ? QColor(QStringLiteral("#e5e5e9"))
                      : QColor(QStringLiteral("#2c2c32")));
    const auto selected =
        colorOr(theme->splits.header.focusedBackground,
                light ? QColor(QStringLiteral("#d9d9df"))
                      : QColor(QStringLiteral("#303038")));
    const auto alternate =
        light ? panel.darker(103) : panel.lighter(106);

    auto palette = this->palette();
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::Base, field);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Button, panel);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::Highlight, selected);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Mid, border);
    this->setPalette(palette);

    this->setStyleSheet(QStringLiteral(R"(
#SeventvAccountDialog { background:%1; color:%4; }
#SeventvAccountDialog QFrame#SeventvAccountHeader,
#SeventvAccountDialog QFrame#SeventvEmotePanel {
    background:%2; border:1px solid %6; border-radius:5px;
}
#SeventvAccountDialog QLabel { color:%4; }
#SeventvAccountDialog QLabel#SeventvEmotePanelTitle {
    color:%4; font-weight:650; padding:1px 2px;
}
#SeventvAccountDialog QTabWidget::pane {
    background:%1; border:1px solid %6; top:-1px;
}
#SeventvAccountDialog QTabBar::tab {
    background:%2; color:%5; border:1px solid %6; padding:7px 13px;
}
#SeventvAccountDialog QTabBar::tab:selected {
    background:%8; color:%4;
}
#SeventvAccountDialog QLineEdit,
#SeventvAccountDialog QComboBox {
    background:%3; color:%4; border:1px solid %6;
    border-radius:4px; padding:3px 7px; selection-background-color:%8;
}
#SeventvAccountDialog QComboBox::drop-down { border:0; width:22px; }
#SeventvAccountDialog QPushButton {
    background:%2; color:%4; border:1px solid %6;
    border-radius:4px; padding:5px 11px;
}
#SeventvAccountDialog QPushButton:hover { background:%7; }
#SeventvAccountDialog QPushButton:pressed { background:%3; }
#SeventvAccountDialog QPushButton:disabled {
    color:%5; background:%3;
}
#SeventvAccountDialog QPushButton#CosmeticPlatformCycleButton { padding:0; }
#SeventvAccountDialog QPushButton#SeventvTableActionButton {
    padding:4px 10px;
}
#SeventvAccountDialog QPushButton#SeventvChannelRemoveButton {
    padding:3px 10px 5px 10px;
}
#SeventvAccountDialog QTableWidget {
    background:%3; alternate-background-color:%9; color:%4;
    border:1px solid %6; gridline-color:%6;
}
#SeventvAccountDialog QTableWidget::item { padding:2px 5px; }
#SeventvAccountDialog QHeaderView::section {
    background:%2; color:%4; border:0; border-right:1px solid %6;
    border-bottom:1px solid %6; padding:5px;
}
#SeventvAccountDialog QSplitter::handle { background:%6; width:1px; }
#SeventvAccountDialog QScrollBar:vertical {
    background:%3; width:10px;
}
#SeventvAccountDialog QScrollBar::handle:vertical {
    background:%5; border-radius:4px; min-height:28px;
}
#SeventvAccountDialog QScrollBar::add-line:vertical,
#SeventvAccountDialog QScrollBar::sub-line:vertical { height:0; }
#SeventvAccountDialog QScrollBar::add-page:vertical,
#SeventvAccountDialog QScrollBar::sub-page:vertical { background:transparent; }
)")
                            .arg(background.name(), panel.name(), field.name(),
                                 text.name(), muted.name(), border.name(),
                                 hover.name(), selected.name(),
                                 alternate.name()));
    for (auto *candidate : this->findChildren<QComboBox *>())
    {
        if (auto *combo = dynamic_cast<CosmeticComboBox *>(candidate))
        {
            combo->applyPopupTheme();
        }
    }
}

void SeventvAccountDialog::updateAccountState()
{
    auto &manager = SeventvAccountManager::instance();
    const bool loggedIn = manager.isLoggedIn();
    if (loggedIn)
    {
        const auto label = manager.displayName().isEmpty()
                               ? QStringLiteral("Connected to 7TV")
                               : QStringLiteral("Connected as %1")
                                     .arg(manager.displayName());
        this->accountStatus_->setText(
            manager.isBusy() ? label + QStringLiteral(" — working…") : label);
        this->accountStatus_->setStyleSheet(
            QStringLiteral("QLabel { color: #70db92; font-weight: 650; }"));
    }
    else
    {
        this->accountStatus_->setText(
            QStringLiteral("Not connected. Sign in to manage 7TV cosmetics "
                           "and emotes."));
        this->accountStatus_->setStyleSheet(
            QStringLiteral("QLabel { color: #aeb4bf; }"));
    }
    this->signInButton_->setVisible(!loggedIn);
    this->refreshButton_->setVisible(loggedIn);
    this->logoutButton_->setVisible(loggedIn);
    this->refreshButton_->setEnabled(!manager.isBusy());
    this->logoutButton_->setEnabled(!manager.isBusy());
    this->tabs_->setEnabled(loggedIn && !manager.isBusy());
}

void SeventvAccountDialog::rebuildInventory()
{
    this->rebuildDefaultControls();
    this->rebuildChannelRows();
}

void SeventvAccountDialog::rebuildDefaultControls()
{
    auto &manager = SeventvAccountManager::instance();
    const auto rebuild = [](QComboBox *combo, const QString &selected,
                            bool mixed, const QString &kind,
                            const auto &items) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        if (mixed)
        {
            addCosmeticChoice(combo,
                              QStringLiteral("Different on Twitch and Kick"),
                              MixedCosmeticValue, kind);
        }
        addCosmeticChoice(combo, QStringLiteral("None"), QString{}, kind);
        for (const auto &item : sortedCosmetics(items, kind))
        {
            addCosmeticChoice(combo,
                              item.name.isEmpty() ? item.id : item.name,
                              item.id, kind, item.data);
        }
        const auto index = combo->findData(selected);
        combo->setCurrentIndex(index < 0 ? 0 : index);
    };
    const auto twitchPaint =
        manager.defaultPaintID(SeventvAccountManager::twitchPlatform());
    const auto twitchBadge =
        manager.defaultBadgeID(SeventvAccountManager::twitchPlatform());
    const auto kickPaint =
        manager.defaultPaintID(SeventvAccountManager::kickPlatform());
    const auto kickBadge =
        manager.defaultBadgeID(SeventvAccountManager::kickPlatform());
    const bool mixedPaint = twitchPaint != kickPaint;
    const bool mixedBadge = twitchBadge != kickBadge;
    rebuild(this->defaultPaint_,
            mixedPaint ? MixedCosmeticValue : twitchPaint,
            mixedPaint, QStringLiteral("PAINT"), manager.paints());
    rebuild(this->defaultBadge_,
            mixedBadge ? MixedCosmeticValue : twitchBadge,
            mixedBadge, QStringLiteral("BADGE"), manager.badges());
    this->applyDefaultsButton_->setEnabled(!mixedPaint && !mixedBadge);
    this->applyDefaultsButton_->setToolTip(
        mixedPaint || mixedBadge
            ? QStringLiteral("Choose a paint and badge to use on both platforms.")
            : QString{});
}

QComboBox *SeventvAccountDialog::makeCosmeticCombo(
    const QString &kind, const QString &selectedID, bool allowInherit,
    QWidget *parent) const
{
    auto *combo = new CosmeticComboBox(parent);
    if (allowInherit)
    {
        addCosmeticChoice(combo, QStringLiteral("Default"),
                          SeventvAccountManager::inheritValue(), kind);
    }
    addCosmeticChoice(combo, QStringLiteral("None"), QString{}, kind);
    const auto &manager = SeventvAccountManager::instance();
    const auto items = sortedCosmetics(
        kind == QStringLiteral("PAINT") ? manager.paints() : manager.badges(),
        kind);
    for (const auto &item : items)
    {
        addCosmeticChoice(combo, item.name.isEmpty() ? item.id : item.name,
                          item.id, kind, item.data);
    }
    const auto index = combo->findData(selectedID);
    combo->setCurrentIndex(index < 0 ? 0 : index);
    return combo;
}

void SeventvAccountDialog::rebuildChannelRows()
{
    const bool preservePlatformCursor = [] {
        auto *hovered = QApplication::widgetAt(QCursor::pos());
        while (hovered != nullptr)
        {
            if (hovered->objectName() ==
                    QStringLiteral("CosmeticPlatformCycleButton") ||
                hovered->objectName() == QStringLiteral("CosmeticPlatformCycleCell"))
            {
                return true;
            }
            hovered = hovered->parentWidget();
        }
        return false;
    }();
    if (preservePlatformCursor)
    {
        QApplication::setOverrideCursor(
            QCursor(Qt::PointingHandCursor));
    }

    auto &manager = SeventvAccountManager::instance();
    auto overrides = manager.channelOverrides();
    std::ranges::sort(overrides, [](const auto &left, const auto &right) {
        const auto byChannel = left.channelLogin.compare(
            right.channelLogin, Qt::CaseInsensitive);
        return byChannel == 0 ? left.platform < right.platform : byChannel < 0;
    });
    for (int row = 0; row < this->channelTable_->rowCount(); ++row)
    {
        for (int column : {0, 2, 3, 4})
        {
            if (auto *editor = this->channelTable_->cellWidget(row, column))
            {
                QObject::disconnect(editor, nullptr, this, nullptr);
            }
        }
    }
    this->channelTable_->setRowCount(0);
    this->channelTable_->setRowCount(static_cast<int>(overrides.size()));
    for (int row = 0; row < static_cast<int>(overrides.size()); ++row)
    {
        const auto entry = overrides.at(static_cast<size_t>(row));
        const auto channelName =
            entry.channelDisplayName.isEmpty() ? entry.channelLogin
                                               : entry.channelDisplayName;
        auto *platformButton =
            new CosmeticPlatformCycleButton(this->channelTable_);
        platformButton->setPlatform(entry.platform);
        platformButton->setChangedCallback(
            [entry](const QString &, const QString &platform) {
                SeventvAccountManager::instance()
                    .changeChannelOverridePlatform(entry, platform);
            });
        auto *platformCell =
            centeredTableCell(platformButton, this->channelTable_);
        platformCell->setObjectName(
            QStringLiteral("CosmeticPlatformCycleCell"));
        platformCell->setCursor(Qt::PointingHandCursor);
        this->channelTable_->setCellWidget(row, 0, platformCell);

        auto *channelItem = new QTableWidgetItem(channelName);
        channelItem->setFlags(channelItem->flags() & ~Qt::ItemIsEditable);
        channelItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        this->channelTable_->setItem(row, 1, channelItem);
        auto *paint = this->makeCosmeticCombo(
            QStringLiteral("PAINT"), entry.paintID, true,
            this->channelTable_);
        auto *badge = this->makeCosmeticCombo(
            QStringLiteral("BADGE"), entry.badgeID, true,
            this->channelTable_);
        auto *remove =
            new QPushButton(QStringLiteral("Remove"), this->channelTable_);
        configureTableActionButton(remove);
        remove->setObjectName(QStringLiteral("SeventvChannelRemoveButton"));
        remove->setFixedSize(88, 29);
        this->channelTable_->setCellWidget(
            row, 2,
            centeredTableCell(paint, this->channelTable_, 2, 4, 2, 4));
        this->channelTable_->setCellWidget(
            row, 3,
            centeredTableCell(badge, this->channelTable_, 2, 4, 2, 4));
        this->channelTable_->setCellWidget(
            row, 4,
            centeredTableCell(remove, this->channelTable_, 3, 4, 8, 5));

        const auto save = [entry, paint, badge] {
            auto updated = entry;
            updated.paintID = paint->currentData().toString();
            updated.badgeID = badge->currentData().toString();
            SeventvAccountManager::instance().setChannelOverride(updated);
        };
        QObject::connect(paint, &QComboBox::currentIndexChanged, this,
                         [save](int) {
                             save();
                         });
        QObject::connect(badge, &QComboBox::currentIndexChanged, this,
                         [save](int) {
                             save();
                         });
        QObject::connect(remove, &QPushButton::clicked, this,
                         [this, channelID = entry.channelID,
                          platform = entry.platform] {
                             SeventvAccountManager::instance()
                                 .removeChannelOverride(channelID, platform);
                         });
        this->channelTable_->setRowHeight(row, 40);
    }

    if (preservePlatformCursor)
    {
        QTimer::singleShot(0, QApplication::instance(), [] {
            QApplication::restoreOverrideCursor();
            auto *hovered = QApplication::widgetAt(QCursor::pos());
            while (hovered != nullptr)
            {
                if (hovered->objectName() ==
                        QStringLiteral("CosmeticPlatformCycleButton") ||
                    hovered->objectName() ==
                        QStringLiteral("CosmeticPlatformCycleCell"))
                {
                    hovered->unsetCursor();
                    hovered->setCursor(Qt::PointingHandCursor);
                    break;
                }
                hovered = hovered->parentWidget();
            }
        });
    }
}

void SeventvAccountDialog::addChannelByLogin()
{
    auto login = this->channelInput_->text().trimmed().toLower();
    while (login.startsWith('@') || login.startsWith('#'))
    {
        login.removeFirst();
    }
    if (login.isEmpty())
    {
        this->setFeedback(QStringLiteral("Enter a channel name."), true);
        return;
    }
    const auto platform = SeventvAccountManager::twitchPlatform();
    this->addChannelButton_->setEnabled(false);
    this->setFeedback(
        QStringLiteral("Looking up %1 on Twitch…").arg(login),
        false);
    QPointer<SeventvAccountDialog> self(this);

    getHelix()->getUserByName(
        login,
        [self, platform](const HelixUser &user) {
            if (self == nullptr)
            {
                return;
            }
            if (user.id.isEmpty())
            {
                self->addChannelButton_->setEnabled(true);
                self->setFeedback(
                    QStringLiteral("That Twitch channel was not found."), true);
                return;
            }
            self->addChannelButton_->setEnabled(true);
            self->addResolvedChannelPreset(platform, user.id, user.login,
                                           user.displayName);
        },
        [self] {
            if (self == nullptr)
            {
                return;
            }
            self->addChannelButton_->setEnabled(true);
            self->setFeedback(
                QStringLiteral("Unable to resolve that Twitch channel."), true);
        });
}

void SeventvAccountDialog::addResolvedChannelPreset(
    const QString &platform, const QString &channelID,
    const QString &channelLogin, const QString &channelDisplayName)
{
    SeventvAccountManager::instance().setChannelOverride({
        .channelID = channelID,
        .channelLogin = channelLogin.toLower(),
        .channelDisplayName = channelDisplayName,
        .platform = platform,
        .paintID = SeventvAccountManager::inheritValue(),
        .badgeID = SeventvAccountManager::inheritValue(),
    });
    this->channelInput_->clear();
    this->setFeedback(
        QStringLiteral("Added a %1 preset for %2.")
            .arg(cosmeticPlatformLabel(platform), channelLogin),
        false);
}

std::optional<SeventvEditorChannel>
    SeventvAccountDialog::selectedEmoteChannel() const
{
    if (this->emoteChannel_ == nullptr ||
        this->emoteChannel_->currentIndex() < 0)
    {
        return std::nullopt;
    }
    const auto index = this->emoteChannel_->currentData().toInt();
    if (index < 0 || index >= static_cast<int>(this->emoteChannels_.size()))
    {
        return std::nullopt;
    }
    return this->emoteChannels_.at(static_cast<size_t>(index));
}

void SeventvAccountDialog::rebuildEmoteChannels()
{
    QString previousSet;
    QString previousKey;
    if (const auto previous = this->selectedEmoteChannel())
    {
        previousSet = previous->emoteSetID;
        previousKey = editorChannelGroupKey(*previous);
    }
    this->emoteChannels_.clear();
    this->emoteChannelGroups_.clear();
    for (const auto &target : dialogEditorTargets())
    {
        if (target.platform.compare(QStringLiteral("DISCORD"),
                                    Qt::CaseInsensitive) == 0)
        {
            continue;
        }
        const auto key = editorChannelGroupKey(target);
        const auto group = std::ranges::find_if(
            this->emoteChannelGroups_, [&key](const auto &current) {
                return !current.empty() &&
                       editorChannelGroupKey(current.front()) == key;
            });
        if (group == this->emoteChannelGroups_.end())
        {
            this->emoteChannelGroups_.push_back({target});
        }
        else
        {
            group->push_back(target);
        }
    }
    std::ranges::sort(this->emoteChannelGroups_,
                      [](const auto &left, const auto &right) {
                          return editorChannelName(left.front()).compare(
                                     editorChannelName(right.front()),
                                     Qt::CaseInsensitive) < 0;
                      });
    for (const auto &group : this->emoteChannelGroups_)
    {
        auto chosen = group.begin();
        const auto preferred = std::ranges::find_if(
            group, [&](const auto &target) {
                return (!previousSet.isEmpty() &&
                        target.emoteSetID == previousSet) ||
                       (!this->contextChannelID_.isEmpty() &&
                        target.connectionID == this->contextChannelID_);
            });
        if (preferred != group.end())
        {
            chosen = preferred;
        }
        else
        {
            chosen = std::ranges::min_element(
                group, [](const auto &left, const auto &right) {
                    return editorPlatformPriority(left) <
                           editorPlatformPriority(right);
                });
        }
        this->emoteChannels_.push_back(*chosen);
    }

    const QSignalBlocker blocker(this->emoteChannel_);
    this->emoteChannel_->clear();
    for (int index = 0; index < static_cast<int>(this->emoteChannels_.size());
         ++index)
    {
        const auto &target =
            this->emoteChannels_.at(static_cast<size_t>(index));
        this->emoteChannel_->addItem(editorChannelLabel(target), index);
    }

    if (this->emoteChannels_.empty())
    {
        this->emoteChannel_->addItem(
            QStringLiteral("No editable 7TV channels found"), -1);
        this->emoteChannel_->setEnabled(false);
        this->rebuildEmotePlatformIndicators();
        this->selectedEmotes_.clear();
        this->loadedEmoteSetID_.clear();
        ++this->emoteLoadGeneration_;
        this->rebuildEmoteRows();
        return;
    }

    this->emoteChannel_->setEnabled(true);
    int selectedIndex = 0;
    for (int index = 0; index < static_cast<int>(this->emoteChannels_.size());
         ++index)
    {
        const auto &target =
            this->emoteChannels_.at(static_cast<size_t>(index));
        if ((!previousKey.isEmpty() &&
             editorChannelGroupKey(target) == previousKey) ||
            (!previousSet.isEmpty() && target.emoteSetID == previousSet) ||
            (!this->contextChannelID_.isEmpty() &&
             target.connectionID == this->contextChannelID_) ||
            (!this->contextChannelLogin_.isEmpty() &&
             target.username.compare(this->contextChannelLogin_,
                                     Qt::CaseInsensitive) == 0))
        {
            selectedIndex = index;
            break;
        }
    }
    this->emoteChannel_->setCurrentIndex(selectedIndex);
    this->rebuildEmotePlatformIndicators();
    QTimer::singleShot(0, this, [this] {
        this->loadSelectedEmoteChannel();
    });
}

void SeventvAccountDialog::rebuildEmotePlatformIndicators()
{
    const auto selectedIndex =
        this->emoteChannel_ == nullptr ||
                this->emoteChannel_->currentIndex() < 0
            ? -1
            : this->emoteChannel_->currentData().toInt();
    const std::vector<SeventvEditorChannel> *selectedGroup = nullptr;
    if (selectedIndex >= 0 &&
        selectedIndex < static_cast<int>(this->emoteChannelGroups_.size()))
    {
        selectedGroup =
            &this->emoteChannelGroups_.at(static_cast<size_t>(selectedIndex));
    }
    for (auto *indicator : this->emotePlatformIndicators_)
    {
        const auto platform = indicator->property("platform").toString();
        const auto name = indicator->property("platformName").toString();
        const bool available = selectedGroup != nullptr &&
                               std::ranges::any_of(
                                   *selectedGroup,
                                   [&platform](const auto &channel) {
                                       return channel.platform.compare(
                                                  platform,
                                                  Qt::CaseInsensitive) == 0;
                                   });
        indicator->setText(
            available
                ? QString(QChar(0x2713)) + QStringLiteral(" ") + name
                : name);
        indicator->setToolTip(
            available
                ? QStringLiteral("This channel has an editable 7TV connection "
                                 "through %1.")
                      .arg(name)
                : QStringLiteral("This channel does not have an editable 7TV "
                                 "connection through %1.")
                      .arg(name));
        indicator->setStyleSheet(
            available
                ? QStringLiteral(
                      "QLabel { background:#394039; border:1px solid #687268; "
                      "border-radius:4px; color:#e2e7e2; padding:2px 6px; }")
                : QStringLiteral(
                      "QLabel { background:#303238; border:1px solid #4b4f56; "
                      "border-radius:4px; color:#8d9199; padding:2px 6px; }"));
    }
}

void SeventvAccountDialog::loadSelectedEmoteChannel(bool force)
{
    const auto selected = this->selectedEmoteChannel();
    if (!selected)
    {
        return;
    }
    if (!force && selected->emoteSetID == this->loadedEmoteSetID_)
    {
        return;
    }

    const int generation = ++this->emoteLoadGeneration_;
    this->loadedEmoteSetID_ = selected->emoteSetID;
    this->selectedEmotes_.clear();
    this->emoteTable_->setRowCount(0);
    this->emoteSetTitle_->setText(
        QStringLiteral("Loading %1...").arg(editorChannelName(*selected)));

    SeventvAccountManager::instance().loadEditorChannelEmotes(
        *selected,
        [self = QPointer<SeventvAccountDialog>(this),
         generation](std::vector<SeventvManagedEmote> emotes) {
            if (self == nullptr || generation != self->emoteLoadGeneration_)
            {
                return;
            }
            self->selectedEmotes_ = std::move(emotes);
            self->rebuildEmoteRows();
        },
        [self = QPointer<SeventvAccountDialog>(this),
         generation](const QString &error) {
            if (self == nullptr || generation != self->emoteLoadGeneration_)
            {
                return;
            }
            self->loadedEmoteSetID_.clear();
            self->selectedEmotes_.clear();
            self->rebuildEmoteRows();
            self->setFeedback(error, true);
        });
}

void SeventvAccountDialog::rebuildEmoteRows()
{
    const auto selected = this->selectedEmoteChannel();
    this->emoteSetTitle_->setText(
        selected ? QStringLiteral("Emotes in %1")
                       .arg(editorChannelName(*selected))
                 : QStringLiteral("Emotes in channel"));

    const auto filter = this->emoteFilter_->text().trimmed();
    std::vector<SeventvManagedEmote> emotes;
    emotes.reserve(this->selectedEmotes_.size());
    for (const auto &emote : this->selectedEmotes_)
    {
        if (filter.isEmpty() ||
            emote.name.contains(filter, Qt::CaseInsensitive))
        {
            emotes.push_back(emote);
        }
    }
    this->emoteTable_->setRowCount(static_cast<int>(emotes.size()));
    for (int row = 0; row < static_cast<int>(emotes.size()); ++row)
    {
        const auto emote = emotes.at(static_cast<size_t>(row));
        auto *item = new QTableWidgetItem(emote.name);
        item->setData(Qt::UserRole, emote.id);
        item->setData(EmoteDataRole,
                      QJsonDocument(emote.data).toJson(QJsonDocument::Compact));
        this->emoteTable_->setItem(row, 0, item);
        auto *rename =
            new QPushButton(QStringLiteral("Rename"), this->emoteTable_);
        auto *remove =
            new QPushButton(QStringLiteral("Remove"), this->emoteTable_);
        configureTableActionButton(rename);
        configureTableActionButton(remove);
        this->emoteTable_->setCellWidget(
            row, 1, centeredTableCell(rename, this->emoteTable_));
        this->emoteTable_->setCellWidget(
            row, 2, centeredTableCell(remove, this->emoteTable_));
        this->emoteTable_->setRowHeight(row, 40);
        QObject::connect(rename, &QPushButton::clicked, this,
                         [this, selected, emote] {
                             if (!selected)
                             {
                                 return;
                             }
                             bool accepted = false;
                             const auto name = QInputDialog::getText(
                                 this, QStringLiteral("Rename 7TV emote"),
                                 QStringLiteral("Emote name:"),
                                 QLineEdit::Normal, emote.name, &accepted);
                             if (accepted && !name.trimmed().isEmpty())
                             {
                                 SeventvAccountManager::instance()
                                     .renameEmoteInEditorChannel(
                                         *selected, emote.id, name.trimmed(),
                                         [self =
                                              QPointer<SeventvAccountDialog>(
                                                  this)] {
                                             if (self != nullptr)
                                             {
                                                 self->loadSelectedEmoteChannel(
                                                     true);
                                             }
                                         },
                                         [self =
                                              QPointer<SeventvAccountDialog>(
                                                  this)](
                                             const QString &error) {
                                             if (self != nullptr)
                                             {
                                                 self->setFeedback(error, true);
                                             }
                                         });
                             }
                         });
        QObject::connect(remove, &QPushButton::clicked, this,
                         [this, selected, emote] {
                             if (!selected)
                             {
                                 return;
                             }
                             SeventvAccountManager::instance()
                                 .removeEmoteFromEditorChannel(
                                     *selected, emote.id,
                                     [self =
                                          QPointer<SeventvAccountDialog>(this)] {
                                         if (self != nullptr)
                                         {
                                             self->loadSelectedEmoteChannel(
                                                 true);
                                         }
                                     },
                                     [self =
                                          QPointer<SeventvAccountDialog>(this)](
                                         const QString &error) {
                                         if (self != nullptr)
                                         {
                                             self->setFeedback(error, true);
                                         }
                                     });
                         });
    }
    this->rebuildSearchRows();
}

void SeventvAccountDialog::rebuildSearchRows()
{
    const auto selected = this->selectedEmoteChannel();
    const auto &results = SeventvAccountManager::instance().searchResults();
    this->searchTable_->setRowCount(static_cast<int>(results.size()));
    for (int row = 0; row < static_cast<int>(results.size()); ++row)
    {
        const auto emote = results.at(static_cast<size_t>(row));
        auto *item = new QTableWidgetItem(emote.name);
        item->setData(Qt::UserRole, emote.id);
        item->setData(EmoteDataRole,
                      QJsonDocument(emote.data).toJson(QJsonDocument::Compact));
        this->searchTable_->setItem(row, 0, item);
        const auto alreadyAdded = std::ranges::any_of(
            this->selectedEmotes_, [&emote](const auto &current) {
                return current.id == emote.id;
            });
        auto *add = new QPushButton(
            alreadyAdded ? QStringLiteral("Added") : QStringLiteral("Add"),
            this->searchTable_);
        configureTableActionButton(add);
        add->setEnabled(selected.has_value() && !alreadyAdded);
        this->searchTable_->setCellWidget(
            row, 1, centeredTableCell(add, this->searchTable_));
        this->searchTable_->setRowHeight(row, 40);
        QObject::connect(
            add, &QPushButton::clicked, this, [this, selected, emote] {
                if (!selected)
                {
                    return;
                }
                SeventvAccountManager::instance().addEmoteToEditorChannel(
                    *selected, emote.id, emote.name,
                    [self = QPointer<SeventvAccountDialog>(this)] {
                        if (self != nullptr)
                        {
                            self->loadSelectedEmoteChannel(true);
                        }
                    },
                    [self = QPointer<SeventvAccountDialog>(this)](
                        const QString &error) {
                        if (self != nullptr)
                        {
                            self->setFeedback(error, true);
                        }
                    });
            });
    }
}

void SeventvAccountDialog::setFeedback(const QString &message, bool error)
{
    this->feedback_->setText(message);
    this->feedback_->setVisible(!message.trimmed().isEmpty());
    this->feedback_->setStyleSheet(
        QStringLiteral("QLabel { color: %1; }")
            .arg(error ? QStringLiteral("#ff7f7f")
                       : QStringLiteral("#aeb4bf")));
}

}  // namespace chatterino
