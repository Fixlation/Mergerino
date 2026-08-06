// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/Notebook.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/ProviderId.hpp"
#include "common/QLogging.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/InitUpdateButton.hpp"
#include "widgets/buttons/PixmapButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <boost/foreach.hpp>
#include <QActionGroup>
#include <QAbstractAnimation>
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFormLayout>
#include <QInputDialog>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QStandardPaths>
#include <QTextOption>
#include <QUuid>
#include <QVariantAnimation>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace chatterino {

namespace {

QString providerIconPath(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Kick:
            return ":/platforms/kick.svg";
        case ProviderId::YouTube:
            return ":/platforms/youtube.svg";
        case ProviderId::Twitch:
        default:
            return ":/platforms/twitch.svg";
    }
}

}  // namespace

class NotebookFolderTab : public BaseWidget
{
public:
    NotebookFolderTab(Notebook *notebook, QString folderId)
        : BaseWidget(notebook)
        , positionChangedAnimation_(this, "pos")
        , notebook_(notebook)
        , folderId_(std::move(folderId))
    {
        this->setMouseTracking(true);
        this->positionChangedAnimation_.setEasingCurve(
            QEasingCurve(QEasingCurve::InCubic));

        this->openCloseAnimation_.setDuration(220);
        this->openCloseAnimation_.setEasingCurve(QEasingCurve::InOutCubic);
        QObject::connect(&this->openCloseAnimation_,
                         &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->openProgress_ = value.toDouble();
                             this->update();
                         });

        this->closeSwapAnimation_.setDuration(150);
        this->closeSwapAnimation_.setEasingCurve(QEasingCurve::InOutCubic);
        QObject::connect(&this->closeSwapAnimation_,
                         &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->closeSwapProgress_ = value.toDouble();
                             this->update();
                         });

        this->updateSize();
    }

    void setTitle(QString title)
    {
        this->title_ = std::move(title);
        this->updateSize();
        this->update();
    }

    void setExpanded(bool expanded, bool animated)
    {
        this->expanded_ = expanded;

        const auto target = expanded ? 1.0 : 0.0;
        if (!animated)
        {
            this->openCloseAnimation_.stop();
            this->openProgress_ = target;
            this->update();
            return;
        }

        this->openCloseAnimation_.stop();
        this->openCloseAnimation_.setStartValue(this->openProgress_);
        this->openCloseAnimation_.setEndValue(target);
        this->openCloseAnimation_.start();
    }

    void setTabLocation(NotebookTabLocation location)
    {
        this->tabLocation_ = location;
        this->update();
    }

    void setInLastRow(bool value)
    {
        this->isInLastRow_ = value;
        this->update();
    }

    void updateSize()
    {
        const int height = int(NOTEBOOK_TAB_HEIGHT * this->scale());
        this->setFixedSize(this->normalTabWidth(), height);
    }

    void growWidth(int width)
    {
        const int height = int(NOTEBOOK_TAB_HEIGHT * this->scale());
        this->setFixedSize(std::max(width, this->normalTabWidth()), height);
    }

    int normalTabWidth() const
    {
        const auto scale = this->scale();
        const auto metrics =
            getApp()->getFonts()->getFontMetrics(FontStyle::UiTabs, scale);
        const int textWidth = metrics.horizontalAdvance(this->title_);
        const QString countText = this->folderCountText();
        const int actionWidth = this->actionSlotWidth(countText);
        const int actionGap = actionWidth > 0 ? int(5 * scale) : 0;

        return std::max(int(78 * scale),
                        this->textLeft() + textWidth + actionGap +
                            actionWidth + this->actionSlotRightPadding());
    }

    QRect getDesiredRect() const
    {
        return QRect(this->positionAnimationDesiredPoint_, this->size());
    }

    void moveAnimated(QPoint targetPos, bool animated = true)
    {
        this->positionAnimationDesiredPoint_ = targetPos;

        if (!animated || !this->notebook_->isVisible())
        {
            this->move(targetPos);
            return;
        }

        if (this->positionChangedAnimation_.state() ==
                QAbstractAnimation::Running &&
            this->positionChangedAnimation_.endValue() == targetPos)
        {
            return;
        }

        this->positionChangedAnimation_.stop();
        this->positionChangedAnimation_.setDuration(75);
        this->positionChangedAnimation_.setStartValue(this->pos());
        this->positionChangedAnimation_.setEndValue(targetPos);
        this->positionChangedAnimation_.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        auto *app = getApp();
        QPainter painter(this);
        const float scale = this->scale();
        const int height = int(scale * NOTEBOOK_TAB_HEIGHT);

        const bool visualSelection =
            this->notebook_->folderContainsSelectedPage(this->folderId_) ||
            this->notebook_->folderContainsBulkSelectedPage(this->folderId_);

        Theme::TabColors colors = visualSelection
                                      ? this->theme->tabs.selected
                                      : this->theme->tabs.regular;

        const bool windowFocused = this->window() == QApplication::activeWindow();
        QBrush tabBackground =
            windowFocused ? colors.backgrounds.regular
                          : colors.backgrounds.unfocused;

        if (this->mouseOver_ && !visualSelection)
        {
            auto hover = colors.backgrounds.hover;
            hover.setAlpha(std::max(hover.alpha(), 70));
            tabBackground = hover;
        }

        auto selectionOffset = ceil((visualSelection ? 0.f : 1.f) * scale);
        auto bgRect = this->rect();
        switch (this->tabLocation_)
        {
            case NotebookTabLocation::Top:
                bgRect.setTop(selectionOffset);
                break;
            case NotebookTabLocation::Left:
                bgRect.setLeft(selectionOffset);
                break;
            case NotebookTabLocation::Right:
                bgRect.setRight(bgRect.width() - selectionOffset);
                break;
            case NotebookTabLocation::Bottom:
                bgRect.setBottom(bgRect.height() - selectionOffset);
                break;
        }

        painter.fillRect(bgRect, tabBackground);

        const auto lineThickness = ceil((visualSelection ? 2.f : 1.f) * scale);
        auto lineColor = this->mouseOver_ ? colors.line.hover
                                          : (windowFocused
                                                 ? colors.line.regular
                                                 : colors.line.unfocused);
        QRect lineRect;
        switch (this->tabLocation_)
        {
            case NotebookTabLocation::Top:
                lineRect =
                    QRect(bgRect.left(), bgRect.y(), bgRect.width(),
                          lineThickness);
                break;
            case NotebookTabLocation::Left:
                lineRect =
                    QRect(bgRect.x(), bgRect.top(), lineThickness,
                          bgRect.height());
                break;
            case NotebookTabLocation::Right:
                lineRect = QRect(bgRect.right() - lineThickness, bgRect.top(),
                                 lineThickness, bgRect.height());
                break;
            case NotebookTabLocation::Bottom:
                lineRect = QRect(bgRect.left(), bgRect.bottom() - lineThickness,
                                 bgRect.width(), lineThickness);
                break;
        }
        painter.fillRect(lineRect, lineColor);

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setFont(app->getFonts()->getFont(FontStyle::UiTabs, scale));
        auto metrics = app->getFonts()->getFontMetrics(FontStyle::UiTabs, scale);

        const int iconWidth = int(18 * scale);
        const int iconHeight = int(16 * scale);
        QRectF iconRect(int(8 * scale), (height - iconHeight) / 2.0,
                        iconWidth, iconHeight);
        const qreal p = std::clamp(this->openProgress_, 0.0, 1.0);
        const qreal s = scale;
        const qreal x = iconRect.left();
        const qreal y = iconRect.top();

        auto folderStroke = colors.text;
        folderStroke.setAlpha(visualSelection || this->mouseOver_ ? 245 : 215);
        QPen folderPen(folderStroke, std::max<qreal>(1.0, 1.28 * s));
        folderPen.setCapStyle(Qt::RoundCap);
        folderPen.setJoinStyle(Qt::RoundJoin);

        painter.setPen(folderPen);
        painter.setBrush(Qt::NoBrush);

        const qreal tabLift = 1.15 * s * p;
        QPainterPath folderBack;
        folderBack.moveTo(x + 1.5 * s, y + 7.6 * s);
        folderBack.lineTo(x + 1.5 * s, y + 4.9 * s);
        folderBack.cubicTo(x + 1.5 * s, y + 3.35 * s, x + 2.35 * s,
                           y + 2.5 * s, x + 3.9 * s, y + 2.5 * s);
        folderBack.lineTo(x + 5.0 * s, y + 2.5 * s);
        folderBack.cubicTo(x + 5.75 * s, y + 2.5 * s, x + 6.15 * s,
                           y + 2.75 * s - tabLift, x + 6.7 * s,
                           y + 3.3 * s - tabLift);
        folderBack.lineTo(x + 7.65 * s, y + 4.15 * s - tabLift);
        folderBack.cubicTo(x + 8.15 * s, y + 4.6 * s - tabLift,
                           x + 8.75 * s, y + 4.85 * s - tabLift,
                           x + 9.55 * s, y + 4.85 * s - tabLift);
        folderBack.lineTo(x + 14.1 * s, y + 4.85 * s - tabLift);
        folderBack.cubicTo(x + 15.65 * s, y + 4.85 * s - tabLift,
                           x + 16.55 * s, y + 5.75 * s - tabLift,
                           x + 16.55 * s, y + 7.45 * s);
        painter.drawPath(folderBack);

        const qreal frontLeft = 7.65 * s + 2.05 * s * p;
        const qreal frontRight = 7.65 * s + 0.85 * s * p;
        const qreal lowerInset = 0.4 * s * p;
        QPainterPath folderFront;
        folderFront.moveTo(x + 1.75 * s, y + frontLeft);
        folderFront.cubicTo(x + 5.4 * s, y + frontLeft - 0.18 * s * p,
                            x + 11.2 * s, y + frontRight - 0.12 * s * p,
                            x + 16.25 * s, y + frontRight);
        folderFront.cubicTo(x + 16.65 * s, y + frontRight,
                            x + 16.95 * s, y + frontRight + 0.3 * s,
                            x + 16.9 * s, y + frontRight + 0.72 * s);
        folderFront.lineTo(x + 16.4 * s, y + 12.0 * s - lowerInset);
        folderFront.cubicTo(x + 16.15 * s, y + 13.4 * s, x + 15.25 * s,
                            y + 14.1 * s, x + 13.85 * s, y + 14.1 * s);
        folderFront.lineTo(x + 4.1 * s, y + 14.1 * s);
        folderFront.cubicTo(x + 2.7 * s, y + 14.1 * s, x + 1.8 * s,
                            y + 13.4 * s, x + 1.55 * s,
                            y + 12.0 * s - lowerInset);
        folderFront.lineTo(x + 1.12 * s, y + frontLeft + 0.72 * s);
        folderFront.cubicTo(x + 1.05 * s, y + frontLeft + 0.3 * s,
                            x + 1.35 * s, y + frontLeft, x + 1.75 * s,
                            y + frontLeft);
        painter.drawPath(folderFront);

        const int textLeft = this->textLeft();
        const QString countText = this->folderCountText();
        const QRect actionRect = this->actionSlotRect(countText);
        const int textRight = actionRect.isNull()
                                  ? this->width() -
                                        this->actionSlotRightPadding()
                                  : actionRect.left() -
                                        this->actionSlotGap(countText);
        QRect textRect(textLeft, 0, std::max(0, textRight - textLeft), height);
        QTextOption textOption(Qt::AlignLeft | Qt::AlignVCenter);
        textOption.setWrapMode(QTextOption::NoWrap);
        painter.setPen(colors.text);
        painter.drawText(textRect, this->title_, textOption);

        const qreal closeSwap =
            std::clamp(this->closeSwapProgress_, 0.0, 1.0);
        if (!countText.isEmpty())
        {
            auto countColor = colors.text;
            countColor.setAlpha(static_cast<int>(145 * (1.0 - closeSwap)));

            if (countColor.alpha() > 0)
            {
                painter.save();
                painter.translate(0.0, 5.0 * scale * closeSwap);
                painter.setPen(countColor);
                painter.drawText(
                    actionRect, countText,
                    QTextOption(Qt::AlignCenter | Qt::AlignVCenter));
                painter.restore();
            }
        }

        if (this->shouldDrawXButton())
        {
            painter.save();
            painter.translate(0.0, 5.0 * scale * (1.0 - closeSwap));
            painter.setOpacity(closeSwap);
            painter.setRenderHint(QPainter::Antialiasing, false);

            const QRect xRect = this->getXRect();
            if (!xRect.isNull())
            {
                if (this->mouseOverX_)
                {
                    painter.fillRect(xRect, QColor(0, 0, 0, 64));

                    if (this->mouseDownX_)
                    {
                        painter.fillRect(xRect, QColor(0, 0, 0, 64));
                    }
                }

                const int inset = static_cast<int>(scale * 4);
                painter.setPen(colors.text);
                painter.drawLine(xRect.topLeft() + QPoint(inset, inset),
                                 xRect.bottomRight() +
                                     QPoint(-inset, -inset));
                painter.drawLine(xRect.topRight() + QPoint(-inset, inset),
                                 xRect.bottomLeft() +
                                     QPoint(inset, -inset));
            }
            painter.restore();
        }

        if (!visualSelection)
        {
            auto borderColor = app->getThemes()->window.background;
            if (this->isInLastRow_)
            {
                QRect borderRect;
                switch (this->tabLocation_)
                {
                    case NotebookTabLocation::Top:
                        borderRect =
                            QRect(0, this->height() - 1, this->width(), 1);
                        break;
                    case NotebookTabLocation::Left:
                        borderRect =
                            QRect(this->width() - 1, 0, 1, this->height());
                        break;
                    case NotebookTabLocation::Right:
                        borderRect = QRect(0, 0, 1, this->height());
                        break;
                    case NotebookTabLocation::Bottom:
                        borderRect = QRect(0, 0, this->width(), 1);
                        break;
                }
                painter.fillRect(borderRect, borderColor);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            this->mouseDown_ = true;
            this->mouseDownX_ = this->getXRect().contains(event->pos());
            event->accept();
            return;
        }

        if (event->button() == Qt::RightButton)
        {
            this->notebook_->showFolderContextMenu(
                this->folderId_, event->globalPosition().toPoint() +
                                     QPoint(0, 8));
            event->accept();
            return;
        }

        BaseWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->mouseDown_)
        {
            const bool wasMouseDownX = this->mouseDownX_;
            this->mouseDown_ = false;
            this->mouseDownX_ = false;

            if (wasMouseDownX && this->getXRect().contains(event->pos()))
            {
                this->notebook_->removeFolder(this->folderId_);
            }
            else if (!wasMouseDownX && this->rect().contains(event->pos()))
            {
                this->notebook_->toggleFolderExpanded(this->folderId_);
            }
            event->accept();
            return;
        }

        BaseWidget::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (this->hasXButton())
        {
            const bool overX = this->getXRect().contains(event->pos());
            if (overX != this->mouseOverX_)
            {
                this->mouseOverX_ = overX;
                this->update();
            }
        }
        else if (this->mouseOverX_)
        {
            this->mouseOverX_ = false;
            this->update();
        }

        BaseWidget::mouseMoveEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton &&
            !this->getXRect().contains(event->pos()))
        {
            this->notebook_->showRenameFolderDialog(this->folderId_);
            event->accept();
            return;
        }

        BaseWidget::mouseDoubleClickEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        this->mouseOver_ = true;
        this->setCloseSwapTarget(1.0);
        this->update();
        BaseWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        this->mouseOver_ = false;
        this->mouseDown_ = false;
        this->mouseOverX_ = false;
        this->mouseDownX_ = false;
        this->setCloseSwapTarget(0.0);
        this->update();
        BaseWidget::leaveEvent(event);
    }

