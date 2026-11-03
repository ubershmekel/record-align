#include "sync/sync.h"

#include <pffft/pffft.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kEnvelopeRate = 100.0;      // Hops per second in the onset envelope.
constexpr double kCoarseSegmentSeconds = 300.0;
constexpr int kAnalysisRate = 16000;
constexpr double kWindowSeconds = 2.0;
constexpr double kAnchorSpacingSeconds = 15.0;
constexpr int kMinAnchors = 3;
constexpr int kMaxAnchors = 32;
constexpr int kExtraAnchors = 6;
constexpr double kShortWindowSeconds = 0.4;
constexpr double kMinStrength = 7.0;         // Pure noise peaks at about 4.
constexpr double kStrongStrength = 15.0;
constexpr double kInlierToleranceSeconds = 0.0015;
constexpr double kMaxDrift = 500e-6;         // Far beyond any real audio clock.
constexpr double kPhatExponent = 0.75;
constexpr double kPriorRadiusSeconds = 1.0;
constexpr double kGoodConfidence = 0.5;
constexpr double kWeakConfidence = 0.2;

double saturate(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

double median_of(std::vector<float> values) {
  if (values.empty()) return 0.0;
  auto mid = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), mid, values.end());
  return *mid;
}

// Real FFT. Spectra are in pffft's ordered form: [dc, nyquist, re1, im1, re2, im2...].
class RealFft {
 public:
  explicit RealFft(int size) : size_(size) {
    setup_ = pffft_new_setup(size, PFFFT_REAL);
    work_ = static_cast<float*>(pffft_aligned_malloc(size * sizeof(float)));
  }
  ~RealFft() {
    pffft_aligned_free(work_);
    pffft_destroy_setup(setup_);
  }
  RealFft(const RealFft&) = delete;
  RealFft& operator=(const RealFft&) = delete;

  int size() const { return size_; }
  void forward(const float* in, float* out) { pffft_transform_ordered(setup_, in, out, work_, PFFFT_FORWARD); }
  void inverse(const float* in, float* out) { pffft_transform_ordered(setup_, in, out, work_, PFFFT_BACKWARD); }

 private:
  int size_;
  PFFFT_Setup* setup_;
  float* work_;
};

class AlignedFloats {
 public:
  explicit AlignedFloats(int size) : size_(size) {
    data_ = static_cast<float*>(pffft_aligned_malloc(size * sizeof(float)));
    std::fill(data_, data_ + size, 0.0f);
  }
  ~AlignedFloats() { pffft_aligned_free(data_); }
  AlignedFloats(const AlignedFloats&) = delete;
  AlignedFloats& operator=(const AlignedFloats&) = delete;
  float* data() { return data_; }
  float& operator[](int i) { return data_[i]; }
  void clear() { std::fill(data_, data_ + size_, 0.0f); }

 private:
  int size_;
  float* data_;
};

int next_power_of_two(int64_t n) {
  int size = 32;
  while (size < n) size *= 2;
  return size;
}

// Rectified change in log energy of the differentiated signal, per 10 ms. This
// peaks on sudden sounds and is insensitive to each microphone's overall level.
std::vector<float> onset_envelope(const AudioView& v) {
  const int hop = std::max(1, int(v.sample_rate / kEnvelopeRate));
  const int64_t hops = int64_t(v.frames / hop);
  std::vector<float> energy(size_t(std::max<int64_t>(hops, 0)));
  double previous = 0.0;
  const float* s = v.samples;
  for (int64_t h = 0; h < hops; h++) {
    double sum = 0.0;
    for (int i = 0; i < hop; i++) {
      double mono = 0.0;
      for (int c = 0; c < v.channels; c++) mono += s[c];
      s += v.channels;
      double diff = mono - previous;
      previous = mono;
      sum += diff * diff;
    }
    energy[size_t(h)] = float(sum);
  }
  std::vector<float> nonzero;
  nonzero.reserve(energy.size());
  for (float e : energy) {
    if (e > 0.0f) nonzero.push_back(e);
  }
  std::vector<float> onset(energy.size(), 0.0f);
  if (nonzero.empty()) return onset;
  const double floor_energy = std::max(median_of(std::move(nonzero)), 1e-12);
  float last = 0.0f;
  for (size_t i = 0; i < energy.size(); i++) {
    float level = float(std::log1p(energy[i] / floor_energy));
    onset[i] = std::max(0.0f, level - last);
    last = level;
  }
  return onset;
}

