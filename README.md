# Mergerino

Mergerino is a desktop chat client that combines a fast multi-channel workflow with support for Twitch, Kick, YouTube, and TikTok LIVE in one app. The goal is to keep chat fast and practical while making multi-platform viewing easier from a single desktop client.

This repository keeps the source here and ships installable Windows builds through GitHub Releases.

Current version: Mergerino 1.3.3

[![Download Windows Build](https://img.shields.io/badge/Download-Windows%20Build-2ea44f?style=for-the-badge)](https://github.com/Fixlation/Mergerino/releases/latest/download/Mergerino.zip)
[![Discord](https://img.shields.io/badge/Discord-Join%20Server-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/tAjdrbvxwT)

## What Is Mergerino?

- Merged desktop chat for Twitch, Kick, YouTube, and TikTok LIVE
- Fast multi-channel chat workflow in a standalone app
- 7TV, BTTV, and FFZ integration
- Windows-only builds packaged around `mergerino.exe`

## Download

If you just want to install the app, do not download the source code zip from the repository page.

Use the latest GitHub release instead:

- Open [the releases page](https://github.com/Fixlation/Mergerino/releases)
- Download [Mergerino.zip](https://github.com/Fixlation/Mergerino/releases/latest/download/Mergerino.zip)
- Extract it somewhere you want to keep it
- Open `mergerino.exe` from inside the extracted folder

## Install

### Windows

1. Download `Mergerino.zip`
2. Extract the zip
3. Open the extracted `Mergerino` folder
4. Run `mergerino.exe`
5. Keep the `.exe` inside that folder with the bundled files

## Build Your Own

Mergerino's official builds and release pipeline currently support Windows only.

For the full Windows self-build guide, see [docs/building-windows.md](docs/building-windows.md).

For the experimental community Linux path, see [docs/building-linux.md](docs/building-linux.md). It is a source-build guide, not an officially supported Linux release.

Clone the repository:

```shell
git clone <your-mergerino-repository-url>
```

The dependency sources currently live directly in the repository, so no separate submodule initialization step is required.

### Unsupported platforms

Linux and macOS builds are not officially supported by this repository right now, and the GitHub workflows only build and package Windows releases. Linux configuration is available only through the explicit unsupported-build option documented in the [community Linux guide](docs/building-linux.md). macOS does not currently have an equivalent guide. Forks and local experiments are welcome, but this project does not guarantee compatibility or support for unofficial non-Windows builds.

### YouTube OAuth in self-builds

Official release builds inject a private YouTube OAuth client secret during CI. That secret is not committed to the repository. Self-builds that need YouTube sign-in, sending, or moderation must provide their own OAuth credential setup; see the [Windows](docs/building-windows.md#youtube-oauth-in-self-builds) or [Linux](docs/building-linux.md#youtube-oauth-sign-in-sending-and-moderation) guide for details.

## Notes

- This is an unofficial fork of [Chatterino](https://github.com/Chatterino/chatterino2).
- Mergerino is intended to provide a merged multi-service chat client and Windows distribution path, not to represent the upstream project.
- GitHub workflows and release packaging in this repo are Windows-only.
- [Privacy Policy](https://fixlation.github.io/Mergerino/privacy/)
- [Terms of Service](https://fixlation.github.io/Mergerino/terms/)
