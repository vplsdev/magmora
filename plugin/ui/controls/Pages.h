// The block pages (pedal, EQ, cabinet): their scenes and controls, from plugin/resources/img/pages.json.
#pragma once

#include <complex>
#include <functional>

#include "AtlasKnob.h"
#include "Sections.h"

namespace magmora {
namespace ui {

namespace pages {
enum class Kind { Knob, Selector, Switch, Action, Lamp, Window };
struct Control {
  Kind kind;
  const char* id;
  int param;           // -1: not a parameter (actions, windows, placeholders)
  const char* file;
  float x, y, w, h, hitW, hitH;
  int frames, columns;
  int source;          // a lamp's parameter, or -1 when it follows a setting named by sourceName
  const char* sourceName;
  float onValue;
  bool disabled;       // drawn dimmed and inert: a control the engine does not have yet
  int detents[3];      // a selector's atlas frame for each value
  int nDetents;
};
struct Page {
  const char* name;
  const char* title;
  const char* background;
  const Control* controls;
  int count;
};
#include "../../generated/PageLayout.inc"

inline const Page* Find(const char* name) {
  for (const Page& p : kPages)
    if (std::strcmp(p.name, name) == 0) return &p;
  return nullptr;
}
inline const Control* Find(const Page& page, const char* id) {
  for (int i = 0; i < page.count; ++i)
    if (std::strcmp(page.controls[i].id, id) == 0) return &page.controls[i];
  return nullptr;
}
inline IRECT Cell(const Control& c) { return layout::Centred(c.x, c.y, c.w, c.h); }
inline IRECT Hit(const Control& c) { return layout::Centred(c.x, c.y, c.hitW, c.hitH); }
constexpr float kDisabled = 0.42f;
}  // namespace pages

// A knob of a page: the amp's handling, the page's atlas. A placeholder knob is dimmed and inert.
class PageKnob final : public DragControl {
public:
  PageKnob(const pages::Control& c, FramePtr frame, int param = -2, bool disabled = false)
  : DragControl(pages::Cell(c), std::move(frame), param != -2 ? std::max(param, 0) : std::max(c.param, 0), kKnobTravel,
                false),
    mC(c), mAtlas(c.file, this), mDisabled(disabled || c.disabled || (param == -2 ? c.param : param) < 0) {
    SetTargetRECT(pages::Hit(c));
    if (mDisabled) mIgnoreMouse = true;
  }

  IRECT ReadoutRect() const { return layout::Centred(mC.x, mC.y - mC.hitH / 2 - 14.f, 52.f, 20.f); }
  bool IsDisabled() const { return mDisabled; }

  bool IsDirty() override {
    if (mAtlas.Update(*GetUI())) SetDirty(false);
    return mDisabled ? IControl::IsDirty() : DragControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    // A disabled knob keeps showing where it stands (a slot's position while it holds a file), dimmed.
    const int frame = mDisabled && mC.param < 0 ? (mC.frames - 1) / 2
                                                : static_cast<int>(std::lround(Normalised() * (mC.frames - 1)));
    const IBlend dim(EBlend::SrcOver, pages::kDisabled);
    mAtlas.Draw(g, pages::Cell(mC), static_cast<float>((frame % mC.columns) * static_cast<int>(mC.w)),
                static_cast<float>((frame / mC.columns) * static_cast<int>(mC.h)), mDisabled ? &dim : nullptr);
  }

protected:
  IRECT EntryRect() const override { return ReadoutRect(); }
  IText EntryText() const override { return style::Panel(16.f, style::Bright()); }

private:
  const pages::Control& mC;
  Sprite mAtlas;
  const bool mDisabled;
};

// A rotary switch: one atlas frame per printed position. Click steps on, drag steps either way, wheel too.
class PageSelector final : public ParamControl {
public:
  PageSelector(const pages::Control& c, FramePtr frame)
  : ParamControl(pages::Cell(c), std::move(frame), c.param), mC(c), mAtlas(c.file, this) {
    SetTargetRECT(pages::Hit(c));
  }

  bool IsDirty() override {
    if (mAtlas.Update(*GetUI())) SetDirty(false);
    return ParamControl::IsDirty();
  }

  void OnMouseDown(float, float y, const IMouseMod& mod) override {
    mY = y;
    mMoved = false;
    if (mod.A) SetIndex(Index(Param().GetDefault(true)));
  }

  void OnMouseDrag(float, float y, float, float, const IMouseMod&) override {
    const float step = 24.f / GetUI()->GetDrawScale();
    if (std::fabs(y - mY) >= step) {
      SetIndex(Index(Normalised()) + (y < mY ? 1 : -1));
      mY = y;
      mMoved = true;
    }
  }

  void OnMouseUp(float, float, const IMouseMod& mod) override {
    if (!mMoved && mod.L && !mod.A) SetIndex((Index(Normalised()) + 1) % Count());
  }

