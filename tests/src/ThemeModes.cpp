#include "mocks/BaseApplication.hpp"
#include "singletons/Theme.hpp"

#include <gtest/gtest.h>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QStyleHints>

#include <cmath>

using namespace chatterino;

namespace {
double contrast(const QColor &foreground, const QColor &background)
{
    const auto luminance = [](const QColor &color) {
        const auto linear = [](double value) {
            return value <= 0.04045 ? value / 12.92
                                    : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return linear(color.redF()) * 0.2126 + linear(color.greenF()) * 0.7152 +
               linear(color.blueF()) * 0.0722;
    };
    const auto a = luminance(foreground);
    const auto b = luminance(background);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}
}  // namespace

TEST(ThemeModes, PickersListNineStaticAndFiveAnimatedThemes)
{
    mock::BaseApplication app;
    const auto available = app.theme.availableThemes();
    size_t animated = 0;
    for (const auto &[name, value] : available)
    {
        EXPECT_NE(name, "Light");
        EXPECT_NE(name, "Dark");
        EXPECT_NE(name, "System");
        if (app.theme.isAnimatedTheme(value.toString()))
        {
            ++animated;
        }
    }
    EXPECT_EQ(available.size() - animated, 9);
    EXPECT_EQ(animated, 5);
}

TEST(ThemeModes, EveryPresetKeepsItsIdentityAcrossModes)
{
    mock::BaseApplication app;
    for (const auto &[name, value] : app.theme.availableThemes())
    {
        SCOPED_TRACE(name.toStdString());
        app.theme.themeName = value.toString();
        app.theme.appearanceMode = "Dark";
        const auto dark = app.theme.currentThemeJson();
        const auto darkBackground = app.theme.window.background;
        EXPECT_FALSE(app.theme.isLightTheme());
        EXPECT_GE(contrast(app.theme.messages.textColors.regular,
                           app.theme.messages.backgrounds.regular),
                  4.5);
        if (value.toString() != "Dark")
        {
            // Default keeps Chatterino's original link colours
            EXPECT_GE(contrast(app.theme.messages.textColors.link,
                               app.theme.messages.backgrounds.regular),
                      4.5);
        }
        app.theme.appearanceMode = "Light";
        EXPECT_EQ(app.theme.themeName.getValue(), value.toString());
        EXPECT_EQ(app.theme.currentThemeDisplayName(), name);
        EXPECT_TRUE(app.theme.isLightTheme());
        EXPECT_GT(app.theme.window.background.lightnessF(), 0.5);
        EXPECT_NE(app.theme.window.background, darkBackground);
        EXPECT_GE(contrast(app.theme.window.text, app.theme.window.background),
                  4.5);
        EXPECT_GE(contrast(app.theme.messages.textColors.regular,
                           app.theme.messages.backgrounds.regular),
                  4.5);
        EXPECT_GE(contrast(app.theme.splits.input.text,
                           app.theme.splits.input.background),
                  4.5);
        if (value.toString() != "Dark")
        {
            // Default keeps Chatterino's original link colours
            EXPECT_GE(contrast(app.theme.messages.textColors.link,
                               app.theme.messages.backgrounds.regular),
                      4.5);
        }
        EXPECT_EQ(app.theme.isAnimatedTheme(value.toString()),
                  app.theme.effect().isAnimated());
        EXPECT_EQ(app.theme.palette.color(QPalette::Window),
                  app.theme.window.background);
        EXPECT_EQ(app.theme.palette.color(QPalette::Highlight),
                  app.theme.accent);
        EXPECT_GE(contrast(app.theme.palette.color(QPalette::HighlightedText),
                           app.theme.palette.color(QPalette::Highlight)),
                  4.5);
        app.theme.appearanceMode = "Dark";
        EXPECT_EQ(app.theme.currentThemeJson(), dark);
        EXPECT_FALSE(app.theme.isLightTheme());
    }
}

TEST(ThemeModes, SettingsPaletteDoesNotRetainPreviousRedTheme)
{
    mock::BaseApplication app;
    app.theme.themeName = "preset.crimson.dark";
    const auto red = app.theme.palette.color(QPalette::Highlight);
    for (const auto &mode : {"Dark", "Light"})
    {
        app.theme.appearanceMode = mode;
        app.theme.themeName = "preset.amber.dark";
        EXPECT_NE(app.theme.palette.color(QPalette::Highlight), red);
        EXPECT_EQ(app.theme.palette.color(QPalette::Highlight),
                  app.theme.accent);
        EXPECT_EQ(app.theme.palette.color(QPalette::Base),
                  app.theme.splits.input.background);
        EXPECT_EQ(app.theme.palette.color(QPalette::Text),
                  app.theme.splits.input.text);
        app.theme.themeName = "preset.cafe.dark";
        EXPECT_EQ(app.theme.palette.color(QPalette::Highlight),
                  app.theme.accent);
        EXPECT_EQ(app.theme.palette.color(QPalette::WindowText),
                  app.theme.window.text);
    }
}

TEST(ThemeModes, MigratesLegacyLightSelectionToLightMode)
{
    mock::BaseApplication app(R"({"appearance":{"theme":{"name":"Light"}}})");
    EXPECT_EQ(app.theme.appearanceMode.getValue(), "Light");
    EXPECT_EQ(app.theme.themeName.getValue(), "Dark");
    EXPECT_TRUE(app.theme.isLightTheme());
}

TEST(ThemeModes, MigratesLegacySystemSelectionToOneColourPreset)
{
    mock::BaseApplication app(R"({"appearance":{"theme":{
        "name":"System","darkSystem":"preset.crimson.dark",
        "lightSystem":"Light"}}})");
    EXPECT_EQ(app.theme.appearanceMode.getValue(), "System");
    EXPECT_TRUE(app.theme.isSystemTheme());
    const auto light =
        QApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light;
    EXPECT_EQ(app.theme.themeName.getValue(),
              light ? "Dark" : "preset.crimson.dark");
    EXPECT_EQ(app.theme.isLightTheme(), light);
}

