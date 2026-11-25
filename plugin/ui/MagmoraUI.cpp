// The only UI translation unit in the plug-in projects. WP4: include further control headers, or .cpp files,
// from here, so that plugin/projects never has to change when the UI gains a file.
#include "MagmoraUI.h"

#include "IPlug_include_in_plug_hdr.h"
#include "IControls.h"
#include "ICornerResizerControl.h"

#include <magmora/Engine.h>

#include "controls/AtlasKnob.h"
#include "controls/Lamp.h"
#include "controls/Overlay.h"
#include "controls/Switch.h"
#include "controls/Sections.h"
#include "controls/Pages.h"
#include "controls/UtilityBar.h"

namespace magmora {
namespace ui {
namespace {

constexpr int kOverlayTag = 1;
constexpr int kScalesPercent[] = {75, 100, 125, 150};
// The bar's fields, in signal order: Input, Transpose, Gate, Output (each with its meter, Transpose with its mode
// under it). Then the Quality and Routing words, the window size, the tuner and the about page.
constexpr float kFieldWidth = 96.f, kFieldGap = 24.f;
constexpr float kInputLeft = 494.f;
constexpr float kTransposeLeft = kInputLeft + kFieldWidth + kFieldGap, kTransposeWidth = 104.f;
constexpr float kGateLeft = kTransposeLeft + kTransposeWidth + kFieldGap;
constexpr float kOutputLeft = kGateLeft + kFieldWidth + kFieldGap;
constexpr float kQualityLeft = 984.f;
static_assert(kOutputLeft + kFieldWidth + 20.f <= kQualityLeft, "the fields keep their distance from Quality");

// Three quiet strokes in the corner instead of IGraphics' solid triangle.
class Corner final : public ICornerResizerControl {
public:
  explicit Corner(const IRECT& bounds) : ICornerResizerControl(bounds, 18.f) {}

