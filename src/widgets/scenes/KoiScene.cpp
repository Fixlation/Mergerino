// SPDX-License-Identifier: MIT

// Koi Pond: a courtyard koi pond seen from directly above, laid as a stone
// mosaic. Every frame the pond is rendered into a low-resolution colour
// field, one value per tessera, which is then drawn as square tiles under a
// cached grout layer. The tiles never move: koi, ripples and light read as
// colour passing from tile to tile.

#include "widgets/scenes/ThemeScene.hpp"

#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace chatterino::scenes {

namespace {

// Colours ---------------------------------------------------------------------

struct Rgb {
    double r = 0;
    double g = 0;
    double b = 0;
};

constexpr Rgb rgb(int r, int g, int b)
{
    return {r / 255.0, g / 255.0, b / 255.0};
}

inline Rgb mixRgb(const Rgb &a, const Rgb &b, double t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t};
}

inline Rgb mulRgb(const Rgb &a, double k)
{
    return {a.r * k, a.g * k, a.b * k};
}

inline Rgb addRgb(const Rgb &a, const Rgb &b, double k)
{
    return {a.r + b.r * k, a.g + b.g * k, a.b + b.b * k};
}

inline double wrapAngle(double a)
{
    return std::remainder(a, TAU);
}

/// Rises and falls once while u goes from 0 to 1.
inline double bump(double u)
{
    return (u <= 0 || u >= 1) ? 0.0 : std::sin(u * TAU / 2);
}

std::uint32_t hashString(const QString &text)
{
    std::uint32_t h = 2166136261U;
    for (const auto ch : text)
    {
        h = (h ^ ch.unicode()) * 16777619U;
    }
    return h;
}

// Water from the shallow rim to the deep middle
constexpr std::array<Rgb, 5> WATER{rgb(62, 106, 127), rgb(45, 84, 105),
                                   rgb(32, 66, 87), rgb(22, 50, 68),
                                   rgb(16, 34, 50)};
constexpr Rgb CALM_WATER = rgb(43, 55, 66);
constexpr Rgb CONTOUR = rgb(18, 31, 45);
constexpr Rgb SUN = rgb(255, 250, 226);
constexpr Rgb DEEP_TINT = rgb(24, 51, 72);
constexpr Rgb WEED = rgb(62, 116, 86);
constexpr std::array<Rgb, 3> PEBBLE{rgb(178, 170, 142), rgb(150, 158, 146),
                                    rgb(196, 176, 140)};

// Warm stone for the rim
constexpr std::array<Rgb, 5> STONE{rgb(99, 91, 81), rgb(88, 81, 74),
                                   rgb(75, 72, 66), rgb(111, 100, 85),
                                   rgb(93, 80, 65)};
constexpr Rgb COPING = rgb(68, 65, 60);
constexpr Rgb MOSS = rgb(88, 99, 71);
constexpr Rgb VERMILION = rgb(194, 84, 54);
constexpr Rgb CHARCOAL = rgb(46, 46, 45);
constexpr Rgb JADE = rgb(64, 128, 104);
constexpr Rgb SLATE = rgb(150, 154, 148);

constexpr std::array<Rgb, 3> PAD{rgb(96, 148, 80), rgb(114, 162, 88),
                                 rgb(84, 134, 76)};
constexpr Rgb PETAL_PINK = rgb(236, 146, 170);
constexpr Rgb PETAL_WHITE = rgb(253, 242, 242);
constexpr Rgb LOTUS_PINK = rgb(222, 98, 136);
constexpr Rgb POLLEN = rgb(246, 204, 86);
constexpr Rgb GOLD = rgb(238, 188, 72);
constexpr Rgb LANTERN_CORE = rgb(255, 232, 178);
constexpr Rgb LANTERN_GLOW = rgb(255, 176, 92);
constexpr Rgb LEAF = rgb(204, 132, 58);

// Koi varieties
enum Look : std::uint8_t {
    KOHAKU,
    ORANGE,
    SHOWA,
    SANKE,
    PLATINUM,
    YAMABUKI,
    GOLDEN,
    TANCHO,
};

struct KoiLook {
    Rgb base;
    Rgb patch;
    Rgb spot;
    Rgb fin;
    /// Noise level above which the patch colour shows, 0 for none
    double patchAt;
    /// Noise level above which spots show, 0 for none
    double spotAt;
    bool metallic;
};

const std::array<KoiLook, 8> LOOKS{{
    {rgb(246, 242, 234), rgb(226, 84, 42), {}, rgb(250, 244, 236), 0.50, 0,
     false},
    {rgb(238, 132, 48), rgb(250, 182, 100), {}, rgb(250, 196, 140), 0.64, 0,
     false},
    {rgb(38, 38, 42), rgb(214, 74, 40), rgb(242, 238, 228), rgb(96, 94, 96),
     0.48, 0.72, false},
    {rgb(246, 242, 234), rgb(222, 78, 40), rgb(36, 36, 40), rgb(248, 242, 234),
     0.54, 0.74, false},
    {rgb(232, 236, 234), rgb(252, 252, 248), {}, rgb(242, 244, 242), 0.6, 0,
     true},
    {rgb(238, 214, 124), rgb(250, 232, 166), {}, rgb(246, 228, 166), 0.6, 0,
     false},
    {rgb(234, 176, 56), rgb(252, 222, 122), {}, rgb(248, 208, 112), 0.56, 0,
     true},
    {rgb(247, 245, 239), rgb(214, 40, 34), {}, rgb(250, 247, 243), 0, 0,
     false},
}};

// Simulation objects ------------------------------------------------------------

enum class Role : std::uint8_t {
    /// Part of the resident school
    School,
    /// Darts from the input along your sent message
    Dart,
    /// The sub koi swimming its lap
    Lap,
    /// Sweeps across with a raid
    Raid,
    /// One of the shoutout pair
    Pair,
    /// Circles a mention
    Circle,
};

struct Koi {
    Role role = Role::School;
    int look = KOHAKU;
    double seed = 0;
    double length = 100;
    double x = 0;
    double y = 0;
    double heading = 0;
    double speed = 20;
    double omega = 0;
    double bend = 0;
    double phase = 0;
    double z = 0.4;
    double zTarget = 0.4;
    double fade = 0;
    /// 0..1; fish with a low rank swim first when chat is slow
    double rank = 0;
    double tx = 0;
    double ty = 0;
    double retargetAt = 0;
    double pushX = 0;
    double pushY = 0;
    bool leaving = false;
    double leaveAt = 1e9;
    double glow = 0;
    /// 0..1 while the koi leaps at the surface
    double leap = 0;
    bool dead = false;

    // Role data
    double start = 0;
    double duration = 0;
    double cx = 0;
    double cy = 0;
    double rx = 0;
    double ry = 0;
    double angle = 0;
    double travelled = 0;
    double cruise = 0;
    std::array<QPointF, 4> path{};
};

struct Ripple {
    double x;
    double y;
    double start;
    double strength;
    double speed;
    double life;
    double width;
};

struct WakeDot {
    double x;
    double y;
    double start;
};

struct Fry {
    double x;
    double y;
    double vx;
    double vy;
    double start;
    double life;
    double phase;
    Rgb color;
};

struct Coin {
    double x;
    double y;
    double start;
    double sinkTime;
    double phase;
    double drift;
};

struct PendingCoin {
    double at;
    double x;
    double y;
};

struct Pad {
    double x;
    double y;
    double fromX;
    double fromY;
    double radius;
    double rot;
    double start;
    double life;
    int look;
    /// Time the flower starts to open, or < 0 for a plain pad
    double bloomAt;
};

struct Lantern {
    double x;
    double y;
    double vx;
    double vy;
    double phase;
};

struct Petal {
    double x;
    double y;
    double vx;
    double vy;
    double start;
    double life;
    bool leaf;
};

struct Glint {
    double x;
    double y;
    double vx;
    double vy;
    double start;
    double life;
    double size;
    /// 0 gold, 1 vermilion, 2 jade, 3 pearl
    int hue;
};

struct Ring {
    double x;
    double y;
    double start;
    double life;
    double r0;
    double r1;
    QColor color;
    double alpha;
};

/// Centre line of a koi: nodes 0..8 run head to tail root, node 9 is the tip
/// of the tail fin.
struct Spine {
    std::array<double, 10> x{};
    std::array<double, 10> y{};
};

/// Half width of a koi body as a fraction of its length, s = 0 at the snout
/// and 1 at the tail root.
double bodyHalf(double s)
{
    if (s < 0.16)
    {
        return 0.165 * (0.45 + 0.55 * std::sqrt(std::max(0.0, s) / 0.16));
    }
    if (s > 1)
    {
        return 0;
    }
    const double u = (s - 0.16) / 0.84;
    return 0.165 * (1 - 0.80 * std::pow(u, 1.30));
}

/// Nearest point on the spine: `s` along the body (up to 1.3 on the tail
/// fin) and the signed distance `d` from the centre line.
void nearestOnSpine(const Spine &sp, double px, double py, double &s,
                    double &d)
{
    double best = 1e30;
    for (int i = 0; i < 9; ++i)
    {
        const double ax = sp.x[i];
        const double ay = sp.y[i];
        const double bx = sp.x[i + 1] - ax;
        const double by = sp.y[i + 1] - ay;
        const double len2 = bx * bx + by * by + 1e-9;
        const double u =
            std::clamp(((px - ax) * bx + (py - ay) * by) / len2, 0.0, 1.0);
        const double qx = px - (ax + bx * u);
        const double qy = py - (ay + by * u);
        const double dist2 = qx * qx + qy * qy;
        if (dist2 < best)
        {
            best = dist2;
            s = i < 8 ? (i + u) / 8.0 : 1.0 + 0.30 * u;
            const double dist = std::sqrt(dist2);
            d = (bx * qy - by * qx) >= 0 ? dist : -dist;
        }
    }
}

QPointF bezier(const std::array<QPointF, 4> &p, double u)
{
    const double v = 1 - u;
    return p[0] * (v * v * v) + p[1] * (3 * v * v * u) +
           p[2] * (3 * v * u * u) + p[3] * (u * u * u);
}

class KoiScene final : public ThemeScene
{
public:
    KoiScene();

    SceneInfo info() const override;
    SceneGlass glass() const override;
    void step(const SceneState &state, const SceneLayout &layout) override;
    void event(const SceneEvent &event, const SceneState &state,
               const SceneLayout &layout) override;
    void paintBack(QPainter &painter, const SceneState &state,
                   const SceneLayout &layout) override;
    bool hasFront() const override;
    void paintFront(QPainter &painter, const SceneState &state,
                    const SceneLayout &layout) override;

private:
    // Grid and cached layers
    void ensureGrid(const SceneLayout &layout);
    void buildGrout();
    void rebuildBase();
    Rgb stoneColour(double x, double y, double wx, double wy, double rd) const;
    double rimDistance(double x, double y) const;
    int tileAt(double x, double y) const;
    double calmAt(double x, double y) const;
    double quietAt(double x, double y) const;
    /// Where koi gather while you type: the head of the middle fish, how far
    /// the others spread up and down, and the point they nose towards
    void gatherSpot(const SceneState &state, QPointF &head, double &spread,
                    QPointF &face) const;
    /// Where the dart from a sent message surfaces: free water in the band
    /// around the input, clear of its text, buttons and emotes
    QPointF landingPoint() const;
    QRectF interior() const;
    double tileX(int i) const;
    double tileY(int j) const;

    // Simulation
    void startUp(const SceneState &state);
    void updateSchoolSize(const SceneState &state);
    void updateKoi(const SceneState &state);
    void steerSchool(Koi &k, const SceneState &state,
                     double &speedWanted, double &turnRate, double &accel);
    void moveDart(Koi &k, const SceneState &state);
    void moveCircle(Koi &k, const SceneState &state);
    void separate(double dt);
    void updateParticles(const SceneState &state);
    void updateSlide(double t, double dt);
    Koi &addKoi(Role role, int look, double length, double x, double y,
                double heading);
    int pickLook();
    QPointF wanderPoint();
    void addRipple(double x, double y, double strength, double speed,
                   double life, double width);
    void addGlint(double x, double y, double vx, double vy, double life,
                  double size, int hue);
    void addRing(double x, double y, double life, double r0, double r1,
                 const QColor &color, double alpha);
    void addChatterPad();
    void addBloom(double x, double y);
    void lightLanterns();
    void startDart(const SceneEvent &event, const SceneState &state);

    // Rendering into the colour field
    void shadeWater();
    void applyRipples();
    void applyWake();
    void drawCoins();
    void drawAllKoi();
    void drawKoi(const Koi &k);
    Spine spineOf(const Koi &k) const;
    Rgb koiColour(const Koi &k, double s, double dn, int i, int j) const;
    void drawFry();
    void drawPads();
    void drawPad(double x, double y, double r, double rot, double alpha,
                 int look);
    void drawFlower(double x, double y, double r, double open, double rot,
                    double alpha, double petals, bool lotus, bool vivid = false);
    void drawLotus();
    void drawPetals();
    void drawRaidCrest();
    void drawHeron();
    void gradeAndLights();
    void writeImage();
    void blendTile(int idx, const Rgb &c, double a);

    // Window and grid
    SceneLayout layout_;
    QSizeF window_;
    qreal dpr_ = 0;
    std::vector<QRectF> chats_;
    QRectF tabs_;
    QRectF tabLabels_;
    std::vector<QRectF> activity_;
    QRectF focusInput_;
    QRectF focusChat_;
    double tile_ = 8;
    int tileDev_ = 8;
    int cols_ = 0;
    int rows_ = 0;
    int oxDev_ = 0;
    int oyDev_ = 0;
    double ox_ = 0;
    double oy_ = 0;
    double rimL_ = 18;
    double rimR_ = 18;
    double rimT_ = 36;
    double rimB_ = 14;
    double rimRadius_ = 64;

    // Per tile
    std::vector<Rgb> base_;
    std::vector<Rgb> field_;
    std::vector<double> water_;
    std::vector<double> depth_;
    std::vector<double> calm_;
    /// 1 where the scene must stay quiet: chat text, tab names, the input's
    /// buttons and the emotes beside it
    std::vector<double> quiet_;
    /// How much evening and darkening may touch a tile: none under chat
    /// text, a little under Activity, fully elsewhere
    std::vector<double> shade_;
    std::vector<double> jitterLight_;
    std::vector<double> jitterWarm_;
    QImage fieldImage_;
    QImage grout_;
    bool baseDirty_ = true;

    // World
    SceneRng rng_{0x6b6f69U};
    bool started_ = false;
    double now_ = 0;
    bool reduced_ = false;
    double shimmer_ = 0;
    double camX_ = 0;
    double camY_ = 0;
    std::uint32_t pattern_ = 1;
    std::uint32_t nextPattern_ = 1;
    double slideStart_ = -10;
    double slideFromX_ = 0;
    double slideFromY_ = 0;
    double slideToX_ = 0;
    double slideToY_ = 0;
    double slideVelX_ = 0;
    double slideVelY_ = 0;
    double evening_ = 0;
    double morningStart_ = -10;
    double nextSpawn_ = 0;
    double lastPlip_ = -10;