TEST(ThemeModes, StoredModeAndColourRemainIndependentAfterReload)
{
    mock::BaseApplication app(R"({"appearance":{"theme":{
        "name":"preset.amber.dark","mode":"Light"}}})");
    EXPECT_EQ(app.theme.appearanceMode.getValue(), "Light");
    EXPECT_EQ(app.theme.themeName.getValue(), "preset.amber.dark");
    EXPECT_TRUE(app.theme.isLightTheme());
}

TEST(ThemeModes, CustomModeSwitchPreservesAuthoredOverrides)
{
    mock::BaseApplication app;
    app.theme.appearanceMode = "Dark";
    ThemeAdvancedSeed overrides;
    overrides.enabled = true;
    overrides.input.enabled = true;
    overrides.input.background = QColor("#121c28");
    overrides.input.text = Qt::white;
    overrides.input.border = QColor("#375575");
    const auto authored = app.theme.createSeedTheme(
        {false, QColor("#181818"), QColor("#ededed"), QColor("#f0aa33")},
        overrides);
    const auto key = app.theme.saveCustomTheme("Mode test", authored);
    const auto stored = app.theme.customThemesJson.getValue();
    app.theme.appearanceMode = "Light";
    EXPECT_EQ(app.theme.themeName.getValue(), key);
    EXPECT_TRUE(app.theme.isLightTheme());
    EXPECT_GT(app.theme.splits.input.background.lightnessF(), 0.5);
    EXPECT_TRUE(app.theme.currentAdvancedSeed().input.enabled);
    EXPECT_EQ(app.theme.customThemesJson.getValue(), stored);
    app.theme.appearanceMode = "Dark";
    EXPECT_EQ(app.theme.currentThemeJson(), authored);
    EXPECT_EQ(app.theme.splits.input.background, QColor("#121c28"));
    EXPECT_EQ(app.theme.customThemesJson.getValue(), stored);
}

TEST(ThemeModes, DefaultDarkMatchesOriginalThemeResourceAndInputStyles)
{
    mock::BaseApplication app;
    app.theme.themeName = "Dark";
    app.theme.appearanceMode = "Dark";
    QFile resource(":/themes/Dark.json");
    ASSERT_TRUE(resource.open(QFile::ReadOnly));
    EXPECT_EQ(app.theme.currentThemeJson(),
              QJsonDocument::fromJson(resource.readAll()).object());
    const auto expected =
        QStringLiteral(R"(
        background: %1;
        border: %2;
        color: %3;
        selection-background-color: %4;
    )")
            .arg(app.theme.splits.input.background.name(QColor::HexArgb),
                 app.theme.tabs.selected.backgrounds.regular.name(
                     QColor::HexArgb),
                 app.theme.messages.textColors.regular.name(QColor::HexArgb),
                 app.theme.tabs.selected.backgrounds.regular.name(
                     QColor::HexArgb));
    EXPECT_EQ(app.theme.splits.input.styleSheet, expected);
}

TEST(ThemeModes, DefaultDarkRestoresEveryOriginalNativePaletteRole)
{
    mock::BaseApplication app;
    const auto original = QApplication::palette();
    for (const auto *previous :
         {"preset.crimson.dark", "preset.amber.dark", "Dark"})
    {
        app.theme.themeName = previous;
        app.theme.appearanceMode = "Light";
        app.theme.themeName = "Dark";
        app.theme.appearanceMode = "Dark";
        for (int group = 0; group < QPalette::NColorGroups; ++group)
        {
            for (int role = 0; role < QPalette::NColorRoles; ++role)
            {
                EXPECT_EQ(app.theme.palette.brush(QPalette::ColorGroup(group),
                                                  QPalette::ColorRole(role)),
                          original.brush(QPalette::ColorGroup(group),
                                         QPalette::ColorRole(role)));
            }
        }
    }
}

