#include <windows.h>
#include <stdlib.h>
#include <math.h>
#include "Sound.h"
#include "DefOrg.h"
#include "OrgData.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, y, z) MIN(MAX((x), (y)), (z))

static struct S_Sound
{
	signed char* samples;
	size_t frames;
	size_t position;
	unsigned long long sample_offset_remainder;  // 16.16 fixed-point
	unsigned long long advance_delta;            // 16.16 fixed-point
	bool playing;
	bool looping;
	short volume;    // 8.8 fixed-point
	short pan_l;     // 8.8 fixed-point
	short pan_r;     // 8.8 fixed-point
	short volume_l;  // 8.8 fixed-point
	short volume_r;  // 8.8 fixed-point

	struct S_Sound* next;
};

static S_Sound* sound_list_head;

static unsigned long output_frequency;

static unsigned short organya_timer;
static unsigned long organya_countdown;

static unsigned short MillibelToScale(long volume) {
	volume = CLAMP(volume, -10000, 0);
	return (unsigned short)(pow(10.0, volume / 2000.0) * 256.0f);
}

static void S_SetSoundFrequency(S_Sound* sound, unsigned long long frequency) {
	if (sound == NULL)
		return;

	sound->advance_delta = (frequency << 16) / output_frequency;
}

static void S_SetSoundVolume(S_Sound* sound, long volume) {
	if (sound == NULL)
		return;

	sound->volume = MillibelToScale(volume);

	sound->volume_l = (sound->pan_l * sound->volume) >> 8;
	sound->volume_r = (sound->pan_r * sound->volume) >> 8;
}

static void S_SetSoundPan(S_Sound* sound, long pan) {
	if (sound == NULL)
		return;

	sound->pan_l = MillibelToScale(-pan);
	sound->pan_r = MillibelToScale(pan);

	sound->volume_l = (sound->pan_l * sound->volume) >> 8;
	sound->volume_r = (sound->pan_r * sound->volume) >> 8;
}

static S_Sound *S_CreateSound(unsigned int frequency, const unsigned char *samples, size_t length) {
	S_Sound *sound = (S_Sound*)malloc(sizeof(S_Sound));
	if (sound == NULL)
		return NULL;

	sound->samples = (signed char*)malloc(length + 1);
	if (sound->samples == NULL) {
		free(sound);
		return NULL;
	}

	for (size_t i = 0; i < length; ++i)
		sound->samples[i] = samples[i] - 0x80;

	sound->frames = length;
	sound->playing = false;
	sound->position = 0;
	sound->sample_offset_remainder = 0;

	S_SetSoundFrequency(sound, frequency);
	S_SetSoundVolume(sound, 0);
	S_SetSoundPan(sound, 0);

	sound->next = sound_list_head;
	sound_list_head = sound;

	return sound;
}

static void S_PlaySound(S_Sound* sound, bool looping) {
	if (sound == NULL)
		return;

	sound->playing = true;
	sound->looping = looping;

	sound->samples[sound->frames] = looping ? sound->samples[0] : 0;	// For the linear interpolator
}

static void S_StopSound(S_Sound* sound) {
	if (sound == NULL)
		return;

	sound->playing = false;
}

static void S_RewindSound(S_Sound* sound) {
	if (sound == NULL)
		return;

	sound->position = 0;
	sound->sample_offset_remainder = 0;
}

static void S_MixSounds(long *stream, size_t frames_total) {
	for (S_Sound *sound = sound_list_head; sound != NULL; sound = sound->next) {
		if (sound->playing) {
			long *stream_pointer = stream;

			for (size_t frames_done = 0; frames_done < frames_total; ++frames_done) {
				const unsigned char subsample = sound->sample_offset_remainder >> 8;
				const short interpolated_sample = sound->samples[sound->position] * (0x100 - subsample) + sound->samples[sound->position + 1] * subsample;

				*stream_pointer++ += (interpolated_sample * sound->volume_l) >> 8;
				*stream_pointer++ += (interpolated_sample * sound->volume_r) >> 8;

				sound->sample_offset_remainder += sound->advance_delta;
				sound->position += sound->sample_offset_remainder >> 16;
				sound->sample_offset_remainder &= 0xFFFF;

				if (sound->position >= sound->frames) {
					if (sound->looping) {
						sound->position %= sound->frames;
					} else {
						sound->playing = false;
						sound->position = 0;
						sound->sample_offset_remainder = 0;
						break;
					}
				}
			}
		}
	}
}