  void OnMouseWheel(float, float, const IMouseMod&, float d) override {
    if (d != 0.f) SetIndex(Index(Normalised()) + (d > 0.f ? 1 : -1));
  }

  void Draw(IGraphics& g) override {
    const int frame = mC.detents[std::min(std::max(Index(Normalised()), 0), mC.nDetents - 1)];
    mAtlas.Draw(g, pages::Cell(mC), static_cast<float>((frame % mC.columns) * static_cast<int>(mC.w)),
                static_cast<float>((frame / mC.columns) * static_cast<int>(mC.h)));
  }

private:
  int Count() const { return std::max(mC.nDetents, 1); }
  int Index(double normalised) const { return static_cast<int>(std::lround(normalised * (Count() - 1))); }
  void SetIndex(int i) {
    i = std::min(std::max(i, 0), Count() - 1);
    if (i != Index(Normalised())) SetOnce(static_cast<double>(i) / (Count() - 1));
  }

  const pages::Control& mC;
  Sprite mAtlas;
  float mY = 0.f;
  bool mMoved = false;
};

// A two-state control that switches a parameter on the press: push-button, bat toggle, footswitch.
class PageSwitch final : public ParamControl {
public:
  PageSwitch(const pages::Control& c, FramePtr frame, int param = -2, bool disabled = false)
  : ParamControl(pages::Cell(c), std::move(frame), param != -2 ? std::max(param, 0) : std::max(c.param, 0)), mC(c),
    mFrames(c.file, this), mDisabled(disabled || c.disabled || (param == -2 ? c.param : param) < 0) {
    SetTargetRECT(pages::Hit(c));
    mDblAsSingleClick = true;
    if (mDisabled) mIgnoreMouse = true;
  }

  bool IsDirty() override {
    if (mFrames.Update(*GetUI())) SetDirty(false);
    return mDisabled ? IControl::IsDirty() : ParamControl::IsDirty();
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (mod.L) SetOnce(Param().GetNormalized() < 0.5 ? 1. : 0.);
  }

  void Draw(IGraphics& g) override {
    const int frame = !mDisabled && Normalised() >= 0.5 ? 1 : 0;
    const IBlend dim(EBlend::SrcOver, pages::kDisabled);
    mFrames.Draw(g, pages::Cell(mC), frame * mC.w, 0.f, mDisabled ? &dim : nullptr);
  }

private:
  const pages::Control& mC;
  Sprite mFrames;
  const bool mDisabled;
};

// A push-button that does something rather than set a parameter: down while held, or latched by a setting.
class PageAction final : public IControl {
public:
  PageAction(const pages::Control& c, std::function<void()> action, std::function<bool()> latched = nullptr)
  : IControl(pages::Cell(c)), mC(c), mFrames(c.file, this), mAction(std::move(action)), mLatched(std::move(latched)) {
    SetTargetRECT(pages::Hit(c));
    mDblAsSingleClick = true;
    if (c.disabled) mIgnoreMouse = true;
  }

  bool IsDirty() override {
    if (mFrames.Update(*GetUI())) SetDirty(false);
    const bool down = mHeld || (mLatched && mLatched());
    if (down != mDown) {
      mDown = down;
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (!mod.L) return;
    mHeld = true;
    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod&) override {
    if (!mHeld) return;
    mHeld = false;
    SetDirty(false);
    if (mTargetRECT.Contains(x, y) && mAction) mAction();
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
    const IBlend dim(EBlend::SrcOver, pages::kDisabled);
    mFrames.Draw(g, pages::Cell(mC), (mDown && !mC.disabled ? 1.f : 0.f) * mC.w, 0.f, mC.disabled ? &dim : nullptr);
  }

private:
  const pages::Control& mC;
  Sprite mFrames;
  std::function<void()> mAction;
  std::function<bool()> mLatched;
  bool mHeld = false, mDown = false;
};

// A lamp of a page: its lit lens and spill, laid over as the state says, settling in 60 ms.
class PageLamp final : public IControl {
public:
  PageLamp(const pages::Control& c, FramePtr frame, std::function<bool()> on = nullptr)
  : IControl(pages::Cell(c)), mC(c), mFrame(std::move(frame)), mSprite(c.file, this), mOn(std::move(on)) {
    mIgnoreMouse = true;
  }

  void OnAttached() override { mLevel = Target(); }

