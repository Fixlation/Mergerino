// SPDX-License-Identifier: MIT

// "Departure Board": the main hall of a grand old railway terminus late at
// night. Iron roof trusses run away into the dark, lamps hang from the roof,
// and a split-flap departure board sits behind the Activity pane. Chat drives
// the board: chatters become destinations, your drafts flip into the NEXT
// DEPARTURE row by the input and sending a message dispatches it onto the
// board while a train pulls out along the platform.

#include "widgets/scenes/ThemeScene.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegion>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace chatterino::scenes {

namespace {

// Palette ------------------------------------------------------------------

constexpr QColor SURFACE(11, 14, 21);
constexpr QColor INK(236, 229, 214);
constexpr QColor ACCENT(242, 169, 59);

/// The black housing the flaps sit in
constexpr QColor HOUSING(5, 6, 8);
constexpr QColor FLAP_WHITE(238, 230, 212);
constexpr QColor FLAP_AMBER(255, 176, 58);
constexpr QColor FLAP_RED(244, 78, 60);
constexpr QColor FLAP_DIM(112, 108, 100);
constexpr QColor LAMP_CORE(255, 238, 206);
constexpr QColor LAMP_WARM(255, 170, 78);
constexpr QColor LAMP_HAZE(206, 126, 54);
constexpr QColor TRAIN_LIGHT(255, 242, 220);
constexpr QColor SIGNAL_RED(255, 64, 44);
constexpr QColor SIGNAL_GREEN(96, 255, 160);
constexpr QColor ENAMEL(214, 204, 182);
constexpr QColor LABEL_AMBER(186, 134, 52);

/// The station clock starts at 21:47:23
constexpr int CLOCK_START_MINUTES = 21 * 60 + 47;
constexpr double CLOCK_START_SECONDS = 23;

constexpr double BOUNCE_TIME = 0.24;
constexpr double FLUTTER_TIME = 0.7;
constexpr int MAX_RIFFLE = 7;
constexpr int SEQ_CAPACITY = 10;
constexpr double COMBO_TIME = 3.0;
constexpr double MARKER_TIME = 3.0;
constexpr double RAID_LIGHT_TIME = 3.0;
constexpr int CROWD_SLOTS = 48;
constexpr int PERSON_VARIANTS = 6;
constexpr int PERSON_BUCKETS = 8;

enum Ink : std::uint8_t {
    INK_WHITE = 0,
    INK_AMBER = 1,
    INK_RED = 2,
    INK_DIM = 3,
};

QColor inkColor(int ink)
{
    switch (ink)
    {
        case INK_AMBER:
            return FLAP_AMBER;
        case INK_RED:
            return FLAP_RED;
        case INK_DIM:
            return FLAP_DIM;
        default:
            return FLAP_WHITE;
    }
}

// Flap drum ------------------------------------------------------------------

/// The characters on each flap unit's drum, in the order the drum turns.
constexpr char16_t DRUM[] =
    u" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:\u00B7/\u2192@#&!?+'%\u25A0";
constexpr int DRUM_SIZE = static_cast<int>(std::size(DRUM)) - 1;

constexpr int drumIndex(char16_t c)
{
    for (int i = 0; i < DRUM_SIZE; ++i)
    {
        if (DRUM[i] == c)
        {
            return i;
        }
    }
    return -1;
}

constexpr int G_BLANK = 0;
constexpr int G_FIRST_ALNUM = 1;
constexpr int G_ALNUM_COUNT = 36;
constexpr int G_DOT = drumIndex(u'\u00B7');
constexpr int G_ARROW = drumIndex(u'\u2192');
constexpr int G_BLOCK = drumIndex(u'\u25A0');
constexpr int G_QUESTION = drumIndex(u'?');

/// Maps any character onto the drum: letters lose accents and case, a few
/// symbols get a close stand-in and anything else becomes a question mark.
int glyphFor(QChar ch)
{
    if (ch.isNull() || ch.isSpace())
    {
        return G_BLANK;
    }
    if (ch.decompositionTag() == QChar::Canonical)
    {
        const auto parts = ch.decomposition();
        if (!parts.isEmpty())
        {
            ch = parts.at(0);
        }
    }
    char16_t u = ch.toUpper().unicode();
    switch (u)
    {
        case u'_':
            u = u'-';
            break;
        case u',':
            u = u'.';
            break;
        case u';':
            u = u':';
            break;
        case u'"':
        case u'`':
            u = u'\'';
            break;
        case u'*':
            u = u'\u00B7';
            break;
        case u'>':
            u = u'\u2192';
            break;
        default:
            break;
    }
    const auto index = drumIndex(u);
    return index >= 0 ? index : G_QUESTION;
}

QString clockText(int minutes)
{
    minutes = ((minutes % 1440) + 1440) % 1440;
    return QStringLiteral("%1:%2")
        .arg(minutes / 60, 2, 10, QChar('0'))
        .arg(minutes % 60, 2, 10, QChar('0'));
}

/// One row of flap characters and their colours.
struct Cells {
    std::vector<std::uint8_t> glyph;
    std::vector<std::uint8_t> ink;

    Cells() = default;

    explicit Cells(int n)
        : glyph(static_cast<size_t>(std::max(0, n)),
                static_cast<std::uint8_t>(G_BLANK))
        , ink(static_cast<size_t>(std::max(0, n)),
              static_cast<std::uint8_t>(INK_WHITE))
    {
    }

    int size() const
    {
        return static_cast<int>(this->glyph.size());
    }

    /// Writes `text` into `width` cells starting at `col`, cut to fit.
    void put(int col, const QString &text, int width, int inkValue,
             bool alignRight = false)
    {
        const int n = std::min(width, static_cast<int>(text.size()));
        const int start = alignRight ? col + width - n : col;
        for (int i = 0; i < n; ++i)
        {
            const int index = start + i;
            if (index >= 0 && index < this->size())
            {
                this->glyph[static_cast<size_t>(index)] =
                    static_cast<std::uint8_t>(glyphFor(text.at(i)));
                this->ink[static_cast<size_t>(index)] =
                    static_cast<std::uint8_t>(inkValue);
            }
        }
    }
};

// Flap rendering ---------------------------------------------------------------

/// A soft black gradient used for the shadows flaps throw while they fall.
/// `up` puts the dark end at the bottom.
const QImage &shadowStrip(bool up)
{
    static const auto make = [](bool flipped) {
        QImage image(1, 32, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 32; ++y)
        {
            const double t = flipped ? (31 - y) / 31.0 : y / 31.0;
            const int a = qRound(210 * std::pow(1 - t, 1.6));
            image.setPixel(0, y, qRgba(0, 0, 0, a));
        }
        return image;
    };
    static const QImage down = make(false);
    static const QImage upward = make(true);
    return up ? upward : down;
}

/// Tile size, font and cached glyph images for one size of flap. Each glyph
/// is painted once as a whole tile; flips draw halves of these images.
class FlapStyle
{
public:
    void configure(int w, int h)
    {
        w = std::max(4, w);
        h = std::max(6, h);
        if (w == this->w_ && h == this->h_)
        {
            return;
        }
        this->w_ = w;
        this->h_ = h;
        this->cache_.clear();
        this->font_ = QFont(QStringLiteral("Bahnschrift"));
        this->font_.setWeight(QFont::DemiBold);
        this->font_.setStretch(QFont::SemiCondensed);
        this->font_.setHintingPreference(QFont::PreferNoHinting);
        // Capitals about 58% of the tile, like a real flap
        this->font_.setPixelSize(100);
        const double capRatio =
            std::max(0.4, QFontMetricsF(this->font_).capHeight() / 100.0);
        this->font_.setPixelSize(std::max(4, qRound(h * 0.58 / capRatio)));
        this->cap_ = QFontMetricsF(this->font_).capHeight();
    }

    int w() const
    {
        return this->w_;
    }

    int h() const
    {
        return this->h_;
    }

    QImage glyph(int g, int ink)
    {
        const int key = g * 8 + ink;
        const auto it = this->cache_.constFind(key);
        if (it != this->cache_.constEnd())
        {
            return *it;
        }
        auto image = this->paint(g, ink);
        this->cache_.insert(key, image);
        return image;
    }

private:
    QImage paint(int g, int ink) const;

    int w_ = 0;
    int h_ = 0;
    double cap_ = 0;
    QFont font_;
    QHash<int, QImage> cache_;
};

QImage FlapStyle::paint(int g, int ink) const
{
    const int w = this->w_;
    const int h = this->h_;
    const int hinge = h / 2;
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(HOUSING);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // The two flap halves, lit from above
    QLinearGradient top(0, 0, 0, hinge);
    top.setColorAt(0, QColor(38, 39, 43));
    top.setColorAt(1, QColor(23, 24, 27));
    p.fillRect(QRect(0, 0, w, hinge), top);
    QLinearGradient bottom(0, hinge, 0, h);
    bottom.setColorAt(0, QColor(25, 26, 29));
    bottom.setColorAt(1, QColor(13, 14, 16));
    p.fillRect(QRect(0, hinge, w, h - hinge), bottom);

    const auto color = inkColor(ink);
    const double cx = w / 2.0;
    if (g == G_BLOCK)
    {
        QLinearGradient fill(0, 0, 0, h);
        fill.setColorAt(0, color.lighter(112));
        fill.setColorAt(0.5, color);
        fill.setColorAt(1, color.darker(140));
        p.fillRect(QRect(1, 1, w - 2, h - 2), fill);
    }
    else if (g == G_ARROW)
    {
        const double len = std::min(w * 0.42, this->cap_ * 0.7);
        const double stroke = std::max(1.3, this->cap_ * 0.17);
        p.setPen(QPen(color, stroke, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(cx - len, hinge), QPointF(cx + len * 0.3, hinge));
        QPainterPath head;
        head.moveTo(cx + len, hinge);
        head.lineTo(cx + len * 0.1, hinge - len * 0.62);
        head.lineTo(cx + len * 0.1, hinge + len * 0.62);
        head.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(head);
    }
    else if (g == G_DOT)
    {
        const double r = std::max(1.0, this->cap_ * 0.11);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(cx, hinge - r * 0.6), r, r);
    }
    else if (g != G_BLANK && g >= 0 && g < DRUM_SIZE)
    {
        const auto text = QString(QChar(DRUM[g]));
        const QFontMetricsF metrics(this->font_);
        const double advance = metrics.horizontalAdvance(text);
        const double room = w - std::max(2.0, w * 0.16);
        const double squeeze = advance > room ? room / advance : 1.0;
        p.setFont(this->font_);
        p.setPen(color);
        p.translate(cx, hinge + this->cap_ / 2.0);
        p.scale(squeeze, 1.0);
        p.drawText(QPointF(-advance / 2.0, 0), text);
        p.resetTransform();
    }

    // The lower flap sits in the upper one's shadow
    p.fillRect(QRect(0, hinge, w, h - hinge), QColor(0, 0, 0, 38));
    p.fillRect(QRect(0, 0, w, 1), QColor(255, 255, 255, 20));
    p.fillRect(QRect(0, h - 1, w, 1), QColor(0, 0, 0, 150));
    // Hinge gap with the pins either side
    p.fillRect(QRect(0, hinge - 1, w, 1), QColor(2, 2, 3));
    if (h >= 26)
    {
        p.fillRect(QRect(0, hinge, w, 1), QColor(0, 0, 0, 110));
    }
    p.fillRect(QRect(0, hinge + (h >= 26 ? 1 : 0), w, 1),
               QColor(255, 255, 255, 12));
    if (w >= 10)
    {
        const int pin = std::max(1, h / 10);
        p.fillRect(QRect(0, hinge - pin, 1, pin * 2), HOUSING);
        p.fillRect(QRect(w - 1, hinge - pin, 1, pin * 2), HOUSING);
    }
    return image;
}

/// One flap unit. It shows `cur` and works through `seq` one flip at a time;
/// each flip drops the top half over the hinge to reveal the next character.
struct Flap {
    std::array<std::uint8_t, SEQ_CAPACITY> seq{};
    std::array<std::uint8_t, SEQ_CAPACITY> seqInk{};
    /// Seconds before the first flip of `seq`
    double delay = 0;
    /// 0..1 through the current flip, -1 between flips
    double phase = -1;
    double stepDur = 0.06;
    /// Seconds since the last flip landed, -1 when still
    double bounce = -1;
    /// Seconds since a breeze lifted the flap, -1 when still
    double flutter = -1;
    double flutterAmp = 0;
    std::uint8_t cur = G_BLANK;
    std::uint8_t curInk = INK_WHITE;
    std::uint8_t prev = G_BLANK;
    std::uint8_t prevInk = INK_WHITE;
    std::uint8_t seqLen = 0;
    std::uint8_t seqPos = 0;
    bool dirty = true;

    bool pending() const
    {
        return this->seqPos < this->seqLen;
    }

    bool busy() const
    {
        return this->pending() || this->bounce >= 0 || this->flutter >= 0;
    }

    int finalGlyph() const
    {
        return this->pending() ? this->seq[this->seqLen - 1] : this->cur;
    }

    int finalInk() const
    {
        return this->pending() ? this->seqInk[this->seqLen - 1] : this->curInk;
    }
};

/// Plans the flips that take `f` to `glyph`. A real drum turns through every
/// character in between; when that would take long, the flap riffles through
/// the last few characters before the target instead.
void planFlap(Flap &f, int glyph, int ink, double delay, int riffle,
              double stepDur, SceneRng &rng, bool force = false)
{
    if (!force && f.finalGlyph() == glyph && f.finalInk() == ink)
    {
        return;
    }
    std::array<std::uint8_t, SEQ_CAPACITY> seq{};
    std::array<std::uint8_t, SEQ_CAPACITY> seqInk{};
    int n = 0;
    const auto push = [&](int g, int k) {
        if (n < SEQ_CAPACITY)
        {
            seq[static_cast<size_t>(n)] = static_cast<std::uint8_t>(g);
            seqInk[static_cast<size_t>(n)] = static_cast<std::uint8_t>(k);
            n++;
        }
    };

    int from = f.cur;
    const bool midStep = f.phase >= 0 && f.pending();
    if (midStep)
    {
        // Let the flap that is already falling land first
        from = f.seq[f.seqPos];
        push(from, f.seqInk[f.seqPos]);
    }
    const bool landing =
        midStep && from == glyph && f.seqInk[f.seqPos] == ink && !force;
    if (!landing)
    {
        riffle = std::clamp(riffle, 0, MAX_RIFFLE);
        const int distance = (glyph - from + DRUM_SIZE) % DRUM_SIZE;
        if (!force && distance > 0 && distance - 1 <= riffle)
        {
            for (int g = (from + 1) % DRUM_SIZE; g != glyph;
                 g = (g + 1) % DRUM_SIZE)
            {
                push(g, ink);
            }
        }
        else
        {
            const bool alnum = glyph >= G_FIRST_ALNUM &&
                               glyph < G_FIRST_ALNUM + G_ALNUM_COUNT;
            for (int i = riffle; i >= 1; --i)
            {
                int g = 0;
                if (alnum && !force)
                {
                    g = G_FIRST_ALNUM + (glyph - G_FIRST_ALNUM - i +
                                         G_ALNUM_COUNT) %
                                            G_ALNUM_COUNT;
                }
                else
                {
                    g = G_FIRST_ALNUM +
                        static_cast<int>(rng.next() * G_ALNUM_COUNT) %
                            G_ALNUM_COUNT;
                }
                push(g, ink);
            }
        }
        n = std::min(n, SEQ_CAPACITY - 1);
        push(glyph, ink);
    }

    f.seq = seq;
    f.seqInk = seqInk;
    f.seqLen = static_cast<std::uint8_t>(n);
    f.seqPos = 0;
    f.stepDur = std::max(0.015, stepDur);
    f.bounce = -1;
    f.flutter = -1;
    if (!midStep)
    {
        f.phase = -1;
        f.delay = std::max(0.0, delay);
    }
    f.dirty = true;
}

void stepFlap(Flap &f, double dt)
{
    if (f.pending() && f.phase < 0)
    {
        f.delay -= dt;
        if (f.delay <= 0)
        {
            f.phase = std::min(0.9, -f.delay / f.stepDur);
            f.delay = 0;
            f.bounce = -1;
            f.flutter = -1;
        }
    }
    else if (f.phase >= 0)
    {
        f.phase += dt / f.stepDur;
        while (f.phase >= 1)
        {
            f.prev = f.cur;
            f.prevInk = f.curInk;
            f.cur = f.seq[f.seqPos];
            f.curInk = f.seqInk[f.seqPos];
            f.seqPos++;
            if (!f.pending())
            {
                // Landed: the flap rebounds off the stop
                f.phase = -1;
                f.seqLen = 0;
                f.seqPos = 0;
                f.bounce = 0;
                break;
            }
            f.phase -= 1;
        }
        f.dirty = true;
        return;
    }

    if (f.bounce >= 0)
    {
        f.bounce += dt;
        if (f.bounce > BOUNCE_TIME)
        {
            f.bounce = -1;
        }
        f.dirty = true;
    }
    if (f.flutter >= 0)
    {
        f.flutter += dt;
        if (f.flutter > FLUTTER_TIME)
        {
            f.flutter = -1;
        }
        f.dirty = true;
    }
}

/// Angle of the falling flap in degrees: 0 is upright, 180 is down. It
/// accelerates as it falls.
double flipAngle(double phase)
{
    return 180.0 * std::pow(clamp01(phase), 1.35);
}

/// How far the landed flap rebounds off the stop, in degrees.
double bounceAngle(double t)
{
    return 62.0 * std::exp(-t * 11.0) * std::abs(std::sin(t * 26.0));
}

double flutterAngle(double t, double amp)
{
    return amp * 88.0 * std::exp(-t * 4.0) * std::abs(std::sin(t * 11.0));
}

/// Paints one flap at (x, y) in device pixels with no transform.
void drawFlap(QPainter &p, int x, int y, const Flap &f, FlapStyle &style)
{
    const int w = style.w();
    const int h = style.h();
    const int hinge = h / 2;
    const int lower = h - hinge;
    int a = f.cur;
    int aInk = f.curInk;
    int b = f.cur;
    int bInk = f.curInk;
    double angle = 0;
    if (f.phase >= 0 && f.pending())
    {
        b = f.seq[f.seqPos];
        bInk = f.seqInk[f.seqPos];
        angle = flipAngle(f.phase);
    }
    else if (f.bounce >= 0)
    {
        a = f.prev;
        aInk = f.prevInk;
        angle = 180.0 - bounceAngle(f.bounce);
    }
    else if (f.flutter >= 0)
    {
        b = (f.cur + 1) % DRUM_SIZE;
        angle = flutterAngle(f.flutter, f.flutterAmp);
    }
    if (angle < 0.5 || angle > 179.5)
    {
        const bool down = angle > 90;
        p.drawImage(QPoint(x, y), style.glyph(down ? b : a, down ? bInk : aInk));
        return;
    }

    const QImage imageA = style.glyph(a, aInk);
    const QImage imageB = style.glyph(b, bInk);
    // Behind the moving flap: the new character's top, the old one's bottom
    p.drawImage(QPoint(x, y), imageB, QRect(0, 0, w, hinge));
    p.drawImage(QPoint(x, y + hinge), imageA, QRect(0, hinge, w, lower));

    const double rad = angle * TAU / 360.0;
    const double side = std::sin(rad);
    if (angle < 90)
    {
        // The old top half leans forward over the hinge
        const int fh = qRound(hinge * std::cos(rad));
        const int uncovered = hinge - fh;
        if (uncovered > 0)
        {
            p.setOpacity(0.75 * side);
            p.drawImage(QRect(x, y, w, uncovered), shadowStrip(true));
            p.setOpacity(1.0);
        }
        if (fh > 0)
        {
            p.drawImage(QRect(x, y + hinge - fh, w, fh), imageA,
                        QRect(0, 0, w, hinge));
            p.fillRect(QRect(x, y + hinge - fh, w, fh),
                       QColor(0, 0, 0, qRound(165 * side)));
            p.fillRect(QRect(x, y + hinge - fh, w, 1),
                       QColor(255, 232, 196, qRound(60 * side)));
        }
    }
    else
    {
        // The new bottom half swings down onto the old one
        const int fh = qRound(lower * -std::cos(rad));
        const int open = lower - fh;
        if (open > 0)
        {
            p.setOpacity(0.9 * side);
            p.drawImage(QRect(x, y + hinge + fh, w, open), shadowStrip(false));
            p.setOpacity(1.0);
        }
        if (fh > 0)
        {
            p.drawImage(QRect(x, y + hinge, w, fh), imageB,
                        QRect(0, hinge, w, lower));
            p.fillRect(QRect(x, y + hinge, w, fh),
                       QColor(0, 0, 0, qRound(145 * side)));
            p.fillRect(QRect(x, y + hinge + fh - 1, w, 1),
                       QColor(255, 232, 196, qRound(70 * side)));
        }
    }
    p.fillRect(QRect(x, y + hinge - 1, w, 1), QColor(2, 2, 3));
}

/// A grid of flaps of one size. Flaps are painted into a cached layer, and
/// only the ones that move are repainted each frame.
class FlapBank
{
public:
    FlapStyle style;

