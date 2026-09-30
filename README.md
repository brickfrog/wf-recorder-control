# wf-recorder-control

A GTK4 screen recorder for Wayland. It uses `slurp` and `wf-recorder` for region and display capture. On niri, it records a selected window through the ScreenCast portal and PipeWire, following that window when it moves or another window covers it.

## Features

- Record a selected window, screen region, or display. Pick the mode from the Region, Window, and Display tiles.
- Optionally count down 3 seconds after you choose the target and before recording starts.
- Watch a live preview while recording, with file size, average bitrate, and an audio level meter. Window capture previews its own frames at up to 15 fps; region and display capture take a `grim` snapshot every second.
- Save MP4, WebM, or Matroska video with codec choices filtered by format and installed encoders.
- Set audio source, frame rate, quality, output folder, and optional `wf-recorder` arguments for region/display capture.
- Keep recording controls and the timer visible while scrolling. Use **Ctrl+R** to start or **Ctrl+Shift+R** to stop (or cancel the countdown) when the app is focused. **Discard** stops at once and deletes the recording, including while it is still saving.
- VP9 and AV1 use real-time encoder settings and 4:2:0 color (unless you set a pixel format), so saving finishes about a second after you press Stop.
- Play the last recording inline, see its length, resolution, and size, and open the video or its folder. Settings persist in `$XDG_CONFIG_HOME/wf-recorder-control/settings.ini`.

| Container | Available video codecs |
| --- | --- |
| MP4 | H.264, H.265 |
| WebM | VP9, AV1 |
| Matroska (`.mkv`) | H.264, H.265, VP9, AV1 |

H.264, H.265, VP9, and AV1 software encoders appear when installed. VA-API H.264/H.265 options appear when their encoder plugin is installed; using them also requires compatible hardware.

## Build and run

Build dependencies: a C compiler, `make`, and development files for GTK4, libportal-gtk4, libpipewire, GStreamer (including `gstreamer-app-1.0`), and FFmpeg's libavcodec.

Runtime dependencies: `wf-recorder`, `slurp`, PipeWire, a working ScreenCast portal for window capture, and `pactl` for listing audio sources. Window recording also needs the relevant GStreamer encoder, parser, and muxer plugins. Window audio recording and the audio level meter need the GStreamer PulseAudio plugin; the meter also needs the `level` plugin from gst-plugins-good. The region/display preview needs `grim` (optional). Inline playback of the last recording uses GTK's GStreamer media backend and the matching decoders.

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

Window capture reads niri's linear DMA-BUF PipeWire stream and sends frames to GStreamer for encoding. niri sends window frames only while the window is on screen. Recording starts with the first frame, so a window that is off screen when you start is recorded from the moment it appears. While recording, the newest frame repeats whenever niri sends none (the window did not change, or it is hidden or off screen), so video and audio stay the full elapsed length.

To keep the recorder controls accessible on niri, add this optional rule to `~/.config/niri/config.kdl`:

```kdl
window-rule {
    match app-id=r#"^local\.wf_recorder_control$"#
    open-floating true
}
```

Validate the rule with `niri validate`. Recordings receive timestamped filenames. Use **Stop and save** before closing the app so the encoder can finalize the video.
