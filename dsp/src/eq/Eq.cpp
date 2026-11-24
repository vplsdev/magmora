#include "eq/Eq.h"

#include <algorithm>
#include <cmath>

#include <magmora/ChainParams.h>

namespace magmora::eq {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFirst = kEqOn;
int at(int id) { return id - kFirst; }
const ParamInfo& info(int k) { return kChainParams[kFirst - kNumParams + k]; }
// Smoothed in plain units (measured: smoothing frequency in log was worse; see kSlewEqSeconds).
bool logTaper(int) { return false; }
bool continuous(int k) { return info(k).smoothed; }

// Butterworth: one section of Q 1/sqrt(2), or two of Q 0.5412 and 1.3066
constexpr double kButterworth2 = 0.70710678118654752;
constexpr double kButterworth4[2] = {0.54119610014619698, 1.30656296487637652};

SvfCoefficients lerp(const SvfCoefficients& a, const SvfCoefficients& b, double t) {
  return {a.g + t * (b.g - a.g),    a.k + t * (b.k - a.k),    a.m0 + t * (b.m0 - a.m0), a.m1 + t * (b.m1 - a.m1),
          a.m2 + t * (b.m2 - a.m2), a.c1 + t * (b.c1 - a.c1), a.c2 + t * (b.c2 - a.c2)};
}

// Which sections a control feeds (bit i: section i; 0, 1 high-pass, 2, 3 low-pass, 4 to 7 the bands).
unsigned sectionsOf(int k) {
  const int id = kFirst + k;
  if (id == kEqHighPassFreq) return 0x3u;
  if (id == kEqLowPassFreq) return 0xCu;
  if (id >= kEqBand1Freq && id <= kEqBand1Type) return 1u << 4;
  if (id >= kEqBand2Freq && id <= kEqBand2Q) return 1u << 5;
  if (id >= kEqBand3Freq && id <= kEqBand3Q) return 1u << 6;
  if (id >= kEqBand4Freq && id <= kEqBand4Type) return 1u << 7;
  return 0u;
}

}  // namespace

void Eq::prepare(double sampleRate) {
  rate_ = sampleRate;
  for (auto& s : smooth_) s.prepare(sampleRate, kRoundingSeconds, kSlewEqSeconds);
  fadeStep_ = 1.0 / std::max(1.0, kFadeSeconds * sampleRate);
  control_ = std::max(4, static_cast<int>(std::lround(kControlSeconds * sampleRate)));
  for (int k = 0; k < kControls; ++k) plain_[k] = info(k).def;
  snapControls(plain_);
  wanted_ = fading_ = switching_ = false;
  wet_ = 0.0;
  reset();
}

void Eq::reset() {
  for (Core& c : core_) c.state = State{};
  phase_ = 0;
  leg_ = false;
  dirty_ = 0;
}

void Eq::snapControls(const double* plain) {
  for (int k = 0; k < kControls; ++k) {
    const double v = std::clamp(plain[k], info(k).min, info(k).max);
    smooth_[k].snap(logTaper(k) ? std::log(v) : v);
  }
  double s[kControls];
  smoothedValues(s);
  Core& c = core_[active_];
  c.config = configOf(plain);
  design(c.config, s, c.to, c.on);
  std::copy(std::begin(c.to), std::end(c.to), c.from);
  refresh(c);
  moving_ = false;
}

void Eq::refresh(Core& c) {
  for (int i = 0; i < kSections; ++i) c.fixed[i] = Step::of(c.to[i]);
}

void Eq::smoothedValues(double* out) const {
  for (int k = 0; k < kControls; ++k) out[k] = logTaper(k) ? std::exp(smooth_[k].value()) : smooth_[k].value();
}

Eq::Config Eq::configOf(const double* p) const {
  Config c;
  c.hpStages = static_cast<int>(std::lround(p[at(kEqHighPassSlope)]));
  c.lpStages = static_cast<int>(std::lround(p[at(kEqLowPassSlope)]));
  c.outer[0] = p[at(kEqBand1Type)] >= 0.5 ? Kind::LowShelf : Kind::Bell;
  c.outer[1] = p[at(kEqBand4Type)] >= 0.5 ? Kind::HighShelf : Kind::Bell;
  return c;
}