  void Draw(IGraphics& g) override {
    const IColor c = GetMouseIsOver() ? style::Middle() : style::Middle().WithOpacity(0.35f);
    for (int i = 1; i <= 3; ++i) {
      const float d = mRECT.W() * static_cast<float>(i) / 4.f;
      g.DrawLine(c, mRECT.R - d, mRECT.B - 1.f, mRECT.R - 1.f, mRECT.B - d, nullptr, 1.f);
    }
  }
};

Overlay* OverlayOf(IGraphics& g) {
  IControl* c = g.GetControlWithTag(kOverlayTag);
  return c ? c->As<Overlay>() : nullptr;
}

int Choice(IGraphics& g, int param) {
  return static_cast<int>(std::lround(g.GetDelegate()->GetParam(param)->Value()));
}

void Choose(IGraphics& g, int param, int value) {
  IGEditorDelegate& d = *g.GetDelegate();
  if (Choice(g, param) == value) return;
  d.BeginInformHostOfParamChangeFromUI(param);
  d.SendParameterValueFromUI(param, d.GetParam(param)->ToNormalized(value));
  d.EndInformHostOfParamChangeFromUI(param);
}

MenuRow Option(IGraphics& g, int param, int value, const char* text) {
  IGraphics* ui = &g;
  return MenuRow::Pick(text, Choice(g, param) == value, [ui, param, value] { Choose(*ui, param, value); });
}

// The preset menu: the factory presets on the left, as before; on the right, saving, the user's presets (by folder)
// with rename and delete on the row, and import. Naming and confirming happen in the menu itself: no system dialog
// but the file picker of Import. The menu's state (naming, confirming, the last message) is the frame's.
constexpr float kPresetColumn = 232.f;

// A file dialog's callback runs when the user answers, which may be after the editor (and its frame) or the plug-in
// is gone: it holds the editor's liveness token and the frame weakly, and gets the frame only while both live.
struct LateFrame {
  std::shared_ptr<const std::atomic<bool>> alive;
  std::weak_ptr<Frame> frame;
  FramePtr Get() const { return alive && alive->load() ? frame.lock() : nullptr; }
};

void OpenPresets(IGraphics& g, const FramePtr& frame, const IRECT& anchor, bool fresh);

void SaveAs(IGraphics& g, const FramePtr& frame, const IRECT& anchor, bool replace) {
  Frame::PresetMenu& m = frame->presets;
  bool exists = false;
  const std::string error = frame->host.SaveUserPreset(replace ? m.name : *m.draft, m.folder, replace, &exists);
  if (exists) {
    m.mode = Frame::PresetMenu::Mode::Replace;
    m.name = *m.draft;
  } else if (!error.empty()) {
    m.mode = Frame::PresetMenu::Mode::SaveAs;
    m.note = error;
    m.noteBad = true;
  } else {
    m.mode = Frame::PresetMenu::Mode::Browse;  // saved: the menu has done its job
    return;
  }
  OpenPresets(g, frame, anchor, false);
}

void Rename(IGraphics& g, const FramePtr& frame, const IRECT& anchor, bool replace) {
  Frame::PresetMenu& m = frame->presets;
  bool exists = false;
  const std::string error = frame->host.RenameUserPreset(m.path, *m.draft, replace, &exists);
  if (exists) {
    m.mode = Frame::PresetMenu::Mode::ReplaceRename;
  } else if (!error.empty()) {
    m.note = error;
    m.noteBad = true;
  } else {
    m.mode = Frame::PresetMenu::Mode::Browse;
    m.note.clear();
  }
  OpenPresets(g, frame, anchor, false);
}

void OpenPresets(IGraphics& g, const FramePtr& frame, const IRECT& anchor, bool fresh) {
  using Mode = Frame::PresetMenu::Mode;
  Host& host = frame->host;
  Frame::PresetMenu& m = frame->presets;
  if (fresh) {
    m.mode = Mode::Browse;
    m.note.clear();
  }
  IGraphics* ui = &g;
  const FramePtr f = frame;
  const auto again = [ui, f, anchor] { OpenPresets(*ui, f, anchor, false); };
  const auto to = [f, again](Mode mode) {
    return [f, again, mode] {
      f->presets.mode = mode;
      again();
    };
  };
  const bool user = host.UserPresetLoaded();
  const std::string current = host.UserPresetPath();

  std::vector<MenuRow> rows;
  std::string category;
  for (int i = 0; i < host.PresetCount(); ++i) {
    if (category != host.PresetCategory(i)) {
      category = host.PresetCategory(i);
      rows.push_back(MenuRow::Head(category));
    }
    rows.push_back(MenuRow::Pick(host.PresetName(i), !user && i == host.PresetIndex(), [f, i] { f->host.ChoosePreset(i); }));
  }
  rows.push_back(MenuRow::Column());

  if (!m.note.empty()) rows.push_back(MenuRow::Info(m.note, m.noteBad));
  const std::vector<std::string> missing = host.MissingPresetFiles();
  if (!missing.empty())
    rows.push_back(MenuRow::Info(std::to_string(missing.size()) + (missing.size() == 1 ? " file missing: " : " files missing: ") +
                                     missing.front(),
                                 true));
  const auto startSaveAs = [f, again, user] {
    Frame::PresetMenu& p = f->presets;
    p.mode = Mode::SaveAs;
    p.note.clear();
    *p.draft = f->host.CurrentPresetName() + " 2";
    p.folder.clear();
    if (user)
      for (const UserPresetInfo& e : f->host.UserPresets())
        if (e.path == f->host.UserPresetPath()) p.folder = e.folder;
    again();
  };
  if (m.mode == Mode::SaveAs) {
    rows.push_back(MenuRow::Head("Save as"));
    rows.push_back(MenuRow::Field(*m.draft, m.draft, [ui, f, anchor] { SaveAs(*ui, f, anchor, false); }));
    const std::vector<std::string> folders = host.UserPresetFolders();
    if (!folders.empty()) {
      MenuRow in = MenuRow::Pick("Folder: " + (m.folder.empty() ? std::string("User") : m.folder) + "  \xE2\x80\xBA", false, [f, again, folders] {
        // the next folder: User (the presets folder itself), then each subfolder
        std::string& at = f->presets.folder;
        const auto it = std::find(folders.begin(), folders.end(), at);
        at = at.empty() ? folders.front() : (it == folders.end() || it + 1 == folders.end() ? std::string() : *(it + 1));
        again();
      });
      in.tip = "The folder it is saved in";
      rows.push_back(in);
    }
    rows.push_back(MenuRow::Ask("", {{"Save", [ui, f, anchor] { SaveAs(*ui, f, anchor, false); }},
                                     {"Cancel", to(Mode::Browse)}}));
  } else if (m.mode == Mode::Replace) {
    rows.push_back(MenuRow::Ask("Replace \"" + m.name + "\"?", {}));
    rows.push_back(MenuRow::Ask("", {{"Replace", [ui, f, anchor] { SaveAs(*ui, f, anchor, true); }}, {"Cancel", to(Mode::Browse)}}));
  } else {
    // Save overwrites the user preset in use, without asking: the edit mark warned. A factory preset is never written:
    // there Save is Save as.
    MenuRow save = MenuRow::Pick("Save", false, nullptr);
    if (!user) {
      save.action = startSaveAs;
      save.tip = "Factory presets are read-only: Save makes a copy";
    } else if (!host.PresetEdited()) {
      save.disabled = true;
    } else {
      save.action = [f, again] {
        const std::string error = f->host.SaveCurrentUserPreset();
        if (error.empty()) return;
        f->presets.note = error;
        f->presets.noteBad = true;
        again();
      };
    }
    rows.push_back(save);
    rows.push_back(MenuRow::Pick("Save as...", false, startSaveAs));
  }

  const std::vector<UserPresetInfo> presets = host.UserPresets();
  std::string folder = "\x01";  // no heading yet
  for (const UserPresetInfo& e : presets) {
    if (e.folder != folder) {
      folder = e.folder;
      rows.push_back(MenuRow::Head(folder.empty() ? "User" : folder));
    }
    const bool target = e.path == m.path;
    if (target && m.mode == Mode::Rename) {
      rows.push_back(MenuRow::Field(*m.draft, m.draft, [ui, f, anchor] { Rename(*ui, f, anchor, false); }));
      rows.push_back(MenuRow::Ask("", {{"Rename", [ui, f, anchor] { Rename(*ui, f, anchor, false); }}, {"Cancel", to(Mode::Browse)}}));
      continue;
    }
    if (target && m.mode == Mode::ReplaceRename) {
      rows.push_back(MenuRow::Ask("Replace \"" + *m.draft + "\"?", {}));
      rows.push_back(MenuRow::Ask("", {{"Replace", [ui, f, anchor] { Rename(*ui, f, anchor, true); }}, {"Cancel", to(Mode::Browse)}}));
      continue;
    }
    if (target && m.mode == Mode::Delete) {
      MenuRow question = MenuRow::Ask("Delete \"" + e.name + "\"?", {});
      question.danger = true;
      rows.push_back(question);
      MenuRow ask = MenuRow::Ask("", {{"Delete", [f, again] {
                                                                   const std::string error = f->host.DeleteUserPreset(f->presets.path);
                                                                   f->presets.mode = Mode::Browse;
                                                                   f->presets.note = error;
                                                                   f->presets.noteBad = !error.empty();
                                                                   again();
                                                                 }, true},
                                                                {"Cancel", to(Mode::Browse)}});
      ask.danger = true;
      rows.push_back(ask);
      continue;
    }
    const std::string path = e.path, name = e.name;
    MenuRow row = MenuRow::Pick(e.name, e.path == current, [f, path] {
      f->presets.note = f->host.LoadUserPreset(path);
      f->presets.noteBad = !f->presets.note.empty();
    });
    row.rename = [f, again, path, name] {
      f->presets.mode = Mode::Rename;
      f->presets.path = path;
      f->presets.note.clear();
      *f->presets.draft = name;
      again();
    };
    row.remove = [f, again, path] {
      f->presets.mode = Mode::Delete;
      f->presets.path = path;
      f->presets.note.clear();
      again();
    };
    rows.push_back(row);
  }
  if (presets.empty()) rows.push_back(MenuRow::Info("No presets of your own yet"));
  const std::weak_ptr<Frame> weak = frame;
  rows.push_back(MenuRow::Ask("", {{"Import...", [ui, f, weak, anchor] {
                                      WDL_String file, dir;
                                      const LateFrame late{f->host.UiAlive(), weak};
                                      ui->PromptForFile(file, dir, EFileAction::Open, "json",
                                                        [ui, late, anchor](const WDL_String& name, const WDL_String&) {
                                                          const FramePtr live = late.Get();
                                                          if (!live || !name.GetLength()) return;
                                                          std::string imported;
                                                          const std::string error = live->host.ImportUserPreset(name.Get(), &imported);
                                                          live->presets.mode = Frame::PresetMenu::Mode::Browse;
                                                          live->presets.note = error.empty() ? "1 imported: " + imported
                                                                                             : "1 rejected: " + error;
                                                          live->presets.noteBad = !error.empty();
                                                          OpenPresets(*ui, live, anchor, false);
                                                        });
                                    }},
                                   {"Show folder", [ui, f] {
                                      WDL_String path(f->host.UserPresetFolder(true).c_str());
                                      if (path.GetLength()) ui->RevealPathInExplorerOrFinder(path, false);
                                    }}}));
  if (Overlay* o = OverlayOf(g)) o->OpenMenu(anchor, kPresetColumn, std::move(rows));
}

void OpenEngine(IGraphics& g, Host& host, const IRECT& anchor) {
  std::vector<MenuRow> rows;
  rows.push_back(MenuRow::Head("Quality (oversampling)"));
  rows.push_back(Option(g, kQuality, static_cast<int>(Quality::Standard), "Standard"));
  rows.push_back(Option(g, kQuality, static_cast<int>(Quality::High), "High"));
  rows.push_back(Option(g, kQuality, static_cast<int>(Quality::Ultra), "Ultra, when rendering offline"));
  if (Choice(g, kQuality) == static_cast<int>(Quality::Ultra) && host.QualityInUse() != static_cast<int>(Quality::Ultra))
    rows.push_back(MenuRow::Info("Plays at High in real time"));
  rows.push_back(MenuRow::Head("Filter phase"));
  rows.push_back(Option(g, kPhaseMode, static_cast<int>(PhaseMode::Linear), "Linear"));
  rows.push_back(Option(g, kPhaseMode, static_cast<int>(PhaseMode::Minimum), "Minimum, no latency"));
  rows.push_back(MenuRow::Head("Speaker load"));
  rows.push_back(Option(g, kLoad, static_cast<int>(Load::Reactive4x12), "Reactive 4x12"));
  rows.push_back(Option(g, kLoad, static_cast<int>(Load::Resistive), "Resistive"));
  rows.push_back(MenuRow::Head("Circuit"));
  for (int i = 0; i < host.CircuitCount(); ++i) {
    std::string label = host.CircuitLabel(i);
    if (i == Engine::defaultValueSet()) label += " (default)";
    rows.push_back(MenuRow::Pick(label, i == host.Circuit(), [&host, i] { host.ChooseCircuit(i); }));
  }
  const std::string note = host.SessionNote();
  if (!note.empty()) rows.push_back(MenuRow::Info(note));
  char latency[96];
  const int samples = host.LatencyInSamples();
  const double rate = host.HostSampleRate();
  // The transposer's share: what it adds now, in its mode, or nothing at 0 st (README, "Transpose").
  const int added = host.TransposeLatencyInSamples();
  if (added > 0) {
    std::snprintf(latency, sizeof latency, "Transpose (%s) adds %d samples, %.2f ms",
                  Choice(g, kTransposeMode) == static_cast<int>(TransposeMode::Standard) ? "Standard" : "Tight", added,
                  rate > 0. ? 1000. * added / rate : 0.);
    rows.push_back(MenuRow::Info(latency));
  } else {
    rows.push_back(MenuRow::Info("Transpose at 0 st adds no latency"));
  }
  std::snprintf(latency, sizeof latency, "Latency %d samples, %.2f ms", samples, rate > 0. ? 1000. * samples / rate : 0.);
  rows.push_back(MenuRow::Info(latency));
  if (Overlay* o = OverlayOf(g)) o->OpenMenu(anchor, 290.f, std::move(rows));
}

void OpenRouting(IGraphics& g, const IRECT& anchor) {
  std::vector<MenuRow> rows;
  rows.push_back(MenuRow::Head("Input to the amp"));
  rows.push_back(Option(g, kChannelMode, static_cast<int>(ChannelMode::MonoLeft), "Mono, left channel"));
  rows.push_back(Option(g, kChannelMode, static_cast<int>(ChannelMode::MonoSum), "Mono, left and right summed"));
  rows.push_back(Option(g, kChannelMode, static_cast<int>(ChannelMode::DualMono), "Dual mono, twice the CPU"));
  if (Overlay* o = OverlayOf(g)) o->OpenMenu(anchor, 248.f, std::move(rows));
}

void OpenScale(IGraphics& g, const IRECT& anchor) {
  IGraphics* ui = &g;
  const int now = static_cast<int>(std::lround(g.GetDrawScale() * 100.f));
  std::vector<MenuRow> rows;
  rows.push_back(MenuRow::Head("Window size"));
  for (int percent : kScalesPercent)
    rows.push_back(MenuRow::Pick(std::to_string(percent) + " %", percent == now,
                                 [ui, percent] { ui->Resize(ui->Width(), ui->Height(), static_cast<float>(percent) / 100.f); }));
  if (Overlay* o = OverlayOf(g)) o->OpenMenu(anchor, 140.f, std::move(rows));
}

void AttachBar(IGraphics& g, const FramePtr& frame) {
  Host* host = &frame->host;
  IGraphics* ui = &g;
  const float w = layout::kCanvasW;

  g.AttachControl(new BarChevron(BarRect(14.f, 38.f), -1, [host](const IRECT&) { host->StepPreset(-1); }));
  g.AttachControl(new BarChevron(BarRect(38.f, 62.f), 1, [host](const IRECT&) { host->StepPreset(1); }));
  const FramePtr f = frame;
  g.AttachControl(new BarPreset(
      BarRect(74.f, 250.f), [host] { return host->CurrentPresetName() + (host->PresetEdited() ? " *" : ""); },
      [ui, f](const IRECT& r) { OpenPresets(*ui, f, r, true); }));

  // The fields start right of the section tabs (with their power toggles) and keep the same gap between them.
  g.AttachControl(new BarField(BarRect(kInputLeft, kInputLeft + kFieldWidth), frame, kInputTrim, "Input", BarField::Meter::Input));
  g.AttachControl(new TransposeField(BarRect(kTransposeLeft, kTransposeLeft + kTransposeWidth), frame));
  g.AttachControl(new BarField(BarRect(kGateLeft, kGateLeft + kFieldWidth), frame, kGateThreshold, "Gate", BarField::Meter::Gate));
  g.AttachControl(new BarField(BarRect(kOutputLeft, kOutputLeft + kFieldWidth), frame, kOutputLevel, "Output", BarField::Meter::Output));

  // The engine's, the routing's and the window's menus, each a word with a small caption naming the setting.
  g.AttachControl(new BarWord(
      BarRect(kQualityLeft, kQualityLeft + 84.f),
      [ui, host]() -> std::string {
        if (!host->SampleRateSupported()) return "Rate not supported";
        static const char* const kNames[] = {"Standard", "High", "Ultra"};
        const int chosen = std::min(std::max(Choice(*ui, kQuality), 0), 2);
        std::string word = kNames[chosen];
        if (chosen == static_cast<int>(Quality::Ultra) && host->QualityInUse() != chosen) word += " (High live)";
        return word;
      },
      [ui, host](const IRECT& r) { OpenEngine(*ui, *host, r); }, EAlign::Center, "QUALITY"));
  g.AttachControl(new BarWord(
      BarRect(kQualityLeft + 88.f, kQualityLeft + 168.f),
      [ui]() -> std::string {
        static const char* const kNames[] = {"Mono left", "Mono sum", "Dual mono"};
        return kNames[std::min(std::max(Choice(*ui, kChannelMode), 0), 2)];
      },
      [ui](const IRECT& r) { OpenRouting(*ui, r); }, EAlign::Center, "ROUTING"));
  g.AttachControl(new BarWord(
      BarRect(w - 124.f, w - 76.f),
      [ui] { return std::to_string(std::lround(ui->GetDrawScale() * 100.f)) + " %"; },
      [ui](const IRECT& r) { OpenScale(*ui, r); }, EAlign::Center, "SIZE"));
  g.AttachControl(new BarTuner(BarRect(w - 72.f, w - 44.f), [ui](const IRECT&) {
    if (Overlay* o = OverlayOf(*ui)) o->OpenTuner();
  }));
  g.AttachControl(new BarAbout(BarRect(w - 40.f, w - 10.f), [ui](const IRECT&) { ShowAbout(*ui); }));
}

// ---- the blocks around the amp: one page each, switched from the bar -------------------------------------------

// Section ids: 0 is the amp (the default page), then the blocks. The bar lists them in signal order.
constexpr const char* kSectionGroups[] = {"amp", "pedal", "eq", "cab"};
constexpr const char* kSectionNames[] = {"Amp", "Pedal", "EQ", "Cab"};
constexpr int kSectionOrder[] = {1, 0, 2, 3};  // Pedal, Amp, EQ, Cab
constexpr int kSections = 4;
constexpr float kSectionsLeft = 258.f;  // the tab group: Pedal, Amp, EQ, Cab, each a power toggle and its name

void ShowSection(IGraphics& g, Frame& frame, int section) {
  frame.section = section;
  frame.pageMessage.clear();
  for (int k = 0; k < kSections; ++k)
    g.ForControlInGroup(kSectionGroups[k], [k, section](IControl* c) { c->Hide(k != section); });
  if (Overlay* o = OverlayOf(g)) o->Close();
  g.SetAllControlsDirty();
}

// Each section's own on/off: amp_on, pedal_on, eq_on, cab_on, by section id.
constexpr int kSectionOn[] = {kAmpOn, kPedalOn, kEqOn, kCabOn};
constexpr const char* kSectionTips[] = {"Amp on/off", "Pedal on/off", "EQ on/off", "Cab on/off"};
constexpr double kFlashSeconds = 0.25;

// A section's name in the bar: brighter when shown, in a quieter (still readable) grey while its section is off. A click
// shows its page; a double click, or a right click, switches the section as its power toggle does (the gestures of
// other amp plug-ins), the double click also showing the page. The power symbol flashes so that the switch is seen.
class BarSection final : public BarWord {
public:
  BarSection(const IRECT& r, FramePtr frame, int section)
  : BarWord(r, nullptr, nullptr), mFrame(std::move(frame)), mSection(section) {
    mDblAsSingleClick = false;
    SetTooltip("Click: show \xC2\xB7 Double-click or right-click: on/off");
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (mod.R) Toggle();
    else if (mod.L) ShowSection(*GetUI(), *mFrame, mSection);
  }

