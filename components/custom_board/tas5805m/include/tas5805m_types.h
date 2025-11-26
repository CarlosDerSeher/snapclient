#ifndef _TAS5805M_TYPES_H_
#define _TAS5805M_TYPES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAS5805M_VOLUME_MUTE 0xff // (-103.5 dB - actual mute)
#define TAS5805M_VOLUME_MIN  0xa8 // (   -60 dB - save value representing barely hearable volume)
#define TAS5805M_VOLUME_MAX  0x30 // (     0 dB - maximum volume that guarantees no distortion )
// 							 0x00 // (+24 dB - maximum volume that DAC can do)

#define TAS5805M_VOLUME_DIGITAL_MAX 255	   // Mute
#define TAS5805M_VOLUME_DIGITAL_DEFAULT 48 //  +0 Db
#define TAS5805M_VOLUME_DIGITAL_MIN 0	   // +24 Db

/* Control states */
typedef enum {
	TAS5805M_CTRL_DEEP_SLEEP = 0x00,
	TAS5805M_CTRL_SLEEP = 0x01,
	TAS5805M_CTRL_HI_Z = 0x02,
	TAS5805M_CTRL_PLAY = 0x03,
	TAS5805M_CTRL_MUTE = 0x08,
	TAS5805M_CTRL_PLAY_MUTE = TAS5805M_CTRL_MUTE | TAS5805M_CTRL_PLAY
} TAS5805M_CTRL_STATE;

/* DAC mode */
typedef enum {
	TAS5805M_DAC_MODE_BTL = 0x00,
	TAS5805M_DAC_MODE_PBTL = 0x01
} TAS5805M_DAC_MODE;

/* Switching frequency (SW) */
typedef enum {
	SW_FREQ_768K = (0x00 << 4),
	SW_FREQ_384K = (0x01 << 4),
	SW_FREQ_480K = (0x03 << 4),
	SW_FREQ_576K = (0x04 << 4),
} TAS5805M_SW_FREQ;

/* BD frequency */
typedef enum {
	SW_FREQ_80K = (0x00 << 5),
	SW_FREQ_100K = (0x01 << 5),
	SW_FREQ_120K = (0x02 << 5),
	SW_FREQ_175K = (0x03 << 5),
} TAS5805M_BD_FREQ;

/* Modulation mode */
typedef enum {
	MOD_MODE_BD = 0x0,
	MOD_MODE_1SPW = 0x1,
	MOD_MODE_HYBRID = 0x2,
} TAS5805M_MOD_MODE;

/* Fault structure */
typedef enum {
	MIXER_UNKNOWN,
	MIXER_STEREO,
	MIXER_STEREO_INVERSE,
	MIXER_MONO,
	MIXER_RIGHT,
	MIXER_LEFT,
} TAS5805M_MIXER_MODE;

typedef enum {
	TAS5805M_MIXER_CHANNEL_LEFT_TO_LEFT = 0x00,
	TAS5805M_MIXER_CHANNEL_RIGHT_TO_LEFT = 0x01,
	TAS5805M_MIXER_CHANNEL_LEFT_TO_RIGHT = 0x02,
	TAS5805M_MIXER_CHANNEL_RIGHT_TO_RIGHT = 0x03,
} TAS5805M_MIXER_CHANNELS;

/* Fault structure */
typedef struct {
	uint8_t err0;
	uint8_t err1;
	uint8_t err2;
	uint8_t ot_warn;
} TAS5805M_FAULT;

/* Cached state structure */
typedef struct {
	int8_t volume;
	TAS5805M_CTRL_STATE state;
	TAS5805M_MIXER_MODE mixer_mode;
} TAS5805_STATE;

// Analog gain
#define TAS5805M_MAX_GAIN 0
#define TAS5805M_MIN_GAIN 31
static const uint8_t tas5805m_again[TAS5805M_MIN_GAIN + 1] = {
	0x00, /* 0dB */
	0x01, /* -0.5Db */
	0x02, /* -1.0dB */
	0x03, /* -1.5dB */
	0x04, /* -2.0dB */
	0x05, /* -2.5dB */
	0x06, /* -3.0dB */
	0x07, /* -3.5dB */
	0x08, /* -4.0dB */
	0x09, /* -4.5dB */
	0x0A, /* -5.0dB */
	0x0B, /* -5.5dB */
	0x0C, /* -6.0dB */
	0x0D, /* -6.5dB */
	0x0E, /* -7.0dB */
	0x0F, /* -7.5dB */
	0x10, /* -8.0dB */
	0x11, /* -8.5dB */
	0x12, /* -9.0dB */
	0x13, /* -9.5dB */
	0x14, /* -10.0dB */
	0x15, /* -10.5dB */
	0x16, /* -11.0dB */
	0x17, /* -11.5dB */
	0x18, /* -12.0dB */
	0x19, /* -12.5dB */
	0x1A, /* -13.0dB */
	0x1B, /* -13.5dB */
	0x1C, /* -14.0dB */
	0x1D, /* -14.5dB */
	0x1E, /* -15.0dB */
	0x1F, /* -15.5dB */
};

#ifdef __cplusplus
}
#endif

#endif /* _TAS5805M_TYPES_H_ */
