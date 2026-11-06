#pragma once
// Pilote Carte : ce qu'un instrument (Orgamon, AnalogSynth, FM6Op) ajoute pour jouer d'après la carte partagée. Pendant la lecture de l'hôte il écrit des notes MIDI dans le
// tampon de l'instrument : l'accord de Carte tenu (voix conduites), ou un motif (celui choisi, ou ceux que la voie « Motif » de Carte appelle), plus un motif de pédale.
// Tout est dans le pilote : lecteur de la carte, 16 emplacements de motifs (copiés dans l'état du projet), réglages, menu, bibliothèque de fichiers.
// Fil audio : `process`. Fil de messages : tout le reste.
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "MotifLibrary.h"
#include "neptune/core/Arpeggiator.h"
#include "neptune/core/ChordFollower.h"
#include "neptune/core/ChordMapEnvironment.h"
#include "neptune/core/ChordTimeline.h"
#include "neptune/core/SharedChordMap.h"

class CarteDriver : private juce::Timer
{
public:
    static constexpr int kMotifSlots = 16;

    // Canaux MIDI par défaut du clavier et de la pédale (0 : pas de pédale) ; `name` : l'instrument, écrit dans les fichiers de motifs exportés.
    CarteDriver (int defaultChordChannel, int defaultBassChannel, const char* name) : name_ (name)
    {
        chordChannel_.store (defaultChordChannel);
        bassChannel_.store (defaultBassChannel);
        env_ = std::make_unique<neptune::core::arp::ChordMapEnvironment> (reader_, audioMotifs_);   // Carte appelle les motifs par leur numéro : ce sont les emplacements du pilote
        pedalEnv_.base = env_.get();
        startTimer (500);
    }
    ~CarteDriver() override { stopTimer(); }

    void prepare (double sampleRate)
    {
        arp_.setSampleRate (sampleRate);
        arp_.reset();
        pedalArp_.setSampleRate (sampleRate);
        pedalArp_.reset();
    }

