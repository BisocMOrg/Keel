// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
//
// The libcamera capture source (SPEC section 26.3). libcamera's API is C++;
// this file is the only one that uses it and exports the C interface
// cam_libcamera_source of camera_internal.h. No exception or C++ type
// crosses that interface.
//
// - One CameraManager per process (libcamera allows no more), shared by the
//   listings and handles that are open and stopped with the last of them.
// - Controls are looked up by name in the camera's ControlInfoMap, so the
//   same code drives libcamera 0.2 (AeEnable era) and 0.5 or later
//   (ExposureTimeMode, AnalogueGainMode): only controls the camera lists are
//   set.
// - libcamera's log goes to an internal buffer, not the application's
//   stderr, unless LIBCAMERA_LOG_FILE or LIBCAMERA_LOG_LEVELS is set; the
//   last error line is appended to failure messages.
// - Frames are libcamera's own buffers, mapped once; a frame goes back to
//   the camera (its request is queued again) when the pipeline releases it.
//
// SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

#include <libcamera/libcamera.h>

#include "camera/camera_internal.h"

namespace {

using namespace libcamera;

// ---- libcamera's log ---------------------------------------------------------

class LogBuffer : public std::streambuf {
public:
    std::string lastError()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return lastError_;
    }

protected:
    int overflow(int c) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        put(static_cast<char>(c));
        return c;
    }

    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (std::streamsize i = 0; i < n; ++i) {
            put(s[i]);
        }
        return n;
    }

private:
    void put(char c)
    {
        if (c != '\n') {
            if (line_.size() < 512) {
                line_.push_back(c);
            }
            return;
        }
        if (line_.find(" ERROR ") != std::string::npos || line_.find(" FATAL ") != std::string::npos) {
            // "[0:00:01.2] [pid] ERROR Category file.cpp:12 message": keep the message.
            std::size_t at = line_.find(".cpp:");
            std::size_t space = at == std::string::npos ? std::string::npos : line_.find(' ', at);
            lastError_ = space == std::string::npos ? line_ : line_.substr(space + 1);
        }
        line_.clear();
    }

    std::mutex mutex_;
    std::string line_;
    std::string lastError_;
};

LogBuffer logBuffer;
std::ostream logStream(&logBuffer);

// ---- the process's camera manager ----------------------------------------------

std::mutex managerMutex;
std::unique_ptr<CameraManager> manager;
unsigned managerUsers = 0;

std::string withLog(const std::string &message)
{
    std::string last = logBuffer.lastError();
    return last.empty() ? message : message + " (libcamera: " + last + ")";
}

int32_t managerAcquire(cam_error *error)
{
    std::lock_guard<std::mutex> guard(managerMutex);
    if (!manager) {
        if (getenv("LIBCAMERA_LOG_FILE") == nullptr && getenv("LIBCAMERA_LOG_LEVELS") == nullptr) {
            logSetStream(&logStream, false);
        }
        try {
            manager = std::make_unique<CameraManager>();
        } catch (const std::bad_alloc &) {
            return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, nullptr,
                            "out of memory");
        }
        int rc = manager->start();
        if (rc != 0) {
            manager.reset();
            return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_LIBCAMERA, -rc, nullptr, "%s",
                            withLog(std::string("libcamera's camera manager failed to start: ") + strerror(-rc))
                                .c_str());
        }
    }
    managerUsers++;
    return KEEL_OK;
}

void managerRelease()
{
    std::lock_guard<std::mutex> guard(managerMutex);
    if (managerUsers > 0 && --managerUsers == 0) {
        manager.reset(); // stops it and joins its threads
    }
}

// Directory of libcamera's IPA modules, for the diagnosis of an empty list.
std::string ipaDirectory()
{
    const char *path = getenv("LIBCAMERA_IPA_MODULE_PATH");
    if (path != nullptr && path[0] != '\0') {
        std::string first(path);
        return first.substr(0, first.find(':'));
    }
    Dl_info info;
    // A function defined in libcamera.so itself (not inline in a header).
    auto symbol = &libcamera::logSetTarget;
    void *address = nullptr;
    static_assert(sizeof(symbol) == sizeof(address), "function pointer size");
    memcpy(&address, &symbol, sizeof(address));
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
        return std::string();
    }
    std::string library(info.dli_fname);
    std::size_t slash = library.rfind('/');
    return slash == std::string::npos ? std::string() : library.substr(0, slash) + "/libcamera/ipa";
}

