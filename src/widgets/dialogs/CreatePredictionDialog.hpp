// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"

#include <QString>
#include <QStringList>
#include <array>

class QComboBox;
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QPropertyAnimation;
class QPushButton;
class QShowEvent;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class KickChannel;
class TwitchChannel;

class CreatePredictionDialog final : public BasePopup
{
public:
    static void showDialog(ChannelPtr channel,
                           const TwitchChannel &twitchChannel);
    static void showKickDialog(ChannelPtr channel,
                               const KickChannel &kickChannel);

    CreatePredictionDialog(ChannelPtr channel, QString broadcasterID,
                           QWidget *parent = nullptr, bool kick = false,
                           QString kickChannelSlug = {});

protected:
    void showEvent(QShowEvent *event) override;
    void themeChangedEvent() override;

private:
    struct OutcomeRow {
        QWidget *container = nullptr;
        QLabel *badge = nullptr;
        QLineEdit *input = nullptr;
        QWidget *removeContainer = nullptr;
        QPushButton *removeButton = nullptr;
        QGraphicsOpacityEffect *removeOpacityEffect = nullptr;
        QPropertyAnimation *removeWidthAnimation = nullptr;
        QPropertyAnimation *removeOpacityAnimation = nullptr;
        int topGap = 0;
        bool visible = false;
        bool removeVisible = false;
    };

    void applyTheme();
    void updateTitleCount();
    void updateOutcomeRows(bool animated = true);
    void updateOutcomeRowVisibility(OutcomeRow &row, bool visible,
                                    bool animated);
    void updateRemoveButton(OutcomeRow &row, bool visible, bool animated);
    int visibleOutcomeCount() const;
    int dialogHeightForVisibleOutcomes(int visibleCount) const;
    int errorLabelHeight() const;
    void updateDialogHeight(int visibleCount, bool animated);
    void updateStartButton();
    void removeOutcome(size_t index);
    void submit();
    void finishSubmitFailure(const QString &message);
    void showError(const QString &message);
    void clearError();
    QStringList outcomes() const;

    ChannelPtr channel_;
    QString broadcasterID_;
    bool kick_ = false;
    QString kickChannelSlug_;

    QLineEdit *title_ = nullptr;
    QLabel *titleCount_ = nullptr;
    QVBoxLayout *outcomesLayout_ = nullptr;
    std::array<OutcomeRow, 10> outcomeRows_{};
    QComboBox *duration_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QPushButton *startButton_ = nullptr;

    bool submitting_ = false;
};

}  // namespace chatterino
