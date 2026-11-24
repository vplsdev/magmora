// The strip above the amp: vectors and type on the wall, separated by spacing alone.
#pragma once

#include "Overlay.h"
#include "ParamControl.h"

namespace magmora {
namespace ui {

constexpr float kBarTextY = 19.f;   // centre line of the type
constexpr float kBarLineY = 34.5f;  // meters
constexpr float kBarTextSize = 15.f;
constexpr float kFieldTravel = 300.f;

inline IRECT BarRect(float left, float right) { return IRECT(left, layout::kBar.y, right, layout::kBar.y + layout::kBar.h); }
inline IRECT BarText(const IRECT& r) { return IRECT(r.L, kBarTextY - 10.f, r.R, kBarTextY + 10.f); }

// The Input meter's target: where the loudest picking of a typical guitar should land, in dBFS after the trim.
//
// The engine takes 1.0 full scale at its input as 2.0 V peak at the amp's input jack (Calibration::inputVoltsPerFS,
// Engine.h), and the meter reads after the trim, so the marks are volts at the jack: -18 dBFS is 0.25 V peak and
// -6 dBFS is 1.0 V peak. Passive pickups played hard peak at about 0.25 V (single coils) to 1 V (hot humbuckers) into
// an amp's input, and that is the range the circuit's first stage was designed for. A DI recorded with the interface
// near its lowest gain (full scale about +5 to +13 dBu, i.e. 2 to 5 V peak) lands there with the trim at 0 dB; a DI
// recorded hotter or quieter is brought between the marks with the trim, so the amp hears the level a guitar plugged
// straight into it would give. README, "The bar", says the same.
constexpr float kInputTargetLowDb = -18.f, kInputTargetHighDb = -6.f;

// A compact drag field with a thin meter under it.
class BarField final : public DragControl {
public:
  enum class Meter { Input, Gate, Output };

  BarField(const IRECT& r, FramePtr frame, int param, const char* label, Meter meter)
  : DragControl(r, std::move(frame), param, kFieldTravel, true), mLabel(label), mMeter(meter) {}

  bool IsDirty() override {
    const Frame& f = *mFrame;
    float level = f.inputDb, hold = f.inputHoldDb;
    if (mMeter == Meter::Output) {
      level = f.outputDb;
      hold = f.outputHoldDb;
    }
    const int x = static_cast<int>(Position(level) * 2.f), h = static_cast<int>(Position(hold) * 2.f);
    const bool open = f.gateGain >= 0.5f;
    const bool clip = Clipped();
    if (x != mLevelX || (mMeter != Meter::Gate && h != mHoldX) || (mMeter == Meter::Gate && open != mGateOpen) ||
        clip != mClip) {
      mLevelX = x;
      mHoldX = h;
      mGateOpen = open;
      mClip = clip;
      SetDirty(false);
    }
    return DragControl::IsDirty();
  }

  // A click on the clip mark clears it; anywhere else on the field it drags as before.
  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    if (mod.L && Clipped() && ClipTarget().Contains(x, y)) {
      (mMeter == Meter::Input ? mFrame->inputClipUntil : mFrame->outputClipUntil) = 0.;
      SetDirty(false);
      return;
    }
    DragControl::OnMouseDown(x, y, mod);
  }

  bool Clipped() const {
    if (mMeter == Meter::Gate) return false;
    return mFrame->now < (mMeter == Meter::Input ? mFrame->inputClipUntil : mFrame->outputClipUntil);
  }

