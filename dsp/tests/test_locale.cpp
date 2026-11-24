// Numbers in presets, mapping files and netlist values are read the same whatever LC_NUMERIC the host has set:
// a host (or another plug-in in it) that calls setlocale with a decimal-comma locale must not break them.
#include <magmora/Chain.h>

#include <catch.hpp>

#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cab/Positions.h"

using namespace magmora;

namespace {

// Sets the first decimal-comma locale this system has for LC_NUMERIC; restores the previous one on exit.
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

std::string readFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

std::vector<double> applied(const std::string& json) {
  Chain c;
  if (!c.applyPreset(json.c_str(), json.size())) return {};
  std::vector<double> v;
  for (int id = 0; id < Chain::parameterCount(); ++id) v.push_back(c.getParameter(id));
  return v;
}

}  // namespace

TEST_CASE("locale: every factory preset loads identically under a decimal-comma locale") {
  std::vector<std::pair<std::string, std::vector<double>>> expected;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(MAGMORA_SOURCE_DIR) / "presets"))
    if (entry.path().extension() == ".json") {
      const std::string json = readFile(entry.path());
      expected.emplace_back(json, applied(json));
      REQUIRE_FALSE(expected.back().second.empty());
    }
  REQUIRE(expected.size() >= 10);
  CommaLocale comma;
  if (!comma.active) {
    WARN("no decimal-comma locale on this system: nothing to compare");
    return;
  }
  for (const auto& [json, values] : expected) CHECK(applied(json) == values);
  const std::string fraction = R"({"params": {"pre_lead": 7.5, "master": 2.25e0}})";
  const std::vector<double> v = applied(fraction);
  REQUIRE_FALSE(v.empty());
  CHECK(v[kPreLead] == 7.5);
}

TEST_CASE("locale: a mapping file and position names with decimals read the same under a decimal-comma locale") {
  const char* mappingText = R"({"schemes": [
      {"regex": "^SPK (?P<speaker>LL|LR) (?P<mic>[A-Za-z]{0,3}57) (?P<x>[0-9.]+)in (?P<dist>[0-9.]+)in$",
       "units": {"x": "in", "dist": "in"}}],
      "files": {"my favourite.wav": {"mic": "57", "x_mm": 19.5, "dist_mm": 2.25, "angle": 22.5}}})";
  auto read = [&](cab::Position& a, cab::Position& b, cab::Position& c) {
    cab::Mapping m;
    std::string error;
    if (!cab::parseMapping(mappingText, m, &error)) return false;
    a = cab::parseName("SPK LR DY57 1.25in 0.5in.wav", &m);
    b = cab::parseName("my favourite.wav", &m);
    c = cab::parseName("DY7B 2,5cm from cap 30mm OA22.5.wav");
    return true;
  };
  cab::Position a0, b0, c0;
  REQUIRE(read(a0, b0, c0));
  CHECK(a0.xMm == Approx(31.75));
  CHECK(b0.xMm == 19.5);
  CommaLocale comma;
  if (!comma.active) {
    WARN("no decimal-comma locale on this system: nothing to compare");
    return;
  }
  cab::Position a1, b1, c1;
  REQUIRE(read(a1, b1, c1));
  CHECK(a1.xMm == a0.xMm);
  CHECK(a1.distMm == a0.distMm);
  CHECK(b1.xMm == b0.xMm);
  CHECK(b1.distMm == b0.distMm);
  CHECK(b1.angleDeg == b0.angleDeg);
  CHECK(c1.xMm == c0.xMm);
  CHECK(c1.angleDeg == c0.angleDeg);
}
