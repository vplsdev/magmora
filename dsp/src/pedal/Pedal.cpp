#include "pedal/Pedal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace magmora::pedal {
namespace {

#include "pedal/PedalData.inc"

constexpr double kVd = 2e-3, kVx = 1.5;  // the cheap form's guard (spec 8)
// While the full form is in use and its last solution is this far past the guard (the op-amp saturated), the cheap form
// is not tried first: it would fail the guard. Its port 0 is the op-amp's input difference, port 1 its output node.
constexpr double kStayVd = 4.0 * kVd, kStayVx = kVx + 0.1;
constexpr int kRebuildEvery = 32;        // host samples, while a knob moves (spec 4)
constexpr int kDegradedIterations = 8;

double alog(double x, double mid) {  // the audio law through (0.5, mid)
  x = std::clamp(x, 0.0, 1.0);
  const double b = (1.0 / mid - 1.0) * (1.0 / mid - 1.0);
  return (std::pow(b, x) - 1.0) / (b - 1.0);
}

enum : unsigned { kO1 = 1, kO2 = 2, kO3 = 4, kO4 = 8 };

}  // namespace

const char* modelHash() { return kPedalModelHash; }

void potOhms(double drive, double tone, double level, double* ohms) {
  const double d = drive / 10.0, t = std::clamp(tone / 10.0, 0.0, 1.0), l = level / 10.0;
  const double w = t <= 0.5 ? 0.5 * alog(2 * t, kPedalTaperMid) : 1.0 - 0.5 * alog(2 * (1 - t), kPedalTaperMid);
  const double fl = alog(l, kPedalTaperMid);
  ohms[0] = std::max(kPedalDriveTotal * alog(d, kPedalDriveMid), kPedalPotMin);
  ohms[1] = std::max(kPedalToneTotal * w, kPedalPotMin);
  ohms[2] = std::max(kPedalToneTotal * (1 - w), kPedalPotMin);
  ohms[3] = std::max(kPedalLevelTotal * (1 - fl), kPedalPotMin);
  ohms[4] = std::max(kPedalLevelTotal * fl, kPedalPotMin);
  ohms[5] = 1.0;
}

void Pedal::prepare(double internalRate, double hostRate, int midFactor) {
  PedalCluster* all[6] = {&o1_, &o2_, &o3_, &o4_, &o2c_, &o3c_};
  const double mid = internalRate * (midFactor == 2 ? 2 : 1);
  for (int k = 0; k < 6; ++k) all[k]->prepare(kPedalClusters[k], k == 0 || k == 3 ? internalRate : mid);
  for (auto& k : knob_) k.prepare(hostRate);
  budgetCeiling = budgetPerSample >= 1e17 ? 1e18 : budgetPerSample * budgetCeilingSamples;
  reset();
}

void Pedal::setKnobs(double drive, double tone, double level) {
  const double t[3] = {std::clamp(drive, 0.0, 10.0), std::clamp(tone, 0.0, 10.0), std::clamp(level, 0.0, 10.0)};
  for (int k = 0; k < 3; ++k)
    if (t[k] != target_[k]) {
      target_[k] = t[k];
      knob_[k].setTarget(t[k]);
      moving_ = true;
    }
}

void Pedal::rebuild(unsigned which, bool refined) {
  double ohms[6];
  potOhms(rot_[0], rot_[1], rot_[2], ohms);
  if (which & kO1) o1_.build(ohms, refined);
  if (which & kO2) {
    o2_.build(ohms, refined);
    o2c_.build(ohms, refined);
  }
  if (which & kO3) {
    o3_.build(ohms, refined);
    o3c_.build(ohms, refined);
  }
  if (which & kO4) o4_.build(ohms, refined);
}

