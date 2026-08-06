#pragma once

#include "widgets/BasePopup.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QString>

#include <memory>
#include <vector>

class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace chatterino {

class Channel;
class KickChannel;
class KickPredictionTimerBar;
class KickPredictionOutcomeWidget;

class KickPredictionDialog final : public BasePopup
{
public:
    static void showDialog(const std::shared_ptr<KickChannel> &channel,
                           const std::shared_ptr<Channel> &messageChannel);
    KickPredictionDialog(std::shared_ptr<KickChannel> channel,
                         std::shared_ptr<Channel> messageChannel,
                         QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;

private:
    void refresh();
    void applyTheme();
    void rebuildOutcomes();
    void updateTimerUi();
    void updateActionButton();
    void updateDialogSize();
    void setChoosingOutcome(bool choosing);
    void selectOutcome(const QString &outcomeID);
    void endSubmissions();
    void resolveSelectedOutcome();
    void refundPrediction();
    void updatePrediction(const QString &state,
                          const QString &winningOutcomeID = {});
    bool submissionsOpen() const;
    double timerProgress() const;

    std::weak_ptr<KickChannel> channel_;
    std::weak_ptr<Channel> messageChannel_;
    QString channelSlug_;
    QString selectedOutcomeID_;
    QLabel *statusLabel_ = nullptr;
    QLabel *descriptionLabel_ = nullptr;
    QLabel *titleLabel_ = nullptr;
    KickPredictionTimerBar *timerBar_ = nullptr;
    QVBoxLayout *outcomesLayout_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *backButton_ = nullptr;
    QPushButton *actionButton_ = nullptr;
    QTimer *timer_ = nullptr;
    std::vector<KickPredictionOutcomeWidget *> outcomeChoices_;
    bool managementPending_ = false;
    bool choosingOutcome_ = false;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
