// Shared by the controls. Included only from MagmoraUI.cpp, after the IGraphics headers.
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include <magmora/Params.h>

#include "IPlugPaths.h"
#include "nanovg.h"

#include "../MagmoraUI.h"

namespace magmora {
namespace ui {

using namespace iplug;
using namespace igraphics;

namespace layout {
enum class Kind { Knob, Switch, Glow, Static };
struct Box {
  float x, y, w, h;
};
struct Element {
  Kind kind;
  const char* id;
  int param;  // for a glow: the parameter it follows; -1 for static art
  const char* file;
  float x, y, w, h, hitW, hitH;  // x and y are centres; hit is the mouse target, which can be smaller than the cell
  int frames, columns;
  int frame;  // the one static art draws
  float onValue;
  int requiresParam;  // a glow needs this one on as well; -1: none
  bool additive;
};
#include "../../generated/Layout.inc"

inline IRECT Rect(const Box& b) { return IRECT(b.x, b.y, b.x + b.w, b.y + b.h); }
inline IRECT Centred(float x, float y, float w, float h) { return IRECT(x - w / 2, y - h / 2, x + w / 2, y + h / 2); }
inline IRECT Cell(const Element& e) { return Centred(e.x, e.y, e.w, e.h); }
inline IRECT Hit(const Element& e) { return Centred(e.x, e.y, e.hitW, e.hitH); }
}  // namespace layout

namespace style {
constexpr const char* kSans = "Barlow-Medium";
constexpr const char* kPanel = "BarlowCondensed-SemiBold";

// IColor is alpha first.
inline IColor Wall() {
  return IColor(255, (layout::kBarColour >> 16) & 255, (layout::kBarColour >> 8) & 255, layout::kBarColour & 255);
}
// The wall's hue at another brightness, so that menus and readouts stay in the scene when the art changes tone.
inline IColor Shade(float k, int alpha = 255) {
  const IColor w = Wall();
  const auto c = [k](int v) { return std::min(255, static_cast<int>(v * k + 0.5f)); };
  return IColor(alpha, c(w.R), c(w.G), c(w.B));
}
// Three tones of warm grey. The meter is the only saturated colour.
inline IColor Bright() { return IColor(255, 232, 222, 208); }
inline IColor Middle() { return IColor(255, 171, 160, 147); }
inline IColor Dim() { return IColor(255, 118, 108, 98); }
// Text that must stay readable although it reads as "off" (a switched-off section's name): 4.5:1 against the wall.
inline IColor DimText() { return IColor(255, 164, 154, 142); }
inline IColor MeterLow() { return IColor(255, 168, 158, 146); }
inline IColor MeterHigh() { return IColor(255, 222, 158, 66); }
inline IColor MeterClip() { return IColor(255, 214, 70, 48); }

inline IText Sans(float size, const IColor& c, EAlign align = EAlign::Near) {
  return IText(size, c, kSans, align, EVAlign::Middle);
}
inline IText Panel(float size, const IColor& c, EAlign align = EAlign::Center) {
  return IText(size, c, kPanel, align, EVAlign::Middle);
}
}  // namespace style

inline double Seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline float ToDb(float linear) { return 20.f * std::log10(std::max(linear, 1e-6f)); }

// State shared by the controls of one window, advanced once per frame by the backdrop, which is polled first.
struct Frame {
  explicit Frame(Host& h) : host(h), now(Seconds()) {}

  Host& host;
  double now;
  float dt = 0.f;
  float inputDb = -120.f, inputHoldDb = -120.f;
  float outputDb = -120.f, outputHoldDb = -120.f;
  float gateGain = 1.f, powerW = 0.f;
  static constexpr float kClipDb = -1.f;
  static constexpr double kClipHold = 10.;
  int section = 0;          // the page on show: 0 the amp, then pedal, EQ, cabinet, response file
  std::string pageMessage;  // the last error of the pack's folder or mapping-file chooser
  std::string irMessage[2];  // per cabinet slot: the last error of its response-file chooser
  // A section switched from its name in the bar: its power symbol flashes until then (BarSection, BarPower).
  double flashUntil[4] = {0., 0., 0., 0.};
  // The meters' clip marks: lit until then (10 s after the level last went into the red), or until clicked.
  double inputClipUntil = 0., outputClipUntil = 0.;
  // The tuner (not part of any state): reference pitch and whether it silences the output.
  float tunerA4 = 440.f;
  bool tunerMute = true;
  // The preset menu's state between its openings: naming, confirming, the last message.
  struct PresetMenu {
    enum class Mode { Browse, SaveAs, Replace, Rename, ReplaceRename, Delete };
    Mode mode = Mode::Browse;
    std::string path, name, folder, note;
    bool noteBad = false;
    std::shared_ptr<std::string> draft = std::make_shared<std::string>();
  } presets;
  // The hint line's text and manual section for controls that are not a parameter's (MagmoraUI.cpp DescribeControls).
  std::map<const IControl*, std::pair<std::string, std::string>> hints;