  void Draw(IGraphics& g) override {
    const IRECT text = BarText(mRECT);
    const bool hot = GetMouseIsOver() || mInGesture;
    g.DrawText(style::Sans(kBarTextSize, hot ? style::Bright() : style::Middle()), mLabel, text);
    const double plain = Plain();
    const bool off = mParam == kGateThreshold && plain <= kParams[mParam].min + 0.05;
    const IColor valueColour = off && !hot ? style::Middle() : style::Bright();
    g.DrawText(style::Sans(kBarTextSize, valueColour, EAlign::Far), DisplayValue(mParam, plain).c_str(), text);

    const float left = mRECT.L, level = left + mLevelX / 2.f;
    Line(g, style::Shade(0.6f), left, mRECT.R);
    if (mMeter == Meter::Gate) {
      if (off) return;
      Line(g, mGateOpen ? style::MeterLow() : style::Dim(), left, level);
      const float t = left + Position(static_cast<float>(plain));
      g.FillRect(style::Bright(), IRECT(t - 0.75f, kBarLineY - 3.f, t + 0.75f, kBarLineY + 3.f));
      return;
    }
    if (mMeter == Meter::Input) {  // the target: two hairlines (kInputTargetLowDb, kInputTargetHighDb)
      for (float db : {kInputTargetLowDb, kInputTargetHighDb}) {
        const float t = std::round(left + Position(db));
        g.FillRect(style::Dim(), IRECT(t, kBarLineY - 4.f, t + 1.f, kBarLineY + 1.f));
      }
    }
    if (mClip) g.FillRect(style::MeterClip(), IRECT(mRECT.R - 2.f, kBarLineY - 4.f, mRECT.R, kBarLineY + 2.f));
    const float warm = left + Position(-12.f), hot_ = left + Position(Frame::kClipDb);
    Line(g, style::MeterLow(), left, std::min(level, warm));
    Line(g, style::MeterHigh(), warm, std::min(level, hot_));
    Line(g, style::MeterClip(), hot_, level);
    if (mHoldX > 0) {
      const float t = left + mHoldX / 2.f;
      Line(g, t > hot_ ? style::MeterClip() : t > warm ? style::MeterHigh() : style::MeterLow(), t - 1.5f, t);
    }
  }

protected:
  IRECT EntryRect() const override { return IRECT(mRECT.MW(), kBarTextY - 11.f, mRECT.R, kBarTextY + 11.f); }
  IText EntryText() const override { return style::Sans(kBarTextSize, style::Bright(), EAlign::Far); }

private:
  // Gate: the threshold's own range, up to full scale. Levels: -60 to +6 dBFS.
  float Position(float db) const {
    const float lo = mMeter == Meter::Gate ? -96.f : -60.f, hi = mMeter == Meter::Gate ? 0.f : 6.f;
    return mRECT.W() * std::min(std::max((db - lo) / (hi - lo), 0.f), 1.f);
  }

  IRECT ClipTarget() const { return IRECT(mRECT.R - 9.f, kBarLineY - 8.f, mRECT.R + 1.f, mRECT.B); }

  static void Line(IGraphics& g, const IColor& c, float from, float to) {
    if (to > from) g.FillRect(c, IRECT(from, kBarLineY - 1.f, to, kBarLineY + 1.f));
  }

  const char* mLabel;
  const Meter mMeter;
  int mLevelX = 0, mHoldX = 0;
  bool mGateOpen = true, mClip = false;
};

// The transposer: semitones in the bar's type, as the fields, and its mode as a small caption under them, where the
// fields have their meters. The value drags, scrolls (a notch a semitone), types and resets to 0; a click on the
// caption switches between Tight and Standard.
class TransposeField final : public DragControl {
public:
  TransposeField(const IRECT& r, FramePtr frame) : DragControl(r, std::move(frame), kTranspose, kFieldTravel, true) {
    SetTooltip("Tight: 5.3 ms, for playing. Standard: 10.7 ms, smoother low notes. Click the mode to switch");
  }

  // Where the mode caption takes clicks: the lower part of the field.
  IRECT ModeRect() const { return IRECT(mRECT.L, kBarLineY - 8.f, mRECT.R, mRECT.B); }
  bool OverMode() const { return mMouseIsOver && mOverMode; }

