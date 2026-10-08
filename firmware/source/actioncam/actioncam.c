/*
 * actioncam.c - OpenIPC action-cam controller for SigmaStar SSC338Q
 *
 *  - PM_GPIO0 : status LED (active low), driven through /dev/mem registers
 *  - PM_GPIO1 : record/photo/shutdown line from the CH32V003 MCU
 *  - BMI270   : gyro+accel logged to .gcsv at 2x the video frame rate
 *  - Majestic : video/photo captured with curl from 127.0.0.1
 *
 * Build (needs the Bosch BMI270 SensorAPI sources: bmi2.c bmi270.c):
 *   $CC -O2 -Wall -o actioncam actioncam.c bmi2.c bmi270.c -lpthread -lrt
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bmi270.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */
#define SD_MOUNT      "/mnt/mmcblk0p1"
#define SD_DEVICE     "/dev/mmcblk0p1"
#define CONFIG_PATH   SD_MOUNT "/config.txt"
#define MAJESTIC_YAML "/etc/majestic.yaml"
#define VIDEO_DIR     SD_MOUNT "/videos"
#define PHOTO_DIR     SD_MOUNT "/photos"
#define LOG_DIR       SD_MOUNT "/log"

#define VIDEO_URL     "http://127.0.0.1/video.mp4"
#define PHOTO_URL     "http://127.0.0.1/image.jpg"

#define I2C_DEV       "/dev/i2c-3"
#define BMI_ADDR      0x68
#define GCSV_ORIENT   "XYZ"      /* IMU Orientation string - set for your board */

#define DEFAULT_CONFIG \
    "jpeg:\n" \
    "  qfactor: 99\n" \
    "  size: 3840x2160\n" \
    "video0:\n" \
    "  size: 1920x1080\n" \
    "  fps: 90\n" \
    "  bitrate: 8192\n" \
    "  storageSaver: off\n" \
    "audio:\n" \
    "  enabled: false\n"

/* PM register page (0x1F007000) and the 16-bit registers we use */
#define PM_PAGE       0x1F007000UL
#define OFS_LED       0xE00      /* PM_GPIO0 pad control */
#define OFS_CTL       0xE04      /* PM_GPIO1 pad control */
#define OFS_I2CM      0xF40      /* PM_I2CM_MODE */
#define PAD_IN        (1u << 0)
#define PAD_OUT       (1u << 1)
#define PAD_OEN       (1u << 2)  /* 0 = output driven */
#define PAD_GPIO_MODE (1u << 3)

/* Control-line decoder timings (ms) */
#define POLL_US          2000
#define HOLD_MS          200     /* steady level this long => video start/stop */
#define SHUT_MIN_MS      20
#define SHUT_MAX_MS      45
#define PHOTO_MIN_MS     100
#define PHOTO_MAX_MS     199
#define SHUT_PULSES      3       /* short pulses within SHUT_WINDOW_MS */
#define SHUT_WINDOW_MS   600

/* ------------------------------------------------------------------ */
/* Time helpers                                                        */
/* ------------------------------------------------------------------ */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Register access (equivalent of devmem, via mmap of /dev/mem)        */
/* ------------------------------------------------------------------ */
static volatile uint8_t *g_pm;

static int regs_init(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return -1; }
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PM_PAGE);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return -1; }
    g_pm = (volatile uint8_t *)p;
    return 0;
}

static inline uint16_t rd16(unsigned ofs) { return *(volatile uint16_t *)(g_pm + ofs); }
static inline void wr16(unsigned ofs, uint16_t v) { *(volatile uint16_t *)(g_pm + ofs) = v; }

static void i2c_padmux(void)
{
    /* clear bits 0-1, set PM_I2CM_MODE_1 (= 1) */
    wr16(OFS_I2CM, (uint16_t)((rd16(OFS_I2CM) & ~0x03u) | 0x01u));
}