const ControlId *findControl(const ControlInfoMap &controls, const char *name)
{
    for (const auto &entry : controls) {
        if (entry.first->name() == name) {
            return entry.first;
        }
    }
    return nullptr;
}

PixelFormat pixelFormat(uint32_t format)
{
    // libcamera names packed RGB after DRM, little-endian: its BGR888 is
    // R, G, B in memory.
    switch (format) {
    case KEEL_CAMERA_FORMAT_RGB888:
        return formats::BGR888;
    case KEEL_CAMERA_FORMAT_BGR888:
        return formats::RGB888;
    default:
        return formats::YUV420;
    }
}

Orientation orientation(uint32_t hflip, uint32_t vflip)
{
    if (hflip && vflip) {
        return Orientation::Rotate180;
    }
    if (hflip) {
        return Orientation::Rotate0Mirror;
    }
    if (vflip) {
        return Orientation::Rotate180Mirror;
    }
    return Orientation::Rotate0;
}

uint32_t bitDepth(const PixelFormat &format)
{
    std::string name = format.toString();
    std::size_t digit = name.find_first_of("0123456789");
    return digit == std::string::npos ? 0u : static_cast<uint32_t>(strtoul(name.c_str() + digit, nullptr, 10));
}

// How describe() learns each mode's frame-rate limit and crop, which need
// the camera acquired and configured in that mode.
enum class Probe {
    Acquire, // a listing: acquire the camera if no process holds it
    Held     // an open: the caller holds the camera
};

std::string sensorOf(const std::shared_ptr<Camera> &camera)
{
    auto model = camera->properties().get(properties::Model);
    return model ? std::string(*model) : std::string("unknown");
}

void describe(const std::shared_ptr<Camera> &camera, uint32_t index, cam_camera_desc *desc, Probe probe)
{
    keel_camera_info &info = desc->info;
    memset(desc, 0, sizeof(*desc));
    info.struct_size = sizeof(info);
    info.index = index;
    cam_copy_string(info.id, sizeof(info.id), camera->id().c_str());
    const ControlList &properties = camera->properties();
    auto model = properties.get(properties::Model);
    std::string sensor = model ? std::string(*model) : std::string("unknown");
    cam_copy_string(info.sensor, sizeof(info.sensor), sensor.c_str());
    cam_copy_string(info.model, sizeof(info.model), cam_model_name(info.sensor));
    auto array = properties.get(properties::PixelArraySize);
    if (array) {
        info.pixel_array_width = array->width;
        info.pixel_array_height = array->height;
    }
    if (findControl(camera->controls(), "AfMode") != nullptr) {
        info.flags |= KEEL_CAMERA_INFO_AUTOFOCUS;
    }
    // Sensor modes are the raw stream's sizes, one per bit depth.
    std::unique_ptr<CameraConfiguration> raw = camera->generateConfiguration({StreamRole::Raw});
    struct Mode {
        uint32_t depth, width, height;
        PixelFormat format;
    };
    std::vector<Mode> modes;
    if (raw && !raw->empty()) {
        const StreamFormats &formats = raw->at(0).formats();
        for (const PixelFormat &format : formats.pixelformats()) {
            uint32_t depth = bitDepth(format);
            for (const Size &size : formats.sizes(format)) {
                bool seen = false;
                for (const Mode &mode : modes) {
                    seen = seen || (mode.depth == depth && mode.width == size.width && mode.height == size.height);
                }
                if (!seen) {
                    modes.push_back({depth, size.width, size.height, format});
                }
            }
        }
    }
    std::sort(modes.begin(), modes.end(), [](const Mode &a, const Mode &b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.width * a.height < b.width * b.height;
    });
    if (modes.size() > CAM_MODES_MAX) {
        modes.resize(CAM_MODES_MAX);
    }
    info.mode_count = static_cast<uint32_t>(modes.size());
    bool acquired = probe == Probe::Held || camera->acquire() == 0;
    if (!acquired) {
        info.flags |= KEEL_CAMERA_INFO_IN_USE;
    }
    for (std::size_t i = 0; i < modes.size(); ++i) {
        keel_camera_mode &out = desc->modes[i];
        out.struct_size = sizeof(out);
        out.width = modes[i].width;
        out.height = modes[i].height;
        out.bit_depth = modes[i].depth;
        if (!acquired) {
            continue;
        }
        raw->at(0).size = Size(modes[i].width, modes[i].height);
        raw->at(0).pixelFormat = modes[i].format;
        if (raw->validate() == CameraConfiguration::Invalid || camera->configure(raw.get()) != 0) {
            continue;
        }
        auto limits = camera->controls().find(&controls::FrameDurationLimits);
        if (limits != camera->controls().end()) {
            int64_t shortest = limits->second.min().get<int64_t>();
            if (shortest > 0) {
                out.max_framerate_milli = static_cast<uint32_t>(1000000000LL / shortest);
            }
        }
        auto crop = camera->properties().get(properties::ScalerCropMaximum);
        if (crop) {
            out.crop_x = static_cast<uint32_t>(crop->x);
            out.crop_y = static_cast<uint32_t>(crop->y);
            out.crop_width = crop->width;
            out.crop_height = crop->height;
        }
    }
    if (acquired && probe == Probe::Acquire) {
        camera->release();
    }
}

