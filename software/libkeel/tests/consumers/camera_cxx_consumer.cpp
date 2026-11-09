// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
//
// A C++17 application that sees only <keel/camera.h> and libkeel-camera.so. It
// wraps the handle in an RAII class whose destructor closes without throwing,
// turns failures into exceptions that carry the library's message, reads
// frames on the owning thread and closes the camera from a second
// std::thread while a read waits, which wakes that read.
#include <keel/camera.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class CameraError : public std::runtime_error {
public:
    CameraError(int32_t status, const std::string &message)
        : std::runtime_error(message), status_(status)
    {
    }
    int32_t status() const { return status_; }

private:
    int32_t status_;
};

class Camera {
public:
    explicit Camera(const keel_camera_options &options)
    {
        keel_camera_error error{};
        error.struct_size = sizeof error;
        int32_t status = keel_camera_open(&options, &camera_, &error);
        if (status != KEEL_OK) {
            throw CameraError(status, error.message);
        }
    }
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;
    ~Camera() { keel_camera_close(camera_); } // never throws; the status is diagnostic

    keel_camera *handle() const { return camera_; }

    void check(int32_t status) const
    {
        if (status != KEEL_OK) {
            keel_camera_error error{};
            error.struct_size = sizeof error;
            keel_camera_last_error(camera_, &error);
            throw CameraError(status, error.message);
        }
    }

    keel_camera_stream_info stream() const
    {
        keel_camera_stream_info info{};
        info.struct_size = sizeof info;
        check(keel_camera_get_stream(camera_, &info));
        return info;
    }

private:
    keel_camera *camera_ = nullptr;
};

} // namespace

int main()
{
    keel_camera_error error{};
    error.struct_size = sizeof error;
    keel_camera_list *list = nullptr;
    uint32_t count = 0;
    if (keel_camera_list_open(&list, &error) != KEEL_OK) {
        std::fprintf(stderr, "listing failed: %s\n", error.message);
        return 1;
    }
    keel_camera_list_count(list, &count);
    keel_camera_list_close(list);
    if (count == 0) {
        std::printf("no camera: nothing to capture\n");
        return 0;
    }
    try {
        keel_camera_options options{};
        options.struct_size = sizeof options;
        options.format = KEEL_CAMERA_FORMAT_BGR888; // OpenCV's order
        options.framerate_milli = 5000;
        Camera camera(options);
        keel_camera_stream_info stream = camera.stream();
        std::vector<uint8_t> frame(stream.frame_bytes);
        camera.check(keel_camera_start(camera.handle()));
        keel_camera_frame_info info{};
        info.struct_size = sizeof info;
        camera.check(keel_camera_read_frame(camera.handle(), frame.data(), static_cast<uint32_t>(frame.size()), 3000,
                                            &info));
        std::printf("BGR frame %ux%u, stride %u\n", info.width, info.height, info.stride0);
        // A second thread closes while this one waits for the next frame.
        std::thread closer([&camera] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            keel_camera_close(camera.handle());
        });
        int32_t status = keel_camera_read_frame(camera.handle(), frame.data(), static_cast<uint32_t>(frame.size()),
                                                10000, &info);
        closer.join();
        if (status != KEEL_E_INVALID_HANDLE) {
            std::fprintf(stderr, "a read during close returned %d\n", static_cast<int>(status));
            return 1;
        }
        std::printf("the close woke the read\n");
    } catch (const CameraError &e) {
        std::fprintf(stderr, "camera error %d: %s\n", static_cast<int>(e.status()), e.what());
        return 1;
    }
    return 0;
}
