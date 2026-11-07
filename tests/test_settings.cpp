#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "neptune/core/FileUtil.h"
#include "neptune/core/Settings.h"

using namespace neptune::core;
namespace fs = std::filesystem;

namespace
{
struct TempDir
{
    fs::path path;
    TempDir()
    {
        static int counter = 0;
        path = fs::temp_directory_path() / ("neptune_test_settings_" + std::to_string (++counter) + "_"
                                            + std::to_string (static_cast<long long> (reinterpret_cast<std::uintptr_t> (this))));
        fs::remove_all (path);
        fs::create_directories (path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all (path, ec); }
};
} // namespace

TEST_CASE ("settings defaults match the documented ones")
{
    const settings::GlobalSettings s;
    CHECK (s.language == "en");
    CHECK (s.theme == "dark");
    CHECK (s.contrast == 1);
    CHECK (s.profile == settings::PerformanceProfile::Live);
    CHECK (s.autosaveIntervalSeconds == 60);
    CHECK (s.autosaveKeepDays == 30);
}

TEST_CASE ("settings round trip through JSON")
{
    settings::GlobalSettings s;
    s.language = "fr";
    s.theme = "hammond";
    s.contrast = 3;
    s.profile = settings::PerformanceProfile::Studio;
    s.samplesDir = "/data/samples";
    s.presetsDir = "C:\\Presets";
    s.autosaveIntervalSeconds = 0;
    s.autosaveKeepDays = 7;

    std::vector<std::string> warnings;
    const auto back = settings::fromJson (settings::toJson (s), &warnings);
    CHECK (warnings.empty());
    CHECK (settings::toJson (back) == settings::toJson (s));
    CHECK (back.language == "fr");
    CHECK (back.theme == "hammond");
    CHECK (back.contrast == 3);
    CHECK (back.profile == settings::PerformanceProfile::Studio);
    CHECK (back.presetsDir == "C:\\Presets");
    CHECK (back.autosaveIntervalSeconds == 0);
}

TEST_CASE ("invalid settings fall back to defaults field by field")
{
    const std::string text = R"({"format":"neptune-settings","schema_version":1,
        "language":"fr","theme":"neon","contrast":9,"performance_profile":"turbo",
        "samples_dir":42,"autosave_interval_seconds":2,"autosave_keep_days":"many"})";
    std::vector<std::string> warnings;
    const auto s = settings::fromJson (text, &warnings);

    CHECK (s.language == "fr");                     // valide : conservé
    CHECK (s.theme == "dark");                      // inconnu
    CHECK (s.contrast == 1);                        // hors bornes : la valeur par défaut
    CHECK (s.profile == settings::PerformanceProfile::Live);
    CHECK (s.samplesDir.empty());                   // mauvais type
    CHECK (s.autosaveIntervalSeconds == 60);        // sous le minimum
    CHECK (s.autosaveKeepDays == 30);               // mauvais type
    CHECK (warnings.size() == 6);
}

TEST_CASE ("unreadable settings give defaults and never throw")
{
    for (const char* text : { "", "garbage {", "[1,2]", R"({"hello":1})" })
    {
        std::vector<std::string> warnings;
        const auto s = settings::fromJson (text, &warnings);
        CHECK (s.language == "en");
        CHECK_FALSE (warnings.empty());
    }
}

TEST_CASE ("settings from a newer schema are read with a warning")
{
    const std::string text = R"({"format":"neptune-settings","schema_version":99,"theme":"light","future_key":true})";
    std::vector<std::string> warnings;
    const auto s = settings::fromJson (text, &warnings);
    CHECK (s.theme == "light");
    CHECK (warnings.size() == 1);
}

TEST_CASE ("settings file: missing gives defaults, save creates folders and reloads")
{
    TempDir dir;
    const auto file = dir.path / "config" / "Neptune" / "settings.json";

    std::vector<std::string> warnings;
    CHECK (settings::loadFile (file, &warnings).language == "en");
    CHECK (warnings.empty());

    settings::GlobalSettings s;
    s.theme = "light";
    std::string err;
    REQUIRE (settings::saveFile (file, s, &err));
    CHECK (settings::loadFile (file).theme == "light");
    CHECK_FALSE (fs::exists (fs::path (file.string() + ".tmp"))); // pas de fichier temporaire oublié
}

TEST_CASE ("atomic write replaces an existing file")
{
    TempDir dir;
    const auto file = dir.path / "a.txt";
    REQUIRE (fileutil::writeAtomic (file, "one"));
    REQUIRE (fileutil::writeAtomic (file, "two"));
    std::string text;
    REQUIRE (fileutil::readAll (file, text));
    CHECK (text == "two");
}

TEST_CASE ("settings: the reduced host list is on by default and round trips", "[settings]")
{
    using namespace neptune::core::settings;
    GlobalSettings s;
    CHECK (s.reducedHostList);
    CHECK (fromJson ("{}").reducedHostList);
    CHECK_FALSE (s.showDiagnostics);
    CHECK_FALSE (fromJson ("{}").showDiagnostics);
    s.showDiagnostics = true;
    CHECK (fromJson (toJson (s)).showDiagnostics);
    s.reducedHostList = false;
    CHECK_FALSE (fromJson (toJson (s)).reducedHostList);
    // zoom du mixeur : 120 par défaut, borné à 60..160
    CHECK (fromJson ("{}").mixerZoomPercent == 120);
    s.mixerZoomPercent = 90;
    CHECK (fromJson (toJson (s)).mixerZoomPercent == 90);
    CHECK (fromJson ("{\"mixer_zoom_percent\": 500}").mixerZoomPercent == 120);
}
