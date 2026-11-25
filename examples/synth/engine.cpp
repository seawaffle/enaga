#include "engine.hpp"

using namespace enaga::dsp;

namespace synth {
namespace {

float percent(int v) { return float(v) * 0.01f; }

}  // namespace

void Engine::init(float sample_rate) {
    rate_ = sample_rate;
    inv_rate_ = 1.0f / sample_rate;
    chorus_.init(sample_rate);
    delay_.init(1.6f, sample_rate);
    reverb_.init(sample_rate);
    for (int p = 0; p < param_count; p++) values[p] = info(p).init;
    update_settings();
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
        int fade = v[i ? lfo2_fade : lfo1_fade];
        s_.lfo_fade_step[i] = fade == 0 ? 1.0f : 1.0f / (seconds(fade) * control_rate);
    }
    s_.sync = v[osc2_sync] != 0;
    s_.ring = percent(v[mix_ring]);
    s_.unison = v[unison_voices];
    s_.detune = 0.01f * float(v[unison_detune]);
    s_.spread = percent(v[unison_spread]);
    static constexpr float kUnisonGain[] = {1.0f, 0.7071f, 0.5774f, 0.5f};   // 1 / sqrt(voices)
    s_.unison_gain = kUnisonGain[s_.unison - 1];
    s_.slot_count = 0;
    for (int i = 0; i < mod_slots; i++)
        if (v[mod_source(i)] != src_off && v[mod_amount(i)] != 0)
            s_.slots[s_.slot_count++] = {uint8_t(v[mod_source(i)]), uint8_t(v[mod_destination(i)]),
                                         percent(v[mod_amount(i)])};
    s_.mode = uint8_t(v[voice_mode]);
    s_.glide = v[voice_glide] == 0 ? 1.0f : 1.0f - exp(-EnvelopeShape::kSpan / (seconds(v[voice_glide]) * control_rate));
    s_.velocity_amp = percent(v[voice_velocity]);
    s_.volume = percent(v[voice_volume]) * percent(v[voice_volume]);

    chorus_.set(rate_hz(v[chorus_rate]), percent(v[chorus_depth]));
    chorus_mix_ = percent(v[chorus_mix]);
    delay_.set(delay_s(v[delay_time]), percent(v[delay_feedback]), percent(v[delay_tone]), v[delay_mode] != 0);
    delay_mix_ = percent(v[delay_mix]);
    delay_send_ = percent(v[delay_send]);
    reverb_.set(decay_s(v[reverb_decay]), percent(v[reverb_damping]));
    reverb_mix_ = 0.5f * percent(v[reverb_mix]);
}

int Engine::allocate() {                          // silent, else the oldest released, else the oldest
    return allocator_.allocate([this](int v) { return voices_[v].amp.active(); });
}

void Engine::start(int v, const NoteEvent &event, bool retrigger, float unison) {
    Voice &voice = voices_[v];
    allocator_.start(v, event.key, retrigger);
    voice.unison = unison;
    voice.note = event.note;
    voice.target = float(event.note);
    voice.velocity = float(event.value) * (1.0f / 127.0f);
    if (!retrigger) return;                       // LEGATO: the same voice slides on
    voice.pressure = 0.0f;
    voice.random = random_.bipolar();
    bool glide = s_.glide < 1.0f && (s_.mode != mode_legato);
    voice.pitch = glide ? last_pitch_ : voice.target;
    // A voice taken while still sounding keeps its oscillators' phases and its filters, and its envelope goes on
    // from its level: resetting them while it plays would put a step in the output, a click.
    if (!voice.amp.active()) {
        if (s_.unison > 1) {                      // unison: each voice from its own phase, or they start as one
            voice.osc1.reset(random_.unipolar());
            voice.osc2.reset(random_.unipolar());
            voice.sub.reset(random_.unipolar());
        } else {
            voice.osc1.reset();
            voice.osc2.reset();
            voice.sub.reset();
        }
        voice.sync_carry = 0.0f;
        voice.svf1.reset();
        voice.svf2.reset();
    }
    voice.lfo_fade[0] = s_.lfo_fade_step[0] >= 1.0f ? 1.0f : 0.0f;
    voice.lfo_fade[1] = s_.lfo_fade_step[1] >= 1.0f ? 1.0f : 0.0f;
    voice.amp.gate_on();
    voice.env2.gate_on();
    voice.env3.gate_on();
    voice.lfo1.reset(s_.lfo_key_sync[0] ? 0.0f : free_lfo_[0].phase());
    voice.lfo2.reset(s_.lfo_key_sync[1] ? 0.0f : free_lfo_[1].phase());
}

