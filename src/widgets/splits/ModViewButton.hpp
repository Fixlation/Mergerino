// SPDX-License-Identifier: MIT
#pragma once

#include "widgets/buttons/LabelButton.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QEasingCurve>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>

namespace chatterino {

// Flat text button using the app's hover and click effects. Padding scales with
// the UI, and an optional fill gives primary actions some weight.
class ModViewButton final : public LabelButton
{
public:
    ModViewButton(const QString &text, BaseWidget *parent,
                  QSize padding = {8, 3})
        : LabelButton(text, parent, padding)
        , basePadding_(padding)
    {
    }

    void setFill(const QColor &fill)
    {
        this->fill_ = fill;
        this->update();
    }

protected:
    void scaleChangedEvent(float scale) override
    {
        LabelButton::scaleChangedEvent(scale);
        this->setPadding(this->basePadding_ * scale);
    }

    void paintContent(QPainter &painter) override
    {
        if (this->fill_.isValid())
            painter.fillRect(this->rect(), this->fill_);
    }

private:
    QSize basePadding_;
    QColor fill_;
};

// An on/off switch in place of a checkbox's box. The app's theme draws
// nothing for an unchecked box, which made settings look like plain text.
// It stays a QCheckBox, so signals and lookups work the same.
class ModViewSwitch final : public QCheckBox
{
public:
    explicit ModViewSwitch(const QString &text, QWidget *parent = nullptr)
        : QCheckBox(text, parent)
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setAttribute(Qt::WA_Hover);
        this->slide_.setDuration(160);
        this->slide_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&this->slide_, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->position_ = value.toReal();
                             this->update();
                         });
        this->hover_.setDuration(120);
        QObject::connect(&this->hover_, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->hovered_ = value.toReal();
                             this->update();
                         });
        QObject::connect(this, &QCheckBox::toggled, this, [this](bool on) {
            // Slides from wherever the knob is, even mid-way
            const auto from = this->slide_.state() == QAbstractAnimation::Running
                                  ? this->position_
                                  : (on ? 0.0 : 1.0);
            this->slide_.stop();
            if (!this->isVisible())
            {
                this->update();
                return;
            }
            this->position_ = from;
            this->slide_.setStartValue(from);
            this->slide_.setEndValue(on ? 1.0 : 0.0);
            this->slide_.start();
        });
    }

    /// Indent that lines content up with the text after the switch.
    static int textIndent(const QFont &font)
    {
        return trackSize(font).width() + gap(font);
    }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(this->font());
        int width = textIndent(this->font());
        if (!this->icon().isNull())
            width += this->iconSize().width() + gap(this->font());
        width += metrics.horizontalAdvance(this->text()) + 1;
        return {width, std::max(metrics.height(),
                                trackSize(this->font()).height()) +
                           4};
    }

    QSize minimumSizeHint() const override
    {
        return {trackSize(this->font()).width() + 1, this->sizeHint().height()};
    }

protected:
    bool hitButton(const QPoint &pos) const override
    {
        return this->rect().contains(pos);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter ||
            event->type() == QEvent::HoverLeave)
        {
            this->hover_.stop();
            this->hover_.setStartValue(this->hovered_);
            this->hover_.setEndValue(
                event->type() == QEvent::HoverEnter ? 1.0 : 0.0);
            this->hover_.start();
        }
        return QCheckBox::event(event);
    }

    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(this->isEnabled() ? 1.0 : 0.6);
        const auto text = this->palette().color(QPalette::WindowText);
        const auto muted = this->palette().color(QPalette::PlaceholderText);
        const auto background = this->surface(text);
        // 0 is off and 1 is on, in between while sliding
        const auto on = this->position();
        const auto edge = mix(muted, text, this->hovered_);

        const auto track = trackSize(this->font());
        const QRectF bar(0.5, (this->height() - track.height()) / 2.0 + 0.5,
                         track.width() - 1, track.height() - 1);
        const auto radius = bar.height() / 2;
        // Off: an outline with a dim knob. On: a filled track with a bright
        // knob, so the knob stands out at every point of the slide
        const bool dark = background.lightness() < 128;
        auto fill =
            mix(background, text, (dark ? 0.42 : 0.68) + 0.1 * this->hovered_);
        fill.setAlphaF(float(on));
        auto outline = edge;
        outline.setAlphaF(float(1.0 - on));
        painter.setPen(on < 1.0 ? QPen(outline, 1) : QPen(Qt::NoPen));
        painter.setBrush(fill);
        painter.drawRoundedRect(bar, radius, radius);
        const auto inset = std::max(2.0, bar.height() / 6);
        const auto knob = bar.height() - inset * 2;
        const auto start = bar.left() + inset;
        const auto end = bar.right() - inset - knob;
        painter.setPen(Qt::NoPen);
        painter.setBrush(mix(edge, dark ? text : background, on));
        painter.drawEllipse(
            QRectF(start + (end - start) * on, bar.top() + inset, knob, knob));

        int left = textIndent(this->font());
        if (!this->icon().isNull())
        {
            const auto size = this->iconSize();
            this->icon().paint(&painter,
                               QRect(left, (this->height() - size.height()) / 2,
                                     size.width(), size.height()));
            left += size.width() + gap(this->font());
        }
        const QRect area(left, 0, this->width() - left, this->height());
        painter.setPen(text);
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(this->font())
                             .elidedText(this->text(), Qt::ElideRight,
                                         area.width()));
    }