    std::vector<Koi> koi_;
    std::vector<Ripple> ripples_;
    std::vector<WakeDot> wake_;
    std::vector<Fry> fry_;
    std::vector<Coin> coins_;
    std::vector<PendingCoin> pendingCoins_;
    std::vector<Pad> pads_;
    std::vector<Lantern> lanterns_;
    std::vector<Petal> petals_;
    std::vector<Glint> glints_;
    std::vector<Ring> rings_;

    // Events that shape behaviour for a while
    double ringUntil_ = -10;
    double ringStart_ = -10;
    QPointF ringCentre_;
    double ringRadius_ = 100;
    double heronStart_ = -10;
    QPointF heronFrom_;
    QPointF heronTo_;
    double heronScale_ = 1;
    double raidStart_ = -10;
    double raidStrength_ = 0;
    double raidSpeed_ = 400;
    double padShift_ = 0;
    double lotusPetals_ = 0;
    int hypeLevel_ = 0;
};

KoiScene::KoiScene() = default;

SceneInfo KoiScene::info() const
{
    return {"koi", "Koi Pond", QColor("#25292f"), QColor("#efe4d2"),
            QColor("#dc895f"), false};
}

SceneGlass KoiScene::glass() const
{
    // Keep the mosaic soft under messages and dim it beneath controls.
    return {.text = 0.74, .chrome = 0.62, .blur = 0.88};
}

// Grid and cached layers --------------------------------------------------------

double KoiScene::tileX(int i) const
{
    return this->ox_ + (i + 0.5) * this->tile_;
}

double KoiScene::tileY(int j) const
{
    return this->oy_ + (j + 0.5) * this->tile_;
}

int KoiScene::tileAt(double x, double y) const
{
    const int i = static_cast<int>(std::floor((x - this->ox_) / this->tile_));
    const int j = static_cast<int>(std::floor((y - this->oy_) / this->tile_));
    if (i < 0 || j < 0 || i >= this->cols_ || j >= this->rows_)
    {
        return -1;
    }
    return j * this->cols_ + i;
}

double KoiScene::calmAt(double x, double y) const
{
    const int idx = this->tileAt(x, y);
    return idx < 0 ? 0.0 : this->calm_[idx];
}

double KoiScene::quietAt(double x, double y) const
{
    const int idx = this->tileAt(x, y);
    return idx < 0 ? 0.0 : this->quiet_[idx];
}

void KoiScene::gatherSpot(const SceneState &state, QPointF &head,
                          double &spread, QPointF &face) const
{
    const auto band = this->layout_.inputBand();
    const auto &input = this->layout_.focusInput;
    const QPointF caret = state.caret;
    const double above = input.top() - band.top();
    if (above >= 30)
    {
        // An emote or reply bar sits above the box: the koi lie in its open
        // right-hand side, noses towards the caret
        const double y = (band.top() + input.top()) / 2;
        const double x = std::clamp(caret.x() + 70,
                                    band.left() + band.width() * 0.64,
                                    std::max(band.left() + band.width() * 0.64,
                                             band.right() - 40));
        head = QPointF(x, y);
        spread = std::min(14.0, above * 0.28);
        face = QPointF(caret.x(), y + 4);
        return;
    }
    // Otherwise in the box itself, between the caret and the buttons
    head = QPointF(std::min(caret.x() + 24, input.right() - 60), caret.y());
    spread = std::min(9.0, input.height() * 0.25);
    face = caret;
}

QPointF KoiScene::landingPoint() const
{
    const auto band = this->layout_.inputBand();
    const auto &input = this->layout_.focusInput;
    if (band.isEmpty() || input.isEmpty())
    {
        const auto inner = this->interior();
        return {inner.right() - 60, inner.bottom() - 30};
    }
    if (input.top() - band.top() >= 30)
    {
        return {band.right() - std::min(56.0, band.width() * 0.16),
                (band.top() + input.top()) / 2};
    }
    return {std::max(input.left() + input.width() * 0.45,
                     input.right() - 175),
            input.center().y()};
}

QRectF KoiScene::interior() const
{
    const double w = this->window_.width();
    const double h = this->window_.height();
    return {this->rimL_ + 26, this->rimT_ + 18,
            std::max(10.0, w - this->rimL_ - this->rimR_ - 52),
            std::max(10.0, h - this->rimT_ - this->rimB_ - 26)};
}

void KoiScene::ensureGrid(const SceneLayout &layout)
{
    const qreal dpr = std::max<qreal>(layout.dpr, 1.0);
    if (!this->fieldImage_.isNull() && layout.window == this->window_ &&
        dpr == this->dpr_ && layout.chats == this->chats_ &&
        layout.tabs == this->tabs_ && layout.tabLabels == this->tabLabels_ &&
        layout.focusInput == this->focusInput_ &&
        layout.focusChat == this->focusChat_ &&
        layout.activity == this->activity_)
    {
        return;
    }
    this->activity_ = layout.activity;
    this->window_ = layout.window;
    this->dpr_ = dpr;
    this->chats_ = layout.chats;
    this->tabs_ = layout.tabs;
    this->tabLabels_ = layout.tabLabels;
    this->focusInput_ = layout.focusInput;
    this->focusChat_ = layout.focusChat;

    const double w = std::max(1.0, layout.window.width());
    const double h = std::max(1.0, layout.window.height());

    // About 160 tiles across; tiles stay 7..10 px and land on whole device
    // pixels so the grout lines up with the colour field.
    const int logical =
        std::clamp(static_cast<int>(std::lround(w / 160.0)), 7, 10);
    this->tileDev_ = std::max(4, static_cast<int>(std::lround(logical * dpr)));
    this->tile_ = this->tileDev_ / dpr;
    const int devW = std::max(1, static_cast<int>(std::lround(w * dpr)));
    const int devH = std::max(1, static_cast<int>(std::lround(h * dpr)));
    this->cols_ = devW / this->tileDev_ + 2;
    this->rows_ = devH / this->tileDev_ + 2;
    this->oxDev_ = -(this->cols_ * this->tileDev_ - devW) / 2;
    this->oyDev_ = -(this->rows_ * this->tileDev_ - devH) / 2;
    this->ox_ = this->oxDev_ / dpr;
    this->oy_ = this->oyDev_ / dpr;

    // The stone rim runs under the whole tab strip.
    const double tabsBottom =
        std::max(layout.tabs.isEmpty() ? 0.0 : layout.tabs.bottom(),
                 layout.tabLabels.isEmpty() ? 0.0 : layout.tabLabels.bottom());
    this->rimT_ = std::clamp(tabsBottom + 6, 16.0, 140.0);
    this->rimL_ = 14;
    this->rimR_ = 14;
    this->rimB_ = 7;
    this->rimRadius_ = std::clamp(std::min(w, h) * 0.09, 30.0, 90.0);

    const auto n = static_cast<size_t>(this->cols_ * this->rows_);
    this->base_.assign(n, {});
    this->field_.assign(n, {});
    this->water_.assign(n, 0);
    this->depth_.assign(n, 0);
    this->calm_.assign(n, 0);
    this->quiet_.assign(n, 0);
    this->shade_.assign(n, 1);
    this->jitterLight_.assign(n, 0);
    this->jitterWarm_.assign(n, 0);

    const double T = this->tile_;
    std::vector<QRectF> quietRects;
    if (!layout.tabLabels.isEmpty())
    {
        quietRects.push_back(layout.tabLabels);
    }
    if (!layout.focusInput.isEmpty())
    {
        // The row of buttons on the right of the message box
        const auto &in = layout.focusInput;
        quietRects.emplace_back(in.right() - std::min(150.0, in.width() * 0.5),
                                in.top(), std::min(150.0, in.width() * 0.5),
                                in.height());
        // The emotes on the left of a bar above it
        const auto band = layout.inputBand();
        if (in.top() - band.top() >= 30)
        {
            quietRects.emplace_back(band.left(), band.top(), band.width() * 0.62,
                                    in.top() - band.top());
        }
    }
    for (int j = 0; j < this->rows_; ++j)
    {
        for (int i = 0; i < this->cols_; ++i)
        {
            const int idx = j * this->cols_ + i;
            // Each tessera is cut from a slightly different stone
            this->jitterLight_[idx] = (hash01(i, j, 5) - 0.5) * 0.10;
            this->jitterWarm_[idx] = (hash01(i, j, 6) - 0.5) * 0.035;

            const double x = this->tileX(i);
            const double y = this->tileY(j);
            double calm = 0;
            for (const auto &chat : layout.chats)
            {
                const double inside = std::min(
                    {x - chat.left(), chat.right() - x, y - chat.top(),
                     chat.bottom() - y});
                calm = std::max(calm, smoothstep(-T, 3 * T, inside));
            }
            this->calm_[idx] = calm;
            double quiet = calm;
            for (const auto &r : quietRects)
            {
                const double inside = std::min(
                    {x - r.left(), r.right() - x, y - r.top(), r.bottom() - y});
                quiet = std::max(quiet, smoothstep(-T, T, inside));
            }
            this->quiet_[idx] = quiet;
            double activity = 0;
            for (const auto &a : layout.activity)
            {
                const double inside = std::min(
                    {x - a.left(), a.right() - x, y - a.top(), a.bottom() - y});
                activity = std::max(activity, smoothstep(-T, 2 * T, inside));
            }
            this->shade_[idx] = (1 - calm) * (1 - 0.55 * activity);
        }
    }

    this->fieldImage_ = QImage(this->cols_, this->rows_, QImage::Format_RGB32);
    this->buildGrout();
    this->baseDirty_ = true;
}

void KoiScene::buildGrout()
{
    const int devW = std::max(
        1, static_cast<int>(std::lround(this->window_.width() * this->dpr_)));
    const int devH = std::max(
        1, static_cast<int>(std::lround(this->window_.height() * this->dpr_)));
    QImage image(devW, devH, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(44, 40, 34, 108));

    const int td = this->tileDev_;
    const int gap = std::max(1, static_cast<int>(std::lround(this->dpr_ * 0.8)));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);

    // Each tessera is a little smaller or larger than its neighbours
    std::vector<QRect> faces;
    faces.reserve(static_cast<size_t>(this->cols_ * this->rows_));
    for (int j = 0; j < this->rows_; ++j)
    {
        for (int i = 0; i < this->cols_; ++i)
        {
            const int x0 = this->oxDev_ + i * td;
            const int y0 = this->oyDev_ + j * td;
            if (x0 + td < 0 || y0 + td < 0 || x0 > devW || y0 > devH)
            {
                continue;
            }
            const int gl = gap + (hash01(i, j, 21) < 0.28 ? 1 : 0);
            const int gt = gap + (hash01(i, j, 22) < 0.28 ? 1 : 0);
            const int gr = hash01(i, j, 23) < 0.45 ? 1 : 0;
            const int gb = hash01(i, j, 24) < 0.45 ? 1 : 0;
            faces.emplace_back(x0 + gl, y0 + gt, td - gl - gr, td - gt - gb);
        }
    }

    p.setCompositionMode(QPainter::CompositionMode_Source);
    for (const auto &face : faces)
    {
        p.fillRect(face, Qt::transparent);
    }

    // A soft bevel: light from the top left
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (size_t k = 0; k < faces.size(); ++k)
    {
        const auto &f = faces[k];
        const int lightA =
            14 + static_cast<int>(hash01(static_cast<int>(k), 31) * 14);
        p.fillRect(QRect(f.left(), f.top(), f.width(), gap),
                   QColor(255, 255, 255, lightA));
        p.fillRect(QRect(f.left(), f.top() + gap, gap, f.height() - gap),
                   QColor(255, 255, 255, lightA / 2));
        p.fillRect(QRect(f.left(), f.bottom() - gap + 1, f.width(), gap),
                   QColor(0, 0, 0, 30));
        p.fillRect(QRect(f.right() - gap + 1, f.top(), gap, f.height() - gap),
                   QColor(0, 0, 0, 18));
    }

    // Under chat text the joints fade so the glass stays quiet
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    for (const auto &chat : this->chats_)
    {
        const QRectF dev(chat.left() * this->dpr_, chat.top() * this->dpr_,
                         chat.width() * this->dpr_, chat.height() * this->dpr_);
        p.fillRect(dev.toAlignedRect(), QColor(0, 0, 0, 40));
    }
    for (const auto &a : this->activity_)
    {
        const QRectF dev(a.left() * this->dpr_, a.top() * this->dpr_,
                         a.width() * this->dpr_, a.height() * this->dpr_);
        p.fillRect(dev.toAlignedRect(), QColor(0, 0, 0, 80));
    }
    // and soften a little under the tab labels
    const QRectF labels =
        this->tabLabels_.isEmpty() ? this->tabs_ : this->tabLabels_;
    if (!labels.isEmpty())
    {
        const QRectF dev(labels.left() * this->dpr_, labels.top() * this->dpr_,
                         labels.width() * this->dpr_,
                         labels.height() * this->dpr_);
        p.fillRect(dev.toAlignedRect(), QColor(0, 0, 0, 170));
    }
    p.end();
    image.setDevicePixelRatio(this->dpr_);
    this->grout_ = image;
}

double KoiScene::rimDistance(double x, double y) const
{
    const double w = this->window_.width();
    const double h = this->window_.height();
    const double l = this->rimL_;
    const double t = this->rimT_;
    const double r = w - this->rimR_;
    const double b = h - this->rimB_;
    const double hx = std::max(1.0, (r - l) / 2);
    const double hy = std::max(1.0, (b - t) / 2);
    const double rad = std::min({this->rimRadius_, hx, hy});
    const double qx = std::abs(x - (l + r) / 2) - (hx - rad);
    const double qy = std::abs(y - (t + b) / 2) - (hy - rad);
    const double outside =
        std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) +
        std::min(std::max(qx, qy), 0.0) - rad;
    return -outside;
}