  void Advance() {
    const double t = Seconds();
    dt = static_cast<float>(std::min(std::max(t - now, 0.0), 0.2));
    now = t;
    const Levels l = host.TakeLevels();
    Follow(ToDb(l.inputPeak), inputDb, inputHoldDb, mInputHeldUntil);
    Follow(ToDb(l.outputPeak), outputDb, outputHoldDb, mOutputHeldUntil);
    // Into the meters' red (above -1 dBFS): the clip mark latches.
    if (ToDb(l.inputPeak) > kClipDb) inputClipUntil = now + kClipHold;
    if (ToDb(l.outputPeak) > kClipDb) outputClipUntil = now + kClipHold;
    gateGain = l.gateGain;
    powerW = l.powerW;
  }

private:
  void Follow(float db, float& level, float& hold, double& heldUntil) const {
    level = std::max(db, std::max(level - 26.f * dt, -120.f));
    if (level >= hold) {
      hold = level;
      heldUntil = now + 1.2;
    } else if (now > heldUntil) {
      hold = std::max(level, hold - 30.f * dt);
    }
  }

  double mInputHeldUntil = 0., mOutputHeldUntil = 0.;
};

using FramePtr = std::shared_ptr<Frame>;

// ---- textures ------------------------------------------------------------------------------------------------
// IGraphics' NanoVG back end keeps every bitmap it ever loaded until the window closes. With a page per block that
// would hold all of them at once (about 100 MB at 2x). The art is therefore loaded here, straight into NanoVG, when
// a control that shows it is drawn, and given back when every control that uses it is hidden: one page's worth of
// textures at a time. Textures are shared per file and scale within a window. Images are created and deleted only
// inside Draw, where the drawing context is current (GL on Windows needs that).
class Sprite;

class TextureBank {
public:
  static TextureBank& Of(IGraphics& g) {
    auto& all = Banks();
    auto it = all.find(&g);
    if (it == all.end()) it = all.emplace(&g, std::unique_ptr<TextureBank>(new TextureBank)).first;
    return *it->second;
  }
  static TextureBank* Find(IGraphics* g) {
    auto& all = Banks();
    auto it = all.find(g);
    return it == all.end() ? nullptr : it->second.get();
  }
  // The window is closing: its context takes the images with it.
  static void Forget(IGraphics& g) { Banks().erase(&g); }

  struct Image {
    int id = 0, w = 0, h = 0, refs = 0;
  };

  const Image* Acquire(IGraphics& g, const std::string& file, int scale) {
    Image& im = mImages[Key(file, scale)];
    if (!im.id) {
      im.id = Load(g, file, scale);
      if (im.id) nvgImageSize(Context(g), im.id, &im.w, &im.h);
    }
    if (!im.id) return nullptr;
    ++im.refs;
    return &im;
  }

  void Release(const std::string& file, int scale) {
    auto it = mImages.find(Key(file, scale));
    if (it == mImages.end() || --it->second.refs > 0) return;
    if (it->second.id) mDoomed.push_back(it->second.id);
    mImages.erase(it);
  }

  void Register(Sprite* s) { mSprites.push_back(s); }
  void Unregister(Sprite* s) { mSprites.erase(std::remove(mSprites.begin(), mSprites.end(), s), mSprites.end()); }

  // Once per frame, before anything draws: gives back what hidden controls hold, deletes what nobody holds.
  inline void Collect(IGraphics& g);

