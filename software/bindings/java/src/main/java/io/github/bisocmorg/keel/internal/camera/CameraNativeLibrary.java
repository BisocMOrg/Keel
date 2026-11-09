// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal.camera;

import io.github.bisocmorg.keel.camera.CameraAbi;
import io.github.bisocmorg.keel.camera.errors.CameraLibraryException;
import io.github.bisocmorg.keel.internal.NativeStringReader;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/**
 * The loaded libkeel-camera: one downcall handle per exported function, and a method per handle that returns the raw
 * result.
 *
 * <p>The camera library is loaded once, under a lock, on the first camera call or through {@link #load(Path)}, and
 * independently of libkeel.so: a program that only uses the camera never loads the board library.
 * {@link CameraLibraryLoader} finds it and checks its ABI. A failed load is not kept, so a corrected
 * {@code KEEL_CAMERA_LIBRARY} takes effect on the next attempt.
 */
public final class CameraNativeLibrary {
    private static final Object LOCK = new Object();
    private static volatile CameraNativeLibrary instance;

    private final String library;
    private final MethodHandle abiVersion;
    private final MethodHandle versionString;
    private final MethodHandle convertI420;
    private final MethodHandle listOpen;
    private final MethodHandle listCount;
    private final MethodHandle listGet;
    private final MethodHandle listGetMode;
    private final MethodHandle listClose;
    private final MethodHandle open;
    private final MethodHandle close;
    private final MethodHandle getInfo;
    private final MethodHandle getStream;
    private final MethodHandle setEncoder;
    private final MethodHandle start;
    private final MethodHandle stop;
    private final MethodHandle readFrame;
    private final MethodHandle readEncoded;
    private final MethodHandle triggerAutofocus;
    private final MethodHandle getStats;
    private final MethodHandle lastError;
    private final MethodHandle outputStart;
    private final MethodHandle outputStop;
    private final MethodHandle getOutputStats;
    private final MethodHandle captureStill;
    private final MethodHandle captureStillFile;
    private volatile String version;

    /**
     * Binds every function of {@link CameraLayouts#FUNCTIONS}.
     *
     * @param symbols the library's symbols
     * @param library a description of the library, for messages
     * @throws CameraLibraryException if a symbol is missing, the JVM has no native linker or native access is denied
     */
    // Linker.downcallHandle is a restricted method; the application grants native access to this module or to the
    // class path (README, "Native access").
    @SuppressWarnings("restricted")
    CameraNativeLibrary(SymbolLookup symbols, String library) {
        this.library = library;
        Map<String, MethodHandle> handles = new HashMap<>();
        try {
            Linker linker = Linker.nativeLinker();
            for (CameraLayouts.Signature signature : CameraLayouts.FUNCTIONS) {
                MemorySegment address = symbols.find(signature.name()).orElseThrow(() -> new CameraLibraryException(
                        library + " does not export " + signature.name() + "; it is not libkeel-camera 1.x"));
                handles.put(signature.name(), linker.downcallHandle(address, signature.descriptor()));
            }
        } catch (UnsupportedOperationException error) {
            throw new CameraLibraryException("this JVM has no native linker for " + System.getProperty("os.arch")
                    + "; the camera binding needs the Foreign Function and Memory API", error);
        } catch (IllegalCallerException error) {
            throw CameraLibraryLoader.nativeAccessDenied(error);
        }
        abiVersion = handles.get("keel_camera_abi_version");
        versionString = handles.get("keel_camera_version_string");
        convertI420 = handles.get("keel_camera_convert_i420");
        listOpen = handles.get("keel_camera_list_open");
        listCount = handles.get("keel_camera_list_count");
        listGet = handles.get("keel_camera_list_get");
        listGetMode = handles.get("keel_camera_list_get_mode");
        listClose = handles.get("keel_camera_list_close");
        open = handles.get("keel_camera_open");
        close = handles.get("keel_camera_close");
        getInfo = handles.get("keel_camera_get_info");
        getStream = handles.get("keel_camera_get_stream");
        setEncoder = handles.get("keel_camera_set_encoder");
        start = handles.get("keel_camera_start");
        stop = handles.get("keel_camera_stop");
        readFrame = handles.get("keel_camera_read_frame");
        readEncoded = handles.get("keel_camera_read_encoded");
        triggerAutofocus = handles.get("keel_camera_trigger_autofocus");
        getStats = handles.get("keel_camera_get_stats");
        lastError = handles.get("keel_camera_last_error");
        outputStart = handles.get("keel_camera_output_start");
        outputStop = handles.get("keel_camera_output_stop");
        getOutputStats = handles.get("keel_camera_get_output_stats");
        captureStill = handles.get("keel_camera_capture_still");
        captureStillFile = handles.get("keel_camera_capture_still_file");
    }

    // ------------------------------------------------------------------ loading ----

    /**
     * Returns the loaded camera library, loading it on first use.
     *
     * @return the library
     * @throws CameraLibraryException if the library cannot be loaded or is not compatible
     */
    public static CameraNativeLibrary get() {
        CameraNativeLibrary current = instance;
        return current != null ? current : load(null);
    }

