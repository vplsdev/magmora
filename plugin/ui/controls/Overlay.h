// Menus, the about page and the tuner. They are drawn in the window, in the bar's tones: no system menu over the amp.
#pragma once

#include <functional>
#include <vector>

#include <magmora/Tuner.h>

#include "Common.h"

namespace magmora {
namespace ui {

struct MenuRow {
  // Choice: a row to click (rename and remove: glyphs shown on the row while it is pointed at). Heading and Note: text.
  // Actions: text, then words to click ("Replace", "Cancel"), or the words alone. Entry: a text field over the row.
  // Break: the rows after it make the next column.
  enum Type { Choice, Heading, Note, Actions, Entry, Break };
  struct Button {
    std::string text;
    std::function<void()> action;
    bool danger = false;
  };
  Type type;
  std::string text;
  bool checked = false;
  std::function<void()> action;
  bool disabled = false;
  bool danger = false;  // drawn in the meter's red
  std::function<void()> rename, remove;
  std::vector<Button> buttons;
  std::shared_ptr<std::string> draft;  // Entry: what was typed lands here
  std::string tip;                     // Choice: shown in the hint line while pointed at

  static MenuRow Head(std::string t) { return {Heading, std::move(t), false, nullptr}; }
  static MenuRow Info(std::string t, bool danger = false) {
    MenuRow r{Note, std::move(t), false, nullptr};
    r.danger = danger;
    return r;
  }
  static MenuRow Pick(std::string t, bool on, std::function<void()> f) { return {Choice, std::move(t), on, std::move(f)}; }
  static MenuRow Dim(std::string t) {
    MenuRow r{Choice, std::move(t), false, nullptr};
    r.disabled = true;
    return r;
  }
  static MenuRow Ask(std::string t, std::vector<Button> b) {
    MenuRow r{Actions, std::move(t), false, nullptr};
    r.buttons = std::move(b);
    return r;
  }
  // `commit` runs when the field is confirmed with Return (or loses the keyboard without a click in the menu).
  static MenuRow Field(std::string initial, std::shared_ptr<std::string> draft, std::function<void()> commit) {
    MenuRow r{Entry, std::move(initial), false, std::move(commit)};
    r.draft = std::move(draft);
    return r;
  }
  static MenuRow Column() { return {Break, "", false, nullptr}; }
};

class Overlay final : public IControl {
public:
  static constexpr float kTunerMinA4 = 430.f, kTunerMaxA4 = 450.f;

  Overlay(const IRECT& bounds, FramePtr frame) : IControl(bounds), mFrame(std::move(frame)) {
    mHide = true;
    mDblAsSingleClick = true;
  }

  // Below `anchor`, aligned to its left edge, or to its right edge where that would leave the window. `width` is
  // per column.
  void OpenMenu(const IRECT& anchor, float width, std::vector<MenuRow> rows) {
    LeaveTuner();
    mMode = Mode::Menu;
    mRows = std::move(rows);
    mHover = -1;
    mHoverPart = 0;
    mColumnW = width;
    mAnchor = anchor;
    mCommitPending = false;
    // Columns: each row's column and position in it.
    mColumn.assign(mRows.size(), 0);
    mSlot.assign(mRows.size(), -1);
    std::vector<int> counts(1, 0);
    for (size_t i = 0; i < mRows.size(); ++i) {
      if (mRows[i].type == MenuRow::Break) {
        counts.push_back(0);
        continue;
      }
      mColumn[i] = static_cast<int>(counts.size()) - 1;
      mSlot[i] = counts.back()++;
    }
    mCounts = counts;
    mOffset.assign(counts.size(), 0);
    const int most = *std::max_element(counts.begin(), counts.end());
    // Below the bar for the bar's menus; below its field, or as near as fits, for a page's. Rows shrink to fit, down
    // to 20 points; a longer column scrolls.
    const float barBottom = layout::kBar.y + layout::kBar.h;
    const float space = mRECT.B - barBottom - 2 * kPad - 8.f;
    mRowH = std::min(25.f, space / std::max(most, 1));
    if (mRowH < 20.f) mRowH = 22.f;
    mVisible = std::max(1, std::min(most, static_cast<int>(std::floor(space / mRowH))));
    const float height = 2 * kPad + mRowH * mVisible;
    const float top = std::max(barBottom, std::min(anchor.B > barBottom + 1.f ? anchor.B + 2.f : barBottom, mRECT.B - 8.f - height));
    const float total = width * static_cast<float>(counts.size());
    float left = anchor.L;
    if (left + total > mRECT.R - 12.f) left = std::max(12.f, std::min(anchor.R, mRECT.R - 12.f) - total);
    mPanel = IRECT(left, top, left + total, top + height);
    Hide(false);
    SetDirty(false);
    // A row with a text field: the field opens over it, its text selected.
    for (size_t i = 0; i < mRows.size(); ++i)
      if (mRows[i].type == MenuRow::Entry) {
        ScrollTo(static_cast<int>(i));
        IText text = style::Sans(15.f, style::Bright());
        text.mTextEntryBGColor = style::Shade(0.45f);
        text.mTextEntryFGColor = style::Bright();
        const IRECT r = RowRect(static_cast<int>(i));
        GetUI()->CreateTextEntry(*this, text, IRECT(r.L + 24.f, r.T + 1.f, r.R - 10.f, r.B - 1.f), mRows[i].text.c_str(), kNoValIdx);
        break;
      }
  }

