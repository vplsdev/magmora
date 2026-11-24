// The frozen interface. These must hold for the stub and for the real engine alike.
#include <magmora/Engine.h>

#include <catch.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <string>

using namespace magmora;

TEST_CASE("parameter table is indexed by id, with unique keys and valid ranges") {
  REQUIRE(static_cast<int>(kNumParams) == 22);
  std::set<std::string> keys;
  for (int i = 0; i < kNumParams; ++i) {
    const ParamInfo& p = kParams[i];
    INFO(p.key);
    CHECK(static_cast<int>(p.id) == i);
    CHECK(keys.insert(p.key).second);
    CHECK(p.min < p.max);
    CHECK(p.def >= p.min);
    CHECK(p.def <= p.max);
  }
}

TEST_CASE("parameters start at their defaults and are clamped") {
  Engine e;
  for (const ParamInfo& p : kParams) {
    INFO(p.key);
    CHECK(e.getParam(p.id) == p.def);
    e.setParam(p.id, p.max + 1000.0);
    CHECK(e.getParam(p.id) == p.max);
    e.setParam(p.id, p.min - 1000.0);
    CHECK(e.getParam(p.id) == p.min);
  }
}

TEST_CASE("prepare accepts 44.1 to 192 kHz only") {
  Engine e;
  for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) CHECK(e.prepare(rate, 512, 2));
  CHECK_FALSE(e.prepare(22050.0, 512, 2));
  CHECK_FALSE(e.prepare(384000.0, 512, 2));
}

TEST_CASE("process takes 0 to maxBlockSize frames, in place, in both precisions") {
  Engine e;
  REQUIRE(e.prepare(48000.0, 16, 2));
  float fl[16] = {}, fr[16] = {};
  double dl[16] = {}, dr[16] = {};
  float* f[] = {fl, fr};
  double* d[] = {dl, dr};
  e.process(f, f, 0);
  e.process(d, d, 0);
  dl[3] = std::numeric_limits<double>::quiet_NaN();
  e.process(f, f, 16);
  e.process(d, d, 16);
  for (int i = 0; i < 16; ++i) {
    CHECK(std::isfinite(fl[i]));
    CHECK(std::isfinite(dl[i]));
  }
}

TEST_CASE("version and model hash are reported") {
  CHECK_FALSE(std::string(versionString()).empty());
  CHECK_FALSE(std::string(modelHash()).empty());
}