private:
    bool hasXButton() const
    {
        return getSettings()->showTabCloseButton &&
               this->notebook_->getAllowUserTabManagement();
    }

    bool shouldDrawXButton() const
    {
        return this->hasXButton() && this->closeSwapProgress_ > 0.01;
    }

    QString folderCountText() const
    {
        const int count = this->notebook_->folderPageCount(this->folderId_);
        return count > 0 ? QString::number(count) : QString();
    }

    int textLeft() const
    {
        const auto scale = this->scale();
        return int(8 * scale + 18 * scale + 6 * scale);
    }

    int actionSlotRightPadding() const
    {
        return int(5 * this->scale());
    }

    int actionSlotGap(const QString &countText) const
    {
        return this->actionSlotWidth(countText) > 0 ? int(5 * this->scale())
                                                    : 0;
    }

    int actionSlotWidth(const QString &countText) const
    {
        const auto scale = this->scale();
        const auto metrics =
            getApp()->getFonts()->getFontMetrics(FontStyle::UiTabs, scale);
        const int countWidth =
            countText.isEmpty()
                ? 0
                : static_cast<int>(std::ceil(
                      metrics.horizontalAdvance(countText)));
        const int closeWidth = this->hasXButton() ? int(16 * scale) : 0;

        return std::max(countWidth, closeWidth);
    }

    QRect actionSlotRect(const QString &countText) const
    {
        const int width = this->actionSlotWidth(countText);
        if (width <= 0)
        {
            return {};
        }

        return QRect(this->width() - this->actionSlotRightPadding() - width, 0,
                     width, this->height());
    }

    QRect actionSlotRect() const
    {
        return this->actionSlotRect(this->folderCountText());
    }

    void setCloseSwapTarget(double target)
    {
        target = std::clamp(target, 0.0, 1.0);
        if (!this->hasXButton())
        {
            target = 0.0;
        }

        if (std::abs(this->closeSwapProgress_ - target) < 0.001)
        {
            this->closeSwapAnimation_.stop();
            this->closeSwapProgress_ = target;
            this->update();
            return;
        }

        this->closeSwapAnimation_.stop();
        this->closeSwapAnimation_.setStartValue(this->closeSwapProgress_);
        this->closeSwapAnimation_.setEndValue(target);
        this->closeSwapAnimation_.start();
    }

    QRect getXRect() const
    {
        if (!this->hasXButton())
        {
            return {};
        }

        const QRect actionRect = this->actionSlotRect();
        if (actionRect.isNull())
        {
            return {};
        }

        const QRect rect = this->rect();
        const float scale = this->scale();
        const int size = static_cast<int>(16 * scale);

        return QRect(actionRect.left() + (actionRect.width() - size) / 2,
                     rect.top() + (rect.height() - size) / 2, size, size);
    }

    QPropertyAnimation positionChangedAnimation_;
    QPoint positionAnimationDesiredPoint_;
    QVariantAnimation openCloseAnimation_;
    QVariantAnimation closeSwapAnimation_;

    Notebook *notebook_;
    QString folderId_;
    QString title_;
    bool expanded_ = true;
    double openProgress_ = 1.0;
    double closeSwapProgress_ = 0.0;
    bool mouseOver_ = false;
    bool mouseDown_ = false;
    bool mouseOverX_ = false;
    bool mouseDownX_ = false;
    bool isInLastRow_ = false;
    NotebookTabLocation tabLocation_ = NotebookTabLocation::Top;
};

Notebook::Notebook(QWidget *parent)
    : BaseWidget(parent)
    , addButton_(new DrawnButton(DrawnButton::Symbol::Plus,
                                 {
                                     .padding = 7,
                                     .thickness = 1,
                                 },
                                 this))
{
    this->addButton_->setHidden(true);
    this->addButton_->enableDrops({"chatterino/split"});

    QObject::connect(
        this->addButton_, &Button::dropEvent, this, [this](QDropEvent *event) {
            auto *draggedSplit = dynamic_cast<Split *>(event->source());
            if (!draggedSplit)
            {
                qCDebug(chatterinoWidget) << "Dropped something that wasn't a "
                                             "split onto a notebook button";
                return;
            }

            event->acceptProposedAction();

            auto *page = new SplitContainer(this);
            auto *tab = this->addPage(page);
            page->setTab(tab);

            draggedSplit->setParent(page);
            page->insertSplit(draggedSplit);
        });

    this->lockNotebookLayoutAction_ = new QAction("Lock Tab Layout", this);

    // Load lock notebook layout state from settings
    this->setLockNotebookLayout(getSettings()->lockNotebookLayout.getValue());

    this->lockNotebookLayoutAction_->setCheckable(true);
    this->lockNotebookLayoutAction_->setChecked(this->lockNotebookLayout_);

    // Update lockNotebookLayout_ value anytime the user changes the checkbox state
    QObject::connect(this->lockNotebookLayoutAction_, &QAction::triggered,
                     [this](bool value) {
                         this->setLockNotebookLayout(value);
                     });

    this->toggleTopMostAction_ = new QAction("Top most window", this);
    this->toggleTopMostAction_->setCheckable(true);
    auto *window = dynamic_cast<BaseWindow *>(this->window());
    if (window)
    {
        auto updateTopMost = [this, window] {
            this->toggleTopMostAction_->setChecked(window->isTopMost());
        };
        updateTopMost();
        QObject::connect(this->toggleTopMostAction_, &QAction::triggered,
                         window, [window] {
                             window->setTopMost(!window->isTopMost());
                         });
        QObject::connect(window, &BaseWindow::topMostChanged, this,
                         updateTopMost);
    }
    else
    {
        qCWarning(chatterinoApp)
            << "Notebook must be created within a BaseWindow";
    }

    this->addTabFolderAction_ = new QAction("Add Tab Folder", this);
    QObject::connect(this->addTabFolderAction_, &QAction::triggered, this,
                     [this] {
                         this->showAddFolderDialog();
                     });

    // Manually resize the add button so the initial paint uses the correct
    // width when computing the maximum width occupied per column in vertical
    // tab rendering.
    this->resizeAddButton();
}

NotebookTab *Notebook::addPage(QWidget *page, QString title, bool select)
{
    return this->addPageAt(page, -1, std::move(title), select);
}

NotebookTab *Notebook::addPageAt(QWidget *page, int position, QString title,
                                 bool select, QString folderId)
{
    // Queue up save because: Tab added
    getApp()->getWindows()->queueSave();

    auto *tab = new NotebookTab(this);
    tab->page = page;

    tab->setCustomTitle(title);
    tab->setTabLocation(this->tabLocation_);

    Item item;
    item.page = page;
    item.tab = tab;
    if (!folderId.isEmpty())
    {
        auto *folder =
            this->ensureFolder(folderId, this->nextFolderTitle(), true);
        if (folder != nullptr)
        {
            item.folderId = folder->id;
        }
    }

    if (position == -1 && !item.folderId.isEmpty())
    {
        const auto lastFolderIndex = this->lastIndexInFolder(item.folderId);
        if (lastFolderIndex != -1)
        {
            position = lastFolderIndex + 1;
        }
    }

    if (position == -1 || position >= this->items_.count())
    {
        this->items_.push_back(item);
    }
    else
    {
        this->items_.insert(position, item);
    }

    page->hide();
    page->setParent(this);

    if (select || this->items_.count() == 1)
    {
        this->select(page);
    }

    this->performLayout();
    tab->setVisible(this->shouldShowTab(tab));
    return tab;
}

void Notebook::removePage(QWidget *page)
{
    int removingIndex = this->indexOf(page);
    if (removingIndex == -1)
    {
        return;
    }

    // Queue up save because: Tab removed
    getApp()->getWindows()->queueSave();

    this->setBulkSelectedTab(this->items_[removingIndex].tab, false);

    if (this->selectedPage_ == page)
    {
        // The page that we are removing is currently selected. We need to determine
        // the best tab to select before we remove this one. We follow a strategy used
        // by many web browsers: select the next tab. If there is no next tab, select
        // the previous tab.
        int countVisible = this->getVisibleTabCount();
        int visibleIndex = this->visibleIndexOf(page);
        assert(visibleIndex != -1);  // A selected page should always be visible

        if (this->items_.count() == 1)
        {
            // Deleting only tab, select nothing
            this->select(nullptr);
        }
        else if (countVisible == 1)
        {
            // Closing the only visible tab, try to select any tab (even if not visible)
            int nextIndex = (removingIndex + 1) % this->items_.count();
            this->select(this->items_[nextIndex].page);
        }
        else if (visibleIndex == countVisible - 1)
        {
            // Closing last visible tab, select the previous visible tab
            this->selectPreviousTab();
        }
        else
        {
            // Otherwise, select the next visible tab
            this->selectNextTab();
        }
    }

    // Remove page and delete resources
    this->items_[removingIndex].page->deleteLater();
    this->items_[removingIndex].tab->deleteLater();
    this->items_.removeAt(removingIndex);

    this->performLayout(true);
}

