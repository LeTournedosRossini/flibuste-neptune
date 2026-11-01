#include "NovaKit.h"
#include "neptune/core/Version.h"

#include <algorithm>
#include <cmath>

using neptune::core::theme::ControlStyle;
using neptune::core::theme::KnobStyle;
using neptune::core::theme::ThemeId;
using neptune::core::theme::Token;

namespace nova
{
namespace
{
juce::Colour toColour (const neptune::core::theme::Rgba& c, float extraAlpha = 1.0f)
{
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a) * extraAlpha);
}
} // namespace

// ------------------------------------------------------------------------------------------------ Skin
Skin::Skin (const neptune::core::theme::Palette& p, KnobStyle knob, std::optional<ThemeId> id)
    : palette_ (p), knob_ (knob)
{
    // Le thème n'est pas transmis par les éditeurs existants : le style de potard « pot » est celui du thème bois.
    const auto& def = neptune::core::theme::definition (id.value_or (knob == KnobStyle::Pot ? ThemeId::Hammond : ThemeId::Dark));
    metrics_ = def.metrics;
    control_ = def.control;
}

juce::Colour Skin::colour (Token t, float alpha) const { return toColour (palette_.color (t), alpha); }

void Skin::fill (juce::Graphics& g, Token t, juce::Rectangle<float> r, float radius) const
{
    const auto& p = palette_.paint (t);
    if (p.gradient)
        g.setGradientFill (juce::ColourGradient (toColour (p.from), r.getX(), r.getY(), toColour (p.to), r.getX(), r.getBottom(), false));
    else
        g.setColour (toColour (p.from));
    if (radius > 0.0f) g.fillRoundedRectangle (r, radius);
    else g.fillRect (r);
}

juce::FontOptions Skin::font (float size, bool bold) const
{
    return juce::FontOptions (size * scale_, bold ? juce::Font::bold : juce::Font::plain);
}

void Skin::fillActive (juce::Graphics& g, juce::Rectangle<float> r, float radius, float strength) const
{
    if (highContrast())
    {
        g.setColour (colour (Token::Accent, 0.22f));
        g.fillRoundedRectangle (r, radius);
        return;
    }
    g.setGradientFill (juce::ColourGradient (colour (Token::Accent, 0.18f * strength), r.getX(), r.getY(),
                                             colour (Token::Pink, 0.10f * strength), r.getRight(), r.getBottom(), false));
    g.fillRoundedRectangle (r, radius);
}

// ------------------------------------------------------------------------------------------------ échelle de fenêtre
float fitScale (int w, int h, int dw, int dh, float minScale, float maxScale)
{
    const float s = std::min (static_cast<float> (w) / static_cast<float> (dw), static_cast<float> (h) / static_cast<float> (dh));
    return juce::jlimit (minScale, maxScale, s);
}

juce::Rectangle<int> fitRect (int w, int h, int dw, int dh, float scale)
{
    const int rw = juce::roundToInt (static_cast<float> (dw) * scale), rh = juce::roundToInt (static_cast<float> (dh) * scale);
    return { (w - rw) / 2, (h - rh) / 2, rw, rh };
}

void configureConstrainer (juce::ComponentBoundsConstrainer& c, int dw, int dh, float minScale, float maxScale)
{
    c.setFixedAspectRatio (static_cast<double> (dw) / static_cast<double> (dh));
    c.setSizeLimits (juce::roundToInt (static_cast<float> (dw) * minScale), juce::roundToInt (static_cast<float> (dh) * minScale),
                     juce::roundToInt (static_cast<float> (dw) * maxScale), juce::roundToInt (static_cast<float> (dh) * maxScale));
}

void configureStretchConstrainer (juce::ComponentBoundsConstrainer& c, int dw, int dh, float minScale, float maxScale, int maxWidth)
{
    c.setFixedAspectRatio (0.0);   // largeur et hauteur libres
    c.setSizeLimits (juce::roundToInt (static_cast<float> (dw) * minScale), juce::roundToInt (static_cast<float> (dh) * minScale),
                     maxWidth, juce::roundToInt (static_cast<float> (dh) * maxScale));
}

// ------------------------------------------------------------------------------------------------ gestes
void setParam (ParamHost& host, neptune::core::ParamId id, double plain)
{
    const auto* spec = host.parameters().find (id);
    if (spec == nullptr) return;
    host.uiBeginEdit (id);
    host.uiSetNormalised (id, spec->toNormalized (plain));
    host.uiEndEdit (id);
}

void setParamNormalised (ParamHost& host, neptune::core::ParamId id, double n)
{
    host.uiBeginEdit (id);
    host.uiSetNormalised (id, std::clamp (n, 0.0, 1.0));
    host.uiEndEdit (id);
}

namespace
{
// version de paramètre vue par un composant : retourne true si elle a changé
bool changed (ParamHost& host, neptune::core::ParamId id, std::uint32_t& seen)
{
    const auto& set = host.parameters();
    const auto index = set.indexOf (id);
    if (index == neptune::core::ParameterSet::npos) return false;
    const auto v = set.versionAt (index);
    if (v == seen) return false;
    seen = v;
    return true;
}
} // namespace