// ---- one acquired camera ---------------------------------------------------------

struct Source;

struct SourceHandle {
    cam_source base; // first: the C side sees a cam_source
    Source *self;
};

struct Source {
    SourceHandle handle{};
    std::shared_ptr<Camera> camera;
    std::unique_ptr<CameraConfiguration> config;
    std::unique_ptr<FrameBufferAllocator> allocator;
    Stream *stream = nullptr;
    std::vector<std::unique_ptr<Request>> requests;
    std::unique_ptr<cam_frame[]> frames; // cam_frame holds an atomic: not copyable
    std::vector<std::pair<void *, size_t>> maps;
    cam_frame_sink sink = nullptr;
    cam_failure_sink failure = nullptr;
    void *context = nullptr;
    std::mutex mutex;
    std::condition_variable focusChanged;
    bool running = false;
    bool triggerPending = false;
    uint32_t afMode = KEEL_CAMERA_AF_DEFAULT;
    uint32_t afState = KEEL_CAMERA_AF_STATE_IDLE;
    uint64_t framesSeen = 0;
    bool hasAutofocus = false;
    cam_camera_desc desc{}; // with the modes probed at open

    void onRequestCompleted(Request *request);
};

Source *sourceOf(cam_source *source)
{
    return reinterpret_cast<SourceHandle *>(source)->self;
}

void Source::onRequestCompleted(Request *request)
{
    if (request->status() == Request::RequestCancelled) {
        return; // stopping: the request stays idle
    }
    const Request::BufferMap &buffers = request->buffers();
    if (buffers.empty()) {
        return;
    }
    FrameBuffer *buffer = buffers.begin()->second;
    const FrameMetadata &metadata = buffer->metadata();
    cam_frame &frame = frames[request->cookie()];
    if (hasAutofocus) {
        const ControlList &md = request->metadata();
        auto state = md.get(controls::AfState);
        if (state) {
            std::lock_guard<std::mutex> guard(mutex);
            afState = static_cast<uint32_t>(*state);
            framesSeen++;
            focusChanged.notify_all();
        }
    }
    if (metadata.status != FrameMetadata::FrameSuccess) {
        handle.base.ops->release(&handle.base, &frame); // a broken frame goes straight back
        return;
    }
    frame.sequence = metadata.sequence;
    frame.timestamp_ns = metadata.timestamp;
    cam_frame_deliver(&frame);
    sink(context, &frame);
}