  void OnMouseDblClick(float, float, const IMouseMod& mod) override {
    if (!mod.L) return;
    Toggle();
    ShowSection(*GetUI(), *mFrame, mSection);
  }

  bool IsDirty() override {
    const bool shown = mFrame->section == mSection;
    const bool on = GetDelegate()->GetParam(kSectionOn[mSection])->Value() >= 0.5;
    if (shown != mShown || on != mOn) {
      mShown = shown;
      mOn = on;
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    const IColor tone = mShown || GetMouseIsOver() ? style::Bright() : mOn ? style::Middle() : style::DimText();
    const IText text = style::Sans(kBarTextSize, tone);
    const IRECT r = BarText(mRECT);
    g.DrawText(text, kSectionNames[mSection], r);
    IRECT measured = r;
    g.MeasureText(text, kSectionNames[mSection], measured);
    if (mShown) g.FillRect(style::Bright(), IRECT(measured.L, kBarLineY - 1.f, measured.R, kBarLineY + 0.5f));
  }

private:
  void Toggle() {
    const int id = kSectionOn[mSection];
    mFrame->host.SetParameterFromUI(id, GetDelegate()->GetParam(id)->Value() >= 0.5 ? 0. : 1.);
    mFrame->flashUntil[mSection] = mFrame->now + kFlashSeconds;
  }

  FramePtr mFrame;
  const int mSection;
  bool mShown = false, mOn = false;

public:
  int Section() const { return mSection; }
};

// A section's power toggle, left of its name: the standard power symbol in vector, lit in the meter's amber
// with a faint halo while the section is on, the bar's dim grey while it is off. It reads its parameter every frame,
// so the pedal's footswitch, the EQ's button and automation move it too. Its mouse target is its own strip of the
// bar, beside (never over) the name's, reaching 6 points further left into the gap before it than the symbol's strip.
class BarPower final : public ParamControl {
public:
  BarPower(const IRECT& r, FramePtr frame, int section)
  : ParamControl(r, std::move(frame), kSectionOn[section]), mSection(section) {
    mDblAsSingleClick = true;  // two quick clicks are two switches
    SetTooltip(kSectionTips[section]);
    SetTargetRECT(IRECT(r.L - kReach, r.T, r.R, r.B));
  }