// ------------------------------------------------------------------------------------------------ barres
void paintHeaderBar (juce::Graphics& g, const Skin& skin, juce::Rectangle<float> r, const juce::String& word, const juce::String& sub, HeaderStyle style)
{
    skin.fill (g, Token::HeaderBg, r);
    g.setColour (skin.colour (Token::Pb));
    g.fillRect (r.getX(), r.getBottom() - 1.0f, r.getWidth(), 1.0f);
    if (style == HeaderStyle::Compact)
    {
        g.setColour (skin.colour (Token::Text));
        g.setFont (skin.font (17.0f, true));
        g.drawText (word, juce::Rectangle<float> (r.getX() + skin.px (18.0f), r.getY(), skin.px (260.0f), r.getHeight()), juce::Justification::centredLeft, false);
        return;
    }
    // logo : carré arrondi en dégradé accent -> rose
    auto logo = juce::Rectangle<float> (r.getX() + skin.px (14.0f), r.getCentreY() - skin.px (17.0f), skin.px (34.0f), skin.px (34.0f));
    g.setGradientFill (juce::ColourGradient (skin.colour (Token::Accent), logo.getX(), logo.getY(), skin.colour (Token::Pink), logo.getRight(),
                                             logo.getBottom(), false));
    g.fillRoundedRectangle (logo, skin.px (9.0f));
    g.setColour (skin.colour (Token::LogoInk));
    juce::Path wave;
    const auto c = logo.getCentre();
    for (int i = 0; i <= 12; ++i)
    {
        const float x = c.x - skin.px (9.0f) + skin.px (18.0f) * static_cast<float> (i) / 12.0f;
        const float y = c.y + std::sin (static_cast<float> (i) / 12.0f * 6.2832f * 1.5f) * skin.px (5.0f);
        if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
    }
    g.strokePath (wave, juce::PathStrokeType (skin.px (2.0f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // nom et sous-titre
    g.setColour (skin.colour (Token::Text));
    g.setFont (skin.font (17.0f, true));
    g.drawText (word, juce::Rectangle<float> (logo.getRight() + skin.px (10.0f), r.getCentreY() - skin.px (17.0f), skin.px (260.0f), skin.px (20.0f)),
                juce::Justification::centredLeft, false);
    g.setColour (skin.colour (Token::Accent));
    g.setFont (skin.font (8.0f, true));
    g.drawText (sub, juce::Rectangle<float> (logo.getRight() + skin.px (10.0f), r.getCentreY() + skin.px (4.0f), skin.px (260.0f), skin.px (12.0f)),
                juce::Justification::centredLeft, false);
    if (style == HeaderStyle::Window) return;
    // numéro de version de développement, à droite : pour vérifier d'un coup d'œil quelle version tourne
    g.setColour (skin.colour (Token::TextDim));
    g.setFont (skin.font (8.0f, false));
    g.drawText (neptune::core::version::kString, r.withTrimmedLeft (r.getWidth() * 0.5f).withTrimmedRight (skin.px (14.0f)), juce::Justification::centredRight, false);
}

void paintBottomBar (juce::Graphics& g, const Skin& skin, juce::Rectangle<float> r)
{
    skin.fill (g, Token::BottomBg, r);
    g.setColour (skin.colour (Token::Pb));
    g.fillRect (r.getX(), r.getY(), r.getWidth(), 1.0f);
}

// ------------------------------------------------------------------------------------------------ Panel
Panel::Panel (const Skin& s, Token fill) : skin_ (s), fill_ (fill) {}

void Panel::setTitle (const juce::String& t) { title_ = t; repaint(); }
void Panel::setMeta (const juce::String& t) { meta_ = t; repaint(); }

juce::Rectangle<int> Panel::contentArea() const
{
    auto r = getLocalBounds().reduced (skin_.ipx (16.0f), skin_.ipx (14.0f));
    if (title_.isNotEmpty() || meta_.isNotEmpty()) r.removeFromTop (skin_.ipx (22.0f));
    return r;
}

void Panel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float rad = skin_.px (static_cast<float> (skin_.metrics().cornerRadius));
    skin_.fill (g, fill_, b, rad);
    g.setColour (skin_.colour (Token::Pb));
    g.drawRoundedRectangle (b, rad, skin_.borderWidth());
    if (title_.isNotEmpty())
    {
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (12.0f, true));
        g.drawText (title_, juce::Rectangle<float> (skin_.px (16.0f), skin_.px (12.0f), b.getWidth() * 0.6f, skin_.px (16.0f)),
                    juce::Justification::centredLeft, true);
    }
    if (meta_.isNotEmpty())
    {
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (11.0f));
        g.drawText (meta_, juce::Rectangle<float> (b.getWidth() * 0.4f, skin_.px (12.0f), b.getWidth() * 0.6f - skin_.px (16.0f + metaInset_), skin_.px (16.0f)),
                    juce::Justification::centredRight, true);
    }
}

// ------------------------------------------------------------------------------------------------ Pill
Pill::Pill (const Skin& s, Look l) : skin_ (s), look_ (l) {}

void Pill::setText (const juce::String& t) { if (t != text_) { text_ = t; repaint(); } }
void Pill::setOn (bool o) { if (o != on_) { on_ = o; repaint(); } }
void Pill::setIcon (juce::Path p, bool strokeOnly) { icon_ = std::move (p); iconStroke_ = strokeOnly; repaint(); }

void Pill::bindToggle (ParamHost& h, neptune::core::ParamId id, juce::String label)
{
    host_ = &h; id_ = id; label_ = std::move (label); choices_.clear();
    seen_ = 0xFFFFFFFFu;
    refresh();
}

void Pill::bindChoice (ParamHost& h, neptune::core::ParamId id, juce::String label, std::vector<juce::String> choices)
{
    host_ = &h; id_ = id; label_ = std::move (label); choices_ = std::move (choices);
    seen_ = 0xFFFFFFFFu;
    refresh();
}

void Pill::bindValue (ParamHost& h, neptune::core::ParamId id, juce::String label)
{
    host_ = &h; id_ = id; label_ = std::move (label); choices_.clear();
    valueMode_ = true;
    seen_ = 0xFFFFFFFFu;
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    refresh();
}

void Pill::refresh()
{
    if (host_ == nullptr || ! changed (*host_, id_, seen_)) return;
    if (valueMode_)
    {
        const auto v = juce::String::fromUTF8 (host_->parameters().toText (id_).c_str());
        setText (label_.isEmpty() ? v : label_ + "  " + v);
        return;
    }
    const int v = static_cast<int> (std::lround (host_->parameters().get (id_)));
    if (choices_.empty())
    {
        setOn (v != 0);
        setText (label_);
    }
    else
    {
        setOn (false);
        const auto& c = choices_[static_cast<std::size_t> (juce::jlimit (0, static_cast<int> (choices_.size()) - 1, v))];
        setText (label_.isEmpty() ? c : label_ + "  " + c);
    }
}

void Pill::mouseDown (const juce::MouseEvent& e)
{
    if (! valueMode_ || e.mods.isPopupMenu() || host_ == nullptr) return;
    dragging_ = false;
    startNorm_ = host_->parameters().getNormalized (id_);
}

void Pill::mouseDrag (const juce::MouseEvent& e)
{
    if (! valueMode_ || host_ == nullptr || e.mods.isPopupMenu()) return;
    if (! dragging_)
    {
        if (std::abs (e.getDistanceFromDragStartY()) < 3) return;
        dragging_ = true;
        host_->uiBeginEdit (id_);
    }
    const double k = e.mods.isShiftDown() ? 0.1 : 1.0;
    host_->uiSetNormalised (id_, std::clamp (startNorm_ - static_cast<double> (e.getDistanceFromDragStartY()) * k / 160.0, 0.0, 1.0));
    repaint();
}

