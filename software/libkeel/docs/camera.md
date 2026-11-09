<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# The camera

The robot's camera is a Raspberry Pi Camera Module 2 or Camera Module 3 on the Raspberry Pi 4's own camera
connector; it does not connect to the Keel board. `libkeel-camera` drives it: a C library of its own
(`libkeel-camera.so.1`, header `keel/camera.h`) and the command-line tool `keel-camera`, and the Python and Java
bindings bind it (sections 4 and 5). It talks to libcamera and to the Pi's hardware encoders directly, starts no
helper programs and needs nothing at run time except libcamera (with its Raspberry Pi IPA module) and the kernel's
V4L2 devices. Section 1 lists what a minimal image, such as a Yocto build, must contain.

What it does:

- **Raw frames** for the program, in I420, RGB888 or BGR888, converted by the Pi's image signal processor at no
  CPU cost.
- **H.264 and MJPEG** on the hardware encoders, read by the program or written by the library itself to files, TCP
  connections, a TCP port that serves several viewers, and UDP. With FFmpeg's libavformat built in (not in the
  release packages), also `.mp4`, `.mkv`, `.mov` and `.ts` files and `rtsp://`, `rtmp://` and `srt://` streams.
- **JPEG stills** up to the full sensor resolution (3280x2464 on Camera Module 2, 4608x2592 on Camera Module 3).
- **Autofocus** on Camera Module 3: continuous, single scans or a fixed lens position.

`libkeel.so` (the board library) knows nothing about the camera, and the camera library needs neither the board nor
`libkeel.so`: either works without the other. [spec.md](spec.md) section 26 has every rule.

## 1. Connect and check

1. Switch the robot off. Connect the camera's flat cable to the Pi's camera connector, between the micro-HDMI ports
   and the audio jack, contacts towards the micro-HDMI ports. The chassis route needs the 150 mm cable described in
   the chassis assembly guide (`chassis/assembly.md` in the repository).
