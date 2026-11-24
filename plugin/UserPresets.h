// The user's presets on disk: where they live, listing them, reading and writing files safely. Main thread only;
// nothing here is ever called from the audio thread. Included once, from Magmora.cpp (so the projects need no edit).
//
//   macOS    ~/Library/Audio/Presets/Magmora/            (per user, no admin rights; Logic only scans .aupreset there)
//   Windows  Documents\Magmora\Presets\                  (SHGetKnownFolderPath; written, not tested)
//   tests    $MAGMORA_PRESETS_DIR, when set
//
// One level of subfolders; each becomes a heading in the menu. The folder is made on the first save, never on load.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <magmora/Preset.h>

#if defined _WIN32
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace user_presets {

namespace fs = std::filesystem;

inline fs::path FromUtf8(const std::string& s) { return fs::u8path(s); }
inline std::string ToUtf8(const fs::path& p) { return p.u8string(); }

inline fs::path Root() {
  if (const char* forced = std::getenv("MAGMORA_PRESETS_DIR"); forced && *forced) return FromUtf8(forced);
#if defined _WIN32
  PWSTR documents = nullptr;
  fs::path root;
  if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) root = fs::path(documents) / "Magmora" / "Presets";
  CoTaskMemFree(documents);
  return root;
#else
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return FromUtf8(home) / "Library" / "Audio" / "Presets" / "Magmora";
#endif
}

inline std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// True if `p` names something inside the presets folder (no "..", no other place): every path the editor hands back
// is checked before anything is written, renamed or deleted.
inline bool Inside(const fs::path& p) {
  const fs::path root = Root();
  if (root.empty() || p.empty()) return false;
  const fs::path r = root.lexically_normal(), q = p.lexically_normal();
  auto a = r.begin(), b = q.begin();
  for (; a != r.end(); ++a, ++b) {
    if (a->empty()) continue;  // a trailing separator
    if (b == q.end() || *a != *b) return false;
  }
  for (; b != q.end(); ++b)
    if (*b == "..") return false;
  return true;
}

struct Entry {
  std::string name;    // from the file ("name" is authoritative)
  std::string folder;  // "" for the folder itself, else the subfolder's name
  std::string path;    // absolute, UTF-8
};

inline bool ReadText(const fs::path& p, std::string& text, std::string* error) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec) {
    if (error) *error = "cannot read the file";
    return false;
  }
  if (size > magmora::kMaxPresetFileBytes) {
    if (error) *error = "too large for a preset";
    return false;
  }
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read the file";
    return false;
  }
  text.assign(static_cast<size_t>(size), '\0');
  if (size && !in.read(&text[0], static_cast<std::streamsize>(size))) {
    if (error) *error = "cannot read the file";
    return false;
  }
  return true;
}

inline bool ReadPreset(const fs::path& p, magmora::UserPreset& preset, std::string* error) {
  std::string text;
  return ReadText(p, text, error) && magmora::readUserPreset(text.data(), text.size(), preset, error);
}

// Written next to the target under a temporary name, flushed, then renamed over it: a crash or a full disk leaves
// either the old file or the new one, never half of one.
inline bool WriteAtomic(const fs::path& target, const std::string& text, std::string* error) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  static std::mt19937 random{std::random_device{}()};
  const fs::path temp = target.parent_path() / FromUtf8("." + ToUtf8(target.filename()) + "." +
                                                        std::to_string(random() % 1000000u) + ".tmp");
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size())) || !out.flush()) {
      out.close();
      fs::remove(temp, ec);
      if (error) *error = "could not write the preset (is the disk full or the folder read-only?)";
      return false;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    if (error) *error = "could not write the preset";
    return false;
  }
  return true;
}

// The subfolders that hold presets, sorted without case.
inline std::vector<std::string> Folders() {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path root = Root();
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = ToUtf8(it->path().filename());
    if (!name.empty() && name[0] != '.' && it->is_directory(ec)) out.push_back(name);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
  return out;
}

