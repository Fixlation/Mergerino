project(chatterino-test)

option(CHATTERINO_TEST_USE_PUBLIC_HTTPBIN "Use public httpbin for testing network requests" OFF)

set(test_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/resources/test-resources.qrc
    ${CMAKE_CURRENT_LIST_DIR}/src/Test.hpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ChannelChatters.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/AccessGuard.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ActivityMessageUtils.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/KickApi.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/NetworkCommon.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/NetworkRequest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/NetworkResult.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ChatterSet.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/HighlightPhrase.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Emojis.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ExponentialBackoff.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Helpers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/RatelimitBucket.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Hotkeys.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/UtilTwitch.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/IrcHelpers.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/TwitchPubSubClient.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/IrcMessageHandler.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/HighlightController.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/FormatTime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/LimitedQueue.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/BasicPubSub.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/SeventvEventAPI.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/BttvLiveUpdates.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Updates.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Filters.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/LinkParser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/InputCompletion.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/XDGDesktopFile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/XDGHelper.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/XDGDirectory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Selection.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/NotebookTab.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/SplitInput.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/LinkInfo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/MessageLayout.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/QMagicEnum.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ModerationAction.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Scrollbar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Commands.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/FlagsEnum.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/MessageLayoutContainer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/CancellationToken.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/Plugins.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/TwitchIrc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/IgnoreController.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/OnceFlag.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/IncognitoBrowser.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/EventSubMessages.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/WebSocketPool.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/NativeMessaging.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/ImageUploader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/TwitchChannel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/TwitchUserColor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/FunctionRef.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/InputHighlighter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/BalancedResolverResults.cpp

    ${CMAKE_CURRENT_LIST_DIR}/src/lib/Snapshot.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/lib/Snapshot.hpp
    # Add your new file above this line!
    )

add_executable(${PROJECT_NAME} ${test_SOURCES})

if(CHATTERINO_SANITIZER_SUPPORT)
    add_sanitizers(${PROJECT_NAME})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRARY_PROJECT})
target_link_libraries(${PROJECT_NAME} PRIVATE chatterino-mocks)

target_link_libraries(${PROJECT_NAME} PRIVATE gtest gmock)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    )

if(CHATTERINO_ENABLE_LTO)
    message(STATUS "Enabling LTO for ${PROJECT_NAME}")
    set_property(TARGET ${PROJECT_NAME}
        PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMAND_EXPAND_LISTS
    )

    # Conan's OpenSSL targets expose import libraries, so TARGET_RUNTIME_DLLS misses the runtime DLLs.
    set(CHATTERINO_TEST_OPENSSL_BIN_DIRS
        ${openssl_OpenSSL_SSL_BIN_DIRS_RELEASE}
        ${openssl_OpenSSL_Crypto_BIN_DIRS_RELEASE}
        ${openssl_BIN_DIRS_RELEASE})
    foreach(CHATTERINO_TEST_OPENSSL_LIBRARY IN ITEMS
            "${OPENSSL_SSL_LIBRARY}"
            "${OPENSSL_CRYPTO_LIBRARY}")
        if(CHATTERINO_TEST_OPENSSL_LIBRARY AND
                IS_ABSOLUTE "${CHATTERINO_TEST_OPENSSL_LIBRARY}")
            get_filename_component(CHATTERINO_TEST_OPENSSL_LIBRARY_DIR
                "${CHATTERINO_TEST_OPENSSL_LIBRARY}" DIRECTORY)
            get_filename_component(CHATTERINO_TEST_OPENSSL_PACKAGE_DIR
                "${CHATTERINO_TEST_OPENSSL_LIBRARY_DIR}" DIRECTORY)
            list(APPEND CHATTERINO_TEST_OPENSSL_BIN_DIRS
                "${CHATTERINO_TEST_OPENSSL_PACKAGE_DIR}/bin")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES CHATTERINO_TEST_OPENSSL_BIN_DIRS)

    set(CHATTERINO_TEST_OPENSSL_RUNTIME_DLLS)
    foreach(CHATTERINO_TEST_OPENSSL_BIN_DIR IN LISTS CHATTERINO_TEST_OPENSSL_BIN_DIRS)
        if(IS_DIRECTORY "${CHATTERINO_TEST_OPENSSL_BIN_DIR}")
            file(GLOB CHATTERINO_TEST_OPENSSL_DLLS
                "${CHATTERINO_TEST_OPENSSL_BIN_DIR}/libssl*.dll"
                "${CHATTERINO_TEST_OPENSSL_BIN_DIR}/libcrypto*.dll")
            list(APPEND CHATTERINO_TEST_OPENSSL_RUNTIME_DLLS
                ${CHATTERINO_TEST_OPENSSL_DLLS})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES CHATTERINO_TEST_OPENSSL_RUNTIME_DLLS)

    if(CHATTERINO_TEST_OPENSSL_RUNTIME_DLLS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CHATTERINO_TEST_OPENSSL_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:${PROJECT_NAME}>
        )
    endif()
endif()

if(CHATTERINO_TEST_USE_PUBLIC_HTTPBIN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CHATTERINO_TEST_USE_PUBLIC_HTTPBIN)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # The warnings are stateful, options from target_compile_options get placed
    # before the cxx flags. /W4 enables the initializer warnings.
    string(REGEX REPLACE "/W4\\b" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        -Wno-missing-designated-field-initializers
        -Wno-missing-field-initializers
    )
endif()

set_target_properties(${PROJECT_NAME}
    PROPERTIES
    AUTORCC ON
    )

if (CHATTERINO_STATIC_QT_BUILD)
    qt_import_plugins(${PROJECT_NAME} INCLUDE_BY_TYPE
        platforms Qt::QXcbIntegrationPlugin
        Qt::QMinimalIntegrationPlugin
    )
endif ()

gtest_discover_tests(${PROJECT_NAME} DISCOVERY_MODE PRE_TEST)
