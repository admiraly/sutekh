/* Output-device smoke test (sutekh/audio.h): opens the default device, plays a quiet 440 Hz tone for 0.4 s and a
 * spatial voice that moves, and checks that the device thread rendered at least that much audio. It cannot hear the
 * result; it checks the stream ran. Exit 77 when no backend or device exists (CI runners, non-Windows). */
/* SPDX-FileCopyrightText: 2026 admiraly */
/* SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sutekh/audio.h"

static double wall_s(void) {
    struct timespec ts;
    (void)timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    SuAudioMixerDesc md;
    SuAudioMixer *m = NULL;
    SuAudioDevice *dev = NULL;
    SuAudioDeviceInfo info;
    SuAudioClip *tone = NULL;
    SuAudioVoiceDesc vd;
    SuAudioVoice v = 0;
    SuAudioStats st;
    char msg[256];
    uint32_t s;
    double t0;
    int fails = 0;
    memset(&md, 0, sizeof md);
    md.struct_size = sizeof md;
    if (su_audio_mixer_create(&md, &m) != SU_OK ||
        su_audio_clip_tone(SU_AUDIO_WAVE_SINE, 440.0f, 0.5f, 48000u, 1, &tone) != SU_OK) {
        return 1;
    }
    s = su_audio_device_open(m, &dev, &info, msg);
    if (s != SU_OK) {
        printf("audio device unavailable: %s\n", msg);
        su_audio_mixer_destroy(m);
        su_audio_clip_free(tone);
        return s == SU_E_UNSUPPORTED_METHOD ? 77 : 1;
    }
    printf("device: %s, %u Hz, buffer %u frames, stream latency %.2f ms\n", info.name, (unsigned)info.sample_rate,
           (unsigned)info.buffer_frames, info.latency_ms);
    memset(&vd, 0, sizeof vd);
    vd.clip = tone;
    vd.gain = 0.05f;
    vd.flags = SU_AUDIO_LOOP | SU_AUDIO_SPATIAL;
    vd.pos = su_wpos(-2.0, 0.0, -1.0);
    vd.doppler = 1.0f;
    if (su_audio_play(m, &vd, &v) != SU_OK) {
        fails++;
    }
    t0 = wall_s();
    /* A device backend may take a moment to start (a PipeWire or PulseAudio node connects before its first pull);
     * the 0.4 s below are measured from the first rendered block, so that start-up is not counted as audio. */
    while (su_audio_device_frames(dev) == 0u && wall_s() - t0 < 5.0) {
        su_audio_update(m);
    }
    if (su_audio_device_frames(dev) == 0u) {
        printf("FAIL: the device thread never rendered\n");
        fails++;
    }
    t0 = wall_s();
    while (wall_s() - t0 < 0.4) {
        /* sweep left to right at a 200 Hz "frame rate", as a game thread would from interpolated transforms */
        double now = wall_s();
        double k = (now - t0) / 0.4;
        SuVec3f vel = {10.0f, 0.0f, 0.0f};
        if (su_audio_voice_update(m, v, su_wpos(-2.0 + 4.0 * k, 0.0, -1.0), vel, 0.05f) != SU_OK) {
            fails++;
        }
        su_audio_update(m);
        while (wall_s() - now < 0.005) {
        }
    }
    if (su_audio_stop(m, v, 0.02f) != SU_OK) {
        printf("FAIL: stop refused\n");
        fails++;
    }
    t0 = wall_s();
    while (wall_s() - t0 < 0.1) {
        su_audio_update(m);
    }
    su_audio_mixer_stats(m, &st);
    printf("rendered %llu frames by the device thread (mixer total %llu), peak %.3f\n",
           (unsigned long long)su_audio_device_frames(dev), (unsigned long long)st.frames_rendered, (double)st.peak);
    if (su_audio_device_frames(dev) < 48000u * 4u / 10u) {
        printf("FAIL: fewer frames than 0.4 s of audio\n");
        fails++;
    }
    su_audio_device_close(dev);
    su_audio_update(m);
    if (su_audio_voice_active(m, v)) {
        printf("FAIL: the stopped voice did not end\n");
        fails++;
    }
    su_audio_mixer_destroy(m);
    su_audio_clip_free(tone);
    printf("test_audio_device: %s\n", fails == 0 ? "ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