    // Ajoute les notes de ce bloc à `midi` (à leur instant dans le bloc).
    void process (juce::AudioPlayHead* head, double sampleRate, int n, juce::MidiBuffer& midi)
    {
        namespace arp = neptune::core::arp;
        namespace pattern = neptune::core::pattern;
        bool playing = false, hasPos = false;
        double ppq = 0.0, bpm = 120.0;
        arp::Transport tr;
        if (head != nullptr)
            if (const auto info = head->getPosition())
            {
                playing = info->getIsPlaying();
                if (const auto p = info->getPpqPosition()) { hasPos = true; ppq = *p; }
                if (const auto b = info->getBpm()) bpm = *b;
                if (const auto ts = info->getTimeSignature())
                    if (ts->denominator > 0) tr.beatsPerBar = 4.0 * ts->numerator / ts->denominator;
                if (const auto bar = info->getPpqPositionOfLastBarStart()) tr.barStartPpq = *bar;
            }
        const bool follow = follow_.load (std::memory_order_relaxed);
        {
            const int ver = motifsVersion_.load (std::memory_order_relaxed);
            if (ver != audioMotifsVersion_)
            {
                const juce::SpinLock::ScopedTryLockType l (motifLock_);
                if (l.isLocked()) { audioMotifs_ = motifs_; audioMotifsVersion_ = ver; }
            }
        }
        auto slotMotif = [&] (int slot) -> const pattern::Pattern*
        {
            return (slot >= 1 && slot <= kMotifSlots && audioMotifsVersion_ == motifsVersion_.load (std::memory_order_relaxed)) ? audioMotifs_[(size_t) slot - 1].get() : nullptr;
        };
        const pattern::Pattern* motif = slotMotif (motifSlot_.load (std::memory_order_relaxed));
        const pattern::Pattern* pedalMotif = slotMotif (pedalSlot_.load (std::memory_order_relaxed));
        // le clavier joue des motifs si un motif est choisi ou si Carte en appelle (voie des motifs) ; sinon le suiveur tient l'accord
        const bool usePattern = follow && (motif != nullptr || env_->hasPatterns());
        const bool usePedal = follow && pedalMotif != nullptr;
        const int chordCh = chordChannel_.load (std::memory_order_relaxed) - 1, chordCh2 = chordChannel2_.load (std::memory_order_relaxed) - 1;
        const int bassCh = bassChannel_.load (std::memory_order_relaxed) - 1;
        const bool led = led_.load (std::memory_order_relaxed);
        const auto put = [&] (bool on, int ch, int note, int vel, int at)
        {
            if (ch < 0) return;
            midi.addEvent (on ? juce::MidiMessage::noteOn (ch + 1, note, static_cast<juce::uint8> (vel)) : juce::MidiMessage::noteOff (ch + 1, note), at);
        };
        const int baseNote = 12 * (octave_.load (std::memory_order_relaxed) + 1);
        follower_.setChordChannel (chordCh);
        follower_.setVoiceLeading (led);
        follower_.setBassChannel (usePedal ? -1 : bassCh);   // un motif de pédale remplace la basse tenue
        follower_.setBaseNote (baseNote);
        const int m = follower_.process (*env_, follow && ! usePattern && playing, hasPos, ppq, bpm, sampleRate, n, followOut_.data(), static_cast<int> (followOut_.size()));
        for (int i = 0; i < m; ++i)
        {
            const auto& e = followOut_[static_cast<size_t> (i)];
            put (e.on, e.channel, e.note, 100, e.offset);
            if (e.channel == chordCh) put (e.on, chordCh2, e.note, 100, e.offset);
        }
        tr.hasPosition = hasPos;
        tr.ppq = ppq;
        tr.bpm = bpm;
        arp::Params ap;
        ap.gate = 0.9;
        ap.fixedVelocity = true;
        ap.velocity = 100;
        arp::ApproachParams approach;
        approach.kind = static_cast<arp::ApproachParams::Kind> (approach_.load (std::memory_order_relaxed));
        approach.chance = approachChance_.load (std::memory_order_relaxed) / 100.0;
        approach.below = approachBelow_.load (std::memory_order_relaxed) / 100.0;
        // clavier : le motif choisi (ou celui que Carte appelle) ; les parties B vont à la pédale, sauf si la pédale a son propre motif
        arp_.setParams (ap);
        arp_.setPattern (usePattern ? motif : nullptr);
        arp_.setEnvironment (usePattern ? env_.get() : nullptr);
        arp_.setBaseNote (baseNote);
        arp_.setVoiceLeading (led);
        arp_.setApproach (approach);
        tr.playing = usePattern && playing;
        const int k = arp_.process (nullptr, 0, tr, n, arpOut_.data(), static_cast<int> (arpOut_.size()));
        for (int i = 0; i < k; ++i)
        {
            const auto& e = arpOut_[static_cast<size_t> (i)];
            if (e.bass)
            {
                if (! usePedal) put (e.on, bassCh, e.note, e.velocity, e.offset);   // pas de pédale ou motif de pédale : la partie B se tait (début et fin)
            }
            else
            {
                put (e.on, chordCh, e.note, e.velocity, e.offset);
                put (e.on, chordCh2, e.note, e.velocity, e.offset);
            }
        }
        // pédale : son motif, toutes ses notes sur le canal de la pédale
        pedalArp_.setParams (ap);
        pedalArp_.setPattern (usePedal ? pedalMotif : nullptr);
        pedalArp_.setEnvironment (usePedal ? &pedalEnv_ : nullptr);
        pedalArp_.setBaseNote (baseNote - 12);
        pedalArp_.setVoiceLeading (led);
        pedalArp_.setApproach (approach);
        tr.playing = usePedal && playing;
        const int kp = pedalArp_.process (nullptr, 0, tr, n, pedalOut_.data(), static_cast<int> (pedalOut_.size()));
        for (int i = 0; i < kp; ++i)
        {
            const auto& e = pedalOut_[static_cast<size_t> (i)];
            put (e.on, bassCh, e.note, e.velocity, e.offset);
        }
        neptune::core::arp::Harmony hm;
        if (follow && playing && hasPos && reader_.connected() && env_->harmonyAt (ppq, hm)) chordNow_.store (hm.rootPc | static_cast<int> (hm.rel) << 8);
        else chordNow_.store (-2);
    }

