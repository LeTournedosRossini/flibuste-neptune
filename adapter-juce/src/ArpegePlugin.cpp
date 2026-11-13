#include "ArpegePlugin.h"
#include "MotifLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace neptune::core;
namespace arp = neptune::core::arp;

namespace
{
constexpr int kDesignW = 1100, kDesignH = 780;

std::filesystem::path configDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    return std::filesystem::u8path (base.getChildFile ("Neptune").getFullPathName().toStdString());
}

theme::Palette makePalette (const settings::GlobalSettings& st)
{
    const auto id = theme::themeFromString (st.theme).value_or (theme::ThemeId::Dark);
    return theme::withContrast (theme::definition (id), st.contrast);
}
const theme::ThemeDefinition& themeOf (const settings::GlobalSettings& st)
{
    return theme::definition (theme::themeFromString (st.theme).value_or (theme::ThemeId::Dark));
}

const char* const kPatternNames[] = { "Montant", "Descendant", "Aller-retour", "Aller-retour (bouts doublés)", "Aléatoire", "Ordre de saisie", "Accord" };
const char* const kRateNames[] = { "1/4", "1/8", "1/8 triolet", "1/16", "1/16 triolet", "1/32", "1/32 triolet", "1/2" };
const char* const kVelNames[] = { "Jouée", "Fixe" };
}

namespace
{
struct Fourni { const char* name; const char* json; };
const Fourni kFournis[] = {
    { "Notes montant", R"({ "id": "n-montant", "name": "Notes montant", "length": [1,2], "x": { "arpege": { "mode": "notes" } }, "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["2"] },
        { "at": [2,8], "dur": [1,8], "parts": ["3"] }, { "at": [3,8], "dur": [1,8], "parts": ["4"] } ] })" },
    { "Notes descendant", R"({ "id": "n-descendant", "name": "Notes descendant", "length": [1,2], "x": { "arpege": { "mode": "notes" } }, "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["4"] }, { "at": [1,8], "dur": [1,8], "parts": ["3"] },
        { "at": [2,8], "dur": [1,8], "parts": ["2"] }, { "at": [3,8], "dur": [1,8], "parts": ["1"] } ] })" },
    { "Notes aller-retour", R"({ "id": "n-allerretour", "name": "Notes aller-retour", "length": [1,1], "x": { "arpege": { "mode": "notes" } }, "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["2"] }, { "at": [2,8], "dur": [1,8], "parts": ["3"] }, { "at": [3,8], "dur": [1,8], "parts": ["4"] },
        { "at": [4,8], "dur": [1,8], "parts": ["4"] }, { "at": [5,8], "dur": [1,8], "parts": ["3"] }, { "at": [6,8], "dur": [1,8], "parts": ["2"] }, { "at": [7,8], "dur": [1,8], "parts": ["1"] } ] })" },
    { "Notes en triolets", R"({ "id": "n-triolets", "name": "Notes en triolets", "length": [1,4], "x": { "arpege": { "mode": "notes" } }, "hits": [
        { "at": [0,12], "dur": [1,12], "parts": ["1"] }, { "at": [1,12], "dur": [1,12], "parts": ["2"] }, { "at": [2,12], "dur": [1,12], "parts": ["3"] } ] })" },
    { "Accord : arpège montant", R"({ "id": "montant", "name": "Accord : arpège montant", "length": [1,2], "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["3"] },
        { "at": [2,8], "dur": [1,8], "parts": ["5"] }, { "at": [3,8], "dur": [1,8], "parts": ["5+1"] } ] })" },
    { "Accord : Alberti", R"({ "id": "alberti", "name": "Accord : Alberti", "length": [1,2], "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["5"] },
        { "at": [2,8], "dur": [1,8], "parts": ["3"] }, { "at": [3,8], "dur": [1,8], "parts": ["5"] } ] })" },
    { "Accord : triolets", R"({ "id": "triolets", "name": "Accord : triolets", "length": [1,4], "hits": [
        { "at": [0,12], "dur": [1,12], "parts": ["1"] }, { "at": [1,12], "dur": [1,12], "parts": ["3"] }, { "at": [2,12], "dur": [1,12], "parts": ["5"] } ] })" },
    { "Accord : ballade", R"({ "id": "ballade", "name": "Accord : ballade", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["B","C"] }, { "at": [1,4], "dur": [1,4], "parts": ["C"] },
        { "at": [2,4], "dur": [1,4], "parts": ["C"] }, { "at": [3,4], "dur": [1,4], "parts": ["C"] } ] })" },
    { "Accord : valse", R"({ "id": "valse", "name": "Accord : valse", "length": [3,4], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["B"] }, { "at": [1,4], "dur": [1,4], "parts": ["C"] }, { "at": [2,4], "dur": [1,4], "parts": ["C"] } ] })" },
    { "Accord : basse et contretemps", R"({ "id": "contretemps", "name": "Accord : basse et contretemps", "length": [1,1], "hits": [
        { "at": [0,8], "dur": [1,4], "parts": ["B"] }, { "at": [1,8], "dur": [1,8], "parts": ["C"] }, { "at": [3,8], "dur": [1,8], "parts": ["C"] },
        { "at": [4,8], "dur": [1,4], "parts": ["B"] }, { "at": [5,8], "dur": [1,8], "parts": ["C"] }, { "at": [7,8], "dur": [1,8], "parts": ["C"] } ] })" },
    { "Accord : tenu", R"({ "id": "tenu", "name": "Accord : tenu", "length": [1,1], "hits": [ { "at": [0,1], "dur": [1,1], "parts": ["C"] } ] })" },
    // ---- motifs d'essai (emplacements 12 à 15 d'un Arpège neuf) : voir docs/exemples-arpege/LISEZMOI.md ; l'exemple de Carte les appelle par leur numéro
    { "Essai 1 : notes, vélocité et durée par coup", R"({ "id": "essai1", "name": "Essai 1 : notes, vélocité et durée par coup", "length": [1,1], "x": { "arpege": { "mode": "notes" } }, "hits": [
        { "at": [0,8], "dur": [1,8], "parts": ["1"], "vel": 50, "gate": 0.3 }, { "at": [1,8], "dur": [1,8], "parts": ["2"], "vel": 65, "gate": 0.9 },
        { "at": [2,8], "dur": [1,8], "parts": ["3"], "vel": 80, "gate": 0.3, "accent": true }, { "at": [3,8], "dur": [1,8], "parts": ["4"], "vel": 95, "gate": 0.9 },
        { "at": [4,8], "dur": [1,8], "parts": ["3"], "vel": 110, "gate": 0.3 }, { "at": [5,8], "dur": [1,8], "parts": ["2"], "vel": 95, "gate": 0.9 },
        { "at": [6,8], "dur": [1,8], "parts": ["3"], "vel": 80, "gate": 0.3, "accent": true }, { "at": [7,8], "dur": [1,8], "parts": ["1+1"], "vel": 120, "gate": 1.0 } ] })" },
    { "Essai 2 : accord, variante tirée au sort", R"({ "id": "essai2", "name": "Essai 2 : accord, variante tirée au sort", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,8], "dur": [1,8], "parts": ["3"] }, { "at": [2,8], "dur": [1,8], "parts": ["5"] }, { "at": [3,8], "dur": [1,8], "parts": ["7"] },
        { "at": [4,8], "dur": [1,8], "parts": ["5"] }, { "at": [5,8], "dur": [1,8], "parts": ["3"] }, { "at": [6,8], "dur": [1,8], "parts": ["5"] }, { "at": [7,8], "dur": [1,8], "parts": ["9"] } ],
        "alts": [ { "id": "reprise", "chance": 0.5, "from": [1,2], "hits": [ { "at": [4,8], "dur": [1,8], "parts": ["5+1"], "accent": true }, { "at": [5,8], "dur": [1,8], "parts": ["3+1"] },
                                                                      { "at": [6,8], "dur": [1,8], "parts": ["1+1"] }, { "at": [7,8], "dur": [1,8], "parts": ["B"], "vel": 110 } ] } ] })" },
    { "Essai 3 : ballade, basse et accord", R"({ "id": "essai3", "name": "Essai 3 : ballade, basse et accord", "length": [1,1], "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["B","C"], "vel": 100, "accent": true }, { "at": [1,4], "dur": [1,4], "parts": ["C"], "vel": 70, "gate": 0.6 },
        { "at": [2,4], "dur": [1,4], "parts": ["B","C"], "vel": 85 }, { "at": [3,4], "dur": [1,4], "parts": ["C"], "vel": 70, "gate": 0.4 } ] })" },
    { "Essai 4 : deux mesures, triolets", R"({ "id": "essai4", "name": "Essai 4 : deux mesures, triolets", "length": [2,1], "bars": 2, "hits": [
        { "at": [0,4], "dur": [1,4], "parts": ["1"] }, { "at": [1,4], "dur": [1,4], "parts": ["5"] }, { "at": [2,4], "dur": [1,4], "parts": ["3"] }, { "at": [3,4], "dur": [1,4], "parts": ["5"] },
        { "at": [1,1], "dur": [1,12], "parts": ["1"] }, { "at": [13,12], "dur": [1,12], "parts": ["3"] }, { "at": [14,12], "dur": [1,12], "parts": ["5"] },
        { "at": [3,2], "dur": [1,2], "parts": ["B","C"], "accent": true } ] })" },
};
constexpr int kNumFournis = (int) (sizeof (kFournis) / sizeof (kFournis[0]));
}

std::vector<neptune::core::pattern::Pattern> ArpegeProcessor::builtinPatterns()
{
    std::vector<pattern::Pattern> v;
    for (const auto& f : kFournis)
    {
        const auto r = pattern::fromJson (f.json);
        if (r.ok && ! r.patterns.empty()) v.push_back (r.patterns[0]);
    }
    return v;
}

const std::array<double, 8>& ArpegeProcessor::rateBeats()
{
    static const std::array<double, 8> v = { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 2.0 };
    return v;
}

// ================================================================================================ processeur

// Choix dont le texte reçu de l'hôte est relu en UTF-8 : le wrapper CLAP passe son `const char*` à juce::String, qui le lit en Latin-1
// (« Alterné » arrive en « AlternÃ© »), et le choix n'est alors pas retrouvé.
class Utf8Choice : public juce::AudioParameterChoice
{
public:
    using juce::AudioParameterChoice::AudioParameterChoice;
    float getValueForText (const juce::String& text) const override
    {
        auto index = choices.indexOf (text);
        if (index < 0)
        {
            std::string bytes;
            bool latin1 = true;
            for (auto c : text) { if (c > 255) { latin1 = false; break; } bytes.push_back (static_cast<char> (c)); }
            if (latin1) index = choices.indexOf (juce::String::fromUTF8 (bytes.c_str()));
        }
        return convertTo0to1 (static_cast<float> (index < 0 ? 0 : index));
    }
};


