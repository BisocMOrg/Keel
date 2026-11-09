// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
package io.github.bisocmorg.keel.camera;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.github.bisocmorg.keel.camera.errors.CameraNotFoundException;
import io.github.bisocmorg.keel.camera.errors.CameraPermissionException;
import io.github.bisocmorg.keel.camera.errors.CameraResource;
import io.github.bisocmorg.keel.camera.errors.ErrorReport;
import io.github.bisocmorg.keel.errors.Status;
import io.github.bisocmorg.keel.support.CameraSourceExtension;
import java.util.List;
import java.util.Set;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.extension.ExtendWith;

/** {@link Camera#listCameras()}: models, capabilities and sensor modes, and a camera that cannot be used. */
@ExtendWith(CameraSourceExtension.class)
class CameraListTest {
    @Test
    void bothSyntheticCamerasAreListedWithTheirModes() {
        List<ListedCamera> cameras = Camera.listCameras();
        assertEquals(2, cameras.size());
        ListedCamera module2 = cameras.get(0);
        ListedCamera module3 = cameras.get(1);
        assertEquals(0, module2.index());
        assertEquals("Camera Module 2", module2.model());
        assertEquals("imx219", module2.sensor());
        assertFalse(module2.autofocus());
        assertFalse(module2.info().inUse());
        assertEquals("Camera Module 3", module3.model());
        assertEquals("imx708", module3.sensor());
        assertTrue(module3.autofocus());
        assertEquals(Set.of(CameraInfoFlag.AUTOFOCUS), module3.info().flags());
        assertEquals(3280, module2.info().pixelArrayWidth());
        assertEquals(2464, module2.info().pixelArrayHeight());
        assertEquals("/base/soc/i2c0mux/i2c@1/imx219@10", module2.info().id());
        assertEquals(8, module2.modes().size());
        assertEquals(module2.info().modeCount(), module2.modes().size());
        assertEquals(3, module3.modes().size());
        assertEquals(new SensorMode(640, 480, 8, 200160, 1000, 752, 1280, 960), module2.modes().get(0));
        assertEquals(200.16, module2.modes().get(0).maxFramerate(), 1e-9);
        assertThrows(UnsupportedOperationException.class, () -> module2.modes().clear());
    }

    @Test
    void noCameraIsAnEmptyList() {
        CameraSourceExtension.fault("no-camera");
        assertTrue(Camera.listCameras().isEmpty());
    }

    @Test
    void aCameraHeldElsewhereIsListedWithoutRates() {
        CameraSourceExtension.fault("busy");
        for (ListedCamera listed : Camera.listCameras()) {
            assertTrue(listed.info().inUse());
            assertTrue(listed.modes().stream()
                    .allMatch(mode -> mode.maxFramerateMilli() == 0 && mode.cropWidth() == 0));
        }
    }

    @Test
    void aCameraThisProcessHoldsIsListedInUse() {
        try (Camera held = Camera.open(CameraOptions.builder().index(1).width(320).height(240).build())) {
            List<ListedCamera> cameras = Camera.listCameras();
            assertFalse(cameras.get(0).info().inUse());
            assertTrue(cameras.get(1).info().inUse());
            assertEquals(1, held.index());
        }
    }

    @Test
    void aDeviceNodeWithoutAccessNamesTheVideoGroup() {
        CameraSourceExtension.fault("permission");
        CameraPermissionException error = assertThrows(CameraPermissionException.class, Camera::listCameras);
        assertEquals(Status.E_PERMISSION, error.status());
        assertEquals("keel_camera_list_open", error.function());
        ErrorReport report = error.report().orElseThrow();
        assertEquals(CameraResource.DEVICE_NODE, report.resource());
        assertEquals("/dev/media0", report.path());
        assertTrue(report.message().contains("video group"), report.message());
        assertTrue(error.getMessage().contains("sudo keel-setup --user NAME"), error.getMessage());
    }

    @Test
    void aMissingIpaModuleIsNotFound() {
        CameraSourceExtension.fault("ipa-missing");
        CameraNotFoundException error = assertThrows(CameraNotFoundException.class, Camera::listCameras);
        assertEquals(CameraResource.IPA, error.report().orElseThrow().resource());
        assertTrue(error.getMessage().contains("ipa_rpi_vc4.so"), error.getMessage());
        assertTrue(error.getMessage().contains("libcamera-ipa"), error.getMessage());
    }
}
