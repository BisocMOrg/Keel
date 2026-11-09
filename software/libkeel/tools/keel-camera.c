/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 BisocM */
/*
 * keel-camera: the camera from the command line (SPEC section 26.10). It
 * lists cameras, takes stills, records, streams and writes raw frames to
 * standard output, through libkeel-camera only (keel/camera.h).
 *
 * SPEC refers to the libkeel specification, software/libkeel/docs/spec.md.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "keel/camera.h"

enum {
    EXIT_RUNTIME = 1,
    EXIT_USAGE = 2,
    EXIT_NOT_FOUND = 3,
    EXIT_IN_USE = 4,
    EXIT_PERMISSION = 5,
    EXIT_UNSUPPORTED = 6,
    POLL_MS = 200
};

static volatile sig_atomic_t stop_signal;

static void on_signal(int signal_number)
{
    stop_signal = signal_number;
}

static const char usage_text[] =
    "Usage: keel-camera COMMAND [OPTIONS]\n"
    "\n"
    "Commands:\n"
    "  list                 list the cameras, their sensor modes and autofocus\n"
    "  still -o FILE.jpg    capture one JPEG image (default: full sensor resolution)\n"
    "  record -o FILE       record video to FILE.h264 or FILE.mjpeg (and, when the\n"
    "                       library has libavformat, .mp4, .mkv, .mov or .ts)\n"
    "  stream DESTINATION   stream to tcp://HOST:PORT, tcp://:PORT?listen,\n"
    "                       udp://HOST:PORT, a file, an rtsp://, rtmp:// or srt:// URL\n"
    "                       (with libavformat), or - for standard output\n"
    "  frames               write raw frames to standard output, rows unpadded\n"
    "\n"
    "Camera options:\n"
    "  -c, --camera N         camera index (default 0; see list)\n"
    "  -W, --width W          frame width (default 640; still: full resolution)\n"
    "  -H, --height H         frame height (default 480; still: full resolution)\n"
    "  -f, --framerate FPS    frames per second, decimals allowed (default 30)\n"
    "      --hflip, --vflip   mirror the image\n"
    "      --rotation DEG     0 or 180\n"
    "      --autofocus MODE   default, manual, auto or continuous (Camera Module 3)\n"
    "      --af-range RANGE   normal, macro or full\n"
    "      --lens-position D  lens position in dioptres with --autofocus manual\n"
    "                         (0 = infinity, 1 = 1 m)\n"
    "      --exposure-us US   fixed exposure time in microseconds\n"
    "      --gain G           fixed analogue gain, e.g. 2.5\n"
    "      --ev EV            exposure compensation, -8..8\n"
    "      --awb MODE         auto, incandescent, tungsten, fluorescent, indoor,\n"
    "                         daylight or cloudy\n"
    "      --awb-gains R,B    fixed red and blue colour gains, e.g. 1.6,1.9\n"
    "\n"
    "Encoding (record, stream):\n"
    "      --codec CODEC      h264 (default) or mjpeg\n"
    "      --bitrate BPS      H.264 bits per second (default 0.15 bit per pixel)\n"
    "      --intra N          H.264 frames between keyframes (default: 1 s)\n"
    "      --profile P        H.264 baseline, main or high (default)\n"
    "      --inline           H.264 parameter sets before every keyframe on stdout\n"
    "  -q, --quality Q        JPEG quality 1..100 (MJPEG and still; default 85)\n"
    "      --max-clients N    clients of tcp://:PORT?listen (default 4)\n"
    "\n"
    "Other options:\n"
    "  -o, --output FILE      file to write (still, record)\n"
    "  -t, --duration SEC     stop after SEC seconds (default: until Ctrl-C)\n"
    "  -n, --count N          frames: stop after N frames\n"
    "      --format F         frames: i420 (default), rgb888 or bgr888\n"
    "      --json             list: JSON instead of text\n"
    "  -h, --help             show this text\n"
    "      --version          show the library version\n"
    "\n"
    "Exit status:\n"
    "  0  done (also when stopped with Ctrl-C or SIGTERM)\n"
    "  1  the camera, the encoder or the destination failed while running\n"
    "  2  usage error\n"
    "  3  no such camera, encoder or destination\n"
    "  4  the camera or the port is in use by another program\n"
    "  5  permission denied: sudo keel-setup --user NAME, then log in again\n"
    "  6  not supported by this camera or this build\n"
    "\n"
    "Examples:\n"
    "  keel-camera still -o photo.jpg\n"
    "  keel-camera record -t 10 -o clip.h264\n"
    "  keel-camera stream tcp://:5000?listen     (then: ffplay tcp://ROBOT:5000)\n"
    "  keel-camera frames --format bgr888 -n 100 | python3 my_model.py\n";

typedef struct settings {
    const char *command;
    keel_camera_options camera;
    keel_camera_encoder_options encoder;
    keel_camera_output_options output;
    const char *path;
    const char *destination;
    double duration;
    long count;
    int json;
    int size_given;
} settings;

static int usage_error(const char *format, const char *value)
{
    fprintf(stderr, "keel-camera: ");
    fprintf(stderr, format, value);
    fprintf(stderr, "\nTry 'keel-camera --help'.\n");
    return EXIT_USAGE;
}

static int exit_for(int32_t status)
{
    switch (status) {
    case KEEL_OK:
        return 0;
    case KEEL_E_INVALID_ARGUMENT:
        return EXIT_USAGE;
    case KEEL_E_NOT_FOUND:
        return EXIT_NOT_FOUND;
    case KEEL_E_IN_USE:
        return EXIT_IN_USE;
    case KEEL_E_PERMISSION:
        return EXIT_PERMISSION;
    case KEEL_E_UNSUPPORTED:
        return EXIT_UNSUPPORTED;
    default:
        return EXIT_RUNTIME;
    }
}

static int report(int32_t status, const keel_camera_error *error, const char *fallback)
{
    fprintf(stderr, "keel-camera: %s\n",
            error != NULL && error->message[0] != '\0' ? error->message : fallback);
    return exit_for(status);
}

static int report_handle(keel_camera *camera, int32_t status, const char *fallback)
{
    keel_camera_error error;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    if (keel_camera_last_error(camera, &error) != KEEL_OK || error.status != status) {
        error.message[0] = '\0';
    }
    return report(status, &error, fallback);
}

static int parse_long(const char *text, long minimum, long maximum, long *out)
{
    char *end;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum) {
        return 0;
    }
    *out = value;
    return 1;
}

/* A decimal number scaled by 1000 ("2.5" gives 2500). */
static int parse_milli(const char *text, double minimum, double maximum, long *out)
{
    char *end;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !(value >= minimum && value <= maximum)) {
        return 0;
    }
    *out = (long)(value * 1000.0 + (value >= 0 ? 0.5 : -0.5));
    return 1;
}

