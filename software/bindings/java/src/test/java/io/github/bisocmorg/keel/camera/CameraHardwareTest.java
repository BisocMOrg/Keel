// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;
import static org.junit.jupiter.api.Assumptions.assumeFalse;

import io.github.bisocmorg.keel.AbiVersion;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.encoding.EncodedChunk;
import io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import io.github.bisocmorg.keel.camera.stills.StillImage;
import io.github.bisocmorg.keel.camera.stills.StillInfo;
import io.github.bisocmorg.keel.camera.stills.StillOptions;
import io.github.bisocmorg.keel.support.LocalResources;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import javax.imageio.ImageIO;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/**
 * Robot stage: the binding against the robot's real camera, through the installed libkeel-camera.
 *
 * <p>Run on the robot only, with the libkeel-camera1 package installed and {@code KEEL_CAMERA_LIBRARY} and
 * {@code KEEL_CAMERA_TEST_SOURCE} unset: {@code mvn -B -Phardware test}. The tests skip when no camera is connected.
 * The camera is not part of the safety function, and nothing here touches the Keel board.
 */
@Tag("hardware")
class CameraHardwareTest {
    private static ListedCamera listed;

    @TempDir
    Path directory;

    @BeforeAll
    static void findTheCamera() {
        if (System.getenv("KEEL_CAMERA_LIBRARY") != null || System.getenv("KEEL_CAMERA_TEST_SOURCE") != null) {
            fail("unset KEEL_CAMERA_LIBRARY and KEEL_CAMERA_TEST_SOURCE: the robot stage tests the real camera");
        }
        List<ListedCamera> cameras = Camera.listCameras();
        assumeFalse(cameras.isEmpty(), "no camera is connected");
        listed = cameras.get(0);
    }

    private static CameraOptions.Builder vga() {
        return CameraOptions.builder().index(listed.index()).width(640).height(480);
    }

    @Test
    void theInstalledLibraryAndTheListing() {
        assertFalse(Camera.versionString().contains("test-hooks"), Camera.versionString());
        assertEquals(new AbiVersion(1, 0), Camera.abiVersion());
        assertTrue(listed.model().startsWith("Camera Module"), listed.model());
        assertTrue(listed.sensor().startsWith("imx219") || listed.sensor().startsWith("imx708"), listed.sensor());
        assertEquals(listed.sensor().startsWith("imx708"), listed.autofocus());
        assertTrue(listed.modes().stream().allMatch(mode -> mode.maxFramerate() > 0));
    }

    @Test
    void framesCarryAnImage() {
        try (Camera camera = Camera.open(vga().pixelFormat(PixelFormat.BGR888).build())) {
            camera.start();
            Frame frame = null;
            long previous = -1;
            for (int i = 0; i < 10; i++) {
                frame = camera.readFrame(Duration.ofSeconds(3));
                assertTrue(frame.sequence() > previous);
                previous = frame.sequence();
            }
            BufferedImage image = frame.toBufferedImage();
            assertEquals(640, image.getWidth());
            byte[] pixels = frame.toByteArray();
            int low = 255;
            int high = 0;
            for (byte pixel : pixels) {
                low = Math.min(low, pixel & 0xFF);
                high = Math.max(high, pixel & 0xFF);
            }
            assertTrue(high > low, "a constant picture");
        }
    }

    @Test
    void i420FramesAndTheCpuConversion() {
        try (Camera camera = Camera.open(vga().build())) {
            camera.start();
            Frame frame = camera.readFrame(Duration.ofSeconds(3));
            assertEquals(640 * 480 * 3, frame.convert(PixelFormat.RGB888).toByteArray().length);
        }
    }

    @Test
    void h264StartsWithAKeyframeWithinTwoSeconds() {
        try (Camera camera = Camera.open(vga().build())) {
            camera.setEncoder(Codec.H264);
            camera.start();
            long started = System.nanoTime();
            EncodedChunk chunk = camera.readChunk(Duration.ofSeconds(2));
            assertTrue(System.nanoTime() - started < 2_000_000_000L);
            assertTrue(chunk.keyframe());
            assertTrue(chunk.info().headers());
            assertEquals(1, chunk.toByteArray()[3]);
        }
    }

    @Test
    void aListeningPortStreamsToAReceiver() throws IOException {
        int port = LocalResources.freePort();
        try (Camera camera = Camera.open(vga().build())) {
            camera.setEncoder(Codec.H264);
            int output = camera.startOutput("tcp://127.0.0.1:" + port + "?listen");
            camera.start();
            try (Socket receiver = new Socket("127.0.0.1", port)) {
                InputStream in = receiver.getInputStream();
                byte[] data = in.readNBytes(20000);
                assertEquals(7, data[4] & 0x1F);
            }
            assertTrue(camera.outputStats(output).chunks() > 0);
        }
    }

    @Test
    void mjpegAndHardwareStills() throws IOException {
        try (Camera camera = Camera.open(CameraOptions.builder().index(listed.index()).width(1280).height(720)
                .build())) {
            camera.setEncoder(Codec.MJPEG);
            camera.start();
            assertEquals((byte) 0xD8, camera.readChunk(Duration.ofSeconds(3)).toByteArray()[1]);
            camera.stop();
            StillImage still = camera.captureStill(StillOptions.of(1920, 1080));
            assertEquals(1920, still.width());
            assertFalse(still.info().software());
            BufferedImage decoded = ImageIO.read(new ByteArrayInputStream(still.toByteArray()));
            assertEquals(1920, decoded.getWidth());
            assertEquals(1080, decoded.getHeight());
            Path file = directory.resolve("photo.jpg");
            StillInfo info = camera.captureStillFile(file, StillOptions.of(1000, 750));
            assertEquals(1000, info.width());
            assertEquals((byte) 0xD8, Files.readAllBytes(file)[1]);
        }
    }

    @Test
    void focusOptionsFollowTheLens() {
        if (listed.autofocus()) {
            try (Camera camera = Camera.open(vga().afMode(AutofocusMode.AUTO).build())) {
                camera.start();
                AutofocusState state = camera.triggerAutofocus(Duration.ofSeconds(3));
                assertTrue(state == AutofocusState.FOCUSED || state == AutofocusState.FAILED, state.toString());
            }
        } else {
            CameraUnsupportedException error = org.junit.jupiter.api.Assertions.assertThrows(
                    CameraUnsupportedException.class,
                    () -> Camera.open(vga().afMode(AutofocusMode.CONTINUOUS).build()));
            assertTrue(error.getMessage().contains("fixed-focus"), error.getMessage());
        }
    }

    @Test
    void closingLeavesNoLibraryThread() throws InterruptedException {
        long before = LocalResources.threads();
        try (Camera camera = Camera.open(vga().build())) {
            camera.setEncoder(Codec.H264);
            camera.start();
            camera.readChunk(Duration.ofSeconds(3));
        }
        for (int i = 0; i < 60 && LocalResources.threads() > before; i++) {
            Thread.sleep(50);
        }
        assertEquals(before, LocalResources.threads());
    }
}
