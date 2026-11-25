#include "cab/VirtualMic.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace magmora::cab {
namespace {

namespace fs = std::filesystem;
constexpr double kEps = 1e-9;
constexpr auto kPoll = std::chrono::milliseconds(3);
constexpr double kStalledSeconds = 0.030;  // a hand-over the audio thread has not taken by then is replaced

bool isWav(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".wav";
}

// virtualmic.bracket: the two recorded coordinates around x and the fraction between them (clamped at the ends).
void bracket(const std::vector<double>& c, double x, size_t& i0, size_t& i1, double& t) {
  if (x <= c.front()) {
    i0 = i1 = 0;
    t = 0.0;
    return;
  }
  if (x >= c.back()) {
    i0 = i1 = c.size() - 1;
    t = 0.0;
    return;
  }
  const size_t j = static_cast<size_t>(std::upper_bound(c.begin(), c.end(), x) - c.begin());
  i0 = j - 1;
  i1 = j;
  t = (x - c[j - 1]) / (c[j] - c[j - 1]);
}

template <class T>
std::vector<T> sortedUnique(std::vector<T> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// The choice if it is offered, else the value with the most files (ties: the first in sort order).
std::string pick(const std::vector<std::string>& values, const std::string& wanted) {
  if (std::find(values.begin(), values.end(), wanted) != values.end()) return wanted;
  std::map<std::string, int> count;
  for (const std::string& v : values) ++count[v];
  std::string best;
  int most = -1;
  for (const auto& [v, n] : count)
    if (n > most) {
      most = n;
      best = v;
    }
  return best;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the pack index

bool Pack::load(const std::string& folder, const std::string& mappingPath, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  std::error_code ec;
  if (folder.empty() || !fs::is_directory(folder, ec)) return fail("the cabinet folder was not found: " + folder);
  Mapping mapping;
  bool haveMapping = false;
  const fs::path mapPath = mappingPath.empty() ? fs::path(folder) / kMappingFileName : fs::path(mappingPath);
  if (fs::exists(mapPath, ec)) {
    std::ifstream in(mapPath, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string why;
    if (!parseMapping(text.str(), mapping, &why)) return fail(mapPath.string() + ": " + why);
    haveMapping = true;
  } else if (!mappingPath.empty()) {
    return fail("the mapping file was not found: " + mappingPath);
  }
  std::vector<fs::path> paths;
  for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec) && isWav(it->path())) paths.push_back(it->path());
  std::sort(paths.begin(), paths.end());
  std::vector<PackFile> files;
  int skipped = 0;
  for (const fs::path& p : paths) {
    Audio a;
    if (!readWavFile(p.string().c_str(), a, nullptr) || a.frames() == 0 ||
        static_cast<double>(a.frames()) / a.rate > kMaxResponseSeconds) {
      ++skipped;
      continue;
    }
    PackFile f;
    f.path = p.string();
    f.name = p.filename().string();
    f.rate = a.rate;
    f.samples = std::move(a.channels[0]);
    files.push_back(std::move(f));
  }
  if (files.empty()) return fail("no readable WAV files in " + folder);
  return index(std::move(files), haveMapping ? &mapping : nullptr, folder, skipped, error);
}

bool Pack::loadMemory(std::vector<PackFile> files, const std::string& mappingText, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  Mapping mapping;
  const bool haveMapping = !mappingText.empty();
  if (haveMapping) {
    std::string why;
    if (!parseMapping(mappingText, mapping, &why)) return fail("mapping: " + why);
  }
  int skipped = 0;
  std::vector<PackFile> usable;
  for (PackFile& f : files) {
    if (f.samples.empty() || !(f.rate > 0.0) || static_cast<double>(f.samples.size()) / f.rate > kMaxResponseSeconds ||
        !finiteSamples(f.samples)) {
      ++skipped;
      continue;
    }
    if (f.path.empty()) f.path = f.name;
    usable.push_back(std::move(f));
  }
  if (usable.empty()) return fail("no usable responses in the pack");
  std::sort(usable.begin(), usable.end(), [](const PackFile& a, const PackFile& b) { return a.name < b.name; });  // as load
  return index(std::move(usable), haveMapping ? &mapping : nullptr, "", skipped, error);
}

// A path whose order only the file names suggest (letters, "pos N": no mapping, no millimetres) is read as running from
// the cap outwards, which is how most packs number it; some number it the other way, dark to bright (ML Sound Lab's
// A to E). The cap is the bright end of a real cone, so such a path is turned round: per microphone, speaker, variant,
// angle and distance, when its last step is brighter than its first. Brightness: the energy of the first difference
// over the energy, times the rate squared (a mean squared frequency, so files at different rates compare).
namespace {
double brightness(const PackFile& f) {
  double e = 0.0, d = 0.0;
  for (size_t i = 0; i < f.samples.size(); ++i) {
    const double v = f.samples[i];
    e += v * v;
    if (i > 0) {
      const double dv = v - static_cast<double>(f.samples[i - 1]);
      d += dv * dv;
    }
  }
  return e > 0.0 ? d / e * f.rate * f.rate : 0.0;
}

void orientGuessedPaths(std::vector<PackFile>& files) {
  std::map<std::string, std::vector<size_t>> tracks;
  for (size_t k = 0; k < files.size(); ++k) {
    const Position& p = files[k].pos;
    if (!p.hasStep || p.hasX || p.source != "tokens") continue;
    std::ostringstream key;
    key << p.mic << '|' << p.speaker << '|' << p.variant << '|' << p.hasAngle << p.angleDeg << '|' << p.hasDist << p.distMm;
    tracks[key.str()].push_back(k);
  }
  for (auto& [key, ks] : tracks) {
    int lo = files[ks.front()].pos.step, hi = lo;
    for (size_t k : ks) lo = std::min(lo, files[k].pos.step), hi = std::max(hi, files[k].pos.step);
    if (lo == hi) continue;
    double first = 0.0, last = 0.0;
    int nFirst = 0, nLast = 0;
    for (size_t k : ks) {
      if (files[k].pos.step == lo) first += brightness(files[k]), ++nFirst;
      if (files[k].pos.step == hi) last += brightness(files[k]), ++nLast;
    }
    if (last / nLast <= first / nFirst) continue;
    for (size_t k : ks) files[k].pos.step = lo + hi - files[k].pos.step;
  }
}
}  // namespace

// Shared by load and loadMemory: positions from the names, variants, the pack gain, microphones and phase.
bool Pack::index(std::vector<PackFile> files, const Mapping* mapping, const std::string& folder, int skipped,
                 std::string* error) {
  double energy = 0.0;
  for (PackFile& f : files) {
    f.pos = parseName(f.name, mapping);
    double e = 0.0;
    for (float v : f.samples) e += static_cast<double>(v) * v;
    energy += e;
  }
  // Files that the names place at the same position of the same microphone and speaker (no variant read) are
  // versions of it: in different sub-folders (a pack's raw and minimum-phase sets), the sub-folder becomes the
  // variant; in one folder (takes through different preamps), the last word of the name.
  {
    auto positionKey = [](const Position& p) {
      std::ostringstream key;
      key << p.speaker << '|' << p.variant << '|' << p.hasAngle << p.angleDeg << '|' << p.hasX << p.xMm << '|' << p.hasDist
          << p.distMm << '|' << p.hasStep << p.step << '|' << p.hasZone << p.zone;
      return key.str();
    };
    // mics with two files at one position key; acrossFolders: some such pair lies in different folders
    auto collidingMics = [&](bool acrossFolders) {
      std::map<std::string, std::map<std::string, std::string>> seen;  // mic -> key -> folder of the first
      std::set<std::string> out;
      for (const PackFile& f : files) {
        const std::string dir = fs::path(f.path).parent_path().string();
        auto [it, fresh] = seen[f.pos.mic].emplace(positionKey(f.pos), dir);
        if (!fresh && (!acrossFolders || it->second != dir)) out.insert(f.pos.mic);
      }
      return out;
    };
    for (int pass = 0; pass < 2; ++pass) {
      const std::set<std::string> colliding = collidingMics(pass == 0);
      for (PackFile& f : files) {
        if (!colliding.count(f.pos.mic)) continue;
        if (pass == 0) {
          const std::string dir = fs::path(f.path).parent_path().lexically_relative(folder).generic_string();
          if (!f.pos.variant.empty() || dir == ".") continue;
          f.pos.variant = dir;
        } else {
          std::string stem = f.name.substr(0, f.name.find_last_of('.'));
          const size_t cut = stem.find_last_of(" _-");
          const std::string word = cut == std::string::npos ? stem : stem.substr(cut + 1);
          f.pos.variant = f.pos.variant.empty() ? word : f.pos.variant + " " + word;
        }
      }
    }
  }
  orientGuessedPaths(files);
  if (!std::isfinite(energy)) {  // the readers keep samples finite: a pack this loud cannot be played
    if (error) *error = "the pack's responses are not finite";
    return false;
  }
  files_ = std::move(files);
  skipped_ = skipped;
  energy /= static_cast<double>(files_.size());
  gain_ = energy > 0.0 ? 1.0 / std::sqrt(energy) : 1.0;
  std::vector<std::string> mics;
  for (const PackFile& f : files_) mics.push_back(f.pos.mic);
  mics_ = sortedUnique(mics);
  if (!mics_.empty() && mics_.front().empty()) std::rotate(mics_.begin(), mics_.begin() + 1, mics_.end());  // last
  phaseDeclared_ = mapping && !mapping->phase.empty();
  phase_ = !phaseDeclared_ ? morph::PackPhase::Aligned
                           : (mapping->phase == "minimum" ? morph::PackPhase::Minimum
                                                          : (mapping->phase == "raw" ? morph::PackPhase::Raw : morph::PackPhase::Aligned));
  cache_.clear();
  return true;
}

size_t Pack::bytes() const {
  size_t n = 0;
  for (const PackFile& f : files_) n += f.samples.capacity() * sizeof(float) + f.path.capacity() + f.name.capacity();
  std::lock_guard<std::mutex> lock(cacheMutex_);
  for (const auto& [k, v] : cache_) n += v.capacity() * sizeof(float);
  return n;
}

std::vector<std::string> Pack::speakers(int mic) const {
  std::vector<std::string> out;
  if (mic < 0 || mic >= static_cast<int>(mics_.size())) return out;
  for (const PackFile& f : files_)
    if (f.pos.mic == mics_[static_cast<size_t>(mic)]) out.push_back(f.pos.speaker);
  return sortedUnique(out);
}

std::vector<std::string> Pack::variants(int mic, const std::string& speaker) const {
  std::vector<std::string> out;
  if (mic < 0 || mic >= static_cast<int>(mics_.size())) return out;
  for (const PackFile& f : files_)
    if (f.pos.mic == mics_[static_cast<size_t>(mic)] && f.pos.speaker == speaker) out.push_back(f.pos.variant);
  return sortedUnique(out);
}

std::vector<double> Pack::angles(int mic, const std::string& speaker, const std::string& variant) const {
  std::vector<double> out;
  if (mic < 0 || mic >= static_cast<int>(mics_.size())) return out;
  for (const PackFile& f : files_)
    if (f.pos.mic == mics_[static_cast<size_t>(mic)] && f.pos.speaker == speaker && f.pos.variant == variant)
      out.push_back(f.pos.hasAngle ? f.pos.angleDeg : 0.0);
  return sortedUnique(out);
}

std::vector<double> Pack::atRate(size_t file, double rate) const {
  const PackFile& f = files_[file];
  if (f.rate == rate) return std::vector<double>(f.samples.begin(), f.samples.end());
  std::lock_guard<std::mutex> lock(cacheMutex_);
  auto& slot = cache_[{file, rate}];
  if (slot.empty()) slot = resample(f.samples, f.rate, rate);  // kept as float32, as the files are
  return std::vector<double>(slot.begin(), slot.end());
}

std::vector<double> Pack::blend(size_t a, size_t b, double t, bool morphing, double rate) const {
  return morph::interpolate(atRate(a, rate), atRate(b, rate), t, rate, morphing ? morph::Method::Morph : morph::Method::Snap,
                            phase_ == morph::PackPhase::Minimum);
}

std::vector<double> Pack::response(int mic, const std::string& speaker, const std::string& variant, double position,
                                   double distanceMm, double angleDeg, double rate, MicResolved& info) const {
  info = MicResolved{};
  info.on = true;
  if (mic < 0 || mic >= static_cast<int>(mics_.size())) return {};
  info.found = true;
  info.mic = micLabel(mics_[static_cast<size_t>(mic)]);
  const std::string& name = mics_[static_cast<size_t>(mic)];
  std::vector<size_t> pool;
  std::vector<std::string> values;
  for (size_t k = 0; k < files_.size(); ++k)
    if (files_[k].pos.mic == name) {
      pool.push_back(k);
      values.push_back(files_[k].pos.speaker);
    }
  info.speaker = pick(values, speaker);
  auto keep = [&](auto pred) {
    std::vector<size_t> next;
    for (size_t k : pool)
      if (pred(files_[k].pos)) next.push_back(k);
    pool = std::move(next);
  };
  keep([&](const Position& p) { return p.speaker == info.speaker; });
  values.clear();
  for (size_t k : pool) values.push_back(files_[k].pos.variant);
  info.variant = pick(values, variant);
  keep([&](const Position& p) { return p.variant == info.variant; });
  // angle snaps to the nearest recorded one (no file in between can prove an interpolation; CAB.md 12.2)
  std::vector<double> angles;
  for (size_t k : pool) angles.push_back(files_[k].pos.hasAngle ? files_[k].pos.angleDeg : 0.0);
  angles = sortedUnique(angles);
  double angle = angles.front();
  for (double a : angles)
    if (std::fabs(a - angleDeg) < std::fabs(angle - angleDeg)) angle = a;
  info.angleDeg = angle;
  keep([&](const Position& p) { return (p.hasAngle ? p.angleDeg : 0.0) == angle; });

  bool anyX = false, anyStep = false, anyZone = false;
  for (size_t k : pool) {
    anyX = anyX || files_[k].pos.hasX;
    anyStep = anyStep || files_[k].pos.hasStep;
    anyZone = anyZone || files_[k].pos.hasZone;
  }
  auto distOf = [&](size_t k) { return files_[k].pos.hasDist ? files_[k].pos.distMm : 0.0; };

  if (anyX) {  // documented millimetres: across the cone, and off the grille (CAB.md 12.2)
    info.kind = static_cast<int>(TrackKind::Millimetres);
    keep([&](const Position& p) { return p.hasX; });
    std::map<std::pair<double, double>, size_t> grid;
    std::vector<double> xs, ds;
    for (size_t k : pool) {
      grid.emplace(std::make_pair(files_[k].pos.xMm, distOf(k)), k);  // the first in path order
      xs.push_back(files_[k].pos.xMm);
      ds.push_back(distOf(k));
    }
    xs = sortedUnique(xs);
    ds = sortedUnique(ds);
    const double x = std::clamp(position, 0.0, 1.0) * std::max(xs.back(), 0.0);
    size_t i0, i1, j0, j1;
    double tx, td;
    bracket(ds, distanceMm, j0, j1, td);
    info.morphDist = j0 != j1 && ds[j1] - ds[j0] <= kMorphGapMm + kEps;
    if (j0 != j1 && !info.morphDist) {  // snap to the nearer recorded distance
      if (td < 0.5) j1 = j0;
      else j0 = j1;
      td = 0.0;
    }
    bracket(xs, x, i0, i1, tx);
    info.morphX = i0 != i1 && xs[i1] - xs[i0] <= kMorphGapMm + kEps;
    info.xMm = info.morphX ? x : (i0 == i1 || tx < 0.5 ? xs[i0] : xs[i1]);
    info.distMm = info.morphDist ? std::clamp(distanceMm, ds.front(), ds.back()) : ds[j0];
    std::vector<double> rows[2];
    bool have[2] = {false, false};
    for (int r = 0; r < (j0 == j1 ? 1 : 2); ++r) {
      const double d = ds[r == 0 ? j0 : j1];
      const auto a = grid.find({xs[i0], d}), b = grid.find({xs[i1], d});
      if (a == grid.end() && b == grid.end()) continue;
      have[r] = true;
      if (a == grid.end() || b == grid.end()) {  // a missing corner: the recorded one
        const size_t k = (a != grid.end() ? a : b)->second;
        rows[r] = atRate(k, rate);
        info.files.push_back(files_[k].name);
      } else {
        rows[r] = blend(a->second, b->second, tx, info.morphX, rate);
        const bool both = info.morphX && tx > 0.0 && b->second != a->second;
        if (both || !(tx >= 0.5 && i0 != i1)) info.files.push_back(files_[a->second].name);
        if (both || (tx >= 0.5 && i0 != i1)) info.files.push_back(files_[b->second].name);
      }
    }
    if (j0 == j1 || !have[0] || !have[1]) return have[0] ? rows[0] : rows[1];
    return morph::interpolate(rows[0], rows[1], td, rate, morph::Method::Morph, phase_ == morph::PackPhase::Minimum);
  }

  // an ordered path (letters, "pos N"), zone words, or files without a position: one dimension, in list order; the
  // grille distance (if documented) snaps
  std::vector<double> ds;
  for (size_t k : pool) ds.push_back(distOf(k));
  ds = sortedUnique(ds);
  double dist = ds.front();
  for (double d : ds)
    if (std::fabs(d - distanceMm) < std::fabs(dist - distanceMm)) dist = d;
  {
    std::vector<size_t> next;
    for (size_t k : pool)
      if (distOf(k) == dist) next.push_back(k);
    pool = std::move(next);
  }
  info.distMm = dist;
  TrackKind kind = anyStep ? TrackKind::Path : (anyZone ? TrackKind::Zones : TrackKind::Files);
  info.kind = static_cast<int>(kind);
  if (kind == TrackKind::Path) keep([&](const Position& p) { return p.hasStep; });
  else if (kind == TrackKind::Zones) keep([&](const Position& p) { return p.hasZone; });
  std::stable_sort(pool.begin(), pool.end(), [&](size_t a, size_t b) {
    const Position &pa = files_[a].pos, &pb = files_[b].pos;
    if (kind == TrackKind::Path && pa.step != pb.step) return pa.step < pb.step;
    if (kind == TrackKind::Zones && pa.zone != pb.zone) return pa.zone < pb.zone;
    return files_[a].name < files_[b].name;
  });
  if (kind == TrackKind::Path)  // one file per step
    pool.erase(std::unique(pool.begin(), pool.end(), [&](size_t a, size_t b) { return files_[a].pos.step == files_[b].pos.step; }),
               pool.end());
  const double index = std::clamp(position, 0.0, 1.0) * static_cast<double>(pool.size() - 1);
  const size_t a = std::min(static_cast<size_t>(std::floor(index)), pool.size() - 1), b = std::min(a + 1, pool.size() - 1);
  const double t = index - static_cast<double>(a);
  // adjacent steps of an ordered path morph; zones and unplaced files snap
  info.morphX = kind == TrackKind::Path && a != b && t > 0.0;
  info.pathIndex = info.morphX ? index : static_cast<double>(t < 0.5 ? a : b);
  info.files.push_back(files_[pool[a]].name);
  if (b != a && t > 0.0) info.files.push_back(files_[pool[b]].name);
  return blend(pool[a], pool[b], t, info.morphX, rate);
}

// ------------------------------------------------------------------------------------------------ the block

VirtualMic::~VirtualMic() { stop(); }

void VirtualMic::prepare(double hostRate, int maxBlockSize, bool stereoInput, const std::atomic<double>* controls,
                         const std::atomic<double>* sources) {
  stop();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = hostRate;
    maxBlock_ = maxBlockSize;
    controls_ = controls;
    sources_ = sources;
    fileAtRateOf_[0] = fileAtRateOf_[1] = nullptr;  // the rate may have changed
    ir_.setNormalize(false);  // the pack's own gain instead: positions and microphones keep their relative levels
    ir_.prepare(hostRate, maxBlockSize, stereoInput);
    built_ = false;
  }
  start();
}

void VirtualMic::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (worker_.joinable() || !controls_) return;
  quit_ = false;
  worker_ = std::thread([this] { run(); });
}

