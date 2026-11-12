#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wunused-parameter", "-Wextra-semi", "-Wnon-virtual-dtor")
#include <clap-juce-extensions/clap-juce-extensions.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include <algorithm>
#include "HostProbe.h"
#include "neptune/core/Parameter.h"

// Vue d'un paramètre du cœur pour l'hôte. La source de vérité reste neptune::core::ParameterSet :
// cette classe ne fait que traduire (valeur normalisée, texte, métadonnées).
// Les paramètres « modulables » du cœur acceptent la modulation CLAP monophonique (hôte), qui s'ajoute à la valeur sans la changer.
class NeptuneParameter : public juce::HostedAudioProcessorParameter,
                         public clap_juce_extensions::clap_juce_parameter_capabilities
{
public:
    NeptuneParameter (neptune::core::ParameterSet& set, std::size_t index, HostProbe& probe)
        : set_ (set), index_ (index), spec_ (set.spec (index)), probe_ (probe) {}

    // Modulation globale de l'hôte seulement (la modulation par note n'est pas prévue : elle n'est pas annoncée).
    // Le montant est normalisé (le paramètre exposé à l'hôte va de 0 à 1) et se range dans l'emplacement de l'hôte, séparé de la matrice.
    bool supportsMonophonicModulation() override { return spec_.modulatable; }
    void applyMonophonicModulation (double amount) override
    {
        probe_.lastModAmount = amount;
        ++probe_.monoModCount;
        set_.setHostModOffsetAt (index_, static_cast<float> (std::clamp (amount, -1.0, 1.0)));
    }
    bool supportsPolyphonicModulation() override { return false; }

    // Identifiant stable pour l'automation : l'id du cœur, en texte (clap-juce-extensions en tire le clap_id par hachage).
    juce::String getParameterID() const override { return juce::String (static_cast<juce::int64> (spec_.id)); }

    float getValue() const override { return static_cast<float> (set_.getNormalizedAt (index_)); }
    void setValue (float newValue) override { set_.setNormalizedAt (index_, static_cast<double> (newValue)); }
    float getDefaultValue() const override { return static_cast<float> (spec_.toNormalized (spec_.defaultValue)); }

    // Nom affiché à l'hôte, modifiable (les emplacements de la réserve d'automation portent le nom du réglage qu'ils pilotent). Vide : le nom du paramètre.
    void setDisplayName (const juce::String& name)
    {
        const juce::SpinLock::ScopedLockType lock (nameLock_);
        displayName_ = name;
    }

    juce::String getName (int maximumStringLength) const override
    {
        juce::String shown;
        {
            const juce::SpinLock::ScopedLockType lock (nameLock_);
            shown = displayName_;
        }
        const auto s = shown.isNotEmpty() ? shown : juce::String::fromUTF8 (spec_.name.c_str());
        return maximumStringLength > 0 ? s.substring (0, maximumStringLength) : s;
    }
    juce::String getLabel() const override { return {}; } // l'unité est incluse dans le texte

    int getNumSteps() const override
    {
        using neptune::core::Kind;
        if (spec_.kind == Kind::Continuous) return AudioProcessorParameter::getNumSteps();
        return static_cast<int> (spec_.max - spec_.min) + 1;
    }
    bool isDiscrete() const override { return spec_.kind != neptune::core::Kind::Continuous; }
    bool isBoolean() const override { return spec_.kind == neptune::core::Kind::Toggle; }
    bool isAutomatable() const override { return spec_.automatable; }

    juce::String getText (float normalised, int maximumStringLength) const override
    {
        const auto s = juce::String::fromUTF8 (set_.toText (spec_, spec_.fromNormalized (normalised)).c_str());
        return maximumStringLength > 0 ? s.substring (0, maximumStringLength) : s;
    }
    float getValueForText (const juce::String& text) const override
    {
        double plain = 0.0;
        if (set_.fromText (spec_.id, text.toStdString(), plain))
            return static_cast<float> (spec_.toNormalized (plain));
        // Le wrapper CLAP passe son `const char*` UTF-8 à juce::String, qui le lit comme du Latin-1 : « Médium » arrive en « MÃ©dium ».
        // Chaque caractère reçu est alors un octet du texte d'origine : on les remet bout à bout.
        std::string bytes;
        bool latin1 = true;
        for (auto c : text) { if (c > 255) { latin1 = false; break; } bytes.push_back (static_cast<char> (c)); }
        if (latin1 && set_.fromText (spec_.id, bytes, plain))
            return static_cast<float> (spec_.toNormalized (plain));
        return getValue();
    }

private:
    mutable juce::SpinLock nameLock_;
    juce::String displayName_;
    neptune::core::ParameterSet& set_;
    std::size_t index_;
    const neptune::core::ParamSpec& spec_;
    HostProbe& probe_;
};