  bool IsDirty() override {
    const int mode = Mode();
    const bool over = OverMode();
    if (mode != mShownMode || over != mShownOver) {
      mShownMode = mode;
      mShownOver = over;
      SetDirty(false);
    }
    return DragControl::IsDirty();
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    const bool over = ModeRect().Contains(x, y);
    if (over != mOverMode || !mMouseIsOver) GetUI()->SetMouseCursor(over ? ECursor::HAND : Cursor());
    mOverMode = over;
    DragControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override {
    mOverMode = false;
    DragControl::OnMouseOut();
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    if (mod.L && !mod.A && ModeRect().Contains(x, y)) {
      ToggleMode();
      return;
    }
    DragControl::OnMouseDown(x, y, mod);
  }

  // A double click on the caption is two clicks: it switches twice. On the value it resets to 0.
  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override {
    if (mod.L && ModeRect().Contains(x, y)) {
      ToggleMode();
      return;
    }
    DragControl::OnMouseDblClick(x, y, mod);
  }

  void Draw(IGraphics& g) override {
    const IRECT text = BarText(mRECT);
    const bool hot = (GetMouseIsOver() && !OverMode()) || mInGesture;
    g.DrawText(style::Sans(kBarTextSize, hot ? style::Bright() : style::Middle()), "Transpose", text);
    const double plain = Plain();
    const bool zero = std::lround(plain) == 0;
    g.DrawText(style::Sans(kBarTextSize, zero && !hot ? style::Middle() : style::Bright(), EAlign::Far),
               DisplayValue(mParam, plain).c_str(), text);
    // the mode: dim while the transposer is out (0 st), brighter while it plays, bright under the pointer
    const IColor caption = OverMode() ? style::Bright() : zero ? style::DimText() : style::Middle();
    g.DrawText(style::Panel(10.5f, caption, EAlign::Near), Mode() == static_cast<int>(TransposeMode::Standard) ? "STANDARD" : "TIGHT",
               IRECT(mRECT.L, kBarLineY - 6.f, mRECT.R, kBarLineY + 5.f));
  }

  int Mode() const {
    const double v = const_cast<TransposeField*>(this)->GetDelegate()->GetParam(kTransposeMode)->Value();
    return static_cast<int>(std::lround(v));
  }

protected:
  double WheelStep() const override { return 1.0 / (Info(kTranspose).max - Info(kTranspose).min); }  // a semitone
  IRECT EntryRect() const override { return IRECT(mRECT.MW(), kBarTextY - 11.f, mRECT.R, kBarTextY + 11.f); }
  IText EntryText() const override { return style::Sans(kBarTextSize, style::Bright(), EAlign::Far); }

private:
  void ToggleMode() {
    auto& d = *GetDelegate();
    const int next = Mode() == 0 ? 1 : 0;
    d.BeginInformHostOfParamChangeFromUI(kTransposeMode);
    d.SendParameterValueFromUI(kTransposeMode, d.GetParam(kTransposeMode)->ToNormalized(next));
    d.EndInformHostOfParamChangeFromUI(kTransposeMode);
    SetDirty(false);
  }

  bool mOverMode = false, mShownOver = false;
  int mShownMode = -1;
};

// A word that opens a menu or does something.
class BarWord : public IControl {
public:
  using Label = std::function<std::string()>;
  using Action = std::function<void(const IRECT&)>;

  BarWord(const IRECT& r, Label label, Action action, EAlign align = EAlign::Center, const char* caption = nullptr)
  : IControl(r), mLabel(std::move(label)), mAction(std::move(action)), mAlign(align), mCaption(caption) {
    mDblAsSingleClick = true;
  }