    void setup(int cols, int rows, int tileW, int tileH, int gapX, int gapY,
               QPoint origin, double dpr)
    {
        const std::vector<Flap> old = std::move(this->flaps_);
        const int oldCols = this->cols_;
        const int oldRows = this->rows_;
        this->cols_ = std::max(0, cols);
        this->rows_ = std::max(0, rows);
        this->flaps_.assign(static_cast<size_t>(this->cols_ * this->rows_),
                            Flap{});
        for (int r = 0; r < std::min(oldRows, this->rows_); ++r)
        {
            for (int c = 0; c < std::min(oldCols, this->cols_); ++c)
            {
                const auto &o = old[static_cast<size_t>(r * oldCols + c)];
                auto &f = this->flaps_[static_cast<size_t>(r * this->cols_ + c)];
                f.cur = static_cast<std::uint8_t>(o.finalGlyph());
                f.curInk = static_cast<std::uint8_t>(o.finalInk());
            }
        }
        this->style.configure(tileW, tileH);
        this->gapX_ = gapX;
        this->gapY_ = gapY;
        this->origin_ = origin;
        this->dpr_ = dpr;
        if (this->flaps_.empty())
        {
            this->layer_ = QImage();
            return;
        }
        this->layer_ = QImage(
            this->cols_ * (this->style.w() + gapX) - gapX,
            this->rows_ * (this->style.h() + gapY) - gapY, QImage::Format_RGB32);
        this->layer_.fill(HOUSING);
        this->dirty_ = true;
    }

    void clear()
    {
        this->flaps_.clear();
        this->cols_ = 0;
        this->rows_ = 0;
        this->layer_ = QImage();
    }

    bool empty() const
    {
        return this->flaps_.empty();
    }

    int cols() const
    {
        return this->cols_;
    }

    int rows() const
    {
        return this->rows_;
    }

    int count() const
    {
        return static_cast<int>(this->flaps_.size());
    }

    Flap &flap(int index)
    {
        return this->flaps_[static_cast<size_t>(index)];
    }

    Flap &at(int r, int c)
    {
        return this->flaps_[static_cast<size_t>(r * this->cols_ + c)];
    }

    const Flap &at(int r, int c) const
    {
        return this->flaps_[static_cast<size_t>(r * this->cols_ + c)];
    }

    QRectF tileRect(int r, int c) const
    {
        const auto w = this->style.w();
        const auto h = this->style.h();
        return {(this->origin_.x() + c * (w + this->gapX_)) / this->dpr_,
                (this->origin_.y() + r * (h + this->gapY_)) / this->dpr_,
                w / this->dpr_, h / this->dpr_};
    }

    QRectF rowRect(int r) const
    {
        const auto first = this->tileRect(r, 0);
        return {first.left(), first.top(), this->bounds().width(),
                first.height()};
    }

    QRectF bounds() const
    {
        return {this->origin_.x() / this->dpr_, this->origin_.y() / this->dpr_,
                this->layer_.width() / this->dpr_,
                this->layer_.height() / this->dpr_};
    }

    QPointF center(int index) const
    {
        return this->tileRect(index / std::max(1, this->cols_),
                              index % std::max(1, this->cols_))
            .center();
    }

    void target(int r, int c, int glyph, int ink, double delay, int riffle,
                double stepDur, SceneRng &rng)
    {
        if (r < 0 || r >= this->rows_ || c < 0 || c >= this->cols_)
        {
            return;
        }
        planFlap(this->at(r, c), glyph, ink, delay, riffle, stepDur, rng);
        this->dirty_ = true;
    }

    /// A quick riffle that ends on the same character.
    void riffle(int index, int count, double delay, double stepDur,
                SceneRng &rng)
    {
        auto &f = this->flap(index);
        if (f.pending())
        {
            return;
        }
        planFlap(f, f.cur, f.curInk, delay, count, stepDur, rng, true);
        this->dirty_ = true;
    }

    void snap(int r, int c, int glyph, int ink)
    {
        if (r < 0 || r >= this->rows_ || c < 0 || c >= this->cols_)
        {
            return;
        }
        auto &f = this->at(r, c);
        f = Flap{};
        f.cur = static_cast<std::uint8_t>(glyph);
        f.curInk = static_cast<std::uint8_t>(ink);
        f.dirty = true;
        this->dirty_ = true;
    }

    void flutter(int index, double amp)
    {
        auto &f = this->flap(index);
        if (f.busy())
        {
            return;
        }
        f.flutter = 0;
        f.flutterAmp = amp;
        f.dirty = true;
        this->dirty_ = true;
    }

    void step(double dt)
    {
        for (auto &f : this->flaps_)
        {
            stepFlap(f, dt);
            if (f.dirty)
            {
                this->dirty_ = true;
            }
        }
    }

    /// Repaints the flaps that changed into the layer.
    void render()
    {
        if (!this->dirty_ || this->layer_.isNull())
        {
            return;
        }
        QPainter p(&this->layer_);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const int w = this->style.w();
        const int h = this->style.h();
        for (int r = 0; r < this->rows_; ++r)
        {
            for (int c = 0; c < this->cols_; ++c)
            {
                auto &f = this->at(r, c);
                if (!f.dirty)
                {
                    continue;
                }
                drawFlap(p, c * (w + this->gapX_), r * (h + this->gapY_), f,
                         this->style);
                f.dirty = false;
            }
        }
        this->dirty_ = false;
    }

    void paint(QPainter &p, double opacity = 1.0) const
    {
        if (this->layer_.isNull() || opacity <= 0.003)
        {
            return;
        }
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.setOpacity(p.opacity() * clamp01(opacity));
        p.drawImage(this->bounds(), this->layer_);
        p.restore();
    }

private:
    std::vector<Flap> flaps_;
    int cols_ = 0;
    int rows_ = 0;
    int gapX_ = 0;
    int gapY_ = 0;
    QPoint origin_;
    double dpr_ = 1;
    QImage layer_;
    bool dirty_ = false;
};

// World pieces -----------------------------------------------------------------

enum class RowKind : std::uint8_t {
    Blank,
    Chatter,
    New,
    Self,
    Sub,
    Gift,
    Raid,
    Mention,
    Shoutout,
    Cancel,
    Service,
    Joined,
    Fare,
};

/// One line of the departure board.
struct BoardRow {
    RowKind kind = RowKind::Blank;
    int id = 0;
    /// Timetable columns
    int minutes = -1;
    QString dest;
    int plat = 0;
    QString remark;
    std::uint8_t ink = INK_WHITE;
    std::uint8_t remarkInk = INK_WHITE;
    /// Free text, longest version first. With a remark it fills the columns
    /// left of the remark, otherwise the whole row.
    QStringList free;
    bool center = false;
    /// The remark is still on its way up from the NEXT DEPARTURE row
    bool remarkHidden = false;
    /// Lower case user name, for hover and timeouts
    QString author;
    double born = 0;
    double glow = 0;
    /// Indicator lamp colour, -1 when off
    int bulb = -1;
    bool pulse = false;
    Cells cells;
};

struct Remark {
    const char *text;
    std::uint8_t ink;
};

constexpr std::array<Remark, 10> REMARKS{{
    {"ON TIME", INK_WHITE},
    {"ON TIME", INK_WHITE},
    {"ON TIME", INK_WHITE},
    {"ON TIME", INK_WHITE},
    {"BOARDING", INK_AMBER},
    {"DELAYED", INK_AMBER},
    {"+5 MIN", INK_AMBER},
    {"LAST CALL", INK_AMBER},
    {"DUE", INK_WHITE},
    {"ON TIME", INK_WHITE},
}};

constexpr std::array<const char *, 14> PLACES{
    "ELMHAVEN",  "GREYFORD",   "LARKSPUR",   "HALEWOOD",    "OAKCROSS",
    "BRIARMOOR", "KESTREL",    "WILLOWMERE", "ASHCOMBE",    "NORTHWICK",
    "FENHOLT",   "SALTERN",    "CLOVERDALE", "HIGH MARROW",
};

struct Lamp {
    QPointF bulb;
    double size = 1;
    double level = 1;
    double target = 1;
    double swell = 0;
};

struct Person {
    double x01 = 0;
    /// 0 is far away, 1 is at the platform edge
    double depth = 0;
    int variant = 0;
    double walk = 0;
    double phase = 0;
};

/// A passenger stepping off a raid train.
struct Arrival {
    double x = 0;
    double depth = 0;
    int variant = 0;
    double speed = 0;
    double born = 0;
    double stopAt = 0;
};

/// A train's light running along the platform.
struct Streak {
    double start = 0;
    double duration = 1.4;
    double from = 0;
    double to = 0;
    double power = 1;
    bool arriving = false;
};

/// A flap tile flying from the NEXT DEPARTURE row up to the board.
struct Flight {
    int glyph = 0;
    int ink = 0;
    QRectF from;
    QRectF to;
    double start = 0;
    double duration = 0.6;
    double tilt = 0;
};

struct Timed {
    double at = 0;
    std::function<void()> run;
};

/// Geometry of the window, recomputed when the layout changes.
struct Geo {
    QSizeF win;
    double dpr = 1;
    QRectF tabs;
    QRectF tabLabels;
    double topBottom = 0;
    QRectF band;
    QRectF input;
    double platTop = 0;
    QPointF vp;
    double springY = 0;
    std::vector<QRectF> chats;

    QRectF column;
    bool hasBoard = false;
    bool overChat = false;
    QRectF housing;
    double titleH = 0;
    double hypeTop = 0;
    QRectF labelsRow;
    QPointF clockC;
    double clockR = 0;
    // The station name sits in the board's title when the tab strip is full
    bool signInHousing = false;
    QRectF titleLabel;
    // The NEXT DEPARTURE row along the foot of the board
    QRectF nextLabelRow;

    QRectF crowd;
    double personNear = 0;
    double personFar = 0;
    bool hasKiosk = false;
    QRectF kiosk;
    QRectF kioskBody;
    QRectF kioskDisplay;

    int timeCol = 0;
    int timeW = 5;
    int destCol = 6;
    int destW = 8;
    int platCol = -1;
    int platW = 0;
    int remarkCol = 0;
    int remarkW = 9;
};

/// A passenger's silhouette in a box one unit tall, feet at the origin.
QPainterPath personPath(int variant)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const double headY = -0.885;
    path.addEllipse(QPointF(0, headY), 0.062, 0.072);
    path.addRect(QRectF(-0.03, -0.83, 0.06, 0.05));
    const double flare = variant == 1 ? 0.2 : 0.155;
    QPainterPath coat;
    coat.moveTo(-0.11, -0.805);
    coat.quadTo(-0.155, -0.79, -0.155, -0.70);
    coat.lineTo(-flare, -0.26);
    coat.lineTo(flare, -0.26);
    coat.lineTo(0.155, -0.70);
    coat.quadTo(0.155, -0.79, 0.11, -0.805);
    coat.closeSubpath();
    path.addPath(coat);
    path.addRect(QRectF(-0.075, -0.29, 0.052, 0.29));
    path.addRect(QRectF(0.023, -0.29, 0.052, 0.29));
    path.addRect(QRectF(-0.09, -0.025, 0.07, 0.025));
    path.addRect(QRectF(0.02, -0.025, 0.07, 0.025));
    switch (variant)
    {
        case 0:
        case 4:
            // Hat with a brim
            path.addEllipse(QPointF(0, headY - 0.045), 0.105, 0.02);
            path.addRect(QRectF(-0.06, headY - 0.125, 0.12, 0.085));
            break;
        case 1:
            path.addEllipse(QPointF(0.045, headY - 0.045), 0.036, 0.036);
            break;
        default:
            break;
    }
    if (variant == 2)
    {
        // Suitcase
        path.addRect(QRectF(0.17, -0.33, 0.2, 0.24));
        path.addRect(QRectF(0.235, -0.38, 0.07, 0.05));
        path.addRect(QRectF(0.13, -0.5, 0.05, 0.18));
    }
    else if (variant == 3)
    {
        // Rucksack
        path.addRoundedRect(QRectF(-0.25, -0.77, 0.12, 0.33), 0.03, 0.03);
    }
    else if (variant == 4)
    {
        // Rolled umbrella
        path.addRect(QRectF(0.16, -0.47, 0.02, 0.47));
        path.addRect(QRectF(0.13, -0.5, 0.06, 0.04));
    }
    else if (variant == 5)
    {
        // Reading the evening paper
        path.addRect(QRectF(0.06, -0.76, 0.24, 0.18));
    }
    return path;
}

void paintTruss(QPainter &p, QPointF c, double rx, double ry, double depth,
                const QColor &color, double width)
{
    const QRectF inner(c.x() - rx, c.y() - ry, rx * 2, ry * 2);
    const QRectF outer = inner.adjusted(-depth, -depth, depth, depth);
    QPainterPath chords;
    chords.arcMoveTo(inner, 180);
    chords.arcTo(inner, 180, -180);
    chords.arcMoveTo(outer, 180);
    chords.arcTo(outer, 180, -180);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, width));
    p.drawPath(chords);

    // Warren lattice between the chords
    QPainterPath lattice;
    constexpr int SEGMENTS = 36;
    for (int i = 0; i <= SEGMENTS; ++i)
    {
        const double a = TAU / 2 * i / SEGMENTS;
        const QPointF in(c.x() + rx * std::cos(a), c.y() - ry * std::sin(a));
        if (i == 0)
        {
            lattice.moveTo(in);
        }
        else
        {
            lattice.lineTo(in);
        }
        if (i < SEGMENTS)
        {
            const double am = TAU / 2 * (i + 0.5) / SEGMENTS;
            lattice.lineTo(QPointF(c.x() + (rx + depth) * std::cos(am),
                                   c.y() - (ry + depth) * std::sin(am)));
        }
    }
    p.setPen(QPen(color, std::max(0.6, width * 0.5)));
    p.drawPath(lattice);
}

QFont labelFont(double pixelSize, int weight, double spacing)
{
    QFont font(QStringLiteral("Bahnschrift"));
    font.setPixelSize(std::max(6, qRound(pixelSize)));
    font.setWeight(static_cast<QFont::Weight>(weight));
    font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    return font;
}

// The scene ------------------------------------------------------------------

class DepartureScene : public ThemeScene
{
public:
    SceneInfo info() const override
    {
        return {QStringLiteral("departure"), QStringLiteral("Departure Board"),
                SURFACE, INK, ACCENT, false};
    }

    SceneGlass glass() const override
    {
        // The board behind the Activity pane must stay legible, so the glass
        // is thinner than usual; the chat area is darkened in the art instead.
        return {0.5, 0.3, 0.25};
    }

    void step(const SceneState &state, const SceneLayout &layout) override;
    void event(const SceneEvent &event, const SceneState &state,
               const SceneLayout &layout) override;
    void paintBack(QPainter &painter, const SceneState &state,
                   const SceneLayout &layout) override;

    bool hasFront() const override
    {
        return this->front_;
    }

    void paintFront(QPainter &painter, const SceneState &state,
                    const SceneLayout &layout) override;

private:
    // Layout and cached art
    bool layoutChanged(const SceneLayout &layout) const;
    void ensureLayout(const SceneLayout &layout);
    void relayout(const SceneLayout &layout);
    void layoutBoard();
    void layoutBanks();
    void layoutLamps();
    void buildStatic();
    void buildCaches();
    void paintHall(QPainter &p) const;
    void paintPlatform(QPainter &p) const;
    void paintVeil(QPainter &p) const;
    void paintLampFittings(QPainter &p) const;
    void paintHousing(QPainter &p) const;
    void paintColumnLabels(QPainter &p) const;
    void paintClockFace(QPainter &p) const;
    void paintKioskBody(QPainter &p) const;
    void paintConcourse(QPainter &p) const;
    void paintSignPlate(QPainter &p) const;

    // Simulation
    void boot(const SceneState &state);
    void after(double delay, std::function<void()> run);
    void runTimeline();
    void syncDraft(const SceneState &state);
    void syncHype(const SceneState &state);
    void syncTicket();
    void turnover(const SceneState &state);
    void breeze(const SceneState &state);
    void stepWorld(const SceneState &state);
    std::array<FlapBank *, 7> banks();

    // Board rows
    BoardRow chatterRow(const QString &name, int rowIndex);
    BoardRow freeRow(RowKind kind, const QStringList &candidates, int ink);
    void formatRow(BoardRow &row) const;
    void applyRow(int r, double delay, double colStagger, int riffle,
                  double stepDur);
    void snapRows();
    void insertTop(BoardRow row, double delay);
    void replaceRow(int r, BoardRow row, double delay, double colStagger,
                    int riffle);
    int findRow(const QString &author) const;
    int findRowId(int id) const;
    int pickRow(bool preferBottom);
    QString takeName();
    QString recentName(int skip, const QString &avoid);
    int clockMinutes() const;
    double boardLight() const;
    Cells hypeCells(int level, double progress) const;
    void lampSwell(double x, double radius, double amount);

    // Reactions
    void onKey(const SceneEvent &e);
    void onSend(const SceneEvent &e);
    void onMessage(const SceneEvent &e);
    void onSub(const SceneEvent &e);
    void onGift(const SceneEvent &e);
    void onBits(const SceneEvent &e);
    void onRaid(const SceneEvent &e);
    void onMention(const SceneEvent &e);
    void onCombo(const SceneEvent &e);
    void onHype(const SceneEvent &e, const SceneState &state);
    void onTimeout(const SceneEvent &e);
    void onOffline();
    void onLive();
    void onHover(const SceneEvent &e);
    void refreshNext(int changed, bool deleted);
    void setSign(const QString &text, bool animate);
    void clickWave(QPointF point);

    // Painting
    void paintLamps(QPainter &p) const;
    void paintHaze(QPainter &p) const;
    void paintSignal(QPainter &p) const;
    void paintCrowd(QPainter &p) const;
    void paintKiosk(QPainter &p) const;
    void paintBoard(QPainter &p) const;
    void paintCombo(QPainter &p) const;
    void paintHype(QPainter &p) const;
    void paintClockHands(QPainter &p) const;
    void paintSign(QPainter &p) const;
    void paintNextRow(QPainter &p, const SceneState &state) const;
    void paintStreaks(QPainter &p) const;
    void paintHeadlight(QPainter &p) const;
    void paintFlights(QPainter &p);
    void paintMarker(QPainter &p) const;
    void paintPigeon(QPainter &p) const;

    Geo geo_;
    SceneLayout last_;
    bool laidOut_ = false;
    bool booted_ = false;
    double now_ = 0;
    double dt_ = 0;
    bool reduced_ = false;
    bool live_ = true;
    SceneRng rng_{0x5EA7u};

    FlapBank board_;
    FlapBank sign_;
    FlapBank next_;
    FlapBank hype_;
    FlapBank combo_;
    FlapBank ticket_;
    FlapBank marker_;
    std::vector<BoardRow> rows_;
    int nextRowId_ = 1;

    QImage static_;
    QImage kioskImage_;
    QImage boardLight_;
    QRegion trainClip_;
    QImage streakRight_;
    QImage streakLeft_;
    QImage trainRight_;
    QImage trainLeft_;
    QImage beam_;
    std::vector<QImage> people_;
    std::array<double, PERSON_BUCKETS> bucketHeights_{};

    std::vector<Lamp> lamps_;
    std::vector<Person> crowd_;
    std::vector<int> crowdOrder_;
    std::vector<Arrival> arrivals_;
    double crowdCount_ = -1;

    std::vector<Timed> timeline_;
    std::vector<Streak> streaks_;
    std::vector<Flight> flights_;
    bool front_ = false;

    // Typing
    std::vector<std::uint8_t> draft_;
    int draftOffset_ = 0;
    std::vector<double> keyGlow_;
    double sendT_ = -10;

    // Clock
    int ticks_ = 0;
    int shownMinute_ = -1;
    double minuteJumpT_ = -10;

