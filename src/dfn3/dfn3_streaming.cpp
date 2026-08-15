// Real-time DeepFilterNet3 streaming orchestrator (the `_ll` split models).
//
// Per 480-sample hop, follows the "Host driving contract" in docs/dfn3-progress.md:
//   1. Stft::analyze(in480) -> noisy complex spectrum re[481]/im[481].
//   2. Dfn3Features::process -> feat_erb[32], feat_spec[2*96] (stateful running norm).
//   3. enc_conv_streaming: rolling W=6 window (stateless, slice-last) -> e0..e3,c0,emb.
//   4. enc_gru_streaming: one frame/hop, plain GRU state h<-h1 -> emb_out, lsnr, h1.
//   5. erb_dec_streaming (W=3, 2-layer GRU) -> ERB mask m[32];
//      df_dec_streaming  (W=5, 3-layer GRU) -> deep-filter coefs[96*10].
//      Both use the decoder advance-by-one state rule (see WindowRunner).
//   6. DeepFilterRing(96,5,4): deep-filter the noisy low bins (<96); ERB-mask the
//      high bins (>=96). df_lookahead=0 => leftPad=4, future=0 (frame ready now).
//   7. Stft::synthesize -> out480.
//
// All GRU hidden = 512. The only algorithmic latency is the STFT's one hop (480).
//
// RT-safety: process() is the MMCSS/relay-thread hot path. After warm-up (once each
// rolling window has filled) it performs NO heap allocation in our code -- all
// containers are preallocated members reused in place, the frame ring is a fixed
// index-based circular buffer, output names/indices are resolved once at load, and
// ORT reads/writes go through persistent input/output OrtValues bound to our reused
// buffers (see WindowRunner). The residual ORT `Run` throw risk is contained by a
// try/catch in Dfn3Streaming::process that fails safe (silence + sticky flag).

#include "dfn3_streaming.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dfn3_features.h"
#include "dfn3_postdsp.h"
#include "erb.h"
#include "stft.h"

namespace vc {

namespace {

constexpr int kHop = 480;
constexpr int kBins = 481;   // fftSize/2 + 1
constexpr int kErb = 32;
constexpr int kDf = 96;
constexpr int kOrder = 5;
constexpr int kLeftPad = 4;  // order - df_lookahead(0) - 1  (causal deep filter)
constexpr int kGru = 512;

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// A float tensor we own the backing storage for (so pointers stay valid across Run calls).
struct OwnedTensor {
  std::vector<float> data;
  std::vector<int64_t> shape;
};

// Query and cache a session's input/output names. The cached `const char*` arrays are
// built once (after the name strings are final) and reused for every Run -- no per-hop
// string construction. NamedSession must not be moved/copied after init(): the cached
// pointers alias the std::string buffers.
struct NamedSession {
  Ort::Session session{nullptr};
  std::vector<std::string> inNames;
  std::vector<std::string> outNames;
  std::vector<const char*> outPtrs;  // outNames[i].c_str(), model order

  void init() {
    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < session.GetInputCount(); ++i)
      inNames.emplace_back(session.GetInputNameAllocated(i, alloc).get());
    for (size_t i = 0; i < session.GetOutputCount(); ++i)
      outNames.emplace_back(session.GetOutputNameAllocated(i, alloc).get());
    outPtrs.reserve(outNames.size());
    for (const std::string& n : outNames) outPtrs.push_back(n.c_str());
  }

