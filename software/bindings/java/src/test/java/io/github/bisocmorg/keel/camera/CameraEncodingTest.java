// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.camera.encoding.ChunkFlag;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.encoding.EncodedChunk;
import io.github.bisocmorg.keel.camera.encoding.EncoderOptions;
import io.github.bisocmorg.keel.camera.encoding.H264Profile;
import io.github.bisocmorg.keel.camera.errors.CameraArgumentException;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.errors.CameraNotFoundException;
import io.github.bisocmorg.keel.camera.errors.CameraStateException;
import io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;

/** The hardware encoders, through the synthetic encoder: H.264 access units and MJPEG images the program reads. */
@ExtendWith(CameraSourceExtension.class)
class CameraEncodingTest {
    private static Camera open() {
        return Camera.open(CameraOptions.builder().width(320).height(240).framerate(120).build());
    }

    private static List<Integer> nalTypes(byte[] data) {
        List<Integer> types = new ArrayList<>();
        for (int i = 0; i + 3 < data.length; i++) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                types.add(data[i + 3] & 0x1F);
                i += 2;
            }
        }
        return types;
    }

    @Test
    void h264ChunksStartWithAKeyframeAndItsParameterSets() {
        try (Camera camera = open()) {
            camera.setEncoder(EncoderOptions.builder(Codec.H264).intraPeriod(4).build());
            assertEquals(Codec.H264, camera.streamInfo().codec());
            assertTrue(camera.streamInfo().encodedBytesMax() > 0);
            camera.start();
            List<EncodedChunk> chunks = new ArrayList<>();
            for (int i = 0; i < 9; i++) {
                chunks.add(camera.readChunk(Duration.ofSeconds(2)));
            }
            EncodedChunk first = chunks.get(0);
            assertEquals(Codec.H264, first.codec());
            assertTrue(first.keyframe());
            assertTrue(first.info().headers());
            assertEquals(Set.of(ChunkFlag.KEYFRAME, ChunkFlag.HEADERS), first.info().flags());
            byte[] data = first.toByteArray();
            assertEquals(List.of(7, 8), nalTypes(data).subList(0, 2));
            assertTrue(nalTypes(data).contains(5));
            assertEquals(first.size(), first.info().bytes());
            assertEquals(first.size(), first.data().remaining());
            assertTrue(first.data().isReadOnly());
            assertEquals(List.of(true, false, false, false, true, false, false, false, true),
                    chunks.stream().map(EncodedChunk::keyframe).toList());
            assertFalse(chunks.get(4).info().headers());
            assertTrue(chunks.get(1).timestampNs() > first.timestampNs());
            assertTrue(chunks.get(1).sequence() > first.sequence());
            assertTrue(first.toString().startsWith("EncodedChunk[H264, "), first.toString());
            assertTrue(first.toString().endsWith(", keyframe]"), first.toString());
            CameraStats stats = camera.stats();
            assertEquals(9, stats.chunksRead());
            assertTrue(stats.chunksEncoded() >= 9);
        }
    }

    @Test
    void inlineHeadersRepeatTheParameterSetsAtEveryKeyframe() {
        try (Camera camera = open()) {
            camera.setEncoder(EncoderOptions.builder(Codec.H264).intraPeriod(3).inlineHeaders(true)
                    .profile(H264Profile.BASELINE).bitrate(500_000).encodedQueue(32).build());
            camera.start();
            for (int i = 0; i < 7; i++) {
                EncodedChunk chunk = camera.readChunk(Duration.ofSeconds(2));
                assertEquals(i % 3 == 0, chunk.keyframe());
                if (chunk.keyframe()) {
                    assertTrue(chunk.info().headers());
                }
            }
        }
    }

    @Test
    void chunksIteratesUntilTheCameraIsClosed() {
        Camera camera = open();
        camera.setEncoder(Codec.H264);
        camera.start();
        int count = 0;
        for (EncodedChunk chunk : camera.chunks()) {
            assertEquals(Codec.H264, chunk.codec());
            if (++count == 4) {
                camera.close();
            }
        }
        assertEquals(4, count);
    }

    @Test
    void mjpegChunksAreWholeJpegImages() {
        try (Camera camera = open()) {
            camera.setEncoder(EncoderOptions.builder(Codec.MJPEG).quality(70).build());
            camera.start();
            for (int i = 0; i < 3; i++) {
                byte[] data = camera.readChunk().toByteArray();
                assertEquals((byte) 0xFF, data[0]);
                assertEquals((byte) 0xD8, data[1]);
                assertEquals((byte) 0xD9, data[data.length - 1]);
            }
            assertEquals(Codec.MJPEG, EncoderOptions.mjpeg().codec());
            assertEquals(EncoderOptions.h264(), EncoderOptions.builder(Codec.H264).build());
        }
    }

    @Test
    void theEncoderCanBeRemovedAgain() {
        try (Camera camera = open()) {
            camera.setEncoder(Codec.MJPEG);
            camera.setEncoder(Codec.NONE);
            assertEquals(Codec.NONE, camera.streamInfo().codec());
            camera.start();
            assertThrows(CameraStateException.class, () -> camera.readChunk(Duration.ofMillis(200)));
        }
    }

    @Test
    void optionsForTheOtherCodecOrOutOfRangeAreRefused() {
        try (Camera camera = open()) {
            for (EncoderOptions options : List.of(EncoderOptions.builder(Codec.H264).quality(50).build(),
                    EncoderOptions.builder(Codec.MJPEG).bitrate(1_000_000).build(),
                    EncoderOptions.builder(Codec.MJPEG).intraPeriod(10).build(),
                    EncoderOptions.builder(Codec.H264).bitrate(1000).build())) {
                CameraArgumentException error = assertThrows(CameraArgumentException.class,
                        () -> camera.setEncoder(options));
                assertEquals("keel_camera_set_encoder", error.function());
            }
            camera.start();
            assertThrows(CameraStateException.class, () -> camera.setEncoder(Codec.H264));
        }
        assertThrows(IllegalArgumentException.class, () -> EncoderOptions.builder(Codec.H264).bitrate(-1).build());
        assertThrows(IllegalArgumentException.class, () -> EncoderOptions.builder(Codec.UNRECOGNIZED).build());
        assertThrows(NullPointerException.class, () -> EncoderOptions.builder(Codec.H264).profile(null).build());
    }

    @Test
    void aStreamLargerThanTheHardwareEncoderTakesIsUnsupported() {
        try (Camera camera = Camera.open(CameraOptions.builder().width(2304).height(1296).build())) {
            CameraUnsupportedException error = assertThrows(CameraUnsupportedException.class,
                    () -> camera.setEncoder(Codec.H264));
            assertTrue(error.getMessage().contains("1920"), error.getMessage());
        }
    }

    @Test
    void aMissingEncoderIsNotFound() {
        CameraSourceExtension.fault("encoder-missing");
        try (Camera camera = open()) {
            assertThrows(CameraNotFoundException.class, () -> camera.setEncoder(Codec.H264));
        }
    }

    @Test
    void anEncoderThatFailsWhileRunning() {
        CameraSourceExtension.fault("encoder-fail");
        try (Camera camera = open()) {
            camera.setEncoder(Codec.H264);
            camera.start();
            CameraException error = assertThrows(CameraException.class, () -> {
                for (int i = 0; i < 10; i++) {
                    camera.readChunk(Duration.ofSeconds(2));
                }
            });
            assertEquals(Status.E_PERIPHERAL, error.status());
            assertEquals(CaptureState.FAILED, camera.stats().state());
        }
    }

    @Test
    void aChunkCopiesItsBytes() {
        byte[] data = {1, 2, 3};
        EncodedChunk chunk = new EncodedChunk(new io.github.bisocmorg.keel.camera.encoding.ChunkInfo(
                Codec.MJPEG.code(), ChunkFlag.KEYFRAME.mask(), 3, 7, 9), data);
        data[0] = 9;
        assertArrayEquals(new byte[] {1, 2, 3}, chunk.toByteArray());
        assertEquals("EncodedChunk[MJPEG, 3 bytes, sequence 7, keyframe]", chunk.toString());
        EncodedChunk delta = new EncodedChunk(new io.github.bisocmorg.keel.camera.encoding.ChunkInfo(
                Codec.H264.code(), 0, 0, 8, 10), new byte[0]);
        assertEquals("EncodedChunk[H264, 0 bytes, sequence 8]", delta.toString());
    }
}
