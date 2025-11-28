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

typedef enum {
	TAS5805M_EQ_MODE_OFF = 0b0111,
	TAS5805M_EQ_MODE_ON = 0b0110,
	TAS5805M_EQ_MODE_BIAMP = 0b1110,
	TAS5805M_EQ_MODE_BIAMP_OFF = 0b1111,
} TAS5805M_EQ_MODE;

typedef enum {
	TAS5805M_EQ_CHANNELS_LEFT = 0x00,
	TAS5805M_EQ_CHANNELS_RIGHT = 0x01,
	TAS5805M_EQ_CHANNELS_BOTH = 0x00,
} TAS5805M_EQ_CHANNELS;

#define TAS5805M_EQ_PROFILES 21

typedef enum {
	FLAT = 0,			  // 0dB
	LF_60HZ_CUTOFF = 1,	  // Low Frequency 60Hz cutoff
	LF_70HZ_CUTOFF = 2,	  // Low Frequency 40Hz cutoff
	LF_80HZ_CUTOFF = 3,	  // Low Frequency 80Hz cutoff
	LF_90HZ_CUTOFF = 4,	  // Low Frequency 90Hz cutoff
	LF_100HZ_CUTOFF = 5,  // Low Frequency 100Hz cutoff
	LF_110HZ_CUTOFF = 6,  // Low Frequency 110Hz cutoff
	LF_120HZ_CUTOFF = 7,  // Low Frequency 120Hz cutoff
	LF_130HZ_CUTOFF = 8,  // Low Frequency 130Hz cutoff
	LF_140HZ_CUTOFF = 9,  // Low Frequency 140Hz cutoff
	LF_150HZ_CUTOFF = 10, // Low Frequency 150Hz cutoff
	HF_60HZ_CUTOFF = 11,  // High Frequency 60Hz cutoff
	HF_70HZ_CUTOFF = 12,  // High Frequency 70Hz cutoff
	HF_80HZ_CUTOFF = 13,  // High Frequency 80Hz cutoff
	HF_90HZ_CUTOFF = 14,  // High Frequency 90Hz cutoff
	HF_100HZ_CUTOFF = 15, // High Frequency 100Hz cutoff
	HF_110HZ_CUTOFF = 16, // High Frequency 110Hz cutoff
	HF_120HZ_CUTOFF = 17, // High Frequency 120Hz cutoff
	HF_130HZ_CUTOFF = 18, // High Frequency 130Hz cutoff
	HF_140HZ_CUTOFF = 19, // High Frequency 140Hz cutoff
	HF_150HZ_CUTOFF = 20, // High Frequency 150Hz cutoff
} TAS5805M_EQ_PROFILE;

#define TAS5805M_MIXER_VALUE_MINDB -24 
#define TAS5805M_MIXER_VALUE_MAXDB 24
#define TAS5805M_MIXER_VALUES_COUNT (TAS5805M_MIXER_VALUE_MAXDB - TAS5805M_MIXER_VALUE_MINDB + 1)

#ifdef __cplusplus
}
#endif

#endif /* _TAS5805M_TYPES_H_ */
