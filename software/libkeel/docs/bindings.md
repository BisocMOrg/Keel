<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 BisocM -->

# Binding rules

These rules apply to every language binding of libkeel: Python (`ctypes`), Java (the Foreign Function and
Memory API) and C# (`LibraryImport`). They are requirements, not suggestions: each one prevents a failure that
would otherwise show up as a stopped robot, a corrupted heap or a robot that keeps driving. The application-level
behaviour they wrap is described in [safety.md](safety.md); the header `include/keel/keel.h` is the contract.

The production bindings follow these rules and live in [`software/bindings`](../../bindings/README.md): the Python
package `keel-robot` (`import keel`, Debian package `python3-keel`) and the Java library `io.github.bisocmorg:keel`
(module `io.github.bisocmorg.keel`). Applications use those. No C# binding is built; the C# rules below are for
anyone who writes one.

`tests/consumers/keel_ctypes.py` is the minimal reference binding that libkeel's own tests use, and
`tests/consumers/test_ctypes.py` is its test against the simulator. Read them alongside this document.

## 1. What a binding wraps

- **Only `keel.h`.** `port.h` is a C porting interface; its functions are not exported from the shared library.
- **The camera is a library of its own.** `libkeel-camera.so.1` (`camera.h`) is bound the same way but loaded
  separately, on first camera use, from `KEEL_CAMERA_LIBRARY` or its SONAME, with its own ABI check
  (`keel_camera_abi_version() >> 16 == 1`); a missing camera library is reported then, never at import. The production
  bindings bind it as `keel.camera` and `io.github.bisocmorg.keel.camera` ([camera.md](camera.md), spec.md section
  26.2).
- **Nothing is compiled.** A binding is pure Java, C# or Python code that calls the system library.
- **No callbacks.** The library never calls into the runtime, so no upcall stubs, delegates or `CFUNCTYPE` are
  needed, and no library thread ever touches a managed object.

## 2. Loading the library

Resolve the library in this order, identically in every binding:

1. `KEEL_LIBRARY`, if set: an absolute path (a development tree, a test build). Refuse a relative path.
2. The SONAME `libkeel.so.1`, found by the dynamic loader. This is the only choice on the robot, where the
   `libkeel1` package installs it.
3. On a desktop, a simulator-only build bundled with the binding package (later releases).

Robot packages never bundle a copy of the library: there is exactly one copy of the safety code on a robot, and
`apt upgrade` updates every language at once.

Right after loading, call `keel_abi_version()` and require `version >> 16 == 1` (the major) and
`version & 0xFFFF` at least the minor that introduced the newest function the binding calls. Refuse to load
otherwise.

| Language | Loading |
|---|---|
| Python | `ctypes.CDLL(os.environ.get("KEEL_LIBRARY") or "libkeel.so.1")` |
| Java | `SymbolLookup.libraryLookup(Path.of(path), arena)` for `KEEL_LIBRARY`, else `SymbolLookup.libraryLookup("libkeel.so.1", arena)` |
| C# | `NativeLibrary.SetDllImportResolver`, returning `NativeLibrary.Load(KEEL_LIBRARY ?? "libkeel.so.1")` |

## 3. Declarations come from the header

- Generate everything from the preprocessed header, never from hand-copied numbers:

  ```sh
  cc -E -P -DKEEL_API= include/keel/keel.h
  ```

  Every constant is an enumerator, so it survives preprocessing; the only macros are the version numbers and
  `KEEL_API`. Python can parse the output with `pycparser` or a few regular expressions
  (`tests/abi/check_constants.py` evaluates every enumerator), Java with `jextract`, C# with ClangSharp or a small
  generator. Released values never change within 1.x (`tests/abi/constants.golden` guards them).
- Enum types never appear in signatures: values travel as `uint32_t` or `int32_t`. Keep them as plain integers
  in the binding, or wrap them in enums that tolerate unknown values (a newer library may return a cause the
  binding does not know yet).
- Parameter types are `uint32_t`, `int32_t`, `uint64_t`, `char` arrays and pointers only:

  | C | Python | Java | C# |
  |---|---|---|---|
  | `uint32_t` | `c_uint32` | `JAVA_INT` (read with `Integer.toUnsignedLong`) | `uint` |
  | `int32_t` | `c_int32` | `JAVA_INT` | `int` |
  | `uint64_t` | `c_uint64` | `JAVA_LONG` (unsigned) | `ulong` |
  | `keel_device *` | `c_void_p` | `ADDRESS` | `nint` inside a `SafeHandle` |
  | `const char *` (returned) | `c_void_p`, read with a bound | `ADDRESS`, `reinterpret(256)` | `nint`, `Marshal.PtrToStringUTF8` |
  | struct pointer | `POINTER(Structure)` | `ADDRESS` to a `MemorySegment` | `ref` a blittable struct, or a pointer |

