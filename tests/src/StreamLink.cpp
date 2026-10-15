#include "util/StreamLink.hpp"

#include "singletons/Settings.hpp"

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

using namespace chatterino;

namespace {
class StreamLinkProcess : public ::testing::Test
{
protected:
    QTemporaryDir directory;
    bool originalCustom = getSettings()->streamlinkUseCustomPath.getValue();
    QString originalPath = getSettings()->streamlinkPath.getValue();

    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        getSettings()->streamlinkUseCustomPath = true;
        getSettings()->streamlinkPath = directory.path();
    }

    void TearDown() override
    {
        getSettings()->streamlinkUseCustomPath = originalCustom;
        getSettings()->streamlinkPath = originalPath;
    }
};
}  // namespace

TEST_F(StreamLinkProcess, MissingExecutableReportsOnceAndReleasesProcess)
{
    QObject context;
    QEventLoop loop;
    int callbacks = 0;
    QPointer<QProcess> process =
        getStreamQualities("https://kick.com/test", &context,
                           [&](QStringList options, QString error) {
                               ++callbacks;
                               EXPECT_TRUE(options.isEmpty());
                               EXPECT_TRUE(error.contains("could not start"));
                           });
    QObject::connect(process, &QObject::destroyed, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_EQ(callbacks, 1);
    EXPECT_TRUE(process.isNull());
}

TEST_F(StreamLinkProcess, DestroyedConsumerNeverReceivesLateResult)
{
    auto context = std::make_unique<QObject>();
    QEventLoop loop;
    int callbacks = 0;
    QPointer<QProcess> process = getStreamQualities(
        "https://kick.com/test", context.get(), [&](QStringList, QString) {
            ++callbacks;
        });
    QObject::connect(process, &QObject::destroyed, &loop, &QEventLoop::quit);
    context.reset();
    // Cancellation happened in the outer loop; flush its deferred deletion
    // before entering a nested event loop.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    if (process)
    {
        loop.exec();
    }
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(process.isNull());
}

TEST_F(StreamLinkProcess, ImmediateServiceChangeCancelsQueuedLookup)
{
    QObject context;
    QEventLoop loop;
    int callbacks = 0;
    QPointer<QProcess> process = getStreamQualities(
        "https://kick.com/test", &context, [&](QStringList, QString) {
            ++callbacks;
        });
    QObject::connect(process, &QObject::destroyed, &loop, &QEventLoop::quit);
    cancelStreamlinkQuery(process);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    if (process)
    {
        loop.exec();
    }
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(process.isNull());
}

TEST(StreamLink, SortsExactQualityNamesAndOmitsAliases)
{
    EXPECT_EQ(parseStreamlinkQualities(R"({"streams":{
        "360p":{},"1080p60":{},"720p60":{},"720p":{},
        "audio_only":{},"best":{},"worst":{}}})"),
              (QStringList{"1080p60", "720p60", "720p", "360p", "audio_only"}));
}

TEST(StreamLink, HandlesSingleAndAudioOnlyStreams)
{
    EXPECT_EQ(parseStreamlinkQualities(
                  R"({"streams":{"720p":{},"best":{},"worst":{}}})"),
              (QStringList{"720p"}));
    EXPECT_EQ(
        parseStreamlinkQualities(R"({"streams":{"audio_only":{},"best":{}}})"),
        (QStringList{"audio_only"}));
    EXPECT_EQ(parseStreamlinkQualities(R"({"streams":{"best":{}}})"),
              (QStringList{"best"}));
}

TEST(StreamLink, RejectsErrorsOfflineAndMalformedOutput)
{
    for (const auto *output :
         {"", "not json", "[]", "{}",
          R"({"error":"No playable streams found"})", R"({"streams":{}})",
          R"({"streams":{"720p":null}})"})
    {
        SCOPED_TRACE(output);
        EXPECT_TRUE(parseStreamlinkQualities(output).isEmpty());
    }
}
