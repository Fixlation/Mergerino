// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "widgets/BasePopup.hpp"

#include <QString>

#include <functional>
#include <vector>

class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class PredictionTimerBar;
class OutcomeChoiceWidget;

class ManagePredictionDialog final : public BasePopup
{
public:
    static void showDialog(ChannelPtr channel, QString broadcasterID,
                           QString channelLogin,
                           const HelixPrediction &prediction,
                           bool useModerationAuth = false);
    static void showViewerDialog(ChannelPtr channel, QString broadcasterID,
                                 QString channelLogin,
                                 const HelixPrediction &prediction);

    ManagePredictionDialog(ChannelPtr channel, QString broadcasterID,
                           QString channelLogin,
                           const HelixPrediction &prediction,
                           bool useModerationAuth = false,
                           bool canManage = true,
                           QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;

private:
    void applyTheme();
    void rebuildOutcomes();
    void updateTimerUi();
    void updateActionButton();
    void updateDialogSize();
    void setChoosingOutcome(bool choosing);
    void setVoting(bool voting);
    void selectOutcome(const QString &outcomeID);
    void submitPrediction();
    void endPrediction(bool refundPoints, QString winningOutcomeID,
                       std::function<void(const HelixPrediction &)>
                           successCallback,
                       std::function<void(const QString &)> failureCallback);
    void cancelPrediction();
    void lockPrediction();
    void resolvePrediction();
    void showError(const QString &message);
    void clearError();

    bool submissionsOpen() const;
    bool canVote() const;
    double timerProgress() const;

    ChannelPtr channel_;
    QString broadcasterID_;
    QString channelLogin_;
    HelixPrediction prediction_;
    QString selectedOutcomeID_;
    bool useModerationAuth_ = false;
    bool canManage_ = true;

    QLabel *statusLabel_ = nullptr;
    QLabel *descriptionLabel_ = nullptr;
    QLabel *titleLabel_ = nullptr;
    PredictionTimerBar *timerBar_ = nullptr;
    QVBoxLayout *outcomesLayout_ = nullptr;
    QWidget *voteControls_ = nullptr;
    QSpinBox *voteAmount_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *predictButton_ = nullptr;
    QPushButton *summaryButton_ = nullptr;
    QPushButton *backButton_ = nullptr;
    QPushButton *actionButton_ = nullptr;
    QTimer *timer_ = nullptr;

    std::vector<OutcomeChoiceWidget *> outcomeChoices_;

    bool submitting_ = false;
    bool lastSubmissionsOpen_ = false;
    bool choosingOutcome_ = false;
    bool voting_ = false;
};

}  // namespace chatterino