void Pill::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (valueMode_ && ! e.mods.isPopupMenu() && onClick) onClick();
}

void Pill::mouseUp (const juce::MouseEvent& e)
{
    if (valueMode_)
    {
        if (dragging_ && host_ != nullptr) host_->uiEndEdit (id_);
        dragging_ = false;
        return;
    }
    if (static_ || ! isEnabled() || e.mods.isPopupMenu() || ! getLocalBounds().contains (e.getPosition())) return;
    if (host_ != nullptr)
    {
        const int cur = static_cast<int> (std::lround (host_->parameters().get (id_)));
        const int n = choices_.empty() ? 2 : static_cast<int> (choices_.size());
        setParam (*host_, id_, (cur + 1) % n);
        refresh();
    }
    if (onClick) onClick();
}

float Pill::idealWidth() const
{
    const float size = fontSize_ > 0.0f ? fontSize_ : (look_ == Look::Pill ? 12.0f : 9.5f) * skin_.pillTextScale();
    const juce::Font f (juce::FontOptions (size, juce::Font::bold));
    return std::ceil (f.getStringWidthFloat (text_)) + 2.0f * (look_ == Look::Pill ? 10.0f : 6.0f) + (dot_ ? 12.0f : 0.0f) + 4.0f;
}

void Pill::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float rad = look_ == Look::Icon ? (skin_.metrics().iconButtonRadius < 0 ? b.getHeight() / 2.0f : skin_.px (static_cast<float> (skin_.metrics().iconButtonRadius)))
                    : look_ == Look::Small ? skin_.px (5.0f)
                    : skin_.px (static_cast<float> (skin_.metrics().pillRadius));
    skin_.fill (g, Token::BgCard, b, rad);
    if (on_) skin_.fillActive (g, b, rad);
    auto border = skin_.colour (Token::CtlBd);
    if (on_) border = skin_.colour (tint_, 0.5f);
    else if (hover_ && ! static_ && ! skin_.highContrast()) border = skin_.colour (tint_, 0.4f);
    g.setColour (border);
    g.drawRoundedRectangle (b, rad, skin_.borderWidth());

    const auto textColour = on_ ? (look_ == Look::Pill ? skin_.colour (Token::Text) : skin_.colour (tint_))
                                : dim_ ? skin_.colour (Token::TextDim2) : hover_ && ! static_ ? skin_.colour (Token::Text) : skin_.colour (Token::TextDim);
    if (! isEnabled()) g.setOpacity (0.4f);
    if (! icon_.isEmpty())
    {
        const float side = std::min (b.getWidth(), b.getHeight()) * 0.5f;
        auto t = icon_.getTransformToScaleToFit (b.getCentreX() - side / 2, b.getCentreY() - side / 2, side, side, true);
        g.setColour (textColour);
        if (iconStroke_) g.strokePath (icon_, juce::PathStrokeType (skin_.px (1.6f) * 24.0f / side), t);
        else g.fillPath (icon_, t);
        return;
    }
    auto area = b.reduced (skin_.px (look_ == Look::Pill ? 10.0f : 6.0f), 0.0f);
    if (dot_)
    {
        const float d = skin_.px (6.0f);
        g.setColour (on_ ? skin_.colour (tint_) : skin_.colour (Token::Grid));
        g.fillEllipse (area.getX(), area.getCentreY() - d / 2, d, d);
        area.removeFromLeft (d + skin_.px (6.0f));
    }
    g.setColour (textColour);
    float size = fontSize_ > 0.0f ? fontSize_ : (look_ == Look::Pill ? 12.0f : 9.5f) * skin_.pillTextScale();
    const float wanted = juce::Font (juce::FontOptions (size * skin_.scale(), juce::Font::bold)).getStringWidthFloat (text_);
    if (wanted > area.getWidth() && wanted > 0.0f) size *= std::max (0.6f, area.getWidth() / wanted);   // texte trop long : réduit plutôt que coupé
    g.setFont (skin_.font (size, true));
    g.drawText (text_, area, juce::Justification::centred, true);
}

// ------------------------------------------------------------------------------------------------ Text
Text::Text (const Skin& s, Token colour, float size, bool bold) : skin_ (s), colour_ (colour), size_ (size), bold_ (bold) { setInterceptsMouseClicks (false, false); }

void Text::setText (const juce::String& t)
{
    if (t == text_) return;
    text_ = t;
    repaint();
}

void Text::paint (juce::Graphics& g)
{
    g.setColour (skin_.colour (colour_));
    g.setFont (skin_.font (size_, bold_));
    g.drawFittedText (text_, getLocalBounds(), juce::Justification::topLeft, 12, 1.0f);
}

// ------------------------------------------------------------------------------------------------ Switch
Switch::Switch (const Skin& s) : skin_ (s) {}

void Switch::setOn (bool o) { if (o != on_) { on_ = o; repaint(); } }

void Switch::bind (ParamHost& h, neptune::core::ParamId id) { host_ = &h; id_ = id; seen_ = 0xFFFFFFFFu; refresh(); }

void Switch::refresh()
{
    if (host_ == nullptr || ! changed (*host_, id_, seen_)) return;
    setOn (std::lround (host_->parameters().get (id_)) != 0);
}

void Switch::mouseUp (const juce::MouseEvent& e)
{
    if (! isEnabled() || e.mods.isPopupMenu() || ! getLocalBounds().contains (e.getPosition())) return;
    setOn (! on_);
    if (host_ != nullptr) setParam (*host_, id_, on_ ? 1.0 : 0.0);
    if (onChange) onChange (on_);
}

