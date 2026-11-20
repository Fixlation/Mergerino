// SPDX-License-Identifier: MIT

// "Raid Arcade": an original vertical cabinet shooter seen on an old CRT.
// The game is drawn by hand into a small pixel buffer (3 to 4 window pixels
// per game pixel), given a phosphor bloom, a vignette and a curved-glass
// sheen, then blown up with nearest-neighbour scanlines.
//
// The battlefield lives behind the Activity pane, the tab strip and the band
// around the input box. Under chat text the cabinet only shows a dim
// starfield.

#include "widgets/scenes/ThemeScene.hpp"

#include <QHash>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace chatterino::scenes {

namespace {

using Pix = std::uint32_t;

int ri(double v)
{
    return static_cast<int>(std::lround(v));
}

int fi(double v)
{
    return static_cast<int>(std::floor(v));
}

// Palette ---------------------------------------------------------------------

/// The cabinet only knows these twelve colours.
enum PaletteIndex : std::uint8_t {
    PAL_VOID,
    PAL_NAVY,
    PAL_DUSK,
    PAL_STEEL,
    PAL_PALE,
    PAL_WHITE,
    PAL_MAGENTA,
    PAL_PLUM,
    PAL_CYAN,
    PAL_TEAL,
    PAL_AMBER,
    PAL_EMBER,
    PAL_COUNT,
    /// Sprite pixel that takes the sprite's own core colour
    PAL_CORE = 254,
    PAL_NONE = 255,
};

constexpr std::array<Pix, PAL_COUNT> PALETTE{
    0xff04060dU, 0xff0b1330U, 0xff1a2756U, 0xff3c5390U,
    0xffbfeee0U, 0xfff2fff9U, 0xffff3d9aU, 0xff6a1b5fU,
    0xff42e6ffU, 0xff127089U, 0xffffbe3dU, 0xffff5530U,
};

Pix pal(std::uint8_t index)
{
    return PALETTE[index < PAL_COUNT ? index : PAL_WHITE];
}

/// Multiplies an opaque colour by f / 256.
Pix shade(Pix c, int f)
{
    const auto k = static_cast<Pix>(std::clamp(f, 0, 256));
    const Pix rb = (((c & 0x00ff00ffU) * k) >> 8) & 0x00ff00ffU;
    const Pix g = (((c & 0x0000ff00U) * k) >> 8) & 0x0000ff00U;
    return 0xff000000U | rb | g;
}

/// Mixes two opaque colours, t in 0..256.
Pix mixPix(Pix a, Pix b, int t)
{
    const auto k = static_cast<Pix>(std::clamp(t, 0, 256));
    const Pix ik = 256 - k;
    const Pix rb =
        (((a & 0x00ff00ffU) * ik + (b & 0x00ff00ffU) * k) >> 8) & 0x00ff00ffU;
    const Pix g =
        (((a & 0x0000ff00U) * ik + (b & 0x0000ff00U) * k) >> 8) & 0x0000ff00U;
    return 0xff000000U | rb | g;
}

/// Hot debris cools from white through amber and ember to plum.
std::uint8_t fireInk(double t)
{
    if (t < 0.12)
    {
        return PAL_WHITE;
    }
    if (t < 0.38)
    {
        return PAL_AMBER;
    }
    if (t < 0.68)
    {
        return PAL_EMBER;
    }
    return PAL_PLUM;
}

constexpr int BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

double bayer(int x, int y)
{
    return (BAYER[y & 3][x & 3] + 0.5) / 16.0;
}

/// The palette colour closest to a user's name colour, used for the little
/// core light of that user's drones.
std::uint8_t nearestCore(const QColor &color)
{
    if (!color.isValid())
    {
        return PAL_CYAN;
    }
    constexpr std::array<std::uint8_t, 6> cores{
        PAL_MAGENTA, PAL_CYAN, PAL_AMBER, PAL_EMBER, PAL_PALE, PAL_WHITE};
    std::uint8_t best = PAL_CYAN;
    int bestDistance = 1 << 30;
    for (const auto core : cores)
    {
        const QColor c = QColor::fromRgb(pal(core));
        const int dr = c.red() - color.red();
        const int dg = c.green() - color.green();
        const int db = c.blue() - color.blue();
        const int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (d < bestDistance)
        {
            bestDistance = d;
            best = core;
        }
    }
    return best;
}

// Bitmap font (3x5) -----------------------------------------------------------

/// One row per argument, 3 bits per row, 4 = left column.
constexpr std::uint16_t glyph(int a, int b, int c, int d, int e)
{
    return static_cast<std::uint16_t>((a << 12) | (b << 9) | (c << 6) |
                                      (d << 3) | e);
}

std::uint16_t glyphBits(char c)
{
    switch (c)
    {
        case 'A':
            return glyph(2, 5, 7, 5, 5);
        case 'B':
            return glyph(6, 5, 6, 5, 6);
        case 'C':
            return glyph(3, 4, 4, 4, 3);
        case 'D':
            return glyph(6, 5, 5, 5, 6);
        case 'E':
            return glyph(7, 4, 6, 4, 7);
        case 'F':
            return glyph(7, 4, 6, 4, 4);
        case 'G':
            return glyph(3, 4, 5, 5, 3);
        case 'H':
            return glyph(5, 5, 7, 5, 5);
        case 'I':
            return glyph(7, 2, 2, 2, 7);
        case 'J':
            return glyph(1, 1, 1, 5, 2);
        case 'K':
            return glyph(5, 5, 6, 5, 5);
        case 'L':
            return glyph(4, 4, 4, 4, 7);
        case 'M':
            return glyph(5, 7, 7, 5, 5);
        case 'N':
            return glyph(6, 5, 5, 5, 5);
        case 'O':
            return glyph(2, 5, 5, 5, 2);
        case 'P':
            return glyph(6, 5, 6, 4, 4);
        case 'Q':
            return glyph(2, 5, 5, 6, 3);
        case 'R':
            return glyph(6, 5, 6, 5, 5);
        case 'S':
            return glyph(3, 4, 2, 1, 6);
        case 'T':
            return glyph(7, 2, 2, 2, 2);
        case 'U':
            return glyph(5, 5, 5, 5, 7);
        case 'V':
            return glyph(5, 5, 5, 5, 2);
        case 'W':
            return glyph(5, 5, 7, 7, 5);
        case 'X':
            return glyph(5, 5, 2, 5, 5);
        case 'Y':
            return glyph(5, 5, 2, 2, 2);
        case 'Z':
            return glyph(7, 1, 2, 4, 7);
        case '0':
            return glyph(7, 5, 5, 5, 7);
        case '1':
            return glyph(2, 6, 2, 2, 7);
        case '2':
            return glyph(6, 1, 2, 4, 7);
        case '3':
            return glyph(6, 1, 2, 1, 6);
        case '4':
            return glyph(5, 5, 7, 1, 1);
        case '5':
            return glyph(7, 4, 6, 1, 6);
        case '6':
            return glyph(3, 4, 6, 5, 2);
        case '7':
            return glyph(7, 1, 2, 2, 2);
        case '8':
            return glyph(2, 5, 2, 5, 2);
        case '9':
            return glyph(2, 5, 3, 1, 6);
        case ':':
            return glyph(0, 2, 0, 2, 0);
        case '!':
            return glyph(2, 2, 2, 0, 2);
        case '+':
            return glyph(0, 2, 7, 2, 0);
        case '-':
            return glyph(0, 0, 7, 0, 0);
        case '*':
            return glyph(0, 5, 2, 5, 0);
        case '.':
            return glyph(0, 0, 0, 0, 2);
        case '/':
            return glyph(1, 1, 2, 4, 4);
        case '?':
            return glyph(6, 1, 2, 0, 2);
        case '\'':
            return glyph(2, 2, 0, 0, 0);
        case '_':
            return glyph(0, 0, 0, 0, 7);
        case '#':
            return glyph(5, 7, 5, 7, 5);
        case '=':
            return glyph(0, 7, 0, 7, 0);
        case '(':
            return glyph(1, 2, 2, 2, 1);
        case ')':
            return glyph(4, 2, 2, 2, 4);
        case '<':
            return glyph(1, 2, 4, 2, 1);
        case '>':
            return glyph(4, 2, 1, 2, 4);
        case '@':
            return glyph(2, 5, 7, 4, 3);
        case '&':
            return glyph(2, 5, 2, 5, 3);
        default:
            return 0;
    }
}

int textWidth(const QString &text, int scale)
{
    return text.isEmpty() ? 0
                          : static_cast<int>(text.size()) * 4 * scale - scale;
}

/// Uppercase letters, digits and a few signs the bitmap font knows.
QString arcadeText(const QString &text, int maxLength)
{
    QString out;
    for (const auto ch : text)
    {
        const auto c = ch.toUpper().toLatin1();
        if (c == ' ' || (c != 0 && glyphBits(c) != 0))
        {
            out += QChar(c);
        }
        if (out.size() >= maxLength)
        {
            break;
        }
    }
    return out.trimmed();
}

// Canvas ----------------------------------------------------------------------

/// Direct pixel access to a premultiplied ARGB image, with an optional mask
/// of rects that must stay untouched and a dirty rect.
class Canvas
{
public:
    void attach(QImage &image)
    {
        this->bits_ = reinterpret_cast<Pix *>(image.bits());
        this->w_ = image.width();
        this->h_ = image.height();
        this->stride_ = static_cast<int>(image.bytesPerLine() / 4);
        this->mask_ = nullptr;
        this->minX_ = this->w_;
        this->minY_ = this->h_;
        this->maxX_ = -1;
        this->maxY_ = -1;
    }

    int width() const
    {
        return this->w_;
    }

    int height() const
    {
        return this->h_;
    }

    void setMask(const std::vector<QRect> *mask)
    {
        this->mask_ = mask;
    }

    void setTrack(bool track)
    {
        this->track_ = track;
    }

    /// Only pixels inside `clip` are drawn (null = everywhere).
    void setClip(const QRect &clip)
    {
        this->clip_ = clip;
    }

    /// Pixels whose ordered-dither threshold is below `amount` are skipped,
    /// for retro dissolves.
    void setDissolve(double amount)
    {
        this->dissolve_ = amount;
    }

    QRect dirty() const
    {
        if (this->maxX_ < this->minX_ || this->maxY_ < this->minY_)
        {
            return {};
        }
        return QRect(QPoint(this->minX_, this->minY_),
                     QPoint(this->maxX_, this->maxY_));
    }

    bool accepts(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= this->w_ || y >= this->h_)
        {
            return false;
        }
        if (!this->clip_.isNull() && !this->clip_.contains(x, y))
        {
            return false;
        }
        if (this->dissolve_ > 0 && bayer(x, y) < this->dissolve_)
        {
            return false;
        }
        if (this->mask_ != nullptr)
        {
            for (const auto &r : *this->mask_)
            {
                if (r.contains(x, y))
                {
                    return false;
                }
            }
        }
        return true;
    }

    Pix get(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= this->w_ || y >= this->h_)
        {
            return 0;
        }
        return this->bits_[y * this->stride_ + x];
    }

    void put(int x, int y, Pix c)
    {
        if (!this->accepts(x, y))
        {
            return;
        }
        this->touch(x, y);
        this->bits_[y * this->stride_ + x] = c;
    }

    /// Source-over with an opaque colour at a / 256.
    void blend(int x, int y, Pix c, int a)
    {
        if (a <= 0 || !this->accepts(x, y))
        {
            return;
        }
        this->touch(x, y);
        Pix &d = this->bits_[y * this->stride_ + x];
        if (a >= 256)
        {
            d = c;
            return;
        }
        const auto k = static_cast<Pix>(a);
        const Pix ik = 256 - k;
        const Pix rb =
            (((c & 0x00ff00ffU) * k + (d & 0x00ff00ffU) * ik) >> 8) &
            0x00ff00ffU;
        const Pix ag =
            ((((c >> 8) & 0x00ff00ffU) * k + ((d >> 8) & 0x00ff00ffU) * ik)) &
            0xff00ff00U;
        d = rb | ag;
    }

    /// Adds light, saturating.
    void add(int x, int y, Pix c, int a)
    {
        if (a <= 0 || !this->accepts(x, y))
        {
            return;
        }
        this->touch(x, y);
        Pix &d = this->bits_[y * this->stride_ + x];
        const int r = std::min(255, static_cast<int>((d >> 16) & 255U) +
                                        ((static_cast<int>((c >> 16) & 255U) *
                                          a) >>
                                         8));
        const int g = std::min(
            255, static_cast<int>((d >> 8) & 255U) +
                     ((static_cast<int>((c >> 8) & 255U) * a) >> 8));
        const int b = std::min(255, static_cast<int>(d & 255U) +
                                        ((static_cast<int>(c & 255U) * a) >> 8));
        const int al =
            std::min(255, static_cast<int>(d >> 24) + ((255 * a) >> 8));
        d = (static_cast<Pix>(al) << 24) | (static_cast<Pix>(r) << 16) |
            (static_cast<Pix>(g) << 8) | static_cast<Pix>(b);
    }

    void fill(int x, int y, int w, int h, Pix c, int a = 256)
    {
        for (int yy = y; yy < y + h; ++yy)
        {
            for (int xx = x; xx < x + w; ++xx)
            {
                this->blend(xx, yy, c, a);
            }
        }
    }

    /// Multiplies a rect by f / 256 (no mask, no dirty tracking).
    void scaleRect(const QRect &rect, int f)
    {
        const QRect r = rect.intersected(QRect(0, 0, this->w_, this->h_));
        if (r.isEmpty())
        {
            return;
        }
        const auto k = static_cast<Pix>(std::clamp(f, 0, 256));
        for (int y = r.top(); y <= r.bottom(); ++y)
        {
            Pix *row = this->bits_ + y * this->stride_;
            for (int x = r.left(); x <= r.right(); ++x)
            {
                const Pix c = row[x];
                row[x] = (c & 0xff000000U) |
                         ((((c & 0x00ff00ffU) * k) >> 8) & 0x00ff00ffU) |
                         ((((c & 0x0000ff00U) * k) >> 8) & 0x0000ff00U);
            }
        }
    }

private:
    void touch(int x, int y)
    {
        if (!this->track_)
        {
            return;
        }
        this->minX_ = std::min(this->minX_, x);
        this->minY_ = std::min(this->minY_, y);
        this->maxX_ = std::max(this->maxX_, x);
        this->maxY_ = std::max(this->maxY_, y);
    }

    Pix *bits_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    int stride_ = 0;
    const std::vector<QRect> *mask_ = nullptr;
    QRect clip_;
    double dissolve_ = 0;
    bool track_ = false;
    int minX_ = 0;
    int minY_ = 0;
    int maxX_ = -1;
    int maxY_ = -1;
};

