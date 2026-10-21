/* USB Audio Class microphone driver for libogc homebrew.  See input_usbmic.h. */

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>

#include "input_usbmic.h"

/* ---- USB Audio Class 1.0 --------------------------------------------------
 *
 * A class-compliant mic has an Audio Control interface (subclass 1) that
 * describes the signal path as terminals and units, and an Audio Streaming
 * interface (subclass 2) whose non-zero alternate settings each offer one
 * format on an isochronous IN endpoint.  Alt 0 has no endpoint at all, so
 * the device uses no bandwidth until the host picks a format.
 */
#define USB_CLASS_AUDIO      0x01
#define AUDIO_SUBCLASS_CTRL  0x01
#define AUDIO_SUBCLASS_STRM  0x02

/* Class-specific descriptor subtypes */
#define AC_INPUT_TERMINAL    0x02
#define AC_OUTPUT_TERMINAL   0x03
#define AC_MIXER_UNIT        0x04
#define AC_SELECTOR_UNIT     0x05
#define AC_FEATURE_UNIT      0x06
#define AC_PROCESSING_UNIT   0x07
#define AC_EXTENSION_UNIT    0x08
#define AS_GENERAL           0x01
#define AS_FORMAT_TYPE       0x02
#define EP_GENERAL           0x01

#define FORMAT_PCM           0x0001
#define FORMAT_TYPE_I        0x01

/* Class requests */
#define REQ_SET_CUR          0x01
#define REQ_GET_CUR          0x81
#define REQ_GET_MIN          0x82
#define REQ_GET_MAX          0x83

#define REQTYPE_CLASS_IF_OUT 0x21
#define REQTYPE_CLASS_IF_IN  0xA1
#define REQTYPE_CLASS_EP_OUT 0x22

/* Control selectors */
#define FU_MUTE              0x01
#define FU_VOLUME            0x02
#define EP_SAMPLING_FREQ     0x01

/* ---- transfers ------------------------------------------------------------
 *
 * Every 1 ms frame carries one packet.  A transfer asks for UM_ISO_PACKETS of
 * them, each as large as the endpoint allows, and two are kept in flight so
 * no frame passes while one is being resubmitted.
 */
#define ISO_SLOTS  2
#define ISO_POISON 0xEE

/* ~5 seconds of headroom at 48 kHz. */
#define RING_SAMPLES (1u << 18)
#define RING_MASK    (RING_SAMPLES - 1)

#define STACKSIZE   16384
#define MAX_DEVICES 32
#define MAX_UNITS   32
#define CFG_MAX     1024

static s32 um_fd = -1;
static bool um_opened = false;
static volatile bool um_running = false;
static um_info um_dev;

/* Feature unit on the capture path, and which of its channels take each
 * control (bit 0 = master). */
static u8 um_ac_if;
static u8 um_fu;
static u16 um_vol_chans, um_mute_chans;

static u8 um_subframe;   /* bytes per sample on the wire */
static bool um_freq_ctl; /* the endpoint takes a sampling-rate request */

static u8 *um_ctl;    /* control transfer buffer, DMA aligned */
static u8 *um_in_buf[ISO_SLOTS];
static u16 *um_in_sizes[ISO_SLOTS];
static u32 um_in_bytes;
static u8 *um_stack;

static s16 *um_ring;
static volatile u32 um_ring_head;
static volatile u32 um_ring_tail;
static volatile u32 um_drops;
static mutex_t um_ring_lock = LWP_MUTEX_NULL;

static lwp_t um_thread = LWP_THREAD_NULL;
static um_stats um_st;

static int (*um_logger)(const char *fmt, ...);
#define um_log(...) (um_logger ? (void)um_logger(__VA_ARGS__) : (void)0)

void UM_SetLog(int (*print)(const char *fmt, ...)) { um_logger = print; }

typedef struct {
	volatile bool done;
	volatile bool pending; /* submitted and not yet collected */
	volatile s32 res;
} um_xfer;

/* static: a late completion writes here */
static um_xfer um_in_x[ISO_SLOTS];
static um_xfer um_ctl_x;

static s32 um_xfer_cb(s32 result, void *arg)
{
	um_xfer *x = arg;

	x->res = result;
	x->done = true;
	return 0;
}

/* A control transfer with a deadline: libogc's synchronous calls never time
 * out, and a device that stops answering would hang the console.  `data` is
 * staged through um_ctl, so callers can pass ordinary buffers. */
