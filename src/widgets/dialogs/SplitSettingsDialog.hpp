// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <pajlada/signals/signal.hpp>

#include <cstdint>

class QCloseEvent;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QVariantAnimation;
class QWidget;

namespace chatterino {

enum class ActivityTimeDisplayMode : std::uint8_t;
enum class PlatformIndicatorMode : std::uint8_t;

class SplitSettingsDialog final : public BaseWindow
{
public:
    explicit SplitSettingsDialog(bool isActivityPane,
                                 bool showTwitchBitsMinimum,
                                 bool showKickKicksMinimum,
                                 bool showTikTokGiftMinimum,
                                 bool showStreamDatabaseBadgeFeed,
                                 QWidget *parent = nullptr);

    void setPlatformIndicatorMode(PlatformIndicatorMode mode);
    PlatformIndicatorMode platformIndicatorMode() const;

    void setFilterActivity(bool enabled);
    bool filterActivity() const;

    void setActivityMessageScale(qreal scale);
    qreal activityMessageScale() const;
    void setCollapseGiftedSubscriptions(bool enabled);
    bool collapseGiftedSubscriptions() const;
    void setActivityTimeDisplayMode(ActivityTimeDisplayMode mode);
    ActivityTimeDisplayMode activityTimeDisplayMode() const;
    void setSlowerChatEnabled(bool enabled);
    bool slowerChatEnabled() const;
    void setSlowerChatMessagesPerSecond(qreal value);
    qreal slowerChatMessagesPerSecond() const;
    void setViewerCountEnabled(bool enabled);
    bool viewerCountEnabled() const;
    void setStreamDatabaseBadgeFeedVisible(bool visible);
    bool streamDatabaseBadgeFeedVisible() const;
    void setActivityMinimum(uint32_t value);
    uint32_t activityMinimum() const;

    bool hasAcceptedChanges() const;

    pajlada::Signals::NoArgSignal closed;

protected:
    void closeEvent(QCloseEvent *event) override;
    void themeChangedEvent() override;
    void scaleChangedEvent(float newScale) override;

private:
    struct {
        QComboBox *indicatorMode{};
        QCheckBox *filterActivity{};
        QComboBox *activityScale{};
        QCheckBox *collapseGiftedSubscriptions{};
        QComboBox *activityTimeDisplayMode{};
        QCheckBox *slowerChat{};
        QDoubleSpinBox *slowerChatRate{};
        QCheckBox *viewerCount{};
        QCheckBox *streamDatabaseBadgeFeed{};
        QSpinBox *activityMinimum{};
        QWidget *slowerChatRateLabel{};
        QWidget *slowerChatRateField{};
        QVariantAnimation *slowerChatRateAnimation{};
        qreal slowerChatRateVisibilityProgress = 1.0;
    } ui_{};

    const bool isActivityPane_;
    const bool showTwitchBitsMinimum_;
    const bool showKickKicksMinimum_;
    const bool showTikTokGiftMinimum_;
    const bool showStreamDatabaseBadgeFeed_;
    bool hasAcceptedChanges_{false};

    void updateSlowerChatVisibility(bool animate = true);
    void applySlowerChatRateVisibilityProgress(qreal progress);
    void ok();
    void addShortcuts() override;
};

}  // namespace chatterino