ArpegeProcessor::ArpegeProcessor() : juce::AudioProcessor (BusesProperties())
{
    juce::StringArray patterns, rates, vels;
    for (auto* s : kPatternNames) patterns.add (juce::String::fromUTF8 (s));
    for (auto* s : kRateNames) rates.add (juce::String::fromUTF8 (s));
    for (auto* s : kVelNames) vels.add (juce::String::fromUTF8 (s));
    addParameter (pattern = new Utf8Choice (juce::ParameterID { "pattern", 1 }, "Motif", patterns, 0));
    addParameter (octaves = new juce::AudioParameterInt (juce::ParameterID { "octaves", 1 }, "Octaves", 1, 4, 1));
    addParameter (rate = new Utf8Choice (juce::ParameterID { "rate", 1 }, "Pas", rates, 3));
    addParameter (gate = new juce::AudioParameterFloat (juce::ParameterID { "gate", 1 }, "Duree de note", juce::NormalisableRange<float> (5.0f, 100.0f, 1.0f), 50.0f));
    addParameter (swing = new juce::AudioParameterFloat (juce::ParameterID { "swing", 1 }, "Swing", juce::NormalisableRange<float> (0.0f, 75.0f, 1.0f), 0.0f));
    addParameter (hold = new juce::AudioParameterBool (juce::ParameterID { "hold", 1 }, "Maintien", false));
    addParameter (velMode = new Utf8Choice (juce::ParameterID { "velMode", 1 }, "Velocite", vels, 0));
    addParameter (velocity = new juce::AudioParameterInt (juce::ParameterID { "velocity", 1 }, "Velocite fixe", 1, 127, 100));
    addParameter (accentEvery = new juce::AudioParameterInt (juce::ParameterID { "accentEvery", 1 }, "Accent un pas sur", 0, 16, 0));
    addParameter (accentBoost = new juce::AudioParameterInt (juce::ParameterID { "accentBoost", 1 }, "Force de l'accent", 0, 60, 20));
    addParameter (transpose = new juce::AudioParameterInt (juce::ParameterID { "transpose", 1 }, "Transposition", -24, 24, 0));

    addParameter (slot = new juce::AudioParameterInt (juce::ParameterID { "slot", 1 }, "Motif", 0, kSlots, 0, juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v == 0 ? juce::String::fromUTF8 ("aucun") : juce::String (v); })));
    addParameter (absentMode = new Utf8Choice (juce::ParameterID { "absent", 1 }, "Degre absent", juce::StringArray { "Sauter", "Jouer", "Gamme" }, 0));
    addParameter (source = new Utf8Choice (juce::ParameterID { "source", 1 }, "Source", juce::StringArray { "Notes tenues", "Carte" }, 0));
    addParameter (carteChannel = new juce::AudioParameterInt (juce::ParameterID { "carteChannel", 1 }, "Canal de Carte", 1, chordmap::kChannels, 1));
    addParameter (baseOctave = new juce::AudioParameterInt (juce::ParameterID { "baseOctave", 1 }, "Octave de base", 1, 6, 3));
    addParameter (approachMode = new Utf8Choice (juce::ParameterID { "approach", 1 }, "Approche", juce::StringArray { "Non", "Dominante", "Triton", "Chromatique", juce::String::fromUTF8 ("Parallèle") }, 0));
    addParameter (strumMode = new Utf8Choice (juce::ParameterID { "strum", 1 }, "Grattage", juce::StringArray { "Non", "Coup bas", juce::String::fromUTF8 ("Alterné") }, 0));
    addParameter (strumDelay = new juce::AudioParameterInt (juce::ParameterID { "strumDelay", 1 }, "Delai du grattage", 0, 60, 0, juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v == 0 ? juce::String::fromUTF8 ("auto") : juce::String (v) + " ms"; })));
    addParameter (ledVoices = new juce::AudioParameterBool (juce::ParameterID { "ledVoices", 1 }, "Voix conduites", false));

    {
        const auto b = builtinPatterns();
        for (size_t i = 0; i < b.size() && i < (size_t) kSlots; ++i) slots_[i] = std::make_shared<const pattern::Pattern> (b[i]);
    }
    std::vector<std::string> warnings;
    settings_ = settings::loadFile (configDirectory() / "settings.json", &warnings);
    in_.reserve (1024);
    out_.resize (4096);
    env_ = std::make_unique<arp::ChordMapEnvironment> (reader_, audioSlots_);
    ticker_.startTimer (500);
}

ArpegeProcessor::~ArpegeProcessor() { ticker_.stopTimer(); }

void ArpegeProcessor::tick()
{
    reader_.setChannel (carteChannel->get());
    reader_.poll();
}

juce::String ArpegeProcessor::carteChordText() const
{
    const auto v = carteChord_.load();
    if (v == 0) return {};
    chords::Chord c;
    c.root = static_cast<int> (v & 0xFF);
    c.bass = (v >> 8 & 0xFF) <= 11 ? static_cast<int> (v >> 8 & 0xFF) : -1;
    c.rel = static_cast<std::uint32_t> (v >> 16 & 0xFFFFFF);
    return juce::String::fromUTF8 (chords::formatChord (c, chords::Naming::Letters).c_str());
}

arp::Params ArpegeProcessor::currentParams() const
{
    arp::Params p;
    p.pattern = static_cast<arp::Pattern> (pattern->getIndex());
    p.octaves = octaves->get();
    p.stepBeats = rateBeats()[static_cast<size_t> (rate->getIndex())];
    p.gate = gate->get() / 100.0;
    p.swing = swing->get() / 100.0;
    p.hold = hold->get();
    p.fixedVelocity = velMode->getIndex() == 1;
    p.velocity = velocity->get();
    p.accentEvery = accentEvery->get();
    p.accentBoost = accentBoost->get();
    p.transpose = transpose->get();
    return p;
}

ArpegeProcessor::PreviewResult ArpegeProcessor::makePreview() const
{
    PreviewResult r;
    if (source->getIndex() != 1)
    {
        r.problem = juce::String::fromUTF8 ("L'aperçu montre ce que joue Arpège avec la carte : choisissez « Source : Carte ».");
        return r;
    }
    if (! reader_.connected())
    {
        r.problem = juce::String::fromUTF8 ("Carte n'est pas connectée : chargez le plugin Carte sur le même canal (le canal de Carte d'Arpège doit être le sien).");
        return r;
    }
    arp::ChordMapEnvironment::Slots slots;
    {
        const juce::SpinLock::ScopedLockType l (slotLock_);
        slots = slots_;
    }
    arp::ChordMapEnvironment env (reader_, slots);
    pattern::Pattern pat;
    arp::PreviewSettings s;
    s.params = currentParams();
    const int sl = slot->get();
    if (sl > 0 && slots[static_cast<size_t> (sl) - 1] != nullptr)
    {
        pat = *slots[static_cast<size_t> (sl) - 1];
        s.pattern = &pat;
    }
    const int am = absentMode->getIndex();
    s.absent = am == 2 ? pattern::Absent::Scale : (am == 1 ? pattern::Absent::Play : pattern::Absent::Skip);
    s.env = &env;
    s.baseNote = 12 * (baseOctave->get() + 1);
    s.ledVoices = ledVoices->get();
    s.strum.mode = static_cast<arp::StrumParams::Mode> (strumMode->getIndex());
    s.strum.delayMs = strumDelay->get();
    s.approach.kind = static_cast<arp::ApproachParams::Kind> (approachMode->getIndex());
    r.preview = arp::renderPreview (s);
    if (r.preview.chords.empty()) r.problem = juce::String::fromUTF8 ("Aucun accord dans Carte : posez des accords sur sa piste d'accords.");
    return r;
}

std::uint64_t ArpegeProcessor::previewKey() const
{
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h] (std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const float v = r->getValue();
            std::uint32_t bits = 0;
            std::memcpy (&bits, &v, sizeof bits);
            mix (bits);
        }
    mix (static_cast<std::uint64_t> (slotsVersion_.load()));
    mix (reader_.sequence());
    mix (reader_.connected() ? 1u : 0u);
    return h;
}

bool ArpegeProcessor::slotUsed (int i) const
{
    const juce::SpinLock::ScopedLockType l (slotLock_);
    return i >= 0 && i < kSlots && slots_[(size_t) i] != nullptr;
}

pattern::Pattern ArpegeProcessor::slotPattern (int i) const
{
    const juce::SpinLock::ScopedLockType l (slotLock_);
    if (i >= 0 && i < kSlots && slots_[(size_t) i] != nullptr) return *slots_[(size_t) i];
    pattern::Pattern p;
    p.id = "motif" + std::to_string (i + 1);
    pattern::setNotesMode (p, true);   // un motif neuf est un vrai arpégiateur (rangs des notes tenues)
    return p;
}

void ArpegeProcessor::setSlotPattern (int i, pattern::Pattern p)
{
    if (i < 0 || i >= kSlots) return;
    if (p.id.empty()) p.id = "motif" + std::to_string (i + 1);
    auto np = std::make_shared<const pattern::Pattern> (std::move (p));
    {
        const juce::SpinLock::ScopedLockType l (slotLock_);
        if (slots_[(size_t) i] != nullptr) retired_.push_back (slots_[(size_t) i]);
        if (retired_.size() > 64) retired_.erase (retired_.begin(), retired_.begin() + 32);
        slots_[(size_t) i] = std::move (np);
    }
    slotsVersion_.fetch_add (1);
}

void ArpegeProcessor::commitSlot (int i, const pattern::Pattern& before, const pattern::Pattern& after, const char* name)
{
    undo_.perform (std::make_unique<SnapshotCommand<pattern::Pattern>> (before, after, [this, i] (const pattern::Pattern& p) { setSlotPattern (i, p); }, name));
}

void ArpegeProcessor::clearSlot (int i)
{
    if (i < 0 || i >= kSlots) return;
    {
        const juce::SpinLock::ScopedLockType l (slotLock_);
        if (slots_[(size_t) i] != nullptr) retired_.push_back (slots_[(size_t) i]);
        slots_[(size_t) i] = nullptr;
    }
    slotsVersion_.fetch_add (1);
}

void ArpegeProcessor::prepareToPlay (double sampleRate, int)
{
    arp_.setSampleRate (sampleRate);
    arp_.reset();
}

void ArpegeProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    audio.clear();
    arp_.setParams (currentParams());

    {
        // motif choisi : on ne change de motif que si le verrou est libre (sinon au bloc suivant)
        const int s = slot->get(), ver = slotsVersion_.load();
        if (s != activeSlot_ || ver != activeVersion_)
        {
            const juce::SpinLock::ScopedTryLockType l (slotLock_);
            if (l.isLocked())
            {
                audioSlots_ = slots_;
                activeSlot_ = s;
                activeVersion_ = ver;
            }
        }
        activePattern_ = (s > 0 && activeSlot_ == s) ? audioSlots_[(size_t) s - 1].get() : nullptr;
        const int am = absentMode->getIndex();
        arp_.setPattern (activePattern_, am == 2 ? pattern::Absent::Scale : (am == 1 ? pattern::Absent::Play : pattern::Absent::Skip));
        arp_.setEnvironment (source->getIndex() == 1 ? env_.get() : nullptr);
        arp_.setBaseNote (12 * (baseOctave->get() + 1));
        arp_.setVoiceLeading (ledVoices->get());
        {
            arp::StrumParams sp;
            sp.mode = static_cast<arp::StrumParams::Mode> (strumMode->getIndex());
            sp.delayMs = strumDelay->get();
            arp_.setStrum (sp);
            arp::ApproachParams ap;
            ap.kind = static_cast<arp::ApproachParams::Kind> (approachMode->getIndex());
            arp_.setApproach (ap);
        }
        carteHasPatterns_.store (source->getIndex() == 1 && reader_.connected() && env_->hasPatterns());
    }

    arp::Transport tr;
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
        {
            tr.playing = pos->getIsPlaying();
            if (const auto ppq = pos->getPpqPosition())
            {
                tr.hasPosition = true;
                tr.ppq = *ppq;
            }
            if (const auto bpm = pos->getBpm()) tr.bpm = *bpm;
            if (const auto ts = pos->getTimeSignature())
                if (ts->denominator > 0) tr.beatsPerBar = 4.0 * ts->numerator / ts->denominator;
            if (const auto bar = pos->getPpqPositionOfLastBarStart()) tr.barStartPpq = *bar;
        }
    playing_.store (tr.playing);
    if (source->getIndex() == 1 && reader_.connected())
    {
        arp::Harmony hm;
        const double t = tr.hasPosition ? tr.ppq : 0.0;
        if (env_->harmonyAt (t, hm)) carteChord_.store (static_cast<std::uint64_t> (hm.rootPc) | static_cast<std::uint64_t> (hm.bassPc < 0 ? 255 : hm.bassPc) << 8 | static_cast<std::uint64_t> (hm.rel) << 16);
        else carteChord_.store (0);
        carteState_.store (carteChord_.load() != 0 ? 1 : 0);
    }
    else
    {
        carteChord_.store (0);
        carteState_.store (-1);
    }

    in_.clear();
    scratch_.clear();
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn() || m.isNoteOff())
            in_.push_back ({ meta.samplePosition, m.isNoteOn(), static_cast<std::uint8_t> (m.getNoteNumber()), static_cast<std::uint8_t> (m.getVelocity()) });
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            arp_.reset();
        else
            scratch_.addEvent (m, meta.samplePosition);   // le reste passe sans changement
    }
    std::stable_sort (in_.begin(), in_.end(), [] (const arp::NoteIn& a, const arp::NoteIn& b) { return a.offset < b.offset; });

    const int n = arp_.process (in_.data(), static_cast<int> (in_.size()), tr, audio.getNumSamples() > 0 ? audio.getNumSamples() : 0, out_.data(), static_cast<int> (out_.size()));
    for (int i = 0; i < n; ++i)
    {
        const auto& e = out_[static_cast<size_t> (i)];
        scratch_.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, static_cast<juce::uint8> (e.velocity))
                                : juce::MidiMessage::noteOff (1, e.note), e.offset);
    }
    midi.swapWith (scratch_);
    held_.store (arp_.heldCount());
}

void ArpegeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("ARPEGE");
    xml.setAttribute ("version", 1);
    xml.setAttribute ("sections", sectionsOpen.load());
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p)) xml.setAttribute (r->getParameterID(), r->convertFrom0to1 (r->getValue()));
    for (int i = 0; i < kSlots; ++i)
        if (slotUsed (i))
        {
            auto* e = xml.createNewChildElement ("SLOT");
            e->setAttribute ("n", i + 1);
            e->addTextElement (juce::String (pattern::toJson ({ slotPattern (i) }, "arpege")));
        }
    copyXmlToBinary (xml, dest);
}

void ArpegeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("ARPEGE") && xml->getIntAttribute ("version") == 1)
            for (auto* p : getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (xml->hasAttribute (r->getParameterID())) r->setValueNotifyingHost (r->convertTo0to1 (static_cast<float> (xml->getDoubleAttribute (r->getParameterID()))));
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName ("ARPEGE"))
        {
            if (xml->hasAttribute ("sections")) sectionsOpen.store (xml->getIntAttribute ("sections") & 0x1F);
            for (int i = 0; i < kSlots; ++i) clearSlot (i);
            undo_.clear();
            for (auto* e : xml->getChildWithTagNameIterator ("SLOT"))
            {
                const int n = e->getIntAttribute ("n");
                const auto r = pattern::fromJson (e->getAllSubText().toStdString());
                if (n >= 1 && n <= kSlots && r.ok && ! r.patterns.empty()) setSlotPattern (n - 1, r.patterns[0]);
            }
        }
    // L'hôte doit relire les valeurs : sans cette demande, clap-validator (state-reproducibility) voit les valeurs changer « en douce ».
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withProgramChanged (true));
}

juce::AudioProcessorEditor* ArpegeProcessor::createEditor() { return new ArpegeEditor (*this); }

// ================================================================================================ interface

// Pastille liée à un paramètre : glisser = régler, clic = changer de choix (ou basculer), double-clic = saisir la valeur.
class ArpegeEditor::ParamPill : public nova::Pill
{
public:
    ParamPill (const nova::Skin& s, juce::RangedAudioParameter& p, juce::String label)
        : nova::Pill (s, nova::Pill::Look::Small), param_ (p), label_ (std::move (label))
    {
        refresh();
    }

    void refresh() override
    {
        const auto text = label_ + "  " + param_.getCurrentValueAsText();
        if (text != shown_) { shown_ = text; setText (text); }
        if (dynamic_cast<juce::AudioParameterBool*> (&param_) != nullptr) setOn (param_.getValue() > 0.5f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartY_ = e.position.y;
        startValue_ = param_.getValue();
        moved_ = false;
        if (! isChoiceLike()) param_.beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (isChoiceLike()) return;
        const float dy = dragStartY_ - e.position.y;
        if (std::abs (dy) > 2.0f) moved_ = true;
        const float fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
        param_.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, startValue_ + fine * dy / 160.0f));
        refresh();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isChoiceLike())
        {
            if (! getLocalBounds().contains (e.getPosition())) return;
            const int n = param_.getNumSteps();
            const int cur = juce::roundToInt (param_.getValue() * (float) (n - 1));
            const int next = e.mods.isRightButtonDown() || e.mods.isShiftDown() ? (cur + n - 1) % n : (cur + 1) % n;
            param_.beginChangeGesture();
            param_.setValueNotifyingHost ((float) next / (float) (n - 1));
            param_.endChangeGesture();
            refresh();
        }
        else
            param_.endChangeGesture();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (isChoiceLike()) return;
        auto* box = new juce::AlertWindow (label_, juce::String::fromUTF8 ("Valeur :"), juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("v", param_.getText (param_.getValue(), 16).retainCharacters ("-0123456789."));
        box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton ("Annuler", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ParamPill> self (this);
        box->enterModalState (true, juce::ModalCallbackFunction::create ([self, box] (int r)
        {
            if (r == 1 && self != nullptr)
            {
                const float v = box->getTextEditorContents ("v").getFloatValue();
                self->param_.beginChangeGesture();
                self->param_.setValueNotifyingHost (self->param_.convertTo0to1 (v));
                self->param_.endChangeGesture();
                self->refresh();
            }
        }), true);
    }

private:
    bool isChoiceLike() const
    {
        return dynamic_cast<juce::AudioParameterChoice*> (&param_) != nullptr || dynamic_cast<juce::AudioParameterBool*> (&param_) != nullptr;
    }

    juce::RangedAudioParameter& param_;
    juce::String label_, shown_;
    float dragStartY_ = 0.0f, startValue_ = 0.0f;
    bool moved_ = false;
};

// Matrice d'un motif : colonnes = cases de la grille sur la longueur du motif, lignes = degrés de l'accord (de bas en haut : basse, 1 à 7, puis les mêmes une
// octave plus haut). Un clic allume ou éteint une case (glisser : peindre) ; la bande du bas pose l'accent d'un coup.
namespace
{
using pattern::Frac;
using pattern::makeFrac;
const char* const kRowPartsChord[] = { "5+1", "4+1", "3+1", "2+1", "1+1", "7", "6", "5", "4", "3", "2", "1", "B" };
const char* const kRowPartsNotes[] = { "5+1", "4+1", "3+1", "2+1", "1+1", "8", "7", "6", "5", "4", "3", "2", "1" };
constexpr int kNumRows = 13;
const Frac kGridFracs[] = { { 1, 8 }, { 1, 12 }, { 1, 16 }, { 1, 24 }, { 1, 32 } };
const char* const kGridNames[] = { "1/8", "1/12 (triolet)", "1/16", "1/24 (triolet)", "1/32" };
constexpr int kNumGrids = 5;
const Frac kLengths[] = { { 1, 4 }, { 1, 2 }, { 3, 4 }, { 1, 1 }, { 3, 2 }, { 2, 1 }, { 3, 1 }, { 4, 1 } };
constexpr int kNumLengths = 8;

juce::String rowLabel (const juce::String& part)
{
    if (part == "B") return "B";
    return part.upToFirstOccurrenceOf ("+", false, false) + (part.contains ("+") ? juce::String::fromUTF8 ("\xE2\x86\x91") : juce::String());
}
}

class ArpegeEditor::MatrixView : public juce::Component
{
public:
    MatrixView (ArpegeProcessor& p, const nova::Skin& s) : proc_ (p), skin_ (s) { setWantsKeyboardFocus (true); }

    int grid = 2;
    std::function<void()> onChange;
    int alt = -1;         // -1 : le motif ; sinon l'indice de la variante qu'on dessine
    int altIndex (const pattern::Pattern& p) const { return (alt >= 0 && alt < (int) p.alts.size()) ? alt : -1; }
    bool notes = false;   // mode du motif affiché (lignes : rangs des notes tenues, ou degrés de l'accord)
    const char* rowPart (int r) const { return notes ? kRowPartsNotes[r] : kRowPartsChord[r]; }

    int slotIndex() const { return proc_.slot->get() - 1; }
    int columns() const
    {
        const auto p = proc_.slotPattern (slotIndex());
        const auto g = kGridFracs[grid];
        const auto len = (alt >= 0 && alt < (int) p.alts.size()) ? p.barLength() : p.length;   // une variante se dessine sur une mesure du motif
        return std::clamp ((int) ((len.n * g.d + len.d * g.n - 1) / (len.d * g.n)), 1, 96);
    }

