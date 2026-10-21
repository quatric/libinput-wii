/* Pressure-sensitive painting with a USB pen tablet.
 *
 * Plug a tablet (Wacom Intuos, One, Bamboo; Huion; XP-Pen...) into a rear
 * USB port, before or after starting.  Line width follows pen pressure, the
 * eraser end (or the upper side switch) erases, and drawings can be saved to
 * the SD card as BMP files.
 *
 *   Pen lower side switch / tablet key 1 / A   next colour
 *   Tablet key 2 / -                           thinner brush
 *   Tablet key 3 / +                           thicker brush
 *   Tablet key 4 / 1                           clear
 *   Pen upper side switch                      eraser while held
 *   Finger, on a Bamboo Pen & Touch            draws too
 *   2                                          save to sd:/sketches/
 *   B                                          show the driver's log
 *   HOME (or RESET on the console)             exit
 *
 * With the Wii on the network it also streams the driver's log and the pen
 * state on TCP port 4301 (`nc <wii-ip> 4301`); sending "q" exits.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

#include <gccore.h>
#include <fat.h>
#include <network.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/machine/processor.h>
#include <wiiuse/wpad.h>

#include <input_wii.h>
#include <input_tablet.h>

#define TEXT_H     80   /* five console lines of status */
#define BAR_H      86   /* the status bar, with the colour swatch under it */
#define CURSOR_R   6
#define MAX_SIZE   48
/* A stroke is only joined to the sample before it when that came this
 * recently and this near: further apart, a report was lost on the way (often
 * the one saying the pen had left), and joining them would draw a stray line
 * across the page.  The pen reports every 5-8 ms. */
#define JOIN_MS    60
#define JOIN_PX    100
/* The tip switch can drop out for a report at light pressure; a stroke ends
 * only after this many lifted reports in a row. */
#define LIFT_REPORTS 2

static GXRModeObj *rmode;
static u32 *xfb;        /* what is on screen: YUYV, two pixels per word */
static u32 *canvas;     /* the drawing alone, in the same format */
static u8 *rgb;         /* the drawing as 24-bit RGB, for saving */
static int fb_w, fb_h, words_per_row;

typedef struct { u8 r, g, b; } colour;

static const colour palette[] = {
	{ 20, 20, 20 },    /* ink */
	{ 200, 30, 40 },   /* red */
	{ 240, 140, 20 },  /* orange */
	{ 240, 210, 40 },  /* yellow */
	{ 40, 160, 60 },   /* green */
	{ 30, 90, 200 },   /* blue */
	{ 130, 50, 170 },  /* purple */
	{ 120, 80, 50 },   /* brown */
};
#define NCOLOURS (int)(sizeof(palette) / sizeof(palette[0]))
static const colour paper = { 250, 248, 240 };

static int cur_colour;
static int brush = 8;

/* ---- pixels -------------------------------------------------------------- */

static u8 clamp8(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : (u8)v);
}

static void to_yuv(colour c, u8 *y, u8 *cb, u8 *cr)
{
	*y = clamp8((299 * c.r + 587 * c.g + 114 * c.b) / 1000);
	*cb = clamp8(128 + (-169 * c.r - 331 * c.g + 500 * c.b) / 1000);
	*cr = clamp8(128 + (500 * c.r - 419 * c.g - 81 * c.b) / 1000);
}

/* Sets one pixel's brightness and its pair's colour, in both buffers. */
static void plot(int x, int y, colour c)
{
	u8 Y, cb, cr;
	u32 i, w;
	u8 *p;

	if (x < 0 || x >= fb_w || y < BAR_H || y >= fb_h)
		return;
	to_yuv(c, &Y, &cb, &cr);

	i = (u32)y * words_per_row + (u32)x / 2;
	w = canvas[i];
	if (x & 1)
		w = (w & 0xFFFF00FFu) | ((u32)Y << 8);
	else
		w = (w & 0x0000FF00u) | ((u32)Y << 24) | (w & 0xFFu);
	w = (w & 0xFF00FF00u) | ((u32)cb << 16) | cr;
	canvas[i] = w;
	xfb[i] = w;

	p = rgb + ((u32)y * fb_w + (u32)x) * 3;
	p[0] = c.r;
	p[1] = c.g;
	p[2] = c.b;
}

