/*
 * music_pcm.c  music server PCM part
 *
 * Copyright (C) 1997-1998 Masaki Chikama (Wren) <chikama@kasumi.ipl.mech.nagoya-u.ac.jp>
 *               1998-                           <masaki-c@is.aist-nara.ac.jp>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
*/
/* $Id: music_pcm.c,v 1.16 2003/11/09 15:06:13 chikama Exp $ */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL_mixer.h>

#include "portab.h"
#include "system.h"
#include "ald_manager.h"
#include "dri.h"
#include "music_pcm.h"
#include "music_private.h"
#include "pcm.sdlmixer.h"
#include "counter.h"
#include "nact.h"
#include "LittleEndian.h"

#define DEFAULT_AUDIO_BUFFER_SIZE 2048

struct _pcmobj {
	Mix_Chunk* chunk;

	int slot; // ロードされているスロット番号
	int channel; // 再生中のチャネル

	boolean playing; // 演奏中
	int start_time;
};
typedef struct _pcmobj pcmobj_t;

// 0:     S comman 用
// 1-128: wavXXX 用
static pcmobj_t *pcmobj[128 + 1];


#define IS_LOADED(slot) (pcmobj[(slot)])

static int unload(int slot);
static int load_wai();
static char *wai_mapadr;

// Volume mixer channel
#define WAIMIXCH(no) LittleEndian_getDW(wai_mapadr, 36 + (no -1) * 4 * 3 + 8)

/**
 * 指定の番号の .WAV|.OGG をロードする。
 * @param no: DRIファイル番号
 */
Mix_Chunk *pcm_sdlmixer_load(int no) {
	dridata *dfile = ald_getdata(DRIFILE_WAVE, no -1);
	if (dfile == NULL) {
		WARNING("DRIFILE_WAVE fail to open %d\n", no -1);
		return NULL;
	}

	Mix_Chunk *chunk = Mix_LoadWAV_RW(SDL_RWFromConstMem(dfile->data, dfile->size), 1);
	ald_freedata(dfile);
	if (chunk == NULL) {
		WARNING("DRIFILE_WAVE %d: not a valid wav file\n", no-1);
		return NULL;
	}
	assert(chunk->allocated);

	return chunk;
}

/**
 * noL と noR の .WAV をロードし、左右合成
 *
 * @param noL: 左の WAV ファイルの番号
 * @param noR: 右の WAV ファイルの番号
 * @return   : 合成後の Mix_Chunk
 */
static Mix_Chunk *pcm_mixlr(int noL, int noR) {
	Mix_Chunk *chunkL = pcm_sdlmixer_load(noL);
	Mix_Chunk *chunkR = pcm_sdlmixer_load(noR);
	if (chunkL == NULL || chunkR == NULL) {
		if (chunkL)
			Mix_FreeChunk(chunkL);
		if (chunkR)
			Mix_FreeChunk(chunkR);
		return NULL;
	}

	short *lbuf = (short*)chunkL->abuf;
	short *rbuf = (short*)chunkR->abuf;
	if (chunkL->alen >= chunkR->alen) {
		int i;
		for (i = 0; i < chunkR->alen / 4; i++) {
			lbuf[i * 2 + 1] = rbuf[i * 2 + 1];
		}
		for (; i < chunkL->alen / 4; i++) {
			lbuf[i * 2 + 1] = 0;
		}
		Mix_FreeChunk(chunkR);
		return chunkL;
	} else {
		int i;
		for (i = 0; i < chunkL->alen / 4; i++) {
			rbuf[i * 2] = lbuf[i * 2];
		}
		for (; i < chunkR->alen / 4; i++) {
			rbuf[i * 2] = 0;
		}
		Mix_FreeChunk(chunkL);
		return chunkR;
	}
}

int muspcm_init(int audio_buffer_size) {
	if (!audio_buffer_size)
		audio_buffer_size = DEFAULT_AUDIO_BUFFER_SIZE;

	Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG);
	if (Mix_OpenAudio(44100, AUDIO_S16LSB, 2, audio_buffer_size) < 0)
		return NG;
	load_wai();
	return OK;
}

int muspcm_exit(void) {
	Mix_CloseAudio();
	Mix_Quit();
	return OK;
}

// 番号指定のPCMファイル読み込み
int muspcm_load_no(int slot, int no) {
	if (IS_LOADED(slot)) unload(slot);
	
	Mix_Chunk *chunk = pcm_sdlmixer_load(no);
	if (chunk == NULL) {
		return NG;
	}
	
	// if has .wai file
	if (wai_mapadr) {
		int ch = WAIMIXCH(no);
		prv.vol_pcm_sub[slot] = ch < 0 ? 0: ch;
	} else {
		prv.vol_pcm_sub[slot] = 0;
	}

	return pcm_sdlmixer_load_chunk(slot, chunk);
}

int muspcm_load_mixlr(int slot, int noL, int noR) {
	/* mix 2 wave files */
	Mix_Chunk* chunk = pcm_mixlr(noL, noR);
	if (chunk == NULL) {
		puts("mixlr fail");
		return NG;
	}

	return pcm_sdlmixer_load_chunk(slot, chunk);
}