void VirtualMic::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_.joinable()) return;
    quit_ = true;
  }
  wake_.notify_all();
  worker_.join();
}

void VirtualMic::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!quit_) {
    wake_.wait_for(lock, kPoll);
    if (quit_) break;
    ir_.collect();
    buildIfChanged(lock);
  }
}

bool VirtualMic::loadPack(const std::string& folder, const std::string& mappingPath, std::string* error) {
  auto pack = std::make_shared<Pack>();
  std::string why;
  const bool ok = pack->load(folder, mappingPath, &why);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
      pack_ = std::move(pack);
      ++generation_;
      built_ = false;
      status_.error.clear();
      status_.folder = folder;
    } else {
      status_.error = why;
    }
  }
  wake_.notify_all();
  if (!ok && error) *error = why;
  return ok;
}

bool VirtualMic::loadPackMemory(std::vector<PackFile> files, const std::string& mappingText, const std::string& reference,
                                std::string* error) {
  auto pack = std::make_shared<Pack>();
  std::string why;
  const bool ok = pack->loadMemory(std::move(files), mappingText, &why);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
      pack_ = std::move(pack);
      ++generation_;
      built_ = false;
      status_.error.clear();
      status_.folder = reference;
    } else {
      status_.error = why;
    }
  }
  wake_.notify_all();
  if (!ok && error) *error = why;
  return ok;
}