  void OnMouseDown(float, float, const IMouseMod& mod) override {
    if (mod.L) SetOnce(Param().GetNormalized() < 0.5 ? 1. : 0.);
  }

  bool IsDirty() override {
    const bool flash = mFrame->now < mFrame->flashUntil[mSection];
    if (flash != mFlash) {
      mFlash = flash;
      SetDirty(false);
    }
    return ParamControl::IsDirty();
  }

  void Draw(IGraphics& g) override {
    const bool on = Normalised() >= 0.5, hot = GetMouseIsOver();
    const float x = mRECT.L + kGlyphX, y = kBarTextY, r = 4.6f;
    IColor c = on ? style::MeterHigh() : hot || mFlash ? style::Middle() : style::Dim();
    if (mFlash) g.FillCircle(style::MeterHigh().WithOpacity(0.4f), x, y, 8.5f);  // switched from the name
    else if (on) g.FillCircle(style::MeterHigh().WithOpacity(hot ? 0.24f : 0.14f), x, y, 7.5f);
    IStrokeOptions round;
    round.mCapOption = ELineCap::Round;
    round.mJoinOption = ELineJoin::Round;
    g.PathClear();
    g.PathArc(x, y, r, 40.f, 320.f);
    g.PathMoveTo(x, y - r - 1.f);
    g.PathLineTo(x, y - 0.8f);
    g.PathStroke(c, 1.4f, round);
  }