void Eq::design(const Config& c, const double* s, SvfCoefficients* out, bool* on, unsigned mask) const {
  auto set = [&](int index, const Section& section) {
    if (mask & (1u << index)) out[index] = toSvf(matched(section, rate_));
    on[index] = true;
  };
  for (int i = 0; i < kSections; ++i) on[i] = false;
  for (int i = 0; i < c.hpStages; ++i)
    set(i, {Kind::HighPass, s[at(kEqHighPassFreq)], 0.0, c.hpStages == 1 ? kButterworth2 : kButterworth4[i]});
  for (int i = 0; i < c.lpStages; ++i)
    set(2 + i, {Kind::LowPass, s[at(kEqLowPassFreq)], 0.0, c.lpStages == 1 ? kButterworth2 : kButterworth4[i]});
  const int freq[4] = {at(kEqBand1Freq), at(kEqBand2Freq), at(kEqBand3Freq), at(kEqBand4Freq)};
  const Kind kinds[4] = {c.outer[0], Kind::Bell, Kind::Bell, c.outer[1]};
  for (int b = 0; b < 4; ++b) set(4 + b, {kinds[b], s[freq[b]], s[freq[b] + 1], s[freq[b] + 2]});
}

void Eq::setControls(const double* plain, bool wanted) {
  if (wanted && !wanted_ && wet_ == 0.0) {  // from idle: start from the settings as they are, empty states
    std::copy(plain, plain + kControls, plain_);
    snapControls(plain);
    reset();
  }
  wanted_ = wanted;
  fading_ = wanted ? wet_ < 1.0 : wet_ > 0.0;
  for (int k = 0; k < kControls; ++k) {
    plain_[k] = std::clamp(plain[k], info(k).min, info(k).max);
    if (continuous(k)) smooth_[k].setTarget(logTaper(k) ? std::log(plain_[k]) : plain_[k]);
  }
  for (const auto& s : smooth_) moving_ = moving_ || s.moving();
  const Config next = configOf(plain_);
  if (!switching_ && next != core_[active_].config) {  // a switch: the other core takes the new settings, crossfade
    Core& from = core_[active_];
    Core& to = core_[1 - active_];
    to.state = from.state;
    to.config = next;
    double s[kControls];
    smoothedValues(s);
    design(to.config, s, to.to, to.on);
    std::copy(std::begin(to.to), std::end(to.to), to.from);
    refresh(to);
    // a section that was not running starts from empty states
    for (int i = 0; i < kSections; ++i)
      if (to.on[i] && !from.on[i])
        for (int ch = 0; ch < 2; ++ch) to.state.ic1[i][ch] = to.state.ic2[i][ch] = to.state.x1[i][ch] = to.state.x2[i][ch] = 0.0;
    active_ = 1 - active_;
    mix_ = 0.0;
    switching_ = true;
  }
}

