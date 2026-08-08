// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/UpdateDialog.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Version.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Updates.hpp"
#include "singletons/WindowManager.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Label.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSize>
#include <QStringList>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

#include <optional>
#include <utility>

namespace chatterino {
namespace {

constexpr auto STREAMDATABASE_LOGIN = "streamdatabase";
constexpr auto STREAMDATABASE_LOGO_URL =
    "https://www.streamdatabase.com/logo.svg";

QPushButton *makeDialogSocialButton(const QString &iconPath,
                                    const QString &tooltip, const QUrl &url,
                                    QWidget *parent)
{
    auto *button = new QPushButton(parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setFlat(true);
    button->setFixedSize({22, 22});
    button->setIcon(QIcon(iconPath));
    button->setIconSize({16, 16});
    button->setToolTip(tooltip);
    button->setStyleSheet(QStringLiteral(R"(
        QPushButton {
            background: transparent;
            border: none;
            padding: 2px;
        }
        QPushButton:hover {
            background-color: rgba(255, 255, 255, 28);
            border-radius: 3px;
        }
    )"));
    QObject::connect(button, &QPushButton::clicked, button, [url] {
        QDesktopServices::openUrl(url);
    });
    return button;
}

void addDialogSocialButtons(QHBoxLayout *layout, QWidget *parent)
{
    layout->addWidget(makeDialogSocialButton(
        QStringLiteral(":/social/discord.svg"),
        QStringLiteral("Join the Mergerino Discord"),
        QUrl(LINK_MERGERINO_DISCORD.toString()), parent));
    layout->addSpacing(2);
    layout->addWidget(makeDialogSocialButton(
        QStringLiteral(":/social/github.svg"),
        QStringLiteral("Open the Mergerino GitHub"),
        QUrl(LINK_MERGERINO_SOURCE.toString()), parent));
    layout->addSpacing(2);
    layout->addWidget(makeDialogSocialButton(
        QStringLiteral(":/social/x.svg"),
        QStringLiteral("Open Mergerino on X"),
        QUrl(LINK_MERGERINO_X.toString()), parent));
}

void setLogoPixmap(QLabel *label, const QPixmap &pixmap, const QSize &size)
{
    label->setPixmap(
        pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

QString trimPatchNotesSection(QStringList lines)
{
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
    {
        lines.removeFirst();
    }

    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
    {
        lines.removeLast();
    }

    return lines.join('\n').trimmed();
}

std::optional<QDate> parsePatchNotesDateHeader(const QString &line)
{
    static const QRegularExpression dateHeader{
        QStringLiteral(R"(^(\d{1,2})/(\d{1,2})/(\d{4})$)")};

    const auto match = dateHeader.match(line.trimmed());
    if (!match.hasMatch())
    {
        return std::nullopt;
    }

    const auto date =
        QDate(match.captured(3).toInt(), match.captured(2).toInt(),
              match.captured(1).toInt());
    if (!date.isValid())
    {
        return std::nullopt;
    }

    return date;
}

QString firstPatchNotesSectionFrom(const QStringList &lines)
{
    QStringList sectionLines;
    bool foundSection = false;
    bool foundContent = false;

    for (const auto &line : lines)
    {
        const auto trimmed = line.trimmed();

        if (!foundSection)
        {
            if (trimmed.isEmpty() ||
                trimmed.compare(QStringLiteral("Patch Notes"),
                                Qt::CaseInsensitive) == 0)
            {
                continue;
            }

            foundSection = true;
        }
        else if (trimmed.isEmpty() && foundContent)
        {
            break;
        }

        if (!trimmed.isEmpty())
        {
            foundContent = true;
        }

        sectionLines.append(trimmed);
    }

    const auto latestSection = sectionLines.join('\n').trimmed();
    return latestSection;
}

QString latestPatchNotesFrom(QString text)
{
    const auto lines =
        text.replace("\r\n", "\n").replace('\r', '\n').split('\n');

    QDate latestDate;
    QStringList latestSectionLines;
    QDate currentDate;
    QStringList currentSectionLines;

    const auto commitCurrentSection = [&] {
        if (!currentDate.isValid())
        {
            return;
        }

        if (!latestDate.isValid() || currentDate > latestDate)
        {
            latestDate = currentDate;
            latestSectionLines = currentSectionLines;
        }
    };

    for (const auto &line : lines)
    {
        const auto trimmed = line.trimmed();

        if (const auto date = parsePatchNotesDateHeader(trimmed))
        {
            commitCurrentSection();
            currentDate = *date;
            currentSectionLines.clear();
            continue;
        }

        if (currentDate.isValid())
        {
            currentSectionLines.append(trimmed);
        }
    }
    commitCurrentSection();

    const auto latestSection = trimPatchNotesSection(latestSectionLines);
    if (!latestSection.isEmpty())
    {
        return latestSection;
    }

    const auto firstSection = firstPatchNotesSectionFrom(lines);
    return firstSection.isEmpty() ? text.trimmed() : firstSection;
}

QString loadLatestPatchNotes()
{
    const QStringList paths = {
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("patchnotes.txt")),
        QStringLiteral(":/patchnotes.txt"),
    };

    for (const auto &path : paths)
    {
        QFile file(path);
        if (!file.open(QFile::ReadOnly | QFile::Text))
        {
            continue;
        }

        auto text = QString::fromUtf8(file.readAll()).trimmed();
        if (!text.isEmpty())
        {
            return latestPatchNotesFrom(text);
        }
    }

    return QStringLiteral("No patch notes were found.");
}

QString patchNotesHtml(const QString &text)
{
    QStringList rows;
    const auto lines =
        QString(text).replace("\r\n", "\n").replace('\r', '\n').split('\n');

    for (const auto &line : lines)
    {
        auto lineHtml = line.toHtmlEscaped();
        const auto linkText = QStringLiteral("Mergerino Discord");
        const auto linkIndex = line.indexOf(linkText, 0, Qt::CaseInsensitive);
        if (linkIndex >= 0)
        {
            const auto before = line.left(linkIndex).toHtmlEscaped();
            const auto label =
                line.mid(linkIndex, linkText.size()).toHtmlEscaped();
            const auto after =
                line.mid(linkIndex + linkText.size()).toHtmlEscaped();

            lineHtml =
                before +
                QStringLiteral("<a href=\"%1\">%2</a>")
                    .arg(LINK_MERGERINO_DISCORD.toString().toHtmlEscaped(),
                         label) +
                after;
        }

        rows.append(QStringLiteral("<div>%1</div>").arg(
            lineHtml.isEmpty() ? QStringLiteral("&nbsp;") : lineHtml));
    }

    return QStringLiteral(
               "<html><head><style>"
               "body { margin: 0; }"
               "a, a:visited { color: #ffffff; text-decoration: underline; "
               "text-decoration-color: #ffffff; }"
               "</style></head><body>%1</body></html>")
        .arg(rows.join('\n'));
}

}  // namespace

StreamDatabaseUpdateDialog::StreamDatabaseUpdateDialog(
    QWidget *parent, std::function<void()> onContinueToPatchNotes)
    : BaseWindow({}, parent)
    , onContinueToPatchNotes_(std::move(onContinueToPatchNotes))
{
    this->setWindowTitle("Mergerino x StreamDatabase");
    this->setAttribute(Qt::WA_DeleteOnClose);

    this->getLayoutContainer()->setStyleSheet(QStringLiteral(R"(
        QLabel#StreamDatabaseTitle {
            color: #f4f6fb;
        }
        QLabel#StreamDatabaseBody {
            color: #d8dbe4;
        }
        QLabel#StreamDatabaseSeparator {
            color: #f4f6fb;
        }
        QPushButton#StreamDatabasePrimaryButton {
            background-color: #7c3cff;
            border: 1px solid #a577ff;
            border-radius: 3px;
            color: white;
            font-weight: bold;
            padding: 5px 16px;
        }
        QPushButton#StreamDatabasePrimaryButton:hover {
            background-color: #8c52ff;
        }
        QPushButton#StreamDatabasePrimaryButton:pressed {
            background-color: #6c2ee5;
        }
        QPushButton#StreamDatabasePrimaryButton:disabled {
            background-color: #363840;
            border-color: #4d5260;
            color: #aeb4c2;
        }
        QPushButton#StreamDatabaseSecondaryButton {
            background-color: #33363d;
            border: 1px solid #555965;
            border-radius: 3px;
            color: #eef1f7;
            padding: 5px 12px;
        }
        QPushButton#StreamDatabaseSecondaryButton:hover {
            background-color: #424650;
        }
        QPushButton#StreamDatabaseSecondaryButton:pressed {
            background-color: #292c33;
        }
    )"));

    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(18, 18, 18, 16);
    layout->setSpacing(0);

    auto *logoRow = new QHBoxLayout;
    logoRow->setContentsMargins(0, 0, 0, 0);
    logoRow->setSpacing(0);
    logoRow->addStretch(1);

    auto *mergerinoLogo = new QLabel(this);
    mergerinoLogo->setFixedSize({58, 58});
    mergerinoLogo->setAlignment(Qt::AlignCenter);
    const QPixmap mergerinoPixmap(QStringLiteral(":/icon.png"));
    if (!mergerinoPixmap.isNull())
    {
        setLogoPixmap(mergerinoLogo, mergerinoPixmap, {50, 50});
    }
    else
    {
        mergerinoLogo->setText(QStringLiteral("Mergerino"));
    }
    logoRow->addWidget(mergerinoLogo);
    logoRow->addSpacing(14);

    auto *separator = new QLabel(QStringLiteral("x"), this);
    separator->setObjectName(QStringLiteral("StreamDatabaseSeparator"));
    auto separatorFont = separator->font();
    separatorFont.setBold(true);
    separatorFont.setPointSize(separatorFont.pointSize() + 3);
    separator->setFont(separatorFont);
    separator->setAlignment(Qt::AlignCenter);
    logoRow->addWidget(separator);
    logoRow->addSpacing(18);

    this->streamDatabaseLogo_ = new QLabel(QStringLiteral("StreamDatabase"), this);
    this->streamDatabaseLogo_->setFixedSize({58, 58});
    this->streamDatabaseLogo_->setAlignment(Qt::AlignCenter);
    logoRow->addWidget(this->streamDatabaseLogo_);
    logoRow->addStretch(1);

    layout->addLayout(logoRow);
    layout->addSpacing(6);

    auto *title = new QLabel(QStringLiteral("Mergerino x StreamDatabase"), this);
    title->setObjectName(QStringLiteral("StreamDatabaseTitle"));
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    title->setWordWrap(true);
    layout->addWidget(title);
    layout->addSpacing(8);

    auto *body = new QLabel(
        QStringLiteral(
            "StreamDatabase is now built into Mergerino as a dedicated Twitch "
            "tab for global badge drops. Track active and upcoming drops, "
            "eligibility notes, linked channels, and badge ownership without "
            "leaving chat."),
        this);
    body->setObjectName(QStringLiteral("StreamDatabaseBody"));
    body->setAlignment(Qt::AlignCenter);
    body->setFixedWidth(380);
    body->setWordWrap(true);
    layout->addWidget(body, 0, Qt::AlignHCenter);
    layout->addSpacing(12);

    this->addTabButton_ =
        new QPushButton(QStringLiteral("Add StreamDatabase tab"), this);
    this->addTabButton_->setObjectName(
        QStringLiteral("StreamDatabasePrimaryButton"));
    this->addTabButton_->setCursor(Qt::PointingHandCursor);
    this->addTabButton_->setMinimumHeight(30);
    this->addTabButton_->setMinimumWidth(220);
    layout->addWidget(this->addTabButton_, 0, Qt::AlignHCenter);
    layout->addSpacing(10);

    auto *buttonBox = new QDialogButtonBox(this);
    buttonBox->setCenterButtons(true);
    auto *closeButton =
        buttonBox->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);
    closeButton->setObjectName(QStringLiteral("StreamDatabaseSecondaryButton"));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setMinimumHeight(28);
    closeButton->setMinimumWidth(78);
    if (this->onContinueToPatchNotes_)
    {
        auto *continueButton = buttonBox->addButton(
            QStringLiteral("Continue to patch notes"),
            QDialogButtonBox::AcceptRole);
        continueButton->setObjectName(
            QStringLiteral("StreamDatabaseSecondaryButton"));
        continueButton->setCursor(Qt::PointingHandCursor);
        continueButton->setMinimumHeight(28);
        continueButton->setMinimumWidth(150);
        QObject::connect(continueButton, &QPushButton::clicked, this, [this] {
            if (this->onContinueToPatchNotes_)
            {
                this->onContinueToPatchNotes_();
            }
            this->close();
        });
    }
    layout->addWidget(buttonBox, 0, Qt::AlignHCenter);
    layout->addSpacing(4);

    QObject::connect(this->addTabButton_, &QPushButton::clicked, this,
                     [this] {
                         this->addStreamDatabaseTab();
                     });
    QObject::connect(closeButton, &QPushButton::clicked, this, [this] {
        this->close();
    });

    this->setScaleIndependentWidth(440);
    this->setFixedHeight(this->sizeHint().height());

    this->requestStreamDatabaseLogo();
}

