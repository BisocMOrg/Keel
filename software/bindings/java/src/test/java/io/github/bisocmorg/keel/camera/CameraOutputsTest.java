// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeFalse;

import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.errors.CameraArgumentException;
import io.github.bisocmorg.keel.camera.errors.CameraBusyException;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.errors.CameraNotFoundException;
import io.github.bisocmorg.keel.camera.errors.CameraResource;
import io.github.bisocmorg.keel.camera.errors.CameraStateException;
import io.github.bisocmorg.keel.camera.errors.CameraTimeoutException;
import io.github.bisocmorg.keel.camera.errors.CameraUnsupportedException;
import io.github.bisocmorg.keel.camera.outputs.OutputOptions;
import io.github.bisocmorg.keel.camera.outputs.OutputState;
import io.github.bisocmorg.keel.camera.outputs.OutputStats;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import io.github.bisocmorg.keel.support.LocalResources;
import java.io.IOException;
import java.io.InputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;
import org.junit.jupiter.api.io.TempDir;

/** Outputs the library writes itself: files, a TCP connection, a listening port, UDP, and the refusals. */
@ExtendWith(CameraSourceExtension.class)
class CameraOutputsTest {
    @TempDir
    Path directory;

    private static Camera h264() {
        Camera camera = Camera.open(CameraOptions.builder().width(320).height(240).framerate(120).build());
        camera.setEncoder(Codec.H264);
        return camera;
    }

    private static boolean waitFor(BooleanSupplier condition) throws InterruptedException {
        long deadline = System.nanoTime() + 3_000_000_000L;
        while (System.nanoTime() < deadline) {
            if (condition.getAsBoolean()) {
                return true;
            }
            Thread.sleep(20);
        }
        return false;
    }

    private static byte[] receive(InputStream in, int size) throws IOException {
        byte[] data = new byte[size];
        int at = 0;
        while (at < size) {
            int read = in.read(data, at, size - at);
            if (read < 0) {
                break;
            }
            at += read;
        }
        return java.util.Arrays.copyOf(data, at);
    }

    @Test
    void anH264File() throws IOException, InterruptedException {
        Path file = directory.resolve("drive.h264");
        try (Camera camera = h264()) {
            int output = camera.startOutput(file);
            assertEquals(OutputState.WAITING, camera.outputStats(output).state());
            camera.start();
            assertTrue(waitFor(() -> camera.outputStats(output).chunks() >= 5));
            camera.stopOutput(output);
            byte[] data = Files.readAllBytes(file);
            assertEquals(1, data[3]);
            assertEquals(7, data[4] & 0x1F);
            assertEquals(0, camera.stats().outputs());
            assertThrows(CameraNotFoundException.class, () -> camera.outputStats(output));
            assertThrows(CameraNotFoundException.class, () -> camera.stopOutput(output));
        }
    }

    @Test
    void anMjpegFileIsFinishedByClose() throws IOException, InterruptedException {
        Path file = directory.resolve("drive.mjpeg");
        try (Camera camera = Camera.open(CameraOptions.builder().width(320).height(240).framerate(120).build())) {
            camera.setEncoder(Codec.MJPEG);
            camera.startOutput(file.toString());
            camera.start();
            Thread.sleep(200);
        }
        byte[] data = Files.readAllBytes(file);
        assertEquals((byte) 0xD8, data[1]);
        assertEquals((byte) 0xD9, data[data.length - 1]);
    }

