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
 *   2                                          save to sd:/sketches/
 *   HOME                                       exit
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include <gccore.h>
#include <fat.h>
#include <wiiuse/wpad.h>

#include <input_wii.h>
#include <input_tablet.h>

#define TEXT_H     48   /* three console lines of status */
#define BAR_H      54   /* the status bar, with the colour swatch under it */
#define CURSOR_R   6
#define MAX_SIZE   48

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

/* Reloading IOS first drops anything still queued in it (a tablet read
 * waits for as long as the pen is away), which would otherwise hold up the
 * Homebrew Channel's stub as it closes every descriptor. */
static void leave(void)
{
	TB_Shutdown();
	INPUT_Shutdown();
	if (sd_ok)
		fatUnmount("sd:");
	VIDEO_SetBlack(true);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	IOS_ReloadIOS(IOS_GetVersion());
	if ((STUB_MAGIC[0] == 0x53545542 && STUB_MAGIC[1] == 0x48415858) ||
	    (STUB_MAGIC[1] == 0x53545542 && STUB_MAGIC[2] == 0x48415858))
		((void (*)(void))STUB_ADDR)();
	SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
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
		line(1, " %s  %dx%d  pressure %u%s%s%s", info.name, (int)info.max_x,
		     (int)info.max_y, (unsigned)info.max_pressure,
		     info.has_tilt ? "  tilt" : "", info.has_eraser ? "  eraser" : "",
		     info.legacy ? "  (old protocol)" : "");
	else
		line(1, " No tablet -- plug one into a rear USB port");

	line(2, " brush %d  pen %s %d,%d  %d%%  tilt %d,%d  keys %04x  %s", brush,
	     !s->in_range ? "away" : s->eraser ? "eraser" :
	     (s->buttons & TB_BUTTON_TIP) ? "down" : "hover",
	     (int)s->x, (int)s->y, (int)(s->pressure * 100), s->tilt_x, s->tilt_y,
	     s->pad, note);
	line(3, " reports %u  pen %u  other %u  errors %u  last: %s",
	     (unsigned)st.reports, (unsigned)st.pen_reports, (unsigned)st.other,
	     (unsigned)st.errors, hex);
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
	tb_sample s, last = { 0 };
	bool drawing = false;
	float lx = 0, ly = 0, lr = 0;
	u16 pad_prev = 0;
	u8 btn_prev = 0;
	char note[64] = "";
	int note_frames = 0;

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
	TB_Init();
	sd_ok = fatInitDefault();

	while (1) {
		u32 down;
		bool changed = false;

		VIDEO_WaitVSync();
		INPUT_Update();
		down = INPUT_ButtonsDown();

		if (down & WPAD_BUTTON_HOME)
			break;

		while (TB_Pop(&s)) {
			u16 pad_down = s.pad & ~pad_prev;
			u8 btn_down = s.buttons & ~btn_prev;
			bool erasing = s.eraser || (s.buttons & TB_BUTTON_BARREL2);
			bool tip = s.in_range && (s.buttons & TB_BUTTON_TIP);
			int sx, sy;

			pad_prev = s.pad;
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
				if (drawing)
					segment(lx, ly, lr, (float)sx, (float)sy, r, c);
				else
					disc((float)sx, (float)sy, r, c);
				lx = (float)sx;
				ly = (float)sy;
				lr = r;
				drawing = true;
			} else {
				drawing = false;
			}
			last = s;
			changed = true;
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
			if (last.in_range && !(last.buttons & TB_BUTTON_TIP)) {
				TB_ToScreen(&last, fb_w, fb_h - BAR_H, &sx, &sy);
				sy += BAR_H;
				draw_cursor(sx, sy, last.eraser || (last.buttons & TB_BUTTON_BARREL2));
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