  size_t ResidentBytes() const {
    size_t n = 0;
    for (const auto& kv : mImages) n += static_cast<size_t>(kv.second.w) * kv.second.h * 4;
    return n;
  }

  static NVGcontext* Context(IGraphics& g) { return static_cast<NVGcontext*>(g.GetDrawContext()); }

private:
  static std::map<IGraphics*, std::unique_ptr<TextureBank>>& Banks() {
    static std::map<IGraphics*, std::unique_ptr<TextureBank>> banks;
    return banks;
  }
  static std::string Key(const std::string& file, int scale) { return file + "@" + std::to_string(scale); }

  // The same search as IGraphics::SearchImageResource: name@2x.ext for the 2x set, in the bundle or the binary.
  static int Load(IGraphics& g, const std::string& file, int scale) {
    std::string name = file;
    const size_t dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (scale > 1 && dot != std::string::npos) name.insert(dot, "@" + std::to_string(scale) + "x");
    WDL_String found;
    const EResourceLocation where = LocateResource(name.c_str(), ext.c_str(), found, g.GetBundleID(),
                                                   g.GetWinModuleHandle(), g.GetSharedResourcesSubPath());
    NVGcontext* vg = Context(g);
    if (!vg) return 0;
    if (where == EResourceLocation::kAbsolutePath) return nvgCreateImage(vg, found.Get(), 0);
#ifdef OS_WIN
    if (where == EResourceLocation::kWinBinary) {
      int size = 0;
      const void* data = LoadWinResource(found.Get(), ext.c_str(), size, g.GetWinModuleHandle());
      if (data) return nvgCreateImageMem(vg, 0, static_cast<unsigned char*>(const_cast<void*>(data)), size);
    }
#endif
    return 0;
  }

  std::map<std::string, Image> mImages;
  std::vector<int> mDoomed;
  std::vector<Sprite*> mSprites;
  friend class Sprite;
};

// One bitmap at the scale it is drawn at, held only while its control is shown. Scale: IGraphics picks 1x or 2x
// from the screen alone; choosing from screen times UI scale keeps 150 % sharp on a 1x screen and 75 % free of
// shimmer.
class Sprite {
public:
  explicit Sprite(const char* file, IControl* owner = nullptr) : mFile(file), mOwner(owner) {}
  Sprite(const Sprite&) = delete;
  Sprite& operator=(const Sprite&) = delete;
  ~Sprite() {
    if (TextureBank* bank = TextureBank::Find(mUI)) {
      Drop(*bank);
      bank->Unregister(this);
    }
  }

  void SetOwner(IControl* owner) { mOwner = owner; }

  // True when the scale to draw at has changed: the caller redraws.
  bool Update(IGraphics& g) {
    const int scale = g.GetScreenScale() * g.GetDrawScale() > 1.05f ? 2 : 1;
    if (scale == mWant) return false;
    mWant = scale;
    return true;
  }

  void Draw(IGraphics& g, const IRECT& dest, float srcX = 0.f, float srcY = 0.f, const IBlend* blend = nullptr,
            float srcW = 0.f, float srcH = 0.f) {
    const TextureBank::Image* im = Hold(g);
    if (!im) return;
    NVGcontext* vg = TextureBank::Context(g);
    // As IGraphicsNanoVG::DrawBitmap, with an optional source size for stretched parts (the microphone stands).
    const float s = static_cast<float>(mHeld);
    const float kx = srcW > 0.f ? dest.W() / srcW : 1.f, ky = srcH > 0.f ? dest.H() / srcH : 1.f;
    NVGpaint paint;
    nvgTransformIdentity(paint.xform);
    paint.xform[0] = kx / s;
    paint.xform[3] = ky / s;
    paint.xform[4] = dest.L - srcX * kx;
    paint.xform[5] = dest.T - srcY * ky;
    paint.extent[0] = static_cast<float>(im->w);
    paint.extent[1] = static_cast<float>(im->h);
    paint.image = im->id;
    paint.radius = paint.feather = 0.f;
    const float weight = blend ? blend->mWeight : 1.f;
    paint.innerColor = paint.outerColor = nvgRGBAf(1, 1, 1, weight);
    nvgBeginPath(vg);
    nvgRect(vg, dest.L, dest.T, dest.W(), dest.H());
    nvgFillPaint(vg, paint);
    if (blend && blend->mMethod == EBlend::Add) nvgGlobalCompositeBlendFunc(vg, NVG_SRC_ALPHA, NVG_DST_ALPHA);
    nvgFill(vg);
    nvgGlobalCompositeOperation(vg, NVG_SOURCE_OVER);
    nvgBeginPath(vg);
  }

