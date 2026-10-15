// SPDX-License-Identifier: MIT

#include "widgets/settingspages/ThemesPage.hpp"

#include "Application.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/dialogs/ColorPickerDialog.hpp"
#include "widgets/helper/FontSettingWidget.hpp"
#include "widgets/helper/color/ColorButton.hpp"
#include "widgets/helper/ThemeBackdrop.hpp"
#include "widgets/helper/ThemePreview.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <pajlada/settings/settinglistener.hpp>
#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHideEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

namespace {

using namespace chatterino;

constexpr auto SHARE_CODE_PREFIX = "MGR1.";
constexpr int THEME_PALETTE_ROLE = Qt::UserRole + 1;
constexpr int THEME_PREVIEW_ROLE = Qt::UserRole + 2;
constexpr int THEME_PALETTE_SIZE = 20;
constexpr int ANIMATED_PALETTE_WIDTH = 84;
constexpr int ANIMATED_PALETTE_HEIGHT = 24;
constexpr int THEME_PALETTE_MARGIN = 6;

/// Black or white, whichever reads better on `background`
QColor readableOn(const QColor &background)
{
    const auto luminance = [](const QColor &color) {
        const auto linear = [](double value) {
            return value <= 0.04045 ? value / 12.92
                                    : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
               0.0722 * linear(color.blueF());
    };
    // Contrast against white beats contrast against black above this point
    return luminance(background) > 0.179 ? QColor(Qt::black)
                                         : QColor(Qt::white);
}

bool hasThemePalette(const QStringList &colors)
{
    return colors.size() == 3 && QColor(colors[0]).isValid() &&
           QColor(colors[1]).isValid() && QColor(colors[2]).isValid();
}

QSize themePaletteSize(bool animated)
{
    return animated ? QSize(ANIMATED_PALETTE_WIDTH, ANIMATED_PALETTE_HEIGHT)
                    : QSize(THEME_PALETTE_SIZE, THEME_PALETTE_SIZE);
}

QRect themePaletteRect(const QRect &area, int rightMargin, bool animated)
{
    auto size = themePaletteSize(animated);
    size.setHeight(std::min(size.height(), std::max(1, area.height() - 2)));
    return {area.right() - rightMargin - size.width() + 1,
            area.center().y() - size.height() / 2, size.width(), size.height()};
}

void paintThemePalette(QPainter &painter, const QRect &rect,
                       const QStringList &colors, const QColor &frame)
{
    if (!hasThemePalette(colors))
    {
        return;
    }

    painter.fillRect(rect, frame);
    const auto inner = rect.adjusted(1, 1, -1, -1);
    auto left = inner.left();
    for (int index = 0; index < colors.size(); ++index)
    {
        const auto right =
            index == colors.size() - 1
                ? inner.right()
                : inner.left() + (index + 1) * inner.width() / colors.size() -
                      1;
        painter.fillRect(QRect(QPoint(left, inner.top()),
                               QPoint(right, inner.bottom())),
                         QColor(colors[index]));
        left = right + 1;
    }
}

void paintThemeSwatch(QPainter &painter, const QRect &rect,
                      const QStringList &colors, const QColor &frame,
                      const QJsonObject &preview, ThemeBackdrop &backdrop,
                      double seconds)
{
    const auto effect = themeEffectFromTheme(preview);
    if (!effect.isAnimated())
    {
        paintThemePalette(painter, rect, colors, frame);
        return;
    }

    painter.fillRect(rect, frame);
    const auto inner = rect.adjusted(1, 1, -1, -1);
    const auto stripWidth = std::max(8, inner.width() / 5);
    const QRect animation(inner.topLeft(),
                          QSize(inner.width() - stripWidth - 1, inner.height()));
    const QRect strip(animation.right() + 2, inner.top(), stripWidth,
                      inner.height());
    QLinearGradient gradient(strip.topLeft(), strip.bottomLeft());
    for (int i = 0; i < 3; ++i)
    {
        gradient.setColorAt(i / 2.0, effect.colors[i]);
    }
    painter.fillRect(strip, gradient);

    const auto surface = QColor(preview.value("colors")
                                    .toObject()
                                    .value("messages")
                                    .toObject()
                                    .value("backgrounds")
                                    .toObject()
                                    .value("regular")
                                    .toString());
    // Render the real backdrop at a useful miniature scale for its particles.
    const QSize canvas(168, 64);
    painter.save();
    painter.setClipRect(animation, Qt::IntersectClip);
    painter.translate(animation.topLeft());
    painter.scale(double(animation.width()) / canvas.width(),
                  double(animation.height()) / canvas.height());
    backdrop.paint(painter, QRect(QPoint(), canvas), {}, canvas, effect,
                   surface.isValid() ? surface : QColor("#181818"), seconds);
    painter.restore();
}

class ThemePaletteDelegate final : public QStyledItemDelegate
{
public:
    explicit ThemePaletteDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
        this->clock_.start();
    }

    void clearPreviews()
    {
        this->backdrops_.clear();
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto colors = index.data(THEME_PALETTE_ROLE).toStringList();
        if (!hasThemePalette(colors))
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const auto preview = index.data(THEME_PREVIEW_ROLE).toJsonObject();

        QStyleOptionViewItem styledOption(option);
        this->initStyleOption(&styledOption, index);
        const auto *style = styledOption.widget != nullptr
                                ? styledOption.widget->style()
                                : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &styledOption,
                             painter, styledOption.widget);

        const auto paletteRect = themePaletteRect(
            styledOption.rect, THEME_PALETTE_MARGIN, !preview.isEmpty());
        const auto textRect = styledOption.rect.adjusted(
            6, 0, -(paletteRect.width() + THEME_PALETTE_MARGIN * 2), 0);
        const auto selected =
            styledOption.state.testFlag(QStyle::State_Selected);
        const auto enabled =
            styledOption.state.testFlag(QStyle::State_Enabled);
        const auto text = styledOption.fontMetrics.elidedText(
            styledOption.text, Qt::ElideRight, textRect.width());

        painter->save();
        painter->setFont(styledOption.font);
        style->drawItemText(painter, textRect,
                            Qt::AlignLeft | Qt::AlignVCenter,
                            styledOption.palette, enabled, text,
                            selected ? QPalette::HighlightedText
                                     : QPalette::Text);
        auto &backdrop = this->backdrops_[index.data(Qt::UserRole).toString()];
        paintThemeSwatch(*painter, paletteRect, colors,
                         styledOption.palette.color(QPalette::Mid), preview,
                         backdrop, double(this->clock_.elapsed()) / 1000.0);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (hasThemePalette(index.data(THEME_PALETTE_ROLE).toStringList()))
        {
            const auto palette = themePaletteSize(
                !index.data(THEME_PREVIEW_ROLE).toJsonObject().isEmpty());
            size.rwidth() += palette.width() + THEME_PALETTE_MARGIN * 2;
            size.setHeight(std::max(size.height(), palette.height() + 4));
        }
        return size;
    }

private:
    QElapsedTimer clock_;
    mutable std::map<QString, ThemeBackdrop> backdrops_;
};

struct SharedTheme {
    QString name;
    QJsonObject theme;
    QString fontFamily;
    int fontSize{};
    int fontWeight{};
};

