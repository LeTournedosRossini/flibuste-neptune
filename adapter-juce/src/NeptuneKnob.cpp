#include <algorithm>
#include "NeptuneKnob.h"
#include "ReserveMenu.h"
#include "neptune/core/Control.h"
#include <cmath>

using neptune::core::theme::Token;
namespace ui = neptune::core::ui;

namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;
// La maquette (canvas) mesure les angles depuis l'est, dans le sens horaire ; JUCE depuis le nord.
constexpr float kA0 = 135.0f * kPi / 180.0f;
constexpr float kA1 = 405.0f * kPi / 180.0f;
float toJuce (float canvasAngle) { return canvasAngle + juce::MathConstants<float>::halfPi; }

void strokeArc (juce::Graphics& g, float cx, float cy, float radius, float from, float to, float width, bool rounded)
{
    if (std::abs (to - from) < 0.001f) return;
    juce::Path p;
    p.addCentredArc (cx, cy, radius, radius, 0.0f, toJuce (std::min (from, to)), toJuce (std::max (from, to)), true);
    g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved,
                                           rounded ? juce::PathStrokeType::rounded : juce::PathStrokeType::butt));
}
} // namespace

NeptuneKnob::NeptuneKnob (ParamHost& p, neptune::core::ParamId id, const neptune::core::theme::Palette& palette,
                          neptune::core::theme::KnobStyle style, OpenModal openModal)
    : processor_ (p), id_ (id), index_ (p.parameters().indexOf (id)), palette_ (palette), style_ (style), openModal_ (std::move (openModal))
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);

    const auto& set = processor_.parameters();
    jassert (index_ != neptune::core::ParameterSet::npos);
    if (index_ != neptune::core::ParameterSet::npos)
    {
        using Kind = neptune::core::Kind;
        const auto& spec = set.spec (index_);
        if (spec.kind != Kind::Continuous) interaction_.setSteps (static_cast<int> (spec.max - spec.min));
        interaction_.setValue (shownNormalised());
        lastVersion_ = set.versionAt (index_);
    }

    ui::KnobCallbacks cb;
    cb.beginGesture = [this] { processor_.uiBeginEdit (id_); };
    cb.setNormalised = [this] (double n) { processor_.uiSetNormalised (id_, paramNormalised (n)); repaint(); };
    cb.endGesture = [this] { processor_.uiEndEdit (id_); };
    cb.resetToDefault = [this]
    {
        processor_.uiResetToDefault (id_);
        interaction_.setValue (shownNormalised());
        repaint();
    };
    cb.openModal = [this] { if (openModal_) openModal_ (id_, terminal()); };
    interaction_.setCallbacks (std::move (cb));

    startTimerHz (30);
}

juce::Colour NeptuneKnob::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

// Plage d'affichage du potard : la course du potard ne couvre qu'une partie [lo, hi] de la plage du paramètre (au-delà, il reste en butée) ;
// la modale (appui long) et l'hôte gardent toute la plage. `exponent` > 1 resserre les petites valeurs (course = ((v - lo) / (hi - lo))^(1/exponent)).
void NeptuneKnob::setDisplayRange (double lo, double hi, double exponent)
{
    hasRange_ = hi > lo;
    rangeLo_ = lo; rangeHi_ = hi; rangeExp_ = exponent > 0.0 ? exponent : 1.0;
    interaction_.setValue (shownNormalised());
    repaint();
}

double NeptuneKnob::shownNormalised() const
{
    if (index_ == neptune::core::ParameterSet::npos) return 0.0;
    const auto& set = processor_.parameters();
    if (! hasRange_) return set.getNormalizedAt (index_);
    const double plain = set.spec (index_).fromNormalized (set.getNormalizedAt (index_));
    const double t = std::clamp ((plain - rangeLo_) / (rangeHi_ - rangeLo_), 0.0, 1.0);
    return rangeExp_ == 1.0 ? t : std::pow (t, 1.0 / rangeExp_);
}