void StreamDatabaseUpdateDialog::addStreamDatabaseTab()
{
    if (this->addedStreamDatabaseTab_)
    {
        return;
    }

    auto &window = getApp()->getWindows()->getMainWindow();
    auto &notebook = window.getNotebook();
    auto *container = notebook.addPage(true);
    auto *split = new Split(container);
    split->setPlatformIndicatorMode(PlatformIndicatorMode::LineColor);
    split->setChannel(getApp()->getTwitch()->getOrAddChannel(
        QString::fromLatin1(STREAMDATABASE_LOGIN)));
    split->setStreamDatabaseBadgeFeedVisible(true);
    container->insertSplit(split);
    container->setSelected(split);
    notebook.select(container);
    split->getChannelView().refreshPlatformIndicatorMode();
    getApp()->getWindows()->queueSave();

    this->addedStreamDatabaseTab_ = true;
    this->addTabButton_->setText(QStringLiteral("StreamDatabase tab added"));
    this->addTabButton_->setEnabled(false);
}

void StreamDatabaseUpdateDialog::requestStreamDatabaseLogo()
{
    if (this->pendingLogoReply_ != nullptr ||
        this->streamDatabaseLogo_ == nullptr)
    {
        return;
    }

    QNetworkRequest request(
        QUrl(QString::fromLatin1(STREAMDATABASE_LOGO_URL)));
    auto *reply = this->network_.get(request);
    this->pendingLogoReply_ = reply;

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        this->handleStreamDatabaseLogoFinished(reply);
    });
}

