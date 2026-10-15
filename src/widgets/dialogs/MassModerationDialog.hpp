// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/moderation/MassModerationQueue.hpp"
#include "ForwardDecl.hpp"
#include "widgets/BasePopup.hpp"

#include <QTimer>

#include <memory>
#include <vector>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace chatterino {

class MassModerationDialogTestAccess;

class MassModerationDialog final : public BasePopup
{
public:
    MassModerationDialog(ChannelPtr channel, bool ban,
                         QWidget *parent = nullptr);
    ~MassModerationDialog() override;

    static MassModerationDialog *find(const ChannelPtr &channel, bool ban);
    static std::vector<MassModerationDialog *> forChannel(
        const ChannelPtr &channel);
    static pajlada::Signals::NoArgSignal &progressChanged();
    void reopen();
    QString previewText() const;
    int completed() const;
    int total() const;
    int previewCompleted() const;
    bool previewExpired() const;
    MassModerationQueue::State state() const;
    void togglePause();
    void cancelAndDismissPreview();

protected:
    void closeEvent(QCloseEvent *event) override;
    void themeChangedEvent() override;

private:
    void updateList();
    void start();
    void updateProgress();
    void startQueue(MassModerationQueue::Action action,
                    const QStringList &names);
    void reset();
    void observePreviewMessage(const MessagePtr &message);
    void publishPreviewProgress();

    friend class MassModerationDialogTestAccess;

    ChannelPtr channel_;
    bool ban_;
    bool kick_;
    std::unique_ptr<MassModerationQueue> queue_;
    QTimer previewExpiry_;
    QTimer previewFallback_;
    QString previewCurrentUser_;
    QString previewWaitingUser_;
    int previewCompleted_ = 0;
    bool previewNoticeSeen_ = false;
    bool previewExpired_ = false;
    bool previewRetentionStarted_ = false;
    bool sessionClosed_ = false;
    QPlainTextEdit *names_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QLineEdit *reason_ = nullptr;
    QSpinBox *interval_ = nullptr;
    QLabel *listStatus_ = nullptr;
    QLabel *progressStatus_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QPushButton *format_ = nullptr;
    QPushButton *start_ = nullptr;
    QPushButton *pause_ = nullptr;
    QPushButton *stop_ = nullptr;
    QPushButton *cancel_ = nullptr;
};

// Embedded above the chat composer. It observes queues without owning them.
class MassModerationPreview final : public BaseWidget
{
public:
    explicit MassModerationPreview(QWidget *parent = nullptr);
    ~MassModerationPreview() override;
    void setChannel(ChannelPtr channel);
    int preferredHeight() const;
    pajlada::Signals::NoArgSignal heightChanged;

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;

private:
    void refresh();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chatterino
