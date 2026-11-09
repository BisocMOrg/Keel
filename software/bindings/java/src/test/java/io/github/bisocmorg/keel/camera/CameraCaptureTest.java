// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.errors.CameraArgumentException;
import io.github.bisocmorg.keel.camera.errors.CameraBusyException;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.errors.CameraNotFoundException;
import io.github.bisocmorg.keel.camera.errors.CameraPermissionException;
import io.github.bisocmorg.keel.camera.errors.CameraResource;
import io.github.bisocmorg.keel.camera.errors.CameraStateException;
import io.github.bisocmorg.keel.camera.errors.CameraTimeoutException;
import io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException;
import io.github.bisocmorg.keel.camera.errors.ErrorReport;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.camera.frames.FrameInfo;
import io.github.bisocmorg.keel.camera.frames.PixelFormat;
import io.github.bisocmorg.keel.camera.frames.StreamInfo;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.support.CameraPatterns;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import java.awt.image.BufferedImage;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

/** Capture: open options, stream layouts, raw frames, conversion, images, drops and focus. */
@ExtendWith(CameraSourceExtension.class)
class CameraCaptureTest {
    private static CameraOptions.Builder small(int width, int height) {
        return CameraOptions.builder().width(width).height(height).framerate(120);
    }

    private static Frame readStarted(CameraOptions options) {
        try (Camera camera = Camera.open(options)) {
            camera.start();
            return camera.readFrame(Duration.ofSeconds(2));
        }
    }

    // --------------------------------------------------------------------- open ----

    @Test
    void defaults() {
        try (Camera camera = Camera.open()) {
            assertEquals(0, camera.index());
            assertEquals("imx219", camera.info().sensor());
            StreamInfo stream = camera.streamInfo();
            assertEquals(PixelFormat.I420, stream.format());
            assertEquals(640, stream.width());
            assertEquals(480, stream.height());
            assertEquals(30.0, stream.framerate());
            assertEquals(3, stream.planeCount());
            assertEquals(640, stream.stride0());
            assertEquals(320, stream.stride1());
            assertEquals(640 * 480, stream.offset1());
            assertEquals(640 * 480 + 320 * 240, stream.offset2());
            assertEquals(640 * 480 * 3 / 2, stream.frameBytes());
            assertEquals(Codec.NONE, stream.codec());
            assertEquals(0, stream.encodedBytesMax());
            assertEquals(CaptureState.STOPPED, camera.stats().state());
            ErrorReport report = camera.lastError();
            assertEquals(Status.OK, report.status());
            assertEquals("", report.message());
            assertEquals("Camera[0: Camera Module 2 (imx219), open]", camera.toString());
        }
    }

    @Test
    void layouts() {
        record Case(PixelFormat format, int width, int height, long stride, long bytes) {
        }
        for (Case each : List.of(new Case(PixelFormat.I420, 1280, 720, 1280, 1280 * 720 * 3 / 2),
                new Case(PixelFormat.I420, 1000, 750, 1024, 1024 * 750 + 512 * 375 * 2),
                new Case(PixelFormat.RGB888, 640, 480, 1920, 1920 * 480),
                new Case(PixelFormat.BGR888, 100, 64, 320, 320 * 64))) {
            CameraOptions options = CameraOptions.builder().width(each.width()).height(each.height()).framerate(15)
                    .pixelFormat(each.format()).build();
            try (Camera camera = Camera.open(options)) {
                StreamInfo stream = camera.streamInfo();
                assertEquals(each.width(), stream.width());
                assertEquals(each.stride(), stream.stride0());
                assertEquals(each.bytes(), stream.frameBytes());
                assertEquals(15000, stream.framerateMilli());
            }
        }
    }

    @Test
    void aSecondHandleOnTheSameCameraIsBusy() {
        try (Camera first = Camera.open(small(160, 120).build())) {
            CameraBusyException busy = assertThrows(CameraBusyException.class, () -> Camera.open(small(160, 120)
                    .build()));
            assertEquals(Status.E_IN_USE, busy.status());
            assertTrue(busy.getMessage().contains("in use"), busy.getMessage());
            try (Camera other = Camera.open(small(160, 120).index(1).build())) {
                assertEquals("imx708", other.info().sensor());
            }
            assertFalse(first.isClosed());
        }
        Camera.open(small(160, 120).build()).close();
    }