template <class Sample>
void Eq::run(Sample* left, Sample* right, int frames) {
  Sample* ch[2] = {left, right};
  const int channels = right ? 2 : 1;
  for (int n = 0; n < frames; ++n) {
    if (moving_) {  // the smoothers run every sample, as the head's do, so a target is taken when it arrives
      bool still = false;
      for (int k = 0; k < kControls; ++k)
        if (smooth_[k].moving()) {
          dirty_ |= sectionsOf(k);
          smooth_[k].next();
          still = still || smooth_[k].moving();
        }
      moving_ = still;
    }
    if (phase_ == 0 && dirty_) {  // a control point (on a free-running grid): from the last design to one at the
                                  // values as they are now
      double s[kControls];
      smoothedValues(s);
      for (int c = 0; c < (switching_ ? 2 : 1); ++c) {
        Core& core = core_[c == 0 ? active_ : 1 - active_];
        std::copy(std::begin(core.to), std::end(core.to), core.from);
        design(core.config, s, core.to, core.on, dirty_);  // only the sections whose controls moved
      }
      dirty_ = 0;
      leg_ = true;
    }
    const bool interpolating = leg_;
    const double t = interpolating ? static_cast<double>(phase_ + 1) / control_ : 1.0;
    double dry[2] = {0.0, 0.0}, y[2][2];
    for (int c = 0; c < channels; ++c) dry[c] = y[0][c] = y[1][c] = static_cast<double>(ch[c][n]);
    for (int which = 0; which < (switching_ ? 2 : 1); ++which) {
      Core& core = core_[which == 0 ? active_ : 1 - active_];
      for (int i = 0; i < kSections; ++i) {
        if (!core.on[i]) continue;
        Step moving;
        if (interpolating) moving = Step::of(lerp(core.from[i], core.to[i], t));
        const Step& step = interpolating ? moving : core.fixed[i];
        for (int c = 0; c < channels; ++c)
          y[which][c] = step.run(y[which][c], core.state.ic1[i][c], core.state.ic2[i][c], core.state.x1[i][c], core.state.x2[i][c]);
      }
    }
    for (int c = 0; c < channels; ++c) {
      double wet = y[0][c];
      if (switching_) {
        const double w = 0.5 - 0.5 * std::cos(kPi * mix_);
        wet = y[1][c] + w * (y[0][c] - y[1][c]);
      }
      double out = wet;
      if (fading_ || wet_ < 1.0) {
        const double w = 0.5 - 0.5 * std::cos(kPi * wet_);
        out = dry[c] + w * (wet - dry[c]);
      }
      ch[c][n] = static_cast<Sample>(out);
    }
    if (++phase_ >= control_) {  // the grid ticks on; a leg that ends hands its end on as the next start
      phase_ = 0;
      if (leg_)
        for (Core& core : core_) std::copy(std::begin(core.to), std::end(core.to), core.from);
      leg_ = false;
      for (Core& core : core_) refresh(core);
    }
    if (switching_ && (mix_ += fadeStep_) >= 1.0) {
      mix_ = 1.0;
      switching_ = false;
    }
    if (fading_) {
      wet_ = std::clamp(wet_ + (wanted_ ? fadeStep_ : -fadeStep_), 0.0, 1.0);
      if (wet_ == 0.0 || wet_ == 1.0) fading_ = false;
    }
  }
}

void Eq::process(float* left, float* right, int frames) {
  if (!idle() && frames > 0) run(left, right, frames);
}
void Eq::process(double* left, double* right, int frames) {
  if (!idle() && frames > 0) run(left, right, frames);
}

std::complex<double> Eq::designedResponse(double hz) const {
  const Core& c = core_[active_];
  double s[kControls];
  smoothedValues(s);
  std::complex<double> h = 1.0;
  const Config& cfg = c.config;
  auto add = [&](const Section& section) { h *= response(matched(section, rate_), 2 * kPi * hz, rate_); };
  for (int i = 0; i < cfg.hpStages; ++i)
    add({Kind::HighPass, s[at(kEqHighPassFreq)], 0.0, cfg.hpStages == 1 ? kButterworth2 : kButterworth4[i]});
  for (int i = 0; i < cfg.lpStages; ++i)
    add({Kind::LowPass, s[at(kEqLowPassFreq)], 0.0, cfg.lpStages == 1 ? kButterworth2 : kButterworth4[i]});
  const int freq[4] = {at(kEqBand1Freq), at(kEqBand2Freq), at(kEqBand3Freq), at(kEqBand4Freq)};
  const Kind kinds[4] = {cfg.outer[0], Kind::Bell, Kind::Bell, cfg.outer[1]};
  for (int b = 0; b < 4; ++b) add({kinds[b], s[freq[b]], s[freq[b] + 1], s[freq[b] + 2]});
  return h;
}

}  // namespace magmora::eq
