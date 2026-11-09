// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.internal;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.Abi;
import io.github.bisocmorg.keel.Keel;
import io.github.bisocmorg.keel.Limits;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.errors.StatusClass;
import io.github.bisocmorg.keel.loop.CycleCommand;
import io.github.bisocmorg.keel.loop.CycleFlag;
import io.github.bisocmorg.keel.loop.CycleInput;
import io.github.bisocmorg.keel.loop.Health;
import io.github.bisocmorg.keel.peripherals.Button;
import io.github.bisocmorg.keel.peripherals.SensorPower;
import io.github.bisocmorg.keel.state.FaultCause;
import io.github.bisocmorg.keel.support.EnumeratorTable;
import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SequenceLayout;
import java.lang.foreign.StructLayout;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.RecordComponent;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.Test;

/**
 * The binding against the frozen ABI: struct layouts against the pins of {@code tests/abi/layout.c},
 * function descriptors and every enumerator against the preprocessed {@code keel.h}, the bound functions against
 * {@code symbols.txt}, and the generated names against the library's own strings.
 */
class AbiTest {
    private static final Path SOURCE = Path.of(System.getProperty("keel.source.dir"));
    private static final Path REPOSITORY = Path.of(System.getProperty("keel.repository.dir"));

    private static final List<StructLayout> STRUCTS = List.of(Layouts.OPEN_OPTIONS, Layouts.OPEN_REPORT,
            Layouts.FAULT, Layouts.SNAPSHOT, Layouts.BUTTONS, Layouts.RANGE, Layouts.CYCLE_INPUT, Layouts.SIM_STATE);

    // ------------------------------------------------------------------ helpers ----

    /** keel.h as binding generators read it: cc -E -P -DKEEL_API= against a plain stdint.h. */
    private static String preprocessedHeader() throws IOException, InterruptedException {
        Path fake = Files.createTempDirectory("keel-abi");
        try {
            Files.writeString(fake.resolve("stdint.h"), """
                    typedef signed char int8_t; typedef unsigned char uint8_t;
                    typedef short int16_t; typedef unsigned short uint16_t;
                    typedef int int32_t; typedef unsigned int uint32_t;
                    typedef long long int64_t; typedef unsigned long long uint64_t;
                    """);
            String cc = System.getenv().getOrDefault("CC", "cc");
            Process process = new ProcessBuilder(cc, "-E", "-P", "-DKEEL_API=", "-nostdinc", "-I", fake.toString(),
                    "-I", SOURCE.resolve("include").toString(), SOURCE.resolve("include/keel/keel.h").toString())
                    .redirectErrorStream(true).start();
            String text = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            assertTrue(process.waitFor(60, TimeUnit.SECONDS), "the preprocessor did not finish");
            assertEquals(0, process.exitValue(), text);
            return text;
        } finally {
            Files.deleteIfExists(fake.resolve("stdint.h"));
            Files.deleteIfExists(fake);
        }
    }

    /** {struct: {field: offset}} and sizes from the FIRST/NEXT/LAST lines of layout.c. */
    private static Map<String, Map<String, Long>> layoutPins(Map<String, Long> sizes) throws IOException {
        Map<String, Map<String, Long>> pins = new LinkedHashMap<>();
        Pattern first = Pattern.compile("^FIRST\\((keel_\\w+), (\\w+)\\)");
        Pattern next = Pattern.compile("^NEXT\\((keel_\\w+), \\w+, (\\w+), (\\d+)\\)");
        Pattern last = Pattern.compile("^LAST\\((keel_\\w+), \\w+, (\\d+)\\)");
        for (String line : Files.readAllLines(SOURCE.resolve("tests/abi/layout.c"))) {
            Matcher matcher;
            if ((matcher = first.matcher(line)).find()) {
                pins.computeIfAbsent(matcher.group(1), name -> new LinkedHashMap<>()).put(matcher.group(2), 0L);
            } else if ((matcher = next.matcher(line)).find()) {
                pins.get(matcher.group(1)).put(matcher.group(2), Long.parseLong(matcher.group(3)));
            } else if ((matcher = last.matcher(line)).find()) {
                sizes.put(matcher.group(1), Long.parseLong(matcher.group(2)));
            }
        }
        return pins;
    }