// ControlList for the start of capture.
ControlList startControls(const std::shared_ptr<Camera> &camera, const cam_stream *stream,
                          const cam_controls *c, bool hasAutofocus)
{
    const ControlInfoMap &infos = camera->controls();
    ControlList list(infos);
    auto set = [&](const char *name, const ControlValue &value) {
        const ControlId *id = findControl(infos, name);
        if (id != nullptr) {
            list.set(id->id(), value);
        }
    };
    if (stream->framerate_milli != 0) {
        int64_t duration = static_cast<int64_t>(1000000000ULL / stream->framerate_milli);
        std::array<int64_t, 2> limits{duration, duration};
        set("FrameDurationLimits", ControlValue(Span<const int64_t, 2>(limits)));
    }
    if (hasAutofocus) {
        int32_t mode = controls::AfModeContinuous;
        if (c->af_mode == KEEL_CAMERA_AF_MANUAL) {
            mode = controls::AfModeManual;
        } else if (c->af_mode == KEEL_CAMERA_AF_AUTO) {
            mode = controls::AfModeAuto;
        }
        set("AfMode", ControlValue(mode));
        if (c->af_mode == KEEL_CAMERA_AF_MANUAL) {
            set("LensPosition", ControlValue(static_cast<float>(c->lens_position_milli) / 1000.0f));
        }
        if (c->af_range != KEEL_CAMERA_AF_RANGE_DEFAULT) {
            int32_t range = c->af_range == KEEL_CAMERA_AF_RANGE_MACRO  ? controls::AfRangeMacro
                            : c->af_range == KEEL_CAMERA_AF_RANGE_FULL ? controls::AfRangeFull
                                                                       : controls::AfRangeNormal;
            set("AfRange", ControlValue(range));
        }
        if (c->af_mode == KEEL_CAMERA_AF_AUTO) {
            set("AfTrigger", ControlValue(static_cast<int32_t>(controls::AfTriggerStart)));
        }
    }
    if (c->exposure_us != 0) {
        set("ExposureTimeMode", ControlValue(static_cast<int32_t>(1))); // Manual (libcamera 0.5 or later)
        set("ExposureTime", ControlValue(static_cast<int32_t>(c->exposure_us)));
    }
    if (c->gain_milli != 0) {
        set("AnalogueGainMode", ControlValue(static_cast<int32_t>(1)));
        set("AnalogueGain", ControlValue(static_cast<float>(c->gain_milli) / 1000.0f));
    }
    if (c->ev_milli != 0) {
        set("ExposureValue", ControlValue(static_cast<float>(c->ev_milli) / 1000.0f));
    }
    if (c->awb_mode == KEEL_CAMERA_AWB_MANUAL) {
        std::array<float, 2> gains{static_cast<float>(c->red_gain_milli) / 1000.0f,
                                   static_cast<float>(c->blue_gain_milli) / 1000.0f};
        set("AwbEnable", ControlValue(false));
        set("ColourGains", ControlValue(Span<const float, 2>(gains)));
    } else {
        set("AwbEnable", ControlValue(true));
        if (c->awb_mode != KEEL_CAMERA_AWB_AUTO) {
            // keel_camera_awb values 1..6 equal libcamera's AwbMode values.
            set("AwbMode", ControlValue(static_cast<int32_t>(c->awb_mode)));
        }
    }
    return list;
}

int32_t failCamera(cam_error *error, int32_t status, int rc, const char *what)
{
    return cam_fail(error, status, KEEL_CAMERA_RESOURCE_LIBCAMERA, rc < 0 ? -rc : rc, nullptr, "%s",
                    withLog(std::string("libcamera ") + what + (rc != 0 ? std::string(": ") + strerror(rc < 0 ? -rc : rc)
                                                                        : std::string()))
                        .c_str());
}

// ---- the C interface ---------------------------------------------------------------

