// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.SetupException;
import io.github.bisocmorg.keel.errors.StatusClass;
import io.github.bisocmorg.keel.internal.HeaderVersion;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import io.github.bisocmorg.keel.internal.NativeStringReader;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.task.KeelTask;
import io.github.bisocmorg.keel.task.TaskOptions;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The entry point: loading libkeel, the library's metadata, and opening a device.
 *
 * <p>The library is loaded on first use, or explicitly with {@link #load()} or {@link #load(Path)}. Class
 * initialisation never touches it, so documentation builds and packaging work without it. Resolution order:
 * {@code KEEL_LIBRARY} (an absolute path; empty counts as unset), then the SONAME {@code libkeel.so.1} through the
 * dynamic loader. On anything but Linux, loading fails with a {@link LibraryException} before any library is opened.
 *
 * <p>Metadata methods are thread-safe and do not block. {@link #open(OpenOptions)} takes up to about 100 ms on the
 * robot. The {@code run} methods execute a single-use task on the calling platform thread and block until the
 * student's sequence returns or fails; its hardware and camera resources are scoped to that execution.
 */
public final class Keel {
    /** The version of this binding: that of the {@code keel.h} it was generated from, for example {@code "1.0.0"}. */
    public static final String VERSION = HeaderVersion.TEXT;

    private static final Map<Integer, String> NAMES = new ConcurrentHashMap<>();
    private static final Map<Integer, String> DESCRIPTIONS = new ConcurrentHashMap<>();
    private static final Map<Integer, String> CAUSES = new ConcurrentHashMap<>();
    private static volatile String versionString;

    private Keel() {
    }

    /**
     * Executes one sequential student task with default resource options and finite time limits.
     * Hardware is automatically opened and closed; the first nonzero movement performs arming.
     *
     * @param task the single-use task to run on this platform thread
     * @throws NullPointerException if task is null
     * @throws Exception the original student failure or a task/native failure
     */
    public static void run(KeelTask task) throws Exception {
        run(task, TaskOptions.defaults());
    }

    /**
     * Executes one sequential task with explicit options. All task and camera work stays on this platform
     * thread; the controller independently enforces deadlines and removes drive between bounded actions.
     *
     * @param task the single-use task
     * @param options resource configuration and millisecond limits
     * @throws NullPointerException if either argument is null
     * @throws Exception the original student failure or a task/native failure
     */
    public static void run(KeelTask task, TaskOptions options) throws Exception {
        Objects.requireNonNull(task, "task").execute(options);
    }

    /**
     * Loads libkeel now, from {@code KEEL_LIBRARY} or by SONAME, and checks its ABI. Loading happens once; later
     * calls return at once.
     *
     * @throws LibraryException if the platform is not Linux, the library cannot be opened, a symbol is missing, the
     *     ABI is incompatible or native access is denied
     */
    public static void load() {
        NativeLibrary.load(null);
    }

    /**
     * Loads libkeel now from an absolute path, ignoring {@code KEEL_LIBRARY}, and checks its ABI.
     *
     * @param path the absolute path of {@code libkeel.so.1}
     * @throws LibraryException as {@link #load()}, if the path is relative, or if another library is already loaded
     */
    public static void load(Path path) {
        NativeLibrary.load(Objects.requireNonNull(path, "path"));
    }

    /**
     * {@code keel_abi_version()}: the ABI of the loaded library.
     *
     * @return the major and minor version
     */
    public static AbiVersion abiVersion() {
        int packed = NativeLibrary.get().abiVersion();
        return new AbiVersion(packed >>> 16, packed & 0xFFFF);
    }

    /**
     * {@code keel_version_string()}: the build identification of the loaded library.
     *
     * @return for example {@code "1.0.0 (aarch64-linux-gnu; backends r3,simulator)"}
     */
    public static String versionString() {
        String text = versionString;
        if (text == null) {
            text = NativeStringReader.read(NativeLibrary.get().versionString(), "keel_version_string");
            versionString = text;
        }
        return text;
    }

    /**
     * {@code keel_status_name(status)}: the symbolic name of a status code.
     *
     * @param status any value
     * @return for example {@code "KEEL_E_IO"}; {@code "KEEL_E_UNKNOWN"} for a value the library does not define
     */
    public static String statusName(int status) {
        return NAMES.computeIfAbsent(status,
                code -> NativeStringReader.read(NativeLibrary.get().statusName(code), "keel_status_name"));
    }

    /**
     * {@code keel_status_string(status)}: a one-sentence description that names the remedy where there is one.
     *
     * @param status any value
     * @return the description
     */
    public static String statusString(int status) {
        return DESCRIPTIONS.computeIfAbsent(status,
                code -> NativeStringReader.read(NativeLibrary.get().statusString(code), "keel_status_string"));
    }

    /**
     * {@code keel_status_class(status)}: the class of a status code.
     *
     * @param status any value
     * @return the class; {@link StatusClass#FAULT} for a value the library does not define
     */
    public static StatusClass statusClass(int status) {
        return StatusClass.of(NativeLibrary.get().statusClass(status));
    }

    /**
     * {@code keel_fault_cause_name(cause)}: the symbolic name of a fault cause.
     *
     * @param cause any value
     * @return for example {@code "TICK_LATE"}, which equals the {@link FaultCause} member name; {@code "UNKNOWN"} for
     *     a value the library does not define
     */
    public static String faultCauseName(int cause) {
        return CAUSES.computeIfAbsent(cause,
                code -> NativeStringReader.read(NativeLibrary.get().faultCauseName(code), "keel_fault_cause_name"));
    }

    /**
     * {@code keel_open} with all defaults: the backend named by the environment variable {@code KEEL_BACKEND}, else
     * the robot's board when the library has that backend. It never falls back to the simulator silently.
     *
     * @return the open device; close it with try-with-resources
     * @throws SetupException for every failure, with the open report and its remedy
     * @throws LibraryException if libkeel cannot be loaded
     */
    public static Device open() {
        return open(OpenOptions.defaults());
    }

    /**
     * {@code keel_open} with options.
     *
     * <p>A device that opened degraded ({@link OpenReport#warnings()} not empty) is logged at WARNING with the
     * report's hint; the report stays available as {@link Device#report()}.
     *
     * @param options the options; zero and empty values mean "default", as in the C struct
     * @return the open device; close it with try-with-resources
     * @throws SetupException for every failure, with the open report and its remedy
     * @throws LibraryException if libkeel cannot be loaded
     */
    public static Device open(OpenOptions options) {
        Objects.requireNonNull(options, "options");
        return Device.open(NativeLibrary.get(), options.backend().code(), options.flagsMask(),
                options.commandTimeoutMs(), options.gpioChipPath().getBytes(StandardCharsets.UTF_8),
                options.i2cPath().getBytes(StandardCharsets.UTF_8));
    }
}