  bool IsDirty() override {
    if (mSprite.Update(*GetUI())) SetDirty(false);
    const float target = Target();
    mLevel += (target - mLevel) * (1.f - std::exp(-mFrame->dt / 0.02f));
    if (std::fabs(target - mLevel) < 0.004f) mLevel = target;
    const int step = static_cast<int>(mLevel * 128.f + 0.5f);
    if (step != mStep) {
      mStep = step;
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    if (mStep <= 0) return;
    const IBlend blend(EBlend::SrcOver, std::min(mStep / 128.f, 1.f));
    mSprite.Draw(g, mRECT, 0.f, 0.f, &blend);
  }

private:
  float Target() const {
    if (mOn) return mOn() ? 1.f : 0.f;
    if (mC.source < 0) return 0.f;
    const double v = const_cast<PageLamp*>(this)->GetDelegate()->GetParam(mC.source)->Value();
    return std::fabs(v - mC.onValue) < 0.5 ? 1.f : 0.f;
  }

  const pages::Control& mC;
  FramePtr mFrame;
  Sprite mSprite;
  std::function<bool()> mOn;
  float mLevel = 0.f;
  int mStep = -1;
};

// Two parameters edited together by one drag. Each opens its host gesture when it first changes, so a drag that
// moves only one of them records only that one.
class PairDrag {
public:
  void Begin(IGEditorDelegate& d, int a, int b) {
    End(d);
    mP[0] = a;
    mP[1] = b;
    for (int i = 0; i < 2; ++i) {
      mN[i] = mP[i] >= 0 ? d.GetParam(mP[i])->GetNormalized() : 0.;
      mOpen[i] = false;
    }
    mActive = true;
  }
  void Move(IGEditorDelegate& d, double da, double db) { Set(d, mN[0] + da, mN[1] + db); }
  void Set(IGEditorDelegate& d, double na, double nb) {
    if (!mActive) return;
    const double v[2] = {na, nb};
    for (int i = 0; i < 2; ++i) {
      if (mP[i] < 0) continue;
      const double n = std::min(std::max(v[i], 0.0), 1.0);
      if (n == mN[i]) continue;
      if (!mOpen[i]) {
        d.BeginInformHostOfParamChangeFromUI(mP[i]);
        mOpen[i] = true;
      }
      mN[i] = n;
      d.SendParameterValueFromUI(mP[i], n);
    }
  }
  void End(IGEditorDelegate& d) {
    for (int i = 0; i < 2; ++i)
      if (mOpen[i]) d.EndInformHostOfParamChangeFromUI(mP[i]);
    mOpen[0] = mOpen[1] = false;
    mActive = false;
  }
  bool Active() const { return mActive; }

private:
  int mP[2] = {-1, -1};
  double mN[2] = {0., 0.};
  bool mOpen[2] = {false, false};
  bool mActive = false;
};

// A slot's microphone on its cone: a numbered marker. Sideways is the position (cap to edge), down the distance
// (close to far); it moves with the knobs and the knobs with it. A slot that holds a response file dims its cone.
// A slot on a pack microphone names the pack and the microphone at the top of the cone, where the marker never goes.
class ConeMarker final : public IControl {
public:
  static constexpr float kMaxDistance = 300.f;  // mm, the distance parameter's range

  ConeMarker(const pages::Control& cone, FramePtr frame, int slot, int on, int position, int distance,
             std::function<bool()> holdsFile, std::function<std::string()> fileText,
             std::function<std::string()> packText = {})
  : IControl(pages::Cell(cone)), mFrame(std::move(frame)), mSlot(slot), mOn(on), mPosition(position),
    mDistance(distance), mHoldsFile(std::move(holdsFile)), mFileText(std::move(fileText)),
    mPackText(std::move(packText)) {}

  bool IsHit(float x, float y) const override {
    if (!Shown()) return false;
    const IRECT m = Marker();
    return m.GetPadded(8.f).Contains(x, y);
  }

  bool IsDirty() override {
    const std::string key = std::to_string(Value(mOn)) + "/" + std::to_string(Value(mPosition)) + "/" +
                            std::to_string(Value(mDistance)) + "/" + (mHoldsFile() ? mFileText() : PackText());
    if (key != mShown) {
      mShown = key;
      SetDirty(false);
    }
    if (mDrag.Active() && !GetUI()->ControlIsCaptured(this)) mDrag.End(*GetDelegate());
    return IControl::IsDirty();
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    if (!mMouseIsOver) GetUI()->SetMouseCursor(ECursor::SIZEALL);
    IControl::OnMouseOver(x, y, mod);
  }
  void OnMouseOut() override {
    if (mMouseIsOver) GetUI()->SetMouseCursor(ECursor::ARROW);
    IControl::OnMouseOut();
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (mod.A) {
      Reset();
      return;
    }
    if (mod.L) mDrag.Begin(*GetDelegate(), mPosition, mDistance);
  }

  // As on the knobs: a double click, like Alt-click, puts it back where a new instance has it.
  void OnMouseDblClick(float, float, const IMouseMod& mod) override {
    if (mod.L) Reset();
  }