- **The handle is a token**, not a pointer to memory: compare it for equality only, never dereference it. A
  closed, stale or forged handle returns `KEEL_E_INVALID_HANDLE`, never a crash.

## 4. Structs

Every struct starts with `uint32_t struct_size`:

- Set it to the size the binding was generated against and zero every other byte. Zero means "default" for
  every input field. A `struct_size` above `KEEL_STRUCT_SIZE_MAX` (4096) is taken for an uninitialised field and
  refused with `KEEL_E_INVALID_ARGUMENT`; the library then touches nothing past `struct_size`.
- A larger struct from a newer binding is accepted only if every byte the library does not know is zero;
  otherwise the call returns `KEEL_E_UNSUPPORTED` (`keel_open`: `KEEL_E_INVALID_ARGUMENT`) and does nothing. Set
  a field added in a later minor version only after checking that minor at load time.
- Outputs: the library writes min(`struct_size`, its own size) bytes and zeroes the rest. It leaves an output
  untouched only on `KEEL_E_INVALID_ARGUMENT`, `KEEL_E_INVALID_HANDLE` and `KEEL_E_NOT_OWNER`; for every other
  status the output is fully written, so a reused buffer never shows stale fields. The one exception is
  `keel_open`'s report: once its own `struct_size` is valid it is written for every status, including
  `KEEL_E_INVALID_ARGUMENT`. Raise the setup error with the report's hint, never discard it.
- Layouts have explicit padding and identical offsets on aarch64, armhf, x86-64 and i386 (`tests/abi/layout.c`
  pins every offset):

  | Struct | Size (bytes) |
  |---|---|
  | `keel_open_options` | 272 |
  | `keel_open_report` | 456 |
  | `keel_fault` | 64 |
  | `keel_snapshot` | 144 |
  | `keel_buttons` | 16 |
  | `keel_range` | 32 |
  | `keel_cycle_input` | 32 |
  | `keel_sim_state` | 48 |

- In garbage-collected runtimes, allocate the structs the loop uses once and reuse them.

## 5. Strings

Returned strings (`keel_version_string`, `keel_status_name`, `keel_status_string`, `keel_fault_cause_name`) are
static, NUL-terminated UTF-8, never NULL, and at most `KEEL_STRING_MAX` (256) bytes including the NUL. **Never
free them** and never let a marshaller free them (see C# below). Strings passed in (paths in
`keel_open_options`) are copied during the call.

## 6. Errors: by class, never by code

`keel_status_class(status)` gives one of six classes; map each class to one exception type. Do not branch on
individual codes, except to report `KEEL_E_INVALID_ARGUMENT` as the runtime's argument exception. Subtypes of the
class-level type that name individual codes are allowed, as long as an unknown code of the class still raises the
class-level type, so an application can always catch by class. Undefined values read as FAULT.

| Class | Python (`keel-robot`) | Java (`io.github.bisocmorg:keel`) | C# |
|---|---|---|---|
| REJECTED | `RejectedError`; per code `InvalidArgumentError` (also a `ValueError`), `StateError`, `AgainError`, `UnsupportedError`, `ConflictError` | `RejectedException`; per code `InvalidArgumentException`, `StateException`, `AgainException`, `UnsupportedException`, `ConflictException` | `ArgumentException`, `InvalidOperationException` |
| FAULT | `SafetyFaultError` (`.fault`) | `SafetyFaultException` (`fault()`) | `SafetyFaultException` |
| PERIPHERAL | `PeripheralError` | `PeripheralException` | `PeripheralException` |
| SETUP | `SetupError` (`.report`) | `SetupException` (`report()`) | `SetupException` |
| HANDLE | `HandleError` | `HandleException` | `ObjectDisposedException` |

- A FAULT exception carries the fault record: call `keel_get_fault` before raising.
- **`keel_open`**: every non-OK status means "no device"; raise the setup error with the open report, whatever
  the class of the code.
- **`keel_close`** never throws, from `close()`, `Dispose()`, a finalizer or a cleaner. Log a non-OK status.
- **`keel_cycle`**: `KEEL_E_INVALID_ARGUMENT` and `KEEL_E_UNSUPPORTED` mean the binding built a bad input; treat
  them as fatal for the loop (disarm, stop the loop, report).
- Inside the loop helper, REJECTED results are logged and counted rather than thrown; a HANDLE result stops the
  loop.

## 7. The control loop

The safety design rests on one thread that both runs the application's control step and ticks the device:

