#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>

#define MAX_MONITORS 16
#define DEFAULT_FPS 30
#define MAX_FPS 240
#define MIN_SLEEP_NS 1000000  // 1ms minimum sleep time

typedef struct {
    Window window;
    cairo_surface_t *surface;
    cairo_t *cr;
    struct SwsContext *sws_ctx;
    XImage *ximage;
    int x, y;
    int width, height;
} MonitorInfo;

typedef struct {
    Display *display;
    Window root;
    int screen;
    Visual *visual;
    int depth;
    MonitorInfo monitors[MAX_MONITORS];
    int monitor_count;
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    AVFrame *frame;
    int video_stream_index;
    double fps;
    int loop;
    int stretch;
    volatile sig_atomic_t running;
} WallpaperContext;

static WallpaperContext ctx;

static FILE *log_file = NULL;

static void log_message(const char *format, ...) {
    if (!log_file) {
        const char *home = getenv("HOME");
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/.local/share/aniwall/aniwall.log", home);
        log_file = fopen(log_path, "a");
        if (!log_file) return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fflush(log_file);
}

static void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [OPTIONS] <video_file>\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --daemon         Run as daemon\n");
    fprintf(stderr, "  --stretch        Stretch video to fill screen\n");
    fprintf(stderr, "  --loop           Loop video playback\n");
    fprintf(stderr, "  --quit           Stop playback\n");
}

static int parse_options(int argc, char *argv[], char **video_path) {
    static struct option long_options[] = {
        {"daemon", no_argument, 0, 'd'},
        {"stretch", no_argument, 0, 's'},
        {"loop", no_argument, 0, 'l'},
        {"quit", no_argument, 0, 'q'},
        {0, 0, 0, 0}
    };

    int daemon_mode = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "dslq", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                daemon_mode = 1;
                break;
            case 's':
                ctx.stretch = 1;
                break;
            case 'l':
                ctx.loop = 1;
                break;
            case 'q':
                return 2;
            default:
                return -1;
        }
    }

    if (daemon_mode) {
        log_message("\n=== Aniwall started ===\n");
    }

    if (optind < argc) {
        *video_path = argv[optind];
        return 0;
    }

    return daemon_mode ? 0 : -1;  // Allow no video path in daemon mode
}