void Switch::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool bezel = skin_.controlStyle() == ControlStyle::Bezel;
    const bool hc = skin_.highContrast();
    const float rad = bezel ? skin_.px (2.0f) : b.getHeight() / 2.0f;
    if (on_ && ! bezel && ! hc)
        g.setGradientFill (juce::ColourGradient (skin_.colour (Token::Accent), b.getX(), 0.0f, skin_.colour (Token::Pink), b.getRight(), 0.0f, false));
    else
        g.setColour (on_ && hc ? skin_.colour (Token::Accent) : bezel ? skin_.colour (Token::BgApp).darker (0.6f) : skin_.colour (Token::Grid));
    g.fillRoundedRectangle (b, rad);
    if (bezel || hc)
    {
        g.setColour (skin_.colour (Token::CtlBd));
        g.drawRoundedRectangle (b, rad, skin_.borderWidth());
    }
    const float m = skin_.px (2.0f), th = b.getHeight() - 2.0f * m;
    const float x = on_ ? b.getRight() - m - (bezel ? th * 0.7f : th) : b.getX() + m;
    g.setColour (bezel && on_ ? skin_.colour (Token::Accent) : hc && ! on_ ? skin_.colour (Token::BgApp) : skin_.colour (Token::Handle));
    if (bezel) g.fillRoundedRectangle (x, b.getY() + m, th * 0.7f, th, skin_.px (1.0f));
    else g.fillEllipse (x, b.getY() + m, th, th);
}

// ------------------------------------------------------------------------------------------------ NavItem
NavItem::NavItem (const Skin& s, juce::String label, juce::Path icon) : skin_ (s), label_ (std::move (label)), icon_ (std::move (icon)) {}

void NavItem::setActive (bool a) { if (a != active_) { active_ = a; repaint(); } }

void NavItem::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! getLocalBounds().contains (e.getPosition())) return;
    if (onClick) onClick();
}

void NavItem::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float rad = skin_.px (10.0f);
    if (active_)
    {
        g.setGradientFill (juce::ColourGradient (skin_.colour (Token::Nav1), b.getX(), b.getY(), skin_.colour (Token::Nav2), b.getRight(), b.getBottom(), false));
        g.fillRoundedRectangle (b, rad);
        g.setColour (skin_.colour (Token::Navb));
        g.drawRoundedRectangle (b, rad, skin_.borderWidth());
    }
    const float isz = skin_.px (16.0f);
    const auto iconBox = juce::Rectangle<float> (b.getX() + skin_.px (12.0f), b.getCentreY() - isz / 2, isz, isz);
    g.setColour (active_ ? skin_.colour (Token::Accent) : skin_.colour (Token::TextDim, 0.8f));
    g.strokePath (icon_, juce::PathStrokeType (2.0f * isz / 24.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                  icon_.getTransformToScaleToFit (iconBox, true));
    g.setColour (active_ ? skin_.colour (Token::Text) : skin_.colour (Token::TextDim));
    g.setFont (skin_.font (13.0f, true));
    g.drawText (label_, b.withTrimmedLeft (skin_.px (12.0f + 16.0f + 12.0f)), juce::Justification::centredLeft, true);
}

// ------------------------------------------------------------------------------------------------ KnobCard
KnobCard::KnobCard (const Skin& s, ParamHost& h, neptune::core::ParamId id, juce::String title, NeptuneKnob::OpenModal modal, bool pink)
    : skin_ (s), host_ (h), id_ (id), title_ (std::move (title)), pink_ (pink)
{
    knob_ = std::make_unique<NeptuneKnob> (h, id, s.palette(), s.knobStyle(), std::move (modal));
    addAndMakeVisible (*knob_);
    refresh();
}

void KnobCard::setDisplayRange (double lo, double hi, double exponent)
{
    knob_->setDisplayRange (lo, hi, exponent);
    if (const auto* spec = host_.parameters().find (id_))
    {
        lo_ = juce::String::fromUTF8 (host_.parameters().toText (*spec, lo).c_str());
        hi_ = juce::String::fromUTF8 (host_.parameters().toText (*spec, hi).c_str());
    }
    repaint();
}

void KnobCard::setParam (neptune::core::ParamId id, juce::String title)
{
    id_ = id;
    title_ = std::move (title);
    knob_->setParamId (id);
    seen_ = 0xFFFFFFFFu;
    if (autoRange_)
        if (const auto* spec = host_.parameters().find (id))
        {
            lo_ = juce::String::fromUTF8 (host_.parameters().toText (*spec, spec->min).c_str());
            hi_ = juce::String::fromUTF8 (host_.parameters().toText (*spec, spec->max).c_str());
        }
    refresh();
    repaint();
}

void KnobCard::refresh()
{
    if (! changed (host_, id_, seen_)) return;
    valueText_ = juce::String::fromUTF8 (host_.parameters().toText (id_).c_str());
    repaint();
}

void KnobCard::resized()
{
    knob_->setScale (skin_.scale());
    const auto b = getLocalBounds();
    const int side = std::min (skin_.ipx (compact_ ? 44.0f : 60.0f), b.getWidth() - skin_.ipx (8.0f));
    knob_->setBounds (juce::Rectangle<int> (side, side).withCentre ({ b.getCentreX(), skin_.ipx (compact_ ? 20.0f : 28.0f) + side / 2 }));
}

void KnobCard::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    if (! plain_)
    {
        const float rad = skin_.px (static_cast<float> (skin_.metrics().cornerRadius));
        skin_.fill (g, Token::PanelBg, b, rad);
        g.setColour (skin_.colour (Token::Pb));
        g.drawRoundedRectangle (b, rad, skin_.borderWidth());
    }
    g.setColour (pink_ ? skin_.colour (Token::Pink2) : skin_.colour (Token::TextDim));
    g.setFont (skin_.font (compact_ ? 9.0f : 10.0f, true));
    g.drawText (title_, b.withTrimmedTop (skin_.px (compact_ ? 4.0f : 10.0f)).withHeight (skin_.px (14.0f)), juce::Justification::centred, true);
    const auto kb = knob_->getBounds().toFloat();
    g.setColour (skin_.colour (Token::Text));
    g.setFont (skin_.font (compact_ ? 11.0f : 14.0f, true));
    g.drawText (valueText_, juce::Rectangle<float> (b.getX(), kb.getBottom() + skin_.px (2.0f), b.getWidth(), skin_.px (18.0f)),
                juce::Justification::centred, true);
    if (lo_.isNotEmpty() || hi_.isNotEmpty())
    {
        g.setColour (skin_.colour (Token::TextDim2));
        g.setFont (skin_.font (13.5f));   // 9 x 1,5
        const auto row = juce::Rectangle<float> (b.getX() + skin_.px (8.0f), b.getBottom() - skin_.px (22.0f), b.getWidth() - skin_.px (16.0f), skin_.px (18.0f));
        g.drawText (lo_, row, juce::Justification::centredLeft, true);
        g.drawText (hi_, row, juce::Justification::centredRight, true);
    }
}