/// Calls f(x, y, step) for each pixel of a line.
template <typename F>
void walkLine(int x0, int y0, int x1, int y1, F &&f)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int i = 0; i < 4096; ++i)
    {
        f(x0, y0, i);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

/// Calls f(x, y) for each pixel of a circle outline.
template <typename F>
void walkCircle(int cx, int cy, int r, F &&f)
{
    if (r <= 0)
    {
        f(cx, cy);
        return;
    }
    int x = r;
    int y = 0;
    int err = 1 - r;
    while (x >= y)
    {
        f(cx + x, cy + y);
        f(cx - x, cy + y);
        f(cx + x, cy - y);
        f(cx - x, cy - y);
        if (x != y)
        {
            f(cx + y, cy + x);
            f(cx - y, cy + x);
            f(cx + y, cy - x);
            f(cx - y, cy - x);
        }
        ++y;
        if (err < 0)
        {
            err += 2 * y + 1;
        }
        else
        {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

void drawDisc(Canvas &cv, int cx, int cy, int r, Pix c, int a = 256)
{
    for (int dy = -r; dy <= r; ++dy)
    {
        const int half = static_cast<int>(
            std::sqrt(std::max(0.0, double(r * r - dy * dy) + 0.3)));
        for (int dx = -half; dx <= half; ++dx)
        {
            cv.blend(cx + dx, cy + dy, c, a);
        }
    }
}

/// A soft additive light, `strength` at the centre.
void drawLight(Canvas &cv, double cx, double cy, double radius, Pix c,
               int strength)
{
    const int r = static_cast<int>(std::ceil(radius));
    const int x0 = fi(cx);
    const int y0 = fi(cy);
    for (int dy = -r; dy <= r; ++dy)
    {
        for (int dx = -r; dx <= r; ++dx)
        {
            const double d =
                std::hypot(x0 + dx + 0.5 - cx, y0 + dy + 0.5 - cy) / radius;
            if (d >= 1)
            {
                continue;
            }
            const double k = (1 - d) * (1 - d);
            cv.add(x0 + dx, y0 + dy, c, ri(strength * k));
        }
    }
}

int drawText(Canvas &cv, const QString &text, int x, int y, Pix c,
             int scale = 1, int alpha = 256)
{
    int cx = x;
    for (const auto ch : text)
    {
        const auto bits = glyphBits(ch.toUpper().toLatin1());
        if (bits != 0)
        {
            for (int row = 0; row < 5; ++row)
            {
                for (int col = 0; col < 3; ++col)
                {
                    if ((bits >> ((4 - row) * 3 + (2 - col))) & 1)
                    {
                        cv.fill(cx + col * scale, y + row * scale, scale,
                                scale, c, alpha);
                    }
                }
            }
        }
        cx += 4 * scale;
    }
    return cx - x;
}

/// Text with a dark one-pixel outline so it reads over anything.
void drawTextOutlined(Canvas &cv, const QString &text, int x, int y, Pix c,
                      int scale = 1, int alpha = 256)
{
    const Pix dark = pal(PAL_VOID);
    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            if (ox != 0 || oy != 0)
            {
                drawText(cv, text, x + ox, y + oy, dark, scale, alpha);
            }
        }
    }
    drawText(cv, text, x, y, c, scale, alpha);
}

void drawTextCentered(Canvas &cv, const QString &text, int cx, int y, Pix c,
                      int scale = 1, int alpha = 256, bool outline = true)
{
    const int x = cx - textWidth(text, scale) / 2;
    if (outline)
    {
        drawTextOutlined(cv, text, x, y, c, scale, alpha);
    }
    else
    {
        drawText(cv, text, x, y, c, scale, alpha);
    }
}

// Sprites ---------------------------------------------------------------------

struct Sprite {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> px;

    std::uint8_t at(int x, int y) const
    {
        return this->px[static_cast<size_t>(y * this->w + x)];
    }
};

std::uint8_t spriteInk(char ch)
{
    switch (ch)
    {
        case 'k':
            return PAL_VOID;
        case 'n':
            return PAL_NAVY;
        case 'd':
            return PAL_DUSK;
        case 's':
            return PAL_STEEL;
        case 'p':
            return PAL_PALE;
        case 'w':
            return PAL_WHITE;
        case 'm':
            return PAL_MAGENTA;
        case 'u':
            return PAL_PLUM;
        case 'c':
            return PAL_CYAN;
        case 't':
            return PAL_TEAL;
        case 'a':
            return PAL_AMBER;
        case 'e':
            return PAL_EMBER;
        case 'x':
            return PAL_CORE;
        default:
            return PAL_NONE;
    }
}

Sprite makeSprite(std::initializer_list<const char *> rows)
{
    Sprite sprite;
    sprite.h = static_cast<int>(rows.size());
    sprite.w = static_cast<int>(std::strlen(*rows.begin()));
    sprite.px.reserve(static_cast<size_t>(sprite.w * sprite.h));
    for (const char *row : rows)
    {
        const int length = static_cast<int>(std::strlen(row));
        for (int x = 0; x < sprite.w; ++x)
        {
            sprite.px.push_back(x < length ? spriteInk(row[x]) : PAL_NONE);
        }
    }
    return sprite;
}

constexpr int ENEMY_TYPES = 5;
constexpr int PRISM = 4;

/// All the hand-drawn pixel art. Every critter and craft is original.
struct SpriteBook {
    std::array<std::array<Sprite, 2>, ENEMY_TYPES> enemy;
    Sprite ship;
    Sprite pod;
    Sprite escort;
    Sprite p2;
    Sprite satellite;
    Sprite life;
    std::array<Sprite, 2> capsule;
    Sprite capsuleBig;
    std::array<Sprite, 4> coin;
};

const SpriteBook &book()
{
    static const SpriteBook sprites = [] {
        SpriteBook b;
        // Flutter: a moth-like drone that beats its wings
        b.enemy[0][0] = makeSprite({
            "u.......u",
            "mu.....um",
            "mmu.p.umm",
            ".mmpxpmm.",
            "..upwpu..",
            "...p.p...",
            "..p...p..",
        });
        b.enemy[0][1] = makeSprite({
            ".........",
            ".........",
            "....p....",
            "u.mpxpm.u",
            "mmmpwpmmm",
            ".mup.pum.",
            "..p...p..",
        });
        // Lantern: a jelly-like drone with swaying tendrils
        b.enemy[1][0] = makeSprite({
            "..ccccc..",
            ".cwppppc.",
            "ccppxppcc",
            "ctctctctc",
            ".t.t.t.t.",
            "t..t..t..",
            ".t..t..t.",
        });
        b.enemy[1][1] = makeSprite({
            "..ccccc..",
            ".cwppppc.",
            "ccppxppcc",
            "ctctctctc",
            ".t.t.t.t.",
            "..t..t..t",
            ".t..t..t.",
        });
        // Cog: a box drone with turning spikes
        b.enemy[2][0] = makeSprite({
            "....p....",
            "....s....",
            "..sssss..",
            "..sdxds..",
            "pssxwxssp",
            "..sdxds..",
            "..sssss..",
            "....s....",
            "....p....",
        });
        b.enemy[2][1] = makeSprite({
            "p.......p",
            ".s.....s.",
            "..sssss..",
            "..sdxds..",
            "..sxwxs..",
            "..sdxds..",
            "..sssss..",
            ".s.....s.",
            "p.......p",
        });
        // Peeper: a one-eyed hopping critter that blinks
        b.enemy[3][0] = makeSprite({
            "..aaaaa..",
            ".aeeeeea.",
            "aewwwwwea",
            "aewwkwwea",
            ".aewwwea.",
            "..a.x.a..",
            ".a.....a.",
        });
        b.enemy[3][1] = makeSprite({
            "..aaaaa..",
            ".aaaaaaa.",
            "aeeeeeeea",
            "aewwkwwea",
            ".aewwwea.",
            "..a.x.a..",
            "a.......a",
        });
        // Prism: first-time chatters arrive as a shimmering crystal
        b.enemy[PRISM][0] = makeSprite({
            "....w....",
            "...wcw...",
            "..wcmcw..",
            ".wcmxmcw.",
            "wcmxwxmcw",
            ".wcmxmcw.",
            "..wcmcw..",
            "...wcw...",
            "....w....",
        });
        b.enemy[PRISM][1] = makeSprite({
            "....w....",
            "...wmw...",
            "..wmcmw..",
            ".wmcxcmw.",
            "wmcxwxcmw",
            ".wmcxcmw.",
            "..wmcmw..",
            "...wmw...",
            "....w....",
        });
        // Your ship: a split-wing dart
        b.ship = makeSprite({
            ".....w.....",
            "....wcw....",
            "....pcp....",
            "...spwps...",
            "..mspppsm..",
            ".mmspppsmm.",
            "mmsdpppdsmm",
            "ms.dsxsd.sm",
            "m...s.s...m",
        });
        // 1UP wingman pod
        b.pod = makeSprite({
            "...w...",
            "..wxw..",
            ".spxps.",
            "mspppsm",
            "m.s.s.m",
        });
        // Autopilot escort
        b.escort = makeSprite({
            "...c...",
            ".tcwct.",
            "tcpppct",
            "t.s.s.t",
        });
        // Player 2, flying sideways (nose to the left)
        b.p2 = makeSprite({
            "......tc...",
            "....ccpc...",
            "wwcppppppcs",
            "..ccwxpccss",
            "wwcppppppcs",
            "....ccpc...",
            "......tc...",
        });
        // Moderator satellite
        b.satellite = makeSprite({
            "s.....s",
            "sssesss",
            "..sws..",
            "...e...",
        });
        b.life = makeSprite({
            "..w..",
            ".wpw.",
            "mpppm",
            "m...m",
        });
        b.capsule[0] = makeSprite({
            ".mmm.",
            "mwwwm",
            "mwmwm",
            "mwwwm",
            "mwmmm",
            "mwmmm",
            ".mmm.",
        });
        b.capsule[1] = makeSprite({
            ".ccc.",
            "cwwwc",
            "cwcwc",
            "cwwwc",
            "cwccc",
            "cwccc",
            ".ccc.",
        });
        b.capsuleBig = makeSprite({
            "..aaa..",
            ".aeeea.",
            "aewweea",
            "aewewea",
            "aewweea",
            "aeweeea",
            "aeweeea",
            ".aeeea.",
            "..aaa..",
        });
        b.coin[0] = makeSprite({
            ".aaa.",
            "awaaa",
            "awaea",
            "aaaea",
            ".aaa.",
        });
        b.coin[1] = makeSprite({
            "..a..",
            ".awa.",
            ".aea.",
            ".aea.",
            "..a..",
        });
        b.coin[2] = makeSprite({
            "..w..",
            "..a..",
            "..a..",
            "..a..",
            "..e..",
        });
        b.coin[3] = b.coin[1];
        return b;
    }();
    return sprites;
}

struct Look {
    std::uint8_t core = PAL_CYAN;
    int alpha = 256;
    /// 0..256 towards white
    int flash = 0;
    /// Brightness, 0..256
    int fade = 256;
    /// Rows from the top that have been erased
    int erased = 0;
};

void drawSprite(Canvas &cv, const Sprite &s, int cx, int cy,
                const Look &look = {})
{
    const int x0 = cx - s.w / 2;
    const int y0 = cy - s.h / 2;
    for (int y = std::max(0, look.erased); y < s.h; ++y)
    {
        for (int x = 0; x < s.w; ++x)
        {
            const auto ink = s.at(x, y);
            if (ink == PAL_NONE)
            {
                continue;
            }
            Pix c = pal(ink == PAL_CORE ? look.core : ink);
            if (look.flash > 0)
            {
                c = mixPix(c, pal(PAL_WHITE), look.flash);
            }
            if (look.fade < 256)
            {
                c = shade(c, look.fade);
            }
            cv.blend(x0 + x, y0 + y, c, look.alpha);
        }
    }
}

// Simulation records ----------------------------------------------------------

constexpr double SLOT_X = 12.0;
constexpr double SLOT_Y = 10.0;
constexpr int MAX_ENEMIES = 40;
constexpr int MIN_KEEP = 4;
constexpr int MAX_PARTICLES = 900;
constexpr int MAX_PARTICLES_REDUCED = 220;
constexpr int STAR_POOL = 300;
constexpr int DUST_POOL = 420;
constexpr int START_HI_SCORE = 20000;

struct Star {
    double u = 0;
    double v = 0;
    int layer = 0;
    double phase = 0;
    std::uint8_t ink = PAL_STEEL;
    bool cross = false;
};

struct CityWindow {
    int x = 0;
    int y = 0;
    double threshold = 0;
    std::uint8_t ink = PAL_AMBER;
    int bright = 256;
};

struct Beacon {
    int x = 0;
    int y = 0;
    double phase = 0;
};

struct Neon {
    int x = 0;
    int y = 0;
    int length = 4;
    std::uint8_t ink = PAL_MAGENTA;
    int seed = 0;
};

struct Enemy {
    int id = 0;
    int type = 0;
    std::uint8_t core = PAL_CYAN;
    int row = 0;
    int col = 0;
    double x = 0;
    double y = 0;
    double age = 0;
    /// 0..1 progress of the entry flight
    double entry = 0;
    double entryTime = 2.0;
    int path = 0;
    double side = 1;
    QPointF start;
    double newTag = 0;
    double flash = 0;
    QString user;
    bool dead = false;
    /// Lent to a combo formation
    bool hidden = false;
    bool fleeing = false;
    double fleeAge = 0;
    /// Mod laser erase progress, < 0 when untouched
    double erase = -1;
    /// Picked by the mod laser
    bool claimed = false;
    int phase = 0;
};

enum ShotKind : std::uint8_t {
    SHOT_ESCORT,
    SHOT_CLICK,
    SHOT_TRACER,
};

constexpr int TARGET_NONE = -1;
constexpr int TARGET_BOSS = -2;

struct Shot {
    double x = 0;
    double y = 0;
    double vx = 0;
    double vy = 0;
    double speed = 150;
    double age = 0;
    int target = TARGET_NONE;
    QPointF goal;
    ShotKind kind = SHOT_ESCORT;
    double damage = 0;
    std::uint8_t ink = PAL_CYAN;
};

enum ParticleKind : std::uint8_t {
    PK_FIRE,
    PK_SPARK,
    PK_DEBRIS,
    PK_SMOKE,
    PK_GATHER,
    PK_TWINKLE,
    PK_ASH,
};

struct Particle {
    double x = 0;
    double y = 0;
    double vx = 0;
    double vy = 0;
    double age = 0;
    double life = 1;
    double gravity = 0;
    double drag = 0;
    std::uint8_t ink = PAL_WHITE;
    ParticleKind kind = PK_FIRE;
    bool front = false;
    bool big = false;
};

struct Ring {
    double x = 0;
    double y = 0;
    double r0 = 1;
    double r1 = 8;
    double age = 0;
    double life = 0.4;
    bool front = false;
};

struct Popup {
    QString text;
    double x = 0;
    double y = 0;
    double age = 0;
    double life = 1.3;
    std::uint8_t ink = PAL_AMBER;
    int scale = 1;
    bool front = false;
    double rise = 9;
};

enum BannerStyle : std::uint8_t {
    BANNER_PLAIN,
    BANNER_WARNING,
    BANNER_STAGE,
    BANNER_BONUS,
    BANNER_CLEAR,
};

struct Banner {
    QString top;
    QString bottom;
    double age = 0;
    double life = 2.6;
    BannerStyle style = BANNER_PLAIN;
    std::uint8_t ink = PAL_WHITE;
    std::uint8_t ink2 = PAL_PALE;
};

struct Beam {
    QPointF from;
    QPointF to;
    double age = 0;
    double life = 0.55;
    double power = 0.5;
    std::uint8_t ink = PAL_MAGENTA;
};

struct Pellet {
    QPointF from;
    double age = 0;
    double life = 0.3;
};

/// Phosphor burn-in left where something big blew up; fades slowly
struct Afterglow {
    double x = 0;
    double y = 0;
    double radius = 6;
    double age = 0;
    double life = 2.6;
};

/// A follow-up blast after a charged shot
struct Boom {
    double x = 0;
    double y = 0;
    double size = 0.5;
    double at = 0;
};

struct PendingKill {
    int id = 0;
    double at = 0;
    int points = 0;
};

struct Pod {
    bool active = false;
    double age = 0;
    double life = 20;
    std::uint8_t ink = PAL_CYAN;
    QPointF start;
    double x = 0;
    double y = 0;
    bool docked = false;
};

struct Capsule {
    double x = 0;
    double y = 0;
    double vy = 80;
    double age = 0;
    double delay = 0;
    double phase = 0;
    bool big = false;
    bool collected = false;
    bool magnet = false;
};

struct Coin {
    double x = 0;
    double y = 0;
    double vx = 0;
    double vy = 0;
    double age = 0;
    double delay = 0;
    double phase = 0;
    int value = 0;
    int bounces = 0;
    bool collected = false;
};

struct ComboDot {
    QPointF from;
    QPointF to;
    double delay = 0;
    int enemy = -1;
    int glyph = 0;
};

struct Combo {
    bool active = false;
    double age = 0;
    double life = 3.0;
    int amount = 0;
    int dot = 2;
    std::vector<ComboDot> dots;
    QPointF label;
};

struct Stamp {
    double x = 0;
    double y = 0;
    double age = 0;
    double life = 3.0;
};

struct ModLaser {
    bool active = false;
    double age = 0;
    int target = -1;
    double tx = 0;
    double ty = 0;
    double satY = 0;
    bool stamped = false;
};

struct Player2 {
    bool active = false;
    double age = 0;
    QRectF row;
};

struct HoverLock {
    bool active = false;
    bool leaving = false;
    double age = 0;
    double leaveAge = 0;
    QRectF row;
};

enum BossPhase : std::uint8_t {
    BOSS_WARNING,
    BOSS_ENTER,
    BOSS_FIGHT,
    BOSS_DYING,
};

struct Boss {
    bool active = false;
    BossPhase phase = BOSS_WARNING;
    double age = 0;
    double hp = 1;
    double maxHp = 1;
    double lagHp = 1;
    double hitsNeeded = 30;
    double scale = 1;
    double x = 0;
    double y = 0;
    int viewers = 0;
    QString name;
    bool podLeft = true;
    bool podRight = true;
    /// Only the hull around the last hit lights up, never the whole boss
    double hitFlash = 0;
    double hitX = 0;
    double hitY = 0;
    double shield = 0;
    int pendingShots = 0;
    double shotTimer = 0;
    double boomTimer = 0;
    double assistTimer = 0;
    bool finalBlast = false;
};

// The scene -------------------------------------------------------------------

class ArcadeScene final : public ThemeScene
{
public:
    ArcadeScene();

    SceneInfo info() const override
    {
        return {QStringLiteral("arcade"), QStringLiteral("Raid Arcade"),
                QColor(0x07, 0x0a, 0x16), QColor(0xd4, 0xf2, 0xe8),
                QColor(0xff, 0x4f, 0xa3), false};
    }

    SceneGlass glass() const override
    {
        SceneGlass g;
        // The cabinet dims itself under chat, so the glass can stay thin
        // enough for the pixel art behind the Activity pane to read.
        g.text = 0.6;
        g.chrome = 0.3;
        g.blur = 0.3;
        return g;
    }

    void step(const SceneState &state, const SceneLayout &layout) override;
    void event(const SceneEvent &event, const SceneState &state,
               const SceneLayout &layout) override;
    void paintBack(QPainter &painter, const SceneState &state,
                   const SceneLayout &layout) override;

    bool hasFront() const override
    {
        return this->frontActive_;
    }

    void paintFront(QPainter &painter, const SceneState &state,
                    const SceneLayout &layout) override;

private:
    // Size and layout
    void ensureSize(const SceneLayout &layout);
    void updateGeometry(const SceneLayout &layout);
    void buildPostTables();
    void buildSky();
    void buildCity(std::uint32_t seed);
    QPointF toGame(QPointF p) const;
    QRectF toGame(const QRectF &r) const;
    QRect toGameRect(const QRectF &r) const;

    // Simulation
    int particleCap() const;
    void spawnParticle(const Particle &p);
    void spawnExplosion(double x, double y, double size, bool front);
    void spawnDebris(const Sprite &sprite, double x, double y,
                     std::uint8_t core, double force, bool front);
    void addPopup(const QString &text, double x, double y, std::uint8_t ink,
                  int scale, bool front, double life = 1.3);
    void addBanner(Banner banner);
    void addScore(long long points);
    int multiplier() const;

    QPointF shipPos() const;
    QPointF shipNose() const;
    QPointF slotPos(int row, int col) const;
    double rowY(int row) const;
    bool findSlot(int &row, int &col) const;
    QPointF entryPos(const Enemy &e, QPointF slot) const;
    Enemy *findEnemy(int id);
    Enemy *nearestEnemy(QPointF from, int exclude = -1);
    int formationCount() const;
    void spawnEnemy(const SceneEvent &e, double energy);
    void killEnemy(Enemy &e, double size, bool front);

    void onSend(const SceneEvent &e);
    void onMessage(const SceneEvent &e, const SceneState &state);
    void onRaid(const SceneEvent &e);
    void onTimeout(const SceneEvent &e);
    void onCombo(const SceneEvent &e);
    void fireBeam(QPointF from, QPointF to, double power, std::uint8_t ink);

    void stepStars(double dt);
    void stepShip(const SceneState &state, double dt);
    void stepEnemies(double dt);
    void stepEscort(double dt);
    void stepShots(double dt);
    void stepBoss(double dt);
    void stepPickups(double dt);
    void stepEffects(double dt);

    // Painting
    void renderGame(const SceneState &state);
    void postProcess(const SceneState &state);
    void expand(const QImage &src, QImage &dst, const QRect &region,
                bool keepAlpha) const;
    void drawStars(Canvas &cv, double t) const;
    void drawAmbient(Canvas &cv, double t) const;
    void drawCity(Canvas &cv, double t) const;
    void drawEnemies(Canvas &cv, double t) const;
    void drawCombo(Canvas &cv, double t) const;
    void drawEscort(Canvas &cv, double t) const;
    void drawShots(Canvas &cv) const;
    void drawPickups(Canvas &cv, double t) const;
    void drawParticles(Canvas &cv, bool front) const;
    void drawRings(Canvas &cv, bool front) const;
    void drawAfterglows(Canvas &cv) const;
    void drawPopups(Canvas &cv, bool front) const;
    void drawBanners(Canvas &cv, double t) const;
    void drawAttract(Canvas &cv, double t) const;
    void drawBoss(Canvas &cv, double t) const;
    void drawBossBar(Canvas &cv, double t) const;
    void drawModLaser(Canvas &cv, double t) const;
    void drawStamps(Canvas &cv, bool front) const;
    void drawPlayer2(Canvas &cv, double t, bool front) const;
    void drawHover(Canvas &cv, double t, bool front) const;
    void drawCrosshair(Canvas &cv, double t) const;
    void drawBeams(Canvas &cv, double t) const;
    void drawPellets(Canvas &cv) const;
    void drawShip(Canvas &cv, double t) const;
    void drawHud(Canvas &cv, double t) const;
    int hudWidth(int level) const;
    void placeHud(const SceneLayout &layout);
    QPointF gaugeTip() const;

    template <typename F>
    void forEachBossPixel(double t, F &&f) const;

    // Cached size
    qreal dpr_ = 0;
    int devPx_ = 3;
    int gw_ = 0;
    int gh_ = 0;
    double scale_ = 1.0 / 3.0;
    double winW_ = 0;
    double winH_ = 0;
    std::array<int, 8> scanFactor_{};

    // Geometry in game pixels
    QRectF arena_;
    bool hasArena_ = false;
    QRectF top_;
    QRectF tabs_;
    QRectF arenaHeader_;
    /// Tab names (keep-out for bright art), game pixels
    QRectF tabLabels_;
    /// Where the HUD goes and how much of it fits
    QRectF hudArea_;
    int hudLevel_ = 0;
    bool hudCentred_ = false;
    bool hudInHeader_ = false;
    /// The ship sits in the strip above the input box (emote or reply bar)
    bool shipInGap_ = false;
    /// Narrow spots get an upright charge gauge left of the ship
    bool gaugeVertical_ = false;
    QRectF band_;
    QPointF shipHome_;
    double ground_ = 0;
    std::vector<QRect> chats_;
    std::vector<QRect> inputs_;
    std::vector<QRect> acts_;
    int maxCols_ = 5;
    QRectF skyArena_;

    // Layers
    QImage game_;
    QImage screen_;
    QImage front_;
    QImage frontScreen_;
    std::vector<Pix> sky_;
    std::vector<Pix> city_;
    int cityTop_ = 0;
    std::vector<Pix> planet_;
    int planetX_ = 0;
    int planetY_ = 0;
    int planetW_ = 0;
    int planetH_ = 0;
    std::vector<std::uint16_t> vignette_;
    std::vector<std::uint8_t> sheen_;
    std::vector<int> bloom_;
    std::vector<int> bloomTmp_;
    std::vector<int> bloomRow_;
    std::vector<int> colX0_;
    std::vector<int> colF_;
    std::vector<CityWindow> windows_;
    std::vector<Beacon> beacons_;
    std::vector<Neon> neons_;
    std::uint32_t citySeed_ = 0x51ab;
    bool citySeeded_ = false;

    // World state
    SceneRng rng_;
    double t_ = 0;
    bool reduced_ = false;
    bool live_ = true;
    int hypeLevel_ = 0;
    double hypeProgress_ = 0;
    double speed_ = 0;
    double energy_ = 0;
    std::vector<Star> stars_;
    std::vector<Star> dust_;
    double viewerFactor_ = 0.35;
    std::array<double, 3> starScroll_{};
    double starCount_ = 120;
    double litShown_ = 0.4;
    double litTarget_ = 0.4;
    double warp_ = 0;
    double cityRise_ = 0;
    double roll_ = 0;

    // Player
    double charge_ = 0;
    double keyPulse_ = 0;
    double drain_ = 0;
    double recoil_ = 0;
    double power_ = 0;
    double pickupFlash_ = 0;
    double shipAway_ = 0;
    double kickAge_ = 10;
    double kickAmp_ = 0;
    int kickX_ = 0;
    int kickY_ = 0;
    long long score_ = 0;
    double shownScore_ = 0;
    long long hiScore_ = START_HI_SCORE;
    int lives_ = 3;
    int credits_ = 0;
    int stage_ = 1;
    std::array<Pod, 2> pods_{};
    std::vector<Pellet> pellets_;
    std::vector<Beam> beams_;

    // Enemies
    std::vector<Enemy> enemies_;
    int nextId_ = 1;
    double scroll_ = 0;
    int activeCols_ = 4;
    int group_ = 0;
    int groupCount_ = 0;
    double groupLast_ = -10;
    int groupPath_ = 0;
    double groupSide_ = 1;
    double groupStart_ = 0.5;
    double ambientTimer_ = 2;
    double escortX_ = 0;
    double escortCool_ = 1.5;
    double escortBoost_ = 0;
    int escortTarget_ = TARGET_NONE;
    std::vector<Shot> shots_;
    std::vector<PendingKill> pendingKills_;
    std::vector<Boom> booms_;
    std::vector<Afterglow> afterglows_;

    // Effects
    std::vector<Particle> particles_;
    std::vector<Ring> rings_;
    std::vector<Popup> popups_;
    std::vector<Banner> banners_;
    std::vector<Capsule> capsules_;
    std::vector<Coin> coins_;
    std::vector<Stamp> stamps_;
    int capsulesLeft_ = 0;
    bool capsulesBig_ = false;
    Combo combo_;
    ModLaser laser_;
    Player2 p2_;
    HoverLock hover_;
    Boss boss_;
    double cometAge_ = 10;
    QPointF cometFrom_;
    QPointF cometDir_;
    double cometTimer_ = 14;
    std::optional<QPointF> pointer_;

    bool frontActive_ = false;
};

ArcadeScene::ArcadeScene()
    : rng_(0xA2CADEU)
{
    SceneRng r(0x5747A5U);
    this->stars_.reserve(STAR_POOL);
    for (int i = 0; i < STAR_POOL; ++i)
    {
        Star s;
        s.u = r.next();
        s.v = r.next();
        // The first stars are mostly faint; the extra ones that big
        // audiences bring in are brighter and closer
        const double k = r.next();
        if (i < 110)
        {
            s.layer = k < 0.6 ? 0 : (k < 0.9 ? 1 : 2);
        }
        else
        {
            s.layer = k < 0.25 ? 0 : (k < 0.62 ? 1 : 2);
        }
        s.phase = r.next() * TAU;
        const double c = r.next();
        if (s.layer == 0)
        {
            s.ink = c < 0.6 ? PAL_DUSK : PAL_STEEL;
        }
        else if (s.layer == 1)
        {
            s.ink = c < 0.55 ? PAL_STEEL : (c < 0.85 ? PAL_PALE : PAL_TEAL);
        }
        else
        {
            s.ink = c < 0.6 ? PAL_PALE : (c < 0.9 ? PAL_WHITE : PAL_CYAN);
            s.cross = r.next() < 0.18;
        }
        this->stars_.push_back(s);
    }
    // Star dust: a faint band across the sky that only big audiences light
    this->dust_.reserve(DUST_POOL);
    for (int i = 0; i < DUST_POOL; ++i)
    {
        const double u = r.next();
        const double spread = (r.next() + r.next() + r.next() - 1.5) * 0.11;
        Star d;
        d.u = u;
        d.v = 0.08 + u * 0.55 + spread;
        d.layer = 0;
        const double c = r.next();
        d.ink = c < 0.55 ? PAL_DUSK : (c < 0.9 ? PAL_STEEL : PAL_PLUM);
        this->dust_.push_back(d);
    }
    this->particles_.reserve(MAX_PARTICLES);
    this->enemies_.reserve(MAX_ENEMIES + 8);
}

// Size and layout -------------------------------------------------------------

void ArcadeScene::ensureSize(const SceneLayout &layout)
{
    const qreal dpr = std::max<qreal>(layout.dpr, 1.0);
    const double logicalPx = layout.window.width() >= 1700 ? 4.0 : 3.0;
    const int devPx = std::clamp(ri(logicalPx * dpr), 2, 8);
    const int gw = std::max(
        8, static_cast<int>(std::ceil(layout.window.width() * dpr / devPx)) +
               1);
    const int gh = std::max(
        8, static_cast<int>(std::ceil(layout.window.height() * dpr / devPx)) +
               1);
    if (gw == this->gw_ && gh == this->gh_ && devPx == this->devPx_ &&
        dpr == this->dpr_)
    {
        return;
    }

    this->dpr_ = dpr;
    this->devPx_ = devPx;
    this->gw_ = gw;
    this->gh_ = gh;
    this->scale_ = dpr / devPx;
    this->winW_ = layout.window.width() * this->scale_;
    this->winH_ = layout.window.height() * this->scale_;

    this->game_ = QImage(gw, gh, QImage::Format_ARGB32_Premultiplied);
    this->game_.fill(pal(PAL_VOID));
    this->screen_ = QImage(gw * devPx, gh * devPx, QImage::Format_RGB32);
    this->screen_.setDevicePixelRatio(dpr);
    this->front_ = QImage();
    this->frontScreen_ = QImage();

    // Scanlines: the last device row of each game row is darker
    for (int k = 0; k < 8; ++k)
    {
        this->scanFactor_[static_cast<size_t>(k)] = 256;
    }
    this->scanFactor_[static_cast<size_t>(devPx - 1)] = 150;
    this->scanFactor_[static_cast<size_t>(devPx - 2)] = devPx >= 4 ? 226 : 244;

    this->buildPostTables();
    this->skyArena_ = QRectF();
    this->updateGeometry(layout);
    this->buildSky();
    this->buildCity(this->citySeed_);
}

QPointF ArcadeScene::toGame(QPointF p) const
{
    return {p.x() * this->scale_, p.y() * this->scale_};
}

QRectF ArcadeScene::toGame(const QRectF &r) const
{
    return {r.x() * this->scale_, r.y() * this->scale_,
            r.width() * this->scale_, r.height() * this->scale_};
}

QRect ArcadeScene::toGameRect(const QRectF &r) const
{
    const QRectF g = this->toGame(r);
    return QRect(QPoint(ri(g.left()), ri(g.top())),
                 QPoint(ri(g.right()) - 1, ri(g.bottom()) - 1));
}

void ArcadeScene::updateGeometry(const SceneLayout &layout)
{
    this->winW_ = layout.window.width() * this->scale_;
    this->winH_ = layout.window.height() * this->scale_;
    this->ground_ = std::floor(this->winH_) - 3;

    // The battlefield: the largest Activity pane, else the upper right
    QRectF best;
    double bestArea = 0;
    for (const auto &r : layout.activity)
    {
        const double area = r.width() * r.height();
        if (area > bestArea)
        {
            bestArea = area;
            best = r;
        }
    }
    const QRectF bestGame = this->toGame(best);
    this->hasArena_ = bestArea > 0 && bestGame.width() >= 40 &&
                      bestGame.height() >= 50;
    if (this->hasArena_)
    {
        this->arena_ = bestGame.adjusted(2, 1, -2, 0);
    }
    else
    {
        this->arena_ = QRectF(this->winW_ * 0.5, 2, this->winW_ * 0.48,
                              this->winH_ * 0.55);
    }

    // Tab strip and split headers along the top
    double topBottom = 0;
    if (!layout.tabs.isEmpty())
    {
        topBottom = layout.tabs.bottom();
    }
    for (const auto &h : layout.headers)
    {
        topBottom = std::max(topBottom, h.bottom());
    }
    if (topBottom <= 0)
    {
        topBottom = layout.window.height() * 0.06;
    }
    this->top_ = QRectF(0, 0, this->winW_, topBottom * this->scale_);
    this->tabs_ = layout.tabs.isEmpty()
                      ? QRectF(0, 0, this->winW_,
                               std::min(10.0, this->top_.height()))
                      : this->toGame(layout.tabs);

    this->arenaHeader_ = QRectF();
    if (this->hasArena_)
    {
        for (const auto &h : layout.headers)
        {
            const QRectF g = this->toGame(h);
            const double overlap =
                std::min(g.right(), this->arena_.right()) -
                std::max(g.left(), this->arena_.left());
            if (overlap > this->arena_.width() * 0.5 &&
                std::abs(g.bottom() - this->arena_.top()) < 6)
            {
                this->arenaHeader_ = g;
            }
        }
    }

    this->band_ = this->toGame(layout.inputBand());
    if (this->band_.height() < 6 || this->band_.width() < 30)
    {
        const double right =
            this->hasArena_ ? this->arena_.left() - 2 : this->winW_ * 0.66;
        this->band_ = QRectF(0, this->winH_ - 14, right, 14);
    }
    // Your ship: in the strip above the input box (emote or reply bar) at its
    // right end, clear of the emotes on its left. Without such a strip it
    // sits inside the input box, between the typed text and the row of
    // buttons on the box's right (about 140 px).
    const QRectF input = this->toGame(layout.focusInput);
    QPointF home(this->band_.right() - 12, this->band_.center().y());
    this->shipInGap_ = false;
    this->gaugeVertical_ = this->band_.width() < 150;
    if (!input.isEmpty() && !layout.focusChat.isEmpty())
    {
        const double chatBottom = this->toGame(layout.focusChat).bottom();
        const double gap = input.top() - chatBottom;
        if (gap >= 14)
        {
            this->shipInGap_ = true;
            home = QPointF(this->band_.right() - 12,
                           chatBottom + gap / 2 + 1);
        }
        else
        {
            const double buttons = 140 * this->scale_;
            const double x =
                std::max(input.left() + 14, input.right() - buttons - 8);
            home = QPointF(x, input.center().y());
            this->gaugeVertical_ = x - input.left() < 110;
        }
    }
    this->shipHome_ = QPointF(std::round(home.x()),
                              std::round(std::min(home.y(), this->winH_ - 7.0)));

    this->chats_.clear();
    for (const auto &r : layout.chats)
    {
        this->chats_.push_back(this->toGameRect(r));
    }
    this->inputs_.clear();
    for (const auto &r : layout.inputs)
    {
        this->inputs_.push_back(this->toGameRect(r));
    }
    this->acts_.clear();
    for (const auto &r : layout.activity)
    {
        this->acts_.push_back(this->toGameRect(r));
    }

    this->maxCols_ = std::clamp(
        static_cast<int>((this->arena_.width() - 14) / SLOT_X), 2, 12);

    this->tabLabels_ = layout.tabLabels.isEmpty()
                           ? QRectF()
                           : this->toGame(layout.tabLabels);
    this->placeHud(layout);

    if (this->arena_ != this->skyArena_ && !this->sky_.empty())
    {
        this->buildSky();
    }
}

void ArcadeScene::buildPostTables()
{
    const int w = this->gw_;
    const int h = this->gh_;
    this->vignette_.assign(static_cast<size_t>(w * h), 256);
    this->sheen_.assign(static_cast<size_t>(w * h), 0);

    // The glass bulges: corners darken and a faint curved highlight sits
    // in the upper left.
    const double cx = this->gw_ * 0.5;
    const double cy = this->gh_ * 0.5;
    const QPointF sheenCentre(w * 0.78, h * 1.35);
    const double sheenRadius =
        std::hypot(sheenCentre.x() - w * 0.1, sheenCentre.y() - h * 0.12);
    const double sheenWidth = h * 0.09;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const double nx = (x + 0.5 - cx) / cx;
            const double ny = (y + 0.5 - cy) / cy;
            const double d2 = nx * nx * 0.8 + ny * ny;
            const double v = 1.0 - 0.32 * std::pow(std::min(1.6, d2), 1.6);
            const auto i = static_cast<size_t>(y * w + x);
            this->vignette_[i] =
                static_cast<std::uint16_t>(std::clamp(ri(v * 256), 120, 256));

            const double d =
                std::hypot(x - sheenCentre.x(), y - sheenCentre.y()) -
                sheenRadius;
            const double arc = std::exp(-(d * d) / (sheenWidth * sheenWidth));
            // Only the upper left part of the arc catches the light
            const double side = clamp01(1.2 - (x / double(w)) * 1.6) *
                                clamp01(1.3 - (y / double(h)) * 1.4);
            this->sheen_[i] = static_cast<std::uint8_t>(
                std::clamp(ri(arc * side * 11 + (1 - y / double(h)) * 2), 0,
                           40));
        }
    }

    // Bloom grid lookup for bilinear upsampling
    const int bw = (w + 3) / 4;
    this->colX0_.assign(static_cast<size_t>(w), 0);
    this->colF_.assign(static_cast<size_t>(w), 0);
    for (int x = 0; x < w; ++x)
    {
        const double g = (x + 0.5) / 4.0 - 0.5;
        const int x0 = std::clamp(fi(g), 0, bw - 1);
        this->colX0_[static_cast<size_t>(x)] = x0;
        this->colF_[static_cast<size_t>(x)] =
            std::clamp(ri((g - x0) * 256), 0, 256);
    }
}

