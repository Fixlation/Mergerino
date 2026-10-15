// SPDX-License-Identifier: MIT

#include "singletons/ThemeEffect.hpp"

#include <QJsonArray>

#include <algorithm>

namespace chatterino {

namespace {

QString storedColor(const QColor &color)
{
    return color.name(color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
}

}  // namespace

int ThemeEffect::frameInterval() const
{
    switch (this->kind)
    {
        case ThemeEffectKind::Embers:
        case ThemeEffectKind::Rain:
            return 33;
        case ThemeEffectKind::Aurora:
        case ThemeEffectKind::Drift:
        case ThemeEffectKind::Starfield:
            return 50;
        case ThemeEffectKind::None:
            break;
    }
    return 0;
}

const std::vector<std::pair<QString, ThemeEffectKind>> &themeEffectKinds()
{
    static const std::vector<std::pair<QString, ThemeEffectKind>> kinds{
        {"Aurora", ThemeEffectKind::Aurora},
        {"Colour drift", ThemeEffectKind::Drift},
        {"Starfield", ThemeEffectKind::Starfield},
        {"Embers", ThemeEffectKind::Embers},
        {"Rain", ThemeEffectKind::Rain},
    };
    return kinds;
}

QString themeEffectKindKey(ThemeEffectKind kind)
{
    switch (kind)
    {
        case ThemeEffectKind::Aurora:
            return "aurora";
        case ThemeEffectKind::Drift:
            return "drift";
        case ThemeEffectKind::Starfield:
            return "starfield";
        case ThemeEffectKind::Embers:
            return "embers";
        case ThemeEffectKind::Rain:
            return "rain";
        case ThemeEffectKind::None:
            break;
    }
    return "none";
}

ThemeEffectKind themeEffectKindFromKey(const QString &key)
{
    for (const auto kind :
         {ThemeEffectKind::Aurora, ThemeEffectKind::Drift,
          ThemeEffectKind::Starfield, ThemeEffectKind::Embers,
          ThemeEffectKind::Rain})
    {
        if (key.compare(themeEffectKindKey(kind), Qt::CaseInsensitive) == 0)
        {
            return kind;
        }
    }
    return ThemeEffectKind::None;
}

QString themeEffectKindName(ThemeEffectKind kind)
{
    for (const auto &[name, value] : themeEffectKinds())
    {
        if (value == kind)
        {
            return name;
        }
    }
    return "None";
}

ThemeEffect themeEffectFromTheme(const QJsonObject &theme)
{
    const auto stored = theme.value("metadata")
                            .toObject()
                            .value("mergerinoEffect")
                            .toObject();
    ThemeEffect effect;
    effect.kind = themeEffectKindFromKey(stored.value("type").toString());
    if (!effect.isAnimated())
    {
        return {};
    }

    const auto colors = stored.value("colors").toArray();
    for (size_t index = 0; index < effect.colors.size(); ++index)
    {
        const auto position = static_cast<qsizetype>(index);
        QColor color(position < colors.size()
                         ? colors.at(position).toString()
                         : QString());
        if (!color.isValid())
        {
            // Fall back to the previous colour so two-colour effects work
            color = index == 0 ? QColor(Qt::white) : effect.colors[index - 1];
        }
        effect.colors[index] = color;
    }
    effect.speed = std::clamp(stored.value("speed").toDouble(1.0),
                              ThemeEffect::MIN_SPEED, ThemeEffect::MAX_SPEED);
    effect.intensity =
        std::clamp(stored.value("intensity").toDouble(0.6), 0.0, 1.0);
    return effect;
}

QJsonObject withThemeEffect(QJsonObject theme, const ThemeEffect &effect)
{
    auto metadata = theme.value("metadata").toObject();
    if (effect.isAnimated())
    {
        QJsonArray colors;
        for (const auto &color : effect.colors)
        {
            colors.append(storedColor(color));
        }
        metadata["mergerinoEffect"] = QJsonObject{
            {"type", themeEffectKindKey(effect.kind)},
            {"colors", colors},
            {"speed", effect.speed},
            {"intensity", effect.intensity},
        };
    }
    else
    {
        metadata.remove("mergerinoEffect");
    }
    theme["metadata"] = metadata;
    return theme;
}

ThemeEffect themeEffectForMode(ThemeEffect effect, bool light)
{
    for (auto &color : effect.colors)
    {
        const auto lightness = color.lightnessF();
        const auto target = light ? std::min(lightness, 0.55F)
                                  : std::max(lightness, 0.5F);
        // Achromatic colours report a hue of -1
        color = QColor::fromHslF(std::max(color.hslHueF(), 0.0F),
                                 color.hslSaturationF(), target,
                                 color.alphaF());
    }
    return effect;
}

}  // namespace chatterino