    /**
     * Loads the camera library once: from {@code path}, else from {@code KEEL_CAMERA_LIBRARY}, else by SONAME.
     *
     * @param path an absolute path, or {@code null} to resolve the library as {@link CameraLibraryLoader} says
     * @return the library; the one already loaded when {@code path} is {@code null} or names it
     * @throws CameraLibraryException if the library cannot be loaded, is not compatible, or another one is loaded
     */
    public static CameraNativeLibrary load(Path path) {
        synchronized (LOCK) {
            CameraNativeLibrary current = instance;
            if (current != null) {
                if (path != null && !current.library.equals(path.toString())) {
                    throw new CameraLibraryException("libkeel-camera is already loaded from " + current.library
                            + "; it stays loaded for the life of the process and cannot be replaced by " + path);
                }
                return current;
            }
            CameraLibraryLoader.checkPlatform(System.getProperty("os.name", ""));
            Path resolved = path != null ? CameraLibraryLoader.requireAbsolute(path, "Camera.load")
                    : CameraLibraryLoader.resolve(System.getenv(CameraLibraryLoader.ENVIRONMENT));
            String library = CameraLibraryLoader.describe(resolved);
            CameraNativeLibrary loaded = new CameraNativeLibrary(CameraLibraryLoader.open(resolved), library);
            CameraLibraryLoader.checkAbi(loaded.abiVersion(), CameraAbi.MAJOR, CameraAbi.MINOR, library);
            instance = loaded;
            return loaded;
        }
    }

    /**
     * Returns the camera library that is loaded, without loading one.
     *
     * @return the library, or empty before the first successful load
     */
    public static Optional<CameraNativeLibrary> loaded() {
        return Optional.ofNullable(instance);
    }

    /**
     * Returns where this library was loaded from.
     *
     * @return the absolute path, or {@code libkeel-camera.so.1} when the dynamic loader found it
     */
    public String library() {
        return library;
    }

    /**
     * Returns the names of the functions the binding calls, in header order.
     *
     * @return the 25 names of {@code tests/abi/camera_symbols.txt}
     */
    public static List<String> functionNames() {
        return CameraLayouts.FUNCTIONS.stream().map(CameraLayouts.Signature::name).toList();
    }

    static RuntimeException rethrow(Throwable thrown) {
        if (thrown instanceof RuntimeException runtime) {
            return runtime;
        }
        if (thrown instanceof Error error) {
            throw error;
        }
        return new IllegalStateException("a downcall threw a checked exception", thrown);
    }

    // ----------------------------------------------------------------- metadata ----