    void paint (juce::Graphics& g) override
    {
        using theme::Token;
        const auto area = getLocalBounds().toFloat();
        skin_.fill (g, Token::PanelBg, area, skin_.px (8.0f));
        if (slotIndex() < 0)
        {
            g.setColour (skin_.colour (Token::TextDim));
            g.setFont (skin_.font (14.0f));
            g.drawFittedText (juce::String::fromUTF8 ("Motifs automatiques. Choisissez un emplacement de motif (1 à 16) pour dessiner le vôtre ici : en mode notes (un vrai arpégiateur, rangs des notes tenues) ou en mode accord (degrés des accords)."), getLocalBounds().reduced (skin_.ipx (20.0f)), juce::Justification::centred, 2);
            return;
        }
        const auto pat = proc_.slotPattern (slotIndex());
        notes = pat.notesMode;
        const auto gr = kGridFracs[grid];
        const int cols = columns();
        const auto geo = geometry (cols);
        // lignes
        for (int r = 0; r < kNumRows; ++r)
        {
            const auto row = juce::Rectangle<float> (geo.x0, geo.y0 + r * geo.rowH, geo.w, geo.rowH);
            if (r % 2 == 0) { g.setColour (skin_.colour (Token::BgCard, 0.45f)); g.fillRect (row); }
            g.setColour (skin_.colour (Token::TextDim));
            g.setFont (skin_.font (12.0f));
            g.drawText (rowLabel (rowPart (r)), juce::Rectangle<float> (0.0f, row.getY(), geo.x0 - skin_.px (6.0f), row.getHeight()), juce::Justification::centredRight);
        }
        // traits de temps et de mesure
        for (int c = 0; c <= cols; ++c)
        {
            const bool beat = (gr.n * c * 4) % gr.d == 0;
            const bool bar = (gr.n * c) % (pat.barLength().n * 1) == 0 && pat.barLength().d == gr.d ? false : false;
            (void) bar;
            if (! beat) continue;
            g.setColour (skin_.colour (Token::Border, 0.8f));
            const float x = geo.x0 + c * geo.cellW;
            g.drawLine (x, geo.y0, x, geo.y0 + geo.rowH * kNumRows, skin_.px (1.0f));
        }
        const int a = altIndex (pat);
        if (a >= 0)
        {
            // le motif de base, en filigrane, pour se repérer ; la variante ne remplace que la fin de la mesure (à partir de son début)
            for (const auto& h : pat.hits)
            {
                const double colF = h.at.value() / gr.value();
                if (std::abs (colF - std::round (colF)) > 1e-6 || (int) std::round (colF) >= cols) continue;
                for (const auto& part : h.parts)
                    for (int r = 0; r < kNumRows; ++r)
                        if (juce::String (part.text) == rowPart (r))
                            { g.setColour (skin_.colour (Token::TextDim2, 0.28f)); g.fillRoundedRectangle (juce::Rectangle<float> (geo.x0 + (float) std::round (colF) * geo.cellW, geo.y0 + r * geo.rowH, std::max (1.0f, (float) (h.dur.value() / gr.value())) * geo.cellW, geo.rowH).reduced (skin_.px (2.0f)), skin_.px (4.0f)); }
            }
            const float fx = geo.x0 + (float) (pat.alts[(size_t) a].from.value() / gr.value()) * geo.cellW;
            g.setColour (skin_.colour (Token::BgApp, 0.55f));
            g.fillRect (geo.x0, geo.y0, fx - geo.x0, geo.rowH * kNumRows);        // avant le début : rien ne se pose
            g.setColour (skin_.colour (Token::Pink));
            g.drawLine (fx, geo.y0, fx, geo.y0 + geo.rowH * kNumRows, skin_.px (2.0f));
        }
        const auto& drawn = a >= 0 ? pat.alts[(size_t) a].hits : pat.hits;
        // cases allumées (la longueur du coup est dessinée)
        for (const auto& h : drawn)
        {
            const double colF = (h.at.value()) / gr.value();
            if (std::abs (colF - std::round (colF)) > 1e-6) continue;   // coup hors de cette grille : visible dans une autre
            const int c = (int) std::round (colF);
            const float widthCells = (float) std::max (1.0, h.dur.value() / gr.value());
            for (const auto& part : h.parts)
            {
                juce::String t (part.text);
                if (t.startsWithChar ('L')) t = t.substring (1);
                for (int r = 0; r < kNumRows; ++r)
                    if (t == rowPart (r))
                    {
                        const auto cell = juce::Rectangle<float> (geo.x0 + c * geo.cellW, geo.y0 + r * geo.rowH, widthCells * geo.cellW, geo.rowH).reduced (skin_.px (1.5f));
                        skin_.fillActive (g, cell, skin_.px (4.0f), 2.0f);
                        g.setColour (skin_.colour (h.accent ? Token::Pink : Token::Accent));
                        g.drawRoundedRectangle (cell, skin_.px (4.0f), skin_.px (1.5f));
                    }
            }
            // bande d'accent
            const auto acc = juce::Rectangle<float> (geo.x0 + c * geo.cellW, geo.accentY, geo.cellW, geo.accentH).reduced (skin_.px (2.0f));
            g.setColour (skin_.colour (h.accent ? Token::Pink : Token::TextDim2, h.accent ? 1.0f : 0.5f));
            g.fillRoundedRectangle (acc, skin_.px (3.0f));
            // bande d'étouffé
            const auto mu = juce::Rectangle<float> (geo.x0 + c * geo.cellW, geo.mutedY, geo.cellW, geo.mutedH).reduced (skin_.px (2.0f));
            g.setColour (skin_.colour (h.muted ? Token::Accent2 : Token::TextDim2, h.muted ? 1.0f : 0.5f));
            g.fillRoundedRectangle (mu, skin_.px (3.0f));
            // vélocité et durée propres au coup (barre pleine) ; sans valeur propre : une barre discrète au niveau du réglage du plugin
            const float colW = std::max (2.0f, geo.cellW - skin_.px (4.0f));
            const float vh = (h.vel > 0 ? (float) h.vel : 100.0f) / 127.0f * geo.velH;
            g.setColour (skin_.colour (h.vel > 0 ? Token::Accent : Token::TextDim2, h.vel > 0 ? 0.9f : 0.35f));
            g.fillRect (geo.x0 + c * geo.cellW + skin_.px (2.0f), geo.velY + geo.velH - vh, colW, vh);
            const float gh = (h.gate > 0.0 ? (float) h.gate : 0.5f) * geo.gateH;
            g.setColour (skin_.colour (h.gate > 0.0 ? Token::Accent2 : Token::TextDim2, h.gate > 0.0 ? 0.9f : 0.35f));
            g.fillRect (geo.x0 + c * geo.cellW + skin_.px (2.0f), geo.gateY + geo.gateH - gh, colW, gh);
        }
        for (const float y : { geo.velY, geo.gateY })
        {
            g.setColour (skin_.colour (Token::BgApp, 0.5f));
            g.drawRect (juce::Rectangle<float> (geo.x0, y, geo.w, y == geo.velY ? geo.velH : geo.gateH), 1.0f);
        }
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (11.0f));
        g.drawText (juce::String::fromUTF8 ("vélocité"), juce::Rectangle<float> (0.0f, geo.velY, geo.x0 - skin_.px (6.0f), geo.velH), juce::Justification::centredRight);
        g.drawText (juce::String::fromUTF8 ("durée"), juce::Rectangle<float> (0.0f, geo.gateY, geo.x0 - skin_.px (6.0f), geo.gateH), juce::Justification::centredRight);
        g.setColour (skin_.colour (Token::TextDim));
        g.setFont (skin_.font (11.0f));
        g.drawText (juce::String::fromUTF8 ("accent"), juce::Rectangle<float> (0.0f, geo.accentY, geo.x0 - skin_.px (6.0f), geo.accentH), juce::Justification::centredRight);
        g.drawText (juce::String::fromUTF8 ("étouffé"), juce::Rectangle<float> (0.0f, geo.mutedY, geo.x0 - skin_.px (6.0f), geo.mutedH), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (slotIndex() < 0) return;
        last_ = { -1, -1 };
        strokeBefore_ = proc_.slotPattern (slotIndex());
        dirty_ = false;
        const auto geo = geometry (columns());
        band_ = e.position.y >= geo.gateY ? Band::Gate : (e.position.y >= geo.velY ? Band::Vel : (e.position.y >= geo.mutedY ? Band::Muted : (e.position.y >= geo.accentY ? Band::Accent : Band::Cells)));
        reset_ = e.mods.isRightButtonDown();   // clic droit sur une barre : retour au réglage du plugin
        apply (e.position, true);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (slotIndex() >= 0 && painting_) apply (e.position, false); }
    void mouseUp (const juce::MouseEvent&) override
    {
        painting_ = false;
        if (dirty_ && slotIndex() >= 0) proc_.commitSlot (slotIndex(), strokeBefore_, proc_.slotPattern (slotIndex()), "Matrice");
        dirty_ = false;
    }

private:
    struct Geo { float x0, y0, w, rowH, cellW, accentY, accentH, mutedY, mutedH, velY, velH, gateY, gateH; };
    Geo geometry (int cols) const
    {
        Geo g {};
        g.x0 = skin_.px (46.0f);
        g.y0 = skin_.px (6.0f);
        g.accentH = skin_.px (16.0f);
        g.mutedH = skin_.px (16.0f);
        g.velH = skin_.px (34.0f);
        g.gateH = skin_.px (34.0f);
        g.w = (float) getWidth() - g.x0 - skin_.px (8.0f);
        g.rowH = ((float) getHeight() - g.y0 - g.accentH - g.mutedH - g.velH - g.gateH - skin_.px (30.0f)) / (float) kNumRows;
        g.cellW = g.w / (float) cols;
        g.accentY = g.y0 + g.rowH * kNumRows + skin_.px (4.0f);
        g.mutedY = g.accentY + g.accentH + skin_.px (4.0f);
        g.velY = g.mutedY + g.mutedH + skin_.px (4.0f);
        g.gateY = g.velY + g.velH + skin_.px (4.0f);
        return g;
    }

    void apply (juce::Point<float> pos, bool first)
    {
        const int idx = slotIndex();
        const int cols = columns();
        const auto geo = geometry (cols);
        const int c = (int) std::floor ((pos.x - geo.x0) / geo.cellW);
        if (c < 0 || c >= cols) return;
        auto pat = proc_.slotPattern (idx);
        notes = pat.notesMode;
        const int a = altIndex (pat);
        const auto gr = kGridFracs[grid];
        const auto at = makeFrac (gr.n * c, gr.d);
        if (band_ == Band::Accent)
        {
            if (! first) return;
            pattern::toggleAccent (pat, at, a);
            proc_.setSlotPattern (idx, pat);
            dirty_ = true;
        }
        else if (band_ == Band::Muted)
        {
            if (! first) return;
            pattern::toggleMuted (pat, at, a);
            proc_.setSlotPattern (idx, pat);
            dirty_ = true;
        }
        else if (band_ == Band::Vel || band_ == Band::Gate)
        {
            painting_ = true;
            const bool vel = band_ == Band::Vel;
            const float y0 = vel ? geo.velY : geo.gateY, h = vel ? geo.velH : geo.gateH;
            const double t = std::clamp (1.0 - (double) (pos.y - y0) / (double) h, 0.0, 1.0);
            const bool ok = vel ? pattern::setHitVel (pat, at, reset_ ? 0 : std::max (1, (int) std::lround (t * 127.0)), a)
                                : pattern::setHitGate (pat, at, reset_ ? 0.0 : 0.05 + 0.95 * t, a);
            if (ok) { proc_.setSlotPattern (idx, pat); dirty_ = true; }
        }
        else
        {
            const int r = (int) std::floor ((pos.y - geo.y0) / geo.rowH);
            if (r < 0 || r >= kNumRows) return;
            if (last_.x == c && last_.y == r) return;
            last_ = { c, r };
            if (first)
            {
                paintOn_ = ! pattern::hasCell (pat, at, rowPart (r), a);
                painting_ = true;
            }
            if (pattern::hasCell (pat, at, rowPart (r), a) == paintOn_) return;
            if (! pattern::toggleCell (pat, at, gr, rowPart (r), a) && paintOn_) return;   // refusé (avant le début de la variante) : rien n'a changé
            proc_.setSlotPattern (idx, pat);
            dirty_ = true;
        }
        repaint();
        if (onChange) onChange();
    }

