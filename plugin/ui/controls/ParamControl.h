// Base classes of everything that edits a parameter: gestures, dragging, wheel and typed values.
#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "Common.h"

namespace magmora {
namespace ui {

// Any parameter of the chain: the head's (Params.h) or a block's (ChainParams.h).
inline const ParamInfo& Info(int param) { return *Chain::parameterInfo(param); }

inline std::string DisplayValue(int param, double value) {
  const ParamInfo& p = Info(param);
  char text[48];
  const std::string unit = p.unit;
  if (param == kTranspose) {  // whole semitones, with a typographic minus
    const long st = std::lround(value);
    if (st == 0) return "0 st";
    std::snprintf(text, sizeof text, "%s%ld st", st < 0 ? "\xE2\x88\x92" : "+", std::labs(st));
  } else if (param == kGateThreshold) {
    if (value <= p.min + 0.05) return "Off";
    std::snprintf(text, sizeof text, "%.1f dB", value);
  } else if (unit == "dB") {
    if (std::fabs(value) < 0.05) return "0.0 dB";
    std::snprintf(text, sizeof text, "%+.1f dB", value);
  } else if (unit == "Hz") {
    if (value >= 1000.)
      std::snprintf(text, sizeof text, value >= 9950. ? "%.1f kHz" : "%.2f kHz", value / 1000.);
    else
      std::snprintf(text, sizeof text, "%.0f Hz", value);
  } else if (unit == "mm") {
    std::snprintf(text, sizeof text, "%.1f mm", value);
  } else if (unit == "deg") {
    std::snprintf(text, sizeof text, "%.0f deg", value);
  } else if (param == kMic1Pan || param == kMic2Pan) {
    if (std::fabs(value) < 0.005) return "Centre";
    std::snprintf(text, sizeof text, "%s %.0f", value < 0 ? "L" : "R", std::fabs(value) * 100.);
  } else if (param == kEqBand1Q || param == kEqBand2Q || param == kEqBand3Q || param == kEqBand4Q) {
    std::snprintf(text, sizeof text, "%.2f", value);
  } else if (param == kMic1Position || param == kMic2Position) {
    std::snprintf(text, sizeof text, "%.0f %%", value * 100.);
  } else {
    std::snprintf(text, sizeof text, "%.1f", value);
  }
  return text;
}

// False if the text holds no number: the value then stays as it was.
inline bool ParseValue(int param, const char* str, double& value) {
  if (!str) return false;
  const ParamInfo& p = Info(param);
  std::string s;
  for (const char* c = str; *c; ++c) {
    if (std::strncmp(c, "\xE2\x88\x92", 3) == 0) {  // the typographic minus, as DisplayValue writes it
      s += '-';
      c += 2;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(*c))) continue;
    s += *c == ',' ? '.' : static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
  }
  if (param == kGateThreshold && s == "off") {
    value = p.min;
    return true;
  }
  const bool pan = param == kMic1Pan || param == kMic2Pan;
  if (pan && (s == "c" || s == "centre" || s == "center")) {
    value = 0.;
    return true;
  }
  double sign = 1.;
  if (pan && !s.empty() && (s[0] == 'l' || s[0] == 'r')) {
    sign = s[0] == 'l' ? -1. : 1.;
    s.erase(0, 1);
  }
  if (!s.empty() && s[0] == 'q') s.erase(0, 1);
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(v)) return false;
  const std::string rest(end);
  if (rest == "khz" || rest == "k") v *= 1000.;
  else if (rest == "%" && (param == kMic1Position || param == kMic2Position)) v /= 100.;
  else if (param == kTranspose && (rest == "st" || rest == "semitones")) v = std::round(v);
  else if (!(rest.empty() || rest == "db" || rest == "hz" || rest == "mm" || rest == "deg" || rest == "%")) return false;
  if (pan) v = sign * (std::fabs(v) > 1. ? v / 100. : v);
  value = std::min(std::max(v, p.min), p.max);
  return true;
}

// The control is deliberately not linked to its parameter through IGraphics. It reads the parameter every frame,
// which covers automation that only reaches process(), and it opens and closes every host gesture itself:
// IGraphics brackets none for double clicks, the wheel or typed values.
class ParamControl : public IControl {
public:
  ParamControl(const IRECT& bounds, FramePtr frame, int param)
  : IControl(bounds), mFrame(std::move(frame)), mParam(param) {}