S_Sound *melodyObject[8][8][2] = {NULL};
S_Sound *drumObject[8] = {NULL};

static bool CreateMelody(char* wavep, char track, char pipi) {
	unsigned long i, j, k;
	unsigned long wav_tp;
	unsigned long wave_size;
	unsigned long data_size;
	unsigned char* wp;
	unsigned char* wp_sub;
	int work;

	for (j = 0; j < 8; j++) {
		for (k = 0; k < 2; k++) {
			wave_size = oct_wave[j].wave_size;

			if (pipi)
				data_size = wave_size * oct_wave[j].oct_size;
			else
				data_size = wave_size;

			wp = (unsigned char*)malloc(data_size);

			if (wp == NULL)
				return false;

			wp_sub = wp;
			wav_tp = 0;

			for (i = 0; i < data_size; i++) {
				work = *(wavep + wav_tp);
				work += 0x80;

				*wp_sub = (unsigned char)work;

				wav_tp += 0x100 / wave_size;
				if (wav_tp > 0xFF)
					wav_tp -= 0x100;

				wp_sub++;
			}

			melodyObject[track][j][k] = S_CreateSound(22050, wp, data_size);

			free(wp);

			if (melodyObject[track][j][k] == NULL)
				return false;

			S_RewindSound(melodyObject[track][j][k]);
		}
	}

	return true;
}

static bool CreateDrum(const char *resourceName, int track) {
	HRSRC hrscr = FindResource(NULL, resourceName, "WAVE");
	if (hrscr == NULL)
		return false;

	const unsigned char* lp = (unsigned char*)LockResource(LoadResource(NULL, hrscr));
	if (lp == NULL)
		return false;
	
	unsigned int sz = *(unsigned int*)(lp + 0x36);

	unsigned char* wp = (unsigned char*)malloc(sz);
	if (wp == NULL)
		return false;

	memcpy(wp, lp + 0x3a, sz);

	drumObject[track] = S_CreateSound(22050, wp, sz);

	free(wp);

	if (drumObject[track] == NULL)
		return false;

	S_RewindSound(drumObject[track]);

	return true;
}

static void MelodyPitch(unsigned char key, signed char track, long a)
{
	double tmpDouble;
	for (int j = 0; j < 8; j++) {
		for (int i = 0; i < 2; i++) {
			tmpDouble = (((double)oct_wave[j].wave_size * freq_tbl[key]) * (double)oct_wave[j].oct_par) / 8.00f + ((double)a - 1000.0f);
			S_SetSoundFrequency(melodyObject[track][j][i], (unsigned int)tmpDouble);//((oct_wave[j].wave_size * freq_tbl[key]) * oct_wave[j].oct_par) / 8 + (a - 1000));	// 1000を+αのデフォルト値とする (1000 is the default value for + α)
		}
	}
}

static unsigned char s_old_key[MAXMELODY] = { 255,255,255,255,255,255,255,255 };
static unsigned char s_key_on[MAXMELODY] = { 0 };
static unsigned char s_key_twin[MAXMELODY] = { 0 };

static void MelodyPan(unsigned char key, unsigned char pan, signed char track) {
	if (s_old_key[track] != PANDUMMY)
		S_SetSoundPan(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]], (pan_tbl[pan] - 0x100) * 10);
}

static void MelodyVolume(int no, long volume, signed char track) {
	if (s_old_key[track] != VOLDUMMY)
		S_SetSoundVolume(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]], (volume - 0xFF) * 8);
}