double NeptuneKnob::paramNormalised (double shown) const
{
    if (! hasRange_ || index_ == neptune::core::ParameterSet::npos) return shown;
    const double t = std::clamp (shown, 0.0, 1.0);
    const double plain = rangeLo_ + (rangeHi_ - rangeLo_) * (rangeExp_ == 1.0 ? t : std::pow (t, rangeExp_));
    return processor_.parameters().spec (index_).toNormalized (plain);
}

void NeptuneKnob::timerCallback()
{
    interaction_.tick (nowMs());
    if (index_ == neptune::core::ParameterSet::npos) return;
    if (const float off = processor_.parameters().modOffsetAt (index_); std::abs (off - lastMod_) > 0.002f) { lastMod_ = off; repaint(); }   // la modulation bouge sans changer la valeur
    // Valeur changée ailleurs (hôte, annulation, modale) : on suit, sauf pendant notre propre geste.
    const auto& set = processor_.parameters();
    const auto v = set.versionAt (index_);
    if (v != lastVersion_)
    {
        lastVersion_ = v;
        if (! interaction_.gestureOpen()) interaction_.setValue (shownNormalised());
        repaint();
    }
}

void NeptuneKnob::mouseDown (const juce::MouseEvent& e)
{
    lastWasTouch_ = e.source.isTouch();
    const auto button = e.mods.isPopupMenu() ? ui::MouseButton::Right : ui::MouseButton::Left;
    interaction_.pointerDown (button, lastWasTouch_ ? ui::PointerKind::Touch : ui::PointerKind::Mouse, e.mods.isShiftDown(),
                              e.position.x, e.position.y, nowMs());
}

void NeptuneKnob::mouseDrag (const juce::MouseEvent& e)
{
    interaction_.pointerMove (e.mods.isShiftDown(), e.position.x, e.position.y, nowMs());
}

void NeptuneKnob::mouseUp (const juce::MouseEvent& e)
{
    // Clic droit sans déplacement sur un réglage de la réserve d'automation : le menu remplace la modale que ce geste ouvrirait.
    const bool reserveClick = e.mods.isPopupMenu() && e.getDistanceFromDragStart() < 4 && reservemenu::available (&processor_, id_);
    if (reserveClick) interaction_.pointerCancel();
    else interaction_.pointerUp (nowMs());
    if (index_ != neptune::core::ParameterSet::npos) interaction_.setValue (shownNormalised());
    repaint();
    // Clic droit sans déplacement : menu des macros (le glissement avec le bouton droit reste le réglage fin).
    if (e.mods.isPopupMenu() && e.getDistanceFromDragStart() < 4)
    {
        if (reservemenu::available (&processor_, id_)) reservemenu::show (processor_, id_, *this);   // la réserve d'automation (Trident)
        else if (processor_.canAssignMacro (id_)) showMacroMenu();                                       // les macros de Neptune
    }
}

void NeptuneKnob::showMacroMenu()
{
    const auto assigned = processor_.macrosOf (id_);
    juce::PopupMenu sub;
    for (int g0 = 0; g0 < neptune::core::control::kMacros; g0 += 16)
    {
        juce::PopupMenu group;
        for (int m = g0; m < g0 + 16; ++m)
            group.addItem (100 + m, "Macro " + juce::String (m + 1), true, std::find (assigned.begin(), assigned.end(), m) != assigned.end());
        sub.addSubMenu ("Macros " + juce::String (g0 + 1) + " - " + juce::String (g0 + 16), group);
    }
    juce::PopupMenu menu;
    menu.addSubMenu (juce::String::fromUTF8 (processor_.uiText ("macro.assign").c_str()), sub);
    menu.addItem (1, juce::String::fromUTF8 (processor_.uiText ("macro.clear").c_str()), ! assigned.empty());
    juce::Component::SafePointer<NeptuneKnob> safe (this);
    menu.setLookAndFeel (&getLookAndFeel());   // sinon le menu prend le style JUCE d'origine
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int choice)
    {
        if (safe == nullptr || choice == 0) return;
        if (choice == 1) safe->processor_.uiClearMacros (safe->id_);
        else safe->processor_.uiToggleMacro (safe->id_, choice - 100);
    });
}

void NeptuneKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? -w.deltaX : w.deltaY;
    if (w.isReversed) d = -d;
    if (d == 0.0f) return;
    interaction_.wheel (d > 0.0f ? 1.0 : -1.0, e.mods.isShiftDown(), nowMs());
}

bool NeptuneKnob::keyPressed (const juce::KeyPress& k)
{
    // Ctrl/Cmd + lettre : raccourcis de l'éditeur (annuler/rétablir), pas pour le potard.
    if (k.getModifiers().isCommandDown() || k.getModifiers().isAltDown()) return false;

    const bool shift = k.getModifiers().isShiftDown();
    ui::Key key;
    if (k.isKeyCode (juce::KeyPress::upKey)) key = ui::Key::Up;
    else if (k.isKeyCode (juce::KeyPress::downKey)) key = ui::Key::Down;
    else if (k.isKeyCode (juce::KeyPress::leftKey)) key = ui::Key::Left;
    else if (k.isKeyCode (juce::KeyPress::rightKey)) key = ui::Key::Right;
    else if (k.isKeyCode (juce::KeyPress::pageUpKey)) key = ui::Key::PageUp;
    else if (k.isKeyCode (juce::KeyPress::pageDownKey)) key = ui::Key::PageDown;
    else if (k.isKeyCode (juce::KeyPress::homeKey)) key = ui::Key::Home;
    else if (k.isKeyCode (juce::KeyPress::endKey)) key = ui::Key::End;
    else if (k.isKeyCode (juce::KeyPress::returnKey)) key = ui::Key::Enter;
    else if (k.isKeyCode (juce::KeyPress::deleteKey) || k.isKeyCode (juce::KeyPress::backspaceKey)) key = ui::Key::Delete;
    else return false;

    lastWasTouch_ = false;
    return interaction_.key (key, shift);
}

void NeptuneKnob::setParamId (neptune::core::ParamId id)
{
    if (id == id_) return;
    id_ = id;
    index_ = processor_.parameters().indexOf (id);
    hasRange_ = false;
    if (index_ != neptune::core::ParameterSet::npos)
    {
        const auto& spec = processor_.parameters().spec (index_);
        interaction_.setSteps (spec.kind != neptune::core::Kind::Continuous ? static_cast<int> (spec.max - spec.min) : 0);
    }
    interaction_.setValue (shownNormalised());
    lastVersion_ = 0;
    repaint();
}

