# Identifiant de construction (affiché dans les interfaces de développement) : hash court du dépôt au moment de la configuration.
execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD OUTPUT_VARIABLE NEPTUNE_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT NEPTUNE_GIT_HASH)
  set(NEPTUNE_GIT_HASH "nogit")
endif()
add_compile_definitions(NEPTUNE_GIT_HASH="${NEPTUNE_GIT_HASH}")

# Nombre de pistes du pool de paramètres « sonde » (palier 0, point 4). 32 pistes = 676 paramètres.
set(NEPTUNE_PROBE_TRACKS 16 CACHE STRING "Pistes du mixeur (paramètres exposés à l'hôte)")

# Sorties stéréo déclarées à l'hôte (décision 2026-10-15 : 32 stéréo = 64 canaux). Le wrapper plafonne à 128 canaux de sortie.
set(NEPTUNE_PROBE_OUTPUTS 32 CACHE STRING "Sorties stéréo de la sonde")

# Identité provisoire : domaine inversé du CLAP_ID à confirmer.
set(NEPTUNE_CLAP_ID "com.tournedosrossini.neptune" CACHE STRING "Identifiant CLAP")

juce_add_plugin(Neptune
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Neptune"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Nept
  IS_SYNTH                    TRUE
  NEEDS_MIDI_INPUT            TRUE
  FORMATS                     Standalone)   # Standalone = banc d'essai local ; le .clap vient de clap-juce-extensions

clap_juce_extensions_plugin(
  TARGET        Neptune
  CLAP_ID       "${NEPTUNE_CLAP_ID}"
  CLAP_FEATURES instrument sampler stereo)

target_sources(Neptune PRIVATE
  src/PluginProcessor.cpp
  src/PluginEditor.cpp
  src/SlotsPanel.cpp
  src/OscServer.cpp
  src/ControlPanel.cpp
  src/AudioInView.cpp
  src/EnvelopeEditor.cpp
  src/ImportWizard.cpp
  src/ZoneTable.cpp
  src/MappingMatrix.cpp
  src/VelocityPanel.cpp
  src/PolyphonyPanel.cpp
  src/ChokePanel.cpp
  src/VoicesView.cpp
  src/FilterEditor.cpp
  src/MixerView.cpp
  src/FacadeView.cpp
  src/ModView.cpp
  src/RouterView.cpp
  src/SynthKeyboard.cpp
  src/PresetManager.cpp
  src/MidiFilterEditor.cpp
  src/FmSysexIO.cpp
  src/UtilityEditor.cpp
  src/InsertOverlay.cpp
  src/InstrumentPanel.cpp
  src/NovaSamplerEditor.cpp
  src/NovaWaveView.cpp
  src/NovaOrganEditor.cpp
  src/NovaSynthEditor.cpp
  src/NovaFmEditor.cpp
  src/NovaInstrumentBase.cpp
  src/NovaPresetUi.cpp
  src/NovaPresetFields.cpp
  src/NovaShaperEditor.cpp
  src/NovaShaperWaveView.cpp
  src/NovaDelayEditor.cpp
  src/NovaChorusEditor.cpp
  src/NovaMicrodecaleEditor.cpp
  src/NovaConvolutionEditor.cpp
  src/NovaFournaiseEditor.cpp
  src/NovaCompressorEditor.cpp
  src/NovaHistoryView.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NovaEqualizerEditor.cpp
  src/NovaSpectrum.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Neptune PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  NEPTUNE_PROBE_TRACKS=${NEPTUNE_PROBE_TRACKS}
  NEPTUNE_PROBE_OUTPUTS=${NEPTUNE_PROBE_OUTPUTS})