void Notebook::duplicatePage(QWidget *page)
{
    auto *item = this->findItem(page);
    assert(item != nullptr);
    if (item == nullptr)
    {
        return;
    }

    auto *container = dynamic_cast<SplitContainer *>(item->page);
    if (!container)
    {
        return;
    }

    auto *newContainer = new SplitContainer(this);
    if (!container->getSplits().empty())
    {
        auto descriptor = container->buildDescriptor();
        newContainer->applyFromDescriptor(descriptor);
    }

    const auto tabPosition = this->indexOf(page);
    auto newTabPosition = -1;
    if (tabPosition != -1)
    {
        newTabPosition = tabPosition + 1;
    }

    QString newTabTitle = "";
    if (item->tab->hasCustomTitle())
    {
        newTabTitle = item->tab->getCustomTitle();
    }

    auto *tab =
        this->addPageAt(newContainer, newTabPosition, newTabTitle, false,
                        item->folderId);
    tab->copyHighlightStateAndSourcesFrom(item->tab);

    newContainer->setTab(tab);
}

void Notebook::removeCurrentPage()
{
    if (auto *selectedPage = this->getSelectedPage())
    {
        this->removePage(selectedPage);
    }
}

int Notebook::indexOf(QWidget *page) const
{
    for (int i = 0; i < this->items_.count(); i++)
    {
        if (this->items_[i].page == page)
        {
            return i;
        }
    }

    return -1;
}

int Notebook::visibleIndexOf(QWidget *page) const
{
    int i = 0;
    for (const auto &item : this->items_)
    {
        if (item.page == page)
        {
            return this->isItemVisible(item) ? i : -1;
        }
        if (this->isItemVisible(item))
        {
            ++i;
        }
    }

    return -1;
}

int Notebook::getVisibleTabCount() const
{
    int i = 0;
    for (const auto &item : this->items_)
    {
        if (this->isItemVisible(item))
        {
            ++i;
        }
    }
    return i;
}

void Notebook::select(QWidget *page, bool focusPage)
{
    if (page == this->selectedPage_)
    {
        // Nothing has changed
        return;
    }

    auto *oldPage = this->selectedPage_;
    auto *oldItem = oldPage ? this->findItem(oldPage) : nullptr;
    auto *newItem = page ? this->findItem(page) : nullptr;

    if (page)
    {
        // A new page has been selected, mark it as selected & focus one of its splits
        if (!newItem)
        {
            return;
        }
    }

    if (oldItem)
    {
        // Hide the previously selected page
        oldPage->hide();
        oldItem->tab->setSelected(false);
        oldItem->selectedWidget = oldPage->focusWidget();
    }

    if (page)
    {
        page->show();

        newItem->tab->setSelected(true);
        newItem->tab->raise();

        if (focusPage)
        {
            if (newItem->selectedWidget == nullptr)
            {
                newItem->page->setFocus();
            }
            else
            {
                if (containsChild(page, newItem->selectedWidget))
                {
                    newItem->selectedWidget->setFocus(Qt::MouseFocusReason);
                }
                else
                {
                    qCDebug(chatterinoWidget) << "Notebook: selected child of "
                                                 "page doesn't exist anymore";
                }
            }
        }
    }

    this->selectedPage_ = page;

    this->performLayout();
    this->updateTabVisibility();
}

bool Notebook::containsPage(QWidget *page)
{
    return std::any_of(this->items_.begin(), this->items_.end(),
                       [page](const auto &item) {
                           return item.page == page;
                       });
}

Notebook::Item *Notebook::findItem(QWidget *page)
{
    auto it = std::find_if(this->items_.begin(), this->items_.end(),
                           [page](const auto &item) {
                               return page == item.page;
                           });
    if (it != this->items_.end())
    {
        return &(*it);
    }
    return nullptr;
}

const Notebook::Item *Notebook::findItem(QWidget *page) const
{
    auto it = std::find_if(this->items_.begin(), this->items_.end(),
                           [page](const auto &item) {
                               return page == item.page;
                           });
    if (it != this->items_.end())
    {
        return &(*it);
    }
    return nullptr;
}

Notebook::Folder *Notebook::findFolder(const QString &folderId)
{
    auto it = std::ranges::find_if(this->folders_, [&](const Folder &folder) {
        return folder.id == folderId;
    });
    if (it == this->folders_.end())
    {
        return nullptr;
    }
    return &(*it);
}

const Notebook::Folder *Notebook::findFolder(const QString &folderId) const
{
    auto it = std::ranges::find_if(this->folders_, [&](const Folder &folder) {
        return folder.id == folderId;
    });
    if (it == this->folders_.end())
    {
        return nullptr;
    }
    return &(*it);
}

Notebook::Folder *Notebook::ensureFolder(const QString &folderId,
                                         const QString &title, bool expanded)
{
    if (folderId.isEmpty())
    {
        return nullptr;
    }

    if (auto *folder = this->findFolder(folderId))
    {
        return folder;
    }

    return this->createFolder(title.isEmpty() ? QStringLiteral("Folder") : title,
                              folderId, expanded, false);
}

Notebook::Folder *Notebook::createFolder(const QString &title,
                                         const QString &id, bool expanded,
                                         bool queueSave)
{
    if (id.isEmpty())
    {
        return nullptr;
    }

    if (auto *existing = this->findFolder(id))
    {
        existing->title = title;
        existing->expanded = expanded;
        existing->tab->setTitle(title);
        existing->tab->setExpanded(expanded, false);
        return existing;
    }

    Folder folder;
    folder.id = id;
    folder.title = title.isEmpty() ? QStringLiteral("Folder") : title;
    folder.expanded = expanded;
    folder.tab = new NotebookFolderTab(this, folder.id);
    folder.tab->setTitle(folder.title);
    folder.tab->setExpanded(folder.expanded, false);
    folder.tab->setTabLocation(this->tabLocation_);
    folder.tab->setVisible(this->showTabs_);

    this->folders_.push_back(folder);

    if (queueSave)
    {
        getApp()->getWindows()->queueSave();
        this->performLayout(true);
        this->updateTabVisibility();
    }

    return &this->folders_.back();
}

void Notebook::showAddFolderDialog()
{
    bool ok = false;
    const auto title = QInputDialog::getText(
        this, QStringLiteral("Add Tab Folder"),
        QStringLiteral("Folder name:"), QLineEdit::Normal,
        this->nextFolderTitle(), &ok);
    if (!ok)
    {
        return;
    }

    const auto trimmed = title.trimmed();
    this->createFolder(trimmed.isEmpty() ? this->nextFolderTitle() : trimmed,
                       QUuid::createUuid().toString(QUuid::WithoutBraces),
                       true, true);
}

void Notebook::showRenameFolderDialog(const QString &folderId)
{
    auto *folder = this->findFolder(folderId);
    if (folder == nullptr)
    {
        return;
    }

    bool ok = false;
    const auto title = QInputDialog::getText(
        this, QStringLiteral("Rename Tab Folder"),
        QStringLiteral("Folder name:"), QLineEdit::Normal, folder->title, &ok);
    if (!ok)
    {
        return;
    }

    const auto trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed == folder->title)
    {
        return;
    }

    folder->title = trimmed;
    folder->tab->setTitle(trimmed);
    getApp()->getWindows()->queueSave();
    this->performLayout(true);
}

void Notebook::showFolderContextMenu(const QString &folderId, QPoint globalPos)
{
    auto *folder = this->findFolder(folderId);
    if (folder == nullptr)
    {
        return;
    }

    QMenu menu(this);
    menu.addAction(QStringLiteral("New Tab in Folder"), [this, folderId] {
        this->createPageInFolder(folderId);
    });
    menu.addAction(QStringLiteral("Rename Folder"), [this, folderId] {
        this->showRenameFolderDialog(folderId);
    });
    menu.addAction(folder->expanded ? QStringLiteral("Collapse Folder")
                                    : QStringLiteral("Expand Folder"),
                   [this, folderId] {
                       this->toggleFolderExpanded(folderId);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Delete tab"), [this, folderId] {
        this->removeFolder(folderId);
    });
    menu.exec(globalPos);
}

void Notebook::toggleFolderExpanded(const QString &folderId)
{
    auto *folder = this->findFolder(folderId);
    if (folder == nullptr)
    {
        return;
    }

    folder->expanded = !folder->expanded;
    folder->tab->setExpanded(folder->expanded, true);

    getApp()->getWindows()->queueSave();
    this->performLayout(true);
    this->updateTabVisibility();
}

void Notebook::removeFolder(const QString &folderId)
{
    auto folderIt =
        std::ranges::find_if(this->folders_, [&](const Folder &folder) {
            return folder.id == folderId;
        });
    if (folderIt == this->folders_.end())
    {
        return;
    }

    for (auto &item : this->items_)
    {
        if (item.folderId == folderId)
        {
            item.folderId.clear();
        }
    }

    folderIt->tab->deleteLater();
    this->folders_.erase(folderIt);

    getApp()->getWindows()->queueSave();
    this->performLayout(true);
    this->updateTabVisibility();
}

bool Notebook::folderContainsSelectedPage(const QString &folderId) const
{
    return std::ranges::any_of(this->items_, [&](const Item &item) {
        return item.folderId == folderId && item.page == this->selectedPage_;
    });
}

bool Notebook::folderContainsBulkSelectedPage(const QString &folderId) const
{
    return std::ranges::any_of(this->items_, [&](const Item &item) {
        return item.folderId == folderId && item.tab->isBulkSelected();
    });
}

int Notebook::folderPageCount(const QString &folderId) const
{
    return static_cast<int>(
        std::ranges::count_if(this->items_, [&](const Item &item) {
            return item.folderId == folderId;
        }));
}

QString Notebook::nextFolderTitle() const
{
    for (int i = 1;; ++i)
    {
        const auto title = QStringLiteral("Folder %1").arg(i);
        const bool exists =
            std::ranges::any_of(this->folders_, [&](const Folder &folder) {
                return folder.title.compare(title, Qt::CaseInsensitive) == 0;
            });
        if (!exists)
        {
            return title;
        }
    }
}

bool Notebook::isFolderDropPreviewItem(const Item &item) const
{
    if (this->folderDropPreviewPage_ == nullptr)
    {
        return false;
    }

    if (item.page == this->folderDropPreviewPage_)
    {
        return true;
    }

    if (item.tab == nullptr || !item.tab->isBulkSelected())
    {
        return false;
    }

    return std::ranges::any_of(this->bulkSelectedTabs_, [this](const auto *tab) {
        return tab != nullptr && tab->page == this->folderDropPreviewPage_;
    });
}

bool Notebook::isItemVisible(const Item &item) const
{
    if (this->tabVisibilityFilter_ && !this->tabVisibilityFilter_(item.tab))
    {
        return false;
    }

    if (item.folderId.isEmpty())
    {
        return true;
    }

    const auto *folder = this->findFolder(item.folderId);
    if (folder == nullptr || folder->expanded)
    {
        return true;
    }

    return item.tab->isSelected() || item.tab->isBulkSelected();
}

std::vector<Notebook::LayoutEntry> Notebook::buildVisibleLayoutEntries()
{
    std::vector<LayoutEntry> entries;
    if (!this->showTabs_)
    {
        return entries;
    }

    entries.reserve(this->items_.size() + this->folders_.size());
    QSet<QString> emittedFolders;

    for (auto &item : this->items_)
    {
        if (this->isFolderDropPreviewItem(item))
        {
            continue;
        }

        if (!item.folderId.isEmpty())
        {
            if (auto *folder = this->findFolder(item.folderId))
            {
                if (!emittedFolders.contains(folder->id))
                {
                    entries.push_back(LayoutEntry{nullptr, folder});
                    emittedFolders.insert(folder->id);
                }

                if (this->isItemVisible(item))
                {
                    entries.push_back(LayoutEntry{&item, nullptr});
                }
                continue;
            }
        }

        if (this->isItemVisible(item))
        {
            entries.push_back(LayoutEntry{&item, nullptr});
        }
    }

    for (auto &folder : this->folders_)
    {
        if (!emittedFolders.contains(folder.id))
        {
            entries.push_back(LayoutEntry{nullptr, &folder});
        }
    }

    return entries;
}