static void disc(float cx, float cy, float r, colour c)
{
	int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
	int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
	float r2 = r * r;
	int x, y;

	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			float dx = x - cx, dy = y - cy;

			if (dx * dx + dy * dy <= r2)
				plot(x, y, c);
		}
}

/* A stroke segment, its width easing from r0 to r1. */
static void segment(float x0, float y0, float r0, float x1, float y1, float r1,
                    colour c)
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	float step = fmaxf(0.5f, fminf(r0, r1) * 0.4f);
	int n = (int)(len / step) + 1, i;

	for (i = 1; i <= n; i++) {
		float t = (float)i / n;

		disc(x0 + dx * t, y0 + dy * t, r0 + (r1 - r0) * t, c);
	}
}

static void clear_canvas(void)
{
	u8 Y, cb, cr;
	u32 w, i;
	int x, y;

	to_yuv(paper, &Y, &cb, &cr);
	w = ((u32)Y << 24) | ((u32)cb << 16) | ((u32)Y << 8) | cr;
	for (i = (u32)BAR_H * words_per_row; i < (u32)fb_h * words_per_row; i++)
		canvas[i] = xfb[i] = w;
	for (y = 0; y < fb_h; y++)
		for (x = 0; x < fb_w; x++) {
			u8 *p = rgb + ((u32)y * fb_w + x) * 3;

			p[0] = paper.r;
			p[1] = paper.g;
			p[2] = paper.b;
		}
}

/* One stroke in progress; the pen and a finger each have their own, so the
 * two are never joined into one line. */
typedef struct {
	bool drawing;
	float x, y, r;
	u32 time;
	int lifted;
} stroke;

/* ---- cursor -------------------------------------------------------------- */

static int cur_x = -1, cur_y = -1;

static void restore(int cx, int cy)
{
	int y, x0, x1;

	if (cx < 0)
		return;
	x0 = (cx - CURSOR_R - 1) / 2;
	x1 = (cx + CURSOR_R + 2) / 2;
	if (x0 < 0) x0 = 0;
	if (x1 >= words_per_row) x1 = words_per_row - 1;
	for (y = cy - CURSOR_R - 1; y <= cy + CURSOR_R + 1; y++) {
		if (y < BAR_H || y >= fb_h)
			continue;
		memcpy(&xfb[y * words_per_row + x0], &canvas[y * words_per_row + x0],
		       (x1 - x0 + 1) * 4);
	}
}

/* A crosshair drawn straight onto the screen, never into the canvas. */
static void draw_cursor(int cx, int cy, bool eraser)
{
	u32 mark = eraser ? 0xEB80EB80u : 0x10801080u; /* white or black */
	int i;

	for (i = -CURSOR_R; i <= CURSOR_R; i++) {
		int x = cx + i, y = cy + i;

		if (x >= 0 && x < fb_w && cy >= BAR_H && cy < fb_h)
			xfb[cy * words_per_row + x / 2] = mark;
		if (y >= BAR_H && y < fb_h && cx >= 0 && cx < fb_w)
			xfb[y * words_per_row + cx / 2] = mark;
	}
}

/* ---- saving -------------------------------------------------------------- */

static bool sd_ok;

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static int save_bmp(char *path, u32 size)
{
	u32 row = ((u32)fb_w * 3 + 3) & ~3u;
	u32 h = (u32)(fb_h - BAR_H);
	u8 hdr[54] = { 'B', 'M' };
	static u8 line[1024 * 3];
	FILE *f;
	int n, y;

	mkdir("sd:/sketches", 0777);
	for (n = 1; n < 1000; n++) {
		struct stat st;

		snprintf(path, size, "sd:/sketches/sketch_%03d.bmp", n);
		if (stat(path, &st) != 0)
			break;
	}
	f = fopen(path, "wb");
	if (!f)
		return -1;

	put32(hdr + 2, 54 + row * h);
	put32(hdr + 10, 54);
	put32(hdr + 14, 40);
	put32(hdr + 18, (u32)fb_w);
	put32(hdr + 22, h);
	hdr[26] = 1;
	hdr[28] = 24;
	put32(hdr + 34, row * h);
	fwrite(hdr, 1, sizeof(hdr), f);

	/* Bottom row first, blue-green-red. */
	for (y = fb_h - 1; y >= BAR_H; y--) {
		const u8 *p = rgb + (u32)y * fb_w * 3;
		int x;

		memset(line, 0, row);
		for (x = 0; x < fb_w; x++) {
			line[x * 3 + 0] = p[x * 3 + 2];
			line[x * 3 + 1] = p[x * 3 + 1];
			line[x * 3 + 2] = p[x * 3 + 0];
		}
		fwrite(line, 1, row, f);
	}
	fclose(f);
	return 0;
}