    ArpegeProcessor& proc_;
    const nova::Skin& skin_;
    juce::Point<int> last_ { -1, -1 };
    bool painting_ = false, paintOn_ = true, dirty_ = false, reset_ = false;
    enum class Band { Cells, Accent, Muted, Vel, Gate } band_ = Band::Cells;
    pattern::Pattern strokeBefore_;
};

// ================================================================================================ sections repliables

// Titre d'une section : une flèche (dépliée / repliée), le titre et un court résumé. Un clic plie ou déplie.
class ArpegeEditor::SectionHeader : public juce::Component
{
public:
    SectionHeader (const nova::Skin& s, juce::String title, juce::String summary) : skin_ (s), title_ (std::move (title)), summary_ (std::move (summary)) {}
    std::function<void()> onClick;
    bool open = true;

    void paint (juce::Graphics& g) override
    {
        using T = theme::Token;
        const auto b = getLocalBounds().toFloat();
        g.setColour (skin_.colour (T::PanelBg, 0.6f));
        g.fillRoundedRectangle (b, skin_.px (5.0f));
        const float cx = skin_.px (14.0f), cy = b.getCentreY(), r = skin_.px (4.5f);
        juce::Path tri;
        if (open) tri.addTriangle (cx - r, cy - r * 0.6f, cx + r, cy - r * 0.6f, cx, cy + r * 0.8f);
        else tri.addTriangle (cx - r * 0.6f, cy - r, cx - r * 0.6f, cy + r, cx + r * 0.8f, cy);
        g.setColour (skin_.colour (T::Accent));
        g.fillPath (tri);
        g.setColour (skin_.colour (T::Text));
        g.setFont (skin_.font (14.0f, true));
        const int tx = skin_.ipx (28.0f);
        const int tw = static_cast<int> (std::ceil (juce::GlyphArrangement::getStringWidth (skin_.font (14.0f, true), title_)));
        g.drawText (title_, tx, 0, tw + 4, getHeight(), juce::Justification::centredLeft, false);
        g.setColour (skin_.colour (T::TextDim));
        g.setFont (skin_.font (12.0f));
        g.drawText (summary_, tx + tw + skin_.ipx (14.0f), 0, getWidth() - tx - tw - skin_.ipx (20.0f), getHeight(), juce::Justification::centredLeft, true);
    }
    void mouseUp (const juce::MouseEvent& e) override { if (getLocalBounds().contains (e.getPosition()) && onClick) onClick(); }

private:
    const nova::Skin& skin_;
    juce::String title_, summary_;
};

struct ArpegeEditor::Section
{
    std::unique_ptr<SectionHeader> header;
    std::vector<size_t> pills, actions;   // indices dans pills_ et actions_
};

// ================================================================================================ l'aperçu en partition

// Ce qu'Arpège jouera quand on lance la lecture, en notation proportionnelle (l'axe du temps est régulier) sur une portée de sol et une de fa. Pour contrôler,
// pas pour écrire : les notes sont posées à leur instant, avec un trait pour leur durée.
class ArpegeEditor::ScoreView : public juce::Component
{
public:
    explicit ScoreView (const nova::Skin& s) : skin_ (s) { setWantsKeyboardFocus (false); }

    void setPreview (neptune::core::arp::Preview pv, juce::String problem)
    {
        pv_ = std::move (pv);
        problem_ = std::move (problem);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using T = theme::Token;
        const auto area = getLocalBounds().toFloat();
        skin_.fill (g, T::PanelBg, area, skin_.px (8.0f));
        // l'écart entre deux lignes de portée suit la hauteur disponible (deux portées, lignes supplémentaires, noms d'accords)
        const float sp = juce::jlimit (skin_.px (8.0f), skin_.px (17.0f), (static_cast<float> (getHeight()) - skin_.px (150.0f)) / 19.0f);
        g.setFont (skin_.font (13.0f));
        g.setColour (skin_.colour (T::TextDim));
        const auto head = juce::Rectangle<int> (skin_.ipx (14.0f), skin_.ipx (6.0f), getWidth() - skin_.ipx (28.0f), skin_.ipx (22.0f));
        if (! problem_.isEmpty())
        {
            g.drawFittedText (problem_, getLocalBounds().reduced (skin_.ipx (40.0f)), juce::Justification::centred, 3);
            return;
        }
        juce::String info = juce::String::fromUTF8 ("Aperçu : ce qu'Arpège jouera à la lecture (Carte seule, sans notes tenues)  ·  ")
                            + juce::String (static_cast<int> (pv_.notes.size())) + " notes  ·  "
                            + juce::String (static_cast<int> (pv_.chords.size())) + " accords";
        if (pv_.truncated) info << juce::String::fromUTF8 ("  ·  début seulement");
        g.drawText (info, head, juce::Justification::centredLeft, true);

        // géométrie
        const float left = skin_.px (62.0f), right = skin_.px (14.0f);
        const float top = skin_.px (78.0f) + 4.0f * sp;
        const float nameY = top - 3.6f * sp - skin_.px (44.0f), approachY = nameY + skin_.px (22.0f);
        const float trebleBottom = top + 4.0f * sp;
        const float gap = 5.0f * sp;
        const float bassBottom = trebleBottom + gap + 4.0f * sp;
        const double span = std::max (1.0, pv_.end - pv_.start);
        const float width = static_cast<float> (getWidth()) - left - right;
        ppb_ = std::max (skin_.px (26.0f), width / static_cast<float> (span));
        scrollMax_ = std::max (0.0f, static_cast<float> (span) * ppb_ - width);
        scroll_ = juce::jlimit (0.0f, scrollMax_, scroll_);
        const auto xOf = [&] (double t) { return left + static_cast<float> (t - pv_.start) * ppb_ - scroll_; };

        g.saveState();
        g.reduceClipRegion (juce::Rectangle<int> (static_cast<int> (left), 0, static_cast<int> (width), getHeight()));

        // bandes des accords, nom au-dessus
        for (size_t i = 0; i < pv_.chords.size(); ++i)
        {
            const auto& c = pv_.chords[i];
            const float x0 = xOf (c.start), x1 = xOf (c.start + c.length);
            if (x1 < left || x0 > left + width) continue;
            if (i % 2 == 0)
            {
                g.setColour (skin_.colour (T::BgApp, 0.35f));
                g.fillRect (x0, nameY, x1 - x0, bassBottom + skin_.px (30.0f) - nameY);
            }
            g.setColour (skin_.colour (T::TextDim, 0.5f));
            g.drawVerticalLine (static_cast<int> (x0), nameY, bassBottom + skin_.px (30.0f));
            g.setColour (skin_.colour (T::Text));
            g.setFont (skin_.font (15.0f, true));
            g.drawText (chordText (c.harmony), juce::Rectangle<float> (x0 + 3.0f, nameY, std::max (30.0f, x1 - x0), skin_.px (20.0f)).toNearestInt(), juce::Justification::centredLeft, false);
            if (c.hasApproach)
            {
                const float ax = xOf (c.approachFrom);
                g.setColour (skin_.colour (T::Pink));
                g.setFont (skin_.font (12.0f, true));
                g.drawText (juce::String::fromUTF8 ("approche ") + chordText (c.approach), juce::Rectangle<float> (ax + 3.0f, approachY, skin_.px (150.0f), skin_.px (18.0f)).toNearestInt(), juce::Justification::centredLeft, false);
                g.drawVerticalLine (static_cast<int> (ax), approachY, bassBottom + skin_.px (30.0f));
            }
        }
        // temps
        g.setColour (skin_.colour (T::TextDim, 0.18f));
        for (double t = std::ceil (pv_.start); t < pv_.end; t += 1.0)
            g.drawVerticalLine (static_cast<int> (xOf (t)), top, bassBottom);

        // portées
        g.setColour (skin_.colour (T::TextDim));
        for (int k = 0; k < 5; ++k)
        {
            g.drawHorizontalLine (static_cast<int> (trebleBottom - k * sp), left, left + width);
            g.drawHorizontalLine (static_cast<int> (bassBottom - k * sp), left, left + width);
        }

        // notes
        for (const auto& n : pv_.notes)
        {
            const float x0 = xOf (n.start), x1 = xOf (n.end);
            if (x1 < left - 20.0f || x0 > left + width + 20.0f) continue;
            const bool sharp = sharpFor (n.start);
            int step = 0, acc = 0;
            spell (n.note, sharp, step, acc);
            const bool treble = n.note >= 60;
            const float base = treble ? trebleBottom : bassBottom;
            const int baseStep = treble ? 30 : 18;
            const float y = base - static_cast<float> (step - baseStep) * sp * 0.5f;
            const auto col = n.approach ? skin_.colour (T::Pink) : (n.bass ? skin_.colour (T::Accent) : skin_.colour (T::Text));
            const float alpha = 0.45f + 0.55f * static_cast<float> (n.velocity) / 127.0f;
            // lignes supplémentaires
            g.setColour (skin_.colour (T::TextDim));
            const int bottomLine = baseStep, topLine = baseStep + 8;
            if (step < bottomLine)
                for (int s = bottomLine - 2; s >= step; s -= 2) g.drawHorizontalLine (static_cast<int> (base - (s - baseStep) * sp * 0.5f), x0 - sp, x0 + sp * 1.4f);
            if (step > topLine)
                for (int s = topLine + 2; s <= step; s += 2) g.drawHorizontalLine (static_cast<int> (base - (s - baseStep) * sp * 0.5f), x0 - sp, x0 + sp * 1.4f);
            if (treble && step < 30 && step >= 28) g.drawHorizontalLine (static_cast<int> (base - (28 - baseStep) * sp * 0.5f), x0 - sp, x0 + sp * 1.4f);
            if (! treble && step > 26) for (int s = 28; s <= step; s += 2) g.drawHorizontalLine (static_cast<int> (base - (s - baseStep) * sp * 0.5f), x0 - sp, x0 + sp * 1.4f);
            // durée
            g.setColour (col.withAlpha (alpha * 0.45f));
            g.fillRect (x0, y - 1.0f, std::max (2.0f, x1 - x0), 2.0f);
            // tête
            g.setColour (col.withAlpha (alpha));
            g.fillEllipse (x0 - sp * 0.55f, y - sp * 0.4f, sp * 1.1f, sp * 0.8f);   // tête fine
            if (acc != 0) drawAccidental (g, x0 - sp * 1.15f, y, sp, acc);
        }
        g.restoreState();

        // clés (étiquettes) et accolade
        g.setColour (skin_.colour (T::Text));
        drawTrebleClef (g, left * 0.5f, trebleBottom - sp, sp);
        drawBassClef (g, left * 0.5f, bassBottom - 3.0f * sp, sp);
        g.setColour (skin_.colour (T::TextDim));
        g.setFont (skin_.font (12.0f));
        g.drawText (juce::String::fromUTF8 ("Rose : accord d'approche  ·  couleur d'accent : partie basse du motif  ·  plus c'est pâle, plus la vélocité est faible  ·  faites glisser pour défiler"),
                    juce::Rectangle<int> (skin_.ipx (14.0f), getHeight() - skin_.ipx (24.0f), getWidth() - skin_.ipx (28.0f), skin_.ipx (18.0f)), juce::Justification::centredLeft, true);
    }

