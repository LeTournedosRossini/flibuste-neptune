#include "InsertOverlay.h"

using namespace neptune::core;
using neptune::core::theme::Token;

InsertOverlay::InsertOverlay (NeptuneProcessor& p, NeptuneProcessor::StripKind kind, int index, int slot, const theme::Palette& palette,
                              const i18n::Translator& tr, theme::KnobStyle knobStyle, std::function<void()> onClose)
    : processor_ (p), kind_ (kind), index_ (index), slot_ (slot), palette_ (palette), tr_ (tr), onClose_ (std::move (onClose))
{
    setWantsKeyboardFocus (true);
    handle_ = processor_.openInsert (kind, index, slot);
    if (handle_.instance == nullptr) return;
    bypassId_ = processor_.insertBypassParam (kind, index, slot);

    const bool isShaper = handle_.instance->type == "shaper", isCompressor = handle_.instance->type == "compressor", isEqualizer = handle_.instance->type == "equalizer", isDelay = handle_.instance->type == "delay", isChorus = handle_.instance->type == "chorus", isMicro = handle_.instance->type == "microdecale", isConv = handle_.instance->type == "convolution", isOverdrive = handle_.instance->type == "overdrive";
    if (isOverdrive)
    {
        NovaFournaiseEditor::Options opt;
        opt.moduleName = "fournaise";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        overdriveContent_ = std::make_unique<NovaFournaiseEditor> (*handle_.host, static_cast<const OverdriveModule*> (&handle_.instance->runner.module()), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*overdriveContent_);
    }
    else if (isConv)
    {
        NovaConvolutionEditor::Options opt;
        opt.moduleName = "convolution";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        convContent_ = std::make_unique<NovaConvolutionEditor> (*handle_.host, static_cast<ConvolutionModule*> (&handle_.instance->runner.module()), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*convContent_);
    }
    else if (isMicro)
    {
        NovaMicrodecaleEditor::Options opt;
        opt.moduleName = "microdecale";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        microContent_ = std::make_unique<NovaMicrodecaleEditor> (*handle_.host, static_cast<const MicrodecaleModule*> (&handle_.instance->runner.module()), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*microContent_);
    }
    else if (isChorus)
    {
        NovaChorusEditor::Options opt;
        opt.moduleName = "chorus";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        chorusContent_ = std::make_unique<NovaChorusEditor> (*handle_.host, static_cast<const ChorusModule*> (&handle_.instance->runner.module()), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*chorusContent_);
    }
    else if (isDelay)
    {
        NovaDelayEditor::Options opt;
        opt.moduleName = "delay";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        delayContent_ = std::make_unique<NovaDelayEditor> (*handle_.host, static_cast<const DelayModule*> (&handle_.instance->runner.module()), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*delayContent_);
    }
    else if (isEqualizer)
    {
        NovaEqualizerEditor::Options opt;
        opt.moduleName = "equalizer";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        equalizerContent_ = std::make_unique<NovaEqualizerEditor> (*handle_.host, static_cast<const EqualizerModule*> (&handle_.instance->runner.module()), palette_,
                                                                    knobStyle, tr_, opt);
        addAndMakeVisible (*equalizerContent_);
    }
    else if (isCompressor)
    {
        NovaCompressorEditor::Options opt;
        opt.moduleName = "compressor";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        compressorContent_ = std::make_unique<NovaCompressorEditor> (*handle_.host, static_cast<const CompressorModule*> (&handle_.instance->runner.module()), palette_,
                                                                      knobStyle, tr_, opt);
        addAndMakeVisible (*compressorContent_);
    }
    else if (isShaper)
    {
        NovaShaperEditor::Options opt;
        opt.moduleName = "shaper";
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        shaperContent_ = std::make_unique<NovaShaperEditor> (*handle_.host, static_cast<const ShaperModule*> (&handle_.instance->runner.module()), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*shaperContent_);
    }
    else
    {
        UtilityEditor::Options opt;
        opt.sampleRate = [this] { return processor_.getSampleRate() > 0.0 ? processor_.getSampleRate() : 48000.0; };
        content_ = std::make_unique<UtilityEditor> (*handle_.host, &handle_.instance->runner.module(), palette_, tr_, knobStyle, opt);
        addAndMakeVisible (*content_);
    }

    juce::String where = kind == NeptuneProcessor::StripKind::Track
                             ? juce::String::fromUTF8 (tr_.format ("insert.where_track", { std::to_string (index + 1) }).c_str())
                             : kind == NeptuneProcessor::StripKind::Bus
                                   ? juce::String::fromUTF8 (tr_.format ("insert.where_bus", { std::to_string (index + 1) }).c_str())
                                   : kind == NeptuneProcessor::StripKind::Parallel
                                         ? juce::String::fromUTF8 (tr_.format ("insert.where_parallel", { std::to_string (index + 1) }).c_str())
                                         : text ("insert.where_master");
    title_ = text (isOverdrive ? "module.overdrive" : isConv ? "module.convolution" : isMicro ? "module.microdecale" : isChorus ? "module.chorus" : isDelay ? "module.delay" : isEqualizer ? "module.equalizer" : isCompressor ? "module.compressor" : isShaper ? "module.shaper" : "module.utility") + "  -  " + where + "  -  "
           + juce::String::fromUTF8 (tr_.format ("insert.slot", { std::to_string (slot + 1) }).c_str());

    bypassButton_.setButtonText (text ("insert.bypass"));
    bypassButton_.setColour (juce::TextButton::buttonOnColourId, colour (Token::KnobHot));
    bypassButton_.onClick = [this]
    {
        const auto* spec = processor_.parameters().find (bypassId_);
        if (spec == nullptr) return;
        const bool on = processor_.parameters().get (bypassId_) < 0.5;
        processor_.uiBeginEdit (bypassId_);
        processor_.uiSetNormalised (bypassId_, spec->toNormalized (on ? 1.0 : 0.0));
        processor_.uiEndEdit (bypassId_);
    };
    addAndMakeVisible (bypassButton_);
    closeButton_.setButtonText (text ("insert.close"));
    closeButton_.onClick = [this] { if (onClose_) onClose_(); };
    addAndMakeVisible (closeButton_);
    startTimerHz (20);
}