void ArcadeScene::buildSky()
{
    const int w = this->gw_;
    const int h = this->gh_;
    this->skyArena_ = this->arena_;
    this->sky_.assign(static_cast<size_t>(w * h), pal(PAL_VOID));
    const double horizon = std::max(1.0, this->winH_);
    for (int y = 0; y < h; ++y)
    {
        const double v = y / horizon;
        for (int x = 0; x < w; ++x)
        {
            const double b = bayer(x, y);
            std::uint8_t ink = PAL_VOID;
            if (v > 0.45 && v <= 0.76)
            {
                ink = (v - 0.45) / 0.31 * 0.8 > b ? PAL_NAVY : PAL_VOID;
            }
            else if (v > 0.76)
            {
                // City glow: dusk blue with a plum haze over the rooftops
                const double k = (v - 0.76) / 0.24;
                ink = k * 0.85 > b ? PAL_DUSK : PAL_NAVY;
                if (b < k * k * 0.4)
                {
                    ink = PAL_PLUM;
                }
            }
            this->sky_[static_cast<size_t>(y * w + x)] = pal(ink);
        }
    }

    // A ringed planet hangs far away over the battlefield
    const double radius =
        std::clamp(this->arena_.width() * 0.11, 7.0, 20.0);
    const double pcx = this->arena_.left() + this->arena_.width() * 0.32;
    const double pcy = this->arena_.top() + this->arena_.height() * 0.6;
    const int halfW = static_cast<int>(radius * 2.1) + 2;
    const int halfH = static_cast<int>(radius) + 3;
    this->planetW_ = halfW * 2 + 1;
    this->planetH_ = halfH * 2 + 1;
    this->planetX_ = ri(pcx) - halfW;
    this->planetY_ = ri(pcy) - halfH;
    this->planet_.assign(
        static_cast<size_t>(this->planetW_ * this->planetH_), 0);
    for (int py = 0; py < this->planetH_; ++py)
    {
        for (int px = 0; px < this->planetW_; ++px)
        {
            const double dx = px - halfW;
            const double dy = py - halfH;
            const int gx = this->planetX_ + px;
            const int gy = this->planetY_ + py;
            const double b = bayer(gx, gy);
            Pix c = 0;

            // Ring (tilted ellipse)
            const double ry = dy - dx * 0.16;
            const double e = std::sqrt((dx / (radius * 2.0)) *
                                           (dx / (radius * 2.0)) +
                                       (ry / (radius * 0.42)) *
                                           (ry / (radius * 0.42)));
            const bool onRing = (e > 0.78 && e < 0.98) && !(e > 0.86 && e < 0.9);
            const double d = std::hypot(dx, dy);
            const bool onDisc = d <= radius;
            if (onRing && (!onDisc || ry > 0))
            {
                c = pal(b < 0.55 ? PAL_STEEL : PAL_DUSK);
            }
            else if (onDisc)
            {
                const double nz =
                    std::sqrt(std::max(0.0, 1 - (d * d) / (radius * radius)));
                const double l = clamp01((-0.62 * dx / radius) +
                                         (-0.5 * dy / radius) + 0.6 * nz);
                const double band =
                    0.1 * std::sin(dy / radius * 7.0 + dx / radius * 0.8);
                const double k = clamp01(l + band);
                // Far away, so mostly dusk and plum with a thin lit rim
                std::uint8_t ink = PAL_NAVY;
                if (k > 0.86)
                {
                    ink = b < (k - 0.86) * 3 ? PAL_MAGENTA : PAL_PLUM;
                }
                else if (k > 0.58)
                {
                    ink = b < 0.7 ? PAL_PLUM : PAL_DUSK;
                }
                else if (k > 0.3)
                {
                    ink = b < (k - 0.3) * 3.2 ? PAL_DUSK : PAL_NAVY;
                }
                c = pal(ink);
            }
            this->planet_[static_cast<size_t>(py * this->planetW_ + px)] = c;
        }
    }
}

void ArcadeScene::buildCity(std::uint32_t seed)
{
    const int w = this->gw_;
    const int h = this->gh_;
    this->city_.assign(static_cast<size_t>(w * h), 0);
    this->windows_.clear();
    this->beacons_.clear();
    this->neons_.clear();
    const int ground = static_cast<int>(this->ground_);
    const double hs = std::clamp(this->winH_ / 250.0, 0.75, 1.6);
    SceneRng r(seed);

    const auto put = [&](int x, int y, std::uint8_t ink) {
        if (x >= 0 && y >= 0 && x < w && y < h)
        {
            this->city_[static_cast<size_t>(y * w + x)] = pal(ink);
        }
    };
    const auto fillRect = [&](int x0, int y0, int x1, int y1,
                              std::uint8_t ink) {
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                put(x, y, ink);
            }
        }
    };

    std::vector<Beacon> candidates;

    // Back row: tall slim towers, far away
    int x = -static_cast<int>(r.range(0, 6));
    while (x < w)
    {
        const int bw = 4 + static_cast<int>(r.next() * 10);
        const int bh =
            static_cast<int>((10 + r.next() * r.next() * 42) * hs);
        const int top = ground - bh;
        fillRect(x, top, x + bw - 1, ground, PAL_NAVY);
        if (r.next() < 0.5)
        {
            for (int wy = top + 2; wy < ground - 2; wy += 3)
            {
                for (int wx = x + 1; wx < x + bw - 1; wx += 2)
                {
                    if (hash01(wx, wy, 11) < 0.12)
                    {
                        put(wx, wy, PAL_DUSK);
                    }
                }
            }
        }
        if (r.next() < 0.25)
        {
            const int ah = 3 + static_cast<int>(r.next() * 6);
            for (int i = 1; i <= ah; ++i)
            {
                put(x + bw / 2, top - i, PAL_NAVY);
            }
            candidates.push_back({x + bw / 2, top - ah - 1, r.next()});
        }
        x += bw + (r.next() < 0.35 ? 1 + static_cast<int>(r.next() * 3) : 0);
    }

    // Front row: blocks with windows, water tanks and the odd neon sign
    x = -static_cast<int>(r.range(0, 8));
    while (x < w)
    {
        const int bw = 6 + static_cast<int>(r.next() * 15);
        const int bh = static_cast<int>((6 + r.next() * r.next() * 28) * hs);
        const int top = ground - bh;
        const int style = static_cast<int>(r.next() * 6);
        fillRect(x, top, x + bw - 1, ground, PAL_VOID);
        for (int i = x; i < x + bw; ++i)
        {
            put(i, top, PAL_DUSK);
        }
        put(x, top + 1, PAL_NAVY);
        if (style == 0 && bw >= 8)
        {
            // Water tank on legs
            const int tx = x + 2 + static_cast<int>(r.next() * (bw - 7));
            fillRect(tx, top - 5, tx + 3, top - 3, PAL_NAVY);
            put(tx, top - 2, PAL_NAVY);
            put(tx + 3, top - 2, PAL_NAVY);
            put(tx + 1, top - 6, PAL_NAVY);
            put(tx + 2, top - 6, PAL_NAVY);
        }
        else if (style == 1 && bw >= 8)
        {
            // Stepped crown
            fillRect(x + 2, top - 3, x + bw - 3, top - 1, PAL_VOID);
            for (int i = x + 2; i <= x + bw - 3; ++i)
            {
                put(i, top - 3, PAL_NAVY);
            }
        }
        else if (style == 2)
        {
            const int ah = 2 + static_cast<int>(r.next() * 5);
            for (int i = 1; i <= ah; ++i)
            {
                put(x + bw / 2, top - i, PAL_NAVY);
            }
            candidates.push_back({x + bw / 2, top - ah - 1, r.next()});
        }

        const int colStep = r.next() < 0.5 ? 2 : 3;
        const int rowStep = r.next() < 0.6 ? 2 : 3;
        const double owl = r.next();
        for (int wy = top + 2; wy <= ground - 3; wy += rowStep)
        {
            for (int wx = x + 1; wx < x + bw - 1; wx += colStep)
            {
                CityWindow cw;
                cw.x = wx;
                cw.y = wy;
                cw.threshold = clamp01(
                    hash01(wx, wy, static_cast<int>(seed & 0xffff)) * 0.85 +
                    owl * 0.15);
                const double k = hash01(wx, wy, 99);
                cw.ink = k < 0.72 ? PAL_AMBER
                                  : (k < 0.9 ? PAL_PALE
                                             : (k < 0.97 ? PAL_CYAN
                                                         : PAL_MAGENTA));
                cw.bright = cw.ink == PAL_AMBER
                                ? 150 + static_cast<int>(hash01(wx, wy, 5) * 106)
                                : 150;
                this->windows_.push_back(cw);
                put(wx, wy, PAL_NAVY);
            }
        }
        if (style >= 4 && bw >= 9 && bh >= 12)
        {
            Neon n;
            n.x = r.next() < 0.5 ? x : x + bw - 1;
            n.y = top + 3;
            n.length = std::min(bh - 6, 4 + static_cast<int>(r.next() * 4));
            n.ink = r.next() < 0.6 ? PAL_MAGENTA : PAL_CYAN;
            n.seed = static_cast<int>(r.next() * 1000);
            this->neons_.push_back(n);
        }
        x += bw + (r.next() < 0.3 ? 1 : 0);
    }

    // Ground line
    for (int gx = 0; gx < w; ++gx)
    {
        put(gx, ground, PAL_TEAL);
        for (int gy = ground + 1; gy < h; ++gy)
        {
            put(gx, gy, gy == ground + 1 ? PAL_NAVY : PAL_VOID);
        }
    }

    // Aircraft warning lights on the four highest masts
    std::sort(candidates.begin(), candidates.end(),
              [](const Beacon &a, const Beacon &b) {
                  return a.y < b.y;
              });
    for (size_t i = 0; i < candidates.size() && i < 4; ++i)
    {
        this->beacons_.push_back(candidates[i]);
    }

    this->cityTop_ = h;
    for (int y = 0; y < h && this->cityTop_ == h; ++y)
    {
        for (int cx = 0; cx < w; ++cx)
        {
            if (this->city_[static_cast<size_t>(y * w + cx)] != 0)
            {
                this->cityTop_ = y;
                break;
            }
        }
    }
}

// Helpers ---------------------------------------------------------------------

int ArcadeScene::particleCap() const
{
    return this->reduced_ ? MAX_PARTICLES_REDUCED : MAX_PARTICLES;
}

void ArcadeScene::spawnParticle(const Particle &p)
{
    if (static_cast<int>(this->particles_.size()) < this->particleCap())
    {
        this->particles_.push_back(p);
    }
}

void ArcadeScene::spawnExplosion(double x, double y, double size, bool front)
{
    const double amount = this->reduced_ ? 0.35 : 1.0;
    // Flash core and expanding fire
    const int fire = ri((10 + size * 22) * amount);
    for (int i = 0; i < fire; ++i)
    {
        const double a = this->rng_.next() * TAU;
        const double v = (18 + this->rng_.next() * 45) * (0.6 + size * 0.6);
        Particle p;
        p.x = x;
        p.y = y;
        p.vx = std::cos(a) * v;
        p.vy = std::sin(a) * v;
        p.life = 0.3 + this->rng_.next() * 0.35 + size * 0.15;
        p.drag = 3.0;
        p.kind = PK_FIRE;
        p.front = front;
        p.big = size > 0.7 && i % 3 == 0;
        this->spawnParticle(p);
    }
    const int sparks = ri((4 + size * 10) * amount);
    for (int i = 0; i < sparks; ++i)
    {
        const double a = this->rng_.next() * TAU;
        const double v = (60 + this->rng_.next() * 70) * (0.7 + size * 0.4);
        Particle p;
        p.x = x;
        p.y = y;
        p.vx = std::cos(a) * v;
        p.vy = std::sin(a) * v - 10;
        p.life = 0.25 + this->rng_.next() * 0.3;
        p.gravity = 70;
        p.drag = 1.2;
        p.kind = PK_SPARK;
        p.front = front;
        this->spawnParticle(p);
    }
    const int smoke = ri((2 + size * 4) * amount);
    for (int i = 0; i < smoke; ++i)
    {
        Particle p;
        p.x = x + this->rng_.range(-2, 2);
        p.y = y + this->rng_.range(-2, 2);
        p.vx = this->rng_.range(-4, 4);
        p.vy = -4 - this->rng_.next() * 6;
        p.life = 1.2 + this->rng_.next() * 1.0;
        p.kind = PK_SMOKE;
        p.ink = this->rng_.next() < 0.5 ? PAL_DUSK : PAL_STEEL;
        this->spawnParticle(p);
    }
    Ring ring;
    ring.x = x;
    ring.y = y;
    ring.r0 = 1;
    ring.r1 = 5 + size * 9;
    ring.life = 0.3 + size * 0.12;
    ring.front = front;
    this->rings_.push_back(ring);
}

void ArcadeScene::spawnDebris(const Sprite &sprite, double x, double y,
                              std::uint8_t core, double force, bool front)
{
    const int step = this->reduced_ ? 3 : 1;
    int n = 0;
    for (int sy = 0; sy < sprite.h; ++sy)
    {
        for (int sx = 0; sx < sprite.w; ++sx)
        {
            const auto ink = sprite.at(sx, sy);
            if (ink == PAL_NONE || (n++ % step) != 0)
            {
                continue;
            }
            const double dx = sx - sprite.w / 2.0 + 0.5;
            const double dy = sy - sprite.h / 2.0 + 0.5;
            Particle p;
            p.x = x + dx;
            p.y = y + dy;
            p.vx = dx * (6 + this->rng_.next() * 9) * force +
                   this->rng_.range(-10, 10);
            p.vy = dy * (6 + this->rng_.next() * 9) * force +
                   this->rng_.range(-22, 4);
            p.gravity = 45;
            p.drag = 0.8;
            p.life = 0.8 + this->rng_.next() * 0.7;
            p.ink = ink == PAL_CORE ? core : ink;
            p.kind = PK_DEBRIS;
            p.front = front;
            this->spawnParticle(p);
        }
    }
}

void ArcadeScene::addPopup(const QString &text, double x, double y,
                           std::uint8_t ink, int scale, bool front,
                           double life)
{
    if (this->popups_.size() > 12)
    {
        this->popups_.erase(this->popups_.begin());
    }
    Popup p;
    p.text = text;
    p.x = x;
    p.y = y;
    p.ink = ink;
    p.scale = scale;
    p.front = front;
    p.life = life;
    this->popups_.push_back(p);
}

void ArcadeScene::addBanner(Banner banner)
{
    // A new banner cuts the current one short instead of waiting
    for (auto &b : this->banners_)
    {
        b.life = std::min(b.life, b.age + 0.25);
    }
    if (this->banners_.size() > 2)
    {
        this->banners_.erase(this->banners_.begin());
    }
    this->banners_.push_back(std::move(banner));
}

void ArcadeScene::addScore(long long points)
{
    this->score_ += std::max(0LL, points);
}

int ArcadeScene::multiplier() const
{
    return std::clamp(this->hypeLevel_ + 1, 1, 5);
}

QPointF ArcadeScene::shipPos() const
{
    double y = this->shipHome_.y();
    if (!this->reduced_)
    {
        y += std::round(std::sin(this->t_ * 1.7) * 0.6);
    }
    y += std::round(this->recoil_ * 2.5);
    y += this->shipAway_ * 30;
    return {this->shipHome_.x(), std::round(y)};
}

QPointF ArcadeScene::shipNose() const
{
    const QPointF s = this->shipPos();
    return {s.x(), s.y() - 5};
}

double ArcadeScene::rowY(int row) const
{
    return this->arena_.top() + 7 + (this->scroll_ - row) * SLOT_Y;
}

QPointF ArcadeScene::slotPos(int row, int col) const
{
    const double breathe =
        this->reduced_ ? 1.0 : 1.0 + 0.05 * std::sin(this->t_ * 0.6);
    const double room =
        std::max(0.0, (this->arena_.width() - this->maxCols_ * SLOT_X) / 2 -
                          2);
    const double sway =
        this->reduced_ ? 0 : std::min(6.0, room) * std::sin(this->t_ * 0.37);
    const double x = this->arena_.center().x() +
                     (col - (this->maxCols_ - 1) / 2.0) * SLOT_X * breathe +
                     sway;
    return {x, this->rowY(row)};
}

bool ArcadeScene::findSlot(int &rowOut, int &colOut) const
{
    const int kTop = fi(this->scroll_);
    const int cols = std::clamp(this->activeCols_, 1, this->maxCols_);
    const int first = (this->maxCols_ - cols) / 2;
    const int mid = first + (cols - 1) / 2;
    for (int k = kTop; k > kTop - 60; --k)
    {
        if (this->rowY(k) > this->arena_.bottom() - 52)
        {
            break;
        }
        for (int i = 0; i < cols * 2 + 2; ++i)
        {
            const int c = mid + ((i % 2 == 0) ? i / 2 : -(i + 1) / 2);
            if (c < first || c >= first + cols)
            {
                continue;
            }
            const bool taken = std::any_of(
                this->enemies_.begin(), this->enemies_.end(),
                [&](const Enemy &e) {
                    return !e.dead && !e.fleeing && e.row == k && e.col == c;
                });
            if (!taken)
            {
                rowOut = k;
                colOut = c;
                return true;
            }
        }
    }
    return false;
}

QPointF ArcadeScene::entryPos(const Enemy &e, QPointF slot) const
{
    const double u = easeInOutSine(e.entry);
    const QPointF p0 = e.start;
    QPointF p1;
    QPointF p2;
    const QRectF &a = this->arena_;
    switch (e.path)
    {
        case 1:
            // Along the tab strip, then down into the field
            p1 = QPointF(a.left() + 6, p0.y());
            p2 = QPointF(a.center().x() + e.side * a.width() * 0.2,
                         a.top() + a.height() * 0.45);
            break;
        case 2:
            // In from the right edge with a loop below the slot
            p1 = QPointF(a.left() + a.width() * 0.2,
                         a.top() + a.height() * 0.55);
            p2 = QPointF(slot.x() + 30, slot.y() + 30);
            break;
        default:
            // Down from the top with a swing
            p1 = QPointF(std::clamp(p0.x() + e.side * a.width() * 0.35,
                                    a.left(), a.right()),
                         a.top() + a.height() * 0.5);
            p2 = QPointF(slot.x() - e.side * 28, slot.y() + 34);
            break;
    }
    const double v = 1 - u;
    return p0 * (v * v * v) + p1 * (3 * v * v * u) + p2 * (3 * v * u * u) +
           slot * (u * u * u);
}

Enemy *ArcadeScene::findEnemy(int id)
{
    for (auto &e : this->enemies_)
    {
        if (e.id == id && !e.dead)
        {
            return &e;
        }
    }
    return nullptr;
}

Enemy *ArcadeScene::nearestEnemy(QPointF from, int exclude)
{
    Enemy *best = nullptr;
    double bestD = 1e18;
    for (auto &e : this->enemies_)
    {
        if (e.dead || e.hidden || e.fleeing || e.erase >= 0 || e.claimed ||
            e.id == exclude || e.y < -4)
        {
            continue;
        }
        const double d = std::hypot(e.x - from.x(), e.y - from.y());
        if (d < bestD)
        {
            bestD = d;
            best = &e;
        }
    }
    return best;
}

int ArcadeScene::formationCount() const
{
    return static_cast<int>(std::count_if(
        this->enemies_.begin(), this->enemies_.end(), [](const Enemy &e) {
            return !e.dead && !e.fleeing;
        }));
}