void Notebook::updateFolderGroupRects()
{
    this->folderGroupRects_.clear();
    if (!this->showTabs_)
    {
        return;
    }

    for (const auto &folder : this->folders_)
    {
        if (folder.tab == nullptr || !folder.tab->isVisible())
        {
            continue;
        }

        QRect groupRect = folder.tab->getDesiredRect();
        for (const auto &item : this->items_)
        {
            if (item.folderId == folder.id &&
                !this->isFolderDropPreviewItem(item) &&
                this->isItemVisible(item))
            {
                groupRect = groupRect.united(item.tab->getDesiredRect());
            }
        }

        if (groupRect.isValid())
        {
            this->folderGroupRects_.push_back(groupRect);
        }
    }
}

int Notebook::lastIndexInFolder(const QString &folderId) const
{
    for (int i = this->items_.count() - 1; i >= 0; --i)
    {
        if (this->items_[i].folderId == folderId)
        {
            return i;
        }
    }

    return -1;
}

bool Notebook::containsChild(const QObject *obj, const QObject *child)
{
    return std::any_of(obj->children().begin(), obj->children().end(),
                       [child](const QObject *o) {
                           if (o == child)
                           {
                               return true;
                           }

                           return containsChild(o, child);
                       });
}

void Notebook::selectIndex(int index, bool focusPage)
{
    if (index < 0 || this->items_.count() <= index)
    {
        return;
    }

    this->select(this->items_[index].page, focusPage);
}

void Notebook::selectVisibleIndex(int index, bool focusPage)
{
    int i = 0;
    for (auto &item : this->items_)
    {
        if (this->isItemVisible(item))
        {
            if (i == index)
            {
                // found the index'th visible page
                this->select(item.page, focusPage);
                return;
            }
            ++i;
        }
    }
}

void Notebook::selectNextTab(bool focusPage)
{
    const int size = this->items_.size();
    if (size == 0)
    {
        return;
    }

    const int startIndex = this->indexOf(this->selectedPage_);

    if (size == 1)
    {
        if (startIndex == -1 && this->isItemVisible(this->items_[0]))
        {
            this->select(this->items_[0].page, focusPage);
        }
        return;
    }

    if (startIndex == -1)
    {
        this->selectVisibleIndex(0, focusPage);
        return;
    }

    // find next tab that is permitted by filter
    auto index = (startIndex + 1) % size;
    while (index != startIndex)
    {
        if (this->isItemVisible(this->items_[index]))
        {
            this->select(this->items_[index].page, focusPage);
            return;
        }
        index = (index + 1) % size;
    }
}

void Notebook::selectPreviousTab(bool focusPage)
{
    const int size = this->items_.size();
    if (size == 0)
    {
        return;
    }

    const int startIndex = this->indexOf(this->selectedPage_);

    if (size == 1)
    {
        if (startIndex == -1 && this->isItemVisible(this->items_[0]))
        {
            this->select(this->items_[0].page, focusPage);
        }
        return;
    }

    if (startIndex == -1)
    {
        this->selectLastTab(focusPage);
        return;
    }

    // find next previous tab that is permitted by filter
    auto index = startIndex == 0 ? size - 1 : startIndex - 1;
    while (index != startIndex)
    {
        if (this->isItemVisible(this->items_[index]))
        {
            this->select(this->items_[index].page, focusPage);
            return;
        }

        index = index == 0 ? size - 1 : index - 1;
    }
}

void Notebook::selectLastTab(bool focusPage)
{
    // find first tab permitted by filter starting from the end
    for (auto it = this->items_.rbegin(); it != this->items_.rend(); ++it)
    {
        if (this->isItemVisible(*it))
        {
            this->select(it->page, focusPage);
            return;
        }
    }
}

int Notebook::getPageCount() const
{
    return this->items_.count();
}

QWidget *Notebook::getPageAt(int index) const
{
    return this->items_[index].page;
}

int Notebook::getSelectedIndex() const
{
    return this->indexOf(this->selectedPage_);
}

QWidget *Notebook::getSelectedPage() const
{
    if (this->selectedPage_ == nullptr ||
        this->indexOf(this->selectedPage_) == -1)
    {
        return nullptr;
    }

    return this->selectedPage_;
}

QWidget *Notebook::tabAt(QPoint point, int &index, int maxWidth)
{
    auto i = 0;

    for (auto &item : this->items_)
    {
        if (this->isFolderDropPreviewItem(item) || !item.tab->isVisible())
        {
            i++;
            continue;
        }

        auto rect = item.tab->getDesiredRect();
        rect.setHeight(int(this->scale() * 24));

        rect.setWidth(std::min(maxWidth, rect.width()));

        if (rect.contains(point))
        {
            index = i;
            return item.page;
        }

        i++;
    }

    index = -1;
    return nullptr;
}

bool Notebook::isFolderHeaderAt(QPoint point) const
{
    return std::ranges::any_of(this->folders_, [&](const Folder &folder) {
        return folder.tab != nullptr && folder.tab->isVisible() &&
               folder.tab->getDesiredRect().contains(point);
    });
}

bool Notebook::movePageIntoFolderAt(QWidget *page, QPoint point)
{
    if (this->isNotebookLayoutLocked())
    {
        return false;
    }

    Folder *targetFolder = nullptr;
    for (auto &folder : this->folders_)
    {
        if (folder.tab != nullptr && folder.tab->isVisible() &&
            folder.tab->getDesiredRect().contains(point))
        {
            targetFolder = &folder;
            break;
        }
    }

    if (targetFolder == nullptr)
    {
        return false;
    }

    const auto draggedIndex = this->indexOf(page);
    if (draggedIndex == -1)
    {
        return false;
    }

    std::vector<int> movingIndices;
    if (this->items_[draggedIndex].tab->isBulkSelected())
    {
        for (int i = 0; i < this->items_.size(); ++i)
        {
            if (this->items_[i].tab->isBulkSelected())
            {
                movingIndices.push_back(i);
            }
        }
    }

    if (movingIndices.empty())
    {
        movingIndices.push_back(draggedIndex);
    }

    const bool alreadyInTarget =
        std::ranges::all_of(movingIndices, [&](int index) {
            return this->items_[index].folderId == targetFolder->id;
    });
    if (alreadyInTarget)
    {
        this->performLayout(true);
        this->updateTabVisibility();
        return true;
    }

    QList<Item> movingItems;
    movingItems.reserve(static_cast<int>(movingIndices.size()));
    for (const auto index : movingIndices)
    {
        auto item = this->items_[index];
        item.folderId = targetFolder->id;
        movingItems.push_back(item);
    }

    for (auto it = movingIndices.rbegin(); it != movingIndices.rend(); ++it)
    {
        this->items_.removeAt(*it);
    }

    auto insertionIndex = this->lastIndexInFolder(targetFolder->id);
    if (insertionIndex == -1)
    {
        insertionIndex = this->items_.count();
    }
    else
    {
        insertionIndex += 1;
    }

    for (const auto &item : movingItems)
    {
        this->items_.insert(insertionIndex, item);
        ++insertionIndex;
    }

    getApp()->getWindows()->queueSave();
    this->performLayout(true);
    this->updateTabVisibility();
    return true;
}

void Notebook::setFolderDropPreviewPage(QWidget *page)
{
    if (page != nullptr && this->indexOf(page) == -1)
    {
        page = nullptr;
    }

    if (this->folderDropPreviewPage_ == page)
    {
        return;
    }

    this->folderDropPreviewPage_ = page;
    this->clearFloatingTabDropPreview();
    this->performLayout(true);
    this->update();
}

bool Notebook::hasTabFolders() const
{
    return !this->folders_.empty();
}

bool Notebook::beginFloatingTabDrag(QWidget *page)
{
    if (this->isNotebookLayoutLocked() || !this->hasTabFolders() ||
        this->indexOf(page) == -1)
    {
        return false;
    }

    this->setFolderDropPreviewPage(page);

    if (auto *tab = this->getTabFromPage(page))
    {
        tab->raise();
    }

    return true;
}

void Notebook::updateFloatingTabDrag(QWidget *page, QPoint point)
{
    if (this->folderDropPreviewPage_ != page)
    {
        return;
    }

    this->updateFloatingTabDropPreview(point);
}

bool Notebook::finishFloatingTabDrag(QWidget *page, QPoint point)
{
    if (this->folderDropPreviewPage_ != page)
    {
        return false;
    }

    const auto insertionIndex = this->tabDropPreviewIndex_;
    const auto folderId = this->tabDropPreviewFolderId_;
    const bool dropIntoFolder = this->isFolderHeaderAt(point);

    this->folderDropPreviewPage_ = nullptr;
    this->clearFloatingTabDropPreview();

    if (dropIntoFolder)
    {
        return this->movePageIntoFolderAt(page, point);
    }

    if (insertionIndex != -1)
    {
        this->movePageToPreviewIndex(page, insertionIndex, folderId);
        return true;
    }

    this->performLayout(true);
    return false;
}

void Notebook::cancelFloatingTabDrag(QWidget *page)
{
    if (this->folderDropPreviewPage_ != page)
    {
        return;
    }

    this->folderDropPreviewPage_ = nullptr;
    this->clearFloatingTabDropPreview();
    this->performLayout(true);
}

void Notebook::updateFloatingTabDropPreview(QPoint point)
{
    QRect nextLineRect;
    QRect nextFolderRect;
    int nextInsertionIndex = -1;
    QString nextFolderId;

    for (const auto &folder : this->folders_)
    {
        if (folder.tab == nullptr || !folder.tab->isVisible())
        {
            continue;
        }

        auto rect = folder.tab->getDesiredRect();
        if (rect.contains(point))
        {
            nextFolderRect = rect;
            break;
        }
    }

    if (nextFolderRect.isNull())
    {
        const auto horizontal =
            this->tabLocation_ == NotebookTabLocation::Top ||
            this->tabLocation_ == NotebookTabLocation::Bottom;
        const auto scale = this->scale();
        const int thickness = std::max(2, int(3 * scale));
        const int padding = std::max(3, int(4 * scale));
        int bestDistance = std::numeric_limits<int>::max();

        for (int i = 0; i < this->items_.size(); ++i)
        {
            const auto &item = this->items_[i];
            if (this->isFolderDropPreviewItem(item) ||
                !this->isItemVisible(item) || item.tab == nullptr ||
                !item.tab->isVisible())
            {
                continue;
            }

            const auto rect = item.tab->getDesiredRect();
            if (!rect.isValid())
            {
                continue;
            }

            auto considerCandidate = [&](int boundary, int insertionIndex,
                                         QRect lineRect) {
                int secondaryDistance = 0;
                int primaryDistance = 0;
                if (horizontal)
                {
                    primaryDistance = std::abs(point.x() - boundary);
                    if (point.y() < rect.top())
                    {
                        secondaryDistance = rect.top() - point.y();
                    }
                    else if (point.y() > rect.bottom())
                    {
                        secondaryDistance = point.y() - rect.bottom();
                    }
                }
                else
                {
                    primaryDistance = std::abs(point.y() - boundary);
                    if (point.x() < rect.left())
                    {
                        secondaryDistance = rect.left() - point.x();
                    }
                    else if (point.x() > rect.right())
                    {
                        secondaryDistance = point.x() - rect.right();
                    }
                }

                if (secondaryDistance > (horizontal ? rect.height()
                                                    : rect.width()))
                {
                    return;
                }

                const int distance = primaryDistance + secondaryDistance * 4;
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    nextInsertionIndex = insertionIndex;
                    nextFolderId = item.folderId;
                    nextLineRect = lineRect;
                }
            };

            if (horizontal)
            {
                const int top = rect.top() + padding;
                const int height = std::max(6, rect.height() - padding * 2);
                const int before = rect.left();
                const int after = rect.right() + 1;
                considerCandidate(before, i,
                                  QRect(before - thickness / 2, top,
                                        thickness, height));
                considerCandidate(after, i + 1,
                                  QRect(after - thickness / 2, top, thickness,
                                        height));
            }
            else
            {
                const int left = rect.left() + padding;
                const int width = std::max(6, rect.width() - padding * 2);
                const int before = rect.top();
                const int after = rect.bottom() + 1;
                considerCandidate(before, i,
                                  QRect(left, before - thickness / 2, width,
                                        thickness));
                considerCandidate(after, i + 1,
                                  QRect(left, after - thickness / 2, width,
                                        thickness));
            }
        }
    }

    if (this->tabDropPreviewIndex_ == nextInsertionIndex &&
        this->tabDropPreviewRect_ == nextLineRect &&
        this->tabDropPreviewFolderId_ == nextFolderId &&
        this->folderDropTargetRect_ == nextFolderRect)
    {
        return;
    }

    this->tabDropPreviewIndex_ = nextInsertionIndex;
    this->tabDropPreviewRect_ = nextLineRect;
    this->tabDropPreviewFolderId_ = nextFolderId;
    this->folderDropTargetRect_ = nextFolderRect;
    this->update();
}