static s32 um_ctrl(u8 type, u8 req, u16 val, u16 idx, u16 len, void *data, int ms)
{
	s32 ret;
	int waited;

	if (len > CFG_MAX)
		return UM_EINVAL;

	if (!(type & 0x80) && len)
		memcpy(um_ctl, data, len);
	else
		memset(um_ctl, 0, len ? len : 1);
	DCFlushRange(um_ctl, CFG_MAX);

	um_ctl_x.done = false;
	um_ctl_x.res = -1;
	um_ctl_x.pending = true;

	if (type & 0x80)
		ret = USB_ReadCtrlMsgAsync(um_fd, type, req, val, idx, len, um_ctl,
		                           um_xfer_cb, &um_ctl_x);
	else
		ret = USB_WriteCtrlMsgAsync(um_fd, type, req, val, idx, len,
		                            len ? um_ctl : NULL, um_xfer_cb, &um_ctl_x);
	if (ret < 0) {
		um_ctl_x.pending = false;
		return ret;
	}

	for (waited = 0; !um_ctl_x.done && waited < ms; waited += 2)
		usleep(2 * 1000);
	if (!um_ctl_x.done)
		return UM_ETIMEOUT; /* stays pending: um_ctl must outlive it */
	um_ctl_x.pending = false;

	if ((type & 0x80) && len) {
		DCInvalidateRange(um_ctl, CFG_MAX);
		memcpy(data, um_ctl, len);
	}
	return um_ctl_x.res;
}

