// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal.camera;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.camera.AutofocusMode;
import io.github.bisocmorg.keel.camera.AutofocusRange;
import io.github.bisocmorg.keel.camera.AutofocusState;
import io.github.bisocmorg.keel.camera.CameraAbi;
import io.github.bisocmorg.keel.camera.CameraFlag;
import io.github.bisocmorg.keel.camera.CameraInfoFlag;
import io.github.bisocmorg.keel.camera.CameraLimits;
import io.github.bisocmorg.keel.camera.CaptureState;
import io.github.bisocmorg.keel.camera.WhiteBalance;
import io.github.bisocmorg.keel.camera.encoding.ChunkFlag;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.encoding.EncoderFlag;
import io.github.bisocmorg.keel.camera.encoding.H264Profile;
import io.github.bisocmorg.keel.camera.errors.CameraResource;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import io.github.bisocmorg.keel.camera.outputs.OutputState;
import io.github.bisocmorg.keel.camera.stills.StillFlag;
import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SequenceLayout;
import java.lang.foreign.StructLayout;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.RecordComponent;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;

/**
 * The generated camera declarations against camera.h: the struct layouts against {@code tests/abi/camera_layout.c},
 * the functions against {@code camera_symbols.txt}, every enumerator against {@code camera_constants.golden}, and the
 * generated enums, records, readers and writers.
 */
class CameraAbiTest {
    private static final Path ABI = Path.of(System.getProperty("keel.source.dir"), "tests", "abi");

    /** Every camera enum of camera.h: its Java type and the prefix its members drop. */
    private static final Map<Class<?>, String> ENUMS = new LinkedHashMap<>();

    static {
        ENUMS.put(CameraAbi.class, "KEEL_CAMERA_ABI_");
        ENUMS.put(PixelFormat.class, "KEEL_CAMERA_FORMAT_");
        ENUMS.put(CameraFlag.class, "KEEL_CAMERA_");
        ENUMS.put(AutofocusMode.class, "KEEL_CAMERA_AF_");
        ENUMS.put(AutofocusRange.class, "KEEL_CAMERA_AF_RANGE_");
        ENUMS.put(AutofocusState.class, "KEEL_CAMERA_AF_STATE_");
        ENUMS.put(WhiteBalance.class, "KEEL_CAMERA_AWB_");
        ENUMS.put(CameraInfoFlag.class, "KEEL_CAMERA_INFO_");
        ENUMS.put(Codec.class, "KEEL_CAMERA_CODEC_");
        ENUMS.put(H264Profile.class, "KEEL_CAMERA_H264_PROFILE_");
        ENUMS.put(EncoderFlag.class, "KEEL_CAMERA_");
        ENUMS.put(ChunkFlag.class, "KEEL_CAMERA_ENCODED_");
        ENUMS.put(StillFlag.class, "KEEL_CAMERA_STILL_");
        ENUMS.put(CaptureState.class, "KEEL_CAMERA_STATE_");
        ENUMS.put(OutputState.class, "KEEL_CAMERA_OUTPUT_");
        ENUMS.put(CameraResource.class, "KEEL_CAMERA_RESOURCE_");
        ENUMS.put(CameraLimits.class, "KEEL_CAMERA_");
    }

    private static final List<StructLayout> STRUCTS = List.of(CameraLayouts.CAMERA_ERROR, CameraLayouts.CAMERA_INFO,
            CameraLayouts.CAMERA_MODE, CameraLayouts.CAMERA_OPTIONS, CameraLayouts.CAMERA_STREAM_INFO,
            CameraLayouts.CAMERA_FRAME_INFO, CameraLayouts.CAMERA_ENCODER_OPTIONS, CameraLayouts.CAMERA_ENCODED_INFO,
            CameraLayouts.CAMERA_OUTPUT_OPTIONS, CameraLayouts.CAMERA_OUTPUT_STATS, CameraLayouts.CAMERA_STATS,
            CameraLayouts.CAMERA_STILL_OPTIONS, CameraLayouts.CAMERA_STILL_INFO);