void StreamDatabaseUpdateDialog::handleStreamDatabaseLogoFinished(
    QNetworkReply *reply)
{
    if (this->pendingLogoReply_ == reply)
    {
        this->pendingLogoReply_ = nullptr;
    }

    if (reply->error() == QNetworkReply::NoError &&
        this->streamDatabaseLogo_ != nullptr)
    {
        QPixmap pixmap;
        pixmap.loadFromData(reply->readAll());
        if (!pixmap.isNull())
        {
            this->streamDatabaseLogo_->clear();
            setLogoPixmap(this->streamDatabaseLogo_, pixmap, {56, 56});
        }
    }

    reply->deleteLater();
}

PostUpdateDialog::PostUpdateDialog(const QString &version, QWidget *parent)
    : BaseWindow({}, parent)
{
    this->setWindowTitle("Mergerino updated");
    this->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this->getLayoutContainer());
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto *title = new QLabel(
        QStringLiteral("Mergerino has been successfully updated to %1")
            .arg(version),
        this);
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *patchNotes = new QTextBrowser(this);
    patchNotes->setReadOnly(true);
    patchNotes->setOpenExternalLinks(true);
    patchNotes->setHtml(patchNotesHtml(loadLatestPatchNotes()));
    patchNotes->setMinimumHeight(170);
    layout->addWidget(patchNotes);

    auto *link = new QLabel(
        QStringLiteral("<a href=\"https://github.com/Fixlation/Mergerino/blob/"
                       "main/patchnotes.txt\">View all patch notes here</a>"),
        this);
    link->setTextFormat(Qt::RichText);
    link->setTextInteractionFlags(Qt::TextBrowserInteraction |
                                  Qt::LinksAccessibleByKeyboard);
    link->setOpenExternalLinks(true);

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->addWidget(link);
    bottomRow->addStretch(1);
    addDialogSocialButtons(bottomRow, this);
    layout->addLayout(bottomRow);

    this->setScaleIndependentWidth(420);
    this->setScaleIndependentHeight(300);
}