/* ---- leaving ------------------------------------------------------------- */

#define STUB_ADDR  0x80001800
#define STUB_MAGIC ((volatile u32 *)0x80001804)

#define PI_INTSR   ((volatile u32 *)0xCC003000)
#define PI_INTMR   ((volatile u32 *)0xCC003004)

static bool stub_found(void)
{
	return (STUB_MAGIC[0] == 0x53545542 && STUB_MAGIC[1] == 0x48415858) ||
	       (STUB_MAGIC[1] == 0x53545542 && STUB_MAGIC[2] == 0x48415858);
}

/* Into the Homebrew Channel's stub, with interrupts off at the processor and
 * at their source so nothing still armed fires into this program once the
 * stub has loaded over it. */
static void __attribute__((noreturn)) jump_to_loader(void)
{
	u32 level;

	_CPU_ISR_Disable(level);
	(void)level;
	*PI_INTMR = 0;
	*PI_INTSR = 0xFFFFFFFF;
	if (stub_found())
		((void (*)(void))STUB_ADDR)();
	_CPU_ISR_Restore(level);
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
	while (1)
		;
}

static u8 rescue_stack[4096] ATTRIBUTE_ALIGN(32);

/* Started as leaving begins: if that is not done in four seconds -- a USB
 * call stuck in IOS, a reload that stalls -- jump anyway. */
static void *rescue_main(void *arg)
{
	(void)arg;
	usleep(4000 * 1000);
	jump_to_loader();
	return NULL;
}

/* RESET on the console leaves and POWER turns the Wii off, from a thread of
 * their own, so they work even with the main loop stuck. */
static volatile bool want_reset, want_power;
static u8 escape_stack[4096] ATTRIBUTE_ALIGN(32);

static void reset_pressed(u32 irq, void *ctx)
{
	(void)irq;
	(void)ctx;
	want_reset = true;
}

static void power_pressed(void)
{
	want_power = true;
}

static void *escape_main(void *arg)
{
	lwp_t rescue;

	(void)arg;
	while (!want_reset && !want_power)
		usleep(50 * 1000);
	if (want_power)
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	LWP_CreateThread(&rescue, rescue_main, NULL, rescue_stack,
	                 sizeof(rescue_stack), LWP_PRIO_HIGHEST);
	IOS_ReloadIOS(IOS_GetVersion());
	jump_to_loader();
	return NULL;
}

/* Reloading IOS first drops anything still queued in it (a tablet read
 * waits for as long as the pen is away), which would otherwise hold up the
 * Homebrew Channel's stub as it closes every descriptor. */
static void leave(void)
{
	lwp_t rescue;

	LWP_CreateThread(&rescue, rescue_main, NULL, rescue_stack,
	                 sizeof(rescue_stack), LWP_PRIO_HIGHEST);
	TB_Shutdown();
	INPUT_Shutdown();
	if (sd_ok)
		fatUnmount("sd:");
	VIDEO_SetBlack(true);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	IOS_ReloadIOS(IOS_GetVersion());
	jump_to_loader();
}

/* ---- network monitor ------------------------------------------------------- */

#define MON_PORT 4301
#define LOG_SIZE 16384

static char log_buf[LOG_SIZE];
static u32 log_len;
static u32 log_cut;   /* bytes dropped from the front, in all */
static mutex_t log_lock = LWP_MUTEX_NULL;
static volatile bool mon_quit;
static char net_state[40] = "net: starting";
static u8 mon_stack[16384] ATTRIBUTE_ALIGN(32);