Rgb KoiScene::stoneColour(double x, double y, double wx, double wy,
                          double rd) const
{
    const double T = this->tile_;
    const double w = this->window_.width();
    const double h = this->window_.height();
    const int wtx = static_cast<int>(std::floor(wx / T));
    const int wty = static_cast<int>(std::floor(wy / T));
    const auto seed = static_cast<int>(this->pattern_ & 0xffff);

    // Each channel's stretch of rim is laid in its own stone
    static const std::array<Rgb, 3> TINTS{rgb(255, 250, 240), rgb(250, 240, 230),
                                          rgb(240, 244, 242)};
    const Rgb tint = TINTS[this->pattern_ % 3];
    const auto tinted = [&](const Rgb &c) {
        return Rgb{c.r * tint.r, c.g * tint.g, c.b * tint.b};
    };

    // The mosaicist's seal, set into the top rim near the right corner
    const double sealX = w - this->rimR_ - this->rimRadius_ - 3 * T;
    const bool sealFits =
        this->rimT_ < 56 &&
        (this->tabLabels_.isEmpty() || this->tabLabels_.right() + T < sealX);
    if (sealFits && y > T && y < this->rimT_ - T && x > sealX &&
        x < sealX + 3 * T)
    {
        const bool centre = x > sealX + T && x < sealX + 2 * T &&
                            y > T * 1.6 && y < this->rimT_ - T * 1.6;
        return centre ? STONE[3] : mulRgb(VERMILION, 0.92);
    }

    // Wet coping stones right at the water
    if (rd > -T * 1.05)
    {
        const double v = hash01(wtx, wty, seed + 3);
        return tinted(mulRgb(COPING, 0.94 + 0.10 * v));
    }

    // A dark frame line along the window edge, set with accent tesserae at
    // a spacing chosen by the channel
    const double edge = std::min({x, y, w - x, h - y});
    if (edge < T * 0.95)
    {
        const bool horizontal = y < T || y > h - T;
        const int u = horizontal ? wtx : wty;
        const int spacing = 4 + static_cast<int>((this->pattern_ / 3) % 4) * 2;
        const Rgb accent = (this->pattern_ / 12) % 2 == 0 ? VERMILION : JADE;
        const int m = ((u % spacing) + spacing) % spacing;
        const Rgb c = m == 0 ? accent : CHARCOAL;
        return mulRgb(c, 0.96 + 0.08 * hash01(wtx, wty, seed + 4));
    }

    // Opus regulatum: stone tiles laid in rows, gently varied
    const double pick = hash01(wtx, wty, seed + 5);
    Rgb c = STONE[static_cast<size_t>(std::min(4.0, pick * 5))];
    const double vein = valueNoise(wx / 70.0 + seed, wy / 70.0);
    c = mulRgb(c, 0.97 + 0.05 * vein);
    // Moss creeps along the rim in places
    const double moss = valueNoise(wx / 38.0 + 11, wy / 38.0 + seed);
    if (rd > -T * 3.2 && moss > 0.66)
    {
        c = mixRgb(c, MOSS, std::min(0.35, (moss - 0.66) * 2.0));
    }
    return tinted(c);
}

void KoiScene::rebuildBase()
{
    this->baseDirty_ = false;
    const double w = this->window_.width();
    const double h = this->window_.height();
    const double T = this->tile_;
    // The deepest water lies towards the Activity pane, where it can be seen
    double cx = (this->rimL_ + w - this->rimR_) / 2;
    double cy = (this->rimT_ + h - this->rimB_) / 2;
    if (!this->layout_.activity.empty())
    {
        const auto act = this->layout_.activity.front().center();
        cx = lerp(cx, act.x(), 0.6);
        cy = lerp(cy, act.y(), 0.4);
    }
    const double hx = std::max(1.0, (w - this->rimL_ - this->rimR_) / 2);
    const double hy = std::max(1.0, (h - this->rimT_ - this->rimB_) / 2);
    const auto seed = static_cast<int>(this->pattern_ & 0xffff);

    for (int j = 0; j < this->rows_; ++j)
    {
        for (int i = 0; i < this->cols_; ++i)
        {
            const int idx = j * this->cols_ + i;
            const double x = this->tileX(i);
            const double y = this->tileY(j);
            const double wx = x + this->camX_;
            const double wy = y + this->camY_;
            const double wobble =
                (valueNoise(wx / 46.0 + seed * 0.37, wy / 46.0) - 0.5) * 7;
            const double rd = this->rimDistance(x, y) + wobble;

            if (rd < 0)
            {
                this->water_[idx] = 0;
                this->depth_[idx] = 0;
                this->base_[idx] = this->stoneColour(x, y, wx, wy, rd);
                continue;
            }

            // Deeper towards the middle, with soft basins and shelves
            const double shelf = std::pow(smoothstep(0, 120, rd), 0.7);
            const double radial =
                1 - std::min(1.0, std::hypot((x - cx) / hx, (y - cy) / hy));
            const double basin =
                (valueNoise(wx / 180.0 + 5, wy / 180.0 + seed) - 0.5) * 0.34;
            const double depth =
                clamp01(0.08 + 0.55 * shelf + 0.32 * radial + basin);
            const double pos = depth * (WATER.size() - 1);
            const auto lo = static_cast<size_t>(std::min(3.0, std::floor(pos)));
            Rgb c = mixRgb(WATER[lo], WATER[lo + 1], pos - lo);

            // Pebbles on the shallow floor, weed in the middle depths
            const int wtx = static_cast<int>(std::floor(wx / T));
            const int wty = static_cast<int>(std::floor(wy / T));
            const double pebble = hash01(wtx, wty, seed + 9);
            if (depth < 0.55 && pebble < 0.14 * (1 - depth * 1.6))
            {
                const auto which = static_cast<size_t>(
                    std::min(2.0, hash01(wtx, wty, seed + 10) * 3));
                c = mixRgb(c, PEBBLE[which], 0.34 * (1 - depth * 1.5));
            }
            const double weed = valueNoise(wx / 58.0 + 31, wy / 58.0 + 7);
            if (weed > 0.66 && depth > 0.22)
            {
                c = mixRgb(c, WEED, std::min(0.4, (weed - 0.66) * 2.4));
            }

            // The rim shades the water right below it
            if (rd < 24)
            {
                c = mulRgb(c, 1 - 0.24 * (1 - rd / 24));
            }

            // Morning sun from the top left
            c = mulRgb(c, 1.06 - 0.14 * (0.6 * x / w + 0.4 * y / h));
            const double calm = this->calm_[idx];
            c = mixRgb(c, CALM_WATER, 0.55 * calm);
            this->water_[idx] = clamp01(rd / (T * 0.6));
            this->depth_[idx] = depth * (1 - 0.6 * calm);
            this->base_[idx] = c;
        }
    }
}

// Simulation ----------------------------------------------------------------------

int KoiScene::pickLook()
{
    // Mostly red-and-white, orange and black koi; pale yellow and platinum
    // are rarer. True gold is kept for subs.
    const double r = this->rng_.next();
    if (r < 0.30)
    {
        return KOHAKU;
    }
    if (r < 0.52)
    {
        return ORANGE;
    }
    if (r < 0.68)
    {
        return SHOWA;
    }
    if (r < 0.84)
    {
        return SANKE;
    }
    if (r < 0.93)
    {
        return PLATINUM;
    }
    return YAMABUKI;
}

Koi &KoiScene::addKoi(Role role, int look, double length, double x, double y,
                      double heading)
{
    Koi k;
    k.role = role;
    k.look = look;
    k.seed = this->rng_.next();
    k.length = length;
    k.x = x;
    k.y = y;
    k.heading = heading;
    k.rank = this->rng_.next();
    k.phase = this->rng_.range(0, TAU);
    k.tx = x;
    k.ty = y;
    k.start = this->now_;
    this->koi_.push_back(k);
    return this->koi_.back();
}

QPointF KoiScene::wanderPoint()
{
    // Koi prefer the open water behind the Activity pane, along the top and
    // around the input; they cross under chat now and then.
    struct Area {
        QRectF rect;
        double weight;
    };
    std::vector<Area> areas;
    const auto inner = this->interior();
    for (const auto &a : this->layout_.activity)
    {
        areas.push_back({a.intersected(inner), 3.0});
    }
    areas.push_back(
        {QRectF(inner.left(), inner.top(), inner.width(), 80).intersected(inner),
         1.6});
    const auto band = this->layout_.inputBand();
    if (!band.isEmpty())
    {
        areas.push_back({band.adjusted(0, -50, 0, 0).intersected(inner), 1.6});
    }
    for (const auto &c : this->layout_.chats)
    {
        areas.push_back({c.intersected(inner), 0.7});
    }

    double total = 0;
    for (const auto &a : areas)
    {
        if (a.rect.width() > 4 && a.rect.height() > 4)
        {
            total += a.weight;
        }
    }
    double pick = this->rng_.next() * total;
    QRectF chosen = inner;
    for (const auto &a : areas)
    {
        if (a.rect.width() <= 4 || a.rect.height() <= 4)
        {
            continue;
        }
        pick -= a.weight;
        if (pick <= 0)
        {
            chosen = a.rect;
            break;
        }
    }
    return {chosen.left() + this->rng_.next() * chosen.width(),
            chosen.top() + this->rng_.next() * chosen.height()};
}

void KoiScene::startUp(const SceneState &state)
{
    this->started_ = true;
    const auto h = hashString(state.channel);
    this->camX_ = 3000 + (h % 4096);
    this->camY_ = 2000 + ((h >> 12) % 2048);
    this->pattern_ = h % 9973;
    this->nextPattern_ = this->pattern_;
    this->evening_ = state.live ? 0 : 1;
    if (!state.live)
    {
        this->lightLanterns();
    }

    // A few petals and a leaf already drift on the water
    for (int i = 0; i < 4; ++i)
    {
        const auto p = this->wanderPoint();
        this->petals_.push_back({p.x(), p.y(), this->rng_.range(-4, 4),
                                 this->rng_.range(-3, 3), -this->rng_.range(0, 20),
                                 1e9, i == 3});
    }
}

void KoiScene::updateSchoolSize(const SceneState &state)
{
    // A few koi at 50 viewers, a big school of large and young koi at 10k
    const double viewers = state.viewers < 0 ? 300.0 : state.viewers;
    const double f =
        clamp01(std::log10(1 + viewers / 40.0) / std::log10(1 + 10000 / 40.0));
    const double area = clamp01(this->window_.width() *
                                this->window_.height() / (1280.0 * 760.0) *
                                0.7 + 0.3);
    const int big = static_cast<int>(std::lround((3 + 7 * f) * std::max(0.7, area)));
    int young = static_cast<int>(std::lround(12 * f * f * area));
    if (state.reduced)
    {
        young /= 2;
    }
    const int wanted = std::min(24, big + young);

    int count = 0;
    for (const auto &k : this->koi_)
    {
        if (k.role == Role::School && !k.leaving && k.look != GOLDEN)
        {
            count++;
        }
    }

    const double scale = std::clamp(
        std::sqrt(this->window_.width() * this->window_.height() /
                  (1280.0 * 760.0)),
        0.75, 1.25);
    const auto spawn = [&](bool instant) {
        const auto p = this->wanderPoint();
        const bool juvenile = count >= big;
        const double length = juvenile ? this->rng_.range(58, 76) * scale
                                       : this->rng_.range(104, 140) * scale;
        auto &k = this->addKoi(Role::School, this->pickLook(), length, p.x(),
                               p.y(), this->rng_.range(0, TAU));
        k.fade = instant ? 1 : 0;
        k.z = instant ? 0.4 : 0.95;
        k.zTarget = 0.35;
        k.retargetAt = this->now_ + this->rng_.range(0, 4);
        count++;
    };

    if (this->koi_.empty())
    {
        while (count < wanted)
        {
            spawn(true);
        }
        return;
    }
    if (count < wanted && this->now_ > this->nextSpawn_)
    {
        spawn(false);
        this->nextSpawn_ = this->now_ + 0.5;
    }
    else if (count > wanted + 1)
    {
        // The highest ranked school fish sinks away
        Koi *pick = nullptr;
        for (auto &k : this->koi_)
        {
            if (k.role == Role::School && !k.leaving && k.look != GOLDEN &&
                (pick == nullptr || k.rank > pick->rank))
            {
                pick = &k;
            }
        }
        if (pick != nullptr)
        {
            pick->leaving = true;
        }
    }

    // Keep at most three golden koi from subs
    int golds = 0;
    for (auto it = this->koi_.rbegin(); it != this->koi_.rend(); ++it)
    {
        if (it->look == GOLDEN && !it->leaving && ++golds > 3)
        {
            it->leaving = true;
        }
    }
}

void KoiScene::steerSchool(Koi &k, const SceneState &state,
                           double &speedWanted, double &turnRate,
                           double &accel)
{
    const double t = state.t;
    if (t > k.retargetAt || std::hypot(k.tx - k.x, k.ty - k.y) < 34)
    {
        const auto p = this->wanderPoint();
        k.tx = p.x();
        k.ty = p.y();
        k.retargetAt = t + this->rng_.range(6, 14);
    }
    const bool active = k.rank < 0.30 + 0.70 * state.energy || k.glow > 0;
    speedWanted = active ? (22 + 26 * state.energy) * (0.85 + 0.3 * k.seed)
                         : 7;
    k.zTarget = active ? 0.22 + 0.3 * k.seed : 0.62;
    turnRate = 1.3;
    accel = 1.2;
    if (k.leaving)
    {
        k.zTarget = 1;
        speedWanted = 16;
    }
}

void KoiScene::moveDart(Koi &k, const SceneState &state)
{
    const double u = clamp01((state.t - k.start) / k.duration);
    const double e = easeOutCubic(u);
    const auto pos = bezier(k.path, e);
    const auto ahead = bezier(k.path, std::min(1.0, e + 0.02));
    const double heading =
        u < 1 ? std::atan2(ahead.y() - pos.y(), ahead.x() - pos.x())
              : k.heading;
    const double dt = std::max(1e-3, state.dt);
    k.omega = wrapAngle(heading - k.heading) / dt;
    k.speed = std::hypot(pos.x() - k.x, pos.y() - k.y) / dt;
    k.heading = heading;

    // Wake: a shimmering trail and a few glints above the glass
    const double moved = std::hypot(pos.x() - k.x, pos.y() - k.y);
    const int steps = std::max(1, static_cast<int>(moved / 5));
    for (int s = 1; s <= steps; ++s)
    {
        const double f = double(s) / steps;
        const double wx = k.x + (pos.x() - k.x) * f;
        const double wy = k.y + (pos.y() - k.y) * f;
        this->wake_.push_back({wx, wy, state.t});
        if (!state.reduced && this->rng_.next() < 0.55)
        {
            // Loose tesserae shaken up by the wake, shown above the glass
            this->addGlint(wx + this->rng_.range(-9, 9),
                           wy + this->rng_.range(-8, 8), this->rng_.range(-12, 12),
                           this->rng_.range(-20, -4), this->rng_.range(1.0, 1.8),
                           this->rng_.range(3.5, 5.5),
                           static_cast<int>(this->rng_.next() * 3.3));
        }
    }
    k.x = pos.x();
    k.y = pos.y();
    k.z = lerp(0.55, 0.05, smoothstep(0, 0.3, u));
    k.zTarget = k.z;
    k.fade = std::min(1.0, k.fade + state.dt * 4);
    k.glow = 0.25;
    // It leaps clear of the water just before the end
    k.leap = state.reduced ? 0.0 : bump((u - 0.62) / 0.38);

    if (u >= 1)
    {
        // Surfacing: a ring of ripples and a lily that opens where it broke
        // the water
        this->addRipple(k.x, k.y, 1.3, 70, 2.2, 11);
        this->addRipple(k.x, k.y, 0.7, 40, 2.6, 8);
        this->addBloom(k.x - std::cos(k.heading) * 6,
                       k.y - std::sin(k.heading) * 6);
        if (!state.reduced)
        {
            for (int g = 0; g < 20; ++g)
            {
                const double a = TAU * g / 20 + this->rng_.range(-0.2, 0.2);
                const double v = this->rng_.range(30, 60);
                this->addGlint(k.x + std::cos(a) * 10, k.y + std::sin(a) * 7,
                               std::cos(a) * v, std::sin(a) * v * 0.7,
                               this->rng_.range(0.9, 1.5),
                               this->rng_.range(3.0, 5.0), g % 4);
            }
        }
        k.role = Role::School;
        k.leap = 0;
        k.speed = 60;
        k.tx = k.x - 160;
        k.ty = k.y - 30;
        k.retargetAt = state.t + 3;
        k.glow = 0;
        if (k.cruise > 0)
        {
            // A koi that rose from the deep for this message goes back down
            k.leaveAt = state.t + 6;
        }
    }
}