juce::Colour InsertOverlay::colour (Token t) const
{
    const auto& c = palette_.color (t);
    return juce::Colour (c.r, c.g, c.b, static_cast<float> (c.a));
}

juce::String InsertOverlay::text (const char* key) const { return juce::String::fromUTF8 (std::string (tr_.tr (key)).c_str()); }

void InsertOverlay::timerCallback()
{
    bypassButton_.setToggleState (processor_.parameters().get (bypassId_) >= 0.5, juce::dontSendNotification);
    // l'emplacement a été vidé ou remplacé pendant que l'éditeur était ouvert (annulation, chargement d'un état) : on ferme
    if (processor_.insertType (kind_, index_, slot_).empty() && onClose_) onClose_();
}

void InsertOverlay::resized()
{
    const int w = juce::jmin (getWidth() - 40, 2400), h = juce::jmin (getHeight() - 40, 1700);   // toute la place disponible : les éditeurs NOVA grossissent avec la fenêtre
    panel_ = juce::Rectangle<int> (0, 0, w, h).withCentre (getLocalBounds().getCentre());
    auto r = panel_.reduced (10);
    auto head = r.removeFromTop (36);
    closeButton_.setBounds (head.removeFromRight (90).reduced (2));
    bypassButton_.setBounds (head.removeFromRight (110).reduced (2));
    r.removeFromTop (4);
    if (content_ != nullptr) content_->setBounds (r);
    if (shaperContent_ != nullptr) shaperContent_->setBounds (r);
    if (equalizerContent_ != nullptr) equalizerContent_->setBounds (r);
    if (delayContent_ != nullptr) delayContent_->setBounds (r);
    if (chorusContent_ != nullptr) chorusContent_->setBounds (r);
    if (microContent_ != nullptr) microContent_->setBounds (r);
    if (convContent_ != nullptr) convContent_->setBounds (r);
    if (overdriveContent_ != nullptr) overdriveContent_->setBounds (r);
    if (compressorContent_ != nullptr) compressorContent_->setBounds (r);
}

void InsertOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.6f));
    g.setColour (colour (Token::BgApp));
    g.fillRoundedRectangle (panel_.toFloat(), 8.0f);
    g.setColour (colour (Token::Accent));
    g.drawRoundedRectangle (panel_.toFloat(), 8.0f, 1.5f);
    g.setColour (colour (Token::Text));
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText (title_, panel_.getX() + 16, panel_.getY() + 10, panel_.getWidth() - 240, 36, juce::Justification::centredLeft);
}

void InsertOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! panel_.contains (e.getPosition()) && onClose_) onClose_();
}

bool InsertOverlay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose_) onClose_(); return true; }
    if (content_ != nullptr) return content_->keyPressed (k);
    if (shaperContent_ != nullptr) return shaperContent_->keyPressed (k);
    if (compressorContent_ != nullptr) return compressorContent_->keyPressed (k);
    if (equalizerContent_ != nullptr) return equalizerContent_->keyPressed (k);
    if (delayContent_ != nullptr) return delayContent_->keyPressed (k);
    if (chorusContent_ != nullptr) return chorusContent_->keyPressed (k);
    if (microContent_ != nullptr) return microContent_->keyPressed (k);
    if (convContent_ != nullptr) return convContent_->keyPressed (k);
    if (overdriveContent_ != nullptr) return overdriveContent_->keyPressed (k);
    return false;
}
