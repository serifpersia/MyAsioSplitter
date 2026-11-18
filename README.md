# MyASIO Splitter

A 64-bit Windows ASIO driver that lets a second application capture the audio
stream of the application that owns the interface, without a virtual cable.

One DLL registers two ASIO devices:

- **MyASIO Splitter**: open this in the application that should own the
  hardware (for example Pianoteq). It forwards every call to the real ASIO
  driver selected in its control panel and publishes the audio to a lock-free
  shared-memory ring.
- **MyASIO Splitter (Tap)**: open this in the capturing application (for
  example OBS through obs-asio). It exposes the same two input channels, paced
  from the master's clock and sample rate.

Only the master touches the hardware. Never select the non-Tap device in the
second application: two processes cannot own the same interface.

## Requirements

- Windows 10/11 x64
- A real 64-bit ASIO driver for the hardware
- The same sample rate in both applications (48 kHz recommended)
- Windows power plan set to **High performance**; the balanced plan can cause
  crackles even with native ASIO drivers on USB interfaces

## Install

Download the latest release zip, extract it, and run `install.bat` as
administrator. The script copies the DLL to `%ProgramFiles%\MyAsioSplitter`
and registers both devices. Running it again offers reinstall or uninstall.
Building from source uses the same script after `cmake --build`.

## Usage

1. Close both applications.
2. Start the master application, select **MyASIO Splitter**, and choose the
   buffer size. Shift-click the driver's Control Panel button to change which
   hardware driver is used.
3. Start the capturing application and select **MyASIO Splitter (Tap)**.
4. If the Tap is silent, run `MyAsioSplitterShmDump.exe` while both
   applications are open. It shows whether the master is publishing and whether
   the tap is consuming.

## Build

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Or run `build.bat`.

Outputs under `build\Release`:

- `MyAsioSplitter.dll`: the driver
- `MyAsioSplitterSmokeTest.exe`: COM/vtable/ring self-test
- `MyAsioSplitterShmDump.exe`: read-only shared-memory inspector

## Notes

- The tap drops frames instead of blocking the master.
- The selected hardware driver is stored in
  `%APPDATA%\MyAsioSplitter\config.ini`.

## License

Distributed under the terms of the [MIT License](LICENSE).

This project contains no Steinberg ASIO SDK code. ASIO is a trademark of
Steinberg Media Technologies GmbH. This project is not affiliated with or
endorsed by Steinberg.
