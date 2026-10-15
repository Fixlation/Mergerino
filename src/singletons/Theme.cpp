// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "singletons/Theme.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/QLogging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStyleHints>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

using namespace chatterino;
using namespace literals;

QString retainedPresetKey(const QString &key)
{
    static const QHash<QString, QString> replacements{
        {u"Black"_s, u"Dark"_s},
        {u"preset.forge.dark"_s, u"Dark"_s},
        {u"preset.northwind.dark"_s, u"Dark"_s},
        {u"preset.rosewood.dark"_s, u"preset.cafe.dark"_s},
        {u"preset.hearth.dark"_s, u"preset.evergreen.dark"_s},
        {u"preset.signal.dark"_s, u"preset.crimson.dark"_s},
        {u"White"_s, u"Light"_s},
        {u"preset.almond.light"_s, u"preset.rosewood.light"_s},
        {u"preset.circuit.dark"_s, u"preset.forge.dark"_s},
        {u"preset.circuit.light"_s, u"preset.forge.light"_s},
        {u"preset.evergreen.light"_s, u"preset.sage.light"_s},
        {u"preset.glacier.dark"_s, u"preset.afterhours.dark"_s},
        {u"preset.glacier.light"_s, u"preset.forge.light"_s},
        {u"preset.iris.dark"_s, u"preset.forge.dark"_s},
        {u"preset.iris.light"_s, u"preset.cafe.light"_s},
        {u"preset.lantern.dark"_s, u"preset.signal.dark"_s},
        {u"preset.mineral.dark"_s, u"preset.hearth.dark"_s},
        {u"preset.monochrome.dark"_s, u"Black"_s},
        {u"preset.monochrome.light"_s, u"Light"_s},
        {u"preset.neonink.dark"_s, u"preset.almond.dark"_s},
        {u"preset.nocturne.dark"_s, u"preset.afterhours.dark"_s},
        {u"preset.paper.dark"_s, u"preset.forge.dark"_s},
        {u"preset.paper.light"_s, u"preset.forge.light"_s},
        {u"preset.quartz.dark"_s, u"preset.forge.dark"_s},
        {u"preset.quartz.light"_s, u"preset.forge.light"_s},
        {u"preset.verdant.dark"_s, u"preset.neongrid.dark"_s},
        {u"preset.workshop.dark"_s, u"preset.forge.dark"_s},
        {u"preset.workshop.light"_s, u"preset.forge.light"_s},
        {u"preset.afterhours.dark"_s, u"preset.forge.dark"_s},
        {u"preset.almond.dark"_s, u"preset.rosewood.dark"_s},
        {u"preset.cafe.light"_s, u"Light"_s},
        {u"preset.forge.light"_s, u"Light"_s},
        {u"preset.hearth.light"_s, u"Light"_s},
        {u"preset.rosewood.light"_s, u"Light"_s},
        {u"preset.saffron.dark"_s, u"preset.amber.dark"_s},
        {u"preset.sage.light"_s, u"Light"_s},
        {u"preset.sentinel.dark"_s, u"preset.cafe.dark"_s},
        {u"preset.signal.light"_s, u"Light"_s},
        {u"preset.sundial.light"_s, u"Light"_s},
    };

    auto retained = key;
    QSet<QString> visited;
    while (!visited.contains(retained))
    {
        visited.insert(retained);
        const auto replacement = replacements.constFind(retained);
        if (replacement == replacements.cend())
        {
            return retained;
        }
        retained = replacement.value();
    }
    return key;
}

