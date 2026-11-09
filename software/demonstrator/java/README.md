# Java demonstrator

This is a separate Java implementation of the shared [demonstrator HTTP API](../API.md).
It uses JDK 25's HTTP server and Keel's Java FFM binding, with no third-party Java runtime dependencies.
The camera and robot have separate owner threads. Mock mode is explicit and works without Linux/native libraries.

From the repository root:

```sh
python3 software/demonstrator/java/build.py --test
java --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true \
  -jar software/demonstrator/java/build/keel-demo.jar \
  --mode mock --web-root software/demonstrator/web/dist
```

On the Pi, install `libkeel1` and `libkeel-camera1` and grant the account the groups and realtime limits
described in the main Keel deployment guide. Serve the dashboard over the LAN:

```sh
java --enable-native-access=ALL-UNNAMED -Djava.awt.headless=true \
  -jar java/build/keel-demo.jar --mode hardware --host 0.0.0.0 --port 8788 \
  --web-root web/dist --token-file /path/to/demo-token
```

Add `--allow-motion` when ready to allow explicit motor arming. The server never arms automatically.
Without it, LEDs, ranging and camera work, and motor arming is refused. Only one server can own the board/camera
at a time. `--mode simulator` uses libkeel's simulator, not the physical board; native camera availability
depends on the installed camera backend. Use mock for a guaranteed hardware-free demonstration.

The token file contains a random access token of at least 16 characters. `KEEL_DEMO_TOKEN` is another option.
If neither is configured, startup prints a newly generated token. Paste it into the dashboard; tokens never go in URLs.
Use a trusted LAN or an SSH tunnel: this HTTP example does not provide TLS.

Camera frames carry timestamps mapped from native `CLOCK_MONOTONIC` into the server clock using an explicit
`clock_gettime` bridge. Invalid/unmappable timestamps fall back to `server-receipt`, which the UI must label as
receipt-to-display rather than sensor-to-display delay. Camera Module 2 has no autofocus. Full-resolution still
capture briefly pauses and then restores an active video stream. At most eight photos / 48 MiB are retained.

`build.py --test` exercises authentication, origin validation, malformed JSON, motor lease expiry independent of
heartbeat, stale command rejection after stop, emergency stops, photos and frame invalidation on a mock backend.
The build bundles the existing binding sources; it never modifies generated bindings or requires Maven.
