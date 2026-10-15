// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/ChatterinoSetting.hpp"
#include "singletons/Paths.hpp"
#include "singletons/ThemeEffect.hpp"
#include "util/RapidJsonSerializeQString.hpp"

#include <pajlada/settings/setting.hpp>
#include <QColor>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <memory>
#include <optional>
#include <vector>

namespace chatterino {

class WindowManager;

struct ThemeDescriptor {
    QString key;

    // Path to the theme on disk
    // Can be a Qt resource path
    QString path;

    // Name of the theme
    QString name;

    bool custom{};
    /// True if the theme paints an animated backdrop behind chat
    bool animated{};
    QJsonObject data;
    /// Hand-tuned light variant; when empty, light mode is derived from data
    QJsonObject lightData;
};

struct ThemeSeed {
    bool light{};
    QColor surface;
    QColor ink;
    QColor accent;
};

struct ThemeAdvancedSeed {
    bool enabled{};

    struct {
        bool enabled{};
        QColor color;
    } accent;
    struct {
        bool enabled{};
        QColor background;
        QColor text;
    } window;
    struct {
        bool enabled{};
        QColor background;
        QColor alternateBackground;
        QColor text;
        QColor systemText;
        QColor link;
    } chat;
    struct {
        bool enabled{};
        QColor background;
        QColor text;
        QColor border;
    } input;
    struct {
        bool enabled{};
        QColor background;
        QColor text;
        QColor focusedBackground;
        QColor accent;
    } header;
    struct {
        bool enabled{};
        QColor background;
        QColor selectedBackground;
        QColor text;
        QColor accent;
    } tabs;
    struct {
        bool enabled{};
        QColor background;
        QColor text;
        QColor secondaryText;
        QColor accent;
    } usercard;
    struct {
        bool enabled{};
        QColor background;
        QColor thumb;
        QColor thumbSelected;
    } scrollbars;
};

class Theme final
{
public:
    static const std::vector<ThemeDescriptor> builtInThemes;

    // The built in theme that will be used if some theme parsing fails
    static const ThemeDescriptor fallbackTheme;

    static const int AUTO_RELOAD_INTERVAL_MS = 500;

    Theme(const Paths &paths);

    bool isLightTheme() const;
    bool isSystemTheme() const;
    bool useLightMode() const;
    static QJsonObject applyMode(const QJsonObject &theme, bool light);

    struct TabColors {
        QColor text;
        struct {
            QColor regular;
            QColor hover;
            QColor unfocused;
        } backgrounds;
        struct {
            QColor regular;
            QColor hover;
            QColor unfocused;
        } line;
    };

    struct TextColors {
        QColor regular;
        QColor caret;
        QColor link;
        QColor system;
        QColor chatPlaceholder;
    };

    struct MessageBackgrounds {
        QColor regular;
        QColor alternate;
    };

    QColor accent{"#00aeef"};

    /// WINDOW
    struct {
        QColor background;
        QColor text;
    } window;

    /// TABS
    struct {
        TabColors regular;
        TabColors newMessage;
        TabColors highlighted;
        TabColors selected;
        QColor dividerLine;

        QColor liveIndicator;
        QColor rerunIndicator;
    } tabs;

    /// MESSAGES
    struct {
        TextColors textColors;
        MessageBackgrounds backgrounds;

        QColor disabled;
        QColor selection;

        QColor highlightAnimationStart;
        QColor highlightAnimationEnd;
    } messages;

    struct {
        TextColors textColors;
        MessageBackgrounds backgrounds;

        QColor disabled;
        QColor selection;
        QColor background;
    } overlayMessages;

    /// SCROLLBAR
    struct {
        QColor background;
        QColor thumb;
        QColor thumbSelected;
    } scrollbars;

    /// SPLITS
    struct {
        QColor messageSeperator;
        QColor background;
        QColor dropPreview;
        QColor dropPreviewBorder;
        QColor dropTargetRect;
        QColor dropTargetRectBorder;
        QColor resizeHandle;
        QColor resizeHandleBackground;

        struct {
            QColor border;
            QColor focusedBorder;
            QColor background;
            QColor focusedBackground;
            QColor text;
            QColor focusedText;
        } header;

        struct {
            QColor background;
            QColor backgroundPulse;
            QColor text;
            QColor border;
            // Used by the ChatterListWidget & QuickSwitcherPopup
            QString styleSheet;
        } input;
    } splits;

    struct {
        QColor background;
        QColor text;
        QColor secondaryText;
        QColor accent;
    } usercard;

    struct {
        QPixmap copy;
    } buttons;

    QPalette palette;