struct CoarseCandidate {
  double lag_seconds = 0.0;
  double ref_seconds = 0.0;
  double strength = 0.0;
};

// Slides pieces of the reference envelope across the whole other envelope. Each
// piece is short enough that clock drift can't smear the correlation peak.
std::vector<CoarseCandidate> find_coarse_candidates(const std::vector<float>& ref, const std::vector<float>& other) {
  std::vector<CoarseCandidate> all;
  if (ref.empty() || other.empty()) return all;

  const int segment = std::min<int>(int(ref.size()), int(kCoarseSegmentSeconds * kEnvelopeRate));
  const int fft_size = next_power_of_two(int64_t(segment) + other.size());
  RealFft fft(fft_size);
  AlignedFloats input(fft_size), other_spectrum(fft_size), ref_spectrum(fft_size), product(fft_size), corr(fft_size);

  double other_mean = 0.0;
  for (float x : other) other_mean += x;
  other_mean /= other.size();
  for (size_t i = 0; i < other.size(); i++) input[int(i)] = float(other[i] - other_mean);
  fft.forward(input.data(), other_spectrum.data());

  const int stride = std::max(1, segment / 2);
  int count = 1 + std::max(0, int(ref.size()) - segment) / stride;
  count = std::min(count, 16);
  const int step = count > 1 ? (int(ref.size()) - segment) / (count - 1) : 0;

  for (int n = 0; n < count; n++) {
    const int start = n * step;
    double mean = 0.0;
    for (int i = 0; i < segment; i++) mean += ref[size_t(start + i)];
    mean /= segment;
    input.clear();
    for (int i = 0; i < segment; i++) input[i] = float(ref[size_t(start + i)] - mean);
    fft.forward(input.data(), ref_spectrum.data());

    product[0] = ref_spectrum[0] * other_spectrum[0];
    product[1] = ref_spectrum[1] * other_spectrum[1];
    for (int k = 2; k < fft_size; k += 2) {
      float ar = ref_spectrum[k], ai = -ref_spectrum[k + 1];
      float br = other_spectrum[k], bi = other_spectrum[k + 1];
      product[k] = ar * br - ai * bi;
      product[k + 1] = ar * bi + ai * br;
    }
    fft.inverse(product.data(), corr.data());

    std::vector<float> magnitudes(fft_size);
    for (int i = 0; i < fft_size; i++) magnitudes[i] = corr[i];
    const double center = median_of(magnitudes);
    for (float& m : magnitudes) m = float(std::fabs(m - center));
    const double spread = 1.4826 * median_of(std::move(magnitudes)) + 1e-9;

    // Keep a few separated peaks: repeated claps can make a wrong lag score well.
    constexpr int kPeaks = 3;
    const int exclusion = int(0.2 * kEnvelopeRate);
    std::vector<int> taken;
    for (int p = 0; p < kPeaks; p++) {
      int best = -1;
      float best_value = -1e30f;
      for (int i = 0; i < fft_size; i++) {
        if (corr[i] <= best_value) continue;
        bool excluded = false;
        for (int t : taken) {
          int d = std::abs(i - t);
          d = std::min(d, fft_size - d);
          if (d < exclusion) excluded = true;
        }
        if (!excluded) {
          best = i;
          best_value = corr[i];
        }
      }
      if (best < 0) break;
      taken.push_back(best);
      int shift = best > fft_size / 2 ? best - fft_size : best;
      CoarseCandidate c;
      c.lag_seconds = (shift - start) / kEnvelopeRate;
      c.ref_seconds = (start + segment * 0.5) / kEnvelopeRate;
      c.strength = (best_value - center) / spread;
      all.push_back(c);
    }
  }

  std::sort(all.begin(), all.end(),
            [](const CoarseCandidate& a, const CoarseCandidate& b) { return a.strength > b.strength; });
  std::vector<CoarseCandidate> unique;
  for (const auto& c : all) {
    bool duplicate = false;
    for (const auto& u : unique) {
      if (std::fabs(u.lag_seconds - c.lag_seconds) < 0.1) duplicate = true;
    }
    if (!duplicate) unique.push_back(c);
    if (unique.size() == 6) break;
  }
  return unique;
}

