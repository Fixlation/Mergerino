// SPDX-License-Identifier: MIT

#include "widgets/helper/ThemePreview.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"

#include <QFontMetrics>
#include <QPainter>

#include <array>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <variant>

namespace {

using namespace chatterino;

QColor readColor(const QJsonObject &colors,
                 std::initializer_list<const char *> path,
                 const QColor &fallback)
{
    QJsonValue value(colors);
    for (const auto *key : path)
    {
        value = value.toObject().value(QLatin1String(key));
    }
    const QColor color(value.toString());
    return color.isValid() ? color : fallback;
}

QColor mix(const QColor &from, const QColor &to, double amount)
{
    return QColor::fromRgbF(
        static_cast<float>(from.redF() + (to.redF() - from.redF()) * amount),
        static_cast<float>(from.greenF() +
                           (to.greenF() - from.greenF()) * amount),
        static_cast<float>(from.blueF() + (to.blueF() - from.blueF()) * amount));
}

QString previewAccountName(AccountController &accounts)
{
    const auto firstName = [](const auto &names) {
        for (const auto &name : names)
        {
            if (!name.trimmed().isEmpty())
            {
                return name;
            }
        }
        return QString{};
    };

    if (const auto account = accounts.twitch.getCurrent();
        account && !account->isAnon() && !account->getUserName().isEmpty())
    {
        return account->getUserName();
    }
    if (const auto name = firstName(accounts.twitch.getUsernames());
        !name.isEmpty())
    {
        return name;
    }
    if (const auto account = accounts.kick.current();
        account && !account->isAnonymous() && !account->username().isEmpty())
    {
        return account->username();
    }
    if (const auto name = firstName(accounts.kick.usernames()); !name.isEmpty())
    {
        return name;
    }
    if (const auto account = accounts.youtube.current();
        account && !account->isAnonymous() && !account->displayName().isEmpty())
    {
        return account->displayName();
    }
    if (const auto name = firstName(accounts.youtube.channelNames());
        !name.isEmpty())
    {
        return name;
    }
    return QStringLiteral("tester123");
}

enum class SampleEmote {
    None,
    Skull,
    TwitchMood,
};

struct SampleMessage {
    const char *time;
    const char *name;
    int nameColor;
    const char *text;
    SampleEmote emote = SampleEmote::None;
};

// Newest last; drawn bottom-up like a live chat. An empty name uses the account.
constexpr std::array<SampleMessage, 6> SAMPLE_MESSAGES{{
    {"12:03", "mossy", 3, "ggs everyone"},
    {"12:04", "pelican_", 0, "that clutch was insane"},
    {"12:04", "forsen", 1, "i forgor ", SampleEmote::Skull},
    {"12:05", nullptr, 0, "Raid incoming from 12 viewers"},
    {"12:05", "xqc", 2, "yo dud ", SampleEmote::TwitchMood},
    {"12:06", "", 4, "this theme looks clean"},
}};

constexpr std::array<const char *, 5> DARK_NAMES{
    "#ff8a5c", "#5aa9ff", "#ff6fb5", "#9bd64a", "#e8b84a"};
constexpr std::array<const char *, 5> LIGHT_NAMES{
    "#c2410c", "#1d5fd0", "#be185d", "#4d7c0f", "#a16207"};

}  // namespace