void Notebook::clearFloatingTabDropPreview()
{
    const bool hadPreview = this->tabDropPreviewIndex_ != -1 ||
                            !this->tabDropPreviewRect_.isNull() ||
                            !this->folderDropTargetRect_.isNull() ||
                            !this->tabDropPreviewFolderId_.isEmpty();

    this->tabDropPreviewIndex_ = -1;
    this->tabDropPreviewRect_ = QRect();
    this->folderDropTargetRect_ = QRect();
    this->tabDropPreviewFolderId_.clear();

    if (hadPreview)
    {
        this->update();
    }
}

void Notebook::movePageToPreviewIndex(QWidget *page, int insertionIndex,
                                      const QString &folderId)
{
    if (this->isNotebookLayoutLocked())
    {
        return;
    }

    const int draggedIndex = this->indexOf(page);
    if (draggedIndex == -1)
    {
        return;
    }

    std::vector<int> movingIndices;
    if (this->items_[draggedIndex].tab->isBulkSelected())
    {
        for (int i = 0; i < this->items_.size(); ++i)
        {
            if (this->items_[i].tab->isBulkSelected())
            {
                movingIndices.push_back(i);
            }
        }
    }

    if (movingIndices.empty())
    {
        movingIndices.push_back(draggedIndex);
    }

    QList<Item> movingItems;
    movingItems.reserve(static_cast<int>(movingIndices.size()));
    for (const auto index : movingIndices)
    {
        auto item = this->items_[index];
        item.folderId = folderId;
        movingItems.push_back(item);
    }

    int adjustedInsertionIndex = insertionIndex;
    for (const auto index : movingIndices)
    {
        if (index < adjustedInsertionIndex)
        {
            --adjustedInsertionIndex;
        }
    }

    for (auto it = movingIndices.rbegin(); it != movingIndices.rend(); ++it)
    {
        this->items_.removeAt(*it);
    }

    adjustedInsertionIndex = std::clamp(
        adjustedInsertionIndex, 0, static_cast<int>(this->items_.size()));

    for (const auto &item : movingItems)
    {
        this->items_.insert(adjustedInsertionIndex, item);
        ++adjustedInsertionIndex;
    }

    getApp()->getWindows()->queueSave();
    this->performLayout(true);
    this->updateTabVisibility();
}

void Notebook::rearrangePage(QWidget *page, int index)
{
    if (this->isNotebookLayoutLocked())
    {
        return;
    }

    const int currentIndex = this->indexOf(page);
    if (currentIndex == -1 || index < 0 || index >= this->items_.size() ||
        currentIndex == index)
    {
        return;
    }

    const auto targetFolderId = this->items_[index].folderId;

    // Queue up save because: Tab rearranged
    getApp()->getWindows()->queueSave();

    this->items_.move(currentIndex, index);
    if (auto *item = this->findItem(page))
    {
        item->folderId = targetFolderId;
    }

    this->performLayout(true);
}

bool Notebook::tryRearrangeBulkSelectedTabs(QWidget *draggedPage,
                                            int targetIndex)
{
    if (this->isNotebookLayoutLocked())
    {
        return true;
    }

    const int draggedIndex = this->indexOf(draggedPage);
    if (draggedIndex == -1 || targetIndex < 0 ||
        targetIndex >= this->items_.size())
    {
        return false;
    }

    auto *draggedTab = this->items_[draggedIndex].tab;
    if (draggedTab == nullptr || !draggedTab->isBulkSelected())
    {
        return false;
    }

    std::vector<int> selectedIndices;
    selectedIndices.reserve(this->bulkSelectedTabs_.size());
    for (int i = 0; i < this->items_.size(); ++i)
    {
        if (this->items_[i].tab->isBulkSelected())
        {
            selectedIndices.push_back(i);
        }
    }

    if (selectedIndices.size() <= 1)
    {
        return false;
    }

    const int firstSelectedIndex = selectedIndices.front();
    const int lastSelectedIndex = selectedIndices.back();
    if (draggedIndex < firstSelectedIndex || draggedIndex > lastSelectedIndex)
    {
        return false;
    }

    for (size_t i = 1; i < selectedIndices.size(); ++i)
    {
        if (selectedIndices[i] != selectedIndices[i - 1] + 1)
        {
            return false;
        }
    }

    if (targetIndex >= firstSelectedIndex && targetIndex <= lastSelectedIndex)
    {
        return true;
    }

    const auto targetFolderId = this->items_[targetIndex].folderId;
    const auto blockSize = static_cast<int>(selectedIndices.size());
    QList<Item> movedItems;
    movedItems.reserve(blockSize);
    for (int i = 0; i < blockSize; ++i)
    {
        movedItems.push_back(this->items_[firstSelectedIndex + i]);
    }

    for (int i = 0; i < blockSize; ++i)
    {
        this->items_.removeAt(firstSelectedIndex);
    }

    int insertionIndex = targetIndex;
    if (targetIndex > lastSelectedIndex)
    {
        insertionIndex = targetIndex - blockSize + 1;
    }

    for (auto &item : movedItems)
    {
        item.folderId = targetFolderId;
    }

    for (const auto &item : movedItems)
    {
        this->items_.insert(insertionIndex, item);
        ++insertionIndex;
    }

    // Queue up save because: Tabs rearranged
    getApp()->getWindows()->queueSave();

    this->performLayout(true);
    return true;
}

void Notebook::setBulkSelectedTab(NotebookTab *tab, bool value)
{
    if (tab == nullptr || this->indexOf(tab->page) == -1)
    {
        return;
    }

    const auto it = std::ranges::find(this->bulkSelectedTabs_, tab);
    const auto isSelected = it != this->bulkSelectedTabs_.end();
    if (isSelected == value)
    {
        return;
    }

    if (value)
    {
        this->bulkSelectedTabs_.push_back(tab);
    }
    else
    {
        this->bulkSelectedTabs_.erase(it);
    }

    tab->setBulkSelected(value);
    this->bulkSelectionAnchorTab_ = tab;
    this->updateSelectedTabVisualState();
    this->bulkSelectionChanged.invoke();
}

void Notebook::toggleBulkSelectedTab(NotebookTab *tab)
{
    if (tab == nullptr)
    {
        return;
    }

    if (this->bulkSelectedTabs_.empty())
    {
        if (auto *selectedPage = this->getSelectedPage())
        {
            if (auto *selectedTab = this->getTabFromPage(selectedPage);
                selectedTab != nullptr && selectedTab != tab)
            {
                this->setBulkSelectedTab(selectedTab, true);
            }
        }
    }

    this->setBulkSelectedTab(tab, !tab->isBulkSelected());
}

NotebookTab *Notebook::bulkRangeAnchor()
{
    if (this->bulkSelectionAnchorTab_ != nullptr &&
        this->indexOf(this->bulkSelectionAnchorTab_->page) != -1)
    {
        return this->bulkSelectionAnchorTab_;
    }

    if (auto *selectedPage = this->getSelectedPage())
    {
        if (auto *selectedTab = this->getTabFromPage(selectedPage))
        {
            return selectedTab;
        }
    }

    return nullptr;
}

void Notebook::updateSelectedTabVisualState()
{
    if (auto *selectedPage = this->getSelectedPage())
    {
        if (auto *selectedTab = this->getTabFromPage(selectedPage))
        {
            selectedTab->updateVisualState();
        }
    }
}

void Notebook::selectBulkRangeTo(NotebookTab *tab, bool keepExisting)
{
    if (tab == nullptr || this->indexOf(tab->page) == -1)
    {
        return;
    }

    auto *anchor = this->bulkRangeAnchor();
    if (anchor == nullptr || this->indexOf(anchor->page) == -1)
    {
        anchor = tab;
    }

    std::vector<NotebookTab *> visibleTabs;
    visibleTabs.reserve(this->items_.size());
    for (const auto &item : this->items_)
    {
        if (item.tab->isVisible())
        {
            visibleTabs.push_back(item.tab);
        }
    }

    auto anchorIt = std::ranges::find(visibleTabs, anchor);
    auto tabIt = std::ranges::find(visibleTabs, tab);
    if (anchorIt == visibleTabs.end() || tabIt == visibleTabs.end())
    {
        anchor = tab;
        anchorIt = std::ranges::find(visibleTabs, anchor);
        tabIt = std::ranges::find(visibleTabs, tab);
    }

    if (anchorIt == visibleTabs.end() || tabIt == visibleTabs.end())
    {
        return;
    }

    if (!keepExisting)
    {
        this->clearBulkSelectedTabs();
    }

    if (tabIt < anchorIt)
    {
        std::swap(anchorIt, tabIt);
    }

    for (auto it = anchorIt; it <= tabIt; ++it)
    {
        this->setBulkSelectedTab(*it, true);
    }

    this->bulkSelectionAnchorTab_ = tab;
}

void Notebook::clearBulkSelectedTabs()
{
    if (this->bulkSelectedTabs_.empty())
    {
        return;
    }

    const auto selectedTabs = this->bulkSelectedTabs_;
    this->bulkSelectedTabs_.clear();
    for (auto *tab : selectedTabs)
    {
        if (tab)
        {
            tab->setBulkSelected(false);
        }
    }

    this->bulkSelectionAnchorTab_ = nullptr;
    this->updateSelectedTabVisualState();
    this->bulkSelectionChanged.invoke();
}

void Notebook::removeBulkSelectedTabs()
{
    if (this->bulkSelectedTabs_.empty())
    {
        return;
    }

    std::vector<QWidget *> pages;
    pages.reserve(this->bulkSelectedTabs_.size());
    for (auto *tab : this->bulkSelectedTabs_)
    {
        if (tab != nullptr && tab->page != nullptr &&
            this->indexOf(tab->page) != -1)
        {
            pages.push_back(tab->page);
        }
    }

    if (pages.empty())
    {
        this->clearBulkSelectedTabs();
        return;
    }

    const auto count = static_cast<int>(pages.size());
    const auto reply = QMessageBox::question(
        this, "Delete selected tabs",
        count == 1 ? QStringLiteral("Delete 1 selected tab?")
                   : QStringLiteral("Delete %1 selected tabs?").arg(count),
        QMessageBox::Yes | QMessageBox::Cancel);
    if (reply != QMessageBox::Yes)
    {
        return;
    }

    for (auto *page : pages)
    {
        if (this->indexOf(page) != -1)
        {
            this->removePage(page);
        }
    }

    this->bulkSelectionAnchorTab_ = nullptr;
    this->clearBulkSelectedTabs();
}

int Notebook::bulkSelectedTabCount() const
{
    return static_cast<int>(this->bulkSelectedTabs_.size());
}

bool Notebook::getAllowUserTabManagement() const
{
    return this->allowUserTabManagement_;
}

void Notebook::setAllowUserTabManagement(bool value)
{
    this->allowUserTabManagement_ = value;
}

std::vector<TabFolderState> Notebook::getTabFolderStates() const
{
    std::vector<TabFolderState> states;
    states.reserve(this->folders_.size());
    for (const auto &folder : this->folders_)
    {
        states.push_back(TabFolderState{
            .id = folder.id,
            .title = folder.title,
            .expanded = folder.expanded,
        });
    }
    return states;
}

QString Notebook::folderIdOfPage(QWidget *page) const
{
    if (const auto *item = this->findItem(page))
    {
        return item->folderId;
    }
    return {};
}

