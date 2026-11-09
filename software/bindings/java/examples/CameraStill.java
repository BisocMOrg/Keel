// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.errors.CameraException;
import io.github.bisocmorg.keel.camera.stills.StillInfo;
import io.github.bisocmorg.keel.camera.stills.StillOptions;
import io.github.bisocmorg.keel.errors.LibraryException;
import java.nio.file.Path;

/**
 * CameraStill: one JPEG photo, by default at the sensor's full resolution.
 *
 * <p>Exposure, white balance and, on Camera Module 3, focus settle first, so a photo takes about a second. Up to
 * 1920x1920 the Raspberry Pi's JPEG encoder does the work; larger photos (the full 3280x2464 of Camera Module 2) are
 * encoded with libjpeg. The camera needs no Keel board.
 *
 * <pre>
 * java --enable-native-access=ALL-UNNAMED -cp keel-1.0.0.jar examples/CameraStill.java -o photo.jpg
 * </pre>
 *
 * <p>Options: {@code -o FILE} (default photo.jpg), {@code --camera N}, {@code --width W --height H} (default: full
 * resolution), {@code --quality Q} (1..100, default 85). Exit status: 0 success, 1 failure, 2 usage.
 */
public final class CameraStill {
    private CameraStill() {
    }

    /**
     * Runs the example.
     *
     * @param args {@code [-o FILE] [--camera N] [--width W --height H] [--quality Q]}
     */
    public static void main(String[] args) {
        System.exit(run(args));
    }

    private static int run(String[] args) {
        Path output = Path.of("photo.jpg");
        int index = 0;
        int width = 0;
        int height = 0;
        int quality = 0;
        try {
            for (int i = 0; i < args.length; i++) {
                switch (args[i]) {
                    case "-o", "--output" -> output = Path.of(args[++i]);
                    case "--camera" -> index = Integer.parseInt(args[++i]);
                    case "--width" -> width = Integer.parseInt(args[++i]);
                    case "--height" -> height = Integer.parseInt(args[++i]);
                    case "--quality" -> quality = Integer.parseInt(args[++i]);
                    default -> throw new IllegalArgumentException(args[i]);
                }
            }
            if ((width == 0) != (height == 0)) {
                throw new IllegalArgumentException("give both --width and --height, or neither");
            }
        } catch (ArrayIndexOutOfBoundsException | IllegalArgumentException error) {
            System.err.println("Usage: CameraStill [-o FILE] [--camera N] [--width W --height H] [--quality Q]");
            return 2;
        }
        try (Camera camera = Camera.open(CameraOptions.builder().index(index).build())) {
            StillInfo info = camera.captureStillFile(output, StillOptions.of(width, height).withQuality(quality));
            System.out.println("still: " + info.width() + "x" + info.height() + ", " + info.bytes() + " bytes, by "
                    + (info.software() ? "libjpeg" : "the hardware encoder") + ", in " + output);
            return 0;
        } catch (CameraException | LibraryException error) {
            System.err.println("still: " + error.getMessage());
            return 1;
        }
    }
}