```
next = keel_now_us()
loop on a dedicated OS thread:
    next += 10 ms
    ok = step()                       # the user's code, inside try/catch, within its budget
    rc = keel_cycle({cycle_id: ++id, health: ok ? KEEL_HEALTH_ALL : 0, command, left, right, flags})
    dispatch rc by class
    keel_sleep_until(next)
```

- Run it on a **dedicated platform thread**: never a virtual thread, a thread pool, a timer or an async task.
  The first tick makes its OS thread the control owner, and a tick from another OS thread is a fault
  (`TICK_THREAD_CHANGED`); a Java virtual thread or an `await` continuation can resume on a different OS thread.
- Health is `KEEL_HEALTH_ALL` only if `step()` returned normally within its budget. Never offer an unbounded
  keepalive that continues arbitrary motion regardless of application progress. The bounded task supervisor
  described below is an explicit application control step, not a replacement for these checks.
- Use the device clock (`keel_now_us`, `keel_sleep_until`), never the language's sleep, so the same loop runs on
  the simulator's virtual clock.
- One call carries the cycle: `keel_cycle` ticks, applies the command and fills the snapshot, including the device
  time it was taken. Besides it, the loop needs only `keel_sleep_until` and the clock reads that measure the step's
  budget; keep the count that low, because every foreign call is a chance to wait (for the GIL in Python).
- Check the duties the application hands the loop against `KEEL_DUTY_MAX` and turn a violation into a failed step
  (health without `KEEL_HEALTH_NO_EXCEPTION`), and refuse an out-of-range intent on the posting thread, posting a
  coast instead. `KEEL_E_INVALID_ARGUMENT` from `keel_cycle` then keeps meaning a defect in the input the binding
  built, which is fatal for the loop. Pass-through calls stay unvalidated, so libkeel's refusal and its coast while
  ARMED apply.
- Report every fault latch once, recognised by the rise of `fault_count` in the snapshot rather than by the state,
  so a fault that latches again in the cycle that cleared the previous one is not missed.
- Offer a thread-safe `set_command(left, right)` that the next cycle sends: user-interface threads post intent,
  only the loop drives. Stop methods (`disarm`, `emergency_stop`, `close`, `set_motors(0, 0)`, `servo_end`) may
  be called from any thread.
- Map the stop button and Ctrl-C to `keel_emergency_stop`. The low-level loop never clears a fault or arms
  automatically. A task's explicit bounded movement call may request arming as described below.
- No libkeel function is async-signal-safe. Never call one from a native signal handler: set a flag there, or
  receive the signal on an ordinary thread (Java's and .NET's signal APIs and Python's handlers already run on
  ordinary threads), and call `keel_emergency_stop` from that thread.
- Close through try-with-resources, `using` or `with`; finalizers and cleaners are a last resort.
- Plan for a worst-case stall of 40 ms (`KEEL_STALL_BUDGET_MS`) per cycle while armed: garbage collection,
  JIT, GIL hand-offs and logging all count.

### Sequential task supervision

The Java and Python [task APIs](../../bindings/docs/tasks.md) are a higher-level application executor over
the existing control loop. A student writes a `KeelTask` with sequential, blocking actions. The task's
explicit movement request supplies signed percentages and a finite duration; the supervisor requests
arming on the owner thread, waits for `ARMED`, then enforces that interval on the control thread itself.

The supervisor must command zero drive outside an active action, acknowledge a stop before completing an
action, and enforce the total task deadline independently of the student's code. Cancellation, faults and
timeouts invalidate further commands and disarm. No task method automatically clears a fault. A paused
or hung student thread cannot renew or extend the action deadline. The supervisor stops drive on the first
control cycle at or after that deadline; if the control thread itself stalls, the native lease and hardware
heartbeat limits remain the fallback.

Task code and camera access stay on one ordinary calling thread; the hardware control loop remains on its
own dedicated thread. Task execution uses monotonic wall time and the real-time simulator. It rejects
virtual-clock simulator flags rather than mixing clocks. The low-level virtual-clock tools are unchanged.

The task facade owns resource cleanup, but cannot safely kill arbitrary Java or Python code. A program
stuck in student code can remain running after motor authority is revoked. In Python, GIL starvation may
also prevent the supervisor from running; native motor leases and the MCU heartbeat remain effective.

## 8. Per-language rules

### C# (.NET 8 or later)

- Declare the four string functions as returning a pointer (`nint` or `byte*`) and convert with
  `Marshal.PtrToStringUTF8`. A `string` return makes the marshaller free the static string with
  `CoTaskMemFree` (`free()` on Linux) and corrupts the heap. The binding's tests call each of them 10^5 times.