void Notebook::restoreTabFolder(const QString &id, const QString &title,
                                bool expanded)
{
    if (id.isEmpty())
    {
        return;
    }

    if (auto *folder = this->findFolder(id))
    {
        folder->title = title;
        folder->expanded = expanded;
        folder->tab->setTitle(title);
        folder->tab->setExpanded(expanded, false);
        return;
    }

    this->createFolder(title, id, expanded, false);
}

void Notebook::setPageFolder(QWidget *page, const QString &folderId,
                             bool animated)
{
    const auto currentIndex = this->indexOf(page);
    if (currentIndex == -1)
    {
        return;
    }

    auto normalizedFolderId = folderId;
    if (!normalizedFolderId.isEmpty())
    {
        auto *folder = this->ensureFolder(normalizedFolderId,
                                          this->nextFolderTitle(), true);
        if (folder == nullptr)
        {
            normalizedFolderId.clear();
        }
        else
        {
            normalizedFolderId = folder->id;
        }
    }

    if (this->items_[currentIndex].folderId == normalizedFolderId)
    {
        return;
    }

    auto item = this->items_[currentIndex];
    item.folderId = normalizedFolderId;
    this->items_.removeAt(currentIndex);

    if (normalizedFolderId.isEmpty())
    {
        this->items_.insert(
            std::min(currentIndex, static_cast<int>(this->items_.count())),
            item);
    }
    else
    {
        auto insertIndex = this->lastIndexInFolder(normalizedFolderId);
        if (insertIndex == -1)
        {
            insertIndex = this->items_.count();
        }
        else
        {
            insertIndex += 1;
        }
        this->items_.insert(insertIndex, item);
    }

    getApp()->getWindows()->queueSave();
    this->performLayout(animated);
    this->updateTabVisibility();
}

void Notebook::createPageInFolder(const QString &)
{
}

bool Notebook::getShowTabs() const
{
    return this->showTabs_;
}

void Notebook::setShowTabs(bool value)
{
    this->showTabs_ = value;

    this->setShowAddButton(value);
    this->performLayout();

    this->updateTabVisibility();

    // show a popup upon hiding tabs
    if (!value && getSettings()->informOnTabVisibilityToggle.getValue())
    {
        this->showTabVisibilityInfoPopup();
    }
}

void Notebook::showTabVisibilityInfoPopup()
{
    auto unhideSeq = getApp()->getHotkeys()->getDisplaySequence(
        HotkeyCategory::Window, "setTabVisibility", {std::vector<QString>()});
    if (unhideSeq.isEmpty())
    {
        unhideSeq = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"toggle"}});
    }
    if (unhideSeq.isEmpty())
    {
        unhideSeq = getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"on"}});
    }
    QString hotkeyInfo = "(currently unbound)";
    if (!unhideSeq.isEmpty())
    {
        hotkeyInfo =
            "(" + unhideSeq.toString(QKeySequence::SequenceFormat::NativeText) +
            ")";
    }
    QMessageBox msgBox(this->window());
    msgBox.window()->setWindowTitle("Mergerino - hidden tabs");
    msgBox.setText("You've just hidden your tabs.");
    msgBox.setInformativeText(
        "You can toggle tabs by using the keyboard shortcut " + hotkeyInfo +
        " or right-clicking the tab area and selecting \"Toggle "
        "visibility of tabs\".");
    msgBox.addButton(QMessageBox::Ok);
    auto *dsaButton =
        msgBox.addButton("Don't show again", QMessageBox::YesRole);

    msgBox.setDefaultButton(QMessageBox::Ok);

    msgBox.exec();

    if (msgBox.clickedButton() == dsaButton)
    {
        getSettings()->informOnTabVisibilityToggle.setValue(false);
    }
}

void Notebook::refresh()
{
    if (this->refreshPaused_)
    {
        this->refreshRequested_ = true;
        return;
    }

    this->performLayout();
    this->updateTabVisibility();
}

void Notebook::updateTabVisibility()
{
    for (auto &item : this->items_)
    {
        item.tab->setVisible(this->shouldShowTab(item.tab));
    }

    for (auto &folder : this->folders_)
    {
        folder.tab->setVisible(this->showTabs_);
    }
}

bool Notebook::getShowAddButton() const
{
    return this->showAddButton_;
}

void Notebook::setShowAddButton(bool value)
{
    this->showAddButton_ = value;

    this->addButton_->setHidden(!value);

    this->refresh();
}

void Notebook::resizeAddButton()
{
    int h = static_cast<int>((NOTEBOOK_TAB_HEIGHT - 1) * this->scale());
    this->addButton_->setFixedSize(h, h);
}

void Notebook::scaleChangedEvent(float /*scale*/)
{
    this->resizeAddButton();
    this->refreshPaused_ = true;
    this->refreshRequested_ = false;
    for (auto &i : this->items_)
    {
        i.tab->updateSize();
    }
    for (auto &folder : this->folders_)
    {
        folder.tab->updateSize();
    }
    this->refreshPaused_ = false;
    if (this->refreshRequested_)
    {
        this->refresh();
    }
}

void Notebook::resizeEvent(QResizeEvent *)
{
    this->performLayout();
}

void Notebook::performLayout(bool animated)
{
    auto entries = this->buildVisibleLayoutEntries();

    const auto scale = this->scale();
    const auto tabHeight = int(NOTEBOOK_TAB_HEIGHT * scale);
    const LayoutContext ctx{
        .left = static_cast<int>(2 * this->scale()),
        .right = this->width(),
        .bottom = this->height(),
        .scale = scale,
        .tabHeight = tabHeight,
        .minimumTabAreaSpace = static_cast<int>(tabHeight * 0.5),
        .addButtonWidth = this->showAddButton_ ? tabHeight : 0,
        .lineThickness = static_cast<int>(2 * scale),
        .tabSpacer = std::max(1, static_cast<int>(scale)),
        .buttonWidth = tabHeight,
        .buttonHeight = tabHeight - 1,
        .entries = std::span<LayoutEntry>(entries),
    };

    if (this->tabLocation_ == NotebookTabLocation::Top ||
        this->tabLocation_ == NotebookTabLocation::Bottom)
    {
        this->performHorizontalLayout(ctx, animated);
    }
    else
    {
        this->performVerticalLayout(ctx, animated);
    }

    if (this->showTabs_)
    {
        // raise elements
        for (auto &i : this->items_)
        {
            i.tab->raise();
        }

        for (auto &folder : this->folders_)
        {
            folder.tab->raise();
        }

        if (this->showAddButton_)
        {
            this->addButton_->raise();
        }
    }

    this->updateFolderGroupRects();
}

void Notebook::performHorizontalLayout(const LayoutContext &ctx, bool animated)
{
    const auto isBottom = this->tabLocation_ == NotebookTabLocation::Bottom;
    const auto reverse = isBottom ? -1 : 1;

    auto x = ctx.left;
    auto y = isBottom ? ctx.bottom - ctx.tabHeight - ctx.tabSpacer : 0;
    auto consumedButtonHeights = 0;

    // set size of custom buttons (settings, user, ...)
    for (auto *btn : this->customButtons_)
    {
        // We use isHidden here since the layout can happen when the button has
        // been added but before it's shown
        if (btn->isHidden())
        {
            continue;
        }

        btn->setFixedSize(ctx.buttonWidth, ctx.buttonHeight);
        btn->move(x, y);
        x += ctx.buttonWidth;

        consumedButtonHeights = ctx.tabHeight;
    }

    if (this->showTabs_)
    {
        auto entryWidth = [](const LayoutEntry &entry) {
            return entry.item != nullptr ? entry.item->tab->width()
                                         : entry.folder->tab->width();
        };
        auto entryHeight = [](const LayoutEntry &entry) {
            return entry.item != nullptr ? entry.item->tab->height()
                                         : entry.folder->tab->height();
        };
        auto growEntry = [](LayoutEntry &entry, int width) {
            if (entry.item != nullptr)
            {
                entry.item->tab->growWidth(width);
            }
            else
            {
                entry.folder->tab->growWidth(width);
            }
        };
        auto moveEntry = [animated](LayoutEntry &entry, QPoint point) {
            if (entry.item != nullptr)
            {
                entry.item->tab->moveAnimated(point, animated);
            }
            else
            {
                entry.folder->tab->moveAnimated(point, animated);
            }
        };
        auto setEntryInLastRow = [](const LayoutEntry &entry, bool value) {
            if (entry.item != nullptr)
            {
                entry.item->tab->setInLastRow(value);
            }
            else
            {
                entry.folder->tab->setInLastRow(value);
            }
        };

        // layout tabs
        /// Notebook tabs need to know if they are in the last row.
        auto *firstInBottomRow =
            ctx.entries.empty() ? nullptr : &ctx.entries.front();

        for (auto &entry : ctx.entries)
        {
            /// Break line if element doesn't fit.
            auto isFirst = &entry == &ctx.entries.front();
            auto isLast = &entry == &ctx.entries.back();

            auto fitsInLine = ((isLast ? ctx.addButtonWidth : 0) + x +
                               entryWidth(entry)) <= this->width();

            if (!isFirst && !fitsInLine)
            {
                y += entryHeight(entry) * reverse;
                x = ctx.left;
                firstInBottomRow = &entry;
            }

            /// Layout tab
            growEntry(entry, 0);
            moveEntry(entry, QPoint(x, y));
            x += entryWidth(entry) + ctx.tabSpacer;
        }

        /// Update which tabs are in the last row
        auto inLastRow = false;
        for (const auto &entry : ctx.entries)
        {
            if (&entry == firstInBottomRow)
            {
                inLastRow = true;
            }
            setEntryInLastRow(entry, inLastRow);
        }

        // move misc buttons
        if (this->showAddButton_)
        {
            this->addButton_->move(x, y);
        }

        if (!isBottom)
        {
            y += ctx.tabHeight;
        }
    }

    if (isBottom)
    {
        int consumedBottomSpace = std::max(
            {ctx.bottom - y, consumedButtonHeights, ctx.minimumTabAreaSpace});
        int tabsStart = ctx.bottom - consumedBottomSpace - ctx.lineThickness;

        if (this->lineOffset_ != tabsStart)
        {
            this->lineOffset_ = tabsStart;
            this->update();
        }

        // set page bounds
        if (auto *selectedPage = this->getSelectedPage())
        {
            selectedPage->move(0, 0);
            selectedPage->resize(this->width(), tabsStart);
            selectedPage->raise();
        }
    }
    else
    {
        y = std::max({y, consumedButtonHeights, ctx.minimumTabAreaSpace});

        if (this->lineOffset_ != y)
        {
            this->lineOffset_ = y;
            this->update();
        }

        /// Increment for the line at the bottom
        y += int(2 * ctx.scale);

        // set page bounds
        if (auto *selectedPage = this->getSelectedPage())
        {
            selectedPage->move(0, y);
            selectedPage->resize(this->width(), this->height() - y);
            selectedPage->raise();
        }
    }
}

