// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace chatterino {

/// The ambient animation an animated theme paints behind chat messages.
enum class ThemeEffectKind : std::uint8_t {
    None,
    Aurora,
    Drift,
    Starfield,
    Embers,
    Rain,
};

struct ThemeEffect {
    static constexpr double MIN_SPEED = 0.25;
    static constexpr double MAX_SPEED = 2.0;

    ThemeEffectKind kind = ThemeEffectKind::None;
    std::array<QColor, 3> colors;
    /// Multiplier for the animation clock
    double speed = 1.0;
    /// How strongly the effect tints the chat background, 0 to 1
    double intensity = 0.6;

    bool isAnimated() const
    {
        return this->kind != ThemeEffectKind::None;
    }

    /// Milliseconds between frames; slow ambient effects need fewer frames
    int frameInterval() const;

    bool operator==(const ThemeEffect &other) const = default;
};

/// The effects users can pick from, in display order (excludes None)
const std::vector<std::pair<QString, ThemeEffectKind>> &themeEffectKinds();

QString themeEffectKindKey(ThemeEffectKind kind);
ThemeEffectKind themeEffectKindFromKey(const QString &key);
QString themeEffectKindName(ThemeEffectKind kind);

/// Reads `metadata.mergerinoEffect` from a theme. Missing or malformed
/// effects yield a static (None) effect.
ThemeEffect themeEffectFromTheme(const QJsonObject &theme);

/// Returns `theme` with `metadata.mergerinoEffect` set (or removed for None)
QJsonObject withThemeEffect(QJsonObject theme, const ThemeEffect &effect);

/// Adjusts effect colours authored for one mode so they still read well on
/// light (or dark) backgrounds.
ThemeEffect themeEffectForMode(ThemeEffect effect, bool light);

}  // namespace chatterino
