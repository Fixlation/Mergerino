// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"

#include <QString>
#include <QStringList>
#include <array>

class QCheckBox;
class QComboBox;
class QEvent;
class QGraphicsOpacityEffect;
class QLabel;
class QLineEdit;
class QObject;
class QPoint;
class QPixmap;
class QPropertyAnimation;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QVariantAnimation;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class KickChannel;
class TwitchChannel;

class CreatePollDialog final : public BasePopup
{
public:
    static void showDialog(ChannelPtr channel, const TwitchChannel &twitchChannel);
    static void showKickDialog(ChannelPtr channel,
                               const KickChannel &kickChannel);

    CreatePollDialog(ChannelPtr channel, QString broadcasterID,
                     QString channelLogin, QWidget *parent = nullptr,
                     bool kick = false);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void themeChangedEvent() override;

private:
    struct ResponseRow {
        QWidget *container = nullptr;
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

    void updateAdditionalVotesCursor(const QPoint &position);
    void applyTheme();
    void loadChannelPointsMetadata();
    void loadChannelPointsIcon(const QString &url);
    void setChannelPointsName(const QString &name);
    void setChannelPointsIcon(const QPixmap &pixmap);
    void updatePointsControls(bool animated = true);
    void updateResponseRows(bool animated = true);
    void updateResponseRowVisibility(ResponseRow &row, bool visible,
                                     bool animated);
    void updateRemoveButton(ResponseRow &row, bool visible, bool animated);
    int visibleResponseCount() const;
    int dialogHeightForVisibleResponses(int visibleCount) const;
    int errorLabelHeight() const;
    void updateDialogHeight(int visibleCount, bool animated);
    void updateStartButton();
    void removeResponse(size_t index);
    void submit();
    void finishSubmitFailure(const QString &message);
    void showError(const QString &message);
    void clearError();
    QStringList responses() const;

    ChannelPtr channel_;
    QString broadcasterID_;
    QString channelLogin_;
    bool kick_ = false;

    QLineEdit *question_ = nullptr;
    QVBoxLayout *responsesLayout_ = nullptr;
    std::array<ResponseRow, 5> responseRows_{};
    QCheckBox *allowAdditionalVotes_ = nullptr;
    QSpinBox *pointsPerVote_ = nullptr;
    QWidget *pointsContainer_ = nullptr;
    QGraphicsOpacityEffect *pointsOpacityEffect_ = nullptr;
    QPropertyAnimation *pointsHeightAnimation_ = nullptr;
    QPropertyAnimation *pointsOpacityAnimation_ = nullptr;
    QVariantAnimation *dialogHeightAnimation_ = nullptr;
    QLabel *channelPointsIcon_ = nullptr;
    QLabel *channelPointsLabel_ = nullptr;
    QComboBox *duration_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QPushButton *startButton_ = nullptr;

    bool submitting_ = false;
};

}  // namespace chatterino