void ArcadeScene::spawnEnemy(const SceneEvent &e, double energy)
{
    if (this->boss_.active ||
        this->formationCount() >= (this->reduced_ ? 24 : MAX_ENEMIES))
    {
        this->escortBoost_ = 3;
        return;
    }
    int row = 0;
    int col = 0;
    if (!this->findSlot(row, col))
    {
        this->escortBoost_ = 3;
        return;
    }

    // Messages that arrive close together fly in as one conga line
    if (this->t_ - this->groupLast_ > 2.2 ||
        this->groupCount_ >= std::max(3, this->activeCols_))
    {
        this->group_++;
        this->groupCount_ = 0;
        const double h = hash01(this->group_, 3);
        this->groupPath_ = h < 0.45 ? 0 : (h < 0.75 ? 1 : 2);
        this->groupSide_ = hash01(this->group_, 5) < 0.5 ? -1.0 : 1.0;
        this->groupStart_ = 0.25 + hash01(this->group_, 7) * 0.5;
    }
    this->groupCount_++;
    this->groupLast_ = this->t_;

    Enemy en;
    en.id = this->nextId_++;
    en.row = row;
    en.col = col;
    en.path = this->groupPath_;
    en.side = this->groupSide_;
    en.entryTime = this->reduced_ ? 2.2 : lerp(2.6, 1.4, clamp01(energy));
    // Keep entries off the tab names: the sweep along the tab strip only
    // runs where the strip is free, and drops from the top start right of
    // the names
    double dropX =
        this->arena_.left() + this->arena_.width() * this->groupStart_;
    if (!this->tabLabels_.isEmpty() &&
        this->tabLabels_.right() > this->arena_.left())
    {
        dropX = std::max(dropX, this->tabLabels_.right() + 6);
    }
    const QRectF sweep(this->arena_.left() - 150, this->tabs_.center().y() - 5,
                       156, 10);
    if (en.path == 1 && (sweep.intersects(this->tabLabels_) ||
                         sweep.intersects(this->hudArea_)))
    {
        en.path = 0;
    }
    if (en.path == 0 && dropX > this->arena_.right() - 6)
    {
        en.path = 2;
    }
    switch (en.path)
    {
        case 1:
            en.start = QPointF(std::max(-8.0, this->arena_.left() - 150),
                               this->tabs_.center().y() + 1);
            break;
        case 2:
            en.start = QPointF(this->winW_ + 8, this->arena_.top() +
                                                    this->arena_.height() *
                                                        0.3);
            break;
        default:
            en.start = QPointF(dropX, -8);
            break;
    }
    en.x = en.start.x();
    en.y = en.start.y();
    const auto userHash = static_cast<std::uint32_t>(qHash(e.user));
    en.type = e.firstTime ? PRISM : static_cast<int>(userHash % 4U);
    en.core = nearestCore(e.color);
    en.newTag = e.firstTime ? 4.5 : 0;
    en.user = e.user;
    en.phase = static_cast<int>(userHash % 7U);
    this->enemies_.push_back(en);
}

void ArcadeScene::killEnemy(Enemy &e, double size, bool front)
{
    if (e.dead)
    {
        return;
    }
    e.dead = true;
    this->spawnExplosion(e.x, e.y, size, front);
    const auto &frames = book().enemy[static_cast<size_t>(e.type)];
    this->spawnDebris(frames[0], e.x, e.y, e.core, 0.6 + size * 0.6, front);
    if (this->escortTarget_ == e.id)
    {
        this->escortTarget_ = TARGET_NONE;
    }
}

// Events ----------------------------------------------------------------------

void ArcadeScene::event(const SceneEvent &e, const SceneState &state,
                        const SceneLayout &layout)
{
    this->ensureSize(layout);
    this->updateGeometry(layout);
    this->t_ = state.t;
    this->reduced_ = state.reduced;

    // A new chat line pushes older rows up; Player 2 keeps to its row
    const bool newLine = e.type == SceneEventType::Message ||
                         e.type == SceneEventType::Send ||
                         e.type == SceneEventType::Mention ||
                         e.type == SceneEventType::Timeout;
    if (newLine && this->p2_.active && !e.row.isEmpty())
    {
        this->p2_.row.translate(0, -this->toGame(e.row).height());
        if (this->p2_.row.bottom() < this->top_.bottom())
        {
            this->p2_.active = false;
        }
    }

    switch (e.type)
    {
        case SceneEventType::Key: {
            if (e.deleted)
            {
                // Backspace bleeds a little charge out of the meter
                this->drain_ = 1;
                const QPointF tip = this->gaugeTip();
                for (int i = 0; i < (this->reduced_ ? 1 : 3); ++i)
                {
                    Particle p;
                    p.x = tip.x();
                    p.y = tip.y();
                    p.vx = this->rng_.range(-8, 8);
                    p.vy = this->rng_.range(-14, -4);
                    p.gravity = 30;
                    p.life = 0.5;
                    p.kind = PK_ASH;
                    p.ink = PAL_STEEL;
                    this->spawnParticle(p);
                }
            }
            else
            {
                this->keyPulse_ = 1;
                const QPointF nose = this->shipNose();
                if (!this->reduced_)
                {
                    Pellet pellet;
                    pellet.from = this->toGame(e.point);
                    if (pellet.from.x() < nose.x() - 6)
                    {
                        this->pellets_.push_back(pellet);
                    }
                    for (int i = 0; i < 3; ++i)
                    {
                        const double a = this->rng_.next() * TAU;
                        const double r = 5 + this->rng_.next() * 4;
                        Particle p;
                        p.x = nose.x() + std::cos(a) * r;
                        p.y = nose.y() + std::sin(a) * r;
                        p.life = 0.22 + this->rng_.next() * 0.1;
                        p.vx = (nose.x() - p.x) / p.life;
                        p.vy = (nose.y() - p.y) / p.life;
                        p.kind = PK_GATHER;
                        p.ink = i == 0 ? PAL_WHITE
                                       : (this->charge_ > 0.7 ? PAL_MAGENTA
                                                              : PAL_CYAN);
                        this->spawnParticle(p);
                    }
                }
            }
        }
        break;

        case SceneEventType::Send:
            this->onSend(e);
            break;

        case SceneEventType::Message:
            this->onMessage(e, state);
            break;

        case SceneEventType::Sub: {
            const int months = std::max(1, e.amount);
            Pod *slot = nullptr;
            for (auto &pod : this->pods_)
            {
                if (!pod.active)
                {
                    slot = &pod;
                    break;
                }
            }
            if (slot == nullptr)
            {
                slot = this->pods_[0].age > this->pods_[1].age
                           ? &this->pods_[0]
                           : &this->pods_[1];
            }
            slot->active = true;
            slot->docked = false;
            slot->age = 0;
            slot->life = std::min(30.0, 16 + std::min(months, 36) * 0.4);
            slot->ink = months >= 24 ? PAL_AMBER
                                     : (months >= 6 ? PAL_MAGENTA : PAL_CYAN);
            slot->start = QPointF(this->arena_.left() + 20,
                                  this->band_.top() - 30);
            if (!this->hasArena_)
            {
                slot->start = QPointF(this->shipHome_.x() + 30, -8);
            }
            slot->x = slot->start.x();
            slot->y = slot->start.y();
            this->lives_ = std::min(6, this->lives_ + 1);
            const QPointF s = this->shipPos();
            const QString label =
                months > 1 ? QStringLiteral("1UP ") + QString::number(months) +
                                 QStringLiteral("MO")
                           : QStringLiteral("1UP");
            const QPointF at =
                this->shipInGap_
                    ? QPointF(s.x() - 12 - textWidth(label, 1) / 2.0, s.y() - 7)
                    : QPointF(s.x() - 50 - textWidth(label, 1) / 2.0, s.y() - 1);
            this->addPopup(label, at.x(), at.y(), PAL_AMBER, 1, false, 2.2);
            this->popups_.back().rise = 3;
        }
        break;

        case SceneEventType::Gift: {
            const int n = std::max(1, e.amount);
            const int count = std::min(n, 30);
            const bool big = n >= 20;
            this->capsulesLeft_ += count;
            this->capsulesBig_ = big;
            const double spread = std::min(1.4, count * 0.07);
            for (int i = 0; i < count; ++i)
            {
                Capsule c;
                c.x = this->arena_.left() + 6 +
                      this->rng_.next() * std::max(1.0, this->arena_.width() - 12);
                c.y = this->arena_.top() - 4 - this->rng_.next() * 12;
                c.vy = 80 + this->rng_.next() * 30;
                c.delay = count > 1 ? spread * i / (count - 1) : 0;
                c.phase = this->rng_.next() * TAU;
                c.big = big;
                this->capsules_.push_back(c);
            }
            this->addPopup(QStringLiteral("GIFT *") + QString::number(n),
                           this->arena_.center().x(),
                           this->arena_.top() + this->arena_.height() * 0.42,
                           PAL_MAGENTA, n >= 20 ? 2 : 1, false, 2.0);
        }
        break;

        case SceneEventType::Bits: {
            const int bits = std::max(1, e.amount);
            const int count = std::clamp(bits / 20, 6, 48);
            const QPointF origin(this->arena_.center().x(),
                                 this->arena_.top() +
                                     this->arena_.height() * 0.35);
            int remaining = bits * 10;
            for (int i = 0; i < count; ++i)
            {
                Coin c;
                c.x = origin.x() + this->rng_.range(-4, 4);
                c.y = origin.y() + this->rng_.range(-3, 3);
                c.vx = this->rng_.range(-55, 55);
                c.vy = this->rng_.range(-75, -20);
                c.delay = i * std::min(0.02, 0.8 / count);
                c.phase = this->rng_.next() * 4;
                c.value = i == count - 1 ? remaining : (bits * 10) / count;
                remaining -= c.value;
                this->coins_.push_back(c);
            }
            this->addPopup(QString::number(bits) + QStringLiteral(" BITS"),
                           origin.x(), origin.y() - 10, PAL_AMBER,
                           bits >= 1000 ? 2 : 1, false, 2.0);
        }
        break;

        case SceneEventType::Raid:
            this->onRaid(e);
            break;

        case SceneEventType::Follow:
            // A shooting star for every new follower
            this->cometAge_ = 0;
            this->cometFrom_ =
                QPointF(this->arena_.left() +
                            this->arena_.width() * (0.4 + 0.5 * this->rng_.next()),
                        this->arena_.top() + 4);
            this->cometDir_ = QPointF(-1.0, 0.55);
            break;

        case SceneEventType::Mention:
            this->p2_.active = true;
            this->p2_.age = 0;
            this->p2_.row = this->toGame(e.row);
            break;

        case SceneEventType::Combo:
            this->onCombo(e);
            break;

        case SceneEventType::Hype:
            if (e.levelUp)
            {
                Banner b;
                b.top = QStringLiteral("LEVEL UP");
                b.bottom = QStringLiteral("SCORE *") +
                           QString::number(std::clamp(e.amount + 1, 1, 5));
                b.ink = PAL_AMBER;
                b.ink2 = PAL_MAGENTA;
                b.life = 2.4;
                this->addBanner(b);
                this->warp_ = std::max(this->warp_, 0.6);
            }
            break;

        case SceneEventType::Shoutout: {
            Banner b;
            b.top = QStringLiteral("BONUS STAGE");
            b.bottom = arcadeText(e.user, 14);
            if (b.bottom.isEmpty())
            {
                b.bottom = QStringLiteral("???");
            }
            b.style = BANNER_BONUS;
            b.ink = PAL_AMBER;
            b.ink2 = PAL_WHITE;
            b.life = 3.0;
            this->addBanner(b);
            for (int i = 0; i < (this->reduced_ ? 4 : 14); ++i)
            {
                Particle p;
                p.x = this->arena_.left() +
                      this->rng_.next() * this->arena_.width();
                p.y = this->arena_.top() +
                      this->rng_.next() * this->arena_.height() * 0.6;
                p.life = 0.8 + this->rng_.next() * 1.2;
                p.kind = PK_TWINKLE;
                p.ink = this->rng_.next() < 0.5 ? PAL_AMBER : PAL_WHITE;
                this->spawnParticle(p);
            }
        }
        break;

        case SceneEventType::Timeout:
            this->onTimeout(e);
            break;

        case SceneEventType::Live: {
            this->live_ = true;
            this->lives_ = 3;
            Banner b;
            b.top = QStringLiteral("PLAYER 1");
            b.bottom = QStringLiteral("READY");
            b.ink = PAL_CYAN;
            b.ink2 = PAL_WHITE;
            b.life = 2.6;
            this->addBanner(b);
        }
        break;

        case SceneEventType::Offline:
            this->live_ = false;
            this->credits_ = 0;
            break;

        case SceneEventType::Tab: {
            this->stage_ = e.amount + 1;
            Banner b;
            b.top = QStringLiteral("STAGE ") + QString::number(this->stage_);
            b.bottom = arcadeText(e.text, 14);
            b.style = BANNER_STAGE;
            b.ink = PAL_CYAN;
            b.ink2 = PAL_PALE;
            b.life = 2.6;
            this->addBanner(b);
            this->warp_ = 1;
            this->citySeed_ = static_cast<std::uint32_t>(qHash(e.text)) | 1U;
            this->buildCity(this->citySeed_);
            this->cityRise_ = 1;
        }
        break;

        case SceneEventType::Hover:
            this->hover_.active = true;
            this->hover_.leaving = false;
            this->hover_.age = 0;
            this->hover_.row = this->toGame(e.row);
            break;

        case SceneEventType::Unhover:
            if (this->hover_.active)
            {
                this->hover_.leaving = true;
                this->hover_.leaveAge = 0;
            }
            break;

        case SceneEventType::Click: {
            const QPointF goal = this->toGame(e.point);
            if (!this->live_)
            {
                this->credits_ = std::min(9, this->credits_ + 1);
                break;
            }
            Shot s;
            const QPointF nose = this->shipNose();
            s.x = nose.x();
            s.y = nose.y();
            s.goal = goal;
            s.kind = SHOT_CLICK;
            s.speed = 320;
            const double d =
                std::max(1.0, std::hypot(goal.x() - s.x, goal.y() - s.y));
            s.vx = (goal.x() - s.x) / d * s.speed;
            s.vy = (goal.y() - s.y) / d * s.speed;
            s.ink = PAL_AMBER;
            this->shots_.push_back(s);
            this->recoil_ = std::max(this->recoil_, 0.4);
        }
        break;
    }
}

void ArcadeScene::fireBeam(QPointF from, QPointF to, double power,
                           std::uint8_t ink)
{
    Beam b;
    b.from = from;
    b.to = to;
    b.power = power;
    b.ink = ink;
    b.life = this->reduced_ ? 0.45 : 0.55;
    this->beams_.push_back(b);
}

void ArcadeScene::onSend(const SceneEvent &e)
{
    const int chars = std::max(1, e.amount);
    const double power =
        std::clamp(std::max(this->charge_, chars / 48.0) + this->power_ * 0.25,
                   0.12, 1.3);
    const int mult = this->multiplier();
    const long long points = static_cast<long long>(chars) * 10 * mult;
    const QPointF nose = this->shipNose();
    const std::uint8_t ink = power > 0.7 ? PAL_MAGENTA : PAL_CYAN;

    QPointF hit;
    if (this->boss_.active && (this->boss_.phase == BOSS_FIGHT ||
                               this->boss_.phase == BOSS_ENTER))
    {
        hit = QPointF(this->boss_.x, this->boss_.y + this->boss_.scale);
        if (this->boss_.phase == BOSS_FIGHT && this->boss_.shield <= 0)
        {
            const double damage =
                this->boss_.maxHp / this->boss_.hitsNeeded * (2 + power * 4);
            this->boss_.hp = std::max(0.0, this->boss_.hp - damage);
            this->boss_.hitFlash = 0.1;
            this->boss_.hitX = hit.x();
            this->boss_.hitY = hit.y();
        }
        this->spawnExplosion(hit.x(), hit.y(), 0.6 + power * 0.5, true);
    }
    else if (Enemy *target = this->nearestEnemy(nose))
    {
        hit = QPointF(target->x, target->y);
        target->flash = 0.1;
        this->killEnemy(*target, 0.8 + power * 0.8, true);

        // A big charge takes the neighbours with it in a short chain
        const double radius = 6 + power * 18;
        for (auto &other : this->enemies_)
        {
            if (other.dead || other.hidden || other.fleeing)
            {
                continue;
            }
            const double d = std::hypot(other.x - hit.x(), other.y - hit.y());
            if (d < radius && power > 0.25)
            {
                this->pendingKills_.push_back(
                    {other.id, this->t_ + 0.08 + d * 0.012, 50 * mult});
            }
        }
    }
    else
    {
        // Nothing on screen: blast a drifting rock in the field
        hit = QPointF(this->arena_.center().x() + this->rng_.range(-10, 10),
                      this->arena_.top() + this->arena_.height() * 0.3);
        this->spawnExplosion(hit.x(), hit.y(), 0.8 + power * 0.6, true);
        for (int i = 0; i < 14; ++i)
        {
            Particle p;
            p.x = hit.x();
            p.y = hit.y();
            const double a = this->rng_.next() * TAU;
            p.vx = std::cos(a) * this->rng_.range(15, 45);
            p.vy = std::sin(a) * this->rng_.range(15, 45);
            p.gravity = 40;
            p.life = 1.0;
            p.ink = this->rng_.next() < 0.5 ? PAL_STEEL : PAL_DUSK;
            p.kind = PK_DEBRIS;
            p.front = true;
            this->spawnParticle(p);
        }
    }

    this->fireBeam(nose, hit, power, ink);
    this->afterglows_.push_back({hit.x(), hit.y(), 5 + power * 6, 0, 2.6});
    // Charged shots keep going off: one or two secondary blasts
    if (power > 0.45)
    {
        this->booms_.push_back({hit.x() + this->rng_.range(-5, 5),
                                hit.y() + this->rng_.range(-4, 3),
                                0.4 + power * 0.35, this->t_ + 0.13});
    }
    if (power > 0.85)
    {
        this->booms_.push_back({hit.x() + this->rng_.range(-6, 6),
                                hit.y() + this->rng_.range(-5, 2),
                                0.35 + power * 0.3, this->t_ + 0.27});
    }
    Ring second;
    second.x = hit.x();
    second.y = hit.y();
    second.r0 = 3;
    second.r1 = 10 + power * 12;
    second.life = 0.5;
    second.age = -0.08;
    second.front = true;
    this->rings_.push_back(second);

    // Docked wingmen fire with you
    for (const auto &pod : this->pods_)
    {
        if (!pod.active || pod.age < 0.9)
        {
            continue;
        }
        const QPointF from(pod.x, pod.y - 3);
        if (Enemy *t2 = this->nearestEnemy(from))
        {
            this->fireBeam(from, QPointF(t2->x, t2->y), 0.25, pod.ink);
            this->killEnemy(*t2, 0.5, true);
            this->addScore(50LL * mult);
        }
    }

    this->addScore(points);
    const QString label = QStringLiteral("+") + QString::number(points);
    const int popScale = points >= 120 ? 2 : 1;
    this->addPopup(label, hit.x(), hit.y() - 8 - popScale * 5, PAL_AMBER,
                   popScale, true, 1.4);
    if (mult > 1)
    {
        this->addPopup(QStringLiteral("*") + QString::number(mult),
                       hit.x(), hit.y() + 4, PAL_MAGENTA, 1, true, 1.2);
    }

    // Muzzle flash at the nose
    Particle flash;
    flash.x = nose.x();
    flash.y = nose.y();
    flash.life = 0.12;
    flash.kind = PK_FIRE;
    flash.big = true;
    this->spawnParticle(flash);

    if (!this->reduced_)
    {
        this->kickAge_ = 0;
        this->kickAmp_ = 1.2 + power * 1.6;
    }
    this->recoil_ = 1;
    this->charge_ = 0;
    this->keyPulse_ = 0;
    this->pellets_.clear();
}

void ArcadeScene::onMessage(const SceneEvent &e, const SceneState &state)
{
    if (this->boss_.active)
    {
        // During a boss fight every chat line becomes a shot from the city
        if (this->boss_.phase == BOSS_DYING)
        {
            return;
        }
        this->boss_.pendingShots = std::min(80, this->boss_.pendingShots + 1);
        return;
    }
    this->spawnEnemy(e, state.energy);
}

void ArcadeScene::onRaid(const SceneEvent &e)
{
    const int viewers = std::max(1, e.amount);
    Boss &b = this->boss_;
    b = Boss();
    b.active = true;
    b.phase = BOSS_WARNING;
    b.viewers = viewers;
    b.maxHp = viewers;
    b.hp = viewers;
    b.lagHp = viewers;
    b.hitsNeeded = std::clamp(18 + std::sqrt(double(viewers)) * 0.9, 20.0,
                              70.0);
    b.scale = std::clamp(0.95 + 0.25 * std::log10(std::max(10, viewers)), 1.0,
                         2.0);
    b.scale = std::min(b.scale, (this->arena_.width() - 8) / 50.0);
    b.scale = std::max(0.8, b.scale);
    b.name = arcadeText(e.user, 12);
    if (b.name.isEmpty())
    {
        b.name = QStringLiteral("RAIDER");
    }
    b.x = this->arena_.center().x();
    b.y = this->arena_.top() - 40;

    Banner banner;
    banner.top = QStringLiteral("WARNING");
    banner.bottom = b.name + QStringLiteral(" *") + QString::number(viewers);
    banner.style = BANNER_WARNING;
    banner.ink = PAL_EMBER;
    banner.ink2 = PAL_AMBER;
    banner.life = 2.6;
    this->addBanner(banner);

    // The formation scatters out of the way
    for (auto &en : this->enemies_)
    {
        if (!en.dead && !en.fleeing)
        {
            en.fleeing = true;
            en.fleeAge = -this->rng_.next() * 0.6;
        }
    }
    this->combo_.active = false;
}

void ArcadeScene::onTimeout(const SceneEvent &e)
{
    ModLaser &l = this->laser_;
    l = ModLaser();
    l.active = true;
    Enemy *target = nullptr;
    for (auto &en : this->enemies_)
    {
        if (!en.dead && !en.fleeing && !en.hidden && en.entry >= 1 &&
            en.erase < 0 && en.user == e.user && !e.user.isEmpty())
        {
            target = &en;
        }
    }
    if (target == nullptr)
    {
        // Otherwise the drone closest to the middle of the field
        const QPointF centre(this->arena_.center().x(),
                             this->arena_.top() + this->arena_.height() * 0.3);
        double best = 1e18;
        for (auto &en : this->enemies_)
        {
            if (en.dead || en.fleeing || en.hidden || en.entry < 1 ||
                en.erase >= 0)
            {
                continue;
            }
            const double d = std::hypot(en.x - centre.x(), en.y - centre.y());
            if (d < best)
            {
                best = d;
                target = &en;
            }
        }
    }
    if (target != nullptr)
    {
        l.target = target->id;
        l.tx = target->x;
        l.ty = target->y;
        target->claimed = true;
    }
    else
    {
        l.tx = this->arena_.center().x();
        l.ty = this->ground_ - 4;
    }
    l.satY = std::max(3.0, this->top_.bottom() - 4);
}

void ArcadeScene::onCombo(const SceneEvent &e)
{
    if (this->boss_.active)
    {
        return;
    }
    const QString text = arcadeText(e.text, 8);
    if (text.isEmpty())
    {
        return;
    }
    // Hand back any drones still borrowed by an older combo
    for (auto &en : this->enemies_)
    {
        en.hidden = false;
    }

    Combo &c = this->combo_;
    c = Combo();
    c.active = true;
    c.amount = std::max(1, e.amount);
    c.life = 3.0;
    const int n = static_cast<int>(text.size());
    const double room = this->arena_.width() - 8;
    const int sp = std::clamp(fi(room / std::max(1, n * 4 - 1)), 2, 6);
    c.dot = std::max(1, sp - 2);
    const int width = (n * 4 - 1) * sp;
    const int x0 = ri(this->arena_.center().x()) - width / 2;
    const int y0 = ri(this->arena_.top() + this->arena_.height() * 0.24);
    c.label = QPointF(this->arena_.center().x(), y0 + 5 * sp + 4);

    // Borrow the closest drones; the rest fly in from above
    std::vector<Enemy *> donors;
    for (auto &en : this->enemies_)
    {
        if (!en.dead && !en.fleeing && en.erase < 0 && en.entry >= 1)
        {
            donors.push_back(&en);
        }
    }
    const QPointF centre(this->arena_.center().x(), y0);
    std::sort(donors.begin(), donors.end(), [&](Enemy *a, Enemy *b) {
        return std::hypot(a->x - centre.x(), a->y - centre.y()) <
               std::hypot(b->x - centre.x(), b->y - centre.y());
    });
    size_t donor = 0;
    for (int g = 0; g < n; ++g)
    {
        const auto bits = glyphBits(text[g].toLatin1());
        for (int row = 0; row < 5; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (!((bits >> ((4 - row) * 3 + (2 - col))) & 1))
                {
                    continue;
                }
                ComboDot d;
                d.to = QPointF(x0 + (g * 4 + col) * sp, y0 + row * sp);
                d.glyph = g;
                if (donor < donors.size())
                {
                    Enemy *en = donors[donor++];
                    en->hidden = true;
                    d.enemy = en->id;
                    d.from = QPointF(en->x, en->y);
                }
                else
                {
                    d.from = QPointF(
                        this->arena_.left() +
                            this->rng_.next() * this->arena_.width(),
                        -6 - this->rng_.next() * 10);
                }
                d.delay = std::min(0.35, 0.012 * c.dots.size());
                c.dots.push_back(d);
            }
        }
    }
}

// Simulation ------------------------------------------------------------------

