// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/**
 * The packaged jar on the module path: the module descriptor, the native-access grant by module
 * name under {@code --illegal-native-access=deny}, the drive and still examples run against it with the source
 * launcher, and the camera's optional use of {@code java.desktop}.
 */
class ModulePathIT {
    private static final String JAR = System.getProperty("keel.jar");
    private static final Path DRIVE = Path.of(System.getProperty("keel.examples.dir"), "Drive.java");
    private static final Path STILL = Path.of(System.getProperty("keel.examples.dir"), "CameraStill.java");
    private static final String JAVA = Path.of(System.getProperty("java.home"), "bin", "java").toString();

    private record Result(int status, String output) {
    }

    private static Result java(String... args) throws IOException, InterruptedException {
        List<String> command = new ArrayList<>(List.of(JAVA));
        command.addAll(List.of(args));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        builder.environment().put("KEEL_CAMERA_TEST_SOURCE", "1");
        Process process = builder.start();
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (!process.waitFor(90, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            fail("the JVM did not finish:\n" + output);
        }
        return new Result(process.exitValue(), output);
    }

    @Test
    void theModuleExportsOnlyItsApi() throws IOException, InterruptedException {
        Result result = java("--module-path", JAR, "--describe-module", "io.github.bisocmorg.keel");
        assertEquals(0, result.status(), result.output());
        assertTrue(result.output().startsWith("io.github.bisocmorg.keel@" + Keel.VERSION), result.output());
        for (String api : List.of("", ".errors", ".loop", ".peripherals", ".sim", ".state", ".camera",
                ".camera.encoding", ".camera.errors", ".camera.frames", ".camera.outputs", ".camera.stills")) {
            assertTrue(result.output().contains("exports io.github.bisocmorg.keel" + api + "\n"), result.output());
        }
        assertTrue(result.output().contains("contains io.github.bisocmorg.keel.internal\n"), result.output());
        assertTrue(result.output().contains("contains io.github.bisocmorg.keel.internal.camera\n"), result.output());
        assertFalse(result.output().contains("exports io.github.bisocmorg.keel.internal"), result.output());
        assertTrue(result.output().contains("requires java.base mandated"), result.output());
        // The modifiers come in no fixed order.
        assertTrue(result.output().lines().anyMatch(line -> line.startsWith("requires java.desktop ")
                && List.of(line.split(" ")).containsAll(List.of("static", "transitive"))), result.output());
        assertFalse(result.output().contains("requires java.logging"), result.output());
    }

    @Test
    void theDriveExampleRunsOnTheModulePathWithTheGrant() throws IOException, InterruptedException {
        Result result = java("--module-path", JAR, "--add-modules", "io.github.bisocmorg.keel",
                "--enable-native-access=io.github.bisocmorg.keel", "--illegal-native-access=deny", DRIVE.toString(),
                "--simulator");
        assertEquals(0, result.status(), result.output());
        assertTrue(result.output().contains("state: DISARMED"), result.output());
        assertFalse(result.output().contains("WARNING"), result.output());
    }

    @Test
    void withoutTheGrantLoadingFailsWithTheRemedy() throws IOException, InterruptedException {
        Result result = java("--module-path", JAR, "--add-modules", "io.github.bisocmorg.keel",
                "--illegal-native-access=deny", DRIVE.toString(), "--simulator");
        assertEquals(1, result.status(), result.output());
        assertTrue(result.output().contains("native access is denied to the Keel binding"), result.output());
        assertTrue(result.output().contains("--enable-native-access=io.github.bisocmorg.keel"), result.output());
    }

    @Test
    void theStillExampleRunsOnTheModulePath(@TempDir Path directory) throws IOException, InterruptedException {
        Path photo = directory.resolve("photo.jpg");
        Result result = java("--module-path", JAR, "--add-modules", "io.github.bisocmorg.keel",
                "--enable-native-access=io.github.bisocmorg.keel", "--illegal-native-access=deny", STILL.toString(),
                "-o", photo.toString(), "--width", "640", "--height", "480");
        assertEquals(0, result.status(), result.output());
        assertTrue(result.output().contains("still: 640x480"), result.output());
        assertFalse(result.output().contains("WARNING"), result.output());
    }

    @Test
    void withoutJavaDesktopToBufferedImageNamesTheModule(@TempDir Path directory)
            throws IOException, InterruptedException {
        Path probe = directory.resolve("ImageProbe.java");
        Files.writeString(probe, """
                import io.github.bisocmorg.keel.camera.frames.Frame;
                import io.github.bisocmorg.keel.camera.frames.FrameInfo;
                import java.nio.ByteBuffer;

                public class ImageProbe {
                    public static void main(String[] args) {
                        Frame frame = new Frame(new FrameInfo(3, 2, 2, 1, 6, 0, 0, 0, 0, 0, 12, 0, 0),
                                ByteBuffer.allocateDirect(12));
                        try {
                            frame.toBufferedImage();
                            System.out.println("an image");
                        } catch (UnsupportedOperationException missing) {
                            System.out.println(missing.getMessage());
                        }
                    }
                }
                """);
        Result result = java("--module-path", JAR, "--add-modules", "io.github.bisocmorg.keel",
                "--limit-modules", "io.github.bisocmorg.keel,jdk.compiler", probe.toString());
        assertEquals(0, result.status(), result.output());
        assertTrue(result.output().contains("toBufferedImage() needs the java.desktop module: add --add-modules "
                + "java.desktop"), result.output());
        Result desktop = java("--module-path", JAR, "--add-modules", "io.github.bisocmorg.keel,java.desktop",
                probe.toString());
        assertTrue(desktop.output().contains("an image"), desktop.output());
    }
}