void Notebook::performVerticalLayout(const LayoutContext &ctx, bool animated)
{
    int x = 0;
    int y = 0;
    int consumedButtonWidths = 0;

    const bool isRight = this->tabLocation_ == NotebookTabLocation::Right;

    if (isRight)
    {
        x = ctx.right;

        // set size of custom buttons (settings, user, ...)
        for (auto btnIt = this->customButtons_.rbegin();
             btnIt != this->customButtons_.rend(); ++btnIt)
        {
            auto *btn = *btnIt;
            if (btn->isHidden())
            {
                continue;
            }

            x -= ctx.buttonWidth;
            btn->setFixedSize(ctx.buttonWidth, ctx.buttonHeight);
            btn->move(x, y);
        }

        consumedButtonWidths = ctx.right - x;
        x = ctx.right;
    }
    else
    {
        x = ctx.left;

        // set size of custom buttons (settings, user, ...)
        for (auto *btn : this->customButtons_)
        {
            if (btn->isHidden())
            {
                continue;
            }

            btn->setFixedSize(ctx.buttonWidth, ctx.buttonHeight);
            btn->move(x, y);
            x += ctx.buttonWidth;
        }

        consumedButtonWidths = x;
        x = ctx.left;
    }

    if (this->visibleButtonCount() > 0)
    {
        y = ctx.tabHeight + ctx.lineThickness;  // account for divider line
    }

    const int top = y + ctx.tabSpacer;  // add margin

    y = top;

    // zneix: if we were to remove buttons when tabs are hidden
    // stuff below to "set page bounds" part should be in conditional statement
    int tabsPerColumn =
        (this->height() - top) / (ctx.tabHeight + ctx.tabSpacer);
    if (tabsPerColumn == 0)  // window hasn't properly rendered yet
    {
        return;
    }
    int count = static_cast<int>(ctx.entries.size()) +
                (this->showAddButton_ ? 1 : 0);
    int columnCount = ceil((float)count / tabsPerColumn);

    // only add width of all the tabs if they are not hidden
    if (this->showTabs_)
    {
        auto entryNormalWidth = [](const LayoutEntry &entry) {
            return entry.item != nullptr ? entry.item->tab->normalTabWidth()
                                         : entry.folder->tab->normalTabWidth();
        };
        auto growEntry = [](LayoutEntry &entry, int width) {
            if (entry.item != nullptr)
            {
                entry.item->tab->growWidth(width);
            }
            else
            {
                entry.folder->tab->growWidth(width);
            }
        };
        auto moveEntry = [animated](LayoutEntry &entry, QPoint point) {
            if (entry.item != nullptr)
            {
                entry.item->tab->moveAnimated(point, animated);
            }
            else
            {
                entry.folder->tab->moveAnimated(point, animated);
            }
        };
        auto setEntryInLastRow = [](const LayoutEntry &entry, bool value) {
            if (entry.item != nullptr)
            {
                entry.item->tab->setInLastRow(value);
            }
            else
            {
                entry.folder->tab->setInLastRow(value);
            }
        };

        for (int col = 0; col < columnCount; col++)
        {
            bool isLastColumn = col == columnCount - 1;
            auto largestWidth = 0;
            int tabStart = col * tabsPerColumn;
            int tabEnd =
                std::min(static_cast<size_t>((col + 1) * tabsPerColumn),
                         ctx.entries.size());

            for (int i = tabStart; i < tabEnd; i++)
            {
                largestWidth =
                    std::max(entryNormalWidth(ctx.entries[i]), largestWidth);
            }

            if (isLastColumn && this->showAddButton_)
            {
                largestWidth =
                    std::max(largestWidth, this->addButton_->width());
            }

            if (isLastColumn)
            {
                if (isRight)
                {
                    int distanceFromRight = this->width() - x;
                    largestWidth = std::max(
                        largestWidth, consumedButtonWidths - distanceFromRight);
                }
                else
                {
                    largestWidth =
                        std::max(largestWidth, consumedButtonWidths - x);
                }
            }

            if (isRight)
            {
                x -= largestWidth + ctx.lineThickness;
            }

            for (int i = tabStart; i < tabEnd; i++)
            {
                auto entry = ctx.entries[i];

                /// Layout tab
                growEntry(entry, largestWidth);
                moveEntry(entry, QPoint(x, y));
                setEntryInLastRow(entry, isLastColumn);
                y += ctx.tabHeight + ctx.tabSpacer;
            }

            if (isLastColumn && this->showAddButton_)
            {
                this->addButton_->move(x, y);
            }

            if (!isRight)
            {
                x += largestWidth + ctx.lineThickness;
            }

            y = top;
        }
    }

    if (isRight)
    {
        // subtract another lineThickness to account for vertical divider
        x -= ctx.lineThickness;
        int consumedRightSpace = std::max(
            {ctx.right - x, consumedButtonWidths, ctx.minimumTabAreaSpace});
        int tabsStart = ctx.right - consumedRightSpace;

        if (this->lineOffset_ != tabsStart)
        {
            this->lineOffset_ = tabsStart;
            this->update();
        }

        // set page bounds
        if (auto *selectedPage = this->getSelectedPage())
        {
            selectedPage->move(0, 0);
            selectedPage->resize(tabsStart, this->height());
            selectedPage->raise();
        }
    }
    else
    {
        x = std::max({x, consumedButtonWidths, ctx.minimumTabAreaSpace});

        if (this->lineOffset_ != x - ctx.lineThickness)
        {
            this->lineOffset_ = x - ctx.lineThickness;
            this->update();
        }

        // set page bounds
        if (auto *selectedPage = this->getSelectedPage())
        {
            selectedPage->move(x, 0);
            selectedPage->resize(this->width() - x, this->height());
            selectedPage->raise();
        }
    }
}

void Notebook::mousePressEvent(QMouseEvent *event)
{
    this->update();

    switch (event->button())
    {
        case Qt::LeftButton: {
            if (this->bulkSelectedTabCount() > 0)
            {
                this->clearBulkSelectedTabs();
                event->accept();
                return;
            }
        }
        break;
        case Qt::RightButton: {
            event->accept();

            if (!this->menu_)
            {
                this->menu_ = new QMenu(this);
                this->addNotebookActionsToMenu(this->menu_);
            }
            this->menu_->popup(event->globalPosition().toPoint() +
                               QPoint(0, 8));
        }
        break;
        default:;
    }
}

void Notebook::setTabLocation(NotebookTabLocation location)
{
    if (location != this->tabLocation_)
    {
        this->tabLocation_ = location;

        // Update all tabs
        for (const auto &item : this->items_)
        {
            item.tab->setTabLocation(location);
        }
        for (const auto &folder : this->folders_)
        {
            folder.tab->setTabLocation(location);
        }

        this->performLayout();
    }
}

void Notebook::paintEvent(QPaintEvent *event)
{
    auto scale = this->scale();

    QPainter painter(this);

    if (this->showTabs_ && !this->folderDropTargetRect_.isNull())
    {
        QColor targetColor(117, 199, 255);
        auto fillColor = targetColor;
        fillColor.setAlpha(35);
        targetColor.setAlpha(180);

        auto rect = this->folderDropTargetRect_;
        rect.adjust(-int(2 * scale), int(2 * scale), int(2 * scale),
                    -int(2 * scale));

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(targetColor, std::max(2, int(2 * scale))));
        painter.setBrush(fillColor);
        painter.drawRoundedRect(rect, int(4 * scale), int(4 * scale));
        painter.setRenderHint(QPainter::Antialiasing, false);
    }

    if (this->showTabs_ && !this->tabDropPreviewRect_.isNull())
    {
        QColor lineColor(117, 199, 255);
        lineColor.setAlpha(230);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(lineColor);
        painter.drawRoundedRect(this->tabDropPreviewRect_, int(2 * scale),
                                int(2 * scale));
        painter.setRenderHint(QPainter::Antialiasing, false);
    }

    if (this->tabLocation_ == NotebookTabLocation::Top ||
        this->tabLocation_ == NotebookTabLocation::Bottom)
    {
        /// horizontal line
        painter.fillRect(0, this->lineOffset_, this->width(), int(2 * scale),
                         this->theme->tabs.dividerLine);
    }
    else if (this->tabLocation_ == NotebookTabLocation::Left ||
             this->tabLocation_ == NotebookTabLocation::Right)
    {
        if (this->visibleButtonCount() > 0)
        {
            if (this->tabLocation_ == NotebookTabLocation::Left)
            {
                painter.fillRect(0, int(NOTEBOOK_TAB_HEIGHT * scale),
                                 this->lineOffset_, int(2 * scale),
                                 this->theme->tabs.dividerLine);
            }
            else
            {
                painter.fillRect(this->lineOffset_,
                                 int(NOTEBOOK_TAB_HEIGHT * scale),
                                 this->width() - this->lineOffset_,
                                 int(2 * scale), this->theme->tabs.dividerLine);
            }
        }

        /// vertical line
        painter.fillRect(this->lineOffset_, 0, int(2 * scale), this->height(),
                         this->theme->tabs.dividerLine);
    }
}

bool Notebook::isNotebookLayoutLocked() const
{
    return this->lockNotebookLayout_;
}

void Notebook::setLockNotebookLayout(bool value)
{
    this->lockNotebookLayout_ = value;
    this->lockNotebookLayoutAction_->setChecked(value);
    getSettings()->lockNotebookLayout.setValue(value);
}

void Notebook::addNotebookActionsToMenu(QMenu *menu)
{
    menu->addAction(this->lockNotebookLayoutAction_);

    menu->addAction(this->addTabFolderAction_);

    menu->addAction(this->toggleTopMostAction_);
}

NotebookTab *Notebook::getTabFromPage(QWidget *page)
{
    for (auto &it : this->items_)
    {
        if (it.page == page)
        {
            return it.tab;
        }
    }

    return nullptr;
}

size_t Notebook::visibleButtonCount() const
{
    size_t i = 0;
    for (auto *btn : this->customButtons_)
    {
        if (!btn->isHidden())
        {
            ++i;
        }
    }
    return i;
}

void Notebook::setTabVisibilityFilter(TabVisibilityFilter filter)
{
    if (filter)
    {
        // Wrap tab filter to always accept selected tabs. This prevents confusion
        // when jumping to hidden tabs with the quick switcher, for example.
        filter = [originalFilter = std::move(filter)](const NotebookTab *tab) {
            return tab->isSelected() || tab->isBulkSelected() ||
                   originalFilter(tab);
        };
    }

    this->tabVisibilityFilter_ = std::move(filter);
    this->performLayout();
    this->updateTabVisibility();
}

bool Notebook::shouldShowTab(const NotebookTab *tab) const
{
    if (!this->showTabs_)
    {
        return false;
    }

    const auto itemIt =
        std::ranges::find_if(this->items_, [&](const Item &item) {
            return item.tab == tab;
        });
    if (itemIt == this->items_.end())
    {
        return false;
    }

    return this->isItemVisible(*itemIt);
}

void Notebook::sortTabsAlphabetically()
{
    assert(!this->isNotebookLayoutLocked() &&
           "sortTabsAlphabetically called while notebook layout is locked");
    std::ranges::sort(this->items_, [](const Item &a, const Item &b) {
        const QString &lhs = a.tab->getTitle();
        const QString &rhs = b.tab->getTitle();
        return lhs.compare(rhs, Qt::CaseInsensitive) < 0;
    });

    getApp()->getWindows()->queueSave();
    this->performLayout(true);
}