/* ---- LED (PM_GPIO0, active low) ---- */
static void led_hw_init(void)
{
    uint16_t v = rd16(OFS_LED);
    v |= PAD_GPIO_MODE | PAD_OUT;      /* GPIO mode, output high = LED off */
    wr16(OFS_LED, v);
    wr16(OFS_LED, rd16(OFS_LED) & ~PAD_OEN); /* enable output */
}

static void led_hw(int on)
{
    uint16_t v = rd16(OFS_LED);
    v = on ? (v & ~PAD_OUT) : (v | PAD_OUT);
    wr16(OFS_LED, v);
}

/* ---- control line (PM_GPIO1, input) ---- */
static void ctl_hw_init(void)
{
    wr16(OFS_CTL, rd16(OFS_CTL) | PAD_GPIO_MODE | PAD_OEN);  /* GPIO, input */
}

static inline int ctl_read(void) { return rd16(OFS_CTL) & PAD_IN; }

/* ------------------------------------------------------------------ */
/* LED patterns (non-blocking, driven from the main loop)             */
/* ------------------------------------------------------------------ */
enum led_mode { LED_OFF, LED_REC, LED_DOUBLE, LED_ERROR };

static enum led_mode g_led_mode = LED_OFF, g_led_after = LED_OFF;
static long g_led_t0;
static int g_led_state = -1;

static void led_set_mode(enum led_mode m, long now)
{
    g_led_mode = m;
    g_led_t0 = now;
}

static void led_update(long now)
{
    long e = now - g_led_t0;
    int on = 0;

    switch (g_led_mode) {
    case LED_OFF:    on = 0; break;
    case LED_REC:    on = ((e / 1000) % 2) == 0; break;       /* 1 s on, 1 s off */
    case LED_ERROR:  on = ((e / 200) % 2) == 0; break;
    case LED_DOUBLE:
        if (e >= 400) { led_set_mode(g_led_after, now); on = 0; break; }
        on = (e < 100) || (e >= 200 && e < 300);              /* on/off/on/off */
        break;
    }
    if (on != g_led_state) { led_hw(on); g_led_state = on; }
}

/* ------------------------------------------------------------------ */
/* sync (non-blocking for the control loop)                            */
/* ------------------------------------------------------------------ */
static void *sync_thread(void *a) { (void)a; sync(); return NULL; }

static void sync_async(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, sync_thread, NULL) == 0)
        pthread_detach(t);
    else
        sync();
}

/* ------------------------------------------------------------------ */
/* Small file helpers                                                  */
/* ------------------------------------------------------------------ */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char buf[1024];
    size_t n;
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    return 0;
}

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static int run_cmd(const char *cmd)
{
    int r = system(cmd);
    return r;
}

/* ------------------------------------------------------------------ */
/* SD card, config, Majestic                                           */
/* ------------------------------------------------------------------ */
static int sd_is_mounted(void)
{
    struct stat a, b;
    if (stat(SD_MOUNT, &a) != 0 || stat("/mnt", &b) != 0) return 0;
    return a.st_dev != b.st_dev;     /* different device => something mounted */
}

static void sd_try_mount(void)
{
    if (!sd_is_mounted())
        run_cmd("mount " SD_DEVICE " " SD_MOUNT " 2>/dev/null");
}

static void make_dirs(void)
{
    mkdir(VIDEO_DIR, 0755);
    mkdir(PHOTO_DIR, 0755);
    mkdir(LOG_DIR,   0755);
    sync();
}

/* Create config.txt with defaults if missing, then copy it to majestic.yaml */
static int apply_config(void)
{
    if (!file_exists(CONFIG_PATH)) {
        FILE *f = fopen(CONFIG_PATH, "w");
        if (!f) { perror("create config.txt"); return -1; }
        fputs(DEFAULT_CONFIG, f);
        fflush(f); fsync(fileno(f)); fclose(f);
        sync();
    }
    if (copy_file(CONFIG_PATH, MAJESTIC_YAML) != 0) {
        perror("copy config to majestic.yaml");
        return -1;
    }
    return 0;
}