// Averages channels and box-filters down to the analysis rate. Reads directly
// from the file mapping. Returns the time of the first output sample's center.
double extract_mono(const AudioView& v, double start_seconds, int count, int rate, float* out) {
  const double step = double(v.sample_rate) / rate;
  const double first = std::round(start_seconds * v.sample_rate);
  for (int i = 0; i < count; i++) {
    int64_t a = int64_t(first + i * step);
    int64_t b = int64_t(first + (i + 1) * step);
    if (b <= a) b = a + 1;
    double sum = 0.0;
    int64_t used = 0;
    for (int64_t f = std::max<int64_t>(a, 0); f < b && f < int64_t(v.frames); f++) {
      const float* frame = v.samples + f * v.channels;
      for (int c = 0; c < v.channels; c++) sum += frame[c];
      used++;
    }
    out[i] = used ? float(sum / (v.channels * (b - a))) : 0.0f;
  }
  return first / v.sample_rate + (step - 1.0) / (2.0 * v.sample_rate);
}

struct AnchorPlace {
  double seconds = 0.0;
  double window_seconds = kWindowSeconds;
};

// Long windows spread across the recording suit speech. Short windows on the
// sharpest sounds keep separate claps apart, so each one counts as its own evidence.
std::vector<AnchorPlace> choose_anchor_times(const std::vector<float>& envelope, double duration) {
  std::vector<AnchorPlace> times;
  if (duration <= 0.0 || envelope.empty()) return times;
  int count = int(duration / kAnchorSpacingSeconds);
  count = std::max(kMinAnchors, std::min(kMaxAnchors, count));
  const size_t size = envelope.size();
  for (int n = 0; n < count; n++) {
    size_t a = size * n / count;
    size_t b = size * (n + 1) / count;
    if (b <= a) continue;
    size_t best = size_t(std::max_element(envelope.begin() + a, envelope.begin() + b) - envelope.begin());
    if (envelope[best] <= 0.0f) continue;
    times.push_back({(best + 0.5) / kEnvelopeRate, kWindowSeconds});
  }

  std::vector<size_t> order(size);
  for (size_t i = 0; i < size; i++) order[i] = i;
  const size_t keep = std::min<size_t>(size, 2000);
  std::partial_sort(order.begin(), order.begin() + keep, order.end(),
                    [&](size_t a, size_t b) { return envelope[a] > envelope[b]; });
  std::vector<double> sharp;
  for (size_t i = 0; i < keep && int(sharp.size()) < kExtraAnchors; i++) {
    if (envelope[order[i]] <= 0.0f) break;
    const double t = (order[i] + 0.5) / kEnvelopeRate;
    bool close = false;
    for (double existing : sharp) {
      if (std::fabs(existing - t) < kShortWindowSeconds) close = true;
    }
    if (!close) sharp.push_back(t);
  }
  for (double t : sharp) times.push_back({t, kShortWindowSeconds});
  return times;
}

// A hypothesis about where the other track's matching sound will be.
struct SearchPlan {
  double lag_at_center = 0.0;
  double center_seconds = 0.0;
  double drift = 0.0;
  double base_radius = 1.0;
  double radius_per_second = 0.0;
};

struct AnchorSearch {
  const AudioView& ref;
  const AudioView& other;
  int rate;