    @Test
    void setupFailures() {
        record Case(String fault, Class<? extends CameraException> type, CameraResource resource, String words) {
        }
        for (Case each : List.of(
                new Case("no-camera", CameraNotFoundException.class, CameraResource.CAMERA, "ribbon cable"),
                new Case("busy", CameraBusyException.class, CameraResource.CAMERA, "in use by another process"),
                new Case("permission", CameraPermissionException.class, CameraResource.DEVICE_NODE, "keel-setup"),
                new Case("ipa-missing", CameraNotFoundException.class, CameraResource.IPA, "IPA module"))) {
            CameraSourceExtension.fault(each.fault());
            CameraException error = assertThrows(each.type(), Camera::open);
            assertEquals("keel_camera_open", error.function());
            assertEquals(each.resource(), error.report().orElseThrow().resource());
            assertTrue(error.getMessage().contains(each.words()), error.getMessage());
        }
    }

    @Test
    void aCameraIndexPastTheLastOne() {
        CameraNotFoundException error = assertThrows(CameraNotFoundException.class,
                () -> Camera.open(CameraOptions.builder().index(2).build()));
        assertTrue(error.getMessage().contains("there is no camera 2"), error.getMessage());
    }

    @Test
    void outOfRangeOptionsAreRefusedByTheLibrary() {
        List<Function<CameraOptions.Builder, CameraOptions.Builder>> changes = List.of(b -> b.width(11),
                b -> b.width(32).height(32), b -> b.framerate(500), b -> b.rotation(90), b -> b.frameQueue(9),
                b -> b.exposureUs(2_000_000), b -> b.gain(0.5), b -> b.ev(9), b -> b.redGain(1.5));
        for (Function<CameraOptions.Builder, CameraOptions.Builder> change : changes) {
            CameraArgumentException error = assertThrows(CameraArgumentException.class,
                    () -> Camera.open(change.apply(CameraOptions.builder()).build()));
            assertEquals(CameraResource.OPTIONS, error.report().orElseThrow().resource());
        }
    }

    @Test
    void theBindingRefusesWhatTheCTypesCannotHold() {
        assertThrows(IllegalArgumentException.class, () -> CameraOptions.builder().framerate(Double.NaN).build());
        assertThrows(IllegalArgumentException.class, () -> CameraOptions.builder().gain(-1).build());
        assertThrows(IllegalArgumentException.class, () -> CameraOptions.builder().ev(3e6).build());
        assertThrows(IllegalArgumentException.class, () -> CameraOptions.builder().gain(5e6).build());
        assertThrows(IllegalArgumentException.class, () -> CameraOptions.builder().width(-1).build());
        assertThrows(IllegalArgumentException.class,
                () -> CameraOptions.builder().pixelFormat(PixelFormat.UNRECOGNIZED).build());
        assertThrows(NullPointerException.class, () -> CameraOptions.builder().afMode(null).build());
        assertThrows(NullPointerException.class, () -> Camera.open(null));
        CameraOptions options = CameraOptions.builder().ev(-2.5).build();
        assertEquals(options, options.toBuilder().build());
        assertEquals(-2.5, options.ev());
    }

    // ------------------------------------------------------------------- frames ----

    @ParameterizedTest
    @EnumSource(value = PixelFormat.class, names = {"I420", "RGB888", "BGR888"})
    void everyFormatCarriesThePattern(PixelFormat format) {
        Frame frame = readStarted(small(96, 64).pixelFormat(format).build());
        assertEquals(format, frame.pixelFormat());
        assertEquals(96, frame.width());
        assertEquals(64, frame.height());
        CameraPatterns.check(frame, false, false);
    }

    @Test
    void flipsAndRotation() {
        CameraPatterns.check(readStarted(small(96, 64).hflip(true).build()), true, false);
        CameraPatterns.check(readStarted(small(96, 64).vflip(true).build()), false, true);
        CameraPatterns.check(readStarted(small(96, 64).hflip(true).vflip(true).build()), true, true);
        CameraPatterns.check(readStarted(small(96, 64).rotation(180).build()), true, true);
    }