    private static boolean hasMethod(Class<?> type, String name) {
        for (Method method : type.getMethods()) {
            if (method.getName().equals(name) && method.getParameterCount() == 0) {
                return true;
            }
        }
        return false;
    }

    @Test
    void structLayoutsEqualTheLayoutPinsAndTheSpecSizes() throws IOException {
        Map<String, Map<String, Long>> pins = new LinkedHashMap<>();
        Map<String, Long> sizes = new LinkedHashMap<>();
        Pattern first = Pattern.compile("FIRST\\((keel_\\w+), (\\w+)\\)");
        Pattern next = Pattern.compile("NEXT\\((keel_\\w+), \\w+, (\\w+), (\\d+)\\)");
        Pattern last = Pattern.compile("LAST\\((keel_\\w+), \\w+, (\\d+)\\)");
        for (String line : Files.readAllLines(ABI.resolve("camera_layout.c"))) {
            Matcher match;
            if ((match = first.matcher(line)).lookingAt()) {
                pins.computeIfAbsent(match.group(1), name -> new LinkedHashMap<>()).put(match.group(2), 0L);
            } else if ((match = next.matcher(line)).lookingAt()) {
                pins.get(match.group(1)).put(match.group(2), Long.parseLong(match.group(3)));
            } else if ((match = last.matcher(line)).lookingAt()) {
                sizes.put(match.group(1), Long.parseLong(match.group(2)));
            }
        }
        Map<String, Long> spec = Map.ofEntries(Map.entry("keel_camera_error", 528L),
                Map.entry("keel_camera_info", 248L), Map.entry("keel_camera_mode", 40L), Map.entry("keel_camera_options", 72L),
                Map.entry("keel_camera_stream_info", 64L), Map.entry("keel_camera_frame_info", 64L),
                Map.entry("keel_camera_encoder_options", 32L), Map.entry("keel_camera_encoded_info", 32L),
                Map.entry("keel_camera_output_options", 24L), Map.entry("keel_camera_output_stats", 304L),
                Map.entry("keel_camera_stats", 72L), Map.entry("keel_camera_still_options", 24L),
                Map.entry("keel_camera_still_info", 32L));
        assertEquals(spec.keySet(), pins.keySet());
        for (StructLayout layout : STRUCTS) {
            String name = layout.name().orElseThrow();
            assertEquals(spec.get(name), layout.byteSize(), name);
            assertEquals(sizes.get(name), layout.byteSize(), name);
            List<String> fields = new ArrayList<>();
            for (MemoryLayout member : layout.memberLayouts()) {
                String field = member.name().orElseThrow();
                fields.add(field);
                assertEquals(pins.get(name).get(field),
                        layout.byteOffset(MemoryLayout.PathElement.groupElement(field)), name + "." + field);
            }
            assertEquals(List.copyOf(pins.get(name).keySet()), fields, name);
        }
    }

    @Test
    void theBoundFunctionsEqualCameraSymbolsTxt() throws IOException {
        List<String> symbols = Files.readAllLines(ABI.resolve("camera_symbols.txt")).stream()
                .filter(line -> !line.isBlank() && !line.startsWith("#")).toList();
        assertEquals(symbols, CameraNativeLibrary.functionNames());
        for (CameraLayouts.Signature signature : CameraLayouts.FUNCTIONS) {
            assertNotNull(signature.descriptor(), signature.name());
        }
    }

