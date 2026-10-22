/* Painting with the uDraw GameTablet.
 *
 * Put the Wii Remote into the tablet and draw: line width follows pen
 * pressure, and up to four tablets (one on each Wii Remote) draw at once.
 * The status bar shows what each tablet reports.
 *
 *   Upper rocker            next colour
 *   Lower rocker            eraser while held
 *   + / -                   thicker / thinner brush
 *   1                       clear
 *   2                       save to sd:/udraw/ as a BMP file
 *   HOME                    exit
 *
 * Built with HAVE_TABLET and linked with
 * -Wl,--wrap=classic_ctrl_handshake,--wrap=parse_event (see the Makefile).
 * `make TEST=1` builds a version that logs every sample to sd:/udraw/test/,
 * saves the drawing after TEST_SECS seconds and switches the Wii off, for
 * running headless in Dolphin.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include <gccore.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#include <input_wii.h>
#include <input_tablet.h>

#define TEXT_H     96   /* six console lines of status */
#define BAR_H      102  /* the status bar, with the colour swatch under it */
#define CURSOR_R   6
#define MAX_SIZE   40
#define TABLETS    4
/* A stroke is only joined to the sample before it when that came this
 * recently: further apart, reports were lost and joining them would draw a
 * stray line.  A uDraw reports every 10 ms. */
#define JOIN_MS    80

#ifndef TEST_SECS
#define TEST_SECS  20
#endif

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
	{ 40, 160, 60 },   /* green */
	{ 30, 90, 200 },   /* blue */
	{ 130, 50, 170 },  /* purple */
};
#define NCOLOURS (int)(sizeof(palette) / sizeof(palette[0]))
static const colour paper = { 250, 248, 240 };

static int brush = 6;

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
static void segment(float x0, float y0, float r0, float x1, float y1, float r1, colour c)
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

/* ---- cursors --------------------------------------------------------------- */

/* Each tablet's crosshair, drawn straight onto the screen and taken off by
 * copying the canvas back. */
static int cur_x[TABLETS] = { -1, -1, -1, -1 }, cur_y[TABLETS];

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
		memcpy(&xfb[y * words_per_row + x0], &canvas[y * words_per_row + x0], (x1 - x0 + 1) * 4);
	}
}

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

/* The drawing (below the status bar) as a 24-bit BMP. */
static bool save_bmp(const char *path)
{
	int h = fb_h - BAR_H, row = (fb_w * 3 + 3) & ~3, x, y;
	u8 head[54] = { 'B', 'M' }, *line;
	FILE *f = fopen(path, "wb");
	bool ok;

	if (!f)
		return false;
	put32(head + 2, 54 + (u32)row * h);
	put32(head + 10, 54);
	put32(head + 14, 40);
	put32(head + 18, fb_w);
	put32(head + 22, h);
	head[26] = 1;
	head[28] = 24;
	put32(head + 34, (u32)row * h);
	ok = fwrite(head, 1, 54, f) == 54;
	line = calloc(1, row);
	for (y = fb_h - 1; ok && line && y >= BAR_H; y--) {
		for (x = 0; x < fb_w; x++) {
			const u8 *p = rgb + ((u32)y * fb_w + x) * 3;

			line[x * 3] = p[2];
			line[x * 3 + 1] = p[1];
			line[x * 3 + 2] = p[0];
		}
		ok = fwrite(line, 1, row, f) == (size_t)row;
	}
	free(line);
	fclose(f);
	return ok && line;
}

static void save_next(char *note, int size)
{
	char path[64];
	struct stat st;
	int n;

	if (!sd_ok) {
		snprintf(note, size, "no SD card");
		return;
	}
	mkdir("sd:/udraw", 0777);
	for (n = 1; n < 1000; n++) {
		snprintf(path, sizeof(path), "sd:/udraw/udraw%03d.bmp", n);
		if (stat(path, &st) != 0)
			break;
	}
	snprintf(note, size, save_bmp(path) ? "saved %s" : "could not save %s", path + 3);
}

/* ---- the test build's log ---------------------------------------------- */

#ifdef UDRAW_TEST
static FILE *test_log;