2. Switch on. `install.sh` installs the camera library and `keel-camera` with the other packages when the system
   provides their libcamera (on the robot's Ubuntu 26.10 it does), and `keel-setup` adds the user to the `video`
   group, which owns the camera's device nodes. Log in again after it ran.
3. List the cameras:

```
$ keel-camera list
0: Camera Module 2 (imx219), fixed focus
   id /base/soc/i2c0mux/i2c@1/imx219@10
   full resolution 3280x2464
   modes 640x480 8-bit, up to 200.16 fps, crop (1000, 752) 1280x960
         1640x1232 8-bit, up to 81.07 fps, crop (0, 0) 3280x2464
         1920x1080 8-bit, up to 47.57 fps, crop (680, 692) 1920x1080
         3280x2464 8-bit, up to 21.19 fps, crop (0, 0) 3280x2464
         640x480 10-bit, up to 200.16 fps, crop (1000, 752) 1280x960
         ...
```

The modes are the sensor's own read-out modes. The library picks one for each stream: of the modes at least as large
as the frames that reach the frame rate, the one that sees the most of the sensor. 640x480 at 30 fps therefore uses
the full-sensor 1640x1232 mode and shows the whole field of view; 1920x1080 at 30 fps can only use the cropped
1920x1080 mode.

`list --json` prints the same as JSON. If nothing is listed, the tool says so; with the power off, check that the
cable is fully inserted at both ends, contacts the right way round, and check that the boot configuration enables
the camera (below).

### Other operating systems and minimal images

The library needs no camera application, no Raspberry Pi tool and no package manager at run time, so it also runs on
a minimal image, such as one built with Yocto and meta-raspberrypi. The image must contain:

- **The camera in the boot configuration.** `camera_auto_detect=1` in `config.txt` (the default on Raspberry Pi OS
  and Ubuntu), or the sensor's overlay: `dtoverlay=imx219` for Camera Module 2, `dtoverlay=imx708` for Camera
  Module 3. With meta-raspberrypi, `RASPBERRYPI_CAMERA_V2 = "1"` or `RASPBERRYPI_CAMERA_V3 = "1"` in `local.conf`
  adds the overlay. Without it the camera receiver has no sensor, and the library reports that no camera is
  detected.
- **The Raspberry Pi kernel drivers.** `bcm2835-unicam` (camera receiver), `bcm2835-isp` (image signal processor),
  `bcm2835-codec` (H.264 and JPEG encoders), the sensor driver (`imx219` or `imx708`) and the CMA heap
  (`/dev/dma_heap/linux,cma`). The Raspberry Pi kernel (`linux-raspberrypi` in meta-raspberrypi) has them all.
- **libcamera** with the Raspberry Pi pipeline handler, its IPA module (`ipa_rpi_vc4.so` on a Pi 4) and the sensors'
  tuning files.
- **libjpeg**, optional, for stills larger than 1920x1920.

Build the camera library for the image from `software/libkeel` (section 7; a C11 and a C++17 compiler, CMake 3.21
and libcamera's development files). Access to `/dev/media*`, `/dev/video*` and `/dev/dma_heap/*` is by the `video`
group, or whatever the image's device rules grant.

## 2. The `keel-camera` tool

```
keel-camera list                       the cameras and their modes
keel-camera still -o photo.jpg         one JPEG image, full resolution by default
keel-camera record -o clip.h264        video to a file until Ctrl-C (-t SECONDS to stop by itself)
keel-camera stream DESTINATION         video to a viewer, a port, UDP, a file or standard output (-)
keel-camera frames                     raw frames on standard output, rows unpadded
```

Common options: `-c N` (camera), `-W` and `-H` (size, default 640x480), `-f FPS` (default 30), `--hflip`,
`--vflip`, `--rotation 180`, `--codec h264|mjpeg`, `--bitrate BPS`, `-q QUALITY`, `-t SECONDS`. On Camera
Module 3: `--autofocus default|manual|auto|continuous`, `--af-range normal|macro|full` and `--lens-position D`
(dioptres, with `--autofocus manual`). Exposure and colour: `--exposure-us`, `--gain`, `--ev`, `--awb MODE`,
`--awb-gains R,B`. `keel-camera --help` lists them all.

Exit status: 0 done (also after Ctrl-C or SIGTERM), 1 the camera, encoder or destination failed while running,
2 usage error, 3 no such camera, encoder or destination, 4 the camera or port is in use, 5 permission denied,
6 not supported by this camera or build. Messages name the remedy, for example:

```
$ keel-camera record --autofocus continuous -o clip.h264
keel-camera: camera 0 (Camera Module 2, imx219) has a fixed-focus lens: leave af_mode, af_range and lens_position_milli at 0
```

### Stills

```sh
keel-camera still -o photo.jpg                     # 3280x2464 on Camera Module 2
keel-camera still -W 1920 -H 1080 -q 90 -o hd.jpg
```

Exposure, white balance and (on Camera Module 3) focus settle before the picture is taken, so a still takes about
a second. Up to 1920x1920 the Pi's JPEG encoder does the work; larger stills are encoded with libjpeg.

### Recording

```sh
keel-camera record -t 30 -o drive.h264                  # H.264, 640x480, 30 fps
keel-camera record -W 1280 -H 720 -t 30 -o drive.h264
keel-camera record --codec mjpeg -q 80 -t 30 -o drive.mjpeg
```

A `.h264` file is the bare stream, without timing. Play it with `ffplay -f h264 -framerate 30 drive.h264`, or
put it into an MP4 container without re-encoding:

```sh
ffmpeg -framerate 30 -i drive.h264 -c copy drive.mp4
```

Ctrl-C and `-t` both end the file cleanly: the frames still in the encoder are written first.

### Watching from another computer

The simplest set-up: the robot listens, and any number of viewers (up to four by default, `--max-clients`)
connect and disconnect when they like. Each viewer starts at the next keyframe, within a second.

```sh
# on the robot
keel-camera stream 'tcp://:5000?listen'

# on the computer (ROBOT is the robot's host name or address)
ffplay -f h264 -fflags nobuffer -flags low_delay -framedrop tcp://ROBOT:5000
vlc tcp/h264://ROBOT:5000
```

The other way round, the computer listens and the robot connects to it:

```sh
ffplay -f h264 -fflags nobuffer 'tcp://0.0.0.0:5000?listen'   # on the computer, first
keel-camera stream tcp://COMPUTER:5000                         # on the robot
```

UDP needs no connection and never makes the robot wait, but a lost datagram corrupts the picture until the next
keyframe:

```sh
ffplay -f h264 -fflags nobuffer udp://0.0.0.0:5000    # or: vlc udp://@:5000 :demux=h264
keel-camera stream udp://COMPUTER:5000
```

For MJPEG, add `--codec mjpeg` on the robot and use `-f mjpeg` with ffplay. MJPEG needs three to six times the
bandwidth of H.264, but every frame stands alone.

A viewer that cannot keep up loses chunks (the tool prints how many when it ends); the camera and the other viewers
are never slowed down. H.264 viewers lose whole groups of pictures, never single frames, so the picture they get
always decodes.

### OpenCV on another computer

OpenCV's FFmpeg back end reads the stream directly:

```python
import cv2

capture = cv2.VideoCapture("tcp://ROBOT:5000", cv2.CAP_FFMPEG)   # keel-camera stream 'tcp://:5000?listen'
while True:
    ok, frame = capture.read()          # a BGR numpy array, 480 x 640 x 3
    if not ok:
        break
    cv2.imshow("robot", frame)
    if cv2.waitKey(1) == 27:            # Esc
        break
```

Opening takes one to two seconds while FFmpeg examines the stream. MJPEG streams (`--codec mjpeg`) open the same
way.

### Piping into ffmpeg

`stream -` writes the H.264 stream to standard output, for any program that reads one:

```sh
keel-camera stream - -t 10 | ffmpeg -f h264 -framerate 30 -i - -c copy clip.mp4
ssh robot keel-camera stream - | ffplay -f h264 -fflags nobuffer -    # from the computer, over SSH
```

Add `--inline` when the reader may start late or restart: the parameter sets then precede every keyframe.

### Frames into a program or a model

`frames` writes raw frames back to back, with no padding and no header, and prints their size on standard error:

```
$ keel-camera frames --format bgr888 -W 320 -H 240 -f 10 | python3 model.py
keel-camera: 320x240 bgr888 frames of 230400 bytes (ffmpeg -f rawvideo -pixel_format bgr24 -video_size 320x240 -i -)
```

```python
# model.py
import sys

import numpy as np

WIDTH, HEIGHT = 320, 240
FRAME_BYTES = WIDTH * HEIGHT * 3

stream = sys.stdin.buffer
while True:
    data = stream.read(FRAME_BYTES)
    if len(data) < FRAME_BYTES:
        break
    image = np.frombuffer(data, np.uint8).reshape(HEIGHT, WIDTH, 3)  # rows, columns, (B, G, R)
    print("mean B, G, R:", image.mean(axis=(0, 1)).round(1))         # the model goes here
```

`--format rgb888` gives R, G, B order (PIL, most deep-learning frameworks), `bgr888` OpenCV's order, and `i420`
(the default) the Y plane followed by the U and V planes, 1.5 bytes per pixel. `-n N` stops after N frames. When
the model is slower than the camera, `frames` still delivers whole frames: the camera's newest frame is written
each time the pipe has room, and the frames in between are skipped.

## 3. Using the library from C

```sh
cc app.c $(pkg-config --cflags --libs keel-camera) -o app
```

```cmake
find_package(Keel 1.0 REQUIRED COMPONENTS camera)
target_link_libraries(app PRIVATE Keel::keel_camera)
```

The header is `keel/camera.h`. It uses the status codes of `keel/keel.h` (which it includes) but links only
`libkeel-camera.so.1`. Its conventions are those of `keel.h`: every struct starts with `struct_size`, every zero
option means the default, every call returns a `keel_status`, and data goes into buffers the caller provides.

### Frames

A complete program: open camera 0 at 640x480 in RGB, read three seconds of frames and print the centre pixel of each.

```c
#include <keel/camera.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    keel_camera_options options = {.struct_size = sizeof options, .width = 640, .height = 480,
                                   .format = KEEL_CAMERA_FORMAT_RGB888};
    keel_camera_error error = {.struct_size = sizeof error};
    keel_camera *camera = NULL;
    int32_t rc = keel_camera_open(&options, &camera, &error);
    if (rc != KEEL_OK) {
        fprintf(stderr, "keel_camera_open failed (%d): %s\n", (int)rc, error.message);
        return 1;
    }

    keel_camera_stream_info stream = {.struct_size = sizeof stream};
    keel_camera_get_stream(camera, &stream);
    uint8_t *frame = malloc(stream.frame_bytes);
    rc = frame != NULL ? keel_camera_start(camera) : KEEL_E_NO_MEMORY;

    for (int i = 0; rc == KEEL_OK && i < 90; ++i) { /* three seconds at 30 fps */
        keel_camera_frame_info info = {.struct_size = sizeof info};
        rc = keel_camera_read_frame(camera, frame, stream.frame_bytes, 1000, &info);
        if (rc == KEEL_OK) {
            /* Pixel (x, y) starts at offset0 + y * stride0 + x * 3: bytes R, G, B. */
            const uint8_t *p = frame + info.offset0 + info.height / 2 * info.stride0 + info.width / 2 * 3;
            printf("frame %llu: centre pixel R %u G %u B %u\n", (unsigned long long)info.sequence, p[0], p[1],
                   p[2]);
        }
    }
    if (rc != KEEL_OK) {
        keel_camera_last_error(camera, &error);
        fprintf(stderr, "camera failed (%d): %s\n", (int)rc, error.message);
    }
    free(frame);
    keel_camera_close(camera); /* stops capture and ends the library's threads */
    return rc == KEEL_OK ? 0 : 1;
}
```

- `keel_camera_get_stream` gives the layout before capture starts, so buffers are allocated once. Rows can be
  padded: always use the strides and offsets, never `width * 3`.
- `keel_camera_read_frame` takes the oldest queued frame. The queue holds one frame by default
  (`options.frame_queue`, up to 8), so a slow reader always gets the newest frame and the ones in between are
  dropped and counted (`keel_camera_get_stats`). `KEEL_E_AGAIN` means no frame arrived within the timeout.
- `info.sequence` comes from the sensor: a gap means a frame the camera dropped. `info.timestamp_ns` is the start
  of exposure on `CLOCK_MONOTONIC`, the clock of libkeel's `keel_now_us` on the robot, so frames line up with
  the control loop.
- Frames are full-range BT.601 (the JPEG colour space). `keel_camera_convert_i420` converts an I420 frame to RGB
  or BGR on the CPU when a program needs both.

### Encoded video and outputs

The library can write the encoder's output itself, to any mix of files and network destinations, while the program
reads raw frames or does nothing at all:

```c
#include <keel/camera.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    keel_camera_error error = {.struct_size = sizeof error};
    keel_camera *camera = NULL;
    int32_t rc = keel_camera_open(NULL, &camera, &error); /* camera 0, 640x480 at 30 fps */
    if (rc != KEEL_OK) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }

    keel_camera_encoder_options h264 = {.struct_size = sizeof h264, .codec = KEEL_CAMERA_CODEC_H264};
    uint32_t viewers = 0;
    uint32_t file = 0;
    rc = keel_camera_set_encoder(camera, &h264);
    if (rc == KEEL_OK) {
        rc = keel_camera_output_start(camera, "tcp://:5000?listen", NULL, &viewers);
    }
    if (rc == KEEL_OK) {
        rc = keel_camera_output_start(camera, "drive.h264", NULL, &file);
    }
    if (rc == KEEL_OK) {
        rc = keel_camera_start(camera);
    }
    if (rc == KEEL_OK) {
        sleep(10); /* the library's threads encode, send and write meanwhile */
        keel_camera_output_stats stats = {.struct_size = sizeof stats};
        keel_camera_get_output_stats(camera, viewers, &stats);
        printf("viewers: %u connected, %llu chunks sent, %llu dropped\n", stats.clients,
               (unsigned long long)stats.chunks, (unsigned long long)stats.chunks_dropped);
    } else {
        keel_camera_last_error(camera, &error);
        fprintf(stderr, "%s\n", error.message);
    }
    keel_camera_close(camera); /* finishes drive.h264 and closes every connection */
    return rc == KEEL_OK ? 0 : 1;
}
```

Destinations of `keel_camera_output_start`:

| Destination | What happens |
|---|---|
| `tcp://HOST:PORT` | connects to a receiver (within 3 s, or the call fails) and sends the stream |
| `tcp://:PORT?listen` | waits for receivers on every address (or `tcp://ADDRESS:PORT?listen` on one) and serves up to `max_clients` at once, each from its next keyframe |
| `udp://HOST:PORT` | sends datagrams of at most `datagram_bytes` (1472 by default) |
| `NAME.h264`, `NAME.264` | H.264 Annex B file |
| `NAME.mjpeg`, `NAME.mjpg` | concatenated JPEG images |
| `NAME.mp4`, `.mkv`, `.mov`, `.ts`; `rtsp://`, `rtmp://`, `srt://` | with libavformat built in only; otherwise `KEEL_E_UNSUPPORTED` |

Every output has its own thread and queue (64 chunks by default). A destination that cannot keep up loses chunks,
counted in its statistics, and never delays the camera, the program or the other outputs. A destination that fails
(the receiver closed the connection, the disk is full) puts that output in state `FAILED` with a message; capture
and the other outputs continue. `keel_camera_output_stop` or `keel_camera_close` sends what is queued and finishes
the file.

To process the encoded stream in the program instead, call `keel_camera_read_encoded` after starting: each chunk is
one encoded frame, flagged `KEEL_CAMERA_ENCODED_KEYFRAME` and `KEEL_CAMERA_ENCODED_HEADERS` where it applies. The
first chunk is always a keyframe with the parameter sets, and after an overflow the queue resumes at a keyframe.

Encoder settings (`keel_camera_encoder_options`): H.264 bit rate (default 0.15 bit per pixel and frame:
1.4 Mbit/s at 640x480 and 30 fps), keyframe interval (default one second), profile (High by default), MJPEG
quality (default 85). The hardware encoders take frames up to 1920x1920; open larger streams for raw frames and
stills only.

### Stills

```c
keel_camera_still_info info = {.struct_size = sizeof info};
rc = keel_camera_capture_still_file(camera, NULL, "photo.jpg", &info); /* full resolution */
```

`keel_camera_capture_still` writes into a caller buffer instead ((width rounded up to 16) x (height rounded up to
16) x 3 + 2048 bytes always suffice). Both need the capture stopped: they configure a still stream, let exposure,
white balance and focus settle, take one frame and encode it. The next `keel_camera_start` returns to the video
stream.

### Camera Module 3: focus

`keel_camera_info.flags` has `KEEL_CAMERA_INFO_AUTOFOCUS` for cameras with a focus motor. By default they focus
continuously. `af_mode = KEEL_CAMERA_AF_AUTO` focuses once at start and on every `keel_camera_trigger_autofocus`
(which waits for the result); `KEEL_CAMERA_AF_MANUAL` holds `lens_position_milli` (dioptres x 1000: 0 is infinity,
1000 is 1 m, 10000 is 10 cm). On a fixed-focus camera such as Camera Module 2 every focus option is
`KEEL_E_UNSUPPORTED`.

### Errors

Every failure records a `keel_camera_error`: the status, the resource (camera, IPA module, device node, encoder,
file, network and so on), the OS error, the path and a one-line message that names the remedy.
`keel_camera_open` and `keel_camera_list_open` fill the struct passed to them; afterwards
`keel_camera_last_error` returns the latest one of the handle.

| Status | Usual reason | Remedy |
|---|---|---|
| `KEEL_E_NOT_FOUND` | no camera, a wrong index, the IPA module or the hardware encoder missing, a receiver that does not answer | check the cable with the power off and the camera entry in `config.txt` ([section 1](#other-operating-systems-and-minimal-images)); install libcamera's IPA modules (`libcamera-ipa` on Debian and Ubuntu); start the receiver first |
| `KEEL_E_IN_USE` | another program or handle has the camera, or the listening port is taken | close the other program; choose another port |
| `KEEL_E_PERMISSION` | a camera, encoder or memory device node is not accessible | `sudo keel-setup --user NAME`, then log in again |
| `KEEL_E_UNSUPPORTED` | autofocus on a fixed-focus camera, a container output without libavformat, an encoder for a stream above 1920x1920 | as the message says |
| `KEEL_E_PERIPHERAL` | the camera or encoder failed while running (state `FAILED`) | `keel_camera_stop`, then start again; check the cable if it repeats |
| `KEEL_E_CONFLICT` | a call from another thread than the one that opened the handle | call from the owning thread; `keel_camera_close` works from any thread |

### Threads and processes

- The thread that opened a handle owns it; other threads get `KEEL_E_CONFLICT`. `keel_camera_close` works from any
  thread and wakes a read blocked in the owner, which then returns `KEEL_E_INVALID_HANDLE`. A control loop and a
  camera can therefore live on separate threads, each owning its own handle.
- The library's threads (capture, encoder, one per output) all end in `keel_camera_stop` or `keel_camera_close`.
  It installs no signal handlers, opens every file descriptor close-on-exec and never raises `SIGPIPE`.
- One camera can be open in one process at a time. A forked child gets `KEEL_E_NOT_OWNER` from every inherited
  handle; it opens its own after the parent closed it.

## 4. Using the camera from Python

The Python binding (`keel-robot`, `python3-keel`) has the camera in `keel.camera`: thin wrappers over
`libkeel-camera.so.1`, with every status raised as an exception and the constants and structs generated from
`camera.h`. It needs neither `libkeel.so` nor a `keel.Device`, and a program that uses only the camera never loads the
board library. The camera library is loaded on the first camera call, by its SONAME or from the absolute path in
`KEEL_CAMERA_LIBRARY`; `python3-keel` recommends `libkeel-camera1`, and numpy is optional
(`sudo apt install python3-numpy`).

```python
from keel import camera

for listed in camera.list_cameras():
    print(listed.index, listed.model, listed.sensor, "autofocus" if listed.autofocus else "fixed focus")

with camera.open(width=640, height=480, pixel_format=camera.PixelFormat.BGR888) as cam:
    cam.start()
    for count, frame in enumerate(cam.frames(), start=1):
        image = frame.array()               # numpy (480, 640, 3), B, G, R: a view of the frame, no copy
        print(frame.sequence, frame.timestamp_ns, image.mean())
        if count == 90:                     # three seconds at 30 fps
            break
```

| C | Python |
|---|---|
| `keel_camera_list_*` | `camera.list_cameras()`: `ListedCamera` objects with `.info` (`CameraInfo`) and `.modes` (`SensorMode`) |
| `keel_camera_open`, `keel_camera_options` | `camera.open(index, width=, height=, framerate=, pixel_format=, hflip=, vflip=, rotation=, af_mode=, af_range=, lens_position=, exposure_us=, gain=, ev=, awb_mode=, red_gain=, blue_gain=, frame_queue=)`; fractions are floats (`framerate=30.0`, `lens_position=1.0` dioptres) |
| `keel_camera_close` | `Camera.close()` or a `with` block; any thread |
| `keel_camera_get_info`, `keel_camera_get_stream` | `Camera.info`, `Camera.stream_info()` |
| `keel_camera_set_encoder` | `Camera.set_encoder(Codec.H264, bitrate=, intra_period=, profile=, inline_headers=, quality=, encoded_queue=)` |
| `keel_camera_start`, `keel_camera_stop` | `Camera.start()`, `Camera.stop()` |
| `keel_camera_read_frame` | `Camera.read_frame(timeout=1.0)` and the iterator `Camera.frames()`: `Frame` |
| `keel_camera_read_encoded` | `Camera.read_chunk(timeout=1.0)` and the iterator `Camera.chunks()`: `EncodedChunk` |
| `keel_camera_output_start`, `_stop`, `keel_camera_get_output_stats` | `Camera.start_output(destination, queue=, max_clients=, datagram_bytes=)` returns the output id; `Camera.stop_output(id)`, `Camera.output_stats(id)` |
| `keel_camera_capture_still`, `_file` | `Camera.capture_still(width=, height=, quality=, timeout=)`: `StillImage`; `Camera.capture_still_file(path, ...)`: `StillInfo` |
| `keel_camera_trigger_autofocus` | `Camera.trigger_autofocus(timeout=2.0)`: `AutofocusState` |
| `keel_camera_get_stats`, `keel_camera_last_error` | `Camera.stats()`, `Camera.last_error()` |
| `keel_camera_convert_i420` | `Frame.convert(PixelFormat.RGB888)` |
| `keel_camera_abi_version`, `keel_camera_version_string` | `camera.abi_version()`, `camera.version_string()`, `camera.load()` |

- **Frames.** A `Frame` owns its buffer: `frame.info` is the `FrameInfo` (layout, `sequence`, `timestamp_ns`),
  `frame.array()` a numpy view of the pixels with no copy (shape (height, width, 3) for RGB888 and BGR888;
  OpenCV's (height * 3 // 2, width) for I420, a copy when the rows are padded), `frame.plane(0)` the Y plane,
  `frame.data` the bytes as a memoryview and `frame.to_bytes()` the rows without padding. The last two need no numpy.
- **Errors.** Each status raises its own type, all subclasses of `CameraError` (`keel.camera.errors`):
  `CameraArgumentError` (also a `ValueError`), `CameraUnsupportedError`, `CameraStateError`, `CameraTimeoutError`
  (also a `TimeoutError`), `CameraThreadError`, `CameraNotFoundError`, `CameraBusyError`, `CameraPermissionError`
  (also a `PermissionError`) and `CameraClosedError`; the others raise `CameraError`. `.report` is the
  `ErrorReport`, and the message is the record's message with its remedy. A missing library raises
  `CameraLibraryError`, a `keel.LibraryError`.
- **Threads.** The thread that opens a camera owns it: another thread gets `CameraThreadError`, except for
  `close()`. A `close()` from another thread ends a `frames()` loop in the owner. A camera that is dropped without
  being closed is closed by a finalizer, with a `ResourceWarning`, and every camera still open is closed at
  interpreter exit.
- **Examples.** `examples/camera_frames.py` (frames into vision code; OpenCV if it is installed),
  `examples/camera_stream.py` (video to the network or a file) and `examples/camera_still.py` (a photo); on the
  robot they are in `/usr/share/doc/python3-keel/examples`.

The Python binding's README has the rest: [python/README.md](../../bindings/python/README.md#camera).

## 5. Using the camera from Java

The Java binding (`io.github.bisocmorg:keel`) has the camera in the package `io.github.bisocmorg.keel.camera` and its
subpackages `frames`, `encoding`, `outputs`, `stills` and `errors`: thin wrappers over `libkeel-camera.so.1` through
the Foreign Function and Memory API, with the same names and shapes as in Python (`listCameras`, `open`,
`readFrame`, `frames`, `setEncoder`, `startOutput`, `captureStill`, ...). Options are records with builders. The JVM
grants native access as for libkeel, and the camera library is found the same way (`KEEL_CAMERA_LIBRARY`, else
`libkeel-camera.so.1`).

```java
import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import java.awt.image.BufferedImage;

public final class Viewer {
    public static void main(String[] args) {
        CameraOptions options = CameraOptions.builder().width(640).height(480).pixelFormat(PixelFormat.BGR888)
                .build();
        try (Camera camera = Camera.open(options)) {
            camera.setEncoder(Codec.H264);
            camera.startOutput("tcp://:5000?listen");      // viewers connect while the program reads frames
            camera.start();
            for (Frame frame : camera.frames()) {
                BufferedImage image = frame.toBufferedImage();
                System.out.println(frame.sequence() + " " + image.getWidth());
            }
        }
    }
}
```

- **Frames.** A `Frame` is backed by a direct buffer of its own: `data()` (a read-only `ByteBuffer`) and `segment()`
  (a `MemorySegment`) give the pixels with no copy, `toByteArray()` the rows without padding and `toBufferedImage()`
  a `TYPE_3BYTE_BGR` image. Only `toBufferedImage()` uses `java.desktop`, which the module requires statically: on
  the module path, add `--add-modules java.desktop`.
- **Errors.** The sealed `CameraException` and one final subclass per status (`CameraArgumentException`,
  `CameraUnsupportedException`, `CameraStateException`, `CameraTimeoutException`, `CameraThreadException`,
  `CameraNotFoundException`, `CameraBusyException`, `CameraPermissionException`, `CameraClosedException`), with
  `report()` holding the `ErrorReport`. A missing library throws `CameraLibraryException`, a `LibraryException`.
- **Threads.** The platform thread that opens a camera owns it, and a virtual thread cannot open one, because it
  changes OS thread. `close()` works from any thread. A cleaner closes a camera that is never closed, with a
  warning, and a shutdown hook every camera still open at JVM exit.
- **Examples.** `examples/CameraFrames.java`, `examples/CameraStream.java` and `examples/CameraStill.java`, run
  with the source launcher like the other examples.

The Java binding's README has the rest: [java/README.md](../../bindings/java/README.md#camera).

## 6. Testing without a camera

Builds of the source tree with `KEEL_CAMERA_TEST_HOOKS=ON` (the CMake presets set it; the packages never do)
replace libcamera and the encoders with a synthetic Camera Module 2 (camera 0) and Camera Module 3 (camera 1) when
the environment has `KEEL_CAMERA_TEST_SOURCE=1`. Frames carry a computable pattern, and
`KEEL_CAMERA_TEST_FAULT` injects failures (`no-camera`, `busy`, `permission`, `stall`, `encoder-fail` and others,
spec.md section 26.9). The library's own tests and the bindings' tests run on it; it is not meant for applications.
The bindings' suites set the variables themselves and need only `KEEL_CAMERA_LIBRARY` pointing at such a build.

```sh
cmake --preset dev && cmake --build --preset dev
KEEL_CAMERA_TEST_SOURCE=1 build/dev/keel-camera list
```

## 7. Building it

The camera library is part of the libkeel source tree. CMake builds it when it finds libcamera (`libcamera-dev`)
and a C++17 compiler, and skips it otherwise (`-DKEEL_CAMERA=ON` makes it required, `OFF` leaves it out).
`KEEL_CAMERA_JPEG` (libjpeg, for stills above 1920x1920) and `KEEL_CAMERA_LIBAV` (FFmpeg's libavformat, for
container and protocol outputs) are `AUTO` by default. The release packages are built with libjpeg and without
libavformat. `keel-camera --version` names what a build contains:

```
$ keel-camera --version
keel-camera 1.0.0 (aarch64-linux-gnu; libcamera v0.7.0; jpeg)
```

libcamera has no stable ABI, so a build of the camera library runs only with the libcamera version it was built
against (the packages depend on it by name, such as `libcamera0.7`). On a distribution with another libcamera,
build from source there; the source works with libcamera 0.2 and later.