    @Test
    void everyEnumeratorEqualsTheGoldenFile() throws IOException, ReflectiveOperationException {
        Map<String, Long> golden = new LinkedHashMap<>();
        for (String line : Files.readAllLines(ABI.resolve("camera_constants.golden"))) {
            if (!line.isBlank() && !line.startsWith("#")) {
                String[] parts = line.split("=");
                golden.put(parts[0], Long.parseLong(parts[1]));
            }
        }
        Map<String, Long> generated = new LinkedHashMap<>();
        for (Map.Entry<Class<?>, String> entry : ENUMS.entrySet()) {
            Class<?> type = entry.getKey();
            if (type.isEnum()) {
                Method value = type.getMethod(hasMethod(type, "code") ? "code" : "mask");
                for (Object constant : type.getEnumConstants()) {
                    if (!((Enum<?>) constant).name().equals("UNRECOGNIZED")) {
                        generated.put(entry.getValue() + ((Enum<?>) constant).name(),
                                ((Integer) value.invoke(constant)).longValue());
                    }
                }
            } else {
                for (Field field : type.getFields()) {
                    if (Modifier.isStatic(field.getModifiers())) {
                        generated.put(entry.getValue() + field.getName(), ((Integer) field.get(null)).longValue());
                    }
                }
            }
        }
        assertEquals(89, generated.size());
        assertEquals(0L, generated.remove("KEEL_CAMERA_ABI_MINOR"));
        assertEquals(0x0001_0000L, generated.remove("KEEL_CAMERA_ABI_VERSION"));
        assertEquals(golden, generated);
    }

    @Test
    void openEnumsTolerateUnknownValues() throws ReflectiveOperationException {
        for (Class<?> type : ENUMS.keySet()) {
            if (!type.isEnum() || !hasMethod(type, "code")) {
                continue;
            }
            Method code = type.getMethod("code");
            Method of = type.getMethod("of", int.class);
            Object[] constants = type.getEnumConstants();
            Object sentinel = constants[constants.length - 1];
            assertEquals("UNRECOGNIZED", ((Enum<?>) sentinel).name(), type.getSimpleName());
            for (Object constant : constants) {
                if (constant != sentinel) {
                    assertSame(constant, of.invoke(null, code.invoke(constant)));
                }
            }
            assertSame(sentinel, of.invoke(null, 0x7654_3210));
            InvocationTargetException thrown = assertThrows(InvocationTargetException.class,
                    () -> code.invoke(sentinel));
            assertTrue(thrown.getCause() instanceof IllegalStateException, type.getSimpleName());
        }
    }

    @Test
    void flagEnumsEncodeAndDecode() throws ReflectiveOperationException {
        for (Class<?> type : ENUMS.keySet()) {
            if (!type.isEnum() || hasMethod(type, "code")) {
                continue;
            }
            Method mask = type.getMethod("mask");
            Method maskOf = type.getMethod("maskOf", type.arrayType());
            Method setOf = type.getMethod("setOf", int.class);
            Object[] constants = type.getEnumConstants();
            Object all = Array.newInstance(type, constants.length);
            int union = 0;
            for (int i = 0; i < constants.length; i++) {
                Array.set(all, i, constants[i]);
                union |= (int) mask.invoke(constants[i]);
                assertEquals(Set.of(constants[i]), setOf.invoke(null, mask.invoke(constants[i])));
            }
            assertEquals(union, maskOf.invoke(null, all));
            assertTrue(((Set<?>) setOf.invoke(null, 1 << 30)).isEmpty(), type.getSimpleName());
        }
    }

    private static String snake(String name) {
        return name.replaceAll("([A-Z])", "_$1").replaceAll("([a-z])([0-9])", "$1$2").toLowerCase(Locale.ROOT);
    }