class DownwardComboBox final : public QComboBox
{
public:
    explicit DownwardComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        this->clock_.start();
        this->delegate_ = new ThemePaletteDelegate(this);
        this->setItemDelegate(this->delegate_);
        this->frameTimer_.setInterval(50);
        QObject::connect(&this->frameTimer_, &QTimer::timeout, this, [this] {
            this->updateFrameTimer();
            if (this->frameTimer_.isActive())
            {
                this->update();
                if (this->view()->isVisible())
                {
                    this->view()->viewport()->update();
                }
            }
        });
        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this,
                         [this] {
                             this->updateFrameTimer();
                         });
    }

    void refreshSwatches()
    {
        this->delegate_->clearPreviews();
        this->hasAnimatedItems_ = false;
        for (int index = 0; index < this->count(); ++index)
        {
            this->hasAnimatedItems_ |=
                !this->itemData(index, THEME_PREVIEW_ROLE).toJsonObject().isEmpty();
        }
        this->updateFrameTimer();
        this->update();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QComboBox::showEvent(event);
        this->updateFrameTimer();
    }

    void hideEvent(QHideEvent *event) override
    {
        QComboBox::hideEvent(event);
        this->updateFrameTimer();
    }

    void hidePopup() override
    {
        QComboBox::hidePopup();
        this->updateFrameTimer();
    }

    void paintEvent(QPaintEvent *event) override
    {
        this->updateFrameTimer();
        const auto preview =
            this->currentData(THEME_PREVIEW_ROLE).toJsonObject();
        const auto colors =
            this->itemData(this->currentIndex(), THEME_PALETTE_ROLE)
                .toStringList();
        if (!hasThemePalette(colors))
        {
            QComboBox::paintEvent(event);
            return;
        }

        QStyleOptionComboBox option;
        this->initStyleOption(&option);
        QPainter painter(this);
        this->style()->drawComplexControl(QStyle::CC_ComboBox, &option,
                                          &painter, this);

        const auto field = this->style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        const auto paletteRect =
            themePaletteRect(field, 2, !preview.isEmpty());
        auto labelOption = option;
        labelOption.rect.setRight(paletteRect.left() - 2);
        this->style()->drawControl(QStyle::CE_ComboBoxLabel, &labelOption,
                                   &painter, this);
        paintThemeSwatch(painter, paletteRect, colors,
                         option.palette.color(QPalette::Mid), preview,
                         this->backdrop_, double(this->clock_.elapsed()) / 1000.0);
    }

    void showPopup() override
    {
        QComboBox::showPopup();
        this->updateFrameTimer();
        QTimer::singleShot(0, this, [this] {
            auto *popup = this->view()->window();
            if (popup == nullptr)
            {
                return;
            }

            this->view()->scrollToTop();
            const auto below = this->mapToGlobal(QPoint(0, this->height()));
            auto geometry = popup->geometry();
            if (const auto *targetScreen = this->screen())
            {
                const auto available = targetScreen->availableGeometry();
                geometry.setWidth(std::min(
                    std::max(geometry.width(), this->width()),
                    available.width()));
                geometry.setHeight(std::min(
                    geometry.height(),
                    std::max(1, available.bottom() - below.y() + 1)));
                geometry.moveLeft(std::clamp(
                    below.x(), available.left(),
                    available.right() - geometry.width() + 1));
            }
            geometry.moveTop(below.y());
            popup->setGeometry(geometry);
        });
    }

private:
    void updateFrameTimer()
    {
        const bool popupVisible = this->view()->isVisible();
        const bool selectedAnimated =
            !this->currentData(THEME_PREVIEW_ROLE).toJsonObject().isEmpty();
        const bool visible = this->isVisible() &&
                             (!this->visibleRegion().isEmpty() || popupVisible);
        if (visible && (selectedAnimated ||
                        (popupVisible && this->hasAnimatedItems_)) &&
            QApplication::applicationState() == Qt::ApplicationActive)
        {
            if (!this->frameTimer_.isActive())
            {
                this->frameTimer_.start();
            }
        }
        else
        {
            this->frameTimer_.stop();
        }
    }

    ThemePaletteDelegate *delegate_{};
    bool hasAnimatedItems_{};
    ThemeBackdrop backdrop_;
    QElapsedTimer clock_;
    QTimer frameTimer_;
};

QString makeShareCode(const Theme &theme, const Settings &settings)
{
    const QJsonObject payload{
        {"format", "mergerino-theme"},
        {"version", 1},
        {"name", theme.currentThemeDisplayName()},
        {"theme", theme.currentThemeJson()},
        {"font", QJsonObject{{"family", settings.chatFontFamily.getValue()},
                             {"size", settings.chatFontSize.getValue()},
                             {"weight", settings.chatFontWeight.getValue()}}},
    };
    const auto encoded = QJsonDocument(payload)
                             .toJson(QJsonDocument::Compact)
                             .toBase64(QByteArray::Base64UrlEncoding |
                                       QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(SHARE_CODE_PREFIX) +
           QString::fromLatin1(encoded);
}

std::optional<SharedTheme> parseShareCode(const QString &input,
                                          QString &errorMessage)
{
    auto code = input.trimmed();
    if (!code.startsWith(QLatin1String(SHARE_CODE_PREFIX)))
    {
        errorMessage = "This is not a Mergerino theme code.";
        return std::nullopt;
    }

    const auto decoded = QByteArray::fromBase64(
        code.mid(static_cast<int>(QLatin1String(SHARE_CODE_PREFIX).size()))
            .toLatin1(),
        QByteArray::Base64UrlEncoding);
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(decoded, &parseError);
    if (!document.isObject())
    {
        errorMessage = "The theme code is damaged or incomplete (" +
                       parseError.errorString() + ").";
        return std::nullopt;
    }

    const auto root = document.object();
    const auto theme = root["theme"].toObject();
    if (root["format"].toString() != QStringLiteral("mergerino-theme") ||
        root["version"].toInt() != 1 || theme["colors"].toObject().isEmpty())
    {
        errorMessage = "This theme code uses an unsupported format.";
        return std::nullopt;
    }

    const auto font = root["font"].toObject();
    auto name = root["name"].toString().trimmed().left(80);
    if (name.isEmpty())
    {
        name = QStringLiteral("Shared theme");
    }

    return SharedTheme{
        .name = name,
        .theme = theme,
        .fontFamily = font["family"].toString(),
        .fontSize = font["size"].toInt(),
        .fontWeight = font["weight"].toInt(),
    };
}

QWidget *actionRow(QWidget *parent,
                   const std::initializer_list<QPushButton *> &buttons)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    for (auto *button : buttons)
    {
        layout->addWidget(button);
    }
    layout->addStretch(1);
    return widget;
}

/// Background, text and accent for colour themes; background and the two
/// leading effect colours for animated themes.
QStringList themeSwatch(const Theme &themes, const QString &key)
{
    const auto seed = themes.seedForTheme(key);
    if (!seed)
    {
        return {};
    }
    QStringList colors{seed->surface.name(QColor::HexRgb)};
    const auto resolved = themes.resolvedThemeJson(key);
    const auto effect =
        resolved ? themeEffectFromTheme(*resolved) : ThemeEffect{};
    if (effect.isAnimated())
    {
        colors = {effect.colors[0].name(QColor::HexRgb),
                  effect.colors[1].name(QColor::HexRgb),
                  effect.colors[2].name(QColor::HexRgb)};
    }
    else
    {
        colors << seed->ink.name(QColor::HexRgb)
               << seed->accent.name(QColor::HexRgb);
    }
    return colors;
}

