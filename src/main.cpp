// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "BrowserExtension.hpp"
#include "common/Args.hpp"
#include "common/Env.hpp"
#include "common/Modes.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "providers/IvrApi.hpp"
#include "providers/NetworkConfigurationProvider.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "RunGui.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Updates.hpp"
#include "util/AttachToConsole.hpp"
#include "util/ChatterinoImport.hpp"
#include "util/IpcQueue.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSslSocket>
#include <QStringList>
#include <QtCore/QtPlugin>
#ifdef Q_OS_WIN
#    include <shobjidl_core.h>
#    include <windows.h>
#    include <dbghelp.h>
#endif

#include <memory>

#ifdef CHATTERINO_WITH_AVIF_PLUGIN
Q_IMPORT_PLUGIN(QAVIFPlugin)
#endif

using namespace chatterino;

#ifdef Q_OS_WIN
#    pragma comment(lib, "Dbghelp.lib")

namespace {

LONG WINAPI writeDiagnosticMinidump(EXCEPTION_POINTERS *exceptionInfo)
{
    if (!exceptionInfo || !exceptionInfo->ExceptionRecord ||
        exceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    wchar_t modulePath[MAX_PATH] = {};
    const auto pathLength =
        GetModuleFileNameW(nullptr, modulePath, _countof(modulePath));
    if (pathLength == 0 || pathLength >= _countof(modulePath))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    for (wchar_t *it = modulePath + pathLength; it != modulePath; --it)
    {
        if (*it == L'\\' || *it == L'/')
        {
            *(it + 1) = L'\0';
            break;
        }
    }

    wcscat_s(modulePath, L"mergerino-crash.dmp");
    HANDLE file = CreateFileW(modulePath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION dumpException = {
        GetCurrentThreadId(),
        exceptionInfo,
        FALSE,
    };
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpWithDataSegs, &dumpException, nullptr, nullptr);
    CloseHandle(file);

    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace
#endif

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    AddVectoredExceptionHandler(1, writeDiagnosticMinidump);
#endif

    QApplication a(argc, argv);

    QCoreApplication::setApplicationName("mergerino");
    QCoreApplication::setApplicationVersion(CHATTERINO_VERSION);
    QCoreApplication::setOrganizationDomain("mergerino.app");
#ifdef Q_OS_WIN
    SetCurrentProcessExplicitAppUserModelID(
        Version::instance().appUserModelID().c_str());
#endif

    std::unique_ptr<Paths> paths;

    try
    {
        paths = std::make_unique<Paths>();
    }
    catch (std::runtime_error &error)
    {
        QMessageBox box;
        if (Modes::instance().isPortable)
        {
            auto errorMessage =
                error.what() +
                QStringLiteral(
                    "\n\nInfo: Portable mode requires the application to "
                    "be in a writeable location. If you don't want "
                    "portable mode reinstall the application.");
            std::cerr << errorMessage.toLocal8Bit().constData() << '\n';
            std::cerr.flush();
            box.setText(errorMessage);
        }
        else
        {
            box.setText(error.what());
        }
        box.exec();
        return 1;
    }
    ipc::initPaths(paths.get());

    const Args args(a, *paths);

#ifdef CHATTERINO_WITH_CRASHPAD
    const auto crashpadHandler = installCrashHandler(args, *paths);
#endif

    // run in gui mode or browser extension host mode
    if (args.shouldRunBrowserExtensionHost)
    {
#ifdef Q_OS_MACOS
        ::chatterinoSetMacOsActivationPolicyProhibited();
#endif
        runBrowserExtensionHost();
    }
    else if (args.printVersion)
    {
        attachToConsole();

        auto version = Version::instance();
        auto versionMessage =
            QString("%1 (commit %2%3)")
                .arg(version.fullVersion())
                .arg(version.commitHash())
                .arg(version.isNightly() ? ", " + version.dateOfBuild() : "");
        std::cout << versionMessage.toLocal8Bit().constData() << '\n';
        std::cout.flush();
    }
    else
    {
        if (args.verbose)
        {
            attachToConsole();
        }

        qCInfo(chatterinoApp).noquote()
            << "Mergerino Qt SSL library build version:"
            << QSslSocket::sslLibraryBuildVersionString();
        qCInfo(chatterinoApp).noquote()
            << "Mergerino Qt SSL library version:"
            << QSslSocket::sslLibraryVersionString();
        qCInfo(chatterinoApp).noquote()
            << "Mergerino Qt SSL active backend:"
            << QSslSocket::activeBackend() << "of"
            << QSslSocket::availableBackends().join(", ");
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        qCInfo(chatterinoApp) << "Mergerino Qt SSL active backend features:"
                              << QSslSocket::supportedFeatures();
#endif
        qCInfo(chatterinoApp) << "Mergerino Qt SSL active backend protocols:"
                              << QSslSocket::supportedProtocols();

        if (chatterino_import::hasPendingImport(*paths))
        {
            auto result = chatterino_import::applyPendingImport(*paths);
            if (!result)
            {
                QMessageBox::warning(
                    nullptr, "Chatterino import failed",
                    "Mergerino could not import Chatterino settings:\n\n" +
                        result.error());
            }
        }

        Settings settings(args, paths->settingsDirectory);

        Updates updates(*paths, settings);

        NetworkConfigurationProvider::applyFromEnv(Env::get());

        IvrApi::initialize();
        Helix::initialize();

        runGui(a, *paths, settings, args, updates);
    }
    return 0;
}