  void DrawFrame(IGraphics& g, const layout::Element& e, int frame) {
    const int f = std::min(std::max(frame, 0), e.frames - 1);
    Draw(g, layout::Cell(e), static_cast<float>((f % e.columns) * static_cast<int>(e.w)),
         static_cast<float>((f / e.columns) * static_cast<int>(e.h)));
  }

  bool Held() const { return mHeld != 0; }
  IControl* Owner() const { return mOwner; }

  void Drop(TextureBank& bank) {
    if (mHeld) bank.Release(mFile, mHeld);
    mHeld = 0;
  }

private:
  const TextureBank::Image* Hold(IGraphics& g) {
    TextureBank& bank = TextureBank::Of(g);
    if (!mUI) {
      mUI = &g;
      bank.Register(this);
    }
    if (!mWant) Update(g);
    if (mHeld && mHeld != mWant) Drop(bank);
    if (!mHeld) {
      const TextureBank::Image* im = bank.Acquire(g, mFile, mWant);
      if (!im) return nullptr;
      mHeld = mWant;
      return im;
    }
    auto it = bank.mImages.find(TextureBank::Key(mFile, mHeld));
    return it == bank.mImages.end() ? nullptr : &it->second;
  }

  std::string mFile;
  IControl* mOwner;
  IGraphics* mUI = nullptr;
  int mWant = 0, mHeld = 0;
};

inline void TextureBank::Collect(IGraphics& g) {
  for (Sprite* s : mSprites)
    if (s->Held() && s->Owner() && s->Owner()->IsHidden()) s->Drop(*this);
  if (NVGcontext* vg = Context(g)) {
    for (int id : mDoomed) nvgDeleteImage(vg, id);
  }
  mDoomed.clear();
}

// The utility bar's ground, and the once-per-frame tick. Control 0: polled and drawn before everything else.
class Backdrop final : public IControl {
public:
  Backdrop(const IRECT& bounds, FramePtr frame) : IControl(bounds), mFrame(std::move(frame)) { mIgnoreMouse = true; }

  bool IsDirty() override {
    mFrame->Advance();
    IGraphics& g = *GetUI();
    if (g.GetDrawScale() != mScale) {
      mScale = g.GetDrawScale();
      mFrame->host.EditorScaleChanged(mScale);
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    // Textures of hidden pages go back here, where the drawing context is current.
    TextureBank::Of(g).Collect(g);
    // The wall colour goes under everything, so a fractional scale cannot open a dark seam under the bar.
    g.FillRect(style::Wall(), mRECT);
  }

private:
  FramePtr mFrame;
  float mScale = 0.f;
};

// A full-width image under a page's controls: the amp's plate, or a block's scene.
class Plate final : public IControl {
public:
  Plate(const IRECT& bounds, const char* file, bool swallow) : IControl(bounds), mArt(file, this) {
    mIgnoreMouse = !swallow;
  }
  void OnMouseDown(float, float, const IMouseMod&) override {}  // a page keeps the mouse off what is under it
  bool IsDirty() override {
    if (mArt.Update(*GetUI())) SetDirty(false);
    return IControl::IsDirty();
  }
  void Draw(IGraphics& g) override { mArt.Draw(g, mRECT); }

private:
  Sprite mArt;
};

// Art that never changes, drawn over the plate: each knob position's own light.
class StaticArt final : public IControl {
public:
  explicit StaticArt(const layout::Element& e) : IControl(layout::Cell(e)), mElement(e), mSprite(e.file, this) {
    mIgnoreMouse = true;
  }

  bool IsDirty() override {
    if (mSprite.Update(*GetUI())) SetDirty(false);
    return IControl::IsDirty();
  }

  void Draw(IGraphics& g) override { mSprite.DrawFrame(g, mElement, mElement.frame); }

private:
  const layout::Element& mElement;
  Sprite mSprite;
};

}  // namespace ui
}  // namespace magmora