// ------------------------------------------------------------------------------------------------ SubSlider
SubSlider::SubSlider (const Skin& s, ParamHost& h, neptune::core::ParamId id, juce::String title, bool pink)
    : skin_ (s), host_ (h), id_ (id), title_ (std::move (title)), pink_ (pink) {}

void SubSlider::refresh() { if (changed (host_, id_, seen_)) repaint(); }

juce::Rectangle<float> SubSlider::trackArea() const
{
    const auto b = getLocalBounds().toFloat();
    return { b.getX() + skin_.px (6.0f), b.getY() + skin_.px (17.0f), b.getWidth() - skin_.px (12.0f), skin_.px (4.0f) };
}

void SubSlider::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (skin_.colour (Token::TextDim2));
    g.setFont (skin_.font (12.0f, true));   // 8 x 1,5
    g.drawText (title_, b.withHeight (skin_.px (15.0f)), juce::Justification::centred, true);
    auto t = trackArea();
    const float n = static_cast<float> (host_.parameters().getNormalized (id_));
    g.setColour (skin_.colour (Token::Grid));
    g.fillRoundedRectangle (t, t.getHeight() / 2);
    const float x = t.getX() + n * t.getWidth();
    if (pink_)
        g.setGradientFill (juce::ColourGradient (skin_.colour (Token::Pink), t.getX(), 0.0f, skin_.colour (Token::Pink2), t.getRight(), 0.0f, false));
    else
        g.setColour (skin_.colour (Token::Accent));
    g.fillRoundedRectangle (t.withRight (std::max (t.getX() + 1.0f, x)), t.getHeight() / 2);
    const float hd = skin_.px (12.0f);
    g.setColour (skin_.colour (Token::Hring));
    g.fillEllipse (x - hd / 2 - 2.0f, t.getCentreY() - hd / 2 - 2.0f, hd + 4.0f, hd + 4.0f);
    g.setColour (skin_.colour (Token::Handle));
    g.fillEllipse (x - hd / 2, t.getCentreY() - hd / 2, hd, hd);
    g.setColour (skin_.colour (Token::TextDim));
    g.setFont (skin_.font (13.5f));   // 9 x 1,5
    g.drawText (juce::String::fromUTF8 (host_.parameters().toText (id_).c_str()), b.withTop (t.getBottom() + skin_.px (4.0f)).withHeight (skin_.px (19.0f)),
                juce::Justification::centred, true);
}

void SubSlider::applyFromX (float x)
{
    const auto t = trackArea();
    host_.uiSetNormalised (id_, std::clamp (static_cast<double> ((x - t.getX()) / t.getWidth()), 0.0, 1.0));
    repaint();
}

void SubSlider::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    host_.uiBeginEdit (id_);
    dragging_ = true;
    applyFromX (e.position.x);
}
void SubSlider::mouseDrag (const juce::MouseEvent& e) { if (dragging_) applyFromX (e.position.x); }
void SubSlider::mouseUp (const juce::MouseEvent&)
{
    if (dragging_) host_.uiEndEdit (id_);
    dragging_ = false;
}
void SubSlider::mouseDoubleClick (const juce::MouseEvent&) { host_.uiResetToDefault (id_); repaint(); }

// ------------------------------------------------------------------------------------------------ VMeter
VMeter::VMeter (const Skin& s, juce::String title, Kind k, bool pink) : skin_ (s), title_ (std::move (title)), kind_ (k), pink_ (pink)
{
    setBufferedToImage (true);
}

juce::Rectangle<float> VMeter::barsArea() const
{
    const auto b = getLocalBounds().toFloat();
    auto r = b.withTrimmedTop (skin_.px (18.0f)).withTrimmedBottom (skin_.px (70.0f));
    const float w = skin_.px (13.0f) * 2.0f + skin_.px (4.0f);
    return r.withSizeKeepingCentre (w, r.getHeight());
}

juce::Rectangle<float> VMeter::modeArea() const
{
    const auto b = getLocalBounds().toFloat();
    const float w = skin_.px (46.0f);
    return { b.getCentreX() - w / 2, b.getBottom() - skin_.px (20.0f), w, skin_.px (18.0f) };
}

bool VMeter::update (float l, float r)
{
    const float in[2] = { l, r };
    // retombée linéaire en dB par seconde (horloge murale), montée immédiate ; la crête tient jusqu'à la remise à zéro
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const float dt = lastTime_ > 0.0 ? static_cast<float> (std::clamp (now - lastTime_, 0.0, 0.25)) : 0.0f;
    lastTime_ = now;
    for (int c = 0; c < 2; ++c)
    {
        const float lo = kind_ == Kind::Level ? floor_ - 40.0f : 0.0f;
        shown_[c] = in[c] > shown_[c] ? in[c] : std::max (lo, shown_[c] - decay_ * dt);
        peak_[c] = std::max (peak_[c], in[c]);
    }
    if (std::fabs (shown_[0] - painted_[0]) < 0.25f && std::fabs (shown_[1] - painted_[1]) < 0.25f
        && std::fabs (peak_[0] - painted_[2]) < 0.1f && std::fabs (peak_[1] - painted_[3]) < 0.1f)
        return false;
    painted_[0] = shown_[0]; painted_[1] = shown_[1]; painted_[2] = peak_[0]; painted_[3] = peak_[1];
    repaint();
    return true;
}

void VMeter::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    if (showMode_ && modeArea().contains (e.position)) { if (onModeClick) onModeClick(); }
    else resetPeak();
}