void ArcadeScene::step(const SceneState &state, const SceneLayout &layout)
{
    this->ensureSize(layout);
    this->updateGeometry(layout);
    const double dt = state.dt;
    this->t_ = state.t;
    this->reduced_ = state.reduced;
    this->live_ = state.live;
    this->speed_ = state.speed;
    this->energy_ = state.energy;
    this->pointer_ = state.pointer;
    if (state.hypeLevel != this->hypeLevel_ && state.hypeLevel == 0)
    {
        this->hypeProgress_ = 0;
    }
    this->hypeLevel_ = state.hypeLevel;
    this->hypeProgress_ +=
        (state.hypeProgress - this->hypeProgress_) * std::min(1.0, dt * 4);

    // Viewers: denser stars and more lit windows, capped
    double f = 0.35;
    if (state.viewers >= 0)
    {
        f = clamp01((std::log10(std::max(1, state.viewers)) - 1) / 3.5);
    }
    this->viewerFactor_ += (f - this->viewerFactor_) * std::min(1.0, dt * 0.8);
    const double starTarget = 40 + 230 * f;
    this->starCount_ += (starTarget - this->starCount_) * std::min(1.0, dt * 0.8);
    this->litTarget_ =
        (0.04 + 0.86 * std::pow(f, 1.3)) * (this->live_ ? 1.0 : 0.35);
    const double litStep = dt * 0.12;
    this->litShown_ += std::clamp(this->litTarget_ - this->litShown_, -litStep,
                                  litStep);

    this->activeCols_ =
        std::min(this->maxCols_, std::max(3, 3 + ri(state.speed * 0.7)));
    this->warp_ = std::max(0.0, this->warp_ - dt / 1.3);
    this->cityRise_ = std::max(0.0, this->cityRise_ - dt / 1.0);
    this->roll_ = std::fmod(this->roll_ + dt * 0.09, 1.5);

    this->stepStars(dt);
    this->stepShip(state, dt);
    this->stepEnemies(dt);
    this->stepEscort(dt);
    this->stepShots(dt);
    this->stepBoss(dt);
    this->stepPickups(dt);
    this->stepEffects(dt);

    // Score rolls up in the HUD
    const double gap = double(this->score_) - this->shownScore_;
    if (gap > 0)
    {
        this->shownScore_ +=
            std::max(std::min(gap, 3.0), gap * std::min(1.0, dt * 4.5));
    }
    else
    {
        this->shownScore_ = double(this->score_);
    }
    this->hiScore_ =
        std::max(this->hiScore_, static_cast<long long>(this->shownScore_));

    // Screen kick
    this->kickAge_ += dt;
    if (this->kickAge_ < 0.4 && !this->reduced_)
    {
        const double env = std::exp(-this->kickAge_ * 11);
        this->kickY_ = ri(this->kickAmp_ * env * std::cos(this->kickAge_ * 40));
        this->kickX_ =
            ri(this->kickAmp_ * 0.45 * env * std::sin(this->kickAge_ * 33));
    }
    else
    {
        this->kickX_ = 0;
        this->kickY_ = 0;
    }

    // Front layer only while a short accent is alive
    bool front = !this->beams_.empty();
    front = front || std::any_of(this->particles_.begin(),
                                 this->particles_.end(), [](const Particle &p) {
                                     return p.front;
                                 });
    front = front || std::any_of(this->popups_.begin(), this->popups_.end(),
                                 [](const Popup &p) {
                                     return p.front;
                                 });
    front = front || std::any_of(this->rings_.begin(), this->rings_.end(),
                                 [](const Ring &r) {
                                     return r.front;
                                 });
    front = front || std::any_of(this->stamps_.begin(), this->stamps_.end(),
                                 [](const Stamp &s) {
                                     return s.age < 1.4;
                                 });
    front = front || (this->hover_.active && this->hover_.age < 1.2);
    front = front || (this->p2_.active && this->p2_.age < 2.2);
    this->frontActive_ = front;
}

void ArcadeScene::stepStars(double dt)
{
    const double base = this->reduced_ ? 0.08 : 1.0;
    const double hype = 1 + std::min(this->hypeLevel_, 4) * 0.25;
    const double warp = 1 + this->warp_ * this->warp_ * 14;
    constexpr std::array<double, 3> speeds{1.1, 3.2, 7.5};
    for (size_t i = 0; i < 3; ++i)
    {
        this->starScroll_[i] += speeds[i] * base * hype * warp * dt;
        this->starScroll_[i] = std::fmod(this->starScroll_[i], 100000.0);
    }
}

void ArcadeScene::stepShip(const SceneState &state, double dt)
{
    const double target =
        state.draftLength <= 0 ? 0 : clamp01(state.draftLength / 48.0);
    const double rate = target > this->charge_ ? 10 : 7;
    this->charge_ += (target - this->charge_) * std::min(1.0, dt * rate);
    this->keyPulse_ = std::max(0.0, this->keyPulse_ - dt * 5);
    this->drain_ = std::max(0.0, this->drain_ - dt * 4);
    this->recoil_ = std::max(0.0, this->recoil_ - dt * 5);
    this->pickupFlash_ = std::max(0.0, this->pickupFlash_ - dt * 4);
    this->power_ = std::max(0.0, this->power_ - dt * 0.05);
    const double away = this->live_ ? 0.0 : 1.0;
    this->shipAway_ += std::clamp(away - this->shipAway_, -dt * 1.2, dt * 1.2);

    for (auto &p : this->pellets_)
    {
        p.age += dt;
    }
    std::erase_if(this->pellets_, [](const Pellet &p) {
        return p.age >= p.life;
    });

    const QPointF s = this->shipPos();
    for (size_t i = 0; i < this->pods_.size(); ++i)
    {
        Pod &pod = this->pods_[i];
        if (!pod.active)
        {
            continue;
        }
        pod.age += dt;
        const QPointF dock(s.x() - 14 - 11.0 * double(i), s.y() - 2);
        if (pod.age < 0.9)
        {
            const double u = easeOutCubic(pod.age / 0.9);
            pod.x = lerp(pod.start.x(), dock.x(), u);
            pod.y = lerp(pod.start.y(), dock.y(), u) -
                    std::sin(u * TAU / 2) * 10;
        }
        else if (pod.age > pod.life - 1.0)
        {
            const double u = clamp01((pod.age - (pod.life - 1.0)) / 1.0);
            pod.x = dock.x() - u * 20;
            pod.y = dock.y() - u * u * 60;
        }
        else
        {
            pod.x = dock.x();
            pod.y = dock.y();
        }
        if (pod.age >= 0.9 && !pod.docked)
        {
            pod.docked = true;
            for (int k = 0; k < 6; ++k)
            {
                Particle p;
                p.x = pod.x;
                p.y = pod.y;
                const double a = k * TAU / 6;
                p.vx = std::cos(a) * 20;
                p.vy = std::sin(a) * 20;
                p.life = 0.4;
                p.kind = PK_TWINKLE;
                p.ink = pod.ink;
                this->spawnParticle(p);
            }
        }
        if (pod.age >= pod.life)
        {
            pod.active = false;
        }
    }
}

void ArcadeScene::stepEnemies(double dt)
{
    const double vy = (0.7 + 0.35 * std::min(this->speed_, 6.0)) *
                      (this->reduced_ ? 0.4 : 1.0);
    this->scroll_ += dt * vy / SLOT_Y;

    for (auto &e : this->enemies_)
    {
        if (e.dead)
        {
            continue;
        }
        e.age += dt;
        e.flash = std::max(0.0, e.flash - dt);
        e.newTag = std::max(0.0, e.newTag - dt);
        if (e.fleeing)
        {
            e.fleeAge += dt;
            if (e.fleeAge > 0)
            {
                e.y -= (30 + e.fleeAge * 140) * dt;
                e.x += e.side * (10 + e.fleeAge * 20) * dt;
            }
            if (e.y < -14)
            {
                e.dead = true;
            }
            continue;
        }
        const QPointF slot = this->slotPos(e.row, e.col);
        if (e.entry < 1)
        {
            e.entry = std::min(1.0, e.entry + dt / e.entryTime);
            // With reduced motion drones fade in at their slot instead
            const QPointF p = this->reduced_ ? slot : this->entryPos(e, slot);
            e.x = p.x();
            e.y = p.y();
        }
        else
        {
            e.x = slot.x();
            e.y = slot.y();
        }
        if (e.erase >= 0)
        {
            e.erase += dt / 0.4;
        }
        // Failsafe: anything that reaches the rooftops bursts quietly
        if (e.entry >= 1 && e.y > this->arena_.bottom() - 10)
        {
            this->killEnemy(e, 0.3, false);
        }
    }

    // Chain reactions from your beam
    for (auto &k : this->pendingKills_)
    {
        if (k.at <= this->t_)
        {
            if (Enemy *e = this->findEnemy(k.id))
            {
                this->killEnemy(*e, 0.55, true);
                this->addScore(k.points);
            }
            k.id = -1;
        }
    }
    std::erase_if(this->pendingKills_, [](const PendingKill &k) {
        return k.id < 0;
    });
    std::erase_if(this->enemies_, [](const Enemy &e) {
        return e.dead;
    });

    // Keep a few idle drones around so a send always has a target
    this->ambientTimer_ -= dt;
    if (this->ambientTimer_ <= 0)
    {
        this->ambientTimer_ = 5;
        if (this->formationCount() < 3 && !this->boss_.active)
        {
            SceneEvent idle;
            idle.user = QStringLiteral("idle") +
                        QString::number(this->nextId_ % 4);
            this->spawnEnemy(idle, 0.2);
        }
    }
}

void ArcadeScene::stepEscort(double dt)
{
    const double y = this->arena_.top() + this->arena_.height() * 0.86;
    if (this->escortX_ <= 0)
    {
        this->escortX_ = this->arena_.center().x();
    }
    this->escortBoost_ = std::max(0.0, this->escortBoost_ - dt);

    const int count = this->formationCount();
    Enemy *target = this->escortTarget_ >= 0
                        ? this->findEnemy(this->escortTarget_)
                        : nullptr;
    if (target != nullptr &&
        (target->hidden || target->fleeing || target->erase >= 0))
    {
        target = nullptr;
    }
    if (target == nullptr && !this->boss_.active)
    {
        this->escortTarget_ = TARGET_NONE;
        Enemy *lowest = nullptr;
        for (auto &e : this->enemies_)
        {
            if (e.dead || e.hidden || e.fleeing || e.entry < 1 ||
                e.erase >= 0 || e.claimed)
            {
                continue;
            }
            if (lowest == nullptr || e.y > lowest->y + 0.5 ||
                (std::abs(e.y - lowest->y) <= 0.5 &&
                 std::abs(e.x - this->escortX_) <
                     std::abs(lowest->x - this->escortX_)))
            {
                lowest = &e;
            }
        }
        const bool needed =
            lowest != nullptr &&
            (count > MIN_KEEP || lowest->y > this->arena_.bottom() - 64);
        if (needed)
        {
            target = lowest;
            this->escortTarget_ = lowest->id;
        }
    }

    double goalX = this->arena_.center().x() +
                   std::sin(this->t_ * 0.21) * this->arena_.width() * 0.25;
    if (target != nullptr)
    {
        goalX = target->x;
    }
    else if (this->boss_.active && this->boss_.phase == BOSS_FIGHT)
    {
        goalX = this->boss_.x;
    }
    const double maxMove = 50 * dt;
    this->escortX_ += std::clamp(goalX - this->escortX_, -maxMove, maxMove);
    this->escortX_ = std::clamp(this->escortX_, this->arena_.left() + 4,
                                this->arena_.right() - 4);

    this->escortCool_ -= dt * (this->escortBoost_ > 0 ? 2.0 : 1.0);
    if (this->escortCool_ > 0)
    {
        return;
    }
    if (target != nullptr && std::abs(target->x - this->escortX_) < 3)
    {
        Shot s;
        s.x = this->escortX_;
        s.y = y - 3;
        s.vx = 0;
        s.vy = -170;
        s.speed = 170;
        s.target = target->id;
        s.kind = SHOT_ESCORT;
        s.ink = PAL_CYAN;
        this->shots_.push_back(s);
        this->escortCool_ = std::clamp(2.4 - 0.055 * count, 0.4, 2.4);
    }
    else if (this->boss_.active && this->boss_.phase == BOSS_FIGHT)
    {
        Shot s;
        s.x = this->escortX_;
        s.y = y - 3;
        s.vy = -160;
        s.speed = 160;
        s.target = TARGET_BOSS;
        s.kind = SHOT_ESCORT;
        s.damage = 0.4;
        s.ink = PAL_CYAN;
        this->shots_.push_back(s);
        this->escortCool_ = 0.9;
    }
}

void ArcadeScene::stepShots(double dt)
{
    for (auto &s : this->shots_)
    {
        s.age += dt;
        // Home in on the target
        QPointF goal = s.goal;
        bool homing = false;
        if (s.target == TARGET_BOSS && this->boss_.active)
        {
            goal = QPointF(this->boss_.x, this->boss_.y);
            homing = true;
        }
        else if (s.target >= 0)
        {
            if (const Enemy *e = this->findEnemy(s.target))
            {
                goal = QPointF(e->x, e->y);
                homing = true;
            }
        }
        if (homing || s.kind == SHOT_CLICK)
        {
            const double dx = goal.x() - s.x;
            const double dy = goal.y() - s.y;
            const double d = std::max(0.001, std::hypot(dx, dy));
            const double turn = s.kind == SHOT_CLICK ? 1.0 : std::min(1.0, dt * 7);
            s.vx = lerp(s.vx, dx / d * s.speed, turn);
            s.vy = lerp(s.vy, dy / d * s.speed, turn);
            const double v = std::max(0.001, std::hypot(s.vx, s.vy));
            s.vx = s.vx / v * s.speed;
            s.vy = s.vy / v * s.speed;

            const double reach =
                s.target == TARGET_BOSS ? 10 * this->boss_.scale : 3.5;
            if (d <= std::max(reach, s.speed * dt))
            {
                if (s.kind == SHOT_CLICK)
                {
                    // Click shots pop whatever is close to the cursor
                    Enemy *best = nullptr;
                    double bestD = 8;
                    for (auto &e : this->enemies_)
                    {
                        if (e.dead || e.hidden || e.fleeing)
                        {
                            continue;
                        }
                        const double ed = std::hypot(e.x - goal.x(),
                                                     e.y - goal.y());
                        if (ed < bestD)
                        {
                            bestD = ed;
                            best = &e;
                        }
                    }
                    if (best != nullptr)
                    {
                        const long long pts = 50LL * this->multiplier();
                        this->killEnemy(*best, 0.5, false);
                        this->addScore(pts);
                        this->addPopup(QStringLiteral("+") +
                                           QString::number(pts),
                                       goal.x(), goal.y() - 6, PAL_AMBER, 1,
                                       false, 1.0);
                    }
                    else
                    {
                        this->spawnExplosion(goal.x(), goal.y(), 0.15, false);
                    }
                }
                else if (s.target == TARGET_BOSS)
                {
                    Boss &b = this->boss_;
                    if (b.phase == BOSS_FIGHT)
                    {
                        if (b.shield > 0)
                        {
                            for (int i = 0; i < 3; ++i)
                            {
                                Particle p;
                                p.x = s.x;
                                p.y = s.y;
                                p.vx = this->rng_.range(-30, 30);
                                p.vy = this->rng_.range(10, 40);
                                p.life = 0.25;
                                p.kind = PK_SPARK;
                                this->spawnParticle(p);
                            }
                        }
                        else
                        {
                            const double damage =
                                b.maxHp / b.hitsNeeded * s.damage;
                            b.hp = std::max(0.0, b.hp - damage);
                            b.hitFlash = 0.07;
                            b.hitX = s.x;
                            b.hitY = s.y;
                            this->spawnExplosion(s.x, s.y, 0.12, false);
                            if (s.kind == SHOT_TRACER && damage >= 1 &&
                                this->rng_.next() < 0.5)
                            {
                                this->addPopup(
                                    QStringLiteral("-") +
                                        QString::number(
                                            static_cast<int>(std::ceil(damage))),
                                    s.x + this->rng_.range(-6, 6), s.y - 4,
                                    PAL_PALE, 1, false, 0.7);
                            }
                        }
                    }
                }
                else if (Enemy *e = this->findEnemy(s.target))
                {
                    this->killEnemy(*e, 0.35, false);
                }
                s.age = 99;
            }
        }
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        if (s.y < -10 || s.y > this->winH_ + 10 || s.x < -10 ||
            s.x > this->winW_ + 10)
        {
            s.age = 99;
        }
    }
    std::erase_if(this->shots_, [](const Shot &s) {
        return s.age > 3;
    });
}

void ArcadeScene::stepBoss(double dt)
{
    Boss &b = this->boss_;
    if (!b.active)
    {
        return;
    }
    b.age += dt;
    b.hitFlash = std::max(0.0, b.hitFlash - dt);
    b.shield = std::max(0.0, b.shield - dt);
    b.lagHp = std::max(b.hp, b.lagHp - b.maxHp * dt * 0.35);
    const double homeY = this->arena_.top() + this->arena_.height() * 0.3;
    const double swing = std::max(
        0.0, this->arena_.width() / 2 - 26 * b.scale - 4);

    switch (b.phase)
    {
        case BOSS_WARNING:
            b.x = this->arena_.center().x();
            if (b.age > 1.8)
            {
                b.phase = BOSS_ENTER;
                b.age = 0;
            }
            break;
        case BOSS_ENTER: {
            const double u = easeOutCubic(b.age / 2.2);
            b.y = lerp(this->arena_.top() - 40, homeY, u);
            if (b.age > 2.2)
            {
                b.phase = BOSS_FIGHT;
                b.age = 0;
            }
        }
        break;
        case BOSS_FIGHT: {
            b.x = this->arena_.center().x() +
                  (this->reduced_ ? 0 : std::sin(b.age * 0.45) * swing);
            b.y = homeY + (this->reduced_ ? 0 : std::sin(b.age * 0.9) * 2);

            // Release the queued chat shots as a barrage from the rooftops
            b.shotTimer -= dt;
            if (b.pendingShots > 0 && b.shotTimer <= 0)
            {
                b.pendingShots--;
                b.shotTimer = 1.0 / 7.0;
                Shot s;
                s.x = this->arena_.left() + 6 +
                      this->rng_.next() * std::max(1.0, this->arena_.width() - 12);
                s.y = this->ground_ - 8 - this->rng_.next() * 14;
                s.vx = 0;
                s.vy = -150;
                s.speed = 150;
                s.target = TARGET_BOSS;
                s.kind = SHOT_TRACER;
                s.damage = 1.0;
                constexpr std::array<std::uint8_t, 4> inks{
                    PAL_CYAN, PAL_MAGENTA, PAL_AMBER, PAL_WHITE};
                s.ink = inks[static_cast<size_t>(this->rng_.next() * 4) % 4];
                this->shots_.push_back(s);
            }

            // If chat goes quiet, your ship joins in after a while
            if (b.age > 26)
            {
                b.assistTimer -= dt;
                if (b.assistTimer <= 0)
                {
                    b.assistTimer = 0.7;
                    const QPointF nose = this->shipNose();
                    Shot s;
                    s.x = nose.x();
                    s.y = nose.y();
                    s.vy = -200;
                    s.speed = 200;
                    s.target = TARGET_BOSS;
                    s.kind = SHOT_TRACER;
                    s.damage = 1.5;
                    s.ink = PAL_MAGENTA;
                    this->shots_.push_back(s);
                }
            }

            // Armour breaks in stages
            const double sc = b.scale;
            if (b.podLeft && b.hp < b.maxHp * 0.66)
            {
                b.podLeft = false;
                b.shield = 1.1;
                this->spawnExplosion(b.x - 21 * sc, b.y - sc, 1.0, false);
            }
            if (b.podRight && b.hp < b.maxHp * 0.33)
            {
                b.podRight = false;
                b.shield = 1.1;
                this->spawnExplosion(b.x + 21 * sc, b.y - sc, 1.0, false);
            }
            if (b.hp <= 0)
            {
                b.phase = BOSS_DYING;
                b.age = 0;
            }
        }
        break;
        case BOSS_DYING: {
            b.boomTimer -= dt;
            if (b.age < 2.0 && b.boomTimer <= 0)
            {
                b.boomTimer = this->reduced_ ? 0.35 : 0.13;
                this->spawnExplosion(
                    b.x + this->rng_.range(-14, 14) * b.scale,
                    b.y + this->rng_.range(-8, 4) * b.scale,
                    0.45 + this->rng_.next() * 0.3, false);
            }
            if (b.age >= 2.0 && !b.finalBlast)
            {
                b.finalBlast = true;
                this->spawnExplosion(b.x, b.y, 2.0, false);
                this->afterglows_.push_back({b.x, b.y, 16 * b.scale, 0, 3.5});
                const int count = this->reduced_ ? 30 : 130;
                for (int i = 0; i < count; ++i)
                {
                    Particle p;
                    const double a = this->rng_.next() * TAU;
                    const double r = this->rng_.next() * 14 * b.scale;
                    p.x = b.x + std::cos(a) * r;
                    p.y = b.y + std::sin(a) * r * 0.6;
                    const double v = 20 + this->rng_.next() * 60;
                    p.vx = std::cos(a) * v;
                    p.vy = std::sin(a) * v - 25;
                    p.gravity = 45;
                    p.drag = 0.6;
                    p.life = 1.0 + this->rng_.next() * 1.0;
                    constexpr std::array<std::uint8_t, 5> inks{
                        PAL_PLUM, PAL_MAGENTA, PAL_STEEL, PAL_DUSK, PAL_WHITE};
                    p.ink = inks[static_cast<size_t>(this->rng_.next() * 5) % 5];
                    p.kind = PK_DEBRIS;
                    this->spawnParticle(p);
                }
                if (!this->reduced_)
                {
                    this->kickAge_ = 0;
                    this->kickAmp_ = 2.5;
                }
                const long long bonus = static_cast<long long>(b.viewers) * 10;
                this->addScore(bonus);
                Banner banner;
                banner.top = QStringLiteral("RAID CLEARED");
                banner.bottom =
                    QStringLiteral("BONUS ") + QString::number(bonus);
                banner.style = BANNER_CLEAR;
                banner.ink = PAL_WHITE;
                banner.ink2 = PAL_AMBER;
                banner.life = 3.0;
                this->addBanner(banner);
            }
            if (b.age > 2.6)
            {
                b.active = false;
            }
        }
        break;
    }
}

void ArcadeScene::stepPickups(double dt)
{
    const QPointF ship = this->shipPos();
    for (auto &c : this->capsules_)
    {
        if (c.delay > 0)
        {
            c.delay -= dt;
            continue;
        }
        c.age += dt;
        if (!c.magnet && (c.y > ship.y() - 60 || c.age > 1.7))
        {
            c.magnet = true;
        }
        if (c.magnet)
        {
            const double dx = ship.x() - c.x;
            const double dy = ship.y() - c.y;
            const double d = std::max(0.001, std::hypot(dx, dy));
            const double v = std::min(d / dt, 90 + c.age * 90);
            c.x += dx / d * v * dt;
            c.y += dy / d * v * dt;
            if (d < 4)
            {
                c.collected = true;
                this->capsulesLeft_ = std::max(0, this->capsulesLeft_ - 1);
                this->power_ = std::min(1.5, this->power_ + (c.big ? 0.06 : 0.04));
                this->pickupFlash_ = 1;
                this->addScore(100LL * this->multiplier());
                Particle p;
                p.x = ship.x() + this->rng_.range(-4, 4);
                p.y = ship.y() + this->rng_.range(-4, 2);
                p.life = 0.45;
                p.kind = PK_TWINKLE;
                p.ink = c.big ? PAL_AMBER : PAL_CYAN;
                this->spawnParticle(p);
                if (this->capsulesLeft_ == 0)
                {
                    this->addPopup(c.big ? QStringLiteral("MAX POWER")
                                         : QStringLiteral("POWER UP"),
                                   ship.x() - 14, ship.y() - 12,
                                   c.big ? PAL_AMBER : PAL_CYAN, 1, false,
                                   1.8);
                }
            }
        }
        else
        {
            c.y += c.vy * dt;
            if (!this->reduced_)
            {
                c.x += std::sin(c.age * 5 + c.phase) * 8 * dt;
            }
        }
    }
    std::erase_if(this->capsules_, [](const Capsule &c) {
        return c.collected || c.age > 6;
    });

    for (auto &c : this->coins_)
    {
        if (c.delay > 0)
        {
            c.delay -= dt;
            continue;
        }
        c.age += dt;
        if (c.age < 1.15)
        {
            c.vy += 130 * dt;
            c.x += c.vx * dt;
            c.y += c.vy * dt;
            c.vx *= 1 - std::min(1.0, dt * 0.8);
            if (c.y > this->ground_ - 2 && c.vy > 0)
            {
                c.y = this->ground_ - 2;
                c.vy = -c.vy * 0.45;
                c.bounces++;
            }
        }
        else
        {
            const double dx = ship.x() - c.x;
            const double dy = ship.y() - c.y;
            const double d = std::max(0.001, std::hypot(dx, dy));
            const double v = std::min(d / dt, 80 + (c.age - 1.15) * 400);
            c.x += dx / d * v * dt;
            c.y += dy / d * v * dt;
            if (d < 4)
            {
                c.collected = true;
                this->addScore(c.value);
                this->pickupFlash_ = std::max(this->pickupFlash_, 0.6);
                if (this->rng_.next() < 0.5)
                {
                    Particle p;
                    p.x = ship.x() + this->rng_.range(-5, 5);
                    p.y = ship.y() + this->rng_.range(-5, 3);
                    p.life = 0.35;
                    p.kind = PK_TWINKLE;
                    p.ink = PAL_AMBER;
                    this->spawnParticle(p);
                }
            }
        }
    }
    std::erase_if(this->coins_, [](const Coin &c) {
        return c.collected || c.age > 5;
    });
}

