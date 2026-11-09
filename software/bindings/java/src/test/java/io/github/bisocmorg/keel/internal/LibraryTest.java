// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.Abi;
import io.github.bisocmorg.keel.AbiVersion;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.errors.StatusClass;
import io.github.bisocmorg.keel.state.FaultCause;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.nio.file.Path;
import java.util.Optional;
import org.junit.jupiter.api.Test;

/** Loading, the ABI check, the bound symbols and the library's strings. */
class LibraryTest {
    private static final String LIBRARY = System.getenv(LibraryLoader.ENVIRONMENT);

    @Test
    void loadsTheLibraryNamedByKeelLibrary() {
        Keel.load();
        assertEquals(Path.of(LIBRARY).toString(), NativeLibrary.loaded().orElseThrow().library());
        Keel.load(Path.of(LIBRARY));
        Keel.load();
        assertSame(NativeLibrary.get(), NativeLibrary.loaded().orElseThrow());
    }

    @Test
    void anotherLibraryCannotReplaceTheLoadedOne() {
        Keel.load();
        LibraryException error = assertThrows(LibraryException.class,
                () -> Keel.load(Path.of("/opt/other/libkeel.so.1")));
        assertTrue(error.getMessage().contains("already loaded"), error.getMessage());
    }

    @Test
    void resolutionPrefersKeelLibraryAndTreatsEmptyAsUnset() {
        assertNull(LibraryLoader.resolve(null));
        assertNull(LibraryLoader.resolve(""));
        assertEquals(Path.of("/usr/lib/libkeel.so.1"), LibraryLoader.resolve("/usr/lib/libkeel.so.1"));
        LibraryException relative = assertThrows(LibraryException.class,
                () -> LibraryLoader.resolve("build/libkeel.so.1"));
        assertTrue(relative.getMessage().contains("KEEL_LIBRARY must be an absolute path"), relative.getMessage());
        assertThrows(LibraryException.class, () -> LibraryLoader.requireAbsolute(Path.of("libkeel.so.1"), "Keel.load"));
        assertEquals("libkeel.so.1 (dynamic loader)", LibraryLoader.describe(null));
    }

    @Test
    void aMissingLibraryIsALibraryException() {
        LibraryException error = assertThrows(LibraryException.class,
                () -> LibraryLoader.open(Path.of("/nonexistent/libkeel.so.1")));
        assertTrue(error.getMessage().contains("cannot open /nonexistent/libkeel.so.1"), error.getMessage());
        assertInstanceOf(IllegalArgumentException.class, error.getCause());
    }

    @Test
    void onlyLinuxIsAccepted() {
        LibraryLoader.checkPlatform("Linux");
        LibraryException windows = assertThrows(LibraryException.class,
                () -> LibraryLoader.checkPlatform("Windows 11"));
        assertEquals("libkeel requires Linux; found Windows 11", windows.getMessage());
        assertThrows(LibraryException.class, () -> LibraryLoader.checkPlatform("Mac OS X"));
    }

    @Test
    void theAbiCheckNeedsTheMajorAndAtLeastTheMinor() {
        LibraryLoader.checkAbi(0x0001_0000, 1, 0, "lib");
        LibraryLoader.checkAbi(0x0001_0005, 1, 0, "lib");
        LibraryLoader.checkAbi(0x0001_0002, 1, 2, "lib");
        LibraryException major = assertThrows(LibraryException.class,
                () -> LibraryLoader.checkAbi(0x0002_0000, 1, 0, "lib"));
        assertTrue(major.getMessage().startsWith("lib has ABI 2.0; this binding needs 1.0"), major.getMessage());
        assertTrue(major.getMessage().contains("dpkg-query -W libkeel1"), major.getMessage());
        LibraryException minor = assertThrows(LibraryException.class,
                () -> LibraryLoader.checkAbi(0x0001_0001, 1, 2, "lib"));
        assertTrue(minor.getMessage().contains("ABI 1.1"), minor.getMessage());
        // Bit 31 set: the major is read with an unsigned shift, never as a negative number.
        LibraryException high = assertThrows(LibraryException.class,
                () -> LibraryLoader.checkAbi(0x8001_0000, 1, 0, "lib"));
        assertTrue(high.getMessage().contains("ABI 32769.0"), high.getMessage());
        assertEquals(new AbiVersion(Abi.MAJOR, Abi.MINOR), Keel.abiVersion());
        assertEquals(Abi.MAJOR + "." + Abi.MINOR, Keel.abiVersion().toString());
    }