    @Test
    void frameMetadataAndBytes() {
        try (Camera camera = Camera.open(small(320, 240).build())) {
            camera.start();
            Frame first = camera.readFrame();
            Frame second = camera.readFrame();
            assertTrue(second.sequence() > first.sequence());
            assertTrue(second.timestampNs() > first.timestampNs());
            assertEquals(camera.streamInfo().frameBytes(), first.info().bytes());
            assertEquals(first.info().bytes(), first.data().remaining());
            assertTrue(first.data().isReadOnly());
            assertEquals(first.info().bytes(), first.segment().byteSize());
            assertTrue(first.segment().isReadOnly());
            assertEquals("Frame[320x240 I420, sequence " + first.sequence() + ", 115200 bytes]", first.toString());
        }
    }

    @Test
    void framesIteratesUntilTheCameraIsClosed() {
        Camera camera = Camera.open(small(160, 120).build());
        camera.start();
        List<Long> sequences = new ArrayList<>();
        for (Frame frame : camera.frames()) {
            sequences.add(frame.sequence());
            if (sequences.size() == 5) {
                camera.close();
            }
        }
        assertEquals(5, sequences.size());
        assertEquals(sequences.stream().sorted().toList(), sequences);
        var ended = camera.frames().iterator();
        assertFalse(ended.hasNext());
        assertThrows(java.util.NoSuchElementException.class, ended::next);
    }

    @Test
    void aSlowReaderGetsTheNewestFrameAndTheDropsAreCounted() throws InterruptedException {
        try (Camera camera = Camera.open(small(320, 240).build())) {
            camera.start();
            Frame first = camera.readFrame();
            Thread.sleep(200);
            Frame newest = camera.readFrame();
            assertTrue(newest.sequence() - first.sequence() > 5);
            CameraStats stats = camera.stats();
            assertTrue(stats.framesDropped() > 0);
            assertEquals(2, stats.framesRead());
            assertTrue(stats.framesCaptured() >= stats.framesRead() + stats.framesDropped());
        }
    }

    @Test
    void aLongerQueueKeepsConsecutiveFrames() throws InterruptedException {
        int queue = CameraLimits.FRAME_QUEUE_MAX;
        try (Camera camera = Camera.open(small(160, 120).frameQueue(queue).build())) {
            camera.start();
            Thread.sleep(200);
            long first = camera.readFrame().sequence();
            for (int i = 1; i < queue; i++) {
                assertEquals(first + i, camera.readFrame().sequence());
            }
        }
    }

    @Test
    void readingBeforeStartIsAStateError() {
        try (Camera camera = Camera.open(small(160, 120).build())) {
            CameraStateException error = assertThrows(CameraStateException.class, camera::readFrame);
            assertEquals("keel_camera_read_frame", error.function());
            assertTrue(error.report().isPresent());
        }
    }

    @Test
    void aStalledCameraTimesOut() {
        CameraSourceExtension.fault("stall");
        try (Camera camera = Camera.open(small(160, 120).build())) {
            camera.start();
            for (int i = 0; i < 3; i++) {
                camera.readFrame();
            }
            CameraTimeoutException error = assertThrows(CameraTimeoutException.class,
                    () -> camera.readFrame(Duration.ofMillis(200)));
            assertEquals(Status.E_AGAIN, error.status());
        }
    }

    @Test
    void aCameraThatFailsWhileRunning() {
        CameraSourceExtension.fault("capture-fail");
        try (Camera camera = Camera.open(small(160, 120).build())) {
            camera.start();
            CameraException error = assertThrows(CameraException.class, () -> {
                for (int i = 0; i < 10; i++) {
                    camera.readFrame();
                }
            });
            assertSame(CameraException.class, error.getClass());
            assertEquals(Status.E_PERIPHERAL, error.status());
            assertTrue(error.getMessage().contains("test fault"), error.getMessage());
            assertEquals(CaptureState.FAILED, camera.stats().state());
            assertEquals(Status.E_PERIPHERAL, camera.lastError().status());
            camera.stop();
            assertEquals(CaptureState.STOPPED, camera.stats().state());
        }
    }