int32_t sourceList(cam_camera_desc *cameras, uint32_t capacity, uint32_t *count, cam_error *error)
{
    *count = 0;
    int32_t rc = managerAcquire(error);
    if (rc != KEEL_OK) {
        return rc;
    }
    try {
        std::vector<std::shared_ptr<Camera>> list = manager->cameras();
        for (std::size_t i = 0; i < list.size() && i < capacity; ++i) {
            describe(list[i], static_cast<uint32_t>(i), &cameras[i], Probe::Acquire);
            (*count)++;
        }
        if (list.empty()) {
            std::string ipa = ipaDirectory();
            rc = cam_diagnose_no_camera("/dev", ipa.empty() ? nullptr : ipa.c_str(), error);
        }
    } catch (const std::bad_alloc &) {
        rc = cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, nullptr, "out of memory");
    }
    managerRelease();
    return rc;
}

// Finds and acquires the camera; every Camera reference it takes is gone
// when it returns, so the caller may then stop the camera manager.
int32_t acquireCamera(uint32_t index, Source **out, cam_camera_desc *desc, cam_error *error)
{
    std::vector<std::shared_ptr<Camera>> list = manager->cameras();
    if (index >= list.size()) {
        if (!list.empty()) {
            return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_CAMERA, ENODEV, nullptr,
                            "there is no camera %u: %zu camera(s) connected", index, list.size());
        }
        std::string ipa = ipaDirectory();
        int32_t rc = cam_diagnose_no_camera("/dev", ipa.empty() ? nullptr : ipa.c_str(), error);
        if (rc != KEEL_OK) {
            return rc;
        }
        return cam_fail(error, KEEL_E_NOT_FOUND, KEEL_CAMERA_RESOURCE_CAMERA, ENODEV, nullptr,
                        "no camera detected: with the power off, check the ribbon cable at the Raspberry "
                        "Pi's camera connector, and that config.txt enables the camera (camera_auto_detect=1, "
                        "or dtoverlay=imx219 or dtoverlay=imx708)");
    }
    const std::shared_ptr<Camera> &camera = list[index];
    int acquired = camera->acquire();
    if (acquired == -EBUSY) {
        return cam_fail(error, KEEL_E_IN_USE, KEEL_CAMERA_RESOURCE_CAMERA, EBUSY, nullptr,
                        "camera %u (%s) is in use by another process or handle; only one can use a camera at "
                        "a time", index, sensorOf(camera).c_str());
    }
    if (acquired != 0) {
        return failCamera(error, KEEL_E_SETUP_IO, acquired, "could not acquire the camera");
    }
    describe(camera, index, desc, Probe::Held);
    Source *source = new Source();
    source->handle.base.ops = &cam_libcamera_source;
    source->handle.self = source;
    source->camera = camera;
    source->hasAutofocus = (desc->info.flags & KEEL_CAMERA_INFO_AUTOFOCUS) != 0;
    source->desc = *desc;
    *out = source;
    return KEEL_OK;
}

int32_t sourceOpen(uint32_t index, cam_source **out, cam_camera_desc *desc, cam_error *error)
{
    Source *source = nullptr;
    *out = nullptr;
    int32_t rc = managerAcquire(error);
    if (rc != KEEL_OK) {
        return rc;
    }
    try {
        rc = acquireCamera(index, &source, desc, error);
    } catch (const std::bad_alloc &) {
        rc = cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, nullptr, "out of memory");
    }
    if (rc != KEEL_OK) {
        managerRelease();
        return rc;
    }
    *out = &source->handle.base;
    return KEEL_OK;
}