  void OnMouseDrag(float, float, float dX, float dY, const IMouseMod& mod) override {
    const float fine = mod.S || mod.C ? 0.1f : 1.f;
    const float r = mRECT.W() / 2.f;
    mDrag.Move(*GetDelegate(), Side() * dX / (r * 0.96f) * fine, dY / (r * 0.86f) * fine);
    SetDirty(false);
  }

  void OnMouseUp(float, float, const IMouseMod&) override { mDrag.End(*GetDelegate()); }

  void Draw(IGraphics& g) override {
    const float cx = mRECT.MW(), cy = mRECT.MH(), r = mRECT.W() / 2.f;
    if (mHoldsFile()) {
      g.FillCircle(IColor(160, 8, 7, 6), cx, cy, r * 1.12f);
      g.DrawText(style::Panel(11.f, IColor(255, 196, 180, 150)), "RESPONSE FILE", IRECT(cx - r, cy - 20.f, cx + r, cy - 2.f));
      g.DrawText(style::Sans(12.5f, IColor(255, 238, 200, 140), EAlign::Center), mFileText().c_str(),
                 IRECT(cx - r, cy + 2.f, cx + r, cy + 20.f));
      return;
    }
    if (!Shown()) return;
    if (const std::string pack = PackText(); !pack.empty())
      g.DrawText(style::Panel(9.5f, IColor(200, 230, 214, 186)), pack.c_str(),
                 IRECT(cx - r * 0.8f, cy - r * 0.62f, cx + r * 0.8f, cy - r * 0.48f));
    const IRECT m = Marker();
    const float x = m.MW(), y = m.MH(), size = m.W() / 2.f;
    g.DrawLine(IColor(70, 230, 214, 186), cx, y, x, y, nullptr, 1.f);
    g.FillCircle(IColor(150, 0, 0, 0), x + 2.f, y + 3.f, size + 1.f);
    g.FillCircle(IColor(230, 30, 26, 22), x, y, size);
    const bool hot = GetMouseIsOver() || mDrag.Active();
    g.DrawCircle(hot ? IColor(255, 255, 236, 190) : IColor(255, 240, 214, 160), x, y, size - 1.25f, nullptr, 2.5f);
    g.DrawCircle(IColor(90, 236, 222, 196), x, y, size * 0.55f, nullptr, 1.f);
    const char digit[2] = {static_cast<char>('0' + mSlot), 0};
    g.DrawText(style::Panel(11.f, IColor(255, 244, 232, 210)), digit, m);
    char text[48];
    std::snprintf(text, sizeof text, "%.0f %%   %.0f mm", Value(mPosition) * 100., Value(mDistance));
    g.DrawText(style::Panel(9.5f, IColor(200, 230, 214, 186)), text, IRECT(x - 60.f, y + size + 4.f, x + 60.f, y + size + 18.f));
  }

  int Slot() const { return mSlot; }

private:
  void Reset() {
    IGEditorDelegate& d = *GetDelegate();
    mDrag.End(d);
    mDrag.Begin(d, mPosition, mDistance);
    mDrag.Set(d, d.GetParam(mPosition)->GetDefault(true), d.GetParam(mDistance)->GetDefault(true));
    mDrag.End(d);
    SetDirty(false);
  }

  float Side() const { return mSlot == 1 ? -1.f : 1.f; }
  std::string PackText() const { return mPackText ? mPackText() : std::string(); }
  double Value(int p) const { return const_cast<ConeMarker*>(this)->GetDelegate()->GetParam(p)->Value(); }
  bool Shown() const { return !mHoldsFile() && Value(mOn) >= 0.5; }
  IRECT Marker() const {
    const float r = mRECT.W() / 2.f;
    const float d = static_cast<float>(Value(mDistance)) / kMaxDistance;
    const float x = mRECT.MW() + Side() * static_cast<float>(Value(mPosition)) * r * 0.96f;
    const float y = mRECT.MH() + d * r * 0.86f;
    const float size = 22.f * (1.f - 0.35f * d);
    return IRECT(x - size, y - size, x + size, y + size);
  }

  FramePtr mFrame;
  const int mSlot, mOn, mPosition, mDistance;
  std::function<bool()> mHoldsFile;
  std::function<std::string()> mFileText;
  std::function<std::string()> mPackText;  // empty: no label
  std::string mShown;
  PairDrag mDrag;
};

// The EQ's screen: the response the engine runs (the analogue prototypes of dsp/src/eq/Design.cpp, summed in dB),
// with a node per band in its colour. Drag a node: sideways its frequency, up and down its gain; the wheel sets its
// width; a double click or Alt-click resets it. The pass filters' nodes move sideways only.
class EqScreen final : public IControl {
public:
  EqScreen(const IRECT& r) : IControl(r), mGlass(pages::kEqGlassFile, this) {}

  bool IsHit(float x, float y) const override { return mRECT.Contains(x, y) && Nearest(x, y) >= 0; }