void Engine::release(int i) {
    Voice &v = voices_[i];
    allocator_.release_voice(i);
    v.amp.gate_off();
    v.env2.gate_off();
    v.env3.gate_off();
}

void Engine::handle(const NoteEvent &e) {
    switch (e.type) {
    case NoteEvent::on:
        if (s_.mode == mode_poly) {
            for (int k = 0; k < s_.unison; k++) start(allocate(), e, true, unison_place(k));
        } else {
            held_.push(e.key, e.note, e.value);
            bool legato = s_.mode == mode_legato && allocator_.held(0);
            for (int k = 0; k < s_.unison; k++) start(k, e, !legato, unison_place(k));
            for (int k = s_.unison; k < kVoices; k++)         // left over from more unison voices
                if (allocator_.held(k)) release(k);
        }
        last_pitch_ = float(e.note);
        break;
    case NoteEvent::off:
        if (s_.mode == mode_poly) {
            allocator_.release(e.key, [this](int v) { release(v); });
        } else {
            if (!held_.remove(e.key)) break;
            if (allocator_.key(0) != e.key || !allocator_.held(0)) break;
            if (!held_.empty()) {                 // back to the last key still held
                const auto &h = held_.top();
                const NoteEvent back = {NoteEvent::on, h.key, h.note, h.velocity};
                for (int k = 0; k < s_.unison; k++) start(k, back, s_.mode == mode_mono, unison_place(k));
            } else {
                for (int v = 0; v < kVoices; v++)
                    if (allocator_.held(v)) release(v);
            }
        }
        break;
    case NoteEvent::pressure:
        allocator_.each_held(e.key, [&](int v) { voices_[v].pressure = float(e.value) * (1.0f / 127.0f); });
        break;
    case NoteEvent::all_off:
        for (int i = 0; i < kVoices; i++) {
            Voice &v = voices_[i];
            allocator_.release_voice(i);
            v.amp.kill();
            v.env2.kill();
            v.env3.kill();
        }
        held_.clear();
        break;
    }
}

