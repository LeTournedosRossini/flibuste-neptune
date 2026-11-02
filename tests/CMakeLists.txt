add_executable(neptune_core_tests test_engine.cpp test_parameter.cpp test_autosave.cpp test_envelope.cpp test_envelope_layout.cpp test_knob_interaction.cpp test_probe_pool.cpp test_profile.cpp test_sampler.cpp test_sample_playback.cpp test_waveform_peaks.cpp test_sample_edit.cpp test_time_stretch.cpp test_volume_footprint.cpp test_audio_recorder.cpp test_sample_meta.cpp test_pitch_detect.cpp test_sample_loader.cpp test_zone_import.cpp test_settings.cpp test_state.cpp test_theme.cpp test_ui_state.cpp test_i18n.cpp test_undo.cpp test_memory_budget.cpp test_streaming.cpp test_mapping_edit.cpp test_velocity.cpp test_polyphony.cpp test_choke.cpp test_filter.cpp test_mixer.cpp test_synth.cpp test_utility.cpp test_parallel.cpp test_preset.cpp test_analogsynth.cpp test_midi_filter.cpp test_loudness.cpp test_fm.cpp test_organ.cpp test_zone_slots.cpp test_control.cpp test_osc.cpp test_audioin.cpp test_shaper.cpp test_compressor.cpp test_equalizer.cpp test_spectrum.cpp test_delay.cpp test_chorus.cpp test_microdecale.cpp test_opus.cpp test_facade.cpp test_stylepack.cpp test_facade_transfer.cpp test_midisend.cpp test_modulation.cpp test_modulation_rig.cpp test_paramgroups.cpp test_trackexport.cpp test_globalpreset.cpp test_noterouter.cpp test_combinations.cpp test_dragvalue.cpp test_wavpack.cpp test_organimport.cpp test_sfzimport.cpp test_soundfont.cpp test_generator.cpp test_tuner.cpp test_irgen.cpp test_mirador.cpp test_overdrive.cpp test_limiter.cpp test_diapason_insert.cpp test_zero_latency.cpp test_latency_compensation.cpp test_conv_engine.cpp conv/test_convolver.cpp conv/test_display.cpp conv/test_eq.cpp conv/test_ir.cpp conv/test_live.cpp conv/test_meters.cpp conv/test_processor.cpp conv/test_stereo.cpp conv/test_wav.cpp)
# Les jeux de tests d'origine du moteur de convolution ont chacun un `main` : on le renomme pour les appeler depuis test_conv_engine.cpp.
foreach(name convolver display eq ir live meters processor stereo wav)
  set_source_files_properties(conv/test_${name}.cpp PROPERTIES COMPILE_DEFINITIONS "main=llc_test_${name}")
endforeach()
target_link_libraries(neptune_core_tests PRIVATE neptune_core SndFile::sndfile Catch2::Catch2WithMain)
# Un MixerState pèse environ 144 Ko : plusieurs exemplaires locaux dans un même test dépassent les 1 Mo de pile par défaut de Windows (MSVC). 8 Mo, comme sous Linux.
if(MSVC)
  target_link_options(neptune_core_tests PRIVATE /STACK:8388608)
endif() # SndFile : les tests écrivent des WAV
include(CTest)
include(Catch)
catch_discover_tests(neptune_core_tests)

# Mesure de performance du callback (hors ctest : dépend de la machine, voir docs/performance.md).
add_executable(neptune_perf_callback perf_callback.cpp)
target_link_libraries(neptune_perf_callback PRIVATE neptune_core)

# Tests WavPack : ils encodent un fichier avec libwavpack avant de le relire.
if(NEPTUNE_WITH_WAVPACK)
  target_link_libraries(neptune_core_tests PRIVATE wavpack)
endif()

# Tests Opus : ils encodent un fichier avec libopus avant de le relire.
if(NEPTUNE_WITH_OPUS)
  target_link_libraries(neptune_core_tests PRIVATE opus)
endif()
