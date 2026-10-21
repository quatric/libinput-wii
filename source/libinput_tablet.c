/* USB pen tablet driver for libogc homebrew.  See input_tablet.h. */

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>

#include "input_tablet.h"
#include "libinput_hidparse.h"

#define WACOM_VID     0x056A

#define HID_SET_REPORT 0x09
#define HID_GET_REPORT 0x01
#define HID_FEATURE    0x03

#define REQTYPE_STD_IF_IN    0x81
#define REQTYPE_CLASS_IF_OUT 0x21
#define REQTYPE_CLASS_IF_IN  0xA1

#define MAX_DEVICES 32
#define MAX_IFACES  4
#define CFG_MAX     512
#define RDESC_MAX   2048
#define INTR_MAX    64
#define STACKSIZE   16384
#define QUEUE       256
#define QUEUE_MASK  (QUEUE - 1)

/* Old Wacom protocol (Bamboo, CTL-480 Intuos): report 2, switched on by
 * writing 2 to feature report 2.  Byte 1 holds the flags, then X, Y and
 * pressure as little-endian 16-bit values. */
#define LEGACY_REPORT 2

/* On USBV5 the handle from USB_OpenDevice is a device_id that may read as
 * negative, so the fd's sign says nothing about whether we are open. */
static s32 tb_fd = -1;
static bool tb_opened;
static bool tb_oh0; /* opened through /dev/usb/oh0 rather than USBV5 */
static volatile bool tb_unplugged;
static volatile bool tb_running;

static tb_info tb_dev;
static hidp_desc tb_desc;
static hidp_pen tb_pen;
static u8 tb_ep;
static u16 tb_max_packet;

static tb_sample tb_state;
static tb_sample tb_queue[QUEUE];
static volatile u32 tb_head, tb_tail;
static mutex_t tb_lock = LWP_MUTEX_NULL;

static tb_stats tb_st;
static lwp_t tb_thread = LWP_THREAD_NULL;
static u8 tb_stack[STACKSIZE] ATTRIBUTE_ALIGN(32);

/* DMA buffers.  Static: a transfer that never completes may still land in
 * them after the device is gone. */
static u8 tb_ctl[RDESC_MAX] ATTRIBUTE_ALIGN(32);
static u8 tb_intr[INTR_MAX] ATTRIBUTE_ALIGN(32);

static int (*tb_logger)(const char *fmt, ...);
#define tb_log(...) (tb_logger ? (void)tb_logger(__VA_ARGS__) : (void)0)

void TB_SetLog(int (*print)(const char *fmt, ...)) { tb_logger = print; }

typedef struct {
	volatile bool done;
	volatile bool pending;
	volatile s32 res;
} tb_xfer;

static tb_xfer tb_ctl_x, tb_intr_x;

static s32 tb_xfer_cb(s32 result, void *arg)
{
	tb_xfer *x = arg;

	x->res = result;
	x->done = true;
	return 0;
}

static s32 tb_removed_cb(s32 result, void *arg)
{
	(void)result;
	(void)arg;
	tb_unplugged = true;
	return 0;
}

/* Waits for a transfer, giving up after `ms` or as soon as the device goes. */
static bool tb_wait(tb_xfer *x, int ms)
{
	int waited;

	for (waited = 0; !x->done; waited++) {
		if (tb_unplugged || (ms >= 0 && waited >= ms))
			return false;
		usleep(1000);
	}
	x->pending = false;
	return true;
}

/* A control transfer with a deadline, as libogc's synchronous calls have
 * none.  `data` is staged through tb_ctl. */