void VirtualMic::clearPack() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pack_.reset();
    ++generation_;
    built_ = false;
    status_ = CabinetStatus{};
  }
  wake_.notify_all();
}

bool VirtualMic::packLoaded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pack_ != nullptr;
}

std::shared_ptr<const Pack> VirtualMic::pack() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pack_;
}

void VirtualMic::setSource(int mic, const std::string& speaker, const std::string& variant) {
  if (mic < 0 || mic > 1) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    speaker_[mic] = speaker;
    variant_[mic] = variant;
    ++generation_;
  }
  wake_.notify_all();
}

std::string VirtualMic::speaker(int mic) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mic == 0 || mic == 1 ? speaker_[mic] : std::string();
}

std::string VirtualMic::variant(int mic) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mic == 0 || mic == 1 ? variant_[mic] : std::string();
}

CabinetStatus VirtualMic::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

double VirtualMic::responseSeconds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rate_ > 0.0 ? static_cast<double>(std::max(lastLeft_.size(), lastRight_.size())) / rate_ : 0.0;
}

void VirtualMic::lastResponse(std::vector<double>& left, std::vector<double>& right) const {
  std::lock_guard<std::mutex> lock(mutex_);
  left = lastLeft_;
  right = lastRight_;
}

std::vector<double> VirtualMic::currentKey() const {  // mutex held
  std::vector<double> key;
  for (int k = 0; k < kCtlCount; ++k) key.push_back(controls_[k].load(std::memory_order_relaxed));
  for (int m = 0; m < 2; ++m) key.push_back(sources_ ? sources_[m].load(std::memory_order_relaxed) : 0.0);
  key.push_back(static_cast<double>(generation_));
  return key;
}