UpdateDialog::UpdateDialog()
    : BaseWindow({BaseWindow::Frameless, BaseWindow::TopMost,
                  BaseWindow::EnableCustomFrame, BaseWindow::DisableLayoutSave})
{
    this->windowDeactivateAction = WindowDeactivateAction::Nothing;
    this->setAttribute(Qt::WA_DeleteOnClose);

    auto layout =
        LayoutCreator<UpdateDialog>(this).setLayoutType<QVBoxLayout>();

    layout.emplace<Label>("You shouldn't be seeing this dialog.")
        .assign(&this->ui_.label)
        ->setWordWrap(true);

    auto buttons = layout.emplace<QDialogButtonBox>();

    const auto *installText = [] {
        if (Version::instance().isNightly())
        {
            return "Yes";
        }

        return "Install";
    }();
    auto *install =
        buttons->addButton(installText, QDialogButtonBox::AcceptRole);
    this->ui_.installButton = install;
    auto *dismiss = buttons->addButton("Dismiss", QDialogButtonBox::RejectRole);

    QObject::connect(install, &QPushButton::clicked, this, [this] {
        getApp()->getUpdates().installUpdates();
    });
    QObject::connect(dismiss, &QPushButton::clicked, this, [this] {
        this->dismissed.invoke();
        this->close();
    });

    this->updateStatusChanged(getApp()->getUpdates().getStatus());
    this->connections_.managedConnect(getApp()->getUpdates().statusUpdated,
                                      [this](auto status) {
                                          this->updateStatusChanged(status);
                                      });

    this->setScaleIndependentHeight(160);
    this->setScaleIndependentWidth(380);
}

