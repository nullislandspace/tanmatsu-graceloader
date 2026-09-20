# Graceloader

This is the Graceloader for the Tanmatsu intergalactic communicator.

Software is available at https://github.com/nullislandspace/tanmatsu-graceloader

## What an app gets

Graceloader is installed once, in appfs, and every app referencing it as its
`interpreter` (`"type": "script"` in metadata.json) is loaded into this process.
It deliberately *links* the components (BSP, PAX, WiFi, BT, …) without
initialising them — the app decides what to bring up — but it does set up what an
app cannot do for itself or would get wrong:

- **Filesystems**: `/int` (internal flash) and, when present, `/sd`.
- **The install path** it loaded the app from, via `graceloader_get_install_basepath()`,
  which is where an app finds the assets installed beside its `app.so`.
- **Merged input** (`gl_input`): native keyboard plus USB HID.
- **Local time** (`main/timezone.c`): the POSIX TZ string the launcher stored in
  NVS is applied to the process before anything is mounted or written. Without
  it a freshly booted app runs at UTC, and since FATFS takes file timestamps
  from `localtime_r()`, every file an app wrote to the SD card was stamped in
  UTC. Set the zone once in the launcher (Settings > Clock > Set timezone); with
  none set, UTC remains and a log line says so.
- **A CPU frequency lock**, held for the whole lifetime: without it DFS can drop
  the CPU below PSRAM speed mid-DMA and corrupt memory (see `idf6_sd_fuckup.md`).
- **Volume limiting** and the USB mode switch back to flash/monitor.

## License

This software is under the [MIT license](https://opensource.org/license/mit). The MIT license allows others to build upon your work without restrictions while also making sure you retain your attribution.

(C) 2026 Rene Schickbauer
