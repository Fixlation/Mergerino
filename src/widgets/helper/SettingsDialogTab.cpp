// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/SettingsDialogTab.hpp"

#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

#include <QPainter>
#include <QStyleOption>

#include <cmath>

namespace chatterino {

SettingsDialogTab::SettingsDialogTab(SettingsDialog *_dialog,
                                     std::function<SettingsPage *()> _lazyPage,
                                     const QString &name, QString imageFileName,
                                     SettingsTabId id,
                                     QStringList searchKeywords)
    : BaseWidget(_dialog)
    , dialog_(_dialog)
    , lazyPage_(std::move(_lazyPage))
    , id_(id)
    , name_(name)
    , searchKeywords_(std::move(searchKeywords))
{
    this->ui_.labelText = name;
    this->ui_.icon.addFile(imageFileName);

    this->setCursor(QCursor(Qt::PointingHandCursor));

}

void SettingsDialogTab::setSelected(bool _selected)
{
    if (this->selected_ == _selected)
    {
        return;
    }

    //    height: <checkbox-size>px;

    this->selected_ = _selected;
    this->selectedChanged(this->selected_);
}

SettingsPage *SettingsDialogTab::page()
{
    if (this->page_)
    {
        return this->page_;
    }

    this->page_ = this->lazyPage_();
    this->page_->setTab(this);
    return this->page_;
}

SettingsPage *SettingsDialogTab::createdPage() const
{
    return this->page_;
}

bool SettingsDialogTab::matchesSearch(const QString &query) const
{
    if (query.isEmpty())
    {
        return true;
    }

    if (this->name_.contains(query, Qt::CaseInsensitive))
    {
        return true;
    }

    for (const auto &keyword : this->searchKeywords_)
    {
        if (keyword.contains(query, Qt::CaseInsensitive))
        {
            return true;
        }
    }

    return false;
}

void SettingsDialogTab::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    QStyleOption opt;
    opt.initFrom(this);

    this->style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);

    int iconSize = 20 * this->scale();
    int pad = (this->height() - iconSize) / 2;
    QPixmap pixmap = this->ui_.icon.pixmap(
        QSize(this->height() - pad * 2, this->height() - pad * 2));

    painter.drawPixmap(pad, pad, pixmap);

    pad = (3 * pad) + iconSize + static_cast<int>(std::round(2 * this->scale()));

    this->style()->drawItemText(
        &painter, QRect(pad, 0, this->width() - pad, this->height()),
        Qt::AlignLeft | Qt::AlignVCenter, this->palette(), this->isEnabled(),
        this->ui_.labelText, QPalette::WindowText);
}

void SettingsDialogTab::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }

    this->dialog_->selectTab(this);

    this->setFocus();
}

const QString &SettingsDialogTab::name() const
{
    return this->name_;
}

SettingsTabId SettingsDialogTab::id() const
{
    return this->id_;
}

}  // namespace chatterino