    void normalizeColor(QColor &color) const;
    void update();

    bool isAutoReloading() const;
    void setAutoReload(bool autoReload);

    /**
     * Return a list of available themes
     **/
    std::vector<std::pair<QString, QVariant>> availableThemes() const;

    QJsonObject createSeedTheme(
        const ThemeSeed &seed,
        const ThemeAdvancedSeed &advanced = ThemeAdvancedSeed{},
        const ThemeEffect &effect = ThemeEffect{}) const;
    QString saveCustomTheme(const QString &name, const QJsonObject &theme,
                            const QString &existingKey = {});
    bool removeCustomTheme(const QString &key);
    bool isCustomTheme(const QString &key) const;
    bool isAnimatedTheme(const QString &key) const;
    /// Resolves a theme the way it would be applied in the current mode,
    /// e.g. for previews. Returns nullopt for unknown themes.
    std::optional<QJsonObject> resolvedThemeJson(const QString &key) const;
    std::optional<ThemeSeed> seedForTheme(const QString &key) const;
    std::optional<ThemeSeed> currentSeed() const;
    ThemeAdvancedSeed currentAdvancedSeed() const;
    const QJsonObject &currentThemeJson() const;
    QString currentThemeDisplayName() const;

    /// The current theme's animated backdrop (None for static themes)
    const ThemeEffect &effect() const;
    /// Animation clock of the current frame, in seconds
    double effectSeconds() const;

    pajlada::Signals::NoArgSignal updated;
    /// Fired for every frame of an animated theme
    pajlada::Signals::NoArgSignal effectFrame;

    QStringSetting themeName{"/appearance/theme/name", "Dark"};
    QStringSetting appearanceMode{"/appearance/theme/mode", ""};
    // Legacy setting, retained only to migrate the former System theme.
    QStringSetting darkSystemThemeName{"/appearance/theme/darkSystem", "Dark"};
    QStringSetting customThemesJson{"/appearance/theme/customThemes", "[]"};
    QStringSetting customThemeName{"/appearance/theme/editor/name", "My theme"};
    QStringSetting customThemeVariant{"/appearance/theme/editor/variant",
                                      "Dark"};
    QStringSetting customThemeSurface{"/appearance/theme/editor/surface",
                                      "#181818"};
    QStringSetting customThemeInk{"/appearance/theme/editor/ink", "#ffffff"};
    QStringSetting customThemeAccent{"/appearance/theme/editor/accent",
                                     "#339cff"};
    BoolSetting customThemeAdvanced{
        "/appearance/theme/editor/advanced/enabled", false};

    QStringSetting customThemeEffect{"/appearance/theme/editor/effect/type",
                                     "aurora"};
    QStringSetting customThemeEffectColor1{
        "/appearance/theme/editor/effect/color1", "#2fe39a"};
    QStringSetting customThemeEffectColor2{
        "/appearance/theme/editor/effect/color2", "#22b8d6"};
    QStringSetting customThemeEffectColor3{
        "/appearance/theme/editor/effect/color3", "#4f7dff"};
    // Percentages of ThemeEffect::speed and ThemeEffect::intensity
    IntSetting customThemeEffectSpeed{"/appearance/theme/editor/effect/speed",
                                      100};
    IntSetting customThemeEffectIntensity{
        "/appearance/theme/editor/effect/intensity", 60};

    BoolSetting customThemeAdvancedAccentEnabled{
        "/appearance/theme/editor/advanced/accent/enabled", false};
    QStringSetting customThemeAdvancedAccent{
        "/appearance/theme/editor/advanced/accent/color", "#339cff"};

    BoolSetting customThemeAdvancedWindowEnabled{
        "/appearance/theme/editor/advanced/window/enabled", false};
    QStringSetting customThemeAdvancedWindowBackground{
        "/appearance/theme/editor/advanced/window/background", "#181818"};
    QStringSetting customThemeAdvancedWindowText{
        "/appearance/theme/editor/advanced/window/text", "#ffffff"};

    BoolSetting customThemeAdvancedChatEnabled{
        "/appearance/theme/editor/advanced/chat/enabled", false};
    QStringSetting customThemeAdvancedChatBackground{
        "/appearance/theme/editor/advanced/chat/background", "#181818"};
    QStringSetting customThemeAdvancedChatAlternate{
        "/appearance/theme/editor/advanced/chat/alternate", "#202020"};
    QStringSetting customThemeAdvancedChatText{
        "/appearance/theme/editor/advanced/chat/text", "#ffffff"};
    QStringSetting customThemeAdvancedChatSystemText{
        "/appearance/theme/editor/advanced/chat/systemText", "#a8a8b3"};
    QStringSetting customThemeAdvancedChatLink{
        "/appearance/theme/editor/advanced/chat/link", "#339cff"};