static s32 tb_ctrl(u8 type, u8 req, u16 val, u16 idx, u16 len, void *data, int ms)
{
	s32 ret;

	if (len > sizeof(tb_ctl) || tb_ctl_x.pending)
		return TB_ESTATE;

	if (!(type & 0x80) && len)
		memcpy(tb_ctl, data, len);
	else
		memset(tb_ctl, 0, len ? len : 1);
	DCFlushRange(tb_ctl, (len + 31) & ~31u);

	tb_ctl_x.done = false;
	tb_ctl_x.res = -1;
	tb_ctl_x.pending = true;
	if (type & 0x80)
		ret = USB_ReadCtrlMsgAsync(tb_fd, type, req, val, idx, len, tb_ctl,
		                           tb_xfer_cb, &tb_ctl_x);
	else
		ret = USB_WriteCtrlMsgAsync(tb_fd, type, req, val, idx, len,
		                            len ? tb_ctl : NULL, tb_xfer_cb, &tb_ctl_x);
	if (ret < 0) {
		tb_ctl_x.pending = false;
		return ret;
	}
	if (!tb_wait(&tb_ctl_x, ms))
		return -6; /* stays pending: tb_ctl outlives it */

	if ((type & 0x80) && len) {
		DCInvalidateRange(tb_ctl, (len + 31) & ~31u);
		memcpy(data, tb_ctl, len);
	}
	return tb_ctl_x.res;
}

static u16 le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

/* ---- finding the tablet -------------------------------------------------- */

typedef struct {
	u8 number;
	u8 subclass, protocol;
	u16 rdesc_len;
	u8 ep;
	u16 max_packet;
} tb_iface;

/* Every HID interface in a configuration descriptor, with its report
 * descriptor length and interrupt IN endpoint. */
static int list_hid_ifaces(const u8 *cfg, u32 total, tb_iface *out, int max)
{
	int n = 0;
	bool in_hid = false;
	u32 off = 0;

	while (off + 2 <= total) {
		const u8 *d = cfg + off;
		u8 len = d[0], type = d[1];

		if (len < 2 || off + len > total)
			break;

		if (type == USB_DT_INTERFACE && len >= 9) {
			in_hid = d[5] == USB_CLASS_HID && d[3] == 0 && n < max;
			if (in_hid) {
				memset(&out[n], 0, sizeof(out[n]));
				out[n].number = d[2];
				out[n].subclass = d[6];
				out[n].protocol = d[7];
				n++;
			}
		} else if (in_hid && type == USB_DT_HID && len >= 9) {
			/* Report descriptors are listed from byte 6 on. */
			int i;

			for (i = 6; i + 3 <= len; i += 3)
				if (d[i] == USB_DT_REPORT) {
					out[n - 1].rdesc_len = le16(d + i + 1);
					break;
				}
		} else if (in_hid && type == USB_DT_ENDPOINT && len >= 7) {
			if ((d[2] & 0x80) && (d[3] & 0x03) == 0x03 && !out[n - 1].ep) {
				out[n - 1].ep = d[2];
				out[n - 1].max_packet = le16(d + 4) & 0x7FF;
			}
		}
		off += len;
	}
	return n;
}

static void read_name(u8 index, char *out, u32 size)
{
	u8 buf[64];
	s32 ret;
	u32 i, n = 0;

	snprintf(out, size, "%04x:%04x", tb_dev.vid, tb_dev.pid);
	if (index == 0)
		return;

	ret = tb_ctrl(0x80, USB_REQ_GETDESCRIPTOR, (USB_DT_STRING << 8) | index,
	              0x0409, sizeof(buf), buf, 300);
	if (ret < 4 || buf[1] != USB_DT_STRING)
		return;
	if (buf[0] < (u32)ret)
		ret = buf[0];
	for (i = 2; i + 1 < (u32)ret && n + 1 < size; i += 2) {
		u16 c = le16(buf + i);

		if (c == 0)
			break;
		out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
	}
	while (n > 0 && out[n - 1] == ' ')
		n--;
	if (n > 0)
		out[n] = '\0';
}

static s32 set_feature(u8 iface, const u8 *report, u16 len)
{
	return tb_ctrl(REQTYPE_CLASS_IF_OUT, HID_SET_REPORT,
	               (u16)((HID_FEATURE << 8) | report[0]), iface, len,
	               (void *)report, 300);
}

