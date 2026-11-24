// User preset files: what is written reads back exactly, under any locale; hostile input is refused cleanly.
#include <magmora/Chain.h>
#include <magmora/Preset.h>

#include <catch.hpp>

#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "common/Number.h"

using namespace magmora;

namespace {

struct CommaLocale {
  std::string previous;
  bool active = false;
  CommaLocale() {
    previous = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "German_Germany.1252", "de_DE"})
      if (std::setlocale(LC_NUMERIC, name) && std::strcmp(std::localeconv()->decimal_point, ",") == 0) {
        active = true;
        return;
      }
    std::setlocale(LC_NUMERIC, previous.c_str());
  }
  ~CommaLocale() { std::setlocale(LC_NUMERIC, previous.c_str()); }
};

UserPreset sample() {
  UserPreset p;
  double values[kNumChainParams];
  for (int id = 0; id < kNumChainParams; ++id) {
    const ParamInfo& info = *Chain::parameterInfo(id);
    // Something other than the default, inside the range, on the step for stepped ones.
    values[id] = info.taper == Taper::Stepped ? info.max : info.min + (info.max - info.min) * 0.3170001;
  }
  setPresetSnapshot(p, values);
  p.name = "Wall \"Of\" Sound \\ 2";
  p.hasCabFiles = true;
  p.cab.pack = {true, "/Library/Audio/Impulse Responses/Pack A", "Pack A", "/Library/Audio/Impulse Responses/Pack A/map.json", -1, true};
  p.cab.mic[0] = "DY57";
  p.cab.mic[1] = "R121";
  p.cab.speaker[1] = "LL";
  p.cab.variant[0] = "PRE1";
  p.cab.ir[1] = {true, "/Library/Audio/Impulse Responses/räum, 2.wav", "räum, 2.wav", "", 52844, false};
  return p;
}

void sameAs(const UserPreset& a, const UserPreset& b) {
  CHECK(a.name == b.name);
  CHECK(a.category == b.category);
  for (int id = 0; id < kNumChainParams; ++id) {
    INFO(Chain::parameterInfo(id)->key);
    CHECK(a.given[id] == b.given[id]);
    if (a.given[id]) CHECK(std::fabs(a.values[id] - b.values[id]) < 1e-8);
  }
  CHECK(a.hasCabFiles == b.hasCabFiles);
  CHECK(a.cab.pack.present == b.cab.pack.present);
  CHECK(a.cab.pack.path == b.cab.pack.path);
  CHECK(a.cab.pack.mapping == b.cab.pack.mapping);
  for (int m = 0; m < 2; ++m) {
    CHECK(a.cab.mic[m] == b.cab.mic[m]);
    CHECK(a.cab.speaker[m] == b.cab.speaker[m]);
    CHECK(a.cab.variant[m] == b.cab.variant[m]);
    CHECK(a.cab.ir[m].present == b.cab.ir[m].present);
    CHECK(a.cab.ir[m].path == b.cab.ir[m].path);
    CHECK(a.cab.ir[m].name == b.cab.ir[m].name);
    CHECK(a.cab.ir[m].bytes == b.cab.ir[m].bytes);
    CHECK(a.cab.ir[m].normalize == b.cab.ir[m].normalize);
  }
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

}  // namespace

TEST_CASE("preset file: a snapshot writes every preset parameter and nothing of the session, and reads back") {
  const UserPreset p = sample();
  const std::string text = writeUserPreset(p);
  for (int id = 0; id < kNumChainParams; ++id) {
    const std::string key = std::string("\"") + Chain::parameterInfo(id)->key + "\"";
    INFO(key);
    CHECK((text.find(key) != std::string::npos) == isPresetParameter(id));
  }
  for (const char* session : {"input_trim", "quality", "phase_mode", "channel_mode", "\"load\"", "cab_floor", "user_ir"})
    CHECK(text.find(session) == std::string::npos);
  CHECK(text.find("\"format\": \"magmora-preset\"") != std::string::npos);
  UserPreset back;
  std::string error;
  REQUIRE(readUserPreset(text.data(), text.size(), back, &error));
  sameAs(p, back);
  // Written again, byte for byte the same.
  CHECK(writeUserPreset(back) == text);
}

TEST_CASE("preset file: numbers are written and read with a '.' under a decimal-comma locale") {
  const UserPreset p = sample();
  const std::string plain = writeUserPreset(p);
  CommaLocale comma;
  if (!comma.active) {
    WARN("no decimal-comma locale on this system: nothing to compare");
    return;
  }
  const std::string written = writeUserPreset(p);
  CHECK(written == plain);
  CHECK(formatDouble(0.71) == "0.71");
  CHECK(formatDouble(-96.0) == "-96");
  CHECK(formatDouble(12000.0) == "12000");
  CHECK(formatDouble(-0.0000000001) == "0");
  UserPreset back;
  REQUIRE(readUserPreset(written.data(), written.size(), back));
  sameAs(p, back);
}

TEST_CASE("preset file: every factory preset reads as a factory-style preset with the values Chain applies") {
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(MAGMORA_SOURCE_DIR) / "presets")) {
    if (entry.path().extension() != ".json") continue;
    const std::string json = readFile(entry.path());
    UserPreset p;
    std::string error;
    INFO(entry.path().filename().string() << ": " << error);
    REQUIRE(readUserPreset(json.data(), json.size(), p, &error));
    CHECK_FALSE(p.hasCabFiles);
    Chain c;
    REQUIRE(c.applyPreset(json.data(), json.size()));
    for (int id = 0; id < kNumChainParams; ++id)
      if (p.given[id]) CHECK(p.values[id] == c.getParameter(id));
    ++count;
  }
  CHECK(count >= 10);
}