/* Read video0.fps from majestic.yaml (default 30) */
static int config_fps(void)
{
    FILE *f = fopen(MAJESTIC_YAML, "r");
    char line[256], section[64] = "";
    int fps = 30;
    if (!f) return fps;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != ' ' && line[0] != '\t' && line[0] != '#') {
            char *c = strchr(line, ':');
            if (c) { size_t n = (size_t)(c - line); if (n >= sizeof section) n = sizeof section - 1;
                     memcpy(section, line, n); section[n] = 0; }
        } else if (strcmp(section, "video0") == 0) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "fps:", 4) == 0) {
                int v = atoi(p + 4);
                if (v > 0 && v <= 240) fps = v;
            }
        }
    }
    fclose(f);
    return fps;
}

static int tcp_port_open(int port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0), ok;
    if (s < 0) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = connect(s, (struct sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

static void restart_majestic(void)
{
    run_cmd("killall -HUP majestic");
    msleep(2000);                                  /* let it reload */
    for (int i = 0; i < 30 && !tcp_port_open(80); i++)
        msleep(500);                               /* wait for the web server */
}

/* ------------------------------------------------------------------ */
/* File numbering                                                      */
/* ------------------------------------------------------------------ */
static int next_number(const char *dir, const char *prefix, const char *ext)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int max = 0;
    size_t pl = strlen(prefix), el = strlen(ext);
    if (!d) return 1;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t l = strlen(n);
        char *end;
        long v;
        if (l <= pl + el || strncmp(n, prefix, pl) != 0) continue;
        if (strcmp(n + l - el, ext) != 0) continue;
        v = strtol(n + pl, &end, 10);
        if (end == n + pl || end != n + l - el) continue;   /* digits only */
        if (v > max) max = (int)v;
    }
    closedir(d);
    return max + 1;
}

/* ------------------------------------------------------------------ */
/* BMI270 over I2C                                                     */
/* ------------------------------------------------------------------ */
static int g_i2c_fd = -1;
static struct bmi2_dev g_bmi;
static int g_gyro_ok;

static BMI2_INTF_RETURN_TYPE bmi_read(uint8_t reg, uint8_t *data, uint32_t len, void *p)
{
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data set;
    (void)p;
    msgs[0].addr = BMI_ADDR; msgs[0].flags = 0;        msgs[0].len = 1;           msgs[0].buf = &reg;
    msgs[1].addr = BMI_ADDR; msgs[1].flags = I2C_M_RD; msgs[1].len = (uint16_t)len; msgs[1].buf = data;
    set.msgs = msgs; set.nmsgs = 2;
    return ioctl(g_i2c_fd, I2C_RDWR, &set) < 0 ? BMI2_E_COM_FAIL : BMI2_OK;
}

static BMI2_INTF_RETURN_TYPE bmi_write(uint8_t reg, const uint8_t *data, uint32_t len, void *p)
{
    uint8_t buf[300];
    struct i2c_msg msg;
    struct i2c_rdwr_ioctl_data set;
    (void)p;
    if (len + 1 > sizeof buf) return BMI2_E_COM_FAIL;
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    msg.addr = BMI_ADDR; msg.flags = 0; msg.len = (uint16_t)(len + 1); msg.buf = buf;
    set.msgs = &msg; set.nmsgs = 1;
    return ioctl(g_i2c_fd, I2C_RDWR, &set) < 0 ? BMI2_E_COM_FAIL : BMI2_OK;
}

static void bmi_delay(uint32_t us, void *p) { (void)p; usleep(us); }

/* smallest BMI270 ODR code >= target_hz (25..1600 Hz) */
static uint8_t odr_code_for(int target_hz, int *actual_hz)
{
    static const struct { int hz; uint8_t code; } t[] = {
        {25, 0x06}, {50, 0x07}, {100, 0x08}, {200, 0x09},
        {400, 0x0A}, {800, 0x0B}, {1600, 0x0C }
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].hz >= target_hz) { *actual_hz = t[i].hz; return t[i].code; }
    *actual_hz = 1600;
    return 0x0C;
}

