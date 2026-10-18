/* Windows output for sutekh/audio.h: WASAPI shared mode, event driven, float32 stereo at the mixer's rate (the
 * audio engine converts with AUTOCONVERTPCM), one render thread that calls su_audio_mixer_render. All COM work
 * happens on that thread; su_audio_device_open waits for its initialisation result. */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sutekh/audio.h"

/* GUIDs written out here so no extra import library is needed. */
static const GUID SU_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID SU_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID SU_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID SU_IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID SU_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const PROPERTYKEY SU_PKEY_Device_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

struct SuAudioDevice {
    SuAudioMixer *mixer;
    HANDLE thread;
    HANDLE ready;           /* signalled when initialisation finished (status in init_status) */
    HANDLE wake;            /* the WASAPI buffer event */
    atomic_int stop;
    _Atomic uint64_t frames;
    uint32_t init_status;
    char message[256];
    SuAudioDeviceInfo info;
    float *scratch;
};

static void msgf(SuAudioDevice *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(d->message, sizeof d->message, fmt, ap);
    va_end(ap);
}

static DWORD WINAPI device_main(LPVOID arg) {
    SuAudioDevice *d = arg;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    WAVEFORMATEXTENSIBLE wf;
    UINT32 frames = 0;
    REFERENCE_TIME latency = 0;
    HRESULT hr;
    int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    hr = CoCreateInstance(&SU_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &SU_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) {
        msgf(d, "MMDeviceEnumerator unavailable (0x%08lx)", (unsigned long)hr);
        goto fail;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) {
        msgf(d, "no default audio output device (0x%08lx)", (unsigned long)hr);
        goto fail;
    }
    {
        IPropertyStore *ps = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(IPropertyStore_GetValue(ps, &SU_PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                (void)WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, d->info.name, (int)sizeof d->info.name, NULL, NULL);
            }
            PropVariantClear(&pv);
            IPropertyStore_Release(ps);
        }
    }
    hr = IMMDevice_Activate(dev, &SU_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr)) {
        msgf(d, "IAudioClient activation failed (0x%08lx)", (unsigned long)hr);
        goto fail;
    }
    memset(&wf, 0, sizeof wf);
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = d->info.sample_rate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = 8;
    wf.Format.nAvgBytesPerSec = d->info.sample_rate * 8u;
    wf.Format.cbSize = sizeof wf - sizeof wf.Format;
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = SU_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 200000 /* 20 ms */, 0, &wf.Format, NULL);
    if (FAILED(hr)) {
        msgf(d, "IAudioClient::Initialize refused float32 stereo at %u Hz (0x%08lx)", (unsigned)d->info.sample_rate,
             (unsigned long)hr);
        goto fail;
    }
    if (FAILED(IAudioClient_GetBufferSize(client, &frames)) || FAILED(IAudioClient_SetEventHandle(client, d->wake)) ||
        FAILED(IAudioClient_GetService(client, &SU_IID_IAudioRenderClient, (void **)&render))) {
        msgf(d, "WASAPI buffer, event or render service unavailable");
        goto fail;
    }
    (void)IAudioClient_GetStreamLatency(client, &latency);
    d->info.buffer_frames = frames;
    d->info.latency_ms = (double)latency / 10000.0;
    d->scratch = malloc((size_t)frames * 2u * sizeof(float));
    if (d->scratch == NULL) {
        msgf(d, "allocation");
        goto fail;
    }
    if (FAILED(IAudioClient_Start(client))) {
        msgf(d, "IAudioClient::Start failed");
        goto fail;
    }
    d->init_status = SU_OK;
    SetEvent(d->ready);
    while (!atomic_load(&d->stop)) {
        UINT32 padding = 0;
        BYTE *buf = NULL;
        DWORD w = WaitForSingleObject(d->wake, 200);
        if (w != WAIT_OBJECT_0 && w != WAIT_TIMEOUT) {
            break;
        }
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding > frames) {
            break; /* device lost: stop rendering (close still works) */
        }
        if (frames - padding == 0u) {
            continue;
        }
        if (FAILED(IAudioRenderClient_GetBuffer(render, frames - padding, &buf))) {
            break;
        }
        su_audio_mixer_render(d->mixer, (float *)(void *)buf, frames - padding);
        (void)IAudioRenderClient_ReleaseBuffer(render, frames - padding, 0);
        atomic_fetch_add(&d->frames, frames - padding);
    }
    (void)IAudioClient_Stop(client);
    IAudioRenderClient_Release(render);
    IAudioClient_Release(client);
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    if (com) {
        CoUninitialize();
    }
    return 0;
fail:
    if (render != NULL) {
        IAudioRenderClient_Release(render);
    }
    if (client != NULL) {
        IAudioClient_Release(client);
    }
    if (dev != NULL) {
        IMMDevice_Release(dev);
    }
    if (en != NULL) {
        IMMDeviceEnumerator_Release(en);
    }
    if (com) {
        CoUninitialize();
    }
    d->init_status = SU_E_UNSUPPORTED_METHOD;
    SetEvent(d->ready);
    return 1;
}

uint32_t su_audio_device_open(SuAudioMixer *m, SuAudioDevice **out, SuAudioDeviceInfo *info, char message[256]) {
    SuAudioDevice *d;
    if (m == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    *out = NULL;
    d = calloc(1, sizeof *d);
    if (d == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    d->mixer = m;
    atomic_init(&d->stop, 0);
    atomic_init(&d->frames, 0u);
    d->info.sample_rate = su_audio_mixer_rate(m);
    d->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (d->ready == NULL || d->wake == NULL) {
        su_audio_device_close(d);
        return SU_E_RESOURCE_LIMIT;
    }
    d->thread = CreateThread(NULL, 0, device_main, d, 0, NULL);
    if (d->thread == NULL) {
        su_audio_device_close(d);
        return SU_E_RESOURCE_LIMIT;
    }
    (void)WaitForSingleObject(d->ready, INFINITE);
    if (d->init_status != SU_OK) {
        if (message != NULL) {
            (void)snprintf(message, 256, "%s", d->message);
        }
        su_audio_device_close(d);
        return SU_E_UNSUPPORTED_METHOD;
    }
    if (info != NULL) {
        *info = d->info;
    }
    *out = d;
    return SU_OK;
}

void su_audio_device_close(SuAudioDevice *d) {
    if (d == NULL) {
        return;
    }
    if (d->thread != NULL) {
        atomic_store(&d->stop, 1);
        SetEvent(d->wake);
        (void)WaitForSingleObject(d->thread, INFINITE);
        CloseHandle(d->thread);
    }
    if (d->ready != NULL) {
        CloseHandle(d->ready);
    }
    if (d->wake != NULL) {
        CloseHandle(d->wake);
    }
    free(d->scratch);
    free(d);
}

uint64_t su_audio_device_frames(const SuAudioDevice *d) {
    return d != NULL ? atomic_load(&((SuAudioDevice *)d)->frames) : 0u;
}