void ArcadeScene::stepEffects(double dt)
{
    for (auto &p : this->particles_)
    {
        p.age += dt;
        if (p.drag > 0)
        {
            const double k = std::max(0.0, 1 - p.drag * dt);
            p.vx *= k;
            p.vy *= k;
        }
        p.vy += p.gravity * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        if (p.kind == PK_SMOKE && !this->reduced_)
        {
            p.x += std::sin(p.age * 3 + p.y) * 2 * dt;
        }
    }
    std::erase_if(this->particles_, [](const Particle &p) {
        return p.age >= p.life;
    });

    for (auto &g : this->afterglows_)
    {
        g.age += dt;
    }
    std::erase_if(this->afterglows_, [](const Afterglow &g) {
        return g.age >= g.life;
    });
    if (this->afterglows_.size() > 8)
    {
        this->afterglows_.erase(this->afterglows_.begin());
    }
    for (auto &b : this->booms_)
    {
        if (b.at <= this->t_)
        {
            this->spawnExplosion(b.x, b.y, b.size, true);
            b.at = -1;
        }
    }
    std::erase_if(this->booms_, [](const Boom &b) {
        return b.at < 0;
    });
    for (auto &r : this->rings_)
    {
        r.age += dt;
    }
    std::erase_if(this->rings_, [](const Ring &r) {
        return r.age >= r.life;
    });
    for (auto &p : this->popups_)
    {
        p.age += dt;
    }
    std::erase_if(this->popups_, [](const Popup &p) {
        return p.age >= p.life;
    });
    for (auto &b : this->beams_)
    {
        b.age += dt;
    }
    std::erase_if(this->beams_, [](const Beam &b) {
        return b.age >= b.life;
    });
    if (!this->banners_.empty())
    {
        this->banners_.front().age += dt;
        if (this->banners_.front().age >= this->banners_.front().life)
        {
            this->banners_.erase(this->banners_.begin());
        }
    }
    for (auto &s : this->stamps_)
    {
        s.age += dt;
    }
    std::erase_if(this->stamps_, [](const Stamp &s) {
        return s.age >= s.life;
    });

    // Combo letters
    if (this->combo_.active)
    {
        this->combo_.age += dt;
        if (this->combo_.age >= this->combo_.life)
        {
            this->combo_.active = false;
            for (auto &e : this->enemies_)
            {
                e.hidden = false;
            }
        }
    }

    // Moderator laser
    ModLaser &l = this->laser_;
    if (l.active)
    {
        l.age += dt;
        Enemy *target = l.target >= 0 ? this->findEnemy(l.target) : nullptr;
        if (target != nullptr && l.age < 0.8)
        {
            l.tx = target->x;
            l.ty = target->y;
        }
        if (target != nullptr && l.age >= 0.8 && target->erase < 0)
        {
            target->erase = 0;
        }
        if (l.age >= 0.8 && l.age < 1.2 && !this->reduced_)
        {
            const double scanY = l.ty - 4 + 9 * clamp01((l.age - 0.8) / 0.4);
            Particle p;
            p.x = l.tx + this->rng_.range(-4, 4);
            p.y = scanY;
            p.vx = this->rng_.range(-20, 20);
            p.vy = this->rng_.range(-15, 5);
            p.gravity = 40;
            p.life = 0.3;
            p.kind = PK_SPARK;
            this->spawnParticle(p);
        }
        if (l.age >= 1.2 && !l.stamped)
        {
            l.stamped = true;
            if (target != nullptr)
            {
                target->dead = true;
            }
            Stamp s;
            s.x = l.tx;
            s.y = l.ty;
            this->stamps_.push_back(s);
        }
        if (l.age > 2.0)
        {
            l.active = false;
        }
    }

    // Player 2 and the hover lock
    if (this->p2_.active)
    {
        this->p2_.age += dt;
        if (this->p2_.age > 3.4)
        {
            this->p2_.active = false;
        }
    }
    if (this->hover_.active)
    {
        this->hover_.age += dt;
        if (this->hover_.leaving)
        {
            this->hover_.leaveAge += dt;
            if (this->hover_.leaveAge > 0.25)
            {
                this->hover_.active = false;
            }
        }
    }

    // Shooting stars, now and then
    this->cometAge_ += dt;
    if (!this->reduced_)
    {
        this->cometTimer_ -= dt;
        if (this->cometTimer_ <= 0)
        {
            this->cometTimer_ = 18 + this->rng_.next() * 26;
            this->cometAge_ = 0;
            this->cometFrom_ = QPointF(
                this->arena_.left() +
                    this->arena_.width() * (0.5 + 0.5 * this->rng_.next()),
                this->arena_.top() + this->rng_.next() * 20);
            this->cometDir_ = QPointF(-1.0, 0.45 + this->rng_.next() * 0.3);
        }
    }
}

// Painting --------------------------------------------------------------------

void ArcadeScene::paintBack(QPainter &painter, const SceneState &state,
                            const SceneLayout &layout)
{
    this->ensureSize(layout);
    this->updateGeometry(layout);
    this->t_ = state.t;
    this->reduced_ = state.reduced;

    this->renderGame(state);
    this->postProcess(state);
    this->expand(this->game_, this->screen_, QRect(0, 0, this->gw_, this->gh_),
                 false);

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(QPointF(0, 0), this->screen_);
    painter.restore();
}

void ArcadeScene::paintFront(QPainter &painter, const SceneState &state,
                             const SceneLayout &layout)
{
    this->ensureSize(layout);
    if (this->front_.size() != this->game_.size())
    {
        this->front_ = QImage(this->gw_, this->gh_,
                              QImage::Format_ARGB32_Premultiplied);
        this->frontScreen_ =
            QImage(this->gw_ * this->devPx_, this->gh_ * this->devPx_,
                   QImage::Format_ARGB32_Premultiplied);
        this->frontScreen_.setDevicePixelRatio(this->dpr_);
    }
    this->front_.fill(Qt::transparent);

    const double t = state.t;
    Canvas cv;
    cv.attach(this->front_);
    cv.setTrack(true);
    // Your shot, its blast and its score sit above everything except chat
    cv.setMask(&this->chats_);
    this->drawBeams(cv, t);
    this->drawRings(cv, true);
    this->drawParticles(cv, true);
    this->drawPopups(cv, true);
    this->drawStamps(cv, true);
    cv.setMask(nullptr);
    // Row accents sit at the far edge of their row
    this->drawHover(cv, t, true);
    this->drawPlayer2(cv, t, true);

    const QRect dirty = cv.dirty();
    if (dirty.isEmpty())
    {
        return;
    }
    // Leave a margin for the screen kick
    const QRect region =
        dirty.adjusted(-4, -4, 4, 4).intersected(this->front_.rect());
    this->expand(this->front_, this->frontScreen_, region, true);

    const int n = this->devPx_;
    const QRectF source(region.left() * n, region.top() * n,
                        region.width() * n, region.height() * n);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(
        QPointF(source.left() / this->dpr_, source.top() / this->dpr_),
        this->frontScreen_, source);
    painter.restore();
}

void ArcadeScene::renderGame(const SceneState &state)
{
    const double t = state.t;
    const int w = this->gw_;
    const int h = this->gh_;

    // Sky
    for (int y = 0; y < h; ++y)
    {
        std::memcpy(this->game_.scanLine(y),
                    this->sky_.data() + static_cast<size_t>(y * w),
                    static_cast<size_t>(w) * sizeof(Pix));
    }

    Canvas cv;
    cv.attach(this->game_);

    this->drawStars(cv, t);

    // Planet
    for (int py = 0; py < this->planetH_; ++py)
    {
        for (int px = 0; px < this->planetW_; ++px)
        {
            const Pix c =
                this->planet_[static_cast<size_t>(py * this->planetW_ + px)];
            if (c != 0)
            {
                cv.put(this->planetX_ + px, this->planetY_ + py, c);
            }
        }
    }

    this->drawAmbient(cv, t);
    this->drawCity(cv, t);
    this->drawBoss(cv, t);
    this->drawEnemies(cv, t);
    this->drawCombo(cv, t);
    this->drawEscort(cv, t);
    this->drawShots(cv);
    this->drawPickups(cv, t);
    this->drawAfterglows(cv);
    this->drawRings(cv, false);
    this->drawParticles(cv, false);
    this->drawAttract(cv, t);

    // Keep the cabinet dim under messages and the draft
    for (const auto &r : this->chats_)
    {
        cv.scaleRect(r, 92);
    }
    for (const auto &r : this->inputs_)
    {
        cv.scaleRect(r, 175);
    }
    for (const auto &r : this->acts_)
    {
        cv.scaleRect(r, 238);
    }

    this->drawModLaser(cv, t);
    this->drawStamps(cv, false);
    this->drawPlayer2(cv, t, false);
    this->drawHover(cv, t, false);
    this->drawCrosshair(cv, t);
    this->drawBeams(cv, t);
    this->drawPellets(cv);
    this->drawShip(cv, t);
    this->drawPopups(cv, false);
    this->drawBanners(cv, t);
    this->drawHud(cv, t);
    this->drawBossBar(cv, t);
}

void ArcadeScene::postProcess(const SceneState &state)
{
    const int w = this->gw_;
    const int h = this->gh_;
    auto *bits = reinterpret_cast<Pix *>(this->game_.bits());
    const int stride = static_cast<int>(this->game_.bytesPerLine() / 4);

    // Bright pass into a grid four times smaller
    const int bw = (w + 3) / 4;
    const int bh = (h + 3) / 4;
    const auto cells = static_cast<size_t>(bw * bh * 3);
    this->bloom_.assign(cells, 0);
    this->bloomTmp_.assign(cells, 0);
    constexpr int THRESHOLD = 120;
    for (int y = 0; y < h; ++y)
    {
        const Pix *row = bits + y * stride;
        int *cellRow = this->bloom_.data() + (y >> 2) * bw * 3;
        for (int x = 0; x < w; ++x)
        {
            const Pix c = row[x];
            const int r = static_cast<int>((c >> 16) & 255U);
            const int g = static_cast<int>((c >> 8) & 255U);
            const int b = static_cast<int>(c & 255U);
            const int m = std::max(r, std::max(g, b));
            if (m <= THRESHOLD)
            {
                continue;
            }
            const int k = m - THRESHOLD;
            int *cell = cellRow + (x >> 2) * 3;
            cell[0] += r * k;
            cell[1] += g * k;
            cell[2] += b * k;
        }
    }
    // Normalise, then blur twice with a [1 2 1] kernel
    const int norm = 16 * (255 - THRESHOLD);
    for (auto &v : this->bloom_)
    {
        v = v * 3 / norm;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int y = 0; y < bh; ++y)
        {
            for (int x = 0; x < bw; ++x)
            {
                const int xl = std::max(0, x - 1);
                const int xr = std::min(bw - 1, x + 1);
                for (int ch = 0; ch < 3; ++ch)
                {
                    this->bloomTmp_[static_cast<size_t>((y * bw + x) * 3 + ch)] =
                        (this->bloom_[static_cast<size_t>((y * bw + xl) * 3 + ch)] +
                         2 * this->bloom_[static_cast<size_t>((y * bw + x) * 3 + ch)] +
                         this->bloom_[static_cast<size_t>((y * bw + xr) * 3 + ch)]) /
                        4;
                }
            }
        }
        for (int y = 0; y < bh; ++y)
        {
            const int yu = std::max(0, y - 1);
            const int yd = std::min(bh - 1, y + 1);
            for (int x = 0; x < bw; ++x)
            {
                for (int ch = 0; ch < 3; ++ch)
                {
                    this->bloom_[static_cast<size_t>((y * bw + x) * 3 + ch)] =
                        (this->bloomTmp_[static_cast<size_t>((yu * bw + x) * 3 + ch)] +
                         2 * this->bloomTmp_[static_cast<size_t>((y * bw + x) * 3 + ch)] +
                         this->bloomTmp_[static_cast<size_t>((yd * bw + x) * 3 + ch)]) /
                        4;
                }
            }
        }
    }

    // A faint roll bar drifts down the tube
    std::vector<int> roll(static_cast<size_t>(h), 0);
    if (!state.reduced)
    {
        const double pos = (this->roll_ - 0.2) * h;
        const double width = h * 0.07;
        for (int y = 0; y < h; ++y)
        {
            const double d = (y - pos) / width;
            if (std::abs(d) < 1)
            {
                roll[static_cast<size_t>(y)] = ri((1 - d * d) * 3);
            }
        }
    }

    // Composite bloom, vignette, sheen and roll in one pass
    this->bloomRow_.assign(static_cast<size_t>(bw * 3), 0);
    for (int y = 0; y < h; ++y)
    {
        const double gy = (y + 0.5) / 4.0 - 0.5;
        const int y0 = std::clamp(fi(gy), 0, bh - 1);
        const int y1 = std::min(bh - 1, y0 + 1);
        const int fy = std::clamp(ri((gy - y0) * 256), 0, 256);
        for (int i = 0; i < bw * 3; ++i)
        {
            this->bloomRow_[static_cast<size_t>(i)] =
                (this->bloom_[static_cast<size_t>(y0 * bw * 3 + i)] * (256 - fy) +
                 this->bloom_[static_cast<size_t>(y1 * bw * 3 + i)] * fy) >>
                8;
        }
        Pix *row = bits + y * stride;
        const std::uint16_t *vig = this->vignette_.data() + y * w;
        const std::uint8_t *sheen = this->sheen_.data() + y * w;
        const int rollAdd = roll[static_cast<size_t>(y)];
        for (int x = 0; x < w; ++x)
        {
            const int cx0 = this->colX0_[static_cast<size_t>(x)];
            const int cx1 = std::min(bw - 1, cx0 + 1);
            const int fx = this->colF_[static_cast<size_t>(x)];
            const int *a = this->bloomRow_.data() + cx0 * 3;
            const int *b = this->bloomRow_.data() + cx1 * 3;
            const int br = (a[0] * (256 - fx) + b[0] * fx) >> 8;
            const int bg = (a[1] * (256 - fx) + b[1] * fx) >> 8;
            const int bb = (a[2] * (256 - fx) + b[2] * fx) >> 8;

            const Pix c = row[x];
            const int v = vig[x];
            const int s = sheen[x] + rollAdd;
            int r = static_cast<int>((c >> 16) & 255U) + br;
            int g = static_cast<int>((c >> 8) & 255U) + bg;
            int bl = static_cast<int>(c & 255U) + bb;
            r = ((r * v) >> 8) + (s * 7 >> 3);
            g = ((g * v) >> 8) + s;
            bl = ((bl * v) >> 8) + s;
            row[x] = 0xff000000U | (static_cast<Pix>(std::min(r, 255)) << 16) |
                     (static_cast<Pix>(std::min(g, 255)) << 8) |
                     static_cast<Pix>(std::min(bl, 255));
        }
    }
}

void ArcadeScene::expand(const QImage &src, QImage &dst, const QRect &region,
                         bool keepAlpha) const
{
    const int n = this->devPx_;
    const int sw = src.width();
    const int sh = src.height();
    const int x0 = std::max(0, region.left());
    const int x1 = std::min(sw - 1, region.right());
    if (x1 < x0)
    {
        return;
    }
    for (int r = std::max(0, region.top()); r <= std::min(sh - 1, region.bottom());
         ++r)
    {
        const int sr = std::clamp(r - this->kickY_, 0, sh - 1);
        const auto *srow = reinterpret_cast<const Pix *>(src.constScanLine(sr));
        auto *first = reinterpret_cast<Pix *>(dst.scanLine(r * n));
        for (int x = x0; x <= x1; ++x)
        {
            const Pix c = srow[std::clamp(x - this->kickX_, 0, sw - 1)];
            Pix *d = first + x * n;
            for (int k = 0; k < n; ++k)
            {
                d[k] = c;
            }
        }
        const size_t count = static_cast<size_t>((x1 - x0 + 1) * n);
        const Pix *from = first + x0 * n;
        for (int k = 1; k < n; ++k)
        {
            Pix *row = reinterpret_cast<Pix *>(dst.scanLine(r * n + k)) + x0 * n;
            const int f = this->scanFactor_[static_cast<size_t>(k)];
            if (f >= 256)
            {
                std::memcpy(row, from, count * sizeof(Pix));
                continue;
            }
            const auto kf = static_cast<Pix>(f);
            for (size_t i = 0; i < count; ++i)
            {
                const Pix c = from[i];
                const Pix rb = (((c & 0x00ff00ffU) * kf) >> 8) & 0x00ff00ffU;
                if (keepAlpha)
                {
                    const Pix ag = (((c >> 8) & 0x00ff00ffU) * kf) & 0xff00ff00U;
                    row[i] = rb | ag;
                }
                else
                {
                    const Pix g = (((c & 0x0000ff00U) * kf) >> 8) & 0x0000ff00U;
                    row[i] = 0xff000000U | rb | g;
                }
            }
        }
    }
}

void ArcadeScene::drawStars(Canvas &cv, double t) const
{
    const double hh = std::max(1.0, this->winH_);
    const int dust = static_cast<int>(this->dust_.size() *
                                      clamp01((this->viewerFactor_ - 0.3) / 0.7));
    for (int i = 0; i < dust; ++i)
    {
        const Star &d = this->dust_[static_cast<size_t>(i)];
        const double fy = std::fmod(d.v * hh + this->starScroll_[0], hh);
        cv.blend(fi(d.u * this->winW_), fi(fy), pal(d.ink), 150);
    }

    const int count =
        std::clamp(ri(this->starCount_), 0, static_cast<int>(this->stars_.size()));
    const double h = std::max(1.0, this->winH_);
    const double streak = this->warp_ * this->warp_;
    for (int i = 0; i < count; ++i)
    {
        const Star &s = this->stars_[static_cast<size_t>(i)];
        const int x = fi(s.u * this->winW_);
        const double fy =
            std::fmod(s.v * h + this->starScroll_[static_cast<size_t>(s.layer)], h);
        const int y = fi(fy);
        int alpha = s.layer == 0 ? 200 : 230;
        if (s.layer == 2 && !this->reduced_)
        {
            alpha = ri(150 + 106 * (0.5 + 0.5 * std::sin(t * 0.9 + s.phase)));
        }
        const Pix c = pal(s.ink);
        cv.blend(x, y, c, alpha);
        if (s.cross)
        {
            cv.blend(x - 1, y, c, alpha / 3);
            cv.blend(x + 1, y, c, alpha / 3);
            cv.blend(x, y - 1, c, alpha / 3);
            cv.blend(x, y + 1, c, alpha / 3);
        }
        if (streak > 0.02 && s.layer > 0)
        {
            const int length = ri(streak * (s.layer * 6 + 2));
            for (int k = 1; k <= length; ++k)
            {
                cv.blend(x, y - k, c, alpha * (length - k + 1) / (length + 2));
            }
        }
    }
}

void ArcadeScene::drawAmbient(Canvas &cv, double t) const
{
    // A satellite crosses the header strip every so often
    const double period = 48;
    const double phase = std::fmod(t + 20, period) / period;
    if (phase < 0.5 && !this->reduced_)
    {
        const int x = ri(lerp(-4, this->winW_ + 4, phase * 2));
        const int y = ri(std::max(2.0, this->top_.bottom() - 4));
        cv.blend(x - 1, y, pal(PAL_STEEL), 200);
        cv.blend(x, y, pal(PAL_PALE), 200);
        cv.blend(x + 1, y, pal(PAL_STEEL), 200);
        if (std::fmod(t, 1.6) < 0.3)
        {
            cv.blend(x, y - 1, pal(PAL_EMBER), 230);
        }
    }

    // Shooting star
    if (this->cometAge_ < 0.9)
    {
        const double u = this->cometAge_ / 0.9;
        const QPointF head =
            this->cometFrom_ + this->cometDir_ * (u * 70);
        for (int k = 0; k < 10; ++k)
        {
            const QPointF p = head - this->cometDir_ * (k * 1.0);
            const int a = ri((1 - k / 10.0) * 230 * std::sin(u * TAU / 2));
            cv.blend(ri(p.x()), ri(p.y()),
                     pal(k == 0 ? PAL_WHITE : (k < 4 ? PAL_PALE : PAL_STEEL)),
                     a);
        }
    }
}

void ArcadeScene::drawCity(Canvas &cv, double t) const
{
    const int w = this->gw_;
    const int h = this->gh_;
    const int dy = ri(easeInOutSine(this->cityRise_) * (h - this->cityTop_));
    for (int y = this->cityTop_; y < h; ++y)
    {
        const int ty = y + dy;
        if (ty >= h)
        {
            break;
        }
        const Pix *row = this->city_.data() + static_cast<size_t>(y * w);
        for (int x = 0; x < w; ++x)
        {
            if (row[x] != 0)
            {
                cv.put(x, ty, row[x]);
            }
        }
    }

    // Lit windows: more viewers, more lights; a few change every so often
    for (size_t i = 0; i < this->windows_.size(); ++i)
    {
        const CityWindow &win = this->windows_[i];
        double thr = win.threshold;
        if (!this->reduced_)
        {
            const int slot = fi(t / 11.0 + win.threshold * 11);
            thr += 0.12 * (hash01(static_cast<int>(i), slot, 3) - 0.5);
        }
        if (thr < this->litShown_)
        {
            cv.put(win.x, win.y + dy, shade(pal(win.ink), win.bright));
        }
    }

    for (size_t i = 0; i < this->neons_.size(); ++i)
    {
        const Neon &n = this->neons_[i];
        const bool off = !this->reduced_ &&
                         hash01(n.seed, fi(t * 1.3), 9) < 0.03;
        if (off || !this->live_)
        {
            continue;
        }
        for (int k = 0; k < n.length; ++k)
        {
            cv.put(n.x, n.y + k + dy, shade(pal(n.ink), 200));
        }
    }

    for (const auto &b : this->beacons_)
    {
        const bool on = this->reduced_ || std::fmod(t + b.phase * 2, 2.0) < 0.4;
        if (on)
        {
            cv.put(b.x, b.y + dy, pal(PAL_EMBER));
            cv.add(b.x - 1, b.y + dy, pal(PAL_EMBER), 60);
            cv.add(b.x + 1, b.y + dy, pal(PAL_EMBER), 60);
        }
    }
}

void ArcadeScene::drawEnemies(Canvas &cv, double t) const
{
    const auto &b = book();
    for (const auto &e : this->enemies_)
    {
        if (e.dead || e.hidden)
        {
            continue;
        }
        const double rate = this->reduced_ ? 0.5 : 2.0;
        const int frame = (fi(t * rate) + e.phase) & 1;
        const Sprite &s = b.enemy[static_cast<size_t>(e.type)][static_cast<size_t>(frame)];
        Look look;
        look.core = e.core;
        look.flash = e.flash > 0 ? 220 : 0;
        if (this->reduced_ && e.entry < 1)
        {
            look.alpha = ri(e.entry * 256);
        }
        if (e.erase >= 0)
        {
            look.erased = ri(clamp01(e.erase) * s.h);
        }
        const int x = ri(e.x);
        const int y = ri(e.y);
        drawSprite(cv, s, x, y, look);
        if (e.erase >= 0 && e.erase < 1)
        {
            // The mod laser's scan line
            const int sy = y - s.h / 2 + look.erased;
            for (int k = -s.w / 2 - 1; k <= s.w / 2 + 1; ++k)
            {
                cv.blend(x + k, sy, pal(PAL_EMBER), 230);
            }
        }
        if (e.newTag > 0 && e.entry > 0.4)
        {
            const bool visible =
                this->reduced_ || std::fmod(e.newTag, 1.0) > 0.3;
            if (visible)
            {
                drawTextCentered(cv, QStringLiteral("NEW"), x,
                                 y - s.h / 2 - 7, pal(PAL_AMBER));
            }
        }
    }
}