static void init_x11(void) {
    log_message("Initializing X11...\n");

    // Check if DISPLAY is set
    if (!getenv("DISPLAY")) {
        fprintf(stderr, "DISPLAY environment variable not set\n");
        exit(1);
    }

    ctx.display = XOpenDisplay(NULL);
    if (!ctx.display) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }

    log_message("Display opened successfully\n");

    ctx.screen = DefaultScreen(ctx.display);
    ctx.root = RootWindow(ctx.display, ctx.screen);
    ctx.visual = DefaultVisual(ctx.display, ctx.screen);
    ctx.depth = DefaultDepth(ctx.display, ctx.screen);
    
    // Get screen resources
    XRRScreenResources *res = XRRGetScreenResources(ctx.display, ctx.root);
    if (!res) {
        fprintf(stderr, "Could not get monitor information\n");
        exit(1);
    }

    // Calculate total dimensions for stretch mode
    int total_width = 0;
    int max_height = 0;
    int min_x = INT_MAX;
    int min_y = INT_MAX;

    for (int i = 0; i < res->ncrtc && i < 16; i++) {
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(ctx.display, res, res->crtcs[i]);
        if (!crtc || crtc->width == 0 || crtc->height == 0) continue;
        
        total_width = crtc->x + crtc->width > total_width ? crtc->x + crtc->width : total_width;
        max_height = crtc->height > max_height ? crtc->height : max_height;
        min_x = crtc->x < min_x ? crtc->x : min_x;
        min_y = crtc->y < min_y ? crtc->y : min_y;
        XRRFreeCrtcInfo(crtc);
    }

    if (ctx.stretch) {
        // Create one large window for stretch mode
        ctx.monitor_count = 1;
        XSetWindowAttributes attrs;
        attrs.background_pixel = BlackPixel(ctx.display, ctx.screen);
        attrs.override_redirect = False;
        attrs.event_mask = StructureNotifyMask;

        ctx.monitors[0].window = XCreateWindow(
            ctx.display, ctx.root,
            min_x, min_y,
            total_width - min_x, max_height,
            0, ctx.depth, InputOutput, ctx.visual,
            CWBackPixel | CWOverrideRedirect | CWEventMask,
            &attrs);

        // Set window properties
        Atom type = XInternAtom(ctx.display, "_NET_WM_WINDOW_TYPE", False);
        Atom desktop = XInternAtom(ctx.display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        XChangeProperty(ctx.display, ctx.monitors[0].window,
                       type, XA_ATOM, 32, PropModeReplace,
                       (unsigned char *)&desktop, 1);

        // Set window state
        Atom state = XInternAtom(ctx.display, "_NET_WM_STATE", False);
        Atom below = XInternAtom(ctx.display, "_NET_WM_STATE_BELOW", False);
        Atom sticky = XInternAtom(ctx.display, "_NET_WM_STATE_STICKY", False);
        Atom skip = XInternAtom(ctx.display, "_NET_WM_STATE_SKIP_TASKBAR", False);
        Atom states[] = {below, sticky, skip};
        XChangeProperty(ctx.display, ctx.monitors[0].window,
                       state, XA_ATOM, 32, PropModeReplace,
                       (unsigned char *)states, 3);

        XMapWindow(ctx.display, ctx.monitors[0].window);
        XLowerWindow(ctx.display, ctx.monitors[0].window);

        ctx.monitors[0].width = total_width - min_x;
        ctx.monitors[0].height = max_height;
        ctx.monitors[0].x = min_x;
        ctx.monitors[0].y = min_y;
    } else {
        ctx.monitor_count = 0;

        // Get info for each monitor
        for (int i = 0; i < res->ncrtc && i < 16; i++) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(ctx.display, res, res->crtcs[i]);
            if (!crtc || crtc->width == 0 || crtc->height == 0) continue;

            ctx.monitors[ctx.monitor_count].x = crtc->x;
            ctx.monitors[ctx.monitor_count].y = crtc->y;
            ctx.monitors[ctx.monitor_count].width = crtc->width;
            ctx.monitors[ctx.monitor_count].height = crtc->height;

            // Create a window for each monitor
            XSetWindowAttributes attrs;
            attrs.background_pixel = BlackPixel(ctx.display, ctx.screen);
            attrs.override_redirect = False;  // Let window manager handle it
            attrs.event_mask = StructureNotifyMask;

            ctx.monitors[ctx.monitor_count].window = XCreateWindow(
                ctx.display, ctx.root,
                crtc->x, crtc->y,
                crtc->width, crtc->height,
                0, ctx.depth, InputOutput, ctx.visual,
                CWBackPixel | CWOverrideRedirect | CWEventMask,
                &attrs);

            XStoreName(ctx.display, ctx.monitors[ctx.monitor_count].window, "Aniwall");

            // Set window type to desktop
            Atom type = XInternAtom(ctx.display, "_NET_WM_WINDOW_TYPE", False);
            Atom desktop = XInternAtom(ctx.display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
            XChangeProperty(ctx.display, ctx.monitors[ctx.monitor_count].window,
                           type, XA_ATOM, 32, PropModeReplace,
                           (unsigned char *)&desktop, 1);

            // Set window state to below and sticky
            Atom state = XInternAtom(ctx.display, "_NET_WM_STATE", False);
            Atom below = XInternAtom(ctx.display, "_NET_WM_STATE_BELOW", False);
            Atom sticky = XInternAtom(ctx.display, "_NET_WM_STATE_STICKY", False);
            Atom skip = XInternAtom(ctx.display, "_NET_WM_STATE_SKIP_TASKBAR", False);
            Atom states[] = {below, sticky, skip};
            XChangeProperty(ctx.display, ctx.monitors[ctx.monitor_count].window,
                           state, XA_ATOM, 32, PropModeReplace,
                           (unsigned char *)states, 3);

            // Just map the window and lower it
            XMapWindow(ctx.display, ctx.monitors[ctx.monitor_count].window);
            XLowerWindow(ctx.display, ctx.monitors[ctx.monitor_count].window);

            ctx.monitors[ctx.monitor_count].surface = cairo_xlib_surface_create(
                ctx.display,
                ctx.monitors[ctx.monitor_count].window,
                ctx.visual,
                crtc->width,
                crtc->height);

            ctx.monitors[ctx.monitor_count].cr = cairo_create(
                ctx.monitors[ctx.monitor_count].surface);

            // Enable backing store to reduce flickering
            attrs.backing_store = WhenMapped;
            XChangeWindowAttributes(ctx.display, ctx.monitors[ctx.monitor_count].window,
                                  CWBackingStore, &attrs);

            // Reduce X synchronization
            XSync(ctx.display, False);

            XRRFreeCrtcInfo(crtc);
            ctx.monitor_count++;
        }
    }

    XRRFreeScreenResources(res);

    XFlush(ctx.display);
}

