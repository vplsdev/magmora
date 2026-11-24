// Where the block pages sit: below the bar, over the whole render. Their controls are in Pages.h.
#pragma once

#include "Common.h"

namespace magmora {
namespace ui {

namespace page {
constexpr float kTop = 44.f;  // the pages cover the render, below the bar
inline IRECT Area() { return IRECT(0.f, kTop, layout::kCanvasW, layout::kCanvasH); }
}  // namespace page

}  // namespace ui
}  // namespace magmora