void parseInto(const QJsonObject &obj, const QJsonObject &fallbackObj,
               QLatin1String key, QColor &color)
{
    auto parseColorFrom = [](const auto &obj,
                             QLatin1String key) -> std::optional<QColor> {
        auto jsonValue = obj[key];
        if (!jsonValue.isString()) [[unlikely]]
        {
            return std::nullopt;
        }
        QColor parsed = {jsonValue.toString()};
        if (!parsed.isValid()) [[unlikely]]
        {
            qCWarning(chatterinoTheme).nospace()
                << "While parsing " << key << ": '" << jsonValue.toString()
                << "' isn't a valid color.";
            return std::nullopt;
        }
        return parsed;
    };

    auto firstColor = parseColorFrom(obj, key);
    if (firstColor.has_value())
    {
        color = firstColor.value();
        return;
    }

    if (!fallbackObj.isEmpty())
    {
        auto fallbackColor = parseColorFrom(fallbackObj, key);
        if (fallbackColor.has_value())
        {
            color = fallbackColor.value();
            return;
        }
    }

    qCWarning(chatterinoTheme) << key
                               << "was expected but not found in the "
                                  "current theme, and no fallback value found.";
}

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define _c2StringLit(s, ty) s##ty
#define parseColor(to, from, key) \
    parseInto(from, from##Fallback, _c2StringLit(#key, _L1), (to).from.key)
// NOLINTEND(cppcoreguidelines-macro-usage)

void parseWindow(const QJsonObject &window, const QJsonObject &windowFallback,
                 chatterino::Theme &theme)
{
    parseColor(theme, window, background);
    parseColor(theme, window, text);
}

void parseTabs(const QJsonObject &tabs, const QJsonObject &tabsFallback,
               chatterino::Theme &theme)
{
    const auto parseTabColors = [](const auto &json, const auto &jsonFallback,
                                   auto &tab) {
        parseInto(json, jsonFallback, "text"_L1, tab.text);
        {
            const auto backgrounds = json["backgrounds"_L1].toObject();
            const auto backgroundsFallback =
                jsonFallback["backgrounds"_L1].toObject();
            parseColor(tab, backgrounds, regular);
            parseColor(tab, backgrounds, hover);
            parseColor(tab, backgrounds, unfocused);
        }
        {
            const auto line = json["line"_L1].toObject();
            const auto lineFallback = jsonFallback["line"_L1].toObject();
            parseColor(tab, line, regular);
            parseColor(tab, line, hover);
            parseColor(tab, line, unfocused);
        }
    };
    parseColor(theme, tabs, dividerLine);
    parseColor(theme, tabs, liveIndicator);
    parseColor(theme, tabs, rerunIndicator);
    parseTabColors(tabs["regular"_L1].toObject(),
                   tabsFallback["regular"_L1].toObject(), theme.tabs.regular);
    parseTabColors(tabs["newMessage"_L1].toObject(),
                   tabsFallback["newMessage"_L1].toObject(),
                   theme.tabs.newMessage);
    parseTabColors(tabs["highlighted"_L1].toObject(),
                   tabsFallback["highlighted"_L1].toObject(),
                   theme.tabs.highlighted);
    parseTabColors(tabs["selected"_L1].toObject(),
                   tabsFallback["selected"_L1].toObject(), theme.tabs.selected);
}

void parseTextColors(const QJsonObject &textColors,
                     const QJsonObject &textColorsFallback, auto &messages)
{
    parseColor(messages, textColors, regular);
    parseColor(messages, textColors, caret);
    parseColor(messages, textColors, link);
    parseColor(messages, textColors, system);
    parseColor(messages, textColors, chatPlaceholder);
}

void parseMessageBackgrounds(const QJsonObject &backgrounds,
                             const QJsonObject &backgroundsFallback,
                             auto &messages)
{
    parseColor(messages, backgrounds, regular);
    parseColor(messages, backgrounds, alternate);
}

void parseMessages(const QJsonObject &messages,
                   const QJsonObject &messagesFallback,
                   chatterino::Theme &theme)
{
    parseTextColors(messages["textColors"_L1].toObject(),
                    messagesFallback["textColors"_L1].toObject(),
                    theme.messages);
    parseMessageBackgrounds(messages["backgrounds"_L1].toObject(),
                            messagesFallback["backgrounds"_L1].toObject(),
                            theme.messages);
    parseColor(theme, messages, disabled);
    parseColor(theme, messages, selection);
    parseColor(theme, messages, highlightAnimationStart);
    parseColor(theme, messages, highlightAnimationEnd);
}

void parseOverlayMessages(const QJsonObject &overlayMessages,
                          const QJsonObject &overlayMessagesFallback,
                          chatterino::Theme &theme)
{
    parseTextColors(overlayMessages["textColors"_L1].toObject(),
                    overlayMessagesFallback["textColors"_L1].toObject(),
                    theme.overlayMessages);
    parseMessageBackgrounds(
        overlayMessages["backgrounds"_L1].toObject(),
        overlayMessagesFallback["backgrounds"_L1].toObject(),
        theme.overlayMessages);
    parseColor(theme, overlayMessages, disabled);
    parseColor(theme, overlayMessages, selection);
    parseColor(theme, overlayMessages, background);
}

void parseScrollbars(const QJsonObject &scrollbars,
                     const QJsonObject &scrollbarsFallback,
                     chatterino::Theme &theme)
{
    parseColor(theme, scrollbars, background);
    parseColor(theme, scrollbars, thumb);
    parseColor(theme, scrollbars, thumbSelected);
}

void parseSplits(const QJsonObject &splits, const QJsonObject &splitsFallback,
                 chatterino::Theme &theme)
{
    parseColor(theme, splits, messageSeperator);
    parseColor(theme, splits, background);
    parseColor(theme, splits, dropPreview);
    parseColor(theme, splits, dropPreviewBorder);
    parseColor(theme, splits, dropTargetRect);
    parseColor(theme, splits, dropTargetRectBorder);
    parseColor(theme, splits, resizeHandle);
    parseColor(theme, splits, resizeHandleBackground);

    {
        const auto header = splits["header"_L1].toObject();
        const auto headerFallback = splitsFallback["header"_L1].toObject();
        parseColor(theme.splits, header, border);
        parseColor(theme.splits, header, focusedBorder);
        parseColor(theme.splits, header, background);
        parseColor(theme.splits, header, focusedBackground);
        parseColor(theme.splits, header, text);
        parseColor(theme.splits, header, focusedText);
    }
    {
        const auto input = splits["input"_L1].toObject();
        const auto inputFallback = splitsFallback["input"_L1].toObject();
        parseColor(theme.splits, input, background);
        parseColor(theme.splits, input, backgroundPulse);
        parseColor(theme.splits, input, text);
        theme.splits.input.border =
            theme.tabs.selected.backgrounds.regular;
        if (input.contains("border"_L1) ||
            inputFallback.contains("border"_L1))
        {
            parseColor(theme.splits, input, border);
        }
    }
}

void parseUsercard(const QJsonObject &usercard,
                   const QJsonObject &usercardFallback,
                   chatterino::Theme &theme)
{
    theme.usercard.background = theme.window.background;
    theme.usercard.text = theme.window.text;
    theme.usercard.secondaryText = theme.messages.textColors.system;
    theme.usercard.accent = theme.accent;

    if (usercard.isEmpty() && usercardFallback.isEmpty())
    {
        return;
    }

    parseColor(theme, usercard, background);
    parseColor(theme, usercard, text);
    parseColor(theme, usercard, secondaryText);
    parseColor(theme, usercard, accent);
}

void parseColors(const QJsonObject &root, const QJsonObject &fallbackTheme,
                 chatterino::Theme &theme)
{
    const auto colors = root["colors"_L1].toObject();
    const auto fallbackColors = fallbackTheme["colors"_L1].toObject();

    parseInto(colors, fallbackColors, "accent"_L1, theme.accent);

    parseWindow(colors["window"_L1].toObject(),
                fallbackColors["window"_L1].toObject(), theme);
    parseTabs(colors["tabs"_L1].toObject(),
              fallbackColors["tabs"_L1].toObject(), theme);
    parseMessages(colors["messages"_L1].toObject(),
                  fallbackColors["messages"_L1].toObject(), theme);
    parseUsercard(colors["usercard"_L1].toObject(),
                  fallbackColors["usercard"_L1].toObject(), theme);
    parseOverlayMessages(colors["overlayMessages"_L1].toObject(),
                         fallbackColors["overlayMessages"_L1].toObject(),
                         theme);
    parseScrollbars(colors["scrollbars"_L1].toObject(),
                    fallbackColors["scrollbars"_L1].toObject(), theme);
    parseSplits(colors["splits"_L1].toObject(),
                fallbackColors["splits"_L1].toObject(), theme);
}
#undef parseColor
#undef _c2StringLit

std::optional<QJsonObject> loadThemeFromPath(const QString &path)
{
    QFile file(path);
    if (!file.open(QFile::ReadOnly))
    {
        qCWarning(chatterinoTheme)
            << "Failed to open" << file.fileName() << "at" << path;
        return std::nullopt;
    }

    QJsonParseError error{};
    auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (!json.isObject())
    {
        qCWarning(chatterinoTheme) << "Failed to parse" << file.fileName()
                                   << "error:" << error.errorString();
        return std::nullopt;
    }

    // TODO: Validate JSON schema?

    return json.object();
}

/**
 * Load the given theme descriptor from its path
 *
 * Returns a JSON object containing theme data if the theme is valid, otherwise nullopt
 *
 * NOTE: No theme validation is done by this function
 **/
std::optional<QJsonObject> loadTheme(const ThemeDescriptor &theme)
{
    if (!theme.data.isEmpty())
    {
        return theme.data;
    }
    return loadThemeFromPath(theme.path);
}

std::optional<ThemeSeed> seedFromThemeJson(const QJsonObject &theme)
{
    const auto metadata = theme["metadata"_L1].toObject();
    const auto savedSeed = metadata["mergerinoSeed"_L1].toObject();
    const auto colors = theme["colors"_L1].toObject();
    const auto window = colors["window"_L1].toObject();

    ThemeSeed seed;
    seed.light = metadata["iconTheme"_L1].toString() == u"dark"_s;
    seed.surface = QColor(
        savedSeed["surface"_L1].toString(window["background"_L1].toString()));
    seed.ink =
        QColor(savedSeed["ink"_L1].toString(window["text"_L1].toString()));
    seed.accent =
        QColor(savedSeed["accent"_L1].toString(colors["accent"_L1].toString()));
    if (!seed.surface.isValid() || !seed.ink.isValid() ||
        !seed.accent.isValid())
    {
        return std::nullopt;
    }
    return seed;
}

QColor mixColors(const QColor &from, const QColor &to, qreal amount)
{
    const auto mixChannel = [amount](int start, int end) {
        return qRound(start + (end - start) * amount);
    };
    return QColor(mixChannel(from.red(), to.red()),
                  mixChannel(from.green(), to.green()),
                  mixChannel(from.blue(), to.blue()));
}

QString colorName(const QColor &color)
{
    return color.name(QColor::HexRgb);
}

double luminance(const QColor &color)
{
    const auto linear = [](double component) {
        return component <= 0.04045
                   ? component / 12.92
                   : std::pow((component + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
           0.0722 * linear(color.blueF());
}

QColor readableColor(QColor color, const QColor &background)
{
    const auto contrast = [&background](const QColor &candidate) {
        const auto a = luminance(candidate);
        const auto b = luminance(background);
        return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    };
    if (!color.isValid() || !background.isValid() || contrast(color) >= 4.5)
    {
        return color;
    }
    const auto target = contrast(Qt::black) > contrast(Qt::white)
                            ? QColor(Qt::black)
                            : QColor(Qt::white);
    const auto original = color;
    for (int step = 1; step <= 100; ++step)
    {
        color = mixColors(original, target, step / 100.0);
        if (contrast(color) >= 4.5)
        {
            break;
        }
    }
    color.setAlpha(original.alpha());
    return color;
}

QColor oppositeModeColor(const QColor &color)
{
    return QColor::fromHslF(color.hslHueF(), color.hslSaturationF(),
                            1.0 - color.lightnessF(), color.alphaF());
}

QJsonObject convertModeColors(QJsonObject object, const QColor &oldAccent,
                              const QColor &newAccent)
{
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        if (it.value().isObject())
        {
            it.value() =
                convertModeColors(it.value().toObject(), oldAccent, newAccent);
        }
        else if (it.value().isString())
        {
            const QColor color(it.value().toString());
            if (color.isValid())
            {
                const auto converted =
                    color == oldAccent ? newAccent : oppositeModeColor(color);
                it.value() = converted.name(QColor::HexArgb);
            }
        }
    }
    return object;
}

void ensureReadable(QJsonObject &object, const QString &textKey,
                    const QColor &background)
{
    const QColor text(object.value(textKey).toString());
    if (text.isValid() && background.isValid())
    {
        object[textKey] = readableColor(text, background).name(QColor::HexArgb);
    }
}

QString colorWithAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color.name(QColor::HexArgb);
}

QJsonObject tabColors(const QColor &background, const QColor &hover,
                      const QColor &line, const QColor &text)
{
    return {
        {"backgrounds", QJsonObject{{"regular", colorName(background)},
                                    {"hover", colorName(hover)},
                                    {"unfocused", colorName(background)}}},
        {"line", QJsonObject{{"regular", colorName(line)},
                             {"hover", colorName(line)},
                             {"unfocused", colorName(line)}}},
        {"text", colorName(text)},
    };
}

QString storedColor(const QColor &color)
{
    if (!color.isValid())
    {
        return {};
    }

    return color.name(color.alpha() == 255 ? QColor::HexRgb
                                           : QColor::HexArgb);
}

QJsonObject makeAdvancedSeedJson(const ThemeAdvancedSeed &advanced)
{
    return {
        {"enabled", advanced.enabled},
        {"accent", QJsonObject{{"enabled", advanced.accent.enabled},
                               {"color", storedColor(advanced.accent.color)}}},
        {"window",
         QJsonObject{{"enabled", advanced.window.enabled},
                     {"background", storedColor(advanced.window.background)},
                     {"text", storedColor(advanced.window.text)}}},
        {"chat",
         QJsonObject{
             {"enabled", advanced.chat.enabled},
             {"background", storedColor(advanced.chat.background)},
             {"alternateBackground",
              storedColor(advanced.chat.alternateBackground)},
             {"text", storedColor(advanced.chat.text)},
             {"systemText", storedColor(advanced.chat.systemText)},
             {"link", storedColor(advanced.chat.link)},
         }},
        {"input",
         QJsonObject{{"enabled", advanced.input.enabled},
                     {"background", storedColor(advanced.input.background)},
                     {"text", storedColor(advanced.input.text)},
                     {"border", storedColor(advanced.input.border)}}},
        {"header",
         QJsonObject{
             {"enabled", advanced.header.enabled},
             {"background", storedColor(advanced.header.background)},
             {"text", storedColor(advanced.header.text)},
             {"focusedBackground",
              storedColor(advanced.header.focusedBackground)},
             {"accent", storedColor(advanced.header.accent)},
         }},
        {"tabs",
         QJsonObject{
             {"enabled", advanced.tabs.enabled},
             {"background", storedColor(advanced.tabs.background)},
             {"selectedBackground",
              storedColor(advanced.tabs.selectedBackground)},
             {"text", storedColor(advanced.tabs.text)},
             {"accent", storedColor(advanced.tabs.accent)},
         }},
        {"usercard",
         QJsonObject{
             {"enabled", advanced.usercard.enabled},
             {"background", storedColor(advanced.usercard.background)},
             {"text", storedColor(advanced.usercard.text)},
             {"secondaryText", storedColor(advanced.usercard.secondaryText)},
             {"accent", storedColor(advanced.usercard.accent)},
         }},
        {"scrollbars",
         QJsonObject{
             {"enabled", advanced.scrollbars.enabled},
             {"background", storedColor(advanced.scrollbars.background)},
             {"thumb", storedColor(advanced.scrollbars.thumb)},
             {"thumbSelected", storedColor(advanced.scrollbars.thumbSelected)},
         }},
    };
}

QJsonObject makeSeedThemeJson(const ThemeSeed &seed,
                              const ThemeAdvancedSeed &advanced = {})
{
    const auto tone = [&seed](qreal amount) {
        return mixColors(seed.surface, seed.ink, amount);
    };
    const auto subtle = tone(0.04);
    const auto hover = tone(0.075);
    const auto raised = tone(0.11);
    const auto selected = tone(0.16);
    const auto muted = mixColors(seed.ink, seed.surface, 0.42);
    const auto quiet = mixColors(seed.ink, seed.surface, 0.58);
    const auto pulse = mixColors(seed.surface, QColor("#36b359"), 0.22);
    // Text-sized accents must stay readable; lines and fills keep the seed
    const auto accentText = readableColor(seed.accent, seed.surface);

    // The window chrome behind the tab strip sits one step further from the
    // ink than the chat, like the Default theme: darker in dark mode, lighter
    // in light mode.
    const auto darkInterface = luminance(seed.ink) > luminance(seed.surface);
    const auto chrome =
        darkInterface ? mixColors(seed.surface, QColor(Qt::black), 0.30)
                      : mixColors(seed.surface, QColor(Qt::white), 0.55);
    const auto chromeTone = [&seed, &chrome](qreal amount) {
        return mixColors(chrome, seed.ink, amount);
    };
    const auto tabText = mixColors(seed.ink, chrome, 0.38);

    const QJsonObject textColors{
        {"caret", colorName(seed.ink)},  {"chatPlaceholder", colorName(quiet)},
        {"link", colorName(accentText)}, {"regular", colorName(seed.ink)},
        {"system", colorName(muted)},
    };

    QJsonObject metadata{
        {"iconTheme", seed.light ? "dark" : "light"},
        {"fallbackTheme", seed.light ? "Light" : "Dark"},
        {"mergerinoSeed",
         QJsonObject{{"variant", seed.light ? "Light" : "Dark"},
                     {"surface", colorName(seed.surface)},
                     {"ink", colorName(seed.ink)},
                     {"accent", colorName(seed.accent)}}},
    };
    metadata["mergerinoAdvanced"] = makeAdvancedSeedJson(advanced);

    QJsonObject colors{
        {"accent", colorName(seed.accent)},
        {"window", QJsonObject{{"background", colorName(chrome)},
                               {"text", colorName(seed.ink)}}},
        {"usercard",
         QJsonObject{{"background", colorName(seed.surface)},
                     {"text", colorName(seed.ink)},
                     {"secondaryText", colorName(muted)},
                     {"accent", colorName(seed.accent)}}},
        {"messages",
         QJsonObject{
             {"backgrounds", QJsonObject{{"regular", colorName(seed.surface)},
                                         {"alternate", colorName(subtle)}}},
             {"disabled", colorWithAlpha(seed.surface, 0x99)},
             {"highlightAnimationStart", colorWithAlpha(seed.accent, 0x6e)},
             {"highlightAnimationEnd", colorWithAlpha(seed.accent, 0x00)},
             {"selection", colorWithAlpha(seed.accent, 0x40)},
             {"textColors", textColors},
         }},
        {"scrollbars",
         QJsonObject{{"background", colorWithAlpha(seed.surface, 0x00)},
                     {"thumb", colorName(selected)},
                     {"thumbSelected",
                      colorName(mixColors(seed.surface, seed.ink, 0.24))}}},
        {"splits",
         QJsonObject{
             {"background", colorName(seed.surface)},
             {"dropPreview", colorWithAlpha(seed.accent, 0x30)},
             {"dropPreviewBorder", colorName(seed.accent)},
             {"dropTargetRect", colorWithAlpha(seed.accent, 0x20)},
             {"dropTargetRectBorder", colorName(seed.accent)},
             {"header", QJsonObject{{"background", colorName(tone(0.06))},
                                    {"border", colorName(tone(0.09))},
                                    {"focusedBackground", colorName(raised)},
                                    {"focusedBorder", colorName(selected)},
                                    {"focusedText", colorName(readableColor(
                                                        seed.accent, raised))},
                                    {"text", colorName(seed.ink)}}},
             {"input", QJsonObject{{"background", colorName(tone(0.05))},
                                   {"backgroundPulse", colorName(pulse)},
                                   {"text", colorName(seed.ink)},
                                   {"border", colorName(tone(0.10))}}},
             {"messageSeperator", colorName(hover)},
             {"resizeHandle", colorWithAlpha(seed.accent, 0x70)},
             {"resizeHandleBackground", colorWithAlpha(seed.accent, 0x20)},
         }},
        {"tabs",
         QJsonObject{
             {"dividerLine", colorName(chromeTone(0.10))},
             {"liveIndicator", "#ff4d4f"},
             {"rerunIndicator", "#d2ad31"},
             {"regular", tabColors(chromeTone(0.06), chromeTone(0.10),
                                   chromeTone(0.10), tabText)},
             {"newMessage", tabColors(chromeTone(0.06), chromeTone(0.10),
                                      tabText, seed.ink)},
             {"highlighted", tabColors(chromeTone(0.06), chromeTone(0.10),
                                       seed.accent, seed.ink)},
             {"selected", tabColors(chromeTone(0.17), chromeTone(0.21),
                                    seed.accent, seed.ink)},
         }},
    };

    if (!advanced.enabled)
    {
        return {{"metadata", metadata}, {"colors", colors}};
    }

    if (advanced.accent.enabled && advanced.accent.color.isValid())
    {
        const auto color = advanced.accent.color;
        colors["accent"] = storedColor(color);

        auto messages = colors["messages"].toObject();
        auto text = messages["textColors"].toObject();
        text["link"] = storedColor(color);
        messages["textColors"] = text;
        messages["selection"] = colorWithAlpha(color, 0x40);
        messages["highlightAnimationStart"] = colorWithAlpha(color, 0x6e);
        messages["highlightAnimationEnd"] = colorWithAlpha(color, 0x00);
        colors["messages"] = messages;

        auto splits = colors["splits"].toObject();
        splits["dropPreview"] = colorWithAlpha(color, 0x30);
        splits["dropPreviewBorder"] = storedColor(color);
        splits["dropTargetRect"] = colorWithAlpha(color, 0x20);
        splits["dropTargetRectBorder"] = storedColor(color);
        splits["resizeHandle"] = colorWithAlpha(color, 0x70);
        splits["resizeHandleBackground"] = colorWithAlpha(color, 0x20);
        auto header = splits["header"].toObject();
        header["focusedText"] = storedColor(color);
        splits["header"] = header;
        auto input = splits["input"].toObject();
        input["border"] = storedColor(color);
        splits["input"] = input;
        colors["splits"] = splits;

        auto tabs = colors["tabs"].toObject();
        auto highlighted = tabs["highlighted"].toObject();
        auto highlightedLine = highlighted["line"].toObject();
        highlightedLine["regular"] = storedColor(color);
        highlightedLine["hover"] = storedColor(color);
        highlightedLine["unfocused"] = storedColor(color);
        highlighted["line"] = highlightedLine;
        tabs["highlighted"] = highlighted;
        auto selectedTab = tabs["selected"].toObject();
        auto selectedLine = selectedTab["line"].toObject();
        selectedLine["regular"] = storedColor(color);
        selectedLine["hover"] = storedColor(color);
        selectedLine["unfocused"] = storedColor(color);
        selectedTab["line"] = selectedLine;
        tabs["selected"] = selectedTab;
        colors["tabs"] = tabs;

        auto usercard = colors["usercard"].toObject();
        usercard["accent"] = storedColor(color);
        colors["usercard"] = usercard;
    }

    if (advanced.window.enabled)
    {
        colors["window"] = QJsonObject{
            {"background", storedColor(advanced.window.background)},
            {"text", storedColor(advanced.window.text)},
        };
    }

    if (advanced.chat.enabled)
    {
        auto messages = colors["messages"].toObject();
        messages["backgrounds"] = QJsonObject{
            {"regular", storedColor(advanced.chat.background)},
            {"alternate", storedColor(advanced.chat.alternateBackground)},
        };
        auto text = messages["textColors"].toObject();
        text["regular"] = storedColor(advanced.chat.text);
        text["caret"] = storedColor(advanced.chat.text);
        text["system"] = storedColor(advanced.chat.systemText);
        text["chatPlaceholder"] = storedColor(advanced.chat.systemText);
        text["link"] = storedColor(advanced.chat.link);
        messages["textColors"] = text;
        colors["messages"] = messages;

        auto splits = colors["splits"].toObject();
        splits["background"] = storedColor(advanced.chat.background);
        colors["splits"] = splits;
    }

    if (advanced.input.enabled)
    {
        auto splits = colors["splits"].toObject();
        auto input = splits["input"].toObject();
        input["background"] = storedColor(advanced.input.background);
        input["text"] = storedColor(advanced.input.text);
        input["border"] = storedColor(advanced.input.border);
        splits["input"] = input;
        colors["splits"] = splits;
    }

    if (advanced.header.enabled)
    {
        auto splits = colors["splits"].toObject();
        auto header = splits["header"].toObject();
        header["background"] = storedColor(advanced.header.background);
        header["border"] = storedColor(advanced.header.background);
        header["text"] = storedColor(advanced.header.text);
        header["focusedBackground"] =
            storedColor(advanced.header.focusedBackground);
        header["focusedBorder"] = storedColor(advanced.header.accent);
        header["focusedText"] = storedColor(advanced.header.accent);
        splits["header"] = header;
        colors["splits"] = splits;
    }

    if (advanced.tabs.enabled)
    {
        const auto tabHover = mixColors(advanced.tabs.background,
                                        advanced.tabs.text, 0.09);
        const auto selectedHover = mixColors(
            advanced.tabs.selectedBackground, advanced.tabs.text, 0.09);
        const auto divider = mixColors(advanced.tabs.background,
                                       advanced.tabs.text, 0.16);
        auto tabs = colors["tabs"].toObject();
        tabs["dividerLine"] = storedColor(divider);
        tabs["regular"] =
            tabColors(advanced.tabs.background, tabHover, divider,
                      advanced.tabs.text);
        tabs["newMessage"] =
            tabColors(advanced.tabs.background, tabHover, divider,
                      advanced.tabs.text);
        tabs["highlighted"] =
            tabColors(advanced.tabs.background, tabHover,
                      advanced.tabs.accent, advanced.tabs.text);
        tabs["selected"] = tabColors(
            advanced.tabs.selectedBackground, selectedHover,
            advanced.tabs.accent, advanced.tabs.text);
        colors["tabs"] = tabs;
    }

    if (advanced.usercard.enabled)
    {
        colors["usercard"] = QJsonObject{
            {"background", storedColor(advanced.usercard.background)},
            {"text", storedColor(advanced.usercard.text)},
            {"secondaryText", storedColor(advanced.usercard.secondaryText)},
            {"accent", storedColor(advanced.usercard.accent)},
        };
    }

    if (advanced.scrollbars.enabled)
    {
        colors["scrollbars"] = QJsonObject{
            {"background", storedColor(advanced.scrollbars.background)},
            {"thumb", storedColor(advanced.scrollbars.thumb)},
            {"thumbSelected", storedColor(advanced.scrollbars.thumbSelected)},
        };
    }

    return {{"metadata", metadata}, {"colors", colors}};
}

/// The same palette for the opposite mode, used when no hand-tuned variant
/// exists: the surface keeps its hue as a soft tint and the ink flips.
ThemeSeed counterpartSeed(const ThemeSeed &seed, bool light)
{
    const auto hue = [](const QColor &color) {
        // Achromatic colours report a hue of -1
        return std::max(color.hslHueF(), 0.0F);
    };
    const auto surface = QColor::fromHslF(
        hue(seed.surface), std::min(seed.surface.hslSaturationF(), 0.35F),
        light ? 0.955F : 0.09F);
    const auto ink = QColor::fromHslF(
        hue(seed.ink),
        std::min(seed.ink.hslSaturationF(), light ? 0.4F : 0.25F),
        light ? 0.17F : 0.9F);
    return {
        .light = light,
        .surface = surface,
        .ink = ink,
        .accent = readableColor(seed.accent, surface),
    };
}

struct PresetSeed {
    const char *surface;
    const char *ink;
    const char *accent;

    ThemeSeed toSeed(bool light) const
    {
        return {light, QColor(surface), QColor(ink), QColor(accent)};
    }
};

ThemeDescriptor makePreset(const char *key, const char *name,
                           PresetSeed dark, PresetSeed light)
{
    return {
        .key = QString::fromLatin1(key),
        .name = QString::fromUtf8(name),
        .data = makeSeedThemeJson(dark.toSeed(false)),
        .lightData = makeSeedThemeJson(light.toSeed(true)),
    };
}

ThemeDescriptor makeAnimatedPreset(const char *key, const char *name,
                                   PresetSeed dark, ThemeEffectKind kind,
                                   std::array<const char *, 3> colors)
{
    ThemeEffect effect{.kind = kind};
    for (size_t index = 0; index < colors.size(); ++index)
    {
        effect.colors[index] = QColor(colors[index]);
    }
    const auto seed = dark.toSeed(false);
    return {
        .key = QString::fromLatin1(key),
        .name = QString::fromUtf8(name),
        .animated = true,
        .data = withThemeEffect(makeSeedThemeJson(seed), effect),
        .lightData =
            withThemeEffect(makeSeedThemeJson(counterpartSeed(seed, true)),
                            themeEffectForMode(effect, true)),
    };
}

/// Whether a theme was generated from base colours alone, so the opposite
/// mode can be regenerated from them instead of inverting every colour.
bool hasPlainSeed(const QJsonObject &theme)
{
    const auto metadata = theme["metadata"_L1].toObject();
    return metadata.contains("mergerinoSeed"_L1) &&
           !metadata["mergerinoAdvanced"_L1]
                .toObject()["enabled"_L1]
                .toBool(false);
}

}  // namespace

namespace chatterino {

const std::vector<ThemeDescriptor> Theme::builtInThemes{
    {
        .key = "Light",
        .path = ":/themes/Light.json",
        .name = "Light",
    },
    {
        .key = "Dark",
        .path = ":/themes/Dark.json",
        .name = "Default",
    },
    // Static colour themes: {surface, ink, accent} for dark, then light
    makePreset("preset.amber.dark", "Amber",
               {"#0f131a", "#c5c3bc", "#e6b450"},
               {"#f8f8f6", "#4f545a", "#b8730a"}),
    makePreset("preset.cafe.dark", "Café",
               {"#1e1e2e", "#cdd6f4", "#cba6f7"},
               {"#eff1f5", "#4c4f69", "#8839ef"}),
    makePreset("preset.copper.dark", "Copper",
               {"#282828", "#ebdbb2", "#fe8019"},
               {"#fbf1c7", "#3c3836", "#af3a03"}),
    makePreset("preset.crimson.dark", "Crimson",
               {"#1c1215", "#f3e7ea", "#ff4f64"},
               {"#fbf6f7", "#3a2a2e", "#c8102e"}),
    makePreset("preset.evergreen.dark", "Evergreen",
               {"#2d353b", "#d3c6aa", "#a7c080"},
               {"#f2efdf", "#4f5b62", "#6f8a00"}),
    makePreset("preset.fjord.dark", "Fjord",
               {"#2e3440", "#e5e9f0", "#88c0d0"},
               {"#eceff4", "#2e3440", "#5e81ac"}),
    makePreset("preset.neongrid.dark", "Neon Grid",
               {"#070b09", "#cdeed6", "#3ce07e"},
               {"#f2f8f4", "#163222", "#0d8a43"}),
    makePreset("preset.sundial.dark", "Sundial",
               {"#002b36", "#a3b1b1", "#268bd2"},
               {"#fdf6e3", "#475b62", "#268bd2"}),

    // Animated themes: a dark palette plus the backdrop painted behind chat
    makeAnimatedPreset("animated.aurora", "Aurora",
                       {"#0a1118", "#dbe7ee", "#46d9a0"},
                       ThemeEffectKind::Aurora,
                       {"#2fe39a", "#22b8d6", "#4f7dff"}),
    makeAnimatedPreset("animated.dusk", "Dusk",
                       {"#17121a", "#f3e7e6", "#ff8f70"},
                       ThemeEffectKind::Drift,
                       {"#ff7a59", "#d9467a", "#f2b35c"}),
    makeAnimatedPreset("animated.embers", "Embers",
                       {"#15100e", "#f2e5d9", "#ff8a3d"},
                       ThemeEffectKind::Embers,
                       {"#ff7a2e", "#ffb54a", "#ff4b2b"}),
    makeAnimatedPreset("animated.rainfall", "Rainfall",
                       {"#10151b", "#d9e1ea", "#78aee0"},
                       ThemeEffectKind::Rain,
                       {"#a9c4e0", "#5d87b3", "#e6f0fa"}),
    makeAnimatedPreset("animated.starlight", "Starlight",
                       {"#0a0d18", "#e1e5f2", "#8fb2ff"},
                       ThemeEffectKind::Starfield,
                       {"#f4f6ff", "#8fb2ff", "#ffd59a"}),
};

// Dark is our default & fallback theme
const ThemeDescriptor Theme::fallbackTheme = Theme::builtInThemes.at(1);

bool Theme::isLightTheme() const
{
    return this->isLight_;
}

bool Theme::isSystemTheme() const
{
    return this->appearanceMode == u"System"_s;
}

bool Theme::useLightMode() const
{
    return this->appearanceMode.getValue() == "Light" ||
           (this->isSystemTheme() &&
            QApplication::styleHints()->colorScheme() ==
                Qt::ColorScheme::Light);
}

QJsonObject Theme::applyMode(const QJsonObject &theme, bool light)
{
    const auto originalMetadata = theme.value("metadata").toObject();
    if ((originalMetadata.value("iconTheme").toString() == "dark") == light)
    {
        return theme;
    }
    const auto effect = themeEffectForMode(themeEffectFromTheme(theme), light);
    if (hasPlainSeed(theme))
    {
        if (const auto seed = seedFromThemeJson(theme))
        {
            return withThemeEffect(
                makeSeedThemeJson(counterpartSeed(*seed, light)), effect);
        }
    }

    const auto originalColors = theme.value("colors").toObject();
    const QColor oldAccent(originalColors.value("accent").toString());
    const QColor background(originalColors.value("window")
                                .toObject()
                                .value("background")
                                .toString());
    const auto newAccent =
        readableColor(oldAccent, oppositeModeColor(background));
    auto result = convertModeColors(theme, oldAccent, newAccent);
    auto metadata = result.value("metadata").toObject();
    metadata["iconTheme"] = light ? "dark" : "light";
    metadata["fallbackTheme"] = light ? "Light" : "Dark";
    auto seed = metadata.value("mergerinoSeed").toObject();
    if (!seed.isEmpty())
    {
        seed["variant"] = light ? "Light" : "Dark";
        metadata["mergerinoSeed"] = seed;
    }
    result["metadata"] = metadata;

    auto colors = result.value("colors").toObject();
    // Overlays sit over video; semantic live/replay indicators keep their hue
    // and brightness regardless of the surrounding interface mode.
    if (originalColors.contains("overlayMessages"))
    {
        colors["overlayMessages"] = originalColors.value("overlayMessages");
    }
    auto window = colors.value("window").toObject();
    ensureReadable(window, "text",
                   QColor(window.value("background").toString()));
    colors["window"] = window;
    auto messages = colors.value("messages").toObject();
    auto text = messages.value("textColors").toObject();
    const QColor chatBackground(
        messages.value("backgrounds").toObject().value("regular").toString());
    for (const auto &key : text.keys())
    {
        ensureReadable(text, key, chatBackground);
    }
    messages["textColors"] = text;
    colors["messages"] = messages;
    auto splits = colors.value("splits").toObject();
    for (const auto &key : {QStringLiteral("input"), QStringLiteral("header")})
    {
        auto section = splits.value(key).toObject();
        ensureReadable(section, "text",
                       QColor(section.value("background").toString()));
        ensureReadable(section, "focusedText",
                       QColor(section.value("focusedBackground").toString()));
        splits[key] = section;
    }
    colors["splits"] = splits;
    auto tabs = colors.value("tabs").toObject();
    const auto originalTabs = originalColors.value("tabs").toObject();
    for (const auto &key :
         {QStringLiteral("liveIndicator"), QStringLiteral("rerunIndicator")})
    {
        if (originalTabs.contains(key))
        {
            tabs[key] = originalTabs.value(key);
        }
    }
    for (const auto &key :
         {QStringLiteral("regular"), QStringLiteral("newMessage"),
          QStringLiteral("highlighted"), QStringLiteral("selected")})
    {
        auto tab = tabs.value(key).toObject();
        ensureReadable(tab, "text",
                       QColor(tab.value("backgrounds")
                                  .toObject()
                                  .value("regular")
                                  .toString()));
        tabs[key] = tab;
    }
    colors["tabs"] = tabs;
    auto usercard = colors.value("usercard").toObject();
    for (const auto &key :
         {QStringLiteral("text"), QStringLiteral("secondaryText"),
          QStringLiteral("accent")})
    {
        ensureReadable(usercard, key,
                       QColor(usercard.value("background").toString()));
    }
    colors["usercard"] = usercard;
    result["colors"] = colors;
    return withThemeEffect(result, effect);
}

Theme::Theme(const Paths &paths)
    : themesDirectory_(paths.themesDirectory)
{
    this->loadAvailableThemes();
    const auto oldSelection = retainedPresetKey(this->themeName.getValue());
    if (this->appearanceMode.getValue() != "Dark" && this->appearanceMode.getValue() != "Light" &&
        this->appearanceMode.getValue() != "System")
    {
        if (oldSelection == "System")
        {
            QStringSetting legacyLight{"/appearance/theme/lightSystem",
                                       "Light"};
            this->themeName =
                retainedPresetKey(QApplication::styleHints()->colorScheme() ==
                                          Qt::ColorScheme::Light
                                      ? legacyLight.getValue()
                                      : this->darkSystemThemeName.getValue());
            this->appearanceMode = "System";
        }
        else
        {
            this->themeName = oldSelection;
            const auto seed = this->seedForTheme(oldSelection);
            this->appearanceMode = seed && seed->light ? "Light" : "Dark";
        }
    }
    else
    {
        this->themeName = oldSelection;
    }
    if (this->themeName.getValue() == "Light" || this->themeName.getValue() == "System")
    {
        this->themeName = "Dark";
    }

    this->themeName.connect(
        [this](auto themeName) {
            qCInfo(chatterinoTheme) << "Theme updated to" << themeName;
            this->update();
        },
        false);
    this->appearanceMode.connect(
        [this](const auto &) {
            this->update();
        },
        false);

    this->customThemesJson.connect(
        [this](const auto &) {
            const auto selected = this->themeName.getValue();
            this->loadAvailableThemes();
            if (!this->findThemeByKey(selected))
            {
                this->themeName = Theme::fallbackTheme.key;
                return;
            }
            this->update();
        },
        false);

    QObject::connect(QApplication::styleHints(),
                     &QStyleHints::colorSchemeChanged, &this->lifetime_,
                     [this] {
                         if (this->isSystemTheme())
                         {
                             this->update();
                             getApp()->getWindows()->forceLayoutChannelViews();
                         }
                     });

    this->update();
}

void Theme::update()
{
    const auto requestedTheme = this->themeName.getValue();
    auto oTheme = this->findThemeByKey(requestedTheme);

    constexpr const double nsToMs = 1.0 / 1000000.0;
    QElapsedTimer timer;
    timer.start();

    const auto light = this->useLightMode();
    std::optional<QJsonObject> themeJSON;
    QString themePath;
    bool useFallback = false;
    if (!oTheme)
    {
        qCWarning(chatterinoTheme)
            << "Theme" << this->themeName
            << "not found, falling back to the fallback theme";
    }
    else
    {
        const auto &theme = *oTheme;
        themeJSON = this->themeForMode(theme, light);
        if (!themeJSON)
        {
            qCWarning(chatterinoTheme)
                << "Theme" << this->themeName
                << "not valid, falling back to the fallback theme";
        }
        else
        {
            themePath = theme.path;
            useFallback = theme.custom || !theme.data.isEmpty();
            this->currentThemeKey_ = theme.key;
        }
    }
    if (!themeJSON)
    {
        themeJSON = this->themeForMode(fallbackTheme, light);
        themePath = fallbackTheme.path;
        this->currentThemeKey_ = fallbackTheme.key;
    }
    auto loadTs = double(timer.nsecsElapsed()) * nsToMs;

    if (!themeJSON)
    {
        qCWarning(chatterinoTheme)
            << "Failed to load" << this->themeName << "or the fallback theme";
        return;
    }

    if (this->isAutoReloading() && this->currentThemeJson_ == *themeJSON)
    {
        return;
    }

    this->parseFrom(*themeJSON, useFallback);
    this->currentThemePath_ = themePath;
    this->currentThemeJson_ = *themeJSON;
    this->effect_ = themeEffectFromTheme(*themeJSON);
    this->updateEffectTimer();

    auto parseTs = double(timer.nsecsElapsed()) * nsToMs;

    this->updated.invoke();
    auto updateTs = double(timer.nsecsElapsed()) * nsToMs;
    qCDebug(chatterinoTheme).nospace().noquote()
        << "Updated theme in " << QString::number(updateTs, 'f', 2)
        << "ms (load: " << QString::number(loadTs, 'f', 2)
        << "ms, parse: " << QString::number(parseTs - loadTs, 'f', 2)
        << "ms, update: " << QString::number(updateTs - parseTs, 'f', 2)
        << "ms)";
}

std::vector<std::pair<QString, QVariant>> Theme::availableThemes() const
{
    std::vector<std::pair<QString, QVariant>> packagedThemes;

    for (const auto &theme : this->availableThemes_)
    {
        // Light is an internal fallback resource, never a colour preset.
        if (theme.key == "Light")
        {
            continue;
        }
        if (theme.custom)
        {
            auto p = std::make_pair(
                QStringLiteral("Custom: %1").arg(theme.name), theme.key);

            packagedThemes.emplace_back(p);
        }
        else
        {
            auto p = std::make_pair(theme.name, theme.key);

            packagedThemes.emplace_back(p);
        }
    }

    std::stable_sort(packagedThemes.begin(), packagedThemes.end(),
                     [](const auto &left, const auto &right) {
                         const bool leftIsDefault =
                             left.second.toString() == "Dark";
                         const bool rightIsDefault =
                             right.second.toString() == "Dark";
                         if (leftIsDefault != rightIsDefault)
                         {
                             return leftIsDefault;
                         }
                         return QString::compare(left.first, right.first,
                                                 Qt::CaseInsensitive) < 0;
                     });

    return packagedThemes;
}

void Theme::loadAvailableThemes()
{
    this->availableThemes_ = Theme::builtInThemes;

    auto dir = QDir(this->themesDirectory_);
    for (const auto &info :
         dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name))
    {
        if (!info.isFile())
        {
            continue;
        }

        if (!info.fileName().endsWith(".json"))
        {
            continue;
        }

        auto themeName = info.baseName();

        auto themeDescriptor = ThemeDescriptor{
            .key = info.fileName(),
            .path = info.absoluteFilePath(),
            .name = themeName,
            .custom = true,
        };

        auto theme = loadTheme(themeDescriptor);
        if (!theme)
        {
            qCWarning(chatterinoTheme) << "Failed to parse theme at" << info;
            continue;
        }
        themeDescriptor.animated = themeEffectFromTheme(*theme).isAnimated();

        this->availableThemes_.emplace_back(std::move(themeDescriptor));
    }

    QJsonParseError error{};
    const auto stored = QJsonDocument::fromJson(
        this->customThemesJson.getValue().toUtf8(), &error);
    if (!stored.isArray())
    {
        if (error.error != QJsonParseError::NoError)
        {
            qCWarning(chatterinoTheme) << "Failed to parse saved custom themes:"
                                       << error.errorString();
        }
        return;
    }

    for (const auto &value : stored.array())
    {
        const auto item = value.toObject();
        const auto id = item["id"_L1].toString();
        const auto name = item["name"_L1].toString();
        const auto data = item["theme"_L1].toObject();
        if (id.isEmpty() || name.isEmpty() ||
            data["colors"_L1].toObject().isEmpty())
        {
            continue;
        }

        this->availableThemes_.emplace_back(ThemeDescriptor{
            .key = u"custom:"_s + id,
            .name = name,
            .custom = true,
            .animated = themeEffectFromTheme(data).isAnimated(),
            .data = data,
        });
    }
}

QJsonObject Theme::createSeedTheme(const ThemeSeed &seed,
                                   const ThemeAdvancedSeed &advanced,
                                   const ThemeEffect &effect) const
{
    return withThemeEffect(makeSeedThemeJson(seed, advanced), effect);
}

QString Theme::saveCustomTheme(const QString &name, const QJsonObject &theme,
                               const QString &existingKey)
{
    const auto cleanName = name.trimmed();
    if (cleanName.isEmpty() || theme["colors"_L1].toObject().isEmpty())
    {
        return {};
    }

    QJsonArray themes;
    const auto stored =
        QJsonDocument::fromJson(this->customThemesJson.getValue().toUtf8());
    if (stored.isArray())
    {
        themes = stored.array();
    }

    QString id;
    if (existingKey.startsWith(u"custom:"_s))
    {
        id = existingKey.mid(7);
    }
    if (id.isEmpty())
    {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    const QJsonObject replacement{
        {"id", id}, {"name", cleanName}, {"theme", theme}};
    bool replaced = false;
    for (qsizetype index = 0; index < themes.size(); ++index)
    {
        if (themes[index].toObject()["id"_L1].toString() == id)
        {
            themes[index] = replacement;
            replaced = true;
            break;
        }
    }
    if (!replaced)
    {
        themes.append(replacement);
    }

    const auto key = u"custom:"_s + id;
    this->customThemesJson =
        QString::fromUtf8(QJsonDocument(themes).toJson(QJsonDocument::Compact));
    this->themeName = key;
    return key;
}

bool Theme::removeCustomTheme(const QString &key)
{
    if (!key.startsWith(u"custom:"_s))
    {
        return false;
    }

    const auto id = key.mid(7);
    const auto stored =
        QJsonDocument::fromJson(this->customThemesJson.getValue().toUtf8());
    if (!stored.isArray())
    {
        return false;
    }

    QJsonArray remaining;
    bool removed = false;
    for (const auto &value : stored.array())
    {
        if (value.toObject()["id"_L1].toString() == id)
        {
            removed = true;
            continue;
        }
        remaining.append(value);
    }
    if (!removed)
    {
        return false;
    }

    if (this->themeName == key)
    {
        this->themeName = Theme::fallbackTheme.key;
    }
    this->customThemesJson = QString::fromUtf8(
        QJsonDocument(remaining).toJson(QJsonDocument::Compact));
    return true;
}

bool Theme::isCustomTheme(const QString &key) const
{
    return key.startsWith(u"custom:"_s);
}

bool Theme::isAnimatedTheme(const QString &key) const
{
    const auto descriptor = this->findThemeByKey(key);
    return descriptor && descriptor->animated;
}

std::optional<QJsonObject> Theme::resolvedThemeJson(const QString &key) const
{
    const auto descriptor = this->findThemeByKey(key);
    if (!descriptor)
    {
        return std::nullopt;
    }
    return this->themeForMode(*descriptor, this->useLightMode());
}

std::optional<ThemeSeed> Theme::seedForTheme(const QString &key) const
{
    const auto descriptor = this->findThemeByKey(key);
    if (!descriptor)
    {
        return std::nullopt;
    }

    // Before initial migration, inspect the theme's authored mode.
    const auto modeChosen = this->appearanceMode.getValue() == "Dark" ||
                            this->appearanceMode.getValue() == "Light" ||
                            this->appearanceMode.getValue() == "System";
    const auto theme = modeChosen
                           ? this->themeForMode(*descriptor,
                                                this->useLightMode())
                           : loadTheme(*descriptor);
    return theme ? seedFromThemeJson(*theme) : std::nullopt;
}

std::optional<ThemeSeed> Theme::currentSeed() const
{
    return seedFromThemeJson(this->currentThemeJson_);
}

ThemeAdvancedSeed Theme::currentAdvancedSeed() const
{
    ThemeAdvancedSeed advanced;
    advanced.accent.color = this->accent;
    advanced.window.background = this->window.background;
    advanced.window.text = this->window.text;
    advanced.chat.background = this->messages.backgrounds.regular;
    advanced.chat.alternateBackground = this->messages.backgrounds.alternate;
    advanced.chat.text = this->messages.textColors.regular;
    advanced.chat.systemText = this->messages.textColors.system;
    advanced.chat.link = this->messages.textColors.link;
    advanced.input.background = this->splits.input.background;
    advanced.input.text = this->splits.input.text;
    advanced.input.border = this->splits.input.border;
    advanced.header.background = this->splits.header.background;
    advanced.header.text = this->splits.header.text;
    advanced.header.focusedBackground =
        this->splits.header.focusedBackground;
    advanced.header.accent = this->splits.header.focusedBorder;
    advanced.tabs.background = this->tabs.regular.backgrounds.regular;
    advanced.tabs.selectedBackground =
        this->tabs.selected.backgrounds.regular;
    advanced.tabs.text = this->tabs.regular.text;
    advanced.tabs.accent = this->tabs.selected.line.regular;
    advanced.usercard.background = this->usercard.background;
    advanced.usercard.text = this->usercard.text;
    advanced.usercard.secondaryText = this->usercard.secondaryText;
    advanced.usercard.accent = this->usercard.accent;
    advanced.scrollbars.background = this->scrollbars.background;
    advanced.scrollbars.thumb = this->scrollbars.thumb;
    advanced.scrollbars.thumbSelected = this->scrollbars.thumbSelected;

    const auto metadata = this->currentThemeJson_["metadata"_L1].toObject();
    const auto stored = metadata["mergerinoAdvanced"_L1].toObject();
    if (stored.isEmpty())
    {
        return advanced;
    }

    const auto readColor = [](const QJsonObject &section, QLatin1String key,
                              QColor &target) {
        const QColor value(section[key].toString());
        if (value.isValid())
        {
            target = value;
        }
    };
    const auto readEnabled = [](const QJsonObject &section) {
        return section["enabled"_L1].toBool(false);
    };

    advanced.enabled = stored["enabled"_L1].toBool(false);
    const auto accent = stored["accent"_L1].toObject();
    advanced.accent.enabled = readEnabled(accent);
    readColor(accent, "color"_L1, advanced.accent.color);

    const auto window = stored["window"_L1].toObject();
    advanced.window.enabled = readEnabled(window);
    readColor(window, "background"_L1, advanced.window.background);
    readColor(window, "text"_L1, advanced.window.text);

    const auto chat = stored["chat"_L1].toObject();
    advanced.chat.enabled = readEnabled(chat);
    readColor(chat, "background"_L1, advanced.chat.background);
    readColor(chat, "alternateBackground"_L1,
              advanced.chat.alternateBackground);
    readColor(chat, "text"_L1, advanced.chat.text);
    readColor(chat, "systemText"_L1, advanced.chat.systemText);
    readColor(chat, "link"_L1, advanced.chat.link);

    const auto input = stored["input"_L1].toObject();
    advanced.input.enabled = readEnabled(input);
    readColor(input, "background"_L1, advanced.input.background);
    readColor(input, "text"_L1, advanced.input.text);
    readColor(input, "border"_L1, advanced.input.border);

    const auto header = stored["header"_L1].toObject();
    advanced.header.enabled = readEnabled(header);
    readColor(header, "background"_L1, advanced.header.background);
    readColor(header, "text"_L1, advanced.header.text);
    readColor(header, "focusedBackground"_L1,
              advanced.header.focusedBackground);
    readColor(header, "accent"_L1, advanced.header.accent);

    const auto tabs = stored["tabs"_L1].toObject();
    advanced.tabs.enabled = readEnabled(tabs);
    readColor(tabs, "background"_L1, advanced.tabs.background);
    readColor(tabs, "selectedBackground"_L1,
              advanced.tabs.selectedBackground);
    readColor(tabs, "text"_L1, advanced.tabs.text);
    readColor(tabs, "accent"_L1, advanced.tabs.accent);

    const auto usercard = stored["usercard"_L1].toObject();
    advanced.usercard.enabled = readEnabled(usercard);
    readColor(usercard, "background"_L1, advanced.usercard.background);
    readColor(usercard, "text"_L1, advanced.usercard.text);
    readColor(usercard, "secondaryText"_L1,
              advanced.usercard.secondaryText);
    readColor(usercard, "accent"_L1, advanced.usercard.accent);

    const auto scrollbars = stored["scrollbars"_L1].toObject();
    advanced.scrollbars.enabled = readEnabled(scrollbars);
    readColor(scrollbars, "background"_L1,
              advanced.scrollbars.background);
    readColor(scrollbars, "thumb"_L1, advanced.scrollbars.thumb);
    readColor(scrollbars, "thumbSelected"_L1,
              advanced.scrollbars.thumbSelected);
    return advanced;
}

const QJsonObject &Theme::currentThemeJson() const
{
    return this->currentThemeJson_;
}

QString Theme::currentThemeDisplayName() const
{
    const auto theme = this->findThemeByKey(this->currentThemeKey_);
    return theme ? theme->name : Theme::fallbackTheme.name;
}

const ThemeEffect &Theme::effect() const
{
    return this->effect_;
}

double Theme::effectSeconds() const
{
    return this->effectSeconds_;
}

std::optional<QJsonObject> Theme::themeForMode(const ThemeDescriptor &theme,
                                               bool light) const
{
    if (theme.key == "Dark" && light)
    {
        return loadThemeFromPath(":/themes/Light.json");
    }
    if (light && !theme.lightData.isEmpty())
    {
        return theme.lightData;
    }
    const auto json = loadTheme(theme);
    if (!json)
    {
        return std::nullopt;
    }
    return Theme::applyMode(*json, light);
}

void Theme::updateEffectTimer()
{
    if (!this->effect_.isAnimated())
    {
        this->effectTimer_.reset();
        return;
    }

    if (!this->effectTimer_)
    {
        this->effectTimer_ = std::make_unique<QTimer>();
        this->effectClock_.start();
        QObject::connect(this->effectTimer_.get(), &QTimer::timeout,
                         &this->lifetime_, [this] {
                             // Follow the emote setting: pause while
                             // Mergerino is in the background
                             if (getSettings()->animationsWhenFocused &&
                                 QApplication::activeWindow() == nullptr)
                             {
                                 return;
                             }
                             this->effectSeconds_ =
                                 double(this->effectClock_.elapsed()) / 1000.0;
                             this->effectFrame.invoke();
                         });
    }
    this->effectTimer_->setInterval(this->effect_.frameInterval());
    if (!this->effectTimer_->isActive())
    {
        this->effectTimer_->start();
    }
}

std::optional<ThemeDescriptor> Theme::findThemeByKey(const QString &key) const
{
    for (const auto &theme : this->availableThemes_)
    {
        if (theme.key == key)
        {
            return theme;
        }
    }

    return std::nullopt;
}

void Theme::parseFrom(const QJsonObject &root, bool isCustomTheme)
{
    this->isLight_ =
        root["metadata"_L1]["iconTheme"_L1].toString() == u"dark"_s;

    std::optional<QJsonObject> fallbackTheme;
    if (isCustomTheme)
    {
        // Only attempt to load a fallback theme if the theme we're loading is a custom theme
        auto fallbackThemeName = retainedPresetKey(
            root["metadata"_L1]["fallbackTheme"_L1].toString(
                this->isLightTheme() ? "Light" : "Dark"));
        for (const auto &theme : Theme::builtInThemes)
        {
            if (fallbackThemeName.compare(theme.key, Qt::CaseInsensitive) == 0)
            {
                fallbackTheme = loadTheme(theme);
                break;
            }
        }
    }

    parseColors(root, fallbackTheme.value_or(QJsonObject()), *this);

    this->splits.input.styleSheet = uR"(
        background: %1;
        border: %2;
        color: %3;
        selection-background-color: %4;
    )"_s.arg(
        this->splits.input.background.name(QColor::HexArgb),
        this->splits.input.border.name(QColor::HexArgb),
        this->splits.input.text.name(QColor::HexArgb),
        this->isLightTheme()
            ? u"#68B1FF"_s
            : this->tabs.selected.backgrounds.regular.name(QColor::HexArgb));

    // Usercard buttons
    if (this->isLightTheme())
    {
        this->buttons.copy = getResources().buttons.copyDark;
    }
    else
    {
        this->buttons.copy = getResources().buttons.copyLight;
    }

    auto palette = QApplication::palette();
    if (this->currentThemeKey_ == "Dark" && !this->isLightTheme())
    {
        // Default/Dark uses the original 1.3.3 Fusion control palette.
        this->palette = palette;
        return;
    }
    palette.setColor(QPalette::Window, this->window.background);
    palette.setColor(QPalette::WindowText, this->window.text);
    palette.setColor(QPalette::Base, this->splits.input.background);
    palette.setColor(QPalette::AlternateBase,
                     this->messages.backgrounds.alternate);
    palette.setColor(QPalette::Text, this->splits.input.text);
    palette.setColor(QPalette::Button, this->tabs.regular.backgrounds.regular);
    palette.setColor(QPalette::ButtonText, this->window.text);
    palette.setColor(QPalette::ToolTipBase, this->window.background);
    palette.setColor(QPalette::ToolTipText, this->window.text);
    palette.setColor(QPalette::Link, this->messages.textColors.link);
    palette.setColor(QPalette::LinkVisited, this->messages.textColors.link);
    palette.setColor(QPalette::Highlight, this->accent);
    palette.setColor(QPalette::HighlightedText,
                     readableColor(Qt::white, this->accent));
    palette.setColor(QPalette::PlaceholderText,
                     this->messages.textColors.system);
    for (const auto role :
         {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
    {
        palette.setColor(QPalette::Disabled, role,
                         this->messages.textColors.system);
    }
    this->palette = palette;
}

bool Theme::isAutoReloading() const
{
    return this->themeReloadTimer_ != nullptr;
}

void Theme::setAutoReload(bool autoReload)
{
    if (autoReload == this->isAutoReloading())
    {
        return;
    }

    if (!autoReload)
    {
        this->themeReloadTimer_.reset();
        this->currentThemeJson_ = {};
        return;
    }

    this->themeReloadTimer_ = std::make_unique<QTimer>();
    QObject::connect(this->themeReloadTimer_.get(), &QTimer::timeout, [this]() {
        this->update();
    });
    this->themeReloadTimer_->setInterval(Theme::AUTO_RELOAD_INTERVAL_MS);
    this->themeReloadTimer_->start();

    qCDebug(chatterinoTheme) << "Enabled theme watcher";
}

void Theme::normalizeColor(QColor &color) const
{
    if (this->isLightTheme())
    {
        if (color.lightnessF() > 0.5)
        {
            color.setHslF(color.hueF(), color.saturationF(), 0.5);
        }

        if (color.lightnessF() > 0.4 && color.hueF() > 0.1 &&
            color.hueF() < 0.33333)
        {
            color.setHslF(
                color.hueF(), color.saturationF(),
                color.lightnessF() - sin((color.hueF() - 0.1) / (0.3333 - 0.1) *
                                         std::numbers::pi) *
                                         color.saturationF() * 0.4);
        }
    }
    else
    {
        if (color.lightnessF() < 0.5)
        {
            color.setHslF(color.hueF(), color.saturationF(), 0.5);
        }

        if (color.lightnessF() < 0.6 && color.hueF() > 0.54444 &&
            color.hueF() < 0.83333)
        {
            color.setHslF(color.hueF(), color.saturationF(),
                          color.lightnessF() +
                              sin((color.hueF() - 0.54444) /
                                  (0.8333 - 0.54444) * std::numbers::pi) *
                                  color.saturationF() * 0.4);
        }
    }
}

Theme *getTheme()
{
    return getApp()->getThemes();
}

}  // namespace chatterino