- Use `[LibraryImport]` with blittable `[StructLayout(LayoutKind.Sequential)]` structs; fixed-size `char` arrays
  become fixed buffers or inline arrays of `byte`.
- Wrap the handle in a `SafeHandle` whose `ReleaseHandle` calls `keel_close` and logs its status; the
  `SafeHandle` reference count also keeps a call in flight from racing a close.
- Run the loop on a dedicated `Thread`, never `Task.Run` or the thread pool. Set
  `GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency` while the robot is armed and pre-allocate.

### Java (FFM, Java 25)

- `SymbolLookup.libraryLookup`, `Linker.downcallHandle` and `MemorySegment.reinterpret` are restricted methods.
  The JVM honours `Enable-Native-Access: ALL-UNNAMED` only in the manifest of the jar it launches and ignores it in
  a library jar on the class path, so applications pass `--enable-native-access=ALL-UNNAMED` (class path) or
  `--enable-native-access=<module>` (module path), or set the attribute in their own executable jar. Without it
  current JDKs warn; with `--illegal-native-access=deny` the restricted call throws `IllegalCallerException`, which
  the binding reports as its library error naming the option.
- Read returned strings with `segment.reinterpret(256).getString(0)`; 256 is `KEEL_STRING_MAX`, the bound the
  header guarantees.
- Allocate structs in confined arenas only: one per call that returns a struct, and the loop's structs once in an
  arena that the loop thread opens and closes. A shared arena could be closed while another thread's call still
  uses it, and stop calls take no struct pointer, so they never need the loop's memory.
- Run the loop on `Thread.ofPlatform()`, never on a virtual thread. Use ZGC (`-XX:+UseZGC`) or
  `-XX:MaxGCPauseMillis=20`.
- `uint32_t` fields arrive as signed `int`; widen with `Integer.toUnsignedLong` before comparing.

### Python (ctypes)

- Set `restype` and `argtypes` for every function (`keel_ctypes.PROTOTYPES`); without them ctypes truncates
  64-bit values and pointers.
- ctypes releases the GIL during foreign calls, so `keel_sleep_until` does not block other Python threads.
  Returning from any foreign call needs the GIL back. Behind one busy thread that takes up to
  `sys.getswitchinterval()` (5 ms by default), so the loop thread sets `sys.setswitchinterval(0.001)` while it runs.
  Behind several CPU-bound Python threads a hand-off can take many switch intervals and exceed the 40 ms stall
  budget: keep CPU-bound work in another process (`spawn` or `forkserver`), or use free-threaded CPython.
- Call `gc.freeze()` after start-up and keep `step()` free of allocation-heavy work.
- Python delivers signals to the main thread only: let the main thread catch `KeyboardInterrupt` and call
  `keel_emergency_stop`, which any thread may call.
- Raspberry Pi OS bookworm and Ubuntu 23.04 or later refuse `pip install` into the system Python (PEP 668). Use
  `python3 -m venv --system-site-packages`.

### C and C++

C programs use `keel.h` directly. C++ programs wrap the handle in an RAII class whose destructor calls
`keel_close` without throwing; `tests/consumers/cxx_consumer.cpp` is an example.

For Ctrl-C and SIGTERM, the handler only sets a `volatile sig_atomic_t` flag that the loop checks (or the program
blocks the signals and a thread waits with `sigwait()` or reads a `signalfd`); that thread, never the handler,
calls `keel_emergency_stop`. A handler that calls into libkeel can deadlock on a lock the interrupted thread holds.

Link with `pkg-config --cflags --libs keel` (the shared library) or, for one self-contained file and for
`port.h`, `pkg-config --cflags --libs keel-static`, which names `libkeel.a` by path.

## 9. Testing a binding

Test against the simulator with `KEEL_BACKEND_SIMULATOR` and `KEEL_OPEN_VIRTUAL_CLOCK`: time moves only inside
`keel_sleep_until`, so a full arm, drive and fault sequence runs in milliseconds and the same way every time.
Drive the virtual-clock device from one thread.

`tests/bindings/scenarios.json` is the language-neutral conformance file that every binding replays through its
public API: open, metadata, arm, drive, the thread rule, faults and their recovery, servo, ranging, LEDs, buttons,
injected failures and closed handles, covering all 37 functions. The file describes its step format. The test
suite proves every expectation against the library by replaying it through `tests/consumers/keel_ctypes.py`
(CTest `ctypes.scenarios`); a binding's own replay maps each C function name to its API, which proves that mapping
complete. Every scenario opens `KEEL_BACKEND_SIMULATOR`, and a replayer refuses one that names another backend.
`tests/consumers/test_ctypes.py` remains the reference for the raw calls.