    // « Carte : ON · canal 1 · Dm7 »
    juce::String text() const
    {
        if (! follow_.load()) return juce::String::fromUTF8 ("Carte : OFF");
        juce::String s = juce::String::fromUTF8 ("Carte : ON  ·  canal ") + juce::String (channel_.load());
        if (! reader_.connected()) return s + juce::String::fromUTF8 ("  ·  non connectée");
        const int v = chordNow_.load();
        if (v < 0) return s + juce::String::fromUTF8 ("  ·  en attente de la lecture");
        neptune::core::chords::Chord c;
        c.root = v & 0xFF;
        c.rel = static_cast<std::uint16_t> (v >> 8 & 0xFFFF);
        return s + "  ·  " + juce::String::fromUTF8 (neptune::core::chords::formatChord (c, neptune::core::chords::Naming::Letters).c_str());
    }

    // ---- réglages (fil de messages)
    bool follow() const noexcept { return follow_.load(); }
    void setFollow (bool on) { follow_.store (on); }
    int channel() const noexcept { return channel_.load(); }                 // canal de liaison avec Carte, 1..8
    void setChannel (int c) { channel_.store (juce::jlimit (1, 8, c)); tick(); }
    int chordChannel() const noexcept { return chordChannel_.load(); }       // canal MIDI du clavier, 1..16
    void setChordChannel (int c) { chordChannel_.store (juce::jlimit (1, 16, c)); }
    int chordChannel2() const noexcept { return chordChannel2_.load(); }     // second clavier recevant les mêmes notes, 0 : aucun
    void setChordChannel2 (int c) { chordChannel2_.store (juce::jlimit (0, 16, c)); }
    int bassChannel() const noexcept { return bassChannel_.load(); }         // canal MIDI de la pédale, 0 : pas de pédale, 1..16
    void setBassChannel (int c) { bassChannel_.store (juce::jlimit (0, 16, c)); }
    int octave() const noexcept { return octave_.load(); }                   // octave de base, 1..6 (3 = do 48)
    void setOctave (int o) { octave_.store (juce::jlimit (1, 6, o)); }
    bool led() const noexcept { return led_.load(); }                        // voix conduites (défaut) ou position fixe
    void setLed (bool on) { led_.store (on); }
    int approach() const noexcept { return approach_.load(); }               // 0 non, 1 dominante, 2 triton, 3 chromatique, 4 parallèle
    void setApproach (int k) { approach_.store (juce::jlimit (0, 4, k)); }
    int approachChance() const noexcept { return approachChance_.load(); }   // % des changements
    void setApproachChance (int pct) { approachChance_.store (juce::jlimit (0, 100, pct)); }
    int approachBelow() const noexcept { return approachBelow_.load(); }     // % des approches parallèles venues d'en dessous
    void setApproachBelow (int pct) { approachBelow_.store (juce::jlimit (0, 100, pct)); }
    int motifSlot() const noexcept { return motifSlot_.load(); }             // motif du clavier (0 : l'accord est tenu)
    void setMotifSlot (int s) { motifSlot_.store (juce::jlimit (0, kMotifSlots, s)); }
    int pedalMotifSlot() const noexcept { return pedalSlot_.load(); }        // motif de la pédale (0 : la partie B du motif du clavier)
    void setPedalMotifSlot (int s) { pedalSlot_.store (juce::jlimit (0, kMotifSlots, s)); }

