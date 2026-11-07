#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Bouton de la rangée du bas dont l'image est tracée en vectoriel (couleur du thème, grisée quand il est désactivé), avec un texte facultatif à droite de l'image.
class IconButton : public juce::TextButton
{
public:
    enum class Icon { Cycle, LockOpen, LockClosed, Left, Right, Cross, Undo, Redo };

    void setIcon (Icon i) { if (icon_ != i) { icon_ = i; repaint(); } }
    Icon icon() const noexcept { return icon_; }
    // Largeur qui convient : l'image seule, ou l'image plus le texte.
    int preferredWidth (int height, const juce::Font& font) const
    {
        const auto t = getButtonText();
        return t.isEmpty() ? height + 10 : height + juce::roundToInt (std::ceil (font.getStringWidthFloat (t))) + 18;
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = getLookAndFeel();
        lf.drawButtonBackground (g, *this, findColour (getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId), over, down);
        auto colour = findColour (juce::TextButton::textColourOffId);
        if (! isEnabled()) colour = colour.withMultipliedAlpha (0.4f);
        g.setColour (colour);
        auto area = getLocalBounds().toFloat();
        const auto text = getButtonText();
        auto iconArea = text.isEmpty() ? area : area.removeFromLeft (area.getHeight() + 4.0f);
        draw (g, iconArea.withSizeKeepingCentre (14.0f, 14.0f));
        if (text.isNotEmpty())
        {
            g.setFont (lf.getTextButtonFont (*this, getHeight()));
            g.drawText (text, area.withTrimmedRight (6.0f), juce::Justification::centredLeft, true);
        }
    }

private:
    void draw (juce::Graphics& g, juce::Rectangle<float> r) const
    {
        const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
        const juce::PathStrokeType stroke (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        constexpr float pi = juce::MathConstants<float>::pi;
        juce::Path p;
        switch (icon_)
        {
            case Icon::Left:
                p.startNewSubPath (x + w * 0.65f, y + h * 0.15f); p.lineTo (x + w * 0.3f, y + h * 0.5f); p.lineTo (x + w * 0.65f, y + h * 0.85f);
                g.strokePath (p, stroke);
                break;
            case Icon::Cross:
                p.startNewSubPath (x + w * 0.2f, y + h * 0.2f); p.lineTo (x + w * 0.8f, y + h * 0.8f);
                p.startNewSubPath (x + w * 0.8f, y + h * 0.2f); p.lineTo (x + w * 0.2f, y + h * 0.8f);
                g.strokePath (p, stroke);
                break;
            case Icon::Right:
                p.startNewSubPath (x + w * 0.35f, y + h * 0.15f); p.lineTo (x + w * 0.7f, y + h * 0.5f); p.lineTo (x + w * 0.35f, y + h * 0.85f);
                g.strokePath (p, stroke);
                break;
            case Icon::Undo:
            case Icon::Redo:
            {
                // arc de plus d'un demi-tour qui revient en arrière (annuler : vers la gauche, rétablir : vers la droite), terminé par une pointe de flèche
                const bool undo = icon_ == Icon::Undo;
                const auto c = r.getCentre().translated (0.0f, h * 0.06f);
                const float rad = w * 0.36f;
                const float a0 = undo ? 2.1f : -2.1f, a1 = undo ? -1.15f : 1.15f;
                juce::Path arc;
                arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, std::min (a0, a1), std::max (a0, a1), true);
                g.strokePath (arc, stroke);
                const juce::Point<float> end (c.x + rad * std::sin (a1), c.y - rad * std::cos (a1));
                const float sign = undo ? -1.0f : 1.0f;   // sens du déplacement le long de l'arc
                const juce::Point<float> back (-sign * std::cos (a1), -sign * std::sin (a1));
                juce::Path head;
                for (const float turn : { 0.75f, -0.75f })
                {
                    const juce::Point<float> d (back.x * std::cos (turn) - back.y * std::sin (turn), back.x * std::sin (turn) + back.y * std::cos (turn));
                    head.startNewSubPath (end);
                    head.lineTo (end + d * 4.6f);
                }
                g.strokePath (head, stroke);
                break;
            }
            case Icon::Cycle:
            {
                // deux arcs de 120 degrés dans le sens horaire, décalés d'un demi-tour, chacun terminé par une pointe de flèche
                const auto c = r.getCentre();
                const float rad = w * 0.42f;
                for (int i = 0; i < 2; ++i)
                {
                    const float a0 = pi * static_cast<float> (i) + 0.35f, a1 = a0 + 2.4f;
                    juce::Path arc;
                    arc.addCentredArc (c.x, c.y, rad, rad, 0.0f, a0, a1, true);
                    g.strokePath (arc, stroke);
                    const juce::Point<float> end (c.x + rad * std::sin (a1), c.y - rad * std::cos (a1));
                    const juce::Point<float> back (-std::cos (a1), -std::sin (a1));   // la tangente (sens horaire) est (cos, sin) ; on recule
                    juce::Path head;
                    for (const float turn : { 0.7f, -0.7f })
                    {
                        const juce::Point<float> d (back.x * std::cos (turn) - back.y * std::sin (turn), back.x * std::sin (turn) + back.y * std::cos (turn));
                        head.startNewSubPath (end);
                        head.lineTo (end + d * 4.2f);
                    }
                    g.strokePath (head, stroke);
                }
                break;
            }
            case Icon::LockClosed:
            case Icon::LockOpen:
            {
                const float bodyTop = y + h * 0.5f;
                g.fillRoundedRectangle (x + w * 0.15f, bodyTop, w * 0.7f, h * 0.45f, 2.0f);
                const float cx = x + w * 0.5f, rad = w * 0.22f, cy = y + h * 0.3f;
                juce::Path shackle;
                shackle.startNewSubPath (cx - rad, bodyTop);
                shackle.lineTo (cx - rad, cy);
                if (icon_ == Icon::LockClosed)
                {
                    shackle.addCentredArc (cx, cy, rad, rad, 0.0f, -pi * 0.5f, pi * 0.5f, false);
                    shackle.lineTo (cx + rad, bodyTop);
                }
                else
                {
                    shackle.addCentredArc (cx, cy, rad, rad, 0.0f, -pi * 0.5f, pi * 0.2f, false);   // l'anse est levée : elle ne rejoint pas le boîtier
                }
                g.strokePath (shackle, stroke);
                break;
            }
        }
    }

    Icon icon_ = Icon::Left;
};