  bool measure(const AnchorPlace& place, const SearchPlan& plan, SyncAnchor* out) const {
    const double t = place.seconds;
    const double elapsed = std::fabs(t - plan.center_seconds);
    const double radius = std::min(3.0, plan.base_radius + plan.radius_per_second * elapsed);
    const double predicted = t + plan.lag_at_center + plan.drift * (t - plan.center_seconds);

    const int window = int(place.window_seconds * rate);
    const int search = int(2.0 * radius * rate);
    const int span = window + search;
    const int fft_size = next_power_of_two(span + window);

    AlignedFloats a(fft_size), b(fft_size), A(fft_size), B(fft_size), C(fft_size), corr(fft_size);
    const double ref_start = extract_mono(ref, t - place.window_seconds * 0.5, window, rate, a.data());
    const double other_start =
        extract_mono(other, predicted - place.window_seconds * 0.5 - radius, span, rate, b.data());

    double ref_energy = 0.0;
    double other_energy = 0.0;
    for (int i = 0; i < window; i++) {
      float w = 0.5f - 0.5f * float(std::cos(2.0 * 3.14159265358979 * i / window));
      a[i] *= w;
      ref_energy += a[i] * a[i];
    }
    for (int i = 0; i < span; i++) other_energy += b[i] * b[i];
    if (ref_energy < 1e-12 || other_energy < 1e-12) return false;

    RealFft fft(fft_size);
    fft.forward(a.data(), A.data());
    fft.forward(b.data(), B.data());

    // Partly whitened correlation sharpens the peak and copes with different
    // microphone tone. Very low frequencies carry mostly hum and rumble.
    const double hz_per_bin = double(rate) / fft_size;
    C[0] = 0.0f;
    C[1] = 0.0f;
    for (int k = 2; k < fft_size; k += 2) {
      double frequency = (k / 2) * hz_per_bin;
      if (frequency < 100.0 || frequency > 0.45 * rate) {
        C[k] = 0.0f;
        C[k + 1] = 0.0f;
        continue;
      }
      double ar = A[k], ai = -A[k + 1];
      double br = B[k], bi = B[k + 1];
      double re = ar * br - ai * bi;
      double im = ar * bi + ai * br;
      double scale = std::pow(re * re + im * im + 1e-30, -0.5 * kPhatExponent);
      C[k] = float(re * scale);
      C[k + 1] = float(im * scale);
    }
    fft.inverse(C.data(), corr.data());

    const int shifts = std::max(1, search);
    std::vector<float> abs_values(shifts);
    int best = 0;
    float best_value = 0.0f;
    for (int i = 0; i < shifts; i++) {
      abs_values[i] = std::fabs(corr[i]);
      if (abs_values[i] > best_value) {
        best_value = abs_values[i];
        best = i;
      }
    }
    const double floor_value = 1.4826 * median_of(std::move(abs_values)) + 1e-12;

    double fraction = 0.0;
    if (best > 0 && best + 1 < shifts) {
      double y0 = std::fabs(corr[best - 1]), y1 = best_value, y2 = std::fabs(corr[best + 1]);
      double denominator = y0 - 2.0 * y1 + y2;
      if (std::fabs(denominator) > 1e-12) fraction = 0.5 * (y0 - y2) / denominator;
      fraction = std::max(-0.5, std::min(0.5, fraction));
    }

    out->ref_seconds = t;
    out->window_seconds = place.window_seconds;
    out->lag_seconds = (other_start + (best + fraction) / rate) - ref_start;
    out->strength = best_value / floor_value;
    out->inlier = false;
    return true;
  }
};

struct LineFit {
  double offset = 0.0;
  double drift = 0.0;
  int inliers = 0;
  double rms = 0.0;
  double span = 0.0;
  bool drift_measured = false;
};

double residual(const SyncAnchor& a, double offset, double drift) {
  return a.lag_seconds - (offset + drift * a.ref_seconds);
}

