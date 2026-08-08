# HandBrake preset

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

Review the selected audio and subtitle tracks before encoding, especially for
sources with multiple languages or commentary tracks.
