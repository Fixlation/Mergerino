// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/QualityPopup.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "singletons/WindowManager.hpp"
#include "util/StreamLink.hpp"
#include "widgets/Window.hpp"

#include <QFormLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>

namespace chatterino {

QualityPopup::QualityPopup(const QList<StreamlinkSource> &sources)
    : BasePopup(
          {
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())))
{
    this->ui_.service = new QComboBox(this);
    this->ui_.selector = new QComboBox(this);
    this->ui_.status = new QLabel(this);
    this->ui_.status->setWordWrap(true);
    this->ui_.status->setTextFormat(Qt::PlainText);
    this->ui_.retry = new QPushButton("Refresh qualities", this);
    this->ui_.vbox = new QVBoxLayout(this);
    this->ui_.buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    QObject::connect(this->ui_.buttonBox, &QDialogButtonBox::accepted, this,
                     &QualityPopup::okButtonClicked);
    QObject::connect(this->ui_.buttonBox, &QDialogButtonBox::rejected, this,
                     &QualityPopup::cancelButtonClicked);

    for (const auto &source : sources)
    {
        this->ui_.service->addItem(source.service, source.url);
    }
    auto *form = new QFormLayout;
    form->addRow("Service", this->ui_.service);
    form->addRow("Quality", this->ui_.selector);
    this->ui_.vbox->addLayout(form);
    this->ui_.vbox->addWidget(this->ui_.status);
    this->ui_.vbox->addWidget(this->ui_.retry);
    this->ui_.vbox->addWidget(this->ui_.buttonBox);

    this->setLayout(this->ui_.vbox);
    QObject::connect(this->ui_.service, &QComboBox::currentIndexChanged, this,
                     [this] {
                         this->refreshQualities();
                     });
    QObject::connect(this->ui_.retry, &QPushButton::clicked, this, [this] {
        this->refreshQualities();
    });
    this->refreshQualities();
}

QualityPopup::~QualityPopup()
{
    this->cancelQuery();
}

void QualityPopup::cancelQuery()
{
    ++this->queryGeneration_;
    if (this->query_)
    {
        QObject::disconnect(this->query_, nullptr, this, nullptr);
        cancelStreamlinkQuery(this->query_);
        this->query_.clear();
    }
}

void QualityPopup::refreshQualities()
{
    this->cancelQuery();
    this->ui_.selector->clear();
    this->ui_.selector->setEnabled(false);
    this->ui_.buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    const auto url = this->ui_.service->currentData().toString();
    if (url.isEmpty())
    {
        this->ui_.status->setText(
            "No stream services are configured for this tab.");
        return;
    }
    this->ui_.status->setText("Loading available qualities...");
    const auto generation = this->queryGeneration_;
    this->query_ = getStreamQualities(
        url, this, [this, generation](QStringList options, QString error) {
            if (generation != this->queryGeneration_)
            {
                return;
            }
            this->query_.clear();
            this->ui_.selector->addItems(options);
            this->ui_.selector->setEnabled(!options.isEmpty());
            this->ui_.buttonBox->button(QDialogButtonBox::Ok)
                ->setEnabled(!options.isEmpty());
            this->ui_.status->setText(error);
        });
}

void QualityPopup::showDialog(const QList<StreamlinkSource> &sources)
{
    auto *instance = new QualityPopup(sources);

    instance->window()->setWindowTitle(
        "Mergerino - select stream service/quality");
    instance->setAttribute(Qt::WA_DeleteOnClose, true);

    instance->show();
    instance->activateWindow();
    instance->raise();
}

void QualityPopup::keyPressEvent(QKeyEvent *e)
{
    if (this->handleEscape(e, this->ui_.buttonBox))
    {
        return;
    }
    if (this->handleEnter(e, this->ui_.buttonBox))
    {
        return;
    }

    BasePopup::keyPressEvent(e);
}

void QualityPopup::okButtonClicked()
{
    if (!this->ui_.buttonBox->button(QDialogButtonBox::Ok)->isEnabled() ||
        this->ui_.selector->currentIndex() < 0)
    {
        return;
    }
    try
    {
        openStreamlink(this->ui_.service->currentData().toString(),
                       this->ui_.selector->currentText());
    }
    catch (const Exception &ex)
    {
        qCWarning(chatterinoWidget)
            << "Exception caught trying to open streamlink:" << ex.what();
    }

    this->close();
}

void QualityPopup::cancelButtonClicked()
{
    this->close();
}

}  // namespace chatterino
