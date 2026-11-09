// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.support;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.stream.Stream;

/** Local ports, and the descriptors and threads of this process, for the camera suites. */
public final class LocalResources {
    private LocalResources() {
    }

    /**
     * Returns a local TCP port that was free a moment ago.
     *
     * @return the port
     */
    public static int freePort() {
        try (ServerSocket probe = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            return probe.getLocalPort();
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }

    /**
     * Returns the number of open file descriptors of this process.
     *
     * @return the count
     */
    public static long openFileDescriptors() {
        return count(Path.of("/proc/self/fd"));
    }

    /**
     * Returns the number of threads of this process, the native ones included.
     *
     * @return the count
     */
    public static long threads() {
        return count(Path.of("/proc/self/task"));
    }

    private static long count(Path directory) {
        try (Stream<Path> entries = Files.list(directory)) {
            return entries.count();
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }
}
