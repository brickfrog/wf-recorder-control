# wf-recorder-control

A compiled GTK4 controller for Wayland. Region selection uses `slurp`; region and display recording use `wf-recorder`. Selected-window recording uses niri's screencast portal with PipeWire and GStreamer, so it follows the window and does not capture other windows placed over it. Stopping lets the encoder finalize the video.

Build and run:

```sh
make
./wf-recorder-control
```

Build dependencies: a C compiler, `make`, GTK4, libportal-gtk4, GStreamer, and FFmpeg's libavcodec development files. Runtime dependencies: `wf-recorder`, `slurp`, PipeWire, `xdg-desktop-portal-gnome`, and the GStreamer PipeWire, PulseAudio, encoding, and muxer plugins. `pactl` lists audio sources. The app has controls for region, selected window, or display capture; audio source; format; codec; frame rate; quality; save folder; and advanced wf-recorder arguments. Hardware codecs offer a target bitrate override (0 uses constant quality). Advanced wf-recorder arguments are disabled in window mode.

Codec choices follow the selected format: MP4 offers H.264/H.265, WebM offers VP9/AV1, and Matroska offers all listed codecs. The same format and codec choices are available for window capture.

On niri, selected-window capture uses the ScreenCast portal. If a user-level `~/.config/xdg-desktop-portal/portals.conf` overrides niri's portal selection, set `org.freedesktop.impl.portal.ScreenCast=gnome` under `[preferred]` so the window picker is available.

The interface groups capture, video, audio, and save controls, with a recording timer and stop button that remain visible while scrolling. **Ctrl+R** starts and **Ctrl+Shift+R** stops when the app is focused. After recording, **Open video** and **Open folder** are available. Settings are saved to `$XDG_CONFIG_HOME/wf-recorder-control/settings.ini` (usually `~/.config/wf-recorder-control/settings.ini`). Codec choices are limited to encoders installed for the selected capture mode.

Files are timestamped and never intentionally overwritten. Select **Stop and save** before closing the app.
