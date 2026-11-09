// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;

import io.github.bisocmorg.keel.Abi;
import io.github.bisocmorg.keel.errors.LibraryException;
import java.lang.foreign.FunctionDescriptor;
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
 * The loaded libkeel: one downcall handle per exported function, and a method per handle that returns the raw
 * result.
 *
 * <p>The library is loaded once, under a lock, on first use or through {@link #load(Path)}. A successful load is
 * kept for the life of the process; a failed one is not, so a corrected {@code KEEL_LIBRARY} takes effect on the next
 * attempt. Calls read the published instance without taking the lock. No call is {@code Linker.Option.critical}:
 * every device call may wait on a libkeel lock.
 */
public final class NativeLibrary {
    private static final Object LOCK = new Object();
    private static volatile NativeLibrary instance;
    private static volatile NativeCallInterceptor interceptor;

    private final String library;
    private final MethodHandle abiVersion;
    private final MethodHandle versionString;
    private final MethodHandle statusName;
    private final MethodHandle statusString;
    private final MethodHandle statusClass;
    private final MethodHandle faultCauseName;
    private final MethodHandle open;
    private final MethodHandle close;
    private final MethodHandle getCapabilities;
    private final MethodHandle nowUs;
    private final MethodHandle sleepUntil;
    private final MethodHandle tick;
    private final MethodHandle cycle;
    private final MethodHandle requestArm;
    private final MethodHandle disarm;
    private final MethodHandle emergencyStop;
    private final MethodHandle clearFault;
    private final MethodHandle getState;
    private final MethodHandle getSnapshot;
    private final MethodHandle getFault;
    private final MethodHandle readSafetyInputs;
    private final MethodHandle setMotors;
    private final MethodHandle brake;
    private final MethodHandle servoStart;
    private final MethodHandle servoSetPulse;
    private final MethodHandle servoEnd;
    private final MethodHandle rangeStart;
    private final MethodHandle rangePoll;
    private final MethodHandle rangeMeasure;
    private final MethodHandle setRgb;
    private final MethodHandle setRgbAll;
    private final MethodHandle setRgbFrame;
    private final MethodHandle getRgb;
    private final MethodHandle setButtonLeds;
    private final MethodHandle takeButtons;
    private final MethodHandle simSet;
    private final MethodHandle simGetState;
    private final MethodHandle gettid;

    /**
     * Binds every function of {@link Layouts#FUNCTIONS} and {@code gettid}.
     *
     * @param symbols the library's symbols
     * @param library a description of the library, for messages
     * @throws LibraryException if a symbol is missing, the JVM has no native linker or native access is denied
     */
    // Linker.downcallHandle is a restricted method; the application grants native access to this module or to the
    // class path (README, "Native access").
    @SuppressWarnings("restricted")
    public NativeLibrary(SymbolLookup symbols, String library) {
        this.library = library;
        Map<String, MethodHandle> handles = new HashMap<>();
        try {
            Linker linker = Linker.nativeLinker();
            for (Layouts.Signature signature : Layouts.FUNCTIONS) {
                MemorySegment address = symbols.find(signature.name()).orElseThrow(() -> new LibraryException(
                        library + " does not export " + signature.name() + "; it is not a libkeel 1.x build"));
                handles.put(signature.name(), linker.downcallHandle(address, signature.descriptor()));
            }
            Optional<MemorySegment> gettidAddress = linker.defaultLookup().find("gettid");
            gettid = linker.downcallHandle(gettidAddress.orElseThrow(() -> new LibraryException(
                    "the C library has no gettid (glibc 2.30 or later is required)")), FunctionDescriptor.of(JAVA_INT));
        } catch (UnsupportedOperationException error) {
            throw new LibraryException("this JVM has no native linker for " + System.getProperty("os.arch")
                    + "; the Keel binding needs the Foreign Function and Memory API", error);
        } catch (IllegalCallerException error) {
            throw LibraryLoader.nativeAccessDenied(error);
        }
        abiVersion = handles.get("keel_abi_version");
        versionString = handles.get("keel_version_string");
        statusName = handles.get("keel_status_name");
        statusString = handles.get("keel_status_string");
        statusClass = handles.get("keel_status_class");
        faultCauseName = handles.get("keel_fault_cause_name");
        open = handles.get("keel_open");
        close = handles.get("keel_close");
        getCapabilities = handles.get("keel_get_capabilities");
        nowUs = handles.get("keel_now_us");
        sleepUntil = handles.get("keel_sleep_until");
        tick = handles.get("keel_tick");
        cycle = handles.get("keel_cycle");
        requestArm = handles.get("keel_request_arm");
        disarm = handles.get("keel_disarm");
        emergencyStop = handles.get("keel_emergency_stop");
        clearFault = handles.get("keel_clear_fault");
        getState = handles.get("keel_get_state");
        getSnapshot = handles.get("keel_get_snapshot");
        getFault = handles.get("keel_get_fault");
        readSafetyInputs = handles.get("keel_read_safety_inputs");
        setMotors = handles.get("keel_set_motors");
        brake = handles.get("keel_brake");
        servoStart = handles.get("keel_servo_start");
        servoSetPulse = handles.get("keel_servo_set_pulse");
        servoEnd = handles.get("keel_servo_end");
        rangeStart = handles.get("keel_range_start");
        rangePoll = handles.get("keel_range_poll");
        rangeMeasure = handles.get("keel_range_measure");
        setRgb = handles.get("keel_set_rgb");
        setRgbAll = handles.get("keel_set_rgb_all");
        setRgbFrame = handles.get("keel_set_rgb_frame");
        getRgb = handles.get("keel_get_rgb");
        setButtonLeds = handles.get("keel_set_button_leds");
        takeButtons = handles.get("keel_take_buttons");
        simSet = handles.get("keel_sim_set");
        simGetState = handles.get("keel_sim_get_state");
    }

    // ------------------------------------------------------------------ loading ----

    /**
     * Returns the loaded library, loading it on first use.
     *
     * @return the library
     * @throws LibraryException if the library cannot be loaded or is not compatible
     */
    public static NativeLibrary get() {
        NativeLibrary current = instance;
        return current != null ? current : load(null);
    }

    /**
     * Loads the library once: from {@code path}, else from {@code KEEL_LIBRARY}, else by SONAME.
     *
     * @param path an absolute path, or {@code null} to resolve the library as {@link LibraryLoader} describes
     * @return the library; the one already loaded when {@code path} is {@code null} or names it
     * @throws LibraryException if the library cannot be loaded, is not compatible, or another one is already loaded
     */
    public static NativeLibrary load(Path path) {
        synchronized (LOCK) {
            NativeLibrary current = instance;
            if (current != null) {
                if (path != null && !current.library.equals(path.toString())) {
                    throw new LibraryException("libkeel is already loaded from " + current.library
                            + "; it stays loaded for the life of the process and cannot be replaced by " + path);
                }
                return current;
            }
            LibraryLoader.checkPlatform(System.getProperty("os.name", ""));
            Path resolved = path != null ? LibraryLoader.requireAbsolute(path, "Keel.load")
                    : LibraryLoader.resolve(System.getenv(LibraryLoader.ENVIRONMENT));
            String library = resolved == null ? LibraryLoader.SONAME : resolved.toString();
            NativeLibrary loaded = new NativeLibrary(LibraryLoader.open(resolved), library);
            LibraryLoader.checkAbi(loaded.abiVersion(), Abi.MAJOR, Abi.MINOR, LibraryLoader.describe(resolved));
            instance = loaded;
            return loaded;
        }
    }

    /**
     * Returns the library that is loaded, without loading one.
     *
     * @return the library, or empty before the first successful load
     */
    public static Optional<NativeLibrary> loaded() {
        return Optional.ofNullable(instance);
    }

    /**
     * Returns the library this instance was loaded from.
     *
     * @return the absolute path, or {@code libkeel.so.1} when the dynamic loader found it
     */
    public String library() {
        return library;
    }

    /**
     * Installs or removes the test interceptor.
     *
     * @param newInterceptor the interceptor, or {@code null} to remove it
     */
    public static void install(NativeCallInterceptor newInterceptor) {
        interceptor = newInterceptor;
    }

    /**
     * Returns the installed test interceptor.
     *
     * @return the interceptor, or empty when none is installed
     */
    public static Optional<NativeCallInterceptor> installed() {
        return Optional.ofNullable(interceptor);
    }

    /**
     * Rethrows what a downcall threw. Downcall handles throw no checked exceptions, so this only passes unchecked
     * ones on.
     *
     * @param thrown what the downcall threw
     * @return the unchecked exception to throw
     */
    public static RuntimeException rethrow(Throwable thrown) {
        if (thrown instanceof RuntimeException runtime) {
            return runtime;
        }
        if (thrown instanceof Error error) {
            throw error;
        }
        return new IllegalStateException("a downcall threw a checked exception", thrown);
    }

    private static void before(NativeCallInterceptor observer, String function, Object... arguments) {
        if (observer != null) {
            observer.before(function, arguments);
        }
    }

    private static int after(NativeCallInterceptor observer, String function, int status) {
        return observer == null ? status : observer.after(function, status);
    }

    /**
     * The kernel id of the calling thread, the id libkeel records as the control owner.
     *
     * @return {@code gettid()}
     */
    public long gettid() {
        try {
            return (int) gettid.invokeExact();
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ----------------------------------------------------------------- metadata ----

    /**
     * {@code keel_abi_version()}.
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
     * {@code keel_version_string()}.
     *
     * @return the static string's address
     */
    public MemorySegment versionString() {
        try {
            return (MemorySegment) versionString.invokeExact();
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_status_name(status)}.
     *
     * @param status any value
     * @return the static string's address
     */
    public MemorySegment statusName(int status) {
        try {
            return (MemorySegment) statusName.invokeExact(status);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_status_string(status)}.
     *
     * @param status any value
     * @return the static string's address
     */
    public MemorySegment statusString(int status) {
        try {
            return (MemorySegment) statusString.invokeExact(status);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_status_class(status)}.
     *
     * @param status any value
     * @return the class value; {@code KEEL_CLASS_FAULT} for undefined values
     */
    public int statusClass(int status) {
        try {
            return (int) statusClass.invokeExact(status);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_fault_cause_name(cause)}.
     *
     * @param cause any value
     * @return the static string's address
     */
    public MemorySegment faultCauseName(int cause) {
        try {
            return (MemorySegment) faultCauseName.invokeExact(cause);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ----------------------------------------------------------------- lifetime ----

    /**
     * {@code keel_open(options, out_device, report)}; on success the test interceptor sees the new device.
     *
     * @param options a {@code keel_open_options}
     * @param outDevice a pointer-sized cell that receives the handle
     * @param report a {@code keel_open_report}
     * @return the status
     */
    public int open(MemorySegment options, MemorySegment outDevice, MemorySegment report) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_open", options, outDevice, report);
        int status;
        try {
            status = (int) open.invokeExact(options, outDevice, report);
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
        if (status == 0 && observer != null) {
            observer.opened(outDevice.get(ADDRESS, 0), report);
        }
        return status;
    }

    /**
     * {@code keel_close(device, out_final_fault)}.
     *
     * @param device the handle
     * @param fault a {@code keel_fault}, or {@link MemorySegment#NULL}
     * @return the status
     */
    public int close(MemorySegment device, MemorySegment fault) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_close", device, fault);
        try {
            return after(observer, "keel_close", (int) close.invokeExact(device, fault));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_get_capabilities(device, out)}.
     *
     * @param device the handle
     * @param out a {@code uint32_t}
     * @return the status
     */
    public int getCapabilities(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_get_capabilities", device, out);
        try {
            return after(observer, "keel_get_capabilities", (int) getCapabilities.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // -------------------------------------------------------------------- clock ----

    /**
     * {@code keel_now_us(device, out)}.
     *
     * @param device the handle
     * @param out a {@code uint64_t}
     * @return the status
     */
    public int nowUs(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_now_us", device, out);
        try {
            return after(observer, "keel_now_us", (int) nowUs.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_sleep_until(device, deadline_us)}.
     *
     * @param device the handle
     * @param deadlineUs the absolute device time
     * @return the status
     */
    public int sleepUntil(MemorySegment device, long deadlineUs) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_sleep_until", device, deadlineUs);
        try {
            return after(observer, "keel_sleep_until", (int) sleepUntil.invokeExact(device, deadlineUs));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------- safety ----

    /**
     * {@code keel_tick(device, cycle_id, health)}.
     *
     * @param device the handle
     * @param cycleId the cycle id
     * @param health the health mask
     * @return the status
     */
    public int tick(MemorySegment device, long cycleId, int health) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_tick", device, cycleId, health);
        try {
            return after(observer, "keel_tick", (int) tick.invokeExact(device, cycleId, health));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_cycle(device, in, out)}.
     *
     * @param device the handle
     * @param in a {@code keel_cycle_input}
     * @param out a {@code keel_snapshot}, or {@link MemorySegment#NULL}
     * @return the status
     */
    public int cycle(MemorySegment device, MemorySegment in, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_cycle", device, in, out);
        try {
            return after(observer, "keel_cycle", (int) cycle.invokeExact(device, in, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_request_arm(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int requestArm(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_request_arm", device);
        try {
            return after(observer, "keel_request_arm", (int) requestArm.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_disarm(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int disarm(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_disarm", device);
        try {
            return after(observer, "keel_disarm", (int) disarm.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_emergency_stop(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int emergencyStop(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_emergency_stop", device);
        try {
            return after(observer, "keel_emergency_stop", (int) emergencyStop.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_clear_fault(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int clearFault(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_clear_fault", device);
        try {
            return after(observer, "keel_clear_fault", (int) clearFault.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_get_state(device, out)}.
     *
     * @param device the handle
     * @param out a {@code uint32_t}
     * @return the status
     */
    public int getState(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_get_state", device, out);
        try {
            return after(observer, "keel_get_state", (int) getState.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_get_snapshot(device, out)}.
     *
     * @param device the handle
     * @param out a {@code keel_snapshot}
     * @return the status
     */
    public int getSnapshot(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_get_snapshot", device, out);
        try {
            return after(observer, "keel_get_snapshot", (int) getSnapshot.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_get_fault(device, out)}.
     *
     * @param device the handle
     * @param out a {@code keel_fault}
     * @return the status
     */
    public int getFault(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_get_fault", device, out);
        try {
            return after(observer, "keel_get_fault", (int) getFault.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_read_safety_inputs(device, out)}.
     *
     * @param device the handle
     * @param out a {@code uint32_t}
     * @return the status
     */
    public int readSafetyInputs(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_read_safety_inputs", device, out);
        try {
            return after(observer, "keel_read_safety_inputs", (int) readSafetyInputs.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------- motion ----

    /**
     * {@code keel_set_motors(device, left, right)}.
     *
     * @param device the handle
     * @param left the left duty, permille
     * @param right the right duty, permille
     * @return the status
     */
    public int setMotors(MemorySegment device, int left, int right) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_set_motors", device, left, right);
        try {
            return after(observer, "keel_set_motors", (int) setMotors.invokeExact(device, left, right));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_brake(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int brake(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_brake", device);
        try {
            return after(observer, "keel_brake", (int) brake.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // -------------------------------------------------------------------- servo ----

    /**
     * {@code keel_servo_start(device, pulse_us, lease_ms)}.
     *
     * @param device the handle
     * @param pulseUs the pulse width
     * @param leaseMs the burst length
     * @return the status
     */
    public int servoStart(MemorySegment device, int pulseUs, int leaseMs) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_servo_start", device, pulseUs, leaseMs);
        try {
            return after(observer, "keel_servo_start", (int) servoStart.invokeExact(device, pulseUs, leaseMs));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_servo_set_pulse(device, pulse_us)}.
     *
     * @param device the handle
     * @param pulseUs the pulse width
     * @return the status
     */
    public int servoSetPulse(MemorySegment device, int pulseUs) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_servo_set_pulse", device, pulseUs);
        try {
            return after(observer, "keel_servo_set_pulse", (int) servoSetPulse.invokeExact(device, pulseUs));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_servo_end(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int servoEnd(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_servo_end", device);
        try {
            return after(observer, "keel_servo_end", (int) servoEnd.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ------------------------------------------------------------------ ranging ----

    /**
     * {@code keel_range_start(device)}.
     *
     * @param device the handle
     * @return the status
     */
    public int rangeStart(MemorySegment device) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_range_start", device);
        try {
            return after(observer, "keel_range_start", (int) rangeStart.invokeExact(device));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_range_poll(device, out)}.
     *
     * @param device the handle
     * @param out a {@code keel_range}
     * @return the status
     */
    public int rangePoll(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_range_poll", device, out);
        try {
            return after(observer, "keel_range_poll", (int) rangePoll.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_range_measure(device, out)}.
     *
     * @param device the handle
     * @param out a {@code keel_range}
     * @return the status
     */
    public int rangeMeasure(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_range_measure", device, out);
        try {
            return after(observer, "keel_range_measure", (int) rangeMeasure.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // --------------------------------------------------------------------- LEDs ----

    /**
     * {@code keel_set_rgb(device, index, rgb24)}.
     *
     * @param device the handle
     * @param index the LED index
     * @param rgb24 the colour
     * @return the status
     */
    public int setRgb(MemorySegment device, int index, int rgb24) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_set_rgb", device, index, rgb24);
        try {
            return after(observer, "keel_set_rgb", (int) setRgb.invokeExact(device, index, rgb24));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_set_rgb_all(device, rgb24)}.
     *
     * @param device the handle
     * @param rgb24 the colour
     * @return the status
     */
    public int setRgbAll(MemorySegment device, int rgb24) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_set_rgb_all", device, rgb24);
        try {
            return after(observer, "keel_set_rgb_all", (int) setRgbAll.invokeExact(device, rgb24));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_set_rgb_frame(device, rgb24, count)}.
     *
     * @param device the handle
     * @param rgb24 an array of {@code uint32_t} colours
     * @param count the number of colours
     * @return the status
     */
    public int setRgbFrame(MemorySegment device, MemorySegment rgb24, int count) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_set_rgb_frame", device, rgb24, count);
        try {
            return after(observer, "keel_set_rgb_frame", (int) setRgbFrame.invokeExact(device, rgb24, count));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_get_rgb(device, index, out)}.
     *
     * @param device the handle
     * @param index the LED index
     * @param out a {@code uint32_t}
     * @return the status
     */
    public int getRgb(MemorySegment device, int index, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_get_rgb", device, index, out);
        try {
            return after(observer, "keel_get_rgb", (int) getRgb.invokeExact(device, index, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_set_button_leds(device, mask, values)}.
     *
     * @param device the handle
     * @param mask the LEDs to change
     * @param values their new levels
     * @return the status
     */
    public int setButtonLeds(MemorySegment device, int mask, int values) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_set_button_leds", device, mask, values);
        try {
            return after(observer, "keel_set_button_leds", (int) setButtonLeds.invokeExact(device, mask, values));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_take_buttons(device, out)}.
     *
     * @param device the handle
     * @param out a {@code keel_buttons}
     * @return the status
     */
    public int takeButtons(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_take_buttons", device, out);
        try {
            return after(observer, "keel_take_buttons", (int) takeButtons.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    // ---------------------------------------------------------------- simulator ----

    /**
     * {@code keel_sim_set(device, item, value)}.
     *
     * @param device the handle
     * @param item the stimulus item
     * @param value its value
     * @return the status
     */
    public int simSet(MemorySegment device, int item, int value) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_sim_set", device, item, value);
        try {
            return after(observer, "keel_sim_set", (int) simSet.invokeExact(device, item, value));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * {@code keel_sim_get_state(device, out)}.
     *
     * @param device the handle
     * @param out a {@code keel_sim_state}
     * @return the status
     */
    public int simGetState(MemorySegment device, MemorySegment out) {
        NativeCallInterceptor observer = interceptor;
        before(observer, "keel_sim_get_state", device, out);
        try {
            return after(observer, "keel_sim_get_state", (int) simGetState.invokeExact(device, out));
        } catch (Throwable thrown) {
            throw rethrow(thrown);
        }
    }

    /**
     * The names of the functions this instance bound, in header order.
     *
     * @return the C names of {@link Layouts#FUNCTIONS}
     */
    public static List<String> functionNames() {
        return Layouts.FUNCTIONS.stream().map(Layouts.Signature::name).toList();
    }
}