/* The driver's log: kept for the monitor, which sends it to each client. */
static int mon_log(const char *fmt, ...)
{
	va_list ap;
	int n;

	LWP_MutexLock(log_lock);
	/* Nearly full: keep the newer half, from a line start.  A monitor
	 * client that had read past the cut gets the rest from the cut. */
	if (log_len > LOG_SIZE - 256) {
		u32 cut = log_len / 2;

		while (cut < log_len && log_buf[cut - 1] != '\n')
			cut++;
		memmove(log_buf, log_buf + cut, log_len - cut);
		log_len -= cut;
		log_cut += cut;
	}
	va_start(ap, fmt);
	n = vsnprintf(log_buf + log_len, LOG_SIZE - log_len, fmt, ap);
	va_end(ap);
	if (n > 0)
		log_len += (u32)n < LOG_SIZE - log_len ? (u32)n : LOG_SIZE - 1 - log_len;
	LWP_MutexUnlock(log_lock);
	return n;
}

static bool mon_send(s32 fd, const char *p, u32 len)
{
	while (len > 0) {
		s32 n = net_send(fd, p, len > 4096 ? 4096 : len, 0);

		if (n <= 0)
			return false;
		p += n;
		len -= (u32)n;
	}
	return true;
}

/* A line of pen state and transfer counts, whenever it changes. */
static int mon_status(char *out, int size)
{
	tb_sample s;
	tb_stats st;
	char hex[3 * 32 + 1] = "";
	int i;

	TB_GetState(&s);
	TB_GetStats(&st);
	for (i = 0; i < st.last_len && i < 32; i++)
		sprintf(hex + i * 3, "%02x ", st.last[i]);
	return snprintf(out, size,
	                "%s range %d tip %d btn %x eraser %d x %d y %d p %u tilt %d,%d"
	                " pad %04x | reports %u pen %u other %u err %u ret %d | %s\n",
	                TB_Connected() ? "on " : "off", s.in_range,
	                (s.buttons & TB_BUTTON_TIP) != 0, s.buttons, s.eraser,
	                (int)s.x, (int)s.y, (unsigned)s.raw_pressure, s.tilt_x, s.tilt_y,
	                s.pad, (unsigned)st.reports, (unsigned)st.pen_reports,
	                (unsigned)st.other, (unsigned)st.errors, (int)st.last_ret, hex);
}

static void *mon_main(void *arg)
{
	struct sockaddr_in sa;
	s32 sock;
	int tries;

	(void)arg;

	for (tries = 0; net_gethostip() == 0 && tries < 3; tries++) {
		char a[16] = "", m[16] = "", g[16] = "";

		s32 r;

		snprintf(net_state, sizeof(net_state), "net: connecting (%d)", tries + 1);
		r = if_config(a, m, g, true, 20);
		mon_log("network: if_config %d\n", (int)r);
		snprintf(net_state, sizeof(net_state), "net: if_config %d", (int)r);
	}
	if (net_gethostip() == 0) {
		mon_log("network: no address\n");
		snprintf(net_state, sizeof(net_state), "net: no address");
		return NULL;
	}

	sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0) {
		mon_log("network: socket %d\n", (int)sock);
		snprintf(net_state, sizeof(net_state), "net: socket %d", (int)sock);
		return NULL;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(MON_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if (net_bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    net_listen(sock, 1) < 0) {
		snprintf(net_state, sizeof(net_state), "net: cannot listen");
		net_close(sock);
		return NULL;
	}
	{
		u32 ip = net_gethostip();

		mon_log("monitor on %u.%u.%u.%u:%d\n", (unsigned)(ip >> 24),
		        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 8) & 0xFF),
		        (unsigned)(ip & 0xFF), MON_PORT);
		snprintf(net_state, sizeof(net_state), "net: %u.%u.%u.%u:%d",
		         (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xFF),
		         (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF), MON_PORT);
	}

	while (!mon_quit) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		char last[512] = "", line[512];
		u32 sent = 0, cut = log_cut; /* `sent` counts from the buffer's start */
		s32 fd = net_accept(sock, (struct sockaddr *)&peer, &plen);

		if (fd < 0) {
			usleep(100 * 1000);
			continue;
		}

		while (!mon_quit) {
			struct pollsd pd = { fd, POLLIN, 0 };
			bool ok = true;
			int n;

			/* New log text first, then the state if it moved. */
			LWP_MutexLock(log_lock);
			if (log_cut != cut) {
				sent = sent > log_cut - cut ? sent - (log_cut - cut) : 0;
				cut = log_cut;
			}
			if (sent < log_len) {
				ok = mon_send(fd, log_buf + sent, log_len - sent);
				sent = log_len;
			}
			LWP_MutexUnlock(log_lock);

			n = mon_status(line, sizeof(line));
			if (ok && strcmp(line, last) != 0) {
				ok = mon_send(fd, line, (u32)n);
				strcpy(last, line);
			}
			if (!ok)
				break;

			if (net_poll(&pd, 1, 20) > 0 && (pd.revents & POLLIN)) {
				char c[16];
				s32 r = net_recv(fd, c, sizeof(c), 0);

				if (r <= 0)
					break;
				if (memchr(c, 'q', (size_t)r))
					mon_quit = true;
			}
		}
		net_close(fd);
	}
	net_close(sock);
	return NULL;
}