static void PlayMelody(unsigned char key, int mode, signed char track, long freq, signed char pipi)
{
	if (melodyObject[track][key / 12][s_key_twin[track]] != NULL) {
		switch (mode) {
		case 0:
			if (s_old_key[track] != 0xFF) {
				S_StopSound(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]]);
				S_RewindSound(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]]);
			}
			break;

		case 1:
			break;

		case 2:
			if (s_old_key[track] != 0xFF) {
				if (pipi == 0) S_PlaySound(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]], false);
				s_old_key[track] = 0xFF;
			}
			break;

		case -1:
			if (s_old_key[track] == 0xFF) {
				MelodyPitch(key % 12, track, freq);	// 周波数を設定して (Set the frequency)
				S_PlaySound(melodyObject[track][key / 12][s_key_twin[track]], pipi == 0);
				s_old_key[track] = key;
				s_key_on[track] = 1;
			} else if (s_key_on[track] == 1 && s_old_key[track] == key) {
				if (pipi == 0) S_PlaySound(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]], false);
				s_key_twin[track]++;
				if (s_key_twin[track] > 1)
					s_key_twin[track] = 0;
				S_PlaySound(melodyObject[track][key / 12][s_key_twin[track]], pipi == 0);
			} else {
				if (pipi == 0) S_PlaySound(melodyObject[track][s_old_key[track] / 12][s_key_twin[track]], false);	// 今なっているのを歩かせ停止 (Stop playback now)
				s_key_twin[track]++;
				if (s_key_twin[track] > 1)
					s_key_twin[track] = 0;
				MelodyPitch(key % 12, track, freq);
				S_PlaySound(melodyObject[track][key / 12][s_key_twin[track]], pipi == 0);
				s_old_key[track] = key;
			}

			break;
		}
	}
}

static void DrumPitch(unsigned char key, signed char track)
{
	S_SetSoundFrequency(drumObject[track], key * 800 + 100);
}

static void DrumPan(unsigned char pan, signed char track)
{
	S_SetSoundPan(drumObject[track], (pan_tbl[pan] - 0x100) * 10);
}

static void DrumVolume(long volume, signed char track)
{
	S_SetSoundVolume(drumObject[track], (volume - 0xFF) * 8);
}

static void PlayDrum(unsigned char key, int mode, signed char track)
{
	if (drumObject[track] != NULL) {
		switch (mode) {
		case 0:
			S_StopSound(drumObject[track]);
			S_RewindSound(drumObject[track]);
			break;

		case 1:
			S_StopSound(drumObject[track]);
			S_RewindSound(drumObject[track]);
			DrumPitch(key, track);
			S_PlaySound(drumObject[track], false);
			break;

		case 2:
			break;

		case -1:
			break;
		}
	}
}

static long s_play_p = 0;
static NOTELIST *s_np[MAXTRACK] = {NULL};
static long s_now_leng[MAXMELODY] = {0};

static void SetPlayPos(MUSICINFO *info, long x)
{
	for (int i = 0; i < MAXTRACK; i++) {
		s_np[i] = info->tdata[i].note_list;
		while (s_np[i] != NULL && s_np[i]->x < x)
			s_np[i] = s_np[i]->to;
	}

	s_play_p = x;
}