    // Chatter
    std::deque<QString> recent_;
    std::deque<QString> waiting_;
    double turnover_ = 0;
    double holdBoardUntil_ = 0;
    double lastNewRowT_ = -10;

    // Event state
    QString hoverUser_;
    bool hovering_ = false;
    double hoverLevel_ = 0;
    double comboT_ = -10;
    double raidT_ = -10;
    double raidPower_ = 0;
    double bitsT_ = -100;
    int bitsAmount_ = 0;
    double bitsDuration_ = 1.5;
    double bitsPower_ = 0;
    double hypeFlipUntil_ = -10;
    double hypeGlowT_ = -10;
    double markerT_ = -10;
    double signalGreenUntil_ = -10;
    QString signText_;
    double signT_ = -10;
    double pigeonFledT_ = -100;
    std::optional<QPointF> lastPointer_;
};

// Layout -----------------------------------------------------------------------

bool DepartureScene::layoutChanged(const SceneLayout &l) const
{
    const auto &o = this->last_;
    return !this->laidOut_ || l.window != o.window || l.dpr != o.dpr ||
           l.tabs != o.tabs || l.tabLabels != o.tabLabels ||
           l.chats != o.chats || l.activity != o.activity ||
           l.headers != o.headers || l.inputs != o.inputs ||
           l.focusChat != o.focusChat || l.focusInput != o.focusInput;
}

void DepartureScene::ensureLayout(const SceneLayout &layout)
{
    if (this->layoutChanged(layout))
    {
        this->relayout(layout);
    }
}

void DepartureScene::relayout(const SceneLayout &l)
{
    this->last_ = l;
    this->laidOut_ = true;

    Geo g;
    g.win = l.window;
    g.dpr = std::max<qreal>(1.0, l.dpr);
    const double W = g.win.width();
    const double H = g.win.height();
    g.tabs = l.tabs;
    g.tabLabels = l.tabLabels;
    g.topBottom = l.tabs.isEmpty() ? 0.0 : l.tabs.bottom();
    for (const auto &header : l.headers)
    {
        if (!header.isEmpty() && header.top() <= g.topBottom + 2)
        {
            g.topBottom = std::max(g.topBottom, header.bottom());
        }
    }
    g.band = l.inputBand();
    g.input = l.focusInput;
    g.platTop = !g.band.isEmpty() && g.band.height() > 8
                    ? g.band.top()
                    : H - std::min(40.0, H * 0.08);
    g.chats = l.chats;
    g.vp = QPointF(W * 0.4, H * 0.5);
    g.springY = H * 0.6;

    // The board lives behind the biggest Activity pane, or on the right
    QRectF column;
    for (const auto &a : l.activity)
    {
        if (a.width() * a.height() > column.width() * column.height())
        {
            column = a;
        }
    }
    if (column.isEmpty() && W >= 560)
    {
        const double x0 = std::max(W * 0.62, W - 560);
        column = QRectF(x0, g.topBottom + 2, W - x0 - 6,
                        g.platTop - g.topBottom - 10);
        g.overChat = true;
    }
    g.column = column;
    g.hasBoard = column.width() >= 150 && column.height() >= 150;
    this->geo_ = g;

    this->layoutBanks();
    this->layoutBoard();
    this->layoutLamps();

    // Trains run where nothing is written over the platform: not under the
    // chats, the input boxes or the emote bar above the focused box
    this->trainClip_ = QRegion(QRect(0, 0, static_cast<int>(std::ceil(W)),
                                     static_cast<int>(std::ceil(H))));
    for (const auto &chat : l.chats)
    {
        this->trainClip_ -= QRegion(chat.toAlignedRect());
    }
    for (const auto &input : l.inputs)
    {
        this->trainClip_ -= QRegion(input.adjusted(-2, -2, 2, 2).toAlignedRect());
    }
    if (!g.band.isEmpty() && !g.input.isEmpty() && g.input.top() > g.band.top())
    {
        this->trainClip_ -= QRegion(
            QRectF(g.band.left(), g.band.top(), g.band.width(),
                   g.input.top() - g.band.top())
                .toAlignedRect());
    }

    // Keep the rows, refit to the new board
    const int rows = this->board_.rows();
    this->rows_.resize(static_cast<size_t>(rows));
    for (int r = 0; r < rows; ++r)
    {
        auto &row = this->rows_[static_cast<size_t>(r)];
        if (this->booted_ && row.kind == RowKind::Blank && this->live_)
        {
            row = this->chatterRow(
                QString::fromLatin1(PLACES[static_cast<size_t>(r) % PLACES.size()]),
                r);
            row.author.clear();
        }
        this->formatRow(row);
    }
    this->snapRows();
    if (!this->sign_.empty() && !this->signText_.isEmpty())
    {
        this->setSign(this->signText_, false);
    }
    this->keyGlow_.assign(static_cast<size_t>(this->next_.cols()), 0.0);
    this->draftOffset_ = -1;
    this->refreshNext(-1, false);
    for (int c = 0; c < this->ticket_.cols(); ++c)
    {
        this->ticket_.snap(0, c, glyphFor('0'), INK_DIM);
    }

    if (this->crowd_.empty())
    {
        for (int i = 0; i < CROWD_SLOTS; ++i)
        {
            Person person;
            person.x01 = hash01(i, 31);
            person.depth = std::pow(hash01(i, 32), 0.8);
            person.variant = static_cast<int>(hash01(i, 33) * PERSON_VARIANTS) %
                             PERSON_VARIANTS;
            person.walk = hash01(i, 34) < 0.3 ? (hash01(i, 35) - 0.5) * 0.02 : 0;
            person.phase = hash01(i, 36) * TAU;
            this->crowd_.push_back(person);
            this->crowdOrder_.push_back(i);
        }
        std::sort(this->crowdOrder_.begin(), this->crowdOrder_.end(),
                  [this](int a, int b) {
                      return this->crowd_[static_cast<size_t>(a)].depth <
                             this->crowd_[static_cast<size_t>(b)].depth;
                  });
    }

    this->buildStatic();
    this->buildCaches();
}

void DepartureScene::layoutBoard()
{
    auto &g = this->geo_;
    const double dpr = g.dpr;
    if (!g.hasBoard)
    {
        this->board_.clear();
        this->hype_.clear();
        this->combo_.clear();
        this->ticket_.clear();
        this->next_.clear();
        g.hasKiosk = false;
        g.signInHousing = false;
        return;
    }

    const auto &col = g.column;
    const double margin = std::clamp(col.width() * 0.025, 5.0, 14.0);
    const double top = col.top() + 10;
    const double left = col.left() + margin;
    const double right = col.right() - margin;
    constexpr double PAD_LEFT = 14;
    constexpr double PAD_RIGHT = 8;
    const double availW = right - left - PAD_LEFT - PAD_RIGHT;
    const int availDev = std::max(40, static_cast<int>(availW * dpr));

    // Tiles about 13 px wide, 22 to 36 to a row
    int cols = std::clamp(static_cast<int>(std::lround(availW / 14.0)), 22, 36);
    int pitch = std::max(5, availDev / cols);
    const int gapX = std::max(1, qRound(pitch * 0.08));
    int tileW = pitch - gapX;
    const int maxTileW = qRound(24 * dpr);
    if (tileW > maxTileW)
    {
        tileW = maxTileW;
        pitch = tileW + gapX;
        cols = availDev / pitch;
    }
    const int tileH = qRound(tileW * 1.45);
    const int gapY = std::max(2, qRound(tileH * 0.2));
    const double labelsH = std::max(10.0, std::round(tileH / dpr * 0.7));

    // Title strip: the name on the left, the clock hanging inside on the right
    g.clockR = std::clamp(col.width() * 0.055, 14.0, 24.0);
    g.titleH = std::max(24.0, g.clockR * 2 + 8);
    const double titleLeft = left + 12;
    const double titleRight = right - 8 - g.clockR * 2 - 10;
    g.titleLabel = QRectF(titleLeft, top + 2, titleRight - titleLeft, g.titleH - 2);
    if (g.signInHousing)
    {
        // No room in the tab strip: the station name flaps sit in the title,
        // under a small DEPARTURES plate
        const double smallH = std::clamp(g.titleH * 0.26, 8.0, 11.0);
        const int sh = qRound(std::clamp(tileH / dpr * 1.25, 12.0, 22.0) * dpr);
        const int sw = qRound(sh * 0.68);
        const int sgap = std::max(1, qRound(1.5 * dpr));
        const int signCols = std::min(
            20, static_cast<int>(((titleRight - titleLeft) * dpr + sgap) /
                                 (sw + sgap)));
        if (signCols >= 5)
        {
            g.titleH = std::max(g.titleH, 12 + smallH + sh / dpr);
            g.titleLabel = QRectF(titleLeft, top + 3, titleRight - titleLeft, smallH);
            this->sign_.setup(signCols, 1, sw, sh, sgap, 0,
                              QPoint(qRound(titleLeft * dpr),
                                     qRound((top + 5 + smallH) * dpr)),
                              dpr);
        }
        else
        {
            g.signInHousing = false;
        }
    }
    g.clockC = QPointF(right - 8 - g.clockR, top + g.titleH / 2 + 1);

    // A strip of short flaps for the hype train runs under the title
    const int hypeH = std::max(6, qRound(tileH * 0.6));
    const double hypeTop = top + g.titleH + 5;
    const double labelsTop = hypeTop + hypeH / dpr + 4;
    const double gridTop = labelsTop + labelsH + 2;
    // The NEXT DEPARTURE row along the foot needs a label and one row
    const double footH = 7 + labelsH + 2 + tileH / dpr;
    const double floorLimit =
        std::min(col.top() + col.height() * 0.68, g.platTop - 70);
    const int rows = std::clamp(
        static_cast<int>(((floorLimit - footH - gridTop) * dpr + gapY) /
                         (tileH + gapY)),
        3, 18);
    const int gridW = cols * pitch - gapX;
    const int gridH = rows * (tileH + gapY) - gapY;
    const QPoint origin(qRound((left + PAD_LEFT) * dpr) + (availDev - gridW) / 2,
                        qRound(gridTop * dpr));
    this->board_.setup(cols, rows, tileW, tileH, gapX, gapY, origin, dpr);
    const double gridBottom = (origin.y() + gridH) / dpr;
    const double footTop = gridBottom + 7;
    g.nextLabelRow = QRectF(left + PAD_LEFT, footTop, availW, labelsH);
    const QPoint nextOrigin(origin.x(), qRound((footTop + labelsH + 2) * dpr));
    this->next_.setup(cols, 1, tileW, tileH, gapX, 0, nextOrigin, dpr);
    const double nextBottom = (nextOrigin.y() + tileH) / dpr;
    g.housing = QRectF(left, top, right - left, nextBottom + 8 - top);
    g.labelsRow = QRectF(left + PAD_LEFT, labelsTop, availW, labelsH);
    g.hypeTop = hypeTop;

    // Timetable columns: TIME DESTINATION PLAT REMARKS
    g.timeCol = 0;
    g.timeW = 5;
    g.remarkW = cols >= 32 ? 9 : 8;
    g.remarkCol = cols - g.remarkW;
    g.platW = cols >= 25 ? 2 : 0;
    g.destCol = g.timeW + 1;
    if (g.platW > 0)
    {
        g.platCol = g.remarkCol - 1 - g.platW;
        g.destW = g.platCol - 1 - g.destCol;
    }
    else
    {
        g.platCol = -1;
        g.destW = g.remarkCol - 1 - g.destCol;
    }

    // Big flaps for emote combos, two rows tall
    const int comboH = tileH * 2 + gapY;
    const int comboW = qRound(comboH * 0.66);
    const int comboGap = std::max(2, gapX * 2);
    const int comboCols = std::max(1, (gridW + comboGap) / (comboW + comboGap));
    const int comboRow = rows >= 5 ? 1 : 0;
    const QPoint comboOrigin(
        origin.x() + (gridW - (comboCols * (comboW + comboGap) - comboGap)) / 2,
        origin.y() + comboRow * (tileH + gapY));
    this->combo_.setup(comboCols, 1, comboW, comboH, comboGap, 0, comboOrigin,
                       dpr);

    this->hype_.setup(cols, 1, tileW, hypeH, gapX, 0,
                      QPoint(origin.x(), qRound(hypeTop * dpr)), dpr);

    // The concourse below the board, with a ticket machine
    const double floorBottom = std::min(col.bottom(), g.platTop);
    g.crowd = QRectF(col.left(), g.housing.bottom() + 6, col.width(),
                     std::max(0.0, floorBottom - g.housing.bottom() - 6));
    g.personNear = std::clamp(g.crowd.height() * 0.6, 0.0, 92.0);
    g.personFar = g.personNear * 0.42;
    g.hasKiosk = g.crowd.height() >= 60 && col.width() >= 220;
    if (g.hasKiosk)
    {
        // A ticket machine with its fare counter on a post above it
        const double kw = std::clamp(col.width() * 0.17, 48.0, 84.0);
        const double kh = std::clamp(g.crowd.height() * 0.5, 46.0, 120.0);
        const double dw = std::clamp(col.width() * 0.3, 84.0, 150.0);
        constexpr int DIGITS = 6;
        const int tGap = std::max(1, qRound(1.5 * dpr));
        const int tPitch =
            std::max(5, static_cast<int>((dw - 12) * dpr / DIGITS));
        const int tW = tPitch - tGap;
        const int tH = qRound(tW * 1.45);
        const int width = DIGITS * tPitch - tGap;
        const double cx = col.right() - std::max(kw, dw) / 2 - 14;
        g.kioskBody = QRectF(cx - kw / 2, floorBottom - 3 - kh, kw, kh);
        const double displayTop = g.kioskBody.top() - 13 - tH / dpr;
        this->ticket_.setup(DIGITS, 1, tW, tH, tGap, 0,
                            QPoint(qRound(cx * dpr) - width / 2,
                                   qRound(displayTop * dpr)),
                            dpr);
        g.kioskDisplay = this->ticket_.bounds();
        g.kiosk = g.kioskDisplay.adjusted(-6, -6, 6, 6).united(g.kioskBody);
    }
    else
    {
        this->ticket_.clear();
    }
}

void DepartureScene::layoutBanks()
{
    auto &g = this->geo_;
    const double dpr = g.dpr;
    g.signInHousing = false;
    this->sign_.clear();

    // Station name sign in the free part of the tab strip, right of the tab
    // names. It shrinks to fit, and moves onto the board when there's no room.
    if (g.tabs.isEmpty() || g.tabs.height() < 16)
    {
        g.signInHousing = g.hasBoard;
        return;
    }
    const double right = (g.hasBoard ? g.column.right() : g.win.width()) - 8;
    double left = g.hasBoard ? g.column.left() + 8
                             : right - std::min(g.win.width() * 0.35, 420.0);
    if (!g.tabLabels.isEmpty())
    {
        left = std::max(left, g.tabLabels.right() + 14);
    }
    constexpr int WANT_COLS = 12;
    constexpr int MIN_COLS = 6;
    const int gap = std::max(1, qRound(1.5 * dpr));
    const double room = (right - left) * dpr;
    const double maxH = (std::min(g.tabs.height(), 30.0) - 8) * dpr;
    const double minH = 12 * dpr;
    const double fitH = ((room + gap) / WANT_COLS - gap) / 0.68;
    const int h = qRound(std::max(minH, std::min(maxH, fitH)));
    const int w = qRound(h * 0.68);
    const int cols =
        std::min(20, static_cast<int>((room + gap) / std::max(1, w + gap)));
    if (room <= 0 || maxH < minH || cols < MIN_COLS)
    {
        g.signInHousing = g.hasBoard;
        return;
    }
    const int total = cols * (w + gap) - gap;
    this->sign_.setup(cols, 1, w, h, gap, 0,
                      QPoint(qRound(right * dpr) - total,
                             qRound(g.tabs.center().y() * dpr - h / 2.0)),
                      dpr);
}

void DepartureScene::layoutLamps()
{
    const auto &g = this->geo_;
    const double W = g.win.width();
    const int n = std::clamp(static_cast<int>(W / 200), 4, 10);
    const double hang = g.topBottom > 20 ? g.topBottom : 40;
    // Bulbs hang below the tab names, never among them
    const double labelsBottom = g.tabLabels.isEmpty()
                                    ? (g.tabs.isEmpty() ? 0.0 : g.tabs.bottom() - 6)
                                    : g.tabLabels.bottom();
    std::vector<Lamp> lamps;
    for (int i = 0; i < n; ++i)
    {
        Lamp lamp;
        const double x = W * (i + 0.5) / n + (hash01(i, 11) - 0.5) * W / n * 0.35;
        lamp.size = 0.8 + 0.45 * hash01(i, 12);
        const double lo =
            std::max(hang * 0.6, labelsBottom + 4 + 10 * lamp.size);
        const double hi = std::max(lo, hang * 0.85);
        lamp.bulb = QPointF(x, lerp(lo, hi, hash01(i, 13)));
        if (i < static_cast<int>(this->lamps_.size()))
        {
            const auto &old = this->lamps_[static_cast<size_t>(i)];
            lamp.level = old.level;
            lamp.target = old.target;
            lamp.swell = old.swell;
        }
        else if (!this->live_)
        {
            lamp.level = 0.12;
            lamp.target = 0.12;
        }
        lamps.push_back(lamp);
    }
    this->lamps_ = std::move(lamps);
}

// Static art -------------------------------------------------------------------

void DepartureScene::buildStatic()
{
    const auto &g = this->geo_;
    this->static_ = QImage(std::max(1, qRound(g.win.width() * g.dpr)),
                           std::max(1, qRound(g.win.height() * g.dpr)),
                           QImage::Format_RGB32);
    this->static_.setDevicePixelRatio(g.dpr);
    QPainter p(&this->static_);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    this->paintHall(p);
    this->paintPlatform(p);
    this->paintVeil(p);
    // Calm the roof behind the tab names, with a soft edge
    if (!g.tabLabels.isEmpty())
    {
        const auto veil = [&](double grow, int alpha) {
            p.fillRect(g.tabLabels.adjusted(-grow, -grow, grow, grow),
                       QColor(SURFACE.red(), SURFACE.green(), SURFACE.blue(), alpha));
        };
        veil(8, 30);
        veil(4, 35);
        veil(0, 45);
    }
    this->paintLampFittings(p);
    if (g.hasBoard)
    {
        this->paintHousing(p);
        this->paintClockFace(p);
        this->paintConcourse(p);
    }
    this->paintSignPlate(p);
}