namespace chatterino {

ThemePreview::ThemePreview(QWidget *parent)
    : QWidget(parent)
{
    this->setAttribute(Qt::WA_OpaquePaintEvent);
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    this->clock_.start();
    QObject::connect(&this->frameTimer_, &QTimer::timeout, this, [this] {
        this->update();
    });
    auto *emotes = getApp()->getEmotes();
    const auto skull = emotes->getEmojis()->parse(QStringView(u"\U0001F480"));
    if (!skull.empty())
    {
        if (const auto *emote = std::get_if<EmotePtr>(&skull.front()))
        {
            this->skullEmote_ = *emote;
        }
    }
    this->moodEmote_ = emotes->getTwitchEmotes()->getOrCreateEmote(
        EmoteId{QStringLiteral("632233")}, EmoteName{QStringLiteral("xqcMood")});

    // Image loads request a global layout; repaint static previews as well.
    this->connections_.managedConnect(
        getApp()->getWindows()->layoutRequested, [this](Channel *channel) {
            if (channel == nullptr && this->isVisible())
            {
                this->update();
            }
        });
    auto *accounts = getApp()->getAccounts();
    const auto accountChanged = [this] {
        this->update();
    };
    accounts->twitch.currentUsername.connect(
        [accountChanged](const auto &) {
            accountChanged();
        },
        this->connections_, false);
    this->connections_.managedConnect(accounts->twitch.userListUpdated,
                                       accountChanged);
    this->connections_.managedConnect(accounts->twitch.currentUserNameChanged,
                                       accountChanged);
    this->connections_.managedConnect(accounts->kick.currentUserChanged,
                                       accountChanged);
    this->connections_.managedConnect(accounts->kick.userListUpdated,
                                       accountChanged);
    this->connections_.managedConnect(accounts->youtube.currentUserChanged,
                                       accountChanged);
    this->connections_.managedConnect(accounts->youtube.userListUpdated,
                                       accountChanged);
    this->setTheme(getTheme()->currentThemeJson());
}

QSize ThemePreview::sizeHint() const
{
    return {420, 176};
}

void ThemePreview::setTheme(const QJsonObject &theme)
{
    const auto *current = getTheme();
    const auto colors = theme.value("colors").toObject();
    Colors c;
    c.light = theme.value("metadata").toObject().value("iconTheme").toString() ==
              "dark";
    c.accent = readColor(colors, {"accent"}, current->accent);
    c.window = readColor(colors, {"window", "background"},
                         current->window.background);
    c.windowText =
        readColor(colors, {"window", "text"}, current->window.text);
    c.tab = readColor(colors, {"tabs", "regular", "backgrounds", "regular"},
                      current->tabs.regular.backgrounds.regular);
    c.tabText = readColor(colors, {"tabs", "regular", "text"},
                          current->tabs.regular.text);
    c.selectedTab =
        readColor(colors, {"tabs", "selected", "backgrounds", "regular"},
                  current->tabs.selected.backgrounds.regular);
    c.selectedTabText = readColor(colors, {"tabs", "selected", "text"},
                                  current->tabs.selected.text);
    c.newMessageTabText = readColor(colors, {"tabs", "newMessage", "text"},
                                    current->tabs.newMessage.text);
    c.header = readColor(colors, {"splits", "header", "focusedBackground"},
                         current->splits.header.focusedBackground);
    c.headerText = readColor(colors, {"splits", "header", "focusedText"},
                             current->splits.header.focusedText);
    c.headerBorder = readColor(colors, {"splits", "header", "focusedBorder"},
                               current->splits.header.focusedBorder);
    c.chat = readColor(colors, {"messages", "backgrounds", "regular"},
                       current->messages.backgrounds.regular);
    c.alternate = readColor(colors, {"messages", "backgrounds", "alternate"},
                            current->messages.backgrounds.alternate);
    c.text = readColor(colors, {"messages", "textColors", "regular"},
                       current->messages.textColors.regular);
    c.system = readColor(colors, {"messages", "textColors", "system"},
                         current->messages.textColors.system);
    c.link = readColor(colors, {"messages", "textColors", "link"},
                       current->messages.textColors.link);
    c.input = readColor(colors, {"splits", "input", "background"},
                        current->splits.input.background);
    c.inputBorder = readColor(colors, {"splits", "input", "border"}, c.selectedTab);
    c.placeholder =
        readColor(colors, {"messages", "textColors", "chatPlaceholder"},
                  current->messages.textColors.chatPlaceholder);

    this->colors_ = c;
    this->effect_ = themeEffectFromTheme(theme);
    this->updateTimer();
    this->update();
}

void ThemePreview::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    this->updateTimer();
}

void ThemePreview::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    this->updateTimer();
}

void ThemePreview::updateTimer()
{
    if (this->effect_.isAnimated() && this->isVisible())
    {
        this->frameTimer_.start(this->effect_.frameInterval());
    }
    else
    {
        this->frameTimer_.stop();
    }
}