static void PlayData(MUSICINFO *info)
{
	int i;
	for (i = 0; i < MAXMELODY; i++) {
		if (s_np[i] != NULL && s_play_p == s_np[i]->x) {
			if (s_np[i]->y != KEYDUMMY) {
				PlayMelody(s_np[i]->y, -1, i, info->tdata[i].freq, info->tdata[i].pipi);
				s_now_leng[i] = s_np[i]->length;
			}
			if (s_np[i]->pan != PANDUMMY)
				MelodyPan(s_np[i]->y, s_np[i]->pan, i);
			if (s_np[i]->volume != VOLDUMMY)
				MelodyVolume(s_np[i]->y, s_np[i]->volume * 100 / 0x7F, i);
			s_np[i] = s_np[i]->to;
		}
		if (s_now_leng[i] == 0)
			PlayMelody(0, 2, i, info->tdata[i].freq, info->tdata[i].pipi);
		if (s_now_leng[i] > 0)
			s_now_leng[i]--;
	}
	for (i = MAXMELODY; i < MAXTRACK; i++) {
		if (s_np[i] != NULL && s_play_p == s_np[i]->x) {
			if (s_np[i]->y != KEYDUMMY)
				PlayDrum(s_np[i]->y, 1, i - MAXMELODY);
			if (s_np[i]->pan != PANDUMMY)
				DrumPan(s_np[i]->pan, i - MAXMELODY);
			if (s_np[i]->volume != VOLDUMMY)
				DrumVolume(s_np[i]->volume * 100 / 0x7F, i - MAXMELODY);
			s_np[i] = s_np[i]->to;
		}
	}
	s_play_p++;
	if (s_play_p >= info->end_x) {
		s_play_p = info->repeat_x;
		SetPlayPos(info, s_play_p);
	}
}

extern char* dram_name[];

void ExportOrganyaBuffer(unsigned long sample_rate, short* output_stream, size_t frames_total, size_t fade_frames) {
	int j, k, l;
	MUSICINFO mi;
	org_data.GetMusicInfo(&mi, 1);
	
	output_frequency = sample_rate;

	for (j = 0; j < MAXMELODY; j++) {
		CreateMelody(&wave_data[0] + mi.tdata[j].wave_no * 256, j, mi.tdata[j].pipi);
	}
	for (j = MAXMELODY; j < MAXTRACK; j++) {
		CreateDrum(dram_name[mi.tdata[j].wave_no], j - MAXMELODY);
	}

	SetPlayPos(&mi, 0);
	organya_countdown = 0;
	organya_timer = mi.wait;

	short* stream = output_stream;
	size_t frames_done = 0;
	while (frames_done != frames_total) {
		long mix_buffer[0x400 * 2];
		size_t subframes = MIN(0x400, frames_total - frames_done);
		memset(mix_buffer, 0, subframes * sizeof(long) * 2);
		if (organya_timer == 0) {
			S_MixSounds(mix_buffer, subframes);
		} else {
			unsigned int subframes_done = 0;
			while (subframes_done != subframes) {
				if (organya_countdown == 0) {
					organya_countdown = (organya_timer * output_frequency) / 1000;
					PlayData(&mi);
				}
				const unsigned int frames_to_do = MIN(organya_countdown, subframes - subframes_done);
				S_MixSounds(mix_buffer + subframes_done * 2, frames_to_do);
				subframes_done += frames_to_do;
				organya_countdown -= frames_to_do;
			}
		}
		for (size_t i = 0; i < subframes * 2; ++i) {
			if (fade_frames > 0 && frames_done + i / 2 > frames_total - fade_frames) {
				if (i % 2 == 0)
					j = (unsigned short)((fade_frames - ((frames_done + i / 2) - (frames_total - fade_frames))) * 256 / fade_frames);
				mix_buffer[i] = (mix_buffer[i] * j) >> 8;
			}

			if (mix_buffer[i] > 0x7FFF)
				*stream++ = 0x7FFF;
			else if (mix_buffer[i] < -0x7FFF)
				*stream++ = -0x7FFF;
			else
				*stream++ = mix_buffer[i];
		}
		frames_done += subframes;
	}
	organya_timer = 0;

	for (S_Sound *sound = sound_list_head; sound != NULL;) {
		S_Sound *pSound = sound;
		sound = sound->next;
		free(pSound->samples);
		free(pSound);
	}
	sound_list_head = NULL;
	for (j = 0; j < 8; j++) {
		for (k = 0; k < 8; k++) {
			for (l = 0; l < 2; l++) {
				melodyObject[j][k][l] = NULL;
			}
		}
		s_old_key[j] = 0xFF;
		s_key_on[j] = 0;
		s_key_twin[j] = 0;
		s_now_leng[j] = 0;
		drumObject[j] = NULL;
	}
}