  bool IsDirty() override {
    if (mGlass.Update(*GetUI())) SetDirty(false);
    double key = 0.;
    for (int p = kEqOn; p <= kEqBand4Type; ++p) key = key * 1.000003 + Value(p) * (p + 1);
    if (key != mKey) {
      mKey = key;
      SetDirty(false);
    }
    if (mDrag.Active() && !GetUI()->ControlIsCaptured(this)) mDrag.End(*GetDelegate());
    return IControl::IsDirty();
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    mHot = Nearest(x, y);
    GetUI()->SetMouseCursor(mHot >= 0 ? ECursor::SIZEALL : ECursor::ARROW);
    SetDirty(false);
    IControl::OnMouseOver(x, y, mod);
  }
  void OnMouseOut() override {
    mHot = -1;
    GetUI()->SetMouseCursor(ECursor::ARROW);
    SetDirty(false);
    IControl::OnMouseOut();
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    mHot = Nearest(x, y);
    if (mHot < 0 || !mod.L) return;
    IGEditorDelegate& d = *GetDelegate();
    const Node n = NodeOf(mHot);
    mDrag.Begin(d, n.freq, n.gain);
    if (mod.A) Reset(n);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override {
    mHot = Nearest(x, y);
    if (mHot < 0 || !mod.L) return;
    IGEditorDelegate& d = *GetDelegate();
    const Node n = NodeOf(mHot);
    mDrag.End(d);
    mDrag.Begin(d, n.freq, n.gain);
    Reset(n);
  }

  void OnMouseDrag(float, float, float dX, float dY, const IMouseMod& mod) override {
    if (!mDrag.Active() || mHot < 0) return;
    IGEditorDelegate& d = *GetDelegate();
    const Node n = NodeOf(mHot);
    const float fine = mod.S || mod.C ? 0.1f : 1.f;
    // sideways: the parameter's normalised value follows log frequency across the screen
    const IParam& f = *d.GetParam(n.freq);
    const double octaves = std::log2(f.GetMax() / f.GetMin());
    const double perPoint = std::log2(kHi / kLo) / Plot().W() / octaves;
    double dg = 0.;
    if (n.gain >= 0) {
      const IParam& gp = *d.GetParam(n.gain);
      dg = -dY / (Plot().H() / 2.f) * kRange / (gp.GetMax() - gp.GetMin());
    }
    mDrag.Move(d, dX * perPoint * fine, dg * fine);
    SetDirty(false);
  }

  void OnMouseUp(float, float, const IMouseMod&) override { mDrag.End(*GetDelegate()); }

  void OnMouseWheel(float x, float y, const IMouseMod& mod, float delta) override {
    const int i = Nearest(x, y);
    if (i < 1 || i > 4 || delta == 0.f) return;
    const int q = NodeOf(i).q;
    IGEditorDelegate& d = *GetDelegate();
    d.BeginInformHostOfParamChangeFromUI(q);
    const double v = d.GetParam(q)->GetNormalized() + (delta > 0 ? 1 : -1) * (mod.S ? 0.005 : 0.03);
    d.SendParameterValueFromUI(q, std::min(std::max(v, 0.0), 1.0));
    d.EndInformHostOfParamChangeFromUI(q);
  }

  void Draw(IGraphics& g) override {
    const IRECT p = Plot();
    const bool on = Value(kEqOn) >= 0.5;
    // A backlit panel behind the glass: brightest in the middle, falling away to the edges.
    g.FillRect(IColor(255, 10, 10, 9), mRECT);
    const float glow = on ? 1.f : 0.45f;
    const IPattern back = IPattern::CreateRadialGradient(mRECT.MW(), mRECT.MH(), mRECT.W() * 0.55f,
                                                         {{IColor(static_cast<int>(70 * glow), 120, 110, 92), 0.f},
                                                          {IColor(0, 120, 110, 92), 1.f}});
    g.PathClear();
    g.PathRect(mRECT);
    g.PathFill(back);
    const IColor grid(34, 170, 160, 140);
    for (float hz : {50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f}) {
      const float x = X(hz);
      g.DrawLine(grid, x, p.T + 2.f, x, p.B - 2.f, nullptr, 0.5f);
    }
    for (float db : {-12.f, -6.f, 6.f, 12.f}) g.DrawLine(grid, p.L + 2.f, Y(db), p.R - 2.f, Y(db), nullptr, 0.5f);
    g.DrawLine(IColor(70, 190, 178, 156), p.L + 2.f, Y(0.f), p.R - 2.f, Y(0.f), nullptr, 0.75f);
    const IColor labels(120, 190, 176, 150);
    const char* names[] = {"100", "1k", "10k"};
    const float at[] = {100.f, 1000.f, 10000.f};
    for (int i = 0; i < 3; ++i)
      g.DrawText(style::Panel(8.5f, labels, EAlign::Near), names[i], IRECT(X(at[i]) + 3.f, p.B - 13.f, X(at[i]) + 40.f, p.B));
    g.DrawText(style::Panel(8.5f, labels, EAlign::Near), "+12", IRECT(p.L + 3.f, Y(12.f) - 12.f, p.L + 30.f, Y(12.f)));
    g.DrawText(style::Panel(8.5f, labels, EAlign::Near), "-12", IRECT(p.L + 3.f, Y(-12.f) - 12.f, p.L + 30.f, Y(-12.f)));
    // the curve: a soft fill to 0 dB, a wide faint bloom, then the line
    const int steps = static_cast<int>(p.W() / 2.f);
    std::vector<float> ys(steps + 1);
    for (int i = 0; i <= steps; ++i) {
      const float hz = kLo * std::pow(kHi / kLo, static_cast<float>(i) / steps);
      ys[i] = std::min(std::max(Y(on ? Total(hz) : 0.f), p.T + 1.f), p.B - 1.f);
    }
    const IColor ink = on ? IColor(255, 255, 214, 150) : IColor(200, 130, 120, 104);
    g.PathClear();
    g.PathMoveTo(p.L, Y(0.f));
    for (int i = 0; i <= steps; ++i) g.PathLineTo(p.L + p.W() * i / steps, ys[i]);
    g.PathLineTo(p.R, Y(0.f));
    g.PathClose();
    g.PathFill(IColor(on ? 30 : 12, 255, 190, 110));
    for (const auto& pass : {std::pair<float, float>{9.f, 0.06f}, {5.f, 0.12f}, {1.5f, 1.f}}) {
      g.PathClear();
      g.PathMoveTo(p.L, ys[0]);
      for (int i = 1; i <= steps; ++i) g.PathLineTo(p.L + p.W() * i / steps, ys[i]);
      g.PathStroke(ink.WithOpacity(ink.A / 255.f * pass.second), pass.first);
    }
    const auto to8 = [](float v) { return static_cast<int>(255.f * std::pow(std::min(std::max(v, 0.f), 1.f), 1.f / 2.2f)); };
    for (int i = 0; i < 6; ++i) {
      if (!Active(i)) continue;
      const IRECT n = NodeRect(i);
      const float* c = pages::kEqColours[i];
      const IColor col(on ? 255 : 140, to8(c[0]), to8(c[1]), to8(c[2]));
      if (i == 0 || i == 5) {
        g.DrawCircle(col, n.MW(), n.MH(), 4.5f, nullptr, 1.5f);
        continue;
      }
      const float r = mHot == i ? 7.f : 5.5f;
      g.FillCircle(col.WithOpacity(0.22f), n.MW(), n.MH(), r * 2.2f);
      g.FillCircle(col, n.MW(), n.MH(), r);
      g.FillCircle(IColor(200, 255, 250, 240), n.MW(), n.MH(), r * 0.45f);
    }
    // the glass over it all: its reflections, rendered with the scene, added
    const IBlend add(EBlend::Add, 1.f);
    mGlass.Draw(g, mRECT, 0.f, 0.f, &add);
  }

private:
  static constexpr float kLo = 20.f, kHi = 20000.f, kRange = 18.f;
  struct Node {
    int freq, gain, q;
  };
  // 0 high-pass, 1..4 the bands, 5 low-pass
  static Node NodeOf(int i) {
    switch (i) {
      case 0: return {kEqHighPassFreq, -1, -1};
      case 1: return {kEqBand1Freq, kEqBand1Gain, kEqBand1Q};
      case 2: return {kEqBand2Freq, kEqBand2Gain, kEqBand2Q};
      case 3: return {kEqBand3Freq, kEqBand3Gain, kEqBand3Q};
      case 4: return {kEqBand4Freq, kEqBand4Gain, kEqBand4Q};
      default: return {kEqLowPassFreq, -1, -1};
    }
  }
  void Reset(const Node& n) {
    IGEditorDelegate& d = *GetDelegate();
    mDrag.Set(d, d.GetParam(n.freq)->GetDefault(true), n.gain >= 0 ? d.GetParam(n.gain)->GetDefault(true) : 0.);
    mDrag.End(d);
    SetDirty(false);
  }
  double Value(int p) const { return const_cast<EqScreen*>(this)->GetDelegate()->GetParam(p)->Value(); }
  IRECT Plot() const { return mRECT.GetPadded(-6.f); }
  float X(float hz) const { return Plot().L + Plot().W() * std::log(hz / kLo) / std::log(kHi / kLo); }
  float Y(float db) const { return Plot().MH() - db / kRange * (Plot().H() / 2.f - 8.f); }
  bool Active(int i) const {
    if (i == 0) return Value(kEqHighPassSlope) >= 0.5;
    if (i == 5) return Value(kEqLowPassSlope) >= 0.5;
    return true;
  }
  IRECT NodeRect(int i) const {
    const float hz = static_cast<float>(Value(NodeOf(i).freq));
    const float x = X(hz), y = Y(Value(kEqOn) >= 0.5 ? Total(hz) : 0.f);
    return IRECT(x - 7.f, y - 7.f, x + 7.f, y + 7.f);
  }
  int Nearest(float x, float y) const {
    int best = -1;
    float bestD = 12.f;
    for (int i = 0; i < 6; ++i) {
      if (!Active(i)) continue;
      const IRECT n = NodeRect(i);
      const float d = std::hypot(n.MW() - x, n.MH() - y);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    }
    return best;
  }

  // The analogue prototypes (the same as dsp/src/eq/Design.cpp), magnitude in dB.
  static float Db(int kind, double hz, double gainDb, double q, double f) {
    q = std::max(q, 1e-3);
    const double a = std::pow(10.0, gainDb / 40.0), r = std::sqrt(a);
    double n2 = 1, n1 = 0, n0 = 1, d2 = 1, d1 = 0, d0 = 1;
    switch (kind) {
      case 0: n2 = 1; n1 = 0; n0 = 0; d1 = 1 / q; break;            // high-pass
      case 1: n2 = 0; n1 = 0; n0 = 1; d1 = 1 / q; break;            // low-pass
      case 2: n1 = a / q; d1 = 1 / (a * q); break;                  // bell
      case 3: n2 = a; n1 = a * r / q; n0 = a * a; d2 = a; d1 = r / q; break;   // low shelf
      default: n2 = a * a; n1 = a * r / q; n0 = a; d1 = r / q; d0 = a; break;  // high shelf
    }
    const std::complex<double> x(0.0, f / hz);
    const std::complex<double> h = (n2 * x * x + n1 * x + n0) / (d2 * x * x + d1 * x + d0);
    return static_cast<float>(20.0 * std::log10(std::max(std::abs(h), 1e-9)));
  }

  float Total(float f) const {
    static const double kBw2 = 0.70710678118654752, kBw4[2] = {0.54119610014619698, 1.30656296487637652};
    double db = 0.;
    const int hp = static_cast<int>(std::lround(Value(kEqHighPassSlope)));
    const int lp = static_cast<int>(std::lround(Value(kEqLowPassSlope)));
    for (int i = 0; i < hp; ++i) db += Db(0, Value(kEqHighPassFreq), 0, hp == 1 ? kBw2 : kBw4[i], f);
    for (int i = 0; i < lp; ++i) db += Db(1, Value(kEqLowPassFreq), 0, lp == 1 ? kBw2 : kBw4[i], f);
    for (int b = 1; b <= 4; ++b) {
      const Node n = NodeOf(b);
      int kind = 2;
      if (b == 1 && Value(kEqBand1Type) >= 0.5) kind = 3;
      if (b == 4 && Value(kEqBand4Type) >= 0.5) kind = 4;
      db += Db(kind, Value(n.freq), Value(n.gain), Value(n.q), f);
    }
    return static_cast<float>(db);
  }

  double mKey = -1.;
  int mHot = -1;
  PairDrag mDrag;
  Sprite mGlass;
};

// Shows the controls a slot has for its source: a pack microphone's, or a response file's (position and distance
// dimmed). The source is the slot's parameter (mic1_source, mic2_source), so it is the session's.
class SlotView final : public IControl {
public:
  SlotView(std::function<bool()> holdsFile, std::vector<IControl*> mic, std::vector<IControl*> file)
  : IControl(IRECT()), mHoldsFile(std::move(holdsFile)), mMic(std::move(mic)), mFile(std::move(file)) {
    mIgnoreMouse = true;
  }