    /** Evaluates a preprocessed enumerator expression: literals, earlier enumerators, | & << >> + - ~ and (). */
    private static long evaluate(String expression, Map<String, Long> known) {
        return new Object() {
            private final String text = expression.replaceAll("\\s+", "");
            private int at;

            long parse() {
                long value = or();
                assertEquals(text.length(), at, "cannot evaluate " + expression);
                return value;
            }

            long or() {
                long value = and();
                while (eat("|")) {
                    value |= and();
                }
                return value;
            }

            long and() {
                long value = shift();
                while (eat("&")) {
                    value &= shift();
                }
                return value;
            }

            long shift() {
                long value = sum();
                while (true) {
                    if (eat("<<")) {
                        value <<= sum();
                    } else if (eat(">>")) {
                        value >>= sum();
                    } else {
                        return value;
                    }
                }
            }

            long sum() {
                long value = unary();
                while (true) {
                    if (eat("+")) {
                        value += unary();
                    } else if (eat("-")) {
                        value -= unary();
                    } else {
                        return value;
                    }
                }
            }

            long unary() {
                if (eat("-")) {
                    return -unary();
                }
                if (eat("~")) {
                    return ~unary();
                }
                if (eat("(")) {
                    long value = or();
                    assertTrue(eat(")"), "unbalanced " + expression);
                    return value;
                }
                Matcher number = Pattern.compile("^(0[xX][0-9a-fA-F]+|\\d+)[uUlL]*").matcher(text.substring(at));
                if (number.find()) {
                    at += number.end();
                    String digits = number.group(1);
                    return digits.startsWith("0x") || digits.startsWith("0X") ? Long.parseLong(digits.substring(2), 16)
                            : Long.parseLong(digits);
                }
                Matcher name = Pattern.compile("^[A-Za-z_]\\w*").matcher(text.substring(at));
                assertTrue(name.find(), "cannot evaluate " + expression);
                at += name.end();
                Long value = known.get(name.group());
                assertNotNull(value, "unknown name " + name.group());
                return value;
            }

            boolean eat(String token) {
                if (text.startsWith(token, at)) {
                    at += token.length();
                    return true;
                }
                return false;
            }
        }.parse();
    }

    private static String camelToSnake(String name) {
        return name.replaceAll("([A-Z])", "_$1").toLowerCase(java.util.Locale.ROOT);
    }

    // -------------------------------------------------------------------- tests ----

    @Test
    void structLayoutsEqualTheLayoutPins() throws IOException {
        Map<String, Long> sizes = new LinkedHashMap<>();
        Map<String, Map<String, Long>> pins = layoutPins(sizes);
        for (StructLayout layout : STRUCTS) {
            String name = layout.name().orElseThrow();
            Map<String, Long> expected = pins.get(name);
            assertNotNull(expected, name);
            assertEquals(sizes.get(name), layout.byteSize(), name);
            List<String> fields = new ArrayList<>();
            for (MemoryLayout member : layout.memberLayouts()) {
                String field = member.name().orElseThrow();
                fields.add(field);
                assertEquals(expected.get(field), layout.byteOffset(MemoryLayout.PathElement.groupElement(field)),
                        name + "." + field);
            }
            assertEquals(List.copyOf(expected.keySet()), fields, name);
            assertTrue(layout.byteSize() <= Limits.STRUCT_SIZE_MAX, name);
        }
    }

    @Test
    void descriptorsEqualTheHeaderDeclarations() throws IOException, InterruptedException {
        String header = preprocessedHeader();
        Matcher declaration = Pattern.compile("([\\w\\s*]+?)\\b(keel_\\w+)\\s*\\(([^)]*)\\)\\s*;").matcher(header);
        List<String> declared = new ArrayList<>();
        Map<String, FunctionDescriptor> descriptors = new LinkedHashMap<>();
        Layouts.FUNCTIONS.forEach(signature -> descriptors.put(signature.name(), signature.descriptor()));
        while (declaration.find()) {
            String name = declaration.group(2);
            declared.add(name);
            List<MemoryLayout> arguments = new ArrayList<>();
            if (!declaration.group(3).strip().equals("void")) {
                for (String parameter : declaration.group(3).split(",")) {
                    arguments.add(layoutOf(parameter.strip().replaceAll("\\w+$", "")));
                }
            }
            FunctionDescriptor expected = FunctionDescriptor.of(layoutOf(declaration.group(1)),
                    arguments.toArray(MemoryLayout[]::new));
            assertEquals(expected, descriptors.get(name), name);
        }
        assertEquals(List.copyOf(descriptors.keySet()), declared);
    }