bool VirtualMic::waitUntilBuilt(double timeoutSeconds) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
  while (std::chrono::steady_clock::now() < until) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (controls_ && built_ && currentKey() == lastKey_) return true;
    }
    wake_.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

bool VirtualMic::setFile(int slot, const Audio& audio, const std::string& name, std::string* error) {
  if (slot < 0 || slot > 1) return false;
  if (audio.channels.empty() || audio.frames() == 0 || !(audio.rate > 0.0)) {
    if (error) *error = "empty response";
    return false;
  }
  if (static_cast<double>(audio.frames()) / audio.rate > kMaxResponseSeconds) {
    if (error) *error = "response longer than 10 s";
    return false;
  }
  for (const auto& c : audio.channels)
    if (!finiteSamples(c)) {
      if (error) *error = "the response has samples that are not finite";
      return false;
    }
  auto a = std::make_shared<Audio>(audio);
  if (a->channels.size() > 2) a->channels.resize(2);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    file_[slot] = std::move(a);
    fileName_[slot] = name;
    ++generation_;
  }
  wake_.notify_all();
  return true;
}

void VirtualMic::clearFile(int slot) {
  if (slot < 0 || slot > 1) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    file_[slot].reset();
    fileName_[slot].clear();
    ++generation_;
  }
  wake_.notify_all();
}