    void mouseDown (const juce::MouseEvent&) override { dragFrom_ = scroll_; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        scroll_ = juce::jlimit (0.0f, scrollMax_, dragFrom_ - static_cast<float> (e.getDistanceFromDragStartX()));
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        scroll_ = juce::jlimit (0.0f, scrollMax_, scroll_ - (std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY) * skin_.px (160.0f));
        repaint();
    }

private:
    // le nom de l'accord, écrit comme Carte l'écrit ; dièses pour les tonalités à dièses, bémols sinon
    juce::String chordText (const neptune::core::arp::Harmony& h) const
    {
        neptune::core::chords::Chord c;
        c.root = h.rootPc;
        c.bass = h.bassPc;
        c.rel = h.rel;
        c.sharp = sharpRoot (h.rootPc);
        return juce::String::fromUTF8 (neptune::core::chords::formatChord (c, neptune::core::chords::Naming::Letters).c_str());
    }
    static bool sharpRoot (int pc) { return pc == 7 || pc == 2 || pc == 9 || pc == 4 || pc == 11 || pc == 6; }
    bool sharpFor (double t) const
    {
        for (const auto& c : pv_.chords)
            if (t >= c.start && t < c.start + c.length)
                return sharpRoot (c.hasApproach && t >= c.approachFrom ? c.approach.rootPc : c.harmony.rootPc);
        return false;
    }
    // position sur la portée (degrés diatoniques depuis do-1) et altération (+1 dièse, -1 bémol)
    static void spell (int midi, bool sharp, int& step, int& acc)
    {
        static const int letterSharp[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
        static const int accSharp[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
        static const int letterFlat[12] = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };
        static const int accFlat[12] = { 0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0 };
        const int pc = ((midi % 12) + 12) % 12, oct = midi / 12 - 1;
        step = oct * 7 + (sharp ? letterSharp[pc] : letterFlat[pc]);
        acc = sharp ? accSharp[pc] : accFlat[pc];
    }
    // Clé de sol (esquisse) : `y0` est la ligne du sol, la deuxième depuis le bas
    static void drawTrebleClef (juce::Graphics& g, float cx, float y0, float sp)
    {
        juce::Path p;
        p.startNewSubPath (cx - 0.5f * sp, y0 + 1.7f * sp);                                                              // crochet du bas
        p.cubicTo (cx - 0.5f * sp, y0 + 2.5f * sp, cx + 0.6f * sp, y0 + 2.5f * sp, cx + 0.6f * sp, y0 + 1.8f * sp);
        p.lineTo (cx + 0.1f * sp, y0 - 3.0f * sp);                                                                       // la hampe, un peu penchée
        p.cubicTo (cx + 0.1f * sp, y0 - 3.9f * sp, cx + 1.2f * sp, y0 - 3.6f * sp, cx + 1.0f * sp, y0 - 2.5f * sp);      // boucle du haut
        p.cubicTo (cx + 0.8f * sp, y0 - 1.5f * sp, cx - 1.0f * sp, y0 - 1.0f * sp, cx - 1.0f * sp, y0 + 0.1f * sp);      // descend autour de la ligne du sol
        p.cubicTo (cx - 1.0f * sp, y0 + 1.1f * sp, cx + 0.9f * sp, y0 + 1.1f * sp, cx + 0.9f * sp, y0 - 0.1f * sp);
        p.cubicTo (cx + 0.9f * sp, y0 - 1.0f * sp, cx - 0.2f * sp, y0 - 1.1f * sp, cx - 0.4f * sp, y0 - 0.2f * sp);      // le rond autour du sol
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.4f, sp * 0.16f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // Clé de fa (esquisse) : `yF` est la ligne du fa, la quatrième depuis le bas
    static void drawBassClef (juce::Graphics& g, float cx, float yF, float sp)
    {
        g.fillEllipse (cx - 0.9f * sp, yF - 0.5f * sp, 1.0f * sp, 1.0f * sp);                                            // la tête
        juce::Path p;
        p.startNewSubPath (cx - 0.9f * sp, yF);
        p.cubicTo (cx - 0.6f * sp, yF - 0.9f * sp, cx + 1.1f * sp, yF - 0.7f * sp, cx + 1.1f * sp, yF + 0.6f * sp);      // la courbe vers la droite
        p.cubicTo (cx + 1.1f * sp, yF + 1.8f * sp, cx - 0.1f * sp, yF + 2.8f * sp, cx - 1.0f * sp, yF + 3.4f * sp);      // puis vers le bas
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.4f, sp * 0.16f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillEllipse (cx + 1.35f * sp, yF - 0.85f * sp, 0.45f * sp, 0.45f * sp);                                        // les deux points
        g.fillEllipse (cx + 1.35f * sp, yF + 0.4f * sp, 0.45f * sp, 0.45f * sp);
    }
    static void drawAccidental (juce::Graphics& g, float x, float y, float sp, int acc)
    {
        juce::Path p;
        if (acc > 0)   // dièse : deux traits verticaux, deux obliques
        {
            p.addLineSegment ({ x - sp * 0.15f, y - sp * 0.8f, x - sp * 0.15f, y + sp * 0.8f }, 1.0f);
            p.addLineSegment ({ x + sp * 0.2f, y - sp * 0.8f, x + sp * 0.2f, y + sp * 0.8f }, 1.0f);
            p.addLineSegment ({ x - sp * 0.4f, y + sp * 0.15f, x + sp * 0.45f, y - sp * 0.15f }, 1.2f);
            p.addLineSegment ({ x - sp * 0.4f, y + sp * 0.5f, x + sp * 0.45f, y + sp * 0.2f }, 1.2f);
        }
        else           // bémol : un trait et une panse
        {
            p.addLineSegment ({ x - sp * 0.1f, y - sp * 1.0f, x - sp * 0.1f, y + sp * 0.5f }, 1.0f);
            p.startNewSubPath (x - sp * 0.1f, y + sp * 0.5f);
            p.cubicTo (x + sp * 0.5f, y + sp * 0.2f, x + sp * 0.5f, y - sp * 0.35f, x - sp * 0.1f, y - sp * 0.05f);
        }
        g.strokePath (p, juce::PathStrokeType (1.1f));
    }

    const nova::Skin& skin_;
    neptune::core::arp::Preview pv_;
    juce::String problem_;
    float ppb_ = 30.0f, scroll_ = 0.0f, scrollMax_ = 0.0f, dragFrom_ = 0.0f;
};

ArpegeEditor::ArpegeEditor (ArpegeProcessor& p)
    : juce::AudioProcessorEditor (p), proc_ (p), skin_ (makePalette (p.settings()), themeOf (p.settings()).knob, themeOf (p.settings()).id)
{
    const auto add = [this] (juce::RangedAudioParameter* prm, const char* label)
    {
        pills_.push_back (std::make_unique<ParamPill> (skin_, *prm, juce::String::fromUTF8 (label)));
        addAndMakeVisible (*pills_.back());
    };
    add (proc_.pattern, "Motif auto");
    add (proc_.octaves, "Octaves");
    add (proc_.rate, "Pas");
    add (proc_.gate, "Durée %");
    add (proc_.swing, "Swing %");
    add (proc_.hold, "Maintien");
    add (proc_.velMode, "Vélocité");
    add (proc_.velocity, "Fixe");
    add (proc_.accentEvery, "Accent 1 pas sur");
    add (proc_.accentBoost, "Force");
    add (proc_.transpose, "Transposition");
    add (proc_.slot, "Motif dessiné");
    add (proc_.absentMode, "Degré absent");
    add (proc_.source, "Source");
    add (proc_.carteChannel, "Canal Carte");
    add (proc_.baseOctave, "Octave de base");
    add (proc_.ledVoices, "Voix conduites");
    add (proc_.strumMode, "Grattage");
    add (proc_.strumDelay, "Délai");
    add (proc_.approachMode, "Approche");

    matrix_ = std::make_unique<MatrixView> (proc_, skin_);
    addAndMakeVisible (*matrix_);
    score_ = std::make_unique<ScoreView> (skin_);
    addChildComponent (*score_);

    // actions : 0 grille, 1 durée, 2 mesures, 3 fourni, 4 charger, 5 importer, 6 exporter, 7 effacer, 8 mode du motif
    for (int i = 0; i < 17; ++i)
    {
        actions_.push_back (std::make_unique<nova::Pill> (skin_, nova::Pill::Look::Small));
        addAndMakeVisible (*actions_.back());
    }
    const auto withPattern = [this] (const std::function<void (pattern::Pattern&)>& edit) { editSlot ("Motif", edit); };
    actions_[0]->onClick = [this] { matrix_->grid = (matrix_->grid + 1) % kNumGrids; matrix_->repaint(); refreshActions(); };
    actions_[1]->onClick = [this, withPattern]
    {
        withPattern ([] (pattern::Pattern& pt)
        {
            int cur = 0;
            for (int i = 0; i < kNumLengths; ++i) if (pattern::compare (pt.length, kLengths[i]) == 0) cur = i;
            pattern::setLengthAndBars (pt, kLengths[(cur + 1) % kNumLengths], pt.bars);
        });
    };
    actions_[2]->onClick = [withPattern] { withPattern ([] (pattern::Pattern& pt) { pattern::setLengthAndBars (pt, pt.length, pt.bars % 4 + 1); }); };
    actions_[3]->onClick = [this] { fournis_ = (fournis_ + 1) % (int) proc_.builtinPatterns().size(); refreshActions(); };
    actions_[4]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        const auto all = proc_.builtinPatterns();
        if (idx >= 0 && fournis_ < (int) all.size()) proc_.commitSlot (idx, proc_.slotPattern (idx), all[(size_t) fournis_], "Charger un motif fourni");
        matrix_->repaint();
        refreshActions();
    };
    actions_[5]->onClick = [this]
    {
        if (proc_.slot->get() < 1) return;
        chooser_ = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Importer un motif (Arpège ou Scorsaire)"), juce::File(), "*.json");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            const auto r = pattern::fromJson (f.loadFileAsString().toStdString());
            if (! r.ok || r.patterns.empty()) return;
            int idx = proc_.slot->get() - 1;
            for (const auto& pt : r.patterns)
            {
                if (idx >= ArpegeProcessor::kSlots) break;
                proc_.commitSlot (idx, proc_.slotPattern (idx), pt, "Importer un motif");
                ++idx;
            }
            matrix_->repaint();
            refreshActions();
        });
    };
    actions_[6]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        chooser_ = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Exporter le motif (le dossier proposé est la bibliothèque lue par Orgamon)"), motifs::fileFor (proc_.slotPattern (idx)), "*.json");
        chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting, [this, idx] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty()) f = f.withFileExtension ("json");
            f.replaceWithText (juce::String (pattern::toJson ({ proc_.slotPattern (idx) }, "arpege")));
        });
    };
    actions_[8]->onClick = [withPattern] { withPattern ([] (pattern::Pattern& pt) { pattern::setNotesMode (pt, ! pt.notesMode); }); };
    // variantes : 11 choisir, 12 ajouter, 13 probabilité, 14 début, 15 retirer
    actions_[11]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        const int n = (int) proc_.slotPattern (idx).alts.size();
        matrix_->alt = matrix_->alt + 1 >= n ? -1 : matrix_->alt + 1;   // le motif, puis chaque variante
        matrix_->repaint();
        refreshActions();
    };
    actions_[12]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        const auto before = proc_.slotPattern (idx);
        auto after = before;
        const auto bar = after.barLength();
        const int k = pattern::addAlt (after, pattern::makeFrac (bar.n, bar.d * 2), 0.5);   // la 2e moitié de la mesure, une fois sur deux
        if (k < 0) return;
        proc_.commitSlot (idx, before, after, "Ajouter une variante");
        matrix_->alt = k;
        matrix_->repaint();
        refreshActions();
    };
    actions_[13]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        const int a = matrix_->alt;
        if (idx < 0 || a < 0) return;
        editSlot ("Probabilité de la variante", [a] (pattern::Pattern& pt)
        {
            if (a >= (int) pt.alts.size()) return;
            const double c = pt.alts[(size_t) a].chance;
            pattern::setAltChance (pt, a, c >= 0.99 ? 0.0 : (c < 0.125 ? 0.25 : (c < 0.375 ? 0.5 : (c < 0.625 ? 0.75 : 1.0))));
        });
    };
    actions_[14]->onClick = [this]
    {
        const int a = matrix_->alt;
        if (a < 0) return;
        editSlot ("Début de la variante", [a] (pattern::Pattern& pt)
        {
            if (a >= (int) pt.alts.size()) return;
            const double bar = pt.barLength().toBeats();
            const double cur = pt.alts[(size_t) a].from.toBeats();
            double next = std::floor (cur + 1.0001);
            if (next >= bar - 1e-9) next = 1.0;                                // un temps après le début, puis on recommence
            pattern::setAltFrom (pt, a, pattern::makeFrac ((std::int64_t) std::llround (next), 4));
        });
    };
    actions_[15]->onClick = [this]
    {
        const int a = matrix_->alt;
        if (a < 0) return;
        editSlot ("Retirer la variante", [a] (pattern::Pattern& pt) { pattern::removeAlt (pt, a); });
        matrix_->alt = -1;
        matrix_->repaint();
        refreshActions();
    };
    actions_[16]->onClick = [this]
    {
        showScore_ = ! showScore_;
        matrix_->setVisible (! showScore_);
        score_->setVisible (showScore_);
        refreshScore (true);
        refreshActions();
        resized();
    };
    actions_[9]->setText ("Annuler");
    actions_[9]->onClick = [this] { proc_.undo().undo(); matrix_->repaint(); refreshActions(); };
    actions_[10]->setText (juce::String::fromUTF8 ("Rétablir"));
    actions_[10]->onClick = [this] { proc_.undo().redo(); matrix_->repaint(); refreshActions(); };
    actions_[7]->onClick = [this]
    {
        const int idx = proc_.slot->get() - 1;
        if (idx < 0) return;
        pattern::Pattern fresh;   // un motif vide, prêt à dessiner
        fresh.id = "motif" + std::to_string (idx + 1);
        pattern::setNotesMode (fresh, true);
        proc_.commitSlot (idx, proc_.slotPattern (idx), fresh, "Effacer le motif");
        matrix_->repaint();
        refreshActions();
    };


    // ---- aide : un texte par réglage, montré en bas quand on le survole ou le touche
    static const char* const pillHelp[20] = {
        "Motif automatique : comment les notes tenues (ou l'accord de Carte) sont parcourues : montant, descendant, aller-retour, aléatoire, dans l'ordre joué, ou accord. Sans effet quand un motif dessiné ou un motif de Carte joue.",
        "Nombre d'octaves parcourues par le motif automatique.",
        "Durée d'un pas du motif automatique, calée sur la grille de l'hôte (1/16 = double-croche).",
        "Longueur de chaque note, en pourcentage du pas.",
        "Retard des pas impairs : 0 = régulier ; plus haut, le rythme est plus chaloupé.",
        "Garde les notes après le relâchement des touches, jusqu'à un nouvel accord.",
        "Jouée : reprend la force de la touche. Fixe : utilise la valeur « Fixe ».",
        "Vélocité utilisée quand le mode est « Fixe ».",
        "Un pas sur N est renforcé (0 : pas d'accent).",
        "Vélocité ajoutée aux pas accentués.",
        "Décale toutes les notes, en demi-tons.",
        "Emplacement (1 à 16) du motif dessiné dans la matrice ; « aucun » : motifs automatiques. Avec Carte, les motifs posés sur sa voie des motifs passent en premier.",
        "Quand un degré demandé par le motif n'existe pas dans l'accord (la neuvième d'un accord simple, par exemple) : le sauter, jouer la note par défaut du degré, ou prendre la note de la gamme de Carte.",
        "Notes tenues : Arpège arpège ce que vous jouez. Carte : il suit les accords, gammes, motifs et mesures de Carte et joue seul pendant la lecture.",
        "Numéro de liaison (1 à 8) : le même que celui du plugin Carte.",
        "Octave où Carte joue seule, sans note tenue (3 = do 48).",
        "Chaque accord est posé au plus près du précédent, avec le moins de mouvement possible (comme dans Scorsaire).",
        "Les notes des parties « accord » partent l'une après l'autre : coup bas, ou alterné bas / haut.",
        "Durée entre deux notes du grattage, en millisecondes ; « auto » : d'après le tempo.",
        "Avant un changement d'accord, un coup du motif joue un accord qui mène au suivant : dominante (V7), triton (bII7), chromatique, ou parallèle. Voir la vue partition." };
    for (size_t i = 0; i < pills_.size() && i < 20; ++i) addHelp (*pills_[i], pillHelp[i]);
    static const char* const actionHelp[17] = {
        "Finesse de la grille de dessin de la matrice.",
        "Longueur du motif dessiné, en temps.",
        "Nombre de mesures que dure le motif avant de se répéter.",
        "Choisit un motif fourni ; « Charger » le copie dans l'emplacement choisi.",
        "Copie le motif fourni dans l'emplacement du motif dessiné (annulable).",
        "Lit un motif dans un fichier JSON (Arpège ou Scorsaire).",
        "Enregistre le motif dans un fichier JSON ; le dossier proposé est la bibliothèque lue par Orgamon.",
        "Vide l'emplacement et prépare un motif neuf à dessiner.",
        "Notes (rangs) : un vrai arpégiateur sur les notes tenues. Accord (degrés) : les lignes sont des degrés de l'accord de Carte.",
        "Annule le dernier changement de motif.",
        "Rétablit le changement annulé.",
        "Choisit ce que la matrice dessine : le motif, ou l'une de ses variantes.",
        "Ajoute une variante : une autre version du motif, jouée une fois sur deux sur la seconde moitié de la mesure.",
        "Part des mesures où la variante remplace le motif.",
        "Temps de la mesure où la variante commence.",
        "Retire la variante choisie.",
        "Bascule entre la matrice du motif et la partition de ce qu'Arpège jouera à la lecture (pour contrôler)." };
    for (size_t i = 0; i < actions_.size() && i < 17; ++i) addHelp (*actions_[i], actionHelp[i]);

    // ---- sections repliables
    struct Def { const char* title; const char* summary; const char* help; std::vector<size_t> pills, actions; };
    const std::vector<Def> defs = {
        { "Source et Carte", "d'où viennent les notes et les accords", "Source et Carte : choisit si Arpège joue ce que vous tenez ou suit la carte d'accords de Carte (canal, octave de base, voix conduites).", { 13, 14, 15, 16 }, {} },
        { "Motif automatique", "ce que joue Arpège sans motif dessiné", "Motif automatique : le parcours des notes (montant, descendant...), sa vitesse, sa durée, le swing, les accents et la vélocité.", { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, {} },
        { "Jeu des accords", "degré absent, grattage, accords d'approche", "Jeu des accords : ce que font les motifs en degrés avec les accords de Carte : degré absent, grattage, accords d'approche.", { 12, 17, 18, 19 }, {} },
        { "Motif dessiné et fichiers", "l'emplacement, la matrice, importer / exporter", "Motif dessiné : choix de l'emplacement 1 à 16, réglages de la matrice, motifs fournis, import et export.", { 11 }, { 0, 1, 2, 8, 3, 4, 5, 6, 7, 9, 10 } },
        { "Variantes et affichage", "variantes du motif ; vue motif ou partition", "Variantes : d'autres versions du motif jouées de temps en temps. Vue : bascule entre la matrice et la partition de ce qui sera joué.", {}, { 11, 12, 13, 14, 15, 16 } },
    };
    for (size_t k = 0; k < defs.size(); ++k)
    {
        auto sec = std::make_shared<Section>();
        sec->header = std::make_unique<SectionHeader> (skin_, juce::String::fromUTF8 (defs[k].title), juce::String::fromUTF8 (defs[k].summary));
        sec->pills = defs[k].pills;
        sec->actions = defs[k].actions;
        sec->header->open = (proc_.sectionsOpen.load() >> k & 1) != 0;
        const auto sp = std::weak_ptr<Section> (sec);
        sec->header->onClick = [this, k, sp]
        {
            if (const auto s = sp.lock())
            {
                s->header->open = ! s->header->open;
                proc_.sectionsOpen.store (s->header->open ? (proc_.sectionsOpen.load() | (1 << k)) : (proc_.sectionsOpen.load() & ~(1 << k)));
                s->header->repaint();
                resized();
                repaint();
            }
        };
        addAndMakeVisible (*sec->header);
        addHelp (*sec->header, defs[k].help);
        sections_.push_back (std::move (sec));
    }

    nova::configureConstrainer (constrainer_, kDesignW, kDesignH, 0.6f, 3.0f);
    setConstrainer (&constrainer_);
    setResizable (true, true);
    setSize (kDesignW, kDesignH);
    refreshActions();
    startTimerHz (15);
}