    private static MemoryLayout layoutOf(String declaration) {
        String type = declaration.replace("const", "").strip();
        if (type.contains("*")) {
            return ADDRESS;
        }
        return switch (type) {
            case "uint32_t", "int32_t" -> JAVA_INT;
            case "uint64_t" -> JAVA_LONG;
            default -> throw new AssertionError("unexpected C type " + type);
        };
    }

    @Test
    void theBoundFunctionsEqualSymbolsTxt() throws IOException {
        List<String> symbols = Files.readAllLines(SOURCE.resolve("tests/abi/symbols.txt")).stream()
                .map(String::strip).filter(line -> !line.isEmpty() && !line.startsWith("#")).toList();
        assertEquals(symbols, NativeLibrary.functionNames());
    }

    @Test
    void everyEnumeratorEqualsTheHeader() throws IOException, InterruptedException {
        String header = preprocessedHeader();
        Matcher enumMatcher = Pattern.compile("\\benum\\s+(\\w+)\\s*\\{(.*?)\\}\\s*;", Pattern.DOTALL).matcher(header);
        Map<String, Long> known = new LinkedHashMap<>();
        int enumerators = 0;
        int enums = 0;
        while (enumMatcher.find()) {
            enums++;
            EnumeratorTable.Binding binding = EnumeratorTable.ENUMS.get(enumMatcher.group(1));
            assertNotNull(binding, "no Java type for " + enumMatcher.group(1));
            Class<?> type = binding.type();
            String prefix = binding.prefix();
            long value = -1;
            List<String> members = new ArrayList<>();
            for (String item : enumMatcher.group(2).split(",")) {
                if (item.isBlank()) {
                    continue;
                }
                String[] parts = item.split("=", 2);
                String name = parts[0].strip();
                value = parts.length == 2 ? evaluate(parts[1], known) : value + 1;
                known.put(name, value);
                enumerators++;
                assertTrue(name.startsWith(prefix), name);
                String member = name.substring(prefix.length());
                members.add(member);
                assertEquals(value, EnumeratorTable.value(type, member), type.getSimpleName() + "." + member);
            }
            if (type.isEnum()) {
                List<String> javaMembers = new ArrayList<>();
                for (Object constant : type.getEnumConstants()) {
                    javaMembers.add(((Enum<?>) constant).name());
                }
                javaMembers.remove("UNRECOGNIZED");
                assertEquals(members, javaMembers, type.getSimpleName());
            }
        }
        assertEquals(25, enums);
        assertEquals(195, enumerators);
    }

    private static boolean hasMethod(Class<?> type, String name) {
        for (Method method : type.getMethods()) {
            if (method.getName().equals(name) && method.getParameterCount() == 0) {
                return true;
            }
        }
        return false;
    }

    @Test
    void generatedNamesEqualTheLibrarysStrings() {
        assertEquals(Abi.VERSION, NativeLibrary.get().abiVersion());
        for (Status status : Status.values()) {
            if (status != Status.UNRECOGNIZED) {
                assertEquals("KEEL_" + status.name(), Keel.statusName(status.code()));
            }
        }
        for (FaultCause cause : FaultCause.values()) {
            if (cause != FaultCause.UNRECOGNIZED) {
                assertEquals(cause.name(), Keel.faultCauseName(cause.code()));
            }
        }
    }