void KoiScene::moveCircle(Koi &k, const SceneState &state)
{
    const double dt = std::max(1e-3, state.dt);
    const double motion = state.reduced ? 0.4 : 1.0;
    k.angle += dt * 0.95 * motion;
    const double nx = k.cx + std::cos(k.angle) * k.rx;
    const double ny = k.cy + std::sin(k.angle) * k.ry;
    const double heading = std::atan2(ny - k.y, nx - k.x);
    k.speed = std::hypot(nx - k.x, ny - k.y) / dt;
    k.omega = wrapAngle(heading - k.heading) / dt;
    k.heading = heading;
    k.x = nx;
    k.y = ny;
    k.z = 0.12;
    k.zTarget = 0.12;
    k.fade = std::min(1.0, k.fade + dt * 1.5);
    if (state.t > k.start + k.duration)
    {
        k.role = Role::School;
        k.leaving = true;
    }
}

void KoiScene::updateKoi(const SceneState &state)
{
    const double t = state.t;
    const double dt = state.dt;
    const double motion = state.reduced ? 0.35 : 1.0;
    const auto &lay = this->layout_;
    const auto inner = this->interior();

    // School fish that gather at the caret while you type
    QPointF gatherHead;
    QPointF gatherFace;
    double gatherSpread = 9;
    this->gatherSpot(state, gatherHead, gatherSpread, gatherFace);
    std::vector<int> gather;
    if (state.typing && !lay.focusInput.isEmpty())
    {
        std::vector<std::pair<double, int>> near;
        for (int i = 0; i < static_cast<int>(this->koi_.size()); ++i)
        {
            const auto &k = this->koi_[i];
            const double d =
                std::hypot(k.x - state.caret.x(), k.y - state.caret.y());
            if (k.role == Role::School && !k.leaving && k.fade > 0.3 && d < 560)
            {
                near.emplace_back(d, i);
            }
        }
        std::sort(near.begin(), near.end());
        const int n = std::min<int>(static_cast<int>(near.size()),
                                    std::min(5, 3 + state.draftLength / 10));
        for (int i = 0; i < n; ++i)
        {
            gather.push_back(near[i].second);
        }
    }

    // The nearest koi grow curious about the pointer
    std::vector<int> curious;
    if (state.pointer && gather.empty())
    {
        std::vector<std::pair<double, int>> near;
        for (int i = 0; i < static_cast<int>(this->koi_.size()); ++i)
        {
            const auto &k = this->koi_[i];
            const double d = std::hypot(k.x - state.pointer->x(),
                                        k.y - state.pointer->y());
            if (k.role == Role::School && !k.leaving && d < 380)
            {
                near.emplace_back(d, i);
            }
        }
        std::sort(near.begin(), near.end());
        for (int i = 0; i < std::min<int>(2, static_cast<int>(near.size())); ++i)
        {
            curious.push_back(near[i].second);
        }
    }

    const double heronU = (t - this->heronStart_) / 2.6;
    const bool heron = heronU > 0 && heronU < 1.15;
    const QPointF heronPos =
        this->heronFrom_ + (this->heronTo_ - this->heronFrom_) * heronU;
    const bool ring = t < this->ringUntil_;
    int ringIndex = 0;
    int ringCount = 0;
    if (ring)
    {
        for (const auto &k : this->koi_)
        {
            ringCount += (k.role == Role::School && !k.leaving) ? 1 : 0;
        }
    }
    const double current = this->raidStrength_ *
                           bump((t - this->raidStart_) / 3.0) * 70 * motion;

    for (int idx = 0; idx < static_cast<int>(this->koi_.size()); ++idx)
    {
        Koi &k = this->koi_[idx];
        if (t > k.leaveAt && !k.leaving)
        {
            k.leaving = true;
        }
        if (k.role == Role::Dart)
        {
            this->moveDart(k, state);
        }
        else if (k.role == Role::Circle)
        {
            this->moveCircle(k, state);
        }
        else
        {
            double speedWanted = 20;
            double turnRate = 1.3;
            double accel = 1.2;
            bool keepInside = true;
            bool freeTarget = false;

            switch (k.role)
            {
                case Role::School: {
                    this->steerSchool(k, state, speedWanted, turnRate, accel);
                    const auto inGather =
                        std::find(gather.begin(), gather.end(), idx);
                    const auto inCurious =
                        std::find(curious.begin(), curious.end(), idx);
                    if (heron && std::hypot(k.x - heronPos.x(),
                                            k.y - heronPos.y()) < 320)
                    {
                        // Scatter from the heron's shadow and dive
                        const double ax = k.x - heronPos.x();
                        const double ay = k.y - heronPos.y();
                        const double d = std::max(1.0, std::hypot(ax, ay));
                        k.tx = k.x + ax / d * 260;
                        k.ty = k.y + ay / d * 260;
                        k.retargetAt = t + 2.5;
                        speedWanted = 150;
                        turnRate = 4.5;
                        accel = 4;
                        k.zTarget = 0.9;
                        freeTarget = true;
                    }
                    else if (ring && !k.leaving)
                    {
                        // Swim a loose ring together
                        const double a = TAU * ringIndex / std::max(1, ringCount) +
                                         (t - this->ringStart_) * 0.45;
                        const double r =
                            this->ringRadius_ * (0.9 + 0.2 * k.seed);
                        k.tx = this->ringCentre_.x() + std::cos(a + 0.5) * r;
                        k.ty = this->ringCentre_.y() + std::sin(a + 0.5) * r;
                        k.retargetAt = t + 1;
                        speedWanted = 70;
                        turnRate = 2.6;
                        accel = 2;
                        k.zTarget = 0.18;
                        ringIndex++;
                    }
                    else if (inGather != gather.end())
                    {
                        // Gather by the caret, heads in, nosing at it
                        const int slot =
                            static_cast<int>(inGather - gather.begin());
                        const int n = static_cast<int>(gather.size());
                        // A shallow fan, heads staggered back from the spot
                        const double off =
                            n > 1 ? (slot - (n - 1) / 2.0) / ((n - 1) / 2.0) : 0;
                        const double sx = gatherHead.x() +
                                          std::abs(off) * 16 + k.length * 0.03;
                        const double sy = gatherHead.y() + off * gatherSpread;
                        const double d = std::hypot(sx - k.x, sy - k.y);
                        k.zTarget = 0.12;
                        if (d < 26)
                        {
                            k.tx = gatherFace.x();
                            k.ty = gatherFace.y() + off * gatherSpread * 0.5;
                            speedWanted =
                                2 + 5 * std::max(0.0, std::sin(t * 3.1 + slot * 1.7));
                            turnRate = 2.4;
                            accel = 3;
                            k.x += (sx - k.x) * std::min(1.0, dt * 1.6);
                            k.y += (sy - k.y) * std::min(1.0, dt * 1.6);
                        }
                        else
                        {
                            // Hurry over, slowing to turn in on arrival
                            k.tx = sx;
                            k.ty = sy;
                            speedWanted = std::clamp(d * 1.3, 14.0, 150.0);
                            turnRate = d < 90 ? 5.0 : 2.8;
                            accel = 2.5;
                        }
                        k.retargetAt = t + 2;
                        freeTarget = true;
                    }
                    else if (inCurious != curious.end() && state.pointer)
                    {
                        // Curious, not frantic: drift up and hover nearby
                        const double px = state.pointer->x();
                        const double py = state.pointer->y();
                        const double d = std::hypot(px - k.x, py - k.y);
                        k.tx = px;
                        k.ty = py;
                        speedWanted = d < 50 ? 3 : std::clamp(d * 0.3, 14.0, 36.0);
                        turnRate = d < 90 ? 2.6 : 1.6;
                        k.zTarget = 0.15;
                        k.retargetAt = t + 2;
                        freeTarget = true;
                    }
                }
                break;
                case Role::Lap: {
                    // Swim a lap: aim a little ahead along the ellipse
                    const double a = std::atan2((k.y - k.cy) / std::max(1.0, k.ry),
                                                (k.x - k.cx) / std::max(1.0, k.rx));
                    k.travelled += wrapAngle(a - k.angle);
                    k.angle = a;
                    k.tx = k.cx + std::cos(a + 0.6) * k.rx;
                    k.ty = k.cy + std::sin(a + 0.6) * k.ry;
                    speedWanted = k.cruise;
                    turnRate = 3.2;
                    accel = 2;
                    k.zTarget = 0.08;
                    if (k.travelled >= TAU)
                    {
                        k.role = Role::School;
                        k.glow = 0.12;
                        k.retargetAt = t;
                    }
                }
                break;
                case Role::Raid: {
                    k.tx = k.x + 300;
                    k.ty = k.cy + std::sin(t * 1.3 + k.seed * TAU) * 30;
                    speedWanted = k.cruise;
                    turnRate = 1.5;
                    accel = 3;
                    keepInside = false;
                    if (k.x > this->window_.width() + k.length * 1.6)
                    {
                        k.dead = true;
                    }
                }
                break;
                case Role::Pair: {
                    // Side by side towards the Activity side
                    const double hx = k.cx - k.x;
                    const double hy = k.cy - k.y;
                    const double d = std::max(1.0, std::hypot(hx, hy));
                    k.tx = k.cx - hy / d * k.rx;
                    k.ty = k.cy + hx / d * k.rx;
                    speedWanted = k.cruise;
                    turnRate = 2.0;
                    accel = 2;
                    k.zTarget = 0.1;
                    keepInside = false;
                    if (d < 70)
                    {
                        k.role = Role::School;
                        k.leaveAt = t + 3;
                        k.retargetAt = t;
                    }
                }
                break;
                case Role::Dart:
                case Role::Circle:
                    break;
            }

            if (k.leaving)
            {
                k.fade -= dt / 2.4;
                k.zTarget = 1;
                if (k.fade <= 0)
                {
                    k.dead = true;
                }
            }
            else
            {
                k.fade = std::min(1.0, k.fade + dt / 1.6);
            }

            // Slow under the chat glass, unless on an errand
            if (!freeTarget)
            {
                speedWanted *= 1 - 0.4 * this->calmAt(k.x, k.y);
            }

            const double desired = std::atan2(k.ty - k.y, k.tx - k.x);
            const double maxTurn = turnRate * dt * (0.5 + 0.5 * motion);
            const double turn =
                std::clamp(wrapAngle(desired - k.heading), -maxTurn, maxTurn);
            k.heading = wrapAngle(k.heading + turn);
            k.omega = dt > 0 ? turn / dt : 0;
            // A raid still crosses in reduced motion, just more gently
            const double pace =
                k.role == Role::Raid ? std::max(motion, 0.6) : motion;
            k.speed += (speedWanted * pace - k.speed) * std::min(1.0, dt * accel);
            k.x += (std::cos(k.heading) * k.speed + k.pushX + current) * dt;
            k.y += (std::sin(k.heading) * k.speed + k.pushY) * dt;
            k.x -= this->slideVelX_ * 0.25 * dt;
            k.y -= this->slideVelY_ * 0.25 * dt;
            const double decay = std::exp(-2.6 * dt);
            k.pushX *= decay;
            k.pushY *= decay;

            if (keepInside && !k.leaving)
            {
                // Turn back from the rim instead of hitting it
                const double m = 18;
                if (!freeTarget &&
                    (k.x < inner.left() + m || k.x > inner.right() - m ||
                     k.y < inner.top() + m || k.y > inner.bottom() - m))
                {
                    k.tx = std::clamp(k.tx, inner.left() + 40, inner.right() - 40);
                    k.ty = std::clamp(k.ty, inner.top() + 30, inner.bottom() - 30);
                }
                k.x = std::clamp(k.x, inner.left() - 14, inner.right() + 14);
                k.y = std::clamp(k.y, inner.top() - 14, inner.bottom() + 14);
            }
        }

        const double bendTarget = std::clamp(
            k.omega * k.length / std::max(k.speed, 14.0), -1.5, 1.5);
        k.bend += (bendTarget - k.bend) * std::min(1.0, dt * 6);
        k.phase += dt * (2.0 + k.speed * 0.10) * motion;
        k.z += (k.zTarget - k.z) * std::min(1.0, dt * 0.9);
        k.glow = std::max(0.0, k.glow - dt * 0.02);
    }

    this->koi_.erase(std::remove_if(this->koi_.begin(), this->koi_.end(),
                                    [](const Koi &k) {
                                        return k.dead;
                                    }),
                     this->koi_.end());
    this->separate(dt);
}

void KoiScene::separate(double dt)
{
    // Koi keep a body's width from each other
    const int n = static_cast<int>(this->koi_.size());
    for (int a = 0; a < n; ++a)
    {
        auto &ka = this->koi_[a];
        if (ka.role == Role::Dart || ka.role == Role::Circle ||
            ka.role == Role::Raid)
        {
            continue;
        }
        const double ax = ka.x - std::cos(ka.heading) * ka.length * 0.35;
        const double ay = ka.y - std::sin(ka.heading) * ka.length * 0.35;
        for (int b = a + 1; b < n; ++b)
        {
            auto &kb = this->koi_[b];
            if (kb.role == Role::Dart || kb.role == Role::Circle ||
                kb.role == Role::Raid)
            {
                continue;
            }
            const double bx = kb.x - std::cos(kb.heading) * kb.length * 0.35;
            const double by = kb.y - std::sin(kb.heading) * kb.length * 0.35;
            const double dx = ax - bx;
            const double dy = ay - by;
            const double d = std::hypot(dx, dy);
            const double want = (ka.length + kb.length) * 0.24;
            if (d < want && d > 0.01)
            {
                const double push = (want - d) / want * 60 * dt;
                ka.x += dx / d * push;
                ka.y += dy / d * push;
                kb.x -= dx / d * push;
                kb.y -= dy / d * push;
            }
        }
    }
}

void KoiScene::addRipple(double x, double y, double strength, double speed,
                         double life, double width)
{
    if (this->reduced_)
    {
        strength *= 0.5;
        speed *= 0.6;
    }
    if (this->ripples_.size() >= 32)
    {
        this->ripples_.erase(this->ripples_.begin());
    }
    this->ripples_.push_back({x, y, this->now_, strength, speed, life, width});
}

void KoiScene::addGlint(double x, double y, double vx, double vy, double life,
                        double size, int hue)
{
    if (this->glints_.size() >= 140)
    {
        return;
    }
    this->glints_.push_back({x, y, vx, vy, this->now_, life, size, hue});
}

void KoiScene::addRing(double x, double y, double life, double r0, double r1,
                       const QColor &color, double alpha)
{
    if (this->rings_.size() >= 8)
    {
        this->rings_.erase(this->rings_.begin());
    }
    this->rings_.push_back({x, y, this->now_, life, r0, r1, color, alpha});
}