void Pedal::reset() {
  for (int k = 0; k < 3; ++k) {
    rot_[k] = target_[k];
    knob_[k].snap(target_[k]);
  }
  moving_ = false;
  double ohms[6];
  potOhms(rot_[0], rot_[1], rot_[2], ohms);
  // each cluster from the previous one's operating point (spec 5: start-up)
  o1_.build(ohms, true);
  o1_.operatingPoint(0.0, s1_.data());
  const double p1 = o1_.outputDc(0);
  o2_.build(ohms, true);
  o2_.operatingPoint(p1, s2_.data());
  o2c_.build(ohms, true);
  o2c_.setPorts(o2_.v() + 2);  // the cheap form's one port is the full form's third, the clipping diodes
  const double o1 = o2_.outputDc(0);
  o3_.build(ohms, true);
  o3_.operatingPoint(o1, s3_.data());
  o3c_.build(ohms, true);
  const double o2 = o3_.outputDc(0);
  dcP1_ = p1;
  dcO2_ = o2;
  o4_.build(ohms, true);
  o4_.operatingPoint(o2, s4_.data());
  std::array<double, 8> scratch{};
  o2c_.operatingPoint(p1, scratch.data());  // the cheap form's own ports and history; the states stay the full form's
  tokens_ = budgetCeiling;  // the (short) credit: the first samples after an operating point take a few more iterations
  tick_ = 0;
  blending_ = dirty_ = 0;
  o2Full_ = o3Full_ = false;
}

void Pedal::hostTick() {
  if (!moving_) return;
  bool still = false;
  for (int k = 0; k < 3; ++k) {  // the head's knob smoothing (30 ms and two 3 ms rounding poles)
    if (!knob_[k].moving()) continue;
    rot_[k] = knob_[k].next();
    still = still || knob_[k].moving();
    dirty_ |= k == 0 ? kO2 : (k == 1 ? (kO2 | kO3) : (kO3 | kO4));
  }
  PedalCluster* byMask[4][2] = {{&o1_, nullptr}, {&o2_, &o2c_}, {&o3_, &o3c_}, {&o4_, nullptr}};
  const int phase = tick_++ % kRebuildEvery;
  if (phase == 0) {
    // a new build every kRebuildEvery host samples (spec 4); the coefficients move to it in a straight line over
    // the next interval, so that a moving knob leaves no steps (G5: with steps, level swept down left -67 dB)
    blending_ = dirty_;
    for (int m = 0; m < 4; ++m)
      if (blending_ & (1u << m))
        for (PedalCluster* c : byMask[m])
          if (c) c->captureFrom();
    rebuild(blending_, false);
    for (int m = 0; m < 4; ++m)
      if (blending_ & (1u << m))
        for (PedalCluster* c : byMask[m])
          if (c) {
            c->captureTo();
            c->blend(0.0);
          }
    dirty_ = 0;
  } else if (blending_) {
    const double t = static_cast<double>(phase) / kRebuildEvery;
    for (int m = 0; m < 4; ++m)
      if (blending_ & (1u << m))
        for (PedalCluster* c : byMask[m])
          if (c) c->blend(t);
  }
  if (!still && phase == kRebuildEvery - 1) {  // settled, and the last interval done: the exact matrices
    rebuild(kO2 | kO3 | kO4, true);
    moving_ = false;
    blending_ = dirty_ = 0;
    tick_ = 0;
  }
}

bool Pedal::solveCluster(PedalCluster& c, double u, double* s, const double* seed) {
  // The head's bounded-work rule (Cluster.h, RecoveryBudget): failed solves and their recoveries spend tokens that
  // refill every sample; while there are none, a solve gets kDegradedIterations and, failing, holds (below).
  held_ = false;
  const bool funded = tokens_ > 0.0;
  if (!funded && c.ports() > 0) {  // the budget is spent: hold at no cost (the worst block bounded)
    c.hold(u, s);
    held_ = true;
    ++stats_.fallbacks;
    return true;
  }
  NewtonSettings st = settings;
  if (!funded) st.maxIterations = kDegradedIterations;
  const int it = c.solve(u, s, st, seed);
  if (it >= 0) {
    stats_.iterations += it;
    stats_.clusterIterations[counting_] += it;
    if (c.ports() > 0) stats_.charged += it;
    // every Newton iteration counts (the worst block bounded like the head's); a cluster without devices (O3's
    // cheap form) is one matrix product, not a Newton solve, and is not charged
    if (funded && c.ports() > 0) tokens_ -= it;
    return true;
  }
  stats_.iterations += st.maxIterations;
  stats_.clusterIterations[counting_] += st.maxIterations;
  stats_.charged += st.maxIterations;
  if (funded) {
    tokens_ -= st.maxIterations;
    int work = 0;
    const bool ok = c.recover(st, work);
    tokens_ -= work;
    stats_.iterations += work;
    stats_.clusterIterations[counting_] += work;
    stats_.charged += work;
    ++stats_.recoveries;
    if (!ok) ++stats_.fallbacks;
  } else {
    // The budget is spent and the solve failed: the cluster holds its last accepted solution and its states stand
    // still for this sample. Measured on 3 V white noise at the internal rate (what a 192 kHz host can send, the
    // head running at 1x there): accepting the failed iterate, the last currents, or a short walk's end all ran
    // away, because the states then integrate currents that are not the network's solution.
    c.keepLast();
    held_ = true;
    ++stats_.fallbacks;
  }
  return true;
}