static int bmi_setup(int target_hz)
{
    struct bmi2_sens_config cfg[2];
    uint8_t list[2] = { BMI2_ACCEL, BMI2_GYRO };
    int8_t r;
    int actual;
    uint8_t odr = odr_code_for(target_hz, &actual);

    g_i2c_fd = open(I2C_DEV, O_RDWR);
    if (g_i2c_fd < 0) { fprintf(stderr, "cannot open %s\n", I2C_DEV); return -1; }

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.intf = BMI2_I2C_INTF;
    g_bmi.read = bmi_read;
    g_bmi.write = bmi_write;
    g_bmi.delay_us = bmi_delay;
    g_bmi.read_write_len = 32;
    g_bmi.intf_ptr = NULL;
    g_bmi.config_file_ptr = NULL;

    r = bmi270_init(&g_bmi);
    if (r != BMI2_OK) { fprintf(stderr, "bmi270_init failed: %d\n", r); return -1; }

    memset(cfg, 0, sizeof cfg);
    cfg[0].type = BMI2_ACCEL;
    cfg[1].type = BMI2_GYRO;
    r = bmi2_get_sensor_config(cfg, 2, &g_bmi);
    if (r != BMI2_OK) { fprintf(stderr, "get_sensor_config: %d\n", r); return -1; }

    cfg[0].cfg.acc.odr = odr;
    cfg[0].cfg.acc.range = BMI2_ACC_RANGE_8G;
    cfg[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    cfg[0].cfg.acc.filter_perf = (odr >= BMI2_ACC_ODR_100HZ) ? BMI2_PERF_OPT_MODE : BMI2_POWER_OPT_MODE;

    cfg[1].cfg.gyr.odr = odr;
    cfg[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    cfg[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    cfg[1].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
    cfg[1].cfg.gyr.filter_perf = (odr >= 0x08) ? BMI2_PERF_OPT_MODE : BMI2_POWER_OPT_MODE;

    r = bmi2_set_sensor_config(cfg, 2, &g_bmi);
    if (r != BMI2_OK) { fprintf(stderr, "set_sensor_config: %d\n", r); return -1; }
    r = bmi2_sensor_enable(list, 2, &g_bmi);
    if (r != BMI2_OK) { fprintf(stderr, "sensor_enable: %d\n", r); return -1; }

    fprintf(stderr, "BMI270 ready: ODR %d Hz (target %d Hz)\n", actual, target_hz);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Gyro logger thread (.gcsv)                                          */
/* ------------------------------------------------------------------ */
struct gyro_job {
    FILE *f;
    int hz;                        /* logging rate = 2 x fps */
    long long t0_us;
    volatile int run;
    pthread_t th;
    int started;
    char video_path[160];          /* file curl is writing; used to detect the first frame */
};
static struct gyro_job g_gyro;


/* ---- first-frame detection -------------------------------------------
 * Majestic streams fragmented MP4: [ftyp][moov] init segment first, then
 * [moof][mdat] fragments that carry the frames. We treat the arrival of the
 * first moof/mdat box as "first frame received". Fallbacks: if the file grows
 * past 256 KiB or 8 s pass without seeing one, start logging anyway.       */
static int first_frame_seen(int fd)
{
    struct stat st;
    uint8_t h[8];
    off_t off = 0;

    if (fstat(fd, &st) != 0) return 0;
    for (int i = 0; i < 16; i++) {
        uint32_t sz;
        if (off + 8 > st.st_size) return 0;                 /* header not here yet */
        if (pread(fd, h, 8, off) != 8) return 0;
        if (!memcmp(h + 4, "moof", 4) || !memcmp(h + 4, "mdat", 4)) return 1;
        sz = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        if (sz < 8) return st.st_size > 262144;             /* unusual box: use size fallback */
        off += sz;
    }
    return 1;
}

static int wait_first_frame(struct gyro_job *j)
{
    int fd = -1, ok = 0;
    long long start = now_us();

    while (j->run) {
        if (fd < 0) fd = open(j->video_path, O_RDONLY);      /* curl creates it lazily */
        if (fd >= 0) {
            struct stat st;
            if (first_frame_seen(fd)) { ok = 1; break; }
            if (fstat(fd, &st) == 0 && st.st_size > 262144) { ok = 1; break; }
        }
        if (now_us() - start > 8000000LL) { ok = 1; break; }  /* give up waiting */
        usleep(1000);
    }
    if (fd >= 0) close(fd);
    return ok;
}

static void *gyro_thread(void *arg)
{
    struct gyro_job *j = arg;
    struct bmi2_sens_data d;
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its;
    uint64_t exp;
    long long last_flush = now_us();

    if (tfd < 0) return NULL;

    /* sensor is already configured; do not log until the first frame arrives */
    if (!wait_first_frame(j)) { close(tfd); return NULL; }
    j->t0_us = now_us();                       /* gyro t = 0  <->  first video frame */
    last_flush = j->t0_us;

    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000000000L / j->hz;
    its.it_interval = its.it_value;
    timerfd_settime(tfd, 0, &its, NULL);

    while (j->run) {
        long long t;
        if (read(tfd, &exp, sizeof exp) != sizeof exp) continue;
        t = now_us() - j->t0_us;
        if (bmi2_get_sensor_data(&d, &g_bmi) != BMI2_OK) continue;
        fprintf(j->f, "%lld,%d,%d,%d,%d,%d,%d\n", t,
                d.gyr.x, d.gyr.y, d.gyr.z, d.acc.x, d.acc.y, d.acc.z);
        {
            long long n = now_us();
            if (n - last_flush > 1000000) { fflush(j->f); last_flush = n; }
        }
    }
    close(tfd);
    return NULL;
}

static int gyro_start(const char *path, const char *video_path, const char *videoname, int fps)
{
    struct gyro_job *j = &g_gyro;
    double gscale = (2000.0 * 3.14159265358979323846 / 180.0) / 32768.0;
    double ascale = 8.0 / 32768.0;

    memset(j, 0, sizeof *j);
    j->f = fopen(path, "w");
    if (!j->f) return -1;
    setvbuf(j->f, NULL, _IOFBF, 1 << 16);
    j->hz = fps * 2;

    fprintf(j->f,
        "GYROFLOW IMU LOG\nversion,1.3\nid,openipc_ssc338q_bmi270\n"
        "orientation," GCSV_ORIENT "\nnote,actioncam\nfwversion,1.0\n"
        "timestamp,%ld\nvendor,openipc\nvideofilename,%s\n"
        "tscale,0.000001\ngscale,%.12f\nascale,%.12f\n"
        "t,gx,gy,gz,ax,ay,az\n",
        (long)time(NULL), videoname, gscale, ascale);

    snprintf(j->video_path, sizeof j->video_path, "%s", video_path);
    j->run = 1;
    if (pthread_create(&j->th, NULL, gyro_thread, j) != 0) {
        fclose(j->f); j->f = NULL; j->run = 0;
        return -1;
    }
    j->started = 1;
    return 0;
}

static void gyro_stop(void)
{
    struct gyro_job *j = &g_gyro;
    if (!j->started) return;
    j->run = 0;
    pthread_join(j->th, NULL);
    fflush(j->f);
    fsync(fileno(j->f));
    fclose(j->f);
    j->f = NULL;
    j->started = 0;
}

/* ------------------------------------------------------------------ */
/* Capture control                                                     */
/* ------------------------------------------------------------------ */
static pid_t g_video_pid = -1, g_photo_pid = -1;
static int g_recording;
static char g_photo_path[256];

static pid_t spawn_curl(int with_timeout, const char *out, const char *url)
{
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        if (with_timeout)
            execlp("curl", "curl", "-s", "-f", "-m", "15", "-o", out, url, (char *)NULL);
        else
            execlp("curl", "curl", "-s", "-N", "-o", out, url, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void start_recording(long now)
{
    int n = next_number(VIDEO_DIR, "video_", ".mp4");
    char vpath[128], gpath[128], lpath[128], vname[64];
    int fps = config_fps();

    snprintf(vname, sizeof vname, "video_%04d.mp4", n);
    snprintf(vpath, sizeof vpath, VIDEO_DIR "/%s", vname);
    snprintf(gpath, sizeof gpath, VIDEO_DIR "/gyro_%04d.gcsv", n);
    snprintf(lpath, sizeof lpath, LOG_DIR "/log_video_%04d.txt", n);

    copy_file(MAJESTIC_YAML, lpath);             /* config snapshot (overwrites) */
    if (g_gyro_ok && gyro_start(gpath, vpath, vname, fps) != 0)
        fprintf(stderr, "gyro logging failed to start\n");

    g_video_pid = spawn_curl(0, vpath, VIDEO_URL);
    g_recording = 1;
    led_set_mode(LED_REC, now);
    sync_async();
    fprintf(stderr, "recording %s (gyro %d Hz)\n", vname, fps * 2);
}

static void stop_recording(long now)
{
    if (!g_recording) return;
    if (g_video_pid > 0) {
        kill(g_video_pid, SIGTERM);
        waitpid(g_video_pid, NULL, 0);
        g_video_pid = -1;
    }
    gyro_stop();
    g_recording = 0;
    led_set_mode(LED_OFF, now);
    sync_async();
    fprintf(stderr, "recording stopped\n");
}

static void take_photo(long now)
{
    int n;
    char lpath[128];

    if (g_recording || g_photo_pid > 0) return;
    n = next_number(PHOTO_DIR, "photo_", ".jpg");
    snprintf(g_photo_path, sizeof g_photo_path, PHOTO_DIR "/photo_%04d.jpg", n);
    snprintf(lpath, sizeof lpath, LOG_DIR "/log_photo_%04d.txt", n);

    copy_file(MAJESTIC_YAML, lpath);
    g_photo_pid = spawn_curl(1, g_photo_path, PHOTO_URL);
    g_led_after = LED_OFF;
    led_set_mode(LED_DOUBLE, now);
    sync_async();
    fprintf(stderr, "photo %s\n", g_photo_path);
}

/* ------------------------------------------------------------------ */
/* Control-line decoder                                                */
/* ------------------------------------------------------------------ */
enum ctl_event { EV_NONE, EV_PHOTO, EV_REC_START, EV_REC_STOP, EV_SHUTDOWN };

struct ctl {
    int level;
    long t_change;
    int rise_known;
    int start_sent, stop_sent;
    long pulse_t[8];
    int pulse_n;
};

static void ctl_init(struct ctl *c, long now)
{
    memset(c, 0, sizeof *c);
    c->level = ctl_read();
    c->t_change = now;
    c->rise_known = 0;         /* line may already be high: unknown rise time */
}

static void ctl_rearm(struct ctl *c, long now, long delay_ms)
{
    c->start_sent = 0;
    c->t_change = now + delay_ms;
}

static enum ctl_event ctl_poll(struct ctl *c, long now)
{
    int l = ctl_read();

    if (l != c->level) {
        if (l) {                                   /* rising edge */
            c->t_change = now;
            c->rise_known = 1;
            c->start_sent = 0;
        } else {                                   /* falling edge */
            long dur = now - c->t_change;
            c->level = l;
            if (c->rise_known && !c->start_sent) {
                if (dur >= SHUT_MIN_MS && dur <= SHUT_MAX_MS) {
                    int i, k = 0;
                    for (i = 0; i < c->pulse_n; i++)
                        if (now - c->pulse_t[i] <= SHUT_WINDOW_MS) c->pulse_t[k++] = c->pulse_t[i];
                    c->pulse_n = k;
                    if (c->pulse_n < 8) c->pulse_t[c->pulse_n++] = now;
                    c->t_change = now; c->stop_sent = 0;
                    if (c->pulse_n >= SHUT_PULSES) return EV_SHUTDOWN;
                    return EV_NONE;
                }
                if (dur >= PHOTO_MIN_MS && dur <= PHOTO_MAX_MS) {
                    c->t_change = now; c->stop_sent = 0;
                    return EV_PHOTO;
                }
            }
            c->t_change = now;
            c->stop_sent = 0;
            return EV_NONE;
        }
        c->level = l;
        return EV_NONE;
    }

    if (l && !c->start_sent && now - c->t_change >= HOLD_MS) {
        c->start_sent = 1;
        return EV_REC_START;
    }
    if (!l && !c->stop_sent && now - c->t_change >= HOLD_MS) {
        c->stop_sent = 1;
        return EV_REC_STOP;
    }
    return EV_NONE;
}

/* ------------------------------------------------------------------ */
/* Shutdown / signals                                                  */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

static void do_shutdown(void)
{
    long now = now_ms();
    fprintf(stderr, "shutdown requested\n");
    stop_recording(now);
    led_set_mode(LED_OFF, now);
    led_update(now);
    sync();
    run_cmd("poweroff");
    for (;;) pause();          /* never exit: the supervisor must not restart us */
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    struct ctl c;
    int fps;
    long now;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (regs_init() != 0) return 1;
    led_hw_init();
    ctl_hw_init();
    led_set_mode(LED_OFF, now_ms());
    led_update(now_ms());

    /* 1. I2C pad-mux */
    i2c_padmux();

    /* 2. SD card (keep retrying; blink fast while missing) */
    ctl_init(&c, now_ms());
    led_set_mode(LED_ERROR, now_ms());
    while (!sd_is_mounted() && !g_quit) {
        long t = now_ms(), next = t;
        sd_try_mount();
        while (now_ms() - next < 1000 && !g_quit) {
            now = now_ms();
            led_update(now);
            if (ctl_poll(&c, now) == EV_SHUTDOWN) do_shutdown();
            usleep(POLL_US);
        }
    }
    led_set_mode(LED_OFF, now_ms());
    led_update(now_ms());
    make_dirs();

    /* 3. config.txt -> majestic.yaml, then restart majestic */
    if (apply_config() != 0)
        fprintf(stderr, "config apply failed; continuing with existing majestic.yaml\n");
    restart_majestic();
    fps = config_fps();

    /* 4. IMU */
    g_gyro_ok = (bmi_setup(fps * 2) == 0);
    if (!g_gyro_ok) fprintf(stderr, "IMU unavailable: recording video without gyro\n");

    /* 5. main loop. ctl_init re-samples the line so a level that is already
     *    high starts recording after the 200 ms hold time. */
    ctl_init(&c, now_ms());

    while (!g_quit) {
        now = now_ms();

        switch (ctl_poll(&c, now)) {
        case EV_SHUTDOWN:  do_shutdown(); break;
        case EV_REC_START: if (!g_recording) start_recording(now); break;
        case EV_REC_STOP:  if (g_recording) stop_recording(now); break;
        case EV_PHOTO:     take_photo(now); break;
        default: break;
        }

        /* reap finished curl processes */
        if (g_photo_pid > 0) {
            int st;
            if (waitpid(g_photo_pid, &st, WNOHANG) == g_photo_pid) {
                if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
                    fprintf(stderr, "photo capture failed\n");
                    unlink(g_photo_path);
                }
                g_photo_pid = -1;
                sync_async();
            }
        }
        if (g_recording && g_video_pid > 0) {
            int st;
            if (waitpid(g_video_pid, &st, WNOHANG) == g_video_pid) {
                fprintf(stderr, "video stream ended unexpectedly\n");
                g_video_pid = -1;
                stop_recording(now);
                if (ctl_read()) ctl_rearm(&c, now, 1800);  /* line still high: retry */
            }
        }

        led_update(now);
        usleep(POLL_US);
    }

    stop_recording(now_ms());
    sync();
    led_hw(0);
    return 0;
}