int muspcm_unload(int slot) {
	return unload(slot);
}

int pcm_sdlmixer_load_chunk(int slot, Mix_Chunk *chunk) {
	if (IS_LOADED(slot)) unload(slot);

	pcmobj_t *obj = calloc(1, sizeof(pcmobj_t));
	
	obj->chunk    = chunk;
	obj->slot     = slot;
	obj->playing  = FALSE;
	
	pcmobj[slot] = obj;
	
	return OK;
}

// PCMデータを再生
int muspcm_start(int slot, int loop) {
	pcmobj_t *obj;
	// printf("pcm start slot = %d, loop = %d\n", slot, loop);
	
	obj = pcmobj[slot];
	if (obj == NULL) return NG;
	
	obj->channel = Mix_PlayChannel(-1, obj->chunk, loop - 1);
	if (obj->channel < 0)
		return NG;
	
	obj->playing = TRUE;
	obj->start_time = get_high_counter(SYSTEMCOUNTER_MSEC);
	return OK;
}

// PCMデータの再生停止
int muspcm_stop(int slot) {
	pcmobj_t *obj;
	
	obj = pcmobj[slot];
	if (obj == NULL) return NG;
	
	if (obj->playing) {
		Mix_HaltChannel(obj->channel);
		obj->playing = FALSE;
	}
	
	return OK;
}

// 指定時間のフェードアウトの後に再生停止
int muspcm_fadeout(int slot, int msec) {
	if (msec == 0)
		return muspcm_stop(slot);

	pcmobj_t *obj = pcmobj[slot];
	if (obj == NULL) return NG;

	if (obj->playing) {
		Mix_FadeOutChannel(obj->channel, msec);
		obj->playing = FALSE;  // FXIME: 停止後にFALSEにするべき
	}

	return OK;
}

// PCMデータのメモリ上からのアンロード
static int unload(int slot) {
	pcmobj_t *obj;
	
	obj = pcmobj[slot];
	if (obj == NULL) return NG;
	
	if (obj->playing) muspcm_stop(slot);
	
	Mix_FreeChunk(obj->chunk);
	free(obj);
	
	pcmobj[slot] = NULL;
	
	return OK;
}

// PCMデータ再生一時停止
int muspcm_pause(int slot) {
	if (pcmobj[slot] != NULL) {
		Mix_Pause(pcmobj[slot]->channel);
	}
	return OK;
}

// PCMデータ再生一時停止解除
int muspcm_unpause(int slot) {
	if (pcmobj[slot] != NULL) {
		Mix_Resume(pcmobj[slot]->channel);
	}
	return OK;
}

// 現在の再生位置を返す
int muspcm_getpos(int slot) {
	pcmobj_t *obj;
	
	obj = pcmobj[slot];
	if (obj == NULL) return 0;
	
	if (!obj->playing) return 0;
	
	long len = obj->chunk->alen * 1000 / (44100 * 4);
	int pos = get_high_counter(SYSTEMCOUNTER_MSEC) - obj->start_time;
	if (pos > len)
		pos = 0;
	return pos;
}

// PCMオブジェクトに対してボリュームをセット
int muspcm_setvol(int dev, int slot, int lv) {
	pcmobj_t *obj;
	
	obj = pcmobj[slot];
	if (obj == NULL) return NG;
	
	Mix_VolumeChunk(obj->chunk, lv * MIX_MAX_VOLUME / 100);
	
	return OK;
}

// PCMデータの長さを取得
int muspcm_getwavelen(int slot) {
	pcmobj_t *obj;
	
	obj = pcmobj[slot];
	if (obj == NULL) return 0;
	
	long len = obj->chunk->alen * 1000 / (44100 * 4);

	return len > 65535 ? 65535 : len;
}

// 指定のスロットが現在演奏中かどうかを取得
boolean muspcm_isplaying(int slot) {
	pcmobj_t *obj;
	
	obj = pcmobj[slot];
	if (obj == NULL)   return FALSE;
	
	return obj->playing;
}

// 指定のチャンネルの再生が終了するまで待つ
int muspcm_waitend(int slot) {
	printf("%s not implemented\n", __func__);
	return NG;
}

static int load_wai() {
	struct stat sbuf;
	char *adr;
	int fd;

	wai_mapadr = NULL;
	if (nact->files.wai == NULL) goto endwai;
	
	if (0 > (fd = open(nact->files.wai, O_RDONLY))) {
		WARNING("open: %s\n", strerror(errno));
		goto endwai;
	}
	
	if (0 > fstat(fd, &sbuf)) {
		WARNING("fstat:%s\n", strerror(errno));
		close(fd);
		goto endwai;
	}
	
	if (MAP_FAILED == (adr = mmap(0, sbuf.st_size, PROT_READ, MAP_SHARED, fd, 0))) {
		WARNING("mmap: %s\n", strerror(errno));
		close(fd);
		goto endwai;
	}
	
	if (*adr != 'X' || *(adr+1) != 'I' || *(adr+2) != '2') {
		WARNING("not WAI file\n");
		munmap(adr, sbuf.st_size);
		close(fd);
		goto endwai;
	}
	
	wai_mapadr = adr;
	
 endwai:
	return OK;
}