static void tlog(const char *fmt, ...)
{
	va_list ap;

	if (!test_log)
		return;
	va_start(ap, fmt);
	vfprintf(test_log, fmt, ap);
	va_end(ap);
	fflush(test_log);
}
#else
#define tlog(...) ((void)0)
#endif

/* ---- status -------------------------------------------------------------- */

static void line(int row, const char *fmt, ...)
{
	char buf[96];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* the console is 80 columns: pad so the old text goes */
	printf("\x1b[%d;0H%-79.79s", row, buf);
}

static void swatch(int colour_of[TABLETS])
{
	int i, x, y;

	for (i = 0; i < TABLETS; i++) {
		colour c = palette[colour_of[i]];
		u8 Y, cb, cr;
		u32 w;

		to_yuv(c, &Y, &cb, &cr);
		w = ((u32)Y << 24) | ((u32)cb << 16) | ((u32)Y << 8) | cr;
		for (y = TEXT_H; y < BAR_H; y++)
			for (x = i * words_per_row / TABLETS; x < (i + 1) * words_per_row / TABLETS; x++)
				xfb[y * words_per_row + x] = w;
	}
}

/* ---- strokes ------------------------------------------------------------- */

typedef struct {
	bool drawing;
	float x, y, r;
	u32 time;
	u8 buttons;      /* the rockers as last seen, for presses */
} stroke;