void DepartureScene::paintHall(QPainter &p) const
{
    const auto &g = this->geo_;
    const double W = g.win.width();
    const double H = g.win.height();

    QLinearGradient air(0, 0, 0, H);
    air.setColorAt(0.0, QColor(9, 13, 23));
    air.setColorAt(0.35, QColor(11, 14, 22));
    air.setColorAt(0.65, QColor(15, 15, 19));
    air.setColorAt(1.0, QColor(9, 9, 11));
    p.fillRect(QRectF(0, 0, W, H), air);

    // Night sky through the glass roof
    QRadialGradient sky(QPointF(g.vp.x(), -H * 0.1), std::max(W, H) * 0.8);
    sky.setColorAt(0.0, QColor(50, 72, 108, 200));
    sky.setColorAt(0.45, QColor(28, 42, 68, 90));
    sky.setColorAt(1.0, QColor(26, 38, 62, 0));
    p.fillRect(QRectF(0, 0, W, H), sky);

    // The roof is a barrel vault of iron trusses receding to the far end
    const double rx0 = W * 0.64;
    const double ry0 = g.springY + H * 0.32;
    constexpr int ARCHES = 9;
    constexpr double RATIO = 0.76;
    const double sEnd = std::pow(RATIO, ARCHES + 1);
    const auto centerY = [&](double s) {
        return g.vp.y() + (g.springY - g.vp.y()) * s;
    };
    const auto floorAt = [&](double s) {
        return g.vp.y() + (H - g.vp.y()) * s;
    };

    // The far gable: a glazed end screen with the city's glow behind it
    {
        const double cy = centerY(sEnd);
        const double rx = rx0 * sEnd;
        const double ry = ry0 * sEnd;
        const QRectF box(g.vp.x() - rx, cy - ry, rx * 2, ry * 2);
        QPainterPath gable;
        gable.arcMoveTo(box, 180);
        gable.arcTo(box, 180, -180);
        gable.lineTo(g.vp.x() + rx, floorAt(sEnd));
        gable.lineTo(g.vp.x() - rx, floorAt(sEnd));
        gable.closeSubpath();
        QRadialGradient city(QPointF(g.vp.x(), cy), ry * 1.3);
        city.setColorAt(0, QColor(34, 46, 66));
        city.setColorAt(1, QColor(17, 23, 35));
        p.fillPath(gable, city);
        p.setPen(QPen(QColor(9, 12, 19, 200), 1.0));
        for (int i = 1; i < 8; ++i)
        {
            const double a = i / 8.0 * TAU / 2;
            p.drawLine(QPointF(g.vp.x(), cy),
                       QPointF(g.vp.x() + rx * std::cos(a), cy - ry * std::sin(a)));
        }
        p.drawLine(QPointF(g.vp.x() - rx, cy), QPointF(g.vp.x() + rx, cy));
        // City glow spilling into the far end of the hall
        drawGlow(p, QPointF(g.vp.x(), cy), ry * 3.2, QColor(40, 56, 84), 0.35);
    }

    // Longitudinal glazing bars from the far end to the nearest truss
    {
        const double cyF = centerY(sEnd);
        const double cyN = centerY(1.0);
        p.setPen(QPen(QColor(6, 9, 15, 200), 1.0));
        for (int i = 1; i < 16; ++i)
        {
            const double a = i / 16.0 * TAU / 2;
            p.drawLine(QPointF(g.vp.x() + rx0 * sEnd * std::cos(a),
                               cyF - ry0 * sEnd * std::sin(a)),
                       QPointF(g.vp.x() + rx0 * std::cos(a),
                               cyN - ry0 * std::sin(a)));
        }
    }

    // Curved purlins between the trusses divide the glass into panes
    p.setPen(QPen(QColor(6, 9, 15, 150), 1.0));
    p.setBrush(Qt::NoBrush);
    for (int k = 0; k < ARCHES * 2; ++k)
    {
        const double s = std::pow(RATIO, (k + 0.5) / 2.0);
        const double cy = centerY(s);
        const QRectF box(g.vp.x() - rx0 * s, cy - ry0 * s, rx0 * s * 2, ry0 * s * 2);
        QPainterPath arc;
        arc.arcMoveTo(box, 160);
        arc.arcTo(box, 160, -140);
        p.drawPath(arc);
    }

    // Floor: platforms and tracks running away to the far end
    {
        const double yFar = floorAt(sEnd);
        p.setPen(QPen(QColor(24, 25, 30), 1.0));
        for (int i = -7; i <= 7; ++i)
        {
            const double xNear = g.vp.x() + i * W * 0.15;
            const double xFar = g.vp.x() + (xNear - g.vp.x()) * sEnd;
            p.drawLine(QPointF(xFar, yFar), QPointF(xNear, H));
        }
        p.setPen(QPen(QColor(22, 23, 28), 1.0));
        for (int k = 1; k <= ARCHES; ++k)
        {
            const double s = std::pow(RATIO, k);
            const double y = floorAt(s);
            p.drawLine(QPointF(g.vp.x() - rx0 * s, y),
                       QPointF(g.vp.x() + rx0 * s, y));
        }
    }

    // Trusses and their columns, far to near, fading into the haze
    for (int k = ARCHES; k >= 0; --k)
    {
        const double s = std::pow(RATIO, k);
        const double cy = centerY(s);
        const double rx = rx0 * s;
        const double ry = ry0 * s;
        const double fog = 1 - std::pow(s, 0.55);
        const auto color =
            mixColor(QColor(4, 6, 10), QColor(30, 38, 54), fog * 0.85);
        const double depth = H * 0.05 * s;
        paintTruss(p, QPointF(g.vp.x(), cy), rx, ry, depth, color,
                   std::max(0.8, 2.4 * s));
        // Lamp light catches the underside of the inner chord
        {
            const QRectF inner(g.vp.x() - rx, cy - ry, rx * 2, ry * 2);
            QPainterPath chord;
            chord.arcMoveTo(inner, 180);
            chord.arcTo(inner, 180, -180);
            p.setPen(QPen(QColor(120, 84, 48, qRound(70 * std::pow(s, 0.4))),
                          std::max(0.6, 1.1 * s)));
            p.drawPath(chord.translated(0, std::max(0.8, 1.6 * s)));
        }
        const double pw = std::max(1.5, W * 0.014 * s);
        const double floorY = floorAt(s);
        for (const int side : {-1, 1})
        {
            const double x = g.vp.x() + side * (rx + depth * 0.5);
            p.fillRect(QRectF(x - pw / 2, cy - depth * 0.2, pw,
                              floorY - cy + depth * 0.2),
                       color);
            p.fillRect(QRectF(x - pw, cy - depth * 0.4, pw * 2, depth * 0.3),
                       color);
        }
    }
}

void DepartureScene::paintPlatform(QPainter &p) const
{
    const auto &g = this->geo_;
    const double W = g.win.width();
    const double H = g.win.height();
    const double top = g.platTop;
    if (top >= H)
    {
        return;
    }
    QLinearGradient face(0, top, 0, H);
    face.setColorAt(0, QColor(17, 17, 20));
    face.setColorAt(1, QColor(8, 8, 10));
    p.fillRect(QRectF(0, top, W, H - top), face);
    // Stone coping and a worn safety line along the edge
    p.fillRect(QRectF(0, top, W, 1.5), QColor(62, 58, 52));
    p.fillRect(QRectF(0, top + 2.5, W, 1.5), QColor(150, 108, 40, 150));
    // Sleepers and rails at the very bottom
    const double rail1 = H - 5.5;
    const double rail2 = H - 2.2;
    for (double x = 3; x < W; x += 11)
    {
        p.fillRect(QRectF(x, rail1 - 0.5, 5, rail2 - rail1 + 2), QColor(13, 12, 12));
    }
    p.fillRect(QRectF(0, rail1, W, 1.3), QColor(48, 54, 64));
    p.fillRect(QRectF(0, rail2, W, 1.6), QColor(58, 64, 76));
}

void DepartureScene::paintVeil(QPainter &p) const
{
    const auto &g = this->geo_;
    for (const auto &chat : g.chats)
    {
        if (chat.isEmpty())
        {
            continue;
        }
        p.save();
        if (g.hasBoard && g.overChat)
        {
            QRegion region(chat.toAlignedRect());
            region -= QRegion(g.column.toAlignedRect());
            p.setClipRegion(region);
        }
        p.fillRect(chat, QColor(SURFACE.red(), SURFACE.green(), SURFACE.blue(), 125));
        p.restore();
    }
}

void DepartureScene::paintLampFittings(QPainter &p) const
{
    for (const auto &lamp : this->lamps_)
    {
        const double s = lamp.size;
        const QPointF b = lamp.bulb;
        p.setPen(QPen(QColor(3, 4, 7), std::max(1.0, s)));
        p.drawLine(QPointF(b.x(), 0), QPointF(b.x(), b.y() - 9 * s));
        QPainterPath shade;
        shade.moveTo(b.x() - 3 * s, b.y() - 10 * s);
        shade.lineTo(b.x() + 3 * s, b.y() - 10 * s);
        shade.quadTo(b.x() + 6 * s, b.y() - 5 * s, b.x() + 10.5 * s, b.y() - 1.5 * s);
        shade.lineTo(b.x() - 10.5 * s, b.y() - 1.5 * s);
        shade.quadTo(b.x() - 6 * s, b.y() - 5 * s, b.x() - 3 * s, b.y() - 10 * s);
        shade.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 9, 12));
        p.drawPath(shade);
        p.setPen(QPen(QColor(150, 104, 48, 170), 1.0));
        p.drawLine(QPointF(b.x() - 10 * s, b.y() - 1.5 * s),
                   QPointF(b.x() + 10 * s, b.y() - 1.5 * s));
    }
}

void DepartureScene::paintHousing(QPainter &p) const
{
    const auto &g = this->geo_;
    const auto h = g.housing;

    // Hanging rods up into the roof
    p.setPen(QPen(QColor(4, 5, 8), 2.0));
    for (const double fx : {0.12, 0.88})
    {
        const double x = h.left() + h.width() * fx;
        p.drawLine(QPointF(x, 0), QPointF(x, h.top()));
    }

    // Cabinet
    QLinearGradient body(h.topLeft(), h.bottomLeft());
    body.setColorAt(0, QColor(18, 19, 23));
    body.setColorAt(1, QColor(9, 10, 12));
    p.setPen(Qt::NoPen);
    p.setBrush(body);
    p.drawRect(h);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(42, 42, 46), 1.0));
    p.drawRect(h.adjusted(0.5, 0.5, -0.5, -0.5));
    p.fillRect(QRectF(h.left() + 1, h.top() + 1, h.width() - 2, 1),
               QColor(120, 108, 88, 120));
    p.setPen(QPen(QColor(0, 0, 0, 180), 1.0));
    p.drawRect(h.adjusted(3.5, 3.5, -3.5, -3.5));

    // Rivets
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(46, 45, 43));
    const int count = std::max(2, static_cast<int>(h.width() / 26));
    for (int i = 0; i <= count; ++i)
    {
        const double x = h.left() + 6 + (h.width() - 12) * i / count;
        p.drawEllipse(QPointF(x, h.top() + 2.2), 1.0, 1.0);
        p.drawEllipse(QPointF(x, h.bottom() - 2.2), 1.0, 1.0);
    }

    // Enamel title left of the clock; small when the station name sits under it
    const QRectF title = g.titleLabel;
    const auto text = QStringLiteral("DEPARTURES");
    double titleW = 0;
    if (title.width() > 40)
    {
        auto font = g.signInHousing
                        ? labelFont(std::clamp(title.height(), 7.0, 11.0),
                                    QFont::DemiBold, 1.6)
                        : labelFont(std::min(g.titleH * 0.42, 20.0),
                                    QFont::DemiBold, 2.0);
        while (QFontMetricsF(font).horizontalAdvance(text) > title.width() &&
               font.pixelSize() > 7)
        {
            font.setPixelSize(font.pixelSize() - 1);
        }
        p.setFont(font);
        p.setPen(g.signInHousing ? withAlpha(ENAMEL, 0.75) : ENAMEL);
        p.drawText(title, Qt::AlignVCenter | Qt::AlignLeft, text);
        titleW = QFontMetricsF(font).horizontalAdvance(text);
    }
    p.fillRect(QRectF(h.left() + 6, h.top() + g.titleH, h.width() - 12, 1),
               QColor(38, 38, 42));
    // Enamel plate between the title and the clock
    const QRectF plate(title.left() + titleW + 16, title.top(),
                       title.right() - (title.left() + titleW + 16), title.height());
    if (titleW > 0 && plate.width() > 50)
    {
        auto font = labelFont(std::min(g.titleH * 0.26, 11.0), QFont::Medium, 1.5);
        if (g.signInHousing)
        {
            font.setPixelSize(std::max(6, qRound(title.height() * 0.8)));
        }
        const auto platforms = QStringLiteral("PLATFORMS 1\u201312");
        while (QFontMetricsF(font).horizontalAdvance(platforms) > plate.width() &&
               font.pixelSize() > 7)
        {
            font.setPixelSize(font.pixelSize() - 1);
        }
        // Left off when it would not fit
        if (QFontMetricsF(font).horizontalAdvance(platforms) <= plate.width())
        {
            p.setFont(font);
            p.setPen(withAlpha(ENAMEL, 0.6));
            p.drawText(plate, Qt::AlignVCenter | Qt::AlignRight, platforms);
        }
    }
    if (!this->hype_.empty())
    {
        p.fillRect(this->hype_.bounds().adjusted(-3, -2, 3, 2), QColor(3, 3, 4));
    }

    this->paintColumnLabels(p);

    // Dark well behind the flaps, and the indicator lamp sockets
    p.fillRect(this->board_.bounds().adjusted(-3, -2, 3, 2), QColor(3, 3, 4));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(22, 22, 24));
    for (int r = 0; r < this->board_.rows(); ++r)
    {
        p.drawEllipse(QPointF(h.left() + 7, this->board_.rowRect(r).center().y()),
                      1.8, 1.8);
    }

    // NEXT DEPARTURE along the foot: a rule, its label, a well and a socket
    if (!this->next_.empty())
    {
        const auto row = this->next_.bounds();
        p.fillRect(QRectF(h.left() + 6, g.nextLabelRow.top() - 4, h.width() - 12, 1),
                   QColor(38, 38, 42));
        const auto font = labelFont(
            std::clamp(g.nextLabelRow.height() * 0.72, 7.0, 11.0), QFont::DemiBold,
            1.2);
        const QFontMetricsF metrics(font);
        p.setFont(font);
        p.setPen(LABEL_AMBER);
        p.drawText(QPointF(row.left(), g.nextLabelRow.center().y() +
                                           metrics.capHeight() / 2),
                   QStringLiteral("NEXT DEPARTURE"));
        p.fillRect(row.adjusted(-3, -2, 3, 2), QColor(3, 3, 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(22, 22, 24));
        p.drawEllipse(QPointF(h.left() + 7, row.center().y()), 1.8, 1.8);
    }
}

void DepartureScene::paintColumnLabels(QPainter &p) const
{
    const auto &g = this->geo_;
    if (this->board_.empty())
    {
        return;
    }
    const auto font = labelFont(std::clamp(g.labelsRow.height() * 0.72, 7.0, 11.0),
                                QFont::Medium, 1.0);
    const QFontMetricsF metrics(font);
    p.setFont(font);
    p.setPen(LABEL_AMBER);
    const auto label = [&](int col, int width, const QString &full,
                           const QString &shortText) {
        if (col < 0)
        {
            return;
        }
        const auto tile = this->board_.tileRect(0, col);
        const double room =
            this->board_.tileRect(0, col + width - 1).right() - tile.left() + 4;
        const auto &text =
            metrics.horizontalAdvance(full) <= room ? full : shortText;
        p.drawText(QPointF(tile.left(), g.labelsRow.center().y() +
                                            metrics.capHeight() / 2),
                   text);
    };
    label(g.timeCol, g.timeW, QStringLiteral("TIME"), QStringLiteral("TIME"));
    label(g.destCol, g.destW, QStringLiteral("DESTINATION"),
          QStringLiteral("TO"));
    if (g.platW > 0)
    {
        label(g.platCol, g.platW, QStringLiteral("PL"), QStringLiteral("PL"));
    }
    label(g.remarkCol, g.remarkW, QStringLiteral("REMARKS"),
          QStringLiteral("NOTE"));
}

void DepartureScene::paintClockFace(QPainter &p) const
{
    const auto &g = this->geo_;
    const QPointF c = g.clockC;
    const double R = g.clockR;
    drawGlow(p, c, R * 1.5, QColor(255, 214, 160), 0.12);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(16, 17, 20));
    p.drawEllipse(c, R, R);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(96, 86, 70), std::max(1.0, R * 0.035)));
    p.drawEllipse(c, R * 0.92, R * 0.92);
    QRadialGradient face(c, R * 0.88);
    face.setColorAt(0, QColor(236, 228, 208));
    face.setColorAt(0.8, QColor(218, 208, 186));
    face.setColorAt(1, QColor(170, 158, 136));
    p.setPen(Qt::NoPen);
    p.setBrush(face);
    p.drawEllipse(c, R * 0.86, R * 0.86);
    for (int i = 0; i < 60; ++i)
    {
        const double a = i * TAU / 60;
        const QPointF dir(std::sin(a), -std::cos(a));
        const bool hour = i % 5 == 0;
        p.setPen(QPen(QColor(22, 22, 24),
                      hour ? R * 0.075 : std::max(0.6, R * 0.022),
                      Qt::SolidLine, Qt::FlatCap));
        p.drawLine(c + dir * (hour ? R * 0.6 : R * 0.73), c + dir * R * 0.8);
    }
}

void DepartureScene::paintConcourse(QPainter &p) const
{
    // Lamp light pooled on the concourse floor, so passengers stand out
    const auto &g = this->geo_;
    const auto &area = g.crowd;
    if (area.height() < 20)
    {
        return;
    }
    p.save();
    p.setClipRect(area.adjusted(0, -10, 0, 0));
    QRadialGradient pool(QPointF(area.center().x(), g.platTop),
                         std::max(area.width(), area.height()) * 0.75);
    pool.setColorAt(0, QColor(72, 52, 34, 150));
    pool.setColorAt(0.5, QColor(52, 38, 28, 70));
    pool.setColorAt(1, QColor(40, 30, 24, 0));
    p.fillRect(area, pool);
    p.restore();
}

