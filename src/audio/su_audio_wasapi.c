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
#include "su_audio_int.h"
#include "su_audio_output.h"

/* GUIDs written out here so no extra import library is needed. */
static const GUID SU_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID SU_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID SU_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID SU_IID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID SU_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const PROPERTYKEY SU_PKEY_Device_FriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

struct SuAudioDevice {
    SuAudioMixer *mixer;
    SuAudioPullFn pull;     /* pull-mode source (integrated runtime ring) instead of the mixer */
    void *pull_user;
    HANDLE thread;
    HANDLE ready;           /* signalled when initialisation finished (status in init_status) */
    HANDLE wake;            /* the WASAPI buffer event */
    atomic_int stop;
    _Atomic uint64_t frames;
    _Atomic uint64_t callbacks, callback_total_us, callback_max_us, callback_over_budget;
    _Atomic uint64_t callback_us[SU_AUDIO_DEVICE_HISTOGRAM_BINS];
    _Atomic uint64_t interval_us[SU_AUDIO_DEVICE_HISTOGRAM_BINS];
    _Atomic uint64_t interval_max_us, empty_buffers, wait_timeouts, backend_errors;
    uint32_t init_status;
    char message[256];
    SuAudioDeviceInfo info;
    SuAudioGap recovery;
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
    LARGE_INTEGER frequency;
    LARGE_INTEGER previous;
    int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    frequency.QuadPart = 1;
    previous.QuadPart = 0;
    (void)QueryPerformanceFrequency(&frequency);
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
        LARGE_INTEGER begin;
        LARGE_INTEGER end;
        uint64_t elapsed = 0u;
        begin.QuadPart = 0;
        end.QuadPart = 0;
        DWORD w = WaitForSingleObject(d->wake, 200);
        if (atomic_load(&d->stop)) {
            break;
        }
        if (w != WAIT_OBJECT_0 && w != WAIT_TIMEOUT) {
            atomic_fetch_add(&d->backend_errors, 1u);
            break;
        }
        (void)QueryPerformanceCounter(&begin);
        if (w == WAIT_TIMEOUT) {
            atomic_fetch_add(&d->wait_timeouts, 1u);
        }
        if (previous.QuadPart != 0) {
            uint64_t interval = (uint64_t)((begin.QuadPart - previous.QuadPart) * 1000000 / frequency.QuadPart);
            atomic_fetch_add(&d->interval_us[su_audio_time_bin(interval)], 1u);
            if (interval > atomic_load(&d->interval_max_us)) {
                atomic_store(&d->interval_max_us, interval);
            }
        }
        previous = begin;
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || padding > frames) {
            atomic_fetch_add(&d->backend_errors, 1u);
            break; /* device lost: stop rendering (close still works) */
        }
        if (frames - padding == 0u) {
            continue;
        }
        if (FAILED(IAudioRenderClient_GetBuffer(render, frames - padding, &buf))) {
            atomic_fetch_add(&d->backend_errors, 1u);
            break;
        }
        if (padding == 0u) {
            atomic_fetch_add(&d->empty_buffers, 1u);
            if (atomic_load(&d->frames) != 0u) {
                memset(&d->recovery, 0, sizeof d->recovery);
                d->recovery.gap = 1;
            }
        }
        if (d->pull != NULL) {
            d->pull(d->pull_user, (float *)(void *)buf, frames - padding);
        } else {
            su_audio_mixer_render(d->mixer, (float *)(void *)buf, frames - padding);
        }
        if (d->recovery.gap || d->recovery.recovering != 0u) {
            su_audio_gap_apply(&d->recovery, (float *)(void *)buf, frames - padding, frames - padding);
        }
        if (FAILED(IAudioRenderClient_ReleaseBuffer(render, frames - padding, 0))) {
            atomic_fetch_add(&d->backend_errors, 1u);
            break;
        }
        (void)QueryPerformanceCounter(&end);
        elapsed = (uint64_t)((end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart);
        atomic_fetch_add(&d->callback_us[su_audio_time_bin(elapsed)], 1u);
        atomic_fetch_add(&d->callback_total_us, elapsed);
        atomic_fetch_add(&d->callbacks, 1u);
        if (elapsed > atomic_load(&d->callback_max_us)) {
            atomic_store(&d->callback_max_us, elapsed);
        }
        if (elapsed * d->info.sample_rate > (uint64_t)(frames - padding) * 1000000u) {
            atomic_fetch_add(&d->callback_over_budget, 1u);
        }
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

static uint32_t device_start(SuAudioMixer *m, SuAudioPullFn pull, void *user, uint32_t rate, SuAudioDevice **out,
                             SuAudioDeviceInfo *info, char message[256]) {
    SuAudioDevice *d;
    *out = NULL;
    d = calloc(1, sizeof *d);
    if (d == NULL) {
        return SU_E_RESOURCE_LIMIT;
    }
    d->mixer = m;
    d->pull = pull;
    d->pull_user = user;
    atomic_init(&d->stop, 0);
    atomic_init(&d->frames, 0u);
    atomic_init(&d->callbacks, 0u);
    atomic_init(&d->callback_total_us, 0u);
    atomic_init(&d->callback_max_us, 0u);
    atomic_init(&d->callback_over_budget, 0u);
    atomic_init(&d->interval_max_us, 0u);
    atomic_init(&d->empty_buffers, 0u);
    atomic_init(&d->wait_timeouts, 0u);
    atomic_init(&d->backend_errors, 0u);
    {
        uint32_t bin;
        for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
            atomic_init(&d->callback_us[bin], 0u);
            atomic_init(&d->interval_us[bin], 0u);
        }
    }
    d->info.sample_rate = rate;
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

void su_audio_device_stats(const SuAudioDevice *device, SuAudioDeviceStats *out) {
    uint32_t bin;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (device == NULL) {
        return;
    }
    out->callbacks = atomic_load(&device->callbacks);
    out->callback_total_us = atomic_load(&device->callback_total_us);
    out->callback_max_us = atomic_load(&device->callback_max_us);
    out->callback_over_budget = atomic_load(&device->callback_over_budget);
    out->interval_max_us = atomic_load(&device->interval_max_us);
    out->empty_buffers = atomic_load(&device->empty_buffers);
    out->wait_timeouts = atomic_load(&device->wait_timeouts);
    out->backend_errors = atomic_load(&device->backend_errors);
    out->stream_latency_ms = device->info.latency_ms;
    out->sample_rate = device->info.sample_rate;
    out->buffer_frames = device->info.buffer_frames;
    memcpy(out->name, device->info.name, sizeof out->name);
    for (bin = 0u; bin < SU_AUDIO_DEVICE_HISTOGRAM_BINS; bin++) {
        out->callback_us[bin] = atomic_load(&device->callback_us[bin]);
        out->interval_us[bin] = atomic_load(&device->interval_us[bin]);
    }
}

uint32_t su_audio_device_open(SuAudioMixer *m, SuAudioDevice **out, SuAudioDeviceInfo *info, char message[256]) {
    if (m == NULL || out == NULL) {
        return SU_E_INVALID_ARGUMENT;
    }
    return device_start(m, NULL, NULL, su_audio_mixer_rate(m), out, info, message);
}

uint32_t su_audio_device_open_pull(uint32_t sample_rate, SuAudioPullFn pull, void *user, SuAudioDevice **out,
                                   SuAudioDeviceInfo *info, char message[256]) {
    if (pull == NULL || out == NULL || sample_rate < 1000u || sample_rate > 384000u) {
        return SU_E_INVALID_ARGUMENT;
    }
    return device_start(NULL, pull, user, sample_rate, out, info, message);
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
