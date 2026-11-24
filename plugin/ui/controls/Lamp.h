#pragma once

#include <cstring>

#include "Common.h"

namespace magmora {
namespace ui {

// A lamp sprite at an intensity that follows the state: laid over, or added (lit minus unlit), as the layout says.
class Lamp final : public IControl {
public:
  Lamp(const layout::Element& e, FramePtr frame)
  : IControl(layout::Cell(e)), mElement(e), mFrame(std::move(frame)), mSprite(e.file, this),
    mValves(std::strcmp(e.id, "tubes") == 0) {
    mIgnoreMouse = true;
  }

  void OnAttached() override {
    mLevel = Target();  // a window that opens on a running amp does not warm up again
  }

  bool IsDirty() override {
    if (mSprite.Update(*GetUI())) SetDirty(false);
    const float target = Target();
    // Lamps settle in 60 ms. Valves take their time to warm and longer to cool.
    const float tau = !mValves ? 0.02f : (target > mLevel ? 0.45f : 0.8f);
    mLevel += (target - mLevel) * (1.f - std::exp(-mFrame->dt / tau));
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
    const float intensity = std::min(mStep / 128.f, 1.f);
    // iPlug2's Add blend applies the weight twice: it is premultiplied into the paint colour and is then the
    // source factor as well (IGraphicsNanoVG: NVG_SRC_ALPHA, NVG_DST_ALPHA). sqrt makes the result linear.
    // Source-over applies it once.
    const IBlend blend = mElement.additive ? IBlend(EBlend::Add, std::sqrt(intensity)) : IBlend(EBlend::SrcOver, intensity);
    mSprite.Draw(g, mRECT, 0, 0, &blend);
  }

private:
  float Target() {
    const auto value = [this](int param) { return GetDelegate()->GetParam(param)->Value(); };
    if (mElement.requiresParam >= 0 && value(mElement.requiresParam) < 0.5) return 0.f;
    if (std::fabs(value(mElement.param) - mElement.onValue) >= 0.5) return 0.f;
    if (!mValves) return 1.f;
    // Heaters glow in standby as well, a little more when the output stage works.
    return value(kStandby) < 0.5 ? 0.82f : 0.88f + 0.12f * std::min(std::max(mFrame->powerW / 60.f, 0.f), 1.f);
  }

  const layout::Element& mElement;
  FramePtr mFrame;
  Sprite mSprite;
  const bool mValves;
  float mLevel = 0.f;
  int mStep = -1;
};

}  // namespace ui
}  // namespace magmora