// Tries a line through every pair of strong anchors, keeps the one most anchors
// agree with, then refines it by least squares on those anchors.
LineFit fit_anchors(std::vector<SyncAnchor>* anchors, double duration, const TimeMap* prior) {
  LineFit best;
  std::vector<const SyncAnchor*> usable;
  for (const auto& a : *anchors) {
    if (a.strength >= kMinStrength) usable.push_back(&a);
  }
  if (usable.empty()) return best;

  double best_score = -1.0;
  const double fallback_drift = prior ? prior->drift : 0.0;
  auto consider = [&](double offset, double drift) {
    if (std::fabs(drift) > kMaxDrift) return;
    int inliers = 0;
    double score = 0.0;
    for (const SyncAnchor* a : usable) {
      double r = std::fabs(residual(*a, offset, drift));
      if (r <= kInlierToleranceSeconds) {
        inliers++;
        score += 1.0 + (1.0 - r / kInlierToleranceSeconds);  // Prefer tighter fits on ties.
      }
    }
    if (score > best_score) {
      best_score = score;
      best.offset = offset;
      best.drift = drift;
      best.inliers = inliers;
    }
  };
  for (size_t i = 0; i < usable.size(); i++) {
    consider(usable[i]->lag_seconds - fallback_drift * usable[i]->ref_seconds, fallback_drift);
    for (size_t j = i + 1; j < usable.size(); j++) {
      double dt = usable[j]->ref_seconds - usable[i]->ref_seconds;
      if (std::fabs(dt) < 1.0) continue;
      double drift = (usable[j]->lag_seconds - usable[i]->lag_seconds) / dt;
      consider(usable[i]->lag_seconds - drift * usable[i]->ref_seconds, drift);
    }
  }

  const double min_span = std::max(20.0, 0.25 * duration);
  for (int round = 0; round < 3; round++) {
    double sum_t = 0.0, sum_y = 0.0, sum_tt = 0.0, sum_ty = 0.0, t_min = 1e300, t_max = -1e300;
    int n = 0;
    for (const SyncAnchor* a : usable) {
      if (std::fabs(residual(*a, best.offset, best.drift)) > kInlierToleranceSeconds) continue;
      sum_t += a->ref_seconds;
      sum_y += a->lag_seconds;
      sum_tt += a->ref_seconds * a->ref_seconds;
      sum_ty += a->ref_seconds * a->lag_seconds;
      t_min = std::min(t_min, a->ref_seconds);
      t_max = std::max(t_max, a->ref_seconds);
      n++;
    }
    if (n == 0) break;
    best.inliers = n;
    best.span = t_max - t_min;
    const double denominator = n * sum_tt - sum_t * sum_t;
    best.drift_measured = n >= 2 && best.span >= min_span && std::fabs(denominator) > 1e-9;
    if (best.drift_measured) {
      double drift = (n * sum_ty - sum_t * sum_y) / denominator;
      if (std::fabs(drift) <= kMaxDrift) best.drift = drift;
      best.offset = (sum_y - best.drift * sum_t) / n;
    } else {
      best.drift = fallback_drift;
      best.offset = (sum_y - best.drift * sum_t) / n;
    }
  }

  double sum_squares = 0.0;
  int n = 0;
  for (auto& a : *anchors) {
    a.inlier = a.strength >= kMinStrength && std::fabs(residual(a, best.offset, best.drift)) <= kInlierToleranceSeconds;
    if (a.inlier) {
      double r = residual(a, best.offset, best.drift);
      sum_squares += r * r;
      n++;
    }
  }
  best.inliers = n;
  best.rms = n ? std::sqrt(sum_squares / n) : 0.0;
  return best;
}

// How many inliers rest on separate stretches of audio. Overlapping windows hear
// the same sound, so a single chance match can't pass itself off as several.
int count_independent_inliers(const std::vector<SyncAnchor>& anchors) {
  std::vector<const SyncAnchor*> picked;
  auto overlaps_picked = [&](const SyncAnchor& a) {
    for (const SyncAnchor* p : picked) {
      if (std::fabs(a.ref_seconds - p->ref_seconds) < 0.5 * (a.window_seconds + p->window_seconds)) return true;
    }
    return false;
  };
  for (double window : {kShortWindowSeconds, kWindowSeconds}) {
    for (const auto& a : anchors) {
      if (a.inlier && a.window_seconds == window && !overlaps_picked(a)) picked.push_back(&a);
    }
  }
  return int(picked.size());
}

