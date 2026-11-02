#include "InstrumentPanel.h"

#include "neptune/core/Mixer.h"

using namespace neptune::core;
using neptune::core::theme::Token;
namespace mx = neptune::core::mixer;

InstrumentPanel::InstrumentPanel (NeptuneProcessor& p, const theme::Palette& palette, const i18n::Translator& tr, theme::KnobStyle knobStyle)
    : processor_ (p), palette_ (palette), tr_ (tr)
{
    setWantsKeyboardFocus (true);
    track_ = processor_.selectedTrack();
    type_ = processor_.trackType (track_);
    auto& proc = processor_;
    const int track = track_;

    InstrumentLink common;
    common.beginGesture = [&proc] { proc.uiBeginGesture(); };
    common.endGesture = [&proc] { proc.uiEndGesture(); };
    common.panic = [&proc] { proc.uiPanic(); };
    common.noteOn = [&proc, track] (int key, float v) { proc.uiKeyOn (track, key, v); };
    common.noteOff = [&proc, track] (int key) { proc.uiKeyOff (track, key); };
    common.heldKeys = [&proc, track] (int word) { return proc.heldKeyWord (track, word); };
    common.track = track;
    common.pitchBend = [&proc, track] (float v) { proc.uiPitchBendTrack (track, v); };   // molettes de l'écran du clavier
    common.modWheel = [&proc, track] (float v) { proc.uiModWheelTrack (track, v); };
    common.outputs = { mx::samplerParam (track, mx::kGain), mx::samplerParam (track, mx::kTuneSemitones), mx::samplerParam (track, mx::kTuneCents) };

    auto options = [&proc] (const char* moduleName)
    {
        NovaSynthEditor::Options opt;
        opt.noBypass = true;
        opt.embedded = true;
        opt.stretchAuto = true;   // le panneau suit la largeur de la fenêtre de Neptune
        opt.moduleName = moduleName;
        opt.sampleRate = [&proc] { return proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0; };
        return opt;
    };
    if (type_ == TrackType::Synth)
    {
        SynthLink link;
        static_cast<InstrumentLink&> (link) = common;
        link.description = [&proc, track] { return proc.synthDescription (track); };
        link.edit = [&proc] (SynthDescription d) { proc.uiEditSynth (std::move (d), "Synth", "synth"); };
        auto opt = options ("analogsynth");
        opt.presetMenu = [this] (juce::Component* anchor) { if (synth_ != nullptr) synth_->showPresetMenu (anchor); };
        synth_ = std::make_unique<NovaSynthEditor> (proc, proc, std::move (link), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*synth_);
    }
    else if (type_ == TrackType::Fm)
    {
        FmLink link;
        static_cast<InstrumentLink&> (link) = common;
        link.description = [&proc, track] { return proc.fmDescription (track); };
        link.edit = [&proc] (fm::FmDescription d, std::string name, std::string mergeKey) { proc.uiEditFm (std::move (d), std::move (name), std::move (mergeKey)); };
        auto opt = options ("fm6op");
        opt.presetMenu = [this] (juce::Component* anchor) { if (fm_ != nullptr) fm_->showPresetMenu (anchor); };
        fm_ = std::make_unique<NovaFmEditor> (proc, proc, std::move (link), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*fm_);
    }
    else if (type_ == TrackType::Organ)
    {
        OrganLink link;
        link.description = [&proc, track] { return proc.organDescription (track); };
        link.edit = [&proc] (organ::OrganDescription d) { proc.uiEditOrgan (std::move (d), "Organ keyboards", "organ"); };
        link.undoRevision = [&proc] { return static_cast<std::uint32_t> (proc.undoManager().revision()); };
        link.panic = [&proc] { proc.uiPanic(); };
        link.noteOn = [&proc, track] (int channel, int key) { proc.uiKeyOn (track, key, 0.8f, channel); };
        link.noteOff = [&proc, track] (int channel, int key) { proc.uiKeyOff (track, key, channel); };
        link.heldWord = [&proc, track] (int word) { return proc.heldKeyWord (track, word); };
        link.heldCount = [&proc, track]
        {
            int n = 0;
            for (int w = 0; w < 2; ++w) for (auto bits = proc.heldKeyWord (track, w); bits != 0; bits &= bits - 1) ++n;
            return n;
        };
        link.outPeak = [&proc, track] (int c) { const auto m = proc.trackMeter (track); return c == 0 ? m.peakL : m.peakR; };
        link.midiLevel = [&proc] (int ch) { return proc.midiLevel (ch); };
        link.ccEnabled = [&proc] { return proc.ccEnabled(); };
        link.setCc = [&proc] (bool on) { proc.uiSetCcEnabled (on); };
        link.track = track;
        link.volumeId = probe::trackParam (track, probe::kVolume);
        link.gainId = mx::samplerParam (track, mx::kGain);
        auto opt = options ("organ");
        opt.presetMenu = [this] (juce::Component* anchor) { if (organ_ != nullptr) organ_->showPresetMenu (anchor); };
        organ_ = std::make_unique<NovaOrganEditor> (proc, proc, std::move (link), palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*organ_);
    }
    else if (type_ == TrackType::Sampler)
    {
        NovaSamplerEditor::Options opt;
        opt.noBypass = true;
        opt.embedded = true;
        opt.noPresets = true;
        opt.noAB = true;
        opt.moduleName = "sampler";
        opt.sampleRate = [&proc] { return proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0; };
        sampler_ = std::make_unique<NovaSamplerEditor> (proc, palette_, knobStyle, tr_, opt);
        addAndMakeVisible (*sampler_);
    }
}

void InstrumentPanel::resized()
{
    if (synth_ != nullptr) synth_->setBounds (getLocalBounds());
    if (fm_ != nullptr) fm_->setBounds (getLocalBounds());
    if (organ_ != nullptr) organ_->setBounds (getLocalBounds());
    if (sampler_ != nullptr) sampler_->setBounds (getLocalBounds());
}

bool InstrumentPanel::keyPressed (const juce::KeyPress& k)
{
    if (synth_ != nullptr) return synth_->keyPressed (k);
    if (fm_ != nullptr) return fm_->keyPressed (k);
    if (organ_ != nullptr) return organ_->keyPressed (k);
    if (sampler_ != nullptr) return sampler_->keyPressed (k);
    return false;
}