bool VirtualMic::fileLoaded(int slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (slot == 0 || slot == 1) && file_[slot] != nullptr;
}
double VirtualMic::fileSeconds(int slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (slot == 0 || slot == 1) && file_[slot] ? static_cast<double>(file_[slot]->frames()) / file_[slot]->rate : 0.0;
}
int VirtualMic::fileChannels(int slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (slot == 0 || slot == 1) && file_[slot] ? static_cast<int>(file_[slot]->channels.size()) : 0;
}
void VirtualMic::setFileNormalize(int slot, bool on) {
  if (slot < 0 || slot > 1) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fileNormalize_[slot] == on) return;
    fileNormalize_[slot] = on;
    ++generation_;
  }
  wake_.notify_all();
}
bool VirtualMic::fileNormalize(int slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (slot == 0 || slot == 1) ? fileNormalize_[slot] : true;
}
void VirtualMic::setFileRaw(int slot, bool raw) {
  if (slot < 0 || slot > 1) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fileRaw_[slot] == raw) return;
    fileRaw_[slot] = raw;
    ++generation_;
  }
  wake_.notify_all();
}
bool VirtualMic::fileRaw(int slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (slot == 0 || slot == 1) && fileRaw_[slot];
}

VirtualMic::Settings VirtualMic::settingsFrom(const double* v, const double* sources) {
  Settings s;
  for (int m = 0; m < 2; ++m) {
    const int o = m == 0 ? 0 : kCtlMic2On - kCtlMic1On;
    s.on[m] = v[kCtlMic1On + o] >= 0.5;
    s.source[m] = sources && sources[m] >= 0.5 ? 1 : 0;
    s.type[m] = static_cast<int>(std::lround(v[kCtlMic1Type + o]));
    s.position[m] = v[kCtlMic1Position + o];
    s.distanceMm[m] = v[kCtlMic1Distance + o];
    s.angleDeg[m] = v[kCtlMic1Angle + o];
    s.mix[m].levelDb = v[kCtlMic1Level + o];
    s.mix[m].pan = v[kCtlMic1Pan + o];
    s.mix[m].invert = v[kCtlMic1Polarity + o] >= 0.5;
  }
  s.align = v[kCtlAlign] >= 0.5;
  s.offsetMm = v[kCtlOffset];
  return s;
}