void VMeter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (pink_ ? skin_.colour (Token::Pink2) : skin_.colour (Token::TextDim));
    g.setFont (skin_.font (9.0f, true));
    g.drawText (title_, b.withHeight (skin_.px (14.0f)), juce::Justification::centred);

    const auto bars = barsArea();
    const float bw = skin_.px (13.0f), gap = skin_.px (4.0f);
    for (int c = 0; c < 2; ++c)
    {
        const juce::Rectangle<float> t (bars.getX() + static_cast<float> (c) * (bw + gap), bars.getY(), bw, bars.getHeight());
        g.setColour (skin_.colour (Token::Grid));
        g.fillRoundedRectangle (t, skin_.px (3.0f));
        const float span = kind_ == Kind::Level ? ceil_ - floor_ : 40.0f;
        const float f = kind_ == Kind::Level ? juce::jlimit (0.0f, 1.0f, (shown_[c] - floor_) / span) : juce::jlimit (0.0f, 1.0f, shown_[c] / span);
        const auto c1 = pink_ ? skin_.colour (Token::Pink2) : skin_.colour (Token::Accent2);
        const auto c2 = pink_ ? skin_.colour (Token::Pink) : skin_.colour (Token::Accent);
        const auto fillRect = kind_ == Kind::Level ? t.withTrimmedTop (t.getHeight() * (1.0f - f)) : t.withHeight (t.getHeight() * f);
        if (zones_ && kind_ == Kind::Level)
        {
            // dégradé diffus vert -> jaune (autour de -18 dB) -> rouge (autour de -6 dB), sur toute la hauteur du mètre
            const juce::Colour green (0xFF3DBE5Eu), yellow (0xFFE8C13Au), red (0xFFE5484Du);
            juce::ColourGradient grad (green, 0.0f, t.getBottom(), red, 0.0f, t.getY(), false);
            auto pos = [this, span] (float db) { return static_cast<double> (juce::jlimit (0.0f, 1.0f, (db - floor_) / span)); };
            grad.addColour (pos (-22.0f), green);
            grad.addColour (pos (-14.0f), yellow);
            grad.addColour (pos (-9.0f), yellow);
            grad.addColour (pos (-3.0f), red);
            g.setGradientFill (grad);
            g.fillRect (t.getX(), fillRect.getY(), t.getWidth(), fillRect.getHeight());
        }
        else
        {
            g.setGradientFill (juce::ColourGradient (c1, 0.0f, t.getY(), c2, 0.0f, t.getBottom(), false));
            g.fillRoundedRectangle (fillRect, skin_.px (3.0f));
        }
        // crête
        const float pf = kind_ == Kind::Level ? juce::jlimit (0.0f, 1.0f, (peak_[c] - floor_) / span) : juce::jlimit (0.0f, 1.0f, peak_[c] / span);
        if (pf > 0.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            const float y = kind_ == Kind::Level ? t.getBottom() - t.getHeight() * pf : t.getY() + t.getHeight() * pf - skin_.px (2.0f);
            g.fillRect (t.getX(), y, t.getWidth(), skin_.px (2.0f));
        }
    }
    const float v = std::max (shown_[0], shown_[1]);
    g.setColour (skin_.colour (Token::TextDim));
    g.setFont (skin_.font (13.5f));   // 9 x 1,5
    g.drawText (v < floor_ - 30.0f && kind_ == Kind::Level ? juce::String ("-inf") : juce::String (kind_ == Kind::Level ? v : -v, 1),
                b.withTop (bars.getBottom() + skin_.px (4.0f)).withHeight (skin_.px (18.0f)), juce::Justification::centred);
    g.drawText (juce::String (kind_ == Kind::Level ? std::max (peak_[0], peak_[1]) : -std::max (peak_[0], peak_[1]), 1),
                b.withTop (bars.getBottom() + skin_.px (23.0f)).withHeight (skin_.px (18.0f)), juce::Justification::centred);
    if (! showMode_) return;
    const auto m = modeArea();
    skin_.fill (g, Token::BgCard, m, skin_.px (5.0f));
    g.setColour (skin_.colour (Token::CtlBd));
    g.drawRoundedRectangle (m.reduced (0.5f), skin_.px (5.0f), skin_.borderWidth());
    g.setColour (pink_ ? skin_.colour (Token::Pink2) : skin_.colour (Token::Accent));
    g.setFont (skin_.font (9.0f, true));
    g.drawText (mode_, m, juce::Justification::centred);
}

// ------------------------------------------------------------------------------------------------ InfoButton
namespace
{
class HelpBubble : public juce::Component
{
public:
    HelpBubble (const Skin& s, const juce::String& title, const juce::String& body) : skin_ (s)
    {
        text_.setJustification (juce::Justification::topLeft);
        text_.append (title + "\n", juce::Font (juce::FontOptions (14.0f, juce::Font::bold)), s.colour (Token::Accent));
        text_.append (body, juce::Font (juce::FontOptions (13.0f)), s.colour (Token::Text));
        layout_.createLayout (text_, 300.0f);
        setSize (320, static_cast<int> (std::ceil (layout_.getHeight())) + 20);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (skin_.colour (Token::BgCard));
        layout_.draw (g, getLocalBounds().toFloat().reduced (10.0f));
    }

private:
    const Skin& skin_;
    juce::AttributedString text_;
    juce::TextLayout layout_;
};
} // namespace

InfoButton::InfoButton (const Skin& s, juce::String title, juce::String body) : skin_ (s), title_ (std::move (title)), body_ (std::move (body))
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void InfoButton::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (hover_ ? skin_.colour (Token::Accent) : skin_.colour (Token::TextDim2));
    g.drawEllipse (b, skin_.px (1.2f));
    g.setFont (skin_.font (10.0f, true));
    g.drawText ("?", b, juce::Justification::centred);
}

void InfoButton::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! getLocalBounds().contains (e.getPosition())) return;
    auto* top = getTopLevelComponent();
    if (top == nullptr) return;
    juce::CallOutBox::launchAsynchronously (std::make_unique<HelpBubble> (skin_, title_, body_), top->getLocalArea (this, getLocalBounds()), top);
}

// ------------------------------------------------------------------------------------------------ fenêtre de réglages
namespace
{
class SettingsBubble : public juce::Component
{
public:
    SettingsBubble (const Skin& s, ParamHost& host, const juce::String& title, std::vector<SettingItem> items) : skin_ (s), title_ (title)
    {
        for (auto& it : items)
        {
            auto p = std::make_unique<Pill> (skin_);
            if (it.choices.empty()) p->bindToggle (host, it.id, it.label);
            else p->bindChoice (host, it.id, it.label, it.choices);
            addAndMakeVisible (*p);
            pills_.push_back (std::move (p));
        }
        const int rows = static_cast<int> ((pills_.size() + 1) / 2);
        setSize (14 * 2 + 160 * 2 + 8, 14 + 22 + rows * 38 + 6);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (skin_.colour (Token::BgCard));
        g.setColour (skin_.colour (Token::Accent));
        g.setFont (skin_.font (12.0f, true));
        g.drawText (title_, 14, 10, getWidth() - 28, 18, juce::Justification::centredLeft);
    }
    void resized() override
    {
        for (std::size_t i = 0; i < pills_.size(); ++i)
            pills_[i]->setBounds (14 + static_cast<int> (i % 2) * 168, 14 + 22 + static_cast<int> (i / 2) * 38, 160, 30);
    }

private:
    Skin skin_;   // copie : la bulle peut survivre à l'éditeur qui l'a ouverte
    juce::String title_;
    std::vector<std::unique_ptr<Pill>> pills_;
};
} // namespace