    // ---- emplacements de motifs
    bool motifUsed (int i) const
    {
        const juce::SpinLock::ScopedLockType l (motifLock_);
        return i >= 0 && i < kMotifSlots && motifs_[(size_t) i] != nullptr;
    }
    juce::String motifName (int i) const
    {
        const juce::SpinLock::ScopedLockType l (motifLock_);
        if (i < 0 || i >= kMotifSlots || motifs_[(size_t) i] == nullptr) return {};
        const auto& p = *motifs_[(size_t) i];
        return juce::String::fromUTF8 ((p.name.empty() ? p.id : p.name).c_str());
    }
    void setMotif (int i, const neptune::core::pattern::Pattern* p)   // nullptr : vide l'emplacement
    {
        if (i < 0 || i >= kMotifSlots) return;
        std::shared_ptr<const neptune::core::pattern::Pattern> np;
        if (p != nullptr) np = std::make_shared<const neptune::core::pattern::Pattern> (*p);
        {
            const juce::SpinLock::ScopedLockType l (motifLock_);
            if (motifs_[(size_t) i] != nullptr) retiredMotifs_.push_back (motifs_[(size_t) i]);   // le fil audio peut encore le lire : on le garde en vie
            if (retiredMotifs_.size() > 64) retiredMotifs_.erase (retiredMotifs_.begin(), retiredMotifs_.begin() + 32);
            motifs_[(size_t) i] = std::move (np);
        }
        motifsVersion_.fetch_add (1);
    }

    // ---- état du projet : un objet JSON « carte »
    void saveState (nlohmann::json& doc) const
    {
        doc["carte"] = { { "follow", follow_.load() }, { "channel", channel_.load() }, { "chordChannel", chordChannel_.load() }, { "bassChannel", bassChannel_.load() },
                         { "octave", octave_.load() }, { "led", led_.load() }, { "motifSlot", motifSlot_.load() }, { "approach", approach_.load() },
                         { "chordChannel2", chordChannel2_.load() }, { "pedalSlot", pedalSlot_.load() }, { "approachChance", approachChance_.load() },
                         { "approachBelow", approachBelow_.load() } };
        auto arr = nlohmann::json::array();
        {
            const juce::SpinLock::ScopedLockType l (motifLock_);
            for (const auto& mp : motifs_) arr.push_back (mp != nullptr ? neptune::core::pattern::toJson ({ *mp }, name_) : std::string());
        }
        doc["carte"]["motifs"] = arr;
    }
    // `defaultBass` : valeur d'un état qui n'a pas encore de clé « bassChannel ».
    void loadState (const nlohmann::json& doc)
    {
        if (! doc.is_object() || ! doc.contains ("carte") || ! doc["carte"].is_object()) return;
        const auto& c = doc["carte"];
        follow_.store (c.value ("follow", false));
        channel_.store (juce::jlimit (1, 8, c.value ("channel", 1)));
        chordChannel_.store (juce::jlimit (1, 16, c.value ("chordChannel", chordChannel_.load())));
        bassChannel_.store (juce::jlimit (0, 16, c.value ("bassChannel", bassChannel_.load())));
        octave_.store (juce::jlimit (1, 6, c.value ("octave", 3)));
        led_.store (c.value ("led", true));
        motifSlot_.store (juce::jlimit (0, kMotifSlots, c.value ("motifSlot", 0)));
        approach_.store (juce::jlimit (0, 4, c.value ("approach", 0)));
        chordChannel2_.store (juce::jlimit (0, 16, c.value ("chordChannel2", 0)));
        pedalSlot_.store (juce::jlimit (0, kMotifSlots, c.value ("pedalSlot", 0)));
        approachChance_.store (juce::jlimit (0, 100, c.value ("approachChance", 100)));
        approachBelow_.store (juce::jlimit (0, 100, c.value ("approachBelow", 0)));
        for (int i = 0; i < kMotifSlots; ++i)
        {
            neptune::core::pattern::Pattern pt;
            bool ok = false;
            if (c.contains ("motifs") && c["motifs"].is_array() && (size_t) i < c["motifs"].size() && c["motifs"][(size_t) i].is_string())
            {
                const auto r = neptune::core::pattern::fromJson (c["motifs"][(size_t) i].get<std::string>());
                if (r.ok && ! r.patterns.empty()) { pt = r.patterns.front(); ok = true; }
            }
            setMotif (i, ok ? &pt : nullptr);
        }
    }