/* Takes the tablet out of the mouse mode it powers up in, where there is
 * one.  Descriptor-described tablets say how with an Input Mode feature;
 * old Wacoms take a 2 in feature report 2. */
static void switch_mode(const tb_iface *ifc)
{
	u8 rep[64];
	int tries;

	if (tb_dev.legacy) {
		for (tries = 0; tries < 5; tries++) {
			s32 ret;

			rep[0] = LEGACY_REPORT;
			rep[1] = 2;
			ret = set_feature(ifc->number, rep, 2);
			if (ret >= 0) {
				memset(rep, 0, 2);
				ret = tb_ctrl(REQTYPE_CLASS_IF_IN, HID_GET_REPORT,
				              (HID_FEATURE << 8) | LEGACY_REPORT,
				              ifc->number, 2, rep, 300);
			}
			tb_log("  tablet mode: %d (%02x %02x)\n", (int)ret, rep[0], rep[1]);
			if (ret >= 2 && rep[1] == 2)
				break;
		}
		return;
	}

	if (tb_pen.input_mode) {
		const hidp_field *f = tb_pen.input_mode;
		u32 len = hidp_report_bytes(&tb_desc, HIDP_FEATURE, f->report_id);
		u32 skip = tb_desc.uses_ids ? 1 : 0;

		if (len > sizeof(rep))
			return;
		memset(rep, 0, sizeof(rep));
		rep[0] = f->report_id;
		hidp_put(f, rep + skip, len - skip, 2);
		tb_log("  input mode (report %u): %d\n", f->report_id,
		       (int)set_feature(ifc->number, rep, (u16)len));
	}
}

/* Reads one HID interface's report descriptor and decides whether it is the
 * tablet's pen interface.  Fills tb_desc and tb_pen when it is. */
static bool probe_iface(const tb_iface *ifc)
{
	static u8 rdesc[RDESC_MAX];
	u16 len = ifc->rdesc_len;
	s32 ret;

	if (len == 0 || len > RDESC_MAX)
		len = RDESC_MAX;

	ret = tb_ctrl(REQTYPE_STD_IF_IN, USB_REQ_GETDESCRIPTOR, USB_DT_REPORT << 8,
	              ifc->number, len, rdesc, 500);
	if (ret <= 0) {
		tb_log("  if %u: no report descriptor (%d)\n", ifc->number, (int)ret);
		return false;
	}
	if (ret > len)
		ret = len;
	if (!hidp_parse(rdesc, (u32)ret, &tb_desc)) {
		tb_log("  if %u: report descriptor unreadable (%d bytes)\n",
		       ifc->number, (int)ret);
		return false;
	}
	if (hidp_find_pen(&tb_desc, &tb_pen)) {
		tb_log("  if %u: pen in report %u, %d fields\n", ifc->number,
		       tb_pen.report_id, tb_desc.nfields);
		return true;
	}
	tb_log("  if %u: %d fields, no pen\n", ifc->number, tb_desc.nfields);
	return false;
}