int main(void)
{
	stroke strokes[TABLETS] = { { 0 } };
	int colour_of[TABLETS] = { 0, 1, 4, 3 };
	tb_sample s, now[TABLETS] = { { 0 } };
	char note[64] = "";
	int note_frames = 0, i, frame = 0;
	u32 samples = 0;

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(NULL);
	fb_w = rmode->fbWidth;
	fb_h = rmode->xfbHeight;
	words_per_row = fb_w / 2;
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	canvas = malloc((size_t)fb_h * words_per_row * 4);
	rgb = malloc((size_t)fb_h * fb_w * 3);
	if (!canvas || !rgb)
		return 1;

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	console_init(xfb, 0, 0, fb_w, TEXT_H, fb_w * VI_DISPLAY_PIX_SZ);
	clear_canvas();

	/* WPAD is all a uDraw needs; TB_Init would add USB tablets. */
	INPUT_InitEx(INPUT_INIT_NO_MOUSE | INPUT_INIT_NO_KEYBOARD);
	INPUT_SetScreenSize(fb_w, fb_h);
	sd_ok = fatInitDefault();
#ifdef UDRAW_TEST
	if (sd_ok) {
		mkdir("sd:/udraw", 0777);
		mkdir("sd:/udraw/test", 0777);
		test_log = fopen("sd:/udraw/test/log.txt", "w");
	}
	tlog("start, sd %d\n", sd_ok);
#endif

	while (1) {
		u32 down;

		VIDEO_WaitVSync();
		INPUT_Update();
		down = INPUT_ButtonsDown();
		frame++;

		if (down & WPAD_BUTTON_HOME)
			break;
		if (down & WPAD_BUTTON_PLUS)
			brush = brush < MAX_SIZE ? brush + 1 : MAX_SIZE;
		if (down & WPAD_BUTTON_MINUS)
			brush = brush > 1 ? brush - 1 : 1;
		if (down & WPAD_BUTTON_1)
			clear_canvas();
		if (down & WPAD_BUTTON_2) {
			save_next(note, sizeof(note));
			note_frames = 180;
		}

		for (i = 0; i < TABLETS; i++)
			restore(cur_x[i], cur_y[i]);

		while (TB_Pop(&s)) {
			stroke *k;
			bool erasing;
			u8 pressed;
			int sx, sy;

			if (s.wiimote < 0 || s.wiimote >= TABLETS)
				continue; /* a USB tablet; this demo has none open */
			k = &strokes[(int)s.wiimote];
			now[(int)s.wiimote] = s;
			samples++;
			pressed = s.buttons & ~k->buttons;
			if (pressed & TB_BUTTON_BARREL2) {
				colour_of[(int)s.wiimote] = (colour_of[(int)s.wiimote] + 1) % NCOLOURS;
				tlog("%u wiimote %d upper rocker: colour %d\n", s.time, s.wiimote, colour_of[(int)s.wiimote]);
			}
			if (pressed & TB_BUTTON_BARREL)
				tlog("%u wiimote %d lower rocker: eraser\n", s.time, s.wiimote);
			erasing = s.buttons & TB_BUTTON_BARREL;
			k->buttons = s.buttons;

			TB_ToScreen(&s, fb_w, fb_h - BAR_H, &sx, &sy);
			sy += BAR_H;

			if (s.in_range && (s.buttons & TB_BUTTON_TIP)) {
				float r = brush * (erasing ? 3.0f : 1.0f) * (0.25f + 1.5f * s.pressure);
				colour c = erasing ? paper : palette[colour_of[(int)s.wiimote]];

				if (k->drawing && s.time - k->time <= JOIN_MS)
					segment(k->x, k->y, k->r, sx, sy, r, c);
				else {
					disc(sx, sy, r, c);
					tlog("%u wiimote %d down at %d,%d raw %d,%d pressure %u\n", s.time, s.wiimote, sx, sy,
					     s.x, s.y, s.raw_pressure);
				}
				k->drawing = true;
				k->x = sx;
				k->y = sy;
				k->r = r;
				k->time = s.time;
			} else if (k->drawing) {
				k->drawing = false;
				tlog("%u wiimote %d up at %d,%d\n", s.time, s.wiimote, (int)k->x, (int)k->y);
			}
		}

		/* the status bar, every few frames: printing is slow */
		if (frame % 6 == 0) {
			line(1, "uDraw GameTablet demo   brush %d   %u samples   %s", brush, samples,
			     note_frames > 0 ? note : "");
			for (i = 0; i < TABLETS; i++) {
				const tb_sample *n = &now[i];

				if (!TB_UDrawConnected(i))
					line(2 + i, "Wii Remote %d: no uDraw", i + 1);
				else if (!n->in_range)
					line(2 + i, "Wii Remote %d: uDraw, pen away%s%s", i + 1,
					     n->buttons & TB_BUTTON_BARREL2 ? "  upper" : "",
					     n->buttons & TB_BUTTON_BARREL ? "  lower" : "");
				else
					line(2 + i, "Wii Remote %d: uDraw, pen at %4d,%4d  pressure %3u/511  %s%s%s", i + 1,
					     n->x, n->y, n->raw_pressure, n->buttons & TB_BUTTON_TIP ? "touching" : "hovering",
					     n->buttons & TB_BUTTON_BARREL2 ? "  upper" : "",
					     n->buttons & TB_BUTTON_BARREL ? "  lower" : "");
			}
			swatch(colour_of);
		}
		if (note_frames > 0)
			note_frames--;

		/* every tablet's pen, hovering or touching */
		for (i = 0; i < TABLETS; i++) {
			cur_x[i] = -1;
			if (TB_UDrawConnected(i) && now[i].in_range) {
				TB_ToScreen(&now[i], fb_w, fb_h - BAR_H, &cur_x[i], &cur_y[i]);
				cur_y[i] += BAR_H;
				draw_cursor(cur_x[i], cur_y[i], now[i].buttons & TB_BUTTON_BARREL);
			}
		}

#ifdef UDRAW_TEST
		if (frame % 60 == 0) {
			tb_info info;
			bool any = TB_GetInfo(&info);

			TB_GetState(&s);
			tlog("%d s: samples %u, connected %d%d%d%d, info %s %d x %d, state wiimote %d in_range %d x %d y %d "
			     "pressure %u buttons %x\n", frame / 60, samples, TB_UDrawConnected(0), TB_UDrawConnected(1),
			     TB_UDrawConnected(2), TB_UDrawConnected(3), any ? info.name : "-", any ? info.max_x : 0,
			     any ? info.max_y : 0, s.wiimote, s.in_range, s.x, s.y, s.raw_pressure, s.buttons);
		}
		if (frame >= TEST_SECS * 60) {
			tlog("saved %d\n", save_bmp("sd:/udraw/test/canvas.bmp"));
			tlog("done\n");
			fclose(test_log);
			fatUnmount("sd:");
			SYS_ResetSystem(SYS_POWEROFF, 0, 0);
		}
#endif
	}

	INPUT_Shutdown();
	if (sd_ok)
		fatUnmount("sd:");
	exit(0);
}