SyncResult evaluate_plan(const AudioView& ref, const AudioView& other, int rate,
                         const std::vector<AnchorPlace>& places, const SearchPlan& plan, const TimeMap* prior) {
  SyncResult result;
  AnchorSearch search{ref, other, rate};
  for (const AnchorPlace& place : places) {
    SyncAnchor anchor;
    if (search.measure(place, plan, &anchor)) result.anchors.push_back(anchor);
  }
  const double duration = ref.seconds();
  LineFit fit = fit_anchors(&result.anchors, duration, prior);

  std::vector<float> strengths;
  int usable = 0;
  int strong_outliers = 0;
  for (const auto& a : result.anchors) {
    if (a.strength >= kMinStrength) usable++;
    if (a.inlier) strengths.push_back(float(a.strength));
    if (a.strength >= kStrongStrength && !a.inlier) strong_outliers++;
  }

  result.map.offset_seconds = fit.offset;
  result.map.drift = fit.drift;
  result.anchors_used = fit.inliers;
  result.anchors_usable = usable;
  result.residual_rms_ms = fit.rms * 1000.0;
  result.drift_measured = fit.drift_measured;
  if (fit.inliers == 0) return result;

  // Any two points fit a line perfectly, so two matches prove nothing.
  const int independent = count_independent_inliers(result.anchors);
  const double count_score = independent < 3 ? 0.0 : saturate(0.4 + 0.2 * (independent - 2));
  const double fraction = double(fit.inliers) / usable;
  const double coverage = duration > 0.0 ? fit.span / duration : 0.0;
  const double coverage_score = fit.drift_measured ? 0.4 + 0.6 * saturate(coverage / 0.5) : 0.7;
  const double strength_score = saturate((median_of(strengths) - kMinStrength) / 13.0);
  double confidence = count_score * coverage_score * (0.4 + 0.6 * saturate(fraction / 0.7)) *
                      (0.4 + 0.6 * strength_score);
  if (!fit.drift_measured && !prior) confidence *= 0.6;

  // More than a stray bad match, and enough of them that a straight line is in doubt.
  result.nonlinear = strong_outliers >= 2 && double(strong_outliers) / usable >= 0.25;
  if (result.nonlinear) confidence = std::min(confidence, 0.45);
  result.confidence = saturate(confidence);
  return result;
}

void set_message(SyncResult* r, bool have_prior) {
  char buffer[512];
  double offset_ms = r->map.offset_seconds * 1000.0;
  double ppm = r->map.drift * 1e6;
  if (r->method == SyncMethod::None) {
    snprintf(buffer, sizeof(buffer),
             "Could not find enough common sound to align this track. Left unaligned. Try clapping near all "
             "microphones at the start and end of the recording.");
  } else if (r->method == SyncMethod::Timestamps) {
    snprintf(buffer, sizeof(buffer),
             "No reliable common sound, so this uses capture timestamps only (offset %.2f ms, drift %.2f ppm). "
             "Check it by ear.",
             offset_ms, ppm);
  } else if (r->nonlinear) {
    snprintf(buffer, sizeof(buffer),
             "Offset %.2f ms, drift %.2f ppm, but strong matches disagree with one straight line. The clock may "
             "have jumped, or speakers may be at different distances. Check it by ear.",
             offset_ms, ppm);
  } else if (!r->drift_measured) {
    snprintf(buffer, sizeof(buffer),
             "Offset %.2f ms. Matches covered too little of the recording to measure drift, so %s. Clap near the "
             "end as well for a measured value.",
             offset_ms, have_prior ? "drift comes from capture timestamps" : "drift is assumed to be zero");
  } else {
    snprintf(buffer, sizeof(buffer), "Offset %.2f ms, drift %.2f ppm, from %d of %d matching sounds.", offset_ms, ppm,
             r->anchors_used, r->anchors_usable);
  }
  r->message = buffer;
}

}  // namespace

ClockFit fit_clock(const std::vector<TimingSample>& samples, int sample_rate) {
  ClockFit fit;
  if (samples.size() < 8 || sample_rate <= 0) return fit;

  // Centering keeps precision: QPC seconds can be large and drift terms tiny.
  const double t0 = double(samples.front().frame) / sample_rate;
  const double h0 = samples.front().host_seconds;
  double slope = 1.0;
  double intercept = 0.0;
  double limit = 1e300;
  int used = 0;
  double rms = 0.0;
  for (int round = 0; round < 4; round++) {
    double sum_t = 0.0, sum_h = 0.0, sum_tt = 0.0, sum_th = 0.0;
    int n = 0;
    for (const auto& s : samples) {
      double t = double(s.frame) / sample_rate - t0;
      double h = s.host_seconds - h0;
      if (std::fabs(h - (intercept + slope * t)) > limit) continue;
      sum_t += t;
      sum_h += h;
      sum_tt += t * t;
      sum_th += t * h;
      n++;
    }
    const double denominator = n * sum_tt - sum_t * sum_t;
    if (n < 8 || denominator <= 1e-9) return fit;
    slope = (n * sum_th - sum_t * sum_h) / denominator;
    intercept = (sum_h - slope * sum_t) / n;
    double sum_squares = 0.0;
    int m = 0;
    for (const auto& s : samples) {
      double t = double(s.frame) / sample_rate - t0;
      double r = (s.host_seconds - h0) - (intercept + slope * t);
      if (std::fabs(r) > limit) continue;
      sum_squares += r * r;
      m++;
    }
    rms = std::sqrt(sum_squares / std::max(1, m));
    used = m;
    limit = std::max(3.0 * rms, 1e-5);
  }
  fit.valid = true;
  fit.slope = slope;
  fit.intercept = h0 + intercept - slope * t0;
  fit.rms_ms = rms * 1000.0;
  fit.samples_used = used;
  return fit;
}

