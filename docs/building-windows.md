# Building on Windows

Mergerino's official builds are Windows-only. These instructions are for building a local Windows copy from source.

## Prerequisites

- Windows 10 or Windows 11, x64
- Visual Studio 2022 with `Desktop development with C++`
- Qt 6 for `msvc2022_64` with the `Qt Image Formats` module
- Python 3
- [Conan 2](https://conan.io/downloads.html)
- [CMake](https://cmake.org/)
- [Ninja](https://ninja-build.org/)
- [Git](https://git-scm.com/)

The GitHub release build currently uses Qt 6.9.3, Conan 2.11.0, Ninja, and Visual Studio 2022. Other close versions may work, but matching CI is the safest starting point.

Make sure your Qt `bin` directory is available on `PATH`, for example:

```cmd
set PATH=C:\Qt\6.9.3\msvc2022_64\bin;%PATH%
```

## Clone

Clone with submodules:

```cmd
git clone --recurse-submodules <your-mergerino-repository-url>
cd <repo-dir>
```

If you already cloned without submodules, run:

```cmd
git submodule update --init --recursive
```

Missing submodules usually show up as CMake errors for folders under `lib/`.

## Configure and build

Open an `x64 Native Tools Command Prompt for VS 2022` from the repository root and run:

```cmd
python -m pip install "conan==2.11.0"
conan profile detect --force
mkdir build-conan
cd build-conan
conan install .. -s build_type=Release -c tools.cmake.cmaketoolchain:generator=Ninja --build=missing --output-folder=. -o with_openssl3=True
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DUSE_PRECOMPILED_HEADERS=ON -DCHATTERINO_LTO=OFF -DCHATTERINO_SPELLCHECK=ON ..
cmake --build . --parallel
```

## Run

```cmd
build-conan\bin\mergerino.exe
```

If your command prompt is still inside `build-conan`, run:

```cmd
bin\mergerino.exe
```

## Useful build options

Pass these to the `cmake -G Ninja ...` configure command when needed:

- `-DCHATTERINO_PLUGINS=ON` enables plugin support. This is currently on by default.
- `-DCHATTERINO_PLUGINS=OFF` is useful when isolating plugin dependency issues.
- `-DCHATTERINO_SPELLCHECK=ON` enables Hunspell spellcheck support. This is used by release builds.
- `-DCHATTERINO_SPELLCHECK=OFF` is useful for a smaller troubleshooting build.
- `-DBUILD_TESTS=ON -DBUILD_APP=OFF` builds the test target instead of the app.
- `-DBUILD_WITH_QTKEYCHAIN=ON` keeps system keychain support enabled. This is currently on by default.

## YouTube OAuth in self-builds

Official release builds generate `src/providers/youtube/YouTubeOAuthSecrets.local.hpp` from a private GitHub Actions secret. That file is intentionally ignored by Git and is not distributed in source form.

Without that generated header, Mergerino reads the YouTube OAuth client secret from the `MRAB` environment variable at runtime:

```cmd
set MRAB=<your-oauth-client-secret>
build-conan\bin\mergerino.exe
```

The checked-in source currently contains the release OAuth client ID in `src/providers/youtube/YouTubeCommon.hpp`, but not the matching private client secret. If you want to test YouTube sign-in, sending, or moderation in a self-build without the release secret, you need to use your own Google OAuth client credentials and adjust the local source/client credential setup accordingly. Do not commit OAuth secrets or generated local secret headers.

Anonymous YouTube chat viewing may still work without OAuth credentials, but authenticated YouTube features should be expected to fail until credentials are configured.

## Deploy Qt libraries

If you need a standalone local bundle, run:

```cmd
windeployqt build-conan\bin\mergerino.exe --release --no-compiler-runtime --no-translations --no-opengl-sw --dir build-conan\bin
```

For a release-style package, the GitHub workflow also copies dependency DLLs from the build `bin` folder and deploys the Visual C++ runtime before zipping the result.

## Troubleshooting

- `windeployqt` or Qt tools are not found: add your Qt `bin` directory to `PATH`.
- `Could not find Qt6`: verify that the installed Qt kit matches `msvc2022_64`, or pass `-DCMAKE_PREFIX_PATH=C:\Qt\6.9.3\msvc2022_64` to CMake.
- Missing `lib/.../CMakeLists.txt`: run `git submodule update --init --recursive`.
- Plugin-related configure errors: first verify submodules are present, then try `-DCHATTERINO_PLUGINS=OFF` to isolate the base app build.
- YouTube login fails in a self-build: configure OAuth credentials as described above; the private release secret is not included in public source builds.