  int outIndex(const std::string& name) const {
    for (size_t i = 0; i < outNames.size(); ++i)
      if (outNames[i] == name) return static_cast<int>(i);
    return -1;
  }
};

// Description of one windowed input tensor.
//   fourDim=true  -> ONNX shape [1, chan, S, feat]  (e0..e3/c0/feat_erb/feat_spec)
//   fourDim=false -> ONNX shape [1, S, feat]        (emb; chan must be 1)
// The per-frame source buffer is laid out [chan][feat] (chan-major); it is scattered
// into the window buffer at time index t of a window holding S frames.
struct WinInput {
  std::string name;
  int chan;
  int feat;
  bool fourDim;
  int perFrame() const { return chan * feat; }
  std::vector<int64_t> shape(int S) const {
    if (fourDim) return {1, chan, S, feat};
    return {1, S, feat};
  }
};

// Drives one *_streaming decoder/encoder graph fed a rolling window of the last W
// input frames, returning the last frame. Handles both stateless windowed graphs
// (layers==0, e.g. enc_conv) and windowed+stateful decoders (layers>0) using the
// "advance-by-one" rule from the host driving contract:
//   *_h1 is the GRU state AFTER the first window frame, so the carried state h is
//   advanced by exactly one frame/hop -- only when the window is full (about to drop
//   its oldest frame) do we set h <- (h1 from the run made while that frame was oldest).
// At W==1 this degrades to a plain stateful GRU (h <- h1), which is enc_gru's case.
//
// Allocation-free after warm-up: the frame history is a fixed W-slot circular buffer,
// window/shape/name scratch are preallocated members, and ORT talks to persistent
// input/output OrtValues bound to those reused buffers. Output shapes are constant
// (slice-last / per-frame), so they are discovered on the first Run and then reused.
class WindowRunner {
public:
  void init(Ort::MemoryInfo* mem, NamedSession* ns, int W, std::vector<WinInput> ins,
            int layers, std::string hIn, std::string hOut) {
    mem_ = mem;
    ns_ = ns;
    W_ = W;
    ins_ = std::move(ins);
    layers_ = layers;
    hIn_ = std::move(hIn);
    hOut_ = std::move(hOut);

    const size_t n = ins_.size();

    // Preallocated per-input window storage (max S == W) and shape scratch.
    winStorage_.resize(n);
    shapeStorage_.resize(n);
    for (size_t i = 0; i < n; ++i) {
      winStorage_[i].assign(static_cast<size_t>(ins_[i].perFrame()) * W_, 0.0f);
      shapeStorage_[i] = ins_[i].shape(W_);  // S dim overwritten in place each hop
    }

    // Input name pointers in OUR feed order: [ins_ ..., hIn_]. Stable for the object's
    // lifetime (they alias ins_[i].name / hIn_, which are not mutated after init).
    inPtrs_.reserve(n + (layers_ > 0 ? 1 : 0));
    for (size_t i = 0; i < n; ++i) inPtrs_.push_back(ins_[i].name.c_str());
    if (layers_ > 0) inPtrs_.push_back(hIn_.c_str());
    inVals_.reserve(n + (layers_ > 0 ? 1 : 0));

    // Fixed W-slot frame ring; each slot holds one flat per-frame buffer per input.
    ring_.resize(W_);
    for (int s = 0; s < W_; ++s) {
      ring_[s].resize(n);
      for (size_t i = 0; i < n; ++i)
        ring_[s][i].assign(static_cast<size_t>(ins_[i].perFrame()), 0.0f);
    }

    if (layers_ > 0) {
      h_.assign(static_cast<size_t>(layers_) * kGru, 0.0f);
      pending_.assign(static_cast<size_t>(layers_) * kGru, 0.0f);
      hShape_ = {layers_, 1, kGru};
      hOutIdx_ = ns_->outIndex(hOut_);
      if (hOutIdx_ < 0)
        throw std::runtime_error("Dfn3Streaming: state output \"" + hOut_ + "\" not found");
    }

    // Output scratch sized on first Run (shapes are constant); one slot per output.
    outScratch_.resize(ns_->outNames.size());
    outVals_.reserve(ns_->outNames.size());
    for (size_t i = 0; i < ns_->outNames.size(); ++i) outVals_.emplace_back(nullptr);

    reset();
  }

  void reset() {
    head_ = 0;
    count_ = 0;
    inBound_ = false;  // window restarts at S=1; persistent input values must be rebuilt
    if (layers_ > 0) {
      std::fill(h_.begin(), h_.end(), 0.0f);
      std::fill(pending_.begin(), pending_.end(), 0.0f);
    }
    // outScratch_/outVals_ (constant shapes) are intentionally kept across resets.
  }

  // srcs: one flat per-frame source pointer per WinInput, in ins_ order. Each points to
  // perFrame() floats laid out [chan][feat]. Data is copied into the ring immediately.
  void run(std::initializer_list<const float*> srcs) {
    // Advance the rolling window (and, for stateful graphs, the carried GRU state).
    if (count_ == W_) {
      if (layers_ > 0) std::copy(pending_.begin(), pending_.end(), h_.begin());
      head_ = (head_ + 1) % W_;  // drop oldest
      --count_;
    }
    const int slot = (head_ + count_) % W_;
    size_t i = 0;
    for (const float* s : srcs) {
      std::copy(s, s + ins_[i].perFrame(), ring_[slot][i].begin());
      ++i;
    }
    ++count_;
    const int S = count_;

    // Scatter the S retained frames into each input's window buffer, layout [chan][S][feat].
    for (size_t j = 0; j < ins_.size(); ++j) {
      const WinInput& in = ins_[j];
      std::vector<float>& win = winStorage_[j];
      for (int t = 0; t < S; ++t) {
        const std::vector<float>& src = ring_[(head_ + t) % W_][j];
        for (int c = 0; c < in.chan; ++c)
          for (int x = 0; x < in.feat; ++x)
            win[(static_cast<size_t>(c) * S + t) * in.feat + x] =
                src[static_cast<size_t>(c) * in.feat + x];
      }
      if (in.fourDim)
        shapeStorage_[j][2] = S;  // [1,chan,S,feat]
      else
        shapeStorage_[j][1] = S;  // [1,S,feat]
    }

    runInference(S);

    if (layers_ > 0)  // *_h1 = GRU state after the window's first frame
      std::copy(outScratch_[hOutIdx_].data.begin(), outScratch_[hOutIdx_].data.end(),
                pending_.begin());
  }

