// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <functional>
#include <memory>

class QLineEdit;

namespace pajlada {
class SettingListener;
}  // namespace pajlada

namespace chatterino {

class DescriptionLabel;
class GeneralPageView;

class ThemesPage final : public SettingsPage
{
public:
    ThemesPage();
    ~ThemesPage() override;

    bool filterElements(const QString &query) override;

private:
    void initLayout(GeneralPageView &layout);
    void setStatus(const QString &text);

    GeneralPageView *view_{};
    QLineEdit *shareCode_{};
    DescriptionLabel *status_{};

    /// The custom editor's type, independent of the active preset picker.
    bool animatedDraft_{};
    std::function<void(bool)> showCustomType_;
    std::unique_ptr<pajlada::SettingListener> draftListener_;
};

}  // namespace chatterino
