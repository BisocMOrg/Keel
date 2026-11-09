// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.errors.CameraBusyException;
import io.github.bisocmorg.keel.camera.errors.CameraClosedException;
import io.github.bisocmorg.keel.camera.errors.CameraThreadException;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import io.github.bisocmorg.keel.support.LocalResources;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.junit.jupiter.api.function.Executable;
import org.junit.jupiter.api.io.TempDir;

/** Lifetime and threads: close from any thread, the owning thread, the cleaner, the shutdown hook, independence. */
@ExtendWith(CameraSourceExtension.class)
class CameraLifecycleTest {
    private static final String JAVA = Path.of(System.getProperty("java.home"), "bin", "java").toString();

    @TempDir
    Path directory;

    private static CameraOptions small() {
        return CameraOptions.builder().width(160).height(120).framerate(120).build();
    }

    private record Result(int status, String output) {
    }

    private static Result probe(String... args) throws IOException, InterruptedException {
        List<String> command = new ArrayList<>(List.of(JAVA, "--enable-native-access=ALL-UNNAMED",
                "--illegal-native-access=deny", "-cp", System.getProperty("java.class.path"),
                "io.github.bisocmorg.keel.support.CameraProbe"));
        command.addAll(List.of(args));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        builder.environment().put("KEEL_CAMERA_TEST_SOURCE", "1");
        Process process = builder.start();
        String output = new String(process.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (!process.waitFor(90, TimeUnit.SECONDS)) {
            process.destroyForcibly();
            fail("the probe did not finish:\n" + output);
        }
        return new Result(process.exitValue(), output);
    }

    /** Collects the binding's camera log records during a block. */
    private static final class Records extends Handler implements AutoCloseable {
        private final Logger logger = Logger.getLogger("io.github.bisocmorg.keel.camera");
        private final List<String> messages = new java.util.concurrent.CopyOnWriteArrayList<>();

        Records() {
            logger.addHandler(this);
        }

        @Override
        public void publish(LogRecord record) {
            messages.add(java.text.MessageFormat.format(record.getMessage(),
                    record.getParameters() == null ? new Object[0] : record.getParameters()));
        }

        @Override
        public void flush() {
        }

        @Override
        public void close() {
            logger.removeHandler(this);
        }

        boolean contains(String text) {
            return messages.stream().anyMatch(message -> message.contains(text));
        }
    }

    @Test
    void tryWithResourcesClosesTheCamera() {
        Camera kept;
        try (Camera camera = Camera.open(small())) {
            assertFalse(camera.isClosed());
            kept = camera;
        }
        assertTrue(kept.isClosed());
        assertEquals("Camera[0: Camera Module 2 (imx219), closed]", kept.toString());
        Camera.open(small()).close();
    }

    @Test
    void closeIsIdempotentAndLaterCallsThrowClosed() {
        Camera camera = Camera.open(small());
        camera.close();
        camera.close();
        List<Executable> calls = List.of(camera::start, camera::stop, camera::readFrame, camera::stats,
                camera::streamInfo, camera::lastError, camera::readChunk, () -> camera.setEncoder(Codec.H264),
                camera::captureStill, () -> camera.outputStats(1));
        for (Executable call : calls) {
            CameraClosedException error = assertThrows(CameraClosedException.class, call);
            assertEquals(Status.E_INVALID_HANDLE, error.status());
            assertTrue(error.report().isEmpty());
            assertTrue(error.getMessage().endsWith("KEEL_E_INVALID_HANDLE: the camera is closed"), error.getMessage());
        }
        assertFalse(camera.frames().iterator().hasNext());
        assertFalse(camera.chunks().iterator().hasNext());
    }

    @Test
    void anotherThreadMayOnlyClose() throws Exception {
        try (Camera camera = Camera.open(small())) {
            camera.start();
            AtomicReference<Throwable> thrown = new AtomicReference<>();
            Thread worker = Thread.ofPlatform().start(() -> {
                try {
                    camera.readFrame();
                } catch (RuntimeException error) {
                    thrown.set(error);
                }
            });
            worker.join();
            CameraThreadException error = (CameraThreadException) thrown.get();
            assertEquals(Status.E_CONFLICT, error.status());
            assertTrue(error.report().isEmpty());
            assertTrue(error.getMessage().contains("only the thread that opened the camera"), error.getMessage());
            assertEquals(160, camera.readFrame().width());
        }
    }

    @Test
    void closeFromAnotherThreadWakesABlockedRead() throws Exception {
        CameraSourceExtension.fault("stall");
        Camera camera = Camera.open(small());
        camera.start();
        for (int i = 0; i < 3; i++) {
            camera.readFrame();
        }
        CompletableFuture<Void> closer = CompletableFuture.runAsync(() -> {
            sleep(300);
            camera.close();
        });
        long started = System.nanoTime();
        assertThrows(CameraClosedException.class, () -> camera.readFrame(Duration.ofSeconds(10)));
        assertTrue(System.nanoTime() - started < 5_000_000_000L);
        closer.get(10, TimeUnit.SECONDS);
        assertTrue(camera.isClosed());
    }

    @Test
    void closeFromAnotherThreadEndsTheFrameLoop() throws Exception {
        CameraSourceExtension.fault("stall");
        Camera camera = Camera.open(small());
        camera.start();
        CompletableFuture<Void> closer = CompletableFuture.runAsync(() -> {
            sleep(300);
            camera.close();
        });
        int count = 0;
        for (Frame frame : camera.frames(Duration.ofSeconds(10))) {
            assertEquals(160, frame.width());
            count++;
        }
        assertEquals(3, count);
        closer.get(10, TimeUnit.SECONDS);
    }

    private static void sleep(long milliseconds) {
        try {
            Thread.sleep(milliseconds);
        } catch (InterruptedException interruption) {
            Thread.currentThread().interrupt();
        }
    }

    @Test
    void concurrentClosesAllReturnAfterTheClose() throws Exception {
        Camera camera = Camera.open(small());
        camera.start();
        CountDownLatch start = new CountDownLatch(1);
        List<Thread> closers = new ArrayList<>();
        for (int i = 0; i < 4; i++) {
            closers.add(Thread.ofPlatform().start(() -> {
                try {
                    start.await();
                } catch (InterruptedException interruption) {
                    Thread.currentThread().interrupt();
                }
                camera.close();
            }));
        }
        start.countDown();
        for (Thread closer : closers) {
            closer.join();
        }
        Camera.open(small()).close();
    }

    @Test
    void anInterruptDuringCloseIsKept() {
        Camera camera = Camera.open(small());
        Thread.currentThread().interrupt();
        camera.close();
        assertTrue(Thread.interrupted());
    }

    @Test
    void anUnclosedCameraIsClosedByTheCleaner() throws InterruptedException {
        try (Records records = new Records()) {
            WeakReference<Camera> reference = new WeakReference<>(Camera.open(small()));
            Camera reopened = null;
            for (int i = 0; i < 100 && reopened == null; i++) {
                System.gc();
                Thread.sleep(20);
                try {
                    reopened = Camera.open(small());
                } catch (CameraBusyException stillOpen) {
                    // the cleaner has not run yet
                }
            }
            assertTrue(reference.refersTo(null));
            assertTrue(reopened != null, "the cleaner did not release the camera");
            reopened.close();
            assertTrue(records.contains("Camera was not closed; the cleaner closed it"));
        }
    }

    @Test
    void theShutdownHookClosesACameraLeftOpen() throws IOException, InterruptedException {
        Path file = directory.resolve("exit.h264");
        Result result = probe("exit", file.toString());
        assertEquals(7, result.status(), result.output());
        assertTrue(result.output().contains("closed at exit: true"), result.output());
        byte[] data = Files.readAllBytes(file);
        assertTrue(data.length > 4 && data[3] == 1, "the file was not written");
    }

    @Test
    void aCameraProgramNeverLoadsLibkeel() throws IOException, InterruptedException {
        Result result = probe("only-camera");
        assertEquals(0, result.status(), result.output());
        assertTrue(result.output().contains("cameras 2, libkeel-camera mapped true, libkeel loaded false"),
                result.output());
    }

    @Test
    void aMissingLibraryNamesThePackage() throws IOException, InterruptedException {
        Result result = probe("missing", "/nonexistent/libkeel-camera.so.1");
        assertEquals(0, result.status(), result.output());
        assertTrue(result.output().startsWith("CameraLibraryException: cannot load /nonexistent/libkeel-camera.so.1"),
                result.output());
        assertTrue(result.output().contains("install the libkeel-camera1 package"), result.output());
    }

    @Test
    void aVirtualThreadCannotOpenACamera() throws Exception {
        AtomicReference<Throwable> thrown = new AtomicReference<>();
        Thread.ofVirtual().start(() -> {
            try {
                Camera.open(small()).close();
            } catch (RuntimeException error) {
                thrown.set(error);
            }
        }).join();
        assertTrue(thrown.get() instanceof IllegalStateException, String.valueOf(thrown.get()));
        assertTrue(thrown.get().getMessage().contains("platform thread"), thrown.get().getMessage());
    }

    @Test
    void reopeningLeavesNoThreadOrDescriptorBehind() throws InterruptedException {
        try (Camera warm = Camera.open(small())) {
            warm.setEncoder(Codec.H264);
            warm.start();
            warm.readChunk();
        }
        long descriptors = LocalResources.openFileDescriptors();
        long threads = LocalResources.threads();
        for (int i = 0; i < 20; i++) {
            try (Camera camera = Camera.open(small())) {
                camera.setEncoder(Codec.H264);
                camera.startOutput(directory.resolve(i + ".h264"));
                camera.start();
                camera.readFrame();
            }
        }
        assertEquals(descriptors, LocalResources.openFileDescriptors());
        assertEquals(threads, LocalResources.threads());
    }

    @Test
    void aFailedCloseIsLoggedNotThrown() throws IOException, InterruptedException {
        try (Records records = new Records()) {
            Camera camera = Camera.open(small());
            camera.setEncoder(Codec.H264);
            int output;
            try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
                output = camera.startOutput("tcp://127.0.0.1:" + server.getLocalPort());
                try (Socket connection = server.accept()) {
                    camera.start();
                    connection.getInputStream().readNBytes(100);
                }
            }
            for (int i = 0; i < 150 && camera.outputStats(output).state() != outputFailed(); i++) {
                Thread.sleep(20);
            }
            camera.close();
            assertTrue(records.contains("keel_camera_close returned KEEL_E_PERIPHERAL"));
        }
    }

    private static io.github.bisocmorg.keel.camera.outputs.OutputState outputFailed() {
        return io.github.bisocmorg.keel.camera.outputs.OutputState.FAILED;
    }
}
