<img src="plasmoid/icons/org.btd700ctl.dongle.svg" alt="plasma-btd700 icon" width="96" align="right">

# plasma-btd700

Unofficial KDE Plasma 6 applet and control daemon for the Sennheiser BTD 700 USB Bluetooth dongle on Linux.

The dongle is configured over USB HID, and the vendor only ships a control app for Windows and macOS. This project gives you the same controls in the Plasma panel or system tray.

This is a fork of [sobalap/btd700ctl](https://github.com/sobalap/btd700ctl), which did the protocol work and provides the driver library. See [Origin and changes](#origin-and-changes).

## Features

- Panel and tray icon that changes shape and colour with the link state
- Shows headphone state, active codec, sample rate and bit depth, audio mode, transport and firmware version
- Switch audio mode: high quality, gaming (low latency), broadcast
- Select the codec from the ones the dongle currently offers
- Connect headphones that are powered on but not linked to the dongle
- Switches the default audio sink to the dongle when the headphones connect and back to your previous sink when they disconnect
- Everything is also available on the session D-Bus, so scripts and other desktops can use it

Not available:

- Headphone battery level. The dongle does not report it over its HID protocol, and the headphones pair with the dongle, not with the host's Bluetooth stack. Reading it through the PC's own Bluetooth adapter was tried too: headphones linked to the dongle and a phone (multipoint) did not show up in a scan from the PC, so there was nothing to connect to.
- Firmware updates
- BTD 600 (it has no control protocol)

## How it works

```
Plasma applet  --D-Bus-->  btd700d  --USB HID-->  BTD 700
(plasmoid/)                (daemon/)   via libbtd700ctl (src/)
                              |
                              +-- pactl: default sink switching
```

`btd700d` is the only process that opens the dongle. Command responses and unsolicited events share one HID stream, so a second reader would steal responses. The applet never touches the device and talks to the daemon over D-Bus only.

## Requirements

- Linux with PipeWire and pipewire-pulse (`pactl` at runtime)
- hidapi (hidraw backend), libsystemd
- For the applet: Plasma 6, Qt 6, extra-cmake-modules
- A C99 and C++ compiler, cmake

```bash
# Arch
sudo pacman -S base-devel cmake hidapi libpulse extra-cmake-modules libplasma kirigami qt6-declarative
```

## Build and install

```bash
cmake -B build -S . -DBUILD_PLASMOID=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

Leave out `-DBUILD_PLASMOID=ON` to build only the library and the daemon, without any KDE dependencies.

Allow non-root access to the dongle:

```bash
sudo cp udev/99-btd700.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Start the daemon:

```bash
systemctl --user enable --now btd700d.service
```

Then add "BTD 700 Dongle" through the panel's "Add Widgets", or enable it under system tray settings, "Entries". If it is not listed right after installing, restart the shell once: `systemctl --user restart plasma-plasmashell`.

To try the applet without installing it:

```bash
./build/btd700d &
QML_IMPORT_PATH=$PWD/build/bin plasmoidviewer -a $PWD/plasmoid/package
```

## Sink switching

When the headphones connect, the daemon makes the dongle the default sink. When they disconnect, it moves the default to the fallback sink: the most recent default sink that was not the dongle. The fallback is stored by name in `$XDG_STATE_HOME/btd700d/fallback-sink` (default `~/.local/state/btd700d/`), so it survives restarts. If you picked another default yourself in the meantime, it is left alone and remembered as the new fallback.

The fallback is chosen in this order, and the choice is logged:

1. `BTD700_FALLBACK_SINK=<sink name>` in the daemon's environment, if that sink exists
2. the remembered sink, if it still exists
3. the available non-dongle sink with the lowest index

### Auto-reconnect

The dongle keeps powered-on headphones connected. If the link is dropped with `Disconnect()`, the dongle re-establishes it on its own a moment later, and no reliable way was found to hold the headphones disconnected from the host side. To move audio away from the dongle, switch the headphones off (the default sink then moves to the fallback) or pick another output device.

## D-Bus API

Session bus name `org.btd700ctl.Dongle`, object `/org/btd700ctl/Dongle`, interface `org.btd700ctl.Dongle1`. The name is claimed even when no dongle is plugged in (`Present` is false). Full interface: [daemon/org.btd700ctl.Dongle1.xml](daemon/org.btd700ctl.Dongle1.xml).

```bash
busctl --user introspect org.btd700ctl.Dongle /org/btd700ctl/Dongle
busctl --user call org.btd700ctl.Dongle /org/btd700ctl/Dongle org.btd700ctl.Dongle1 SetAudioMode s gaming
busctl --user call org.btd700ctl.Dongle /org/btd700ctl/Dongle org.btd700ctl.Dongle1 SetCodec s aptx-adaptive
busctl --user call org.btd700ctl.Dongle /org/btd700ctl/Dongle org.btd700ctl.Dongle1 Disconnect
busctl --user monitor org.btd700ctl.Dongle
```

| Property | Type | Values |
|---|---|---|
| `Present` | b | dongle is plugged in and open |
| `State` | s | `none`, `disconnected`, `connected`, `streaming-audio`, `streaming-voice` |
| `AudioMode` | s | `high-quality`, `gaming`, `broadcast`, `unknown` |
| `Transport` | s | `disconnected`, `classic`, `le-audio`, `multipoint`, `unknown` |
| `SupportedCodecs` | as | `sbc`, `aptx`, `aptx-adaptive`, `aptx-lossless`, `aptx-lite`, `lc3` |
| `ActiveCodecs` | as | same tokens |
| `SampleRate` | u | Hz, 0 if unknown |
| `BitDepth` | u | bits, 0 if unknown |
| `GamingAvailable` | b | unreliable, see below |
| `FirmwareVersion` | s | `major.minor.build` |

All properties are read-only and emit `PropertiesChanged`.

Methods: `SetAudioMode(s)`, `SetCodec(s)`, `Connect()`, `Disconnect()`, `Refresh()`. `Connect` and `Disconnect` are plain triggers; after `Disconnect` the dongle reconnects by itself. `SetAudioMode` keeps the current transport, so changing the mode does not drop you out of LE Audio or multipoint. Errors: `org.freedesktop.DBus.Error.InvalidArgs`, `org.btd700ctl.Error.NotPresent`, `org.btd700ctl.Error.Failed`.

## Notes

- The set of supported codecs depends on the audio mode. In gaming mode the dongle offers aptX Adaptive only.
- The "gaming available" query is not answered by the dongle, so `GamingAvailable` stays false. Setting gaming mode works regardless.
- Set `BTD700_DEBUG=1` in the daemon's environment to hex-dump every unsolicited dongle packet to stderr. Unrecognised event IDs are marked `UNKNOWN`.
- Tested with dongle firmware 3.11.0.

## Origin and changes

Forked from [sobalap/btd700ctl](https://github.com/sobalap/btd700ctl) at commit `747f3d6`. The driver library, the protocol definitions, the udev rules and the original sink switching daemon are sobalap's work.

Changes made in this fork (October 2026):

- `daemon/btd700d.c`: D-Bus API, reworked main loop, `pactl` based sink handling with a persisted fallback sink
- `src/btd700.c`: optional debug dump of unsolicited packets (`BTD700_DEBUG`)
- `CMakeLists.txt`: libsystemd dependency, `BUILD_PLASMOID` option
- new: `daemon/org.btd700ctl.Dongle1.xml`, `plasmoid/`
- `README.md` rewritten

The changes in this fork were written with AI assistance (Claude Code) and tested by the maintainer on real hardware.

Not affiliated with or endorsed by Sennheiser.

## License

GNU LGPL-2.1, same as upstream. See [LICENSE](LICENSE).