// Modulation for every sounding voice, once per control step. The scalar part (envelopes, LFOs, the matrix) runs
// voice by voice; the exponentials, tangents and reciprocals are gathered into arrays across the voices and
// converted by the library's block functions, which cl6x pipelines: one voice at a time they were a third of the
// synth's CPU.
void Engine::control_step() {
    constexpr int V = kVoices;
    float ex[5 * V] = {};                         // exponents: LFO 1 and 2 rates, oscillators 1 and 2, cutoff
    float g[2 * V] = {}, k[2 * V], inv[2 * V];
    for (int j = 0; j < 2 * V; j++) k[j] = inv[j] = 1.0f;
    for (int j = 0; j < V; j++) {
        Voice &v = voices_[j];
        if (!v.amp.active()) continue;
        Control &c = control_[j];
        float src[source_count];
        src[src_off] = 0.0f;
        src[src_env2] = v.env2.step(s_.env2);
        src[src_env3] = v.env3.step(s_.env3);
        src[src_lfo1] = v.lfo_fade[0] * v.lfo1.step(s_.lfo_shape[0], s_.lfo_rate[0] * v.lfo_rate[0], random_);
        src[src_lfo2] = v.lfo_fade[1] * v.lfo2.step(s_.lfo_shape[1], s_.lfo_rate[1] * v.lfo_rate[1], random_);
        v.lfo_fade[0] = clamp(v.lfo_fade[0] + s_.lfo_fade_step[0], 0.0f, 1.0f);
        v.lfo_fade[1] = clamp(v.lfo_fade[1] + s_.lfo_fade_step[1], 0.0f, 1.0f);
        src[src_velocity] = v.velocity;
        src[src_pressure] = v.pressure;
        src[src_key] = (float(v.note) - 60.0f) * (1.0f / 24.0f);
        src[src_random] = v.random;
        float d[destination_count] = {};          // a local: stores to it can't alias the settings
        for (int i = 0; i < s_.slot_count; i++) d[s_.slots[i].destination] += s_.slots[i].amount * src[s_.slots[i].source];
        ex[j] = clamp(d[dst_lfo1_rate], -1.0f, 1.0f) * 4.0f;                 // +-4 octaves
        ex[V + j] = clamp(d[dst_lfo2_rate], -1.0f, 1.0f) * 4.0f;

        v.pitch += s_.glide * (v.target - v.pitch);
        float pitch = v.pitch + 12.0f * d[dst_pitch] + s_.detune * v.unison;
        ex[2 * V + j] = (pitch + s_.offset1 + 12.0f * d[dst_pitch1] - 69.0f) * (1.0f / 12.0f);
        ex[3 * V + j] = (pitch + s_.offset2 + 12.0f * d[dst_pitch2] - 69.0f) * (1.0f / 12.0f);
        float cutoff = s_.cutoff + s_.filter_env * src[src_env2] + s_.filter_key * (float(v.note) - 60.0f) +
                       s_.filter_velocity * (v.velocity - 1.0f) + 60.0f * d[dst_cutoff];
        ex[4 * V + j] = (cutoff - 69.0f) * (1.0f / 12.0f);
        k[j] = 2.0f - 1.98f * clamp(s_.resonance + d[dst_resonance], 0.0f, 1.0f);
        k[V + j] = 1.85f;

        float gain = (1.0f - s_.velocity_amp * (1.0f - v.velocity)) * clamp(1.0f + d[dst_amp], 0.0f, 2.0f) *
                     s_.unison_gain;
        float pan = clamp(d[dst_pan] + s_.spread * v.unison, -1.0f, 1.0f);
        c.target_l = 0.5f * gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
        c.target_r = 0.5f * gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
        for (int i = 0; i < destination_count; i++) c.d[i] = d[i];
    }

    enaga_exp2_buffer(ex, 5 * V);
    for (int j = 0; j < V; j++) {
        Voice &v = voices_[j];
        if (!v.amp.active()) continue;
        Control &c = control_[j];
        v.lfo_rate[0] = ex[j];
        v.lfo_rate[1] = ex[V + j];
        inv[j] = c.dt1 = clamp(440.0f * ex[2 * V + j] * inv_rate_, 1e-5f, 0.45f);
        inv[V + j] = c.dt2 = clamp(440.0f * ex[3 * V + j] * inv_rate_, 1e-5f, 0.45f);
        g[j] = clamp(440.0f * ex[4 * V + j] * inv_rate_, 0.0002f, 0.45f);
    }
    for (int j = 0; j < V; j++) g[V + j] = g[j];
    enaga_tan_pi_buffer(g, 2 * V);                // 16 rather than 8: see dsp.h on short buffers
    enaga_reciprocal_buffer(inv, 2 * V);
    SvfCoefficients coefficients[2 * V];
    enaga_svf_coefficients_buffer(g, k, coefficients, s_.filter_type == filter_lp24 ? 2 * V : V);
    for (int j = 0; j < V; j++) {
        Control &c = control_[j];
        c.inv1 = inv[j];
        c.inv2 = inv[V + j];
        c.c2 = coefficients[j];
        c.c1 = coefficients[V + j];
    }
}