void KoiScene::addBloom(double x, double y)
{
    // Keep at most four flowering lilies from your messages
    int blooms = 0;
    for (auto it = this->pads_.rbegin(); it != this->pads_.rend(); ++it)
    {
        if (it->bloomAt >= 0 && ++blooms >= 4)
        {
            it->life = std::min(it->life, this->now_ - it->start);
        }
    }
    const double r = 22 * std::clamp(this->tile_ / 8.0, 0.9, 1.2);
    this->pads_.push_back({x, y, x, y + 10, r, this->rng_.range(0, TAU),
                           this->now_, 32, static_cast<int>(this->rng_.next() * 3) % 3,
                           this->now_ + 0.15});
}

void KoiScene::addChatterPad()
{
    // A new lily pad slides out from under the rim into open water
    const double w = this->window_.width();
    const double h = this->window_.height();
    int count = 0;
    for (auto it = this->pads_.rbegin(); it != this->pads_.rend(); ++it)
    {
        if (it->bloomAt < 0 && ++count >= 8)
        {
            it->life = std::min(it->life, this->now_ - it->start);
        }
    }
    const double r = this->rng_.range(12, 19);
    double x = 0;
    double y = 0;
    double fx = 0;
    double fy = 0;
    const double side = this->rng_.next();
    const QRectF act = this->layout_.activity.empty()
                           ? QRectF(w * 0.6, 0, w * 0.4, h)
                           : this->layout_.activity.front();
    if (side < 0.4)
    {
        // Along the top rim
        x = this->rng_.range(this->rimL_ + 60, w - this->rimR_ - 60);
        y = this->rimT_ + r * 0.9;
        fx = x;
        fy = this->rimT_ - r * 1.6;
    }
    else if (side < 0.75)
    {
        // Along the right rim, behind the Activity pane
        x = w - this->rimR_ - r * 0.9;
        y = this->rng_.range(std::max(act.top(), this->rimT_ + 60),
                             std::min(act.bottom(), h - this->rimB_ - 60));
        fx = w - this->rimR_ + r * 1.6;
        fy = y;
    }
    else
    {
        // Along the bottom rim, by the input
        x = this->rng_.range(this->rimL_ + 60, w - this->rimR_ - 60);
        y = h - this->rimB_ - r * 0.8;
        fx = x;
        fy = h - this->rimB_ + r * 1.6;
    }
    this->pads_.push_back({x, y, fx, fy, r, this->rng_.range(0, TAU), this->now_,
                           45, static_cast<int>(this->rng_.next() * 3) % 3,
                           -1});
}

void KoiScene::lightLanterns()
{
    this->lanterns_.clear();
    const int n = this->reduced_ ? 3 : 6;
    for (int i = 0; i < n; ++i)
    {
        auto p = this->wanderPoint();
        for (int tries = 0; tries < 8 && this->quietAt(p.x(), p.y()) > 0.3;
             ++tries)
        {
            p = this->wanderPoint();
        }
        this->lanterns_.push_back({p.x(), p.y(), this->rng_.range(-5, 5),
                                   this->rng_.range(-3, 3),
                                   this->rng_.range(0, TAU)});
    }
}

void KoiScene::startDart(const SceneEvent &event, const SceneState &state)
{
    const auto &lay = this->layout_;
    const QPointF caret = event.point.isNull() ? state.caret : event.point;
    QRectF row = event.row;
    if (row.isEmpty())
    {
        row = QRectF(lay.focusChat.left(), lay.focusChat.bottom() - 22,
                     lay.focusChat.width(), 22);
    }

    // A koi gathered by the caret takes the message; if none is close one
    // rises from the deep right there.
    QPointF spot;
    QPointF face;
    double spread = 0;
    this->gatherSpot(state, spot, spread, face);
    (void)face;
    (void)spread;
    Koi *dart = nullptr;
    double best = 220;
    for (auto &k : this->koi_)
    {
        const double d = std::min(std::hypot(k.x - caret.x(), k.y - caret.y()),
                                  std::hypot(k.x - spot.x(), k.y - spot.y()));
        if (k.role == Role::School && !k.leaving && d < best)
        {
            best = d;
            dart = &k;
        }
    }
    if (dart == nullptr)
    {
        auto &k = this->addKoi(Role::Dart, KOHAKU, 112, spot.x(), spot.y(),
                               TAU / 2);
        k.fade = 0.2;
        k.z = 0.8;
        k.cruise = 1;
        dart = &k;
    }

    // Up into your message row, along it to its far end, and back down to
    // break the surface in open water by the input, where the lily opens
    const QPointF from(dart->x, dart->y);
    const QPointF end = this->landingPoint();
    const double rowY = row.center().y();
    dart->role = Role::Dart;
    dart->start = state.t;
    dart->path = {from, QPointF(lerp(from.x(), row.left(), 0.5), rowY - 6),
                  QPointF(row.right() - 16, rowY - 30), end};
    double len = 0;
    for (int i = 0; i < 3; ++i)
    {
        const auto d = dart->path[static_cast<size_t>(i) + 1] -
                       dart->path[static_cast<size_t>(i)];
        len += std::hypot(d.x(), d.y());
    }
    dart->duration = std::clamp(0.45 + len / 1400, 0.7, 1.25);
    dart->leaving = false;

    // Everyone else at the caret flinches back from the burst
    for (auto &k : this->koi_)
    {
        if (&k == dart)
        {
            continue;
        }
        const double dx = k.x - caret.x();
        const double dy = k.y - caret.y();
        const double d = std::max(1.0, std::hypot(dx, dy));
        if (d < 200)
        {
            k.pushX += dx / d * 90 * (1 - d / 200);
            k.pushY += dy / d * 90 * (1 - d / 200);
        }
    }
    this->addRipple(caret.x(), caret.y(), 1.1, 120, 1.5, 12);
}

void KoiScene::updateSlide(double t, double dt)
{
    const double u = (t - this->slideStart_) / 1.6;
    if (u < 0 || u > 1.05)
    {
        this->slideVelX_ = 0;
        this->slideVelY_ = 0;
        return;
    }
    const double e = easeInOutSine(u);
    const double nx = lerp(this->slideFromX_, this->slideToX_, e);
    const double ny = lerp(this->slideFromY_, this->slideToY_, e);
    const double step = std::max(1e-3, dt);
    this->slideVelX_ = (nx - this->camX_) / step;
    this->slideVelY_ = (ny - this->camY_) / step;
    this->camX_ = nx;
    this->camY_ = ny;
    if (u >= 0.5)
    {
        this->pattern_ = this->nextPattern_;
    }
    this->baseDirty_ = true;
}

