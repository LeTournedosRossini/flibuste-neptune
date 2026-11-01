add_library(neptune_core STATIC src/Autosave.cpp src/Engine.cpp src/FileUtil.cpp src/Parameter.cpp src/Profile.cpp src/ProbePool.cpp src/Envelope.cpp src/EnvelopeLayout.cpp src/KnobInteraction.cpp src/Instrument.cpp src/SampleLoader.cpp src/ZoneImport.cpp src/SamplerParams.cpp src/SamplerVoices.cpp src/Settings.cpp src/State.cpp src/Theme.cpp src/UiState.cpp src/I18n.cpp src/Undo.cpp src/MemoryBudget.cpp src/WaveformPeaks.cpp src/SampleEdit.cpp src/TimeStretch.cpp src/VolumeFootprint.cpp src/AudioRecorder.cpp src/SampleMeta.cpp src/PitchDetect.cpp src/DiskStreamer.cpp src/MappingEdit.cpp src/InstrumentEdit.cpp src/VelocityResponse.cpp src/Filter.cpp src/Mixer.cpp src/MixerState.cpp src/Synth.cpp src/WaveCatalog.cpp src/SynthVoices.cpp src/StockModule.cpp src/UtilityModule.cpp src/Preset.cpp src/PresetLibrary.cpp src/AnalogSynth.cpp src/MidiFilter.cpp src/Loudness.cpp src/FmPatch.cpp src/FmMsfa.cpp src/FmVoices.cpp src/FmPreset.cpp src/FmSynth.cpp src/OrganCore.c src/OrganEngine.cpp src/OrganSynth.cpp src/OrganPreset.cpp src/ZoneSlots.cpp src/Control.cpp src/Osc.cpp src/Oversampler.cpp src/ShaperModule.cpp src/ShaperPresets.cpp src/CompressorModule.cpp src/CompressorPresets.cpp src/EqualizerModule.cpp src/EqualizerPresets.cpp src/SpectrumAnalyzer.cpp src/DelayModule.cpp src/DelayPresets.cpp src/ChorusModule.cpp src/ChorusPresets.cpp src/MicrodecaleModule.cpp src/MicrodecalePresets.cpp src/OpusFile.cpp src/ConvolutionModule.cpp src/ConvolutionPresets.cpp src/Facade.cpp src/StylePack.cpp src/FacadeTransfer.cpp src/MidiSend.cpp src/Modulation.cpp src/ParamGroups.cpp src/TrackExport.cpp src/GlobalPreset.cpp src/NoteRouter.cpp src/Combinations.cpp src/DragValue.cpp src/WavPackFile.cpp src/OrganImport.cpp src/SfzImport.cpp src/SoundFont.cpp src/SignalGenerator.cpp src/IrGenerator.cpp src/Mirador.cpp src/OverdriveModule.cpp src/OverdrivePresets.cpp src/LimiterModule.cpp)
target_include_directories(neptune_core PUBLIC include)

# Numéro de version de développement affiché dans les interfaces : date de compilation, commit, numéro de run de la CI.
set(NEPTUNE_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE NEPTUNE_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE NEPTUNE_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NEPTUNE_GIT_SHA STREQUAL "")
    set(NEPTUNE_GIT_SHA "unknown")
  elseif(NOT NEPTUNE_GIT_DIRTY STREQUAL "")
    set(NEPTUNE_GIT_SHA "${NEPTUNE_GIT_SHA}+")
  endif()
endif()
if(DEFINED ENV{GITHUB_RUN_NUMBER})
  set(NEPTUNE_BUILD "CI #$ENV{GITHUB_RUN_NUMBER}")
else()
  set(NEPTUNE_BUILD "local")