    BoolSetting customThemeAdvancedInputEnabled{
        "/appearance/theme/editor/advanced/input/enabled", false};
    QStringSetting customThemeAdvancedInputBackground{
        "/appearance/theme/editor/advanced/input/background", "#202020"};
    QStringSetting customThemeAdvancedInputText{
        "/appearance/theme/editor/advanced/input/text", "#ffffff"};
    QStringSetting customThemeAdvancedInputBorder{
        "/appearance/theme/editor/advanced/input/border", "#3a3a3a"};

    BoolSetting customThemeAdvancedHeaderEnabled{
        "/appearance/theme/editor/advanced/header/enabled", false};
    QStringSetting customThemeAdvancedHeaderBackground{
        "/appearance/theme/editor/advanced/header/background", "#202020"};
    QStringSetting customThemeAdvancedHeaderText{
        "/appearance/theme/editor/advanced/header/text", "#ffffff"};
    QStringSetting customThemeAdvancedHeaderFocusedBackground{
        "/appearance/theme/editor/advanced/header/focusedBackground",
        "#2a2a2a"};
    QStringSetting customThemeAdvancedHeaderAccent{
        "/appearance/theme/editor/advanced/header/accent", "#339cff"};

    BoolSetting customThemeAdvancedTabsEnabled{
        "/appearance/theme/editor/advanced/tabs/enabled", false};
    QStringSetting customThemeAdvancedTabsBackground{
        "/appearance/theme/editor/advanced/tabs/background", "#202020"};
    QStringSetting customThemeAdvancedTabsSelectedBackground{
        "/appearance/theme/editor/advanced/tabs/selectedBackground",
        "#343434"};
    QStringSetting customThemeAdvancedTabsText{
        "/appearance/theme/editor/advanced/tabs/text", "#d0d0d0"};
    QStringSetting customThemeAdvancedTabsAccent{
        "/appearance/theme/editor/advanced/tabs/accent", "#339cff"};

    BoolSetting customThemeAdvancedUsercardEnabled{
        "/appearance/theme/editor/advanced/usercard/enabled", false};
    QStringSetting customThemeAdvancedUsercardBackground{
        "/appearance/theme/editor/advanced/usercard/background", "#181818"};
    QStringSetting customThemeAdvancedUsercardText{
        "/appearance/theme/editor/advanced/usercard/text", "#ffffff"};
    QStringSetting customThemeAdvancedUsercardSecondaryText{
        "/appearance/theme/editor/advanced/usercard/secondaryText", "#aaaaaa"};
    QStringSetting customThemeAdvancedUsercardAccent{
        "/appearance/theme/editor/advanced/usercard/accent", "#339cff"};

    BoolSetting customThemeAdvancedScrollbarsEnabled{
        "/appearance/theme/editor/advanced/scrollbars/enabled", false};
    QStringSetting customThemeAdvancedScrollbarBackground{
        "/appearance/theme/editor/advanced/scrollbars/background", "#00181818"};
    QStringSetting customThemeAdvancedScrollbarThumb{
        "/appearance/theme/editor/advanced/scrollbars/thumb", "#454545"};
    QStringSetting customThemeAdvancedScrollbarThumbSelected{
        "/appearance/theme/editor/advanced/scrollbars/thumbSelected",
        "#666666"};

private:
    bool isLight_ = false;

    std::vector<ThemeDescriptor> availableThemes_;

    QString currentThemePath_;
    std::unique_ptr<QTimer> themeReloadTimer_;
    // Retained for exporting, editing, and auto-reload comparisons.
    QJsonObject currentThemeJson_;
    QString currentThemeKey_;
    QString themesDirectory_;

    ThemeEffect effect_;
    std::unique_ptr<QTimer> effectTimer_;
    QElapsedTimer effectClock_;
    double effectSeconds_{};

    QObject lifetime_;

    /**
     * Figure out which themes are available in the Themes directory
     *
     * NOTE: This is currently not built to be reloadable
     **/
    void loadAvailableThemes();

    std::optional<ThemeDescriptor> findThemeByKey(const QString &key) const;
    std::optional<QJsonObject> themeForMode(const ThemeDescriptor &theme,
                                            bool light) const;
    void updateEffectTimer();

    void parseFrom(const QJsonObject &root, bool isCustomTheme);

    pajlada::Signals::NoArgSignal repaintVisibleChatWidgets_;

    friend class WindowManager;
};

Theme *getTheme();
}  // namespace chatterino