QWidget *themePickerRow(QWidget *parent, const QString &label, Theme &themes,
                        QStringSetting &setting, bool animated,
                        pajlada::Signals::SignalHolder &connections)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    auto *combo = new DownwardComboBox(widget);
    combo->setPlaceholderText(animated ? "Choose an animated theme"
                                       : "Choose a colour theme");
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(label + ":", widget));
    layout->addStretch(1);
    layout->addWidget(combo);

    const auto selectValue = [combo](const QString &value) {
        // Themes of the other kind leave this picker on its placeholder
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(combo->findData(value));
    };
    const auto refresh = [&themes, &setting, combo, animated] {
        auto available = themes.availableThemes();
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto &[name, value] : available)
        {
            const auto themeKey = value.toString();
            if (themes.isAnimatedTheme(themeKey) != animated)
            {
                continue;
            }
            combo->addItem(name, value);
            if (themeKey == "Dark")
            {
                continue;
            }
            const auto swatch = themeSwatch(themes, themeKey);
            if (!swatch.isEmpty())
            {
                combo->setItemData(combo->count() - 1, swatch,
                                   THEME_PALETTE_ROLE);
                if (animated)
                {
                    if (const auto resolved = themes.resolvedThemeJson(themeKey))
                    {
                        combo->setItemData(combo->count() - 1, *resolved,
                                           THEME_PREVIEW_ROLE);
                    }
                }
            }
        }
        combo->setCurrentIndex(combo->findData(setting.getValue()));
        combo->setMinimumWidth(combo->minimumSizeHint().width() +
                               themePaletteSize(animated).width() +
                               THEME_PALETTE_MARGIN * 2);
        combo->refreshSwatches();
    };

    QObject::connect(combo, &QComboBox::currentIndexChanged, widget,
                     [combo, &setting](int index) {
                         if (index >= 0)
                         {
                             setting = combo->itemData(index).toString();
                         }
                     });
    setting.connect(selectValue, connections);
    connections.managedConnect(themes.updated, [refresh] { refresh(); });
    themes.customThemesJson.connect(
        [refresh](const auto &) {
            refresh();
        },
        connections);
    refresh();
    return widget;
}

/// Two-option segmented control, styled from the active theme
QWidget *segmentBar(QWidget *parent, QPushButton *first, QPushButton *second,
                    Theme &themes, pajlada::Signals::SignalHolder &connections)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(0);
    auto *group = new QButtonGroup(widget);
    group->setExclusive(true);
    for (auto *button : {first, second})
    {
        button->setParent(widget);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        group->addButton(button);
        layout->addWidget(button);
    }
    first->setObjectName("segmentFirst");
    second->setObjectName("segmentSecond");
    layout->addStretch(1);

    const auto restyle = [widget, &themes] {
        const auto border = themes.tabs.selected.backgrounds.regular;
        const auto idle = themes.tabs.regular.backgrounds.regular;
        const auto hover = themes.tabs.regular.backgrounds.hover;
        const auto accent = themes.accent;
        widget->setStyleSheet(
            QStringLiteral(
                "QPushButton { padding: 5px 18px; border: 1px solid %1;"
                " background: %2; color: %3; }"
                "QPushButton:hover:!checked { background: %4; }"
                "QPushButton:checked { background: %5; border-color: %5;"
                " color: %6; }"
                "#segmentFirst { border-top-left-radius: 5px;"
                " border-bottom-left-radius: 5px; }"
                "#segmentSecond { border-top-right-radius: 5px;"
                " border-bottom-right-radius: 5px; border-left: none; }")
                .arg(border.name(), idle.name(), themes.window.text.name(),
                     hover.name(), accent.name(),
                     readableOn(accent).name()));
    };
    connections.managedConnect(themes.updated, restyle);
    restyle();
    return widget;
}

QWidget *sliderRow(QWidget *parent, const QString &label, IntSetting &setting,
                   int minimum, int maximum,
                   const std::function<QString(int)> &format,
                   pajlada::Signals::SignalHolder &connections)
{
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    auto *name = new QLabel(label + ":", row);
    name->setMinimumWidth(145);
    auto *slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(minimum, maximum);
    slider->setSingleStep(5);
    slider->setPageStep(10);
    slider->setAccessibleName(label);
    auto *value = new QLabel(row);
    value->setMinimumWidth(44);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    rowLayout->addWidget(name);
    rowLayout->addWidget(slider, 1);
    rowLayout->addWidget(value);

    setting.connect(
        [slider, value, format](int current) {
            const QSignalBlocker blocker(slider);
            slider->setValue(current);
            value->setText(format(current));
        },
        connections);
    QObject::connect(slider, &QSlider::valueChanged, row,
                     [&setting, value, format](int current) {
                         value->setText(format(current));
                         setting = current;
                     });
    return row;
}

/// Colours each effect starts with; they match the built-in animated themes
std::array<const char *, 3> defaultEffectColors(ThemeEffectKind kind)
{
    switch (kind)
    {
        case ThemeEffectKind::Drift:
            return {"#ff7a59", "#d9467a", "#f2b35c"};
        case ThemeEffectKind::Starfield:
            return {"#f4f6ff", "#8fb2ff", "#ffd59a"};
        case ThemeEffectKind::Embers:
            return {"#ff7a2e", "#ffb54a", "#ff4b2b"};
        case ThemeEffectKind::Rain:
            return {"#a9c4e0", "#5d87b3", "#e6f0fa"};
        case ThemeEffectKind::Aurora:
        case ThemeEffectKind::None:
            break;
    }
    return {"#2fe39a", "#22b8d6", "#4f7dff"};
}

QWidget *effectKindRow(QWidget *parent, Theme &themes,
                       pajlada::Signals::SignalHolder &connections)
{
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    auto *name = new QLabel("Effect:", row);
    name->setMinimumWidth(145);
    auto *combo = new QComboBox(row);
    for (const auto &[label, kind] : themeEffectKinds())
    {
        combo->addItem(label, themeEffectKindKey(kind));
    }
    rowLayout->addWidget(name);
    rowLayout->addStretch(1);
    rowLayout->addWidget(combo);

    themes.customThemeEffect.connect(
        [combo](const QString &key) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(std::max(0, combo->findData(key)));
        },
        connections);
    QObject::connect(
        combo, &QComboBox::currentIndexChanged, row, [combo, &themes](int) {
            const auto previous =
                themeEffectKindFromKey(themes.customThemeEffect.getValue());
            const auto next = combo->currentData().toString();
            // Untouched colours follow the effect; edited ones are kept
            const auto oldDefaults = defaultEffectColors(previous);
            const std::array<QStringSetting *, 3> colors{
                &themes.customThemeEffectColor1,
                &themes.customThemeEffectColor2,
                &themes.customThemeEffectColor3,
            };
            bool untouched = true;
            for (size_t index = 0; index < colors.size(); ++index)
            {
                untouched = untouched &&
                            QColor(colors[index]->getValue()) ==
                                QColor(oldDefaults[index]);
            }
            themes.customThemeEffect = next;
            if (untouched)
            {
                const auto newDefaults =
                    defaultEffectColors(themeEffectKindFromKey(next));
                for (size_t index = 0; index < colors.size(); ++index)
                {
                    *colors[index] = QString::fromLatin1(newDefaults[index]);
                }
            }
        });
    return row;
}