TEST(ThemeModes, PresetsUseTheirHandTunedLightVariant)
{
    mock::BaseApplication app;
    app.theme.themeName = "preset.cafe.dark";
    app.theme.appearanceMode = "Light";
    EXPECT_EQ(app.theme.messages.backgrounds.regular, QColor("#eff1f5"));
    EXPECT_EQ(app.theme.messages.textColors.regular, QColor("#4c4f69"));
    app.theme.appearanceMode = "Dark";
    EXPECT_EQ(app.theme.messages.backgrounds.regular, QColor("#1e1e2e"));
}

TEST(ThemeModes, PlainCustomThemesRegenerateABrightLightVariant)
{
    mock::BaseApplication app;
    app.theme.appearanceMode = "Dark";
    const auto authored = app.theme.createSeedTheme(
        {false, QColor("#2e3440"), QColor("#e5e9f0"), QColor("#88c0d0")});
    const auto key = app.theme.saveCustomTheme("Slate", authored);
    app.theme.appearanceMode = "Light";
    EXPECT_EQ(app.theme.themeName.getValue(), key);
    // Inverting #2e3440 would give a muddy mid-tone; regenerating keeps the
    // hue as a soft tint on a bright surface.
    EXPECT_GT(app.theme.messages.backgrounds.regular.lightnessF(), 0.9);
    EXPECT_GE(contrast(app.theme.messages.textColors.regular,
                       app.theme.messages.backgrounds.regular),
              7.0);
    app.theme.appearanceMode = "Dark";
    EXPECT_EQ(app.theme.currentThemeJson(), authored);
}

TEST(ThemeModes, EffectJsonRoundTripsAndRejectsUnknownEffects)
{
    ThemeEffect effect{.kind = ThemeEffectKind::Embers,
                       .colors = {QColor("#ff7a2e"), QColor("#ffb54a"),
                                  QColor("#ff4b2b")},
                       .speed = 1.5,
                       .intensity = 0.4};
    const auto theme = withThemeEffect(QJsonObject{}, effect);
    EXPECT_EQ(themeEffectFromTheme(theme), effect);
    EXPECT_FALSE(
        themeEffectFromTheme(withThemeEffect(theme, ThemeEffect{})).isAnimated());

    auto metadata = theme["metadata"].toObject();
    auto stored = metadata["mergerinoEffect"].toObject();
    stored["type"] = "fireworks";
    metadata["mergerinoEffect"] = stored;
    EXPECT_FALSE(themeEffectFromTheme(QJsonObject{{"metadata", metadata}})
                     .isAnimated());

    stored["type"] = "rain";
    stored["speed"] = 40.0;
    stored["intensity"] = -1.0;
    metadata["mergerinoEffect"] = stored;
    const auto clamped =
        themeEffectFromTheme(QJsonObject{{"metadata", metadata}});
    EXPECT_EQ(clamped.kind, ThemeEffectKind::Rain);
    EXPECT_EQ(clamped.speed, ThemeEffect::MAX_SPEED);
    EXPECT_EQ(clamped.intensity, 0.0);
}

TEST(ThemeModes, AnimatedThemesKeepTheirEffectInBothModes)
{
    mock::BaseApplication app;
    app.theme.appearanceMode = "Dark";
    app.theme.themeName = "animated.aurora";
    EXPECT_EQ(app.theme.effect().kind, ThemeEffectKind::Aurora);
    EXPECT_GT(app.theme.effect().frameInterval(), 0);

    app.theme.appearanceMode = "Light";
    EXPECT_TRUE(app.theme.isLightTheme());
    EXPECT_EQ(app.theme.effect().kind, ThemeEffectKind::Aurora);
    for (const auto &color : app.theme.effect().colors)
    {
        EXPECT_LE(color.lightnessF(), 0.56F);
    }

    app.theme.themeName = "preset.amber.dark";
    EXPECT_FALSE(app.theme.effect().isAnimated());
}

TEST(ThemeModes, CustomAnimatedThemesAreSavedAndListedAsAnimated)
{
    mock::BaseApplication app;
    app.theme.appearanceMode = "Dark";
    const ThemeEffect effect{
        .kind = ThemeEffectKind::Starfield,
        .colors = {QColor("#ffffff"), QColor("#8fb2ff"), QColor("#ffd59a")},
        .speed = 0.75,
        .intensity = 0.5};
    const auto authored = app.theme.createSeedTheme(
        {false, QColor("#0a0d18"), QColor("#e1e5f2"), QColor("#8fb2ff")}, {},
        effect);
    const auto key = app.theme.saveCustomTheme("Night shift", authored);
    EXPECT_TRUE(app.theme.isAnimatedTheme(key));
    EXPECT_EQ(app.theme.effect(), effect);

    const auto stored = app.theme.customThemesJson.getValue();
    app.theme.appearanceMode = "Light";
    EXPECT_EQ(app.theme.effect().kind, ThemeEffectKind::Starfield);
    EXPECT_EQ(app.theme.customThemesJson.getValue(), stored);
    app.theme.appearanceMode = "Dark";
    EXPECT_EQ(app.theme.effect(), effect);
}
