/* USB Audio Class microphone driver for libogc homebrew.
 *
 * Captures from any class-compliant (UAC 1.0) USB microphone -- the kind that
 * needs no driver on a PC.  The streaming format is read from the device's
 * own descriptors: 16-bit PCM at 48 kHz is preferred, and stereo or 24-bit
 * streams are folded down to 16-bit mono.  Full-speed devices only, which is
 * what the Wii's rear ports run at.
 */

#ifndef INPUT_USBMIC_H
#define INPUT_USBMIC_H

#include <gctypes.h>

/* Errors.  USB-level failures are returned as-is (always negative). */
#define UM_OK        0
#define UM_ENODEV   -1
#define UM_EINVAL   -2
#define UM_ESTATE   -3
#define UM_ENOMEM   -5
#define UM_ETIMEOUT -6
#define UM_ENOTSUP  -8

typedef struct {
	u16 vid, pid;
	char name[48];      /* product string, or "VID:PID" without one */
	u32 rate;           /* samples per second, as delivered by UM_Read */
	u8 channels;        /* channels the device sends (folded to mono) */
	u8 bits;            /* bits per sample the device sends */
	u8 interface, alt;  /* the streaming interface in use */
	u8 endpoint;
	u16 max_packet;
	bool has_volume;
	bool has_mute;
	s16 vol_min, vol_max, vol_cur; /* 1/256 dB, as the device reports */
} um_info;

/* Where the device scan and errors go, e.g. UM_SetLog(printf). */
void UM_SetLog(int (*print)(const char *fmt, ...));

/* Finds the first USB microphone, selects its best capture format, unmutes
 * it and fills `info` (which may be NULL).  UM_ENODEV when there is none. */
s32 UM_Open(um_info *info);
s32 UM_Close(void);
bool UM_IsOpen(void);

/* Streaming.  UM_Start() spawns the transfer thread; samples then accumulate
 * in an internal ring buffer until read. */
s32 UM_Start(void);
s32 UM_Stop(void);

/* Stops the transfer thread from queueing anything new, without waiting for
 * it -- for an emergency exit. */
void UM_Quiesce(void);

/* Copies up to `max_samples` 16-bit mono samples out of the ring buffer and
 * returns how many were taken. */
u32 UM_Read(s16 *dst, u32 max_samples);
u32 UM_Available(void);
u32 UM_Dropped(void);

/* Volume as a fraction of the device's range, 0-100.  UM_ENOTSUP when the
 * mic has no volume control. */
s32 UM_SetVolume(u32 percent);
s32 UM_SetMute(bool mute);

/* ---- debugging ---------------------------------------------------------- */

#define UM_ISO_PACKETS 8

typedef struct {
	u32 calls, errors, samples, empty_packets;
	s32 last_ret;
	u16 last_sizes[UM_ISO_PACKETS];
	bool sizes_rewritten; /* IOS reported actual packet lengths */
} um_stats;

void UM_GetStats(um_stats *out);

#endif /* INPUT_USBMIC_H */