static void line(int row, const char *fmt, ...);

/* The driver log's last line, on status row 5. */
static void log_tail(void)
{
	char tail[2][96] = { "", "" };
	u32 end, start;
	int row;

	LWP_MutexLock(log_lock);
	end = log_len;
	for (row = 1; row >= 0 && end > 0; row--) {
		u32 n;

		while (end > 0 && log_buf[end - 1] == '\n')
			end--;
		start = end;
		while (start > 0 && log_buf[start - 1] != '\n')
			start--;
		n = end - start < sizeof(tail[0]) - 1 ? end - start : sizeof(tail[0]) - 1;
		memcpy(tail[row], log_buf + start, n);
		tail[row][n] = '\0';
		end = start;
	}
	LWP_MutexUnlock(log_lock);

	line(5, "%s", tail[1]);
}

/* The whole driver log over the canvas, its last lines that fit, until B is
 * pressed again.  The console takes the whole screen for it and goes back to
 * the status bar after. */
static void show_log(void)
{
	static char copy[LOG_SIZE];
	tb_sample drop;
	int cols, rows, n = 0, col = 0;
	u32 len, start;

	CON_Init(xfb, 0, 0, fb_w, fb_h, fb_w * VI_DISPLAY_PIX_SZ);
	CON_GetMetrics(&cols, &rows);

	LWP_MutexLock(log_lock);
	len = log_len;
	memcpy(copy, log_buf, len);
	LWP_MutexUnlock(log_lock);
	copy[len] = '\0';

	/* Back from the end to the start of the last rows - 2 lines. */
	start = len;
	while (start > 0 && n < rows - 2) {
		start--;
		if (copy[start] == '\n' && start + 1 < len)
			n++;
	}
	if (start > 0)
		start++;
	printf("\x1b[1;0H Driver log (B to go back)\n");
	for (; start < len; start++) {
		/* Wrap nothing: a long line is cut at the screen's edge. */
		if (copy[start] == '\n')
			col = 0;
		else if (++col >= cols - 1)
			continue;
		putchar(copy[start]);
	}
	fflush(stdout);

	for (;;) {
		VIDEO_WaitVSync();
		INPUT_Update();
		if (INPUT_ButtonsDown() & (WPAD_BUTTON_B | WPAD_BUTTON_HOME))
			break;
	}

	/* What the pen did meanwhile was not seen being drawn; drop it. */
	while (TB_Pop(&drop))
		;
	CON_Init(xfb, 0, 0, fb_w, TEXT_H, fb_w * VI_DISPLAY_PIX_SZ);
	memcpy(xfb + BAR_H * words_per_row, canvas + BAR_H * words_per_row,
	       (size_t)(fb_h - BAR_H) * words_per_row * 4);
}

/* ---- main ---------------------------------------------------------------- */

/* One status line, padded to the full width without wrapping (which would
 * scroll the three-line console). */
static void line(int row, const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("\x1b[%d;0H%-79.79s", row, buf);
}

