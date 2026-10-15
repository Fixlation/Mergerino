// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/BaseWindow.hpp"

#include "mocks/BaseApplication.hpp"
#include "Test.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#ifdef USEWINSDK
#    include <Windows.h>

#    include <algorithm>
#    include <vector>

namespace chatterino {
class BaseWindowTestAccess
{
public:
    static void visible(BaseWindow &window, QRect bounds)
    {
        window.lastVisibleBounds_ = bounds;
    }
    static void minimize(BaseWindow &window)
    {
        window.savePreMinimizeBounds();
    }
    static bool correct(BaseWindow &window, WINDOWPOS &position)
    {
        return window.handleWINDOWPOSCHANGING(&position);
    }
    static std::optional<QRect> previous(const BaseWindow &window)
    {
        return window.preMinimizeBounds_;
    }
    static void size(BaseWindow &window, WPARAM state)
    {
        MSG message{};
        message.wParam = state;
        window.handleSIZE(&message);
    }
    static void normal(BaseWindow &window, QRect bounds)
    {
        window.currentBounds_ = bounds;
    }
    static bool native(BaseWindow &window, WINDOWPOS &position, qintptr &result)
    {
        MSG message{};
        message.message = WM_WINDOWPOSCHANGING;
        message.lParam = reinterpret_cast<LPARAM>(&position);
        return window.nativeEvent("windows_generic_MSG", &message, &result);
    }
};
}  // namespace chatterino

using namespace chatterino;

namespace {
BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfo(monitor, &info))
    {
        auto &monitors = *reinterpret_cast<std::vector<QRect> *>(context);
        monitors.emplace_back(info.rcMonitor.left, info.rcMonitor.top,
                              info.rcMonitor.right - info.rcMonitor.left,
                              info.rcMonitor.bottom - info.rcMonitor.top);
    }
    return TRUE;
}

QRect smallBounds(QRect monitor)
{
    return {monitor.x() + 30, monitor.y() + 40,
            std::min(400, monitor.width() - 60),
            std::min(300, monitor.height() - 80)};
}

WINDOWPOS proposal(QRect bounds, UINT flags = 0)
{
    WINDOWPOS pos{};
    pos.x = bounds.x();
    pos.y = bounds.y();
    pos.cx = bounds.width();
    pos.cy = bounds.height();
    pos.flags = flags;
    return pos;
}

QRect positionBounds(const WINDOWPOS &position)
{
    return {position.x, position.y, position.cx, position.cy};
}

class BaseWindowRestore : public ::testing::Test
{
protected:
    mock::BaseApplication app;
    BaseWindow window{{BaseWindow::EnableCustomFrame,
                       BaseWindow::DisableLayoutSave, BaseWindow::TopMost}};
    std::vector<QRect> monitors;

    void SetUp() override
    {
        ASSERT_TRUE(
            ::EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                                  reinterpret_cast<LPARAM>(&this->monitors)));
        ASSERT_FALSE(this->monitors.empty());
    }
    void arm(QRect bounds)
    {
        BaseWindowTestAccess::visible(this->window, bounds);
        BaseWindowTestAccess::minimize(this->window);
    }
};
}  // namespace

// These tests read native monitor geometry but never show, move, or minimize
// a desktop window. The QWidget and its events use Qt's offscreen platform.
TEST_F(BaseWindowRestore, CorrectsNormalRestoreBeforeNativePositionIsApplied)
{
    if (this->monitors.size() < 2)
    {
        GTEST_SKIP() << "Requires two native display monitors";
    }
    for (size_t i = 0; i < this->monitors.size(); ++i)
    {
        BaseWindow local{{BaseWindow::EnableCustomFrame,
                          BaseWindow::DisableLayoutSave, BaseWindow::TopMost}};
        const auto previous = smallBounds(this->monitors[i]);
        BaseWindowTestAccess::visible(local, previous);
        BaseWindowTestAccess::minimize(local);
        auto pos = proposal(
            smallBounds(this->monitors[(i + 1) % this->monitors.size()]));
        EXPECT_TRUE(BaseWindowTestAccess::correct(local, pos));
        EXPECT_EQ(positionBounds(pos), previous);
    }
}

TEST_F(BaseWindowRestore,
       MaximizedRestoreUsesVisibleMonitorInsteadOfNormalBounds)
{
    if (this->monitors.size() < 2)
    {
        GTEST_SKIP() << "Requires two native display monitors";
    }
    const auto normal = smallBounds(this->monitors[0]);
    const auto maximized = this->monitors[1];
    BaseWindowTestAccess::normal(this->window, normal);
    this->arm(maximized);
    auto pos = proposal(this->monitors[0]);
    ASSERT_TRUE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(positionBounds(pos), maximized);
    EXPECT_EQ(this->window.getBounds(), normal);
}

TEST_F(BaseWindowRestore, MoveOnlyRestoreHonoursNoSizeFlag)
{
    if (this->monitors.size() < 2)
    {
        GTEST_SKIP() << "Requires two native display monitors";
    }
    const auto previous = smallBounds(this->monitors[1]);
    this->arm(previous);
    auto pos = proposal(smallBounds(this->monitors[0]), SWP_NOSIZE);
    pos.cx = 0;
    pos.cy = 0;
    ASSERT_TRUE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(QPoint(pos.x, pos.y), previous.topLeft());
    EXPECT_EQ(pos.cx, 0);
    EXPECT_EQ(pos.cy, 0);
    EXPECT_EQ(pos.flags, SWP_NOSIZE);
}