namespace {
void shiftInPlace(std::vector<double>& b, double move, size_t n) {  // as virtualmic.two_mics moves microphone 2
  if (std::fabs(move) <= 1e-9) return;
  if (move > 0.0) {
    std::vector<double> padded(b);
    padded.resize(n + 64, 0.0);
    b = morph::shift(padded, move);
    b.resize(n);
  } else {
    b = morph::shift(b, move);
  }
}
}  // namespace

morph::TwoMics VirtualMic::build(const Pack* pack, const Settings& s, const std::string* speakers,
                                 const std::string* variants, const SlotFile* files, double rate, CabinetStatus& status,
                                 bool packGain) {
  // each slot: one response (a pack microphone) or one or two channels (a file)
  std::vector<double> h[2][2];
  bool isFile[2] = {false, false}, raw[2] = {false, false};
  for (int m = 0; m < 2; ++m) {
    status.mic[m] = CabinetMicStatus{};
    status.mic[m].source = s.source[m];
    if (!s.on[m]) continue;
    if (s.source[m] == 1) {
      status.mic[m].on = true;
      const SlotFile& f = files[m];
      if (!f.channels || f.channels->empty()) continue;
      double energy = 0.0;
      for (const auto& c : *f.channels) {
        double e = 0.0;
        for (double v : c) e += v * v;
        energy = std::max(energy, e);
      }
      if (!std::isfinite(energy)) continue;  // refused on loading; never played
      status.mic[m].found = true;
      status.mic[m].mic = f.name;
      status.mic[m].files.push_back(f.name);
      isFile[m] = true;
      raw[m] = f.raw;
      const double g = f.normalize && energy > 0.0 ? 1.0 / std::sqrt(energy) : 1.0;
      for (size_t c = 0; c < f.channels->size() && c < 2; ++c) {
        h[m][c] = (*f.channels)[c];
        for (double& v : h[m][c]) v *= g;
      }
    } else if (pack) {
      h[m][0] = pack->response(s.type[m], speakers[m], variants[m], s.position[m], s.distanceMm[m], s.angleDeg[m], rate,
                               status.mic[m]);
      raw[m] = pack->phase() == morph::PackPhase::Raw;
      if (packGain)
        for (double& v : h[m][0]) v *= pack->gain();
    } else {
      status.mic[m].on = true;
    }
  }
  morph::TwoMics out;
  const bool have[2] = {!h[0][0].empty(), !h[1][0].empty()};
  if (have[0] && have[1] && !isFile[0] && !isFile[1]) {  // two pack microphones: virtualmic.two_mics exactly
    out = morph::twoMics(h[0][0], h[1][0], rate, s.mix[0], s.mix[1], s.align, s.offsetMm, pack->phase());
  } else if ((have[0] || have[1]) && !isFile[have[0] ? 0 : 1] && !(have[0] && have[1])) {  // one pack microphone
    const int m = have[0] ? 0 : 1;
    out = morph::twoMics(h[m][0], {}, rate, s.mix[m], s.mix[m], s.align, 0.0, pack->phase());
  } else if (have[0] || have[1]) {
    // a file in a slot: the same constant-power pan as a pack microphone (a stereo file keeps its sides, left by the
    // left gain, right by the right). The second slot moves by the measured delay (both declared raw) and the offset,
    // as microphone 2 does.
    size_t n = 0;
    for (int m = 0; m < 2; ++m) n = std::max({n, h[m][0].size(), h[m][1].size()});
    std::vector<double> side[2][2];  // slot, channel
    for (int m = 0; m < 2; ++m) {
      if (!have[m]) continue;
      double l, r;
      morph::panGains(s.mix[m].pan, l, r);
      const double g = std::pow(10.0, s.mix[m].levelDb / 20.0) * (s.mix[m].invert ? -1.0 : 1.0);
      const std::vector<double>& a = h[m][0];
      const std::vector<double>& b = h[m][1].empty() ? h[m][0] : h[m][1];
      for (int c = 0; c < 2; ++c) {
        const std::vector<double>& src = c == 0 ? a : b;
        side[m][c].assign(n, 0.0);
        for (size_t k = 0; k < src.size(); ++k) side[m][c][k] = g * (c == 0 ? l : r) * src[k];
      }
    }
    if (have[0] && have[1]) {
      std::vector<double> a(h[0][0]), b(h[1][0]);
      a.resize(n, 0.0);
      b.resize(n, 0.0);
      if (raw[0] && raw[1]) morph::relativeDelay(b, a, rate, out.measuredDelay, out.detectedPolarity);
      out.moved = ((s.align && raw[0] && raw[1]) ? -out.measuredDelay : 0.0) + s.offsetMm * 1e-3 / 343.0 * rate;
      for (int c = 0; c < 2; ++c) shiftInPlace(side[1][c], out.moved, n);
    }
    out.left.assign(n, 0.0);
    out.right.assign(n, 0.0);
    for (int m = 0; m < 2; ++m)
      if (have[m])
        for (size_t k = 0; k < n; ++k) {
          out.left[k] += side[m][0][k];
          out.right[k] += side[m][1][k];
        }
  } else {
    out.left.assign(1, 0.0);  // no source: silence (a cabinet with no microphone in front of it)
    out.right.assign(1, 0.0);
  }
  status.measuredDelaySamples = out.measuredDelay;
  status.detectedPolarity = out.detectedPolarity;
  return out;
}