void ArcadeScene::drawCombo(Canvas &cv, double t) const
{
    const Combo &c = this->combo_;
    if (!c.active)
    {
        return;
    }
    const double holdEnd = c.life - 0.7;
    for (const auto &d : c.dots)
    {
        QPointF back = d.from;
        if (d.enemy >= 0)
        {
            for (const auto &e : this->enemies_)
            {
                if (e.id == d.enemy)
                {
                    back = this->slotPos(e.row, e.col);
                }
            }
        }
        QPointF p;
        int alpha = 256;
        if (c.age < holdEnd)
        {
            const double u = easeOutCubic((c.age - d.delay) / 0.75);
            p = d.from + (d.to - d.from) * u;
        }
        else
        {
            const double u = easeInOutSine((c.age - holdEnd) / 0.7);
            p = d.to + (back - d.to) * u;
            if (d.enemy < 0)
            {
                alpha = ri(256 * (1 - u));
            }
        }
        const bool even = d.glyph % 2 == 0;
        std::uint8_t ink = even ? PAL_CYAN : PAL_MAGENTA;
        if (c.amount >= 8 && !this->reduced_)
        {
            constexpr std::array<std::uint8_t, 4> wave{PAL_CYAN, PAL_WHITE,
                                                       PAL_MAGENTA, PAL_AMBER};
            ink = wave[static_cast<size_t>(
                (fi(d.to.x() / 6.0 - t * 4) % 4 + 4) % 4)];
        }
        const int pulse =
            this->reduced_ ? 256
                           : 200 + ri(56 * std::sin(t * 3 + d.to.x() * 0.2));
        const Pix col = shade(pal(ink), pulse);
        const int x = ri(p.x());
        const int y = ri(p.y());
        // While flying the tile is a single pixel; it unfolds in place
        const bool settled = c.age - d.delay > 0.6 && c.age < holdEnd + 0.1;
        const int size = settled ? c.dot : 1;
        for (int ty = 0; ty < size; ++ty)
        {
            for (int tx = 0; tx < size; ++tx)
            {
                Pix tile = col;
                if (tx == size - 1 || ty == size - 1)
                {
                    tile = shade(col, 150);
                }
                if (tx == 0 && ty == 0 && size > 1)
                {
                    tile = mixPix(col, pal(PAL_WHITE), 150);
                }
                cv.blend(x + tx, y + ty, tile, alpha);
            }
        }
    }
    if (c.age > 0.7 && c.age < holdEnd)
    {
        drawTextCentered(cv,
                         QStringLiteral("COMBO *") + QString::number(c.amount),
                         ri(c.label.x()), ri(c.label.y()), pal(PAL_AMBER));
    }
}

void ArcadeScene::drawEscort(Canvas &cv, double t) const
{
    const double y = this->arena_.top() + this->arena_.height() * 0.86 +
                     (this->reduced_ ? 0 : std::round(std::sin(t * 1.3)));
    const int x = ri(this->escortX_);
    drawSprite(cv, book().escort, x, ri(y));
    if (std::fmod(t, 1.2) < 0.25)
    {
        cv.put(x, ri(y) - 3, pal(PAL_WHITE));
    }
    // Thruster
    const int flame = 1 + static_cast<int>(hash01(fi(t * 20), 4) * 2);
    for (int k = 0; k < flame; ++k)
    {
        cv.blend(x, ri(y) + 2 + k, pal(k == 0 ? PAL_AMBER : PAL_EMBER), 200);
    }
}

void ArcadeScene::drawShots(Canvas &cv) const
{
    for (const auto &s : this->shots_)
    {
        const double v = std::max(1.0, std::hypot(s.vx, s.vy));
        const double ux = s.vx / v;
        const double uy = s.vy / v;
        const int length = s.kind == SHOT_TRACER ? 6 : (s.kind == SHOT_CLICK ? 6 : 3);
        if (s.kind == SHOT_CLICK)
        {
            drawLight(cv, s.x + 0.5, s.y + 0.5, 3.5, pal(PAL_AMBER), 120);
        }
        for (int k = 0; k < length; ++k)
        {
            const int x = ri(s.x - ux * k);
            const int y = ri(s.y - uy * k);
            const Pix c = k == 0 ? pal(PAL_WHITE) : pal(s.ink);
            cv.blend(x, y, c, 256 - k * 256 / (length + 1));
        }
    }
}

void ArcadeScene::drawPickups(Canvas &cv, double t) const
{
    const auto &b = book();
    for (const auto &c : this->capsules_)
    {
        if (c.delay > 0)
        {
            continue;
        }
        const Sprite &s =
            c.big ? b.capsuleBig
                  : b.capsule[static_cast<size_t>(fi(t * 3 + c.phase) & 1)];
        drawSprite(cv, s, ri(c.x), ri(c.y));
    }
    for (const auto &c : this->coins_)
    {
        if (c.delay > 0)
        {
            continue;
        }
        const int frame =
            this->reduced_ ? 0 : (fi(t * 10 + c.phase) % 4 + 4) % 4;
        drawSprite(cv, b.coin[static_cast<size_t>(frame)], ri(c.x), ri(c.y));
    }
}

void ArcadeScene::drawParticles(Canvas &cv, bool front) const
{
    for (const auto &p : this->particles_)
    {
        if (front && !p.front)
        {
            continue;
        }
        const double u = clamp01(p.age / p.life);
        const int x = ri(p.x);
        const int y = ri(p.y);
        switch (p.kind)
        {
            case PK_FIRE: {
                const Pix c = pal(fireInk(u));
                cv.blend(x, y, c, 256);
                if (p.big && u < 0.45)
                {
                    cv.blend(x + 1, y, c, 256);
                    cv.blend(x, y + 1, c, 256);
                    cv.blend(x + 1, y + 1, c, 256);
                }
            }
            break;
            case PK_SPARK: {
                const Pix c = pal(u < 0.4 ? PAL_WHITE : PAL_AMBER);
                const double v = std::max(1.0, std::hypot(p.vx, p.vy));
                cv.blend(x, y, c, 256);
                cv.blend(ri(p.x - p.vx / v * 1.5), ri(p.y - p.vy / v * 1.5),
                         pal(PAL_EMBER), 200);
            }
            break;
            case PK_DEBRIS: {
                Pix c = pal(p.ink);
                if (u > 0.55)
                {
                    c = mixPix(c, pal(PAL_PLUM), ri((u - 0.55) / 0.45 * 256));
                }
                const int a = u > 0.8 ? ri((1 - u) / 0.2 * 256) : 256;
                cv.blend(x, y, c, a);
            }
            break;
            case PK_SMOKE:
                cv.blend(x, y, pal(p.ink), ri(180 * (1 - u)));
                break;
            case PK_GATHER:
                cv.blend(x, y, pal(p.ink), 256);
                break;
            case PK_TWINKLE: {
                const Pix c = pal(p.ink);
                const int arm = u < 0.5 ? 2 : 1;
                const int a = ri(256 * (1 - u * u));
                cv.blend(x, y, pal(PAL_WHITE), a);
                for (int k = 1; k <= arm; ++k)
                {
                    cv.blend(x - k, y, c, a / k);
                    cv.blend(x + k, y, c, a / k);
                    cv.blend(x, y - k, c, a / k);
                    cv.blend(x, y + k, c, a / k);
                }
            }
            break;
            case PK_ASH:
                cv.blend(x, y, pal(p.ink), ri(220 * (1 - u)));
                break;
        }
    }
}

void ArcadeScene::drawRings(Canvas &cv, bool front) const
{
    for (const auto &r : this->rings_)
    {
        if ((front && !r.front) || r.age < 0)
        {
            continue;
        }
        const double u = clamp01(r.age / r.life);
        const int radius = ri(lerp(r.r0, r.r1, easeOutCubic(u)));
        const Pix c = pal(fireInk(u * 0.9));
        const int seed = fi(r.x * 7 + r.y * 13);
        walkCircle(ri(r.x), ri(r.y), radius, [&](int x, int y) {
            // The ring dissolves as it grows
            if (hash01(x, y, seed) > u * 1.1)
            {
                cv.blend(x, y, c, 230);
            }
        });
    }
}

void ArcadeScene::drawAfterglows(Canvas &cv) const
{
    for (const auto &g : this->afterglows_)
    {
        const double u = clamp01(g.age / g.life);
        // Hot at first, then a dim plum stain that dithers away
        const Pix c = pal(u < 0.15 ? PAL_EMBER : PAL_PLUM);
        const int r = static_cast<int>(std::ceil(g.radius));
        const int cx = ri(g.x);
        const int cy = ri(g.y);
        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                const double d = std::hypot(dx, dy) / g.radius;
                if (d >= 1 || bayer(cx + dx, cy + dy) < u)
                {
                    continue;
                }
                cv.add(cx + dx, cy + dy, c, ri((1 - d) * (1 - u) * 150));
            }
        }
    }
}

void ArcadeScene::drawPopups(Canvas &cv, bool front) const
{
    for (const auto &p : this->popups_)
    {
        if (front && !p.front)
        {
            continue;
        }
        const double u = clamp01(p.age / p.life);
        const int a = u > 0.75 ? ri((1 - u) / 0.25 * 256) : 256;
        const int y = ri(p.y - p.rise * easeOutCubic(u));
        // Score popups pop in a size larger for two frames
        const int scale = (p.scale > 1 && p.age < 0.07 && !this->reduced_)
                              ? p.scale + 1
                              : p.scale;
        drawTextCentered(cv, p.text, ri(p.x), y - scale * 2, pal(p.ink), scale,
                         a);
    }
}

QRectF bannerRegion(const QRectF &arena, const QRectF &top, bool hasArena)
{
    if (hasArena)
    {
        return arena;
    }
    return QRectF(top.left() + top.width() * 0.3, top.top(), top.width() * 0.4,
                  std::max(14.0, top.height()));
}

void ArcadeScene::drawBanners(Canvas &cv, double t) const
{
    if (this->banners_.empty())
    {
        return;
    }
    const Banner &b = this->banners_.front();
    const QRectF region = bannerRegion(this->arena_, this->top_, this->hasArena_);
    const int cx = ri(region.center().x());

    const int topScale = textWidth(b.top, 2) <= region.width() - 6 ? 2 : 1;
    const int bottomScale =
        textWidth(b.bottom, 2) <= region.width() - 6 && topScale == 2 &&
                b.bottom.size() <= 8
            ? 2
            : 1;
    const int topH = 5 * topScale;
    const int bottomH = b.bottom.isEmpty() ? 0 : 5 * bottomScale + 3;
    const int height = topH + bottomH + 8;
    const int y0 = this->hasArena_
                       ? ri(region.top() + region.height() * 0.36) - height / 2
                       : ri(region.center().y()) - height / 2;

    // Slide in from the right edge of its own region, then dissolve away.
    // Nothing ever travels over chat.
    double offset = 0;
    const int alpha = 256;
    const double outro = clamp01((b.age - (b.life - 0.3)) / 0.3);
    double dissolve = outro;
    if (this->reduced_)
    {
        dissolve = std::max(outro, 1 - clamp01(b.age / 0.3));
    }
    else if (b.age < 0.25)
    {
        offset = (1 - easeOutCubic(b.age / 0.25)) * region.width();
    }
    const int ox = ri(offset);
    cv.setClip(QRect(ri(region.left()), 0, ri(region.width()), this->gh_));
    cv.setDissolve(dissolve);

    // Dark strip behind the text
    const int left = ri(region.left()) + ox;
    const int width = ri(region.width());
    cv.fill(left, y0, width, height, pal(PAL_VOID), ri(alpha * 0.82));
    for (int x = left; x < left + width; ++x)
    {
        cv.blend(x, y0, pal(b.ink), alpha / 2);
        cv.blend(x, y0 + height - 1, pal(b.ink), alpha / 2);
    }

    if (b.style == BANNER_WARNING)
    {
        // Hazard stripes above and below
        const int shift = this->reduced_ ? 0 : fi(t * 8);
        for (int k = 0; k < 3; ++k)
        {
            for (int x = left; x < left + width; ++x)
            {
                const bool on = ((x + k + shift) / 3) % 2 == 0;
                const Pix c = pal(on ? PAL_AMBER : PAL_VOID);
                cv.blend(x, y0 - 4 + k, c, alpha);
                cv.blend(x, y0 + height + 1 + k, c, alpha);
            }
        }
    }
    else if (b.style == BANNER_BONUS)
    {
        // Marquee bulbs chase around the frame
        const int shift = this->reduced_ ? 0 : fi(t * 6);
        int i = 0;
        for (int x = left; x < left + width; x += 2, ++i)
        {
            if ((i + shift) % 3 == 0)
            {
                cv.blend(x, y0 - 2, pal(PAL_AMBER), alpha);
                cv.blend(x, y0 + height + 1, pal(PAL_AMBER), alpha);
            }
        }
    }
    else if (b.style == BANNER_STAGE && !this->reduced_)
    {
        // A scan wipe sweeps once across the strip
        const int sx = left + ri(clamp01(b.age / 0.8) * width);
        for (int y = y0; y < y0 + height; ++y)
        {
            cv.blend(sx, y, pal(PAL_CYAN), alpha / 2);
        }
    }

    Pix topInk = pal(b.ink);
    if (b.style == BANNER_WARNING && !this->reduced_)
    {
        // Glows between ember and amber, about once a second
        topInk = mixPix(pal(PAL_EMBER), pal(PAL_AMBER),
                        ri(128 + 128 * std::sin(b.age * TAU)));
    }
    drawTextCentered(cv, b.top, cx + ox, y0 + 4, topInk, topScale, alpha);
    if (!b.bottom.isEmpty())
    {
        drawTextCentered(cv, b.bottom, cx + ox, y0 + 4 + topH + 3, pal(b.ink2),
                         bottomScale, alpha);
    }
    cv.setClip(QRect());
    cv.setDissolve(0);
}

void ArcadeScene::drawAttract(Canvas &cv, double t) const
{
    if (this->live_)
    {
        return;
    }
    const QRectF region = bannerRegion(this->arena_, this->top_, this->hasArena_);
    const int cx = ri(region.center().x());
    const int scale = textWidth(QStringLiteral("GAME OVER"), 2) <=
                              region.width() - 4
                          ? 2
                          : 1;
    const int y = this->hasArena_ ? ri(region.top() + region.height() * 0.42)
                                  : ri(region.center().y() - 5);
    drawTextCentered(cv, QStringLiteral("GAME OVER"), cx, y, pal(PAL_MAGENTA),
                     scale);
    // Slow, smooth blink: never a strobe
    const double pulse =
        this->reduced_ ? 1.0 : 0.25 + 0.75 * (0.5 + 0.5 * std::cos(t * 2.6));
    drawTextCentered(cv, QStringLiteral("INSERT COIN"), cx, y + 5 * scale + 6,
                     pal(PAL_PALE), 1, ri(256 * pulse));
}

template <typename F>
void ArcadeScene::forEachBossPixel(double t, F &&f) const
{
    const Boss &b = this->boss_;
    const double s = b.scale;
    int cx = ri(b.x);
    int cy = ri(b.y);
    if (b.phase == BOSS_DYING && !this->reduced_)
    {
        cx += ri(std::sin(t * 61) * 1.2);
        cy += ri(std::cos(t * 47) * 1.0);
    }
    const int hullRx = ri(15 * s);
    const int hullRy = ri(9 * s);

    // Arm pods with spinning blades
    for (int side = -1; side <= 1; side += 2)
    {
        const bool alive = side < 0 ? b.podLeft : b.podRight;
        const int px = cx + side * ri(21 * s);
        const int py = cy - ri(s);
        const int pr = std::max(2, ri(4 * s));
        for (int k = ri(hullRx * 0.75); k <= std::abs(px - cx) - pr; ++k)
        {
            f(cx + side * k, cy - 1, PAL_STEEL);
            f(cx + side * k, cy, PAL_DUSK);
        }
        if (!alive)
        {
            f(px - side * pr, py, PAL_EMBER);
            continue;
        }
        for (int dy = -pr; dy <= pr; ++dy)
        {
            for (int dx = -pr; dx <= pr; ++dx)
            {
                const int d2 = dx * dx + dy * dy;
                if (d2 > pr * pr)
                {
                    continue;
                }
                f(px + dx, py + dy,
                  d2 > (pr - 1) * (pr - 1) ? PAL_STEEL : PAL_NAVY);
            }
        }
        const double a = (this->reduced_ ? 0.3 : t * 3.0) * side;
        for (int arm = 0; arm < 2; ++arm)
        {
            const double aa = a + arm * TAU / 4;
            for (int k = -(pr - 1); k <= pr - 1; ++k)
            {
                f(px + ri(std::cos(aa) * k), py + ri(std::sin(aa) * k),
                  std::abs(k) == pr - 1 ? PAL_CYAN : PAL_PALE);
            }
        }
    }

    // Domed carapace
    for (int dy = -hullRy; dy <= 0; ++dy)
    {
        const double fy = double(dy) / hullRy;
        const int half = ri(hullRx * std::sqrt(std::max(0.0, 1 - fy * fy)));
        for (int dx = -half; dx <= half; ++dx)
        {
            std::uint8_t ink = PAL_PLUM;
            const double side = std::abs(dx) / std::max(1.0, double(half));
            if (std::abs(dx) == half || dy == -hullRy)
            {
                ink = PAL_MAGENTA;
            }
            else if ((dy + hullRy) % 3 == 2)
            {
                ink = PAL_NAVY;
            }
            else if (bayer(cx + dx, cy + dy) < side * side * 0.8)
            {
                ink = PAL_NAVY;
            }
            else if (dy < -hullRy + 3 && dx < 0 && dx > -half + 2)
            {
                ink = PAL_MAGENTA;
            }
            f(cx + dx, cy + dy, ink);
        }
    }
    // Skirt with running lights
    const int skirt = std::max(2, ri(3 * s));
    for (int dy = 1; dy <= skirt; ++dy)
    {
        const int half = hullRx - ri(dy * 2.2);
        for (int dx = -half; dx <= half; ++dx)
        {
            std::uint8_t ink = std::abs(dx) == half ? PAL_STEEL : PAL_DUSK;
            if (dy == 1 && dx % 4 == 0)
            {
                const int seq = (dx / 4 + fi(t * 6)) % 3;
                ink = (seq == 0 && !this->reduced_) ? PAL_AMBER : PAL_TEAL;
            }
            f(cx + dx, cy + dy, ink);
        }
    }

    // Crown spikes
    for (int i = 0; i < 5; ++i)
    {
        const double a = TAU / 2 + TAU / 2 * (0.15 + 0.7 * i / 4.0);
        const int bx = cx + ri(std::cos(a) * hullRx * 0.85);
        const int by = cy + ri(std::sin(a) * hullRy * 0.95);
        const int length = ri((i == 2 ? 4 : 3) * s);
        for (int k = 1; k <= length; ++k)
        {
            f(bx + ri(std::cos(a) * k * 0.6), by - k,
              k == length ? PAL_AMBER : PAL_STEEL);
        }
    }

    // The eye tracks your ship
    const int coreY = cy + ri(s);
    const int rc = std::max(3, ri(4.5 * s));
    const QPointF ship = this->shipPos();
    const double ex = ship.x() - cx;
    const double ey = ship.y() - coreY;
    const double ed = std::max(1.0, std::hypot(ex, ey));
    const int pupX = cx + ri(ex / ed * rc * 0.45);
    const int pupY = coreY + ri(ey / ed * rc * 0.45);
    const bool hurt = b.hp < b.maxHp * 0.33;
    for (int dy = -rc; dy <= rc; ++dy)
    {
        for (int dx = -rc; dx <= rc; ++dx)
        {
            const int d2 = dx * dx + dy * dy;
            if (d2 > rc * rc)
            {
                continue;
            }
            std::uint8_t ink = PAL_CYAN;
            if (d2 > (rc - 1) * (rc - 1))
            {
                ink = PAL_STEEL;
            }
            else if (d2 < rc * rc / 4)
            {
                ink = hurt ? PAL_AMBER : PAL_WHITE;
            }
            else if (hurt)
            {
                ink = PAL_EMBER;
            }
            f(cx + dx, coreY + dy, ink);
        }
    }
    for (int dy = 0; dy < 2; ++dy)
    {
        for (int dx = 0; dx < 2; ++dx)
        {
            f(pupX + dx - 1, pupY + dy - 1, PAL_VOID);
        }
    }

    // Tendrils
    const int tendrilLength = ri(8 * s);
    for (int i = 0; i < 4; ++i)
    {
        const int bx = cx + ri((-9 + i * 6) * s);
        for (int k = 0; k < tendrilLength; ++k)
        {
            const double sway =
                this->reduced_
                    ? 0
                    : std::sin(t * 2 + i * 1.3 + k * 0.45) * k * 0.2;
            f(bx + ri(sway), cy + skirt + 1 + k,
              k < tendrilLength / 2 ? PAL_TEAL : PAL_DUSK);
        }
    }
}

void ArcadeScene::drawBoss(Canvas &cv, double t) const
{
    const Boss &b = this->boss_;
    if (!b.active || b.phase == BOSS_WARNING ||
        (b.phase == BOSS_DYING && b.finalBlast))
    {
        return;
    }
    const bool flash = b.hitFlash > 0 && !this->reduced_;
    const double hx = b.hitX;
    const double hy = b.hitY;
    this->forEachBossPixel(t, [&](int x, int y, std::uint8_t ink) {
        Pix c = pal(ink);
        if (flash && ink != PAL_VOID)
        {
            const double d = std::hypot(x - hx, y - hy);
            if (d < 5)
            {
                c = mixPix(c, pal(PAL_WHITE), ri((1 - d / 5) * 200));
            }
        }
        cv.put(x, y, c);
    });
    if (b.shield > 0)
    {
        const int r = ri(26 * b.scale);
        const int shift = this->reduced_ ? 0 : fi(t * 12);
        walkCircle(ri(b.x), ri(b.y), r, [&](int x, int y) {
            if (((x + y + shift) & 3) == 0)
            {
                cv.blend(x, y, pal(PAL_CYAN), 200);
            }
        });
    }
}

void ArcadeScene::drawBossBar(Canvas &cv, double t) const
{
    const Boss &b = this->boss_;
    if (!b.active || b.phase == BOSS_WARNING)
    {
        return;
    }
    QRectF area = this->hudInHeader_ ? QRectF() : this->arenaHeader_;
    if (area.isEmpty() && this->hudInHeader_)
    {
        area = QRectF(this->arena_.left(), this->arena_.top() + 1,
                      this->arena_.width(), 11);
    }
    else if (area.isEmpty())
    {
        // No header over the field: use the strip under the tabs, clear of
        // the HUD and of the header's own text at both ends
        const double stripTop = this->tabs_.bottom();
        const double stripHeight = this->top_.bottom() - stripTop;
        if (stripHeight >= 9)
        {
            area = QRectF(this->arena_.left() + this->arena_.width() * 0.05,
                          stripTop, this->arena_.width() * 0.7, stripHeight);
        }
        else
        {
            area = QRectF(this->arena_.left(), this->top_.bottom() + 1,
                          this->arena_.width(), 11);
        }
    }
    const int left = ri(area.left() + std::max(30.0, area.width() * 0.3));
    const int right = ri(area.right()) - 4;
    if (right - left < 24)
    {
        return;
    }
    const int textY = ri(area.center().y()) - 4;
    const int barY = textY + 7;
    const QString hp = QString::number(static_cast<int>(std::ceil(b.hp)));
    const int hpW = textWidth(hp, 1);
    drawText(cv, QStringLiteral("BOSS"), left, textY, pal(PAL_EMBER));
    const int nameX = left + 18;
    const int nameRoom = right - hpW - 4 - nameX;
    if (nameRoom > 8)
    {
        const int chars = std::max(0, (nameRoom + 1) / 4);
        drawText(cv, b.name.left(chars), nameX, textY, pal(PAL_PALE));
    }
    drawText(cv, hp, right - hpW, textY, pal(PAL_WHITE));

    const int width = right - left;
    const int fill = ri(width * clamp01(b.hp / b.maxHp));
    const int lag = ri(width * clamp01(b.lagHp / b.maxHp));
    for (int x = 0; x < width; ++x)
    {
        std::uint8_t ink = PAL_NAVY;
        if (x < fill)
        {
            ink = x < width / 3 ? PAL_EMBER : PAL_MAGENTA;
        }
        else if (x < lag)
        {
            ink = PAL_WHITE;
        }
        cv.put(left + x, barY, pal(ink));
        cv.put(left + x, barY + 1, shade(pal(ink), x < fill ? 190 : 256));
    }
    cv.put(left - 1, barY, pal(PAL_STEEL));
    cv.put(left - 1, barY + 1, pal(PAL_STEEL));
    cv.put(right, barY, pal(PAL_STEEL));
    cv.put(right, barY + 1, pal(PAL_STEEL));
    (void)t;
}

