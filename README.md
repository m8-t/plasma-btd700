<img src="plasmoid/icons/org.btd700ctl.dongle.svg" alt="plasma-btd700 icon" width="96" align="right">

# plasma-btd700

Unofficial KDE Plasma 6 applet and control daemon for the Sennheiser BTD 700 USB Bluetooth dongle on Linux.

The dongle is configured over USB HID, and the vendor only ships a control app for Windows and macOS. This project gives you the same controls in the Plasma panel or system tray.

This is a fork of [sobalap/btd700ctl](https://github.com/sobalap/btd700ctl), which did the protocol work and provides the driver library. See [Origin and changes](#origin-and-changes).

## Features

- Panel and tray icon that changes shape and colour with the link state
- Shows headphone state, active codec, sample rate and bit depth, audio mode, transport and firmware version
- Switch audio mode: high quality or gaming (low latency)
- Select the codec from the ones the dongle currently offers
- Connect headphones that are powered on but not linked to the dongle
- Headphone battery level when the headphones are switched on (best effort), read over Bluetooth LE through the PC's own adapter, without pairing and without taking one of the headphones' multipoint slots (see [Headphone battery](#headphone-battery))
- Switches the default audio sink to the dongle when the headphones connect and back to your previous sink when they disconnect
- Everything is also available on the session D-Bus, so scripts and other desktops can use it

Not available:

- Headphone settings such as noise cancellation (ANC). The vendor's phone app sends them straight to the headphones (Qualcomm GAIA), and the dongle has no pass-through. Over the PC's own adapter the headphones refuse these channels on an unpaired LE connection. An LE-only pairing was tested: it needs pairing mode, which drops the dongle and the phone, the GAIA requests still went unanswered, and the headphones rejected the PC's key after a power cycle. Not worth it.
- Firmware updates
- BTD 600 (it has no control protocol)

## How it works

```
Plasma applet  --D-Bus-->  btd700d  --USB HID-->  BTD 700  ==Bluetooth==>  headphones
(plasmoid/)                (daemon/)   via libbtd700ctl (src/)                 ^
                              |                                                |
                              +-- pactl: default sink switching                |
                              +-- PC Bluetooth adapter, LE: battery level -----+
```

`btd700d` is the only process that opens the dongle. Command responses and unsolicited events share one HID stream, so a second reader would steal responses. The applet never touches the device and talks to the daemon over D-Bus only.

## Requirements

- Linux with PipeWire and pipewire-pulse (`pactl` at runtime)
- hidapi (hidraw backend), libsystemd
- For the battery level: a Bluetooth adapter in the PC with LE enabled, and bluetoothd to find the headphones once
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

After an update, restart both parts, otherwise they keep running the old version. Package managers do not do this for user services:

```bash
systemctl --user restart btd700d.service
systemctl --user restart plasma-plasmashell
```

The daemon restart covers changes to btd700d, the shell restart loads the new applet. `journalctl --user -u btd700d` shows a new process ID after the restart.

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

## Headphone battery

The dongle does not report the headphones' battery, but the HDB 630 offers the standard Bluetooth Battery Service over LE, readable without pairing. btd700d reads it through the PC's own Bluetooth adapter.

Treat it as best effort. It relies on undocumented behaviour of the headphones, the value is a snapshot from when they were switched on, and it can be missing, for example when audio starts too early or the headphones briefly refuse the connection. How it works:

- When the headphones connect to the dongle, usually right after switching them on, it opens a short unpaired LE connection, reads the Battery Level characteristic and disconnects, typically within a second. No multipoint slot is used, the dongle and a phone stay connected.
- The HDB 630 stops advertising over LE as soon as audio plays through the dongle, and starts again only after it is switched off and on (tested for 20 minutes of silence, with and without the phone). So once the headphones' address is known, the default sink moves to the dongle only after that first reading, waiting at most 3 seconds for it. On the very first connection the address is not known yet: the sink switches right away and the scan follows, so play nothing at all then. Once audio has played btd700d does not try again until the headphones connect anew. The applet keeps showing the last value and adds its age once it is older than 10 minutes, for example "80% (2 hours ago)".
- While nothing has played and the reads succeed, it reads again every 5 minutes. After a failed attempt (the headphones did not answer, or a read failed three times) it waits for the next connection. `Refresh()` makes one attempt at the next moment the dongle is not streaming, which only succeeds if nothing played since the headphones were switched on.
- The headphones' LE address is found once through bluetoothd, by scanning for the Sennheiser (Sonova) service UUID `0xFCFE`, and stored in `$XDG_STATE_HOME/btd700d/headset`. With several Sennheiser devices around, the strongest signal wins. `BTD700_HEADSET_ADDRESS=AA:BB:CC:DD:EE:FF` skips the scan (append `/random` for a random static address). If the stored address gets no connection a dozen times in a row, for example with a new pair, btd700d scans again and keeps the address unless other Sennheiser headphones advertise.
- It can be turned off in the applet's context menu (right click, "Read Headphone Battery") or with `busctl --user call org.btd700ctl.Dongle /org/btd700ctl/Dongle org.btd700ctl.Dongle1 SetBatteryReading b false`. Then btd700d does no LE scans or connections at all. The choice is stored in `$XDG_STATE_HOME/btd700d/battery-reading`; it is on by default.
- `BTD700_BATTERY_INTERVAL=<seconds>` changes the 5 minute interval (60 seconds to 7 days).

If no battery level shows up:

- LE must be enabled on the PC adapter: `sudo btmgmt info` has to list `le` under current settings. `ControllerMode = bredr` in `/etc/bluetooth/main.conf` turns it off; use `dual`.
- Nothing may play to the dongle when the headphones connect. An app bound to the BTD 700 sink, or a WirePlumber rule that keeps the sink awake (`session.suspend-timeout-seconds = 0`, `node.pause-on-idle = false`, which makes it stream silence and also costs headphone battery), ends the advertising before the reading. The same happens when the headphones come back into range after playing: switch them off and on with nothing playing.
- Do not pair the headphones with the PC. A bonded LE link made them invisible to the PC after a power cycle in testing, and pairing mode drops the dongle and the phone.
- `connect: Function not implemented` in the log means the headphones were seen but did not accept the connection. btd700d tries again after a minute, up to two more times, as long as nothing plays.
- `journalctl --user -u btd700d` shows which headphones were found and why reads failed.

Tested with the HDB 630. Other Sennheiser headphones that advertise `0xFCFE` and the Battery Service may work too.

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
| `HeadsetBattery` | i | headphone battery in percent, -1 if unknown |
| `HeadsetBatteryUpdated` | t | unix time of the battery reading, 0 if none |
| `BatteryReading` | b | battery reading turned on, see `SetBatteryReading` |

All properties are read-only and emit `PropertiesChanged`.

Methods: `SetAudioMode(s)`, `SetCodec(s)`, `Connect()`, `Disconnect()`, `Refresh()`, `SetBatteryReading(b)`. `Connect` and `Disconnect` are plain triggers; after `Disconnect` the dongle reconnects by itself. `SetAudioMode` keeps the current transport, so changing the mode does not drop you out of LE Audio or multipoint. `Refresh` also tries one headphone battery read at the next moment the dongle is not streaming. `SetBatteryReading` stores the choice and works without a dongle. Errors: `org.freedesktop.DBus.Error.InvalidArgs`, `org.btd700ctl.Error.NotPresent`, `org.btd700ctl.Error.Failed`.

## Notes

- Broadcast (Auracast) mode is not offered in the applet, because switching to it drops the paired headphone link. It is still available with `SetAudioMode s broadcast` over D-Bus; broadcast name, key and quality are not exposed.
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
- new: `daemon/org.btd700ctl.Dongle1.xml`, `daemon/headset.c` (battery level over Bluetooth LE), `plasmoid/`
- `README.md` rewritten

The changes in this fork were written with AI assistance (Claude Code) and tested by the maintainer on real hardware.

Not affiliated with or endorsed by Sennheiser.

## License

GNU LGPL-2.1, same as upstream. See [LICENSE](LICENSE).