  static constexpr float kWidth = 16.f;   // the symbol's strip of the bar
  static constexpr float kReach = 6.f;    // the mouse target's extra width, to the left: 22 points in all
  static constexpr float kGlyphX = 7.f;   // the symbol's centre within the strip

private:
  const int mSection;
  bool mFlash = false;
};

void AttachSections(IGraphics& g, const FramePtr& frame) {
  const float widths[] = {32.f, 38.f, 22.f, 28.f};  // the names' strips, by section id
  float x = kSectionsLeft;
  for (int k : kSectionOrder) {
    g.AttachControl(new BarPower(BarRect(x, x + BarPower::kWidth), frame, k));
    x += BarPower::kWidth;
    g.AttachControl(new BarSection(BarRect(x, x + widths[k]), frame, k));
    x += widths[k] + 12.f;
  }
}

// The last part of a path; trailing separators ignored, as a folder chooser may return "/a/b/".
std::string FileName(const std::string& path) {
  const size_t end = path.find_last_not_of("/\\");
  if (end == std::string::npos) return path;
  const size_t slash = path.find_last_of("/\\", end);
  return path.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1) + 1);
}

using Actions = std::map<std::string, std::pair<std::function<void()>, std::function<bool()>>>;
using Sources = std::map<std::string, std::function<bool()>>;

// A page: its scene, then its controls in the order the contract lists them (the order they are drawn in).
void AttachPage(IGraphics& g, const FramePtr& frame, const char* group, const pages::Page& page,
                const Actions& actions = {}, const Sources& sources = {},
                const std::function<bool(const char*)>& skip = nullptr) {
  g.AttachControl(new Plate(page::Area(), page.background, true), kNoTag, group);
  std::vector<std::pair<DragControl*, IRECT>> readouts;
  for (int i = 0; i < page.count; ++i) {
    const pages::Control& c = page.controls[i];
    if (skip && skip(c.id)) continue;
    switch (c.kind) {
      case pages::Kind::Knob: {
        auto* knob = new PageKnob(c, frame);
        g.AttachControl(knob, kNoTag, group);
        if (!c.disabled) readouts.emplace_back(knob, knob->ReadoutRect());
        break;
      }
      case pages::Kind::Selector: g.AttachControl(new PageSelector(c, frame), kNoTag, group); break;
      case pages::Kind::Switch: g.AttachControl(new PageSwitch(c, frame), kNoTag, group); break;
      case pages::Kind::Action: {
        const auto it = actions.find(c.id);
        if (it != actions.end()) g.AttachControl(new PageAction(c, it->second.first, it->second.second), kNoTag, group);
        break;
      }
      case pages::Kind::Lamp: {
        const auto it = sources.find(c.sourceName);
        g.AttachControl(new PageLamp(c, frame, it != sources.end() ? it->second : nullptr), kNoTag, group);
        break;
      }
      case pages::Kind::Window: break;  // filled by the page's own code
    }
  }
  for (const auto& r : readouts) g.AttachControl(new KnobReadout(r.first, r.second, r.first->ParamId()), kNoTag, group);
}

void AttachPedal(IGraphics& g, const FramePtr& frame) {
  if (const pages::Page* page = pages::Find("pedal")) AttachPage(g, frame, kSectionGroups[1], *page);
}

void AttachEq(IGraphics& g, const FramePtr& frame) {
  const pages::Page* page = pages::Find("eq");
  if (!page) return;
  AttachPage(g, frame, kSectionGroups[2], *page);
  if (const pages::Control* screen = pages::Find(*page, "eq_screen"))
    g.AttachControl(new EqScreen(pages::Cell(*screen)), kNoTag, kSectionGroups[2]);
}

// The cabinet: two slots, each a cone with its marker and a row under it, summed in parallel. A slot's
// source is one of the pack's microphones or its own response file (mic1_source, mic2_source: the session's). A slot
// holding a file dims its cone, hides its marker and position and distance; on, level, pan and polarity work for both.
void AttachCab(IGraphics& g, const FramePtr& frame) {
  const pages::Page* page = pages::Find("cab");
  if (!page) return;
  const char* group = kSectionGroups[3];
  Host* host = &frame->host;
  IGraphics* ui = &g;
  Frame* f = frame.get();  // the menus below live in this editor: they cannot outlive the frame
  const std::weak_ptr<Frame> weak = frame;  // what the file dialogs' late callbacks hold

  Actions actions;
  actions["cab_pack"] = {[ui, host, f, weak] {
    std::vector<MenuRow> rows;
    const CabinetStatus s = host->CabStatus();
    rows.push_back(MenuRow::Head("Microphone pack"));
    const bool builtin = host->CabIsBuiltin();
    if (builtin) {
      rows.push_back(MenuRow::Info("Built-in: " + host->BuiltinCabName() + ", modelled responses"));
    } else if (s.packLoaded) {
      rows.push_back(MenuRow::Info(FileName(s.folder) + ", " + std::to_string(s.files) + " files, " +
                                   std::to_string(s.skipped) + " skipped, phase " + s.phase));
    } else {
      rows.push_back(MenuRow::Info("None loaded: pack microphones are silent"));
    }
    const std::string message = !f->pageMessage.empty() ? f->pageMessage : host->CabMessage();
    if (!message.empty()) rows.push_back(MenuRow::Info(message));
    if (!host->BuiltinCabName().empty())
      rows.push_back(MenuRow::Pick("Built-in: " + host->BuiltinCabName(), builtin, [host, f] {
        f->pageMessage = host->LoadBuiltinCab();
      }));
    rows.push_back(MenuRow::Pick("Choose pack folder...", false, [ui, host, weak] {
      WDL_String dir;
      const LateFrame late{host->UiAlive(), weak};
      ui->PromptForDirectory(dir, [late](const WDL_String&, const WDL_String& path) {
        const FramePtr live = late.Get();
        if (live && path.GetLength()) live->pageMessage = live->host.LoadCabPack(path.Get(), "");
      });
    }));
    rows.push_back(MenuRow::Pick("Mapping file...", false, [ui, host, f, weak] {
      const std::string folder = host->CabStatus().folder;
      if (folder.empty() || host->CabIsBuiltin()) {
        f->pageMessage = "Choose a pack folder first.";
        return;
      }
      WDL_String file, path;
      const LateFrame late{host->UiAlive(), weak};
      ui->PromptForFile(file, path, EFileAction::Open, "json", [late, folder](const WDL_String& name, const WDL_String&) {
        const FramePtr live = late.Get();
        if (live && name.GetLength()) live->pageMessage = live->host.LoadCabPack(folder, name.Get());
      });
    }));
    rows.push_back(MenuRow::Pick("Clear the pack", false, [host, f] {
      host->ClearCabPack();
      f->pageMessage.clear();
    }));
    if (Overlay* o = OverlayOf(*ui)) {
      const pages::Control* c = pages::Find(*pages::Find("cab"), "cab_pack");
      o->OpenMenu(c ? pages::Cell(*c) : IRECT(560.f, 500.f, 600.f, 540.f), 300.f, std::move(rows));
    }
  }, nullptr};
  // Everything of the two slots is made here; the generic part makes the scene and the corner.
  AttachPage(g, frame, group, *page, actions, {}, [](const char* id) {
    return std::strncmp(id, "mic1_", 5) == 0 || std::strncmp(id, "mic2_", 5) == 0 || std::strncmp(id, "slot", 4) == 0 ||
           std::strncmp(id, "cone", 4) == 0;
  });

  const auto set = [ui](int param, double plain) {
    IGEditorDelegate& d = *ui->GetDelegate();
    if (d.GetParam(param)->Value() == plain) return;
    d.BeginInformHostOfParamChangeFromUI(param);
    d.SendParameterValueFromUI(param, d.GetParam(param)->ToNormalized(plain));
    d.EndInformHostOfParamChangeFromUI(param);
  };

  for (int n = 1; n <= 2; ++n) {
    const int m = n - 1;
    const int on = n == 1 ? kMic1On : kMic2On, type = n == 1 ? kMic1Type : kMic2Type;
    const int source = n == 1 ? kMic1Source : kMic2Source;
    const int base = n == 1 ? kMic1Position : kMic2Position;  // position, distance, angle, level, pan, polarity
    const std::string p = "mic" + std::to_string(n) + "_";
    const auto at = [page](const std::string& id) -> const pages::Control& { return *pages::Find(*page, id.c_str()); };
    const auto holds = [ui, source] { return ui->GetDelegate()->GetParam(source)->Value() >= 0.5; };
    const auto fileText = [host, m]() -> std::string {
      const CabIrInfo ir = host->CabIr(m);
      if (!ir.loaded) return ir.reference.empty() ? "No file loaded" : "Missing: " + FileName(ir.reference);
      char detail[48];
      std::snprintf(detail, sizeof detail, "   %.2f s %s", ir.seconds, ir.channels > 1 ? "stereo" : "mono");
      return FileName(ir.reference) + detail;
    };
    // a pack microphone: the pack's name (the built-in cabinet's, or the folder's) and the microphone
    const auto packText = [host, ui, type]() -> std::string {
      if (host->CabMicCount() == 0) return {};
      const std::string pack = host->CabIsBuiltin() ? host->BuiltinCabName() : FileName(host->CabStatus().folder);
      const std::string mic = host->CabMicName(Choice(*ui, type));
      if (pack.empty()) return mic;
      return mic.empty() ? pack : pack + "  \xC2\xB7  " + mic;
    };
    if (const pages::Control* cone = pages::Find(*page, n == 1 ? "cone1" : "cone2"))
      g.AttachControl(new ConeMarker(*cone, frame, n, on, base, base + 1, holds, fileText, packText), kNoTag, group);

    // The row: what both sources have, then position and distance twice (working for a microphone, dimmed for a
    // file); SlotView shows the pair that fits the slot's source.
    std::vector<IControl*> shared, micSet, fileSet;
    const auto add = [&](IControl* c, std::vector<IControl*>& set_) {
      g.AttachControl(c, kNoTag, group);
      set_.push_back(c);
    };
    add(new PageSwitch(at(p + "on"), frame, on), shared);
    add(new PageKnob(at(p + "level"), frame, base + 3), shared);
    add(new PageKnob(at(p + "pan"), frame, base + 4), shared);
    add(new PageSwitch(at(p + "polarity"), frame, base + 5), shared);
    add(new PageLamp(at(p + "on_led"), frame), shared);
    add(new PageLamp(at(p + "polarity_led"), frame), shared);
    add(new PageKnob(at(p + "position"), frame, base), micSet);
    add(new PageKnob(at(p + "position"), frame, base, true), fileSet);
    add(new PageKnob(at(p + "distance"), frame, base + 1), micSet);
    add(new PageKnob(at(p + "distance"), frame, base + 1, true), fileSet);
    for (std::vector<IControl*>* set_ : {&shared, &micSet}) {
      std::vector<PageKnob*> knobs;
      for (IControl* c : *set_)
        if (auto* k = dynamic_cast<PageKnob*>(c); k && !k->IsDisabled()) knobs.push_back(k);
      for (PageKnob* k : knobs) add(new KnobReadout(k, k->ReadoutRect(), k->ParamId()), *set_);
    }
    g.AttachControl(new SlotView(holds, micSet, fileSet), kNoTag, group);

    g.AttachControl(new GlassField(
        pages::Cell(at("slot" + std::to_string(n) + "_window")), "SOURCE",
        [host, ui, m, type, holds]() -> std::string {
          if (holds()) {
            const CabIrInfo ir = host->CabIr(m);
            if (ir.loaded) return "File: " + FileName(ir.reference);
            return ir.reference.empty() ? std::string("File: none loaded") : "Missing: " + FileName(ir.reference);
          }
          if (host->CabMicCount() == 0) return "No pack";
          const std::string name = host->CabMicName(Choice(*ui, type));
          return name.empty() ? "Not in pack" : name;
        },
        [host, ui, f, weak, m, on, type, source, holds, set](const IRECT& anchor) {
          std::vector<MenuRow> rows;
          const bool file = holds();
          rows.push_back(MenuRow::Head(host->CabMicCount() ? "Microphones in the pack" : "No pack loaded"));
          for (int i = 0; i < std::min(host->CabMicCount(), kMaxCabinetMics); ++i) {
            const bool chosen = !file && Choice(*ui, type) == i;
            rows.push_back(MenuRow::Pick(host->CabMicName(i), chosen, [ui, on, type, source, i, set] {
              Choose(*ui, type, i);
              set(source, 0.);
              set(on, 1.);
            }));
          }
          if (!file && host->CabMicCount() > 0) {
            const std::vector<std::string> speakers = host->CabSpeakers(m);
            if (speakers.size() > 1) {
              rows.push_back(MenuRow::Head("Speaker"));
              const std::string variant = host->CabSource(m, true);
              rows.push_back(MenuRow::Pick("Automatic", host->CabSource(m, false).empty(),
                                           [host, m, variant] { host->SetCabSource(m, "", variant); }));
              for (const std::string& sp : speakers)
                rows.push_back(MenuRow::Pick(sp, sp == host->CabSource(m, false), [host, m, sp] { host->SetCabSource(m, sp, ""); }));
            }
            const std::string speaker = host->CabSource(m, false);
            const std::vector<std::string> variants = host->CabVariants(m, speaker);
            if (variants.size() > 1) {
              rows.push_back(MenuRow::Head("Variant (preamp, take)"));
              rows.push_back(MenuRow::Pick("Automatic", host->CabSource(m, true).empty(),
                                           [host, m, speaker] { host->SetCabSource(m, speaker, ""); }));
              for (const std::string& v : variants)
                rows.push_back(MenuRow::Pick(v, v == host->CabSource(m, true), [host, m, speaker, v] { host->SetCabSource(m, speaker, v); }));
            }
            const CabinetMicStatus st = host->CabStatus().mic[m];
            if (st.on && st.found)
              rows.push_back(MenuRow::Info(st.morphX || st.morphDist ? "Now between recordings" : "Now on a recording"));
          }
          rows.push_back(MenuRow::Head("Response file"));
          const CabIrInfo ir = host->CabIr(m);
          if (ir.loaded) {
            rows.push_back(MenuRow::Pick(FileName(ir.reference), file, [on, source, set] {
              set(source, 1.);
              set(on, 1.);
            }));
          }
          if (!ir.reference.empty()) {  // the other files of its folder, one step at a time
            const auto step = [host, f, m, on, source, set](int by) {
              return [host, f, m, on, source, set, by] {
                f->irMessage[m] = host->StepCabIr(m, by);
                if (f->irMessage[m].empty()) {
                  set(source, 1.);
                  set(on, 1.);
                }
              };
            };
            rows.push_back(MenuRow::Ask("", {{"\xE2\x80\xB9 Previous file", step(-1)}, {"Next file \xE2\x80\xBA", step(1)}}));
          }
          rows.push_back(MenuRow::Pick("Load IR file...", false, [ui, host, weak, m, on, source] {
            WDL_String name, path;
            const LateFrame late{host->UiAlive(), weak};
            ui->PromptForFile(name, path, EFileAction::Open, "wav", [late, m, on, source](const WDL_String& file, const WDL_String&) {
              const FramePtr live = late.Get();
              if (!live || !file.GetLength()) return;
              live->irMessage[m] = live->host.LoadCabIr(m, file.Get());
              if (live->irMessage[m].empty()) {  // through the plug-in: the editor need not be the one that asked
                live->host.SetParameterFromUI(source, 1.);
                live->host.SetParameterFromUI(on, 1.);
              }
            });
          }));
          if (ir.loaded) {
            rows.push_back(MenuRow::Pick("Normalise", ir.normalize, [host, m, ir] { host->SetCabIrNormalize(m, !ir.normalize); }));
            rows.push_back(MenuRow::Pick("Clear", false, [host, f, m, source, set] {
              host->ClearCabIr(m);
              set(source, 0.);
              f->irMessage[m].clear();
            }));
          }
          if (!f->irMessage[m].empty()) rows.push_back(MenuRow::Info("Could not use it: " + f->irMessage[m]));
          else if (!ir.message.empty()) rows.push_back(MenuRow::Info(ir.message));
          if (Overlay* o = OverlayOf(*ui)) o->OpenMenu(anchor, 260.f, std::move(rows));
        },
        // The arrows on the field step through the folder of the slot's file, as long as the slot plays a file.
        [host, f, m](int by) { f->irMessage[m] = host->StepCabIr(m, by); },
        [host, m, holds] { return holds() && !host->CabIr(m).reference.empty(); }), kNoTag, group);
  }
}

void AttachPages(IGraphics& g, const FramePtr& frame) {
  AttachPedal(g, frame);
  AttachEq(g, frame);
  AttachCab(g, frame);
  for (int k = 1; k < kSections; ++k) g.ForControlInGroup(kSectionGroups[k], [](IControl* c) { c->Hide(true); });
}

// ---- the hint line ------------------------------------------------------------------------------------------------

// The manual (config.h); empty until the project has a public home.
std::string ManualUrl() { return MAGMORA_MANUAL_URL; }

// Help where a control's job is not obvious from its name.
const char* HelpFor(int param) {
  switch (param) {
    case kInputTrim: return "Aim the loudest picking between the marks; Input trim adjusts it";
    case kGateThreshold: return "Noise gate threshold; at its minimum the gate is off";
    case kOutputLevel: return "Level sent to the host";
    case kResonance: return "Low-end thump of the power amp, around the speaker's resonance";
    case kPresence: return "Treble of the power amp, set in its negative feedback";
    case kStandby: return "Standby: the valves stay warm, the amp is silent";
    case kInputJack: return "High or Normal input: Normal is 6 dB quieter";
    case kMicAlign: return "Align: removes the difference in arrival time between the two microphones";
    case kMicOffset: return "Offset: puts a chosen arrival-time difference back, in millimetres of distance";
    case kEqPosition: return "Order: the EQ before or after the cabinet";
    case kTranspose: return "Semitones, -12 to +12; at 0 st it is out of the signal, with no latency";
    default: return "";
  }
}

// The manual's section (README.md headings) for a parameter.
const char* AnchorFor(int param) {
  if (param == kTranspose || param == kTransposeMode) return "#transpose";
  if (param == kInputTrim || param == kGateThreshold || param == kOutputLevel) return "#the-bar";
  if (param == kPedalOn || param == kAmpOn || param == kEqOn || param == kCabOn) return "#the-pages";
  return "#mouse-and-keyboard";
}

// A line under the bar, shown after the pointer has rested on a control for 0.6 s: its name, its value and what it is
// for, and a link to that part of the manual (when MAGMORA_MANUAL_URL is set). It stays while the pointer is on it,
// so that the link can be reached, and for a second after the pointer leaves the controls.
class HintStrip final : public IControl {
public:
  static constexpr float kHeight = 22.f;