void UpdateDialog::updateStatusChanged(Updates::Status status)
{
    this->ui_.installButton->setVisible(status == Updates::UpdateAvailable);
    this->ui_.installButton->setEnabled(status == Updates::UpdateAvailable);

    switch (status)
    {
        case Updates::None: {
            this->ui_.label->setText("Check for updates to load the latest "
                                     "Mergerino release information.");
            this->updateGeometry();
        }
        break;

        case Updates::Searching: {
            this->ui_.label->setText("Checking for Mergerino updates...");
            this->updateGeometry();
        }
        break;

        case Updates::UpdateAvailable: {
            this->ui_.label->setText(
                getApp()->getUpdates().buildUpdateAvailableText());
            this->updateGeometry();
        }
        break;

        case Updates::NoUpdateAvailable: {
            const auto latestVersion = getApp()->getUpdates().getOnlineVersion();
            if (latestVersion.isEmpty())
            {
                this->ui_.label->setText("Mergerino is up to date.");
            }
            else
            {
                this->ui_.label->setText(
                    QString("Mergerino is up to date.\n\nCurrent: %1\nLatest: "
                            "%2")
                        .arg(getApp()->getUpdates().getCurrentVersion(),
                             latestVersion));
            }
            this->updateGeometry();
        }
        break;

        case Updates::SearchFailed: {
            this->ui_.label->setText("Failed to load version information.");
            this->updateGeometry();
        }
        break;

        case Updates::Downloading: {
            this->ui_.label->setText(
                "Downloading updates.\n\nMergerino will restart "
                "automatically when the download is done.");
            this->updateGeometry();
        }
        break;

        case Updates::DownloadFailed: {
            this->ui_.label->setText("Failed to download the update.");
            this->updateGeometry();
        }
        break;

        case Updates::WriteFileFailed: {
            this->ui_.label->setText("Failed to save the update to disk.");
            this->updateGeometry();
        }
        break;

        case Updates::MissingPortableUpdater: {
            this->ui_.label->setText("The portable updater (expected in " %
                                     Updates::portableUpdaterPath() %
                                     ") was not found.");
            this->updateGeometry();
        }
        break;

        case Updates::RunUpdaterFailed: {
            this->ui_.label->setText("Failed to run the updater.");
            this->updateGeometry();
        }
        break;
    }
}

}  // namespace chatterino