  int outIndex(const std::string& name) const { return ns_->outIndex(name); }
  const OwnedTensor& out(int i) const { return outScratch_[static_cast<size_t>(i)]; }
  const float* outData(int i) const { return outScratch_[static_cast<size_t>(i)].data.data(); }

private:
  // (Re)build the ORT input value list over our reused buffers. Once the window is full
  // (S==W) the values are created once and reused every hop (persistent binding); ORT
  // re-reads the same buffers, which we refill in place -- so the steady state allocates
  // nothing. During warm-up (S<W) the shape changes each hop, so we rebuild.
  void bindInputs(int S) {
    const bool persistent = (S == W_);
    if (inBound_ && persistent) return;  // already bound; buffers stable, just reused
    inVals_.clear();
    for (size_t i = 0; i < ins_.size(); ++i)
      inVals_.emplace_back(Ort::Value::CreateTensor<float>(
          *mem_, winStorage_[i].data(), static_cast<size_t>(ins_[i].perFrame()) * S,
          shapeStorage_[i].data(), shapeStorage_[i].size()));
    if (layers_ > 0)
      inVals_.emplace_back(Ort::Value::CreateTensor<float>(
          *mem_, h_.data(), h_.size(), hShape_.data(), hShape_.size()));
    inBound_ = persistent;
  }

  void runInference(int S) {
    bindInputs(S);
    if (!outBound_) {
      // First Run: discover output shapes and bind persistent output values in place.
      std::vector<Ort::Value> outs = ns_->session.Run(
          Ort::RunOptions{nullptr}, inPtrs_.data(), inVals_.data(), inVals_.size(),
          ns_->outPtrs.data(), ns_->outPtrs.size());
      for (size_t i = 0; i < outs.size(); ++i) {
        if (!outs[i].IsTensor()) continue;
        auto info = outs[i].GetTensorTypeAndShapeInfo();
        outScratch_[i].shape = info.GetShape();
        const size_t cnt = info.GetElementCount();
        const float* d = outs[i].GetTensorData<float>();
        outScratch_[i].data.assign(d, d + cnt);
        outVals_[i] = Ort::Value::CreateTensor<float>(
            *mem_, outScratch_[i].data.data(), cnt, outScratch_[i].shape.data(),
            outScratch_[i].shape.size());
      }
      outBound_ = true;
    } else {
      // Steady state: ORT writes results directly into our reused output buffers.
      ns_->session.Run(Ort::RunOptions{nullptr}, inPtrs_.data(), inVals_.data(),
                       inVals_.size(), ns_->outPtrs.data(), outVals_.data(),
                       outVals_.size());
    }
  }

  Ort::MemoryInfo* mem_ = nullptr;
  NamedSession* ns_ = nullptr;
  int W_ = 1;
  int layers_ = 0;
  std::string hIn_, hOut_;
  std::vector<WinInput> ins_;

  // Input side (all reused).
  std::vector<std::vector<float>> winStorage_;      // [ins_][perFrame*W]
  std::vector<std::vector<int64_t>> shapeStorage_;  // [ins_] shape vecs (S dim mutable)
  std::vector<int64_t> hShape_;                     // {layers,1,kGru}
  std::vector<const char*> inPtrs_;                 // feed order: [ins_ names..., hIn]
  std::vector<Ort::Value> inVals_;                  // reused; persistent once S==W
  bool inBound_ = false;
  std::vector<float> h_, pending_;

  // Fixed-capacity frame ring: ring_[slot][input] = flat [perFrame] frame buffer.
  std::vector<std::vector<std::vector<float>>> ring_;
  int head_ = 0;
  int count_ = 0;