void DepartureScene::paintKioskBody(QPainter &p) const
{
    const auto &g = this->geo_;
    if (!g.hasKiosk)
    {
        return;
    }
    // Painted into its own image at the kiosk's window position
    const auto k = g.kioskBody;
    const auto display = g.kioskDisplay;
    const auto box = display.adjusted(-5, -5, 5, 5);
    // Post and the fare counter's box
    p.fillRect(QRectF(k.center().x() - 2.5, box.bottom(), 5, k.top() - box.bottom() + 2),
               QColor(14, 15, 18));
    p.setPen(QPen(QColor(110, 84, 46), 1.0));
    p.setBrush(QColor(12, 13, 16));
    p.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
    p.fillRect(display.adjusted(-2, -2, 2, 2), QColor(2, 2, 3));
    QPainterPath body;
    body.moveTo(k.left(), k.bottom());
    body.lineTo(k.left(), k.top() + k.width() * 0.18);
    body.lineTo(k.right(), k.top());
    body.lineTo(k.right(), k.bottom());
    body.closeSubpath();
    QLinearGradient shade(k.topLeft(), k.topRight());
    shade.setColorAt(0, QColor(13, 14, 17));
    shade.setColorAt(1, QColor(24, 25, 30));
    p.fillPath(body, shade);
    p.setPen(QPen(QColor(50, 47, 42), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(body);

    // Label, ticket slot and a little keypad
    const double face = k.top() + k.width() * 0.18;
    const auto font = labelFont(std::clamp(k.width() * 0.12, 6.0, 9.0),
                                QFont::DemiBold, 1.0);
    p.setFont(font);
    p.setPen(LABEL_AMBER);
    p.drawText(QRectF(k.left(), face + 4, k.width(), 12),
               Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("TICKETS"));
    const double slotY = face + 20;
    if (slotY + 20 < k.bottom())
    {
        p.fillRect(QRectF(k.center().x() - k.width() * 0.26, slotY,
                          k.width() * 0.52, 3),
                   QColor(1, 1, 2));
        p.fillRect(QRectF(k.center().x() - k.width() * 0.26, slotY + 3,
                          k.width() * 0.52, 1),
                   QColor(70, 64, 56));
        for (int i = 0; i < 6; ++i)
        {
            const double kx = k.center().x() - 9 + (i % 3) * 7;
            const double ky = slotY + 10 + (i / 3) * 6;
            if (ky + 4 < k.bottom())
            {
                p.fillRect(QRectF(kx, ky, 4, 3), QColor(36, 36, 40));
            }
        }
    }
}

void DepartureScene::paintSignPlate(QPainter &p) const
{
    if (this->sign_.empty())
    {
        return;
    }
    const auto plate = this->sign_.bounds().adjusted(-4, -2, 4, 2);
    p.fillRect(plate, QColor(9, 10, 12));
    p.setPen(QPen(QColor(48, 45, 40), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(plate.adjusted(0.5, 0.5, -0.5, -0.5));
}

void DepartureScene::buildCaches()
{
    const auto &g = this->geo_;
    const double dpr = g.dpr;

    // Warm light from the lamps above the board, darker towards its foot
    this->boardLight_ = QImage();
    if (g.hasBoard)
    {
        this->boardLight_ = makeLayer(g.housing.size(), dpr);
        QPainter p(&this->boardLight_);
        const QRectF r(QPointF(0, 0), g.housing.size());
        QRadialGradient light(QPointF(r.width() * 0.5, -r.height() * 0.1),
                              r.height() * 0.95);
        light.setColorAt(0, QColor(255, 190, 110, 40));
        light.setColorAt(0.6, QColor(255, 170, 90, 12));
        light.setColorAt(1, QColor(255, 170, 90, 0));
        p.fillRect(r, light);
        QLinearGradient foot(0, r.height() * 0.55, 0, r.height());
        foot.setColorAt(0, QColor(0, 0, 0, 0));
        foot.setColorAt(1, QColor(0, 0, 0, 70));
        p.fillRect(r, foot);
    }

    // Light streak of a passing train, head at one end
    {
        constexpr int SW = 256;
        constexpr int SH = 16;
        QImage right(SW, SH, QImage::Format_ARGB32_Premultiplied);
        QImage left(SW, SH, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < SH; ++y)
        {
            const double vy = std::exp(-std::pow((y - (SH - 1) / 2.0) / 4.2, 2));
            auto *rowR = reinterpret_cast<QRgb *>(right.scanLine(y));
            auto *rowL = reinterpret_cast<QRgb *>(left.scanLine(y));
            for (int x = 0; x < SW; ++x)
            {
                const double t = x / double(SW - 1);
                const double a = clamp01(
                    (std::pow(t, 2.4) + 0.7 * std::exp(-std::pow((1 - t) * 40, 2))) *
                    vy);
                const auto px = qRgba(qRound(TRAIN_LIGHT.red() * a),
                                      qRound(TRAIN_LIGHT.green() * a),
                                      qRound(TRAIN_LIGHT.blue() * a),
                                      qRound(255 * a));
                rowR[x] = px;
                rowL[SW - 1 - x] = px;
            }
        }
        this->streakRight_ = right;
        this->streakLeft_ = left;
    }

    // The lit windows of a passing train, engine at the right
    if (this->trainRight_.isNull())
    {
        constexpr int TW = 640;
        constexpr int TH = 14;
        QImage train(TW, TH, QImage::Format_ARGB32_Premultiplied);
        train.fill(Qt::transparent);
        {
            QPainter p(&train);
            p.setPen(Qt::NoPen);
            constexpr int ENGINE = 46;
            for (int x = TW - ENGINE - 8; x > 0; x -= 62)
            {
                for (int w = 0; w < 6; ++w)
                {
                    const int wx = x - 9 - w * 9;
                    if (wx < 0)
                    {
                        break;
                    }
                    const double t = double(wx) / TW;
                    const int a = qRound(255 * std::pow(t, 1.3));
                    p.setBrush(QColor(255, 210, 146, a / 3));
                    p.drawRect(QRectF(wx, 3, 8, 8));
                    p.setBrush(QColor(255, 214, 152, a));
                    p.drawRect(QRectF(wx + 0.5, 4.5, 7, 5));
                }
            }
            p.setBrush(QColor(255, 244, 222));
            p.drawRect(QRectF(TW - 7, 5, 7, 4));
        }
        QImage left(TW, TH, QImage::Format_ARGB32_Premultiplied);
        left.fill(Qt::transparent);
        {
            QPainter p(&left);
            p.translate(TW, 0);
            p.scale(-1, 1);
            p.drawImage(0, 0, train);
        }
        this->trainRight_ = train;
        this->trainLeft_ = left;
    }

    // Headlight beam, apex on the left
    if (this->beam_.isNull())
    {
        constexpr int BW = 384;
        constexpr int BH = 192;
        QImage beam(BW, BH, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < BH; ++y)
        {
            auto *row = reinterpret_cast<QRgb *>(beam.scanLine(y));
            const double dy = (y - (BH - 1) / 2.0) / ((BH - 1) / 2.0);
            for (int x = 0; x < BW; ++x)
            {
                const double t = x / double(BW - 1);
                const double spread = 0.06 + 0.94 * t;
                const double a =
                    std::exp(-std::pow(dy / spread, 2) * 2.4) *
                    std::pow(1 - t, 1.3) * smoothstep(0.0, 0.05, t);
                row[x] = qRgba(qRound(255 * a), qRound(236 * a),
                               qRound(204 * a), qRound(255 * a));
            }
        }
        this->beam_ = beam;
    }

    this->kioskImage_ = QImage();
    if (g.hasKiosk)
    {
        const auto box = g.kiosk.adjusted(-6, -6, 6, 6);
        this->kioskImage_ = makeLayer(box.size(), dpr);
        QPainter p(&this->kioskImage_);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-box.topLeft());
        this->paintKioskBody(p);
    }

    // Passenger silhouettes at a few sizes, with a warm rim from the lamps.
    // Farther passengers fade into the haze of the hall.
    this->people_.clear();
    if (g.hasBoard && g.personNear >= 12)
    {
        for (int b = 0; b < PERSON_BUCKETS; ++b)
        {
            this->bucketHeights_[static_cast<size_t>(b)] =
                lerp(g.personFar, g.personNear, b / double(PERSON_BUCKETS - 1));
        }
        for (int v = 0; v < PERSON_VARIANTS; ++v)
        {
            const auto path = personPath(v);
            for (int b = 0; b < PERSON_BUCKETS; ++b)
            {
                const double h = this->bucketHeights_[static_cast<size_t>(b)];
                const double w = h * 0.8;
                auto image = makeLayer(QSizeF(w, h + 1), dpr);
                QPainter p(&image);
                p.setRenderHint(QPainter::Antialiasing);
                p.translate(w / 2, h);
                p.scale(h, h);
                p.setPen(Qt::NoPen);
                const double fog = 1 - b / double(PERSON_BUCKETS - 1);
                p.setBrush(QColor(90, 62, 34, qRound(210 * (1 - 0.6 * fog))));
                p.drawPath(path.translated(0.25 / h, -1.1 / h));
                p.setBrush(mixColor(QColor(3, 4, 7), QColor(34, 30, 32), fog * 0.85));
                p.drawPath(path);
                p.end();
                this->people_.push_back(image);
            }
        }
    }
}

// Rows ---------------------------------------------------------------------------

int DepartureScene::clockMinutes() const
{
    return CLOCK_START_MINUTES +
           static_cast<int>(std::floor((this->now_ + CLOCK_START_SECONDS) / 60)) +
           this->ticks_;
}

BoardRow DepartureScene::chatterRow(const QString &name, int rowIndex)
{
    BoardRow row;
    row.kind = RowKind::Chatter;
    row.id = this->nextRowId_++;
    row.minutes = this->clockMinutes() + 2 + std::max(0, rowIndex) * 2 +
                  static_cast<int>(this->rng_.next() * 3);
    row.dest = name;
    row.plat = 1 + static_cast<int>(this->rng_.next() * 12);
    const auto &remark =
        REMARKS[static_cast<size_t>(this->rng_.next() * REMARKS.size()) %
                REMARKS.size()];
    row.remark = QString::fromLatin1(remark.text);
    row.remarkInk = remark.ink;
    row.author = name.toLower();
    row.born = this->now_;
    this->formatRow(row);
    return row;
}

BoardRow DepartureScene::freeRow(RowKind kind, const QStringList &candidates,
                                 int ink)
{
    BoardRow row;
    row.kind = kind;
    row.id = this->nextRowId_++;
    row.free = candidates;
    row.ink = static_cast<std::uint8_t>(ink);
    row.born = this->now_;
    this->formatRow(row);
    return row;
}

void DepartureScene::formatRow(BoardRow &row) const
{
    const auto &g = this->geo_;
    const int cols = this->board_.cols();
    row.cells = Cells(cols);
    if (cols <= 0 || row.kind == RowKind::Blank)
    {
        return;
    }
    const bool hasRemark = !row.remark.isEmpty();
    if (!row.free.isEmpty())
    {
        const int width = hasRemark ? g.remarkCol - 1 : cols;
        QString text = row.free.back();
        for (const auto &candidate : row.free)
        {
            if (candidate.size() <= width)
            {
                text = candidate;
                break;
            }
        }
        const int len = std::min(width, static_cast<int>(text.size()));
        const int start = row.center ? std::max(0, (width - len) / 2) : 0;
        row.cells.put(start, text, width - start, row.ink);
    }
    else
    {
        if (row.minutes >= 0)
        {
            row.cells.put(g.timeCol, clockText(row.minutes), g.timeW, row.ink);
        }
        row.cells.put(g.destCol, row.dest, g.destW, row.ink);
        if (g.platW > 0 && row.plat > 0)
        {
            row.cells.put(g.platCol, QString::number(row.plat), g.platW, row.ink,
                          true);
        }
    }
    if (hasRemark && !row.remarkHidden)
    {
        auto remark = row.remark;
        if (remark.size() > g.remarkW)
        {
            // Shorter wording for narrow boards
            if (remark == QStringLiteral("CANCELLED"))
            {
                remark = QStringLiteral("CANCELED");
            }
            else if (remark == QStringLiteral("LAST CALL"))
            {
                remark = QStringLiteral("CLOSING");
            }
        }
        row.cells.put(g.remarkCol, remark, g.remarkW, row.remarkInk);
    }
}

void DepartureScene::applyRow(int r, double delay, double colStagger,
                              int riffle, double stepDur)
{
    if (r < 0 || r >= static_cast<int>(this->rows_.size()) ||
        this->board_.empty())
    {
        return;
    }
    if (this->reduced_)
    {
        riffle = 0;
        colStagger *= 0.5;
    }
    const auto &cells = this->rows_[static_cast<size_t>(r)].cells;
    const int cols = std::min(this->board_.cols(), cells.size());
    for (int c = 0; c < cols; ++c)
    {
        this->board_.target(r, c, cells.glyph[static_cast<size_t>(c)],
                            cells.ink[static_cast<size_t>(c)],
                            delay + c * colStagger, riffle, stepDur, this->rng_);
    }
}

void DepartureScene::snapRows()
{
    const int rows = std::min(this->board_.rows(),
                              static_cast<int>(this->rows_.size()));
    for (int r = 0; r < rows; ++r)
    {
        const auto &cells = this->rows_[static_cast<size_t>(r)].cells;
        for (int c = 0; c < std::min(this->board_.cols(), cells.size()); ++c)
        {
            this->board_.snap(r, c, cells.glyph[static_cast<size_t>(c)],
                              cells.ink[static_cast<size_t>(c)]);
        }
    }
}

void DepartureScene::insertTop(BoardRow row, double delay)
{
    if (this->rows_.empty())
    {
        return;
    }
    row.born = this->now_;
    this->formatRow(row);
    this->rows_.pop_back();
    this->rows_.insert(this->rows_.begin(), std::move(row));
    // The rows below are pushed down by a wave of flips from the top
    const int n = static_cast<int>(this->rows_.size());
    const double rowStagger = this->reduced_ ? 0.03 : 0.065;
    for (int r = 0; r < n; ++r)
    {
        this->applyRow(r, delay + r * rowStagger, 0.011, 2, 0.045);
    }
    this->holdBoardUntil_ =
        std::max(this->holdBoardUntil_, this->now_ + delay + n * rowStagger + 0.5);
}

void DepartureScene::replaceRow(int r, BoardRow row, double delay,
                                double colStagger, int riffle)
{
    if (r < 0 || r >= static_cast<int>(this->rows_.size()))
    {
        return;
    }
    this->formatRow(row);
    this->rows_[static_cast<size_t>(r)] = std::move(row);
    this->applyRow(r, delay, colStagger, riffle, 0.05);
}

int DepartureScene::findRow(const QString &author) const
{
    if (author.isEmpty())
    {
        return -1;
    }
    for (int r = 0; r < static_cast<int>(this->rows_.size()); ++r)
    {
        if (this->rows_[static_cast<size_t>(r)].author == author)
        {
            return r;
        }
    }
    return -1;
}

int DepartureScene::findRowId(int id) const
{
    for (int r = 0; r < static_cast<int>(this->rows_.size()); ++r)
    {
        if (this->rows_[static_cast<size_t>(r)].id == id)
        {
            return r;
        }
    }
    return -1;
}

int DepartureScene::pickRow(bool preferBottom)
{
    std::vector<int> options;
    for (int r = 0; r < static_cast<int>(this->rows_.size()); ++r)
    {
        const auto &row = this->rows_[static_cast<size_t>(r)];
        const double age = this->now_ - row.born;
        bool ordinary = false;
        switch (row.kind)
        {
            case RowKind::Blank:
            case RowKind::Chatter:
                ordinary = true;
                break;
            case RowKind::Joined:
            case RowKind::New:
                ordinary = age > 20;
                break;
            case RowKind::Cancel:
            case RowKind::Service:
                ordinary = false;
                break;
            default:
                ordinary = age > 45;
                break;
        }
        const bool hovered = this->hoverLevel_ > 0.05 &&
                             !this->hoverUser_.isEmpty() &&
                             row.author == this->hoverUser_;
        if (ordinary && !hovered)
        {
            options.push_back(r);
        }
    }
    if (options.empty())
    {
        return -1;
    }
    if (preferBottom)
    {
        return options.back();
    }
    return options[static_cast<size_t>(this->rng_.next() * options.size()) %
                   options.size()];
}

QString DepartureScene::takeName()
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        QString name;
        if (!this->waiting_.empty())
        {
            name = this->waiting_.front();
            this->waiting_.pop_front();
        }
        else if (!this->recent_.empty() && this->rng_.next() < 0.6)
        {
            name = this->recent_[static_cast<size_t>(this->rng_.next() *
                                                     this->recent_.size()) %
                                 this->recent_.size()];
        }
        else
        {
            name = QString::fromLatin1(
                PLACES[static_cast<size_t>(this->rng_.next() * PLACES.size()) %
                       PLACES.size()]);
        }
        if (this->findRow(name.toLower()) < 0)
        {
            return name;
        }
    }
    return QString::fromLatin1(
        PLACES[static_cast<size_t>(this->rng_.next() * PLACES.size()) %
               PLACES.size()]);
}

QString DepartureScene::recentName(int skip, const QString &avoid)
{
    // The skip-th distinct recent chatter, newest first
    QStringList seen;
    for (auto it = this->recent_.rbegin(); it != this->recent_.rend(); ++it)
    {
        if (it->compare(avoid, Qt::CaseInsensitive) == 0 ||
            seen.contains(*it, Qt::CaseInsensitive))
        {
            continue;
        }
        seen.append(*it);
        if (static_cast<int>(seen.size()) > skip)
        {
            return *it;
        }
    }
    return QStringLiteral("PASSENGER %1").arg(skip + 1);
}

double DepartureScene::boardLight() const
{
    const auto &g = this->geo_;
    double sum = 0;
    int n = 0;
    for (const auto &lamp : this->lamps_)
    {
        if (lamp.bulb.x() > g.column.left() - 80 &&
            lamp.bulb.x() < g.column.right() + 80)
        {
            sum += lamp.level * (1 + 0.5 * lamp.swell);
            n++;
        }
    }
    if (n == 0)
    {
        for (const auto &lamp : this->lamps_)
        {
            sum += lamp.level;
            n++;
        }
    }
    return n > 0 ? sum / n : 1.0;
}

void DepartureScene::lampSwell(double x, double radius, double amount)
{
    for (auto &lamp : this->lamps_)
    {
        const double d = std::abs(lamp.bulb.x() - x);
        if (d < radius)
        {
            lamp.swell = std::max(lamp.swell, amount * (1 - d / radius));
        }
    }
}

Cells DepartureScene::hypeCells(int level, double progress) const
{
    const int n = this->hype_.cols();
    Cells cells(n);
    if (level <= 0 || n <= 0)
    {
        return cells;
    }
    int barStart = 0;
    if (n >= 12)
    {
        cells.put(0, QStringLiteral("HYPE"), 4, INK_AMBER);
        cells.put(5, QStringLiteral("LV%1").arg(std::min(level, 99)), 4, INK_WHITE);
        barStart = level >= 10 ? 10 : 9;
    }
    else
    {
        cells.put(0, QString::number(std::min(level, 9)), 1, INK_AMBER);
        barStart = 2;
    }
    const int barN = n - barStart;
    const int filled = std::clamp(qRound(clamp01(progress) * barN), 0, barN);
    for (int i = 0; i < filled; ++i)
    {
        cells.glyph[static_cast<size_t>(barStart + i)] =
            static_cast<std::uint8_t>(G_BLOCK);
        cells.ink[static_cast<size_t>(barStart + i)] = INK_AMBER;
    }
    return cells;
}

// Simulation -------------------------------------------------------------------

std::array<FlapBank *, 7> DepartureScene::banks()
{
    return {&this->board_, &this->sign_,   &this->next_,  &this->hype_,
            &this->combo_, &this->ticket_, &this->marker_};
}

void DepartureScene::after(double delay, std::function<void()> run)
{
    if (this->timeline_.size() > 800)
    {
        return;
    }
    this->timeline_.push_back({this->now_ + delay, std::move(run)});
}

void DepartureScene::runTimeline()
{
    const auto it = std::partition(
        this->timeline_.begin(), this->timeline_.end(), [this](const Timed &t) {
            return t.at > this->now_ + 1e-9;
        });
    if (it == this->timeline_.end())
    {
        return;
    }
    std::vector<Timed> due(std::make_move_iterator(it),
                           std::make_move_iterator(this->timeline_.end()));
    this->timeline_.erase(it, this->timeline_.end());
    std::stable_sort(due.begin(), due.end(), [](const Timed &a, const Timed &b) {
        return a.at < b.at;
    });
    for (auto &item : due)
    {
        item.run();
    }
}

void DepartureScene::boot(const SceneState &state)
{
    this->booted_ = true;
    this->live_ = state.live;
    const int n = static_cast<int>(this->rows_.size());
    for (int r = 0; r < n; ++r)
    {
        auto &row = this->rows_[static_cast<size_t>(r)];
        if (row.kind == RowKind::Blank)
        {
            row = this->chatterRow(
                QString::fromLatin1(PLACES[static_cast<size_t>(r * 5 + 3) %
                                           PLACES.size()]),
                r);
            row.author.clear();
        }
        if (!state.live)
        {
            row = this->freeRow(RowKind::Service,
                                {QStringLiteral("SERVICE ENDED")}, INK_WHITE);
            row.center = true;
            this->formatRow(row);
        }
        // The board clatters into life when the theme starts
        this->applyRow(r, 0.25 + r * 0.07, 0.012, 3, 0.05);
    }
    if (!state.live)
    {
        for (auto &lamp : this->lamps_)
        {
            lamp.level = 0.12;
            lamp.target = 0.12;
        }
    }
    this->setSign(state.channel, true);
}

void DepartureScene::step(const SceneState &state, const SceneLayout &layout)
{
    this->now_ = state.t;
    this->dt_ = state.dt;
    this->reduced_ = state.reduced;
    this->ensureLayout(layout);
    if (!this->booted_)
    {
        this->boot(state);
    }
    this->runTimeline();
    if (state.channel != this->signText_)
    {
        this->setSign(state.channel, true);
    }
    this->syncDraft(state);
    this->syncHype(state);
    this->syncTicket();
    this->turnover(state);
    this->breeze(state);
    this->stepWorld(state);
    for (auto *bank : this->banks())
    {
        bank->step(state.dt);
    }

    this->flights_.erase(
        std::remove_if(this->flights_.begin(), this->flights_.end(),
                       [this](const Flight &f) {
                           return this->now_ > f.start + f.duration;
                       }),
        this->flights_.end());
    this->streaks_.erase(
        std::remove_if(this->streaks_.begin(), this->streaks_.end(),
                       [this](const Streak &s) {
                           return this->now_ > s.start + s.duration;
                       }),
        this->streaks_.end());
    this->front_ = !this->flights_.empty() ||
                   (!this->marker_.empty() &&
                    this->now_ - this->markerT_ < MARKER_TIME);
}

void DepartureScene::stepWorld(const SceneState &state)
{
    const double dt = state.dt;

    // Lamps ease towards their level; swells fade
    for (auto &lamp : this->lamps_)
    {
        const double rate = lamp.target < lamp.level ? 2.0 : 1.3;
        lamp.level += (lamp.target - lamp.level) * std::min(1.0, dt * rate);
        lamp.swell = std::max(0.0, lamp.swell - dt * 0.35);
    }

    // Row glows fade slowly; the hovered row lights up
    for (auto &row : this->rows_)
    {
        row.glow = std::max(0.0, row.glow - dt * 0.05);
    }
    const bool hoverFound =
        this->hovering_ && this->findRow(this->hoverUser_) >= 0;
    this->hoverLevel_ +=
        ((hoverFound ? 1.0 : 0.0) - this->hoverLevel_) * std::min(1.0, dt * 7);

    for (auto &glow : this->keyGlow_)
    {
        glow = std::max(0.0, glow - dt * 3);
    }

    // The station clock: the minute hand jumps when the minute turns
    const int minute = this->clockMinutes();
    if (minute != this->shownMinute_)
    {
        if (this->shownMinute_ >= 0)
        {
            this->minuteJumpT_ = this->now_;
        }
        this->shownMinute_ = minute;
    }

    // Crowd density follows the viewer count
    double target = 8;
    if (state.viewers >= 0)
    {
        const double f = std::log10(std::max(1, state.viewers));
        target = 3 + 41 * std::pow(clamp01((f - 1.5) / 2.6), 1.3);
    }
    if (!this->live_)
    {
        target *= 0.35;
    }
    if (this->crowdCount_ < 0)
    {
        this->crowdCount_ = target;
    }
    this->crowdCount_ += (target - this->crowdCount_) * std::min(1.0, dt * 0.8);

    for (auto &a : this->arrivals_)
    {
        if (a.x < a.stopAt)
        {
            a.x = std::min(a.stopAt, a.x + a.speed * dt);
        }
    }
    this->arrivals_.erase(
        std::remove_if(this->arrivals_.begin(), this->arrivals_.end(),
                       [this](const Arrival &a) {
                           return this->now_ - a.born > 26;
                       }),
        this->arrivals_.end());
}

void DepartureScene::syncDraft(const SceneState &state)
{
    if (this->now_ - this->sendT_ < 1.5)
    {
        return;
    }
    if (state.draftLength < static_cast<int>(this->draft_.size()))
    {
        this->draft_.resize(static_cast<size_t>(std::max(0, state.draftLength)));
        this->refreshNext(-1, true);
    }
}

void DepartureScene::syncHype(const SceneState &state)
{
    if (this->hype_.empty() || this->now_ < this->hypeFlipUntil_)
    {
        return;
    }
    const auto cells = this->hypeCells(state.hypeLevel, state.hypeProgress);
    int changed = 0;
    for (int c = 0; c < this->hype_.cols(); ++c)
    {
        const auto &f = this->hype_.at(0, c);
        const int g = cells.glyph[static_cast<size_t>(c)];
        const int k = cells.ink[static_cast<size_t>(c)];
        if (f.finalGlyph() != g || f.finalInk() != k)
        {
            this->hype_.target(0, c, g, k, changed * 0.03,
                               this->reduced_ ? 0 : 1, 0.04, this->rng_);
            changed++;
        }
    }
}

void DepartureScene::syncTicket()
{
    if (this->ticket_.empty())
    {
        return;
    }
    const double u = this->now_ - this->bitsT_;
    if (u < 0 || u > this->bitsDuration_ + 6.5)
    {
        return;
    }
    const int n = this->ticket_.cols();
    long long value = this->bitsAmount_;
    if (u < this->bitsDuration_ && !this->reduced_)
    {
        value = std::llround(this->bitsAmount_ *
                             easeOutCubic(u / this->bitsDuration_));
    }
    const bool settled = u > this->bitsDuration_ + 6.0;
    const auto digits = QString::number(value).rightJustified(n, QChar('0'));
    const int significant =
        static_cast<int>(QString::number(this->bitsAmount_).size());
    for (int c = 0; c < n; ++c)
    {
        const bool lit = !settled && c >= n - significant;
        this->ticket_.target(0, c, glyphFor(digits.at(c)),
                             lit ? INK_AMBER : INK_DIM, 0, 0, 0.042, this->rng_);
    }
}

void DepartureScene::turnover(const SceneState &state)
{
    if (!this->live_ || this->board_.empty() ||
        this->now_ < this->holdBoardUntil_)
    {
        return;
    }
    // Busy chat keeps the board clattering; a quiet one turns a row now
    // and then
    double rate = 0.06 + 1.7 * state.energy;
    if (this->reduced_)
    {
        rate *= 0.4;
    }
    this->turnover_ += rate * state.dt;
    while (this->turnover_ >= 1)
    {
        this->turnover_ -= 1;
        const int r = this->pickRow(false);
        if (r < 0)
        {
            break;
        }
        auto &row = this->rows_[static_cast<size_t>(r)];
        if (row.kind == RowKind::Chatter && this->rng_.next() < 0.3)
        {
            // Only the remark changes: boarding, delayed, last call...
            const auto &remark =
                REMARKS[static_cast<size_t>(this->rng_.next() * REMARKS.size()) %
                        REMARKS.size()];
            row.remark = QString::fromLatin1(remark.text);
            row.remarkInk = remark.ink;
            this->formatRow(row);
            this->applyRow(r, 0, 0.03, 3, 0.05);
            continue;
        }
        this->replaceRow(r, this->chatterRow(this->takeName(), r), 0.0, 0.028,
                         2 + static_cast<int>(this->rng_.next() * 3));
    }
}

void DepartureScene::breeze(const SceneState &state)
{
    if (this->reduced_ || !state.pointer)
    {
        this->lastPointer_ = state.pointer;
        return;
    }
    const QPointF ptr = *state.pointer;
    double speed = 0;
    if (this->lastPointer_ && state.dt > 0)
    {
        const auto d = ptr - *this->lastPointer_;
        speed = std::hypot(d.x(), d.y()) / state.dt;
    }
    this->lastPointer_ = ptr;

    // Flaps near the pointer lift a little, as if in a draught
    constexpr double RADIUS = 72;
    const double rate = 1.2 + std::min(speed / 200.0, 5.0);
    for (auto *bank : {&this->board_, &this->sign_, &this->next_, &this->hype_,
                       &this->ticket_})
    {
        if (bank->empty() ||
            !bank->bounds().adjusted(-RADIUS, -RADIUS, RADIUS, RADIUS).contains(ptr))
        {
            continue;
        }
        for (int i = 0; i < bank->count(); ++i)
        {
            const auto d = bank->center(i) - ptr;
            const double dist = std::hypot(d.x(), d.y());
            if (dist > RADIUS)
            {
                continue;
            }
            const double near = 1 - dist / RADIUS;
            if (this->rng_.next() < rate * state.dt * near * near * 0.9)
            {
                bank->flutter(i, 0.45 + 0.55 * near);
            }
        }
    }
}

void DepartureScene::refreshNext(int changed, bool deleted)
{
    const int n = this->next_.cols();
    if (n <= 0)
    {
        return;
    }
    // Long drafts page along by half a row at a time
    const int len = static_cast<int>(this->draft_.size());
    const int page = std::max(4, n / 2);
    const int offset = len < n ? 0 : ((len - n) / page + 1) * page;
    const auto glyphAt = [&](int i) {
        return i < len ? this->draft_[static_cast<size_t>(i)] : G_BLANK;
    };
    if (offset != this->draftOffset_ || changed < 0)
    {
        const bool paging = this->draftOffset_ >= 0;
        this->draftOffset_ = offset;
        for (int c = 0; c < n; ++c)
        {
            this->next_.target(0, c, glyphAt(offset + c), INK_WHITE,
                               paging ? c * 0.014 : 0.0,
                               this->reduced_ || !paging ? 0 : 1, 0.03,
                               this->rng_);
        }
        return;
    }
    const int c = changed - offset;
    if (c >= 0 && c < n)
    {
        this->next_.target(0, c, glyphAt(changed), INK_WHITE, 0,
                           this->reduced_ ? 0 : (deleted ? 2 : 3), 0.028,
                           this->rng_);
        if (c < static_cast<int>(this->keyGlow_.size()))
        {
            this->keyGlow_[static_cast<size_t>(c)] = 1;
        }
    }
}

void DepartureScene::setSign(const QString &text, bool animate)
{
    this->signText_ = text;
    if (this->sign_.empty())
    {
        return;
    }
    const int n = this->sign_.cols();
    auto name = text.toUpper();
    if (name.size() > n)
    {
        name = name.left(n);
    }
    // Centred on a hanging sign, read from the left in the board's title
    const int start =
        this->geo_.signInHousing ? 0 : (n - static_cast<int>(name.size())) / 2;
    for (int c = 0; c < n; ++c)
    {
        const int k = c - start;
        const int g = k >= 0 && k < name.size() ? glyphFor(name.at(k)) : G_BLANK;
        if (animate)
        {
            this->sign_.target(0, c, g, INK_WHITE, 0.05 + c * 0.035,
                               this->reduced_ ? 0 : 4, 0.045, this->rng_);
        }
        else
        {
            this->sign_.snap(0, c, g, INK_WHITE);
        }
    }
    if (animate)
    {
        this->signT_ = this->now_;
    }
}

void DepartureScene::clickWave(QPointF point)
{
    if (this->reduced_)
    {
        return;
    }
    constexpr double RADIUS = 420;
    constexpr double SPEED = 760;
    for (auto *bank : {&this->board_, &this->sign_, &this->next_, &this->hype_,
                       &this->ticket_})
    {
        for (int i = 0; i < bank->count(); ++i)
        {
            const auto d = bank->center(i) - point;
            const double dist = std::hypot(d.x(), d.y());
            if (dist > RADIUS)
            {
                continue;
            }
            bank->riffle(i, dist < 160 ? 2 : 1, dist / SPEED, 0.04, this->rng_);
        }
    }
}

// Events -------------------------------------------------------------------------

void DepartureScene::event(const SceneEvent &e, const SceneState &state,
                           const SceneLayout &layout)
{
    this->now_ = state.t;
    this->reduced_ = state.reduced;
    this->ensureLayout(layout);
    switch (e.type)
    {
        case SceneEventType::Key:
            this->onKey(e);
            break;
        case SceneEventType::Send:
            this->onSend(e);
            break;
        case SceneEventType::Message:
            this->onMessage(e);
            break;
        case SceneEventType::Sub:
            this->onSub(e);
            break;
        case SceneEventType::Gift:
            this->onGift(e);
            break;
        case SceneEventType::Bits:
            this->onBits(e);
            break;
        case SceneEventType::Raid:
            this->onRaid(e);
            break;
        case SceneEventType::Follow: {
            const int r = this->pickRow(true);
            if (r >= 0 && !e.user.isEmpty())
            {
                auto row = this->chatterRow(e.user, r);
                row.kind = RowKind::Joined;
                row.remark = QStringLiteral("JOINED");
                row.remarkInk = INK_AMBER;
                row.bulb = INK_AMBER;
                row.glow = 0.4;
                this->replaceRow(r, std::move(row), 0, 0.025, 3);
            }
        }
        break;
        case SceneEventType::Mention:
            this->onMention(e);
            break;
        case SceneEventType::Combo:
            this->onCombo(e);
            break;
        case SceneEventType::Hype:
            this->onHype(e, state);
            break;
        case SceneEventType::Shoutout: {
            const auto name = e.user.toUpper();
            auto row = this->freeRow(
                RowKind::Shoutout,
                {QStringLiteral("SPECIAL SERVICE TO %1").arg(name),
                 QStringLiteral("SPECIAL SVC TO %1").arg(name),
                 QStringLiteral("SPECIAL \u00B7 %1").arg(name)},
                INK_AMBER);
            row.bulb = INK_AMBER;
            row.glow = 0.7;
            this->insertTop(std::move(row), 0);
            this->lampSwell(this->geo_.column.center().x(),
                            this->geo_.column.width(), 0.5);
        }
        break;
        case SceneEventType::Timeout:
            this->onTimeout(e);
            break;
        case SceneEventType::Live:
            this->onLive();
            break;
        case SceneEventType::Offline:
            this->onOffline();
            break;
        case SceneEventType::Tab:
            this->setSign(e.text.isEmpty() ? state.channel : e.text, true);
            break;
        case SceneEventType::Hover:
            this->onHover(e);
            break;
        case SceneEventType::Unhover:
            this->hovering_ = false;
            break;
        case SceneEventType::Click:
            this->clickWave(e.point);
            break;
    }
}

void DepartureScene::onKey(const SceneEvent &e)
{
    const int amount = std::max(0, e.amount);
    const int before = static_cast<int>(this->draft_.size());
    if (e.deleted || amount < before)
    {
        if (amount < before)
        {
            this->draft_.resize(static_cast<size_t>(amount));
            this->refreshNext(before - amount == 1 ? amount : -1, true);
        }
        return;
    }
    // Usually one character; unknown characters in a jump become dots
    const auto typed = e.text.isEmpty() ? QStringLiteral("?") : e.text.right(1);
    while (static_cast<int>(this->draft_.size()) < amount - 1)
    {
        this->draft_.push_back(static_cast<std::uint8_t>(G_DOT));
    }
    if (static_cast<int>(this->draft_.size()) < amount)
    {
        this->draft_.push_back(static_cast<std::uint8_t>(glyphFor(typed.at(0))));
    }
    else if (amount > 0)
    {
        this->draft_[static_cast<size_t>(amount - 1)] =
            static_cast<std::uint8_t>(glyphFor(typed.at(0)));
    }
    this->refreshNext(amount - before == 1 ? amount - 1 : -1, false);
    if (this->next_.empty())
    {
        // No board to write on: the lamp above the caret brightens a little
        const double x = e.point.x() > 0 ? e.point.x() : this->geo_.win.width() / 2;
        this->lampSwell(x, 160, 0.3);
    }
}

void DepartureScene::onSend(const SceneEvent &e)
{
    const auto &g = this->geo_;
    this->sendT_ = this->now_;
    this->draft_.clear();
    this->draftOffset_ = 0;
    this->signalGreenUntil_ = this->now_ + 6;

    // 1. The NEXT DEPARTURE row snaps to DEPARTED
    const auto word = QStringLiteral("DEPARTED");
    const int wordLen = static_cast<int>(word.size());
    const int n = this->next_.cols();
    // Under the REMARKS column, so the letters rise straight up to row one
    const bool underRemarks = !this->board_.empty() &&
                              n == this->board_.cols() &&
                              g.remarkCol + wordLen <= n;
    const int start = underRemarks ? g.remarkCol : std::max(0, (n - wordLen) / 2);
    for (int c = 0; c < n; ++c)
    {
        const int k = c - start;
        const int glyph = k >= 0 && k < wordLen ? glyphFor(word.at(k)) : G_BLANK;
        this->next_.target(0, c, glyph, glyph == G_BLANK ? INK_WHITE : INK_AMBER,
                           c * 0.016, this->reduced_ ? 0 : 2, 0.03, this->rng_);
    }

    // 2. The board makes room for your train at the top
    const double liftAt = this->reduced_ ? 0.5 : 0.42;
    const double flightTime = this->reduced_ ? 0.4 : 0.6;
    const double stagger = this->reduced_ ? 0.02 : 0.05;
    int selfId = -1;
    if (!this->board_.empty())
    {
        const auto channel = this->signText_.toUpper();
        BoardRow row;
        row.kind = RowKind::Self;
        row.id = this->nextRowId_++;
        const auto time = clockText(this->clockMinutes() + 1);
        row.free = {QStringLiteral("%1 YOU\u2192#%2").arg(time, channel),
                    QStringLiteral("YOU\u2192#%1").arg(channel),
                    QStringLiteral("YOU\u2192%1").arg(channel)};
        row.ink = INK_AMBER;
        row.remark = word;
        row.remarkInk = INK_AMBER;
        row.remarkHidden = true;
        row.bulb = INK_AMBER;
        row.glow = 0.55;
        selfId = row.id;
        this->after(liftAt + 0.05, [this, row]() mutable {
            this->insertTop(std::move(row), 0.0);
        });
    }

    // 3. The DEPARTED flaps lift off and fly up into the remark column
    for (int i = 0; i < wordLen; ++i)
    {
        const int c = start + i;
        Flight flight;
        flight.glyph = glyphFor(word.at(i));
        flight.ink = INK_AMBER;
        flight.from = c < n ? this->next_.tileRect(0, c)
                            : QRectF(e.point - QPointF(6, 9), QSizeF(12, 18));
        flight.to = this->board_.empty()
                        ? flight.from.translated(0, -200)
                        : this->board_.tileRect(0, g.remarkCol + i);
        flight.start = this->now_ + liftAt + i * stagger;
        flight.duration = flightTime;
        flight.tilt = (hash01(i, static_cast<int>(this->now_ * 10)) - 0.5) * 24;
        // With reduced motion the letters simply appear on the board
        if (!this->board_.empty() && !this->reduced_)
        {
            this->flights_.push_back(flight);
        }
        // The tile leaves its slot as it lifts
        this->after(liftAt + i * stagger, [this, c]() {
            if (c < this->next_.cols())
            {
                this->next_.snap(0, c, G_BLANK, INK_WHITE);
            }
        });
        // and lands on the board with a clack
        this->after(liftAt + i * stagger + flightTime,
                    [this, selfId, i, glyph = flight.glyph]() {
                        const int r = this->findRowId(selfId);
                        if (r < 0)
                        {
                            return;
                        }
                        this->board_.target(r, this->geo_.remarkCol + i, glyph,
                                            INK_AMBER, 0, 1, 0.035, this->rng_);
                    });
    }
    this->after(liftAt + wordLen * stagger + flightTime + 0.1, [this, selfId]() {
        const int r = this->findRowId(selfId);
        if (r >= 0)
        {
            auto &row = this->rows_[static_cast<size_t>(r)];
            row.remarkHidden = false;
            row.glow = std::max(row.glow, 0.8);
            this->formatRow(row);
            this->applyRow(r, 0, 0.0, 0, 0.04);
        }
    });

    // 4. A train pulls out along the platform under the board and the clock
    //    ticks on
    if (!this->reduced_)
    {
        const double fromX = g.hasBoard ? g.column.left() + g.column.width() * 0.2
                                        : e.point.x();
        this->streaks_.push_back({this->now_ + 0.3, 1.5, fromX,
                                  g.win.width() + 160.0, 1.0, false});
    }
    this->after(liftAt + flightTime + 0.15, [this]() {
        this->ticks_++;
    });
}

void DepartureScene::onMessage(const SceneEvent &e)
{
    if (e.user.isEmpty())
    {
        return;
    }
    this->recent_.push_back(e.user);
    while (this->recent_.size() > 24)
    {
        this->recent_.pop_front();
    }
    this->waiting_.push_back(e.user);
    while (this->waiting_.size() > 6)
    {
        this->waiting_.pop_front();
    }
    if (e.firstTime && this->live_ && !this->board_.empty() &&
        this->now_ - this->lastNewRowT_ > 3.0 && this->now_ - this->raidT_ > 4)
    {
        this->lastNewRowT_ = this->now_;
        auto row = this->chatterRow(e.user, 0);
        row.kind = RowKind::New;
        row.remark = QStringLiteral("NEW");
        row.remarkInk = INK_AMBER;
        row.bulb = INK_AMBER;
        row.glow = 0.5;
        this->insertTop(std::move(row), 0);
    }
}

void DepartureScene::onSub(const SceneEvent &e)
{
    const int months = std::max(1, e.amount);
    const auto name = e.user.toUpper();
    const auto unit = months == 1 ? QStringLiteral("MONTH") : QStringLiteral("MONTHS");
    auto row = this->freeRow(
        RowKind::Sub,
        {QStringLiteral("PLATFORM 1 \u00B7 %1 \u00B7 %2 %3").arg(name).arg(months).arg(unit),
         QStringLiteral("PLAT 1 \u00B7 %1 \u00B7 %2 %3").arg(name).arg(months).arg(unit),
         QStringLiteral("PLAT 1 \u00B7 %1 \u00B7 %2 MO").arg(name).arg(months),
         QStringLiteral("%1 \u00B7 %2 MO").arg(name).arg(months)},
        INK_AMBER);
    row.author = e.user.toLower();
    row.bulb = INK_AMBER;
    row.glow = 0.6 + 0.4 * clamp01(months / 24.0);
    this->insertTop(std::move(row), 0);
    // The lamp above the board swells, more for long subscriptions
    this->lampSwell(this->geo_.column.center().x(), this->geo_.column.width() * 0.8,
                    0.45 + 0.5 * clamp01(months / 24.0));
}

void DepartureScene::onGift(const SceneEvent &e)
{
    if (this->board_.empty())
    {
        return;
    }
    const int count = std::max(1, e.amount);
    const int rows = std::min(count, this->reduced_ ? 3 : 8);
    const auto gifter = e.user.toUpper();
    // One cascading row per gift; big gifts cascade faster
    const double spacing = count >= 10 ? 0.16 : 0.28;
    for (int i = 0; i < rows; ++i)
    {
        this->after(i * spacing, [this, i, count, user = e.user]() {
            auto row = this->chatterRow(this->recentName(i, user), 0);
            row.kind = RowKind::Gift;
            row.minutes = this->clockMinutes();
            row.ink = INK_AMBER;
            row.remark = QStringLiteral("GIFT %1/%2").arg(i + 1).arg(count);
            if (count == 1)
            {
                row.remark = QStringLiteral("GIFTED");
            }
            else if (row.remark.size() > this->geo_.remarkW)
            {
                row.remark = QStringLiteral("%1 OF %2").arg(i + 1).arg(count);
            }
            row.remarkInk = INK_AMBER;
            row.bulb = INK_AMBER;
            row.glow = 0.5;
            row.author.clear();
            this->insertTop(std::move(row), 0);
        });
    }
    if (count < 10)
    {
        return;
    }
    // Big gifts end in a headline row and an amber sweep over the whole board
    const double sweepAt = rows * spacing + 0.1;
    const double power = clamp01(std::log10(count) / 2.0);
    auto headline = this->freeRow(
        RowKind::Gift,
        {QStringLiteral("%1 GIFTS \u00B7 FROM %2").arg(count).arg(gifter),
         QStringLiteral("%1 GIFTS \u00B7 %2").arg(count).arg(gifter)},
        INK_AMBER);
    headline.bulb = INK_AMBER;
    headline.glow = 1.0;
    this->holdBoardUntil_ =
        std::max(this->holdBoardUntil_, this->now_ + sweepAt + 2.6);
    if (this->reduced_)
    {
        this->after(sweepAt, [this, headline]() mutable {
            this->insertTop(std::move(headline), 0);
        });
        return;
    }
    // A diagonal amber sweep over the whole board; the headline is waiting
    // underneath when it clears
    this->after(sweepAt, [this, power, headline]() mutable {
        const int rowsN = this->board_.rows();
        const int cols = this->board_.cols();
        for (int r = 0; r < rowsN; ++r)
        {
            for (int c = 0; c < cols; ++c)
            {
                this->board_.target(r, c, G_BLOCK, INK_AMBER,
                                    (c + r * 1.6) * 0.012, 1, 0.04, this->rng_);
            }
        }
        this->lampSwell(this->geo_.column.center().x(),
                        this->geo_.win.width(), 0.4 + 0.5 * power);
        const double hold = 1.0 + 0.3 * power;
        this->after(hold, [this, headline]() mutable {
            if (this->rows_.empty())
            {
                return;
            }
            headline.born = this->now_;
            this->formatRow(headline);
            this->rows_.pop_back();
            this->rows_.insert(this->rows_.begin(), std::move(headline));
            for (int r = 0; r < static_cast<int>(this->rows_.size()); ++r)
            {
                this->applyRow(r, r * 1.6 * 0.012, 0.012, 2, 0.045);
            }
        });
    });
}

void DepartureScene::onBits(const SceneEvent &e)
{
    const int amount = std::clamp(e.amount, 1, 999999);
    if (this->ticket_.empty())
    {
        const auto name = e.user.toUpper();
        auto row = this->freeRow(
            RowKind::Fare,
            {QStringLiteral("FARE \u00B7 %1 \u00B7 %2 BITS").arg(name).arg(amount),
             QStringLiteral("FARE \u00B7 %1 \u00B7 %2").arg(name).arg(amount)},
            INK_AMBER);
        row.bulb = INK_AMBER;
        row.glow = 0.6;
        this->insertTop(std::move(row), 0);
        return;
    }
    this->bitsT_ = this->now_;
    this->bitsAmount_ = amount;
    this->bitsDuration_ = std::clamp(0.7 + std::log10(amount) * 0.45, 0.9, 2.6);
    this->bitsPower_ = clamp01(std::log10(amount) / 4.5);
    this->lampSwell(this->geo_.kiosk.center().x(), 160, 0.3 + 0.5 * this->bitsPower_);
}

void DepartureScene::onRaid(const SceneEvent &e)
{
    const auto &g = this->geo_;
    const int amount = std::max(1, e.amount);
    const double power = clamp01(std::log10(amount) / 4.0);
    this->raidT_ = this->now_;
    this->raidPower_ = power;
    if (this->now_ - this->pigeonFledT_ > 30 && !this->reduced_)
    {
        // The pigeon on the board takes fright at the train
        this->pigeonFledT_ = this->now_ + 0.9;
    }
    const auto name = e.user.toUpper();

    if (!this->board_.empty())
    {
        // The arriving train fills the top of the board; the rest of the
        // board riffles as the wave passes
        const int rowsN = this->board_.rows();
        const int affected =
            std::clamp(1 + qRound(power * rowsN * 0.45), 1, rowsN);
        auto head = this->freeRow(
            RowKind::Raid,
            {QStringLiteral("ARRIVAL \u00B7 %1 FROM %2").arg(amount).arg(name),
             QStringLiteral("ARRIVAL \u00B7 %1 \u00B7 %2").arg(amount).arg(name),
             QStringLiteral("ARR %1 %2").arg(amount).arg(name)},
            INK_AMBER);
        head.bulb = INK_AMBER;
        head.glow = 0.9;
        this->rows_[0] = head;
        for (int r = 1; r < affected; ++r)
        {
            auto row = this->chatterRow(name, r);
            row.kind = RowKind::Raid;
            row.minutes = this->clockMinutes();
            row.remark = QStringLiteral("ARRIVED");
            row.remarkInk = INK_AMBER;
            row.author.clear();
            row.born = this->now_;
            this->formatRow(row);
            this->rows_[static_cast<size_t>(r)] = row;
        }
        const double waveAt = this->reduced_ ? 0.2 : 0.7;
        const int riffle = this->reduced_ ? 0 : 3 + qRound(3 * power);
        const int cols = this->board_.cols();
        for (int r = 0; r < rowsN; ++r)
        {
            const auto &cells = this->rows_[static_cast<size_t>(r)].cells;
            for (int c = 0; c < cols; ++c)
            {
                const double delay = waveAt + c * 0.032 + r * 0.006;
                if (r < affected)
                {
                    this->board_.target(r, c, cells.glyph[static_cast<size_t>(c)],
                                        cells.ink[static_cast<size_t>(c)], delay,
                                        riffle, 0.04, this->rng_);
                }
                else if (!this->reduced_)
                {
                    this->board_.riffle(r * cols + c, 2 + qRound(2 * power),
                                        delay, 0.04, this->rng_);
                }
            }
        }
        this->holdBoardUntil_ = std::max(this->holdBoardUntil_,
                                         this->now_ + waveAt + cols * 0.032 + 1.5);
    }

    // The train runs in along the platform and stops by the board
    if (!this->reduced_)
    {
        this->streaks_.push_back({this->now_ + 1.5, 1.7, -120.0,
                                  g.win.width() * 0.72, 0.6 + 0.4 * power, true});
    }
    this->lampSwell(g.win.width() * 0.5, g.win.width(), 0.25 + 0.3 * power);

    // Passengers spill onto the concourse
    if (g.hasBoard && g.crowd.height() > 30)
    {
        const int people = std::clamp(qRound(3 + power * 18), 3,
                                      this->reduced_ ? 6 : 22);
        for (int i = 0; i < people; ++i)
        {
            Arrival a;
            a.depth = this->rng_.next();
            a.variant = static_cast<int>(this->rng_.next() * PERSON_VARIANTS) %
                        PERSON_VARIANTS;
            a.speed = 26 + this->rng_.next() * 20;
            a.born = this->now_ + 2.2 + i * 0.18;
            a.x = g.crowd.left() - 12 - i * 9;
            a.stopAt = g.crowd.left() + g.crowd.width() * (0.12 + 0.75 * this->rng_.next());
            this->arrivals_.push_back(a);
        }
        if (this->arrivals_.size() > 40)
        {
            this->arrivals_.erase(this->arrivals_.begin(),
                                  this->arrivals_.end() - 40);
        }
    }
}

void DepartureScene::onMention(const SceneEvent &e)
{
    auto row = this->freeRow(RowKind::Mention, {QStringLiteral("NOW BOARDING @YOU")},
                             INK_AMBER);
    row.bulb = INK_AMBER;
    row.glow = 0.9;
    row.pulse = true;
    this->insertTop(std::move(row), 0);

    // A small marker flap at the edge of the message row
    if (!e.row.isEmpty())
    {
        const double dpr = this->geo_.dpr;
        const int h = qRound(std::clamp(e.row.height() - 4, 12.0, 20.0) * dpr);
        const int w = qRound(h * 0.7);
        this->marker_.setup(1, 1, w, h, 0, 0,
                            QPoint(qRound((e.row.right() - 10) * dpr) - w,
                                   qRound(e.row.center().y() * dpr - h / 2.0)),
                            dpr);
        this->marker_.snap(0, 0, G_BLANK, INK_WHITE);
        this->marker_.target(0, 0, drumIndex(u'@'), INK_AMBER, 0.05,
                             this->reduced_ ? 0 : 4, 0.04, this->rng_);
        this->markerT_ = this->now_;
    }
}

void DepartureScene::onCombo(const SceneEvent &e)
{
    if (this->combo_.empty() || e.text.isEmpty())
    {
        return;
    }
    this->comboT_ = this->now_;
    const int n = this->combo_.cols();
    auto text = e.text.toUpper();
    if (e.amount > 1 && text.size() + 3 <= n)
    {
        text += QStringLiteral(" X%1").arg(e.amount);
    }
    text = text.left(n);
    const int start = (n - static_cast<int>(text.size())) / 2;
    for (int c = 0; c < n; ++c)
    {
        this->combo_.snap(0, c, G_BLANK, INK_WHITE);
        const int k = c - start;
        const int g = k >= 0 && k < text.size() ? glyphFor(text.at(k)) : G_BLANK;
        const int ink = k >= static_cast<int>(e.text.size()) ? INK_AMBER : INK_WHITE;
        this->combo_.target(0, c, g, ink, 0.15 + c * 0.05,
                            this->reduced_ ? 0 : 4, 0.045, this->rng_);
    }
    this->after(2.0, [this, n]() {
        for (int c = 0; c < n; ++c)
        {
            this->combo_.target(0, c, G_BLANK, INK_WHITE, c * 0.03,
                                this->reduced_ ? 0 : 1, 0.04, this->rng_);
        }
    });
}

void DepartureScene::onHype(const SceneEvent &e, const SceneState &state)
{
    if (this->hype_.empty())
    {
        return;
    }
    const int level = state.hypeLevel > 0 ? state.hypeLevel : e.amount;
    if (!e.levelUp)
    {
        return;
    }
    // A level-up flips the whole bar from left to right
    const auto cells = this->hypeCells(level, state.hypeProgress);
    for (int c = 0; c < this->hype_.cols(); ++c)
    {
        planFlap(this->hype_.at(0, c), cells.glyph[static_cast<size_t>(c)],
                 cells.ink[static_cast<size_t>(c)], c * 0.035,
                 this->reduced_ ? 0 : 4, 0.04, this->rng_, !this->reduced_);
    }
    this->hype_.target(0, 0, cells.glyph[0], cells.ink[0], 0, 0, 0.04, this->rng_);
    this->hypeFlipUntil_ = this->now_ + this->hype_.cols() * 0.035 + 0.6;
    this->hypeGlowT_ = this->now_;
    this->lampSwell(this->hype_.bounds().center().x(), 260, 0.3 + 0.05 * level);
}

void DepartureScene::onTimeout(const SceneEvent &e)
{
    if (this->rows_.empty())
    {
        return;
    }
    const auto author = e.user.toLower();
    int r = this->findRow(author);
    if (r < 0)
    {
        r = this->pickRow(true);
        if (r < 0)
        {
            r = static_cast<int>(this->rows_.size()) - 1;
        }
        this->rows_[static_cast<size_t>(r)] = this->chatterRow(e.user, r);
    }
    auto &row = this->rows_[static_cast<size_t>(r)];
    row.kind = RowKind::Cancel;
    row.free.clear();
    if (row.dest.isEmpty())
    {
        row.dest = e.user;
    }
    row.ink = INK_RED;
    row.remark = QStringLiteral("CANCELLED");
    row.remarkInk = INK_RED;
    row.bulb = INK_RED;
    row.glow = 0;
    row.pulse = false;
    row.born = this->now_;
    this->formatRow(row);
    this->applyRow(r, 0, 0.022, 3, 0.045);
    // Then the row clears
    this->after(2.7, [this, id = row.id]() {
        const int index = this->findRowId(id);
        if (index < 0)
        {
            return;
        }
        auto blank = BoardRow{};
        blank.id = this->nextRowId_++;
        blank.born = this->now_;
        this->formatRow(blank);
        this->rows_[static_cast<size_t>(index)] = blank;
        this->applyRow(index, 0, 0.02, 1, 0.04);
    });
}

void DepartureScene::onOffline()
{
    this->live_ = false;
    // Every row flips to SERVICE ENDED, top to bottom
    for (int r = 0; r < static_cast<int>(this->rows_.size()); ++r)
    {
        auto row = this->freeRow(RowKind::Service, {QStringLiteral("SERVICE ENDED")},
                                 INK_WHITE);
        row.center = true;
        this->formatRow(row);
        this->rows_[static_cast<size_t>(r)] = row;
        this->applyRow(r, 0.2 + r * 0.12, 0.02, 3, 0.05);
    }
    // and the lamps go out one by one
    for (int i = 0; i < static_cast<int>(this->lamps_.size()); ++i)
    {
        this->after(0.6 + i * (this->reduced_ ? 0.2 : 0.5), [this, i]() {
            if (!this->live_ && i < static_cast<int>(this->lamps_.size()))
            {
                this->lamps_[static_cast<size_t>(i)].target = 0.12;
            }
        });
    }
}

void DepartureScene::onLive()
{
    this->live_ = true;
    const int n = static_cast<int>(this->lamps_.size());
    for (int i = 0; i < n; ++i)
    {
        this->after(0.2 + (n - 1 - i) * (this->reduced_ ? 0.15 : 0.4), [this, i]() {
            if (this->live_ && i < static_cast<int>(this->lamps_.size()))
            {
                auto &lamp = this->lamps_[static_cast<size_t>(i)];
                lamp.target = 1.0;
                lamp.swell = std::max(lamp.swell, 0.35);
            }
        });
    }
    for (int r = 0; r < static_cast<int>(this->rows_.size()); ++r)
    {
        BoardRow row;
        if (r == 0)
        {
            row = this->freeRow(RowKind::Shoutout,
                                {QStringLiteral("SERVICE RESUMED")}, INK_AMBER);
            row.center = true;
            row.bulb = INK_AMBER;
            row.glow = 0.7;
            this->formatRow(row);
        }
        else
        {
            row = this->chatterRow(this->takeName(), r);
        }
        this->rows_[static_cast<size_t>(r)] = row;
        this->applyRow(r, 0.6 + r * 0.08, 0.015, 3, 0.05);
    }
    this->holdBoardUntil_ = this->now_ + 3;
}

void DepartureScene::onHover(const SceneEvent &e)
{
    this->hoverUser_ = e.user.toLower();
    this->hovering_ = !this->hoverUser_.isEmpty();
    if (!this->hovering_ || this->rows_.empty() || !this->live_)
    {
        return;
    }
    if (this->findRow(this->hoverUser_) < 0)
    {
        // Put them on the board so their row can light up
        const int r = this->pickRow(true);
        if (r >= 0)
        {
            this->replaceRow(r, this->chatterRow(e.user, r), 0, 0.02, 2);
        }
    }
}

// Painting -----------------------------------------------------------------------

void DepartureScene::paintBack(QPainter &p, const SceneState &state,
                               const SceneLayout &layout)
{
    this->ensureLayout(layout);
    if (this->static_.isNull())
    {
        p.fillRect(QRectF(QPointF(0, 0), layout.window), SURFACE);
        return;
    }
    for (auto *bank : this->banks())
    {
        bank->render();
    }
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(QPointF(0, 0), this->static_);
    p.restore();

    this->paintSignal(p);
    this->paintCrowd(p);
    this->paintBoard(p);
    this->paintClockHands(p);
    this->paintHaze(p);
    this->paintLamps(p);
    this->paintPigeon(p);
    this->paintSign(p);
    this->paintNextRow(p, state);
    this->paintStreaks(p);
    this->paintHeadlight(p);
}

void DepartureScene::paintFront(QPainter &p, const SceneState &state,
                                const SceneLayout &layout)
{
    (void)state;
    (void)layout;
    this->paintFlights(p);
    this->paintMarker(p);
}

void DepartureScene::paintSignal(QPainter &p) const
{
    const auto &g = this->geo_;
    const QPointF pos(g.vp.x() + g.win.width() * 0.028,
                      g.vp.y() + g.win.height() * 0.035);
    const bool green = this->now_ < this->signalGreenUntil_;
    const auto color = green ? SIGNAL_GREEN : SIGNAL_RED;
    drawGlow(p, pos, 7, color, 0.45);
    p.setPen(Qt::NoPen);
    p.setBrush(mixColor(color, Qt::white, 0.4));
    p.drawEllipse(pos, 1.2, 1.2);
}

void DepartureScene::paintCrowd(QPainter &p) const
{
    const auto &g = this->geo_;
    if (this->people_.empty() || g.crowd.height() < 20)
    {
        if (!this->kioskImage_.isNull())
        {
            p.drawImage(g.kiosk.adjusted(-6, -6, 6, 6), this->kioskImage_);
            this->paintKiosk(p);
        }
        return;
    }
    const auto &area = g.crowd;
    const double farFeet = area.top() + g.personFar + 2;
    const double nearFeet = g.platTop - 2;
    const double t = this->reduced_ ? 0.0 : this->now_;
    const auto draw = [&](int variant, double depth, double x, double bob,
                          double alpha) {
        const int bucket = std::clamp(
            qRound(depth * (PERSON_BUCKETS - 1)), 0, PERSON_BUCKETS - 1);
        const auto &image =
            this->people_[static_cast<size_t>(variant * PERSON_BUCKETS + bucket)];
        const double w = image.width() / g.dpr;
        const double h = image.height() / g.dpr;
        const double feet = lerp(farFeet, nearFeet, depth);
        p.setOpacity(alpha);
        p.drawImage(QPointF(std::round((x - w / 2) * g.dpr) / g.dpr,
                            std::round((feet - h + bob) * g.dpr) / g.dpr),
                    image);
    };
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    bool kioskDrawn = this->kioskImage_.isNull();
    const auto drawKiosk = [&]() {
        p.setOpacity(1.0);
        p.drawImage(g.kiosk.adjusted(-6, -6, 6, 6), this->kioskImage_);
        this->paintKiosk(p);
        kioskDrawn = true;
    };
    for (const int index : this->crowdOrder_)
    {
        const double a = clamp01(this->crowdCount_ - index);
        if (a <= 0.01)
        {
            continue;
        }
        const auto &person = this->crowd_[static_cast<size_t>(index)];
        if (!kioskDrawn && person.depth > 0.62)
        {
            drawKiosk();
        }
        double u = person.x01 + person.walk * t;
        u -= std::floor(u);
        const double x = area.left() + 8 + u * (area.width() - 16);
        const double bob =
            person.walk != 0 ? std::sin(t * 4.2 + person.phase) * 0.6 : 0.0;
        draw(person.variant, person.depth, x, bob, a);
    }
    if (!kioskDrawn)
    {
        drawKiosk();
    }
    for (const auto &arrival : this->arrivals_)
    {
        const double age = this->now_ - arrival.born;
        if (age < 0)
        {
            continue;
        }
        const double a = smoothstep(0, 0.6, age) * (1 - smoothstep(20, 26, age));
        const bool moving = arrival.x < arrival.stopAt;
        const double bob = moving ? std::sin(t * 7 + arrival.speed) * 0.8 : 0.0;
        if (arrival.x > area.left() - 20)
        {
            draw(arrival.variant, arrival.depth, arrival.x, bob, a);
        }
    }
    p.restore();
}

void DepartureScene::paintKiosk(QPainter &p) const
{
    const auto &g = this->geo_;
    if (!g.hasKiosk || this->ticket_.empty())
    {
        return;
    }
    this->ticket_.paint(p);
    const double u = this->now_ - this->bitsT_;
    if (u < 0 || u > this->bitsDuration_ + 9)
    {
        return;
    }
    const double active =
        u < this->bitsDuration_
            ? 1.0
            : clamp01(1 - (u - this->bitsDuration_) / 7.0);
    const auto display = this->ticket_.bounds();
    drawGlow(p, display.center(), display.width() * 0.9, LAMP_WARM,
             (0.15 + 0.25 * this->bitsPower_) * active);
    // A ticket slides out of the slot, longer for bigger amounts
    const auto &k = g.kioskBody;
    const double length =
        std::clamp(4 + std::log10(this->bitsAmount_ + 1) * 3, 5.0, 18.0) *
        clamp01((u - this->bitsDuration_ * 0.6) / 0.5);
    if (length > 0.5)
    {
        const double slotY = k.top() + k.width() * 0.18 + 21;
        const double w = std::max(8.0, k.width() * 0.3);
        const double x = k.center().x() - w / 2;
        p.save();
        p.setOpacity(0.85 * clamp01(1 - (u - this->bitsDuration_ - 6) / 2.5));
        p.fillRect(QRectF(x, slotY, w, length), QColor(196, 186, 162));
        p.setOpacity(p.opacity() * 0.6);
        for (double y = slotY + 3; y < slotY + length - 2; y += 3)
        {
            p.fillRect(QRectF(x + 2, y, w * 0.6, 1), QColor(70, 60, 50));
        }
        p.restore();
    }
}

void DepartureScene::paintBoard(QPainter &p) const
{
    const auto &g = this->geo_;
    if (!g.hasBoard || this->board_.empty())
    {
        return;
    }
    this->board_.paint(p);
    const double light = this->boardLight();
    if (!this->boardLight_.isNull())
    {
        p.save();
        p.setOpacity(clamp01(light));
        p.drawImage(g.housing, this->boardLight_);
        p.restore();
    }

    // Lit rows and the indicator lamps beside them
    p.save();
    const double pulse =
        this->reduced_ ? 1.0 : 0.72 + 0.28 * std::sin(this->now_ * TAU / 3.4);
    for (int r = 0; r < static_cast<int>(this->rows_.size()) &&
                    r < this->board_.rows();
         ++r)
    {
        const auto &row = this->rows_[static_cast<size_t>(r)];
        const double glow = row.pulse ? row.glow * pulse : row.glow;
        const bool hovered = this->hoverLevel_ > 0.01 &&
                             !this->hoverUser_.isEmpty() &&
                             row.author == this->hoverUser_;
        const auto rect = this->board_.rowRect(r).adjusted(-2, -1, 2, 1);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        if (glow > 0.01)
        {
            p.fillRect(rect, QColor(qRound(60 * glow), qRound(36 * glow),
                                    qRound(9 * glow)));
        }
        if (hovered)
        {
            const double h = this->hoverLevel_;
            p.fillRect(rect, QColor(qRound(58 * h), qRound(46 * h),
                                    qRound(28 * h)));
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const QPointF bulb(g.housing.left() + 7, rect.center().y());
        if (row.bulb >= 0 || hovered)
        {
            const auto color = inkColor(row.bulb >= 0 ? row.bulb : INK_AMBER);
            const double a = clamp01(0.45 + std::max(glow, hovered ? this->hoverLevel_ : 0.0));
            drawGlow(p, bulb, 6, color, a * 0.8);
            p.setPen(Qt::NoPen);
            p.setBrush(mixColor(color, Qt::white, 0.35));
            p.drawEllipse(bulb, 1.5, 1.5);
        }
        if (hovered)
        {
            // Brass pointer at the far end of the row
            const double x = this->board_.bounds().right() + 2.5;
            const double y = rect.center().y();
            QPainterPath arrow;
            arrow.moveTo(x, y);
            arrow.lineTo(x + 4, y - 3.5);
            arrow.lineTo(x + 4, y + 3.5);
            arrow.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(FLAP_AMBER, this->hoverLevel_));
            p.drawPath(arrow);
        }
    }
    p.restore();

    // When the service ends, the board darkens with the lamps
    if (light < 0.98)
    {
        p.fillRect(g.housing, QColor(0, 0, 0, qRound(120 * clamp01(1 - light))));
    }
    this->paintCombo(p);
    this->paintHype(p);
}

void DepartureScene::paintCombo(QPainter &p) const
{
    const double u = this->now_ - this->comboT_;
    if (this->combo_.empty() || u < 0 || u > COMBO_TIME)
    {
        return;
    }
    const double a =
        smoothstep(0, 0.18, u) * (1 - smoothstep(COMBO_TIME - 0.35, COMBO_TIME, u));
    const auto panel = this->combo_.bounds().adjusted(-6, -5, 6, 5);
    p.save();
    p.setOpacity(a);
    p.fillRect(panel, QColor(9, 10, 12));
    p.setPen(QPen(QColor(120, 88, 40), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(panel.adjusted(0.5, 0.5, -0.5, -0.5));
    p.restore();
    this->combo_.paint(p, a);
}

void DepartureScene::paintHype(QPainter &p) const
{
    if (this->hype_.empty())
    {
        return;
    }
    this->hype_.paint(p);
    // Warm light under the filled part of the bar
    double left = -1;
    double right = -1;
    for (int c = 0; c < this->hype_.cols(); ++c)
    {
        if (this->hype_.at(0, c).cur == G_BLOCK)
        {
            const auto tile = this->hype_.tileRect(0, c);
            left = left < 0 ? tile.left() : left;
            right = tile.right();
        }
    }
    const double flash = clamp01(1 - (this->now_ - this->hypeGlowT_) / 1.5);
    if (left >= 0)
    {
        const auto bounds = this->hype_.bounds();
        p.save();
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        const double k = 0.6 + 0.4 * flash;
        p.fillRect(QRectF(left - 1, bounds.top() - 1, right - left + 2,
                          bounds.height() + 2),
                   QColor(qRound(40 * k), qRound(22 * k), qRound(4 * k)));
        p.restore();
    }
}

void DepartureScene::paintClockHands(QPainter &p) const
{
    const auto &g = this->geo_;
    if (!g.hasBoard)
    {
        return;
    }
    const QPointF c = g.clockC;
    const double R = g.clockR;
    const double secs = std::fmod(this->now_ + CLOCK_START_SECONDS, 60.0);
    // The second hand sweeps round in 58.5 s and rests at twelve while the
    // minute hand jumps
    const double secAngle = secs < 58.5 ? secs / 58.5 * 360.0 : 0.0;
    const double u = clamp01((this->now_ - this->minuteJumpT_) / 0.45);
    const double jump =
        this->reduced_ ? 1.0 : 1 - std::exp(-u * 7) * std::cos(u * 14);
    const double minute = this->shownMinute_ - 1 + (u >= 1 ? 1.0 : jump);
    const double minAngle = std::fmod(minute, 60.0) * 6;
    const double hourAngle = std::fmod(minute / 60.0, 12.0) * 30;
    const auto hand = [&](double angleDeg, double back, double length,
                          double width, const QColor &color) {
        const double a = angleDeg * TAU / 360;
        const QPointF dir(std::sin(a), -std::cos(a));
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(c - dir * back, c + dir * length);
        return dir;
    };
    const QColor black(20, 20, 22);
    hand(hourAngle, R * 0.12, R * 0.48, R * 0.085, black);
    hand(minAngle, R * 0.14, R * 0.74, R * 0.06, black);
    const QColor red(196, 52, 38);
    const auto dir = hand(secAngle, R * 0.22, R * 0.56,
                          std::max(1.0, R * 0.024), red);
    p.setPen(Qt::NoPen);
    p.setBrush(red);
    p.drawEllipse(c + dir * R * 0.56, R * 0.075, R * 0.075);
    p.setBrush(black);
    p.drawEllipse(c, R * 0.05, R * 0.05);
}

void DepartureScene::paintSign(QPainter &p) const
{
    if (this->sign_.empty())
    {
        return;
    }
    this->sign_.paint(p);
    const double u = this->now_ - this->signT_;
    if (u >= 0 && u < 1.8)
    {
        const auto bounds = this->sign_.bounds();
        drawGlow(p, bounds.center(), bounds.width() * 0.55, LAMP_WARM,
                 0.12 * (1 - u / 1.8));
    }
}

void DepartureScene::paintNextRow(QPainter &p, const SceneState &state) const
{
    if (this->next_.empty())
    {
        return;
    }
    const auto bounds = this->next_.bounds();
    this->next_.paint(p);

    // The row's indicator lamp: amber while you type, green as you leave
    const double sinceSend = this->now_ - this->sendT_;
    const bool departing = sinceSend >= 0 && sinceSend < 2.5;
    if (state.typing || state.draftLength > 0 || departing)
    {
        const auto color = departing ? SIGNAL_GREEN : FLAP_AMBER;
        const double breathe =
            this->reduced_ || departing
                ? 0.85
                : 0.6 + 0.3 * std::sin(this->now_ * TAU / 1.6);
        const double fade = departing ? 1 - smoothstep(1.8, 2.5, sinceSend) : 1.0;
        const QPointF bulb(this->geo_.housing.left() + 7, bounds.center().y());
        drawGlow(p, bulb, 6, color, 0.7 * breathe * fade);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(mixColor(color, Qt::white, 0.35), fade));
        p.drawEllipse(bulb, 1.5, 1.5);
    }

    // Warm light under the flap that just turned, and the next free slot
    for (int c = 0; c < static_cast<int>(this->keyGlow_.size()); ++c)
    {
        const double k = this->keyGlow_[static_cast<size_t>(c)];
        if (k > 0.01)
        {
            const auto tile = this->next_.tileRect(0, c);
            drawGlow(p, tile.center(), tile.height() * 0.9, LAMP_WARM, 0.32 * k);
        }
    }
    const int slot = static_cast<int>(this->draft_.size()) - std::max(0, this->draftOffset_);
    if (state.typing && slot >= 0 && slot < this->next_.cols() &&
        this->now_ - this->sendT_ > 1.5)
    {
        const auto tile = this->next_.tileRect(0, slot);
        const double breathe =
            this->reduced_ ? 0.7 : 0.55 + 0.25 * std::sin(this->now_ * TAU / 1.6);
        p.fillRect(QRectF(tile.left() + 1, tile.bottom() + 1.5, tile.width() - 2, 1.5),
                   withAlpha(FLAP_AMBER, breathe));
    }
}

void DepartureScene::paintStreaks(QPainter &p) const
{
    const auto &g = this->geo_;
    const double W = g.win.width();
    const double H = g.win.height();
    const double rail1 = H - 5.5 + 0.6;
    const double rail2 = H - 2.2 + 0.8;
    const QRectF platform(0, g.platTop, W, H - g.platTop);
    bool clipped = false;
    for (const auto &s : this->streaks_)
    {
        const double u = (this->now_ - s.start) / s.duration;
        if (u < 0 || u > 1)
        {
            continue;
        }
        if (!clipped)
        {
            p.save();
            p.setClipRegion(this->trainClip_);
            clipped = true;
        }
        const double e = s.arriving ? easeOutCubic(u) : std::pow(u, 1.8);
        const double head = lerp(s.from, s.to, e);
        const double speed = s.arriving ? 1 - u : u;
        const double tail = 40 + 640 * speed * s.power;
        const double a = smoothstep(0.0, 0.08, u) * (1 - smoothstep(0.7, 1.0, u)) *
                         s.power;
        const bool rightward = s.to > s.from;
        const auto &image = rightward ? this->streakRight_ : this->streakLeft_;
        const double x0 = rightward ? head - tail : head;
        p.save();
        p.setOpacity(clamp01(a));
        p.drawImage(QRectF(x0, rail1 - 1.4, tail, 2.8), image);
        p.drawImage(QRectF(x0, rail2 - 1.6, tail, 3.2), image);
        p.setOpacity(clamp01(a * 0.28));
        p.drawImage(QRectF(x0, platform.top() + 3, tail, platform.height() - 4), image);
        // The carriages themselves, a blur of lit windows
        const auto &train = rightward ? this->trainRight_ : this->trainLeft_;
        const double carriage = tail * 1.15 + 30;
        const double cx0 = rightward ? head - carriage : head;
        const double bandH = std::min(18.0, platform.height() * 0.42);
        p.setOpacity(clamp01(a * 0.75));
        p.drawImage(QRectF(cx0, platform.center().y() - bandH / 2, carriage, bandH),
                    train);
        p.restore();
        drawGlow(p, QPointF(head, rail1), 22, TRAIN_LIGHT, clamp01(a * 0.8));
        drawGlow(p, QPointF(head, platform.center().y()), 70, LAMP_WARM,
                 clamp01(a * 0.2));
    }
    if (clipped)
    {
        p.restore();
    }
}

void DepartureScene::paintHeadlight(QPainter &p) const
{
    const double u = this->now_ - this->raidT_;
    if (u < 0 || u > RAID_LIGHT_TIME)
    {
        return;
    }
    const auto &g = this->geo_;
    const double H = g.win.height();
    const double power = 0.5 + 0.5 * this->raidPower_;
    const double fade =
        smoothstep(0, 0.9, u) * (1 - smoothstep(RAID_LIGHT_TIME - 0.7, RAID_LIGHT_TIME, u));
    // The headlight comes in from the far end of the hall
    const double approach =
        this->reduced_ ? 0.0 : easeInOutSine(clamp01(u / 2.4));
    const QPointF far(g.vp.x(), g.vp.y() + (H - g.vp.y()) * 0.1);
    const QPointF near(g.win.width() * 0.1, g.platTop - 14);
    const QPointF light = far + (near - far) * approach;
    const double size = lerp(6, 40, approach) * (0.7 + 0.3 * power);
    drawGlow(p, light, size * 4.0, LAMP_WARM, 0.3 * fade * power);
    drawGlow(p, light, size * 1.4, TRAIN_LIGHT, 0.9 * fade);
    if (this->reduced_)
    {
        return;
    }
    // and its beam sweeps across the hall and the board
    const double angle = lerp(-36, 6, easeInOutSine(clamp01(u / 2.8)));
    const double len = g.win.width() * 1.15;
    p.save();
    p.translate(light);
    p.rotate(angle);
    p.setOpacity(clamp01(0.34 * fade * power));
    p.drawImage(QRectF(0, -len * 0.28, len, len * 0.56), this->beam_);
    p.restore();
}

void DepartureScene::paintFlights(QPainter &p)
{
    auto &style = this->next_.empty() ? this->board_.style : this->next_.style;
    for (const auto &f : this->flights_)
    {
        const double u = (this->now_ - f.start) / f.duration;
        if (u < 0 || u > 1)
        {
            continue;
        }
        const double e = easeInOutSine(u);
        const QPointF p0 = f.from.center();
        const QPointF p2 = f.to.center();
        const QPointF p1(lerp(p0.x(), p2.x(), 0.2), lerp(p0.y(), p2.y(), 0.9));
        const QPointF pos = p0 * ((1 - e) * (1 - e)) + p1 * (2 * (1 - e) * e) +
                            p2 * (e * e);
        const double w = lerp(f.from.width(), f.to.width(), e);
        const double h = lerp(f.from.height(), f.to.height(), e);
        const double lift = std::sin(u * TAU / 2);
        drawGlow(p, pos, h * 1.5, LAMP_WARM, 0.3 * lift);
        p.save();
        p.translate(pos);
        p.rotate(f.tilt * lift);
        const double grow = 1 + 0.15 * lift;
        p.scale(grow, grow);
        p.drawImage(QRectF(-w / 2, -h / 2, w, h), style.glyph(f.glyph, f.ink));
        p.restore();
    }
}

void DepartureScene::paintMarker(QPainter &p) const
{
    const double u = this->now_ - this->markerT_;
    if (this->marker_.empty() || u < 0 || u > MARKER_TIME)
    {
        return;
    }
    const double a =
        smoothstep(0, 0.12, u) * (1 - smoothstep(MARKER_TIME - 0.6, MARKER_TIME, u));
    const auto bounds = this->marker_.bounds();
    const double breathe =
        this->reduced_ ? 0.8 : 0.7 + 0.3 * std::sin(u * TAU / 1.8);
    drawGlow(p, bounds.center(), bounds.height() * 1.2, LAMP_WARM,
             0.3 * a * breathe);
    this->marker_.paint(p, a);
}

void DepartureScene::paintPigeon(QPainter &p) const
{
    // A pigeon perched on a lamp shade, pecking now and then
    if (this->lamps_.size() < 2)
    {
        return;
    }
    const auto &lamp = this->lamps_[this->lamps_.size() - 2];
    const QPointF perch(lamp.bulb.x() + 4 * lamp.size,
                        lamp.bulb.y() - 7.2 * lamp.size);
    QPointF pos = perch;
    double alpha = 1;
    bool flying = false;
    const double away = this->now_ - this->pigeonFledT_;
    if (away >= 0 && away < 30)
    {
        if (away < 1.4)
        {
            pos += QPointF(-away * 110, -away * away * 70);
            alpha = 1 - smoothstep(0.9, 1.4, away);
            flying = true;
        }
        else if (away < 26)
        {
            return;
        }
        else
        {
            alpha = smoothstep(26, 30, away);
        }
    }
    const double cycle = std::fmod(this->now_ + 3.0, 7.3);
    const double peck = !flying && !this->reduced_ && cycle < 0.5
                            ? std::sin(cycle / 0.5 * TAU / 2)
                            : 0.0;
    const double scale = 1.15 * lamp.size;
    p.save();
    p.translate(pos);
    p.scale(scale, scale);
    p.setOpacity(alpha);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(9, 10, 13));
    QPainterPath bird;
    bird.setFillRule(Qt::WindingFill);
    bird.addEllipse(QPointF(0, -3.3), 4.3, 2.8);
    bird.moveTo(3.0, -3.9);
    bird.lineTo(7.8, -2.4);
    bird.lineTo(3.4, -1.7);
    bird.closeSubpath();
    const QPointF head(-3.7, -6.0 + peck * 2.8);
    bird.addEllipse(head, 1.65, 1.5);
    bird.moveTo(-4.6, -4.6);
    bird.lineTo(head.x() - 0.6, head.y() + 0.4);
    bird.lineTo(head.x() + 1.4, head.y() + 0.6);
    bird.lineTo(-1.6, -4.4);
    bird.closeSubpath();
    if (flying)
    {
        const double flap = std::sin(this->now_ * 26);
        bird.moveTo(-1.5, -4.2);
        bird.lineTo(1.5, -4.2 - 6 * flap);
        bird.lineTo(3.2, -4.0);
        bird.closeSubpath();
    }
    p.drawPath(bird);
    p.setBrush(QColor(150, 110, 60, 160));
    p.drawEllipse(QPointF(head.x() - 1.75, head.y() + 0.3), 0.6, 0.35);
    if (!flying)
    {
        p.fillRect(QRectF(-1.2, -0.9, 0.6, 0.9), QColor(60, 40, 34));
        p.fillRect(QRectF(0.6, -0.9, 0.6, 0.9), QColor(60, 40, 34));
    }
    // Lamp light catching its breast from below
    p.setPen(QPen(QColor(190, 136, 70, 150), 0.7));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(-4.3, -6.1, 8.6, 5.6), 200 * 16, 110 * 16);
    p.restore();
}