TEST_CASE("preset file: hostile and damaged files are refused and leave the preset untouched") {
  const std::string good = writeUserPreset(sample());
  const char* const bad[] = {
      "",
      "{",
      "[]",
      "null",
      "{\"name\": \"x\"}",  // neither format nor params
      "{\"format\": \"something-else\", \"name\": \"x\", \"params\": {}}",
      "{\"format\": \"magmora-preset\", \"version\": 0, \"name\": \"x\"}",
      "{\"format\": \"magmora-preset\", \"version\": \"1\", \"name\": \"x\"}",
      "{\"format\": \"magmora-preset\", \"version\": 1}",  // no name
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"   \"}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": 7}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"params\": []}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"pedal\": {\"pedal_drive\": \"loud\"}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"eq\": {\"eq_on\": null}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"cab_files\": []}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"cab_files\": {\"mic1\": {\"path\": 3}}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"cab_files\": {\"mic1\": {\"path\": \"a\\u0000b\"}}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"cab_files\": {\"mic1\": {\"path\": \"/a\", \"bytes\": -4}}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"cab_files\": {\"mic1\": {}}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\", \"cab_files\": {\"pack\": {\"path\": \"/p\", \"mics\": [\"a\", \"b\", \"c\"]}}}",
      "{\"format\": \"magmora-preset\", \"version\": 1, \"name\": \"x\"} trailing",
  };
  UserPreset kept;
  REQUIRE(readUserPreset(good.data(), good.size(), kept));
  const std::string before = writeUserPreset(kept);
  for (const char* text : bad) {
    INFO(text);
    std::string error;
    CHECK_FALSE(readUserPreset(text, std::strlen(text), kept, &error));
    CHECK_FALSE(error.empty());
    CHECK(writeUserPreset(kept) == before);
  }
  // Deep nesting, a huge file, binary junk.
  std::string deep = "{\"format\": \"magmora-preset\", \"name\": \"x\", \"junk\": ";
  for (int i = 0; i < 10000; ++i) deep += "[";
  CHECK_FALSE(readUserPreset(deep.data(), deep.size(), kept));
  std::string huge = good;
  huge.insert(huge.size() - 2, std::string(kMaxPresetFileBytes, ' '));
  CHECK_FALSE(readUserPreset(huge.data(), huge.size(), kept));
  std::string junk(4096, '\0');
  for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 7919u) & 0xFF);
  CHECK_FALSE(readUserPreset(junk.data(), junk.size(), kept));
  // A truncated copy of a good file at every length.
  for (size_t n = 0; n + 2 < good.size(); n += 7) CHECK_FALSE(readUserPreset(good.data(), n, kept));
  CHECK(writeUserPreset(kept) == before);
}

TEST_CASE("preset file: values are clamped and rounded, unknown keys ignored, missing ones default") {
  const char* text = R"({"format": "magmora-preset", "version": 3, "name": "  Future  ", "future_key": {"a": [1, 2]},
      "params": {"pre_lead": 99, "channel": 0.7, "not_a_param": 4},
      "pedal": {"pedal_drive": -5}})";
  UserPreset p;
  REQUIRE(readUserPreset(text, std::strlen(text), p));
  CHECK(p.name == "Future");
  CHECK(p.values[kPreLead] == 10.0);
  CHECK(p.values[kChannel] == 1.0);
  CHECK(p.given[kLow]);
  CHECK(p.values[kLow] == Chain::parameterInfo(kLow)->def);
  CHECK(p.given[kPedalDrive]);
  CHECK(p.values[kPedalDrive] == 0.0);
  CHECK(p.given[kPedalLevel]);
  CHECK_FALSE(p.given[kEqOn]);  // no "eq" object: the EQ is left as it is
  CHECK_FALSE(p.given[kInputTrim]);
}

TEST_CASE("preset file: names and file names are cleaned for every platform") {
  CHECK(cleanPresetName("  Club\tLead \n") == "ClubLead");
  CHECK(cleanPresetName("   ").empty());
  const std::string longName(100, 'a');
  CHECK(cleanPresetName(longName).size() == kMaxPresetNameBytes);
  std::string accents;
  for (int i = 0; i < 40; ++i) accents += "\xC3\xA9";  // é, two bytes each: never cut in half
  const std::string cut = cleanPresetName(accents);
  CHECK(cut.size() == 62);
  CHECK(presetFileStem("A/B\\C:D*E?F\"G<H>I|J") == "A-B-C-D-E-F-G-H-I-J");
  CHECK(presetFileStem("..") == "Preset");
  CHECK(presetFileStem("...hidden") == "hidden");
  CHECK(presetFileStem("trailing. . ") == "trailing");
  CHECK(presetFileStem("") == "Preset");
  CHECK(presetFileStem("con") == "con_");
  CHECK(presetFileStem("LPT1.txt") == "LPT1.txt_");
  CHECK(presetFileStem("Wall Of Sound") == "Wall Of Sound");
  CHECK(presetFileStem("Räum") == "Räum");
}
