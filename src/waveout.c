/* Windows waveOUT API Output.
 *
 * Copyright (C) 2016 Reece H. Dunn, 2018 Martin Schreiber
 *
 * This file is part of pcaudiolib.
 *
 * pcaudiolib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pcaudiolib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pcaudiolib.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "audio_priv.h"
#include <stdbool.h>

#include <Mmsystem.h>
#pragma comment(lib, "Winmm.lib")

struct wavebuffer
{
	WAVEHDR header;  //first!
	size_t buffersize;
	HANDLE ready; //event object
};

typedef struct wavebuffer *pwavebuffer;

#define BUFFERMASK 3
#define BUFFERCOUNT (BUFFERMASK+1)

struct waveout_object
{
	struct audio_object vtable;
	HWAVEOUT waveout;
	LPWAVEFORMATEX format;
	LPWSTR devicename;
	UINT deviceid;
	int bufferindex;
	struct wavebuffer bufferar[BUFFERCOUNT];
};
typedef struct waveout_object *pwaveout_object;

static void
inibuffer(pwavebuffer buffer)
{
	buffer->ready = CreateEvent(NULL,TRUE,TRUE,NULL);
	buffer->buffersize = 0;
	buffer->header.dwFlags = 0;
	buffer->header.lpData = NULL;
}

static void
finibuffer(pwavebuffer buffer)
{
	if (buffer->header.lpData) {
		free(buffer->header.lpData);
	};
	CloseHandle(buffer->ready);
}


void
waveout_object_close(struct audio_object *object);

#define to_waveout_object(object) container_of(object, struct waveout_object, vtable)

static bool
unprepare(pwaveout_object self, pwavebuffer buffer) //returns true if busy
{
	MMRESULT mr;

	if (buffer->header.dwFlags & WHDR_PREPARED) {
		mr = waveOutUnprepareHeader(self->waveout, &buffer->header,
														sizeof(WAVEHDR));
		return (mr == WAVERR_STILLPLAYING);
	}
	else {
		return false;
	}
};

static void 
CALLBACK waveoutcallback(HWAVEOUT hwo,UINT uMsg,DWORD_PTR dwInstance,
								DWORD_PTR dwParam1,	DWORD_PTR dwParam2)
{
	pwaveout_object self;
	int i1;

	switch (uMsg){
	case WOM_DONE:
		SetEvent(((pwavebuffer)dwParam1)->ready);
		break;
	case WOM_CLOSE:
		self = dwInstance;
		for (i1 = 0; i1 <= BUFFERMASK; i1++) {
			SetEvent(self->bufferar[i1].ready);
		}
		break;
	}
};

int
waveout_object_write(struct audio_object *object,
	const void *data,
	size_t bytes)
{
	struct waveout_object *self = to_waveout_object(object);
	MMRESULT mr;
	pwavebuffer buffer;
	LONG i1;

	if (bytes) {
		i1 = InterlockedIncrement(&self->bufferindex);
		buffer = &self->bufferar[i1 & BUFFERMASK];

		WaitForSingleObject(buffer->ready, INFINITE);
		if (buffer->buffersize < bytes) {
			if (unprepare(self, buffer)) goto error;
			if (buffer->header.lpData) {
				free(buffer->header.lpData);
			}
			buffer->header.lpData = malloc(bytes);
			if (!buffer->header.lpData && bytes) {
				buffer->buffersize = 0;
				return errno;
			}
			buffer->buffersize = bytes;
			buffer->header.dwFlags = 0;
			mr = waveOutPrepareHeader(self->waveout, &buffer->header,
															sizeof(WAVEHDR));
			if (mr != MMSYSERR_NOERROR) goto error;
		}
		ResetEvent(buffer->ready);
		buffer->header.dwBufferLength = bytes;
		memcpy(buffer->header.lpData, data, bytes);
		mr = waveOutWrite(self->waveout, &buffer->header, sizeof(WAVEHDR));
		if (mr != MMSYSERR_NOERROR) goto error;
	};
	return S_OK;
error:
	return mr;
}

int
waveout_object_read(struct audio_object *object,
	const void *data,
	size_t bytes)
{
	struct waveout_object *self = to_waveout_object(object);
	MMRESULT mr;
	pwavebuffer buffer;
	LONG i1;

	if (bytes) {
		i1 = InterlockedIncrement(&self->bufferindex);
		buffer = &self->bufferar[i1 & BUFFERMASK];

		WaitForSingleObject(buffer->ready, INFINITE);
		if (buffer->buffersize < bytes) {
			if (unprepare(self, buffer)) goto error;
			if (buffer->header.lpData) {
				free(buffer->header.lpData);
			}
			buffer->header.lpData = malloc(bytes);
			if (!buffer->header.lpData && bytes) {
				buffer->buffersize = 0;
				return errno;
			}
			buffer->buffersize = bytes;
			buffer->header.dwFlags = 0;
			mr = waveOutPrepareHeader(self->waveout, &buffer->header,
															sizeof(WAVEHDR));
			if (mr != MMSYSERR_NOERROR) goto error;
		}
		ResetEvent(buffer->ready);
		buffer->header.dwBufferLength = bytes;
		memcpy(buffer->header.lpData, data, bytes);
		mr = waveOutWrite(self->waveout, &buffer->header, sizeof(WAVEHDR));
		if (mr != MMSYSERR_NOERROR) goto error;
	};
	return S_OK;
error:
	return mr;
}

int
waveout_object_open(struct audio_object *object,
                    enum audio_object_format format,
                    uint32_t rate,
                    uint8_t channels)
{
	struct waveout_object *self = to_waveout_object(object);
	HRESULT hr;
	MMRESULT mr;
	int err,i1;

	self->bufferindex = 0;
	for (i1 = 0; i1 <= BUFFERMASK; i1++) {
		inibuffer(&self->bufferar[i1]);
	}
	hr = CreateWaveFormat(format, rate, channels, &self->format);
	     //CoTaskMemAlloc() not necessary, normal mem could be used
	if (FAILED(hr)) {
		err = hr;
		goto error;
	}

	mr = waveOutOpen(&self->waveout, self->deviceid, self->format, 
			&waveoutcallback, self, CALLBACK_FUNCTION | WAVE_ALLOWSYNC);

	if (mr != MMSYSERR_NOERROR) {
		err = mr;
		goto error;
	};
	return S_OK;
error:
	waveout_object_close(object);
	return err;
}

int
waveout_object_openrec(struct audio_object *object,
                    enum audio_object_format format,
                    uint32_t rate,
                    uint8_t channels)
{
	struct waveout_object *self = to_waveout_object(object);
	HRESULT hr;
	MMRESULT mr;
	int err,i1;

	self->bufferindex = 0;
	for (i1 = 0; i1 <= BUFFERMASK; i1++) {
		inibuffer(&self->bufferar[i1]);
	}
	hr = CreateWaveFormat(format, rate, channels, &self->format);
	     //CoTaskMemAlloc() not necessary, normal mem could be used
	if (FAILED(hr)) {
		err = hr;
		goto error;
	}

	mr = waveOutOpen(&self->waveout, self->deviceid, self->format, 
			&waveoutcallback, self, CALLBACK_FUNCTION | WAVE_ALLOWSYNC);

	if (mr != MMSYSERR_NOERROR) {
		err = mr;
		goto error;
	};
	return S_OK;
error:
	waveout_object_close(object);
	return err;
}

void
waveout_object_close(struct audio_object *object)
{
	struct waveout_object *self = to_waveout_object(object);
	int i1;

	waveOutClose(self->waveout);
	for (i1 = 0; i1 <= BUFFERMASK; i1++) {
		while (unprepare(self, &self->bufferar[i1])) { 
			Sleep(0);
		};
	}
	for (i1 = 0; i1 <= BUFFERMASK; i1++) {
		WaitForSingleObject(self->bufferar[i1].ready, INFINITE);
	}
	for (i1 = 0; i1 <= BUFFERMASK; i1++) {
		finibuffer(&self->bufferar[i1]);
	}
	if (self->format != NULL) {
		CoTaskMemFree(self->format);
		self->format = NULL;
	}
}

void
waveout_object_destroy(struct audio_object *object)
{
	struct waveout_object *self = to_waveout_object(object);
	free(self->devicename);
	free(self);
}

int
waveout_object_drain(struct audio_object *object)
{
	struct waveout_object *self = to_waveout_object(object);
	int i1;
    
	for (i1 = 0; i1 <= BUFFERMASK; i1++) {
		WaitForSingleObject(self->bufferar[i1].ready, INFINITE);
	}
	return S_OK;
}

int
waveout_object_flush(struct audio_object *object)
{
	struct waveout_object *self = to_waveout_object(object);

	waveOutReset(self->waveout);
	return S_OK;
}


struct audio_object *
create_waveout_object(const char *device,
                      const char *application_name,
                      const char *description)
{
	struct waveout_object *self = (struct waveout_object *)
									malloc(sizeof(struct waveout_object));
	UINT_PTR i1, i2;
	WAVEOUTCAPS caps;
	LPCWSTR p1, p2, p3;

	if (!self)
		return NULL;
	self->waveout = 0;
	self->devicename = device ? str2wcs(device) : NULL;
	self->deviceid = WAVE_MAPPER;
	if (self->devicename) {
		i2 = waveOutGetNumDevs();
		for (i1 = 0; i1 < i2; i1++) {
			if (waveOutGetDevCaps(i1, &caps, sizeof(WAVEOUTCAPS)) ==
														MMSYSERR_NOERROR) {
				p1 = self->devicename;
				p2 = caps.szPname;
				p3 = p2;
				while ((*p1 == *p2) && (*p1) && (*p2)) {
					p1++;
					p2++;
				}
				if ((!*p1) && (!*p2) || ((p2-p3)== MAXPNAMELEN-1)) {
					self->deviceid = i1;
					break;
				}
			}
		};
	};
	
	self->vtable.open = waveout_object_open;
	self->vtable.openrec = waveout_object_openrec;
	self->vtable.close = waveout_object_close;
	self->vtable.destroy = waveout_object_destroy;
	self->vtable.write = waveout_object_write;
	self->vtable.read = waveout_object_read;
	self->vtable.drain = waveout_object_drain;
	self->vtable.flush = waveout_object_flush;
	self->vtable.strerror = windows_hresult_strerror;

	return &self->vtable;
}