/* With a device open, decides whether it is a tablet and sets it up. */
static bool probe_device(u16 vid, u16 pid)
{
	static u8 cfg[CFG_MAX];
	tb_iface ifaces[MAX_IFACES];
	u8 dev[18];
	u16 total;
	s32 ret;
	int n, i, pick = -1;

	ret = tb_ctrl(0x80, USB_REQ_GETDESCRIPTOR, USB_DT_DEVICE << 8, 0,
	              sizeof(dev), dev, 300);
	if (ret < 18)
		return false;
	ret = tb_ctrl(0x80, USB_REQ_GETDESCRIPTOR, USB_DT_CONFIG << 8, 0, 9, cfg, 300);
	if (ret < 9)
		return false;
	total = le16(cfg + 2);
	if (total > CFG_MAX)
		total = CFG_MAX;
	ret = tb_ctrl(0x80, USB_REQ_GETDESCRIPTOR, USB_DT_CONFIG << 8, 0, total,
	              cfg, 500);
	if (ret < 9)
		return false;
	if ((u32)ret < total)
		total = (u16)ret;

	n = list_hid_ifaces(cfg, total, ifaces, MAX_IFACES);
	if (n == 0)
		return false;

	memset(&tb_dev, 0, sizeof(tb_dev));
	tb_dev.vid = vid;
	tb_dev.pid = pid;

	for (i = 0; i < n && pick < 0; i++)
		if (ifaces[i].ep && probe_iface(&ifaces[i]))
			pick = i;

	/* A Wacom that describes no pen is one of the old fixed-format ones. */
	if (pick < 0 && vid == WACOM_VID && ifaces[0].ep) {
		pick = 0;
		memset(&tb_desc, 0, sizeof(tb_desc));
		memset(&tb_pen, 0, sizeof(tb_pen));
		tb_dev.legacy = true;
		tb_log("  no pen described: old Wacom protocol\n");
	}
	if (pick < 0)
		return false;

	tb_dev.interface = ifaces[pick].number;
	tb_ep = ifaces[pick].ep;
	tb_max_packet = ifaces[pick].max_packet;
	if (tb_max_packet == 0 || tb_max_packet > INTR_MAX)
		tb_max_packet = INTR_MAX;

	if (tb_dev.legacy) {
		/* Intuos S/M ranges; grown if the pen goes further. */
		tb_dev.report_id = LEGACY_REPORT;
		tb_dev.max_x = 15200;
		tb_dev.max_y = 9500;
		tb_dev.max_pressure = 1023;
	} else {
		tb_dev.report_id = tb_pen.report_id;
		tb_dev.max_x = tb_pen.x->logical_max;
		tb_dev.max_y = tb_pen.y->logical_max;
		tb_dev.max_pressure = tb_pen.pressure ? (u32)tb_pen.pressure->logical_max : 1;
		tb_dev.has_tilt = tb_pen.tilt_x && tb_pen.tilt_y;
		tb_dev.has_eraser = tb_pen.eraser || tb_pen.invert;
		tb_dev.pad_buttons = (u8)tb_pen.npad;
	}
	if (tb_dev.max_x <= 0) tb_dev.max_x = 1;
	if (tb_dev.max_y <= 0) tb_dev.max_y = 1;
	if (tb_dev.max_pressure == 0) tb_dev.max_pressure = 1;
	read_name(dev[15], tb_dev.name, sizeof(tb_dev.name));

	if (ifaces[pick].subclass == 1 && ifaces[pick].protocol == 2)
		tb_log("  (boot mouse interface: libogc's mouse driver may claim it"
		       " too -- INPUT_InitEx(INPUT_INIT_NO_MOUSE) avoids that)\n");

	/* Report protocol, in case a mouse driver left it in boot protocol. */
	(void)tb_ctrl(REQTYPE_CLASS_IF_OUT, USB_REQ_SETPROTOCOL, 1,
	              tb_dev.interface, 0, NULL, 300);
	switch_mode(&ifaces[pick]);

	tb_log("  %s: if %u ep %02x, %dx%d, pressure %u%s%s, %u buttons\n",
	       tb_dev.name, tb_dev.interface, tb_ep, (int)tb_dev.max_x,
	       (int)tb_dev.max_y, (unsigned)tb_dev.max_pressure,
	       tb_dev.has_tilt ? ", tilt" : "", tb_dev.has_eraser ? ", eraser" : "",
	       tb_dev.pad_buttons);
	return true;
}

/* Opens each HID device in turn, keeping the first that is a tablet.
 * USBV0 IOSes open devices by VID/PID through /dev/usb/oh0, USBV5 ones by
 * the device_id the list hands back. */