// The sensor mode for a stream: among the modes at least as large as the
// frames that reach the frame rate, the one that sees the most of the
// sensor (largest crop), then the smallest, then the deepest. libcamera's
// own choice favours the closest size, which on the IMX219 crops 640x480
// to the centre of the sensor. Returns -1 to leave the choice to libcamera.
int chooseMode(const cam_camera_desc &desc, const cam_stream *stream)
{
    int best = -1;
    for (uint32_t i = 0; i < desc.info.mode_count; ++i) {
        const keel_camera_mode &m = desc.modes[i];
        if (m.width < stream->width || m.height < stream->height || m.crop_width == 0) {
            continue;
        }
        if (stream->framerate_milli != 0 && m.max_framerate_milli < stream->framerate_milli) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const keel_camera_mode &b = desc.modes[best];
        uint64_t area = static_cast<uint64_t>(m.crop_width) * m.crop_height;
        uint64_t bestArea = static_cast<uint64_t>(b.crop_width) * b.crop_height;
        uint64_t size = static_cast<uint64_t>(m.width) * m.height;
        uint64_t bestSize = static_cast<uint64_t>(b.width) * b.height;
        if (area > bestArea || (area == bestArea && (size < bestSize ||
                                                      (size == bestSize && m.bit_depth > b.bit_depth)))) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Builds and validates the configuration of a stream.
int32_t buildConfig(Source *source, cam_stream *stream, std::unique_ptr<CameraConfiguration> *out,
                    cam_error *error)
{
    std::unique_ptr<CameraConfiguration> config = source->camera->generateConfiguration(
        {stream->still ? StreamRole::StillCapture : StreamRole::VideoRecording});
    if (!config || config->empty()) {
        return failCamera(error, KEEL_E_SETUP_IO, 0, "offers no stream for this camera");
    }
    StreamConfiguration &sc = config->at(0);
    PixelFormat wanted = pixelFormat(stream->format);
    sc.size = Size(stream->width, stream->height);
    sc.pixelFormat = wanted;
    sc.bufferCount = stream->buffer_count;
    sc.colorSpace = ColorSpace::Sycc;
    Orientation wantedOrientation = orientation(stream->hflip, stream->vflip);
    config->orientation = wantedOrientation;
    int mode = chooseMode(source->desc, stream);
    if (mode >= 0) {
        config->sensorConfig = SensorConfiguration();
        config->sensorConfig->bitDepth = source->desc.modes[mode].bit_depth;
        config->sensorConfig->outputSize = Size(source->desc.modes[mode].width, source->desc.modes[mode].height);
        if (config->validate() == CameraConfiguration::Invalid) {
            config->sensorConfig.reset(); // libcamera chooses
            sc.size = Size(stream->width, stream->height);
            sc.pixelFormat = wanted;
        }
    }
    if (config->validate() == CameraConfiguration::Invalid) {
        return cam_fail(error, KEEL_E_INVALID_ARGUMENT, KEEL_CAMERA_RESOURCE_OPTIONS, 0, nullptr,
                        "the camera cannot produce %ux%u frames", stream->width, stream->height);
    }
    if (sc.pixelFormat != wanted) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_CAMERA, 0, nullptr,
                        "the camera cannot produce %s frames", wanted.toString().c_str());
    }
    if (config->orientation != wantedOrientation) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_CAMERA, 0, nullptr,
                        "the camera cannot flip or rotate its image this way");
    }
    stream->width = sc.size.width;
    stream->height = sc.size.height;
    stream->buffer_count = sc.bufferCount;
    // Plane layout: libcamera gives the stride of plane 0; the chroma planes
    // of YUV420 follow at half the stride.
    memset(stream->stride, 0, sizeof(stream->stride));
    memset(stream->offset, 0, sizeof(stream->offset));
    stream->stride[0] = sc.stride;
    if (stream->format == KEEL_CAMERA_FORMAT_I420) {
        stream->plane_count = 3;
        stream->stride[1] = sc.stride / 2;
        stream->stride[2] = sc.stride / 2;
        stream->offset[1] = sc.stride * sc.size.height;
        stream->offset[2] = stream->offset[1] + (sc.stride / 2) * (sc.size.height / 2);
    } else {
        stream->plane_count = 1;
    }
    stream->frame_bytes = sc.frameSize;
    if (out != nullptr) {
        *out = std::move(config);
    }
    return KEEL_OK;
}

int32_t sourceConfigure(cam_source *base, cam_stream *stream, cam_error *error)
{
    try {
        return buildConfig(sourceOf(base), stream, nullptr, error);
    } catch (const std::bad_alloc &) {
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, nullptr, "out of memory");
    }
}

void unmapAll(Source *source)
{
    for (auto &map : source->maps) {
        munmap(map.first, map.second);
    }
    source->maps.clear();
}