double Pedal::process(double x) { return back(mid(front(x))); }

double Pedal::front(double x) {
  ++stats_.samples;
  tokens_ = std::min(budgetCeiling, tokens_ + budgetPerSample * budgetScale);
  // O1: input buffer
  counting_ = 0;
  solveCluster(o1_, x, s1_.data());
  const double p1 = o1_.y()[0];
  probe_[0] = p1;
  probe_[1] = o1_.y()[1];
  if (!held_) o1_.commit(x, s1_.data());
  return p1;
}

double Pedal::mid(double p1) {
  ++stats_.midSamples;
  // O2: clipping amplifier, cheap form while its op-amp is linear
  counting_ = 1;
  PedalCluster* used = &o2c_;
  const bool stay2 = o2Full_ && (std::fabs(o2_.v()[0]) > kStayVd || std::fabs(o2_.v()[1] - kPedalBias) > kStayVx);
  if (cheap && !stay2) solveCluster(o2c_, p1, s2_.data());
  const double* g = o2c_.y();
  if (!cheap || stay2 || std::fabs(g[1]) > kVd || std::fabs(g[2] - kPedalBias) > kVx) {
    const double seed[3] = {g[1], g[2], o2c_.v()[0]};
    if (cheap && !o2Full_) {  // entering the full form: its own history is stale, start from the guard values
      o2_.setPorts(seed);
      solveCluster(o2_, p1, s2_.data(), seed);
    } else {
      solveCluster(o2_, p1, s2_.data());
    }
    used = &o2_;
    o2c_.setPorts(o2_.v() + 2);
    ++stats_.fullO2;
  }
  o2Full_ = used == &o2_;
  const double o1 = used->y()[0];
  if (!held_) used->commit(p1, s2_.data());
  // O3: tone stage
  counting_ = 2;
  used = &o3c_;
  const bool stay3 = o3Full_ && (std::fabs(o3_.v()[0]) > kStayVd || std::fabs(o3_.v()[1] - kPedalBias) > kStayVx);
  if (cheap && !stay3) solveCluster(o3c_, o1, s3_.data());
  const double* h = o3c_.y();
  if (!cheap || stay3 || std::fabs(h[1]) > kVd || std::fabs(h[2] - kPedalBias) > kVx) {
    const double seed[2] = {h[1], h[2]};
    if (cheap && !o3Full_) {
      o3_.setPorts(seed);
      solveCluster(o3_, o1, s3_.data(), seed);
    } else {
      solveCluster(o3_, o1, s3_.data());
    }
    used = &o3_;
    ++stats_.fullO3;
  }
  o3Full_ = used == &o3_;
  const double o2 = used->y()[0];
  if (!held_) used->commit(o1, s3_.data());
  probe_[2] = o1;
  probe_[3] = o2;
  return o2;
}

double Pedal::back(double o2) {
  // O4: level and output buffer
  counting_ = 3;
  solveCluster(o4_, o2, s4_.data());
  const double out = o4_.y()[0];
  probe_[4] = out;
  probe_[5] = o4_.y()[1];
  if (!held_) o4_.commit(o2, s4_.data());
  return out;
}

}  // namespace magmora::pedal
