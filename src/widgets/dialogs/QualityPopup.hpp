// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/StreamLink.hpp"
#include "widgets/BasePopup.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QPointer>
#include <QVBoxLayout>

class QLabel;
class QPushButton;
class QProcess;

namespace chatterino {

class QualityPopup : public BasePopup
{
public:
    explicit QualityPopup(const QList<StreamlinkSource> &sources);
    ~QualityPopup() override;
    static void showDialog(const QList<StreamlinkSource> &sources);

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void okButtonClicked();
    void cancelButtonClicked();
    void refreshQualities();
    void cancelQuery();

    struct {
        QVBoxLayout *vbox;
        QComboBox *selector;
        QComboBox *service;
        QLabel *status;
        QPushButton *retry;
        QDialogButtonBox *buttonBox;
    } ui_{};

    QPointer<QProcess> query_;
    uint64_t queryGeneration_ = 0;
};

}  // namespace chatterino
