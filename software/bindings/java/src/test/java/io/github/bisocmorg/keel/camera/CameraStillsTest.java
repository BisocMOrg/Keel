// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.camera.errors.CameraArgumentException;
import io.github.bisocmorg.keel.camera.errors.CameraNotFoundException;
import io.github.bisocmorg.keel.camera.errors.CameraStateException;
import io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException;
import io.github.bisocmorg.keel.camera.stills.StillFlag;
import io.github.bisocmorg.keel.camera.stills.StillImage;
import io.github.bisocmorg.keel.camera.stills.StillInfo;
import io.github.bisocmorg.keel.camera.stills.StillOptions;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import java.util.Set;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.junit.jupiter.api.io.TempDir;

/** JPEG stills: the hardware encoder up to 1920x1920, libjpeg above it, into memory and into files. */
@ExtendWith(CameraSourceExtension.class)
class CameraStillsTest {
    @TempDir
    Path directory;

    private static Camera open() {
        return Camera.open(CameraOptions.builder().width(320).height(240).framerate(120).build());
    }

    private static void assertJpeg(byte[] data) {
        assertEquals((byte) 0xFF, data[0]);
        assertEquals((byte) 0xD8, data[1]);
        assertEquals((byte) 0xD9, data[data.length - 1]);
    }

    @Test
    void aStillOnTheHardwareEncoder() {
        try (Camera camera = open()) {
            StillImage still = camera.captureStill(StillOptions.of(640, 480).withQuality(90));
            assertEquals(640, still.width());
            assertEquals(480, still.height());
            assertEquals(still.info().bytes(), still.toByteArray().length);
            assertEquals(still.info().bytes(), still.data().remaining());
            assertTrue(still.data().isReadOnly());
            assertJpeg(still.toByteArray());
            assertFalse(still.info().software());
            assertTrue(still.info().timestampNs() > 0);
            assertTrue(still.toString().startsWith("StillImage[640x480, "), still.toString());
        }
    }

    @Test
    void aFullResolutionStillUsesLibjpeg() {
        try (Camera camera = open()) {
            if (!Camera.versionString().contains("jpeg")) {
                assertThrows(CameraUnsupportedException.class, camera::captureStill);
                return;
            }
            StillImage still = camera.captureStill();
            assertEquals(3280, still.width());
            assertEquals(2464, still.height());
            assertTrue(still.info().software());
            assertEquals(Set.of(StillFlag.SOFTWARE), still.info().flags());
            assertTrue(still.toString().endsWith(", libjpeg]"), still.toString());
            assertJpeg(still.toByteArray());
        }
    }

    @Test
    void aStillIntoAFile() throws IOException {
        Path file = directory.resolve("photo.jpg");
        try (Camera camera = open()) {
            StillInfo info = camera.captureStillFile(file, StillOptions.of(1920, 1080));
            assertEquals(1920, info.width());
            assertEquals(1080, info.height());
            byte[] data = Files.readAllBytes(file);
            assertEquals(info.bytes(), data.length);
            assertJpeg(data);
            assertThrows(CameraNotFoundException.class,
                    () -> camera.captureStillFile(Path.of("/nonexistent-directory/photo.jpg"), StillOptions.of(640,
                            480)));
            assertThrows(IllegalArgumentException.class,
                    () -> camera.captureStillFile(Path.of("/tmp/" + "x".repeat(300) + ".jpg")));
        }
    }

    @Test
    void videoReturnsAfterAStill() {
        try (Camera camera = open()) {
            camera.captureStill(StillOptions.of(640, 480));
            camera.start();
            assertEquals(320, camera.readFrame().width());
        }
    }

    @Test
    void stillsNeedTheCaptureStopped() {
        try (Camera camera = open()) {
            camera.start();
            assertThrows(CameraStateException.class, () -> camera.captureStill(StillOptions.of(640, 480)));
            assertThrows(CameraStateException.class,
                    () -> camera.captureStillFile(directory.resolve("never.jpg"), StillOptions.of(640, 480)));
        }
    }

    @Test
    void stillOptionsAreCheckedByTheLibraryAndTheBinding() {
        try (Camera camera = open()) {
            for (StillOptions options : List.of(new StillOptions(640, 0, 0, Duration.ZERO), StillOptions.of(641, 480),
                    StillOptions.of(640, 480).withQuality(101))) {
                assertThrows(CameraArgumentException.class, () -> camera.captureStill(options));
            }
        }
        assertThrows(IllegalArgumentException.class, () -> StillOptions.defaults().withTimeout(Duration.ofSeconds(-1)));
        assertThrows(NullPointerException.class, () -> StillOptions.defaults().withTimeout(null));
        assertThrows(IllegalArgumentException.class, () -> StillOptions.of(-2, 2));
    }

    @Test
    void aStillOnTheAutofocusCameraAndItsCopies() {
        try (Camera camera = Camera.open(CameraOptions.builder().index(1).width(320).height(240).build())) {
            StillImage still = camera.captureStill(StillOptions.of(1536, 864).withTimeout(Duration.ofSeconds(5)));
            assertEquals(1536, still.width());
            assertEquals(864, still.height());
            byte[] jpeg = still.toByteArray();
            jpeg[0] = 0;
            assertJpeg(still.toByteArray());
            byte[] source = {1, 2};
            StillImage copy = new StillImage(still.info(), source);
            source[0] = 5;
            assertArrayEquals(new byte[] {1, 2}, copy.toByteArray());
        }
    }
}