    // ---- bibliothèque de fichiers (fil de messages)
    bool saveMotifToLibrary (int slot)
    {
        neptune::core::pattern::Pattern pt;
        if (! copyMotif (slot, pt)) return false;
        return motifs::fileFor (pt).replaceWithText (juce::String (neptune::core::pattern::toJson ({ pt }, name_)));   // même nom : on remplace la version de la bibliothèque
    }
    void importMotifs()
    {
        chooser_ = std::make_shared<juce::FileChooser> (juce::String::fromUTF8 ("Importer des motifs (format d'Arpège ou de Scorsaire)"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.json");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [name = name_] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            const auto r = neptune::core::pattern::fromJson (f.loadFileAsString().toStdString());
            if (! r.ok) return;
            for (const auto& pt : r.patterns) freeLibraryFile (pt).replaceWithText (juce::String (neptune::core::pattern::toJson ({ pt }, name)));
        });
    }
    void exportMotif (int slot)
    {
        neptune::core::pattern::Pattern pt;
        if (! copyMotif (slot, pt)) return;
        chooser_ = std::make_shared<juce::FileChooser> (juce::String::fromUTF8 ("Exporter le motif"), motifs::fileFor (pt), "*.json");
        chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting, [pt, name = name_] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty()) f = f.withFileExtension ("json");
            f.replaceWithText (juce::String (neptune::core::pattern::toJson ({ pt }, name)));
        });
    }

    // ---- menu (style de la suite : celui du composant d'ancrage)
    void showMenu (juce::Component* anchor)
    {
        const auto U = [] (const char* t) { return juce::String::fromUTF8 (t); };
        auto* look = &anchor->getLookAndFeel();   // le style de la suite pour le menu et ses sous-menus (sinon celui de JUCE)
        const auto sub = [look] { juce::PopupMenu mm; mm.setLookAndFeel (look); return mm; };
        const auto library = motifs::files();   // lu à l'ouverture du menu
        const auto slotLabel = [&] (int i) { return juce::String (i + 1) + " : " + (motifUsed (i) ? motifName (i) : U ("(vide)")); };
        auto m = sub();
        m.addItem (1, U ("Suivre les accords de Carte"), true, follow());
        m.addItem (2, U ("Voix conduites (sinon position fixe)"), true, led());
        m.addSeparator();

        // clavier : upper, lower ou les deux (les accords tenus comme les motifs)
        auto kb = sub(), other = sub();
        const int c1 = chordChannel(), c2 = chordChannel2();
        kb.addItem (201, U ("Upper (canal 1)"), true, c1 == 1 && c2 == 0);
        kb.addItem (202, U ("Lower (canal 2)"), true, c1 == 2 && c2 == 0);
        kb.addItem (203, U ("Upper et lower"), true, c1 == 1 && c2 == 2);
        for (int i = 1; i <= 16; ++i) other.addItem (220 + i, juce::String (i), true, c2 == 0 && c1 == i);
        kb.addSubMenu (U ("Autre canal"), other);
        m.addSubMenu (U ("Clavier des accords et des motifs"), kb);

        auto pedal = sub();
        pedal.addItem (300, U ("aucune pédale"), true, bassChannel() == 0);
        for (int i = 1; i <= 16; ++i) pedal.addItem (300 + i, i == 3 ? U ("3 (pédale)") : juce::String (i), true, bassChannel() == i);
        m.addSubMenu (U ("Canal MIDI de la pédale"), pedal);
        m.addSeparator();

        auto play = sub(), playPedal = sub();
        play.addItem (500, U ("aucun (tenir l'accord)"), true, motifSlot() == 0);
        playPedal.addItem (700, U ("aucun (la partie B du motif du clavier)"), true, pedalMotifSlot() == 0);
        for (int i = 0; i < kMotifSlots; ++i)
        {
            play.addItem (501 + i, slotLabel (i), motifUsed (i), motifSlot() == i + 1);
            playPedal.addItem (701 + i, slotLabel (i), motifUsed (i), pedalMotifSlot() == i + 1);
        }
        m.addSubMenu (U ("Motif du clavier"), play);
        m.addSubMenu (U ("Motif de la pédale"), playPedal);

        auto appr = sub();
        {
            const char* names[] = { "aucun", "dominante (V7 de l'accord suivant)", "triton (bII7)", "chromatique (majeur un demi-ton plus bas)", "parallèle (l'accord suivant, un demi-ton plus haut)" };
            for (int k = 0; k < 5; ++k) appr.addItem (600 + k, U (names[k]), true, approach() == k);
            appr.addSeparator();
            const int pcts[] = { 25, 50, 75, 100 };
            for (int k = 0; k < 4; ++k) appr.addItem (810 + k, juce::String (pcts[k]) + U (" % des changements"), approach() != 0, approachChance() == pcts[k]);
            appr.addSeparator();
            appr.addItem (820, U ("parallèle : toujours au-dessus"), approach() == 4, approachBelow() == 0);
            appr.addItem (821, U ("parallèle : toujours en dessous"), approach() == 4, approachBelow() == 100);
            appr.addItem (822, U ("parallèle : au-dessus et en dessous"), approach() == 4, approachBelow() == 50);
        }
        m.addSubMenu (U ("Accord d'approche"), appr);
        m.addSeparator();

        // bibliothèque : charger, enregistrer, exporter, importer
        auto lib = sub(), load = sub(), save = sub(), exp = sub();
        for (int i = 0; i < kMotifSlots; ++i)
        {
            auto files = sub();
            files.addItem (10000 + i * 256, U ("vider l'emplacement"), motifUsed (i));
            for (int f = 0; f < library.size() && f < 200; ++f) files.addItem (10000 + i * 256 + 1 + f, library[f].getFileNameWithoutExtension());
            if (library.isEmpty()) files.addItem (-1, U ("(la bibliothèque est vide)"), false);
            load.addSubMenu (slotLabel (i), files);
            save.addItem (9100 + i, slotLabel (i), motifUsed (i));
            exp.addItem (9200 + i, slotLabel (i), motifUsed (i));
        }
        lib.addSubMenu (U ("Charger dans l'emplacement"), load);
        lib.addSubMenu (U ("Enregistrer l'emplacement dans la bibliothèque"), save);
        lib.addSubMenu (U ("Exporter l'emplacement vers un fichier"), exp);
        lib.addItem (9001, U ("Importer un fichier de motifs..."));
        lib.addSeparator();
        lib.addItem (9000, U ("Ouvrir le dossier de la bibliothèque"));
        m.addSubMenu (U ("Bibliothèque de motifs"), lib);
        m.addSeparator();

        auto ch = sub(), oct = sub();
        for (int i = 1; i <= 8; ++i) ch.addItem (100 + i, juce::String (i), true, channel() == i);
        for (int i = 1; i <= 6; ++i) oct.addItem (400 + i, juce::String (i) + (i == 3 ? U (" (do 48)") : juce::String()), true, octave() == i);
        m.addSubMenu (U ("Canal de Carte"), ch);
        m.addSubMenu (U ("Octave de base"), oct);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (anchor), [this] (int r) { onMenu (r); });
    }