static u16 le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static u32 le24(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

/* ---- descriptor parsing -------------------------------------------------- */

typedef struct {
	u8 id, subtype, source;
	u16 type;
	/* feature units only */
	u16 vol_chans, mute_chans;
} um_unit;

/* One streaming alternate setting, and the format it would run at. */
typedef struct {
	u8 iface, alt;
	u8 term_link;
	u16 format;
	u8 channels, subframe, bits;
	u8 ep;
	u16 max_packet;
	bool iso_in;
	bool freq_ctl;
	u32 rate;
	int rate_score;
} um_alt;

/* How much a rate is wanted: 48 kHz first, as most mics run natively there,
 * then CD rate, then anything the Wii can play back. */
static int rate_score(u32 hz)
{
	if (hz == 48000) return 4;
	if (hz == 44100) return 3;
	if (hz == 32000) return 2;
	if (hz >= 8000 && hz <= 48000) return 1;
	return 0;
}

static void pick_rate(um_alt *a, const u8 *d)
{
	u8 n = d[7];
	u32 i;

	a->rate = 0;
	a->rate_score = 0;

	if (n == 0) {
		/* Continuous range: lower and upper bound. */
		u32 lo, hi;

		if (d[0] < 14)
			return;
		lo = le24(d + 8);
		hi = le24(d + 11);
		if (lo <= 48000 && hi >= 48000)
			a->rate = 48000;
		else if (lo <= 44100 && hi >= 44100)
			a->rate = 44100;
		else
			a->rate = hi > 48000 ? 48000 : hi;
		if (a->rate < lo)
			a->rate = 0;
		a->rate_score = rate_score(a->rate);
		return;
	}

	for (i = 0; i < n && 8 + 3 * i + 3 <= d[0]; i++) {
		u32 hz = le24(d + 8 + 3 * i);
		int s = rate_score(hz);

		if (s > a->rate_score || (s == a->rate_score && s == 1 && hz > a->rate)) {
			a->rate = hz;
			a->rate_score = s;
		}
	}
}

static bool alt_usable(const um_alt *a)
{
	u32 frame_bytes;

	if (!a->iso_in || a->format != FORMAT_PCM || a->rate_score == 0)
		return false;
	if (a->channels < 1 || a->channels > 2)
		return false;
	if (a->subframe < 2 || a->subframe > 4)
		return false;

	/* One frame's worth of audio has to fit in a packet. */
	frame_bytes = (a->rate + 999) / 1000 * a->channels * a->subframe;
	return a->max_packet >= frame_bytes;
}

static int alt_score(const um_alt *a)
{
	return a->rate_score * 100 + (a->subframe == 2 ? 20 : 0) +
	       (a->channels == 1 ? 10 : 0);
}

static um_unit *find_unit(um_unit *units, int n, u8 id)
{
	int i;

	for (i = 0; i < n; i++)
		if (units[i].id == id)
			return &units[i];
	return NULL;
}

/* Walks from the streaming terminal back towards the mic until it meets a
 * feature unit -- the one whose volume and mute govern what we record. */
static um_unit *capture_fu(um_unit *units, int n, u8 term_link)
{
	um_unit *u = find_unit(units, n, term_link);
	int hops;

	for (hops = 0; u && hops < MAX_UNITS; hops++) {
		if (u->subtype == AC_FEATURE_UNIT)
			return u;
		if (u->subtype == AC_INPUT_TERMINAL)
			break;
		u = find_unit(units, n, u->source);
	}

	/* No path to follow: fall back to the first feature unit fed by a
	 * microphone-type input terminal (0x02xx), then to any at all. */
	for (hops = 0; hops < n; hops++) {
		um_unit *src;

		if (units[hops].subtype != AC_FEATURE_UNIT)
			continue;
		src = find_unit(units, n, units[hops].source);
		if (src && src->subtype == AC_INPUT_TERMINAL &&
		    (src->type & 0xFF00) == 0x0200)
			return &units[hops];
	}
	for (hops = 0; hops < n; hops++)
		if (units[hops].subtype == AC_FEATURE_UNIT)
			return &units[hops];
	return NULL;
}

static void parse_unit(um_unit *u, const u8 *d)
{
	u8 len = d[0];

	memset(u, 0, sizeof(*u));
	u->subtype = d[2];
	u->id = len > 3 ? d[3] : 0;

	switch (u->subtype) {
	case AC_INPUT_TERMINAL:
		if (len >= 6)
			u->type = le16(d + 4);
		break;
	case AC_OUTPUT_TERMINAL:
		if (len >= 8) {
			u->type = le16(d + 4);
			u->source = d[7];
		}
		break;
	case AC_MIXER_UNIT:
	case AC_SELECTOR_UNIT:
		if (len >= 6 && d[4] > 0)
			u->source = d[5];
		break;
	case AC_FEATURE_UNIT: {
		u8 size = len > 5 ? d[5] : 0;
		int ch, nch;

		if (len < 7 || size == 0)
			break;
		u->source = d[4];
		nch = (len - 7) / size; /* the master plus each channel */
		if (nch > 16)
			nch = 16;
		for (ch = 0; ch < nch; ch++) {
			u8 ctl = d[6 + ch * size];

			if (ctl & 0x01)
				u->mute_chans |= 1u << ch;
			if (ctl & 0x02)
				u->vol_chans |= 1u << ch;
		}
		break;
	}
	case AC_PROCESSING_UNIT:
	case AC_EXTENSION_UNIT:
		if (len >= 8 && d[6] > 0)
			u->source = d[7];
		break;
	}
}

/* Reads through a configuration descriptor for the best streaming format.
 * Returns false when the device has no usable capture interface. */
static bool parse_config(const u8 *cfg, u32 total, um_alt *best, u8 *ac_if,
                         um_unit *fu_out)
{
	static um_unit units[MAX_UNITS];
	int nunits = 0;
	um_alt cur;
	bool in_as = false, in_ac = false, have = false;
	u32 off = 0;

	memset(&cur, 0, sizeof(cur));
	memset(best, 0, sizeof(*best));
	*ac_if = 0;

	while (off + 2 <= total) {
		const u8 *d = cfg + off;
		u8 len = d[0], type = d[1];

		if (len < 2 || off + len > total)
			break;

		if (type == USB_DT_INTERFACE && len >= 9) {
			if (in_as && alt_usable(&cur) &&
			    (!have || alt_score(&cur) > alt_score(best))) {
				*best = cur;
				have = true;
			}

			in_ac = d[5] == USB_CLASS_AUDIO && d[6] == AUDIO_SUBCLASS_CTRL;
			in_as = d[5] == USB_CLASS_AUDIO && d[6] == AUDIO_SUBCLASS_STRM &&
			        d[3] != 0;
			if (in_ac)
				*ac_if = d[2];

			memset(&cur, 0, sizeof(cur));
			cur.iface = d[2];
			cur.alt = d[3];
		} else if (type == USB_DT_CLASS_SPECIFIC_INTERFACE && len >= 3) {
			if (in_ac && nunits < MAX_UNITS &&
			    d[2] >= AC_INPUT_TERMINAL && d[2] <= AC_EXTENSION_UNIT)
				parse_unit(&units[nunits++], d);
			else if (in_as && d[2] == AS_GENERAL && len >= 7) {
				cur.term_link = d[3];
				cur.format = le16(d + 5);
			} else if (in_as && d[2] == AS_FORMAT_TYPE && len >= 8 &&
			           d[3] == FORMAT_TYPE_I) {
				cur.channels = d[4];
				cur.subframe = d[5];
				cur.bits = d[6];
				pick_rate(&cur, d);
			}
		} else if (type == USB_DT_ENDPOINT && len >= 7 && in_as) {
			/* Isochronous IN; a second (feedback) endpoint is not ours. */
			if ((d[2] & 0x80) && (d[3] & 0x03) == 0x01 && !cur.iso_in) {
				cur.iso_in = true;
				cur.ep = d[2];
				cur.max_packet = le16(d + 4) & 0x7FF;
			}
		} else if (type == USB_DT_CLASS_SPECIFIC_ENDPOINT && len >= 4 &&
		           in_as && d[2] == EP_GENERAL) {
			cur.freq_ctl = (d[3] & 0x01) != 0;
		}

		off += len;
	}

	if (in_as && alt_usable(&cur) && (!have || alt_score(&cur) > alt_score(best))) {
		*best = cur;
		have = true;
	}

	if (have) {
		um_unit *fu = capture_fu(units, nunits, best->term_link);

		if (fu)
			*fu_out = *fu;
		else
			memset(fu_out, 0, sizeof(*fu_out));
	}

	return have;
}

/* ---- device discovery ---------------------------------------------------- */

static void read_name(u8 index, u16 vid, u16 pid, char *out, u32 size)
{
	u8 buf[64];
	s32 ret;
	u32 i, n = 0;

	snprintf(out, size, "%04x:%04x", vid, pid);
	if (index == 0)
		return;

	/* Two-byte or longer reads only: see libinput_wiispeak.c on USBV5 and
	 * one-byte control reads. */
	ret = um_ctrl(0x80, USB_REQ_GETDESCRIPTOR, (USB_DT_STRING << 8) | index,
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
	else
		snprintf(out, size, "%04x:%04x", vid, pid);
}

/* With the device open, decides whether it is a mic we can use and, if so,
 * fills um_dev and the feature unit. */
static bool probe_open_device(u16 vid, u16 pid)
{
	static u8 cfg[CFG_MAX];
	u8 dev[18];
	um_alt best;
	um_unit fu;
	u8 ac_if;
	u16 total;
	s32 ret;

	ret = um_ctrl(0x80, USB_REQ_GETDESCRIPTOR, USB_DT_DEVICE << 8, 0,
	              sizeof(dev), dev, 300);
	if (ret < 18) {
		um_log("  %04x:%04x: no device descriptor (%d)\n", vid, pid, (int)ret);
		return false;
	}

	ret = um_ctrl(0x80, USB_REQ_GETDESCRIPTOR, USB_DT_CONFIG << 8, 0, 9, cfg, 300);
	if (ret < 9) {
		um_log("  %04x:%04x: no config descriptor (%d)\n", vid, pid, (int)ret);
		return false;
	}
	total = le16(cfg + 2);
	if (total > CFG_MAX)
		total = CFG_MAX;

	ret = um_ctrl(0x80, USB_REQ_GETDESCRIPTOR, USB_DT_CONFIG << 8, 0, total,
	              cfg, 500);
	if (ret < 9) {
		um_log("  %04x:%04x: config read failed (%d)\n", vid, pid, (int)ret);
		return false;
	}
	if ((u32)ret < total)
		total = (u16)ret;

	if (!parse_config(cfg, total, &best, &ac_if, &fu)) {
		um_log("  %04x:%04x: no usable capture format\n", vid, pid);
		return false;
	}

	memset(&um_dev, 0, sizeof(um_dev));
	um_dev.vid = vid;
	um_dev.pid = pid;
	um_dev.rate = best.rate;
	um_dev.channels = best.channels;
	um_dev.bits = best.bits ? best.bits : best.subframe * 8;
	um_dev.interface = best.iface;
	um_dev.alt = best.alt;
	um_dev.endpoint = best.ep;
	um_dev.max_packet = best.max_packet;
	read_name(dev[15], vid, pid, um_dev.name, sizeof(um_dev.name));

	um_ac_if = ac_if;
	um_fu = fu.id;
	um_vol_chans = fu.vol_chans;
	um_mute_chans = fu.mute_chans;
	um_dev.has_volume = um_fu && um_vol_chans;
	um_dev.has_mute = um_fu && um_mute_chans;

	um_subframe = best.subframe;
	um_freq_ctl = best.freq_ctl;

	um_log("  %s: if %u alt %u ep %02x, %u ch %u-bit %u Hz, pkt %u\n",
	       um_dev.name, best.iface, best.alt, best.ep, best.channels,
	       um_dev.bits, (unsigned)best.rate, best.max_packet);
	return true;
}

/* Opens each candidate in turn, keeping the first that turns out to be a mic.
 * On USBV0 IOSes devices are opened by VID/PID through /dev/usb/oh0; on USBV5
 * ones by the device_id the device list hands back. */
static s32 um_open_device(void)
{
	usb_device_entry *list;
	u8 count = 0;
	bool oh0;
	s32 fd;
	int pass, i;

	fd = IOS_Open("/dev/usb/oh0", 0);
	oh0 = fd >= 0;
	if (oh0)
		IOS_Close(fd);

	list = memalign(32, MAX_DEVICES * sizeof(usb_device_entry));
	if (!list)
		return UM_ENOMEM;

	/* Audio-class devices first; then everything, since some IOSes only
	 * match the class of a device's first interface. */
	for (pass = 0; pass < 2; pass++) {
		memset(list, 0, MAX_DEVICES * sizeof(usb_device_entry));
		count = 0;
		if (USB_GetDeviceList(list, MAX_DEVICES,
		                      pass == 0 ? USB_CLASS_AUDIO : 0, &count) < 0)
			continue;
		um_log("  USB scan (%s): %d device(s)\n",
		       pass == 0 ? "audio class" : "all", (int)count);

		for (i = 0; i < count; i++) {
			u16 vid = list[i].vid, pid = list[i].pid;
			s32 ret;

			/* The Wii Speak is not class-compliant; it has its own
			 * driver, and opening it here would only confuse it. */
			if (vid == 0x057E && pid == 0x0308)
				continue;
			if (vid == 0 && pid == 0)
				continue;

			ret = USB_OpenDevice(oh0 ? USB_OH0_DEVICE_ID : list[i].device_id,
			                     vid, pid, &um_fd);
			if (ret < 0) {
				um_log("  %04x:%04x: open failed (%d)\n", vid, pid, (int)ret);
				continue;
			}

			if (probe_open_device(vid, pid)) {
				free(list);
				return UM_OK;
			}

			USB_CloseDevice(&um_fd);
			um_fd = -1;
		}
	}

	free(list);
	return UM_ENODEV;
}

/* ---- class controls ------------------------------------------------------ */

static s32 fu_set(u8 cs, u8 ch, const void *data, u16 len)
{
	return um_ctrl(REQTYPE_CLASS_IF_OUT, REQ_SET_CUR, (u16)((cs << 8) | ch),
	               (u16)((um_fu << 8) | um_ac_if), len, (void *)data, 300);
}

static s32 fu_get16(u8 req, u8 cs, u8 ch, s16 *out)
{
	u8 v[2] = { 0, 0 };
	s32 ret = um_ctrl(REQTYPE_CLASS_IF_IN, req, (u16)((cs << 8) | ch),
	                  (u16)((um_fu << 8) | um_ac_if), 2, v, 300);

	if (ret >= 2)
		*out = (s16)le16(v);
	return ret;
}

/* The lowest channel that has the control: the master if it does. */
static u8 first_chan(u16 mask)
{
	u8 ch;

	for (ch = 0; ch < 16; ch++)
		if (mask & (1u << ch))
			return ch;
	return 0;
}

static void read_volume_range(void)
{
	u8 ch = first_chan(um_vol_chans);
	s16 lo = 0, hi = 0, cur = 0;
	s32 r1, r2;

	if (!um_dev.has_volume)
		return;

	r1 = fu_get16(REQ_GET_MIN, FU_VOLUME, ch, &lo);
	r2 = fu_get16(REQ_GET_MAX, FU_VOLUME, ch, &hi);
	if (r1 < 2 || r2 < 2 || hi <= lo) {
		um_log("  volume range unreadable (unit %u ch %u: %d %d, %d..%d);"
		       " volume control off\n", um_fu, ch, (int)r1, (int)r2, lo, hi);
		um_dev.has_volume = false;
		return;
	}
	fu_get16(REQ_GET_CUR, FU_VOLUME, ch, &cur);

	um_dev.vol_min = lo;
	um_dev.vol_max = hi;
	um_dev.vol_cur = cur;
	um_log("  volume %d..%d dB, now %d dB\n", lo / 256, hi / 256, cur / 256);
}

s32 UM_SetMute(bool mute)
{
	u8 v = mute ? 1 : 0;
	s32 ret = UM_ENOTSUP;
	u8 ch;

	if (!um_opened)
		return UM_ESTATE;
	if (!um_dev.has_mute)
		return UM_ENOTSUP;

	for (ch = 0; ch < 16; ch++)
		if (um_mute_chans & (1u << ch))
			ret = fu_set(FU_MUTE, ch, &v, 1);
	return ret < 0 ? ret : UM_OK;
}

s32 UM_SetVolume(u32 percent)
{
	s32 range, vol;
	s32 ret = UM_ENOTSUP;
	u8 v[2];
	u8 ch;

	if (!um_opened)
		return UM_ESTATE;
	if (!um_dev.has_volume)
		return UM_ENOTSUP;
	if (percent > 100)
		percent = 100;

	range = (s32)um_dev.vol_max - um_dev.vol_min;
	vol = um_dev.vol_min + range * (s32)percent / 100;
	v[0] = (u8)vol;
	v[1] = (u8)(vol >> 8);

	for (ch = 0; ch < 16; ch++)
		if (um_vol_chans & (1u << ch))
			ret = fu_set(FU_VOLUME, ch, v, 2);
	if (ret >= 0)
		um_dev.vol_cur = (s16)vol;
	return ret < 0 ? ret : UM_OK;
}

static s32 select_alt(u8 alt)
{
	s32 ret = USB_SetAlternativeInterface(um_fd, um_dev.interface, alt);

	if (ret < 0)
		ret = um_ctrl(USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_STANDARD |
		              USB_CTRLTYPE_REC_INTERFACE, USB_REQ_SETINTERFACE, alt,
		              um_dev.interface, 0, NULL, 300);
	return ret;
}

/* ---- streaming ----------------------------------------------------------- */

static void um_push(const s16 *samples, u32 count)
{
	u32 i;

	LWP_MutexLock(um_ring_lock);

	for (i = 0; i < count; i++) {
		u32 next = (um_ring_head + 1) & RING_MASK;
		if (next == um_ring_tail) {
			um_drops += count - i;
			break;
		}
		um_ring[um_ring_head] = samples[i];
		um_ring_head = next;
	}

	LWP_MutexUnlock(um_ring_lock);
}

/* One sample of `bytes` little-endian bytes, as 16-bit. */
static s16 sample16(const u8 *p, u8 bytes)
{
	return (s16)(p[bytes - 2] | (p[bytes - 1] << 8));
}

/* Folds `len` bytes of interleaved frames to mono, returning the count. */
static u32 convert(const u8 *src, u32 len, s16 *out)
{
	u32 frame = um_dev.channels * um_subframe;
	u32 n = 0, off;

	for (off = 0; off + frame <= len; off += frame) {
		if (um_dev.channels == 1)
			out[n++] = sample16(src + off, um_subframe);
		else
			out[n++] = (s16)(((s32)sample16(src + off, um_subframe) +
			                  sample16(src + off + um_subframe, um_subframe)) / 2);
	}
	return n;
}

/* Copies the audio out of one IN transfer, returning the sample count.
 *
 * IOS lays the packets out at their requested sizes, one after the other.
 * Where it writes each packet's real length back into the size table, those
 * lengths are used.  /dev/usb/oh0 returns 0 and leaves the table holding
 * nothing usable (a Fifine mic came back with 36960 in every entry, for 96
 * bytes received into 200-byte slots), so there a packet's length is found
 * by trimming the poison it was filled with off its end, a frame at a time. */
static u32 extract(s32 ret, const u8 *src, const u16 *sizes, s16 *out)
{
	u32 frame = um_dev.channels * um_subframe;
	u32 pkt = um_dev.max_packet;
	bool rewritten = false, plausible = true;
	u32 i, n = 0;

	for (i = 0; i < UM_ISO_PACKETS; i++) {
		if (sizes[i] != pkt)
			rewritten = true;
		if (sizes[i] > pkt)
			plausible = false;
	}
	rewritten = rewritten && plausible;
	um_st.sizes_rewritten = rewritten;

	if (ret < 0)
		return 0;

	/* A byte count and untouched sizes: the data is packed. */
	if (!rewritten && ret > 0) {
		u32 len = (u32)ret < um_in_bytes ? (u32)ret : um_in_bytes;
		return convert(src, len - len % frame, out);
	}

	for (i = 0; i < UM_ISO_PACKETS; i++) {
		const u8 *p = src + i * pkt;
		u32 len = rewritten ? (sizes[i] < pkt ? sizes[i] : pkt) : pkt;

		len -= len % frame;
		if (!rewritten) {
			while (len >= frame) {
				u32 j;

				for (j = len - frame; j < len && p[j] == ISO_POISON; j++)
					;
				if (j < len)
					break;
				len -= frame;
			}
		}

		if (len == 0) {
			um_st.empty_packets++;
			continue;
		}
		n += convert(p, len, out + n);
	}
	return n;
}

static s32 um_submit(int slot)
{
	um_xfer *x = &um_in_x[slot];
	s32 ret;
	u32 i;

	for (i = 0; i < UM_ISO_PACKETS; i++)
		um_in_sizes[slot][i] = um_dev.max_packet;
	memset(um_in_buf[slot], ISO_POISON, um_in_bytes);
	DCFlushRange(um_in_buf[slot], um_in_bytes);
	DCFlushRange(um_in_sizes[slot], 32);

	x->done = false;
	x->pending = true;
	ret = USB_ReadIsoMsgAsync(um_fd, um_dev.endpoint, UM_ISO_PACKETS,
	                          um_in_sizes[slot], um_in_buf[slot], um_xfer_cb, x);
	if (ret < 0) {
		x->pending = false;
		um_st.errors++;
		um_st.last_ret = ret;
	}
	return ret;
}

/* Waits for a transfer for as long as streaming is on, so UM_Stop() never
 * joins a thread stuck on a transfer that will not complete. */
static bool um_wait(um_xfer *x)
{
	while (!x->done) {
		if (!um_running)
			return false;
		usleep(1000);
	}
	return true;
}

/* Keeps ISO_SLOTS transfers queued and collects them in submission order. */
static void *um_worker(void *arg)
{
	static s16 samples[UM_ISO_PACKETS * 1023 / 2];
	int slot;

	(void)arg;

	for (slot = 0; slot < ISO_SLOTS; slot++)
		um_submit(slot);

	slot = 0;
	while (um_running) {
		um_xfer *x = &um_in_x[slot];
		s32 ret;
		u32 n, i;

		if (!x->pending && um_submit(slot) < 0) {
			usleep(1000);
			slot = (slot + 1) % ISO_SLOTS;
			continue;
		}

		if (!um_wait(x))
			break;
		x->pending = false;

		ret = x->res;
		DCInvalidateRange(um_in_buf[slot], um_in_bytes);
		DCInvalidateRange(um_in_sizes[slot], 32);

		um_st.calls++;
		um_st.last_ret = ret;
		for (i = 0; i < UM_ISO_PACKETS; i++)
			um_st.last_sizes[i] = um_in_sizes[slot][i];

		if (ret < 0) {
			/* Back off: failures can come back instantly, and this
			 * thread outranks the main loop. */
			um_st.errors++;
			usleep(1000);
		} else {
			n = extract(ret, um_in_buf[slot], um_in_sizes[slot], samples);
			um_st.samples += n;
			if (n > 0)
				um_push(samples, n);
		}

		if (um_running)
			um_submit(slot);
		slot = (slot + 1) % ISO_SLOTS;
	}

	return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */

static void um_free_buffers(void)
{
	int i;

	/* A transfer that never completed may still land in these. */
	for (i = 0; i < ISO_SLOTS; i++) {
		if (!um_in_x[i].pending) {
			free(um_in_buf[i]);
			free(um_in_sizes[i]);
		}
		um_in_buf[i] = NULL;
		um_in_sizes[i] = NULL;
	}
	if (!um_ctl_x.pending)
		free(um_ctl);
	um_ctl = NULL;
	free(um_stack); um_stack = NULL;
	free(um_ring);  um_ring = NULL;
}

static bool um_alloc_stream(void)
{
	int i;

	um_in_bytes = (UM_ISO_PACKETS * um_dev.max_packet + 31) & ~31u;
	um_stack = memalign(32, STACKSIZE);
	um_ring = memalign(32, RING_SAMPLES * sizeof(s16));
	if (!um_stack || !um_ring)
		return false;

	for (i = 0; i < ISO_SLOTS; i++) {
		um_in_buf[i] = memalign(32, um_in_bytes);
		um_in_sizes[i] = memalign(32, 32);
		if (!um_in_buf[i] || !um_in_sizes[i])
			return false;
	}
	return true;
}

s32 UM_Open(um_info *info)
{
	s32 ret;

	if (um_opened)
		return UM_ESTATE;

	um_ctl = memalign(32, CFG_MAX);
	if (!um_ctl)
		return UM_ENOMEM;
	if (um_ring_lock == LWP_MUTEX_NULL)
		LWP_MutexInit(&um_ring_lock, false);

	USB_Initialize();
	memset(&um_st, 0, sizeof(um_st));

	ret = um_open_device();
	if (ret < 0) {
		um_fd = -1;
		um_free_buffers();
		return ret;
	}
	um_opened = true;

	/* /dev/usb/oh0 configures a device as it opens it; USBV5 may not. */
	{
		u8 cfg = 0xFF;

		ret = USB_GetConfiguration(um_fd, &cfg);
		if (ret < 0 || cfg == 0)
			um_log("  set config 1: %d\n", (int)USB_SetConfiguration(um_fd, 1));
	}

	if (!um_alloc_stream()) {
		ret = UM_ENOMEM;
		goto fail;
	}

	ret = select_alt(um_dev.alt);
	um_log("  set alt %u: %d\n", um_dev.alt, (int)ret);
	if (ret < 0)
		goto fail;

	if (um_freq_ctl) {
		u8 hz[3] = { (u8)um_dev.rate, (u8)(um_dev.rate >> 8),
		             (u8)(um_dev.rate >> 16) };

		/* Not fatal: plenty of mics run one rate and refuse the request. */
		um_log("  set rate %u: %d\n", (unsigned)um_dev.rate,
		       (int)um_ctrl(REQTYPE_CLASS_EP_OUT, REQ_SET_CUR,
		                    EP_SAMPLING_FREQ << 8, um_dev.endpoint, 3, hz, 300));
	}

	read_volume_range();
	if (um_dev.has_mute)
		um_log("  unmute: %d\n", (int)UM_SetMute(false));

	um_ring_head = um_ring_tail = um_drops = 0;
	if (info)
		*info = um_dev;
	return UM_OK;

fail:
	USB_CloseDevice(&um_fd);
	um_fd = -1;
	um_opened = false;
	um_free_buffers();
	return ret;
}

s32 UM_Close(void)
{
	if (!um_opened)
		return UM_ESTATE;

	UM_Stop();

	/* Back to the zero-bandwidth setting, as a PC driver leaves it. */
	um_log("  um: alt 0 %d\n", (int)select_alt(0));
	um_log("  um: close %d\n", (int)USB_CloseDevice(&um_fd));
	um_fd = -1;
	um_opened = false;

	um_free_buffers();
	return UM_OK;
}

bool UM_IsOpen(void)
{
	return um_opened;
}

s32 UM_Start(void)
{
	if (!um_opened)
		return UM_ESTATE;
	if (um_running)
		return UM_OK;

	um_running = true;
	if (LWP_CreateThread(&um_thread, um_worker, NULL, um_stack, STACKSIZE,
	                     80) < 0) {
		um_running = false;
		um_thread = LWP_THREAD_NULL;
		return UM_ENOMEM;
	}
	return UM_OK;
}

s32 UM_Stop(void)
{
	if (!um_running)
		return UM_OK;

	um_running = false;
	if (um_thread != LWP_THREAD_NULL) {
		LWP_JoinThread(um_thread, NULL);
		um_thread = LWP_THREAD_NULL;
	}
	return UM_OK;
}

void UM_Quiesce(void)
{
	um_running = false;
}

u32 UM_Read(s16 *dst, u32 max_samples)
{
	u32 taken = 0;

	if (!dst || !um_ring)
		return 0;

	LWP_MutexLock(um_ring_lock);

	while (taken < max_samples && um_ring_tail != um_ring_head) {
		dst[taken++] = um_ring[um_ring_tail];
		um_ring_tail = (um_ring_tail + 1) & RING_MASK;
	}

	LWP_MutexUnlock(um_ring_lock);
	return taken;
}

u32 UM_Available(void)
{
	return (um_ring_head - um_ring_tail) & RING_MASK;
}

u32 UM_Dropped(void)
{
	return um_drops;
}

void UM_GetStats(um_stats *out)
{
	if (out)
		*out = um_st;
}