  void OpenAbout(std::vector<MenuRow> links) {
    LeaveTuner();
    mMode = Mode::About;
    mRows = std::move(links);
    mHover = -1;
    Hide(false);
    SetDirty(false);
  }

  void OpenTuner() {
    mMode = Mode::Tuner;
    mRows.clear();
    mTunerSince = mFrame->now;
    mNote = -1;
    mHistory.clear();
    mFrame->host.SetTuner(true, mFrame->tunerMute);
    Hide(false);
    SetDirty(false);
  }

  Host& HostOf() const { return mFrame->host; }
  bool TunerOpen() const { return !IsHidden() && mMode == Mode::Tuner; }
  bool MenuOpen() const { return !IsHidden() && mMode == Mode::Menu; }
  const IRECT& Anchor() const { return mAnchor; }

  void Close() {
    if (IsHidden()) return;
    LeaveTuner();
    Hide(true);
    mRows.clear();
    mMode = Mode::Menu;
    mCommitPending = false;
    GetUI()->SetAllControlsDirty();
  }

  // The tip of what the pointer is on in an open menu (the hint line shows it).
  std::string HoverTip() const {
    if (IsHidden() || mMode != Mode::Menu || mHover < 0) return {};
    const MenuRow& r = mRows[static_cast<size_t>(mHover)];
    if (mHoverPart == 1) return "Rename this preset";
    if (mHoverPart == 2) return "Delete this preset";
    return r.tip;
  }

  void OnTextEntryCompletion(const char* str, int) override {
    if (mMode == Mode::Tuner) {
      double v = 0.;
      if (str && ParseA4(str, v)) mFrame->tunerA4 = static_cast<float>(v);
      SetDirty(false);
      return;
    }
    for (MenuRow& r : mRows)
      if (r.type == MenuRow::Entry && r.draft) *r.draft = str ? str : "";
    // Return: confirmed at the next frame, unless a click in the menu (which also ends the field) decides first.
    mCommitPending = true;
  }

