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

#include "dfn3_streaming.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
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

// Query and cache a session's input/output names as std::string.
struct NamedSession {
  Ort::Session session{nullptr};
  std::vector<std::string> inNames;
  std::vector<std::string> outNames;

  void init() {
    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < session.GetInputCount(); ++i)
      inNames.emplace_back(session.GetInputNameAllocated(i, alloc).get());
    for (size_t i = 0; i < session.GetOutputCount(); ++i)
      outNames.emplace_back(session.GetOutputNameAllocated(i, alloc).get());
  }
};

// Run a session by name. inputs maps input-name -> (data ptr, shape). All the
// session's outputs are extracted into owned buffers keyed by output name.
std::map<std::string, OwnedTensor> runSession(
    Ort::MemoryInfo& mem, NamedSession& ns,
    const std::map<std::string, std::pair<float*, std::vector<int64_t>>>& inputs) {
  std::vector<Ort::Value> inVals;
  std::vector<const char*> inPtrs;
  inVals.reserve(ns.inNames.size());
  for (const std::string& name : ns.inNames) {
    auto it = inputs.find(name);
    if (it == inputs.end())
      throw std::runtime_error("Dfn3Streaming: missing input tensor for \"" + name + "\"");
    const auto& shape = it->second.second;
    size_t count = 1;
    for (int64_t d : shape) count *= static_cast<size_t>(d);
    inVals.push_back(Ort::Value::CreateTensor<float>(
        mem, it->second.first, count, shape.data(), shape.size()));
    inPtrs.push_back(name.c_str());
  }

  std::vector<const char*> outPtrs;
  for (const std::string& n : ns.outNames) outPtrs.push_back(n.c_str());

  std::vector<Ort::Value> outs =
      ns.session.Run(Ort::RunOptions{nullptr}, inPtrs.data(), inVals.data(), inVals.size(),
                     outPtrs.data(), outPtrs.size());

  std::map<std::string, OwnedTensor> result;
  for (size_t i = 0; i < outs.size(); ++i) {
    if (!outs[i].IsTensor()) continue;
    auto info = outs[i].GetTensorTypeAndShapeInfo();
    OwnedTensor ot;
    ot.shape = info.GetShape();
    size_t count = info.GetElementCount();
    const float* d = outs[i].GetTensorData<float>();
    ot.data.assign(d, d + count);
    result.emplace(ns.outNames[i], std::move(ot));
  }
  return result;
}

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
    if (layers_ > 0) {
      h_.assign(static_cast<size_t>(layers_) * kGru, 0.0f);
      pending_ = h_;
    }
    reset();
  }

  void reset() {
    frames_.clear();
    if (layers_ > 0) {
      std::fill(h_.begin(), h_.end(), 0.0f);
      std::fill(pending_.begin(), pending_.end(), 0.0f);
    }
  }

  // frameBufs: one flat buffer per WinInput, in the same order as ins_.
  std::map<std::string, OwnedTensor> run(std::vector<std::vector<float>> frameBufs) {
    // Advance the rolling window (and, for stateful graphs, the carried GRU state).
    if (static_cast<int>(frames_.size()) == W_) {
      if (layers_ > 0) h_ = pending_;
      frames_.pop_front();
    }
    frames_.push_back(std::move(frameBufs));
    const int S = static_cast<int>(frames_.size());

    // Build the window tensors (owned; must outlive the Run call below).
    std::vector<std::vector<float>> wins(ins_.size());
    std::map<std::string, std::pair<float*, std::vector<int64_t>>> inputs;
    for (size_t i = 0; i < ins_.size(); ++i) {
      const WinInput& in = ins_[i];
      wins[i].assign(static_cast<size_t>(in.perFrame()) * S, 0.0f);
      for (int t = 0; t < S; ++t) {
        const std::vector<float>& src = frames_[t][i];
        for (int c = 0; c < in.chan; ++c)
          for (int x = 0; x < in.feat; ++x)
            wins[i][(static_cast<size_t>(c) * S + t) * in.feat + x] =
                src[static_cast<size_t>(c) * in.feat + x];
      }
      inputs[in.name] = {wins[i].data(), in.shape(S)};
    }

    std::vector<int64_t> hShape;
    if (layers_ > 0) {
      hShape = {layers_, 1, kGru};
      inputs[hIn_] = {h_.data(), hShape};
    }

    std::map<std::string, OwnedTensor> out = runSession(*mem_, *ns_, inputs);

    if (layers_ > 0) {
      auto it = out.find(hOut_);
      if (it == out.end())
        throw std::runtime_error("Dfn3Streaming: state output \"" + hOut_ + "\" not found");
      pending_ = it->second.data;  // = GRU state after buf[0]
    }
    return out;
  }

private:
  Ort::MemoryInfo* mem_ = nullptr;
  NamedSession* ns_ = nullptr;
  int W_ = 1;
  int layers_ = 0;
  std::string hIn_, hOut_;
  std::vector<WinInput> ins_;
  std::deque<std::vector<std::vector<float>>> frames_;
  std::vector<float> h_, pending_;
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

  // Per-hop scratch (kept as members to avoid reallocation on the hot path).
  std::vector<float> re, im;             // noisy spectrum [481]
  std::vector<float> featErb, featSpec;  // [32], [2*96]
  std::vector<float> outRe, outIm;       // enhanced spectrum [481]
  std::vector<float> lowRe, lowIm;       // deep-filtered low bins [96]
  float lastLsnr = 0.0f;

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
  }

  void process(const float* in480, float* out480) {
    // 1. STFT analysis -> noisy spectrum.
    stft.analyze(in480, re.data(), im.data());

    // 2. Features (feat_spec layout: [0..95]=real, [96..191]=imag of bins 0..95).
    feats.process(re.data(), im.data(), featErb.data(), featSpec.data());

    // 3. enc_conv (W=6, stateless).
    std::map<std::string, OwnedTensor> encOut = encConv.run({featErb, featSpec});

    // 4. enc_gru (one frame/hop) -> emb_out, lsnr, h1.
    std::map<std::string, OwnedTensor> gruOut = encGru.run({encOut.at("emb").data});
    const std::vector<float>& lsnr = gruOut.at("lsnr").data;
    lastLsnr = lsnr.empty() ? 0.0f : lsnr[0];
    std::vector<float>& embOut = gruOut.at("emb_out").data;

    // 5. Decoders.
    std::map<std::string, OwnedTensor> erbOut = erbDec.run(
        {embOut, encOut.at("e3").data, encOut.at("e2").data, encOut.at("e1").data,
         encOut.at("e0").data});
    const std::vector<float>& mask = erbOut.at("m").data;  // [32]

    std::map<std::string, OwnedTensor> dfOut = dfDec.run({embOut, encOut.at("c0").data});
    const std::vector<float>& coefs = dfOut.at("coefs").data;  // [96*10]

    // 6. Deep filter (low bins) + ERB mask (high bins).
    int outT = 0;
    const bool ready =
        ring.push(re.data(), im.data(), coefs.data(), outT, lowRe.data(), lowIm.data());

    std::copy(re.begin(), re.end(), outRe.begin());
    std::copy(im.begin(), im.end(), outIm.begin());
    apply_erb_mask(fb, mask.data(), outRe.data(), outIm.data());  // masks all bins
    if (ready) {                                                   // overwrite low bins (<96)
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
  p_->process(in480, out480);
}

uint32_t Dfn3Streaming::latencySamples() const noexcept { return kHop; }
float Dfn3Streaming::lastLsnr() const noexcept { return p_->lastLsnr; }
void Dfn3Streaming::reset() noexcept { p_->reset(); }

}  // namespace vc
