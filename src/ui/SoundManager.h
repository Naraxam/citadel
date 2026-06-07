#pragma once
#include <SFML/Audio.hpp>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <array>

namespace ui {

inline constexpr const char* SND_LAND_TAP      = "land_tap";
inline constexpr const char* SND_SPELL_CAST    = "spell_cast";
inline constexpr const char* SND_CREATURE_ETB  = "creature_etb";
inline constexpr const char* SND_COMBAT_HIT    = "combat_hit";
inline constexpr const char* SND_CREATURE_DIES = "creature_dies";
inline constexpr const char* SND_DRAW_CARD     = "draw_card";
inline constexpr const char* SND_WIN           = "win";
inline constexpr const char* SND_LOSE          = "lose";
inline constexpr const char* SND_PHASE_CHANGE  = "phase_change";
inline constexpr const char* SND_TOKEN_CREATE  = "token_create";
inline constexpr const char* SND_PRIORITY      = "priority";   // soft "you may act" chime

class SoundManager {
public:
    SoundManager()  = default;
    ~SoundManager() = default;

    // Generate all sound buffers.  Call once after the SFML audio device is
    // available (i.e. after the main window is created).
    void init();

    // Play a named sound.  If the sound is already playing in a slot, a second
    // concurrent instance starts in another slot (up to kSlots concurrent sounds).
    // Silent no-op when muted or if the name is not found.
    void play(const char* name);

    // Master mute toggle — persists via settings.
    bool muted() const noexcept { return m_muted; }
    void setMuted(bool m) noexcept { m_muted = m; }

    // Master volume [0, 100].
    float volume() const noexcept { return m_volume; }
    void  setVolume(float v) noexcept;

    // Per-category volumes [0, 100].
    float sfxVolume() const noexcept { return m_sfxVolume; }
    float uiVolume()  const noexcept { return m_uiVolume; }
    void  setSfxVolume(float v) noexcept { m_sfxVolume = std::clamp(v, 0.f, 100.f); }
    void  setUiVolume (float v) noexcept { m_uiVolume  = std::clamp(v, 0.f, 100.f); }

    // Singleton accessor — stored as a member of GameWindow, passed by pointer
    // to subsystems that need it.
    static SoundManager& instance();

private:
    static constexpr int kSlots      = 8;   // concurrent sound channels
    static constexpr int kSampleRate = 44100;

    std::unordered_map<std::string, sf::SoundBuffer> m_buffers;
    std::array<sf::Sound, kSlots>                     m_slots;
    int   m_nextSlot = 0;
    bool  m_muted      = false;
    float m_volume     = 70.f;   // master volume [0-100]
    float m_sfxVolume  = 100.f;  // SFX category scale [0-100]
    float m_uiVolume   = 100.f;  // UI sounds category scale [0-100]

    // ── PCM synthesis helpers ─────────────────────────────────────────────────
    static std::vector<sf::Int16> makeSine(float freqHz, float durationSec,
                                            float decayPerSec  = 8.f,
                                            float attackSec    = 0.005f);

    static std::vector<sf::Int16> makeSweep(float f1, float f2, float durationSec,
                                             float decayPerSec = 6.f);

    static std::vector<sf::Int16> makeNoise(float durationSec,
                                             float decayPerSec = 12.f,
                                             float cutoffHz    = 400.f);

    static std::vector<sf::Int16> makeChord(const std::initializer_list<float>& freqs,
                                             float durationSec, float decayPerSec = 5.f);

    void addBuffer(const char* name, std::vector<sf::Int16> samples, int channels = 1);
};

} // namespace ui