using AdvancedColorSetting = std::pair<QString, QStringSetting *>;

QString editableColorValue(const QColor &color)
{
    return color.name(color.alpha() == 255 ? QColor::HexRgb
                                           : QColor::HexArgb);
}

QWidget *advancedColorEditor(
    QWidget *parent, const QString &label, QStringSetting &setting,
    pajlada::Signals::SignalHolder &connections)
{
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    auto *name = new QLabel(label + ":", row);
    name->setMinimumWidth(145);
    auto *swatch = new ColorButton(QColor(setting.getValue()), row);
    swatch->setFixedWidth(54);
    swatch->setAccessibleName("Choose " + label.toLower() + " colour");
    swatch->setToolTip("Click to open the colour picker.");
    auto *value = new QLineEdit(row);
    value->setMaximumWidth(125);
    value->setMaxLength(9);
    value->setPlaceholderText("#RRGGBB");
    value->setToolTip(
        "Enter #RRGGBB or #AARRGGBB, or use Choose colour.");
    auto *choose = new QPushButton("Choose colour...", row);

    rowLayout->addWidget(name);
    rowLayout->addStretch(1);
    rowLayout->addWidget(swatch);
    rowLayout->addWidget(value);
    rowLayout->addWidget(choose);

    const auto showValue = [swatch, value](const QString &stored) {
        const QColor color(stored);
        swatch->setColor(color);
        value->setText(color.isValid() ? editableColorValue(color) : stored);
        value->setStyleSheet(color.isValid()
                                 ? QString()
                                 : QStringLiteral("border: 1px solid #d94b4b;"));
    };
    setting.connect(showValue, connections);
    showValue(setting.getValue());

    QObject::connect(value, &QLineEdit::textChanged, row,
                     [value](const QString &text) {
                         value->setStyleSheet(
                             QColor(text.trimmed()).isValid()
                                 ? QString()
                                 : QStringLiteral(
                                       "border: 1px solid #d94b4b;"));
                     });
    QObject::connect(value, &QLineEdit::editingFinished, row,
                     [value, &setting] {
                         const QColor color(value->text().trimmed());
                         if (color.isValid())
                         {
                             setting = editableColorValue(color);
                         }
                         else
                         {
                             value->setText(setting.getValue());
                         }
                     });

    const auto openPicker = [row, &setting] {
        auto *dialog = new ColorPickerDialog(QColor(setting.getValue()), row);
        QObject::connect(
            dialog, &ColorPickerDialog::colorConfirmed, row,
            [&setting](const QColor &selected) {
                if (selected.isValid())
                {
                    setting = editableColorValue(selected);
                }
            });
        dialog->show();
    };
    QObject::connect(swatch, &ColorButton::clicked, row, openPicker);
    QObject::connect(choose, &QPushButton::clicked, row, openPicker);

    return row;
}

QGroupBox *advancedSection(
    QWidget *parent, const QString &title, const QString &description,
    BoolSetting &enabled,
    const std::initializer_list<AdvancedColorSetting> &colors,
    pajlada::Signals::SignalHolder &connections)
{
    auto *group = new QGroupBox("Customise " + title.toLower(), parent);
    group->setCheckable(true);
    group->setChecked(enabled.getValue());
    group->setToolTip(
        "Checked: use these colours. Unchecked: inherit from the base theme.");
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(12, 10, 12, 12);
    groupLayout->setSpacing(7);

    auto *descriptionLabel = new QLabel(description, group);
    descriptionLabel->setWordWrap(true);
    descriptionLabel->setProperty("muted", true);
    groupLayout->addWidget(descriptionLabel);

    auto *inheritanceLabel =
        new QLabel("Unchecked = inherit this section from the base theme.",
                   group);
    inheritanceLabel->setProperty("muted", true);
    groupLayout->addWidget(inheritanceLabel);

    for (const auto &[label, setting] : colors)
    {
        groupLayout->addWidget(
            advancedColorEditor(group, label, *setting, connections));
    }

    QObject::connect(group, &QGroupBox::toggled, group,
                     [&enabled](bool checked) {
                         enabled = checked;
                     });
    enabled.connect(
        [group](bool checked) {
            const QSignalBlocker blocker(group);
            group->setChecked(checked);
        },
        connections);
    return group;
}

}  // namespace

