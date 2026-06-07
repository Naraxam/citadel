#pragma once
#include <SFML/Graphics.hpp>
#include <string>

namespace ui {

class MainMenuScreen {
public:
    explicit MainMenuScreen(const sf::Font& font);

    enum class Action { None, DeckBuilder, PlayVsAI, DownloadArt, Settings, Quit,
                        RefreshCombos, DownloadSalt };
    void initVolume(float vol, bool muted) noexcept { m_volume = vol; m_muted = muted; }
    Action onEvent(const sf::Event& ev);
    void   draw(sf::RenderWindow& w) const;

private:
    const sf::Font& m_font;
    int m_hover = -1;

    static constexpr float WIN_W       = 1560.f;
    static constexpr float WIN_H       = 800.f;
    static constexpr float NAV_X       = 60.f;
    static constexpr float NAV_W       = 520.f;
    static constexpr float NAV_ITEM_H  = 62.f;     // shrunk to fit 6 items
    static constexpr float NAV_ITEM_G  = 8.f;
    static constexpr float NAV_ITEM_Y0 = 290.f;    // 6 items: 290..(290+6*70)=710

    bool m_showSettings = false;
    float m_volume      = 70.f;
    bool  m_muted       = false;

    // AI-data overlay state. Set by clicking the "AI Data" nav item; cleared
    // by clicking outside the panel or completing a sub-action.
    bool m_showAiData   = false;
    // Caller writes these so the overlay can show counts + last-action status.
public:
    void setAiDataCounts(int combos, int salt) { m_comboCount = combos; m_saltCount = salt; }
    void setAiDataStatus(std::string s) { m_aiDataStatus = std::move(s); }
private:
    int         m_comboCount   = -1;
    int         m_saltCount    = -1;
    std::string m_aiDataStatus;
    void  drawAiDataOverlay(sf::RenderTarget& t) const;

    bool  hitNavItem(float px, float py, int idx) const;
    float navItemY(int idx) const;
    void  drawSettingsOverlay(sf::RenderTarget& t) const;
    void  drawNavItem(sf::RenderTarget& t, int idx,
                      const char* label, const char* sub,
                      bool primary, bool hover) const;
    void  drawTxt(sf::RenderTarget& t, const std::string& s,
                  float x, float y, unsigned sz,
                  sf::Color col, bool bold = false) const;
};

} // namespace ui