static s32 open_tablet(void)
{
	static usb_device_entry list[MAX_DEVICES] ATTRIBUTE_ALIGN(32);
	u8 count = 0;
	bool oh0;
	s32 fd;
	int i;

	fd = IOS_Open("/dev/usb/oh0", 0);
	oh0 = fd >= 0;
	if (oh0)
		IOS_Close(fd);

	memset(list, 0, sizeof(list));
	if (USB_GetDeviceList(list, MAX_DEVICES, USB_CLASS_HID, &count) < 0)
		return TB_ENODEV;
	tb_oh0 = oh0;

	for (i = 0; i < count; i++) {
		u16 vid = list[i].vid, pid = list[i].pid;

		if ((vid == 0 && pid == 0) || (vid == 0x057E && pid == 0x0308))
			continue;

		tb_unplugged = false;
		if (USB_OpenDevice(oh0 ? USB_OH0_DEVICE_ID : list[i].device_id,
		                   vid, pid, &tb_fd) < 0)
			continue;

		if (probe_device(vid, pid)) {
			tb_opened = true;
			USB_DeviceRemovalNotifyAsync(tb_fd, tb_removed_cb, NULL);
			return TB_OK;
		}
		USB_CloseDevice(&tb_fd);
		tb_fd = -1;
	}
	return TB_ENODEV;
}

static void close_tablet(void)
{
	if (!tb_opened)
		return;

	/* A read stays outstanding while the pen is away.  Under oh0 closing
	 * is a real IOS_Close, which waits for it -- forever, if nobody
	 * touches the tablet -- so give it a moment and otherwise leave the
	 * handle to the IOS reload that ends the app.  USBV5's close never
	 * reaches IOS. */
	if (tb_oh0 && tb_intr_x.pending && !tb_wait(&tb_intr_x, 50))
		tb_log("  tablet left open: a read is outstanding\n");
	else
		USB_CloseDevice(&tb_fd);
	tb_fd = -1;
	tb_opened = false;
	/* Whatever was outstanding belongs to the old device now. */
	tb_intr_x.pending = false;
	tb_ctl_x.pending = false;
	tb_log("  tablet closed\n");
}

/* ---- reports ------------------------------------------------------------- */

static void push(const tb_sample *s)
{
	u32 next;

	LWP_MutexLock(tb_lock);
	tb_state = *s;
	next = (tb_head + 1) & QUEUE_MASK;
	if (next == tb_tail)
		tb_tail = (tb_tail + 1) & QUEUE_MASK; /* drop the oldest */
	tb_queue[tb_head] = *s;
	tb_head = next;
	LWP_MutexUnlock(tb_lock);
}

static s32 get(const hidp_field *f, const u8 *data, u32 len)
{
	return f ? hidp_get(f, data, len) : 0;
}

static void finish(tb_sample *s)
{
	if (s->x < 0) s->x = 0;
	if (s->y < 0) s->y = 0;
	if (s->x > tb_dev.max_x) s->x = tb_dev.max_x;
	if (s->y > tb_dev.max_y) s->y = tb_dev.max_y;
	if (s->raw_pressure > tb_dev.max_pressure)
		s->raw_pressure = tb_dev.max_pressure;
	s->nx = (float)s->x / (float)tb_dev.max_x;
	s->ny = (float)s->y / (float)tb_dev.max_y;
	s->pressure = (float)s->raw_pressure / (float)tb_dev.max_pressure;
}