void ArcadeScene::drawModLaser(Canvas &cv, double t) const
{
    const ModLaser &l = this->laser_;
    if (!l.active)
    {
        return;
    }
    const int x = ri(l.tx);
    // The satellite drops in, fires, and pulls back up
    double satY = l.satY;
    if (l.age < 0.35)
    {
        satY = lerp(-6, l.satY, easeOutCubic(l.age / 0.35));
    }
    else if (l.age > 1.6)
    {
        satY = lerp(l.satY, -6, clamp01((l.age - 1.6) / 0.4));
    }
    drawSprite(cv, book().satellite, x, ri(satY));

    double reach = 0;
    if (l.age >= 0.4 && l.age < 0.8)
    {
        reach = easeInOutSine((l.age - 0.4) / 0.4);
    }
    else if (l.age >= 0.8 && l.age < 1.25)
    {
        reach = 1;
    }
    else if (l.age >= 1.25 && l.age < 1.5)
    {
        reach = 1 - (l.age - 1.25) / 0.25;
    }
    if (reach <= 0)
    {
        return;
    }
    const int y0 = ri(satY) + 3;
    const int y1 = y0 + ri((l.ty - y0) * reach);
    const int wobble = this->reduced_ ? 0 : (fi(t * 30) & 1);
    for (int y = y0; y <= y1; ++y)
    {
        cv.blend(x, y, pal(PAL_WHITE), 256);
        cv.blend(x - 1, y, pal(PAL_EMBER), 230);
        cv.blend(x + 1, y, pal(PAL_EMBER), 230);
        if (wobble != 0)
        {
            cv.blend(x - 2, y, pal(PAL_PLUM), 160);
            cv.blend(x + 2, y, pal(PAL_PLUM), 160);
        }
    }
    // Sweep line at the tip
    for (int k = -6; k <= 6; ++k)
    {
        cv.blend(x + k, y1, pal(PAL_EMBER), 220 - std::abs(k) * 25);
    }
}

void ArcadeScene::drawStamps(Canvas &cv, bool front) const
{
    for (const auto &s : this->stamps_)
    {
        if (front && s.age >= 1.4)
        {
            continue;
        }
        const double u = s.age / s.life;
        int alpha = 256;
        if (u > 0.8)
        {
            alpha = ri((1 - u) / 0.2 * 256);
        }
        const int scale = (s.age < 0.08 && !this->reduced_) ? 2 : 1;
        const QString text = QStringLiteral("BANNED");
        const int tw = textWidth(text, scale);
        const int w = tw + 6 * scale;
        const int h = 5 * scale + 6;
        const int x0 = ri(s.x) - w / 2;
        const int y0 = ri(s.y) - h / 2;
        cv.fill(x0, y0, w, h, pal(PAL_VOID), alpha * 3 / 4);
        for (int x = x0; x < x0 + w; ++x)
        {
            cv.blend(x, y0, pal(PAL_EMBER), alpha);
            cv.blend(x, y0 + h - 1, pal(PAL_EMBER), alpha);
        }
        for (int y = y0; y < y0 + h; ++y)
        {
            cv.blend(x0, y, pal(PAL_EMBER), alpha);
            cv.blend(x0 + w - 1, y, pal(PAL_EMBER), alpha);
        }
        drawText(cv, text, x0 + 3 * scale, y0 + 3, pal(PAL_WHITE), scale,
                 alpha);
        // Ink specks around the stamp
        for (int i = 0; i < 6; ++i)
        {
            const int sx = x0 - 2 + static_cast<int>(hash01(i, ri(s.x), 1) * (w + 4));
            const int sy = y0 - 2 + static_cast<int>(hash01(i, ri(s.y), 2) * (h + 4));
            if (sx < x0 || sx >= x0 + w || sy < y0 || sy >= y0 + h)
            {
                cv.blend(sx, sy, pal(PAL_EMBER), alpha / 2);
            }
        }
    }
}

void ArcadeScene::drawPlayer2(Canvas &cv, double t, bool front) const
{
    const Player2 &p = this->p2_;
    if (!p.active)
    {
        return;
    }
    const QRectF &row = p.row;
    const double startX = row.right() - 8;
    const double endX = row.left() + 10;
    const double cy = row.center().y();

    if (front)
    {
        // "2UP" lights up where Player 2 joined, once the ship has left it
        if (p.age > 0.35 && p.age < 2.2)
        {
            const int a = p.age > 1.8 ? ri((2.2 - p.age) / 0.4 * 256) : 256;
            drawTextOutlined(cv, QStringLiteral("2UP"), ri(startX) - 4,
                             ri(cy) - 2, pal(PAL_CYAN), 1, a);
        }
        return;
    }

    double x = startX;
    double y = cy;
    int alpha = 256;
    if (p.age < 0.3)
    {
        // Warp-in: a vertical slit that opens into the ship
        const double u = p.age / 0.3;
        const int half = ri((1 - u) * 6);
        for (int k = -half; k <= half; ++k)
        {
            cv.blend(ri(x), ri(y) + k, pal(PAL_CYAN), 220);
        }
        alpha = ri(u * 256);
    }
    else if (p.age < 2.6)
    {
        x = lerp(startX, endX, easeInOutSine((p.age - 0.3) / 2.3));
        if (!this->reduced_)
        {
            y += std::round(std::sin(p.age * 6) * 0.6);
        }
    }
    else
    {
        const double u = clamp01((p.age - 2.6) / 0.8);
        x = endX - u * 10;
        y = cy - u * u * 40;
        alpha = ri((1 - u) * 256);
    }

    // Trail along the bottom of the row
    const double trailFade = clamp01(1 - (p.age - 0.3) / 3.1);
    if (p.age > 0.3)
    {
        const int y1 = ri(row.bottom()) - 1;
        for (int tx = ri(x) + 6; tx <= ri(startX); ++tx)
        {
            const double along = (tx - x) / std::max(1.0, startX - x);
            const int a = ri(200 * trailFade * (1 - along * 0.7));
            if (((tx + fi(t * 20)) & 1) == 0 || this->reduced_)
            {
                cv.blend(tx, y1, pal(PAL_CYAN), a);
            }
        }
    }

    Look look;
    look.core = PAL_WHITE;
    look.alpha = alpha;
    drawSprite(cv, book().p2, ri(x), ri(y), look);
    // Engine on the right
    const int flame = 1 + static_cast<int>(hash01(fi(t * 20), 8) * 3);
    for (int k = 0; k < flame; ++k)
    {
        cv.blend(ri(x) + 6 + k, ri(y), pal(k == 0 ? PAL_WHITE : PAL_CYAN),
                 alpha * (flame - k) / flame);
    }
}

void ArcadeScene::drawHover(Canvas &cv, double t, bool front) const
{
    const HoverLock &h = this->hover_;
    if (!h.active)
    {
        return;
    }
    if (front && h.age >= 1.2)
    {
        return;
    }
    const int cx = ri(h.row.right() - 7);
    const int cy = ri(h.row.center().y());
    double d = lerp(9, 3, easeOutCubic(h.age / 0.3));
    int alpha = 256;
    if (h.leaving)
    {
        d += h.leaveAge * 30;
        alpha = ri(256 * clamp01(1 - h.leaveAge / 0.25));
    }
    if (front && h.age > 0.9)
    {
        alpha = std::min(alpha, ri((1.2 - h.age) / 0.3 * 256));
    }
    const bool locked = h.age > 0.3;
    const Pix c = pal(locked ? PAL_EMBER : PAL_AMBER);
    const int k = ri(d);
    for (int sx = -1; sx <= 1; sx += 2)
    {
        for (int sy = -1; sy <= 1; sy += 2)
        {
            const int x = cx + sx * k;
            const int y = cy + sy * k;
            cv.blend(x, y, c, alpha);
            cv.blend(x - sx, y, c, alpha);
            cv.blend(x, y - sy, c, alpha);
        }
    }
    if (locked && (this->reduced_ || std::fmod(t, 1.0) < 0.6))
    {
        cv.blend(cx, cy, pal(PAL_WHITE), alpha);
    }
}

void ArcadeScene::drawCrosshair(Canvas &cv, double t) const
{
    if (!this->pointer_)
    {
        return;
    }
    const QPointF p = this->toGame(*this->pointer_);
    const int x = ri(p.x());
    const int y = ri(p.y());
    bool onTarget = false;
    for (const auto &e : this->enemies_)
    {
        if (!e.dead && !e.hidden && std::hypot(e.x - p.x(), e.y - p.y()) < 7)
        {
            onTarget = true;
        }
    }
    const Pix c = pal(onTarget ? PAL_AMBER : PAL_PALE);
    const int a = onTarget ? 256 : 190;
    const int gap = onTarget && !this->reduced_ ? 2 + (fi(t * 4) & 1) : 2;
    for (int k = gap; k <= gap + 1; ++k)
    {
        cv.blend(x - k, y, c, a);
        cv.blend(x + k, y, c, a);
        cv.blend(x, y - k, c, a);
        cv.blend(x, y + k, c, a);
    }
    cv.blend(x, y, pal(PAL_WHITE), a / 2);
}

void ArcadeScene::drawBeams(Canvas &cv, double t) const
{
    for (const auto &b : this->beams_)
    {
        const double u = b.age / b.life;
        const double grow = 1.0;
        int width = std::max(1, ri(2 + b.power * 4));
        int alpha = 256;
        if (b.age > 0.28)
        {
            const double fade = clamp01((b.age - 0.28) / (b.life - 0.28));
            width = std::max(1, ri(width * (1 - fade)));
            alpha = ri(256 * (1 - fade * fade));
        }
        else if (!this->reduced_ && (fi(t * 30) & 1) && width > 1)
        {
            width += 1;
        }
        const QPointF to = b.from + (b.to - b.from) * grow;
        const double dx = to.x() - b.from.x();
        const double dy = to.y() - b.from.y();
        const double len = std::max(1.0, std::hypot(dx, dy));
        const double nx = -dy / len;
        const double ny = dx / len;
        const int phase = this->reduced_ ? 0 : fi(t * 60);
        // Soft light around the beam (bloom spreads it further)
        const int steps = ri(len / 4);
        for (int i = 0; i <= steps; ++i)
        {
            const double f = steps > 0 ? double(i) / steps : 0.0;
            drawLight(cv, b.from.x() + dx * f + 0.5, b.from.y() + dy * f + 0.5,
                      2.5 + width * 0.7, pal(b.ink), ri(alpha * 0.22));
        }
        for (int k = -width / 2; k <= width / 2; ++k)
        {
            const int ax = ri(b.from.x() + nx * k);
            const int ay = ri(b.from.y() + ny * k);
            const int bx = ri(to.x() + nx * k);
            const int by = ri(to.y() + ny * k);
            const bool core = k == 0;
            const bool edge = std::abs(k) == width / 2 && width > 2;
            walkLine(ax, ay, bx, by, [&](int x, int y, int i) {
                Pix c;
                if (core)
                {
                    c = pal(PAL_WHITE);
                }
                else if (edge)
                {
                    c = pal(b.ink == PAL_MAGENTA ? PAL_PLUM : PAL_TEAL);
                }
                else
                {
                    // Energy ripples run up the beam
                    const bool ripple = ((i + phase) / 3) % 2 == 0;
                    c = ripple ? pal(b.ink) : pal(PAL_PALE);
                }
                cv.blend(x, y, c, alpha);
            });
        }
        // Impact and muzzle flares
        if (u < 0.5)
        {
            const int r = ri((1 - u * 2) * (2 + b.power * 3));
            drawDisc(cv, ri(to.x()), ri(to.y()), r, pal(PAL_WHITE), alpha);
            drawDisc(cv, ri(b.from.x()), ri(b.from.y()), std::max(1, r / 2),
                     pal(PAL_WHITE), alpha);
        }
    }
}

void ArcadeScene::drawPellets(Canvas &cv) const
{
    // Each keystroke sends a spark of energy from the caret into the ship.
    // It accelerates, so it reads as a comet with a long tail.
    const QPointF nose = this->shipNose();
    const auto at = [&](const Pellet &p, double u) {
        const double e = u * u;
        return QPointF(lerp(p.from.x(), nose.x() - 2, e),
                       lerp(p.from.y(), nose.y() + 3, e) -
                           std::sin(u * TAU / 2) * 2);
    };
    for (const auto &p : this->pellets_)
    {
        const double u = clamp01(p.age / p.life);
        const QPointF head = at(p, u);
        const QPointF tail = at(p, std::max(0.0, u - 0.2));
        const int hx = ri(head.x());
        const int hy = ri(head.y());
        const int tx = ri(tail.x());
        const int ty = ri(tail.y());
        const int length = std::max(1, std::max(std::abs(hx - tx), std::abs(hy - ty)));
        walkLine(tx, ty, hx, hy, [&](int x, int y, int i) {
            const double f = double(i) / length;  // 0 at the tail, 1 at the head
            const std::uint8_t ink =
                f > 0.9 ? PAL_WHITE : (f > 0.45 ? PAL_CYAN : PAL_TEAL);
            cv.blend(x, y, pal(ink), ri(60 + 196 * f));
        });
        cv.add(hx, hy - 1, pal(PAL_CYAN), 90);
        cv.add(hx, hy + 1, pal(PAL_CYAN), 90);
    }
}

void ArcadeScene::drawShip(Canvas &cv, double t) const
{
    if (this->shipAway_ >= 0.99)
    {
        return;
    }
    const QPointF s = this->shipPos();
    const int x = ri(s.x());
    const int y = ri(s.y());
    const double charge = this->charge_;

    // Power-up aura from gift capsules lingers and fades
    if (this->power_ > 0.02)
    {
        const int r = 8;
        const int shift = this->reduced_ ? 0 : fi(t * 8);
        walkCircle(x, y, r, [&](int px, int py) {
            if (((px + py + shift) & 3) == 0)
            {
                cv.blend(px, py, pal(PAL_CYAN),
                         ri(220 * std::min(1.0, this->power_)));
            }
        });
    }

    // Charge glow at the nose
    const double glow = charge + this->keyPulse_ * 0.4 + this->pickupFlash_ * 0.4;
    if (glow > 0.02)
    {
        drawLight(cv, x + 0.5, y - 4.5, 2 + glow * 5,
                  pal(charge > 0.7 ? PAL_MAGENTA : PAL_CYAN),
                  ri(60 + 140 * std::min(1.0, glow)));
    }

    // Engine flames flicker harder while you type
    const int frame = fi(t * 20);
    const double boost = this->keyPulse_ + charge * 0.5;
    for (int side = -1; side <= 1; side += 2)
    {
        const int length = 1 +
                           static_cast<int>(hash01(frame, side + 3) * 2.0) +
                           ri(boost * 2);
        for (int k = 0; k < length; ++k)
        {
            const std::uint8_t ink =
                k == 0 ? PAL_WHITE : (k == 1 ? PAL_AMBER : PAL_EMBER);
            cv.blend(x + side, y + 5 + k, pal(ink),
                     256 - k * 256 / (length + 1));
        }
    }

    Look look;
    look.core = charge > 0.5 ? PAL_WHITE : PAL_AMBER;
    look.flash = std::clamp(
        ri(charge * 70 + this->keyPulse_ * 70 + this->pickupFlash_ * 110 -
           this->drain_ * 30),
        0, 200);
    look.fade = std::clamp(ri(256 - this->drain_ * 50), 0, 256);
    look.alpha = ri(256 * (1 - this->shipAway_));
    drawSprite(cv, book().ship, x, y, look);

    // Wing tips burn white when the cannon is full
    if (charge > 0.95 && (this->reduced_ || std::fmod(t, 1.2) < 0.8))
    {
        cv.put(x - 5, y + 2, pal(PAL_WHITE));
        cv.put(x + 5, y + 2, pal(PAL_WHITE));
    }

    // Wingmen
    for (const auto &pod : this->pods_)
    {
        if (!pod.active)
        {
            continue;
        }
        Look pl;
        pl.core = pod.ink;
        drawSprite(cv, book().pod, ri(pod.x), ri(pod.y), pl);
        const int fl = 1 + static_cast<int>(hash01(frame, ri(pod.x)) * 2);
        for (int k = 0; k < fl; ++k)
        {
            cv.blend(ri(pod.x), ri(pod.y) + 3 + k,
                     pal(k == 0 ? PAL_AMBER : PAL_EMBER), 200);
        }
    }

    // Charge gauge: twelve segments that fill as you type, the newest one
    // flaring on every keystroke. It runs into the ship from the left, or
    // stands upright beside it where the spot is narrow.
    const int segments = 12;
    const double exact = charge * segments;
    const int filled =
        std::min(segments, static_cast<int>(std::ceil(exact - 0.05)));
    const bool vertical = this->gaugeVertical_;
    const int gx = vertical ? x - 10 : x - 9 - segments * 3;
    const int gy = vertical ? y + 4 : y + 3;
    for (int i = 0; i < segments; ++i)
    {
        std::uint8_t ink = PAL_NAVY;
        if (i < filled)
        {
            ink = i < 7 ? PAL_CYAN : (i < 11 ? PAL_MAGENTA : PAL_WHITE);
        }
        Pix c = pal(ink);
        if (i == filled - 1 && this->keyPulse_ > 0)
        {
            c = mixPix(c, pal(PAL_WHITE), ri(this->keyPulse_ * 220));
        }
        if (filled >= segments && i == segments - 1 && !this->reduced_ &&
            std::fmod(t, 1.4) > 0.9)
        {
            c = pal(PAL_MAGENTA);
        }
        const Pix dim = shade(c, i < filled ? 190 : 256);
        if (vertical)
        {
            const int sy = gy - i;
            cv.put(gx, sy, c);
            cv.put(gx + 1, sy, dim);
        }
        else
        {
            const int sx = gx + i * 3;
            cv.put(sx, gy, c);
            cv.put(sx + 1, gy, c);
            cv.put(sx, gy + 1, dim);
            cv.put(sx + 1, gy + 1, dim);
        }
    }
    if (vertical)
    {
        cv.put(gx - 1, gy + 1, pal(PAL_STEEL));
        cv.put(gx + 2, gy + 1, pal(PAL_STEEL));
    }
    else
    {
        cv.put(gx - 2, gy, pal(PAL_STEEL));
        cv.put(gx - 2, gy + 1, pal(PAL_STEEL));
        if (filled >= segments && (this->reduced_ || std::fmod(t, 1.4) < 0.9))
        {
            drawText(cv, QStringLiteral("MAX"), gx + segments * 3 - 13, gy - 7,
                     pal(PAL_WHITE));
        }
    }
}

QPointF ArcadeScene::gaugeTip() const
{
    const QPointF s = this->shipPos();
    if (this->gaugeVertical_)
    {
        return {s.x() - 10, s.y() + 4 - 12 * this->charge_};
    }
    return {s.x() - 45 + 36 * this->charge_, s.y() + 3};
}

QString hudNumber(long long v)
{
    return QString::number(v).rightJustified(6, QChar('0'));
}

int ArcadeScene::hudWidth(int level) const
{
    // level 0: 1UP score, HI score, lives; 1: no hi-score; 2: just the score
    int w = 0;
    const auto gap = [&w] {
        if (w > 0)
        {
            w += 8;
        }
    };
    if (this->hypeLevel_ > 0)
    {
        w += textWidth(QStringLiteral("*5"), 1) + (level < 2 ? 16 : 0);
    }
    gap();
    w += textWidth((level < 2 ? QStringLiteral("1UP ") : QString()) +
                       hudNumber(static_cast<long long>(this->shownScore_)),
                   1);
    if (level == 0)
    {
        gap();
        w += textWidth(QStringLiteral("HI ") + hudNumber(this->hiScore_), 1);
    }
    gap();
    if (!this->live_)
    {
        w += textWidth(QStringLiteral("CREDIT 0"), 1);
    }
    else
    {
        w += std::min(this->lives_, level < 2 ? 6 : 3) * 6 - 1;
    }
    return w;
}

void ArcadeScene::placeHud(const SceneLayout &layout)
{
    // Spots in order of preference: the tab strip right of the tab names,
    // the empty middle of a title bar above the tabs, the Activity header
    struct Spot {
        QRectF area;
        bool centred = false;
        bool header = false;
    };
    std::vector<Spot> spots;
    const QRectF tabs =
        layout.tabs.isEmpty() ? QRectF() : this->toGame(layout.tabs);
    if (!tabs.isEmpty())
    {
        const double left = this->tabLabels_.isEmpty()
                                ? tabs.left() + tabs.width() * 0.4
                                : this->tabLabels_.right() + 6;
        spots.push_back(
            {QRectF(left, tabs.top(), tabs.right() - 3 - left, tabs.height()),
             false, false});
        if (tabs.top() >= 7)
        {
            spots.push_back({QRectF(this->winW_ * 0.2, 0, this->winW_ * 0.46,
                                    tabs.top()),
                             true, false});
        }
    }
    else
    {
        spots.push_back({QRectF(this->winW_ * 0.4, 0, this->winW_ * 0.6 - 3,
                                std::max(8.0, this->top_.height() * 0.5)),
                         false, false});
    }
    if (!this->arenaHeader_.isEmpty())
    {
        const QRectF &h = this->arenaHeader_;
        const double left = h.left() + std::max(30.0, h.width() * 0.3);
        spots.push_back(
            {QRectF(left, h.top(), h.right() - 3 - left, h.height()), false,
             true});
    }

    for (int level = 0; level < 3; ++level)
    {
        const int need = this->hudWidth(level);
        for (const auto &spot : spots)
        {
            if (spot.area.width() >= need && spot.area.height() >= 7)
            {
                this->hudArea_ = spot.area;
                this->hudLevel_ = level;
                this->hudCentred_ = spot.centred;
                this->hudInHeader_ = spot.header;
                return;
            }
        }
    }
    // Nothing fits: squeeze the shortest HUD into the first spot
    this->hudArea_ = spots.front().area;
    this->hudLevel_ = 2;
    this->hudCentred_ = false;
    this->hudInHeader_ = spots.front().header;
}

void ArcadeScene::drawHud(Canvas &cv, double t) const
{
    const QRectF &area = this->hudArea_;
    const int level = this->hudLevel_;
    const int width = this->hudWidth(level);
    const int y = ri(area.center().y() - 2.5);
    int x = this->hudCentred_ ? ri(area.center().x() - width / 2.0)
                              : ri(area.right()) - width;

    // Left to right: hype meter, score, hi-score, lives or credits
    if (this->hypeLevel_ > 0)
    {
        const QString mult =
            QStringLiteral("*") + QString::number(this->multiplier());
        x += drawText(cv, mult, x, y, pal(PAL_AMBER));
        if (level < 2)
        {
            const int barW = 14;
            const int fill = ri(barW * clamp01(this->hypeProgress_));
            for (int i = 0; i < barW; ++i)
            {
                const std::uint8_t ink =
                    i < fill ? (i == fill - 1 ? PAL_WHITE : PAL_AMBER)
                             : PAL_DUSK;
                cv.put(x + i, y + 1, pal(ink));
                cv.put(x + i, y + 2, pal(ink));
                cv.put(x + i, y + 3, shade(pal(ink), 170));
            }
            x += barW + 2;
        }
        x += 7;
    }

    const auto shown = static_cast<long long>(this->shownScore_);
    const bool rolling = shown < this->score_;
    if (level < 2)
    {
        // 1UP blinks gently, the way cabinets show whose turn it is
        const int upAlpha =
            this->reduced_ ? 256 : (std::fmod(t, 1.6) < 1.1 ? 256 : 110);
        x += drawText(cv, QStringLiteral("1UP "), x, y, pal(PAL_MAGENTA), 1,
                      upAlpha);
    }
    x += drawText(cv, hudNumber(shown), x, y,
                  pal(rolling ? PAL_WHITE : PAL_PALE)) +
         7;

    if (level == 0)
    {
        const bool newHi =
            this->hiScore_ > START_HI_SCORE &&
            this->hiScore_ <= static_cast<long long>(this->shownScore_);
        x += drawText(cv, QStringLiteral("HI "), x, y, pal(PAL_CYAN));
        x += drawText(cv, hudNumber(this->hiScore_), x, y,
                      pal(newHi ? PAL_AMBER : PAL_PALE)) +
             7;
    }

    if (!this->live_)
    {
        drawText(cv, QStringLiteral("CREDIT ") + QString::number(this->credits_),
                 x, y, pal(PAL_PALE));
    }
    else
    {
        const auto &life = book().life;
        const int count = std::min(this->lives_, level < 2 ? 6 : 3);
        for (int i = 0; i < count; ++i)
        {
            drawSprite(cv, life, x + 2, y + 2);
            x += 6;
        }
    }
}

}  // namespace

std::unique_ptr<ThemeScene> makeArcadeScene()
{
    return std::make_unique<ArcadeScene>();
}

}  // namespace chatterino::scenes