target_link_libraries(Neptune
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Moussaillon (nom provisoire) : l'Utilitaire de la suite, plugin CLAP indépendant ---------------------------------------
# Même DSP (neptune_core : StockModule / UtilityModule) et même interface (UtilityEditor, NeptuneKnob, KnobModal) que l'insert
# de Neptune ; seul l'adaptateur change (MoussaillonPlugin : l'hôte porte le plugin).
set(MOUSSAILLON_CLAP_ID "com.tournedosrossini.moussaillon" CACHE STRING "Identifiant CLAP de l'Utilitaire")

juce_add_plugin(Moussaillon
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Moussaillon"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Mous
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Moussaillon
  CLAP_ID       "${MOUSSAILLON_CLAP_ID}"
  CLAP_FEATURES audio-effect utility stereo)

target_sources(Moussaillon PRIVATE
  src/MoussaillonPlugin.cpp
  src/UtilityEditor.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Moussaillon PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Moussaillon
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- La Flèche : le shaper de transitoires (série NOVA), plugin CLAP effet indépendant ---------------------------------------------
# Même DSP (neptune_core : ShaperModule) et même interface (NovaShaperEditor) que l'insert de Neptune ; entrée sidechain de l'hôte.
set(LAFLECHE_CLAP_ID "com.tournedosrossini.lafleche" CACHE STRING "Identifiant CLAP du shaper de transitoires")

juce_add_plugin(LaFleche
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "LaFleche"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Flch
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        LaFleche
  CLAP_ID       "${LAFLECHE_CLAP_ID}"
  CLAP_FEATURES audio-effect transient-shaper stereo)

target_sources(LaFleche PRIVATE
  src/ShaperPlugin.cpp
  src/NovaShaperEditor.cpp
  src/NovaShaperWaveView.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(LaFleche PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(LaFleche
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Compresseur (nom provisoire) : compresseur porté de ZL Compressor (AGPLv3), plugin CLAP effet indépendant ------------------------------
set(COMPRESSEUR_CLAP_ID "com.tournedosrossini.compresseur" CACHE STRING "Identifiant CLAP du compresseur")

juce_add_plugin(Compresseur
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Compresseur"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Comp
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            TRUE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Compresseur
  CLAP_ID       "${COMPRESSEUR_CLAP_ID}"
  CLAP_FEATURES audio-effect compressor stereo)

target_sources(Compresseur PRIVATE
  src/CompressorPlugin.cpp
  src/NovaCompressorEditor.cpp
  src/NovaHistoryView.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Compresseur PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Compresseur
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Egaliseur (nom provisoire) : égaliseur porté de ZL Equalizer (AGPLv3), plugin CLAP effet indépendant ------------------------------
set(EGALISEUR_CLAP_ID "com.tournedosrossini.egaliseur" CACHE STRING "Identifiant CLAP de l'égaliseur")

juce_add_plugin(Egaliseur
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Egaliseur"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Eqlz
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Egaliseur
  CLAP_ID       "${EGALISEUR_CLAP_ID}"
  CLAP_FEATURES audio-effect equalizer stereo)

target_sources(Egaliseur PRIVATE
  src/EqualizerPlugin.cpp
  src/NovaEqualizerEditor.cpp
  src/NovaSpectrum.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Egaliseur PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Egaliseur
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)


# ---- Delai (nom provisoire) : delay à huit lignes (série NOVA), plugin CLAP effet indépendant ------------------------------
set(DELAI_CLAP_ID "com.tournedosrossini.delai" CACHE STRING "Identifiant CLAP du delay")

juce_add_plugin(Delai
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Delai"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Dely
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Delai
  CLAP_ID       "${DELAI_CLAP_ID}"
  CLAP_FEATURES audio-effect delay stereo)

target_sources(Delai PRIVATE
  src/DelayPlugin.cpp
  src/NovaDelayEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Delai PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Delai
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)


# ---- Choeur (nom provisoire) : chorus (série NOVA, moteur standard et mode TAL), plugin CLAP effet indépendant ------------------------------
set(CHOEUR_CLAP_ID "com.tournedosrossini.choeur" CACHE STRING "Identifiant CLAP du chorus")

juce_add_plugin(Choeur
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Choeur"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Chor
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Choeur
  CLAP_ID       "${CHOEUR_CLAP_ID}"
  CLAP_FEATURES audio-effect chorus stereo)

target_sources(Choeur PRIVATE
  src/ChorusPlugin.cpp
  src/NovaChorusEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Choeur PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Choeur
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)


# ---- Microdecale (nom provisoire) : micro-décalage de hauteur de type H3000 MICROPITCHSHIFT (série NOVA), plugin CLAP effet indépendant ------------------------------
set(MICRODECALE_CLAP_ID "com.tournedosrossini.microdecale" CACHE STRING "Identifiant CLAP de Microdécale")

juce_add_plugin(Microdecale
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Microdecale"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Micr
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Microdecale
  CLAP_ID       "${MICRODECALE_CLAP_ID}"
  CLAP_FEATURES audio-effect pitch-correction stereo)

target_sources(Microdecale PRIVATE
  src/MicrodecalePlugin.cpp
  src/NovaMicrodecaleEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Microdecale PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Microdecale
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Convolution (nom provisoire) : chargeur de réponses impulsionnelles à latence nulle (série NOVA, moteur LowLatConv), plugin CLAP effet indépendant ------------------------------
set(CONVOLUTION_CLAP_ID "com.tournedosrossini.convolution" CACHE STRING "Identifiant CLAP de Convolution")

juce_add_plugin(Convolution
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Convolution"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Conv
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Convolution
  CLAP_ID       "${CONVOLUTION_CLAP_ID}"
  CLAP_FEATURES audio-effect reverb stereo)

target_sources(Convolution PRIVATE
  src/ConvolutionPlugin.cpp
  src/NovaConvolutionEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Convolution PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Convolution
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- AnalogSynth (nom provisoire) : le synthé soustractif de Neptune, plugin CLAP instrument indépendant --------------------
# Même moteur (neptune_core : AnalogSynth.h, Synth.h, SynthVoices.h) que la piste Synthé de Neptune, interface NOVA (NovaSynthEditor) ;
# en plus, le système de presets (Preset.h, PresetLibrary.h : banques MIDI, grille concert, corbeille) et son gestionnaire.
set(ANALOGSYNTH_CLAP_ID "com.tournedosrossini.analogsynth" CACHE STRING "Identifiant CLAP du synthé indépendant")

juce_add_plugin(AnalogSynth
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "AnalogSynth"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Anls
  IS_SYNTH                    TRUE
  NEEDS_MIDI_INPUT            TRUE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        AnalogSynth
  CLAP_ID       "${ANALOGSYNTH_CLAP_ID}"
  CLAP_FEATURES instrument synthesizer stereo)

target_sources(AnalogSynth PRIVATE
  src/AnalogSynthPlugin.cpp
  src/NovaSynthEditor.cpp
  src/NovaInstrumentBase.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NovaPresetUi.cpp
  src/NovaPresetFields.cpp
  src/PresetManager.cpp
  src/SynthKeyboard.cpp
  src/EnvelopeEditor.cpp
  src/ModView.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(AnalogSynth PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AnalogSynth
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- FM6Op (nom provisoire) : le module FM de type DX7 de Neptune, plugin CLAP instrument indépendant ---------------------------
# Même moteur (neptune_core : msfa via FmEngine / FmVoices / FmSynth) que le type de piste FM, interface NOVA (NovaFmEditor) ; presets
# partagés avec Neptune et AnalogSynth ; import / export sysex DX7.
set(FM6OP_CLAP_ID "com.tournedosrossini.fm6op" CACHE STRING "Identifiant CLAP du synthé FM indépendant")

juce_add_plugin(FM6Op
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "FM6Op"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Fm6o
  IS_SYNTH                    TRUE
  NEEDS_MIDI_INPUT            TRUE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        FM6Op
  CLAP_ID       "${FM6OP_CLAP_ID}"
  CLAP_FEATURES instrument synthesizer stereo)

target_sources(FM6Op PRIVATE
  src/FmPlugin.cpp
  src/NovaFmEditor.cpp
  src/NovaInstrumentBase.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NovaPresetUi.cpp
  src/NovaPresetFields.cpp
  src/FmSysexIO.cpp
  src/PresetManager.cpp
  src/SynthKeyboard.cpp
  src/EnvelopeEditor.cpp
  src/ModView.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(FM6Op PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FM6Op
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Orgamon (nom provisoire) : l'orgue à roues phoniques de Neptune (moteur de setBfree), plugin CLAP instrument indépendant ------
set(ORGAMON_CLAP_ID "com.tournedosrossini.orgamon" CACHE STRING "Identifiant CLAP de l'orgue indépendant")

juce_add_plugin(Orgamon
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Orgamon"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Orgm
  IS_SYNTH                    TRUE
  NEEDS_MIDI_INPUT            TRUE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Orgamon
  CLAP_ID       "${ORGAMON_CLAP_ID}"
  CLAP_FEATURES instrument synthesizer stereo)

target_sources(Orgamon PRIVATE
  src/OrganPlugin.cpp
  src/NovaOrganEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NovaPresetUi.cpp
  src/NovaPresetFields.cpp
  src/PresetManager.cpp
  src/SynthKeyboard.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Orgamon PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Orgamon
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Diapason (nom provisoire) : générateur de signaux de test et petite boîte à rythme, plugin CLAP instrument indépendant ------------------------------
# Cœur : SignalGenerator (signaux, bruits, balayages, huit sons de batterie calculés, séquenceur d'une mesure) ; interface NOVA (NovaDiapasonEditor).
set(DIAPASON_CLAP_ID "com.tournedosrossini.diapason" CACHE STRING "Identifiant CLAP de Diapason")

juce_add_plugin(Diapason
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Diapason"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Diap
  IS_SYNTH                    TRUE
  NEEDS_MIDI_INPUT            TRUE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Diapason
  CLAP_ID       "${DIAPASON_CLAP_ID}"
  CLAP_FEATURES instrument utility stereo)

target_sources(Diapason PRIVATE
  src/DiapasonPlugin.cpp
  src/NovaDiapasonEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Diapason PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Diapason
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Mirador : mesure et visualisation (sonie EBU R128 par libebur128, niveaux, stéréo, spectre), plugin CLAP effet transparent indépendant ------------------
# Cœur : Mirador (mesures) + LoudnessMeter ; interface NOVA (NovaMiradorEditor), spectre de l'égaliseur (SpectrumAnalyzer).
set(MIRADOR_CLAP_ID "com.tournedosrossini.mirador" CACHE STRING "Identifiant CLAP de Mirador")

juce_add_plugin(Mirador
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Mirador"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Mira
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Mirador
  CLAP_ID       "${MIRADOR_CLAP_ID}"
  CLAP_FEATURES audio-effect analyzer utility stereo)

target_sources(Mirador PRIVATE
  src/MiradorPlugin.cpp
  src/NovaMiradorEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Mirador PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Mirador
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- Fournaise : overdrive et saturation à quatre modèles (lampe, bande, transistor, fuzz), plugin CLAP effet indépendant -------------------------------
# Cœur : OverdriveModule (module stock) ; interface NOVA (NovaFournaiseEditor) ; entrée sidechain de l'hôte (module le drive).
set(FOURNAISE_CLAP_ID "com.tournedosrossini.fournaise" CACHE STRING "Identifiant CLAP de Fournaise")

juce_add_plugin(Fournaise
  COMPANY_NAME                "Le Tournedos Rossini"
  PRODUCT_NAME                "Fournaise"
  PLUGIN_MANUFACTURER_CODE    Trss
  PLUGIN_CODE                 Four
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  FORMATS                     Standalone)

clap_juce_extensions_plugin(
  TARGET        Fournaise
  CLAP_ID       "${FOURNAISE_CLAP_ID}"
  CLAP_FEATURES audio-effect distortion stereo)

target_sources(Fournaise PRIVATE
  src/FournaisePlugin.cpp
  src/NovaFournaiseEditor.cpp
  src/NovaEditor.cpp
  src/NovaKit.cpp
  src/NeptuneKnob.cpp
  src/KnobModal.cpp)

target_compile_definitions(Fournaise PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Fournaise
  PRIVATE
    neptune_core
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    clap_juce_extensions
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---- NovaGallery : rendu hors-ligne en PNG des composants communs NOVA (NovaKit), pour la comparaison avec les maquettes HTML ----------
# Outil de développement (CI Linux : exécuté sous xvfb, images archivées) ; non installé.
juce_add_console_app(NovaGallery PRODUCT_NAME "NovaGallery")
target_sources(NovaGallery PRIVATE tools/NovaGallery.cpp src/NovaKit.cpp src/NovaEditor.cpp src/NovaCompressorEditor.cpp src/NovaHistoryView.cpp src/NovaEqualizerEditor.cpp src/NovaSpectrum.cpp src/NovaShaperEditor.cpp src/NovaShaperWaveView.cpp src/NovaDelayEditor.cpp src/NovaChorusEditor.cpp src/NovaMicrodecaleEditor.cpp src/NovaDiapasonEditor.cpp src/NovaMiradorEditor.cpp src/NovaConvolutionEditor.cpp src/NovaFournaiseEditor.cpp src/NovaSynthEditor.cpp src/NovaFmEditor.cpp src/FmSysexIO.cpp src/NovaInstrumentBase.cpp src/NovaPresetUi.cpp src/NovaPresetFields.cpp src/PresetManager.cpp src/EnvelopeEditor.cpp src/ModView.cpp src/SynthKeyboard.cpp src/KnobModal.cpp src/NeptuneKnob.cpp)
target_compile_definitions(NovaGallery PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(NovaGallery
  PRIVATE
    neptune_core
    juce::juce_gui_basics
    juce::juce_graphics
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