  bool IsDirty() override {
    if (mCommitPending && !IsHidden() && mMode == Mode::Menu) {
      mCommitPending = false;
      for (const MenuRow& r : mRows)
        if (r.type == MenuRow::Entry && r.action) {
          const auto commit = r.action;
          commit();
          break;
        }
    }
    if (TunerOpen()) Listen();
    return IControl::IsDirty();
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    mCommitPending = false;
    if (mMode == Mode::Tuner) {
      TunerMouseDown(x, y, mod);
      return;
    }
    if (mMode == Mode::About) {
      for (size_t i = 0; i < mRows.size(); ++i)
        if (AboutRow(static_cast<int>(i)).Contains(x, y) && mRows[i].action) {
          const auto action = mRows[i].action;
          Close();
          action();
          return;
        }
      Close();
      return;
    }
    int part = 0;
    const int i = RowAt(x, y, &part);
    if (i < 0 && mPanel.Contains(x, y)) return;  // a heading, a note, or the menu's margin
    std::function<void()> action;
    if (i >= 0) {
      const MenuRow& r = mRows[static_cast<size_t>(i)];
      if (part == 1) action = r.rename;
      else if (part == 2) action = r.remove;
      else if (part >= 10) action = r.buttons[static_cast<size_t>(part - 10)].action;
      else action = r.action;
    }
    Close();
    if (action) action();
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override {
    int part = 0;
    const int i = mMode == Mode::Menu ? RowAt(x, y, &part) : -1;
    if (i != mHover || part != mHoverPart) {
      mHover = i;
      mHoverPart = part;
      GetUI()->SetMouseCursor(i >= 0 ? ECursor::HAND : ECursor::ARROW);
      SetDirty(false);
    }
    IControl::OnMouseOver(x, y, mod);
  }

  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override {
    if (mMode == Mode::Tuner) {
      if (A4Rect().Contains(x, y) && d != 0.f) SetA4(mFrame->tunerA4 + (d > 0.f ? 1.f : -1.f) * (mod.S ? 0.1f : 1.f));
      return;
    }
    if (mMode != Mode::Menu || !mPanel.Contains(x, y) || d == 0.f) return;
    const int c = std::min(static_cast<int>((x - mPanel.L) / mColumnW), static_cast<int>(mCounts.size()) - 1);
    const int most = std::max(0, mCounts[static_cast<size_t>(c)] - mVisible);
    mOffset[static_cast<size_t>(c)] = std::min(std::max(mOffset[static_cast<size_t>(c)] + (d > 0.f ? -3 : 3), 0), most);
    OnMouseOver(x, y, mod);
    SetDirty(false);
  }

  void OnMouseDrag(float, float, float, float dY, const IMouseMod& mod) override {
    if (mMode == Mode::Tuner && mDraggingA4) SetA4(mDragA4 -= dY * (mod.S ? 0.01f : 0.1f));
  }
  void OnMouseUp(float, float, const IMouseMod&) override { mDraggingA4 = false; }

  void Draw(IGraphics& g) override {
    if (mMode == Mode::About) DrawAbout(g);
    else if (mMode == Mode::Tuner) DrawTuner(g);
    else DrawMenu(g);
  }

private:
  enum class Mode { Menu, About, Tuner };
  static constexpr float kPad = 8.f;
  static constexpr float kGlyph = 20.f;  // the rename and remove glyphs' targets

  void LeaveTuner() {
    if (mMode == Mode::Tuner) mFrame->host.SetTuner(false, false);
  }

  void ScrollTo(int i) {
    const size_t c = static_cast<size_t>(mColumn[static_cast<size_t>(i)]);
    const int slot = mSlot[static_cast<size_t>(i)];
    if (slot < mOffset[c]) mOffset[c] = slot;
    if (slot >= mOffset[c] + mVisible) mOffset[c] = slot - mVisible + 1;
  }

  IRECT RowRect(int i) const {
    const size_t k = static_cast<size_t>(i);
    const float left = mPanel.L + mColumnW * static_cast<float>(mColumn[k]);
    const float top = mPanel.T + kPad + mRowH * static_cast<float>(mSlot[k] - mOffset[static_cast<size_t>(mColumn[k])]);
    return IRECT(left, top, left + mColumnW, top + mRowH);
  }

  bool RowShown(int i) const {
    const size_t k = static_cast<size_t>(i);
    if (mSlot[k] < 0) return false;
    const int at = mSlot[k] - mOffset[static_cast<size_t>(mColumn[k])];
    return at >= 0 && at < mVisible;
  }

  // The words of an Actions row, right of its text (or from the left without one).
  std::vector<IRECT> ButtonRects(const MenuRow& row, const IRECT& r) const {
    std::vector<IRECT> out;
    IGraphics* g = const_cast<Overlay*>(this)->GetUI();
    const IText text = style::Sans(15.f, style::Bright());
    float x = r.L + 30.f;
    if (!row.text.empty()) {
      x = r.R - 10.f;
      for (auto it = row.buttons.rbegin(); it != row.buttons.rend(); ++it) {
        IRECT m(0.f, r.T, 0.f, r.B);
        g->MeasureText(text, it->text.c_str(), m);
        out.insert(out.begin(), IRECT(x - m.W() - 8.f, r.T, x, r.B));
        x -= m.W() + 14.f;
      }
      return out;
    }
    for (const MenuRow::Button& b : row.buttons) {
      IRECT m(0.f, r.T, 0.f, r.B);
      g->MeasureText(text, b.text.c_str(), m);
      out.emplace_back(x - 6.f, r.T, x + m.W() + 6.f, r.B);
      x += m.W() + 24.f;
    }
    return out;
  }

  int RowAt(float x, float y, int* part) const {
    *part = 0;
    if (mMode != Mode::Menu || !mPanel.Contains(x, y)) return -1;
    for (size_t k = 0; k < mRows.size(); ++k) {
      const int i = static_cast<int>(k);
      if (!RowShown(i) || !RowRect(i).Contains(x, y)) continue;
      const MenuRow& row = mRows[k];
      const IRECT r = RowRect(i);
      if (row.type == MenuRow::Actions) {
        const std::vector<IRECT> b = ButtonRects(row, r);
        for (size_t n = 0; n < b.size(); ++n)
          if (b[n].Contains(x, y) && row.buttons[n].action) {
            *part = 10 + static_cast<int>(n);
            return i;
          }
        return -1;
      }
      if (row.type != MenuRow::Choice || row.disabled) return -1;
      if (row.remove && x >= r.R - kGlyph - 4.f) *part = 2;
      else if (row.rename && x >= r.R - 2 * kGlyph - 4.f) *part = 1;
      return i;
    }
    return -1;
  }

  static void Pencil(IGraphics& g, const IColor& c, float x, float y) {
    g.PathClear();
    g.PathMoveTo(x - 4.5f, y + 4.5f);
    g.PathLineTo(x - 4.f, y + 2.f);
    g.PathLineTo(x + 2.5f, y - 4.5f);
    g.PathLineTo(x + 4.5f, y - 2.5f);
    g.PathLineTo(x - 2.f, y + 4.f);
    g.PathClose();
    g.PathStroke(c, 1.1f);
  }

  static void Bin(IGraphics& g, const IColor& c, float x, float y) {
    g.DrawLine(c, x - 4.5f, y - 3.5f, x + 4.5f, y - 3.5f, nullptr, 1.1f);
    g.DrawLine(c, x - 1.5f, y - 5.f, x + 1.5f, y - 5.f, nullptr, 1.1f);
    g.PathClear();
    g.PathMoveTo(x - 3.5f, y - 2.f);
    g.PathLineTo(x - 2.8f, y + 5.f);
    g.PathLineTo(x + 2.8f, y + 5.f);
    g.PathLineTo(x + 3.5f, y - 2.f);
    g.PathStroke(c, 1.1f);
  }

  void DrawMenu(IGraphics& g) const {
    g.DrawFastDropShadow(mPanel, mPanel.GetPadded(24.f), 4.f, 0.f, 22.f);
    g.FillRect(style::Shade(0.62f), mPanel);
    for (size_t c = 1; c < mCounts.size(); ++c) {
      const float x = mPanel.L + mColumnW * static_cast<float>(c);
      g.FillRect(style::Shade(0.78f), IRECT(x, mPanel.T + kPad, x + 1.f, mPanel.B - kPad));
    }
    for (size_t i = 0; i < mRows.size(); ++i) {
      if (!RowShown(static_cast<int>(i))) continue;
      const MenuRow& row = mRows[i];
      const IRECT r = RowRect(static_cast<int>(i));
      const bool hot = static_cast<int>(i) == mHover;
      switch (row.type) {
        case MenuRow::Choice: {
          if (hot) g.FillRect(style::Shade(0.95f), r);
          if (row.checked) g.FillCircle(style::Bright(), r.L + 17.f, r.MH(), 2.4f);
          const IColor c = row.disabled ? style::Dim() : row.checked || (hot && mHoverPart == 0) ? style::Bright() : style::Middle();
          const float right = hot && (row.rename || row.remove) ? r.R - 2 * kGlyph - 6.f : r.R - 8.f;
          g.DrawText(style::Sans(15.f, c), row.text.c_str(), IRECT(r.L + 30.f, r.T, right, r.B));
          if (hot && row.remove)
            Bin(g, mHoverPart == 2 ? style::MeterClip() : style::Middle(), r.R - kGlyph / 2.f - 4.f, r.MH());
          if (hot && row.rename)
            Pencil(g, mHoverPart == 1 ? style::Bright() : style::Middle(), r.R - 1.5f * kGlyph - 4.f, r.MH());
          break;
        }
        case MenuRow::Actions: {
          if (!row.text.empty())
            g.DrawText(style::Sans(15.f, row.danger ? style::MeterClip() : style::Bright()), row.text.c_str(),
                       IRECT(r.L + 14.f, r.T, r.R - 8.f, r.B));
          const std::vector<IRECT> b = ButtonRects(row, r);
          for (size_t n = 0; n < b.size(); ++n) {
            const bool on = hot && mHoverPart == 10 + static_cast<int>(n);
            if (on) g.FillRect(style::Shade(0.95f), b[n]);
            const IColor c = row.buttons[n].danger ? style::MeterClip() : on ? style::Bright() : style::Middle();
            g.DrawText(style::Sans(15.f, c, EAlign::Center), row.buttons[n].text.c_str(), b[n]);
          }
          break;
        }
        case MenuRow::Entry:
          g.FillRect(style::Shade(0.45f), IRECT(r.L + 22.f, r.T + 1.f, r.R - 8.f, r.B - 1.f));
          g.DrawText(style::Sans(15.f, style::Bright()), (row.draft && !row.draft->empty() ? *row.draft : row.text).c_str(),
                     IRECT(r.L + 26.f, r.T, r.R - 10.f, r.B));
          break;
        case MenuRow::Heading:
        case MenuRow::Note:
          g.DrawText(style::Sans(13.5f, row.danger ? style::MeterClip() : style::Dim()), row.text.c_str(),
                     IRECT(r.L + 14.f, r.T, r.R - 8.f, r.B));
          break;
        case MenuRow::Break: break;
      }
    }
    // A column that scrolls: a thin bar at its right edge.
    for (size_t c = 0; c < mCounts.size(); ++c) {
      if (mCounts[c] <= mVisible) continue;
      const float x = mPanel.L + mColumnW * static_cast<float>(c + 1) - 4.f;
      const float h = mPanel.H() - 2 * kPad;
      const float t = mPanel.T + kPad + h * static_cast<float>(mOffset[c]) / mCounts[c];
      g.FillRect(style::Shade(0.95f), IRECT(x, mPanel.T + kPad, x + 2.f, mPanel.B - kPad));
      g.FillRect(style::Middle(), IRECT(x, t, x + 2.f, t + h * static_cast<float>(mVisible) / mCounts[c]));
    }
  }

  IRECT AboutRow(int i) const;
  void DrawAbout(IGraphics& g) const;

  // ---- tuner ----
  IRECT Strip() const { return IRECT(mRECT.L, mRECT.MH() - 120.f, mRECT.R, mRECT.MH() + 130.f); }
  IRECT MuteRect() const { return IRECT(mRECT.MW() - 170.f, Strip().B - 46.f, mRECT.MW() - 70.f, Strip().B - 20.f); }
  IRECT A4Rect() const { return IRECT(mRECT.MW() + 50.f, Strip().B - 46.f, mRECT.MW() + 170.f, Strip().B - 20.f); }

  static bool ParseA4(const char* s, double& v) {
    std::string t;
    for (const char* c = s; *c; ++c)
      if (!std::isspace(static_cast<unsigned char>(*c))) t += *c == ',' ? '.' : *c;
    if (t.rfind("A=", 0) == 0 || t.rfind("a=", 0) == 0) t.erase(0, 2);
    char* end = nullptr;
    const double x = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(x)) return false;
    const std::string rest(end);
    if (!(rest.empty() || rest == "Hz" || rest == "hz")) return false;
    v = std::min(std::max(x, static_cast<double>(kTunerMinA4)), static_cast<double>(kTunerMaxA4));
    return true;
  }