// Worker. Called and returns with the mutex held; the build itself runs without it, so that the main thread's calls
// (state, status, a new file) never wait for a build. Inputs are copied under the lock, the result is published under
// it. ir_ and fileAtRate_ are the worker's alone while it runs (prepare stops it first).
bool VirtualMic::buildIfChanged(std::unique_lock<std::mutex>& lock) {
  if (!controls_ || rate_ <= 0.0) return false;
  std::vector<double> key = currentKey();
  if (built_ && key == lastKey_) return false;
  if (!pack_ && !file_[0] && !file_[1]) {  // nothing to play from: back to the dry signal
    if (built_ || ir_.hasResponse()) ir_.clear();
    lastLeft_.clear();
    lastRight_.clear();
    lastKey_ = std::move(key);
    built_ = true;
    return false;
  }
  if (ir_.handoverPending() &&
      std::chrono::duration<double>(std::chrono::steady_clock::now() - handedOver_).count() < kStalledSeconds)
    return false;  // at most one new response per crossfade: the audio thread takes it, then the latest is built
  const auto t0 = std::chrono::steady_clock::now();
  double v[kCtlCount], src[2] = {0.0, 0.0};
  for (int k = 0; k < kCtlCount; ++k) v[k] = key[static_cast<size_t>(k)];
  for (int m = 0; m < 2; ++m) src[m] = key[static_cast<size_t>(kCtlCount + m)];
  const Settings s = settingsFrom(v, src);
  // the inputs, as they are now
  const std::shared_ptr<const Pack> pack = pack_;
  std::shared_ptr<const Audio> file[2] = {file_[0], file_[1]};
  const bool normalize[2] = {fileNormalize_[0], fileNormalize_[1]}, raw[2] = {fileRaw_[0], fileRaw_[1]};
  const std::string name[2] = {fileName_[0], fileName_[1]};
  const std::string speakers[2] = {speaker_[0], speaker_[1]}, variants[2] = {variant_[0], variant_[1]};
  const double rate = rate_;
  CabinetStatus st = status_;

  lock.unlock();
  SlotFile files[2];
  for (int m = 0; m < 2; ++m) {
    if (!file[m]) continue;
    if (fileAtRateOf_[m] != file[m].get()) {  // resampled once per file and rate
      fileAtRate_[m].clear();
      for (const auto& c : file[m]->channels) {
        const std::vector<float> r = resample(c, file[m]->rate, rate);
        fileAtRate_[m].emplace_back(r.begin(), r.end());
      }
      fileAtRateOf_[m] = file[m].get();
    }
    files[m] = SlotFile{&fileAtRate_[m], normalize[m], raw[m], name[m]};
  }
  morph::TwoMics r = build(pack.get(), s, speakers, variants, files, rate, st);
  Audio a;
  a.rate = rate;
  // both sides the same (one microphone, or both panned alike; the centre's cos and sin differ in the last bit): one
  // convolution instead of two
  double peak = 0.0, diff = 0.0;
  for (size_t k = 0; k < r.left.size(); ++k) {
    peak = std::max(peak, std::fabs(r.left[k]));
    diff = std::max(diff, std::fabs(r.left[k] - r.right[k]));
  }
  const bool mono = diff <= 1e-12 * peak;
  for (int c = 0; c < (mono ? 1 : 2); ++c) {
    const std::vector<double>& from = c == 0 ? r.left : r.right;
    std::vector<float> narrow(from.size());
    for (size_t k = 0; k < from.size(); ++k) narrow[k] = narrowSample(from[k]);  // a file played loud may pass float range
    a.channels.push_back(std::move(narrow));
  }
  ir_.load(a);
  const auto handedOver = std::chrono::steady_clock::now();
  st.packLoaded = pack != nullptr;
  st.files = pack ? static_cast<int>(pack->files().size()) : 0;
  st.skipped = pack ? pack->skipped() : 0;
  st.phase = !pack ? "" : (pack->phase() == morph::PackPhase::Minimum ? "minimum" : (pack->phase() == morph::PackPhase::Raw ? "raw" : "aligned"));
  st.lastBuildMs = std::chrono::duration<double, std::milli>(handedOver - t0).count();
  lock.lock();

  // Published. What changed meanwhile (a pack load, an error) is kept: only what this build decides is taken over.
  handedOver_ = handedOver;
  st.folder = status_.folder;
  st.error = status_.error;
  st.builds = status_.builds + 1;
  status_ = std::move(st);
  lastLeft_ = std::move(r.left);
  lastRight_ = std::move(r.right);
  lastKey_ = std::move(key);  // the key it was built for: if the controls moved meanwhile, the next turn builds again
  built_ = true;
  return true;
}

void VirtualMic::process(float* left, float* right, int frames) {
  ir_.setControls(controls_ && controls_[kCtlOn].load(std::memory_order_relaxed) >= 0.5, 0.0);
  ir_.process(left, right, frames);
}

void VirtualMic::process(double* left, double* right, int frames) {
  ir_.setControls(controls_ && controls_[kCtlOn].load(std::memory_order_relaxed) >= 0.5, 0.0);
  ir_.process(left, right, frames);
}

}  // namespace magmora::cab
