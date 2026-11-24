#pragma once

#include "ParamControl.h"

namespace magmora {
namespace ui {

// Push-button, bat toggle and rocker: the same two-frame behaviour with different art.
// It switches on the press, as the hardware does.
class Switch final : public ParamControl {
public:
  Switch(const layout::Element& e, FramePtr frame)
  : ParamControl(layout::Cell(e), std::move(frame), e.param), mElement(e), mFrames(e.file, this) {
    SetTargetRECT(layout::Hit(e));
    mDblAsSingleClick = true;  // two quick presses are two presses
  }

  bool IsDirty() override {
    if (mFrames.Update(*GetUI())) SetDirty(false);
    return ParamControl::IsDirty();
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (mod.L) SetOnce(Param().GetNormalized() < 0.5 ? 1. : 0.);
  }

  void Draw(IGraphics& g) override { mFrames.DrawFrame(g, mElement, Normalised() < 0.5 ? 0 : 1); }

private:
  const layout::Element& mElement;
  Sprite mFrames;
};

}  // namespace ui
}  // namespace magmora
