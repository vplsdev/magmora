// Tube and diode laws: port of proto/blockamp/devices.py. Parameters come from the generated netlist.
// The closed forms here are the specification; Evaluators.h holds the tabulated product versions.
#pragma once
#include <array>
#include <memory>
#include <vector>

#include "devices/Tables.h"

namespace magmora::devices {

using SoftplusTable = UniformTable<2048>;   // softplus(x) on [-24, 24], linear above
using GridTable = UniformTable<4096>;       // grid current against vgk, closed form above kGridTableMax
using PowerTable = OctaveTable<-40, 14, 5>; // e^ex
using KneeTable = OctaveTable<1, 12, 7>;    // knee line a against vpk + kKneeShift
using MinimumTable = UniformTable<256>;     // (1 + r^3)^(-1/3) on [0, 1]

constexpr double kGridTableMax = 16.0;
constexpr double kKneeShift = 8.0;

struct TriodeParams {
  double mu, ex, kg1, kp, kvb, gg, cg, xi, vg0, vct;
  static TriodeParams from(const double* p);
};

struct PentodeParams {
  double mu, ex, kg1, kp, kv, rs, gg, cg, xi, lam, npar, asc;
  static PentodeParams from(const double* p);
};

struct DiodeParams {
  double isat, nvt, vcrit;
  static DiodeParams from(const double* p);
};

// out: ip, ig, then dip/dvpk, dip/dvgk, dig/dvpk (zero), dig/dvgk.
void triode(const TriodeParams&, double vpk, double vgk, double* out);
// out: ip, is, ig, then the Jacobian row-major with respect to (vpk, vsk, vgk).
void pentode(const PentodeParams&, double vpk, double vsk, double vgk, double* out);

void softplus(double x, double& y, double& dy);

struct TriodeLaw {
  std::array<double, 12> key;
  TriodeParams p;
  PowerTable power;
  GridTable grid;
};

struct PentodeLaw {
  std::array<double, 12> key;
  PentodeParams p;
  PowerTable power;
  GridTable grid;
  KneeTable knee;
};

struct DiodeLaw {
  std::array<double, 12> key;
  DiodeParams p;
  double exponentialAtVcrit, reverseLimit;

  MAGMORA_INLINE void closed(double v, double& i, double& g) const {
    if (v > p.vcrit) {
      const double e = exponentialAtVcrit;
      i = p.isat * (e - 1.0) + p.isat * e / p.nvt * (v - p.vcrit);
      g = p.isat * e / p.nvt;
    } else {
      const double e = std::exp(v / p.nvt);
      i = p.isat * (e - 1.0);
      g = p.isat * e / p.nvt + 1e-12;
    }
  }

  MAGMORA_INLINE void fast(double v, double& i, double& g) const {
    if (v < reverseLimit) {  // exp(-40) is below double precision of the sum
      i = -p.isat;
      g = 1e-12;
    } else {
      closed(v, i, g);
    }
  }
};

// Every distinct law of one netlist, with its tables. Allocates on add(); immutable while audio runs.
class Library {
 public:
  Library();
  const TriodeLaw* addTriode(const std::array<double, 12>& params);
  const PentodeLaw* addPentode(const std::array<double, 12>& params);
  const DiodeLaw* addDiode(const std::array<double, 12>& params);

  SoftplusTable softplusTable;
  MinimumTable minimumTable;

 private:
  std::vector<std::unique_ptr<TriodeLaw>> triodes_;
  std::vector<std::unique_ptr<PentodeLaw>> pentodes_;
  std::vector<std::unique_ptr<DiodeLaw>> diodes_;
};

// The devices of one cluster in port order: triodes (p-k, g-k), pentodes (p-k, g2-k, g-k), other diodes.
struct DeviceSet {
  const Library* library = nullptr;
  std::vector<const TriodeLaw*> triodes;
  std::vector<const PentodeLaw*> pentodes;
  std::vector<const DiodeLaw*> flyback;  // per pentode: the diode folded into its plate port (reversed), or null
  std::vector<const DiodeLaw*> diodes;

  int ports() const { return static_cast<int>(2 * triodes.size() + 3 * pentodes.size() + diodes.size()); }
};

}  // namespace magmora::devices