void KoiScene::updateParticles(const SceneState &state)
{
    const double t = state.t;
    const double dt = state.dt;
    const double motion = state.reduced ? 0.35 : 1.0;

    std::erase_if(this->ripples_, [t](const Ripple &r) {
        return t - r.start > r.life;
    });
    std::erase_if(this->wake_, [t](const WakeDot &d) {
        return t - d.start > 2.6;
    });
    if (this->wake_.size() > 260)
    {
        this->wake_.erase(this->wake_.begin(),
                          this->wake_.begin() +
                              static_cast<std::ptrdiff_t>(this->wake_.size() - 260));
    }

    for (auto &f : this->fry_)
    {
        const double age = t - f.start;
        const double wig = std::sin(age * 9 + f.phase) * 18 * motion;
        const double sp = std::max(1.0, std::hypot(f.vx, f.vy));
        f.x += (f.vx + -f.vy / sp * wig) * dt;
        f.y += (f.vy + f.vx / sp * wig) * dt;
        const double drag = std::exp(-0.9 * dt);
        f.vx *= drag;
        f.vy *= drag;
    }
    std::erase_if(this->fry_, [t](const Fry &f) {
        return t - f.start > f.life;
    });

    for (auto it = this->pendingCoins_.begin();
         it != this->pendingCoins_.end();)
    {
        if (t >= it->at)
        {
            this->coins_.push_back({it->x, it->y, t, this->rng_.range(2.4, 3.4),
                                    this->rng_.range(0, TAU),
                                    this->rng_.range(-8, 8)});
            this->addRipple(it->x, it->y, 0.6, 50, 1.0, 6);
            it = this->pendingCoins_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    std::erase_if(this->coins_, [t](const Coin &c) {
        return t - c.start > c.sinkTime;
    });

    std::erase_if(this->pads_, [t](const Pad &p) {
        return t - p.start > p.life + 4;
    });
    std::erase_if(this->glints_, [t](const Glint &g) {
        return t - g.start > g.life;
    });
    for (auto &g : this->glints_)
    {
        g.x += g.vx * dt;
        g.y += g.vy * dt;
    }
    std::erase_if(this->rings_, [t](const Ring &r) {
        return t - r.start > r.life;
    });

    // Lanterns and petals drift slowly on the surface
    const auto inner = this->interior();
    for (auto &l : this->lanterns_)
    {
        const double nx = l.x + (l.vx + this->padShift_ * 0.4) * dt * motion;
        const double ny = l.y + l.vy * dt * motion;
        // Lanterns drift in open water and turn back from under chat text
        if (this->quietAt(nx, ny) > 0.5 && this->quietAt(l.x, l.y) <= 0.5)
        {
            l.vx = -l.vx;
            l.vy = -l.vy;
            continue;
        }
        l.x = nx;
        l.y = ny;
        if (l.x < inner.left() + 20 || l.x > inner.right() - 20)
        {
            l.vx = -l.vx;
        }
        if (l.y < inner.top() + 10 || l.y > inner.bottom() - 10)
        {
            l.vy = -l.vy;
        }
        l.x = std::clamp(l.x, inner.left(), inner.right());
        l.y = std::clamp(l.y, inner.top(), inner.bottom());
    }
    for (auto &p : this->petals_)
    {
        p.x += (p.vx + this->padShift_ * 0.6) * dt * motion;
        p.y += p.vy * dt * motion;
        p.x -= this->slideVelX_ * dt;
        p.y -= this->slideVelY_ * dt;
        if (p.x < inner.left() - 40)
        {
            p.x = inner.right();
        }
        if (p.x > inner.right() + 40)
        {
            p.x = inner.left();
        }
        if (p.y < inner.top() - 20)
        {
            p.y = inner.bottom();
        }
        if (p.y > inner.bottom() + 20)
        {
            p.y = inner.top();
        }
    }
    std::erase_if(this->petals_, [t](const Petal &p) {
        return t - p.start > p.life;
    });

    // The raid's current pushes the pads and settles again
    const double current =
        this->raidStrength_ * bump((t - this->raidStart_) / 3.0) * 34;
    this->padShift_ += (current - this->padShift_) * std::min(1.0, dt * 1.8);

    // The lotus opens one petal after another with the hype level
    const double petals =
        this->hypeLevel_ <= 0
            ? 0.0
            : std::min(16.0, 3.0 + this->hypeLevel_ * 3.0 +
                                 state.hypeProgress * 3.0);
    const double rate = petals > this->lotusPetals_ ? 2.2 : 1.2;
    this->lotusPetals_ +=
        std::clamp(petals - this->lotusPetals_, -rate * dt, rate * dt);

}

void KoiScene::step(const SceneState &state, const SceneLayout &layout)
{
    this->ensureGrid(layout);
    this->layout_ = layout;
    this->now_ = state.t;
    this->reduced_ = state.reduced;
    if (!this->started_)
    {
        this->startUp(state);
    }

    // Chat speed drives how fast the sunlight shimmers
    this->shimmer_ += state.dt * (0.25 + 1.3 * state.energy) *
                      (state.reduced ? 0.12 : 1.0);
    const double eveningTarget = state.live ? 0.0 : 1.0;
    this->evening_ += std::clamp(eveningTarget - this->evening_,
                                 -state.dt / 3.0, state.dt / 3.0);
    if (this->evening_ <= 0 && !this->lanterns_.empty())
    {
        this->lanterns_.clear();
    }
    this->hypeLevel_ = state.hypeLevel;

    this->updateSlide(state.t, state.dt);
    this->updateSchoolSize(state);
    this->updateKoi(state);
    this->updateParticles(state);
}

void KoiScene::event(const SceneEvent &event, const SceneState &state,
                     const SceneLayout &layout)
{
    this->ensureGrid(layout);
    this->layout_ = layout;
    this->now_ = state.t;
    this->reduced_ = state.reduced;
    if (!this->started_)
    {
        this->startUp(state);
    }
    const double w = this->window_.width();
    const double h = this->window_.height();
    const auto inner = this->interior();
    const QRectF act = layout.activity.empty() ? QRectF()
                                               : layout.activity.front();
    const double scale = std::clamp(
        std::sqrt(w * h / (1280.0 * 760.0)), 0.75, 1.25);

    switch (event.type)
    {
        case SceneEventType::Key: {
            // A tiny ripple from the caret; the nearest koi nips at it
            this->addRipple(event.point.x(), event.point.y(),
                            event.deleted ? 0.35 : 0.55, 60, 0.9, 6);
            for (auto &k : this->koi_)
            {
                const double dx = event.point.x() - k.x;
                const double dy = event.point.y() - k.y;
                const double d = std::hypot(dx, dy);
                if (k.role == Role::School && d < 60 && d > 1)
                {
                    const double nip = event.deleted ? -40 : 40;
                    k.pushX += dx / d * nip;
                    k.pushY += dy / d * nip;
                    break;
                }
            }
        }
        break;
        case SceneEventType::Send:
            this->startDart(event, state);
            break;
        case SceneEventType::Message: {
            if (event.firstTime)
            {
                this->addChatterPad();
            }
            else if (state.t - this->lastPlip_ > 0.35 && !act.isEmpty())
            {
                // A koi rising somewhere behind the Activity pane
                this->lastPlip_ = state.t;
                const auto a = act.intersected(inner);
                this->addRipple(a.left() + this->rng_.next() * a.width(),
                                a.top() + this->rng_.next() * a.height(), 0.35,
                                40, 1.2, 6);
            }
        }
        break;
        case SceneEventType::Sub: {
            // A golden koi joins and swims a lap around the Activity pane
            QRectF lap = act.isEmpty()
                             ? QRectF(w * 0.2, this->rimT_ + 30, w * 0.6, 160)
                             : act.adjusted(30, 40, -30, -40);
            lap = lap.intersected(inner.adjusted(20, 20, -20, -20));
            const double months = std::clamp(event.amount, 1, 48);
            auto &k = this->addKoi(Role::Lap, GOLDEN,
                                   (118 + months * 0.8) * scale, lap.left(),
                                   lap.center().y(), -TAU / 4);
            k.cx = lap.center().x();
            k.cy = lap.center().y();
            k.rx = std::max(40.0, lap.width() / 2);
            k.ry = std::max(40.0, lap.height() / 2);
            k.angle = TAU / 2;
            k.cruise = std::clamp(
                TAU * std::sqrt((k.rx * k.rx + k.ry * k.ry) / 2) / 5.5, 90.0,
                320.0);
            k.speed = k.cruise * 0.6;
            k.fade = 0;
            k.z = 0.7;
            k.glow = 0.4;
            this->addRipple(k.x, k.y, 0.9, 80, 1.6, 10);
        }
        break;
        case SceneEventType::Gift: {
            // A cloud of fry that bursts out and disperses
            const QPointF origin = act.isEmpty()
                                       ? layout.inputBand().center()
                                       : QPointF(act.center().x(),
                                                 act.bottom() - 70);
            int count = std::clamp(event.amount * 5, 10, 150);
            if (state.reduced)
            {
                count = count / 3 + 4;
            }
            const double power = std::clamp(std::sqrt(event.amount) * 22, 40.0, 170.0);
            static const std::array<Rgb, 4> FRY{rgb(240, 128, 48), rgb(226, 82, 44),
                                                rgb(236, 180, 64), rgb(44, 44, 48)};
            for (int i = 0; i < count; ++i)
            {
                const double a = this->rng_.range(0, TAU);
                const double v = power * this->rng_.range(0.3, 1.0);
                this->fry_.push_back(
                    {origin.x() + this->rng_.range(-10, 10),
                     origin.y() + this->rng_.range(-10, 10), std::cos(a) * v,
                     std::sin(a) * v, state.t, this->rng_.range(2.4, 3.6),
                     this->rng_.range(0, TAU),
                     FRY[static_cast<size_t>(i) % FRY.size()]});
            }
            if (this->fry_.size() > 220)
            {
                this->fry_.erase(this->fry_.begin(),
                                 this->fry_.begin() +
                                     static_cast<std::ptrdiff_t>(this->fry_.size() - 220));
            }
            this->addRipple(origin.x(), origin.y(), 0.8 + std::min(1.0, event.amount / 30.0),
                            110, 1.8, 12);
        }
        break;
        case SceneEventType::Bits: {
            // Coins dropped in one by one, sinking and darkening
            const int count = std::clamp(3 + event.amount / 60, 3, 36);
            const QRectF area = (act.isEmpty() ? QRectF(w * 0.55, this->rimT_ + 20,
                                                        w * 0.4, h * 0.4)
                                               : act.adjusted(20, 20, -20, -act.height() * 0.35))
                                    .intersected(inner);
            for (int i = 0; i < count; ++i)
            {
                this->pendingCoins_.push_back(
                    {state.t + i * std::min(0.12, 1.6 / count),
                     area.left() + this->rng_.next() * area.width(),
                     area.top() + this->rng_.next() * area.height()});
            }
        }
        break;
        case SceneEventType::Raid: {
            // A big school sweeps across the pond in a wave
            const double size = clamp01(std::log10(1 + event.amount / 10.0) / 3.0);
            int count = static_cast<int>(std::lround(10 + 34 * size));
            if (state.reduced)
            {
                count /= 2;
            }
            this->raidStart_ = state.t;
            this->raidStrength_ = 0.4 + 0.6 * size;
            this->raidSpeed_ = (w + 360) / 2.8;
            for (int i = 0; i < count; ++i)
            {
                const double y = inner.top() + (i + this->rng_.next()) /
                                                   count * inner.height();
                const double x = -60 - 110 * std::sin((y / h) * TAU / 2) -
                                 this->rng_.range(0, 90);
                auto &k = this->addKoi(Role::Raid, this->pickLook(),
                                       this->rng_.range(54, 80) * scale, x, y,
                                       this->rng_.range(-0.15, 0.15));
                k.cy = y;
                k.cruise = this->raidSpeed_ * this->rng_.range(0.92, 1.08);
                k.speed = k.cruise;
                k.fade = 1;
                k.z = this->rng_.range(0.15, 0.55);
                k.zTarget = k.z;
            }
            for (auto &k : this->koi_)
            {
                if (k.role == Role::School)
                {
                    k.pushX += 50 * this->raidStrength_;
                }
            }
        }
        break;
        case SceneEventType::Follow: {
            // A leaf drops onto the water
            const auto p = this->wanderPoint();
            this->petals_.push_back({p.x(), p.y(), this->rng_.range(-5, 5),
                                     this->rng_.range(-3, 3), state.t, 30, true});
            this->addRipple(p.x(), p.y(), 0.6, 50, 1.4, 7);
        }
        break;
        case SceneEventType::Mention: {
            // A tancho koi circles slowly at the end of the row
            const QRectF row = event.row.isEmpty()
                                   ? QRectF(layout.focusChat.left(),
                                            layout.focusChat.bottom() - 22,
                                            layout.focusChat.width(), 22)
                                   : event.row;
            const double cx = row.right() - 70;
            const double cy = row.center().y() - 6;
            auto &k = this->addKoi(Role::Circle, TANCHO, 92 * scale, cx + 30, cy,
                                   TAU / 4);
            k.cx = cx;
            k.cy = cy;
            k.rx = 30;
            k.ry = 22;
            k.angle = 0;
            k.duration = 9;
            k.fade = 0;
            k.glow = 0.3;
            this->addRing(row.left() + 3, row.center().y(), 2.4, 2, 12,
                          QColor(180, 67, 43), 0.6);
            this->addRing(cx, cy, 2.6, 10, 60, QColor(180, 67, 43), 0.35);
        }
        break;
        case SceneEventType::Combo: {
            this->ringStart_ = state.t;
            this->ringUntil_ = state.t + 4.2;
            this->ringCentre_ = act.isEmpty() ? inner.center() : act.center();
            const double room = act.isEmpty()
                                    ? std::min(inner.width(), inner.height()) / 2
                                    : std::min(act.width(), act.height()) / 2;
            this->ringRadius_ = std::clamp(120.0 + event.amount * 8.0, 120.0,
                                           std::max(120.0, room - 30));
        }
        break;
        case SceneEventType::Hype: {
            if (event.levelUp)
            {
                const double lx = act.isEmpty() ? w - 90 : act.right() - 74;
                this->addRipple(lx, this->rimT_ + 36, 0.9, 70, 1.6, 9);
            }
        }
        break;
        case SceneEventType::Shoutout: {
            // A pair of koi swims together to the Activity side
            const double y = layout.headers.empty()
                                 ? inner.top() + 30
                                 : layout.headers.front().center().y() + 6;
            const QPointF dest = act.isEmpty()
                                     ? QPointF(w * 0.85, h * 0.45)
                                     : QPointF(act.center().x(),
                                               act.top() + act.height() * 0.3);
            for (int i = 0; i < 2; ++i)
            {
                auto &k = this->addKoi(Role::Pair, i == 0 ? KOHAKU : SHOWA,
                                       108 * scale, -40 - i * 18,
                                       y + (i == 0 ? -12 : 14), 0);
                k.cx = dest.x();
                k.cy = dest.y();
                k.rx = i == 0 ? -18 : 18;
                k.cruise = 230;
                k.speed = 200;
                k.fade = 1;
                k.z = 0.1;
                k.phase = i * 0.8;
            }
        }
        break;
        case SceneEventType::Timeout: {
            // A heron passes over: its shadow crosses the pond
            this->heronStart_ = state.t;
            this->heronScale_ = std::clamp(w / 1280.0, 0.7, 1.4);
            this->heronFrom_ = QPointF(w + 260, h * 0.12);
            this->heronTo_ = QPointF(-260, h * 0.92);
        }
        break;
        case SceneEventType::Live:
            this->morningStart_ = state.t;
            break;
        case SceneEventType::Offline:
            this->lightLanterns();
            break;
        case SceneEventType::Tab: {
            const auto hsh = hashString(event.text);
            this->slideStart_ = state.t;
            this->slideFromX_ = this->camX_;
            this->slideFromY_ = this->camY_;
            this->slideToX_ = 3000 + (hsh % 4096);
            this->slideToY_ = 2000 + ((hsh >> 12) % 2048);
            if (std::abs(this->slideToX_ - this->slideFromX_) < 400)
            {
                this->slideToX_ += 900;
            }
            this->nextPattern_ = hsh % 9973;
        }
        break;
        case SceneEventType::Hover: {
            this->addRipple(event.row.left() + 12, event.row.center().y(), 0.5,
                            34, 1.6, 7);
            this->addRing(event.row.left() + 3, event.row.center().y(), 1.3, 2,
                          11, QColor(31, 45, 43), 0.32);
        }
        break;
        case SceneEventType::Unhover:
            break;
        case SceneEventType::Click: {
            // A dropped ring ripple; the koi flinch away from it
            const auto p = event.point;
            this->addRipple(p.x(), p.y(), 1.2, 110, 2.4, 12);
            this->addRipple(p.x(), p.y(), 0.6, 70, 2.8, 8);
            for (auto &k : this->koi_)
            {
                const double dx = k.x - p.x();
                const double dy = k.y - p.y();
                const double d = std::max(1.0, std::hypot(dx, dy));
                if (d < 240 && (k.role == Role::School || k.role == Role::Lap))
                {
                    const double f = 1 - d / 240;
                    k.pushX += dx / d * 170 * f;
                    k.pushY += dy / d * 170 * f;
                    k.heading = wrapAngle(
                        k.heading +
                        wrapAngle(std::atan2(dy, dx) - k.heading) * 0.5 * f);
                    k.tx = k.x + dx / d * 200;
                    k.ty = k.y + dy / d * 200;
                    k.retargetAt = state.t + 2;
                }
            }
        }
        break;
    }
}

// Rendering into the colour field --------------------------------------------------

void KoiScene::blendTile(int idx, const Rgb &c, double a)
{
    if (a <= 0)
    {
        return;
    }
    this->field_[idx] = mixRgb(this->field_[idx], c, std::min(1.0, a));
}

void KoiScene::shadeWater()
{
    // Sunlight caustics: bright threads where three slow waves cancel out
    const double ct = this->shimmer_;
    const double sun = 1 - 0.85 * this->evening_;
    for (int j = 0; j < this->rows_; ++j)
    {
        const double y = this->tileY(j);
        const double wy = y + this->camY_;
        for (int i = 0; i < this->cols_; ++i)
        {
            const int idx = j * this->cols_ + i;
            Rgb c = this->base_[idx];
            const double water = this->water_[idx];
            if (water > 0)
            {
                const double wx = this->tileX(i) + this->camX_;
                const double a = wx * 0.043 + 1.25 * std::sin(wy * 0.021 + ct * 0.9) +
                                 ct * 1.1;
                const double b = wy * 0.049 + 1.1 * std::sin(wx * 0.026 - ct * 0.8) -
                                 ct * 0.9;
                const double e = (wx - wy) * 0.031 + ct * 0.7;
                const double v =
                    std::abs(std::sin(a) + std::sin(b) + std::sin(e)) / 3;
                const double u = 1 - v;
                const double u2 = u * u;
                const double caustic = u2 * u2 * u2 * u2;
                const double amount = caustic *
                                      (0.17 - 0.07 * this->depth_[idx]) *
                                      (1 - 0.8 * this->quiet_[idx]) * sun * water;
                c = addRgb(c, SUN, amount);
            }
            this->field_[idx] = c;
        }
    }
}

void KoiScene::applyRipples()
{
    const double T = this->tile_;
    for (const auto &r : this->ripples_)
    {
        const double age = this->now_ - r.start;
        if (age < 0 || age > r.life)
        {
            continue;
        }
        const double front = age * r.speed;
        const double fade = 1 - age / r.life;
        const double amp = r.strength * fade * fade;
        const double reach = front + r.width * 2.2;
        const int i0 = std::max(0, static_cast<int>((r.x - reach - this->ox_) / T));
        const int i1 = std::min(this->cols_ - 1,
                                static_cast<int>((r.x + reach - this->ox_) / T));
        const int j0 = std::max(0, static_cast<int>((r.y - reach - this->oy_) / T));
        const int j1 = std::min(this->rows_ - 1,
                                static_cast<int>((r.y + reach - this->oy_) / T));
        for (int j = j0; j <= j1; ++j)
        {
            const double dy = this->tileY(j) - r.y;
            for (int i = i0; i <= i1; ++i)
            {
                const int idx = j * this->cols_ + i;
                const double water = this->water_[idx];
                if (water <= 0)
                {
                    continue;
                }
                const double dist = std::hypot(this->tileX(i) - r.x, dy);
                const double k = (dist - front) / r.width;
                if (k < -2.6 || k > 1.6)
                {
                    continue;
                }
                // A bright crest with a darker trough behind it
                const double wave = -std::sin(k * 2.4) * std::exp(-k * k * 0.7);
                const double amount =
                    wave * amp * 0.24 * (1 - 0.6 * this->calm_[idx]) * water;
                if (amount > 0)
                {
                    this->field_[idx] = addRgb(this->field_[idx], SUN, amount);
                }
                else
                {
                    this->field_[idx] = mulRgb(this->field_[idx], 1 + amount * 0.9);
                }
            }
        }
    }
}

void KoiScene::applyWake()
{
    // The dart's wake: tiles along the path twinkle and settle
    const double T = this->tile_;
    for (const auto &d : this->wake_)
    {
        const double age = this->now_ - d.start;
        const double fade = std::pow(clamp01(1 - age / 2.6), 1.5);
        if (fade <= 0)
        {
            continue;
        }
        const double reach = T * 1.6;
        const int i0 = std::max(0, static_cast<int>((d.x - reach - this->ox_) / T));
        const int i1 = std::min(this->cols_ - 1,
                                static_cast<int>((d.x + reach - this->ox_) / T));
        const int j0 = std::max(0, static_cast<int>((d.y - reach - this->oy_) / T));
        const int j1 = std::min(this->rows_ - 1,
                                static_cast<int>((d.y + reach - this->oy_) / T));
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = i0; i <= i1; ++i)
            {
                const int idx = j * this->cols_ + i;
                const double dist =
                    std::hypot(this->tileX(i) - d.x, this->tileY(j) - d.y);
                if (dist > reach)
                {
                    continue;
                }
                const double twinkle =
                    0.5 + 0.5 * std::sin(this->now_ * 7 + hash01(i, j, 41) * TAU);
                const double amount = fade * (1 - dist / reach) *
                                      (0.16 + 0.30 * twinkle) *
                                      this->water_[idx];
                this->field_[idx] = addRgb(this->field_[idx], SUN, amount);
            }
        }
    }
}

void KoiScene::drawCoins()
{
    const double T = this->tile_;
    for (const auto &c : this->coins_)
    {
        const double age = this->now_ - c.start;
        const double z = clamp01(age / c.sinkTime);
        const double x = c.x + std::sin(c.phase + age * 3.2) * 5 * (1 - z) +
                         c.drift * age;
        const double y = c.y + age * 4;
        const double r = 7.5 * (1 - 0.5 * z) * std::clamp(T / 8.0, 0.9, 1.3);
        // A coin seen flipping catches the light on and off
        const double flip = std::abs(std::cos(c.phase + age * 4.5));
        Rgb col = mulRgb(GOLD, 0.65 + 0.45 * flip);
        col = mixRgb(col, DEEP_TINT, 0.8 * z);
        const double alpha = 1 - z * z;
        const int i0 = std::max(0, static_cast<int>((x - r - T - this->ox_) / T));
        const int i1 = std::min(this->cols_ - 1,
                                static_cast<int>((x + r + T - this->ox_) / T));
        const int j0 = std::max(0, static_cast<int>((y - r - T - this->oy_) / T));
        const int j1 = std::min(this->rows_ - 1,
                                static_cast<int>((y + r + T - this->oy_) / T));
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = i0; i <= i1; ++i)
            {
                const int idx = j * this->cols_ + i;
                const double dist =
                    std::hypot(this->tileX(i) - x, this->tileY(j) - y);
                const double cov = clamp01((r - dist) / T + 0.5);
                if (cov > 0)
                {
                    Rgb tile = col;
                    if (z < 0.25 && flip > 0.8)
                    {
                        tile = addRgb(tile, SUN, 0.3 * (1 - z * 4));
                    }
                    this->blendTile(idx, tile, cov * alpha * this->water_[idx]);
                }
            }
        }
    }
}

