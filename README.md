# CapView

Play your Switch (or any console) on your PC through a USB capture card: plug in,
open, play. A simple, low-latency replacement for fiddling with OBS.

## Download

1. Grab the latest **`CapView-x.x.x.zip`** from [Releases](https://github.com/VashuChogada/CapView/releases/latest).
2. Unzip and run **`CapView.exe`**. No install needed.
3. If Windows says *"Windows protected your PC"*, click **More info → Run anyway**.

Needs Windows 10 or 11 (64-bit).

## Use

Move the mouse to show the controls. Hover any button to see what it does.

| | |
|---|---|
| Fullscreen | Double-click |
| Volume | Scroll wheel |
| Mute | `M` |
| Screenshot | `S` (saved to Pictures › CapView) |
| Settings | Right-click |

Settings let you pick the video/audio source, quality, and scaling.

## Build it yourself

Install Visual Studio with the **Desktop development with C++** workload, then run `build.bat`.

To publish a new version: bump `src/version.h`, commit, then push a matching tag
(`git tag v0.3.0 && git push origin v0.3.0`). GitHub builds it and creates the release.

## License

[MIT](LICENSE): free to use, modify and share.