  void SetA4(float v) {
    mFrame->tunerA4 = std::min(std::max(std::round(v * 10.f) / 10.f, kTunerMinA4), kTunerMaxA4);
    SetDirty(false);
  }

  void TunerMouseDown(float x, float y, const IMouseMod& mod) {
    if (MuteRect().Contains(x, y)) {
      mFrame->tunerMute = !mFrame->tunerMute;
      mFrame->host.SetTuner(true, mFrame->tunerMute);
      SetDirty(false);
      return;
    }
    if (A4Rect().Contains(x, y)) {
      if (mod.R) {
        char text[16];
        std::snprintf(text, sizeof text, "%.1f", mFrame->tunerA4);
        for (char* c = text; *c; ++c)
          if (*c == ',') *c = '.';
        GetUI()->CreateTextEntry(*this, style::Sans(15.f, style::Bright(), EAlign::Far), A4Rect(), text, kNoValIdx);
      } else if (mod.A) {
        SetA4(440.f);
      } else {
        mDraggingA4 = true;
        mDragA4 = mFrame->tunerA4;
      }
      return;
    }
    if (!Strip().Contains(x, y)) Close();
  }

  // On the editor's timer: the newest window of the input, its pitch, and a reading that holds still.
  void Listen() {
    const double now = mFrame->now;
    if (now - mLastListen < 0.03) return;
    mLastListen = now;
    double rate = 0.;
    float probe = 0.f;
    mFrame->host.TunerSamples(&probe, 1, rate);
    if (!(rate > 0.)) return;
    const int n = std::min(mDetector.windowSamples(rate), TunerFeed::kCapacity);
    mWindow.resize(static_cast<size_t>(n));
    if (!mFrame->host.TunerSamples(mWindow.data(), n, rate)) return;
    const Pitch p = mDetector.detect(mWindow.data(), n, rate);
    mLevel = static_cast<float>(p.rms);
    if (p.found && p.clarity >= 0.8) {
      mHistory.push_back(p.hz);
      if (mHistory.size() > 5) mHistory.erase(mHistory.begin());
      std::vector<double> sorted = mHistory;
      std::sort(sorted.begin(), sorted.end());
      const double hz = sorted[sorted.size() / 2];
      const NoteReading r = nearestNote(hz, mFrame->tunerA4);
      // A new note shows after two readings agree; within a note the needle follows smoothly.
      if (r.midi != mNote) {
        if (r.midi == mCandidate && ++mAgree >= 2) {
          mNote = r.midi;
          mCents = static_cast<float>(r.cents);
        } else if (r.midi != mCandidate) {
          mCandidate = r.midi;
          mAgree = 1;
        }
      } else {
        const double cents = 1200.0 * std::log2(hz / (mFrame->tunerA4 * std::pow(2.0, (mNote - 69) / 12.0)));
        mCents += (static_cast<float>(cents) - mCents) * 0.45f;
      }
      if (mNote >= 0) {
        mHz = static_cast<float>(hz);
        mHeard = now;
      }
    } else if (now - mHeard > 1.5) {
      mHistory.clear();
      mNote = -1;
    }
    SetDirty(false);
  }