  bool IsDirty() override {
    std::string s = mLabel ? mLabel() : std::string();
    if (s != mShown) {
      mShown = std::move(s);
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (mod.L) mAction(mRECT);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    if (!mMouseIsOver) GetUI()->SetMouseCursor(ECursor::HAND);
    IControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override {
    if (mMouseIsOver) GetUI()->SetMouseCursor(ECursor::ARROW);
    IControl::OnMouseOut();
  }

  void Draw(IGraphics& g) override {
    g.DrawText(style::Sans(kBarTextSize, Tone(), mAlign), mShown.c_str(), BarText(mRECT));
    // A small caption under the word, where the fields have their meters: what the word is a setting of.
    if (mCaption)
      g.DrawText(style::Panel(10.5f, style::DimText(), mAlign), mCaption, IRECT(mRECT.L, kBarLineY - 6.f, mRECT.R, kBarLineY + 5.f));
  }

  const char* Caption() const { return mCaption; }

protected:
  IColor Tone() const { return GetMouseIsOver() ? style::Bright() : style::Middle(); }

  Label mLabel;
  Action mAction;
  EAlign mAlign;
  const char* mCaption;
  std::string mShown;
};

class BarChevron final : public BarWord {
public:
  BarChevron(const IRECT& r, int direction, Action action) : BarWord(r, nullptr, std::move(action)), mDir(direction) {}

  void Draw(IGraphics& g) override {
    const float x = mRECT.MW(), y = kBarTextY, d = 2.5f * mDir;
    g.PathClear();
    g.PathMoveTo(x - d, y - 5.f);
    g.PathLineTo(x + d, y);
    g.PathLineTo(x - d, y + 5.f);
    IStrokeOptions round;
    round.mCapOption = ELineCap::Round;
    round.mJoinOption = ELineJoin::Round;
    g.PathStroke(Tone(), 1.5f, round);
  }

private:
  const int mDir;
};

// The preset's name, brighter than the rest of the bar, with a mark once it has been edited.
class BarPreset final : public BarWord {
public:
  BarPreset(const IRECT& r, Label label, Action action) : BarWord(r, std::move(label), std::move(action), EAlign::Near) {}

  void Draw(IGraphics& g) override {
    const IRECT r = BarText(mRECT);
    const IText text = style::Sans(16.f, style::Bright());
    g.DrawText(text, mShown.c_str(), r);
    IRECT measured = r;
    g.MeasureText(text, mShown.c_str(), measured);
    const float x = measured.R + 9.f, y = kBarTextY + 1.f;
    g.PathClear();
    g.PathMoveTo(x - 3.5f, y - 2.f);
    g.PathLineTo(x, y + 2.f);
    g.PathLineTo(x + 3.5f, y - 2.f);
    g.PathStroke(Tone(), 1.3f);
  }
};

// The tuner: a tuning fork, in vector.
class BarTuner final : public BarWord {
public:
  BarTuner(const IRECT& r, Action action) : BarWord(r, nullptr, std::move(action)) {}

  void Draw(IGraphics& g) override {
    const float x = mRECT.MW(), y = kBarTextY;
    IStrokeOptions round;
    round.mCapOption = ELineCap::Round;
    round.mJoinOption = ELineJoin::Round;
    g.PathClear();
    g.PathMoveTo(x + 3.2f, y - 8.f);
    g.PathLineTo(x + 3.2f, y + 0.5f);
    g.PathArc(x, y + 0.5f, 3.2f, 90.f, 270.f);
    g.PathLineTo(x - 3.2f, y - 8.f);
    g.PathStroke(Tone(), 1.4f, round);
    g.PathClear();
    g.PathMoveTo(x, y + 3.7f);
    g.PathLineTo(x, y + 8.5f);
    g.PathStroke(Tone(), 1.6f, round);
  }
};

class BarAbout final : public BarWord {
public:
  BarAbout(const IRECT& r, Action action) : BarWord(r, nullptr, std::move(action)) {}

  void Draw(IGraphics& g) override {
    const float x = mRECT.MW(), y = kBarTextY;
    g.DrawCircle(Tone(), x, y, 7.5f, nullptr, 1.2f);
    g.FillCircle(Tone(), x, y - 3.4f, 1.1f);
    g.FillRect(Tone(), IRECT(x - 0.8f, y - 1.2f, x + 0.8f, y + 4.2f));
  }
};

}  // namespace ui
}  // namespace magmora
