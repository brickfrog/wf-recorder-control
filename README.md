# wf-recorder-control

A compiled GTK4 controller for Wayland. Region selection uses `slurp`; region and display recording use `wf-recorder`. Selected-window recording uses niri's screencast portal with PipeWire and GStreamer, so it follows the window and does not capture other windows placed over it. Stopping lets the encoder finalize the video.

Build and run:

```sh
make
./wf-recorder-control
```

Build dependencies: a C compiler, `make`, GTK4, libportal-gtk4, and GStreamer development files. Runtime dependencies: `wf-recorder`, `slurp`, PipeWire, `xdg-desktop-portal-gnome`, and the GStreamer PipeWire, PulseAudio, encoding, and muxer plugins. `pactl` lists audio sources. The app has controls for region, selected window, or display capture; audio source; format; codec; frame rate; CRF; preset; save folder; and advanced wf-recorder arguments. CRF applies to software codecs; preset applies to software H.264/H.265. Advanced wf-recorder arguments are disabled in window mode. Hardware encoding needs a working FFmpeg or GStreamer encoder and GPU driver.

Codec choices follow the selected format: MP4 offers H.264/H.265, WebM offers VP9/AV1, and Matroska offers all listed codecs. The same format and codec choices are available for window capture.

Files are timestamped and never intentionally overwritten. Select **Stop and save** before closing the app.