private:
    void timerCallback() override { tick(); }
    void tick()
    {
        reader_.setChannel (channel_.load());
        reader_.poll();
    }

    bool copyMotif (int slot, neptune::core::pattern::Pattern& out) const
    {
        const juce::SpinLock::ScopedLockType l (motifLock_);
        if (slot < 0 || slot >= kMotifSlots || motifs_[(size_t) slot] == nullptr) return false;
        out = *motifs_[(size_t) slot];
        return true;
    }

    // Fichier libre dans la bibliothèque pour ce motif (« Valse.json », puis « Valse 2.json »...).
    static juce::File freeLibraryFile (const neptune::core::pattern::Pattern& pt)
    {
        auto f = motifs::fileFor (pt);
        for (int i = 2; f.existsAsFile() && i < 1000; ++i) f = f.getSiblingFile (f.getFileNameWithoutExtension().upToLastOccurrenceOf (" ", false, false).trimEnd() + " " + juce::String (i) + ".json");
        return f;
    }

    void onMenu (int r)
    {
        if (r == 1) setFollow (! follow());
        else if (r == 2) setLed (! led());
        else if (r > 100 && r <= 108) setChannel (r - 100);
        else if (r == 201) { setChordChannel (1); setChordChannel2 (0); }
        else if (r == 202) { setChordChannel (2); setChordChannel2 (0); }
        else if (r == 203) { setChordChannel (1); setChordChannel2 (2); }
        else if (r > 220 && r <= 236) { setChordChannel (r - 220); setChordChannel2 (0); }
        else if (r >= 300 && r <= 316) setBassChannel (r - 300);
        else if (r > 400 && r <= 406) setOctave (r - 400);
        else if (r >= 500 && r <= 516) setMotifSlot (r - 500);
        else if (r >= 600 && r <= 604) setApproach (r - 600);
        else if (r >= 700 && r <= 716) setPedalMotifSlot (r - 700);
        else if (r >= 810 && r <= 813) { const int pcts[] = { 25, 50, 75, 100 }; setApproachChance (pcts[r - 810]); }
        else if (r == 820) setApproachBelow (0);
        else if (r == 821) setApproachBelow (100);
        else if (r == 822) setApproachBelow (50);
        else if (r == 9000) motifs::directory().revealToUser();
        else if (r == 9001) importMotifs();
        else if (r >= 9100 && r < 9116) saveMotifToLibrary (r - 9100);
        else if (r >= 9200 && r < 9216) exportMotif (r - 9200);
        else if (r >= 10000 && r < 10000 + 16 * 256)
        {
            const int slot = (r - 10000) / 256, idx = (r - 10000) % 256;
            if (idx == 0)
            {
                setMotif (slot, nullptr);
                if (motifSlot() == slot + 1) setMotifSlot (0);
                if (pedalMotifSlot() == slot + 1) setPedalMotifSlot (0);
            }
            else
            {
                const auto files = motifs::files();
                neptune::core::pattern::Pattern pt;
                if (idx - 1 < files.size() && motifs::read (files[idx - 1], pt)) setMotif (slot, &pt);
            }
        }
    }

    std::string name_;
    neptune::core::chordmap::Reader reader_;
    std::unique_ptr<neptune::core::arp::ChordMapEnvironment> env_;
    neptune::core::arp::ChordFollower follower_;
    std::array<neptune::core::arp::FollowEvent, 64> followOut_ {};
    std::atomic<bool> follow_ { false }, led_ { true };
    std::atomic<int> channel_ { 1 }, chordChannel_ { 1 }, chordChannel2_ { 0 }, bassChannel_ { 3 }, octave_ { 3 };
    std::atomic<int> approach_ { 0 }, approachChance_ { 100 }, approachBelow_ { 0 }, pedalSlot_ { 0 }, motifSlot_ { 0 };
    // Motifs (fil de messages : motifs_ sous verrou ; fil audio : copie audioMotifs_ refaite quand la version change)
    mutable juce::SpinLock motifLock_;
    neptune::core::arp::ChordMapEnvironment::Slots motifs_, audioMotifs_;
    std::vector<std::shared_ptr<const neptune::core::pattern::Pattern>> retiredMotifs_;
    std::atomic<int> motifsVersion_ { 0 };
    int audioMotifsVersion_ = -1;
    // la pédale a son propre moteur de motifs : il ne lit de Carte que l'harmonie (pas la voie des motifs, qui est pour le clavier)
    struct HarmonyOnly : neptune::core::arp::Environment
    {
        const neptune::core::arp::Environment* base = nullptr;
        bool harmonyAt (double t, neptune::core::arp::Harmony& h) const noexcept override { return base != nullptr && base->harmonyAt (t, h); }
        bool measureAt (double t, double& a, double& b, int& c) const noexcept override { return base != nullptr && base->measureAt (t, a, b, c); }
        bool nextChordStart (double t, double& s) const noexcept override { return base != nullptr && base->nextChordStart (t, s); }
        bool nextMeasureStart (double t, double& s) const noexcept override { return base != nullptr && base->nextMeasureStart (t, s); }
    } pedalEnv_;
    neptune::core::arp::Arpeggiator arp_, pedalArp_;
    std::array<neptune::core::arp::NoteOut, 256> arpOut_ {}, pedalOut_ {};
    std::atomic<int> chordNow_ { -2 };   // -2 : pas en lecture ou pas de Carte ; sinon racine | rel << 8
    std::shared_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CarteDriver)
};