void DepartureScene::paintLamps(QPainter &p) const
{
    const auto &g = this->geo_;
    const double H = g.win.height();
    for (const auto &lamp : this->lamps_)
    {
        const double level = clamp01(lamp.level) * (1 + 0.6 * lamp.swell);
        const QPointF bulb = lamp.bulb;
        const double s = lamp.size;
        drawGlow(p, bulb + QPointF(0, 26 * s), 120 * s, LAMP_HAZE, 0.075 * level);
        drawGlow(p, bulb, 30 * s, LAMP_WARM, 0.42 * level);
        drawGlow(p, bulb, 9 * s, LAMP_CORE, clamp01(0.15 + 0.85 * level));
        // A pool of light on the platform and a glint on the rails
        p.save();
        p.translate(bulb.x(), g.platTop + 2);
        p.scale(1, 0.16);
        drawGlow(p, QPointF(0, 0), 70 * s, LAMP_WARM, 0.25 * level);
        p.restore();
        p.save();
        p.translate(bulb.x(), H - 4);
        p.scale(1, 0.1);
        drawGlow(p, QPointF(0, 0), 26 * s, LAMP_CORE, 0.55 * level);
        p.restore();
    }
}

void DepartureScene::paintHaze(QPainter &p) const
{
    const auto &g = this->geo_;
    const double W = g.win.width();
    double level = 0;
    for (const auto &lamp : this->lamps_)
    {
        level += lamp.level;
    }
    level = this->lamps_.empty() ? 1.0 : level / this->lamps_.size();
    const double t = this->reduced_ ? 0.0 : this->now_;
    const double top = std::max(30.0, g.topBottom);
    // Drifting haze stays below the tab names
    const double floorY =
        g.tabLabels.isEmpty() ? 0.0 : g.tabLabels.bottom() + 20;
    for (int i = 0; i < 6; ++i)
    {
        const double speed = 3 + 4 * hash01(i, 71);
        const double span = W + 300;
        double x = hash01(i, 72) * span + t * speed;
        x = std::fmod(x, span) - 150;
        const double y = std::max(top * 0.7, floorY) + hash01(i, 73) * 50 +
                         18 * std::sin(t * 0.06 + i * 1.7);
        drawGlow(p, QPointF(x, y), 90 + 60 * hash01(i, 74), LAMP_HAZE,
                 0.04 * level);
    }
}

}  // namespace

std::unique_ptr<ThemeScene> makeDepartureScene()
{
    return std::make_unique<DepartureScene>();
}

}  // namespace chatterino::scenes