  bool IsDirty() override {
    // Only while the page is shown: the page's own switch hides everything else.
    if (!IsHidden()) {
      const bool file = mHoldsFile();
      if (file != mShowsFile || mFresh) {
        mShowsFile = file;
        mFresh = false;
        for (IControl* c : mMic) c->Hide(file);
        for (IControl* c : mFile) c->Hide(!file);
        GetUI()->SetAllControlsDirty();
      }
    } else {
      mFresh = true;
    }
    return false;
  }

  void Hide(bool hide) override {
    IControl::Hide(hide);
    if (!hide) {  // the page came back: apply the slot straight away, before anything draws
      const bool file = mHoldsFile();
      for (IControl* c : mMic) c->Hide(file);
      for (IControl* c : mFile) c->Hide(!file);
      mShowsFile = file;
      mFresh = false;
    }
  }

  void Draw(IGraphics&) override {}

private:
  std::function<bool()> mHoldsFile;
  std::vector<IControl*> mMic, mFile;
  bool mShowsFile = false, mFresh = true;
};

// Text behind a display window's glass: small labels, a value in warm white. Clicking a field can open a menu.
class GlassField final : public IControl {
public:
  using Text = std::function<std::string()>;
  using Open = std::function<void(const IRECT&)>;

  using Step = std::function<void(int)>;
  using Can = std::function<bool()>;