endif()
string(TIMESTAMP NEPTUNE_BUILD_DATE "%Y-%m-%d %H:%M" UTC)
configure_file(Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/neptune/core/Version.h @ONLY)
target_include_directories(neptune_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(neptune_core PUBLIC cxx_std_17)
# Règle : aucune dépendance JUCE ici. Le cœur reste portable (tests, web app/WASM plus tard).
# nlohmann/json reste un détail du cœur (State.cpp) ; PUBLIC seulement parce que StateMigration.h l'expose aux tests de migration.
target_link_libraries(neptune_core PUBLIC nlohmann_json::nlohmann_json)
# Le cœur est lié dans une bibliothèque partagée (.clap) : code indépendant de la position obligatoire sous Linux.
set_target_properties(neptune_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Lecture des samples (palier 1) : libsndfile pour WAV/AIFF, dr_flac (en-tête seul) pour le FLAC.
target_link_libraries(neptune_core PRIVATE SndFile::sndfile)
target_include_directories(neptune_core PRIVATE ${dr_libs_SOURCE_DIR})

# Convolution (moteur LowLatConv, en-têtes conv/*.hpp) : PFFFT (C, licence de type BSD) pour la FFT, dr_wav (domaine public / MIT-0) pour lire les WAV d'IR.
# LLC_USE_PFFFT est PUBLIC : tous les fichiers qui incluent le moteur (cœur, interface, tests) doivent le voir de la même façon.
set(PFFFT_DIR ${CMAKE_SOURCE_DIR}/third_party/pffft)
target_sources(neptune_core PRIVATE ${PFFFT_DIR}/src/pffft.c ${PFFFT_DIR}/src/pffft_common.c ${PFFFT_DIR}/src/fftpack.c ${CMAKE_SOURCE_DIR}/third_party/dr_wav/dr_wav_impl.cpp)
set_source_files_properties(${PFFFT_DIR}/src/pffft.c ${PFFFT_DIR}/src/pffft_common.c ${PFFFT_DIR}/src/fftpack.c ${CMAKE_SOURCE_DIR}/third_party/dr_wav/dr_wav_impl.cpp
                            PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
target_include_directories(neptune_core PUBLIC ${PFFFT_DIR}/include ${PFFFT_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/dr_wav)
target_compile_definitions(neptune_core PUBLIC LLC_USE_PFFFT PFFFT_STATIC_DEFINE)

# stb_vorbis (domaine public) : décodage des échantillons Ogg Vorbis des banques SF3. Fichier C compilé à part, sans avertissements.
target_sources(neptune_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb_vorbis/stb_vorbis.c)
set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/stb_vorbis/stb_vorbis.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
target_include_directories(neptune_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb_vorbis)

# Opus (libopus, BSD) : décodage en mémoire des fichiers .opus ; sans l'option, ils sont reconnus mais refusés avec un message.
if(NEPTUNE_WITH_OPUS)
  target_link_libraries(neptune_core PRIVATE opus)
  target_compile_definitions(neptune_core PUBLIC NEPTUNE_HAS_OPUS=1)
endif()

# WavPack (libwavpack, BSD) : décodage en mémoire des fichiers WavPack (reconnus par leur contenu, même nommés .wav) ; sans l'option, ils sont refusés avec un message.
if(NEPTUNE_WITH_WAVPACK)
  target_link_libraries(neptune_core PRIVATE wavpack)
  target_compile_definitions(neptune_core PUBLIC NEPTUNE_HAS_WAVPACK=1)
endif()

# Thread disque du streaming (std::thread) : pthread explicite pour les glibc anciennes.
find_package(Threads REQUIRED)
target_link_libraries(neptune_core PUBLIC Threads::Threads)

# LUFS du Master (libebur128) : lié seulement si l'option est active ; sinon le compteur rend « -inf ».
if(NEPTUNE_WITH_EBUR128)
  target_link_libraries(neptune_core PRIVATE ebur128)
  target_compile_definitions(neptune_core PUBLIC NEPTUNE_HAS_EBUR128=1)
endif()

# Orgue à roues phoniques : moteur de setBfree (C, GPL v2 ou ultérieure, third_party/setbfree) et sa liaison (OrganCore.c).
set(SETBFREE_DIR ${CMAKE_SOURCE_DIR}/third_party/setbfree)
target_sources(neptune_core PRIVATE
  ${SETBFREE_DIR}/src/tonegen.c ${SETBFREE_DIR}/src/vibrato.c
  ${SETBFREE_DIR}/b_whirl/whirl.c ${SETBFREE_DIR}/b_whirl/eqcomp.c
  ${SETBFREE_DIR}/b_overdrive/overdrive.c ${SETBFREE_DIR}/b_overdrive/filterTools.c
  ${SETBFREE_DIR}/b_reverb/reverb.c)
set(SETBFREE_SOURCES src/OrganCore.c ${SETBFREE_DIR}/src/tonegen.c ${SETBFREE_DIR}/src/vibrato.c
  ${SETBFREE_DIR}/b_whirl/whirl.c ${SETBFREE_DIR}/b_whirl/eqcomp.c ${SETBFREE_DIR}/b_overdrive/overdrive.c
  ${SETBFREE_DIR}/b_overdrive/filterTools.c ${SETBFREE_DIR}/b_reverb/reverb.c)
set_source_files_properties(${SETBFREE_SOURCES} PROPERTIES COMPILE_DEFINITIONS "VERSION=\"neptune\";_USE_MATH_DEFINES")
target_include_directories(neptune_core PRIVATE
  ${SETBFREE_DIR}/src ${SETBFREE_DIR}/b_whirl ${SETBFREE_DIR}/b_overdrive ${SETBFREE_DIR}/b_reverb)
if(MSVC)
  target_include_directories(neptune_core PRIVATE ${SETBFREE_DIR}/compat) # <strings.h>
endif()
