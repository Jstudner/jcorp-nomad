# HandBrake presets

`Nomad-480p-Web-DirectPlay-HandBrake-1.11.2.json` is an optional preset for
creating small, browser-friendly video files for Nomad.

The preset was exported from and validated with HandBrake 1.11.2. It produces
a web-optimized MP4 with H.264 Main Level 3.0 video, a maximum 720x480 frame,
up to 30 fps, a 600 kbps average video bitrate using two-pass encoding, and
80 kbps AAC stereo audio. Upscaling is disabled.

## Import

1. Open HandBrake 1.11.2 or later.
2. Choose **Presets > Import from file**.
3. Select `Nomad-480p-Web-DirectPlay-HandBrake-1.11.2.json`.
4. Choose **Nomad 480p Web Direct Play - Small** before starting the encode.

## 720p

`Nomad-720p-Web-DirectPlay-HandBrake-1.11.2.json` (contributed in #145) is the same idea
at a higher resolution: H.264 Main Level 3.1, a maximum 1280x720 frame, up to 30 fps,
a 900 kbps average video bitrate, and 96 kbps AAC stereo audio. Upscaling is disabled.
Import it the same way and choose **Nomad 720p Web Direct Play**.

Review the selected audio and subtitle tracks before encoding, especially for
sources with multiple languages or commentary tracks.

## After encoding

HandBrake writes every frame as its own chunk, which makes a long film's index
twice the size it needs to be, and nothing plays until the whole index has
arrived. Run **Fix videos** in Nomad-Tools on the finished files: its Fast start
re-lays them with a smaller index so they start faster, without touching the
picture or sound.
