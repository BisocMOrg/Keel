// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;

import io.github.bisocmorg.keel.AbiVersion;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.encoding.EncodedChunk;
import io.github.bisocmorg.keel.camera.encoding.EncoderFlag;
import io.github.bisocmorg.keel.camera.encoding.EncoderOptions;
import io.github.bisocmorg.keel.camera.errors.CameraClosedException;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.errors.CameraLibraryException;
import io.github.bisocmorg.keel.camera.errors.ErrorReport;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.StreamInfo;
import io.github.bisocmorg.keel.camera.outputs.OutputOptions;
import io.github.bisocmorg.keel.camera.outputs.OutputStats;
import io.github.bisocmorg.keel.camera.stills.StillImage;
import io.github.bisocmorg.keel.camera.stills.StillInfo;
import io.github.bisocmorg.keel.camera.stills.StillOptions;
import io.github.bisocmorg.keel.internal.camera.CameraErrorAccess;
import io.github.bisocmorg.keel.internal.camera.CameraErrorRegistry;
import io.github.bisocmorg.keel.internal.camera.CameraLayouts;
import io.github.bisocmorg.keel.internal.camera.CameraNativeLibrary;
import io.github.bisocmorg.keel.internal.camera.CameraStructCodec;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Cleaner;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;

/**
 * One open camera of libkeel-camera, the camera library of the Keel robot: the Raspberry Pi camera on the Pi 4's
 * own connector. Open it with {@link #open(CameraOptions)} and close it with try-with-resources.
 *
 * <p>A camera opens stopped: configure its encoder ({@link #setEncoder(EncoderOptions)}) and outputs
 * ({@link #startOutput(String)}), then {@link #start()} it. Its frames feed, at the same time, the raw frame queue
 * ({@link #readFrame()}, {@link #frames()}), the hardware encoder, whose chunks feed the encoded queue
 * ({@link #readChunk()}, {@link #chunks()}), and every output the library writes itself from its own threads. Every
 * queue is bounded and drops instead of growing; {@link #stats()} counts each drop.
 *
 * <p>The camera library is loaded on the first camera call ({@code libkeel-camera.so.1}, or the absolute path in
 * {@code KEEL_CAMERA_LIBRARY}); class initialisation never touches it, and it never loads libkeel.so.
 *
 * <p>Threads: the platform thread that opens a camera owns it, and every method but {@link #close()} must be called
 * from that thread; from another one libkeel-camera refuses the call with no effect
 * ({@link io.github.bisocmorg.keel.camera.errors.CameraThreadException}). A virtual thread cannot open a camera,
 * because it changes OS thread. {@code close()} works from any thread: it wakes a read blocked in the owning thread,
 * which then throws {@link CameraClosedException}, or ends its {@code frames()} or {@code chunks()} loop. A camera
 * that is never closed is closed by a cleaner, with a WARNING, and every camera still open at JVM exit by a shutdown
 * hook, so files are finished.
 */
public final class Camera implements AutoCloseable {
    private static final System.Logger LOG = System.getLogger("io.github.bisocmorg.keel.camera");
    private static final Cleaner CLEANER = Cleaner.create(task -> Thread.ofPlatform().daemon(true)
            .name("keel-camera-cleaner").unstarted(task));
    private static final Set<Closer> OPEN = ConcurrentHashMap.newKeySet();
    private static final AtomicBoolean HOOK = new AtomicBoolean();
    private static final Duration DEFAULT_TIMEOUT = Duration.ofSeconds(1);
    private static final Duration AUTOFOCUS_TIMEOUT = Duration.ofSeconds(2);

    private final CameraNativeLibrary library;
    private final MemorySegment token;
    private final Closer closer;
    private final CameraInfo info;
    private long frameBytes;
    private ByteBuffer chunkBuffer = ByteBuffer.allocateDirect(1);