  // Output side (all reused). outScratch_[i]/outVals_[i] correspond to ns_->outNames[i].
  std::vector<OwnedTensor> outScratch_;
  std::vector<Ort::Value> outVals_;
  bool outBound_ = false;
  int hOutIdx_ = -1;
};

}  // namespace

struct Dfn3Streaming::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "dfn3-streaming"};
  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  NamedSession encConvS, encGruS, erbDecS, dfDecS;

  Stft stft{960, kHop};
  Dfn3Features feats;
  ErbFilterBank fb{kBins, kErb, 48000.0f, 2};
  DeepFilterRing ring{kDf, kOrder, kLeftPad};

  WindowRunner encConv, encGru, erbDec, dfDec;

  // Cached output indices (resolved once at load) so the hot path does no name lookups.
  int ecEmb = 0, ecE3 = 0, ecE2 = 0, ecE1 = 0, ecE0 = 0, ecC0 = 0;  // enc_conv outputs
  int egEmbOut = 0, egLsnr = 0;                                     // enc_gru outputs
  int erbM = 0;                                                     // erb_dec output
  int dfCoefs = 0;                                                  // df_dec output

  // Per-hop scratch (kept as members to avoid reallocation on the hot path).
  std::vector<float> re, im;             // noisy spectrum [481]
  std::vector<float> featErb, featSpec;  // [32], [2*96]
  std::vector<float> outRe, outIm;       // enhanced spectrum [481]
  std::vector<float> lowRe, lowIm;       // deep-filtered low bins [96]
  float lastLsnr = 0.0f;

  bool hopFailed = false;      // true if the most recent hop threw (output was silenced)
  uint64_t failedHops = 0;     // sticky count of silenced hops since construction
  bool needsResync = false;    // set on failure; triggers a reset() before the next hop
  bool warnedFail = false;     // one-time stderr warning has been emitted

  explicit Impl(const std::string& modelDir) {
    std::string dir = modelDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);

    auto load = [&](NamedSession& ns, const std::string& file) {
      try {
        ns.session = Ort::Session(env, widen(dir + file).c_str(), opts);
      } catch (const Ort::Exception& e) {
        throw std::runtime_error("Dfn3Streaming: failed to load " + dir + file + ": " + e.what());
      }
      ns.init();
    };
    load(encConvS, "enc_conv_streaming.onnx");
    load(encGruS, "enc_gru_streaming.onnx");
    load(erbDecS, "erb_dec_streaming.onnx");
    load(dfDecS, "df_dec_streaming.onnx");

    // enc_conv: windowed W=6, stateless.
    encConv.init(&mem, &encConvS, /*W=*/6,
                 {{"feat_erb", 1, kErb, true}, {"feat_spec", 2, kDf, true}},
                 /*layers=*/0, "", "");
    // enc_gru: one frame/hop, plain stateful GRU (advance-by-one at W=1).
    encGru.init(&mem, &encGruS, /*W=*/1, {{"emb", 1, kGru, false}}, /*layers=*/1, "h0", "h1");
    // erb_dec: windowed W=3, 2-layer GRU.
    erbDec.init(&mem, &erbDecS, /*W=*/3,
                {{"emb", 1, kGru, false},
                 {"e3", 64, 8, true},
                 {"e2", 64, 8, true},
                 {"e1", 64, 16, true},
                 {"e0", 64, 32, true}},
                /*layers=*/2, "erb_h0", "erb_h1");
    // df_dec: windowed W=5, 3-layer GRU.
    dfDec.init(&mem, &dfDecS, /*W=*/5,
               {{"emb", 1, kGru, false}, {"c0", 64, kDf, true}},
               /*layers=*/3, "df_h0", "df_h1");

    // Resolve output indices once (no map/string work on the hot path).
    auto idx = [](WindowRunner& r, const char* name) {
      int i = r.outIndex(name);
      if (i < 0)
        throw std::runtime_error(std::string("Dfn3Streaming: missing output \"") + name + "\"");
      return i;
    };
    ecEmb = idx(encConv, "emb");
    ecE3 = idx(encConv, "e3");
    ecE2 = idx(encConv, "e2");
    ecE1 = idx(encConv, "e1");
    ecE0 = idx(encConv, "e0");
    ecC0 = idx(encConv, "c0");
    egEmbOut = idx(encGru, "emb_out");
    egLsnr = idx(encGru, "lsnr");
    erbM = idx(erbDec, "m");
    dfCoefs = idx(dfDec, "coefs");

    re.assign(kBins, 0.0f);
    im.assign(kBins, 0.0f);
    featErb.assign(kErb, 0.0f);
    featSpec.assign(2 * kDf, 0.0f);
    outRe.assign(kBins, 0.0f);
    outIm.assign(kBins, 0.0f);
    lowRe.assign(kDf, 0.0f);
    lowIm.assign(kDf, 0.0f);
  }

  void reset() {
    stft.reset();
    feats.reset();
    ring = DeepFilterRing(kDf, kOrder, kLeftPad);
    encConv.reset();
    encGru.reset();
    erbDec.reset();
    dfDec.reset();
    lastLsnr = 0.0f;
    hopFailed = false;
  }

  // The per-hop inference. May throw (ORT Run can raise Ort::Exception); the caller
  // (Dfn3Streaming::process) contains that so the audio thread never terminates.
  void process(const float* in480, float* out480) {
    // 1. STFT analysis -> noisy spectrum.
    stft.analyze(in480, re.data(), im.data());

    // 2. Features (feat_spec layout: [0..95]=real, [96..191]=imag of bins 0..95).
    feats.process(re.data(), im.data(), featErb.data(), featSpec.data());

    // 3. enc_conv (W=6, stateless).
    encConv.run({featErb.data(), featSpec.data()});

    // 4. enc_gru (one frame/hop) -> emb_out, lsnr, h1.
    encGru.run({encConv.outData(ecEmb)});
    const std::vector<float>& lsnr = encGru.out(egLsnr).data;
    lastLsnr = lsnr.empty() ? 0.0f : lsnr[0];
    const float* embOut = encGru.outData(egEmbOut);

    // 5. Decoders.
    erbDec.run({embOut, encConv.outData(ecE3), encConv.outData(ecE2),
                encConv.outData(ecE1), encConv.outData(ecE0)});
    const float* mask = erbDec.outData(erbM);  // [32]

    dfDec.run({embOut, encConv.outData(ecC0)});
    const float* coefs = dfDec.outData(dfCoefs);  // [96*10]

    // 6. Deep filter (low bins) + ERB mask (high bins).
    int outT = 0;
    const bool ready = ring.push(re.data(), im.data(), coefs, outT, lowRe.data(), lowIm.data());

    std::copy(re.begin(), re.end(), outRe.begin());
    std::copy(im.begin(), im.end(), outIm.begin());
    apply_erb_mask(fb, mask, outRe.data(), outIm.data());  // masks all bins
    if (ready) {                                           // overwrite low bins (<96)
      for (int b = 0; b < kDf; ++b) {
        outRe[b] = lowRe[b];
        outIm[b] = lowIm[b];
      }
    }

    // 7. ISTFT overlap-add.
    stft.synthesize(outRe.data(), outIm.data(), out480);
  }
};

