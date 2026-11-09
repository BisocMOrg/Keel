# Python demonstrator

This is an independent Python server using the repository's `keel` and
`keel.camera` bindings. The HTTP server and explicit mock mode need only Python
3.11 or newer; native mode needs the binding's supported Python and Linux.
See [the shared API](../API.md) and [demonstrator instructions](../README.md).

```sh
python3 keel_demo.py --mode mock
python3 keel_demo.py --mode hardware --host 0.0.0.0 --token-file ~/.config/keel-demo.token
```

Add `--allow-motion` only when ready to allow deliberate arming from the browser.
Without it real LEDs, ranging and camera functions still work. The default web
root is `../web/dist`; build the shared frontend first. The token is printed only
when the server generates it; configured secrets are not logged. Both versions
can run simultaneously in mock mode, but only one may own the real robot/camera.

The binding's `RobotSession` owns the hardware loop and enforces each finite drive
lease. The application retains its 350 ms drive-command deadline and 1500 ms
controller lease, passing the original command expiry to the session. HTTP
traffic, camera encoding and photo capture do not execute on the hardware
thread. Arm requests are revalidated against lease/generation when executed,
so a queued request cannot undo a newer stop. The app creates no `ControlLoop`.

When run from the deployment bundle, its matching Python binding source takes
priority over an older system-installed package. No system package replacement
is required to use the deployed session API.

The camera runs on its own owner thread. It keeps a single latest MJPEG frame;
slow browsers skip old frames. Still capture pauses and resumes streaming. Four
photos, at most 32 MiB in total, remain available until eviction/restart. Native
frame timestamps are start-of-exposure CLOCK_MONOTONIC; Python uses that same
clock for `/api/ping`. Invalid timestamps are explicitly marked server-receipt.
Native hardware errors are reported, never replaced with synthetic frames.

HTTP connections use `TCP_NODELAY`. Responses write headers and bodies separately,
so leaving Nagle enabled can make small JSON replies wait for a delayed TCP
acknowledgement on persistent connections. This also biases the browser's clock
synchronization. Transport tests verify the accepted socket option and response
framing across repeated JSON, binary, HEAD and empty responses on one connection.

Run the portable safety/HTTP tests with:

```sh
python3 -m unittest discover -s tests -v
```

The bundled mock JPEG is a labelled synthetic test card. Native mode requires no
Pillow, NumPy, camera helper process or Python web framework.