Spine KoiScene::spineOf(const Koi &k) const
{
    Spine sp;
    sp.x[0] = k.x;
    sp.y[0] = k.y;
    const double effort =
        clamp01(k.speed / 70.0 + std::abs(k.omega) * 0.15);
    const double amp = this->reduced_ ? 0.06 : 0.10 + 0.20 * effort;
    for (int i = 0; i < 9; ++i)
    {
        const double s = i / 8.0;
        const double theta = k.heading - k.bend * s +
                             amp * std::sin(k.phase - s * 4.6) * (0.15 + s);
        const double full = k.length * (1 + 0.22 * k.leap);
        const double len = i < 8 ? full / 8.0 : full * 0.26;
        sp.x[i + 1] = sp.x[i] - std::cos(theta) * len;
        sp.y[i + 1] = sp.y[i] - std::sin(theta) * len;
    }
    return sp;
}

Rgb KoiScene::koiColour(const Koi &k, double s, double dn, int i, int j) const
{
    const auto &look = LOOKS[static_cast<size_t>(k.look)];
    Rgb c = look.base;
    if (k.look == TANCHO)
    {
        // The single red crown on the head
        const double crown = std::hypot((s - 0.13) / 0.075, dn / 0.8);
        if (crown < 1)
        {
            c = look.patch;
        }
    }
    else if (look.patchAt > 0)
    {
        const double n =
            valueNoise(s * 3.3 + k.seed * 17.0, dn * 0.9 + k.seed * 7.0);
        c = mixRgb(c, look.patch,
                   smoothstep(look.patchAt - 0.04, look.patchAt + 0.04, n));
    }
    if (look.spotAt > 0)
    {
        const double n = valueNoise(s * 6.5 + k.seed * 5.0 + 40,
                                    dn * 1.6 + k.seed * 3.0);
        c = mixRgb(c, look.spot, smoothstep(look.spotAt - 0.03, look.spotAt + 0.03, n));
    }
    if (look.metallic || k.glow > 0)
    {
        // Metallic scales glitter tile by tile
        const double tw = 0.5 + 0.5 * std::sin(this->now_ * 3.0 +
                                               hash01(i, j, 61) * TAU);
        c = addRgb(c, SUN, (look.metallic ? 0.12 : 0) * tw * tw + k.glow * 0.25);
    }
    // A rounded back: brighter along the spine, darker on the flanks
    return mulRgb(c, 1.04 - 0.30 * dn * dn);
}

void KoiScene::drawKoi(const Koi &k)
{
    if (k.fade <= 0.01)
    {
        return;
    }
    const double T = this->tile_;
    const double L = k.length * (1 + 0.22 * k.leap);
    const Spine sp = this->spineOf(k);
    const auto &look = LOOKS[static_cast<size_t>(k.look)];

    // The shadow falls on the floor below and to the right
    const double drop = 5 + 9 * (1 - k.z) + 18 * k.leap;
    const double sox = drop * 0.55;
    const double soy = drop * 0.85;

    double minX = 1e9;
    double maxX = -1e9;
    double minY = 1e9;
    double maxY = -1e9;
    for (int n = 0; n < 10; ++n)
    {
        minX = std::min(minX, sp.x[n]);
        maxX = std::max(maxX, sp.x[n]);
        minY = std::min(minY, sp.y[n]);
        maxY = std::max(maxY, sp.y[n]);
    }
    const double pad = L * 0.32 + T;
    const int i0 = std::max(0, static_cast<int>((minX - pad - this->ox_) / T));
    const int i1 = std::min(this->cols_ - 1,
                            static_cast<int>((maxX + pad + sox - this->ox_) / T));
    const int j0 = std::max(0, static_cast<int>((minY - pad - this->oy_) / T));
    const int j1 = std::min(this->rows_ - 1,
                            static_cast<int>((maxY + pad + soy - this->oy_) / T));
    if (i0 > i1 || j0 > j1)
    {
        return;
    }

    const double alphaBase = k.fade * (0.97 - 0.55 * k.z);
    const double flap = 0.75 + 0.25 * std::sin(k.phase * 0.5);
    const Rgb fin = mixRgb(look.fin, DEEP_TINT, 0.3 * k.z);

    for (int j = j0; j <= j1; ++j)
    {
        const double y = this->tileY(j);
        for (int i = i0; i <= i1; ++i)
        {
            const int idx = j * this->cols_ + i;
            const double water = this->water_[idx];
            if (water <= 0)
            {
                continue;
            }
            const double x = this->tileX(i);
            const double calm = this->calm_[idx];

            double s = 0;
            double d = 0;
            nearestOnSpine(sp, x - sox, y - soy, s, d);
            if (s <= 1.0)
            {
                const double half = bodyHalf(s) * L;
                const double cover = clamp01((half + 3 - std::abs(d)) / (T * 1.3));
                if (cover > 0)
                {
                    this->field_[idx] = mulRgb(
                        this->field_[idx],
                        1 - 0.26 * cover * k.fade * (1 - 0.5 * k.z) *
                                (1 - 0.5 * calm));
                }
            }

            nearestOnSpine(sp, x, y, s, d);
            const double alpha = alphaBase * water * (1 - 0.3 * calm);
            if (s <= 1.0)
            {
                const double half = bodyHalf(s) * L;
                const double cover = clamp01((half - std::abs(d)) / T + 0.5);
                double finCover = 0;
                if (s > 0.17 && s < 0.40)
                {
                    const double reach =
                        half + 0.15 * L * std::sin(TAU / 2 * (s - 0.17) / 0.23) * flap;
                    finCover = clamp01((reach - std::abs(d)) / T + 0.5) * (1 - cover);
                }
                if (finCover > 0)
                {
                    this->blendTile(idx, fin, finCover * alpha * 0.72);
                }
                if (cover > 0)
                {
                    Rgb c = this->koiColour(k, s, d / std::max(1.0, half), i, j);
                    c = mixRgb(c, DEEP_TINT, 0.42 * k.z);
                    // Mosaicists outline a figure with a row of dark tiles
                    const double edge =
                        clamp01((std::abs(d) - half + T * 0.7) / (T * 0.7));
                    c = mixRgb(c, CONTOUR,
                               0.40 * edge * clamp01((half - T) / T) *
                                   (1 - 0.6 * k.z));
                    this->blendTile(idx, c, std::min(1.0, cover * 1.25) * alpha);
                }
            }
            else
            {
                // A forked tail fin
                const double q = (s - 1) / 0.30;
                const double spread = L * (0.035 + 0.13 * q);
                const double notch = q > 0.6 ? L * 0.05 * (q - 0.6) / 0.4 : 0;
                double cover = clamp01((spread - std::abs(d)) / T + 0.5);
                if (std::abs(d) < notch)
                {
                    cover *= 0.6;
                }
                // The tail takes the body colour at its root and thins out
                const Rgb tail = mixRgb(this->koiColour(k, 0.97, 0, i, j), fin,
                                        smoothstep(0, 0.7, q));
                this->blendTile(idx, mixRgb(tail, DEEP_TINT, 0.42 * k.z),
                                cover * alpha * (0.85 - 0.35 * q));
            }
        }
    }
}

void KoiScene::drawAllKoi()
{
    // Deep fish first so shallow fish and their shadows lie over them
    std::vector<const Koi *> order;
    order.reserve(this->koi_.size());
    for (const auto &k : this->koi_)
    {
        order.push_back(&k);
    }
    std::sort(order.begin(), order.end(), [](const Koi *a, const Koi *b) {
        return a->z > b->z;
    });
    for (const auto *k : order)
    {
        this->drawKoi(*k);
    }
}

void KoiScene::drawFry()
{
    const double T = this->tile_;
    for (const auto &f : this->fry_)
    {
        const double age = this->now_ - f.start;
        const double alpha =
            smoothstep(0, 0.2, age) * (1 - smoothstep(f.life - 1.2, f.life, age));
        // Spread each fry over the four nearest tiles so it moves smoothly
        const double fx = (f.x - this->ox_) / T - 0.5;
        const double fy = (f.y - this->oy_) / T - 0.5;
        const int i = static_cast<int>(std::floor(fx));
        const int j = static_cast<int>(std::floor(fy));
        const double ax = fx - i;
        const double ay = fy - j;
        const std::array<double, 4> weights{(1 - ax) * (1 - ay), ax * (1 - ay),
                                            (1 - ax) * ay, ax * ay};
        for (int n = 0; n < 4; ++n)
        {
            const int ti = i + (n % 2);
            const int tj = j + (n / 2);
            if (ti < 0 || tj < 0 || ti >= this->cols_ || tj >= this->rows_)
            {
                continue;
            }
            const int idx = tj * this->cols_ + ti;
            this->blendTile(idx, f.color,
                            weights[static_cast<size_t>(n)] * alpha * 0.95 *
                                this->water_[idx]);
        }
    }
}

void KoiScene::drawPad(double x, double y, double r, double rot, double alpha,
                       int look)
{
    const double T = this->tile_;
    const double reach = r + T + 6;
    const int i0 = std::max(0, static_cast<int>((x - reach - this->ox_) / T));
    const int i1 = std::min(this->cols_ - 1,
                            static_cast<int>((x + reach - this->ox_) / T));
    const int j0 = std::max(0, static_cast<int>((y - reach - this->oy_) / T));
    const int j1 = std::min(this->rows_ - 1,
                            static_cast<int>((y + reach - this->oy_) / T));
    const Rgb green = PAD[static_cast<size_t>(look % 3)];
    for (int j = j0; j <= j1; ++j)
    {
        const double dy = this->tileY(j) - y;
        for (int i = i0; i <= i1; ++i)
        {
            const int idx = j * this->cols_ + i;
            const double water = this->water_[idx];
            if (water <= 0)
            {
                continue;
            }
            const double dx = this->tileX(i) - x;
            const double shadow = std::hypot(dx - 4, dy - 5);
            if (shadow < r + T)
            {
                this->field_[idx] = mulRgb(
                    this->field_[idx],
                    1 - 0.16 * alpha * clamp01((r - shadow) / T + 0.5));
            }
            const double dist = std::hypot(dx, dy);
            double cover = clamp01((r - dist) / T + 0.5);
            if (cover <= 0)
            {
                continue;
            }
            const double ang = std::atan2(dy, dx);
            const double off = std::abs(wrapAngle(ang - rot));
            if (dist > r * 0.15)
            {
                // The notch cut into every lily pad
                cover *= smoothstep(0.14, 0.34, off);
            }
            Rgb c = mulRgb(green, 0.93 + 0.07 * std::cos((ang - rot) * 9));
            if (dist > r - T * 0.9)
            {
                c = mulRgb(c, 1.10);
            }
            this->blendTile(idx, c, cover * alpha * water);
        }
    }
}

void KoiScene::drawFlower(double x, double y, double r, double open,
                          double rot, double alpha, double petals, bool lotus,
                          bool vivid)
{
    const double T = this->tile_;
    const double reach = r + T;
    const int n = lotus ? 16 : 10;
    const double slotW = TAU / n;
    const Rgb pink = (lotus || vivid) ? LOTUS_PINK : PETAL_PINK;
    const int i0 = std::max(0, static_cast<int>((x - reach - this->ox_) / T));
    const int i1 = std::min(this->cols_ - 1,
                            static_cast<int>((x + reach - this->ox_) / T));
    const int j0 = std::max(0, static_cast<int>((y - reach - this->oy_) / T));
    const int j1 = std::min(this->rows_ - 1,
                            static_cast<int>((y + reach - this->oy_) / T));
    for (int j = j0; j <= j1; ++j)
    {
        const double dy = this->tileY(j) - y;
        for (int i = i0; i <= i1; ++i)
        {
            const int idx = j * this->cols_ + i;
            const double dx = this->tileX(i) - x;
            const double dist = std::hypot(dx, dy);
            if (dist > reach)
            {
                continue;
            }
            const double rel = wrapAngle(std::atan2(dy, dx) - rot) + TAU / 2;
            const int slot = std::min(n - 1, static_cast<int>(rel / slotW));
            const double off = (rel - (slot + 0.5) * slotW) / (slotW / 2);
            // Lotus petals open in a scattered order all around the flower
            const double petalOpen =
                lotus ? clamp01(petals - ((slot * 5) % n)) : 1.0;
            const double len = r * open * easeOutCubic(petalOpen) *
                               (1 - 0.5 * off * off);
            const double rel2 = std::fmod(rel + slotW / 2, TAU);
            const double off2 =
                (rel2 - (std::floor(rel2 / slotW) + 0.5) * slotW) / (slotW / 2);
            const double inner = r * 0.62 * std::max(open * 0.6, std::min(1.0, open)) *
                                 (lotus ? clamp01(petals / 6) : 1.0) *
                                 (1 - 0.5 * off2 * off2);
            const double outerCover = clamp01((len - dist) / T + 0.5);
            const double innerCover = clamp01((inner - dist) / T + 0.5);
            const double heart = r * 0.22 * std::max(0.5, open);
            if (dist < heart)
            {
                this->blendTile(idx, POLLEN, alpha * clamp01((heart - dist) / T + 0.8));
            }
            else if (innerCover > 0)
            {
                this->blendTile(idx, mixRgb(PETAL_WHITE, pink, 0.25),
                                innerCover * alpha);
            }
            else if (outerCover > 0)
            {
                this->blendTile(
                    idx, mixRgb(PETAL_WHITE, pink, smoothstep(0.25, 1.0, dist / r)),
                    outerCover * alpha);
            }
        }
    }
}