// Rescanned each time (the menu opens, prev/next): files added in Finder show up at once. Names are cached by path,
// size and modification time, so a rescan reads only what changed. Root first, then each folder; by name, no case.
inline std::vector<Entry> Scan() {
  struct Cached {
    uintmax_t size;
    fs::file_time_type time;
    std::string name;
    bool valid;
  };
  static std::map<std::string, Cached> cache;  // main thread only
  std::vector<Entry> out;
  const fs::path root = Root();
  if (root.empty()) return out;
  std::vector<std::string> folders{""};
  for (const std::string& f : Folders()) folders.push_back(f);
  for (const std::string& folder : folders) {
    std::vector<Entry> here;
    std::error_code ec;
    const fs::path dir = folder.empty() ? root : root / FromUtf8(folder);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const fs::path& p = it->path();
      const std::string file = ToUtf8(p.filename());
      if (file.empty() || file[0] == '.' || Lower(ToUtf8(p.extension())) != ".json" || !it->is_regular_file(ec)) continue;
      std::error_code e2;
      const uintmax_t size = fs::file_size(p, e2);
      const fs::file_time_type time = fs::last_write_time(p, e2);
      const std::string key = ToUtf8(p);
      auto c = cache.find(key);
      if (c == cache.end() || c->second.size != size || c->second.time != time) {
        magmora::UserPreset preset;
        const bool valid = ReadPreset(p, preset, nullptr);
        c = cache.insert_or_assign(key, Cached{size, time, valid ? preset.name : std::string(), valid}).first;
      }
      if (c->second.valid) here.push_back({c->second.name, folder, key});
    }
    std::sort(here.begin(), here.end(), [](const Entry& a, const Entry& b) {
      return Lower(a.name) != Lower(b.name) ? Lower(a.name) < Lower(b.name) : a.path < b.path;
    });
    out.insert(out.end(), here.begin(), here.end());
  }
  return out;
}

inline std::string Relative(const std::string& absolute) {
  const fs::path p = FromUtf8(absolute);
  if (!Inside(p)) return {};
  return ToUtf8(p.lexically_normal().lexically_relative(Root().lexically_normal()));
}

inline std::string Absolute(const std::string& relative) {
  if (relative.empty()) return {};
  const fs::path p = Root() / FromUtf8(relative);
  return Inside(p) ? ToUtf8(p.lexically_normal()) : std::string();
}

inline bool Exists(const std::string& path) {
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(FromUtf8(path), ec);
}

// A file next to the preset, in "<preset> files/" beside it, or in `folder` (the last folder a file was loaded from):
// how a preset finds its response files after it, or they, moved.
inline std::string Locate(const std::string& saved, const std::string& name, const std::string& presetPath,
                          const std::string& stem, const std::string& folder, bool directory) {
  std::error_code ec;
  const auto there = [&](const fs::path& p) { return directory ? fs::is_directory(p, ec) : fs::is_regular_file(p, ec); };
  if (!saved.empty() && there(FromUtf8(saved))) return saved;
  const std::string file = !name.empty() ? name : ToUtf8(FromUtf8(saved).filename());
  if (file.empty() || file.find_first_of("/\\") != std::string::npos || file == ".." || file == ".") return {};
  std::vector<fs::path> candidates;
  if (!presetPath.empty()) {
    const fs::path dir = FromUtf8(presetPath).parent_path();
    candidates.push_back(dir / FromUtf8(file));
    candidates.push_back(dir / FromUtf8(stem + " files") / FromUtf8(file));
  }
  if (!folder.empty()) candidates.push_back(FromUtf8(folder) / FromUtf8(file));
  for (const fs::path& c : candidates)
    if (there(c)) return ToUtf8(c);
  return {};
}

inline int64_t SizeOf(const std::string& path) {
  std::error_code ec;
  const auto size = fs::file_size(FromUtf8(path), ec);
  return ec ? -1 : static_cast<int64_t>(size);
}

// The .wav files of a folder, sorted without case.
inline std::vector<std::string> WavFiles(const fs::path& folder) {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string file = ToUtf8(it->path().filename());
    if (!file.empty() && file[0] != '.' && Lower(ToUtf8(it->path().extension())) == ".wav" && it->is_regular_file(ec))
      out.push_back(ToUtf8(it->path()));
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return Lower(a) != Lower(b) ? Lower(a) < Lower(b) : a < b;
  });
  return out;
}

}  // namespace user_presets
