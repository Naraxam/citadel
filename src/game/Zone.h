#pragma once
#include "Card.h"
#include "ZoneType.h"
#include <functional>
#include <vector>
#include <algorithm>
#include <random>

namespace mtg {

// An ordered collection of Cards. Zones don't own their cards —
// GameState owns all Card objects; Zone holds non-owning raw pointers.
//
// Ordering matters differently per zone:
//   Library   — ordered (top = index 0, bottom = last index)
//   Hand      — unordered in rules; stored in insertion order for display
//   Graveyard — ordered (most recent on top, index 0)
//   Stack     — LIFO (top of stack = index 0)
//   Battlefield, Exile, Command — unordered
class Zone {
public:
    explicit Zone(ZoneType type) : m_type(type) {}

    ZoneType type() const noexcept { return m_type; }

    // ── Mutation ──────────────────────────────────────────────────────────

    // Add to the "top" (index 0) — used for library draw, stack push, graveyard.
    void addToFront(Card* card) {
        m_cards.insert(m_cards.begin(), card);
    }

    // Add to the "bottom" (end) — normal insertion for hand, battlefield, exile.
    void addToBack(Card* card) {
        m_cards.push_back(card);
    }

    // Default add: graveyard and stack go to front; everything else to back.
    void add(Card* card) {
        if (m_type == ZoneType::Graveyard || m_type == ZoneType::Stack)
            addToFront(card);
        else
            addToBack(card);
    }

    bool remove(ObjectId id) {
        auto it = std::find_if(m_cards.begin(), m_cards.end(),
                               [id](const Card* c) { return c->id == id; });
        if (it == m_cards.end()) return false;
        m_cards.erase(it);
        return true;
    }

    // Shuffle the zone in place (library use).
    void shuffle(std::mt19937& rng) {
        std::shuffle(m_cards.begin(), m_cards.end(), rng);
        // Shuffling hides the order again: any cards that were "known on top"
        // (from a scry/peek) are no longer known to their owner.
        for (Card* c : m_cards) if (c) c->revealedToOwner = false;
    }

    // ── Query ─────────────────────────────────────────────────────────────

    Card* find(ObjectId id) const noexcept {
        auto it = std::find_if(m_cards.begin(), m_cards.end(),
                               [id](const Card* c) { return c->id == id; });
        return it != m_cards.end() ? *it : nullptr;
    }

    // Top of library / top of stack (index 0).
    Card* front() const noexcept { return m_cards.empty() ? nullptr : m_cards.front(); }

    // Bottom of library (last index).
    Card* back()  const noexcept { return m_cards.empty() ? nullptr : m_cards.back(); }

    // Returns a snapshot copy of the card list. Returning by value (not by const
    // reference) makes range-for iterations immune to mid-loop mutations of m_cards
    // — effects that move/destroy cards, create tokens, etc. would otherwise
    // invalidate the iterators of any outer for-loop iterating this zone and cause
    // use-after-free crashes. The cost is one vector copy per call.
    std::vector<Card*> cards() const { return m_cards; }

    // Raw view (no copy). Only use this when you are certain nothing inside the
    // loop body can add to or remove from this zone.
    const std::vector<Card*>& cardsRaw() const noexcept { return m_cards; }

    size_t size()  const noexcept { return m_cards.size(); }
    bool   empty() const noexcept { return m_cards.empty(); }

    // Deep-copy helpers for GameState::clone() --------------------------------

    // Copy the card pointer list from src (pointers still point into src's objects).
    // Call rebuildPointers() afterwards to fix them up.
    void copyCardsFrom(const Zone& src) { m_cards = src.m_cards; }

    // Replace every Card* in this zone with the result of lookup(card->id).
    // Used after cloning m_objects to fix up stale pointers.
    void rebuildPointers(const std::function<Card*(ObjectId)>& lookup) {
        for (Card*& ptr : m_cards) {
            if (Card* nc = lookup(ptr->id)) ptr = nc;
        }
    }

private:
    ZoneType          m_type;
    std::vector<Card*> m_cards;
};

} // namespace mtg