void Engine::render_voice(Voice &v, const Control &c, float *left, float *right) {
    const float *d = c.d;
    // Oscillators and noise. These, the filter and the amplitude are the library's block functions, built by cl6x
    // when it is installed.
    float buffer[kControl] = {};
    float level1 = s_.level1 * clamp(1.0f + d[dst_osc1], 0.0f, 2.0f);
    float level2 = s_.level2 * clamp(1.0f + d[dst_osc2], 0.0f, 2.0f);
    const float width = 0.45f * d[dst_width];
    if (s_.sync || s_.ring > 0.0f) {
        // Each oscillator on its own, for the ring modulator's product; OSC 2 synced to OSC 1 first, while OSC 1's
        // phase is still the block's start.
        float b1[kControl] = {}, b2[kControl] = {};
        if (s_.sync)
            v.osc2.sync_add(b2, kControl, v.osc1, c.dt1, c.inv1, v.sync_carry, s_.wave2, c.dt2, c.inv2, 1.0f,
                            s_.width2 + width);
        else
            v.osc2.render_add(b2, kControl, s_.wave2, c.dt2, c.inv2, 1.0f, s_.width2 + width);
        v.osc1.render_add(b1, kControl, s_.wave1, c.dt1, c.inv1, 1.0f, s_.width1 + width);
        enaga_mix(buffer, b1, kControl, level1);
        enaga_mix(buffer, b2, kControl, level2);
        if (s_.ring > 0.0f) enaga_mix_product(buffer, b1, b2, kControl, s_.ring);
    } else {
        if (level1 > 0.0f) v.osc1.render_add(buffer, kControl, s_.wave1, c.dt1, c.inv1, level1, s_.width1 + width);
        if (level2 > 0.0f) v.osc2.render_add(buffer, kControl, s_.wave2, c.dt2, c.inv2, level2, s_.width2 + width);
    }
    if (s_.level_sub > 0.0f)
        v.sub.render_add(buffer, kControl, Wave::pulse, 0.5f * c.dt1, 2.0f * c.inv1, s_.level_sub);
    float noise = s_.level_noise * clamp(1.0f + d[dst_noise], 0.0f, 2.0f);
    if (noise > 0.0f) random_.add_noise(buffer, kControl, noise);

    // Drive and filter.
    enaga_soft_clip_buffer(buffer, kControl, s_.drive);
    switch (s_.filter_type) {
    case filter_lp12: v.svf2.process(buffer, buffer, kControl, c.c2, Svf::low_pass); break;
    case filter_lp24: Svf::cascade(v.svf1, c.c1, v.svf2, c.c2, buffer, buffer, kControl); break;
    case filter_bp: v.svf2.process(buffer, buffer, kControl, c.c2, Svf::band_pass); break;
    default: v.svf2.process(buffer, buffer, kControl, c.c2, Svf::high_pass); break;
    }

    // Amplitude, per sample, with the control-rate gains ramped across the step.
    float amp[kControl];
    v.amp.render(amp, kControl, s_.amp);
    enaga_multiply(buffer, amp, kControl);
    float gl = v.gain_l, gr = v.gain_r;
    enaga_ramp_stereo_add(buffer, left, right, kControl, &gl, &gr, (c.target_l - gl) * (1.0f / kControl),
                          (c.target_r - gr) * (1.0f / kControl));
    v.gain_l = c.target_l;
    v.gain_r = c.target_r;
}

void Engine::render(float *out, uint32_t frames) {
    update_settings();
    NoteEvent event;
    while (events_.pop(event)) handle(event);

    float left[ENAGA_BLOCK_FRAMES], right[ENAGA_BLOCK_FRAMES];
    if (frames > ENAGA_BLOCK_FRAMES) frames = ENAGA_BLOCK_FRAMES;
    for (uint32_t i = 0; i < frames; i++) left[i] = right[i] = 0.0f;
    uint32_t active = 0;
    for (uint32_t at = 0; at < frames; at += kControl) {
        free_lfo_[0].step(s_.lfo_shape[0], s_.lfo_rate[0], random_);
        free_lfo_[1].step(s_.lfo_shape[1], s_.lfo_rate[1], random_);
        control_step();
        for (int j = 0; j < kVoices; j++)
            if (voices_[j].amp.active()) render_voice(voices_[j], control_[j], left + at, right + at);
    }
    for (Voice &v : voices_) active += v.amp.active();
    active_voices = active;

    // Effects. The chorus works in place; with its mix at 0 it only takes in the signal, so turning it up never
    // plays stale audio. Then the delay's echoes join the dry signal; the reverb gets the dry signal and
    // `delay_send_` of the echoes.
    float wet_l[ENAGA_BLOCK_FRAMES] = {}, wet_r[ENAGA_BLOCK_FRAMES] = {};
    float send_l[ENAGA_BLOCK_FRAMES] = {}, send_r[ENAGA_BLOCK_FRAMES] = {};
    if (chorus_mix_ > 0.0f) {
        chorus_.process(left, right, left, right, int(frames), chorus_mix_);
    } else {
        enaga_delay_write_block(&chorus_.state_.left, left, int(frames));
        enaga_delay_write_block(&chorus_.state_.right, right, int(frames));
    }
    delay_.process(left, right, wet_l, wet_r, int(frames), delay_mix_);
    for (uint32_t i = 0; i < frames; i++) {
        send_l[i] = left[i] + delay_send_ * wet_l[i];
        send_r[i] = right[i] + delay_send_ * wet_r[i];
        left[i] += wet_l[i];
        right[i] += wet_r[i];
    }
    reverb_.process(send_l, send_r, left, right, int(frames), reverb_mix_);

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