    @Test
    void everySymbolMustResolve() {
        SymbolLookup library = LibraryLoader.open(Path.of(LIBRARY));
        new NativeLibrary(library, LIBRARY);
        SymbolLookup partial = name -> name.equals("keel_tick") ? Optional.empty() : library.find(name);
        LibraryException error = assertThrows(LibraryException.class, () -> new NativeLibrary(partial, "partial"));
        assertTrue(error.getMessage().contains("does not export keel_tick"), error.getMessage());
        assertEquals(37, NativeLibrary.functionNames().size());
    }

    @Test
    void downcallFailuresAreRethrownUnchecked() {
        IllegalStateException runtime = new IllegalStateException("runtime");
        assertSame(runtime, NativeLibrary.rethrow(runtime));
        assertThrows(AssertionError.class, () -> NativeLibrary.rethrow(new AssertionError("error")));
        Exception checked = new Exception("checked");
        assertSame(checked, NativeLibrary.rethrow(checked).getCause());
        IllegalCallerException denied = new IllegalCallerException("denied");
        LibraryException mapped = LibraryLoader.nativeAccessDenied(denied);
        assertTrue(mapped.getMessage().contains("--enable-native-access=ALL-UNNAMED"), mapped.getMessage());
        assertSame(denied, mapped.getCause());
    }

    @Test
    void stringsAreReadWithinTheirBound() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment text = arena.allocate(Limits.STRING_MAX + 64);
            text.fill((byte) 'A');
            LibraryException unterminated = assertThrows(LibraryException.class,
                    () -> NativeStringReader.read(text, "f"));
            assertTrue(unterminated.getMessage().contains("no NUL within KEEL_STRING_MAX"),
                    unterminated.getMessage());
            text.set(JAVA_BYTE, 10, (byte) 0);
            assertEquals("AAAAAAAAAA", NativeStringReader.read(text, "f"));
            text.set(JAVA_BYTE, Limits.STRING_MAX - 1, (byte) 0);
            text.set(JAVA_BYTE, 10, (byte) 'A');
            assertEquals(Limits.STRING_MAX - 1, NativeStringReader.read(text, "f").length());
            assertThrows(LibraryException.class, () -> NativeStringReader.read(MemorySegment.NULL, "f"));
        }
    }

    @Test
    void everyStatusHasItsNameDescriptionAndClass() {
        for (Status status : Status.values()) {
            if (status == Status.UNRECOGNIZED) {
                continue;
            }
            assertEquals("KEEL_" + status.name(), Keel.statusName(status.code()));
            assertFalse(Keel.statusString(status.code()).isEmpty(), status.name());
            assertTrue(Keel.statusClass(status.code()) != StatusClass.UNRECOGNIZED, status.name());
        }
        assertEquals("KEEL_E_UNKNOWN", Keel.statusName(-99));
        assertEquals(StatusClass.FAULT, Keel.statusClass(-99));
        assertEquals(StatusClass.OK, Keel.statusClass(0));
    }

    @Test
    void everyCauseHasItsName() {
        for (FaultCause cause : FaultCause.values()) {
            if (cause != FaultCause.UNRECOGNIZED) {
                assertEquals(cause.name(), Keel.faultCauseName(cause.code()));
            }
        }
        assertEquals("UNKNOWN", Keel.faultCauseName(9999));
    }

    @Test
    void theVersionStringNamesThisVersion() {
        String version = Keel.versionString();
        assertTrue(version.startsWith(Keel.VERSION + " ("), version);
        assertTrue(version.contains("simulator"), version);
        assertSame(version, Keel.versionString());
    }

    @Test
    void theAccessRegistryHoldsOneImplementationOfEach() {
        ErrorAccess errors = AccessRegistry.errors();
        DeviceAccess devices = AccessRegistry.devices();
        assertSame(errors, AccessRegistry.errors());
        assertSame(devices, AccessRegistry.devices());
        IllegalStateException again = assertThrows(IllegalStateException.class,
                () -> AccessRegistry.registerErrors(errors));
        assertTrue(again.getMessage().contains("already registered"), again.getMessage());
        assertThrows(IllegalStateException.class, () -> AccessRegistry.registerDevices(devices));
    }
}
