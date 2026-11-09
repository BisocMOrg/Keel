// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.CameraStats;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.frames.Frame;
import io.github.bisocmorg.keel.errors.LibraryException;
import java.nio.ByteBuffer;
import java.time.Duration;

/**
 * CameraFrames: camera frames fed to vision code, with the brightness and the motion of every frame.
 *
 * <p>Each frame arrives in I420, so its first plane is a greyscale image with no conversion: the example reads it
 * straight from the frame's direct buffer ({@code Frame.data()}), with no copy. {@code Frame.toBufferedImage()}
 * gives a colour image for {@code java.awt} and {@code javax.imageio}, or open the camera with
 * {@code PixelFormat.BGR888} for frames in the byte order OpenCV's Java binding expects. A slow model never slows the
 * camera: the queue keeps the newest frame and counts the ones it dropped. The camera needs no Keel board.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/CameraFrames.java --frames 60
 * </pre>
 *
 * <p>Options: {@code --camera N}, {@code --frames N} (default 60), {@code --width W}, {@code --height H}
 * (default 640x480), {@code --framerate FPS} (default 30). Exit status: 0 success, 1 failure, 2 usage.
 */
public final class CameraFrames {
    private CameraFrames() {
    }

    /**
     * Runs the example.
     *
     * @param args {@code [--camera N] [--frames N] [--width W] [--height H] [--framerate FPS]}
     */
    public static void main(String[] args) {
        System.exit(run(args));
    }

    private static int run(String[] args) {
        CameraOptions.Builder options = CameraOptions.builder();
        int frames = 60;
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "--camera" -> options.index(Integer.parseInt(args[++i]));
                    case "--frames" -> frames = Integer.parseInt(args[++i]);
                    case "--width" -> options.width(Integer.parseInt(args[++i]));
                    case "--height" -> options.height(Integer.parseInt(args[++i]));
                    case "--framerate" -> options.framerate(Double.parseDouble(args[++i]));
                    default -> throw new IllegalArgumentException(args[i]);
                }
            }
            if (frames < 1 || frames > 100_000) {
                throw new IllegalArgumentException("--frames");
            }
        } catch (ArrayIndexOutOfBoundsException | IllegalArgumentException error) {
            System.err.println("Usage: CameraFrames [--camera N] [--frames N] [--width W] [--height H] "
                    + "[--framerate FPS]   (N: 1..100000)");
            return 2;
        }
        try (Camera camera = Camera.open(options.build())) {
            System.out.println("frames: " + camera.info().model() + " (" + camera.info().sensor() + ")");
            camera.start();
            int count = 0;
            byte[] previous = null;
            for (Frame frame : camera.frames(Duration.ofSeconds(3))) {
                byte[] grey = luma(frame);
                double brightness = 0;
                double motion = 0;
                for (int i = 0; i < grey.length; i++) {
                    brightness += grey[i] & 0xFF;
                    if (previous != null) {
                        motion += Math.abs((grey[i] & 0xFF) - (previous[i] & 0xFF));
                    }
                }
                previous = grey;
                count++;
                if (count % 10 == 0 || count == frames) {
                    System.out.printf("frame %d: brightness %.1f, motion %.1f%n", frame.sequence(),
                            brightness / grey.length, motion / grey.length);
                }
                if (count == frames) {
                    break;
                }
            }
            CameraStats stats = camera.stats();
            System.out.println("frames: done, " + stats.framesRead() + " read, " + stats.framesDropped() + " dropped");
            return 0;
        } catch (CameraException | LibraryException error) {
            System.err.println("frames: " + error.getMessage());
            return 1;
        }
    }

    /** The Y plane of an I420 frame, without row padding: one brightness byte per pixel. */
    private static byte[] luma(Frame frame) {
        ByteBuffer data = frame.data();
        int width = frame.width();
        int stride = (int) frame.info().stride0();
        byte[] grey = new byte[width * frame.height()];
        for (int row = 0; row < frame.height(); row++) {
            data.get((int) frame.info().offset0() + row * stride, grey, row * width, width);
        }
        return grey;
    }
}
