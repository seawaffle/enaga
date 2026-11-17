#include "engine.hpp"

using namespace enaga::dsp;

namespace synth {
namespace {

float percent(int v) { return float(v) * 0.01f; }

}  // namespace

void Engine::init(float sample_rate) {
    rate_ = sample_rate;
    inv_rate_ = 1.0f / sample_rate;
    delay_.init(1.6f, sample_rate);
    reverb_.init(sample_rate);
    for (int p = 0; p < param_count; p++) values[p] = info(p).init;
    update_settings();
}

bool Engine::post(const NoteEvent &event) {
    uint32_t w = queue_write_;
    if (w - queue_read_ >= 64) return false;
    queue_[w & 63] = event;
    asm volatile("" ::: "memory");                // the event is in place before the index moves
    queue_write_ = w + 1;
    return true;
}

void Engine::update_settings() {
    int16_t v[param_count];
    bool changed = first_;
    for (int p = 0; p < param_count; p++) {
        v[p] = values[p];
        changed |= v[p] != applied_[p];
    }
    if (!changed) return;
    first_ = false;
    for (int p = 0; p < param_count; p++) applied_[p] = v[p];

    const float control_rate = rate_ / float(kControl);
    s_.wave1 = enaga::dsp::Wave(v[osc1_wave]);
    s_.wave2 = enaga::dsp::Wave(v[osc2_wave]);
    s_.width1 = percent(v[osc1_width]);
    s_.width2 = percent(v[osc2_width]);
    s_.offset1 = float(v[osc1_semi]) + percent(v[osc1_fine]);
    s_.offset2 = float(v[osc2_semi]) + percent(v[osc2_fine]);
    s_.level1 = percent(v[mix_osc1]);
    s_.level2 = percent(v[mix_osc2]);
    s_.level_sub = percent(v[mix_sub]);
    s_.level_noise = percent(v[mix_noise]);
    s_.filter_type = uint8_t(v[filter_type]);
    s_.cutoff = float(v[filter_cutoff] + 8);
    s_.resonance = percent(v[filter_resonance]);
    s_.drive = 1.0f + 0.07f * float(v[filter_drive]);
    s_.filter_env = 0.6f * float(v[filter_env]);  // +-60 semitones
    s_.filter_key = percent(v[filter_key]);
    s_.filter_velocity = 0.36f * float(v[filter_velocity]);   // up to 36 semitones darker at the softest
    s_.amp = EnvelopeShape::make(seconds(v[amp_attack]), seconds(v[amp_decay]), percent(v[amp_sustain]),
                                 seconds(v[amp_release]), rate_);
    s_.env2 = EnvelopeShape::make(seconds(v[env2_attack]), seconds(v[env2_decay]), percent(v[env2_sustain]),
                                  seconds(v[env2_release]), control_rate);
    s_.env3 = EnvelopeShape::make(seconds(v[env3_attack]), seconds(v[env3_decay]), percent(v[env3_sustain]),
                                  seconds(v[env3_release]), control_rate);
    for (int i = 0; i < 2; i++) {
        int base = i ? lfo2_shape : lfo1_shape;
        s_.lfo_shape[i] = LfoShape(v[base]);
        s_.lfo_rate[i] = rate_hz(v[base + 1]) / control_rate;
        s_.lfo_key_sync[i] = v[base + 2] != 0;
    }
    s_.slot_count = 0;
    for (int i = 0; i < mod_slots; i++)
        if (v[mod_source(i)] != src_off && v[mod_amount(i)] != 0)
            s_.slots[s_.slot_count++] = {uint8_t(v[mod_source(i)]), uint8_t(v[mod_destination(i)]),
                                         percent(v[mod_amount(i)])};
    s_.mode = uint8_t(v[voice_mode]);
    s_.glide = v[voice_glide] == 0 ? 1.0f : 1.0f - exp(-EnvelopeShape::kSpan / (seconds(v[voice_glide]) * control_rate));
    s_.velocity_amp = percent(v[voice_velocity]);
    s_.volume = percent(v[voice_volume]) * percent(v[voice_volume]);

    delay_.set(delay_s(v[delay_time]), percent(v[delay_feedback]), percent(v[delay_tone]), v[delay_mode] != 0);
    delay_mix_ = percent(v[delay_mix]);
    reverb_.set(decay_s(v[reverb_decay]), percent(v[reverb_damping]));
    reverb_mix_ = 0.5f * percent(v[reverb_mix]);
}

Engine::Voice &Engine::allocate() {
    Voice *best = nullptr;
    for (Voice &v : voices_)                      // a free voice
        if (!v.amp.active()) return v;
    for (Voice &v : voices_)                      // else the oldest released one
        if (!v.gate && (!best || v.started < best->started)) best = &v;
    if (best) return *best;
    for (Voice &v : voices_)                      // else the oldest
        if (!best || v.started < best->started) best = &v;
    return *best;
}

void Engine::start(Voice &voice, const NoteEvent &event, bool retrigger) {
    voice.key = event.key;
    voice.note = event.note;
    voice.gate = true;
    voice.target = float(event.note);
    voice.velocity = float(event.value) * (1.0f / 127.0f);
    if (!retrigger) return;                       // LEGATO: the same voice slides on
    voice.started = ++clock_;
    voice.pressure = 0.0f;
    voice.random = random_.bipolar();
    bool glide = s_.glide < 1.0f && (s_.mode != mode_legato);
    voice.pitch = glide ? last_pitch_ : voice.target;
    voice.osc1.reset();
    voice.osc2.reset();
    voice.sub.reset();
    voice.svf1.reset();
    voice.svf2.reset();
    voice.amp.gate_on();
    voice.env2.gate_on();
    voice.env3.gate_on();
    voice.lfo1.reset(s_.lfo_key_sync[0] ? 0.0f : free_lfo_[0].phase());
    voice.lfo2.reset(s_.lfo_key_sync[1] ? 0.0f : free_lfo_[1].phase());
}

void Engine::handle(const NoteEvent &e) {
    switch (e.type) {
    case NoteEvent::on:
        if (s_.mode == mode_poly) {
            start(allocate(), e, true);
        } else {
            if (held_count_ == 16) {              // forget the oldest
                for (int i = 1; i < 16; i++) held_[i - 1] = held_[i];
                held_count_--;
            }
            held_[held_count_++] = {e.key, e.note, e.value};
            Voice &v = voices_[0];
            bool legato = s_.mode == mode_legato && v.gate;
            start(v, e, !legato);
        }
        last_pitch_ = float(e.note);
        break;
    case NoteEvent::off:
        if (s_.mode == mode_poly) {
            for (Voice &v : voices_)
                if (v.gate && v.key == e.key) {
                    v.gate = false;
                    v.amp.gate_off();
                    v.env2.gate_off();
                    v.env3.gate_off();
                }
        } else {
            int i = 0;
            while (i < held_count_ && held_[i].key != e.key) i++;
            if (i == held_count_) break;
            for (; i + 1 < held_count_; i++) held_[i] = held_[i + 1];
            held_count_--;
            Voice &v = voices_[0];
            if (v.key != e.key || !v.gate) break;
            if (held_count_) {                    // back to the last key still held
                const Held &h = held_[held_count_ - 1];
                start(v, {NoteEvent::on, h.key, h.note, h.velocity}, s_.mode == mode_mono);
            } else {
                v.gate = false;
                v.amp.gate_off();
                v.env2.gate_off();
                v.env3.gate_off();
            }
        }
        break;
    case NoteEvent::pressure:
        for (Voice &v : voices_)
            if (v.gate && v.key == e.key) v.pressure = float(e.value) * (1.0f / 127.0f);
        break;
    case NoteEvent::all_off:
        for (Voice &v : voices_) {
            v.gate = false;
            v.amp.kill();
            v.env2.kill();
            v.env3.kill();
        }
        held_count_ = 0;
        break;
    }
}

void Engine::render_voice(Voice &v, float *left, float *right) {
    // Modulation, once per control step.
    float src[source_count];
    src[src_off] = 0.0f;
    src[src_env2] = v.env2.step(s_.env2);
    src[src_env3] = v.env3.step(s_.env3);
    src[src_lfo1] = v.lfo1.step(s_.lfo_shape[0], s_.lfo_rate[0] * v.lfo_rate[0], random_);
    src[src_lfo2] = v.lfo2.step(s_.lfo_shape[1], s_.lfo_rate[1] * v.lfo_rate[1], random_);
    src[src_velocity] = v.velocity;
    src[src_pressure] = v.pressure;
    src[src_key] = (float(v.note) - 60.0f) * (1.0f / 24.0f);
    src[src_random] = v.random;
    float d[destination_count] = {};
    for (int i = 0; i < s_.slot_count; i++) d[s_.slots[i].destination] += s_.slots[i].amount * src[s_.slots[i].source];
    v.lfo_rate[0] = exp2(clamp(d[dst_lfo1_rate], -1.0f, 1.0f) * 4.0f);   // +-4 octaves
    v.lfo_rate[1] = exp2(clamp(d[dst_lfo2_rate], -1.0f, 1.0f) * 4.0f);

    v.pitch += s_.glide * (v.target - v.pitch);
    float pitch = v.pitch + 12.0f * d[dst_pitch];
    float hz1 = 440.0f * semitones(pitch + s_.offset1 + 12.0f * d[dst_pitch1] - 69.0f);
    float hz2 = 440.0f * semitones(pitch + s_.offset2 + 12.0f * d[dst_pitch2] - 69.0f);
    float dt1 = clamp(hz1 * inv_rate_, 1e-5f, 0.45f), dt2 = clamp(hz2 * inv_rate_, 1e-5f, 0.45f);
    float inv1 = 1.0f / dt1, inv2 = 1.0f / dt2;

    float cutoff = s_.cutoff + s_.filter_env * src[src_env2] + s_.filter_key * (float(v.note) - 60.0f) +
                   s_.filter_velocity * (v.velocity - 1.0f) + 60.0f * d[dst_cutoff];
    float g = tan_pi(clamp(440.0f * semitones(cutoff - 69.0f) * inv_rate_, 0.0002f, 0.45f));
    float resonance = clamp(s_.resonance + d[dst_resonance], 0.0f, 1.0f);
    SvfCoefficients c2 = SvfCoefficients::from_g(g, 2.0f - 1.98f * resonance);

    float gain = (1.0f - s_.velocity_amp * (1.0f - v.velocity)) * clamp(1.0f + d[dst_amp], 0.0f, 2.0f);
    float pan = clamp(d[dst_pan], -1.0f, 1.0f);
    float target_l = 0.5f * gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
    float target_r = 0.5f * gain * (pan < 0.0f ? 1.0f + pan : 1.0f);

    // Oscillators and noise.
    float buffer[kControl] = {};
    float level1 = s_.level1 * clamp(1.0f + d[dst_osc1], 0.0f, 2.0f);
    float level2 = s_.level2 * clamp(1.0f + d[dst_osc2], 0.0f, 2.0f);
    if (level1 > 0.0f) v.osc1.render_add(buffer, kControl, s_.wave1, dt1, inv1, level1, s_.width1 + 0.45f * d[dst_width]);
    if (level2 > 0.0f) v.osc2.render_add(buffer, kControl, s_.wave2, dt2, inv2, level2, s_.width2 + 0.45f * d[dst_width]);
    if (s_.level_sub > 0.0f) v.sub.render_add(buffer, kControl, enaga::dsp::Wave::pulse, 0.5f * dt1, 2.0f * inv1, s_.level_sub);
    float noise = s_.level_noise * clamp(1.0f + d[dst_noise], 0.0f, 2.0f);
    if (noise > 0.0f)
        for (int i = 0; i < kControl; i++) buffer[i] += noise * random_.bipolar();

    // Drive and filter.
    const float drive = s_.drive;
    switch (s_.filter_type) {
    case filter_lp12:
        for (int i = 0; i < kControl; i++) buffer[i] = v.svf2.low(soft_clip(drive * buffer[i]), c2);
        break;
    case filter_lp24: {
        SvfCoefficients c1 = SvfCoefficients::from_g(g, 1.85f);
        for (int i = 0; i < kControl; i++) buffer[i] = v.svf2.low(v.svf1.low(soft_clip(drive * buffer[i]), c1), c2);
        break;
    }
    case filter_bp:
        for (int i = 0; i < kControl; i++) buffer[i] = v.svf2.step(soft_clip(drive * buffer[i]), c2).band;
        break;
    default:
        for (int i = 0; i < kControl; i++) buffer[i] = v.svf2.step(soft_clip(drive * buffer[i]), c2).high;
        break;
    }

    // Amplitude, per sample, with the control-rate gains ramped across the step.
    float gl = v.gain_l, gr = v.gain_r;
    const float step_l = (target_l - gl) * (1.0f / kControl), step_r = (target_r - gr) * (1.0f / kControl);
    for (int i = 0; i < kControl; i++) {
        float a = v.amp.step(s_.amp) * buffer[i];
        gl += step_l;
        gr += step_r;
        left[i] += a * gl;
        right[i] += a * gr;
    }
    v.gain_l = target_l;
    v.gain_r = target_r;
}

void Engine::render(float *out, uint32_t frames) {
    update_settings();
    while (queue_read_ != queue_write_) {
        handle(queue_[queue_read_ & 63]);
        queue_read_ = queue_read_ + 1;
    }

    float left[ENAGA_BLOCK_FRAMES], right[ENAGA_BLOCK_FRAMES];
    if (frames > ENAGA_BLOCK_FRAMES) frames = ENAGA_BLOCK_FRAMES;
    for (uint32_t i = 0; i < frames; i++) left[i] = right[i] = 0.0f;
    uint32_t active = 0;
    for (uint32_t at = 0; at < frames; at += kControl) {
        free_lfo_[0].step(s_.lfo_shape[0], s_.lfo_rate[0], random_);
        free_lfo_[1].step(s_.lfo_shape[1], s_.lfo_rate[1], random_);
        for (Voice &v : voices_)
            if (v.amp.active()) render_voice(v, left + at, right + at);
    }
    for (Voice &v : voices_) active += v.amp.active();
    active_voices = active;

    // Effects: the delay's echoes join the dry signal, and both feed the reverb.
    float wet_l[ENAGA_BLOCK_FRAMES] = {}, wet_r[ENAGA_BLOCK_FRAMES] = {};
    delay_.process(left, right, wet_l, wet_r, int(frames), delay_mix_);
    for (uint32_t i = 0; i < frames; i++) {
        left[i] += wet_l[i];
        right[i] += wet_r[i];
    }
    reverb_.process(left, right, left, right, int(frames), reverb_mix_);

    const float volume = s_.volume;
    uint32_t w = scope_write;
    for (uint32_t i = 0; i < frames; i++) {
        float l = soft_clip(volume * left[i]), r = soft_clip(volume * right[i]);
        out[2 * i] = l;
        out[2 * i + 1] = r;
        scope[w++ & (kScope - 1)] = l + r;
    }
    scope_write = w;
}

}  // namespace synth
