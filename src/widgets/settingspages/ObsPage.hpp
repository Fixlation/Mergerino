// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingsPage.hpp"

namespace chatterino {

void addObsSettings(GeneralPageView &layout);

class ObsPage final : public SettingsPage
{
public:
    ObsPage();

    bool filterElements(const QString &query) override;

private:
    GeneralPageView *view_{};
};

}  // namespace chatterino