static void status(const tb_sample *s, const char *note)
{
	tb_info info;
	tb_stats st;
	char hex[3 * 12 + 1] = "";
	int i;

	TB_GetStats(&st);
	for (i = 0; i < st.last_len && i < 12; i++)
		sprintf(hex + i * 3, "%02x ", st.last[i]);

	if (TB_GetInfo(&info))
		line(1, " %s  %dx%d  pressure %u%s%s%s%s  if %u ep %02x/%u", info.name,
		     (int)info.max_x, (int)info.max_y, (unsigned)info.max_pressure,
		     info.has_touch ? "  touch" : "",
		     info.has_tilt ? "  tilt" : "", info.has_eraser ? "  eraser" : "",
		     info.legacy ? "  (old protocol)" : "", info.interface,
		     info.endpoint, info.max_packet);
	else
		line(1, " No tablet -- plug one into a rear USB port");

	line(2, " brush %d  pen %s %d,%d  %d%%  tilt %d,%d  keys %04x  %s", brush,
	     s->finger ? (s->in_range ? "finger" : "lifted") :
	     !s->in_range ? "away" : s->eraser ? "eraser" :
	     (s->buttons & TB_BUTTON_TIP) ? "down" : "hover",
	     (int)s->x, (int)s->y, (int)(s->pressure * 100), s->tilt_x, s->tilt_y,
	     s->pad, note);
	line(3, " reports %u  pen %u  touch %u  other %u  errors %u (%d)  last: %s",
	     (unsigned)st.reports, (unsigned)st.pen_reports,
	     (unsigned)st.touch_reports, (unsigned)st.other,
	     (unsigned)st.errors, (int)st.last_ret, hex);
	line(4, " opens %u  proto %d  mode %d/%d (%02x)  err %d %d %d  %s",
	     (unsigned)st.opens, (int)st.proto_ret, (int)st.mode_set, (int)st.mode_get,
	     st.mode_val, st.nfirst > 0 ? (int)st.first_err[0] : 0,
	     st.nfirst > 1 ? (int)st.first_err[1] : 0,
	     st.nfirst > 2 ? (int)st.first_err[2] : 0, net_state);
	log_tail();
}

static void swatch(void)
{
	int x, y;
	u8 Y, cb, cr;
	u32 w;

	to_yuv(palette[cur_colour], &Y, &cb, &cr);
	w = ((u32)Y << 24) | ((u32)cb << 16) | ((u32)Y << 8) | cr;
	for (y = TEXT_H; y < BAR_H; y++)
		for (x = 0; x < words_per_row; x++)
			xfb[y * words_per_row + x] = w;
}