private:
    qreal position() const
    {
        if (this->slide_.state() == QAbstractAnimation::Running)
            return this->position_;
        return this->isChecked() ? 1.0 : 0.0;
    }

    static QColor mix(const QColor &from, const QColor &to, qreal amount)
    {
        const auto blend = [amount](float a, float b) {
            return float(a + (b - a) * amount);
        };
        return QColor::fromRgbF(blend(from.redF(), to.redF()),
                                blend(from.greenF(), to.greenF()),
                                blend(from.blueF(), to.blueF()),
                                blend(from.alphaF(), to.alphaF()));
    }

    /// The checkbox itself is transparent; the knob uses the pane behind it.
    QColor surface(const QColor &text) const
    {
        for (auto *widget = this->parentWidget(); widget;
             widget = widget->parentWidget())
            if (widget->autoFillBackground())
                return widget->palette().color(QPalette::Window);
        return text.lightness() > 128 ? QColor(Qt::black) : QColor(Qt::white);
    }

    static QSize trackSize(const QFont &font)
    {
        const auto height =
            std::max(10, int(QFontMetrics(font).height() * 0.75));
        return {int(height * 1.8), height};
    }

    static int gap(const QFont &font)
    {
        return std::max(5, QFontMetrics(font).height() / 3);
    }

    QVariantAnimation slide_;
    QVariantAnimation hover_;
    qreal position_ = 0;
    qreal hovered_ = 0;
};

// Dropdown matching the app's other dialogs: the stylesheet hides the native
// arrow, so a small one is painted in the placeholder text color.
class ModViewComboBox final : public QComboBox
{
public:
    explicit ModViewComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        this->setFocusPolicy(Qt::StrongFocus);
        this->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    }

    // Narrow panes may squeeze the box; the text is elided
    QSize minimumSizeHint() const override
    {
        auto size = QComboBox::minimumSizeHint();
        size.setWidth(std::min(
            size.width(),
            this->fontMetrics().horizontalAdvance("Mmmm") + this->height()));
        return size;
    }

protected:
    void showPopup() override
    {
        QComboBox::showPopup();
        if (this->view() && this->model() && this->currentIndex() >= 0)
            this->view()->setCurrentIndex(this->model()->index(
                this->currentIndex(), this->modelColumn(),
                this->rootModelIndex()));
    }

    void paintEvent(QPaintEvent *event) override
    {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal size = std::max(2.0, this->height() / 9.0);
        const QPointF center(this->width() - size * 2.4,
                             this->height() / 2.0 + size * 0.4);
        QPolygonF arrow;
        arrow << QPointF(center.x() - size * 1.4, center.y() - size)
              << QPointF(center.x() + size * 1.4, center.y() - size)
              << QPointF(center.x(), center.y() + size * 0.6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(this->palette().color(this->isEnabled()
                                                   ? QPalette::Active
                                                   : QPalette::Disabled,
                                               QPalette::PlaceholderText));
        painter.drawPolygon(arrow);
    }
};

}  // namespace chatterino
