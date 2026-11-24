#pragma once

#include "ParamControl.h"

namespace magmora {
namespace ui {

constexpr float kKnobTravel = 200.f;  // screen points from 0 to 10

class AtlasKnob final : public DragControl {
public:
  AtlasKnob(const layout::Element& e, FramePtr frame)
  : DragControl(layout::Cell(e), std::move(frame), e.param, kKnobTravel, false), mElement(e), mAtlas(e.file, this) {
    SetTargetRECT(layout::Hit(e));  // the cells overlap their neighbours
  }

  // Just above the mouse target, clear of the scale's numeral and on the grille's dark edge.
  IRECT ReadoutRect() const { return layout::Centred(mElement.x, mElement.y - mElement.hitH / 2 - 20.f, 42.f, 20.f); }

  bool IsDirty() override {
    if (mAtlas.Update(*GetUI())) SetDirty(false);
    return DragControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    mAtlas.DrawFrame(g, mElement, static_cast<int>(std::lround(Normalised() * (mElement.frames - 1))));
  }

protected:
  IRECT EntryRect() const override { return ReadoutRect(); }
  IText EntryText() const override { return style::Panel(17.f, style::Bright()); }

private:
  const layout::Element& mElement;
  Sprite mAtlas;
};

// The number, shown while a knob is held or pointed at. Separate, so that it draws above the neighbours.
class KnobReadout final : public IControl {
public:
  KnobReadout(const AtlasKnob* knob, int param) : KnobReadout(knob, knob->ReadoutRect(), param) {}
  KnobReadout(const DragControl* knob, const IRECT& r, int param) : IControl(r), mKnob(knob), mParam(param) {
    mIgnoreMouse = true;
    mHide = true;
  }

  bool IsDirty() override {
    const bool show = mKnob->ShowsValue();
    if (show == mHide) Hide(!show);
    if (show) {
      std::string text = DisplayValue(mParam, mKnob->Plain());
      if (text != mText_) {
        mText_ = std::move(text);
        SetDirty(false);
      }
    }
    return IControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    g.FillRect(style::Shade(0.35f, 240), mRECT);
    g.DrawText(style::Panel(17.f, style::Bright()), mText_.c_str(), mRECT);
  }

private:
  const DragControl* mKnob;
  const int mParam;
  std::string mText_;
};

}  // namespace ui
}  // namespace magmora
