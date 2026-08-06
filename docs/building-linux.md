# Experimental Linux self-build

This guide explains how to build and run a personal copy of Mergerino on Linux.

> **Status:** Linux is a community, experimental build target. Mergerino does not publish Linux binaries, run Linux CI, or promise Linux support. A community member has reported building and running the app successfully, but the maintainer cannot currently reproduce or test Linux releases. Expect some features to be incomplete.

The copy-and-paste package commands below target **Debian 13 (Trixie), 64-bit** with the distribution's dynamic Qt 6 packages. Other current Linux distributions may work, but package names differ. macOS is not covered by this guide.

## What works differently on Linux

The build is only one part of the setup:

- Twitch viewing and the normal Twitch login use the app's public client ID and do not need a private maintainer secret.
- Kick viewing is anonymous. Sending and moderation require a Kick developer app belonging to the user.
- YouTube viewing can be anonymous. Sign-in, sending, and moderation require a Google OAuth client ID and matching secret belonging to the builder.
- TikTok LIVE chat needs a locally installed Chromium-family browser.
- Public 7TV, BTTV, and FFZ emotes do not need private credentials. The current 7TV account-linking flow is not reliable on Linux.
- The in-app updater is Windows-only. Linux builders must update and rebuild from source.

The [service and module matrix](#service-and-module-matrix) has the full breakdown.

## 1. Install the build dependencies

Open a terminal and run:

```bash
sudo apt update
sudo apt install \
    build-essential \
    ca-certificates \
    cmake \
    git \
    ninja-build \
    pkg-config \
    qt6-base-dev \
    qt6-base-private-dev \
    qt6-svg-dev \
    qt6-image-formats-plugins \
    libboost-dev \
    libnotify-dev \
    libsecret-1-dev \
    libssl-dev
```

Why the less-obvious packages are needed:

- `qt6-base-private-dev` supplies private Qt Network and Widgets headers used by Mergerino. It must match the installed Qt libraries.
- `qt6-svg-dev` supplies both the Qt SVG and SVG Widgets modules.
- `libnotify-dev` enables Linux desktop notifications.
- `libsecret-1-dev` is required by the bundled QtKeychain Linux backend.
- `qt6-image-formats-plugins` adds useful image decoders at runtime.

Mergerino uses C++23. Debian 13's normal compiler and Qt 6.8 packages are a sensible baseline. Very old distributions and mixed Qt package versions are likely to fail.

Optional feature and runtime packages:

```bash
# Native Qt Wayland support, if the desktop uses Wayland
sudo apt install qt6-wayland

# Used only by Mergerino's "Open in streamlink" feature
sudo apt install streamlink

# Build-time dependency used only when spellcheck is enabled
sudo apt install libhunspell-dev
```

Only install the optional packages for features you intend to use. To build
spellcheck support, install Hunspell before configuring and change the configure
option to `-DCHATTERINO_SPELLCHECK=ON`.

### Other distributions

Find the equivalent development packages for:

- a C++23 compiler, CMake, Ninja, Git, and `pkg-config`
- Qt 6 Core, Widgets, GUI, Network, Concurrent, DBus, SVG, and SVG Widgets
- the private development headers for the exact same Qt 6 version
- Boost, OpenSSL, libnotify, and libsecret

Use a dynamic, distribution-provided Qt build for the first attempt. The inherited static-Qt path does not provide a complete native Wayland plugin setup.

## 2. Get the source

For a fresh checkout:

```bash
git clone https://github.com/Fixlation/Mergerino.git
cd Mergerino
```

The dependency sources currently live directly in the repository as normal
tracked files, so a normal clone is complete and no separate submodule
initialization step is required. Historical `.gitmodules` metadata is still
present, but `git submodule update` is not part of this build. Missing or
incomplete vendored dependencies normally appear as CMake errors mentioning a
path under `lib/`.

## 3. Configure

From the repository root:

```bash
cmake -S . -B build-linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMERGERINO_ALLOW_UNSUPPORTED_LINUX=ON \
    -DUSE_PRECOMPILED_HEADERS=OFF \
    -DCHATTERINO_NO_AVIF_PLUGIN=ON \
    -DCHATTERINO_SPELLCHECK=OFF \
    -DSKIP_JSON_GENERATION=ON
```

The important options are:

- `MERGERINO_ALLOW_UNSUPPORTED_LINUX=ON` deliberately opts in to the unsupported community build.
- `USE_PRECOMPILED_HEADERS=OFF` avoids a common compiler-specific source of first-build problems.
- `CHATTERINO_NO_AVIF_PLUGIN=ON` skips the bundled AVIF plugin rather than making libavif another build dependency. 7TV falls back to WebP when no AVIF decoder is available.
- `CHATTERINO_SPELLCHECK=OFF` keeps Hunspell optional.
- `SKIP_JSON_GENERATION=ON` uses the checked-in Twitch EventSub generated sources instead of creating a Python environment during configuration.

Plugin support, QtKeychain, and libnotify remain enabled by default.

## 4. Build

```bash
cmake --build build-linux --parallel
```

The executable should be created at:

```text
build-linux/bin/mergerino
```

If the compiler is killed because the machine runs out of memory, retry with fewer simultaneous jobs:

```bash
cmake --build build-linux --parallel 2
```

## 5. Run

```bash
./build-linux/bin/mergerino
```

No YouTube secret is needed for anonymous YouTube chat viewing. Only use the special launch procedure in the [YouTube OAuth section](#youtube-oauth-sign-in-sending-and-moderation) when testing authenticated YouTube features.

On a typical Linux desktop, Qt stores Mergerino data below:

```text
$XDG_DATA_HOME/mergerino
```

If `XDG_DATA_HOME` is not set, that normally resolves to:

```text
~/.local/share/mergerino
```

By default, settings, account tokens, logs, and cache live below that root. A custom cache path or a `modes` file beside the executable can override some or all of these locations.

## Optional local install

Running directly from `build-linux/bin` is enough. To install the executable and icon for only the current user:

```bash
cmake --install build-linux --prefix "$HOME/.local"
mkdir -p "$HOME/.local/bin"
cp -a build-linux/bin/kick_onboarding "$HOME/.local/bin/"
"$HOME/.local/bin/mergerino"
```

The extra copy keeps the Kick account-wizard images beside the installed
executable, where Mergerino loads them. The current CMake rule installs another
copy at `$HOME/.local/kick_onboarding`; that copy is harmless.

This is not a portable application bundle. It continues to depend on the Qt
and other system libraries installed in step 1. The current CMake install rules
install the app icon but do not create a `.desktop` application-menu entry.

## Service and module matrix

| Feature | Expected community-build status | Setup or limitation |
| --- | --- | --- |
| Twitch anonymous viewing | Expected to work | No private secret is required. |
| Twitch login, sending, and normal moderation | Expected to work | Uses a public client ID and a loopback callback at `http://localhost:38276`. Keep that port free during login. |
| Kick anonymous viewing and emotes | Expected to work | No Kick developer credentials are required for viewing. |
| Kick login, sending, and moderation | Builder setup required | Create a Kick developer app, then enter its client ID and secret in Mergerino's Kick account wizard. Use `http://localhost:38275` as the redirect URL. |
| YouTube anonymous chat viewing | Expected to work | No OAuth secret is required. |
| YouTube login, sending, and moderation | Builder setup required | Requires a matching Google OAuth client ID and secret. Follow the next section. |
| TikTok LIVE chat | Expected to work with an extra runtime dependency | Install Google Chrome, Brave, Chromium, or Microsoft Edge so Mergerino can launch a local Chromium process. No API key is required. |
| 7TV public emotes and updates | Expected to work | No private secret is required. |
| 7TV account linking and cosmetic management | Known Linux limitation | The current browser-session discovery uses Windows browser-profile locations. On Linux, the sign-in window can open but token discovery will normally time out. |
| BTTV and FFZ emotes | Expected to work | Their public APIs do not need private credentials. |
| Desktop notifications | Expected to work with desktop-specific differences | Requires `libnotify-dev` at build time and a normal notification service at runtime. Shell association, icon, and action presentation may vary because the current install creates no matching `.desktop` entry. |
| QtKeychain-backed generic credentials | Desktop-dependent | Requires a working Secret Service or KWallet session. Provider OAuth tokens described below are still stored in Mergerino's settings file. |
| Browser connection | Partial and unverified | Watching-channel synchronization requires a separately obtained compatible extension and its ID in Settings. Automatic manifest setup only targets Firefox and Google Chrome's standard Linux paths. Attaching/detaching Twitch chat to a browser window is Windows-only. |
| Automatic streamer-mode detection | Not implemented on Linux | Manual streamer mode remains available, but automatic OBS/process detection is Windows-only. |
| In-app Chatterino import | Unavailable on Linux | The importer has a Windows-only source path and no manual source selector. Manual file migration is outside the importer. |
| In-app updating | Do not use | It downloads the Windows release archive and invokes a Windows PowerShell updater. Update the source build manually instead. |

"Expected to work" means no obvious Linux blocker was found in the source audit. It is not a support guarantee or a substitute for testing on a real Linux system.

## Kick authenticated setup

Anonymous Kick chat does not need this setup. To send messages or use authenticated moderation:

1. Open **Settings → Accounts** in Mergerino and start the Kick login/account wizard.
2. Open [Kick's developer settings](https://kick.com/settings/developer).
3. Create an application using this exact redirect URL:

   ```text
   http://localhost:38275
   ```

4. Enter that application's client ID and client secret in the Mergerino wizard.
5. Complete the browser authorization.

Port `38275` must be free while the loopback callback is running.

## YouTube OAuth: sign-in, sending, and moderation

The public repository intentionally does not contain the private secret used by official builds. A self-builder needs a Google OAuth **Desktop app** belonging to them.

### Create the Google credentials

1. Create or select a project in [Google Cloud Console](https://console.cloud.google.com/).
2. Enable [YouTube Data API v3](https://developers.google.com/youtube/v3/getting-started).
3. Configure the OAuth consent screen. If the app remains in testing, add the Google account you will use as a test user.
4. Create an OAuth client with application type **Desktop app**.
5. Record the client ID and client secret; keep the client secret private.

Mergerino uses the installed-app loopback flow described in [Google's OAuth guide](https://developers.google.com/youtube/v3/guides/auth/installed-apps), with this local callback:

```text
http://127.0.0.1:38277
```

Port `38277` must be free during login.

### Put the matching client ID in your local source

Open:

```text
src/providers/youtube/YouTubeCommon.hpp
```

In `youTubeOAuthClientID()`, replace the existing string with your own Desktop app client ID. A client ID is public application identity, not the client secret.

Rebuild after changing it:

```bash
cmake --build build-linux --parallel
```

There is currently no CMake option or environment variable for overriding the client ID.

### Supply the secret only when launching

Mergerino reads the matching client secret from the `MRAB` environment variable when the private release header is absent. Prompt for it without placing it in shell history:

```bash
read -rsp 'YouTube OAuth client secret: ' MRAB
printf '\n'
MRAB="$MRAB" ./build-linux/bin/mergerino
unset MRAB
```

Keep that terminal open until Mergerino exits.

Important:

- A `.env` file by itself does nothing; Mergerino has no dotenv loader.
- Do not put the secret in source code, a CMake option/cache, a shell startup file, a desktop launcher, or workflow YAML.
- Do not commit `src/providers/youtube/YouTubeOAuthSecrets.local.hpp`.
- A secret from your Google app will not work with the repository's existing client ID. The ID and secret must belong to the same OAuth client.
- Without this setup, anonymous YouTube viewing can still work; YouTube sign-in, sending, and moderation will fail.

## Credential and profile safety

Treat the Linux Mergerino data directory as private.

Twitch and YouTube OAuth tokens, the Kick client ID, client secret and tokens, and the 7TV session token are currently saved in:

```text
Settings/settings.json
```

Mergerino also keeps settings backups. These provider tokens are not protected by QtKeychain, even when QtKeychain support is enabled. Never post or attach:

- `settings.json` or its backup files
- the Mergerino profile/data directory
- the complete `build-linux` tree if it contains locally generated credential
  files or copied diagnostics
- OAuth token responses
- logs or screenshots containing tokens
- proxy URLs containing a username or password

If QtKeychain configuration itself prevents a build, this troubleshooting option exists:

```text
-DBUILD_WITH_QTKEYCHAIN=OFF
```

Use it only if necessary. The generic credentials subsystem then falls back to an unencrypted `Settings/credentials.json` file.

## Important updater warning

Dismiss any update prompt and do not click **Install Update** or **Settings → Check for updates** in a Linux build.

The current updater only understands the official Windows `Mergerino.zip`. Its install path writes a PowerShell script and expects Windows executables. The `CHATTERINO_UPDATER=OFF` CMake option currently defines an unused macro and disables none of the updater behavior. Mergerino still performs its startup check; dismiss the result and never run **Install Update**.

Update a source build manually instead.

## Updating the source build

From the repository root:

```bash
git pull --ff-only
cmake -S . -B build-linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMERGERINO_ALLOW_UNSUPPORTED_LINUX=ON \
    -DUSE_PRECOMPILED_HEADERS=OFF \
    -DCHATTERINO_NO_AVIF_PLUGIN=ON \
    -DCHATTERINO_SPELLCHECK=OFF \
    -DSKIP_JSON_GENERATION=ON
cmake --build build-linux --parallel
```

If `git pull --ff-only` refuses because you changed the source, preserve your work and resolve the branch difference normally. Do not reset or delete the checkout just to update it. Builders who changed the YouTube client ID need to keep or reapply that local change.

## Troubleshooting

### CMake still says Windows is the only supported platform

Make sure the configure command contains:

```text
-DMERGERINO_ALLOW_UNSUPPORTED_LINUX=ON
```

If an older checkout does not recognize the option, update the source first.

### CMake cannot find Qt6, WidgetsPrivate, or NetworkPrivate

Install both matching packages:

```bash
sudo apt install qt6-base-dev qt6-base-private-dev
```

Do not mix private headers from one Qt version with libraries from another.

### CMake cannot find Qt6Svg or Qt6SvgWidgets

```bash
sudo apt install qt6-svg-dev
```

### CMake cannot find libsecret

```bash
sudo apt install libsecret-1-dev
```

Alternatively, reconfigure with `-DBUILD_WITH_QTKEYCHAIN=OFF` and read the plaintext-storage warning above.

### CMake cannot find libnotify

Install it:

```bash
sudo apt install libnotify-dev
```

Or build without desktop notifications:

```text
-DBUILD_WITH_LIBNOTIFY=OFF
```

### CMake reports missing files below `lib/`

The dependency folders are vendored directly in this repository; `git submodule update` will not restore them. Check `git status --short`, preserve any local work, and restore the missing files from the matching Git commit or make a fresh clone in a separate folder. Also check whether security or cleanup software removed the files.

### Plugin support is the failing part

To isolate the base app, add:

```text
-DCHATTERINO_PLUGINS=OFF
```

This disables Mergerino's alpha plugin support; it does not disable Twitch, Kick, YouTube, TikTok, or emote providers.

### TikTok says no supported browser was found

Install a Chromium-family browser and make sure one of these commands is available on `PATH`:

```text
google-chrome
brave-browser
chromium
chromium-browser
microsoft-edge
```

### OAuth login never returns to Mergerino

Check that the matching local port is free and that a firewall is not blocking loopback traffic:

- Kick: `localhost:38275`
- Twitch: `localhost:38276`
- YouTube: `127.0.0.1:38277`

### The app fails on Wayland

Install the distribution's Qt 6 Wayland package:

```bash
sudo apt install qt6-wayland
```

For diagnosis, try the X11/XWayland backend:

```bash
QT_QPA_PLATFORM=xcb ./build-linux/bin/mergerino
```

### Reporting a useful Linux result

Include:

- distribution and release
- CPU architecture
- compiler, CMake, and Qt versions
- the exact configure command
- the first complete configure or compiler error

Useful version commands are:

```bash
cat /etc/os-release
uname -m
g++ --version
cmake --version
qtpaths6 --qt-version
```

Remove usernames, home-directory details, tokens, proxy credentials, and private settings before posting logs.

## GitHub Actions and forks

The repository's current build and test jobs are Windows-only. Its Ubuntu
release job only republishes the Windows build artifact; it does not compile or
test Mergerino on Linux. This guide does not add a Linux workflow or produce a
Linux release artifact.

Forks do not inherit the private credentials used by the official Windows
workflow. Local Linux builds do not need GitHub Actions, and anonymous YouTube
viewing does not need a private credential. If you later create your own
workflow for authenticated features, store its values in encrypted Actions
secrets and never write them directly into workflow YAML.
