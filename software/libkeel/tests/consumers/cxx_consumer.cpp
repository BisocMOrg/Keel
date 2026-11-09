// SPDX-License-Identifier: MIT
// Copyright (c) 2026 BisocM
//
// A C++17 application that sees only <keel/keel.h> and the shared library. It
// wraps the handle in an RAII class whose destructor closes without throwing,
// turns statuses into exceptions by class (never by individual code), runs the
// control loop on the main thread and exercises "stop from anywhere, go only
// from the loop" from a second std::thread.
#include <keel/keel.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

// A status that is not KEEL_OK, with its class kept for dispatch.
class KeelError : public std::runtime_error {
public:
    explicit KeelError(int32_t status)
        : std::runtime_error(std::string(keel_status_name(status)) + ": " + keel_status_string(status)),
          status_(status),
          class_(keel_status_class(status))
    {
    }
    int32_t status() const { return status_; }
    uint32_t status_class() const { return class_; }

private:
    int32_t status_;
    uint32_t class_;
};

void check(int32_t status)
{
    if (status != KEEL_OK) {
        throw KeelError(status);
    }
}

template <typename T>
T sized()
{
    T value{};
    value.struct_size = sizeof value;
    return value;
}

// Owns one device handle. Copying is forbidden; closing happens exactly once.
class Device {
public:
    explicit Device(uint32_t flags)
    {
        keel_open_options options = sized<keel_open_options>();
        options.backend = KEEL_BACKEND_SIMULATOR;
        options.flags = flags;
        keel_open_report report = sized<keel_open_report>();
        int32_t status = keel_open(&options, &handle_, &report);
        if (status != KEEL_OK) {
            // Every failed open is a setup error carrying the report.
            throw std::runtime_error(std::string("keel_open: ") + keel_status_name(status) + ": " +
                                     report.hint);
        }
    }
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;
    // The close status is diagnostic only: report it, never throw from here.
    ~Device()
    {
        int32_t status = keel_close(handle_, nullptr);
        if (status != KEEL_OK) {
            std::fprintf(stderr, "cxx_consumer: keel_close: %s\n", keel_status_name(status));
        }
    }
    keel_device *get() const { return handle_; }

private:
    keel_device *handle_ = nullptr;
};

// The bindings' loop: one keel_cycle per 10 ms on the device clock.
class Loop {
public:
    explicit Loop(keel_device *device) : device_(device) { check(keel_now_us(device_, &next_)); }
    keel_snapshot cycle(uint32_t command = KEEL_CYCLE_NONE, int32_t left = 0, int32_t right = 0)
    {
        keel_cycle_input in = sized<keel_cycle_input>();
        in.health = KEEL_HEALTH_ALL;
        in.cycle_id = ++cycle_id_;
        in.command = command;
        in.left = left;
        in.right = right;
        keel_snapshot out = sized<keel_snapshot>();
        int32_t status = keel_cycle(device_, &in, &out);
        next_ += static_cast<uint64_t>(KEEL_TICK_PERIOD_MS) * 1000u;
        check(status);
        check(keel_sleep_until(device_, next_));
        return out;
    }

private:
    keel_device *device_;
    uint64_t next_ = 0;
    uint64_t cycle_id_ = 0;
};

void expect(bool condition, const char *what)
{
    if (!condition) {
        throw std::logic_error(what);
    }
}

void run()
{
    Device device(KEEL_OPEN_VIRTUAL_CLOCK);
    Loop loop(device.get());

    check(keel_request_arm(device.get()));
    keel_snapshot snapshot = loop.cycle();
    for (int i = 0; i < 200 && snapshot.state != KEEL_STATE_ARMED; ++i) {
        snapshot = loop.cycle();
    }
    expect(snapshot.state == KEEL_STATE_ARMED, "the simulator did not arm");
    snapshot = loop.cycle(KEEL_CYCLE_DRIVE, 400, -400);
    expect(snapshot.output_left == 400 && snapshot.output_right == -400, "the command was not applied");

    // Another thread may stop the motors, but may not drive them.
    int32_t foreign_go = KEEL_OK;
    int32_t foreign_stop = KEEL_E_INTERNAL;
    std::thread other([&] {
        foreign_go = keel_set_motors(device.get(), 100, 100);
        foreign_stop = keel_set_motors(device.get(), 0, 0);
    });
    other.join();
    expect(foreign_go == KEEL_E_CONFLICT, "a foreign thread could drive");
    expect(keel_status_class(foreign_go) == KEEL_CLASS_REJECTED, "CONFLICT is not REJECTED");
    expect(foreign_stop == KEEL_OK, "a foreign thread could not coast");
    snapshot = loop.cycle();
    expect(snapshot.state == KEEL_STATE_ARMED && snapshot.output_left == 0, "the coast did not hold");

    // A fault surfaces as an exception of the FAULT class from the loop.
    std::thread stopper([&] { check(keel_emergency_stop(device.get())); });
    stopper.join();
    try {
        loop.cycle();
        expect(false, "a cycle after an emergency stop succeeded");
    } catch (const KeelError &error) {
        expect(error.status() == KEEL_E_FAULTED, "unexpected status after the emergency stop");
        expect(error.status_class() == KEEL_CLASS_FAULT, "FAULTED is not FAULT class");
    }
    keel_fault fault = sized<keel_fault>();
    check(keel_get_fault(device.get(), &fault));
    expect(fault.cause == KEEL_CAUSE_USER_STOP, "the fault record lost the cause");
    std::printf("cxx_consumer: armed, drove, stopped from another thread (%s), closing\n",
                keel_fault_cause_name(fault.cause));
}

} // namespace

int main()
{
    try {
        run();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "cxx_consumer: %s\n", error.what());
        return 1;
    }
    return 0;
}