static void legacy_report(const u8 *r, u32 len)
{
	tb_sample s = tb_state;
	bool prox;

	if (r[0] != LEGACY_REPORT || len < 8) {
		tb_st.other++;
		return;
	}
	tb_st.pen_reports++;

	s.in_range = (r[1] & 0x80) != 0;
	prox = (r[1] & 0x40) != 0;
	s.buttons = 0;
	s.raw_pressure = 0;
	if (r[1] & 0x20) { /* data ready */
		s.raw_pressure = le16(r + 6);
		if (r[1] & 0x01) s.buttons |= TB_BUTTON_TIP;
		if (r[1] & 0x02) s.buttons |= TB_BUTTON_BARREL;
		if (r[1] & 0x04) s.buttons |= TB_BUTTON_BARREL2;
	}
	if (prox) {
		s.x = le16(r + 2);
		s.y = le16(r + 4);
		s.eraser = (r[1] & 0x08) != 0;
		/* The range table is a guess; let the pen correct it. */
		if (s.x > tb_dev.max_x) tb_dev.max_x = s.x;
		if (s.y > tb_dev.max_y) tb_dev.max_y = s.y;
		if (s.raw_pressure > tb_dev.max_pressure)
			tb_dev.max_pressure = s.raw_pressure > 1023 ? 2047 : 1023;
	}
	finish(&s);
	push(&s);
}

static void hid_report(const u8 *r, u32 len)
{
	u8 id = tb_desc.uses_ids ? r[0] : 0;
	const u8 *data = tb_desc.uses_ids ? r + 1 : r;
	u32 n = tb_desc.uses_ids ? len - 1 : len;
	tb_sample s = tb_state;
	bool used = false;
	int i;

	if (len == 0)
		return;

	for (i = 0; i < tb_pen.npad; i++) {
		const hidp_field *f = tb_pen.pad[i];

		if (f->report_id != id)
			continue;
		used = true;
		if (hidp_get(f, data, n))
			s.pad |= (u16)(1u << i);
		else
			s.pad &= (u16)~(1u << i);
	}

	if (id == tb_pen.report_id) {
		used = true;
		if (tb_pen.valid && !hidp_get(tb_pen.valid, data, n))
			return;
		tb_st.pen_reports++;

		s.x = get(tb_pen.x, data, n) - tb_pen.x->logical_min;
		s.y = get(tb_pen.y, data, n) - tb_pen.y->logical_min;
		s.raw_pressure = tb_pen.pressure ? (u32)get(tb_pen.pressure, data, n) : 0;
		s.in_range = tb_pen.in_range ? get(tb_pen.in_range, data, n) != 0 : true;
		s.eraser = get(tb_pen.eraser, data, n) || get(tb_pen.invert, data, n);
		s.buttons = 0;
		if (tb_pen.tip ? get(tb_pen.tip, data, n) || get(tb_pen.eraser, data, n)
		               : s.raw_pressure > 0)
			s.buttons |= TB_BUTTON_TIP;
		if (get(tb_pen.barrel, data, n))
			s.buttons |= TB_BUTTON_BARREL;
		if (get(tb_pen.barrel2, data, n))
			s.buttons |= TB_BUTTON_BARREL2;
		s.tilt_x = (s8)get(tb_pen.tilt_x, data, n);
		s.tilt_y = (s8)get(tb_pen.tilt_y, data, n);
		if (!s.in_range) {
			s.buttons = 0;
			s.raw_pressure = 0;
		}
	}

	if (!used) {
		tb_st.other++;
		return;
	}
	finish(&s);
	push(&s);
}