Dfn3Streaming::Dfn3Streaming(const std::string& modelDir) : p_(new Impl(modelDir)) {}
Dfn3Streaming::~Dfn3Streaming() { delete p_; }

void Dfn3Streaming::process(const float* in480, float* out480) noexcept {
  // Fail safe on the RT thread: ORT's Run can throw Ort::Exception. Rather than let an
  // exception escape a noexcept function (std::terminate on the audio thread), silence
  // this hop and record it. State carried into the next hop may be stale after a failure,
  // but the stream keeps running instead of crashing the process.
  try {
    // Recover from a prior failure: WindowRunner::run advances the rolling window and
    // GRU state BEFORE calling ORT, so a throw leaves the model desynced. Restart from a
    // clean state (reset() zeroes windows/GRU/ring) before processing this hop. The
    // reset alloc happens only on the rare failure path; the success path stays alloc-free.
    if (p_->needsResync) {
      p_->reset();
      p_->needsResync = false;
    }
    p_->process(in480, out480);
    p_->hopFailed = false;
  } catch (...) {
    std::fill(out480, out480 + kHop, 0.0f);
    p_->hopFailed = true;
    ++p_->failedHops;
    p_->needsResync = true;
    if (!p_->warnedFail) {
      std::fprintf(stderr,
                   "[dfn3] engine hop failed (ORT); resyncing. Further failures counted silently.\n");
      p_->warnedFail = true;
    }
  }
}

uint64_t Dfn3Streaming::failedHops() const noexcept { return p_->failedHops; }

uint32_t Dfn3Streaming::latencySamples() const noexcept { return kHop; }
float Dfn3Streaming::lastLsnr() const noexcept { return p_->lastLsnr; }
bool Dfn3Streaming::lastHopFailed() const noexcept { return p_->hopFailed; }
void Dfn3Streaming::reset() noexcept { p_->reset(); }

}  // namespace vc