static int init_video(const char *filename) {
    int ret;

    if (avformat_open_input(&ctx.format_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open video file\n");
        return -1;
    }

    ret = avformat_find_stream_info(ctx.format_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream info\n");
        return -1;
    }

    ctx.video_stream_index = -1;
    for (unsigned int i = 0; i < ctx.format_ctx->nb_streams; i++) {
        if (ctx.format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx.video_stream_index = i;
            break;
        }
    }

    if (ctx.video_stream_index == -1) {
        fprintf(stderr, "Could not find video stream\n");
        return -1;
    }

    AVCodecParameters *codecParams = ctx.format_ctx->streams[ctx.video_stream_index]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec\n");
        return -1;
    }

    ctx.codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx.codec_ctx) {
        fprintf(stderr, "Could not allocate codec context\n");
        return -1;
    }

    if (avcodec_parameters_to_context(ctx.codec_ctx, codecParams) < 0) {
        fprintf(stderr, "Could not copy codec params\n");
        return -1;
    }

    if (avcodec_open2(ctx.codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }

    ctx.frame = av_frame_alloc();

    AVRational frame_rate = ctx.format_ctx->streams[ctx.video_stream_index]->r_frame_rate;
    ctx.fps = av_q2d(frame_rate);
    
    if (ctx.fps <= 0 || ctx.fps > 240) {  // Sanity check for FPS
        ctx.fps = 30;
    }

    fprintf(stderr, "Video FPS: %f\n", ctx.fps);  // Debug output

    // Create SwsContext and frame surfaces for each monitor
    for (int i = 0; i < ctx.monitor_count; i++) {
        int target_width, target_height;
        if (ctx.stretch) {
            // Scale to fill entire monitor span
            target_width = ctx.monitors[i].width;
            target_height = ctx.monitors[i].height;
        } else {
            // Original scaling code for individual monitors
            double scale = (double)ctx.monitors[i].height / ctx.codec_ctx->height;
            target_width = ctx.codec_ctx->width * scale;
            target_height = ctx.monitors[i].height;
        }

        ctx.monitors[i].sws_ctx = sws_getContext(
            ctx.codec_ctx->width, ctx.codec_ctx->height, ctx.codec_ctx->pix_fmt,
            target_width, target_height,
            AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, NULL, NULL, NULL
        );
        if (!ctx.monitors[i].sws_ctx) {
            fprintf(stderr, "Could not create scaling context\n");
            return -1;
        }

        // Create XImage for direct rendering
        ctx.monitors[i].ximage = XCreateImage(ctx.display, ctx.visual,
            ctx.depth, ZPixmap, 0, NULL,
            target_width, target_height,
            32, 0);
        
        ctx.monitors[i].ximage->data = malloc(
            target_width * target_height * 4
        );
    }

    return 0;
}

static void render_frame(AVFrame *frame) {
    static int first_frame = 1;
    static int frame_count = 0;
    if (first_frame) {
        log_message("\n=== Video Setup ===\n");
        log_message("Video dimensions: %dx%d\n", ctx.codec_ctx->width, ctx.codec_ctx->height);
        log_message("Stretch mode: %s\n", ctx.stretch ? "yes" : "no");
        first_frame = 0;
    }

    for (int i = 0; i < ctx.monitor_count; i++) {
        log_message("Monitor %d: %dx%d at (%d,%d)\n", 
                i, ctx.monitors[i].width, ctx.monitors[i].height,
                ctx.monitors[i].x, ctx.monitors[i].y);

        uint8_t *dest[4] = { (uint8_t *)ctx.monitors[i].ximage->data, NULL, NULL, NULL };
        int stride[4] = { ctx.monitors[i].ximage->width * 4, 0, 0, 0 };

        sws_scale(ctx.monitors[i].sws_ctx,
            (const uint8_t *const *)frame->data, frame->linesize,
            0, ctx.codec_ctx->height,
            dest, stride);

        // Center the scaled image
        int x = (ctx.monitors[i].width - ctx.monitors[i].ximage->width) / 2;
        int y = 0;  // Align to top

        // Put the image directly to the window
        XPutImage(ctx.display, ctx.monitors[i].window, 
                 DefaultGC(ctx.display, ctx.screen),
                 ctx.monitors[i].ximage, 0, 0, x, y,
                 ctx.monitors[i].ximage->width,
                 ctx.monitors[i].ximage->height);
    }
    if (frame_count % 2 == 0) {
        XFlush(ctx.display);
    }
    frame_count++;
}