void paintNeptuneKnob (juce::Graphics& g, juce::Rectangle<float> b, float n, const neptune::core::theme::Palette& palette,
                       neptune::core::theme::KnobStyle style, float k)
{
    auto colour = [&palette] (Token t)
    {
        const auto& c = palette.color (t);
        return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
    };
    const float S = std::min (b.getWidth(), b.getHeight());
    const float cx = b.getCentreX(), cy = b.getCentreY();
    const float radius = S * 0.4f;
    n = juce::jlimit (0.0f, 1.0f, n);
    const float angle = kA0 + n * (kA1 - kA0);

    if (style == neptune::core::theme::KnobStyle::Pot)
    {
        // Potentiomètre physique avec pointeur (nova-knob-styles.js : drawPot)
        juce::ColourGradient grad (colour (Token::KnobPotFace1), cx - 3.0f, cy - 4.0f, colour (Token::KnobPotFace3),
                                   cx - 3.0f + radius + 5.0f, cy - 4.0f, true);
        grad.addColour (0.55, colour (Token::KnobPotFace2));
        g.setGradientFill (grad);
        g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        g.setColour (colour (Token::KnobPotRing));
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.5f * k);

        g.setColour (colour (Token::Accent));
        strokeArc (g, cx, cy, radius - 5.0f * k, kA0, angle, 2.4f * k, true);

        juce::Path tri;
        tri.addTriangle (radius - 7.0f * k, 0.0f, radius - 14.0f * k, -2.2f * k, radius - 14.0f * k, 2.2f * k);
        g.setColour (colour (Token::KnobPotPointer));
        g.fillPath (tri, juce::AffineTransform::rotation (angle).translated (cx, cy));

        g.setColour (colour (Token::KnobPotCenter));
        g.fillEllipse (cx - 4.5f * k, cy - 4.5f * k, 9.0f * k, 9.0f * k);
    }
    else
    {
        // Anneau LED avec dégradé d'intensité (nova-knob-styles.js : drawLED). Le halo (jusqu'à 12,5 px de large) doit tenir dans
        // le composant : sinon il est coupé net et dessine un carré aux angles tronqués (visible depuis le potard réduit à 48 px).
        const float r = S * 0.36f;
        g.setColour (colour (Token::KnobFace));
        g.fillEllipse (cx - r * 0.72f, cy - r * 0.72f, r * 1.44f, r * 1.44f);

        g.setColour (colour (Token::KnobTrack).withAlpha (0.8f));
        strokeArc (g, cx, cy, r, kA0, kA1, 5.0f * k, false);

        const auto c1 = colour (Token::Accent).interpolatedWith (colour (Token::KnobHot), n);
        const auto c2 = colour (Token::Accent2).interpolatedWith (colour (Token::KnobHot2), n);
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (c1.withAlpha (0.07f * static_cast<float> (i)));
            strokeArc (g, cx, cy, r, kA0, angle, (5.0f + static_cast<float> (i) * 2.5f) * k, true);
        }
        g.setGradientFill (juce::ColourGradient (c1, cx - r, cy, c2, cx + r, cy, false));
        strokeArc (g, cx, cy, r, kA0, angle, 5.0f * k, true);

        const float ix = cx + std::cos (angle) * (r - 1.0f), iy = cy + std::sin (angle) * (r - 1.0f);
        g.setColour (colour (Token::KnobDot));
        g.fillEllipse (ix - 2.6f * k, iy - 2.6f * k, 5.2f * k, 5.2f * k);
    }
}

void paintModulationRing (juce::Graphics& g, juce::Rectangle<float> b, float base, float modulated, const neptune::core::theme::Palette& palette, float k)
{
    const auto& c = palette.color (Token::Accent2);
    const float S = std::min (b.getWidth(), b.getHeight());
    const float cx = b.getCentreX(), cy = b.getCentreY(), r = S * 0.46f;
    const float a0 = kA0 + juce::jlimit (0.0f, 1.0f, base) * (kA1 - kA0), a1 = kA0 + juce::jlimit (0.0f, 1.0f, modulated) * (kA1 - kA0);
    g.setColour (juce::Colour (c.r, c.g, c.b, 1.0f));
    strokeArc (g, cx, cy, r, std::min (a0, a1), std::max (a0, a1), 3.0f * k, true);
    g.fillEllipse (cx + std::cos (a1) * r - 3.0f * k, cy + std::sin (a1) * r - 3.0f * k, 6.0f * k, 6.0f * k);
}

void NeptuneSliderLook::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos, float, float, juce::Slider& s)
{
    const auto area = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y), static_cast<float> (width), static_cast<float> (height));
    paintNeptuneKnob (g, area, pos, palette_, style_);
    if (s.hasKeyboardFocus (false))
    {
        g.setColour (juce::Colour (palette_.color (Token::Accent).r, palette_.color (Token::Accent).g, palette_.color (Token::Accent).b));
        g.drawEllipse (area.reduced (1.0f), 1.5f);
    }
}

void NeptuneKnob::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const float n = index_ == neptune::core::ParameterSet::npos ? 0.0f : static_cast<float> (shownNormalised());
    paintNeptuneKnob (g, b, n, palette_, style_, scale_);
    if (index_ != neptune::core::ParameterSet::npos)
        if (const float off = processor_.parameters().modOffsetAt (index_); off != 0.0f)
            paintModulationRing (g, b, n, n + off, palette_, scale_);

    if (hasKeyboardFocus (false))
    {
        g.setColour (colour (Token::Accent));
        g.drawEllipse (b.reduced (1.0f), 1.5f);
    }
}
