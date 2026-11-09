// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.outputs.OutputState;
import io.github.bisocmorg.keel.camera.outputs.OutputStats;
import io.github.bisocmorg.keel.errors.LibraryException;

/**
 * CameraStream: the camera streamed to the network or a file, with the receivers reported once a second.
 *
 * <p>The destination is anything {@code Camera.startOutput} takes; the default, {@code tcp://:5000?listen}, lets up
 * to four viewers connect and leave when they like:
 *
 * <pre>
 * ffplay -f h264 -fflags nobuffer -flags low_delay -framedrop tcp://ROBOT:5000
 * vlc tcp/h264://ROBOT:5000
 * </pre>
 *
 * <p>{@code udp://COMPUTER:5000} sends to one computer ({@code ffplay -f h264 udp://0.0.0.0:5000}),
 * {@code tcp://COMPUTER:5000} connects to a viewer that listens, and a path ending in {@code .h264} or {@code .mjpeg}
 * records. The library sends from its own threads; the program only reports. It runs for {@code --seconds S}, or
 * until Ctrl-C, when the shutdown hook closes the camera and finishes any file. The camera needs no Keel board.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/CameraStream.java 'tcp://:5000?listen'
 * </pre>
 *
 * <p>Options: {@code --camera N}, {@code --seconds S} (default: until Ctrl-C), {@code --mjpeg}, {@code --width W},
 * {@code --height H}. Exit status: 0 success, 1 failure, 2 usage, 130 after Ctrl-C.
 */
public final class CameraStream {
    private CameraStream() {
    }

    /**
     * Runs the example.
     *
     * @param args {@code [DESTINATION] [--camera N] [--seconds S] [--mjpeg] [--width W] [--height H]}
     * @throws InterruptedException if the main thread is interrupted
     */
    public static void main(String[] args) throws InterruptedException {
        System.exit(run(args));
    }

    private static int run(String[] args) throws InterruptedException {
        CameraOptions.Builder options = CameraOptions.builder();
        String destination = "tcp://:5000?listen";
        double seconds = 0;
        Codec codec = Codec.H264;
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "--camera" -> options.index(Integer.parseInt(args[++i]));
                    case "--seconds" -> seconds = Double.parseDouble(args[++i]);
                    case "--mjpeg" -> codec = Codec.MJPEG;
                    case "--width" -> options.width(Integer.parseInt(args[++i]));
                    case "--height" -> options.height(Integer.parseInt(args[++i]));
                    default -> {
                        if (args[i].startsWith("--")) {
                            throw new IllegalArgumentException(args[i]);
                        }
                        destination = args[i];
                    }
                }
            }
            if (seconds < 0) {
                throw new IllegalArgumentException("--seconds");
            }
        } catch (ArrayIndexOutOfBoundsException | IllegalArgumentException error) {
            System.err.println("Usage: CameraStream [DESTINATION] [--camera N] [--seconds S] [--mjpeg] [--width W] "
                    + "[--height H]");
            return 2;
        }
        CameraOptions chosen = options.build();
        try (Camera camera = Camera.open(chosen)) {
            camera.setEncoder(codec);
            int output = camera.startOutput(destination);
            camera.start();
            System.out.println("stream: " + codec + " " + chosen.width() + "x" + chosen.height() + " to "
                    + destination);
            long deadline = seconds > 0 ? System.nanoTime() + (long) (seconds * 1e9) : Long.MAX_VALUE;
            while (System.nanoTime() < deadline) {
                Thread.sleep(Math.max(0, Math.min(1000, (deadline - System.nanoTime()) / 1_000_000)));
                OutputStats stats = camera.outputStats(output);
                System.out.println("stream: " + stats.state() + ", " + stats.clients() + " receiver(s), "
                        + stats.chunks() + " chunks sent, " + stats.chunksDropped() + " dropped");
                if (stats.state() == OutputState.FAILED) {
                    System.err.println("stream: " + stats.message());
                    return 1;
                }
            }
            camera.stopOutput(output);
            System.out.println("stream: done");
            return 0;
        } catch (CameraException | LibraryException error) {
            System.err.println("stream: " + error.getMessage());
            return 1;
        }
    }
}
