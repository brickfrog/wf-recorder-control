# wf-recorder-control

A compiled GTK4 controller for `wf-recorder` on Wayland. It calls `slurp` to select a region, lists displays through `wf-recorder -L`, and shows audio sources through `pactl`. Recording uses `wf-recorder` directly; stopping sends SIGINT so the video can be finalized.

Build and run:

```sh
make
./wf-recorder-control
```

Build dependencies: a C compiler, `make`, and GTK4 development files. Runtime dependencies: `wf-recorder`, `slurp`, and optionally `pactl` for listing audio sources. The app has controls for region or display capture, audio source, format, codec, frame rate, CRF, preset, save folder, and advanced encoder arguments. CRF and preset apply to software H.264/H.265. Hardware encoding needs a working FFmpeg encoder and device on your system.

Files are timestamped and never intentionally overwritten. Select **Stop and save** before closing the app.