static void cleanup(void) {
    if (ctx.frame) av_frame_free(&ctx.frame);
    if (ctx.codec_ctx) avcodec_free_context(&ctx.codec_ctx);
    if (ctx.format_ctx) avformat_close_input(&ctx.format_ctx);
    for (int i = 0; i < ctx.monitor_count; i++) {
        if (ctx.monitors[i].cr) cairo_destroy(ctx.monitors[i].cr);
        if (ctx.monitors[i].surface) cairo_surface_destroy(ctx.monitors[i].surface);
        if (ctx.monitors[i].ximage) {
            free(ctx.monitors[i].ximage->data);
            ctx.monitors[i].ximage->data = NULL;
            XDestroyImage(ctx.monitors[i].ximage);
        }
        if (ctx.monitors[i].sws_ctx) sws_freeContext(ctx.monitors[i].sws_ctx);
        if (ctx.monitors[i].window) XDestroyWindow(ctx.display, ctx.monitors[i].window);
    }
    if (ctx.display) XCloseDisplay(ctx.display);
}

static void signal_handler(int signum) {
    (void)signum;
    ctx.running = 0;
}

static void play_video(void) {
    AVPacket *packet = av_packet_alloc();
    int ret;
    struct timespec last_frame_time, current_time, sleep_time;
    double frame_duration = 1.0 / ctx.fps;

    // Pre-calculate nanoseconds per frame
    long frame_duration_ns = (long)(frame_duration * 1e9);

    // Disable X synchronization during playback
    XSynchronize(ctx.display, False);

    ctx.running = 1;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    do {
        clock_gettime(CLOCK_MONOTONIC, &last_frame_time);

        while (ctx.running && av_read_frame(ctx.format_ctx, packet) >= 0) {
            if (packet->stream_index == ctx.video_stream_index) {
                ret = avcodec_send_packet(ctx.codec_ctx, packet);
                if (ret < 0) break;

                while (ret >= 0) {
                    ret = avcodec_receive_frame(ctx.codec_ctx, ctx.frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) goto end;

                    render_frame(ctx.frame);

                    clock_gettime(CLOCK_MONOTONIC, &current_time);
                    long elapsed_ns = (current_time.tv_sec - last_frame_time.tv_sec) * 1000000000L +
                                     (current_time.tv_nsec - last_frame_time.tv_nsec);
                    
                    long sleep_ns = frame_duration_ns - elapsed_ns;
                    if (sleep_ns > 1000000) {  // Only sleep if we have more than 1ms to wait
                        sleep_time.tv_sec = 0;
                        sleep_time.tv_nsec = sleep_ns;
                        nanosleep(&sleep_time, NULL);
                    }
                    
                    clock_gettime(CLOCK_MONOTONIC, &last_frame_time);
                }
            }
            av_packet_unref(packet);
        }

        if (ctx.loop && ctx.running) {
            av_seek_frame(ctx.format_ctx, ctx.video_stream_index, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(ctx.codec_ctx);
        }
    } while (ctx.loop && ctx.running);

end:
    av_packet_free(&packet);
}

int main(int argc, char *argv[]) {
    char *video_path = NULL;
    memset(&ctx, 0, sizeof(ctx));

    // Set defaults
    ctx.loop = 1;  // Loop by default
    ctx.stretch = 0;  // Don't stretch by default

    // Debug output
    fprintf(stderr, "Starting aniwall-daemon...\n");

    int result = parse_options(argc, argv, &video_path);
    if (result < 0) {
        print_usage(argv[0]);
        return 1;
    } else if (result == 2) {
        // Quit command received
        if (system("pkill -f aniwall-daemon") == -1) {
            fprintf(stderr, "Failed to stop daemon\n");
        }
        return 0;
    }

    // Read options from config if in daemon mode
    if (video_path == NULL) {
        const char *home = getenv("HOME");
        char video_path_buf[1024];
        snprintf(video_path_buf, sizeof(video_path_buf), 
                "%s/.local/share/aniwall/video.mp4", home);
        video_path = video_path_buf;
        fprintf(stderr, "Using video path: %s\n", video_path);
        char options_path[1024];
        snprintf(options_path, sizeof(options_path),
                "%s/.local/share/aniwall/options", home);
        FILE *f = fopen(options_path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "--stretch")) ctx.stretch = 1;
                if (strstr(line, "--loop")) ctx.loop = 1;
            }
            fclose(f);
            fprintf(stderr, "Options loaded: stretch=%d, loop=%d\n", ctx.stretch, ctx.loop);
        }
    }

    init_x11();
    if (init_video(video_path) < 0) {
        cleanup();
        return 1;
    }

    play_video();
    cleanup();
    return 0;
} 