  bool IsDirty() override {
    const double v = Normalised();
    if (v != mShown) {
      mShown = v;
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    if (!mMouseIsOver) {
      mOverSince = mFrame->now;
      GetUI()->SetMouseCursor(Cursor());
    }
    IControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override {
    if (mMouseIsOver) GetUI()->SetMouseCursor(ECursor::ARROW);
    IControl::OnMouseOut();
  }

  // The window is closing, or the gesture was taken away.
  virtual void Cancel() { End(); }

  // While the mouse holds the control, the mouse wins over automation and over the host's echo of older values.
  double Normalised() const {
    const double v = mInGesture ? mValue : Param().GetNormalized();
    return std::isfinite(v) ? std::min(std::max(v, 0.0), 1.0) : 0.0;
  }

  double Plain() const { return Param().FromNormalized(Normalised()); }
  int ParamId() const { return mParam; }

protected:
  virtual ECursor Cursor() const { return ECursor::HAND; }

  const IParam& Param() const { return *const_cast<ParamControl*>(this)->GetDelegate()->GetParam(mParam); }

  void Begin() {
    if (mInGesture) return;
    mValue = Param().GetNormalized();
    mInGesture = true;
    GetDelegate()->BeginInformHostOfParamChangeFromUI(mParam);
  }

  void Move(double normalised) {
    if (!mInGesture) return;
    const double v = std::min(std::max(normalised, 0.0), 1.0);
    if (v == mValue) return;
    mValue = v;
    GetDelegate()->SendParameterValueFromUI(mParam, v);
    SetDirty(false);
  }

  void End() {
    if (!mInGesture) return;
    mInGesture = false;
    GetDelegate()->EndInformHostOfParamChangeFromUI(mParam);
    SetDirty(false);
  }

  void SetOnce(double normalised) {
    End();
    Begin();
    Move(normalised);
    End();
  }

  FramePtr mFrame;
  const int mParam;
  double mOverSince = 0.;
  double mValue = 0.;
  bool mInGesture = false;

private:
  double mShown = -1.;
};

// Vertical drag, Shift (or Ctrl) for fine, double click or Alt-click for the default, wheel, right click to type.
class DragControl : public ParamControl {
public:
  DragControl(const IRECT& bounds, FramePtr frame, int param, float travel, bool horizontalToo)
  : ParamControl(bounds, std::move(frame), param), mTravel(travel), mHorizontalToo(horizontalToo) {}

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    End();
    mWheelUntil = 0.;
    if (mod.R) {
      TypeValue();
    } else if (mod.A) {
      SetOnce(Param().GetDefault(true));
    } else {
      mDragging = true;
      Begin();
    }
  }

  void OnMouseDrag(float, float, float dX, float dY, const IMouseMod& mod) override {
    if (!mDragging) return;
    if (!mCursorHidden) {
      mCursorHidden = true;
      GetUI()->HideMouseCursor(true, true);
    }
    // Screen points, so the travel feels the same at every UI scale.
    const float points = (-dY + (mHorizontalToo ? dX : 0.f)) * GetUI()->GetDrawScale();
    Move(mValue + points / mTravel * (Fine(mod) ? kFine : 1.f));
  }

  void OnMouseUp(float, float, const IMouseMod&) override { StopDrag(); }
  void OnTouchCancelled(float, float, const IMouseMod&) override { StopDrag(); }

  void OnMouseDblClick(float, float, const IMouseMod& mod) override {
    if (mod.R) return;
    StopDrag();
    mWheelUntil = 0.;
    SetOnce(Param().GetDefault(true));
  }

  void OnMouseWheel(float, float, const IMouseMod& mod, float d) override {
    if (mDragging || d == 0.f) return;
    Begin();
    const float notches = std::min(std::max(std::fabs(d), 0.2f), 6.f);
    Move(mValue + (d > 0.f ? 1 : -1) * notches * WheelStep() * (Fine(mod) ? kFine : 1.f));
    // A turn of the wheel is one gesture, closed when it has come to rest.
    mWheelUntil = mFrame->now + 0.35;
  }

  bool IsDirty() override {
    // A resize or a double click releases the capture without a mouse-up.
    if (mDragging && !GetUI()->ControlIsCaptured(this)) StopDrag();
    if (mWheelUntil > 0. && mFrame->now > mWheelUntil) {
      mWheelUntil = 0.;
      End();
    }
    return ParamControl::IsDirty();
  }

  void OnTextEntryCompletion(const char* str, int) override {
    double value;
    // Unchanged text must not round the value to the precision it was shown with.
    if (str && mTyped != str && ParseValue(mParam, str, value)) SetOnce(Param().ToNormalized(value));
  }

  void Cancel() override {
    mWheelUntil = 0.;
    StopDrag();
    End();
  }

  bool ShowsValue() const {
    return mDragging || mWheelUntil > 0. || (mMouseIsOver && mFrame->now - mOverSince > 0.25);
  }

protected:
  ECursor Cursor() const override { return ECursor::SIZENS; }
  virtual double WheelStep() const { return 0.01; }  // normalised travel per notch
  virtual IRECT EntryRect() const = 0;
  virtual IText EntryText() const = 0;

private:
  static constexpr float kFine = 0.1f;
  static bool Fine(const IMouseMod& mod) { return mod.S || mod.C; }

  void StopDrag() {
    if (mCursorHidden) {
      mCursorHidden = false;
      GetUI()->HideMouseCursor(false);
    }
    if (mDragging) {
      mDragging = false;
      End();
    }
  }

  void TypeValue() {
    mTyped = DisplayValue(mParam, Param().Value());
    const size_t unit = mTyped.find(" dB");
    if (unit != std::string::npos) mTyped.erase(unit);
    IText text = EntryText();
    text.mTextEntryBGColor = style::Shade(0.45f);
    text.mTextEntryFGColor = style::Bright();
    // Not bound to the parameter: iPlug2 would turn an emptied field into 0 and cannot accept "Off".
    GetUI()->CreateTextEntry(*this, text, EntryRect(), mTyped.c_str(), kNoValIdx);
  }

  const float mTravel;
  const bool mHorizontalToo;
  bool mDragging = false;
  bool mCursorHidden = false;
  double mWheelUntil = 0.;
  std::string mTyped;
};

}  // namespace ui
}  // namespace magmora