    @Test
    void timeouts() {
        try (Camera camera = Camera.open(small(160, 120).build())) {
            assertThrows(IllegalArgumentException.class, () -> camera.readFrame(Duration.ofMillis(-1)));
            assertThrows(IllegalArgumentException.class, () -> camera.readFrame(Duration.ofDays(100)));
            assertThrows(NullPointerException.class, () -> camera.readFrame(null));
            camera.start();
            assertThrows(CameraArgumentException.class, () -> camera.readFrame(Duration.ofSeconds(61)));
            assertEquals(160, camera.readFrame(Duration.ofMillis(500)).width());
        }
        assertEquals(2, Camera.milliseconds(Duration.ofNanos(1_000_001)));
        assertEquals(0, Camera.milliseconds(Duration.ZERO));
    }

    @Test
    void aFrameNeedsItsBytes() {
        Frame frame = readStarted(small(160, 120).build());
        IllegalArgumentException error = assertThrows(IllegalArgumentException.class,
                () -> new Frame(frame.info(), ByteBuffer.allocate(3)));
        assertEquals("the frame needs " + frame.info().bytes() + " bytes, but data has 3", error.getMessage());
        Frame copy = new Frame(frame.info(), ByteBuffer.wrap(frameBytes(frame)));
        assertArrayEquals(frame.toByteArray(), copy.toByteArray());
    }

    private static byte[] frameBytes(Frame frame) {
        byte[] bytes = new byte[frame.data().remaining()];
        frame.data().get(bytes);
        return bytes;
    }

    @Test
    void toByteArrayDropsTheRowPadding() {
        Frame i420 = readStarted(small(96, 64).build());
        assertEquals(128, i420.info().stride0());
        byte[] packed = i420.toByteArray();
        assertEquals(96 * 64 * 3 / 2, packed.length);
        int n = (int) (i420.sequence() & 0xFF);
        assertEquals((7 * 5 + 3 * 2 + n) & 0xFF, packed[2 * 96 + 5] & 0xFF);
        Frame rgb = readStarted(small(96, 64).pixelFormat(PixelFormat.RGB888).build());
        assertEquals(96 * 64 * 3, rgb.toByteArray().length);
    }

    // --------------------------------------------------------------- conversion ----

    @Test
    void i420ToRgbAndBgr() {
        for (int width : new int[] {320, 96}) {
            Frame frame = readStarted(small(width, 64).build());
            byte[] expected = CameraPatterns.referenceRgb(frame);
            Frame rgb = frame.convert(PixelFormat.RGB888);
            Frame bgr = frame.convert(PixelFormat.BGR888);
            assertEquals(PixelFormat.RGB888, rgb.pixelFormat());
            assertEquals(frame.sequence(), rgb.sequence());
            assertEquals(frame.timestampNs(), rgb.timestampNs());
            assertEquals(width * 3, rgb.info().stride0());
            assertArrayEquals(expected, rgb.toByteArray());
            byte[] swapped = bgr.toByteArray();
            for (int at = 0; at < swapped.length; at += 3) {
                assertEquals(expected[at], swapped[at + 2]);
                assertEquals(expected[at + 2], swapped[at]);
            }
            Frame heap = new Frame(frame.info(), ByteBuffer.wrap(frameBytes(frame)));
            assertArrayEquals(expected, heap.convert(PixelFormat.RGB888).toByteArray());
        }
    }

    @Test
    void convertNeedsI420AndAPackedTarget() {
        Frame rgb = readStarted(small(96, 64).pixelFormat(PixelFormat.RGB888).build());
        assertThrows(IllegalArgumentException.class, () -> rgb.convert(PixelFormat.BGR888));
        Frame i420 = readStarted(small(96, 64).build());
        assertThrows(IllegalArgumentException.class, () -> i420.convert(PixelFormat.I420));
        assertThrows(NullPointerException.class, () -> i420.convert(null));
        FrameInfo info = i420.info();
        FrameInfo broken = new FrameInfo(info.formatValue(), info.width(), info.height(), info.planeCount(),
                info.stride0(), info.stride1(), info.stride2(), info.offset0(), info.offset1(), 1 << 20, info.bytes(),
                info.sequence(), info.timestampNs());
        CameraArgumentException error = assertThrows(CameraArgumentException.class,
                () -> new Frame(broken, i420.data()).convert(PixelFormat.RGB888));
        assertEquals("keel_camera_convert_i420", error.function());
        assertTrue(error.report().isEmpty());
    }

