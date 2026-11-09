// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import java.util.List;
import java.util.Objects;

/**
 * One camera that was present when {@link Camera#listCameras()} ran: its {@link CameraInfo} and its sensor modes.
 *
 * @param info the camera's index, model, sensor, full resolution and capabilities
 * @param modes the sensor's read-out modes, ordered by bit depth, then by size
 */
public record ListedCamera(CameraInfo info, List<SensorMode> modes) {

    /**
     * Validates and copies the components.
     *
     * @throws NullPointerException if a component is {@code null}
     */
    public ListedCamera {
        Objects.requireNonNull(info, "info");
        modes = List.copyOf(modes);
    }

    /**
     * Returns the index to pass to {@link CameraOptions#index()}.
     *
     * @return the camera index
     */
    public int index() {
        return (int) info.index();
    }

    /**
     * Returns the product name.
     *
     * @return for example {@code "Camera Module 3"}, or the sensor name for other cameras
     */
    public String model() {
        return info.model();
    }

    /**
     * Returns the sensor name.
     *
     * @return for example {@code "imx219"} or {@code "imx708_wide"}
     */
    public String sensor() {
        return info.sensor();
    }

    /**
     * Returns whether the camera has an autofocus lens (Camera Module 3).
     *
     * @return {@code true} with autofocus
     */
    public boolean autofocus() {
        return info.autofocus();
    }
}