void showSettingsPopup (juce::Component* anchor, const Skin& skin, ParamHost& host, const juce::String& title, std::vector<SettingItem> items)
{
    auto* top = anchor->getTopLevelComponent();
    if (top == nullptr) return;
    juce::CallOutBox::launchAsynchronously (std::make_unique<SettingsBubble> (skin, host, title, std::move (items)), top->getLocalArea (anchor, anchor->getLocalBounds()), top);
}

// ------------------------------------------------------------------------------------------------ LookAndFeel
LookAndFeel::LookAndFeel (const Skin& s) : skin_ (s)
{
    setColour (juce::PopupMenu::backgroundColourId, s.colour (Token::BgCard));
    setColour (juce::PopupMenu::textColourId, s.colour (Token::Text));
    setColour (juce::PopupMenu::headerTextColourId, s.colour (Token::TextDim2));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, s.colour (Token::Accent, 0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, s.colour (Token::Text));
    setColour (juce::TooltipWindow::backgroundColourId, s.colour (Token::BgCard));
    setColour (juce::TooltipWindow::textColourId, s.colour (Token::Text));
    setColour (juce::TooltipWindow::outlineColourId, s.colour (Token::CtlBd));
    setColour (juce::ResizableWindow::backgroundColourId, s.colour (Token::BgCard));
    setColour (juce::TextEditor::backgroundColourId, s.colour (Token::BgCard));
    setColour (juce::TextEditor::textColourId, s.colour (Token::Text));
    setColour (juce::TextEditor::outlineColourId, s.colour (Token::CtlBd));
    setColour (juce::TextEditor::focusedOutlineColourId, s.colour (Token::Accent));
    setColour (juce::ScrollBar::thumbColourId, s.colour (Token::TextDim2));
    setColour (juce::ComboBox::textColourId, s.colour (Token::Text));
    setColour (juce::ComboBox::arrowColourId, s.colour (Token::TextDim));
    setColour (juce::TextButton::textColourOffId, s.colour (Token::TextDim));
    setColour (juce::TextButton::textColourOnId, s.colour (Token::Text));
    setColour (juce::ToggleButton::textColourId, s.colour (Token::Text));
    setColour (juce::Label::textColourId, s.colour (Token::TextDim));
    setColour (juce::Slider::textBoxTextColourId, s.colour (Token::Text));
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
}

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (skin_.colour (Token::BgCard));
    g.setColour (skin_.colour (Token::CtlBd));
    g.drawRect (0, 0, w, h, 1);
}

juce::Font LookAndFeel::getPopupMenuFont() { return juce::Font (skin_.font (13.0f * popupScale_)); }

// Bulle ancrée (routage, etc.) : carte NOVA avec sa flèche, au lieu du cadre JUCE d'origine
void LookAndFeel::drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics& g, const juce::Path& path, juce::Image&)
{
    g.setColour (skin_.colour (Token::BgCard));
    g.fillPath (path);
    g.setColour (skin_.colour (Token::CtlBd));
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

void LookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical, int thumbStart,
                                 int thumbSize, bool over, bool down)
{
    const auto thumb = vertical ? juce::Rectangle<int> (x, y + thumbStart, width, thumbSize) : juce::Rectangle<int> (x + thumbStart, y, thumbSize, height);
    g.setColour (skin_.colour (Token::TextDim2, over || down ? 0.9f : 0.55f));
    g.fillRoundedRectangle (thumb.toFloat().reduced (1.0f), skin_.px (static_cast<float> (skin_.metrics().scrollbarRadius)));
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool highlighted, bool down)
{
    const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float rad = skin_.px (5.0f);
    const bool on = b.getToggleState();
    skin_.fill (g, Token::BgCard, r, rad);
    if (on) skin_.fillActive (g, r, rad);
    else if (down) { g.setColour (skin_.colour (Token::Accent, 0.18f)); g.fillRoundedRectangle (r, rad); }
    g.setColour (on ? skin_.colour (Token::Accent, 0.5f) : highlighted || down ? skin_.colour (Token::Accent, 0.4f) : skin_.colour (Token::CtlBd));
    g.drawRoundedRectangle (r, rad, skin_.borderWidth());
}

