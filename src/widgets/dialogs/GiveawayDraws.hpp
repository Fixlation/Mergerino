// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QLineEdit>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <vector>

class QVariantAnimation;

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

/// One eligible entrant of a draw. The weight is the entrant's ticket count.
struct GiveawayDrawEntry {
    QString key;
    QString name;
    MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    int weight = 1;

    bool operator==(const GiveawayDrawEntry &other) const = default;
};

QString giveawayPlatformIconPath(MessagePlatform platform);

/// Picks an index into entries with probability proportional to its weight.
/// Returns -1 if there is nothing to pick from.
int pickWeightedGiveawayEntry(const std::vector<GiveawayDrawEntry> &entries);

struct GiveawayDrawColors {
    QColor background;
    QColor panel;
    QColor text;
    QColor muted;
    QColor border;
};

/// Horizontal case-opening reel: cards scroll past a fixed marker and come to
/// rest on a winner that was chosen before the spin started.
class GiveawayReel final : public QWidget
{
public:
    explicit GiveawayReel(QWidget *parent = nullptr);
    ~GiveawayReel() override;

    void setColors(const GiveawayDrawColors &colors);

    /// Spins towards entries[winner]. The other cards are drawn from entries
    /// in proportion to their weights.
    void spin(const std::vector<GiveawayDrawEntry> &entries, int winner);
    /// Jumps straight to the resting position.
    void finish();
    void stop();
    bool spinning() const;
    bool landed() const;

    /// Called once the reel has come to rest on the winner.
    std::function<void()> onLanded;
    /// Called when the reel is clicked after it has landed.
    std::function<void()> onDismissed;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    struct Card {
        GiveawayDrawEntry entry;
    };

    double travelFor(double progress) const;
    void land();
    const QPixmap &platformIcon(MessagePlatform platform);

    GiveawayDrawColors colors_;
    std::vector<Card> cards_;
    int winnerSlot_ = 0;
    double offset_ = 0;
    double landingOffset_ = 0;
    double restingOffset_ = 0;
    bool spinning_ = false;
    bool landed_ = false;
    QVariantAnimation *spin_ = nullptr;
    QVariantAnimation *settle_ = nullptr;
    std::array<QPixmap, 4> platformIcons_;
    qreal platformIconRatio_ = 0;
};

/// Prize wheel with one slice per entrant, sized by weight.
class GiveawayWheel final : public QWidget
{
public:
    explicit GiveawayWheel(QWidget *parent = nullptr);
    ~GiveawayWheel() override;

    void setColors(const GiveawayDrawColors &colors);

    /// Replaces the slices. While the wheel spins or shows a result the new
    /// slices are kept back until the result has been shown.
    void setEntries(std::vector<GiveawayDrawEntry> entries);
    const std::vector<GiveawayDrawEntry> &entries() const;

    void spin(int winner);
    void finish();
    void stop();
    bool spinning() const;

    /// Called once the wheel has come to rest on the winner.
    std::function<void()> onLanded;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    int sliceAt(double angle) const;
    void rebuildFace();
    void applyPendingEntries();
    QRectF wheelRect() const;

    GiveawayDrawColors colors_;
    std::vector<GiveawayDrawEntry> entries_;
    std::vector<GiveawayDrawEntry> pendingEntries_;
    bool hasPendingEntries_ = false;
    // start angle (degrees, clockwise from the pointer) of every slice
    std::vector<double> sliceStarts_;
    double totalWeight_ = 0;
    QPixmap face_;
    double rotation_ = 0;
    double spinFrom_ = 0;
    double spinTo_ = 0;
    int winner_ = -1;
    int lastSlice_ = -1;
    double pointerKick_ = 0;
    bool spinning_ = false;
    bool showingResult_ = false;
    QVariantAnimation *spin_ = nullptr;
    QTimer resultTimer_;
    QTimer pointerTimer_;
};

/// Small square showing the target emote of an emote guess. While the target
/// is hidden it spins a blurred reel of decoy emotes, and a win can land that
/// reel on the real emote.
class GiveawayEmoteTile final : public QWidget
{
public:
    explicit GiveawayEmoteTile(QWidget *parent = nullptr);

    void setColors(const GiveawayDrawColors &colors);
    void setEmote(EmotePtr emote);
    /// Emotes cycled through while the target is hidden
    void setDecoys(std::vector<EmotePtr> decoys);
    /// With animated set, a hidden tile lands its reel on the target first
    void setRevealed(bool revealed, bool animated = false);
    bool revealed() const;
    bool landing() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    bool spinning() const;
    void tick();
    void updateTimer();
    void paintReel(QPainter &painter, const QRectF &frame);
    QPixmap blurred(const EmotePtr &emote);

    GiveawayDrawColors colors_;
    EmotePtr emote_;
    std::vector<EmotePtr> decoys_;
    QHash<const void *, QPixmap> blurCache_;
    bool revealed_ = true;
    double scroll_ = 0;
    QElapsedTimer clock_;
    qint64 lastTick_ = 0;
    bool landing_ = false;
    double landingFrom_ = 0;
    double landingTo_ = 0;
    qint64 landingStart_ = 0;
    qint64 landingDuration_ = 1;
    qint64 landingSlot_ = 0;
    QTimer frameTimer_;
};

/// Line edit that keeps its text secret by showing a fixed-length run of
/// shifting characters instead, so not even the length gives it away.
class GiveawaySecretLineEdit final : public QLineEdit
{
public:
    explicit GiveawaySecretLineEdit(QWidget *parent = nullptr);

    void setSecret(bool secret);
    bool secret() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void updateTimer();

    bool secret_ = false;
    QString glyphs_;
    QTimer scrambleTimer_;
};

}  // namespace chatterino