  explicit HintStrip(FramePtr frame)
  : IControl(IRECT(0.f, layout::kBar.y + layout::kBar.h, layout::kCanvasW, layout::kBar.y + layout::kBar.h + kHeight)),
    mFrame(std::move(frame)) {}

  bool IsHit(float x, float y) const override { return mShown && mRECT.Contains(x, y); }

  bool IsDirty() override {
    IGraphics& g = *GetUI();
    const double now = mFrame->now;
    const int idx = g.GetMouseOver();
    IControl* c = idx >= 0 && idx < g.NControls() ? g.GetControl(idx) : nullptr;
    Overlay* o = OverlayOf(g);
    std::string text, anchor;
    bool show = mShown;
    if (o && !o->IsHidden()) {
      show = false;
      mTarget = nullptr;
    } else if (c == this) {
      mLinger = now + 1.0;
      text = mText;
      anchor = mAnchor;
    } else if (Describe(c, text, anchor)) {
      if (c != mTarget) {
        mTarget = c;
        mSince = now;
      }
      if (mShown || now - mSince >= 0.6) show = true;
      mLinger = now + 1.0;
      if (!show) text.clear();
    } else {
      mTarget = nullptr;
      if (now > mLinger) show = false;
      text = mText;
      anchor = mAnchor;
    }
    if (show && text.empty()) {
      text = mText;
      anchor = mAnchor;
    }
    if (show != mShown || text != mText) {
      mShown = show;
      mText = text;
      mAnchor = anchor;
      SetDirty(false);
    }
    return IControl::IsDirty();
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override {
    if (mod.L && mShown && HasManual() && LinkRect().Contains(x, y))
      GetUI()->OpenURL((ManualUrl() + mAnchor).c_str());
  }

  void Draw(IGraphics& g) override {
    if (!mShown || mText.empty()) return;
    g.FillRect(style::Shade(0.92f, 238), mRECT);
    g.FillRect(style::Shade(1.25f), IRECT(mRECT.L, mRECT.B - 1.f, mRECT.R, mRECT.B));
    g.DrawText(style::Sans(13.f, style::Bright()), mText.c_str(), IRECT(14.f, mRECT.T, mRECT.R - 120.f, mRECT.B));
    if (HasManual()) g.DrawText(style::Sans(13.f, style::Middle(), EAlign::Far), "Manual \xE2\x86\x97", LinkRect());
  }

  const std::string& Text() const { return mText; }

private:
  static bool HasManual() { return !ManualUrl().empty(); }
  IRECT LinkRect() const { return IRECT(mRECT.R - 110.f, mRECT.T, mRECT.R - 14.f, mRECT.B); }

  bool Describe(IControl* c, std::string& text, std::string& anchor) const {
    if (!c) return false;
    const auto it = mFrame->hints.find(c);
    if (it != mFrame->hints.end()) {
      text = it->second.first;
      anchor = it->second.second;
      return true;
    }
    if (auto* t = dynamic_cast<TransposeField*>(c); t && t->OverMode()) {
      text = std::string("Transpose mode   ") + (t->Mode() == static_cast<int>(TransposeMode::Standard) ? "Standard" : "Tight") +
             "   \xC2\xB7   Tight: 5.3 ms, for playing. Standard: 10.7 ms, smoother low notes   \xC2\xB7   Click to switch";
      anchor = AnchorFor(kTransposeMode);
      return true;
    }
    auto* p = dynamic_cast<ParamControl*>(c);
    if (!p) return false;
    const int id = p->ParamId();
    const auto* drag = dynamic_cast<DragControl*>(c);
    std::string value;
    if (drag) {
      value = DisplayValue(id, p->Plain());
    } else {
      WDL_String display;
      const_cast<HintStrip*>(this)->GetDelegate()->GetParam(id)->GetDisplay(display);
      value = display.Get();
    }
    text = std::string(Info(id).name) + "   " + value;
    const std::string help = HelpFor(id);
    if (!help.empty()) text += "   \xC2\xB7   " + help;
    text += drag ? "   \xC2\xB7   Drag (Shift: fine), double-click: default, right-click: type" : "   \xC2\xB7   Click to switch";
    anchor = AnchorFor(id);
    return true;
  }

  FramePtr mFrame;
  IControl* mTarget = nullptr;
  double mSince = 0., mLinger = 0.;
  bool mShown = false;
  std::string mText, mAnchor;
};

// The hint line's text for the controls that are not a parameter's.
void DescribeControls(IGraphics& g, const FramePtr& frame) {
  const auto hint = [&](IControl* c, const std::string& text, const char* anchor) { frame->hints[c] = {text, anchor}; };
  const char* const dot = "   \xC2\xB7   ";
  for (int i = 0; i < g.NControls(); ++i) {
    IControl* c = g.GetControl(i);
    if (dynamic_cast<BarPreset*>(c))
      hint(c, std::string("Presets") + dot + "Click for the menu: factory presets, yours, Save, Save as, Import", "#presets");
    else if (dynamic_cast<BarChevron*>(c))
      hint(c, std::string("Previous or next preset") + dot + "Factory presets, then yours, round and round", "#presets");
    else if (auto* s = dynamic_cast<BarSection*>(c))
      hint(c, std::string(kSectionNames[s->Section()]) + dot + "Click: show the page" + dot + "Double-click or right-click: switch it on or off",
           "#the-pages");
    else if (dynamic_cast<BarTuner*>(c))
      hint(c, std::string("Tuner") + dot + "Chromatic, A4 430 to 450 Hz; mutes the output while open (switchable)", "#tuner");
    else if (dynamic_cast<BarAbout*>(c))
      hint(c, std::string("About") + dot + "Version, the mouse gestures, the manual", "#mouse-and-keyboard");
    else if (auto* w = dynamic_cast<BarWord*>(c); w && w->Caption()) {
      const std::string caption = w->Caption();
      if (caption == "QUALITY")
        hint(c, std::string("Quality") + dot + "Oversampling, filter phase, speaker load and circuit", "#the-bar");
      else if (caption == "ROUTING")
        hint(c, std::string("Routing") + dot + "Which input channels reach the amp: left, both summed, or two amps", "#the-bar");
      else
        hint(c, std::string("Window size") + dot + "Or drag the bottom-right corner", "#the-bar");
    } else if (auto* m = dynamic_cast<ConeMarker*>(c))
      hint(c, "Microphone " + std::to_string(m->Slot()) + dot + "Drag: sideways for the position, down for the distance" + dot +
                  "Shift: fine" + dot + "Double-click: back to the default", "#the-pages");
    else if (dynamic_cast<EqScreen*>(c))
      hint(c, std::string("EQ points") + dot + "Drag: frequency and gain" + dot + "Wheel: width" + dot + "Double-click: default", "#the-pages");
    else if (dynamic_cast<GlassField*>(c))
      hint(c, std::string("Source") + dot + "A microphone of the pack, or a response file; the arrows step through its folder", "#the-pages");
    else if (dynamic_cast<PageAction*>(c))
      hint(c, std::string("Pack") + dot + "The cabinet's microphones: the built-in cabinet or a folder of your own", "#the-pages");
  }
}

// What the about page lists of the mouse, and its links (manual; audio settings in the stand-alone).
const char* const kHelp[][2] = {
    {"Drag", "turn a knob, or change a value in the bar"},
    {"Shift-drag", "fine adjustment"},
    {"Double-click or Alt-click", "default value (knobs, microphones, EQ points)"},
    {"Scroll", "small steps"},
    {"Right-click", "type a value"},
    {"Double- or right-click a tab", "switch that section on or off"},
};
constexpr float kAboutTop = 92.f;
constexpr float kAboutLinks = kAboutTop + 54.f + 4 * 24.f + 18.f + 26.f * std::size(kHelp) + 14.f;

}  // namespace

IRECT Overlay::AboutRow(int i) const {
  const float left = mRECT.MW() - 250.f;
  return IRECT(left, kAboutLinks + 26.f * i, left + 300.f, kAboutLinks + 26.f * (i + 1));
}

void Overlay::DrawAbout(IGraphics& g) const {
  g.FillRect(style::Shade(0.3f, 250), mRECT);
  const float left = mRECT.MW() - 250.f;
  float y = kAboutTop;
  const auto line = [&](const IText& text, const std::string& s, float height) {
    g.DrawText(text, s.c_str(), IRECT(left, y, left + 500.f, y + height));
    y += height;
  };
  const IText body = style::Sans(16.f, style::Middle());
  line(IText(46.f, style::Bright(), style::kPanel, EAlign::Near, EVAlign::Middle), PLUG_NAME, 54.f);
  line(body, "A circuit model of a two-channel, 120 watt, all-valve high-gain head.", 24.f);
  line(body, "An independent project. Not affiliated with any amplifier maker.", 24.f);
  line(body, "Circuit: Revised uses a few values of a later drawing, which fit two", 24.f);
  line(body, "captured amps better. As drawn keeps every value of the 1992 drawing.", 24.f);
  y += 18.f;

  for (const auto& row : kHelp) {
    const IRECT r(left, y, left + 500.f, y + 26.f);
    g.DrawText(style::Sans(16.f, style::Bright()), row[0], r);
    g.DrawText(body, row[1], IRECT(left + 230.f, r.T, r.R + 40.f, r.B));
    y += 26.f;
  }
  for (size_t i = 0; i < mRows.size(); ++i) {
    const IRECT r = AboutRow(static_cast<int>(i));
    const bool hot = static_cast<int>(i) == mHover;
    g.DrawText(style::Sans(16.f, hot ? style::Bright() : style::MeterHigh()), (mRows[i].text + "  \xE2\x86\x97").c_str(), r);
  }
  y = kAboutLinks + 26.f * static_cast<float>(mRows.size()) + 12.f;

  const IText fineprint = style::Sans(14.f, style::DimText());
  line(fineprint, std::string("Version ") + PLUG_VERSION_STR + ", engine " + versionString() + ", model "
                  + std::string(modelHash()).substr(0, 12), 22.f);
  line(fineprint, std::string(PLUG_COPYRIGHT_STR) + ". MIT licence.", 22.f);
  line(fineprint, "Built with iPlug2. Type: Barlow, SIL Open Font License.", 22.f);
}

void Layout(IGraphics& g, Host& host) {
  if (g.NControls()) return;
  g.SetScaleConstraints(kMinScale, kMaxScale);
  g.EnableMouseOver(true);
  g.EnableTooltips(true);  // the section tabs' toggles and names carry one; everything has a line in the hint strip
  g.LoadFont(style::kSans, "Barlow-Medium.ttf");
  g.LoadFont(style::kPanel, "BarlowCondensed-SemiBold.ttf");

  const FramePtr frame = std::make_shared<Frame>(host);
  const IRECT all(0.f, 0.f, layout::kCanvasW, layout::kCanvasH);

  // Back to front. The backdrop is control 0: it is polled first and owns the frame's tick.
  g.AttachControl(new Backdrop(all, frame));
  // The amp, in its own group so that a block's page can take its place (and its textures' memory).
  const char* amp = kSectionGroups[0];
  g.AttachControl(new Plate(layout::Rect(layout::kBackground), layout::kBackgroundFile, false), kNoTag, amp);
  // The layout lists the art in drawing order.
  std::vector<std::pair<AtlasKnob*, int>> knobs;
  for (const layout::Element& e : layout::kElements) {
    switch (e.kind) {
      case layout::Kind::Knob: {
        auto* knob = new AtlasKnob(e, frame);
        g.AttachControl(knob, kNoTag, amp);
        knobs.emplace_back(knob, e.param);
        break;
      }
      case layout::Kind::Switch: g.AttachControl(new Switch(e, frame), kNoTag, amp); break;
      case layout::Kind::Glow: g.AttachControl(new Lamp(e, frame), kNoTag, amp); break;
      case layout::Kind::Static: g.AttachControl(new StaticArt(e), kNoTag, amp); break;
    }
  }
  AttachBar(g, frame);
  for (const auto& k : knobs) g.AttachControl(new KnobReadout(k.first, k.second), kNoTag, amp);
  AttachPages(g, frame);
  AttachSections(g, frame);
  DescribeControls(g, frame);
  g.AttachControl(new HintStrip(frame));
  g.AttachControl(new Overlay(all, frame), kOverlayTag);

  g.AttachCornerResizer(new Corner(all), EUIResizerMode::Scale);

  IGraphics* ui = &g;
  g.SetKeyHandlerFunc([ui](const IKeyPress& key, bool isUp) {
    Overlay* o = OverlayOf(*ui);
    if (isUp || key.VK != kVK_ESCAPE || !o || o->IsHidden()) return false;  // the space bar stays the host's
    o->Close();
    return true;
  });
}

void Closing(IGraphics& g) {
  g.ForStandardControlsFunc([](IControl* c) {
    if (auto* p = dynamic_cast<ParamControl*>(c)) p->Cancel();
  });
  if (Overlay* o = OverlayOf(g)) o->Close();  // the tuner lets go of the input and the mute
  TextureBank::Forget(g);
}

void ShowAbout(IGraphics& g) {
  Overlay* o = OverlayOf(g);
  if (!o) return;
  std::vector<MenuRow> links;
  IGraphics* ui = &g;
  if (!ManualUrl().empty())
    links.push_back(MenuRow::Pick("Open the manual", false, [ui] { ui->OpenURL(ManualUrl().c_str()); }));
  Host* host = &o->HostOf();
  if (host->CanShowAudioSettings())
    links.push_back(MenuRow::Pick("Audio settings...", false, [host] { host->ShowAudioSettings(); }));
  o->OpenAbout(std::move(links));
}

}  // namespace ui
}  // namespace magmora
