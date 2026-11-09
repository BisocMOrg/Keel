# Keel demonstrator API

Both implementations serve the same offline browser assets. Default listen address is 127.0.0.1;
use `--host 0.0.0.0` on a trusted LAN. Python defaults to port 8787, Java to 8788.
Both accept `--mode mock|simulator|hardware`, `--allow-motion`, `--port`, `--host`,
`--web-root`, and `--token-file`. Mock is the default and is clearly identified.
Hardware uses the actual language binding; simulator uses libkeel's simulator.
Real motor arming requires --allow-motion. Maximum demo duty is 350 permille.

Every /api route requires Authorization: Bearer TOKEN. A token is loaded from the
specified file or KEEL_DEMO_TOKEN, or generated and printed at startup. Never send
tokens in URL query parameters. Reject cross-origin writes; no permissive CORS.
Static assets have no secrets. JSON errors have {"error":"useful explanation"}.
Reject invalid types, nonfinite numbers, out-of-range values and oversized bodies.

## State

GET /api/state returns this shape (unknown measurements use null):

```json
{
  "language":"Python", "mode":"mock", "serverTimeMs":12345,
  "motionAllowed":true, "maxDuty":350, "driveTimeoutMs":350,
  "controller":{"clientId":null,"expiresInMs":0},
  "robot":{"state":"DISARMED","left":0,"right":0,"motorEnabled":false,
    "rgb":[0,0,0,0,0,0],"buttonLeds":0,"buttons":0,"distanceMm":null,
    "fault":null,"capabilities":["RGB","MOTORS","RANGE"]},
  "loop":{"running":true,"cycles":0,"overruns":0,"maxGapMs":0,"maxStepMs":0},
  "camera":{"available":true,"state":"stopped","width":640,"height":480,
    "fps":0,"frames":0,"dropped":0,"bytes":0,"source":"mock","error":null},
  "system":{"uptimeS":0,"cpuTempC":null}
}
```

GET /api/ping returns {"serverReceiveMs":12345,"serverSendMs":12345} on the same
monotonic server clock as frame timestamps. The browser uses repeated low-RTT
samples to estimate clock offset and labels frame-to-display delay as estimated,
with uncertainty from half the synchronization RTT. Network RTT is separate.

## Commands

POST /api/command accepts {"type":"...","clientId":"browser UUID","seq":1,...}.
seq is a monotonically increasing integer per client. Reject stale motion commands so
an old drive request cannot restart motion after a newer stop. Authenticated
stop/disarm/estop always execute even with an older sequence, and current-owner
release is also honored; preserve the maximum prior watermark. A stop from another
client revokes the old owner's lease. Process commands
atomically with controller ownership and deadlines. Return {"ok":true} or
the photo result below; errors are JSON with meaningful 4xx/5xx status codes.

| type | additional fields | effect |
| --- | --- | --- |
| claim | | Acquire controller; competing active owner returns 409. |
| heartbeat | | Renew controller lease (1500 ms), never drive deadline. |
| release | | Stop/disarm and relinquish controller. |
| arm | | Explicit arming; does not start motion. |
| disarm, stop | | Immediately stop/disarm; any authenticated client may stop. |
| estop | | Latch emergency stop; any authenticated client may stop. |
| clearFault | | Explicit clear; no automatic rearm or drive. |
| drive | left, right: integer -350..350 | Only when armed; repeat while held, expires after 350 ms. Zero clears drive intent. |
| rgb | index: 0..5 or -1 for all; color: integer 0..16777215 | Set RGB LED(s). |
| buttonLeds | mask: integer 0..15 | Set four button indicators. |
| range | | Request ultrasonic measurement; report result in state. |
| cameraStart | | Start MJPEG acquisition at 640x480, target 20 fps. |
| cameraStop | | Stop acquisition, invalidate latest frame. |
| autofocus | | Trigger focus scan when camera supports it. |
| photo | | Capture a full-resolution JPEG; pause/restart stream on its owner thread. |

All other commands require controller ownership. Expired owner or drive deadline
stops/disarms independently of the HTTP thread and browser; no automatic rearming.
Use one managed board `RobotSession` and one separate camera owner thread. The
binding owns its internal control loop; application code does not tick the board.
Pass the original absolute command expiry into the session instead of extending
an old request by assigning a new lease when it is dispatched. HTTP handlers
must not call thread-affine native camera methods. Shutdown stops the hardware.
Camera failure must not stop the board loop. Report unavailable capabilities.

## Camera frames and photos

GET /api/camera/frame?after=N returns latest JPEG, or 204 if no newer frame.
Keep only the newest frame; slow viewers must not back up the acquisition thread.
Headers: X-Sequence, X-Captured-Ms, X-Sent-Ms, X-Timestamp-Basis,
X-Capture-Age-Ms. Times are milliseconds on the /api/ping clock. Convert native
timestamps properly; use basis `sensor` only for actual sensor timestamps,
otherwise `server-receipt`. Never mislabel receipt-to-display as sensor latency.
Responses are no-store. Native frame/chunk resources must be released promptly.

photo returns {"ok":true,"photo":"/api/photos/ID.jpg"}; GET that URL returns
image/jpeg with attachment filename. Keep a bounded number/size of captures.
Mock frames/photos are visibly marked synthetic. Mock never silently substitutes
for hardware errors and does not require native libraries.