void freeAll(Source *source)
{
    source->camera->requestCompleted.disconnect(source);
    source->requests.clear();
    unmapAll(source);
    source->frames.reset();
    if (source->allocator && source->stream != nullptr) {
        source->allocator->free(source->stream);
    }
    source->allocator.reset();
    source->config.reset();
    source->stream = nullptr;
}

int32_t startCapture(Source *source, const cam_stream *wanted, const cam_controls *controls, cam_error *error)
{
    cam_stream stream = *wanted;
    std::unique_ptr<CameraConfiguration> config;
    int32_t rc = buildConfig(source, &stream, &config, error);
    if (rc != KEEL_OK) {
        return rc;
    }
    if (stream.width != wanted->width || stream.height != wanted->height || stream.stride[0] != wanted->stride[0]) {
        return cam_fail(error, KEEL_E_SETUP_IO, KEEL_CAMERA_RESOURCE_LIBCAMERA, 0, nullptr,
                        "libcamera changed the stream from %ux%u to %ux%u", wanted->width, wanted->height,
                        stream.width, stream.height);
    }
    int configured = source->camera->configure(config.get());
    if (configured != 0) {
        return failCamera(error, KEEL_E_SETUP_IO, configured, "refused the configuration");
    }
    source->config = std::move(config);
    source->stream = source->config->at(0).stream();
    source->allocator = std::make_unique<FrameBufferAllocator>(source->camera);
    int count = source->allocator->allocate(source->stream);
    if (count <= 0) {
        source->allocator.reset();
        return failCamera(error, count == -ENOMEM ? KEEL_E_NO_MEMORY : KEEL_E_SETUP_IO, count,
                          "could not allocate frame buffers");
    }
    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = source->allocator->buffers(source->stream);
    source->frames.reset(new cam_frame[buffers.size()]());
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const FrameBuffer &buffer = *buffers[i];
        auto planes = buffer.planes();
        int fd = planes[0].fd.get();
        std::size_t length = 0;
        for (std::size_t p = 0; p < planes.size(); ++p) {
            if (planes[p].fd.get() != fd || (p > 0 && planes[p].offset != stream.offset[p])) {
                freeAll(source);
                return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_LIBCAMERA, 0, nullptr,
                                "libcamera's frame buffers do not hold their planes contiguously");
            }
            length = std::max(length, static_cast<std::size_t>(planes[p].offset) + planes[p].length);
        }
        void *map = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            int os_error = errno;
            freeAll(source);
            return failCamera(error, KEEL_E_SETUP_IO, os_error, "frame buffers could not be mapped");
        }
        source->maps.emplace_back(map, length);
        cam_frame &frame = source->frames[i];
        frame.source = &source->handle.base;
        frame.index = static_cast<uint32_t>(i);
        frame.data = static_cast<uint8_t *>(map) + planes[0].offset;
        frame.bytes = stream.frame_bytes;
        frame.fd = fd;
        frame.fd_length = static_cast<uint32_t>(length);
        std::unique_ptr<Request> request = source->camera->createRequest(i);
        if (!request || request->addBuffer(source->stream, buffers[i].get()) != 0) {
            freeAll(source);
            return failCamera(error, KEEL_E_SETUP_IO, 0, "could not create a capture request");
        }
        source->requests.push_back(std::move(request));
    }
    source->camera->requestCompleted.connect(source, &Source::onRequestCompleted);
    ControlList start = startControls(source->camera, &stream, controls, source->hasAutofocus);
    {
        std::lock_guard<std::mutex> guard(source->mutex);
        source->afMode = controls->af_mode;
        source->afState = KEEL_CAMERA_AF_STATE_IDLE;
        source->framesSeen = 0;
        source->triggerPending = false;
    }
    int started = source->camera->start(&start);
    if (started != 0) {
        freeAll(source);
        return failCamera(error, KEEL_E_SETUP_IO, started, "could not start the camera");
    }
    {
        std::lock_guard<std::mutex> guard(source->mutex);
        source->running = true;
        for (auto &request : source->requests) {
            int queued = source->camera->queueRequest(request.get());
            if (queued != 0) {
                source->running = false;
                break;
            }
        }
    }
    if (!source->running) {
        source->camera->stop();
        freeAll(source);
        return failCamera(error, KEEL_E_SETUP_IO, 0, "could not queue the capture requests");
    }
    return KEEL_OK;
}