float LookAndFeel::buttonFontSize (int buttonHeight) const noexcept
{
    if (buttonTextScale_ == 1.0f) return 10.5f;
    return std::max (10.5f, std::min (10.5f * buttonTextScale_, static_cast<float> (buttonHeight) * 0.55f / skin_.scale()));
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int h) { return juce::Font (skin_.font (buttonFontSize (h), true)); }

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    g.setFont (skin_.font (buttonFontSize (b.getHeight()), true));
    g.setColour (skin_.colour (b.getToggleState() ? Token::Accent : highlighted ? Token::Text : Token::TextDim).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.drawText (b.getButtonText(), b.getLocalBounds().reduced (skin_.ipx (4.0f), 0), juce::Justification::centred, true);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    const float side = std::min (skin_.px (16.0f), static_cast<float> (b.getHeight()) - 2.0f);
    const juce::Rectangle<float> box (skin_.px (2.0f), (static_cast<float> (b.getHeight()) - side) / 2.0f, side, side);
    const float rad = skin_.px (4.0f);
    skin_.fill (g, Token::BgCard, box, rad);
    if (b.getToggleState())
    {
        skin_.fillActive (g, box, rad);
        juce::Path tick;
        tick.startNewSubPath (box.getX() + side * 0.25f, box.getCentreY());
        tick.lineTo (box.getX() + side * 0.43f, box.getY() + side * 0.72f);
        tick.lineTo (box.getX() + side * 0.78f, box.getY() + side * 0.28f);
        g.setColour (skin_.colour (Token::Accent));
        g.strokePath (tick, juce::PathStrokeType (skin_.px (1.8f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour (b.getToggleState() ? skin_.colour (Token::Accent, 0.5f) : highlighted ? skin_.colour (Token::Accent, 0.4f) : skin_.colour (Token::CtlBd));
    g.drawRoundedRectangle (box.reduced (0.5f), rad, skin_.borderWidth());
    g.setFont (skin_.font (11.5f));
    g.setColour (b.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.drawText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (static_cast<int> (side + skin_.px (10.0f))), juce::Justification::centredLeft, true);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (w), static_cast<float> (h)).reduced (0.5f);
    const float rad = skin_.px (5.0f);
    skin_.fill (g, Token::BgCard, r, rad);
    g.setColour (down || box.hasKeyboardFocus (true) ? skin_.colour (Token::Accent, 0.5f) : skin_.colour (Token::CtlBd));
    g.drawRoundedRectangle (r, rad, skin_.borderWidth());
    const float c = static_cast<float> (w) - skin_.px (14.0f), m = static_cast<float> (h) / 2.0f, a = skin_.px (3.5f);
    juce::Path arrow;
    arrow.startNewSubPath (c - a, m - a * 0.5f);
    arrow.lineTo (c, m + a * 0.5f);
    arrow.lineTo (c + a, m - a * 0.5f);
    g.setColour (skin_.colour (Token::TextDim).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (arrow, juce::PathStrokeType (skin_.px (1.5f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox&) { return juce::Font (skin_.font (11.5f)); }

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (skin_.ipx (8.0f), 1, box.getWidth() - skin_.ipx (26.0f), box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle style, juce::Slider& s)
{
    const float alpha = s.isEnabled() ? 1.0f : 0.4f;
    // modulation (hôte ou matrice) : la propriété « modValue » porte la valeur modulée ; un trait de la valeur réglée à la valeur modulée
    // et un petit point au bout
    const bool modulated = s.getProperties().contains ("modValue");
    const float modPos = modulated ? s.getPositionOfValue (static_cast<double> (s.getProperties()["modValue"])) : pos;
    // même teinte que l'anneau de modulation des potards, tirée vers le jaune pour bien ressortir du remplissage orange
    const auto modColour = skin_.colour (Token::Accent2, alpha).interpolatedWith (juce::Colour (0xffffe066).withAlpha (alpha), 0.55f);
    if (style == juce::Slider::LinearVertical && s.getProperties().getWithDefault ("fader", false))
    {
        // fader du mixeur : rail sombre, remplissage orange dégradé depuis le bas, poignée blanche arrondie
        const float cx = static_cast<float> (x) + static_cast<float> (w) / 2.0f, tw = skin_.px (5.0f);
        const juce::Rectangle<float> track (cx - tw / 2.0f, static_cast<float> (y), tw, static_cast<float> (h));
        g.setColour (skin_.colour (Token::Grid));
        g.fillRoundedRectangle (track, tw / 2.0f);
        const auto fill = track.withTop (pos);
        g.setGradientFill (juce::ColourGradient (skin_.colour (Token::Accent, alpha).brighter (0.3f), fill.getX(), fill.getY(),
                                                 skin_.colour (Token::Accent, alpha), fill.getX(), fill.getBottom(), false));
        g.fillRoundedRectangle (fill, tw / 2.0f);
        const float tw2 = std::min (static_cast<float> (w) - 2.0f, skin_.px (24.0f)), th = skin_.px (10.0f);
        g.setColour (skin_.colour (Token::Text, alpha == 1.0f ? 1.0f : 0.5f));
        g.fillRoundedRectangle (cx - tw2 / 2.0f, pos - th / 2.0f, tw2, th, skin_.px (3.0f));
        if (modulated)
        {
            // trait fin sur le bord droit, à côté du rail : la valeur réglée reste visible, la modulation se lit à côté
            const float lw = skin_.px (2.5f), lx = static_cast<float> (x + w) - lw - skin_.px (0.5f);
            const float dm = skin_.px (6.0f);
            g.setColour (modColour);
            g.fillRoundedRectangle (juce::Rectangle<float> (lx, std::min (pos, modPos), lw, std::max (std::abs (pos - modPos), skin_.px (2.0f))), lw / 2.0f);
            g.fillEllipse (lx + lw / 2.0f - dm / 2.0f, modPos - dm / 2.0f, dm, dm);
        }
        return;
    }
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0.0f, 0.0f, style, s);
        return;
    }
    const float cy = static_cast<float> (y) + static_cast<float> (h) / 2.0f, th = skin_.px (4.0f);
    const juce::Rectangle<float> track (static_cast<float> (x), cy - th / 2.0f, static_cast<float> (w), th);
    g.setColour (skin_.colour (Token::Grid));
    g.fillRoundedRectangle (track, th / 2.0f);
    g.setColour (skin_.colour (Token::Accent, alpha));
    if (s.getProperties().getWithDefault ("bipolar", false))
    {
        // réglage centré (balance) : le remplissage part du milieu
        const float mid = static_cast<float> (x) + static_cast<float> (w) / 2.0f;
        g.fillRoundedRectangle (juce::Rectangle<float> (std::min (mid, pos), track.getY(), std::abs (pos - mid), th), th / 2.0f);
    }
    else g.fillRoundedRectangle (track.withRight (pos), th / 2.0f);
    const float d = skin_.px (11.0f);
    g.setColour (skin_.colour (Token::Text, s.isEnabled() ? 1.0f : 0.5f));
    g.fillEllipse (pos - d / 2.0f, cy - d / 2.0f, d, d);
    if (modulated)
    {
        // trait fin au-dessus du rail, de la valeur réglée à la valeur modulée, avec un petit point au bout (comme l'anneau des potards)
        const float lh = skin_.px (2.5f), ly = std::max (static_cast<float> (y) + skin_.px (2.5f), cy - d / 2.0f - lh - skin_.px (1.0f));
        const float dm = skin_.px (6.0f);
        g.setColour (modColour);
        g.fillRoundedRectangle (juce::Rectangle<float> (std::min (pos, modPos), ly, std::max (std::abs (pos - modPos), skin_.px (2.0f)), lh), lh / 2.0f);
        g.fillEllipse (modPos - dm / 2.0f, ly + lh / 2.0f - dm / 2.0f, dm, dm);
    }
}
} // namespace nova