  // step: the previous (-1) or next (+1) file of the folder, from the arrows shown while canStep says so.
  GlassField(const IRECT& r, const char* label, Text value, Open open = nullptr, Step step = nullptr, Can canStep = nullptr)
  : IControl(r), mLabel(label), mValue(std::move(value)), mOpen(std::move(open)), mStep(std::move(step)),
    mCanStep(std::move(canStep)) {
    mDblAsSingleClick = true;
    if (!mOpen) mIgnoreMouse = true;
  }

  bool IsDirty() override {
    std::string v = mValue();
    const bool arrows = Arrows();
    if (v != mShown || arrows != mArrows) {
      mShown = std::move(v);
      mArrows = arrows;
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    if (mod.L && mArrows && ArrowRect(-1).Contains(x, y)) mStep(-1);
    else if (mod.L && mArrows && ArrowRect(1).Contains(x, y)) mStep(1);
    else if (mod.L && mOpen) mOpen(mRECT);
  }
  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    if (!mMouseIsOver && mOpen) GetUI()->SetMouseCursor(ECursor::HAND);
    const int part = mArrows && ArrowRect(-1).Contains(x, y) ? -1 : mArrows && ArrowRect(1).Contains(x, y) ? 1 : 0;
    if (part != mHotArrow) {
      mHotArrow = part;
      SetDirty(false);
    }
    IControl::OnMouseOver(x, y, mod);
  }
  // The arrows' targets, left of the menu's caret: previous, next.
  IRECT ArrowRect(int dir) const {
    const float right = mRECT.R - (dir < 0 ? 30.f : 16.f);
    return IRECT(right - 14.f, mRECT.T, right, mRECT.B);
  }
  bool Arrows() const { return mStep && mCanStep && mCanStep(); }
  void OnMouseOut() override {
    if (mMouseIsOver) GetUI()->SetMouseCursor(ECursor::ARROW);
    IControl::OnMouseOut();
  }