int32_t sourceStart(cam_source *base, const cam_stream *stream, const cam_controls *controls, cam_frame_sink sink,
                    cam_failure_sink failure, void *context, cam_error *error)
{
    Source *source = sourceOf(base);
    source->sink = sink;
    source->failure = failure;
    source->context = context;
    try {
        return startCapture(source, stream, controls, error);
    } catch (const std::bad_alloc &) {
        freeAll(source);
        return cam_fail(error, KEEL_E_NO_MEMORY, KEEL_CAMERA_RESOURCE_MEMORY, ENOMEM, nullptr, "out of memory");
    }
}

void sourceRelease(cam_source *base, cam_frame *frame)
{
    Source *source = sourceOf(base);
    std::lock_guard<std::mutex> guard(source->mutex);
    if (!source->running || frame->index >= source->requests.size()) {
        return;
    }
    Request *request = source->requests[frame->index].get();
    request->reuse(Request::ReuseBuffers);
    if (source->triggerPending) {
        const ControlId *id = findControl(source->camera->controls(), "AfTrigger");
        if (id != nullptr) {
            request->controls().set(id->id(), ControlValue(static_cast<int32_t>(controls::AfTriggerStart)));
        }
        source->triggerPending = false;
    }
    if (source->camera->queueRequest(request) != 0 && source->failure != nullptr) {
        cam_error error;
        cam_error_clear(&error);
        cam_fail(&error, KEEL_E_PERIPHERAL, KEEL_CAMERA_RESOURCE_LIBCAMERA, 0, nullptr, "%s",
                 withLog("libcamera stopped accepting capture requests").c_str());
        source->failure(source->context, &error);
    }
}

int32_t sourceAutofocus(cam_source *base, uint32_t timeout_ms, uint32_t *state, cam_error *error)
{
    Source *source = sourceOf(base);
    std::unique_lock<std::mutex> lock(source->mutex);
    if (!source->hasAutofocus || source->afMode == KEEL_CAMERA_AF_MANUAL) {
        return cam_fail(error, KEEL_E_UNSUPPORTED, KEEL_CAMERA_RESOURCE_CAMERA, 0, nullptr,
                        source->hasAutofocus ? "the lens is in manual mode" : "this camera has no autofocus");
    }
    uint64_t first = source->framesSeen;
    if (source->afMode == KEEL_CAMERA_AF_AUTO) {
        source->triggerPending = true;
    }
    // Two frames give the request with the trigger time to complete.
    bool ended = source->focusChanged.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return source->framesSeen >= first + 2 && (source->afState == KEEL_CAMERA_AF_STATE_FOCUSED ||
                                                    source->afState == KEEL_CAMERA_AF_STATE_FAILED);
    });
    *state = ended ? source->afState : static_cast<uint32_t>(KEEL_CAMERA_AF_STATE_SCANNING);
    return KEEL_OK;
}

void sourceStop(cam_source *base)
{
    Source *source = sourceOf(base);
    {
        std::lock_guard<std::mutex> guard(source->mutex);
        if (!source->running) {
            return;
        }
        source->running = false;
    }
    source->camera->stop();
}

void sourceFreeBuffers(cam_source *base)
{
    freeAll(sourceOf(base));
}

void sourceClose(cam_source *base)
{
    Source *source = sourceOf(base);
    sourceStop(base);
    freeAll(source);
    source->camera->release();
    source->camera.reset();
    delete source;
    managerRelease();
}

} // namespace

extern "C" {

const cam_source_ops cam_libcamera_source = {
    "libcamera",   sourceList,         sourceOpen, sourceConfigure, sourceStart, sourceRelease,
    sourceAutofocus, sourceStop, sourceFreeBuffers, sourceClose,
};

const char *cam_libcamera_version(void)
{
    static const std::string version = CameraManager::version();
    return version.c_str();
}

} // extern "C"