int main(void)
{
	tb_sample s, last = { 0 }, last_pen = { 0 };
	stroke strokes[2] = { { 0 } }; /* pen, finger */
	u16 pad_prev = 0;
	u8 btn_prev = 0;
	char note[64] = "";
	int note_frames = 0;

	/* A fresh IOS, as the Homebrew Channel leaves its own network and USB
	 * state behind; on it the network comes up reliably. */
	IOS_ReloadIOS(IOS_GetVersion());

	{
		lwp_t esc;

		SYS_SetResetCallback(reset_pressed);
		SYS_SetPowerCallback(power_pressed);
		LWP_CreateThread(&esc, escape_main, NULL, escape_stack,
		                 sizeof(escape_stack), LWP_PRIO_HIGHEST - 1);
	}

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	fb_w = rmode->fbWidth;
	fb_h = rmode->xfbHeight;
	words_per_row = fb_w / 2;
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	canvas = malloc((size_t)fb_h * words_per_row * 4);
	rgb = malloc((size_t)fb_h * fb_w * 3);
	if (!canvas || !rgb)
		leave();

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	/* The console is the status bar: the top three rows only. */
	console_init(xfb, 0, 0, fb_w, TEXT_H, fb_w * VI_DISPLAY_PIX_SZ);
	clear_canvas();

	/* No USB mouse: libogc's mouse driver would read the tablet too, if it
	 * shows up as one, and steal half its reports. */
	INPUT_InitEx(INPUT_INIT_NO_MOUSE);
	INPUT_SetScreenSize(fb_w, fb_h);
	LWP_MutexInit(&log_lock, false);
	TB_SetLog(mon_log);
	TB_Init();
	sd_ok = fatInitDefault();
	{
		lwp_t mon;

		LWP_CreateThread(&mon, mon_main, NULL, mon_stack, sizeof(mon_stack), 40);
	}

	while (1) {
		u32 down;
		bool changed = false;

		VIDEO_WaitVSync();
		INPUT_Update();
		down = INPUT_ButtonsDown();

		if ((down & WPAD_BUTTON_HOME) || mon_quit)
			break;

		while (TB_Pop(&s)) {
			u16 pad_down = s.pad & ~pad_prev;
			u8 btn_down = s.buttons & ~btn_prev;
			bool erasing = s.eraser || (s.buttons & TB_BUTTON_BARREL2);
			bool tip = s.in_range &&
			           ((s.buttons & TB_BUTTON_TIP) || s.raw_pressure > 0);
			int sx, sy;

			stroke *k = &strokes[s.finger ? 1 : 0];

			pad_prev = s.pad;
			if (s.finger)
				btn_down = 0; /* a finger has no side switches */
			else
				btn_prev = s.buttons;
			if (pad_down & 1) cur_colour = (cur_colour + 1) % NCOLOURS;
			if (pad_down & 2) brush = brush > 1 ? brush - 1 : 1;
			if (pad_down & 4) brush = brush < MAX_SIZE ? brush + 1 : MAX_SIZE;
			if (pad_down & 8) clear_canvas();
			if (btn_down & TB_BUTTON_BARREL)
				cur_colour = (cur_colour + 1) % NCOLOURS;

			TB_ToScreen(&s, fb_w, fb_h - BAR_H, &sx, &sy);
			sy += BAR_H;

			if (tip) {
				float r = brush * (erasing ? 2.5f : 1.0f) *
				          (0.15f + 0.85f * s.pressure);
				colour c = erasing ? paper : palette[cur_colour];

				if (r < 0.6f)
					r = 0.6f;
				restore(cur_x, cur_y);
				cur_x = -1;
				if (k->drawing && s.time - k->time <= JOIN_MS &&
				    hypotf((float)sx - k->x, (float)sy - k->y) <= JOIN_PX)
					segment(k->x, k->y, k->r, (float)sx, (float)sy, r, c);
				else
					disc((float)sx, (float)sy, r, c);
				k->x = (float)sx;
				k->y = (float)sy;
				k->r = r;
				k->time = s.time;
				k->drawing = true;
				k->lifted = 0;
			} else if (k->drawing && (!s.in_range || ++k->lifted >= LIFT_REPORTS)) {
				k->drawing = false;
				k->lifted = 0;
			}
			last = s;
			if (!s.finger)
				last_pen = s;
			changed = true;
		}

		if (down & WPAD_BUTTON_B) {
			show_log();
			cur_x = -1;
			continue;
		}
		if (down & WPAD_BUTTON_A)     cur_colour = (cur_colour + 1) % NCOLOURS;
		if (down & WPAD_BUTTON_MINUS) brush = brush > 1 ? brush - 1 : 1;
		if (down & WPAD_BUTTON_PLUS)  brush = brush < MAX_SIZE ? brush + 1 : MAX_SIZE;
		if (down & WPAD_BUTTON_1)     clear_canvas();
		if (down & WPAD_BUTTON_2) {
			char path[64];

			if (!sd_ok)
				snprintf(note, sizeof(note), "no SD card");
			else if (save_bmp(path, sizeof(path)) == 0)
				snprintf(note, sizeof(note), "%s", path + 13);
			else
				snprintf(note, sizeof(note), "save failed");
			note_frames = 180;
		}
		if (note_frames > 0 && --note_frames == 0)
			note[0] = '\0';

		/* Hovering pen: a crosshair, moved without touching the canvas. */
		if (changed || cur_x >= 0) {
			int sx, sy;

			restore(cur_x, cur_y);
			cur_x = -1;
			if (last_pen.in_range && !(last_pen.buttons & TB_BUTTON_TIP)) {
				TB_ToScreen(&last_pen, fb_w, fb_h - BAR_H, &sx, &sy);
				sy += BAR_H;
				draw_cursor(sx, sy, last_pen.eraser ||
				            (last_pen.buttons & TB_BUTTON_BARREL2));
				cur_x = sx;
				cur_y = sy;
			}
		}

		status(&last, note);
		swatch();
	}

	leave();
	return 0;
}