ArpegeEditor::~ArpegeEditor() { stopTimer(); }

void ArpegeEditor::refreshActions()
{
    const int idx = proc_.slot->get() - 1;
    const auto all = proc_.builtinPatterns();
    const bool on = idx >= 0;
    juce::String grid = juce::String::fromUTF8 ("Grille ") + kGridNames[matrix_->grid];
    juce::String len = juce::String::fromUTF8 ("Durée "), bars = juce::String::fromUTF8 ("Mesures ");
    if (on)
    {
        const auto pt = proc_.slotPattern (idx);
        len << juce::String (pt.length.toBeats(), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") << " temps";
        bars << pt.bars;
    }
    actions_[8]->setText (on && ! proc_.slotPattern (idx).notesMode ? juce::String::fromUTF8 ("Mode : accord (degrés)") : juce::String::fromUTF8 ("Mode : notes (rangs)"));
    actions_[8]->setDim (! on);
    actions_[0]->setText (grid);
    actions_[1]->setText (len);
    actions_[2]->setText (bars);
    actions_[3]->setText (juce::String::fromUTF8 ("Fourni : ") + (all.empty() ? juce::String() : juce::String (all[(size_t) fournis_ % all.size()].name)));
    actions_[4]->setText ("Charger");
    actions_[5]->setText ("Importer");
    actions_[6]->setText ("Exporter");
    actions_[7]->setText (juce::String::fromUTF8 ("Effacer"));
    for (int i = 1; i < 8; ++i) if (i != 3) actions_[(size_t) i]->setDim (! on);
    actions_[0]->setDim (! on);
    {
        // variantes
        const int nAlts = on ? (int) proc_.slotPattern (idx).alts.size() : 0;
        if (matrix_->alt >= nAlts) matrix_->alt = -1;
        const int a = matrix_->alt;
        actions_[11]->setText (a < 0 ? juce::String::fromUTF8 ("Dessine : le motif") : juce::String::fromUTF8 ("Dessine : variante ") + juce::String (a + 1) + "/" + juce::String (nAlts));
        actions_[12]->setText (juce::String::fromUTF8 ("+ Variante"));
        actions_[15]->setText (juce::String::fromUTF8 ("Retirer la variante"));
        if (a >= 0)
        {
            const auto pt = proc_.slotPattern (idx);
            actions_[13]->setText (juce::String::fromUTF8 ("Probabilité ") + juce::String (juce::roundToInt (pt.alts[(size_t) a].chance * 100.0)) + " %");
            actions_[14]->setText (juce::String::fromUTF8 ("Début : temps ") + juce::String (juce::roundToInt (pt.alts[(size_t) a].from.toBeats()) + 1));
        }
        else
        {
            actions_[13]->setText (juce::String::fromUTF8 ("Probabilité"));
            actions_[14]->setText (juce::String::fromUTF8 ("Début"));
        }
        actions_[11]->setDim (! on || nAlts == 0);
        actions_[12]->setDim (! on || nAlts >= 7);
        for (int i : { 13, 14, 15 }) actions_[(size_t) i]->setDim (a < 0);
    }
    actions_[16]->setText (showScore_ ? juce::String::fromUTF8 ("Vue : partition") : juce::String::fromUTF8 ("Vue : motif"));
    actions_[9]->setDim (! proc_.undo().canUndo());
    actions_[10]->setDim (! proc_.undo().canRedo());
}

void ArpegeEditor::editSlot (const char* name, const std::function<void (pattern::Pattern&)>& edit)
{
    const int idx = proc_.slot->get() - 1;
    if (idx < 0) return;
    const auto before = proc_.slotPattern (idx);
    auto after = before;
    edit (after);
    proc_.commitSlot (idx, before, after, name);
    matrix_->repaint();
    refreshActions();
}

bool ArpegeEditor::keyPressed (const juce::KeyPress& k)
{
    const bool cmd = k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown();
    const int code = k.getKeyCode();
    if (cmd && (code == 'Z' || code == 'z'))
    {
        if (k.getModifiers().isShiftDown()) proc_.undo().redo(); else proc_.undo().undo();
        matrix_->repaint();
        refreshActions();
        return true;
    }
    if (cmd && (code == 'Y' || code == 'y')) { proc_.undo().redo(); matrix_->repaint(); refreshActions(); return true; }
    return false;
}

void ArpegeEditor::resized()
{
    skin_.setScale (nova::fitScale (getWidth(), getHeight(), kDesignW, kDesignH, 0.6f, 3.0f));
    const auto px = [this] (float v) { return skin_.ipx (v); };
    const float w[] = { 260, 110, 150, 120, 120, 120, 120, 100, 190, 100, 170, 160, 150, 140, 110, 120, 110, 140, 130, 140 };
    const float aw[] = { 160, 130, 100, 160, 90, 90, 90, 90, 170, 90, 100, 180, 120, 170, 150, 190, 160 };
    const int left = px (16.0f), right = getWidth() - px (16.0f), h = px (30.0f), gap = px (8.0f), hh = px (26.0f);
    int y = px (56.0f);
    for (auto& sec : sections_)
    {
        sec->header->setBounds (left, y, right - left, hh);
        y += hh + px (4.0f);
        const bool open = sec->header->open;
        int x = left;
        const auto place = [&] (juce::Component& c, float width)
        {
            c.setVisible (open);
            if (! open) return;
            const int wd = px (width);
            if (x + wd > right && x > left) { x = left; y += h + gap; }
            c.setBounds (x, y, wd, h);
            x += wd + gap;
        };
        for (size_t i : sec->pills) place (*pills_[i], w[i]);
        for (size_t i : sec->actions) place (*actions_[i], aw[i]);
        if (open && (! sec->pills.empty() || ! sec->actions.empty())) y += h + px (8.0f);
        else y += px (2.0f);
    }
    const int my = y + px (2.0f);

    matrix_->setBounds (px (16.0f), my, getWidth() - px (32.0f), std::max (px (80.0f), getHeight() - my - px (50.0f)));
    score_->setBounds (matrix_->getBounds());
}

void ArpegeEditor::paint (juce::Graphics& g)
{
    g.fillAll (skin_.colour (theme::Token::BgApp));
    nova::paintHeaderBar (g, skin_, juce::Rectangle<float> (0.0f, 0.0f, static_cast<float> (getWidth()), skin_.px (46.0f)), juce::String::fromUTF8 ("Arpège"), juce::String::fromUTF8 ("arpégiateur"));
    g.setFont (skin_.font (13.0f));
    g.setColour (skin_.colour (theme::Token::TextDim));
    juce::String plays;
    {
        const bool viaCarte = proc_.source->getIndex() == 1 && proc_.carteHasPatterns();
        const int sl = proc_.slot->get();
        if (viaCarte) plays = juce::String::fromUTF8 ("Joue : les motifs placés dans Carte") + (sl > 0 ? juce::String::fromUTF8 (" (sinon le motif ") + juce::String (sl) + ")" : juce::String::fromUTF8 (" (sinon rien)"));
        else if (sl > 0) plays = juce::String::fromUTF8 ("Joue : le motif dessiné n° ") + juce::String (sl);
        else plays = juce::String::fromUTF8 ("Joue : le motif automatique (") + juce::String (proc_.pattern->getCurrentValueAsText()) + ")";
        plays << "   ·   ";
    }
    juce::String carteInfo;
    if (proc_.source->getIndex() == 1)
    {
        const int st = proc_.carteState();
        carteInfo = juce::String::fromUTF8 ("   ·   Carte (canal ") + juce::String (proc_.carteChannel->get()) + juce::String::fromUTF8 ("), ")
                    + (st < 0 ? juce::String::fromUTF8 ("non connectée") : (st == 0 ? juce::String::fromUTF8 ("pas d'accord ici") : juce::String::fromUTF8 ("accord ") + proc_.carteChordText()
                    + (proc_.hostPlaying() ? juce::String::fromUTF8 (" : joue seule, sans note tenue") : juce::String::fromUTF8 (" : lancez la lecture pour qu'elle joue"))));
    }
    const auto text = plays + juce::String::fromUTF8 ("Notes tenues : ") + juce::String (proc_.heldCount()) + "   ·   "
                      + (proc_.hostPlaying() ? juce::String::fromUTF8 ("calé sur la lecture de l'hôte") : juce::String::fromUTF8 ("lecture arrêtée : horloge interne"))
                      + carteInfo;
    if (helpText_.isNotEmpty())   // l'aide du réglage survolé ou touché remplace la ligne d'état
    {
        g.setColour (skin_.colour (theme::Token::Text));
        g.setFont (skin_.font (13.0f));
        g.drawFittedText (helpText_, juce::Rectangle<int> (skin_.ipx (16.0f), getHeight() - skin_.ipx (44.0f), getWidth() - skin_.ipx (32.0f), skin_.ipx (38.0f)), juce::Justification::centredLeft, 2);
        return;
    }
    g.drawText (text, juce::Rectangle<float> (skin_.px (16.0f), static_cast<float> (getHeight()) - skin_.px (34.0f), static_cast<float> (getWidth()) - skin_.px (32.0f), skin_.px (24.0f)).toNearestInt(),
                juce::Justification::centredLeft, true);
}

void ArpegeEditor::addHelp (juce::Component& c, const char* text)
{
    help_[&c] = juce::String::fromUTF8 (text);
    c.addMouseListener (this, false);
}

void ArpegeEditor::mouseEnter (const juce::MouseEvent& e)
{
    const auto it = help_.find (e.eventComponent);
    if (it == help_.end()) return;
    helpText_ = it->second;
    helpAt_ = juce::Time::getMillisecondCounter();
    repaint (0, getHeight() - skin_.ipx (50.0f), getWidth(), skin_.ipx (50.0f));
}

void ArpegeEditor::refreshScore (bool force)
{
    if (! showScore_) return;
    const auto key = proc_.previewKey();
    if (! force && key == scoreKey_) return;
    scoreKey_ = key;
    auto r = proc_.makePreview();
    score_->setPreview (std::move (r.preview), r.problem);
}

void ArpegeEditor::timerCallback()
{
    refreshScore (false);
    for (auto& p : pills_) p->refresh();
    // les réglages qui ne servent pas au mode en cours sont grisés (ils restent réglables)
    const bool carteSrc = proc_.source->getIndex() == 1;
    const bool autoActive = proc_.slot->get() == 0 && ! proc_.carteHasPatterns();
    for (size_t i : { 0u, 1u, 2u, 4u, 8u }) pills_[i]->setDim (! autoActive);   // motif auto, octaves, pas, swing, accent 1 pas sur : motifs automatiques seulement
    pills_[19]->setDim (autoActive || ! proc_.carteHasPatterns() && proc_.source->getIndex() != 1);   // approche : motifs en degrés avec Carte
    pills_[17]->setDim (autoActive);                                           // grattage et son délai : motifs seulement
    pills_[18]->setDim (autoActive || proc_.strumMode->getIndex() == 0);
    pills_[16]->setDim (autoActive);                                           // voix conduites : motifs en degrés seulement
    pills_[12]->setDim (autoActive);                                           // degré absent : motifs en degrés seulement
    pills_[14]->setDim (! carteSrc);                                           // canal de Carte
    pills_[15]->setDim (! carteSrc);                                           // octave de base
    const int idx = proc_.slot->get() - 1;
    if (idx != shownSlot_)
    {
        shownSlot_ = idx;
        matrix_->alt = -1;
        if (idx >= 0 && ! proc_.slotUsed (idx)) proc_.setSlotPattern (idx, proc_.slotPattern (idx));   // emplacement libre : un motif vide, prêt à dessiner
        matrix_->repaint();
        refreshActions();
    }
    if (helpText_.isNotEmpty() && juce::Time::getMillisecondCounter() - helpAt_ > 7000) helpText_ = {};   // l'aide s'efface au bout de quelques secondes (utile au doigt)
    repaint (0, getHeight() - skin_.ipx (50.0f), getWidth(), skin_.ipx (50.0f));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ArpegeProcessor(); }