SplitNotebook::SplitNotebook(Window *parent)
    : Notebook(parent)
{
    QObject::connect(this->addButton_, &Button::leftClicked, [this]() {
        QTimer::singleShot(80, this, [this] {
            this->addPage(true);
        });
    });

    // add custom buttons if they are not in the parent window frame
    if (!parent->hasCustomWindowFrame())
    {
        this->addCustomButtons();
    }

    auto *tabVisibilityActionGroup = new QActionGroup(this);
    tabVisibilityActionGroup->setExclusionPolicy(
        QActionGroup::ExclusionPolicy::Exclusive);

    this->showAllTabsAction = new QAction("Show all tabs", this);
    this->showAllTabsAction->setCheckable(true);
    this->showAllTabsAction->setShortcut(
        getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"on"}}));
    QObject::connect(this->showAllTabsAction, &QAction::triggered, this,
                     [this] {
                         this->setShowTabs(true);
                         getSettings()->tabVisibility.setValue(
                             NotebookTabVisibility::AllTabs);
                         this->showAllTabsAction->setChecked(true);
                     });
    tabVisibilityActionGroup->addAction(this->showAllTabsAction);

    this->onlyShowLiveTabsAction = new QAction("Only show live tabs", this);
    this->onlyShowLiveTabsAction->setCheckable(true);
    this->onlyShowLiveTabsAction->setShortcut(
        getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"liveOnly"}}));
    QObject::connect(this->onlyShowLiveTabsAction, &QAction::triggered, this,
                     [this] {
                         this->setShowTabs(true);
                         getSettings()->tabVisibility.setValue(
                             NotebookTabVisibility::LiveOnly);
                         this->onlyShowLiveTabsAction->setChecked(true);
                     });
    tabVisibilityActionGroup->addAction(this->onlyShowLiveTabsAction);

    this->hideAllTabsAction = new QAction("Hide all tabs", this);
    this->hideAllTabsAction->setCheckable(true);
    this->hideAllTabsAction->setShortcut(
        getApp()->getHotkeys()->getDisplaySequence(
            HotkeyCategory::Window, "setTabVisibility", {{"off"}}));
    QObject::connect(this->hideAllTabsAction, &QAction::triggered, this,
                     [this] {
                         this->setShowTabs(false);
                         getSettings()->tabVisibility.setValue(
                             NotebookTabVisibility::AllTabs);
                         this->hideAllTabsAction->setChecked(true);
                     });
    tabVisibilityActionGroup->addAction(this->hideAllTabsAction);

    this->sortTabsAlphabeticallyAction_ =
        new QAction("Sort Tabs Alphabetically", this);
    if (this->isNotebookLayoutLocked())
    {
        this->sortTabsAlphabeticallyAction_->setEnabled(false);
    }
    QObject::connect(this->sortTabsAlphabeticallyAction_, &QAction::triggered,
                     [this] {
                         this->sortTabsAlphabetically();
                     });

    switch (getSettings()->tabVisibility.getEnum())
    {
        case NotebookTabVisibility::AllTabs: {
            this->showAllTabsAction->setChecked(true);
        }
        break;

        case NotebookTabVisibility::LiveOnly: {
            this->onlyShowLiveTabsAction->setChecked(true);
        }
        break;
    }

    getSettings()->tabVisibility.connect(
        [this](int val, auto) {
            auto visibility = NotebookTabVisibility(val);
            // Set the correct TabVisibilityFilter for the given visibility setting.
            // Note that selected tabs are always shown regardless of what the tab
            // filter returns, so no need to include `tab->isSelected()` in the
            // predicate. See Notebook::setTabVisibilityFilter.
            switch (visibility)
            {
                case NotebookTabVisibility::LiveOnly:
                    this->setTabVisibilityFilter([](const NotebookTab *tab) {
                        return tab->isLive();
                    });
                    break;
                case NotebookTabVisibility::AllTabs:
                default:
                    this->setTabVisibilityFilter(nullptr);
                    break;
            }
        },
        this->signalHolder_, true);

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->selectSplit, [this](Split *split) {
            for (auto &&item : this->items())
            {
                if (auto *sc = dynamic_cast<SplitContainer *>(item.page))
                {
                    auto &&splits = sc->getSplits();
                    if (std::find(splits.begin(), splits.end(), split) !=
                        splits.end())
                    {
                        this->select(item.page);
                        split->setFocus();
                        break;
                    }
                }
            }
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->selectSplitContainer,
        [this](SplitContainer *sc) {
            this->select(sc);
        });

    this->signalHolder_.managedConnect(
        getApp()->getWindows()->scrollToMessageSignal,
        [this](const MessagePtr &message, bool startReply) {
            for (auto &&item : this->items())
            {
                if (auto *sc = dynamic_cast<SplitContainer *>(item.page))
                {
                    for (auto *split : sc->getSplits())
                    {
                        auto type = split->getChannel()->getType();
                        if (type != Channel::Type::TwitchMentions &&
                            type != Channel::Type::TwitchAutomod)
                        {
                            if (split->getChannelView().scrollToMessage(
                                    message, startReply))
                            {
                                return;
                            }
                        }
                    }
                }
            }
        });
}

void SplitNotebook::addNotebookActionsToMenu(QMenu *menu)
{
    Notebook::addNotebookActionsToMenu(menu);

    menu->addAction(this->sortTabsAlphabeticallyAction_);

    auto *submenu = menu->addMenu("Tab visibility");
    submenu->addAction(this->showAllTabsAction);
    submenu->addAction(this->onlyShowLiveTabsAction);
    submenu->addAction(this->hideAllTabsAction);
}

void SplitNotebook::toggleTabVisibility()
{
    if (this->getShowTabs())
    {
        this->hideAllTabsAction->trigger();
    }
    else
    {
        this->showAllTabsAction->trigger();
    }
}

void SplitNotebook::showEvent(QShowEvent * /*event*/)
{
    if (auto *page = this->getSelectedPage())
    {
        auto *split = page->getSelectedSplit();
        if (!split)
        {
            split = page->findChild<Split *>();
        }

        if (split)
        {
            split->setFocus(Qt::OtherFocusReason);
        }
    }
}

void SplitNotebook::addCustomButtons()
{
    this->addBulkSelectionButton();

    // settings
    auto *settingsBtn = this->addCustomButton<SvgButton>(SvgButton::Src{
        .dark = ":/buttons/settings-darkMode.svg",
        .light = ":/buttons/settings-lightMode.svg",
    });

    settingsBtn->setPadding({0, 0});

    // This is to ensure you can't lock yourself out of the settings
    if (getApp()->getArgs().safeMode)
    {
        settingsBtn->setVisible(true);
    }
    else
    {
        settingsBtn->setVisible(
            !getSettings()->hidePreferencesButton.getValue());

        getSettings()->hidePreferencesButton.connect(
            [this, settingsBtn](bool hide) {
                auto oldVisibility = settingsBtn->isVisible();
                auto newVisibility = !hide;
                settingsBtn->setVisible(newVisibility);
                if (oldVisibility != newVisibility)
                {
                    this->performLayout();
                }
            },
            this->signalHolder_, false);
    }

    QObject::connect(settingsBtn, &Button::leftClicked, [this] {
        getApp()->getWindows()->showSettingsDialog(this);
    });

    // account
    this->accountButton_ = this->addCustomButton<SvgButton>(SvgButton::Src{
        .dark = ":/platforms/twitch.svg",
        .light = ":/platforms/twitch.svg",
    });

    this->accountButton_->setPadding({0, 0});

    this->accountButton_->setVisible(!getSettings()->hideUserButton.getValue());
    getSettings()->hideUserButton.connect(
        [this](bool hide) {
            auto oldVisibility = this->accountButton_->isVisible();
            auto newVisibility = !hide;
            this->accountButton_->setVisible(newVisibility);
            if (oldVisibility != newVisibility)
            {
                this->performLayout();
            }
        },
        this->signalHolder_, false);

    QObject::connect(this->accountButton_, &Button::leftClicked,
                     [this]() {
        getApp()->getWindows()->showAccountSelectPopup(
            this->mapToGlobal(this->accountButton_->rect().bottomRight()));
    });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->activeAccountProviderChanged,
        [this](ProviderId) { this->updateAccountButtonIcon(); });
    this->updateAccountButtonIcon();

    // streamer mode
    this->streamerModeIcon_ = this->addCustomButton<PixmapButton>();
    QObject::connect(this->streamerModeIcon_, &Button::leftClicked, [this] {
        getApp()->getWindows()->showSettingsDialog(
            this, SettingsDialogPreference::StreamerMode);
    });
    QObject::connect(getApp()->getStreamerMode(), &IStreamerMode::changed, this,
                     &SplitNotebook::updateStreamerModeIcon);
    this->updateStreamerModeIcon();

    this->performLayout(false);
}

void SplitNotebook::addBulkSelectionButton()
{
    this->bulkClearButton_ = this->addCustomButton<SvgButton>(
        SvgButton::Src{
            .dark = ":/buttons/x-darkMode.svg",
            .light = ":/buttons/x-lightMode.svg",
        });
    this->bulkClearButton_->setPadding({0, 0});
    this->bulkClearButton_->setContentSize(QSize{16, 16});
    this->bulkClearButton_->setHidden(true);
    this->bulkClearButton_->setToolTip("Deselect selected tabs");

    this->bulkDeleteButton_ = this->addCustomButton<SvgButton>(
        SvgButton::Src{
            .dark = ":/buttons/trash-darkMode.svg",
            .light = ":/buttons/trash-lightMode.svg",
        });
    this->bulkDeleteButton_->setPadding({0, 0});
    this->bulkDeleteButton_->setContentSize(QSize{16, 16});
    this->bulkDeleteButton_->setHidden(true);
    this->bulkDeleteButton_->setToolTip("Delete selected tabs");

    QObject::connect(this->bulkClearButton_, &Button::leftClicked, this,
                     [this] {
                         this->clearBulkSelectedTabs();
                     });
    QObject::connect(this->bulkDeleteButton_, &Button::leftClicked, this,
                     [this] {
                         this->removeBulkSelectedTabs();
                     });
    this->signalHolder_.managedConnect(this->bulkSelectionChanged, [this] {
        this->updateBulkSelectionButton();
    });
    this->updateBulkSelectionButton();
}

void SplitNotebook::updateBulkSelectionButton()
{
    if (this->bulkClearButton_ == nullptr || this->bulkDeleteButton_ == nullptr)
    {
        return;
    }

    const auto selectedCount = this->bulkSelectedTabCount();
    const auto shouldShow = selectedCount > 0;
    const auto wasHidden = this->bulkDeleteButton_->isHidden();
    this->bulkClearButton_->setHidden(!shouldShow);
    this->bulkDeleteButton_->setHidden(!shouldShow);
    this->bulkClearButton_->setToolTip(
        shouldShow ? QStringLiteral("Deselect selected tabs") : QString());
    this->bulkDeleteButton_->setToolTip(
        shouldShow ? QStringLiteral("Delete %1 selected tab%2")
                         .arg(selectedCount)
                         .arg(selectedCount == 1 ? QString()
                                                 : QStringLiteral("s"))
                   : QString());

    if (wasHidden == shouldShow)
    {
        this->performLayout(false);
    }
}

void SplitNotebook::updateStreamerModeIcon()
{
    if (this->streamerModeIcon_ == nullptr)
    {
        return;
    }
    // A duplicate of this code is in Window class
    // That copy handles the TitleBar icon in Window (main window on Windows)
    // This one is the one near splits (on linux and mac or non-main windows on Windows)
    if (getTheme()->isLightTheme())
    {
        this->streamerModeIcon_->setPixmap(
            getResources().buttons.streamerModeEnabledLight);
    }
    else
    {
        this->streamerModeIcon_->setPixmap(
            getResources().buttons.streamerModeEnabledDark);
    }

    auto oldVisibility = this->streamerModeIcon_->isVisible();
    auto newVisibility = getApp()->getStreamerMode()->isEnabled();

    this->streamerModeIcon_->setVisible(newVisibility);

    if (oldVisibility != newVisibility)
    {
        this->performLayout();
    }
}

void SplitNotebook::updateAccountButtonIcon()
{
    if (this->accountButton_ == nullptr)
    {
        return;
    }

    const auto path =
        providerIconPath(getApp()->getWindows()->activeAccountProvider());
    this->accountButton_->setSource(
        SvgButton::Src{
            .dark = path,
            .light = path,
        });
}

void SplitNotebook::themeChangedEvent()
{
    this->updateAccountButtonIcon();
    this->updateStreamerModeIcon();
}

SplitContainer *SplitNotebook::addPage(bool select)
{
    auto *container = new SplitContainer(this);
    auto *tab = Notebook::addPage(container, QString(), select);
    container->setTab(tab);
    tab->setParent(this);
    return container;
}

SplitContainer *SplitNotebook::addPageInFolder(const QString &folderId,
                                               bool select)
{
    auto *container = new SplitContainer(this);
    auto *tab = Notebook::addPageAt(container, -1, QString(), select, folderId);
    container->setTab(tab);
    tab->setParent(this);
    return container;
}

void SplitNotebook::createPageInFolder(const QString &folderId)
{
    this->addPageInFolder(folderId, true);
}

SplitContainer *SplitNotebook::getOrAddSelectedPage()
{
    auto *selectedPage = this->getSelectedPage();

    if (selectedPage)
    {
        return dynamic_cast<SplitContainer *>(selectedPage);
    }

    return this->addPage();
}

SplitContainer *SplitNotebook::getSelectedPage()
{
    return dynamic_cast<SplitContainer *>(Notebook::getSelectedPage());
}

void SplitNotebook::select(QWidget *page, bool focusPage)
{
    // If there's a previously selected page, go through its splits and
    // update their "last read message" indicator
    if (auto *selectedPage = this->getSelectedPage())
    {
        if (auto *splitContainer = dynamic_cast<SplitContainer *>(selectedPage))
        {
            for (auto *split : splitContainer->getSplits())
            {
                split->updateLastReadMessage();
            }
        }
    }

    this->Notebook::select(page, focusPage);
}

void SplitNotebook::forEachSplit(const std::function<void(Split *)> &cb)
{
    for (const auto &item : this->items())
    {
        auto *page = dynamic_cast<SplitContainer *>(item.page);
        if (!page)
        {
            continue;
        }
        for (auto *split : page->getSplits())
        {
            cb(split);
        }
    }
}

void SplitNotebook::setLockNotebookLayout(bool value)
{
    Notebook::setLockNotebookLayout(value);
    this->sortTabsAlphabeticallyAction_->setEnabled(!value);
}

}  // namespace chatterino
