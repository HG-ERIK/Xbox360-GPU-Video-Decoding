# Xbox 360 GPU Video Decoding

Hardware video decoding for Xbox 360 homebrew, using the console's built-in H.264 decoder.
Requires an RGH/JTAG console.

## Supported

| Codec         | Resolution | Frame rate | Bitrate (tested) | Status |
|---|---|---|---|---|
| H.264         | 720p       | 30 fps     | up to 15 Mbps    | Works   |
| H.264         | 720p       | 60 fps     | up to 15 Mbps    | Works   |
| H.264         | 1080p      | 30 fps     | up to 30 Mbps    | Works   |
| H.264         | 1080p      | 60 fps     | -                | not yet |
| HEVC (H.265)  | 720p       | 30 fps     | -                | Not yet |
| HEVC (H.265)  | 720p       | 60 fps     | -                | Not yet |
| HEVC (H.265)  | 1080p      | 30 fps     | -                | Not yet |
| HEVC (H.265)  | 1080p      | 60 fps     | -                | not yet |

Containers: MKV, MP4, M4V, MOV, WMV

All figures are measured on a real console from its HDMI output.

## How it works (simple)

1. The player opens the video file and splits it into video frames and audio.
2. Each compressed frame goes to the console's built-in decoder, together with a time stamp.
3. The CPU unpacks the compressed data.
4. The GPU rebuilds the picture from it, using 83 small GPU programs (shaders).
5. The finished picture stays in GPU memory and is drawn straight to the screen.
6. The player shows each picture on the right screen refresh, in sync with the audio.

## Roadmap

- [x] Find the H.264 decoder in the system software (`dash.ExtraAVCodecs.xex`)
- [x] Load it from homebrew through XAV, the media player library Microsoft's own apps use
- [x] Find why it never produced a picture: its GPU work needs a low-priority GPU queue that apps must create
- [x] Drive the decoder directly, without XAV's player
- [x] 1080p 30 fps H.264
- [x] 720p 60 fps H.264
- [x] MKV, MP4, M4V, MOV, WMV
- [x] Exact seeking
- [ ] 1080p 60 fps
- [ ] multichannel audio
- [ ] HEVC (H.265)
- [ ] 10-bit video

## How it works (in depth)

As far as we know, this is the only public description of this decoder. Everything below was found by
disassembling the modules and reading their state live on a running console, and checked by experiment.
Microsoft's code is not included.

### Where the decoder lives

The decoder is a system module, `dash.ExtraAVCodecs.xex`. The dashboard's video player and Microsoft's other apps use
it. It decodes H.264 Baseline, Main and High up to Level 4.1 (designed for 1080p 30 fps or 720p 60 fps), plus
VC-1/WMV and MPEG-2. It has no HEVC.

`XAVInitialize` from the XDK's `xav.lib` loads the module and hands it a table of 66 Direct3D functions. The decoder
draws through the app's own Direct3D device through that table, so the module works with any app.

### CPU and GPU split

- **CPU:** CABAC entropy decoding and slice parsing run on worker threads (up to 8; we use 6). A separate
  "motor" thread drives every open decoder.
- **GPU:** 83 shaders do the rest: dequantisation, inverse transforms (4x4, 8x8), intra prediction, motion
  compensation, deblocking strength and the deblocking filter itself.
- The decoder records its GPU work into **low-priority command buffers**. A Direct3D device has no low-priority
  queue until the app calls `D3DDevice_ResetLowPriorityQueues(device, 1, 1024, 0)`. Without it the decoder takes
  input but never outputs a picture. This was the main blocker.
- The GPU runs low-priority work only at **yield points**. Every swap inserts one, so the decoder runs in the gaps
  between the app's own frames.

### Driving it without XAV's player

The decoder's API (the `H264...` functions behind `xav.lib`):

1. `H264InitParamStructureEx` fills a parameter block. We set the read callback, a notify callback, the thread
   count, the maximum size (up to 2048x1088), the level and our Direct3D device.
2. `H264DecodeCreateEx` and `H264DecodeStart` open it.
3. The decoder's threads call our **read callback** for data. Each call gets one access unit in Annex B format
   (start codes, not MP4/MKV length prefixes), plus a sample info block with a 64-bit time stamp. SPS/PPS go in
   only before the first keyframe: sending them again makes the decoder reconfigure and free pictures still on
   screen.
4. `H264DecodeGetOutputInfo` says whether a picture is ready and returns our time stamp with it.
   `H264DecodeGetOutputGPU` gives the picture: three linear 8-bit planes (Y, U, V) in GPU memory, coded as
   1920x1088, plus the crop to 1920x1080.
5. We point texture headers at the three planes and draw them with a YUV-to-RGB shader (BT.709). No copy.
6. `H264DecodeReturnOutputGPU` gives the picture back once the GPU has finished drawing it (with a fence).

### Rules that keep it stable

- **The motor lock.** The motor thread works inside its own lock, and the decoder's output and return calls have
  no lock of their own. Calling them without holding the motor's lock crashed the console at random.
- **Swap, not Present, during playback.** While the low-priority queue is visible, `Present` counts two swap
  callbacks per frame, and the decoder then gets only one job per frame (3-9 fps). `Swap` counts one. In menus
  the queue is hidden and `Present` is fine.
- **Throttle input, not output.** Holding finished pictures back makes the decoder wait on the GPU and stalls the
  whole GPU. We feed at most about 1 second ahead of the audio clock instead.
- **Close cleanly.** The decoder's GPU jobs only run at yield points. Before closing, we keep inserting yield points
  until its job count is 0; otherwise the next decoder opened never starts. When the app exits to the dashboard, a
  shutdown routine closes the decoder, or its threads would run on freed memory and crash the console.

### Timing and seeking

- The decoder can drop or reorder pictures. Each one is matched to its input by the time stamp it carries.
- The stamp also carries a seek counter, so a picture decoded from before a seek can never be shown after it.
- On the way to a seek target, B-frames that no other picture depends on are not decoded at all.
- The display clock moves exactly one screen refresh per vertical blank and follows the audio clock gently. Each
  picture is shown on the refresh it belongs to: 24 fps gives the even 3,2,3,2 pattern, 60 fps one picture per
  refresh.

### Limits

- 1080p 60 fps decodes at about 48-54 fps: Level 4.2 is beyond what the decoder was designed for.
- HEVC and 10-bit video need a new decoder, written for the Xbox 360's CPU and GPU.