  void Draw(IGraphics& g) override {
    const bool hot = GetMouseIsOver() && mOpen;
    const IRECT label(mRECT.L + 6.f, mRECT.T + 2.f, mRECT.R - 4.f, mRECT.T + 12.f);
    const IRECT value(mRECT.L + 6.f, mRECT.T + 11.f, mRECT.R - (mArrows ? 48.f : mOpen ? 14.f : 4.f), mRECT.B - 1.f);
    g.DrawText(style::Panel(9.5f, IColor(255, 150, 132, 104), EAlign::Near), mLabel, label);
    const IColor ink = hot && mHotArrow == 0 ? IColor(255, 255, 222, 170) : IColor(255, 238, 200, 140);
    const IText text = style::Sans(12.5f, ink, EAlign::Near);
    g.DrawText(text, Elided(g, text, mShown, value.W()).c_str(), value);
    if (mArrows) {
      for (int dir : {-1, 1}) {
        const IRECT a = ArrowRect(dir);
        const float x = a.MW(), y = value.MH(), d = 2.f * dir;
        const IColor c = mHotArrow == dir ? IColor(255, 255, 222, 170) : IColor(255, 238, 200, 140).WithOpacity(0.75f);
        g.PathClear();
        g.PathMoveTo(x - d, y - 4.f);
        g.PathLineTo(x + d, y);
        g.PathLineTo(x - d, y + 4.f);
        g.PathStroke(c, 1.3f);
      }
    }
    if (mOpen) {
      const float x = mRECT.R - 8.f, y = value.MH();
      g.PathClear();
      g.PathMoveTo(x - 3.f, y - 1.5f);
      g.PathLineTo(x, y + 1.5f);
      g.PathLineTo(x + 3.f, y - 1.5f);
      g.PathStroke(ink.WithOpacity(0.8f), 1.2f);
    }
  }

  // Shortened with an ellipsis to fit `width`.
  static std::string Elided(IGraphics& g, const IText& text, const std::string& s, float width) {
    IRECT m;
    g.MeasureText(text, s.c_str(), m);
    if (m.W() <= width || s.size() < 2) return s;
    std::string cut = s;
    while (cut.size() > 1) {
      cut.pop_back();
      while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();
      const std::string t = cut + "\xE2\x80\xA6";
      g.MeasureText(text, t.c_str(), m);
      if (m.W() <= width) return t;
    }
    return s.substr(0, 1);
  }

private:
  const char* mLabel;
  Text mValue;
  Open mOpen;
  Step mStep;
  Can mCanStep;
  std::string mShown;
  bool mArrows = false;
  int mHotArrow = 0;
};

}  // namespace ui
}  // namespace magmora