    @Test
    void aTcpConnectionToAReceiver() throws IOException {
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress()); Camera camera = h264()) {
            int output = camera.startOutput("tcp://127.0.0.1:" + server.getLocalPort());
            try (Socket connection = server.accept()) {
                camera.start();
                byte[] data = receive(connection.getInputStream(), 2000);
                assertEquals(1, data[3]);
                OutputStats stats = camera.outputStats(output);
                assertEquals(1, stats.clients());
                assertTrue(stats.bytes() > 0);
                assertEquals(Status.OK, stats.status());
            }
        }
    }

    @Test
    void aListeningPortServesReceiversFromAKeyframe() throws IOException, InterruptedException {
        int port = LocalResources.freePort();
        try (Camera camera = h264()) {
            int output = camera.startOutput("tcp://127.0.0.1:" + port + "?listen",
                    OutputOptions.defaults().withMaxClients(2).withQueue(32));
            assertEquals(0, camera.outputStats(output).clients());
            camera.start();
            Thread.sleep(200);
            try (Socket first = new Socket("127.0.0.1", port); Socket second = new Socket("127.0.0.1", port)) {
                for (Socket receiver : List.of(first, second)) {
                    assertEquals(7, receive(receiver.getInputStream(), 2000)[4] & 0x1F);
                }
                assertTrue(waitFor(() -> camera.outputStats(output).clients() == 2));
                assertEquals(OutputState.RUNNING, camera.outputStats(output).state());
            }
        }
    }

    @Test
    void udpDatagrams() throws IOException, InterruptedException {
        try (DatagramSocket receiver = new DatagramSocket(0, InetAddress.getLoopbackAddress());
                Camera camera = h264()) {
            receiver.setSoTimeout(3000);
            int output = camera.startOutput("udp://127.0.0.1:" + receiver.getLocalPort(),
                    OutputOptions.defaults().withDatagramBytes(1000));
            camera.start();
            DatagramPacket packet = new DatagramPacket(new byte[65536], 65536);
            receiver.receive(packet);
            assertTrue(packet.getLength() > 0 && packet.getLength() <= 1000);
            assertEquals(1, packet.getData()[3]);
            assertTrue(waitFor(() -> camera.outputStats(output).chunks() > 0));
        }
    }

    @Test
    void aReceiverThatGoesAwayFailsOnlyItsOutput() throws IOException, InterruptedException {
        try (Camera camera = h264()) {
            int output;
            try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
                output = camera.startOutput("tcp://127.0.0.1:" + server.getLocalPort());
                try (Socket connection = server.accept()) {
                    camera.start();
                    receive(connection.getInputStream(), 500);
                }
            }
            assertTrue(waitFor(() -> camera.outputStats(output).state() == OutputState.FAILED));
            OutputStats stats = camera.outputStats(output);
            assertEquals(Status.E_PERIPHERAL, stats.status());
            assertTrue(stats.message().contains("disconnected"), stats.message());
            assertEquals(CaptureState.RUNNING, camera.stats().state());
            camera.readChunk();
            CameraException error = assertThrows(CameraException.class, () -> camera.stopOutput(output));
            assertEquals(Status.E_PERIPHERAL, error.status());
        }
    }

    @Test
    void aReceiverThatRefuses() {
        try (Camera camera = h264()) {
            CameraNotFoundException error = assertThrows(CameraNotFoundException.class,
                    () -> camera.startOutput("tcp://127.0.0.1:" + LocalResources.freePort()));
            assertTrue(error.getMessage().contains("refused"), error.getMessage());
            assertEquals(CameraResource.NETWORK, error.report().orElseThrow().resource());
        }
    }

    @Test
    void aListeningPortInUse() throws IOException {
        try (ServerSocket taken = new ServerSocket(0, 1, InetAddress.getLoopbackAddress()); Camera camera = h264()) {
            CameraBusyException error = assertThrows(CameraBusyException.class,
                    () -> camera.startOutput("tcp://127.0.0.1:" + taken.getLocalPort() + "?listen"));
            assertTrue(error.getMessage().contains("already in use"), error.getMessage());
        }
    }

    @Test
    void destinationsThatAreRefused() {
        try (Camera camera = h264()) {
            for (String[] each : new String[][] {{"", "empty"}, {"http://host/x", "unknown scheme"},
                {"/tmp/x.avi", "unknown file type"}, {"/tmp/x.mjpeg", "MJPEG"}, {"tcp://host:70000", "port"},
                {"udp://host:5000?listen", "cannot listen"}}) {
                CameraArgumentException error = assertThrows(CameraArgumentException.class,
                        () -> camera.startOutput(each[0]));
                assertTrue(error.getMessage().contains(each[1]), error.getMessage());
            }
            CameraNotFoundException missing = assertThrows(CameraNotFoundException.class,
                    () -> camera.startOutput("/nonexistent-directory/x.h264"));
            assertTrue(missing.getMessage().contains("cannot create"), missing.getMessage());
            for (OutputOptions options : List.of(OutputOptions.defaults().withMaxClients(2),
                    OutputOptions.defaults().withDatagramBytes(1000), OutputOptions.defaults().withQueue(257))) {
                assertThrows(CameraArgumentException.class,
                        () -> camera.startOutput(directory.resolve("x.h264").toString(), options));
            }
            assertThrows(IllegalArgumentException.class, () -> camera.startOutput("/tmp/" + "x".repeat(300)));
            assertThrows(IllegalArgumentException.class, () -> camera.startOutput("/tmp/a\0b.h264"));
            assertThrows(NullPointerException.class, () -> camera.startOutput((String) null));
        }
        assertThrows(IllegalArgumentException.class, () -> new OutputOptions(-1, 0, 0));
    }

    @Test
    void containerAndProtocolOutputsNeedLibavformat() {
        assumeFalse(Camera.versionString().contains("libav"), "this build has libavformat");
        try (Camera camera = h264()) {
            CameraUnsupportedException error = assertThrows(CameraUnsupportedException.class,
                    () -> camera.startOutput(directory.resolve("x.mp4")));
            assertTrue(error.getMessage().contains("libavformat"), error.getMessage());
            assertThrows(CameraUnsupportedException.class, () -> camera.startOutput("rtsp://127.0.0.1:8554/robot"));
        }
    }

    @Test
    void anOutputNeedsAnEncoder() {
        try (Camera camera = Camera.open(CameraOptions.builder().width(320).height(240).build())) {
            CameraStateException error = assertThrows(CameraStateException.class,
                    () -> camera.startOutput(directory.resolve("x.h264")));
            assertTrue(error.getMessage().contains("keel_camera_set_encoder"), error.getMessage());
        }
    }

    @Test
    void atMostEightOutputs() {
        try (Camera camera = h264()) {
            List<Integer> outputs = new ArrayList<>();
            for (int i = 0; i < CameraLimits.OUTPUTS_MAX; i++) {
                outputs.add(camera.startOutput(directory.resolve(i + ".h264")));
            }
            assertEquals(CameraLimits.OUTPUTS_MAX, camera.stats().outputs());
            CameraTimeoutException error = assertThrows(CameraTimeoutException.class,
                    () -> camera.startOutput(directory.resolve("one-more.h264")));
            assertEquals(Status.E_AGAIN, error.status());
            assertThrows(CameraStateException.class, () -> camera.setEncoder(Codec.MJPEG));
            for (int output : outputs) {
                camera.stopOutput(output);
            }
        }
    }
}