  void DrawTuner(IGraphics& g) const {
    const float barBottom = layout::kBar.y + layout::kBar.h;
    g.FillRect(style::Shade(0.3f, 215), IRECT(mRECT.L, barBottom, mRECT.R, mRECT.B));
    const IRECT s = Strip();
    g.FillRect(style::Shade(0.62f), s);
    g.FillRect(style::Shade(0.8f), IRECT(s.L, s.T, s.R, s.T + 1.f));
    g.FillRect(style::Shade(0.8f), IRECT(s.L, s.B - 1.f, s.R, s.B));
    const float cx = s.MW();
    g.DrawText(style::Sans(13.5f, style::Dim(), EAlign::Center), "TUNER", IRECT(cx - 100.f, s.T + 8.f, cx + 100.f, s.T + 26.f));
    const bool live = mNote >= 0;
    const bool inTune = live && std::fabs(mCents) <= 3.f;
    // the scale: -50 to +50 cents
    const float w = 440.f, y = s.T + 132.f;
    for (int c = -50; c <= 50; c += 10) {
      const float x = cx + w / 2.f * c / 50.f;
      const float h = c == 0 ? 14.f : c % 50 == 0 ? 10.f : 6.f;
      g.FillRect(c == 0 ? style::Bright() : style::Middle(), IRECT(x - 0.6f, y - h, x + 0.6f, y));
    }
    g.FillRect(style::Shade(1.4f), IRECT(cx - w / 2.f, y, cx + w / 2.f, y + 1.f));
    g.DrawText(style::Sans(12.f, style::Dim(), EAlign::Center), "-50", IRECT(cx - w / 2.f - 20.f, y + 4.f, cx - w / 2.f + 20.f, y + 20.f));
    g.DrawText(style::Sans(12.f, style::Dim(), EAlign::Center), "+50", IRECT(cx + w / 2.f - 20.f, y + 4.f, cx + w / 2.f + 20.f, y + 20.f));
    if (live) {
      const float x = cx + w / 2.f * std::min(std::max(mCents, -50.f), 50.f) / 50.f;
      const IColor c = inTune ? style::Bright() : style::MeterHigh();
      g.FillCircle(c.WithOpacity(0.18f), x, y - 12.f, 12.f);
      g.FillRect(c, IRECT(x - 1.2f, y - 30.f, x + 1.2f, y + 6.f));
    }
    // the note, large, its octave small; Hz and cents under the scale
    const IColor ink = !live ? style::Dim() : inTune ? style::Bright() : style::MeterHigh();
    const std::string name = live ? noteName(mNote) : "-";
    g.DrawText(IText(64.f, ink, style::kPanel, EAlign::Center, EVAlign::Middle), name.c_str(), IRECT(cx - 80.f, s.T + 26.f, cx + 80.f, s.T + 92.f));
    if (live)
      g.DrawText(style::Sans(16.f, ink), std::to_string(noteOctave(mNote)).c_str(), IRECT(cx + 26.f, s.T + 60.f, cx + 60.f, s.T + 84.f));
    char line[64];
    if (live) std::snprintf(line, sizeof line, "%.1f Hz   %+.0f cents", mHz, mCents);
    else std::snprintf(line, sizeof line, "%s", mLevel < static_cast<float>(PitchDetector::kSilence) ? "Play a single string" : "Listening");
    g.DrawText(style::Sans(14.f, style::Middle(), EAlign::Center), line, IRECT(cx - 200.f, y + 18.f, cx + 200.f, y + 38.f));
    // Mute and the reference pitch
    const IRECT m = MuteRect();
    const bool muted = mFrame->tunerMute;
    g.DrawCircle(style::Middle(), m.L + 8.f, m.MH(), 5.f, nullptr, 1.2f);
    if (muted) g.FillCircle(style::MeterHigh(), m.L + 8.f, m.MH(), 3.f);
    g.DrawText(style::Sans(15.f, muted ? style::Bright() : style::Middle()), "Mute output", IRECT(m.L + 20.f, m.T, m.R + 30.f, m.B));
    const IRECT a = A4Rect();
    char ref[32];
    std::snprintf(ref, sizeof ref, "%.1f Hz", mFrame->tunerA4);
    for (char* c = ref; *c; ++c)
      if (*c == ',') *c = '.';
    g.DrawText(style::Sans(15.f, style::Middle()), "A4", a);
    g.DrawText(style::Sans(15.f, style::Bright(), EAlign::Far), ref, a);
    g.FillRect(style::Shade(1.4f), IRECT(a.L, a.B - 1.f, a.R, a.B));
    g.DrawText(style::Sans(12.f, style::Dim(), EAlign::Center), "Esc or a click outside closes",
               IRECT(cx - 200.f, s.B - 18.f, cx + 200.f, s.B - 2.f));
  }

  FramePtr mFrame;
  Mode mMode = Mode::Menu;
  std::vector<MenuRow> mRows;
  std::vector<int> mColumn, mSlot, mCounts, mOffset;
  IRECT mPanel, mAnchor;
  float mRowH = 25.f, mColumnW = 232.f;
  int mVisible = 1;
  int mHover = -1, mHoverPart = 0;
  bool mCommitPending = false;
  // tuner
  PitchDetector mDetector;
  std::vector<float> mWindow;
  std::vector<double> mHistory;
  double mLastListen = 0., mHeard = 0., mTunerSince = 0.;
  int mNote = -1, mCandidate = -1, mAgree = 0;
  float mCents = 0.f, mHz = 0.f, mLevel = 0.f, mDragA4 = 440.f;
  bool mDraggingA4 = false;
};

}  // namespace ui
}  // namespace magmora