bool time_map_from_clocks(const ClockFit& ref, const ClockFit& other, TimeMap* out) {
  if (!ref.valid || !other.valid) return false;
  const double drift = ref.slope / other.slope - 1.0;
  if (std::fabs(drift) > 2.0 * kMaxDrift) return false;
  out->offset_seconds = (ref.intercept - other.intercept) / other.slope;
  out->drift = drift;
  return true;
}

SyncResult align_tracks(const AudioView& ref, const AudioView& other, const TimeMap* prior,
                        std::atomic<float>* progress) {
  auto report = [&](float value) {
    if (progress) progress->store(value);
  };
  report(0.0f);

  const int rate = std::min({kAnalysisRate, ref.sample_rate, other.sample_rate});
  const std::vector<float> ref_envelope = onset_envelope(ref);
  report(0.2f);
  const std::vector<float> other_envelope = onset_envelope(other);
  report(0.4f);
  const std::vector<AnchorPlace> times = choose_anchor_times(ref_envelope, ref.seconds());

  SyncResult best;
  bool have_best = false;
  auto consider = [&](SyncResult candidate) {
    candidate.method = SyncMethod::Audio;
    if (!have_best || candidate.confidence > best.confidence ||
        (candidate.confidence == best.confidence && candidate.anchors_used > best.anchors_used)) {
      best = std::move(candidate);
      have_best = true;
    }
  };

  if (prior) {
    SearchPlan plan;
    plan.lag_at_center = prior->offset_seconds;
    plan.drift = prior->drift;
    plan.base_radius = kPriorRadiusSeconds;
    consider(evaluate_plan(ref, other, rate, times, plan, prior));
  }
  report(0.55f);

  // Skip the coarse search when timestamps already led to a good match.
  if (!have_best || best.confidence < kGoodConfidence) {
    const std::vector<CoarseCandidate> candidates = find_coarse_candidates(ref_envelope, other_envelope);
    report(0.7f);
    for (const auto& c : candidates) {
      SearchPlan plan;
      plan.lag_at_center = c.lag_seconds;
      plan.center_seconds = c.ref_seconds;
      plan.base_radius = 0.3;
      plan.radius_per_second = kMaxDrift;
      consider(evaluate_plan(ref, other, rate, times, plan, prior));
    }
  }
  report(0.95f);

  if (!have_best || best.confidence < kWeakConfidence) {
    std::vector<SyncAnchor> anchors;
    int usable = 0;
    if (have_best) {
      anchors = std::move(best.anchors);
      usable = best.anchors_usable;
    }
    best = SyncResult();
    best.anchors = std::move(anchors);
    best.anchors_usable = usable;
    if (prior) {
      best.map = *prior;
      best.method = SyncMethod::Timestamps;
      best.confidence = 0.3;
      best.drift_measured = true;
    }
  }
  set_message(&best, prior != nullptr);
  report(1.0f);
  return best;
}

const char* sync_method_name(SyncMethod method) {
  switch (method) {
    case SyncMethod::Audio:
      return "common sound";
    case SyncMethod::Timestamps:
      return "capture timestamps";
    default:
      return "none";
  }
}

const char* confidence_label(double confidence) {
  if (confidence >= 0.7) return "High";
  if (confidence >= 0.4) return "Medium";
  if (confidence > 0.0) return "Low";
  return "None";
}
