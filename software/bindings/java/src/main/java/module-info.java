// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM

/**
 * The Java binding of libkeel, the host library of the Keel robot baseboard, and of its camera library
 * libkeel-camera.
 *
 * <p>The module exports {@link io.github.bisocmorg.keel} and its API packages
 * {@link io.github.bisocmorg.keel.errors}, {@link io.github.bisocmorg.keel.loop},
 * {@link io.github.bisocmorg.keel.peripherals}, {@link io.github.bisocmorg.keel.sim} and
 * {@link io.github.bisocmorg.keel.state}, and the camera API {@link io.github.bisocmorg.keel.camera} with its packages
 * {@link io.github.bisocmorg.keel.camera.errors}, {@link io.github.bisocmorg.keel.camera.frames},
 * {@link io.github.bisocmorg.keel.camera.encoding}, {@link io.github.bisocmorg.keel.camera.outputs} and
 * {@link io.github.bisocmorg.keel.camera.stills}. It needs only {@code java.base} and the system's
 * {@code libkeel.so.1}, and {@code libkeel-camera.so.1} for the camera, which it calls through the Foreign Function
 * and Memory API, so the JVM must grant it native access: {@code --enable-native-access=io.github.bisocmorg.keel} on
 * the module path, or {@code --enable-native-access=ALL-UNNAMED} when the jar is on the class path. Only
 * {@code Frame.toBufferedImage()} uses {@code java.desktop}, which the module requires statically: an application on
 * the module path that calls it adds that module.
 */
module io.github.bisocmorg.keel {
    requires static transitive java.desktop;

    exports io.github.bisocmorg.keel;
    exports io.github.bisocmorg.keel.errors;
    exports io.github.bisocmorg.keel.loop;
    exports io.github.bisocmorg.keel.peripherals;
    exports io.github.bisocmorg.keel.sim;
    exports io.github.bisocmorg.keel.state;
    exports io.github.bisocmorg.keel.camera;
    exports io.github.bisocmorg.keel.camera.encoding;
    exports io.github.bisocmorg.keel.camera.errors;
    exports io.github.bisocmorg.keel.camera.frames;
    exports io.github.bisocmorg.keel.camera.outputs;
    exports io.github.bisocmorg.keel.camera.stills;
}