void KoiScene::drawPads()
{
    const double t = this->now_;
    const double w = this->window_.width();
    const double h = this->window_.height();
    const double size = std::clamp(this->tile_ / 8.0, 0.9, 1.25);

    // Resident lilies live in world cells near the rim, so a tab switch
    // slides new ones into view
    const double cell = 150;
    const auto seed = static_cast<int>(this->pattern_ & 0xffff);
    const int cx0 = static_cast<int>(std::floor((this->camX_ - cell) / cell));
    const int cx1 = static_cast<int>(std::floor((this->camX_ + w + cell) / cell));
    const int cy0 = static_cast<int>(std::floor((this->camY_ - cell) / cell));
    const int cy1 = static_cast<int>(std::floor((this->camY_ + h + cell) / cell));
    for (int cy = cy0; cy <= cy1; ++cy)
    {
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const double hsh = hash01(cx, cy, seed + 71);
            if (hsh > 0.66)
            {
                continue;
            }
            const int count = hsh < 0.22 ? 2 : 1;
            for (int n = 0; n < count; ++n)
            {
                const double r = (14 + 14 * hash01(cx, cy, seed + 73 + n)) * size;
                double x = cx * cell + hash01(cx, cy, seed + 75 + n) * cell -
                           this->camX_ + n * r * 1.7;
                double y = cy * cell + hash01(cx, cy, seed + 77 + n) * cell -
                           this->camY_ + n * r * 0.6;
                const double rd = this->rimDistance(x, y);
                if (rd < -r * 0.4)
                {
                    continue;
                }
                const double alpha = (1 - smoothstep(60, 190, rd)) *
                                     (1 - 0.92 * this->quietAt(x, y));
                if (alpha < 0.05)
                {
                    continue;
                }
                const double k = hash01(cx, cy, seed + 79 + n);
                x += this->padShift_ * (0.6 + 0.4 * k) +
                     std::sin(t * 0.21 + k * 20) * 2;
                y += std::cos(t * 0.17 + k * 20) * 1.5;
                const double rot = k * TAU + this->padShift_ * 0.012 +
                                   std::sin(t * 0.1 + k * 9) * 0.08;
                this->drawPad(x, y, r, rot, alpha, static_cast<int>(k * 30));
                if (k < 0.22)
                {
                    this->drawFlower(x - r * 0.15, y - r * 0.1, r * 0.55,
                                     0.55 + 0.45 * hash01(cx, cy, seed + 81), k * 9,
                                     alpha, 0, false);
                }
            }
        }
    }

    // Lily pads from new chatters, and the lilies your messages opened
    for (const auto &p : this->pads_)
    {
        const double age = t - p.start;
        const double slide = easeOutCubic(age / 2.2);
        const double x = lerp(p.fromX, p.x, slide) + this->padShift_;
        const double y = lerp(p.fromY, p.y, slide);
        const double fadeOut = clamp01((age - p.life) / 4);
        const double alpha = smoothstep(0, 0.4, age) * (1 - fadeOut);
        const double r = p.radius * (1 - 0.3 * fadeOut);
        const double rot = p.rot + this->padShift_ * 0.015;
        this->drawPad(x, y, r, rot, alpha, p.look);
        if (p.bloomAt >= 0 && t > p.bloomAt)
        {
            const double open =
                easeOutCubic((t - p.bloomAt) / 1.4) * (1 - fadeOut);
            this->drawFlower(x, y - 1, r * 1.05, open, p.rot * 3, alpha, 0,
                             false, true);
        }
    }
}

void KoiScene::drawLotus()
{
    if (this->lotusPetals_ <= 0.02 && this->hypeLevel_ <= 0)
    {
        return;
    }
    const double w = this->window_.width();
    const QRectF act = this->layout_.activity.empty()
                           ? QRectF()
                           : this->layout_.activity.front();
    const double x = act.isEmpty() ? w - 90 : act.right() - 74;
    const double y = this->rimT_ + 38;
    const double size = std::clamp(this->tile_ / 8.0, 0.9, 1.25);
    const double r = (24 + 4 * std::min(this->hypeLevel_, 5)) * size;
    const double alpha = std::min(1.0, 0.3 + this->lotusPetals_ / 3);
    this->drawPad(x + r * 0.35 + this->padShift_, y + r * 0.25, r * 1.35, 0.6,
                  alpha, 0);
    this->drawFlower(x + this->padShift_, y, r, 1.0, 0.2, alpha,
                     this->lotusPetals_, true);
}

void KoiScene::drawPetals()
{
    const double T = this->tile_;
    for (const auto &p : this->petals_)
    {
        const double age = this->now_ - p.start;
        const double alpha = smoothstep(0, 1, age) *
                             (1 - smoothstep(p.life - 3, p.life, age));
        const Rgb c = p.leaf ? LEAF : PETAL_PINK;
        const int idx = this->tileAt(p.x, p.y);
        if (idx >= 0)
        {
            this->blendTile(idx, c, alpha * 0.9 * this->water_[idx]);
        }
        if (p.leaf)
        {
            const int idx2 = this->tileAt(p.x + T * 0.8, p.y - T * 0.4);
            if (idx2 >= 0)
            {
                this->blendTile(idx2, mulRgb(c, 0.85),
                                alpha * 0.8 * this->water_[idx2]);
            }
        }
    }
}

void KoiScene::drawRaidCrest()
{
    const double age = this->now_ - this->raidStart_;
    if (age < 0 || age > 3.4)
    {
        return;
    }
    // The leading edge of the raid school pushes a bright wave ahead of it
    const double T = this->tile_;
    const double h = this->window_.height();
    const double amp = this->raidStrength_ * (1 - smoothstep(2.6, 3.4, age));
    for (int j = 0; j < this->rows_; ++j)
    {
        const double y = this->tileY(j);
        const double crest = -30 + this->raidSpeed_ * age -
                             110 * std::sin((y / h) * TAU / 2);
        const int ic = static_cast<int>((crest - this->ox_) / T);
        for (int i = std::max(0, ic - 8); i <= std::min(this->cols_ - 1, ic + 3);
             ++i)
        {
            const int idx = j * this->cols_ + i;
            const double k = (this->tileX(i) - crest) / (T * 1.6);
            const double wave = std::exp(-k * k) - (k < 0 ? 0.35 * std::exp(k * 0.5) : 0);
            const double amount =
                wave * amp * 0.16 * this->water_[idx] * (1 - 0.6 * this->calm_[idx]);
            if (amount > 0)
            {
                this->field_[idx] = addRgb(this->field_[idx], SUN, amount);
            }
            else
            {
                this->field_[idx] = mulRgb(this->field_[idx], 1 + amount);
            }
        }
    }
}

void KoiScene::drawHeron()
{
    const double u = (this->now_ - this->heronStart_) / 2.6;
    if (u <= 0 || u >= 1)
    {
        return;
    }
    const double T = this->tile_;
    const QPointF p = this->heronFrom_ + (this->heronTo_ - this->heronFrom_) * u;
    const QPointF dir = this->heronTo_ - this->heronFrom_;
    const double len = std::max(1.0, std::hypot(dir.x(), dir.y()));
    const double ux = dir.x() / len;
    const double uy = dir.y() / len;
    const double s = this->heronScale_;
    const double wing = 0.85 + 0.15 * std::sin(u * TAU * 2.2);
    const double strength = (this->reduced_ ? 0.22 : 0.36) *
                            smoothstep(0, 0.12, u) * (1 - smoothstep(0.88, 1, u));

    // A heron in flight: body, neck folded back, long bill, trailing legs
    // and broad wings, as ellipses in its own frame
    struct Part {
        double x;
        double y;
        double a;
        double b;
    };
    const std::array<Part, 8> parts{{{0, 0, 62, 15},
                                     {58, 0, 20, 9},
                                     {80, 2, 11, 7},
                                     {99, 3, 15, 2.6},
                                     {-98, 0, 40, 3.2},
                                     {-6, 78 * wing, 40, 84 * wing},
                                     {-6, -78 * wing, 40, 84 * wing},
                                     {-28, 0, 22, 10}}};
    const double reach = 200 * s;
    const int i0 = std::max(0, static_cast<int>((p.x() - reach - this->ox_) / T));
    const int i1 = std::min(this->cols_ - 1,
                            static_cast<int>((p.x() + reach - this->ox_) / T));
    const int j0 = std::max(0, static_cast<int>((p.y() - reach - this->oy_) / T));
    const int j1 = std::min(this->rows_ - 1,
                            static_cast<int>((p.y() + reach - this->oy_) / T));
    for (int j = j0; j <= j1; ++j)
    {
        for (int i = i0; i <= i1; ++i)
        {
            const double dx = this->tileX(i) - p.x();
            const double dy = this->tileY(j) - p.y();
            const double lx = (dx * ux + dy * uy) / s;
            const double ly = (-dx * uy + dy * ux) / s;
            double q = 1e9;
            for (const auto &part : parts)
            {
                const double ex = (lx - part.x) / part.a;
                const double ey = (ly - part.y) / part.b;
                q = std::min(q, ex * ex + ey * ey);
            }
            const double cover = 1 - smoothstep(0.7, 1.4, q);
            if (cover > 0)
            {
                const int idx = j * this->cols_ + i;
                this->field_[idx] = mulRgb(
                    this->field_[idx],
                    1 - strength * cover * (0.35 + 0.65 * this->shade_[idx]));
            }
        }
    }
}

void KoiScene::gradeAndLights()
{
    const double t = this->now_;
    const double ev = this->evening_;
    const double heronU = (t - this->heronStart_) / 2.6;
    const double dim = (this->reduced_ ? 0.05 : 0.12) * bump(heronU);
    const double morning = 0.10 * bump((t - this->morningStart_) / 4.5);
    const double h = std::max(1.0, this->window_.height());
    const Rgb dawn = rgb(255, 222, 180);
    // Blue hour darkens the open water while warm light lingers at the rim.
    const Rgb duskWater = rgb(26, 43, 67);
    const Rgb afterglow = rgb(178, 115, 83);
    const Rgb duskStone = rgb(102, 77, 63);
    if (ev > 0.001 || dim > 0.001 || morning > 0.001)
    {
        for (int j = 0; j < this->rows_; ++j)
        {
            const double warm = morning * (1 - 0.6 * this->tileY(j) / h);
            const double glowRow = 0.16 * (1 - std::min(1.0, this->tileY(j) / h * 1.6));
            for (int i = 0; i < this->cols_; ++i)
            {
                const int idx = j * this->cols_ + i;
                auto &c = this->field_[idx];
                const double shade = this->shade_[idx];
                const double e = ev * shade;
                if (e > 0.001)
                {
                    const double water = this->water_[idx];
                    if (water > 0)
                    {
                        c = mixRgb(mulRgb(c, 1 - 0.10 * e), duskWater,
                                   0.30 * e * water);
                        c = mixRgb(c, afterglow, glowRow * e * water);
                    }
                    else
                    {
                        c = mixRgb(mulRgb(c, 1 - 0.08 * e), duskStone, 0.30 * e);
                    }
                }
                if (dim > 0)
                {
                    c = mulRgb(c, 1 - dim * shade);
                }
                if (warm > 0)
                {
                    c = mixRgb(c, dawn, warm);
                }
            }
        }
    }

    // Floating paper lanterns light the evening water around them
    if (ev <= 0.01)
    {
        return;
    }
    const double T = this->tile_;
    for (const auto &l : this->lanterns_)
    {
        const double flicker = 0.9 + 0.1 * std::sin(t * 2.3 + l.phase) *
                                         (this->reduced_ ? 0.2 : 1.0);
        const double reach = 70;
        const int i0 = std::max(0, static_cast<int>((l.x - reach - this->ox_) / T));
        const int i1 = std::min(this->cols_ - 1,
                                static_cast<int>((l.x + reach - this->ox_) / T));
        const int j0 = std::max(0, static_cast<int>((l.y - reach - this->oy_) / T));
        const int j1 = std::min(this->rows_ - 1,
                                static_cast<int>((l.y + reach - this->oy_) / T));
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = i0; i <= i1; ++i)
            {
                const int idx = j * this->cols_ + i;
                const double dx = this->tileX(i) - l.x;
                const double dy = this->tileY(j) - l.y;
                const double dist = std::hypot(dx, dy);
                if (dist > reach || this->water_[idx] <= 0)
                {
                    continue;
                }
                const double glow = (1 - dist / reach) * (1 - this->calm_[idx]);
                this->field_[idx] = addRgb(this->field_[idx], LANTERN_GLOW,
                                           0.45 * glow * glow * ev * flicker);
                // The lantern itself: lit paper in a vermilion frame
                const double ax = std::abs(dx) / T;
                const double ay = std::abs(dy) / T;
                if (ax < 1.0 && ay < 1.0)
                {
                    this->blendTile(idx, LANTERN_CORE, 0.95 * ev);
                }
                else if (ax < 1.6 && ay < 1.9)
                {
                    this->blendTile(idx, mixRgb(VERMILION, LANTERN_GLOW, 0.35),
                                    0.9 * ev);
                }
            }
        }
    }
}

void KoiScene::writeImage()
{
    for (int j = 0; j < this->rows_; ++j)
    {
        auto *line = reinterpret_cast<QRgb *>(this->fieldImage_.scanLine(j));
        for (int i = 0; i < this->cols_; ++i)
        {
            const int idx = j * this->cols_ + i;
            const auto &c = this->field_[idx];
            const double light = 1 + this->jitterLight_[idx];
            const double warm = this->jitterWarm_[idx];
            const auto to8 = [](double v) {
                return std::clamp(static_cast<int>(v * 255.0 + 0.5), 0, 255);
            };
            line[i] = qRgb(to8(c.r * light + warm), to8(c.g * light),
                           to8(c.b * light - warm));
        }
    }
}

void KoiScene::paintBack(QPainter &painter, const SceneState &state,
                         const SceneLayout &layout)
{
    this->ensureGrid(layout);
    if (!this->started_)
    {
        this->layout_ = layout;
        this->startUp(state);
    }
    if (this->baseDirty_)
    {
        this->rebuildBase();
    }
    this->shadeWater();
    this->applyRipples();
    this->applyWake();
    this->drawCoins();
    this->drawAllKoi();
    this->drawFry();
    this->drawPads();
    this->drawLotus();
    this->drawPetals();
    this->drawRaidCrest();
    this->drawHeron();
    this->gradeAndLights();
    this->writeImage();

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRectF(this->ox_, this->oy_, this->cols_ * this->tile_,
                             this->rows_ * this->tile_),
                      this->fieldImage_);
    painter.drawImage(QPointF(0, 0), this->grout_);
    painter.restore();
}

bool KoiScene::hasFront() const
{
    return !this->glints_.empty() || !this->rings_.empty();
}

void KoiScene::paintFront(QPainter &painter, const SceneState &state,
                          const SceneLayout &layout)
{
    (void)layout;
    const double t = state.t;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const auto &g : this->glints_)
    {
        // Loose tesserae of light, square like the rest of the mosaic
        const double u = (t - g.start) / g.life;
        const double twinkle =
            0.6 + 0.4 * std::sin((t - g.start) * 14 + g.x * 0.1);
        const double alpha = (1 - u) * (1 - u) * 0.85 * twinkle;
        static const std::array<QColor, 4> HUES{
            QColor(218, 160, 52), QColor(194, 80, 50), QColor(46, 134, 116),
            QColor(250, 246, 236)};
        const QColor c = HUES[static_cast<size_t>(std::clamp(g.hue, 0, 3))];
        painter.fillRect(QRectF(g.x - g.size / 2, g.y - g.size / 2, g.size, g.size),
                         withAlpha(c, alpha));
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    for (const auto &r : this->rings_)
    {
        const double u = clamp01((t - r.start) / r.life);
        const double radius = lerp(r.r0, r.r1, easeOutCubic(u));
        for (int k = 0; k < 2; ++k)
        {
            const double rr = radius * (k == 0 ? 1.0 : 0.6);
            painter.setPen(QPen(withAlpha(r.color, r.alpha * (1 - u) * (k == 0 ? 1.0 : 0.6)),
                                1.4));
            painter.drawEllipse(QPointF(r.x, r.y), rr, rr * 0.72);
        }
    }
    painter.restore();
}

}  // namespace

std::unique_ptr<ThemeScene> makeKoiScene()
{
    return std::make_unique<KoiScene>();
}

}  // namespace chatterino::scenes