namespace chatterino {

ThemesPage::ThemesPage()
{
    auto *outer = new QVBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;
    outer->addWidget(view);
    this->setLayout(outer);

    this->initLayout(*view);
}

ThemesPage::~ThemesPage() = default;

bool ThemesPage::filterElements(const QString &query)
{
    return this->view_ != nullptr &&
           (this->view_->filterElements(query) || query.isEmpty());
}

void ThemesPage::initLayout(GeneralPageView &layout)
{
    auto *themes = getApp()->getThemes();
    auto &settings = *getSettings();

    layout.addTitle("Appearance");
    layout.addDescription(
        "Static themes recolour the interface. Animated themes also paint a "
        "slow, subtle backdrop behind your chats. Mode controls light or dark "
        "backgrounds and keeps your theme selected in either.");

    auto *staticTab = new QPushButton("Static themes");
    auto *animatedTab = new QPushButton("Animated themes");
    layout.addWidget(segmentBar(this, staticTab, animatedTab, *themes,
                                this->managedConnections_),
                     {"static", "animated", "animation", "theme"});

    auto *pickers = new QStackedWidget(this);
    pickers->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    pickers->addWidget(themePickerRow(pickers, "Colour theme", *themes,
                                      themes->themeName, false,
                                      this->managedConnections_));
    pickers->addWidget(themePickerRow(pickers, "Animated theme", *themes,
                                      themes->themeName, true,
                                      this->managedConnections_));
    layout.addWidget(pickers, {"theme", "preset", "appearance", "color",
                               "colour", "animated", "animation"});
    if (themes->isSystemTheme())
    {
        // Keep the visible appearance when retiring the System option.
        themes->appearanceMode = themes->isLightTheme() ? "Light" : "Dark";
    }
    SettingWidget::dropdown("Mode", themes->appearanceMode,
                            {{"Dark", "Dark"}, {"Light", "Light"}})
        ->addTo(layout);

    auto *currentPreview = new ThemePreview(this);
    currentPreview->setToolTip("Preview of the active theme");
    layout.addWidget(currentPreview, {"preview", "theme", "animated"});
    this->managedConnections_.managedConnect(
        themes->updated, [currentPreview, themes] {
            currentPreview->setTheme(themes->currentThemeJson());
        });

    auto *resetTheme = new QPushButton("Reset theme", this);
    resetTheme->setToolTip(
        "Restore Default colour theme and Dark mode. Saved custom themes "
        "and typography are kept.");
    QObject::connect(resetTheme, &QPushButton::clicked, this, [themes] {
        themes->appearanceMode = "Dark";
        themes->themeName = "Dark";
        themes->update();
    });
    layout.addWidget(actionRow(this, {resetTheme}),
                     {"reset", "default", "theme", "appearance"});

    layout.addTitle("Custom theme");
    auto *customStatic = new QPushButton("Static");
    auto *customAnimated = new QPushButton("Animated");
    layout.addWidget(segmentBar(this, customStatic, customAnimated, *themes,
                                this->managedConnections_),
                     {"custom theme", "static", "animated", "type"});
    auto *customDescription = layout.addDescription({});
    auto *loadButton = new QPushButton("Start from current theme", this);
    loadButton->setToolTip(
        "Copy the active theme into this editor, then edit and preview it "
        "before saving.");
    layout.addWidget(actionRow(this, {loadButton}),
                     {"custom theme", "load", "current", "copy"});
    SettingWidget::lineEdit("Name", themes->customThemeName, "My theme")
        ->addTo(layout);
    layout.addSubtitle("Base colours");
    layout.addWidget(advancedColorEditor(
                         this, "Background", themes->customThemeSurface,
                         this->managedConnections_),
                     {"background", "color", "picker", "custom theme"});
    layout.addWidget(advancedColorEditor(
                         this, "Text", themes->customThemeInk,
                         this->managedConnections_),
                     {"text", "color", "picker", "custom theme"});
    layout.addWidget(advancedColorEditor(
                         this, "Accent", themes->customThemeAccent,
                         this->managedConnections_),
                     {"accent", "color", "picker", "custom theme"});

    // Shown only for animated themes. The search filter shows registered
    // widgets, so the wrapper is registered and the group inside is toggled.
    auto *animationWrapper = new QWidget(this);
    auto *animationWrapperLayout = new QVBoxLayout(animationWrapper);
    animationWrapperLayout->setContentsMargins(0, 4, 0, 4);
    auto *animationEditor = new QGroupBox("Animation", animationWrapper);
    animationWrapperLayout->addWidget(animationEditor);
    auto *animationLayout = new QVBoxLayout(animationEditor);
    animationLayout->setContentsMargins(12, 10, 12, 12);
    animationLayout->setSpacing(7);
    auto *animationHint = new QLabel(
        "The effect is painted behind messages in your chat splits. It pauses "
        "in the background when \"Animate only when Mergerino is focused\" is "
        "enabled under Emotes.",
        animationEditor);
    animationHint->setWordWrap(true);
    animationHint->setProperty("muted", true);
    animationLayout->addWidget(animationHint);
    animationLayout->addWidget(
        effectKindRow(animationEditor, *themes, this->managedConnections_));
    animationLayout->addWidget(advancedColorEditor(
        animationEditor, "Colour 1", themes->customThemeEffectColor1,
        this->managedConnections_));
    animationLayout->addWidget(advancedColorEditor(
        animationEditor, "Colour 2", themes->customThemeEffectColor2,
        this->managedConnections_));
    animationLayout->addWidget(advancedColorEditor(
        animationEditor, "Colour 3", themes->customThemeEffectColor3,
        this->managedConnections_));
    animationLayout->addWidget(sliderRow(
        animationEditor, "Speed", themes->customThemeEffectSpeed,
        qRound(ThemeEffect::MIN_SPEED * 100),
        qRound(ThemeEffect::MAX_SPEED * 100),
        [](int value) {
            return QString::number(value / 100.0, 'f', 2) + QChar(0x00D7);
        },
        this->managedConnections_));
    animationLayout->addWidget(sliderRow(
        animationEditor, "Intensity", themes->customThemeEffectIntensity, 10,
        100,
        [](int value) {
            return QString::number(value) + '%';
        },
        this->managedConnections_));
    layout.addWidget(animationWrapper,
                     {"animated", "animation", "effect", "aurora", "drift",
                      "starfield", "embers", "rain", "speed", "intensity"});

    layout.addSubtitle("Preview");
    auto *draftPreview = new ThemePreview(this);
    draftPreview->setToolTip("Preview of the theme you are editing");
    layout.addWidget(draftPreview, {"preview", "custom theme", "draft"});
    auto *createButton = new QPushButton("Create static theme", this);
    auto *updateButton = new QPushButton("Update selected", this);
    auto *deleteButton = new QPushButton("Delete selected", this);
    layout.addWidget(actionRow(this, {createButton, updateButton, deleteButton}),
                     {"custom", "create", "update", "delete", "save"});

    SettingWidget::checkbox("Advanced colours",
                            themes->customThemeAdvanced)
        ->setDescription(
            "Optional colour overrides for individual interface sections.")
        ->addTo(layout);

    auto *advancedWrapper = new QWidget(this);
    auto *advancedWrapperLayout = new QVBoxLayout(advancedWrapper);
    advancedWrapperLayout->setContentsMargins(0, 0, 0, 0);
    auto *advancedEditor = new QWidget(advancedWrapper);
    advancedWrapperLayout->addWidget(advancedEditor);
    auto *advancedLayout = new QVBoxLayout(advancedEditor);
    advancedLayout->setContentsMargins(0, 4, 0, 4);
    advancedLayout->setSpacing(10);
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Accent",
        "Pick the shared highlight and active-indicator colour. A "
        "section-specific accent below takes priority.",
        themes->customThemeAdvancedAccentEnabled,
        {{"Accent", &themes->customThemeAdvancedAccent}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Windows and popups",
        "Choose the background and text colours for the main window and "
        "ordinary popups.",
        themes->customThemeAdvancedWindowEnabled,
        {{"Background", &themes->customThemeAdvancedWindowBackground},
         {"Text", &themes->customThemeAdvancedWindowText}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Chat messages",
        "Choose every colour used by the message list. This does not change "
        "the message entry box.",
        themes->customThemeAdvancedChatEnabled,
        {{"Background", &themes->customThemeAdvancedChatBackground},
         {"Alternating row", &themes->customThemeAdvancedChatAlternate},
         {"Text", &themes->customThemeAdvancedChatText},
         {"System text", &themes->customThemeAdvancedChatSystemText},
         {"Links", &themes->customThemeAdvancedChatLink}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Message box",
        "Choose the colours for the composer where you type chat messages.",
        themes->customThemeAdvancedInputEnabled,
        {{"Background", &themes->customThemeAdvancedInputBackground},
         {"Text", &themes->customThemeAdvancedInputText},
         {"Border and selection", &themes->customThemeAdvancedInputBorder}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Channel header",
        "Choose the colours for the strip above each chat, including its "
        "focused state.",
        themes->customThemeAdvancedHeaderEnabled,
        {{"Background", &themes->customThemeAdvancedHeaderBackground},
         {"Text", &themes->customThemeAdvancedHeaderText},
         {"Focused background",
          &themes->customThemeAdvancedHeaderFocusedBackground},
         {"Focused accent", &themes->customThemeAdvancedHeaderAccent}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Tabs",
        "Choose the tab, selected-tab, text and active-line colours.",
        themes->customThemeAdvancedTabsEnabled,
        {{"Background", &themes->customThemeAdvancedTabsBackground},
         {"Selected background",
          &themes->customThemeAdvancedTabsSelectedBackground},
         {"Text", &themes->customThemeAdvancedTabsText},
         {"Active line", &themes->customThemeAdvancedTabsAccent}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Usercards",
        "Choose usercard colours without recolouring other popup windows.",
        themes->customThemeAdvancedUsercardEnabled,
        {{"Background", &themes->customThemeAdvancedUsercardBackground},
         {"Text", &themes->customThemeAdvancedUsercardText},
         {"Secondary text",
          &themes->customThemeAdvancedUsercardSecondaryText},
         {"Accent", &themes->customThemeAdvancedUsercardAccent}},
        this->managedConnections_));
    advancedLayout->addWidget(advancedSection(
        advancedEditor, "Scrollbars",
        "Choose the scrollbar track, thumb and hovered-thumb colours.",
        themes->customThemeAdvancedScrollbarsEnabled,
        {{"Track", &themes->customThemeAdvancedScrollbarBackground},
         {"Thumb", &themes->customThemeAdvancedScrollbarThumb},
         {"Hovered thumb",
          &themes->customThemeAdvancedScrollbarThumbSelected}},
        this->managedConnections_));
    layout.addWidget(
        advancedWrapper,
        {"advanced", "section", "accent", "chat", "message box", "tabs",
         "usercard", "header", "scrollbar", "window", "popup"});

    const auto showAdvanced = [advancedEditor](bool enabled) {
        advancedEditor->setVisible(enabled);
    };
    themes->customThemeAdvanced.connect(showAdvanced,
                                        this->managedConnections_);
    showAdvanced(themes->customThemeAdvanced.getValue());

    const auto draftTheme = [this, themes]() -> std::optional<QJsonObject> {
        const ThemeSeed seed{
            .light = themes->useLightMode(),
            .surface = QColor(themes->customThemeSurface.getValue()),
            .ink = QColor(themes->customThemeInk.getValue()),
            .accent = QColor(themes->customThemeAccent.getValue()),
        };
        if (!seed.surface.isValid() || !seed.ink.isValid() ||
            !seed.accent.isValid())
        {
            return std::nullopt;
        }

        const ThemeAdvancedSeed advanced{
            .enabled = themes->customThemeAdvanced.getValue(),
            .accent =
                {
                    .enabled = themes->customThemeAdvancedAccentEnabled
                                   .getValue(),
                    .color = QColor(
                        themes->customThemeAdvancedAccent.getValue()),
                },
            .window =
                {
                    .enabled = themes->customThemeAdvancedWindowEnabled
                                   .getValue(),
                    .background = QColor(themes
                                             ->customThemeAdvancedWindowBackground
                                             .getValue()),
                    .text = QColor(
                        themes->customThemeAdvancedWindowText.getValue()),
                },
            .chat =
                {
                    .enabled =
                        themes->customThemeAdvancedChatEnabled.getValue(),
                    .background = QColor(
                        themes->customThemeAdvancedChatBackground.getValue()),
                    .alternateBackground = QColor(
                        themes->customThemeAdvancedChatAlternate.getValue()),
                    .text = QColor(
                        themes->customThemeAdvancedChatText.getValue()),
                    .systemText = QColor(
                        themes->customThemeAdvancedChatSystemText.getValue()),
                    .link = QColor(
                        themes->customThemeAdvancedChatLink.getValue()),
                },
            .input =
                {
                    .enabled =
                        themes->customThemeAdvancedInputEnabled.getValue(),
                    .background = QColor(
                        themes->customThemeAdvancedInputBackground.getValue()),
                    .text = QColor(
                        themes->customThemeAdvancedInputText.getValue()),
                    .border = QColor(
                        themes->customThemeAdvancedInputBorder.getValue()),
                },
            .header =
                {
                    .enabled =
                        themes->customThemeAdvancedHeaderEnabled.getValue(),
                    .background = QColor(themes
                                             ->customThemeAdvancedHeaderBackground
                                             .getValue()),
                    .text = QColor(
                        themes->customThemeAdvancedHeaderText.getValue()),
                    .focusedBackground = QColor(
                        themes->customThemeAdvancedHeaderFocusedBackground
                            .getValue()),
                    .accent = QColor(
                        themes->customThemeAdvancedHeaderAccent.getValue()),
                },
            .tabs =
                {
                    .enabled =
                        themes->customThemeAdvancedTabsEnabled.getValue(),
                    .background = QColor(
                        themes->customThemeAdvancedTabsBackground.getValue()),
                    .selectedBackground = QColor(
                        themes->customThemeAdvancedTabsSelectedBackground
                            .getValue()),
                    .text = QColor(
                        themes->customThemeAdvancedTabsText.getValue()),
                    .accent = QColor(
                        themes->customThemeAdvancedTabsAccent.getValue()),
                },
            .usercard =
                {
                    .enabled = themes->customThemeAdvancedUsercardEnabled
                                   .getValue(),
                    .background = QColor(
                        themes->customThemeAdvancedUsercardBackground
                            .getValue()),
                    .text = QColor(
                        themes->customThemeAdvancedUsercardText.getValue()),
                    .secondaryText = QColor(
                        themes->customThemeAdvancedUsercardSecondaryText
                            .getValue()),
                    .accent = QColor(
                        themes->customThemeAdvancedUsercardAccent.getValue()),
                },
            .scrollbars =
                {
                    .enabled = themes->customThemeAdvancedScrollbarsEnabled
                                   .getValue(),
                    .background = QColor(
                        themes->customThemeAdvancedScrollbarBackground
                            .getValue()),
                    .thumb = QColor(
                        themes->customThemeAdvancedScrollbarThumb.getValue()),
                    .thumbSelected = QColor(
                        themes->customThemeAdvancedScrollbarThumbSelected
                            .getValue()),
                },
        };

        const auto valid = [](bool enabled,
                              const std::initializer_list<QColor> &colors) {
            return !enabled ||
                   std::all_of(colors.begin(), colors.end(),
                               [](const QColor &color) {
                                   return color.isValid();
                               });
        };
        if (advanced.enabled &&
            (!valid(advanced.accent.enabled, {advanced.accent.color}) ||
             !valid(advanced.window.enabled,
                    {advanced.window.background, advanced.window.text}) ||
             !valid(advanced.chat.enabled,
                    {advanced.chat.background,
                     advanced.chat.alternateBackground, advanced.chat.text,
                     advanced.chat.systemText, advanced.chat.link}) ||
             !valid(advanced.input.enabled,
                    {advanced.input.background, advanced.input.text,
                     advanced.input.border}) ||
             !valid(advanced.header.enabled,
                    {advanced.header.background, advanced.header.text,
                     advanced.header.focusedBackground,
                     advanced.header.accent}) ||
             !valid(advanced.tabs.enabled,
                    {advanced.tabs.background,
                     advanced.tabs.selectedBackground, advanced.tabs.text,
                     advanced.tabs.accent}) ||
             !valid(advanced.usercard.enabled,
                    {advanced.usercard.background, advanced.usercard.text,
                     advanced.usercard.secondaryText,
                     advanced.usercard.accent}) ||
             !valid(advanced.scrollbars.enabled,
                    {advanced.scrollbars.background,
                     advanced.scrollbars.thumb,
                     advanced.scrollbars.thumbSelected})))
        {
            return std::nullopt;
        }

        ThemeEffect effect;
        if (this->animatedDraft_)
        {
            effect.kind =
                themeEffectKindFromKey(themes->customThemeEffect.getValue());
            if (!effect.isAnimated())
            {
                effect.kind = ThemeEffectKind::Aurora;
            }
            effect.colors = {
                QColor(themes->customThemeEffectColor1.getValue()),
                QColor(themes->customThemeEffectColor2.getValue()),
                QColor(themes->customThemeEffectColor3.getValue()),
            };
            if (!std::all_of(effect.colors.begin(), effect.colors.end(),
                             [](const QColor &color) {
                                 return color.isValid();
                             }))
            {
                return std::nullopt;
            }
            effect.speed = std::clamp(
                themes->customThemeEffectSpeed.getValue() / 100.0,
                ThemeEffect::MIN_SPEED, ThemeEffect::MAX_SPEED);
            effect.intensity = std::clamp(
                themes->customThemeEffectIntensity.getValue() / 100.0, 0.0,
                1.0);
        }
        return themes->createSeedTheme(seed, advanced, effect);
    };

    QObject::connect(loadButton, &QPushButton::clicked, this, [this, themes] {
        const auto seed = themes->currentSeed();
        if (!seed)
        {
            this->setStatus("The current theme does not expose editable "
                            "seed colors.");
            return;
        }
        themes->customThemeName = themes->currentThemeDisplayName() + " Custom";
        themes->customThemeVariant = seed->light ? "Light" : "Dark";
        themes->customThemeSurface = seed->surface.name(QColor::HexRgb);
        themes->customThemeInk = seed->ink.name(QColor::HexRgb);
        themes->customThemeAccent = seed->accent.name(QColor::HexRgb);
        const auto advanced = themes->currentAdvancedSeed();
        const auto colorValue = [](const QColor &color) {
            return color.name(color.alpha() == 255 ? QColor::HexRgb
                                                    : QColor::HexArgb);
        };
        themes->customThemeAdvanced = advanced.enabled;
        themes->customThemeAdvancedAccentEnabled = advanced.accent.enabled;
        themes->customThemeAdvancedAccent = colorValue(advanced.accent.color);
        themes->customThemeAdvancedWindowEnabled = advanced.window.enabled;
        themes->customThemeAdvancedWindowBackground =
            colorValue(advanced.window.background);
        themes->customThemeAdvancedWindowText =
            colorValue(advanced.window.text);
        themes->customThemeAdvancedChatEnabled = advanced.chat.enabled;
        themes->customThemeAdvancedChatBackground =
            colorValue(advanced.chat.background);
        themes->customThemeAdvancedChatAlternate =
            colorValue(advanced.chat.alternateBackground);
        themes->customThemeAdvancedChatText = colorValue(advanced.chat.text);
        themes->customThemeAdvancedChatSystemText =
            colorValue(advanced.chat.systemText);
        themes->customThemeAdvancedChatLink = colorValue(advanced.chat.link);
        themes->customThemeAdvancedInputEnabled = advanced.input.enabled;
        themes->customThemeAdvancedInputBackground =
            colorValue(advanced.input.background);
        themes->customThemeAdvancedInputText = colorValue(advanced.input.text);
        themes->customThemeAdvancedInputBorder =
            colorValue(advanced.input.border);
        themes->customThemeAdvancedHeaderEnabled = advanced.header.enabled;
        themes->customThemeAdvancedHeaderBackground =
            colorValue(advanced.header.background);
        themes->customThemeAdvancedHeaderText =
            colorValue(advanced.header.text);
        themes->customThemeAdvancedHeaderFocusedBackground =
            colorValue(advanced.header.focusedBackground);
        themes->customThemeAdvancedHeaderAccent =
            colorValue(advanced.header.accent);
        themes->customThemeAdvancedTabsEnabled = advanced.tabs.enabled;
        themes->customThemeAdvancedTabsBackground =
            colorValue(advanced.tabs.background);
        themes->customThemeAdvancedTabsSelectedBackground =
            colorValue(advanced.tabs.selectedBackground);
        themes->customThemeAdvancedTabsText = colorValue(advanced.tabs.text);
        themes->customThemeAdvancedTabsAccent =
            colorValue(advanced.tabs.accent);
        themes->customThemeAdvancedUsercardEnabled =
            advanced.usercard.enabled;
        themes->customThemeAdvancedUsercardBackground =
            colorValue(advanced.usercard.background);
        themes->customThemeAdvancedUsercardText =
            colorValue(advanced.usercard.text);
        themes->customThemeAdvancedUsercardSecondaryText =
            colorValue(advanced.usercard.secondaryText);
        themes->customThemeAdvancedUsercardAccent =
            colorValue(advanced.usercard.accent);
        themes->customThemeAdvancedScrollbarsEnabled =
            advanced.scrollbars.enabled;
        themes->customThemeAdvancedScrollbarBackground =
            colorValue(advanced.scrollbars.background);
        themes->customThemeAdvancedScrollbarThumb =
            colorValue(advanced.scrollbars.thumb);
        themes->customThemeAdvancedScrollbarThumbSelected =
            colorValue(advanced.scrollbars.thumbSelected);
        const auto &effect = themes->effect();
        if (effect.isAnimated())
        {
            themes->customThemeEffect = themeEffectKindKey(effect.kind);
            themes->customThemeEffectColor1 = colorValue(effect.colors[0]);
            themes->customThemeEffectColor2 = colorValue(effect.colors[1]);
            themes->customThemeEffectColor3 = colorValue(effect.colors[2]);
            themes->customThemeEffectSpeed = qRound(effect.speed * 100);
            themes->customThemeEffectIntensity = qRound(effect.intensity * 100);
        }
        this->showCustomType_(effect.isAnimated());
        this->setStatus("Current theme loaded into the editor.");
    });

    QObject::connect(
        createButton, &QPushButton::clicked, this, [this, themes, draftTheme] {
            const auto data = draftTheme();
            if (!data || themes->customThemeName.getValue().trimmed().isEmpty())
            {
                this->setStatus(
                    "Choose a name and make every enabled color valid first.");
                return;
            }
            themes->saveCustomTheme(themes->customThemeName.getValue(), *data);
            this->setStatus("Custom theme created and applied.");
        });

    QObject::connect(
        updateButton, &QPushButton::clicked, this, [this, themes, draftTheme] {
            const auto selected = themes->themeName.getValue();
            const auto data = draftTheme();
            if (!themes->isCustomTheme(selected))
            {
                this->setStatus(
                    "Select one of your custom themes to update it.");
                return;
            }
            if (!data || themes->customThemeName.getValue().trimmed().isEmpty())
            {
                this->setStatus(
                    "Choose a name and make every enabled color valid first.");
                return;
            }
            themes->saveCustomTheme(themes->customThemeName.getValue(), *data,
                                    selected);
            this->setStatus("Custom theme updated.");
        });

    QObject::connect(deleteButton, &QPushButton::clicked, this, [this, themes] {
        const auto selected = themes->themeName.getValue();
        if (!themes->isCustomTheme(selected))
        {
            this->setStatus("Select one of your custom themes to delete it.");
            return;
        }
        if (QMessageBox::question(this, "Delete custom theme",
                                  "Delete the selected custom theme?",
                                  QMessageBox::Yes | QMessageBox::Cancel,
                                  QMessageBox::Cancel) != QMessageBox::Yes)
        {
            return;
        }
        if (themes->removeCustomTheme(selected))
        {
            this->setStatus("Custom theme deleted.");
        }
    });

    auto *draftRefresh = new QTimer(this);
    draftRefresh->setSingleShot(true);
    draftRefresh->setInterval(30);
    QObject::connect(draftRefresh, &QTimer::timeout, this,
                     [draftPreview, draftTheme] {
                         if (const auto draft = draftTheme())
                         {
                             draftPreview->setTheme(*draft);
                         }
                     });
    this->draftListener_ = std::make_unique<pajlada::SettingListener>();
    this->draftListener_->setCB([draftRefresh] {
        draftRefresh->start();
    });
    for (auto *setting : {
             &themes->appearanceMode,
             &themes->customThemeSurface,
             &themes->customThemeInk,
             &themes->customThemeAccent,
             &themes->customThemeEffect,
             &themes->customThemeEffectColor1,
             &themes->customThemeEffectColor2,
             &themes->customThemeEffectColor3,
             &themes->customThemeAdvancedAccent,
             &themes->customThemeAdvancedWindowBackground,
             &themes->customThemeAdvancedWindowText,
             &themes->customThemeAdvancedChatBackground,
             &themes->customThemeAdvancedChatAlternate,
             &themes->customThemeAdvancedChatText,
             &themes->customThemeAdvancedChatSystemText,
             &themes->customThemeAdvancedChatLink,
             &themes->customThemeAdvancedInputBackground,
             &themes->customThemeAdvancedInputText,
             &themes->customThemeAdvancedInputBorder,
             &themes->customThemeAdvancedHeaderBackground,
             &themes->customThemeAdvancedHeaderText,
             &themes->customThemeAdvancedHeaderFocusedBackground,
             &themes->customThemeAdvancedHeaderAccent,
             &themes->customThemeAdvancedTabsBackground,
             &themes->customThemeAdvancedTabsSelectedBackground,
             &themes->customThemeAdvancedTabsText,
             &themes->customThemeAdvancedTabsAccent,
             &themes->customThemeAdvancedUsercardBackground,
             &themes->customThemeAdvancedUsercardText,
             &themes->customThemeAdvancedUsercardSecondaryText,
             &themes->customThemeAdvancedUsercardAccent,
             &themes->customThemeAdvancedScrollbarBackground,
             &themes->customThemeAdvancedScrollbarThumb,
             &themes->customThemeAdvancedScrollbarThumbSelected,
         })
    {
        this->draftListener_->addSetting(*setting);
    }
    for (auto *setting : {
             &themes->customThemeAdvanced,
             &themes->customThemeAdvancedAccentEnabled,
             &themes->customThemeAdvancedWindowEnabled,
             &themes->customThemeAdvancedChatEnabled,
             &themes->customThemeAdvancedInputEnabled,
             &themes->customThemeAdvancedHeaderEnabled,
             &themes->customThemeAdvancedTabsEnabled,
             &themes->customThemeAdvancedUsercardEnabled,
             &themes->customThemeAdvancedScrollbarsEnabled,
         })
    {
        this->draftListener_->addSetting(*setting);
    }
    this->draftListener_->addSetting(themes->customThemeEffectSpeed);
    this->draftListener_->addSetting(themes->customThemeEffectIntensity);

    const auto showPresetType = [staticTab, animatedTab, pickers](bool animated) {
        staticTab->setChecked(!animated);
        animatedTab->setChecked(animated);
        pickers->setCurrentIndex(animated ? 1 : 0);
    };
    QObject::connect(staticTab, &QPushButton::clicked, this, [showPresetType] {
        showPresetType(false);
    });
    QObject::connect(animatedTab, &QPushButton::clicked, this, [showPresetType] {
        showPresetType(true);
    });
    themes->themeName.connect(
        [themes, showPresetType](const QString &key) {
            showPresetType(themes->isAnimatedTheme(key));
        },
        this->managedConnections_, false);
    showPresetType(themes->isAnimatedTheme(themes->themeName.getValue()));

    // The draft type belongs to the editor, independently of the preset browser.
    this->showCustomType_ = [this, customStatic, customAnimated, animationEditor,
                             customDescription, createButton,
                             draftRefresh](bool animated) {
        this->animatedDraft_ = animated;
        customStatic->setChecked(!animated);
        customAnimated->setChecked(animated);
        animationEditor->setVisible(animated);
        createButton->setText(animated ? "Create animated theme"
                                       : "Create static theme");
        customDescription->setText(
            animated ? "Choose base colours and an animation for the chat "
                       "background, then preview and save your theme."
                     : "Choose base colours, then preview and save your theme. "
                       "Advanced colours below are optional.");
        draftRefresh->start();
    };
    QObject::connect(customStatic, &QPushButton::clicked, this, [this] {
        this->showCustomType_(false);
    });
    QObject::connect(customAnimated, &QPushButton::clicked, this, [this] {
        this->showCustomType_(true);
    });
    this->showCustomType_(themes->isAnimatedTheme(themes->themeName.getValue()));

    layout.addTitle("Typography");
    layout.addDescription("Pick any font installed on this computer and tune "
                          "its size and weight.");
    layout.addWidget(
        new FontSettingWidget(settings.chatFontFamily, settings.chatFontSize,
                              settings.chatFontWeight),
        {"font", "family", "weight", "size", "custom font"});

    layout.addTitle("Share codes");
    layout.addDescription(
        "A share code contains the current palette and chat font settings. "
        "Paste one below to add it to your custom library.");
    this->shareCode_ = new QLineEdit(this);
    this->shareCode_->setPlaceholderText("MGR1...");
    this->shareCode_->setClearButtonEnabled(true);
    this->shareCode_->setMaxLength(200000);
    layout.addWidget(this->shareCode_,
                     {"share", "code", "import", "export", "clipboard"});

    auto *copyButton = new QPushButton("Copy current code", this);
    auto *importButton = new QPushButton("Import code", this);
    layout.addWidget(actionRow(this, {copyButton, importButton}),
                     {"share", "copy", "paste", "import", "export"});

    QObject::connect(copyButton, &QPushButton::clicked, this,
                     [this, themes, &settings] {
                         const auto code = makeShareCode(*themes, settings);
                         this->shareCode_->setText(code);
                         QApplication::clipboard()->setText(code);
                         this->setStatus("Theme code copied to the clipboard.");
                     });

    QObject::connect(
        importButton, &QPushButton::clicked, this, [this, themes, &settings] {
            auto code = this->shareCode_->text().trimmed();
            if (code.isEmpty())
            {
                code = QApplication::clipboard()->text().trimmed();
            }

            QString error;
            const auto shared = parseShareCode(code, error);
            if (!shared)
            {
                QMessageBox::warning(this, "Theme import failed", error);
                return;
            }

            themes->saveCustomTheme(shared->name, shared->theme);
            if (!shared->fontFamily.isEmpty())
            {
                settings.chatFontFamily = shared->fontFamily;
            }
            if (shared->fontSize >= 6 && shared->fontSize <= 72)
            {
                settings.chatFontSize = shared->fontSize;
            }
            if (shared->fontWeight >= 1 && shared->fontWeight <= 1000)
            {
                settings.chatFontWeight = shared->fontWeight;
            }
            this->shareCode_->setText(code);
            this->setStatus("Shared theme imported and applied.");
        });

    this->status_ = layout.addDescription("Ready.");
    layout.addStretch();
}

void ThemesPage::setStatus(const QString &text)
{
    if (this->status_ != nullptr)
    {
        this->status_->setText(text);
    }
}

}  // namespace chatterino
