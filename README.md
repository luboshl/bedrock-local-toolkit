# Bedrock Local Toolkit

A local add-on for Minecraft Bedrock on Windows. It consists of a C# launcher and a custom C++ game module that adds local controls and rendering features. It is not a replacement game client.

This is an unofficial community project. It is not affiliated with, endorsed by, or sponsored by Mojang or Microsoft.

Use this software at your own risk. It is provided "as is", without warranties of any kind. To the maximum extent permitted by applicable law, the authors and contributors are not liable for damage or loss arising from its use.

## Basic project principles

- **Local-only changes:** No server or world changes.
- **No external client or updater:** No automatic updates.
- **No hidden network activity:** No telemetry or runtime downloads.
- **Auditable source:** Built from source; MinHook source and license included.
- **Verified changes:** Exact game build and targets checked before modification.

## Features

| Feature | Key (default) | Details and verification |
| --- | --- | --- |
| Zoom | C (hold), mouse wheel | Smooth local FOV transition; the wheel adjusts the target FOV. FOV and sensitivity are changed only after their targets are uniquely verified. The latest mouse input path has not yet been verified in-game. |
| Always day | F6 | Changes the appearance of profiled local rendering paths; does not change world time or simulation. Basic brightening has been verified. The sun, stars, clouds, and disable-time restoration still need in-game visual verification. |
| Nametag | F7 | Displays a custom name in third-person views. Verified in a local game. |
| Full Bright | F10 | Temporarily changes parameters of the standard local light texture; it does not change gamma, player state, or saved lighting. Confirmed working; other rendering modes and interaction with Always day have not been verified. |

### Utility controls

| Control | Key (default) | Behavior |
| --- | --- | --- |
| Status indicator | F8 | Shows or hides the local status indicator. |
| Safe detach | F9 | Restores modified values and hooks, then detaches the module when restoration is safe. |

Zoom and keyboard shortcuts are configurable in [`config/zoom.ini`](config/zoom.ini). Shortcuts must be distinct. The build script copies the configuration next to the launcher.

## Compatibility

- **Operating system:** Windows, x64.
- **Supported game build:** `Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe` only.
- **Game mode:** Local single-player worlds only. Multiplayer is unsupported, and the add-on cannot reliably detect the current game mode.

The launcher checks the game build and rejects unsupported versions. Do not assume another game version is compatible.

## Install and run

To use a prebuilt package, download a `win-x64` ZIP from [GitHub Releases](https://github.com/luboshl/bedrock-local-toolkit/releases). If no release is published yet, follow the source build steps below.

1. Download the `win-x64` ZIP for the release and extract its contents into a folder.
2. Install the .NET 10 runtime if it is not already installed.
3. Start Minecraft Bedrock, then run `Launcher.exe` from the extracted folder.

Alternatively, build the project locally:

1. Install the [build requirements](#build-from-source).
2. Run the build and test command below.
3. Start Minecraft Bedrock.
4. Run `artifacts\launcher\Launcher.exe`.

The launcher rejects game builds other than the one listed above. Do not use the add-on in multiplayer or on a world you are not willing to use with unsupported software.

## Build from source

Requirements:

- Windows x64
- x64 MSVC and the Windows SDK
- .NET 10 SDK

From the repository root, run:

```powershell
.\build.ps1 -Test
```

This builds the C++ game module and C# launcher, then runs the isolated automated tests. Tests cover implementation details against synthetic memory; passing tests do not prove that behavior works in a live game. See [`SPECIFICATION.md`](SPECIFICATION.md) for the security requirements and verification boundaries.

## Publish a release

Releases are built by GitHub Actions when a version tag is pushed. From a clean checkout of the changes intended for release, create and push a version tag:

```powershell
git tag -a v1.0.0 -m "v1.0.0"
git push origin v1.0.0
```

The workflow builds and tests the project, then creates a draft GitHub Release with generated notes and a `win-x64` ZIP containing the launcher and its required files. Review the draft and its asset on GitHub, then publish it when ready. The ZIP contains the framework-dependent launcher, which requires the .NET 10 runtime.

## Safety and limitations

- Use this project only in a local single-player world. Multiplayer is unsupported.
- Features affect local controls or rendering. The add-on does not intentionally modify the server protocol, world time, saved world, player attributes, or persistent game objects.
- The project targets one exact game build. A game update may make it incompatible; do not bypass the launcher's version check.
- Some features or rendering combinations still need in-game verification. The feature table distinguishes verified behavior from outstanding checks.
- Safe detach may defer restoration and detachment if another party has changed the relevant code or a callback is still running. Do not force-unload the module in that state.

## Troubleshooting

- **The launcher rejects the game:** Check that the installed package is exactly `Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe`. Other builds are not supported.
- **A shortcut does not work:** Check [`config/zoom.ini`](config/zoom.ini) and make sure every configured shortcut is distinct.
- **A feature behaves differently than expected:** Check its verification status in [Features](#features); some behavior has not yet been tested in-game.
- **The build fails:** Confirm that the x64 MSVC tools, Windows SDK, and .NET 10 SDK are installed, then review the build output.

## Contributing

Before starting substantial work, open an [issue](https://github.com/luboshl/bedrock-local-toolkit/issues) to discuss the proposed change. Changes to game behavior or security mechanisms must follow [`SPECIFICATION.md`](SPECIFICATION.md).

## Support and bug reports

Please use the [GitHub issue tracker](https://github.com/luboshl/bedrock-local-toolkit/issues) for questions and bug reports. Include the supported game build, steps to reproduce, expected and actual behavior, and relevant build or test output. Do not include personal data or sensitive files.

## Third-party components

The repository includes MinHook source under [`GameMod/vendor/minhook/`](GameMod/vendor/minhook/). See its included [`LICENSE.txt`](GameMod/vendor/minhook/LICENSE.txt) for the third-party license. The release ZIP includes the project and MinHook license texts in its `licenses/` folder.

## Project documentation

- [`AGENTS.md`](AGENTS.md) — repository guidelines for development.
- [`SPECIFICATION.md`](SPECIFICATION.md) — project scope, security requirements, and verification limits.
- [`FINDINGS.md`](FINDINGS.md) — technical findings and design rationale.
- [`diagnostics/`](diagnostics/) — read-only diagnostic tools for the supported game build.

## License

Original code in this repository is licensed under the [MIT License](LICENSE). This license does not cover Minecraft or grant rights to Minecraft's code, assets, or trademarks. Third-party components retain their own licenses; see [`GameMod/vendor/minhook/LICENSE.txt`](GameMod/vendor/minhook/LICENSE.txt).
