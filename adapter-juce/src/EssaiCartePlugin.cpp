#include "EssaiCartePlugin.h"

using namespace neptune::core::chordmap;

namespace
{
constexpr const char* kNotes[12] = { "Do", "Do#", "Ré", "Mib", "Mi", "Fa", "Fa#", "Sol", "Lab", "La", "Sib", "Si" };

std::uint16_t triadMask (int root, bool minor)
{
    return static_cast<std::uint16_t> ((1u << root) | (1u << ((root + (minor ? 3 : 4)) % 12)) | (1u << ((root + 7) % 12)));
}

juce::String chordName (const Container& c)
{
    const bool minor = (c.pitchMask & (1u << ((c.root + 3) % 12))) != 0 && (c.pitchMask & (1u << ((c.root + 4) % 12))) == 0;
    return juce::String::fromUTF8 (kNotes[c.root % 12]) + (minor ? " mineur" : " majeur");
}
} // namespace

EssaiCarteProcessor::EssaiCarteProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (role_ = new juce::AudioParameterChoice (juce::ParameterID { "role", 1 }, "Role", juce::StringArray { "Ecrivain", "Lecteur" }, 1));
    addParameter (channel_ = new juce::AudioParameterInt (juce::ParameterID { "channel", 1 }, "Canal", 1, kChannels, 1));
    ticker_.startTimer (500);
}

EssaiCarteProcessor::~EssaiCarteProcessor() { ticker_.stopTimer(); }

bool EssaiCarteProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void EssaiCarteProcessor::publishExample()
{
    std::vector<Container> v;
    const int cycle[4][2] = { { 0, 0 }, { 5, 0 }, { 7, 0 }, { 9, 1 } };   // Do | Fa | Sol | La mineur
    for (int rep = 0; rep < 32; ++rep)
        for (int i = 0; i < 4; ++i)
        {
            Container c;
            c.startBeats = rep * 16.0 + i * 4.0;
            c.lengthBeats = 4.0;
            c.kind = Kind::Chord;
            c.root = static_cast<std::uint8_t> (cycle[i][0]);
            c.pitchMask = triadMask (cycle[i][0], cycle[i][1] != 0);
            v.push_back (c);
        }
    Container key;
    key.startBeats = 0.0; key.lengthBeats = 512.0; key.kind = Kind::Key; key.root = 0;
    key.pitchMask = static_cast<std::uint16_t> ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 11));   // Do majeur
    v.push_back (key);
    writer_.publish (std::move (v));
}

void EssaiCarteProcessor::tick()
{
    const int r = role_->getIndex(), ch = channel_->get();
    if (r != openedRole_ || ch != openedChannel_)
    {
        writer_.close();
        reader_.setChannel (ch);
        writerStatus_ = -1;
        if (r == 0)
        {
            const auto s = writer_.open (ch);
            writerStatus_ = static_cast<int> (s);
            if (s == Status::Ok) publishExample();
        }
        openedRole_ = r;
        openedChannel_ = ch;
    }
    if (r == 0)
    {
        if (writer_.isOpen()) writer_.touch();
        else if (writerStatus_ == static_cast<int> (Status::InUse) && writer_.open (ch) == Status::Ok)   // le canal s'est libéré
        {
            writerStatus_ = static_cast<int> (Status::Ok);
            publishExample();
        }
    }
    else
        reader_.poll();
}

void EssaiCarteProcessor::processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    if (openedRole_ != 1) return;   // seul le lecteur consulte la carte ; le son passe sans changement
    Seen s;
    s.connected = reader_.connected();
    s.sequence = reader_.sequence();
    if (auto* head = getPlayHead())
        if (const auto pos = head->getPosition())
            if (const auto ppq = pos->getPpqPosition())
            {
                s.hasPosition = true;
                s.beats = *ppq;
            }
    if (s.hasPosition)
    {
        s.hasChord = reader_.lookup (s.beats, Kind::Chord, s.chord);
        s.hasNext = reader_.next (s.beats, Kind::Chord, s.next);
        s.hasKey = reader_.lookup (s.beats, Kind::Key, s.key);
    }
    seenGen_.fetch_add (1, std::memory_order_acq_rel);
    seenData_ = s;
    seenGen_.fetch_add (1, std::memory_order_release);
}

EssaiCarteProcessor::Seen EssaiCarteProcessor::seen() const
{
    Seen out;
    for (int i = 0; i < 8; ++i)
    {
        const auto g1 = seenGen_.load (std::memory_order_acquire);
        if ((g1 & 1u) != 0) continue;
        out = seenData_;
        std::atomic_thread_fence (std::memory_order_acquire);
        if (seenGen_.load (std::memory_order_relaxed) == g1) break;
    }
    out.writerStatus = writerStatus_;
    return out;
}

void EssaiCarteProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::MemoryOutputStream out (dest, false);
    out.writeInt (1);   // version
    out.writeInt (role_->getIndex());
    out.writeInt (channel_->get());
}

void EssaiCarteProcessor::setStateInformation (const void* data, int size)
{
    juce::MemoryInputStream in (data, static_cast<size_t> (size), false);
    if (size < 12 || in.readInt() != 1) return;
    const int r = in.readInt(), ch = in.readInt();
    *role_ = juce::jlimit (0, 1, r);
    *channel_ = juce::jlimit (1, kChannels, ch);
}

juce::AudioProcessorEditor* EssaiCarteProcessor::createEditor() { return new EssaiCarteEditor (*this); }

// ---------------------------------------------------------------------------------------------------- interface

EssaiCarteEditor::EssaiCarteEditor (EssaiCarteProcessor& p) : juce::AudioProcessorEditor (p), proc_ (p)
{
    role_.addItem (juce::String::fromUTF8 ("Écrivain (publie la carte)"), 1);
    role_.addItem (juce::String::fromUTF8 ("Lecteur (lit la carte)"), 2);
    role_.setSelectedId (p.role()->getIndex() + 1, juce::dontSendNotification);
    role_.onChange = [this] { proc_.role()->beginChangeGesture(); *proc_.role() = role_.getSelectedId() - 1; proc_.role()->endChangeGesture(); };
    for (int i = 1; i <= kChannels; ++i) channel_.addItem (juce::String (i), i);
    channel_.setSelectedId (p.channel()->get(), juce::dontSendNotification);
    channel_.onChange = [this] { proc_.channel()->beginChangeGesture(); *proc_.channel() = channel_.getSelectedId(); proc_.channel()->endChangeGesture(); };
    roleLabel_.setText (juce::String::fromUTF8 ("Rôle"), juce::dontSendNotification);
    channelLabel_.setText ("Canal", juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &role_, &channel_, &roleLabel_, &channelLabel_ }) addAndMakeVisible (c);
    setSize (460, 260);
    startTimerHz (10);
}

void EssaiCarteEditor::resized()
{
    roleLabel_.setBounds (12, 10, 60, 24);
    role_.setBounds (76, 10, 250, 24);
    channelLabel_.setBounds (336, 10, 50, 24);
    channel_.setBounds (390, 10, 56, 24);
}

void EssaiCarteEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14181f));
    g.setColour (juce::Colours::white);
    g.setFont (15.0f);
    const auto s = proc_.seen();
    juce::StringArray lines;
    lines.add (juce::String::fromUTF8 ("ESSAI de la carte d'accords partagée (pas le plugin final)"));
    if (proc_.role()->getIndex() == 0)
    {
        lines.add (s.writerStatus == static_cast<int> (Status::Ok) ? juce::String::fromUTF8 ("Écrivain : carte publiée sur le canal ") + juce::String (proc_.channel()->get())
                   : s.writerStatus == static_cast<int> (Status::InUse) ? juce::String::fromUTF8 ("Écrivain : canal déjà occupé par un autre écrivain vivant")
                                                                          : juce::String::fromUTF8 ("Écrivain : ouverture impossible"));
        lines.add (juce::String::fromUTF8 ("Carte d'exemple : Do | Fa | Sol | La mineur, 4 temps chacun"));
    }
    else
    {
        lines.add (juce::String::fromUTF8 ("Lecteur : ") + (s.connected ? juce::String::fromUTF8 ("connecté au canal ") + juce::String (proc_.channel()->get())
                                                                         : juce::String::fromUTF8 ("pas de carte sur le canal ") + juce::String (proc_.channel()->get())));
        lines.add (s.hasPosition ? juce::String::fromUTF8 ("Position : ") + juce::String (s.beats, 2) + " temps" : juce::String::fromUTF8 ("Position : inconnue (hôte à l'arrêt ?)"));
        lines.add (s.hasChord ? juce::String::fromUTF8 ("Accord : ") + chordName (s.chord) : juce::String::fromUTF8 ("Accord : aucun"));
        lines.add (s.hasNext ? juce::String::fromUTF8 ("Prochain : ") + chordName (s.next) + juce::String::fromUTF8 (" à ") + juce::String (s.next.startBeats, 1) + " temps" : juce::String::fromUTF8 ("Prochain : aucun"));
        lines.add (s.hasKey ? juce::String::fromUTF8 ("Tonalité : ") + juce::String::fromUTF8 (kNotes[s.key.root % 12]) + " majeur" : juce::String::fromUTF8 ("Tonalité : aucune"));
        lines.add ("Publication n° " + juce::String (static_cast<juce::int64> (s.sequence)));
    }
    int y = 50;
    for (const auto& l : lines) { g.drawText (l, 12, y, getWidth() - 24, 22, juce::Justification::left); y += 26; }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EssaiCarteProcessor(); }