    @Test
    void statusClassesEqualTheHeadersTags() throws IOException {
        List<String> lines = Files.readAllLines(SOURCE.resolve("include/keel/keel.h"));
        int checked = 0;
        StringBuilder doc = new StringBuilder();
        boolean inside = false;
        for (String line : lines) {
            if (line.startsWith("enum keel_status {")) {
                inside = true;
            } else if (inside && line.startsWith("};")) {
                break;
            } else if (inside && line.strip().startsWith("KEEL_")) {
                String name = line.strip().split("[\\s=]")[0];
                Matcher tag = Pattern.compile("\\[([A-Z]+)\\]").matcher(doc);
                assertTrue(tag.find(), name);
                Status status = Status.valueOf(name.substring("KEEL_".length()));
                assertEquals(StatusClass.valueOf(tag.group(1)), Keel.statusClass(status.code()), name);
                checked++;
                doc.setLength(0);
            } else if (inside) {
                doc.append(line);
            }
        }
        assertEquals(Status.values().length - 1, checked);
    }

    @Test
    void openEnumsTolerateUnknownValues() throws ReflectiveOperationException {
        for (EnumeratorTable.Binding binding : EnumeratorTable.ENUMS.values()) {
            Class<?> type = binding.type();
            if (!type.isEnum()) {
                continue;
            }
            Object[] constants = type.getEnumConstants();
            for (Object constant : constants) {
                assertFalse(((Enum<?>) constant).name().startsWith("UNRECOGNIZED_"), type.getSimpleName());
            }
            if (hasMethod(type, "code")) {
                Method code = type.getMethod("code");
                Method of = type.getMethod("of", int.class);
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
            } else {
                for (Object constant : constants) {
                    assertFalse(((Enum<?>) constant).name().equals("UNRECOGNIZED"), type.getSimpleName());
                }
            }
        }
        assertSame(SensorPower.UNKNOWN, SensorPower.of(0));
    }

    @Test
    void flagEnumsEncodeAndDecode() throws ReflectiveOperationException {
        for (EnumeratorTable.Binding binding : EnumeratorTable.ENUMS.values()) {
            Class<?> type = binding.type();
            if (!type.isEnum() || hasMethod(type, "code")) {
                continue;
            }
            Method mask = type.getMethod("mask");
            Method maskOf = type.getMethod("maskOf", type.arrayType());
            Method setOf = type.getMethod("setOf", int.class);
            Object all = java.lang.reflect.Array.newInstance(type, type.getEnumConstants().length);
            int union = 0;
            for (int i = 0; i < type.getEnumConstants().length; i++) {
                Object constant = type.getEnumConstants()[i];
                java.lang.reflect.Array.set(all, i, constant);
                union |= (int) mask.invoke(constant);
                java.util.Set<?> decoded = (java.util.Set<?>) setOf.invoke(null, mask.invoke(constant));
                if (Integer.bitCount((int) mask.invoke(constant)) == 1) {
                    assertEquals(java.util.Set.of(constant), decoded);
                } else {
                    assertFalse(decoded.contains(constant), "a composite member is never decoded");
                }
            }
            assertEquals(union, maskOf.invoke(null, all));
            assertEquals(0, maskOf.invoke(null, java.lang.reflect.Array.newInstance(type, 0)));
            java.util.Set<?> unknown = (java.util.Set<?>) setOf.invoke(null, 1 << 30);
            assertTrue(unknown.isEmpty(), type.getSimpleName());
            assertThrows(UnsupportedOperationException.class, () -> unknown.clear());
        }
        assertEquals(EnumSet.of(Button.A, Button.Y), Button.setOf(Button.maskOf(Button.A, Button.Y)));
        assertEquals(Button.ALL.mask(), Button.maskOf(Button.A, Button.B, Button.X, Button.Y));
    }