    /** Every record field reads from the offset the layout gives it, with its documented width and sign. */
    @Test
    void structsReadEveryFieldAtItsLayoutOffset() throws ReflectiveOperationException {
        Map<StructLayout, String> readers = new LinkedHashMap<>();
        readers.put(CameraLayouts.CAMERA_ERROR, "cameraError");
        readers.put(CameraLayouts.CAMERA_INFO, "cameraInfo");
        readers.put(CameraLayouts.CAMERA_MODE, "cameraMode");
        readers.put(CameraLayouts.CAMERA_STREAM_INFO, "cameraStreamInfo");
        readers.put(CameraLayouts.CAMERA_FRAME_INFO, "cameraFrameInfo");
        readers.put(CameraLayouts.CAMERA_ENCODED_INFO, "cameraEncodedInfo");
        readers.put(CameraLayouts.CAMERA_OUTPUT_STATS, "cameraOutputStats");
        readers.put(CameraLayouts.CAMERA_STATS, "cameraStats");
        readers.put(CameraLayouts.CAMERA_STILL_INFO, "cameraStillInfo");
        try (Arena arena = Arena.ofConfined()) {
            for (Map.Entry<StructLayout, String> entry : readers.entrySet()) {
                StructLayout layout = entry.getKey();
                MemorySegment segment = CameraStructCodec.allocate(arena, layout);
                assertEquals(layout.byteSize(), segment.get(JAVA_INT, 0));
                Map<String, Object> written = new LinkedHashMap<>();
                for (MemoryLayout member : layout.memberLayouts()) {
                    String field = member.name().orElseThrow();
                    long offset = layout.byteOffset(MemoryLayout.PathElement.groupElement(field));
                    if (field.equals("struct_size")) {
                        continue;
                    }
                    if (member instanceof SequenceLayout) {
                        String text = field + "é";
                        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
                        MemorySegment.copy(bytes, 0, segment, ValueLayout.JAVA_BYTE, offset, bytes.length);
                        written.put(field, text);
                    } else if (member.byteSize() == 8) {
                        long value = 0x8000_0000_0000_0000L | offset;
                        segment.set(JAVA_LONG, offset, value);
                        written.put(field, value);
                    } else {
                        int value = 0x8000_0000 | (int) offset;
                        segment.set(JAVA_INT, offset, value);
                        written.put(field, value);
                    }
                }
                Object record = CameraStructCodec.class.getMethod(entry.getValue(), MemorySegment.class)
                        .invoke(null, segment);
                for (RecordComponent component : record.getClass().getRecordComponents()) {
                    String field = snake(component.getName().replaceAll("Value$", ""));
                    Object expected = written.get(field);
                    assertNotNull(expected, layout.name() + " has no field " + field);
                    Object actual = component.getAccessor().invoke(record);
                    if (component.getType() == long.class && expected instanceof Integer bits) {
                        expected = Integer.toUnsignedLong(bits);
                    }
                    assertEquals(expected, actual, layout.name().orElseThrow() + "." + field);
                }
                for (Method method : record.getClass().getDeclaredMethods()) {
                    if (Modifier.isPublic(method.getModifiers()) && method.getParameterCount() == 0
                            && !Modifier.isStatic(method.getModifiers())) {
                        assertNotNull(method.invoke(record), method.getName());
                    }
                }
            }
        }
    }

    @Test
    void structWritersFillTheirFields() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment options = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_OPTIONS);
            CameraStructCodec.writeCameraOptions(options, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -13, 14, 15, 16, 17);
            assertEquals(72, options.get(JAVA_INT, 0));
            for (int field = 1; field <= 17; field++) {
                assertEquals(field == 13 ? -13 : field, options.get(JAVA_INT, 4L * field), "field " + field);
            }
            MemorySegment encoder = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_ENCODER_OPTIONS);
            CameraStructCodec.writeEncoderOptions(encoder, 1, 2, 3, 4, 5, 6, 7);
            MemorySegment output = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_OUTPUT_OPTIONS);
            CameraStructCodec.writeOutputOptions(output, 1, 2, 3, 4);
            MemorySegment still = CameraStructCodec.allocate(arena, CameraLayouts.CAMERA_STILL_OPTIONS);
            CameraStructCodec.writeStillOptions(still, 1, 2, 3, 4);
            for (MemorySegment segment : List.of(encoder, output, still)) {
                for (int field = 1; field <= (segment == encoder ? 7 : 4); field++) {
                    assertEquals(field, segment.get(JAVA_INT, 4L * field));
                }
            }
            assertFalse(output.get(JAVA_INT, 20) != 0, "the reserved field stays zero");
        }
    }
}