    private Camera(CameraNativeLibrary library, MemorySegment token) {
        this.library = library;
        this.token = token;
        this.closer = new Closer(library, token);
        closer.cleanable = CLEANER.register(this, closer);
        OPEN.add(closer);
        ensureShutdownHook();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_INFO);
            check(library.getInfo(token, raw), "keel_camera_get_info");
            info = CameraStructCodec.cameraInfo(raw);
            refreshBuffers();
        } catch (RuntimeException | Error failure) {
            close();
            throw failure;
        }
    }

    private static CameraErrorAccess errors() {
        return CameraErrorRegistry.errors();
    }

    private void check(int code, String function) {
        if (code != 0) {
            throw errors().failed(library, token, code, function);
        }
    }

    private void refreshBuffers() {
        StreamInfo stream = streamInfo();
        frameBytes = stream.frameBytes();
        int chunk = (int) Math.max(stream.encodedBytesMax(), 1);
        if (chunkBuffer.capacity() != chunk) {
            chunkBuffer = ByteBuffer.allocateDirect(chunk);
        }
    }

    // ----------------------------------------------------------------- library ----

    /**
     * Loads libkeel-camera before the first camera call, from {@code KEEL_CAMERA_LIBRARY} or by SONAME, and checks
     * its ABI. Loading happens once; later calls return at once.
     *
     * @throws CameraLibraryException if the platform is not Linux, the library cannot be opened (the libkeel-camera1
     *     package is not installed), a symbol is missing, the ABI is incompatible or native access is denied
     */
    public static void load() {
        CameraNativeLibrary.load(null);
    }

    /**
     * Loads libkeel-camera from an absolute path, ignoring {@code KEEL_CAMERA_LIBRARY}, and checks its ABI.
     *
     * @param path the absolute path of {@code libkeel-camera.so.1}
     * @throws CameraLibraryException as {@link #load()}, if the path is relative, or if another library is loaded
     */
    public static void load(Path path) {
        CameraNativeLibrary.load(Objects.requireNonNull(path, "path"));
    }

    /**
     * {@code keel_camera_abi_version()}: the ABI of the loaded camera library; compare it with {@link CameraAbi}.
     *
     * @return the major and minor version
     */
    public static AbiVersion abiVersion() {
        int packed = CameraNativeLibrary.get().abiVersion();
        return new AbiVersion(packed >>> 16, packed & 0xFFFF);
    }

    /**
     * {@code keel_camera_version_string()}: the build identification of the loaded camera library, naming the
     * libcamera it was built against and the optional features built in ({@code jpeg}, {@code libav}).
     *
     * @return for example {@code "1.0.0 (aarch64-linux-gnu; libcamera v0.7.0; jpeg)"}
     */
    public static String versionString() {
        return CameraNativeLibrary.get().versionString();
    }

    /**
     * Lists the cameras present, with their models, capabilities and sensor modes.
     *
     * <p>Each camera that no process holds is briefly acquired to read the frame-rate limit and crop of every mode; a
     * held camera has {@link CameraInfo#inUse()} and zero rates. No camera at all is an empty list; a camera that is
     * connected but cannot be used throws with the reason. Takes up to about 2 s with a camera connected.
     *
     * @return the cameras, in libcamera's order
     * @throws CameraException for a camera that is connected but cannot be used
     * @throws CameraLibraryException if libkeel-camera cannot be loaded
     */
    public static List<ListedCamera> listCameras() {
        CameraNativeLibrary library = CameraNativeLibrary.get();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment out = arena.allocate(ADDRESS);
            MemorySegment record = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ERROR);
            int code = library.listOpen(out, record);
            if (code != 0) {
                throw fromRecord(code, "keel_camera_list_open", record);
            }
            MemorySegment list = out.get(ADDRESS, 0);
            try {
                MemorySegment count = arena.allocate(JAVA_INT);
                listCheck(library.listCount(list, count), "keel_camera_list_count");
                List<ListedCamera> cameras = new ArrayList<>();
                for (int index = 0; index < count.get(JAVA_INT, 0); index++) {
                    MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_INFO);
                    listCheck(library.listGet(list, index, raw), "keel_camera_list_get");
                    CameraInfo camera = CameraStructCodec.cameraInfo(raw);
                    List<SensorMode> modes = new ArrayList<>();
                    for (int mode = 0; mode < camera.modeCount(); mode++) {
                        MemorySegment rawMode = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_MODE);
                        listCheck(library.listGetMode(list, index, mode, rawMode), "keel_camera_list_get_mode");
                        modes.add(CameraStructCodec.cameraMode(rawMode));
                    }
                    cameras.add(new ListedCamera(camera, modes));
                }
                return List.copyOf(cameras);
            } finally {
                int status = library.listClose(list);
                if (status != 0) {
                    LOG.log(System.Logger.Level.WARNING, "keel_camera_list_close returned {0}",
                            CameraException.statusName(status));
                }
            }
        }
    }

    private static void listCheck(int code, String function) {
        if (code != 0) {
            throw errors().of(code, function, null);
        }
    }

    private static CameraException fromRecord(int code, String function, MemorySegment record) {
        ErrorReport report = CameraStructCodec.cameraError(record);
        return errors().of(code, function, report.statusValue() == code ? report : null);
    }

    /**
     * Opens camera 0 with every default ({@link CameraOptions#defaults()}).
     *
     * @return the open camera; close it with try-with-resources
     * @throws CameraException if the camera cannot be opened
     * @throws CameraLibraryException if libkeel-camera cannot be loaded
     */
    public static Camera open() {
        return open(CameraOptions.defaults());
    }

    /**
     * {@code keel_camera_open}: opens a camera for this thread (up to about 1 s).
     *
     * <p>On a fixed-focus camera such as Camera Module 2 every focus option throws
     * {@link io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException}. Other failures:
     * {@link io.github.bisocmorg.keel.camera.errors.CameraNotFoundException} (no camera with this index),
     * {@link io.github.bisocmorg.keel.camera.errors.CameraBusyException} (another process or handle has it),
     * {@link io.github.bisocmorg.keel.camera.errors.CameraPermissionException} (the user is not in the video group),
     * {@link io.github.bisocmorg.keel.camera.errors.CameraArgumentException} (an option out of range).
     *
     * @param options the options; zero numbers mean "default", as in the C struct
     * @return the open camera; close it with try-with-resources
     * @throws IllegalStateException on a virtual thread
     * @throws CameraException if the camera cannot be opened
     * @throws CameraLibraryException if libkeel-camera cannot be loaded
     */
    public static Camera open(CameraOptions options) {
        Objects.requireNonNull(options, "options");
        if (Thread.currentThread().isVirtual()) {
            throw new IllegalStateException("open the camera on a platform thread: the thread that opens a camera "
                    + "owns it, and a virtual thread changes OS thread between calls");
        }
        CameraNativeLibrary library = CameraNativeLibrary.get();
        int flags = (options.hflip() ? CameraFlag.HFLIP.mask() : 0) | (options.vflip() ? CameraFlag.VFLIP.mask() : 0);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_OPTIONS);
            CameraStructCodec.writeCameraOptions(raw, options.index(), options.width(), options.height(),
                    CameraOptions.milli("framerate", options.framerate(), false), options.pixelFormat().code(), flags,
                    options.rotation(), options.afMode().code(), options.afRange().code(),
                    CameraOptions.milli("lensPosition", options.lensPosition(), false), options.exposureUs(),
                    CameraOptions.milli("gain", options.gain(), false), CameraOptions.milli("ev", options.ev(), true),
                    options.awbMode().code(), CameraOptions.milli("redGain", options.redGain(), false),
                    CameraOptions.milli("blueGain", options.blueGain(), false), options.frameQueue());
            MemorySegment out = arena.allocate(ADDRESS);
            MemorySegment record = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ERROR);
            int code = library.open(raw, out, record);
            if (code != 0) {
                throw fromRecord(code, "keel_camera_open", record);
            }
            return new Camera(library, out.get(ADDRESS, 0));
        }
    }

    // ---------------------------------------------------------------- lifetime ----

    /**
     * Returns the camera this handle opened ({@code keel_camera_get_info}, read at open).
     *
     * @return the camera info
     */
    public CameraInfo info() {
        return info;
    }

    /**
     * Returns the camera's index.
     *
     * @return the index
     */
    public int index() {
        return (int) info.index();
    }

    /**
     * Returns whether {@link #close()} has begun, from any thread.
     *
     * @return {@code true} once closing
     */
    public boolean isClosed() {
        return closer.closing.get();
    }

    /**
     * Stops capture and every output, finishes their files, ends the library's threads and releases the camera.
     *
     * <p>Safe from any thread and idempotent; every caller returns once the camera is closed. A read blocked in the
     * owning thread wakes up and throws {@link CameraClosedException}. Never throws: a failed close (an output that
     * could not finish its file) is logged at WARNING. Takes up to about 3 s when an output is finishing a file or a
     * network peer.
     */
    @Override
    public void close() {
        closer.close(true);
    }

    @Override
    public String toString() {
        return "Camera[" + info.index() + ": " + info.model() + " (" + info.sensor() + "), "
                + (isClosed() ? "closed]" : "open]");
    }

    /**
     * {@code keel_camera_get_stream}: the layout of the configured stream and the buffer sizes the reads need. The
     * camera may have adjusted the requested size: width and height here are the ones it delivers.
     *
     * @return the stream info
     */
    public StreamInfo streamInfo() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_STREAM_INFO);
            check(library.getStream(token, raw), "keel_camera_get_stream");
            return CameraStructCodec.cameraStreamInfo(raw);
        }
    }

    /**
     * Chooses a codec with every default ({@link EncoderOptions#builder(Codec)}).
     *
     * @param codec {@link Codec#H264}, {@link Codec#MJPEG}, or {@link Codec#NONE} to remove the encoder
     */
    public void setEncoder(Codec codec) {
        setEncoder(EncoderOptions.builder(codec).build());
    }

    /**
     * {@code keel_camera_set_encoder}: chooses the hardware encoder for {@link #readChunk()}, {@link #chunks()} and
     * the outputs. Allowed while stopped and without outputs.
     *
     * @param options the encoder options
     * @throws io.github.bisocmorg.keel.camera.errors.CameraArgumentException for an option meant for the other codec
     *     or out of range
     * @throws io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException for a stream larger than 1920x1920
     */
    public void setEncoder(EncoderOptions options) {
        Objects.requireNonNull(options, "options");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ENCODER_OPTIONS);
            CameraStructCodec.writeEncoderOptions(raw, options.codec().code(), options.bitrate(),
                    options.intraPeriod(), options.profile().code(),
                    options.inlineHeaders() ? EncoderFlag.INLINE_HEADERS.mask() : 0, options.quality(),
                    options.encodedQueue());
            check(library.setEncoder(token, raw), "keel_camera_set_encoder");
        }
        refreshBuffers();
    }

    // ----------------------------------------------------------------- capture ----

    /** {@code keel_camera_start}: starts capture, the encoder and the data flow of every output (up to 500 ms). */
    public void start() {
        check(library.start(token), "keel_camera_start");
        refreshBuffers();
    }

    /**
     * {@code keel_camera_stop}: stops capture and the encoder. Queued frames and chunks are discarded; outputs stay
     * open and resume at the next keyframe after {@link #start()}.
     */
    public void stop() {
        check(library.stop(token), "keel_camera_stop");
    }

    /**
     * Takes the oldest queued raw frame, waiting up to one second.
     *
     * @return the frame
     * @throws io.github.bisocmorg.keel.camera.errors.CameraTimeoutException when no frame arrived in time
     */
    public Frame readFrame() {
        return readFrame(DEFAULT_TIMEOUT);
    }

    /**
     * {@code keel_camera_read_frame}: takes the oldest queued raw frame. The queue keeps
     * {@link CameraOptions#frameQueue()} frames (by default one, so a slow reader always gets the newest frame).
     * Each frame has a direct buffer of its own.
     *
     * @param timeout the longest wait, up to 60 s
     * @return the frame
     * @throws io.github.bisocmorg.keel.camera.errors.CameraTimeoutException when no frame arrived in time
     * @throws io.github.bisocmorg.keel.camera.errors.CameraStateException before {@link #start()}
     * @throws CameraClosedException once another thread closed the camera
     */
    public Frame readFrame(Duration timeout) {
        int milliseconds = milliseconds(timeout);
        ByteBuffer buffer = ByteBuffer.allocateDirect((int) frameBytes);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_FRAME_INFO);
            check(library.readFrame(token, MemorySegment.ofBuffer(buffer), (int) frameBytes, milliseconds, raw),
                    "keel_camera_read_frame");
            return new Frame(CameraStructCodec.cameraFrameInfo(raw), buffer);
        }
    }

    /**
     * Iterates over raw frames, each {@link #readFrame()} with a one-second timeout.
     *
     * @return an iterable whose iterators end when the camera is closed
     */
    public Iterable<Frame> frames() {
        return frames(DEFAULT_TIMEOUT);
    }

    /**
     * Iterates over raw frames until the camera is closed: {@link #readFrame(Duration)} in a loop. The iteration ends
     * normally when {@link #close()} is called, from this thread or another one; a frame that does not arrive in time
     * throws {@link io.github.bisocmorg.keel.camera.errors.CameraTimeoutException} from {@code hasNext()}.
     *
     * @param timeout the longest wait for each frame
     * @return an iterable; use it on the owning thread
     */
    public Iterable<Frame> frames(Duration timeout) {
        int milliseconds = milliseconds(timeout);
        return () -> new Reader<>(() -> readFrame(Duration.ofMillis(Integer.toUnsignedLong(milliseconds))));
    }

    /**
     * Takes the oldest queued encoded chunk, waiting up to one second.
     *
     * @return the chunk
     * @throws io.github.bisocmorg.keel.camera.errors.CameraTimeoutException when no chunk arrived in time
     */
    public EncodedChunk readChunk() {
        return readChunk(DEFAULT_TIMEOUT);
    }

    /**
     * {@code keel_camera_read_encoded}: takes the oldest queued encoded chunk, one H.264 access unit or one JPEG
     * image. The first chunk after {@link #start()} is a keyframe with the H.264 parameter sets; after the queue
     * overflowed, H.264 resumes at a keyframe. Needs an encoder.
     *
     * @param timeout the longest wait, up to 60 s
     * @return the chunk
     * @throws io.github.bisocmorg.keel.camera.errors.CameraTimeoutException when no chunk arrived in time
     * @throws io.github.bisocmorg.keel.camera.errors.CameraStateException before {@link #start()} or without encoder
     */
    public EncodedChunk readChunk(Duration timeout) {
        int milliseconds = milliseconds(timeout);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ENCODED_INFO);
            MemorySegment buffer = MemorySegment.ofBuffer(chunkBuffer);
            check(library.readEncoded(token, buffer, chunkBuffer.capacity(), milliseconds, raw),
                    "keel_camera_read_encoded");
            var chunk = CameraStructCodec.cameraEncodedInfo(raw);
            byte[] data = new byte[(int) chunk.bytes()];
            chunkBuffer.get(0, data);
            return new EncodedChunk(chunk, data);
        }
    }

    /**
     * Iterates over encoded chunks, each {@link #readChunk()} with a one-second timeout.
     *
     * @return an iterable whose iterators end when the camera is closed
     */
    public Iterable<EncodedChunk> chunks() {
        return chunks(DEFAULT_TIMEOUT);
    }

    /**
     * Iterates over encoded chunks until the camera is closed: {@link #readChunk(Duration)} in a loop, ending as
     * {@link #frames(Duration)} does.
     *
     * @param timeout the longest wait for each chunk
     * @return an iterable; use it on the owning thread
     */
    public Iterable<EncodedChunk> chunks(Duration timeout) {
        int milliseconds = milliseconds(timeout);
        return () -> new Reader<>(() -> readChunk(Duration.ofMillis(Integer.toUnsignedLong(milliseconds))));
    }

    /**
     * Runs one autofocus scan and waits up to two seconds for its result.
     *
     * @return the focus state
     */
    public AutofocusState triggerAutofocus() {
        return triggerAutofocus(AUTOFOCUS_TIMEOUT);
    }

    /**
     * {@code keel_camera_trigger_autofocus}: in {@link AutofocusMode#AUTO} it starts one scan; in
     * {@link AutofocusMode#DEFAULT} or {@link AutofocusMode#CONTINUOUS} it waits until the continuous search reports
     * focus.
     *
     * @param timeout the longest wait, 1 ms..60 s
     * @return {@link AutofocusState#FOCUSED}, {@link AutofocusState#FAILED}, or {@link AutofocusState#SCANNING} when
     *     the timeout ended first
     * @throws io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException on a camera without autofocus
     *     (Camera Module 2) and in {@link AutofocusMode#MANUAL}
     */
    public AutofocusState triggerAutofocus(Duration timeout) {
        int milliseconds = milliseconds(timeout);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment state = arena.allocate(JAVA_INT);
            check(library.triggerAutofocus(token, milliseconds, state), "keel_camera_trigger_autofocus");
            return AutofocusState.of(state.get(JAVA_INT, 0));
        }
    }

    /**
     * {@code keel_camera_get_stats}: the counters since open: frames captured, read and dropped, chunks encoded,
     * read and dropped, and the outputs.
     *
     * @return the counters
     */
    public CameraStats stats() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_STATS);
            check(library.getStats(token, raw), "keel_camera_get_stats");
            return CameraStructCodec.cameraStats(raw);
        }
    }

    /**
     * {@code keel_camera_last_error}: the latest failure on the handle, with its message.
     *
     * @return the record; its status is OK when no call failed
     */
    public ErrorReport lastError() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ERROR);
            check(library.lastError(token, raw), "keel_camera_last_error");
            return CameraStructCodec.cameraError(raw);
        }
    }

    // ----------------------------------------------------------------- outputs ----

    /**
     * Starts an output with every default ({@link OutputOptions#defaults()}).
     *
     * @param destination see {@link #startOutput(String, OutputOptions)}
     * @return the output's id
     */
    public int startOutput(String destination) {
        return startOutput(destination, OutputOptions.defaults());
    }

    /**
     * Records to a file with every default.
     *
     * @param file a path ending in {@code .h264}, {@code .264}, {@code .mjpeg} or {@code .mjpg}, and with
     *     libavformat built in also {@code .mp4}, {@code .mkv}, {@code .mov} or {@code .ts}
     * @return the output's id
     */
    public int startOutput(Path file) {
        return startOutput(file.toString(), OutputOptions.defaults());
    }

    /**
     * {@code keel_camera_output_start}: makes the library write the encoded stream to a destination.
     *
     * <p>Destinations: {@code tcp://HOST:PORT} connects to a receiver; {@code tcp://:PORT?listen} waits for up to
     * {@link OutputOptions#maxClients()} receivers (4 by default) and serves each from its next keyframe;
     * {@code udp://HOST:PORT} sends datagrams of at most {@link OutputOptions#datagramBytes()} (1472 by default); a
     * path ending in {@code .h264} or {@code .264} (H.264), or {@code .mjpeg} or {@code .mjpg} (MJPEG). With FFmpeg's
     * libavformat built in, also {@code .mp4}, {@code .mkv}, {@code .mov} and {@code .ts} files and {@code rtsp://},
     * {@code rtmp://} and {@code srt://} URLs; without it these throw
     * {@link io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException}.
     *
     * <p>Needs an encoder. Opens the file, connects (up to 3 s) or binds the port at once, so those errors throw
     * here; a failure later puts the output in {@code OutputState.FAILED} ({@link #outputStats(int)}) without
     * stopping the capture.
     *
     * @param destination the destination, at most 255 bytes in UTF-8
     * @param options the output options
     * @return the output's id
     * @throws IllegalArgumentException if the destination does not fit or contains a NUL
     */
    public int startOutput(String destination, OutputOptions options) {
        Objects.requireNonNull(options, "options");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment url = arena.allocateFrom(checkText("destination", destination));
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_OUTPUT_OPTIONS);
            CameraStructCodec.writeOutputOptions(raw, 0, options.queue(), options.maxClients(),
                    options.datagramBytes());
            MemorySegment out = arena.allocate(JAVA_INT);
            check(library.outputStart(token, url, raw, out), "keel_camera_output_start");
            return out.get(JAVA_INT, 0);
        }
    }

    /**
     * {@code keel_camera_output_stop}: stops an output, sends or writes what it has queued, finishes its file and
     * closes its sockets (up to about 3 s).
     *
     * @param output the id from {@link #startOutput(String, OutputOptions)}
     * @throws CameraException ({@code KEEL_E_PERIPHERAL}) when the output had failed or its file could not be
     *     finished; it is stopped anyway
     */
    public void stopOutput(int output) {
        check(library.outputStop(token, output), "keel_camera_output_stop");
    }

    /**
     * {@code keel_camera_get_output_stats}: the state and counters of one output.
     *
     * @param output the id from {@link #startOutput(String, OutputOptions)}
     * @return the counters: clients, bytes and chunks sent, chunks dropped, and any failure
     */
    public OutputStats outputStats(int output) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_OUTPUT_STATS);
            check(library.getOutputStats(token, output, raw), "keel_camera_get_output_stats");
            return CameraStructCodec.cameraOutputStats(raw);
        }
    }

    // ------------------------------------------------------------------ stills ----

    /**
     * Captures one full-resolution JPEG still.
     *
     * @return the still
     */
    public StillImage captureStill() {
        return captureStill(StillOptions.defaults());
    }

    /**
     * {@code keel_camera_capture_still}: captures one JPEG image. Exposure, white balance and, on an autofocus
     * camera, focus settle first, so a still takes about a second. Up to 1920x1920 the hardware JPEG encoder does the
     * work, above it libjpeg ({@link StillInfo#software()}). Needs the capture stopped; the next {@link #start()}
     * returns to the video stream.
     *
     * @param options the still options
     * @return the still
     * @throws io.github.bisocmorg.keel.camera.errors.CameraStateException while capture is running
     */
    public StillImage captureStill(StillOptions options) {
        Objects.requireNonNull(options, "options");
        long width = options.width() == 0 ? info.pixelArrayWidth() : options.width();
        long height = options.height() == 0 ? info.pixelArrayHeight() : options.height();
        long size = (width + 15) / 16 * 16 * ((height + 15) / 16 * 16) * 3 + 2048;
        ByteBuffer buffer = ByteBuffer.allocateDirect((int) Math.min(size, Integer.MAX_VALUE - 8));
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment raw = stillOptions(arena, options);
            MemorySegment out = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_STILL_INFO);
            check(library.captureStill(token, raw, MemorySegment.ofBuffer(buffer), buffer.capacity(), out),
                    "keel_camera_capture_still");
            StillInfo still = CameraStructCodec.cameraStillInfo(out);
            byte[] jpeg = new byte[(int) still.bytes()];
            buffer.get(0, jpeg);
            return new StillImage(still, jpeg);
        }
    }

    /**
     * Captures one full-resolution JPEG still into a file.
     *
     * @param path the file, created or replaced
     * @return the size, flags and timestamp of the still
     */
    public StillInfo captureStillFile(Path path) {
        return captureStillFile(path, StillOptions.defaults());
    }

    /**
     * {@code keel_camera_capture_still_file}: captures one JPEG image into a file, created or replaced; as
     * {@link #captureStill(StillOptions)}.
     *
     * @param path the file, at most 255 bytes in UTF-8
     * @param options the still options
     * @return the size, flags and timestamp of the still
     * @throws IllegalArgumentException if the path does not fit or contains a NUL
     */
    public StillInfo captureStillFile(Path path, StillOptions options) {
        Objects.requireNonNull(path, "path");
        Objects.requireNonNull(options, "options");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment file = arena.allocateFrom(checkText("path", path.toString()));
            MemorySegment raw = stillOptions(arena, options);
            MemorySegment out = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_STILL_INFO);
            check(library.captureStillFile(token, raw, file, out), "keel_camera_capture_still_file");
            return CameraStructCodec.cameraStillInfo(out);
        }
    }

    private static MemorySegment stillOptions(Arena arena, StillOptions options) {
        MemorySegment raw = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_STILL_OPTIONS);
        CameraStructCodec.writeStillOptions(raw, options.width(), options.height(), options.quality(),
                milliseconds(options.timeout()));
        return raw;
    }

    // ------------------------------------------------------------------ helpers ----

    /**
     * A timeout as whole milliseconds, rounded up, as the bit pattern of a {@code uint32_t}.
     *
     * @param timeout the timeout
     * @return milliseconds
     */
    static int milliseconds(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative()) {
            throw new IllegalArgumentException("timeout must not be negative, not " + timeout);
        }
        long milliseconds = timeout.toMillis();
        if (timeout.minusMillis(milliseconds).isPositive()) {
            milliseconds++;
        }
        if (milliseconds > 0xFFFF_FFFFL) {
            throw new IllegalArgumentException("timeout " + timeout + " does not fit uint32_t milliseconds");
        }
        return (int) milliseconds;
    }

    private static String checkText(String name, String text) {
        Objects.requireNonNull(text, name);
        if (text.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(name + " contains a NUL character");
        }
        int length = text.getBytes(StandardCharsets.UTF_8).length;
        if (length >= CameraLimits.PATH_MAX) {
            throw new IllegalArgumentException(name + " is " + length + " bytes in UTF-8; at most "
                    + (CameraLimits.PATH_MAX - 1) + " fit");
        }
        return text;
    }

    private static void ensureShutdownHook() {
        if (HOOK.compareAndSet(false, true)) {
            try {
                Runtime.getRuntime().addShutdownHook(Thread.ofPlatform().name("keel-camera-shutdown")
                        .unstarted(() -> {
                            for (Closer open : List.copyOf(OPEN)) {
                                open.close(false);
                            }
                        }));
            } catch (IllegalStateException shuttingDown) {
                LOG.log(System.Logger.Level.DEBUG, "the JVM is shutting down; no camera shutdown hook registered");
            }
        }
    }

    /** The iterator of {@link #frames(Duration)} and {@link #chunks(Duration)}. */
    private final class Reader<T> implements Iterator<T> {
        private final Supplier<T> read;
        private T next;
        private boolean ended;

        Reader(Supplier<T> read) {
            this.read = read;
        }

        @Override
        public boolean hasNext() {
            if (next != null) {
                return true;
            }
            if (ended) {
                return false;
            }
            try {
                next = read.get();
                return true;
            } catch (CameraClosedException closed) {
                if (isClosed()) {
                    ended = true;
                    return false;
                }
                throw closed;
            }
        }

        @Override
        public T next() {
            if (!hasNext()) {
                throw new NoSuchElementException("the camera is closed");
            }
            T item = next;
            next = null;
            return item;
        }
    }

    /**
     * The part of a camera that outlives it: the handle and the close, which the cleaner, the shutdown hook and
     * {@link #close()} share. It never refers to the {@code Camera}, so an unclosed camera can be collected.
     */
    private static final class Closer implements Runnable {
        private final CameraNativeLibrary library;
        private final MemorySegment token;
        private final CountDownLatch done = new CountDownLatch(1);
        private final AtomicBoolean closing = new AtomicBoolean();
        private volatile String reason;
        private Cleaner.Cleanable cleanable;

        Closer(CameraNativeLibrary library, MemorySegment token) {
            this.library = library;
            this.token = token;
        }

        void close(boolean explicit) {
            if (reason == null) {
                reason = explicit ? "" : "Camera was still open at JVM exit; the shutdown hook closed it";
            }
            closing.set(true);
            cleanable.clean();
            boolean interrupted = false;
            while (true) {
                try {
                    done.await();
                    break;
                } catch (InterruptedException interruption) {
                    interrupted = true;
                }
            }
            if (interrupted) {
                Thread.currentThread().interrupt();
            }
        }

        @Override
        public void run() {
            try {
                closing.set(true);
                String why = reason;
                if (why == null) {
                    LOG.log(System.Logger.Level.WARNING,
                            "Camera was not closed; the cleaner closed it. Close cameras with try-with-resources.");
                } else if (!why.isEmpty()) {
                    LOG.log(System.Logger.Level.WARNING, why);
                }
                int status = library.close(token);
                if (status != 0) {
                    LOG.log(System.Logger.Level.WARNING, "keel_camera_close returned {0}",
                            CameraException.statusName(status));
                }
            } catch (RuntimeException | Error failure) {
                LOG.log(System.Logger.Level.ERROR, "closing the camera failed", failure);
            } finally {
                OPEN.remove(this);
                done.countDown();
            }
        }
    }
}