    /**
     * {@code keel_camera_abi_version()}.
     *
     * @return the packed ABI version {@code (major << 16) | minor}
     */
    public int abiVersion() {
        try {
            return (int) abiVersion.invokeExact();
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_version_string()}, read once.
     *
     * @return the build identification
     */
    public String versionString() {
        String text = version;
        if (text == null) {
            try {
                text = NativeStringReader.read((MemorySegment) versionString.invokeExact(),
                        "keel_camera_version_string");
            } catch (Throwable thrown) {
                throw rethrow(thrown);
            }
            version = text;
        }
        return text;
    }

    /**
     * {@code keel_camera_convert_i420}.
     *
     * @param frame the I420 frame
     * @param layout its {@code keel_camera_frame_info}
     * @param out the destination
     * @param outSize the destination's size
     * @param outFormat the destination format
     * @param outStride the destination stride
     * @return the status
     */
    public int convertI420(MemorySegment frame, MemorySegment layout, MemorySegment out, int outSize, int outFormat,
            int outStride) {
        try {
            return (int) convertI420.invokeExact(frame, layout, out, outSize, outFormat, outStride);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------ listing ----

    /**
     * {@code keel_camera_list_open}.
     *
     * @param outList receives the list handle
     * @param error the error record
     * @return the status
     */
    public int listOpen(MemorySegment outList, MemorySegment error) {
        try {
            return (int) listOpen.invokeExact(outList, error);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_list_count}.
     *
     * @param list the list handle
     * @param outCount receives the count
     * @return the status
     */
    public int listCount(MemorySegment list, MemorySegment outCount) {
        try {
            return (int) listCount.invokeExact(list, outCount);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_list_get}.
     *
     * @param list the list handle
     * @param index the camera index
     * @param out the camera info
     * @return the status
     */
    public int listGet(MemorySegment list, int index, MemorySegment out) {
        try {
            return (int) listGet.invokeExact(list, index, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_list_get_mode}.
     *
     * @param list the list handle
     * @param index the camera index
     * @param mode the mode index
     * @param out the mode
     * @return the status
     */
    public int listGetMode(MemorySegment list, int index, int mode, MemorySegment out) {
        try {
            return (int) listGetMode.invokeExact(list, index, mode, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_list_close}.
     *
     * @param list the list handle
     * @return the status
     */
    public int listClose(MemorySegment list) {
        try {
            return (int) listClose.invokeExact(list);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ----------------------------------------------------------------- lifetime ----

    /**
     * {@code keel_camera_open}.
     *
     * @param options the options
     * @param outCamera receives the handle
     * @param error the error record
     * @return the status
     */
    public int open(MemorySegment options, MemorySegment outCamera, MemorySegment error) {
        try {
            return (int) open.invokeExact(options, outCamera, error);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_close}.
     *
     * @param camera the handle
     * @return the status
     */
    public int close(MemorySegment camera) {
        try {
            return (int) close.invokeExact(camera);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_get_info}.
     *
     * @param camera the handle
     * @param out the camera info
     * @return the status
     */
    public int getInfo(MemorySegment camera, MemorySegment out) {
        try {
            return (int) getInfo.invokeExact(camera, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_get_stream}.
     *
     * @param camera the handle
     * @param out the stream info
     * @return the status
     */
    public int getStream(MemorySegment camera, MemorySegment out) {
        try {
            return (int) getStream.invokeExact(camera, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_set_encoder}.
     *
     * @param camera the handle
     * @param options the encoder options
     * @return the status
     */
    public int setEncoder(MemorySegment camera, MemorySegment options) {
        try {
            return (int) setEncoder.invokeExact(camera, options);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------ capture ----

    /**
     * {@code keel_camera_start}.
     *
     * @param camera the handle
     * @return the status
     */
    public int start(MemorySegment camera) {
        try {
            return (int) start.invokeExact(camera);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_stop}.
     *
     * @param camera the handle
     * @return the status
     */
    public int stop(MemorySegment camera) {
        try {
            return (int) stop.invokeExact(camera);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_read_frame}.
     *
     * @param camera the handle
     * @param buffer the frame buffer
     * @param size its size
     * @param timeoutMs the longest wait
     * @param outInfo the frame info
     * @return the status
     */
    public int readFrame(MemorySegment camera, MemorySegment buffer, int size, int timeoutMs, MemorySegment outInfo) {
        try {
            return (int) readFrame.invokeExact(camera, buffer, size, timeoutMs, outInfo);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_read_encoded}.
     *
     * @param camera the handle
     * @param buffer the chunk buffer
     * @param size its size
     * @param timeoutMs the longest wait
     * @param outInfo the chunk info
     * @return the status
     */
    public int readEncoded(MemorySegment camera, MemorySegment buffer, int size, int timeoutMs,
            MemorySegment outInfo) {
        try {
            return (int) readEncoded.invokeExact(camera, buffer, size, timeoutMs, outInfo);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_trigger_autofocus}.
     *
     * @param camera the handle
     * @param timeoutMs the longest wait
     * @param outState receives the autofocus state
     * @return the status
     */
    public int triggerAutofocus(MemorySegment camera, int timeoutMs, MemorySegment outState) {
        try {
            return (int) triggerAutofocus.invokeExact(camera, timeoutMs, outState);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_get_stats}.
     *
     * @param camera the handle
     * @param out the counters
     * @return the status
     */
    public int getStats(MemorySegment camera, MemorySegment out) {
        try {
            return (int) getStats.invokeExact(camera, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_last_error}.
     *
     * @param camera the handle
     * @param out the error record
     * @return the status
     */
    public int lastError(MemorySegment camera, MemorySegment out) {
        try {
            return (int) lastError.invokeExact(camera, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------ outputs ----

    /**
     * {@code keel_camera_output_start}.
     *
     * @param camera the handle
     * @param url the destination, NUL-terminated
     * @param options the output options
     * @param outOutput receives the output id
     * @return the status
     */
    public int outputStart(MemorySegment camera, MemorySegment url, MemorySegment options, MemorySegment outOutput) {
        try {
            return (int) outputStart.invokeExact(camera, url, options, outOutput);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_output_stop}.
     *
     * @param camera the handle
     * @param output the output id
     * @return the status
     */
    public int outputStop(MemorySegment camera, int output) {
        try {
            return (int) outputStop.invokeExact(camera, output);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_get_output_stats}.
     *
     * @param camera the handle
     * @param output the output id
     * @param out the counters
     * @return the status
     */
    public int getOutputStats(MemorySegment camera, int output, MemorySegment out) {
        try {
            return (int) getOutputStats.invokeExact(camera, output, out);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------- stills ----

    /**
     * {@code keel_camera_capture_still}.
     *
     * @param camera the handle
     * @param options the still options
     * @param buffer the JPEG buffer
     * @param size its size
     * @param outInfo the still info
     * @return the status
     */
    public int captureStill(MemorySegment camera, MemorySegment options, MemorySegment buffer, int size,
            MemorySegment outInfo) {
        try {
            return (int) captureStill.invokeExact(camera, options, buffer, size, outInfo);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_camera_capture_still_file}.
     *
     * @param camera the handle
     * @param options the still options
     * @param path the file path, NUL-terminated
     * @param outInfo the still info
     * @return the status
     */
    public int captureStillFile(MemorySegment camera, MemorySegment options, MemorySegment path,
            MemorySegment outInfo) {
        try {
            return (int) captureStillFile.invokeExact(camera, options, path, outInfo);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }
}