    /** Every record field reads from the offset the layout gives it, with its documented width and sign. */
    @Test
    void structsReadEveryFieldAtItsLayoutOffset() throws ReflectiveOperationException {
        Map<StructLayout, Method> readers = new LinkedHashMap<>();
        readers.put(Layouts.OPEN_REPORT, StructCodec.class.getMethod("openReport", MemorySegment.class));
        readers.put(Layouts.FAULT, StructCodec.class.getMethod("fault", MemorySegment.class));
        readers.put(Layouts.SNAPSHOT, StructCodec.class.getMethod("snapshot", MemorySegment.class));
        readers.put(Layouts.BUTTONS, StructCodec.class.getMethod("buttons", MemorySegment.class));
        readers.put(Layouts.RANGE, StructCodec.class.getMethod("range", MemorySegment.class));
        readers.put(Layouts.CYCLE_INPUT, StructCodec.class.getMethod("cycleInput", MemorySegment.class));
        readers.put(Layouts.SIM_STATE, StructCodec.class.getMethod("simState", MemorySegment.class));
        try (Arena arena = Arena.ofConfined()) {
            for (Map.Entry<StructLayout, Method> entry : readers.entrySet()) {
                StructLayout layout = entry.getKey();
                MemorySegment segment = StructCodec.allocate(arena, layout);
                assertEquals(layout.byteSize(), segment.get(JAVA_INT, 0));
                Map<String, Object> written = new LinkedHashMap<>();
                for (MemoryLayout member : layout.memberLayouts()) {
                    String field = member.name().orElseThrow();
                    long offset = layout.byteOffset(MemoryLayout.PathElement.groupElement(field));
                    if (field.equals("struct_size")) {
                        continue;
                    }
                    if (member instanceof SequenceLayout sequence) {
                        String text = field + "é";
                        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
                        assertTrue(bytes.length < sequence.byteSize());
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
                Object record = entry.getValue().invoke(null, segment);
                for (RecordComponent component : record.getClass().getRecordComponents()) {
                    String field = camelToSnake(component.getName().replaceAll("Value$", ""));
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
            MemorySegment input = StructCodec.allocate(arena, Layouts.CYCLE_INPUT);
            CycleInput written = new CycleInput(Health.ALL.mask(), -2L, CycleCommand.DRIVE.code(), -1000, 1000,
                    CycleFlag.TAKE_BUTTONS.mask());
            StructCodec.writeCycleInput(input, written);
            assertEquals(written, StructCodec.cycleInput(input));
            assertEquals(EnumSet.allOf(Health.class).stream().filter(h -> h != Health.ALL).toList(),
                    List.copyOf(written.health()));
            assertEquals(CycleCommand.DRIVE, written.command());
            assertEquals(java.util.Set.of(CycleFlag.TAKE_BUTTONS), written.flags());

            MemorySegment options = StructCodec.allocate(arena, Layouts.OPEN_OPTIONS);
            byte[] path = "/dev/gpiochip0".getBytes(StandardCharsets.UTF_8);
            StructCodec.writeOpenOptions(options, 2, 8, 500, path, new byte[0]);
            assertEquals(272, options.get(JAVA_INT, 0));
            assertEquals(2, options.get(JAVA_INT, 4));
            assertEquals(8, options.get(JAVA_INT, 8));
            assertEquals(500, options.get(JAVA_INT, 12));
            assertEquals("/dev/gpiochip0", options.getString(16));
            assertEquals("", options.getString(144));
            byte[] tooLong = new byte[Limits.PATH_MAX];
            assertThrows(IllegalArgumentException.class,
                    () -> StructCodec.writeOpenOptions(options, 0, 0, 0, tooLong, new byte[0]));
        }
    }

    /** The checked-in generated sources equal a fresh run of keelgen.py against keel.h. */
    @Test
    void generatedSourcesEqualAFreshKeelgenRun() throws IOException, InterruptedException {
        String python = System.getenv().getOrDefault("PYTHON", "python3");
        Process probe;
        try {
            probe = new ProcessBuilder(python, "-c", "import pycparser").redirectErrorStream(true).start();
        } catch (IOException missing) {
            Assumptions.abort("python3 is not installed, so keelgen.py --check cannot run here");
            return;
        }
        Assumptions.assumeTrue(probe.waitFor() == 0,
                "pycparser is not installed, so keelgen.py --check cannot run here (the CI generate job runs it)");
        Process process = new ProcessBuilder(python, REPOSITORY.resolve("software/bindings/tools/keelgen.py")
                .toString(), "--check", "--lang", "java").redirectErrorStream(true).start();
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertTrue(process.waitFor(120, TimeUnit.SECONDS));
        assertEquals(0, process.exitValue(), output);
    }
}
