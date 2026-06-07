#include "SoundManager.h"
#include <cmath>
#include <random>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ui {

// ── PCM helpers 

std::vector<sf::Int16> SoundManager::makeSine(float freqHz, float durationSec,
                                               float decayPerSec, float attackSec) {
    int n = static_cast<int>(durationSec * kSampleRate);
    std::vector<sf::Int16> out(n);
    for (int i = 0; i < n; ++i) {
        float t   = static_cast<float>(i) / kSampleRate;
        float env = (t < attackSec)
                  ? (t / attackSec)
                  : std::exp(-(t - attackSec) * decayPerSec);
        float val = std::sin(2.f * static_cast<float>(M_PI) * freqHz * t) * env;
        out[i]    = static_cast<sf::Int16>(val * 28000.f);
    }
    return out;
}

std::vector<sf::Int16> SoundManager::makeSweep(float f1, float f2, float durationSec,
                                                float decayPerSec) {
    int n = static_cast<int>(durationSec * kSampleRate);
    std::vector<sf::Int16> out(n);
    float phase = 0.f;
    for (int i = 0; i < n; ++i) {
        float t    = static_cast<float>(i) / kSampleRate;
        float frac = t / durationSec;
        float freq = f1 + (f2 - f1) * frac;
        float env  = std::exp(-t * decayPerSec);
        phase += 2.f * static_cast<float>(M_PI) * freq / kSampleRate;
        out[i] = static_cast<sf::Int16>(std::sin(phase) * env * 28000.f);
    }
    return out;
}

std::vector<sf::Int16> SoundManager::makeNoise(float durationSec, float decayPerSec,
                                                float cutoffHz) {
    int n = static_cast<int>(durationSec * kSampleRate);
    std::vector<sf::Int16> out(n);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    // Simple 1-pole low-pass filter to shape the noise colour
    float rc  = 1.f / (2.f * static_cast<float>(M_PI) * cutoffHz);
    float dt  = 1.f / kSampleRate;
    float alpha = dt / (rc + dt);
    float prev = 0.f;
    for (int i = 0; i < n; ++i) {
        float t   = static_cast<float>(i) / kSampleRate;
        float env = std::exp(-t * decayPerSec);
        prev = prev + alpha * (dist(rng) - prev);  // low-pass
        out[i] = static_cast<sf::Int16>(prev * env * 28000.f);
    }
    return out;
}

std::vector<sf::Int16> SoundManager::makeChord(
    const std::initializer_list<float>& freqs,
    float durationSec, float decayPerSec) {
    int n = static_cast<int>(durationSec * kSampleRate);
    std::vector<sf::Int16> out(n, 0);
    float amp = 28000.f / static_cast<float>(std::max(1, (int)freqs.size()));
    for (float freq : freqs) {
        for (int i = 0; i < n; ++i) {
            float t   = static_cast<float>(i) / kSampleRate;
            float env = std::exp(-t * decayPerSec);
            out[i]   += static_cast<sf::Int16>(
                std::sin(2.f * static_cast<float>(M_PI) * freq * t) * env * amp);
        }
    }
    return out;
}

void SoundManager::addBuffer(const char* name, std::vector<sf::Int16> samples, int channels) {
    sf::SoundBuffer buf;
    buf.loadFromSamples(samples.data(), samples.size(),
                        static_cast<unsigned>(channels),
                        static_cast<unsigned>(kSampleRate));
    m_buffers.emplace(name, std::move(buf));
}

// ── init ──────────────────────────────────────────────────────────────────────

void SoundManager::init() {
    // Land tap: short high click
    addBuffer(SND_LAND_TAP,     makeSine(1800.f, 0.08f, 30.f));

    // Spell cast: sweeping mid-tone
    addBuffer(SND_SPELL_CAST,   makeSweep(500.f, 220.f, 0.30f, 5.f));

    // Creature ETB: bright ascending blip
    addBuffer(SND_CREATURE_ETB, makeSweep(440.f, 880.f, 0.15f, 8.f));

    // Combat hit: thuddy noise burst
    addBuffer(SND_COMBAT_HIT,   makeNoise(0.15f, 18.f, 300.f));

    // Creature death: descending moan
    addBuffer(SND_CREATURE_DIES, makeSweep(300.f, 80.f, 0.25f, 6.f));

    // Draw card: soft high tick
    addBuffer(SND_DRAW_CARD,    makeSine(1200.f, 0.06f, 25.f));

    // Win fanfare: ascending triad (C5, E5, G5)
    addBuffer(SND_WIN,          makeChord({523.25f, 659.25f, 783.99f}, 0.8f, 2.5f));

    // Lose: descending minor chord
    addBuffer(SND_LOSE,         makeChord({523.25f, 466.16f, 392.f}, 0.6f, 4.f));

    // Phase change: subtle pop
    addBuffer(SND_PHASE_CHANGE, makeSine(600.f, 0.05f, 40.f));

    // Token create: similar to creature ETB but softer
    addBuffer(SND_TOKEN_CREATE, makeSweep(350.f, 600.f, 0.12f, 10.f));

    // Priority: soft two-note bell
    // nudges the player they may act
    addBuffer(SND_PRIORITY,     makeChord({880.f, 1318.51f}, 0.45f, 6.f));
}

// ── play 

void SoundManager::play(const char* name) {
    if (m_muted) return;
    auto it = m_buffers.find(name);
    if (it == m_buffers.end()) return;

    // Find a free or oldest slot
    sf::Sound& slot = m_slots[m_nextSlot % kSlots];
    m_nextSlot = (m_nextSlot + 1) % kSlots;

    slot.setBuffer(it->second);
    // Apply category volume: UI sounds (draw, phase) use uiVolume; others use sfxVolume
    bool isUiSound = (std::string_view(name).find("draw") != std::string_view::npos ||
                      std::string_view(name).find("phase") != std::string_view::npos);
    float catScale = (isUiSound ? m_uiVolume : m_sfxVolume) / 100.f;
    slot.setVolume(m_volume * catScale);
    slot.play();
}

void SoundManager::setVolume(float v) noexcept {
    m_volume = std::clamp(v, 0.f, 100.f);
    for (auto& s : m_slots)
        s.setVolume(m_volume);
}

} // namespace ui
