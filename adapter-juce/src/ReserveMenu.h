#pragma once
// Menu clic droit de la réserve d'automation (« Automatiser ce réglage ») et pastille numérotée des réglages assignés, communs à tous les composants de réglage.
// Style nova : les menus prennent le LookAndFeel de leur composant. Les textes sont en français (la réserve n'est pas encore traduite).
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ParamHost.h"

namespace reservemenu
{
inline juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

inline bool available (const ParamHost* h, neptune::core::ParamId id) { return h != nullptr && h->reserveCount() > 0 && h->reserveCanAssign (id); }
inline int slotOf (const ParamHost* h, neptune::core::ParamId id) { return available (h, id) ? h->reserveSlotOf (id) : -1; }

inline juce::String slotLabel (const ParamHost& h, int k)
{
    const auto name = h.reserveSlotName (k);
    return juce::String (k + 1) + U (" \xc2\xb7 ") + (name.empty() ? U ("libre") : juce::String::fromUTF8 (name.c_str()));
}

// Petit disque numéroté dans le coin haut gauche de `area` : le réglage est automatisable par cet emplacement.
inline void drawBadge (juce::Graphics& g, juce::Rectangle<float> area, float px, juce::Colour fill, juce::Colour text, int slot, bool locked)
{
    const float h = 15.0f * px, w = std::max (h, (slot >= 99 ? 26.0f : 20.0f) * px);
    const auto r = juce::Rectangle<float> (area.getX() + 3.0f * px, area.getY() + 3.0f * px, w, h);
    g.setColour (fill);
    g.fillRoundedRectangle (r, h / 2.0f);
    g.setColour (text);
    g.setFont (juce::FontOptions (10.5f * px, juce::Font::bold));
    g.drawText (juce::String (slot + 1) + (locked ? "*" : ""), r, juce::Justification::centred, false);
}

// La pastille est peinte par le composant parent du potard : on repeint toute la fenêtre.
inline void repaintAll (juce::Component* c)
{
    if (c == nullptr) return;
    if (auto* top = c->getTopLevelComponent()) top->repaint();
    c->repaint();
}

// Affiche le menu. `changed` est appelé après un changement (pour repeindre).
inline void show (ParamHost& host, neptune::core::ParamId id, juce::Component& anchor, std::function<void()> changed = {})
{
    if (! available (&host, id)) return;
    const int cur = host.reserveSlotOf (id);
    const int total = host.reserveCount();
    juce::PopupMenu menu;
    if (cur >= 0)
    {
        menu.addItem (999, U ("Automatisé par l'emplacement ") + slotLabel (host, cur), false);
        menu.addSeparator();
        menu.addItem (2, host.reserveLocked (cur) ? U ("Retirer l'automatisation (emplacement verrouillé)") : U ("Retirer l'automatisation"), ! host.reserveLocked (cur));
        if (host.reserveSupportsLock()) menu.addItem (3, host.reserveLocked (cur) ? U ("Déverrouiller l'emplacement") : U ("Verrouiller l'emplacement"));
    }
    else
    {
        bool anyFree = false;
        for (int k = 0; k < total && ! anyFree; ++k) anyFree = host.reserveSlotName (k).empty();
        menu.addItem (1, anyFree ? U ("Automatiser ce réglage") : U ("Automatiser ce réglage (aucun emplacement libre)"), anyFree);
    }
    juce::PopupMenu choose;
    for (int g0 = 0; g0 < total; g0 += 16)
    {
        juce::PopupMenu sub;
        for (int k = g0; k < std::min (total, g0 + 16); ++k)
            sub.addItem (100 + k, slotLabel (host, k) + (host.reserveLocked (k) ? U (" (verrouillé)") : juce::String()), ! host.reserveLocked (k) && k != cur, k == cur);
        choose.addSubMenu (U ("Emplacements ") + juce::String (g0 + 1) + U (" à ") + juce::String (std::min (total, g0 + 16)), sub);
    }
    menu.addSubMenu (cur >= 0 ? U ("Changer d'emplacement") : U ("Choisir l'emplacement"), choose);

    juce::Component::SafePointer<juce::Component> safe (&anchor);
    menu.setLookAndFeel (&anchor.getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [&host, id, cur, safe, changed] (int r)
    {
        if (r == 0 || safe == nullptr) return;
        auto done = [safe, changed] { repaintAll (safe.getComponent()); if (changed) changed(); };
        if (r == 1) { host.uiReserveAssign (id, -1, false); done(); return; }
        if (r == 2) { host.uiReserveRelease (id); done(); return; }
        if (r == 3) { host.uiReserveSetLocked (cur, ! host.reserveLocked (cur)); done(); return; }
        if (r < 100) return;
        const int k = r - 100;
        if (host.reserveSlotName (k).empty()) { host.uiReserveAssign (id, k, false); done(); return; }
        // emplacement occupé : confirmation (menu au même style), l'ancien réglage perd son automation
        juce::PopupMenu ask;
        ask.addItem (999, U ("L'emplacement ") + slotLabel (host, k) + U (" est déjà utilisé."), false);
        ask.addItem (1, U ("Le reprendre pour ce réglage"));
        ask.addItem (2, U ("Annuler"));
        ask.setLookAndFeel (&safe->getLookAndFeel());
        ask.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (safe.getComponent()), [&host, id, k, safe, changed] (int a)
        {
            if (a != 1 || safe == nullptr) return;
            host.uiReserveAssign (id, k, true);
            repaintAll (safe.getComponent());
            if (changed) changed();
        });
    });
}
} // namespace reservemenu