void ThemePreview::paintEvent(QPaintEvent * /*event*/)
{
    const auto &c = this->colors_;
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    QFont font = this->font();
    const auto family = getSettings()->chatFontFamily.getValue();
    if (!family.isEmpty())
    {
        font.setFamily(family);
    }
    font.setPixelSize(12);
    auto bold = font;
    bold.setBold(true);
    const QFontMetrics metrics(font);

    const auto bounds = this->rect();
    painter.fillRect(bounds, c.window);

    // Tab strip
    constexpr int stripHeight = 24;
    struct Tab {
        const char *name;
        QColor background;
        QColor text;
        QColor line;
    };
    const std::array<Tab, 3> tabs{{
        {"general", c.selectedTab, c.selectedTabText, c.accent},
        {"/whispers", c.tab, c.newMessageTabText, {}},
        {"/mentions", c.tab, c.tabText, {}},
    }};
    painter.setFont(font);
    int x = 4;
    for (const auto &tab : tabs)
    {
        const QString name = QString::fromLatin1(tab.name);
        const QRect area(x, 3, metrics.horizontalAdvance(name) + 22,
                         stripHeight - 3);
        painter.fillRect(area, tab.background);
        if (tab.line.isValid())
        {
            painter.fillRect(QRect(area.left(), area.top(), area.width(), 2),
                             tab.line);
        }
        painter.setPen(tab.text);
        painter.drawText(area, Qt::AlignCenter, name);
        x = area.right() + 2;
    }

    // Split header
    const QRect header(0, stripHeight, bounds.width(), 24);
    painter.fillRect(header, c.header);
    painter.fillRect(QRect(0, header.bottom(), bounds.width(), 1),
                     c.headerBorder);
    painter.setPen(c.headerText);
    painter.drawText(header, Qt::AlignCenter, QStringLiteral("general"));

    // Message box
    const QRect input(0, bounds.height() - 30, bounds.width(), 30);
    const QRect chat(0, header.bottom() + 1, bounds.width(),
                     input.top() - header.bottom() - 1);

    if (this->effect_.isAnimated())
    {
        this->backdrop_.paint(painter, chat, chat.topLeft(), bounds.size(),
                              this->effect_, c.chat,
                              double(this->clock_.elapsed()) / 1000.0);
    }
    else
    {
        painter.fillRect(chat, c.chat);
    }

    auto alternate = c.alternate;
    if (this->effect_.isAnimated())
    {
        alternate = c.text;
        alternate.setAlpha(12);
    }

    painter.save();
    painter.setClipRect(chat);
    const auto lineHeight = metrics.height() + 5;
    const auto &names = c.light ? LIGHT_NAMES : DARK_NAMES;
    auto y = chat.bottom() + 1 - 4;
    for (auto it = SAMPLE_MESSAGES.rbegin(); it != SAMPLE_MESSAGES.rend(); ++it)
    {
        y -= lineHeight;
        if (y < chat.top())
        {
            break;
        }
        const auto row = std::distance(SAMPLE_MESSAGES.rbegin(), it);
        if (row % 2 == 1)
        {
            painter.fillRect(QRect(0, y, bounds.width(), lineHeight),
                             alternate);
        }
        const auto baseline = y + (lineHeight + metrics.ascent() -
                                   metrics.descent()) /
                                      2;
        int cursor = 8;
        const auto drawRun = [&](const QString &text, const QColor &color,
                                 const QFont &runFont) {
            painter.setFont(runFont);
            painter.setPen(color);
            painter.drawText(cursor, baseline, text);
            cursor += QFontMetrics(runFont).horizontalAdvance(text);
        };

        drawRun(QString::fromLatin1(it->time) + ' ', c.system, font);
        if (it->name == nullptr)
        {
            drawRun(QString::fromLatin1(it->text), c.system, font);
            continue;
        }
        const auto name = it->name[0] == '\0'
                              ? previewAccountName(*getApp()->getAccounts())
                              : QString::fromLatin1(it->name);
        drawRun(name, QColor(QString::fromLatin1(names[it->nameColor])), bold);
        drawRun(QStringLiteral(": ") + QString::fromLatin1(it->text), c.text,
                font);
        if (it->emote != SampleEmote::None)
        {
            const auto &emote = it->emote == SampleEmote::Skull
                                    ? this->skullEmote_
                                    : this->moodEmote_;
            std::optional<QPixmap> pixmap;
            if (emote)
            {
                const auto &image = emote->images.getImageOrLoaded(
                    static_cast<float>(this->devicePixelRatioF()));
                pixmap = image->pixmapOrLoad();
            }
            if (pixmap)
            {
                QSizeF size(pixmap->size());
                size.scale(lineHeight - 2, lineHeight - 2,
                           Qt::KeepAspectRatio);
                const QRectF target(cursor, y + (lineHeight - size.height()) / 2,
                                     size.width(), size.height());
                painter.drawPixmap(target, *pixmap, pixmap->rect());
            }
            else
            {
                drawRun(it->emote == SampleEmote::Skull
                            ? QStringLiteral("\U0001F480")
                            : QStringLiteral("xqcMood"),
                        c.text, font);
            }
        }
    }
    painter.restore();

    painter.fillRect(input, c.input);
    painter.fillRect(QRect(0, input.top(), bounds.width(), 1), c.inputBorder);
    painter.setFont(font);
    painter.setPen(c.placeholder);
    painter.drawText(input.adjusted(10, 0, -10, 0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Send message..."));

    // Window frame
    painter.setPen(mix(c.window, c.windowText, 0.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bounds.adjusted(0, 0, -1, -1));
}

}  // namespace chatterino
