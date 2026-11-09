// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import io.github.bisocmorg.keel.support.CameraSourceExtension;
import io.github.bisocmorg.keel.support.LocalResources;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.StringWriter;
import java.net.ConnectException;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.tools.JavaCompiler;
import javax.tools.ToolProvider;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.junit.jupiter.api.io.TempDir;

/** The camera examples compiled against the built classes and run on the synthetic camera, and the README's part. */
@ExtendWith(CameraSourceExtension.class)
class CameraExamplesTest {
    private static final Path EXAMPLES = Path.of(System.getProperty("keel.examples.dir"));
    private static final String CLASSES = System.getProperty("keel.classes.dir");
    private static final Path README = Path.of(System.getProperty("keel.readme"));
    private static final String JAVA = Path.of(System.getProperty("java.home"), "bin", "java").toString();

    @TempDir
    static Path compiled;

    @TempDir
    Path directory;

    private record Result(int status, String output) {
    }

    @BeforeAll
    static void compileTheExamples() {
        List<Path> sources = List.of(EXAMPLES.resolve("CameraFrames.java"), EXAMPLES.resolve("CameraStream.java"),
                EXAMPLES.resolve("CameraStill.java"));
        compile(sources, compiled);
    }

    private static void compile(List<Path> sources, Path output) {
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        StringWriter diagnostics = new StringWriter();
        List<String> options = List.of("--release", "25", "-Xlint:all", "-Werror", "-classpath", CLASSES, "-d",
                output.toString());
        var fileManager = compiler.getStandardFileManager(null, null, StandardCharsets.UTF_8);
        boolean ok = compiler.getTask(diagnostics, fileManager, null, options, null,
                fileManager.getJavaFileObjectsFromStrings(sources.stream().map(Path::toString).toList())).call();
        assertTrue(ok, diagnostics.toString());
        assertTrue(diagnostics.toString().isEmpty(), diagnostics.toString());
    }

    private static Process start(Path classes, String mainClass, String... args) throws IOException {
        return start(null, classes, mainClass, args);
    }

    private static Process start(Path workingDirectory, Path classes, String mainClass, String... args)
            throws IOException {
        List<String> command = new ArrayList<>(List.of(JAVA, "--enable-native-access=ALL-UNNAMED",
                "--illegal-native-access=deny", "-cp", CLASSES + File.pathSeparator + classes, mainClass));
        command.addAll(List.of(args));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        builder.environment().put("KEEL_CAMERA_TEST_SOURCE", "1");
        if (workingDirectory != null) {
            builder.directory(workingDirectory.toFile());
        }
        return builder.start();
    }

    private static Result finish(Process process, String what) throws IOException, InterruptedException {
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (!process.waitFor(90, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            fail(what + " did not finish:\n" + output);
        }
        return new Result(process.exitValue(), output);
    }

    private static Result run(String mainClass, String... args) throws IOException, InterruptedException {
        return finish(start(compiled, mainClass, args), mainClass);
    }

    private static void assertSucceeds(Result result, String... lines) {
        assertEquals(0, result.status(), result.output());
        for (String line : lines) {
            assertTrue(result.output().contains(line), "missing \"" + line + "\" in:\n" + result.output());
        }
    }

    @Test
    void framesIntoVisionCode() throws IOException, InterruptedException {
        Result result = run("CameraFrames", "--frames", "20", "--width", "320", "--height", "240", "--framerate",
                "120");
        assertSucceeds(result, "frames: Camera Module 2 (imx219)", "frames: done, 20 read");
        assertTrue(Pattern.compile("frame \\d+: brightness \\d+\\.\\d, motion \\d+\\.\\d").matcher(result.output())
                .find(), result.output());
    }

    @Test
    void streamToAFile() throws IOException, InterruptedException {
        Path file = directory.resolve("clip.h264");
        Result result = run("CameraStream", file.toString(), "--seconds", "1.5", "--width", "320", "--height", "240");
        assertSucceeds(result, "stream: H264 320x240 to " + file, "stream: done");
        assertTrue(result.output().contains("stream: RUNNING, 1 receiver(s), "), result.output());
        byte[] data = Files.readAllBytes(file);
        assertEquals(1, data[3]);
    }

    private static Socket connect(int port) throws IOException, InterruptedException {
        long deadline = System.nanoTime() + 10_000_000_000L;
        while (true) {
            try {
                return new Socket("127.0.0.1", port);
            } catch (ConnectException notYet) {
                assertTrue(System.nanoTime() < deadline, "the example did not listen");
                Thread.sleep(100);
            }
        }
    }

    @Test
    void streamToAListeningPortUntilCtrlC() throws IOException, InterruptedException {
        int port = LocalResources.freePort();
        Process process = start(compiled, "CameraStream", "tcp://127.0.0.1:" + port + "?listen", "--mjpeg");
        try {
            try (Socket receiver = connect(port)) {
                InputStream in = receiver.getInputStream();
                assertEquals(0xFF, in.read());
                Thread.sleep(1500);
            }
            assertEquals(0, new ProcessBuilder("kill", "-INT", Long.toString(process.pid())).start().waitFor());
            Result result = finish(process, "CameraStream");
            assertEquals(130, result.status(), result.output());
            assertTrue(result.output().contains("stream: MJPEG 640x480"), result.output());
            assertTrue(result.output().contains("1 receiver(s)"), result.output());
        } finally {
            process.destroyForcibly();
        }
    }

    @Test
    void aStillPhoto() throws IOException, InterruptedException {
        Path photo = directory.resolve("photo.jpg");
        Result small = run("CameraStill", "-o", photo.toString(), "--width", "640", "--height", "480", "--quality",
                "90");
        assertSucceeds(small, "still: 640x480, ", "by the hardware encoder");
        assertEquals((byte) 0xD8, Files.readAllBytes(photo)[1]);
        assertSucceeds(run("CameraStill", "-o", directory.resolve("full.jpg").toString()), "still: 3280x2464, ");
    }

    @Test
    void usageAndCameraErrors() throws IOException, InterruptedException {
        assertEquals(2, run("CameraFrames", "--frames", "0").status());
        assertEquals(2, run("CameraStream", "--seconds", "-1").status());
        assertEquals(2, run("CameraStill", "--width", "640").status());
        Result missing = run("CameraStill", "--camera", "5");
        assertEquals(1, missing.status(), missing.output());
        assertTrue(missing.output().contains("still: keel_camera_open: KEEL_E_NOT_FOUND"), missing.output());
        Result refused = run("CameraStream", "/tmp/x.avi");
        assertEquals(1, refused.status(), refused.output());
        assertTrue(refused.output().contains("stream: keel_camera_output_start: KEEL_E_INVALID_ARGUMENT"),
                refused.output());
    }

    @Test
    void theReadmeCameraExample() throws IOException, InterruptedException {
        String text = Files.readString(README);
        String section = text.substring(text.indexOf("## Camera"));
        Matcher match = Pattern.compile("```java\n(.*?)```", Pattern.DOTALL).matcher(section);
        assertTrue(match.find(), "the README's camera section has no Java block");
        Path source = directory.resolve("ReadmeCamera.java");
        Files.writeString(source, match.group(1));
        Path classes = directory.resolve("classes");
        compile(List.of(source), classes);
        assertSucceeds(finish(start(directory, classes, "ReadmeCamera"), "ReadmeCamera"));
        assertEquals(1, Files.readAllBytes(directory.resolve("drive.h264"))[3]);
        assertEquals((byte) 0xD8, Files.readAllBytes(directory.resolve("photo.jpg"))[1]);
    }
}
