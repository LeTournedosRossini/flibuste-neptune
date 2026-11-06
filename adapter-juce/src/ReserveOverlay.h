#pragma once
// Fenêtre « AUTOMATION » : la liste des emplacements de la réserve d'automation d'un plugin (nom du réglage relié, verrou, retrait), par pages de 32.
// Recouvre l'éditeur ; se ferme par « Fermer » ou Échap. Pour les plugins dont l'éditeur n'a pas de page dédiée.
#include <array>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "NovaKit.h"
#include "ReserveMenu.h"

namespace nova
{
class ReserveOverlay : public juce::Component, private juce::Timer
{
public:
    static constexpr int kRows = 32;

    ReserveOverlay (const Skin& skin, ParamHost& host, std::function<void()> onClose) : skin_ (skin), host_ (host), onClose_ (std::move (onClose))
    {
        setWantsKeyboardFocus (true);
        close_ = std::make_unique<Pill> (skin_, Pill::Look::Small);
        close_->setText (reservemenu::U ("Fermer"));
        close_->onClick = [this] { if (onClose_) onClose_(); };
        addAndMakeVisible (*close_);
        const int pages = pageCount();
        for (int p = 0; p < pages; ++p)
        {
            auto pill = std::make_unique<Pill> (skin_, Pill::Look::Small);
            pill->setText (juce::String (p * kRows + 1) + "-" + juce::String (std::min ((p + 1) * kRows, host_.reserveCount())));
            pill->onClick = [this, p] { page_ = p; refreshRows(); };
            addAndMakeVisible (*pill);
            pagePills_.push_back (std::move (pill));
        }
        for (int r = 0; r < kRows; ++r)
        {
            Row row;
            row.lock = std::make_unique<Pill> (skin_, Pill::Look::Small);
            row.lock->onClick = [this, r] { const int k = slot (r); host_.uiReserveSetLocked (k, ! host_.reserveLocked (k)); refreshRows(); };
            row.clear = std::make_unique<Pill> (skin_, Pill::Look::Small);
            row.clear->setText (reservemenu::U ("Retirer"));
            row.clear->onClick = [this, r] { host_.uiReserveReleaseSlot (slot (r)); refreshRows(); };
            addChildComponent (*row.lock);
            addChildComponent (*row.clear);
            rows_[static_cast<std::size_t> (r)] = std::move (row);
        }
        refreshRows();
        startTimerHz (6);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        const auto panel = panelBounds().toFloat();
        const float rad = skin_.px (static_cast<float> (skin_.metrics().cornerRadius));
        g.setColour (skin_.colour (neptune::core::theme::Token::BgApp));   // fond opaque : le thème de la carte peut être translucide
        g.fillRoundedRectangle (panel, rad);
        skin_.fill (g, neptune::core::theme::Token::PanelBg, panel, rad);
        g.setColour (skin_.colour (neptune::core::theme::Token::Pb));
        g.drawRoundedRectangle (panel, rad, skin_.borderWidth());
        g.setColour (skin_.colour (neptune::core::theme::Token::Text));
        g.setFont (skin_.font (13.0f, true));
        g.drawText (reservemenu::U ("AUTOMATION : ") + juce::String (host_.reserveCount()) + reservemenu::U (" EMPLACEMENTS"),
                    juce::Rectangle<float> (panel.getX() + skin_.px (18.0f), panel.getY() + skin_.px (12.0f), skin_.px (420.0f), skin_.px (20.0f)), juce::Justification::centredLeft, true);
        g.setColour (skin_.colour (neptune::core::theme::Token::TextDim2));
        g.setFont (skin_.font (11.0f));
        g.drawText (reservemenu::U ("Clic droit sur un réglage : « Automatiser ce réglage ». L'emplacement porte son nom dans l'hôte."),
                    juce::Rectangle<float> (panel.getX() + skin_.px (18.0f), panel.getY() + skin_.px (36.0f), skin_.px (560.0f), skin_.px (18.0f)), juce::Justification::centredLeft, true);
        g.setFont (skin_.font (12.0f));
        for (int r = 0; r < kRows; ++r)
        {
            const int k = slot (r);
            if (k >= host_.reserveCount()) break;
            const bool used = ! host_.reserveSlotName (k).empty();
            g.setColour (skin_.colour (used ? neptune::core::theme::Token::Text : neptune::core::theme::Token::TextDim2));
            g.drawText (reservemenu::slotLabel (host_, k), textBounds (r).toFloat(), juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        const auto panel = panelBounds();
        close_->setBounds (juce::Rectangle<int> (panel.getRight() - skin_.ipx (98.0f), panel.getY() + skin_.ipx (10.0f), skin_.ipx (80.0f), skin_.ipx (28.0f)));
        for (std::size_t p = 0; p < pagePills_.size(); ++p)
            pagePills_[p]->setBounds (panel.getRight() - skin_.ipx (98.0f + 12.0f + (static_cast<float> (pagePills_.size() - p)) * 62.0f), panel.getY() + skin_.ipx (10.0f), skin_.ipx (56.0f), skin_.ipx (28.0f));
        for (int r = 0; r < kRows; ++r)
        {
            const auto t = textBounds (r);
            rows_[static_cast<std::size_t> (r)].lock->setBounds (t.getRight() + skin_.ipx (6.0f), t.getY(), skin_.ipx (76.0f), skin_.ipx (26.0f));
            rows_[static_cast<std::size_t> (r)].clear->setBounds (t.getRight() + skin_.ipx (88.0f), t.getY(), skin_.ipx (66.0f), skin_.ipx (26.0f));
        }
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { if (onClose_) onClose_(); return true; }
        return false;
    }
    void visibilityChanged() override { if (isVisible()) { refreshRows(); grabKeyboardFocus(); } }

private:
    struct Row { std::unique_ptr<Pill> lock, clear; };
    int pageCount() const { return (host_.reserveCount() + kRows - 1) / kRows; }
    int slot (int row) const { return page_ * kRows + row; }
    juce::Rectangle<int> panelBounds() const
    {
        const int w = skin_.ipx (940.0f), h = skin_.ipx (640.0f);
        return juce::Rectangle<int> (w, h).withCentre (getLocalBounds().getCentre()).constrainedWithin (getLocalBounds());
    }
    juce::Rectangle<int> textBounds (int r) const
    {
        const auto panel = panelBounds();
        const int col = r / 16, row = r % 16;
        return { panel.getX() + skin_.ipx (18.0f + static_cast<float> (col) * 462.0f), panel.getY() + skin_.ipx (70.0f + static_cast<float> (row) * 34.0f), skin_.ipx (270.0f), skin_.ipx (26.0f) };
    }
    void refreshRows()
    {
        for (int r = 0; r < kRows; ++r)
        {
            const int k = slot (r);
            auto& row = rows_[static_cast<std::size_t> (r)];
            const bool exists = k < host_.reserveCount();
            const bool used = exists && ! host_.reserveSlotName (k).empty();
            const bool locked = exists && host_.reserveLocked (k);
            row.lock->setVisible (used || locked);
            row.lock->setText (reservemenu::U (locked ? "Verrouillé" : "Verrouiller"));
            row.lock->setOn (locked);
            row.clear->setVisible (used);
            row.clear->setDim (locked);
        }
        for (std::size_t p = 0; p < pagePills_.size(); ++p) pagePills_[p]->setOn (static_cast<int> (p) == page_);
        repaint();
    }
    void timerCallback() override { if (isVisible()) refreshRows(); }

    const Skin& skin_;
    ParamHost& host_;
    std::function<void()> onClose_;
    std::unique_ptr<Pill> close_;
    std::vector<std::unique_ptr<Pill>> pagePills_;
    std::array<Row, kRows> rows_;
    int page_ = 0;
};
} // namespace nova
