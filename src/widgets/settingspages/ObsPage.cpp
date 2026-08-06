// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/ObsPage.hpp"

#include "singletons/Settings.hpp"
#include "util/Clipboard.hpp"
#include "util/ObsBrowserDockServer.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace chatterino {

ObsPage::ObsPage()
    : view_(GeneralPageView::withoutNavigation(this))
{
    auto *outerLayout = new QVBoxLayout;
    auto *rowLayout = new QHBoxLayout;
    rowLayout->addWidget(this->view_);

    auto *frame = new QFrame;
    frame->setLayout(rowLayout);
    outerLayout->addWidget(frame);
    this->setLayout(outerLayout);

    addObsSettings(*this->view_);
    this->view_->addStretch();
}

bool ObsPage::filterElements(const QString &query)
{
    return this->view_ != nullptr &&
           (this->view_->filterElements(query) || query.isEmpty());
}

void addObsSettings(GeneralPageView &layout)
{
    auto &settings = *getSettings();

    layout.addTitle("OBS chat overlay");
    layout.addDescription(
        "Mergerino hosts a transparent browser source on this computer. "
        "Keep Mergerino open while streaming, then add the URL below to OBS "
        "as a Browser Source.");
    SettingWidget::checkbox("Enable OBS chat overlay",
                            settings.obsOverlayEnabled)
        ->addTo(layout);

    auto *urlRow = new QWidget;
    auto *urlLayout = new QHBoxLayout(urlRow);
    urlLayout->setContentsMargins(0, 0, 0, 0);

    auto *urlEdit = new QLineEdit(ObsBrowserDockServer::overlayUrl());
    urlEdit->setReadOnly(true);
    urlEdit->setToolTip(
        "Paste this local URL into an OBS Browser Source. It is available "
        "only while Mergerino is running.");
    urlLayout->addWidget(urlEdit, 1);

    auto *copyButton = new QPushButton("Copy URL");
    QObject::connect(copyButton, &QPushButton::clicked, copyButton,
                     [copyButton, urlEdit] {
                         crossPlatformCopy(urlEdit->text());
                         copyButton->setText("Copied");
                         QTimer::singleShot(1200, copyButton, [copyButton] {
                             copyButton->setText("Copy URL");
                         });
                     });
    urlLayout->addWidget(copyButton);

    auto *previewButton = new QPushButton("Open Preview");
    QObject::connect(previewButton, &QPushButton::clicked, previewButton, [] {
        QDesktopServices::openUrl(
            QUrl(ObsBrowserDockServer::overlayUrl() +
                 QStringLiteral("?preview=1")));
    });
    urlLayout->addWidget(previewButton);
    layout.addWidget(urlRow,
                     {"obs", "browser source", "overlay", "url", "copy",
                      "preview"});

    layout.addDescription(
        "In OBS: Sources, select Add, then Browser. Paste the URL and choose "
        "a canvas size such as 700 by 900. The browser page itself is always "
        "transparent; message cards are also fully transparent by default.");

    layout.addTitle("Chat source");
    std::vector<std::pair<QString, QVariant>> tabItems;
    const auto availableTabs = ObsBrowserDockServer::availableOverlayTabNames();
    for (const auto &tabName : availableTabs)
    {
        tabItems.emplace_back(tabName, QVariant(tabName));
    }

    const auto selectedTab = settings.obsOverlayTabName.getValue().trimmed();
    if (!selectedTab.isEmpty() &&
        !availableTabs.contains(selectedTab, Qt::CaseInsensitive))
    {
        tabItems.emplace_back(selectedTab, QVariant(selectedTab));
    }
    if (tabItems.empty())
    {
        tabItems.emplace_back("No chat tabs available", QVariant(QString()));
    }

    SettingWidget::dropdown("Chat tab", settings.obsOverlayTabName, tabItems)
        ->setTooltip(
            "Choose which existing Mergerino chat tab the overlay follows. "
            "By default Mergerino reuses the linked account's existing chat, "
            "preferring Twitch. A channel-named tab is created only when that "
            "account chat is not already open.")
        ->addTo(layout);

    layout.addTitle("Appearance");
    SettingWidget::fontButton(
        "Font", settings.obsOverlayFontFamily,
        [&settings] {
            return QFont(settings.obsOverlayFontFamily.getValue(),
                         settings.obsOverlayFontSize.getValue(),
                         settings.obsOverlayFontWeight.getValue());
        },
        [&settings](const QFont &font) {
            settings.obsOverlayFontFamily = font.family();
            settings.obsOverlayFontSize =
                std::max(8, font.pointSize() > 0 ? font.pointSize() : 28);
            settings.obsOverlayFontWeight =
                static_cast<int>(font.weight());
        })
        ->addTo(layout);
    SettingWidget::intInput("Font size", settings.obsOverlayFontSize,
                            {
                                .min = 8,
                                .max = 96,
                                .singleStep = 1,
                                .suffix = "px",
                            })
        ->addTo(layout);
    SettingWidget::intInput("Font weight", settings.obsOverlayFontWeight,
                            {
                                .min = 100,
                                .max = 900,
                                .singleStep = 100,
                            })
        ->addTo(layout);
    SettingWidget::colorButton("Text color", settings.obsOverlayTextColor)
        ->addTo(layout);
    SettingWidget::colorButton("Message background",
                               settings.obsOverlayBackgroundColor)
        ->addTo(layout);
    SettingWidget::intInput("Message background opacity",
                            settings.obsOverlayBackgroundOpacity,
                            {
                                .min = 0,
                                .max = 255,
                                .singleStep = 1,
                            })
        ->setTooltip("0 is fully transparent and 255 is fully opaque.")
        ->addTo(layout);
    SettingWidget::colorButton("Text shadow color",
                               settings.obsOverlayShadowColor)
        ->addTo(layout);
    SettingWidget::intInput("Text shadow opacity",
                            settings.obsOverlayShadowOpacity,
                            {
                                .min = 0,
                                .max = 255,
                                .singleStep = 1,
                            })
        ->addTo(layout);
    SettingWidget::intInput("Text shadow blur",
                            settings.obsOverlayShadowBlur,
                            {
                                .min = 0,
                                .max = 24,
                                .singleStep = 1,
                                .suffix = "px",
                            })
        ->addTo(layout);
    SettingWidget::intInput("Message spacing",
                            settings.obsOverlayMessageSpacing,
                            {
                                .min = 0,
                                .max = 40,
                                .singleStep = 1,
                                .suffix = "px",
                            })
        ->addTo(layout);
    SettingWidget::intInput("Rounded corners",
                            settings.obsOverlayBorderRadius,
                            {
                                .min = 0,
                                .max = 40,
                                .singleStep = 1,
                                .suffix = "px",
                            })
        ->addTo(layout);
    SettingWidget::intInput("Emote size", settings.obsOverlayEmoteSize,
                            {
                                .min = 12,
                                .max = 96,
                                .singleStep = 1,
                                .suffix = "px",
                            })
        ->addTo(layout);

    layout.addTitle("Behaviour");
    SettingWidget::intInput("Maximum visible messages",
                            settings.obsOverlayMaxMessages,
                            {
                                .min = 1,
                                .max = 50,
                                .singleStep = 1,
                            })
        ->addTo(layout);
    SettingWidget::intInput("Hide messages after",
                            settings.obsOverlayMessageLifetime,
                            {
                                .min = 0,
                                .max = 300,
                                .singleStep = 1,
                                .suffix = "s",
                            })
        ->setTooltip("Set to 0 to keep messages until they leave the limit.")
        ->addTo(layout);
    SettingWidget::intInput("Fade duration",
                            settings.obsOverlayFadeDuration,
                            {
                                .min = 0,
                                .max = 5000,
                                .singleStep = 100,
                                .suffix = "ms",
                            })
        ->addTo(layout);
    SettingWidget::checkbox("Message animations",
                            obsOverlayMessageAnimationsSetting())
        ->setTooltip(
            "Animate new messages and smoothly move existing messages when "
            "the chat advances.")
        ->addTo(layout);
    SettingWidget::checkbox("Newest messages at the bottom",
                            settings.obsOverlayNewestAtBottom)
        ->addTo(layout);

    layout.addTitle("Visible message elements");
    SettingWidget::checkbox("Use username colors",
                            settings.obsOverlayUseUsernameColors)
        ->addTo(layout);
    SettingWidget::checkbox("Show timestamps",
                            settings.obsOverlayShowTimestamps)
        ->addTo(layout);
    SettingWidget::dropdown(
        "Platform decoration", settings.obsOverlayPlatformStyle,
        {
            {"Logos", QVariant(QStringLiteral("logos"))},
            {"Accent line", QVariant(QStringLiteral("accent-line"))},
            {"None", QVariant(QStringLiteral("none"))},
        })
        ->setTooltip(
            "Choose platform logos beside usernames, a platform-colored line "
            "on the left of each message, or no platform decoration.")
        ->addTo(layout);
    SettingWidget::checkbox("Show user badges",
                            settings.obsOverlayShowBadges)
        ->addTo(layout);
    SettingWidget::checkbox("Show 7TV emotes",
                            settings.obsOverlayShowSevenTVEmotes)
        ->addTo(layout);
    SettingWidget::checkbox("Show 7TV badges",
                            settings.obsOverlayShowSevenTVBadges)
        ->addTo(layout);
    SettingWidget::checkbox("Show 7TV username paints",
                            settings.obsOverlayShowSevenTVPaints)
        ->setTooltip(
            "Uses Mergerino's loaded 7TV cosmetics for Twitch and Kick "
            "usernames. Animated paints remain animated in the browser "
            "overlay.")
        ->addTo(layout);
    SettingWidget::checkbox("Show highlight colors",
                            settings.obsOverlayShowHighlights)
        ->addTo(layout);
    SettingWidget::checkbox("Show system and event messages",
                            settings.obsOverlayShowSystemMessages)
        ->setTooltip(
            "Shows connection and joined-chat notices, users joined/parted "
            "lists, stream live/offline and app status notices, emote-set "
            "updates, subscriptions and gifts, cheers/bits, paid messages, "
            "channel-point rewards, and watch-streak events. Ordinary viewer "
            "chat remains visible when this is off. Moderation notices use "
            "the separate setting below.")
        ->addTo(layout);
    SettingWidget::checkbox("Show moderation messages",
                            settings.obsOverlayShowModerationMessages)
        ->addTo(layout);

}

}  // namespace chatterino
