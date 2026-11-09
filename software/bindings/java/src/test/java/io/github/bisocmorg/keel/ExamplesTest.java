// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import java.io.File;
import java.io.IOException;
import java.io.StringWriter;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.tools.JavaCompiler;
import javax.tools.ToolProvider;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/**
 * Every example compiled against the built classes and run with {@code --simulator} on the real-time clock, the
 * README quick start, and each example's motion check called directly.
 */
class ExamplesTest {
    private static final Path EXAMPLES = Path.of(System.getProperty("keel.examples.dir"));
    private static final String CLASSES = System.getProperty("keel.classes.dir");
    private static final Path README = Path.of(System.getProperty("keel.readme"));
    private static final String JAVA = Path.of(System.getProperty("java.home"), "bin", "java").toString();

    @TempDir
    static Path compiled;

    private record Result(int status, String output) {
    }

    @BeforeAll
    static void compileTheExamples() throws IOException {
        try (var files = Files.list(EXAMPLES)) {
            compile(files.filter(path -> path.toString().endsWith(".java")).toList(), compiled);
        }
    }

    private static void compile(List<Path> sources, Path output) {
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        StringWriter diagnostics = new StringWriter();
        List<String> options = List.of("--release", "25", "-Xlint:all", "-Werror", "-classpath", CLASSES, "-d",
                output.toString());
        List<String> names = sources.stream().map(Path::toString).toList();
        var fileManager = compiler.getStandardFileManager(null, null, StandardCharsets.UTF_8);
        boolean ok = compiler.getTask(diagnostics, fileManager, null, options, null,
                fileManager.getJavaFileObjectsFromStrings(names)).call();
        assertTrue(ok, diagnostics.toString());
        assertTrue(diagnostics.toString().isEmpty(), diagnostics.toString());
    }

    private static Result run(Path classes, Map<String, String> environment, String mainClass, String... args)
            throws IOException, InterruptedException {
        List<String> command = new ArrayList<>(List.of(JAVA, "--enable-native-access=ALL-UNNAMED",
                "--illegal-native-access=deny", "-cp", CLASSES + File.pathSeparator + classes, mainClass));
        command.addAll(List.of(args));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        builder.environment().putAll(environment);
        Process process = builder.start();
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (!process.waitFor(90, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            fail(mainClass + " did not finish:\n" + output);
        }
        return new Result(process.exitValue(), output);
    }

    private static Result run(String mainClass, String... args) throws IOException, InterruptedException {
        return run(compiled, Map.of(), mainClass, args);
    }

    private static void assertSucceeds(Result result, String... lines) {
        assertEquals(0, result.status(), result.output());
        for (String line : lines) {
            assertTrue(result.output().contains(line), "missing \"" + line + "\" in:\n" + result.output());
        }
    }

    @Test
    void drive() throws IOException, InterruptedException {
        assertSucceeds(run("Drive", "--simulator"), "opened SIMULATOR", "arming",
                "drove 2000 ms at 20 percent; output 0/0", "task complete; robot disarmed and closed");
    }

    /** Exercises the same sequential task example a student runs, on the native simulator. */
    @Test
    void sequentialMoves() throws IOException, InterruptedException {
        assertSucceeds(run("SequentialMoves", "--simulator"));
    }

    /** Refuses unspecified physical motion before the example can open a real device. */
    @Test
    void sequentialMovesRequiresExplicitMotionChoice() throws IOException, InterruptedException {
        Result result = run("SequentialMoves");
        assertEquals(2, result.status());
        assertTrue(result.output().contains("requires --allow-motion"));
    }

    @Test
    void buttons() throws IOException, InterruptedException {
        assertSucceeds(run("Buttons", "--simulator", "--duration", "300"), "buttons: done");
        assertSucceeds(run("Buttons", "--simulator", "--drive", "300", "-300", "500"), "bounded drive started", "drive stopped; robot disarmed");
    }

    @Test
    void leds() throws IOException, InterruptedException {
        assertSucceeds(run("Leds", "--simulator", "--duration", "600"), "frame: RGB1..RGB6 walking",
                "state: FAULTED; error colour shown", "LEDs off");
    }

    @Test
    void ranging() throws IOException, InterruptedException {
        assertSucceeds(run("Ranging", "--simulator"), "range 1: VALID 500 mm", "range 5: VALID 500 mm",
                "range_measure from the main thread: VALID 500 mm");
        assertSucceeds(run("Ranging", "--simulator", "--count", "2"), "range 2: VALID 500 mm");
    }

    @Test
    void safety() throws IOException, InterruptedException {
        assertSucceeds(run("Safety", "--simulator"), "managed session armed; motors remain at zero",
                "emergency stop: E_FAULTED, cause USER_STOP", "explicit fault recovery completed");
    }

    @Test
    void usageErrorsExitWithTwo() throws IOException, InterruptedException {
        assertEquals(2, run("Drive", "--bogus").status());
        assertEquals(2, run("Buttons", "--drive", "2000", "0", "10").status());
        assertEquals(2, run("Leds", "--duration").status());
        assertEquals(2, run("Ranging", "--fast").status());
        assertEquals(2, run("Ranging", "--count", "0").status());
        assertEquals(2, run("Safety", "--bogus").status());
        assertEquals(0, run("Drive", "--help").status());
    }

    @Test
    void motionNeedsTheSimulatorOrExplicitIntent() throws Exception {
        try (URLClassLoader loader = new URLClassLoader(new URL[] {compiled.toUri().toURL()},
                getClass().getClassLoader())) {
            for (String example : List.of("Drive", "Safety")) {
                Method check = loader.loadClass(example).getDeclaredMethod("motionAllowed", Set.class, boolean.class);
                check.setAccessible(true);
                Set<Capability> simulated = EnumSet.of(Capability.SIMULATED, Capability.MOTORS);
                Set<Capability> robot = EnumSet.of(Capability.MOTORS, Capability.BRAKE, Capability.REALTIME_PWM);
                assertTrue((boolean) check.invoke(null, simulated, false), example);
                assertTrue((boolean) check.invoke(null, simulated, true), example);
                assertFalse((boolean) check.invoke(null, robot, false), example);
                assertTrue((boolean) check.invoke(null, robot, true), example);
            }
        }
    }

    /** The README quick start opens with AUTO, so it runs only against a library whose one backend is the simulator. */
    @Test
    void theReadmeQuickStart(@TempDir Path work) throws IOException, InterruptedException {
        assertTrue(Keel.versionString().endsWith("backends simulator)"),
                "the quick start opens with AUTO; run this suite against a sim-only libkeel, not "
                        + Keel.versionString());
        Matcher block = Pattern.compile("## Quick start.*?```java\\n(.*?)```", Pattern.DOTALL)
                .matcher(Files.readString(README));
        assertTrue(block.find(), "README.md has no Java quick start");
        Path source = work.resolve("QuickStart.java");
        Files.writeString(source, block.group(1));
        Path classes = Files.createDirectory(work.resolve("classes"));
        compile(List.of(source), classes);
        Result result = run(classes, Map.of("KEEL_BACKEND", "simulator"), "QuickStart");
        assertSucceeds(result);
    }
}
