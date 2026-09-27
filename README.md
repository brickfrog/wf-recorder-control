# wf-recorder-control

A GTK4 screen recorder for Wayland. It uses `slurp` and `wf-recorder` for region and display capture. On niri, it records a selected window through the ScreenCast portal and PipeWire, following that window when it moves or another window covers it.

## Features

- Record a selected window, screen region, or display.
- Save MP4, WebM, or Matroska video with codec choices filtered by format and installed encoders.
- Set audio source, frame rate, quality, output folder, and optional `wf-recorder` arguments for region/display capture.
- Keep recording controls and the timer visible while scrolling. Use **Ctrl+R** to start or **Ctrl+Shift+R** to stop when the app is focused.
- Open the saved video or its folder from the app. Settings persist in `$XDG_CONFIG_HOME/wf-recorder-control/settings.ini`.

| Container | Available video codecs |
| --- | --- |
| MP4 | H.264, H.265 |
| WebM | VP9, AV1 |
| Matroska (`.mkv`) | H.264, H.265, VP9, AV1 |

H.264, H.265, VP9, and AV1 software encoders appear when installed. VA-API H.264/H.265 options appear when their encoder plugin is installed; using them also requires compatible hardware.

## Build and run

Build dependencies: a C compiler, `make`, and development files for GTK4, libportal-gtk4, libpipewire, GStreamer (including `gstreamer-app-1.0`), and FFmpeg's libavcodec.

Runtime dependencies: `wf-recorder`, `slurp`, PipeWire, a working ScreenCast portal for window capture, and `pactl` for listing audio sources. Window recording also needs the relevant GStreamer encoder, parser, and muxer plugins. Window audio recording needs the GStreamer PulseAudio plugin.

Build, test, and run from this directory:

```sh
make
make check
./wf-recorder-control
```

## Selected-window capture on niri

Choose **Select a window**, click **Start recording**, and pick the target in the portal dialog. On niri, the ScreenCast portal needs a window-capable backend such as `xdg-desktop-portal-gnome`. If your `~/.config/xdg-desktop-portal/portals.conf` overrides portal selection, add this under `[preferred]`:

```ini
org.freedesktop.impl.portal.ScreenCast=gnome
```

Window capture reads niri's linear DMA-BUF PipeWire stream and sends frames to GStreamer for encoding. A window that does not change still produces a video with the full elapsed recording time.

To keep the recorder controls accessible on niri, add this optional rule to `~/.config/niri/config.kdl`:

```kdl
window-rule {
    match app-id=r#"^local\.wf_recorder_control$"#
    open-floating true
}
```

Validate the rule with `niri validate`. Recordings receive timestamped filenames. Use **Stop and save** before closing the app so the encoder can finalize the video.