TEST_F(BaseWindowRestore, SameMonitorRestoreKeepsProposedSizeAndPosition)
{
    const auto previous = smallBounds(this->monitors[0]);
    this->arm(previous);
    const auto desired = previous.adjusted(10, 10, -20, -20);
    auto pos = proposal(desired);
    EXPECT_FALSE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(positionBounds(pos), desired);
}

TEST_F(BaseWindowRestore, NormalMovesAfterRestoreAreNotPinnedToOldMonitor)
{
    this->arm(smallBounds(this->monitors[0]));
    BaseWindowTestAccess::size(this->window, SIZE_RESTORED);
    EXPECT_FALSE(BaseWindowTestAccess::previous(this->window));
    const auto desired = smallBounds(this->monitors.back());
    auto pos = proposal(desired);
    EXPECT_FALSE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(positionBounds(pos), desired);
}

TEST_F(BaseWindowRestore, MinimizedParkingMoveDoesNotRestorePrematurely)
{
    this->arm(smallBounds(this->monitors[0]));
    const QRect parking{-32000, -32000, 160, 28};
    auto pos = proposal(parking);
    EXPECT_FALSE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(positionBounds(pos), parking);
}

TEST_F(BaseWindowRestore, DisconnectedMonitorAllowsNativeFallback)
{
    this->arm({1000000, 1000000, 400, 300});
    const auto fallback = smallBounds(this->monitors[0]);
    auto pos = proposal(fallback);
    EXPECT_FALSE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(positionBounds(pos), fallback);
}

TEST_F(BaseWindowRestore, NonMoveAndHideOperationsAreUntouched)
{
    this->arm(smallBounds(this->monitors[0]));
    const auto desired = smallBounds(this->monitors.back());
    for (auto flags : {SWP_NOMOVE, SWP_HIDEWINDOW, SWP_NOMOVE | SWP_NOSIZE})
    {
        auto pos = proposal(desired, flags);
        EXPECT_FALSE(BaseWindowTestAccess::correct(this->window, pos));
        EXPECT_EQ(positionBounds(pos), desired);
        EXPECT_EQ(pos.flags, flags);
    }
}

TEST_F(BaseWindowRestore, EmptyVisibleBoundsDoNotInventRestorePosition)
{
    BaseWindowTestAccess::minimize(this->window);
    EXPECT_FALSE(BaseWindowTestAccess::previous(this->window));
    const auto desired = smallBounds(this->monitors[0]);
    auto pos = proposal(desired);
    EXPECT_FALSE(BaseWindowTestAccess::correct(this->window, pos));
    EXPECT_EQ(positionBounds(pos), desired);
}

TEST_F(BaseWindowRestore, RepeatedMinimizeRetainsFirstVisibleBounds)
{
    const auto previous = smallBounds(this->monitors[0]);
    this->arm(previous);
    BaseWindowTestAccess::visible(this->window, {-32000, -32000, 160, 28});
    BaseWindowTestAccess::minimize(this->window);
    ASSERT_TRUE(BaseWindowTestAccess::previous(this->window));
    EXPECT_EQ(*BaseWindowTestAccess::previous(this->window), previous);
}

TEST_F(BaseWindowRestore, NativeCorrectionStillAllowsDefaultWindowValidation)
{
    this->arm(smallBounds(this->monitors[0]));
    auto pos = proposal(smallBounds(this->monitors.back()));
    qintptr result = 42;
    EXPECT_FALSE(BaseWindowTestAccess::native(this->window, pos, result));
    EXPECT_EQ(result, 42);
    EXPECT_EQ(positionBounds(pos), smallBounds(this->monitors[0]));
}

TEST_F(BaseWindowRestore, QueuedNormalBoundsWaitForQtGeometryUpdate)
{
    const QRect oldBounds{20, 30, 300, 150};
    BaseWindowTestAccess::normal(this->window, oldBounds);
    BaseWindowTestAccess::size(this->window, SIZE_RESTORED);
    EXPECT_EQ(this->window.getBounds(), oldBounds);
    this->window.setGeometry(50, 60, 400, 250);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    EXPECT_EQ(this->window.getBounds(), this->window.geometry());
}

TEST_F(BaseWindowRestore, MinimizeBeforeQueuedUpdatePreservesNormalBounds)
{
    const QRect oldBounds{20, 30, 300, 150};
    BaseWindowTestAccess::normal(this->window, oldBounds);
    BaseWindowTestAccess::size(this->window, SIZE_RESTORED);
    BaseWindowTestAccess::size(this->window, SIZE_MINIMIZED);
    this->window.setGeometry(-32000, -32000, 160, 28);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    EXPECT_EQ(this->window.getBounds(), oldBounds);
}

TEST_F(BaseWindowRestore, DestroyingWindowCancelsQueuedBoundsUpdate)
{
    auto local = std::make_unique<BaseWindow>(FlagsEnum<BaseWindow::Flags>{
        BaseWindow::EnableCustomFrame, BaseWindow::DisableLayoutSave,
        BaseWindow::TopMost});
    BaseWindowTestAccess::size(*local, SIZE_RESTORED);
    local.reset();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

#endif
