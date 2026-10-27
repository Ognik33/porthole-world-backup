# World backup: a Porthole add-on

[Русский](README.ru.md)

Automatic backups of Minecraft Java worlds you host through [Porthole](https://store.steampowered.com/app/4963920).

> **Status: in development.** Version 0.0.1 is a test build. It starts the add-on's helper and
> checks the connection to it. Worlds are not backed up yet.

## Planned for 0.1.0

- Minecraft Java with the official launcher, MultiMC and Prism.
- The world is found on its own: from the port Minecraft opened to the world folder.
- A backup when you close the world. Optional "hot" backups every N minutes while it is open.
- One question the first time you close a world, then the add-on follows your answer.
- The last N backups of every world in one folder, `Documents\Porthole Backups` by default.
  Backups are ordinary zip files, the same format as the game's own "Make Backup" button.
- A tab with your worlds, their backups and the backup folder.

Later: friends in the lobby download a copy of the world and keep playing on their own PC.

## Install

1. Download `world-backup-<version>.paddon` from [Releases](https://github.com/Ognik33/porthole-world-backup/releases).
2. **Leave the lobby** (add-ons can't be changed while you're in one).
3. Porthole → Settings → Add-ons → "Install from file", or drag the file onto the window.
4. Enable it and start or join a lobby. The "World backup" tab shows whether the helper is running.

## Permissions and why

| Permission | Why |
|---|---|
| `ui` | the tab and notifications |
| `identity` | whether you are in a lobby, so the helper runs only during a session |
| `system` | see which programs listen on ports (`on_listening`), send commands to the add-on's own helper on `127.0.0.1` |
| `run_helper` + `natives` | run `world_backup.dll`: finding the world folder and making backups |

No administrator rights. The add-on doesn't go online. The helper's command port listens on
`127.0.0.1` only and accepts commands only with a random token the add-on knows. It reads another
program's command line only to find `--gameDir` and never writes it to the log: the official
launcher puts your login token there.

## How it works

- **Luau** (`src/*.luau`, bundled into one `main.luau` at build time) decides what to do: when to
  back up, what to ask, what to show.
- **world_backup** (`native/*.c`, one DLL) does everything that touches files and processes.
  One process per session; commands require a random token; events are text lines on stdout.
  See [docs/protocol.md](docs/protocol.md).

## Building and tests

Needs `mingw-w64`, `python3` (3.11+), `zip` (Linux or WSL), and [`luau`](https://github.com/luau-lang/luau/releases) for the Luau tests:

```
sudo apt install gcc-mingw-w64-x86-64 zip python3
./tools/test.sh          # texts, helper protocol, Luau unit tests and scenarios
./tools/build.sh         # -> dist/world-backup-<version>.paddon, dist/world_backup_cli.exe
./tools/check_repro.sh   # builds twice and compares hashes
```

The build is reproducible: the `.paddon` in Releases (built by GitHub Actions from this code) is
bit-for-bit identical to a local build with the same compiler.

## Compatibility and limits

- Tested on Porthole 3.0.2, add-on API version 1. SeStudio calls the API unstable, so a Porthole
  update may need a fix here.
- Windows only.
- Minecraft Java only.

## License

[MIT](LICENSE). Unofficial add-on, not affiliated with SeStudio or Mojang.