static int lookup(const char *text, const char *const *names, unsigned count, uint32_t *out)
{
    unsigned i;
    for (i = 0; i < count; ++i) {
        if (strcmp(text, names[i]) == 0) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

enum {
    OPT_HFLIP = 256, OPT_VFLIP, OPT_ROTATION, OPT_AUTOFOCUS, OPT_AF_RANGE, OPT_LENS, OPT_EXPOSURE, OPT_GAIN,
    OPT_EV, OPT_AWB, OPT_AWB_GAINS, OPT_CODEC, OPT_BITRATE, OPT_INTRA, OPT_PROFILE, OPT_INLINE, OPT_MAX_CLIENTS,
    OPT_FORMAT, OPT_JSON, OPT_VERSION
};

static int parse(int argc, char **argv, settings *s)
{
    static const struct option options[] = {
        {"camera", required_argument, NULL, 'c'},       {"width", required_argument, NULL, 'W'},
        {"height", required_argument, NULL, 'H'},       {"framerate", required_argument, NULL, 'f'},
        {"hflip", no_argument, NULL, OPT_HFLIP},        {"vflip", no_argument, NULL, OPT_VFLIP},
        {"rotation", required_argument, NULL, OPT_ROTATION},
        {"autofocus", required_argument, NULL, OPT_AUTOFOCUS},
        {"af-range", required_argument, NULL, OPT_AF_RANGE},
        {"lens-position", required_argument, NULL, OPT_LENS},
        {"exposure-us", required_argument, NULL, OPT_EXPOSURE},
        {"gain", required_argument, NULL, OPT_GAIN},    {"ev", required_argument, NULL, OPT_EV},
        {"awb", required_argument, NULL, OPT_AWB},      {"awb-gains", required_argument, NULL, OPT_AWB_GAINS},
        {"codec", required_argument, NULL, OPT_CODEC},  {"bitrate", required_argument, NULL, OPT_BITRATE},
        {"intra", required_argument, NULL, OPT_INTRA},  {"profile", required_argument, NULL, OPT_PROFILE},
        {"inline", no_argument, NULL, OPT_INLINE},      {"quality", required_argument, NULL, 'q'},
        {"max-clients", required_argument, NULL, OPT_MAX_CLIENTS},
        {"output", required_argument, NULL, 'o'},       {"duration", required_argument, NULL, 't'},
        {"count", required_argument, NULL, 'n'},        {"format", required_argument, NULL, OPT_FORMAT},
        {"json", no_argument, NULL, OPT_JSON},          {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, OPT_VERSION},    {NULL, 0, NULL, 0},
    };
    static const char *const af_modes[] = {"default", "manual", "auto", "continuous"};
    static const char *const af_ranges[] = {"default", "normal", "macro", "full"};
    static const char *const awb_modes[] = {"auto", "incandescent", "tungsten", "fluorescent", "indoor",
                                            "daylight", "cloudy"};
    static const char *const codecs[] = {"none", "h264", "mjpeg"};
    static const char *const profiles[] = {"default", "baseline", "main", "high"};
    static const char *const formats[] = {"none", "i420", "rgb888", "bgr888"};
    long value;
    long red;
    int option;
    uint32_t choice;
    memset(s, 0, sizeof(*s));
    s->camera.struct_size = sizeof(s->camera);
    s->encoder.struct_size = sizeof(s->encoder);
    s->encoder.codec = KEEL_CAMERA_CODEC_H264;
    s->output.struct_size = sizeof(s->output);
    s->count = -1;
    while ((option = getopt_long(argc, argv, "c:W:H:f:q:o:t:n:h", options, NULL)) != -1) {
        switch (option) {
        case 'c':
            if (!parse_long(optarg, 0, 15, &value)) {
                return usage_error("--camera needs an index 0..15, not '%s'", optarg);
            }
            s->camera.index = (uint32_t)value;
            break;
        case 'W':
        case 'H':
            if (!parse_long(optarg, KEEL_CAMERA_SIZE_MIN, KEEL_CAMERA_SIZE_MAX, &value) || (value & 1)) {
                return usage_error("the width and height must be even numbers 64..16384, not '%s'", optarg);
            }
            *(option == 'W' ? &s->camera.width : &s->camera.height) = (uint32_t)value;
            s->size_given = 1;
            break;
        case 'f':
            if (!parse_milli(optarg, 1, 240, &value)) {
                return usage_error("--framerate needs 1..240 frames per second, not '%s'", optarg);
            }
            s->camera.framerate_milli = (uint32_t)value;
            break;
        case OPT_HFLIP:
            s->camera.flags |= KEEL_CAMERA_HFLIP;
            break;
        case OPT_VFLIP:
            s->camera.flags |= KEEL_CAMERA_VFLIP;
            break;
        case OPT_ROTATION:
            if (strcmp(optarg, "0") != 0 && strcmp(optarg, "180") != 0) {
                return usage_error("--rotation must be 0 or 180, not '%s'", optarg);
            }
            s->camera.rotation = (uint32_t)atoi(optarg);
            break;
        case OPT_AUTOFOCUS:
            if (!lookup(optarg, af_modes, 4, &s->camera.af_mode)) {
                return usage_error("--autofocus must be default, manual, auto or continuous, not '%s'", optarg);
            }
            break;
        case OPT_AF_RANGE:
            if (!lookup(optarg, af_ranges, 4, &s->camera.af_range) || s->camera.af_range == 0) {
                return usage_error("--af-range must be normal, macro or full, not '%s'", optarg);
            }
            break;
        case OPT_LENS:
            if (!parse_milli(optarg, 0, 32, &value)) {
                return usage_error("--lens-position needs 0..32 dioptres, not '%s'", optarg);
            }
            s->camera.lens_position_milli = (uint32_t)value;
            break;
        case OPT_EXPOSURE:
            if (!parse_long(optarg, 1, 1000000, &value)) {
                return usage_error("--exposure-us needs 1..1000000, not '%s'", optarg);
            }
            s->camera.exposure_us = (uint32_t)value;
            break;
        case OPT_GAIN:
            if (!parse_milli(optarg, 1, 64, &value)) {
                return usage_error("--gain needs 1..64, not '%s'", optarg);
            }
            s->camera.gain_milli = (uint32_t)value;
            break;
        case OPT_EV:
            if (!parse_milli(optarg, -8, 8, &value)) {
                return usage_error("--ev needs -8..8, not '%s'", optarg);
            }
            s->camera.ev_milli = (int32_t)value;
            break;
        case OPT_AWB:
            if (!lookup(optarg, awb_modes, 7, &s->camera.awb_mode)) {
                return usage_error("--awb needs a mode such as auto or daylight, not '%s'", optarg);
            }
            break;
        case OPT_AWB_GAINS: {
            char red_text[32];
            const char *comma = strchr(optarg, ',');
            if (comma == NULL || (size_t)(comma - optarg) >= sizeof(red_text)) {
                return usage_error("--awb-gains needs R,B such as 1.6,1.9, not '%s'", optarg);
            }
            memcpy(red_text, optarg, (size_t)(comma - optarg));
            red_text[comma - optarg] = '\0';
            if (!parse_milli(red_text, 0.001, 32, &red) || !parse_milli(comma + 1, 0.001, 32, &value)) {
                return usage_error("--awb-gains needs R,B such as 1.6,1.9, not '%s'", optarg);
            }
            s->camera.awb_mode = KEEL_CAMERA_AWB_MANUAL;
            s->camera.red_gain_milli = (uint32_t)red;
            s->camera.blue_gain_milli = (uint32_t)value;
            break;
        }
        case OPT_CODEC:
            if (!lookup(optarg, codecs, 3, &choice) || choice == 0) {
                return usage_error("--codec must be h264 or mjpeg, not '%s'", optarg);
            }
            s->encoder.codec = choice;
            break;
        case OPT_BITRATE:
            if (!parse_long(optarg, KEEL_CAMERA_BITRATE_MIN, KEEL_CAMERA_BITRATE_MAX, &value)) {
                return usage_error("--bitrate needs 25000..25000000 bits per second, not '%s'", optarg);
            }
            s->encoder.bitrate = (uint32_t)value;
            break;
        case OPT_INTRA:
            if (!parse_long(optarg, 1, 1000, &value)) {
                return usage_error("--intra needs 1..1000 frames, not '%s'", optarg);
            }
            s->encoder.intra_period = (uint32_t)value;
            break;
        case OPT_PROFILE:
            if (!lookup(optarg, profiles, 4, &s->encoder.profile) || s->encoder.profile == 0) {
                return usage_error("--profile must be baseline, main or high, not '%s'", optarg);
            }
            break;
        case OPT_INLINE:
            s->encoder.flags |= KEEL_CAMERA_INLINE_HEADERS;
            break;
        case 'q':
            if (!parse_long(optarg, 1, 100, &value)) {
                return usage_error("--quality needs 1..100, not '%s'", optarg);
            }
            s->encoder.quality = (uint32_t)value;
            break;
        case OPT_MAX_CLIENTS:
            if (!parse_long(optarg, 1, KEEL_CAMERA_CLIENTS_MAX, &value)) {
                return usage_error("--max-clients needs 1..16, not '%s'", optarg);
            }
            s->output.max_clients = (uint32_t)value;
            break;
        case 'o':
            s->path = optarg;
            break;
        case 't': {
            char *end;
            s->duration = strtod(optarg, &end);
            if (end == optarg || *end != '\0' || !(s->duration > 0 && s->duration <= 86400)) {
                return usage_error("--duration needs seconds, 0..86400, not '%s'", optarg);
            }
            break;
        }
        case 'n':
            if (!parse_long(optarg, 1, 1000000000L, &s->count)) {
                return usage_error("--count needs a positive number, not '%s'", optarg);
            }
            break;
        case OPT_FORMAT:
            if (!lookup(optarg, formats, 4, &s->camera.format) || s->camera.format == 0) {
                return usage_error("--format must be i420, rgb888 or bgr888, not '%s'", optarg);
            }
            break;
        case OPT_JSON:
            s->json = 1;
            break;
        case 'h':
            fputs(usage_text, stdout);
            exit(0);
        case OPT_VERSION:
            printf("keel-camera %s\n", keel_camera_version_string());
            exit(0);
        default:
            fprintf(stderr, "Try 'keel-camera --help'.\n");
            return EXIT_USAGE;
        }
    }
    if (optind >= argc) {
        return usage_error("%s", "name a command: list, still, record, stream or frames");
    }
    s->command = argv[optind++];
    if (strcmp(s->command, "stream") == 0) {
        if (optind >= argc) {
            return usage_error("%s", "stream needs a destination, for example tcp://:5000?listen");
        }
        s->destination = argv[optind++];
    }
    if (optind < argc) {
        return usage_error("unexpected argument '%s'", argv[optind]);
    }
    if (s->encoder.codec == KEEL_CAMERA_CODEC_MJPEG &&
        (s->encoder.bitrate != 0 || s->encoder.intra_period != 0 || s->encoder.profile != 0 || s->encoder.flags)) {
        return usage_error("%s", "--bitrate, --intra, --profile and --inline apply to h264");
    }
    if (s->encoder.codec == KEEL_CAMERA_CODEC_H264 && s->encoder.quality != 0 && strcmp(s->command, "still") != 0) {
        return usage_error("%s", "--quality applies to mjpeg and still");
    }
    if (s->camera.lens_position_milli != 0 && s->camera.af_mode != KEEL_CAMERA_AF_MANUAL) {
        return usage_error("%s", "--lens-position needs --autofocus manual");
    }
    return 0;
}

static void install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

static double now_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* ---- list --------------------------------------------------------------- */

static void json_string(const char *text)
{
    putchar('"');
    for (; *text != '\0'; ++text) {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static int command_list(const settings *s)
{
    keel_camera_error error;
    keel_camera_list *list;
    uint32_t count = 0;
    uint32_t i;
    int32_t rc;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    rc = keel_camera_list_open(&list, &error);
    if (rc != KEEL_OK) {
        return report(rc, &error, "listing the cameras failed");
    }
    keel_camera_list_count(list, &count);
    if (s->json) {
        printf("{\"cameras\": [");
    } else if (count == 0) {
        printf("No camera detected: with the power off, check the ribbon cable at the Raspberry Pi's camera "
               "connector, and that config.txt enables the camera (camera_auto_detect=1, or dtoverlay=imx219 or "
               "dtoverlay=imx708).\n");
    }
    for (i = 0; i < count; ++i) {
        keel_camera_info info;
        uint32_t m;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        keel_camera_list_get(list, i, &info);
        if (s->json) {
            printf("%s{\"index\": %u, \"model\": ", i > 0 ? ", " : "", info.index);
            json_string(info.model);
            printf(", \"sensor\": ");
            json_string(info.sensor);
            printf(", \"id\": ");
            json_string(info.id);
            printf(", \"autofocus\": %s, \"in_use\": %s, \"width\": %u, \"height\": %u, \"modes\": [",
                   (info.flags & KEEL_CAMERA_INFO_AUTOFOCUS) ? "true" : "false",
                   (info.flags & KEEL_CAMERA_INFO_IN_USE) ? "true" : "false", info.pixel_array_width,
                   info.pixel_array_height);
        } else {
            printf("%u: %s (%s), %s%s\n   id %s\n   full resolution %ux%u\n", info.index, info.model, info.sensor,
                   (info.flags & KEEL_CAMERA_INFO_AUTOFOCUS) ? "autofocus" : "fixed focus",
                   (info.flags & KEEL_CAMERA_INFO_IN_USE) ? ", in use by another program" : "", info.id,
                   info.pixel_array_width, info.pixel_array_height);
        }
        for (m = 0; m < info.mode_count; ++m) {
            keel_camera_mode mode;
            memset(&mode, 0, sizeof(mode));
            mode.struct_size = sizeof(mode);
            keel_camera_list_get_mode(list, i, m, &mode);
            if (s->json) {
                printf("%s{\"width\": %u, \"height\": %u, \"bit_depth\": %u, \"max_fps\": %.2f, "
                       "\"crop\": [%u, %u, %u, %u]}",
                       m > 0 ? ", " : "", mode.width, mode.height, mode.bit_depth,
                       mode.max_framerate_milli / 1000.0, mode.crop_x, mode.crop_y, mode.crop_width,
                       mode.crop_height);
            } else if (mode.max_framerate_milli != 0) {
                printf("   %s %ux%u %u-bit, up to %.2f fps, crop (%u, %u) %ux%u\n", m == 0 ? "modes" : "     ",
                       mode.width, mode.height, mode.bit_depth, mode.max_framerate_milli / 1000.0, mode.crop_x,
                       mode.crop_y, mode.crop_width, mode.crop_height);
            } else {
                printf("   %s %ux%u %u-bit\n", m == 0 ? "modes" : "     ", mode.width, mode.height,
                       mode.bit_depth);
            }
        }
        if (s->json) {
            printf("]}");
        }
    }
    if (s->json) {
        printf("]}\n");
    }
    keel_camera_list_close(list);
    return 0;
}

/* ---- capture commands --------------------------------------------------- */

static int open_camera(const settings *s, keel_camera **camera)
{
    keel_camera_error error;
    int32_t rc;
    memset(&error, 0, sizeof(error));
    error.struct_size = sizeof(error);
    rc = keel_camera_open(&s->camera, camera, &error);
    if (rc != KEEL_OK) {
        return report(rc, &error, "the camera could not be opened");
    }
    return 0;
}

/* Waits for the duration or a signal; 0, or an exit status when the
 * capture or the output failed. */
static int run_until_stopped(keel_camera *camera, const settings *s, uint32_t output)
{
    double end = s->duration > 0 ? now_seconds() + s->duration : 0;
    while (!stop_signal && (end == 0 || now_seconds() < end)) {
        keel_camera_stats stats;
        struct timespec pause = {0, POLL_MS * 1000000L};
        memset(&stats, 0, sizeof(stats));
        stats.struct_size = sizeof(stats);
        keel_camera_get_stats(camera, &stats);
        if (stats.state == KEEL_CAMERA_STATE_FAILED) {
            keel_camera_error error;
            memset(&error, 0, sizeof(error));
            error.struct_size = sizeof(error);
            keel_camera_last_error(camera, &error);
            fprintf(stderr, "keel-camera: %s\n", error.message[0] != '\0' ? error.message : "the camera failed");
            return EXIT_RUNTIME;
        }
        if (output != 0) {
            keel_camera_output_stats out;
            memset(&out, 0, sizeof(out));
            out.struct_size = sizeof(out);
            if (keel_camera_get_output_stats(camera, output, &out) == KEEL_OK &&
                out.state == KEEL_CAMERA_OUTPUT_FAILED) {
                fprintf(stderr, "keel-camera: %s\n", out.message[0] != '\0' ? out.message : "the output failed");
                return EXIT_RUNTIME;
            }
        }
        nanosleep(&pause, NULL);
    }
    return 0;
}

static int command_still(settings *s)
{
    keel_camera_still_options still;
    keel_camera_still_info info;
    keel_camera *camera;
    int32_t rc;
    int status;
    if (s->path == NULL) {
        return usage_error("%s", "still needs -o FILE.jpg");
    }
    memset(&still, 0, sizeof(still));
    still.struct_size = sizeof(still);
    still.width = s->camera.width;
    still.height = s->camera.height;
    still.quality = s->encoder.quality;
    if ((still.width == 0) != (still.height == 0)) {
        return usage_error("%s", "give both --width and --height, or neither for full resolution");
    }
    /* The video stream of the handle is not used: keep it small. */
    s->camera.width = 0;
    s->camera.height = 0;
    status = open_camera(s, &camera);
    if (status != 0) {
        return status;
    }
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    rc = keel_camera_capture_still_file(camera, &still, s->path, &info);
    if (rc == KEEL_OK) {
        fprintf(stderr, "keel-camera: %ux%u still, %u bytes, written to %s%s\n", info.width, info.height,
                info.bytes, s->path, (info.flags & KEEL_CAMERA_STILL_SOFTWARE) ? " (libjpeg)" : "");
        status = 0;
    } else {
        status = report_handle(camera, rc, "the still failed");
    }
    keel_camera_close(camera);
    return status;
}

static int start_encoder(keel_camera *camera, const settings *s)
{
    int32_t rc = keel_camera_set_encoder(camera, &s->encoder);
    if (rc != KEEL_OK) {
        return report_handle(camera, rc, "the encoder could not be set up");
    }
    return 0;
}

/* record and stream to a destination the library writes. */
static int command_output(const settings *s, const char *destination)
{
    keel_camera_output_stats stats;
    keel_camera *camera;
    uint32_t output = 0;
    int32_t rc;
    int status = open_camera(s, &camera);
    if (status != 0) {
        return status;
    }
    status = start_encoder(camera, s);
    if (status == 0) {
        rc = keel_camera_output_start(camera, destination, &s->output, &output);
        if (rc != KEEL_OK) {
            status = report_handle(camera, rc, "the output could not be started");
        }
    }
    if (status == 0) {
        rc = keel_camera_start(camera);
        if (rc != KEEL_OK) {
            status = report_handle(camera, rc, "the camera could not start");
        }
    }
    if (status == 0) {
        if (strstr(destination, "?listen") != NULL) {
            fprintf(stderr, "keel-camera: waiting for receivers on %s, e.g. ffplay tcp://ROBOT:PORT or VLC\n",
                    destination);
        } else {
            fprintf(stderr, "keel-camera: writing to %s; Ctrl-C stops\n", destination);
        }
        status = run_until_stopped(camera, s, output);
        keel_camera_stop(camera);
        memset(&stats, 0, sizeof(stats));
        stats.struct_size = sizeof(stats);
        keel_camera_get_output_stats(camera, output, &stats);
        rc = keel_camera_output_stop(camera, output);
        if (rc != KEEL_OK && status == 0) {
            status = report_handle(camera, rc, "the output did not finish cleanly");
        }
        fprintf(stderr, "keel-camera: %llu chunks, %llu bytes written, %llu dropped\n",
                (unsigned long long)stats.chunks, (unsigned long long)stats.bytes,
                (unsigned long long)stats.chunks_dropped);
    }
    keel_camera_close(camera);
    return status;
}

static int write_all(const uint8_t *data, size_t bytes)
{
    while (bytes > 0) {
        ssize_t written = write(STDOUT_FILENO, data, bytes);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        data += written;
        bytes -= (size_t)written;
    }
    return 0;
}

/* Encoded chunks to standard output. */
static int command_stdout(const settings *s)
{
    keel_camera_stream_info stream;
    keel_camera_encoded_info info;
    keel_camera *camera;
    uint8_t *buffer = NULL;
    double end = s->duration > 0 ? now_seconds() + s->duration : 0;
    int status;
    int32_t rc;
    if (isatty(STDOUT_FILENO)) {
        return usage_error("%s", "standard output is a terminal: pipe it, e.g. keel-camera stream - | ffplay -");
    }
    status = open_camera(s, &camera);
    if (status != 0) {
        return status;
    }
    status = start_encoder(camera, s);
    memset(&stream, 0, sizeof(stream));
    stream.struct_size = sizeof(stream);
    keel_camera_get_stream(camera, &stream);
    if (status == 0) {
        buffer = malloc(stream.encoded_bytes_max);
        rc = buffer == NULL ? KEEL_E_NO_MEMORY : keel_camera_start(camera);
        if (rc != KEEL_OK) {
            status = report_handle(camera, rc, "the camera could not start");
        }
    }
    while (status == 0 && !stop_signal && (end == 0 || now_seconds() < end)) {
        int written;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        rc = keel_camera_read_encoded(camera, buffer, stream.encoded_bytes_max, POLL_MS, &info);
        if (rc == KEEL_E_AGAIN) {
            continue;
        }
        if (rc != KEEL_OK) {
            status = report_handle(camera, rc, "the capture failed");
            break;
        }
        written = write_all(buffer, info.bytes);
        if (written == EPIPE) {
            break; /* the reader is done */
        }
        if (written != 0) {
            fprintf(stderr, "keel-camera: writing to standard output failed: %s\n", strerror(written));
            status = EXIT_RUNTIME;
        }
    }
    free(buffer);
    keel_camera_close(camera);
    return status;
}

/* Raw frames to standard output, each plane without row padding. */
static int command_frames(const settings *s)
{
    keel_camera_stream_info stream;
    keel_camera_frame_info info;
    keel_camera *camera;
    uint8_t *buffer = NULL;
    double end = s->duration > 0 ? now_seconds() + s->duration : 0;
    long written_frames = 0;
    int status;
    int32_t rc;
    if (isatty(STDOUT_FILENO)) {
        return usage_error("%s", "standard output is a terminal: pipe it into a program or a file");
    }
    status = open_camera(s, &camera);
    if (status != 0) {
        return status;
    }
    memset(&stream, 0, sizeof(stream));
    stream.struct_size = sizeof(stream);
    keel_camera_get_stream(camera, &stream);
    buffer = malloc(stream.frame_bytes);
    rc = buffer == NULL ? KEEL_E_NO_MEMORY : keel_camera_start(camera);
    if (rc != KEEL_OK) {
        status = report_handle(camera, rc, "the camera could not start");
    } else {
        fprintf(stderr, "keel-camera: %ux%u %s frames of %u bytes (ffmpeg -f rawvideo -pixel_format %s "
                        "-video_size %ux%u -i -)\n", stream.width, stream.height,
                stream.format == KEEL_CAMERA_FORMAT_I420 ? "i420" : stream.format == KEEL_CAMERA_FORMAT_RGB888
                                                                        ? "rgb888"
                                                                        : "bgr888",
                stream.format == KEEL_CAMERA_FORMAT_I420 ? stream.width * stream.height * 3u / 2u
                                                         : stream.width * stream.height * 3u,
                stream.format == KEEL_CAMERA_FORMAT_I420 ? "yuv420p" : stream.format == KEEL_CAMERA_FORMAT_RGB888
                                                                           ? "rgb24"
                                                                           : "bgr24",
                stream.width, stream.height);
    }
    while (status == 0 && !stop_signal && (end == 0 || now_seconds() < end) &&
           (s->count < 0 || written_frames < s->count)) {
        uint32_t plane;
        uint32_t planes;
        int failed = 0;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        rc = keel_camera_read_frame(camera, buffer, stream.frame_bytes, POLL_MS, &info);
        if (rc == KEEL_E_AGAIN) {
            continue;
        }
        if (rc != KEEL_OK) {
            status = report_handle(camera, rc, "the capture failed");
            break;
        }
        planes = info.plane_count;
        for (plane = 0; plane < planes && !failed; ++plane) {
            uint32_t stride = plane == 0 ? info.stride0 : plane == 1 ? info.stride1 : info.stride2;
            uint32_t offset = plane == 0 ? info.offset0 : plane == 1 ? info.offset1 : info.offset2;
            uint32_t width = info.format == KEEL_CAMERA_FORMAT_I420 ? (plane == 0 ? info.width : info.width / 2u)
                                                                     : info.width * 3u;
            uint32_t height = plane == 0 ? info.height : info.height / 2u;
            uint32_t row;
            for (row = 0; row < height && !failed; ++row) {
                int rc_write = write_all(buffer + offset + (size_t)row * stride, width);
                if (rc_write == EPIPE) {
                    failed = 2;
                } else if (rc_write != 0) {
                    fprintf(stderr, "keel-camera: writing to standard output failed: %s\n", strerror(rc_write));
                    failed = 1;
                }
            }
        }
        if (failed == 2) {
            break;
        }
        if (failed) {
            status = EXIT_RUNTIME;
            break;
        }
        written_frames++;
    }
    free(buffer);
    keel_camera_close(camera);
    return status;
}

int main(int argc, char **argv)
{
    settings s;
    int status = parse(argc, argv, &s);
    if (status != 0) {
        return status;
    }
    install_signals();
    if (strcmp(s.command, "list") == 0) {
        return command_list(&s);
    }
    if (strcmp(s.command, "still") == 0) {
        return command_still(&s);
    }
    if (strcmp(s.command, "record") == 0) {
        if (s.path == NULL) {
            return usage_error("%s", "record needs -o FILE (.h264, .mjpeg, or .mp4, .mkv, .mov, .ts)");
        }
        return command_output(&s, s.path);
    }
    if (strcmp(s.command, "stream") == 0) {
        return strcmp(s.destination, "-") == 0 ? command_stdout(&s) : command_output(&s, s.destination);
    }
    if (strcmp(s.command, "frames") == 0) {
        return command_frames(&s);
    }
    return usage_error("unknown command '%s': use list, still, record, stream or frames", s.command);
}
