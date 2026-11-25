The Ogg fixtures are original procedural sine tones, not game audio. Generate
with FFmpeg's `sine` source: mono 440 Hz at 22050 Hz for 0.05 seconds; stereo
660 Hz at 48000 Hz for 0.07 seconds, encoded with libvorbis. They exercise the
native stream decoder, resampler, transport and current-build state contract.

Configure this directory with CMake, build, and run CTest. The runtime decoder
is vendored from [stb](https://github.com/nothings/stb/blob/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_vorbis.c),
with its original license notices. Playback command semantics follow
[Mesen's enhanced audio API](https://github.com/SourMesen/Mesen/blob/master/Docs/content/hdpacks/_index.md#replacing-audio-in-games).