static void *tb_worker(void *arg)
{
	int fails = 0;

	(void)arg;

	while (tb_running) {
		s32 ret;

		if (!tb_opened) {
			int ms;

			if (open_tablet() == TB_OK) {
				fails = 0;
				continue;
			}
			/* Nothing yet: look again in a second. */
			for (ms = 0; ms < 1000 && tb_running; ms += 10)
				usleep(10 * 1000);
			continue;
		}

		if (tb_unplugged || fails >= 20) {
			tb_sample s = tb_state;

			close_tablet();
			s.in_range = false;
			s.buttons = 0;
			s.pad = 0;
			push(&s);
			continue;
		}

		if (tb_intr_x.pending) {
			/* A read from before is still out; wait it through. */
			tb_wait(&tb_intr_x, 100);
			continue;
		}

		memset(tb_intr, 0, sizeof(tb_intr));
		DCFlushRange(tb_intr, sizeof(tb_intr));
		tb_intr_x.done = false;
		tb_intr_x.pending = true;
		ret = USB_ReadIntrMsgAsync(tb_fd, tb_ep, tb_max_packet, tb_intr,
		                           tb_xfer_cb, &tb_intr_x);
		if (ret < 0) {
			tb_intr_x.pending = false;
			tb_st.errors++;
			tb_st.last_ret = ret;
			fails++;
			usleep(10 * 1000);
			continue;
		}

		/* A pen out of range sends nothing, so no deadline here. */
		while (tb_running && !tb_unplugged && !tb_intr_x.done)
			usleep(1000);
		if (!tb_intr_x.done)
			continue;
		tb_intr_x.pending = false;

		ret = tb_intr_x.res;
		tb_st.last_ret = ret;
		if (ret < 0) {
			tb_st.errors++;
			fails++;
			usleep(5 * 1000);
			continue;
		}
		fails = 0;
		if (ret > INTR_MAX)
			ret = INTR_MAX;
		DCInvalidateRange(tb_intr, sizeof(tb_intr));

		tb_st.reports++;
		tb_st.last_len = (u8)ret;
		tb_st.last_id = ret > 0 ? tb_intr[0] : 0;
		memcpy(tb_st.last, tb_intr, (u32)ret);

		if (tb_dev.legacy)
			legacy_report(tb_intr, (u32)ret);
		else
			hid_report(tb_intr, (u32)ret);
	}

	close_tablet();
	return NULL;
}

/* ---- API ----------------------------------------------------------------- */

s32 TB_Init(void)
{
	if (tb_running)
		return TB_OK;

	if (tb_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&tb_lock, false);
	USB_Initialize();

	memset(&tb_state, 0, sizeof(tb_state));
	memset(&tb_st, 0, sizeof(tb_st));
	tb_head = tb_tail = 0;

	tb_running = true;
	if (LWP_CreateThread(&tb_thread, tb_worker, NULL, tb_stack, STACKSIZE, 70) < 0) {
		tb_running = false;
		tb_thread = LWP_THREAD_NULL;
		return TB_ENOMEM;
	}
	return TB_OK;
}

void TB_Shutdown(void)
{
	if (!tb_running)
		return;
	tb_running = false;
	if (tb_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(tb_thread, NULL);
		tb_thread = LWP_THREAD_NULL;
	}
}

bool TB_Connected(void)
{
	return tb_opened;
}

bool TB_GetInfo(tb_info *info)
{
	if (!tb_opened)
		return false;
	if (info)
		*info = tb_dev;
	return true;
}

void TB_GetState(tb_sample *out)
{
	if (!out)
		return;
	LWP_MutexLock(tb_lock);
	*out = tb_state;
	LWP_MutexUnlock(tb_lock);
}

bool TB_Pop(tb_sample *out)
{
	bool got = false;

	LWP_MutexLock(tb_lock);
	if (tb_tail != tb_head) {
		if (out)
			*out = tb_queue[tb_tail];
		tb_tail = (tb_tail + 1) & QUEUE_MASK;
		got = true;
	}
	LWP_MutexUnlock(tb_lock);
	return got;
}

void TB_ToScreen(const tb_sample *s, int w, int h, int *x, int *y)
{
	float aspect = (float)tb_dev.max_x / (float)(tb_dev.max_y ? tb_dev.max_y : 1);
	float aw = (float)w, ah = (float)w / aspect;

	if (!s || tb_dev.max_x <= 0) {
		if (x) *x = 0;
		if (y) *y = 0;
		return;
	}
	if (ah > (float)h) {
		ah = (float)h;
		aw = ah * aspect;
	}
	if (x) *x = (int)(((float)w - aw) / 2 + s->nx * (aw - 1));
	if (y) *y = (int)(((float)h - ah) / 2 + s->ny * (ah - 1));
}

void TB_GetStats(tb_stats *out)
{
	if (out)
		*out = tb_st;
}
