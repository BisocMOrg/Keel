// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import io.github.bisocmorg.keel.camera.Camera;
import io.github.bisocmorg.keel.camera.CameraOptions;
import io.github.bisocmorg.keel.camera.encoding.Codec;
import io.github.bisocmorg.keel.errors.LibraryException;
import io.github.bisocmorg.keel.internal.NativeLibrary;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Camera checks that need a JVM of their own, which the camera suites start as child processes:
 *
 * <ul>
 * <li>{@code only-camera}: a program that lists, opens and reads the camera never loads libkeel.so;</li>
 * <li>{@code missing PATH}: loading a camera library that does not exist fails with the remedy;</li>
 * <li>{@code exit FILE}: a camera left open with an output at {@code System.exit} is closed by the binding's
 * shutdown hook, which finishes the file; a hook of the probe's own reports it.</li>
 * </ul>
 */
public final class CameraProbe {
    private CameraProbe() {
    }

    /**
     * Runs one check and exits with its status.
     *
     * @param args the check and its argument
     * @throws IOException if {@code /proc/self/maps} cannot be read
     * @throws InterruptedException if the probe is interrupted
     */
    public static void main(String[] args) throws IOException, InterruptedException {
        switch (args[0]) {
            case "only-camera" -> {
                int cameras = Camera.listCameras().size();
                try (Camera camera = Camera.open(CameraOptions.builder().width(160).height(120).build())) {
                    camera.start();
                    camera.readFrame();
                }
                String maps = Files.readString(Path.of("/proc/self/maps"));
                boolean board = NativeLibrary.loaded().isPresent() || maps.contains("libkeel.so");
                System.out.println("cameras " + cameras + ", libkeel-camera mapped "
                        + maps.contains("libkeel-camera.so") + ", libkeel loaded " + board);
                System.exit(board ? 1 : 0);
            }
            case "missing" -> {
                try {
                    Camera.load(Path.of(args[1]));
                    System.exit(2);
                } catch (LibraryException error) {
                    System.out.println(error.getClass().getSimpleName() + ": " + error.getMessage());
                    System.exit(0);
                }
            }
            case "exit" -> {
                Camera camera = Camera.open(CameraOptions.builder().width(160).height(120).framerate(120).build());
                camera.setEncoder(Codec.H264);
                camera.startOutput(Path.of(args[1]));
                camera.start();
                Thread.sleep(300);
                Runtime.getRuntime().addShutdownHook(Thread.ofPlatform().unstarted(() -> {
                    for (int i = 0; i < 250 && !camera.isClosed(); i++) {
                        try {
                            Thread.sleep(20);
                        } catch (InterruptedException interruption) {
                            return;
                        }
                    }
                    System.out.println("closed at exit: " + camera.isClosed());
                }));
                System.out.println("exiting with " + camera);
                System.exit(7);
            }
            default -> throw new IllegalArgumentException(args[0]);
        }
    }
}