    @Test
    void framesAsBufferedImages() {
        for (PixelFormat format : List.of(PixelFormat.BGR888, PixelFormat.RGB888, PixelFormat.I420)) {
            Frame frame = readStarted(small(96, 64).pixelFormat(format).build());
            BufferedImage image = frame.toBufferedImage();
            assertEquals(BufferedImage.TYPE_3BYTE_BGR, image.getType());
            assertEquals(96, image.getWidth());
            assertEquals(64, image.getHeight());
            int rgb = image.getRGB(7, 5) & 0xFFFFFF;
            if (format == PixelFormat.I420) {
                byte[] expected = CameraPatterns.referenceRgb(frame);
                int at = (5 * 96 + 7) * 3;
                assertEquals((expected[at] & 0xFF) << 16 | (expected[at + 1] & 0xFF) << 8 | expected[at + 2] & 0xFF,
                        rgb);
            } else {
                int n = (int) (frame.sequence() & 0xFF);
                assertEquals(((7 + n) & 0xFF) << 16 | ((5 + n) & 0xFF) << 8 | 12, rgb);
            }
        }
    }

    @Test
    void anUnknownPixelFormatHasNoPlanes() {
        FrameInfo unknown = new FrameInfo(9, 2, 2, 1, 6, 0, 0, 0, 0, 0, 12, 0, 0);
        Frame frame = new Frame(unknown, ByteBuffer.allocateDirect(12));
        assertSame(PixelFormat.UNRECOGNIZED, frame.pixelFormat());
        assertThrows(IllegalStateException.class, frame::toByteArray);
        assertInstanceOf(MemorySegment.class, frame.segment());
    }

    // -------------------------------------------------------------------- focus ----

    @Test
    void cameraModule2HasAFixedFocusLens() {
        for (CameraOptions options : List.of(CameraOptions.builder().afMode(AutofocusMode.CONTINUOUS).build(),
                CameraOptions.builder().afRange(AutofocusRange.MACRO).build())) {
            CameraUnsupportedException error = assertThrows(CameraUnsupportedException.class,
                    () -> Camera.open(options));
            assertTrue(error.getMessage().contains("fixed-focus"), error.getMessage());
            assertTrue(error.getMessage().contains("Camera Module 2"), error.getMessage());
        }
        try (Camera camera = Camera.open(small(160, 120).build())) {
            camera.start();
            assertThrows(CameraUnsupportedException.class, () -> camera.triggerAutofocus(Duration.ofMillis(500)));
        }
    }

    @Test
    void cameraModule3Focuses() {
        CameraOptions options = CameraOptions.builder().index(1).width(320).height(240).afMode(AutofocusMode.AUTO)
                .afRange(AutofocusRange.FULL).build();
        try (Camera camera = Camera.open(options)) {
            assertThrows(CameraStateException.class, () -> camera.triggerAutofocus(Duration.ofMillis(500)));
            camera.start();
            assertThrows(CameraArgumentException.class, () -> camera.triggerAutofocus(Duration.ZERO));
            assertEquals(AutofocusState.FOCUSED, camera.triggerAutofocus());
        }
    }

    @Test
    void manualFocusHoldsTheLensAndHasNoScan() {
        CameraOptions options = CameraOptions.builder().index(1).width(320).height(240)
                .afMode(AutofocusMode.MANUAL).lensPosition(2.0).build();
        try (Camera camera = Camera.open(options)) {
            camera.start();
            camera.readFrame();
            assertThrows(CameraUnsupportedException.class, () -> camera.triggerAutofocus(Duration.ofMillis(500)));
        }
        CameraArgumentException error = assertThrows(CameraArgumentException.class, () -> Camera.open(
                CameraOptions.builder().index(1).afMode(AutofocusMode.MANUAL).lensPosition(32.001).build()));
        assertTrue(error.getMessage().contains("lens_position"), error.getMessage());
    }

    @Test
    void exposureAndWhiteBalanceOptionsReachTheCamera() {
        CameraOptions options = small(160, 120).exposureUs(10_000).gain(2.0).ev(-1.0)
                .awbMode(WhiteBalance.MANUAL).redGain(1.5).blueGain(1.25).build();
        try (Camera camera = Camera.open(options)) {
            camera.start();
            assertEquals(160, camera.readFrame().width());
        }
    }
}
