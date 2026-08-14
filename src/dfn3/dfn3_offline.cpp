// Offline whole-utterance DeepFilterNet3 orchestrator.
//
// Pipeline (see MEMORY / dfn3-integration for the libDF references):
//   1. Frame the input with Stft (hop=480) -> keep the NOISY complex spectrogram.
//   2. Extract DFN3 features (Dfn3Features) per frame -> feat_erb / feat_spec tensors.
//   3. enc(feat_erb, feat_spec) -> {e0..e3, emb, c0, lsnr}.
//   4. erb_dec(emb, e3, e2, e1, e0) -> ERB gain mask m[1,1,T,32].
//   5. df_dec(emb, c0) -> deep-filter coefs[1,T,96,10].
//   6. Assemble enhanced spectrum: deep-filter the low bins (<96) of the noisy
//      spectrum, ERB-mask the high bins (>=96).
//   7. ISTFT -> trim STFT latency (480) and the tail padding -> enhanced mono.

#include "dfn3_offline.h"

#include <onnxruntime_cxx_api.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "dfn3_features.h"
#include "dfn3_postdsp.h"
#include "erb.h"
#include "stft.h"

namespace vc {

namespace {

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

  NamedSession() = default;

  void init() {
    Ort::AllocatorWithDefaultOptions alloc;
    for (size_t i = 0; i < session.GetInputCount(); ++i)
      inNames.emplace_back(session.GetInputNameAllocated(i, alloc).get());
    for (size_t i = 0; i < session.GetOutputCount(); ++i)
      outNames.emplace_back(session.GetOutputNameAllocated(i, alloc).get());
  }
};

}  // namespace

struct Dfn3Offline::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "dfn3-offline"};
  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  NamedSession enc, erbDec, dfDec;

  explicit Impl(const std::string& modelDir) {
    std::string dir = modelDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);

    auto load = [&](NamedSession& ns, const std::string& file) {
      try {
        ns.session = Ort::Session(env, widen(dir + file).c_str(), opts);
      } catch (const Ort::Exception& e) {
        throw std::runtime_error("Dfn3Offline: failed to load " + dir + file + ": " + e.what());
      }
      ns.init();
    };
    load(enc, "enc.onnx");
    load(erbDec, "erb_dec.onnx");
    load(dfDec, "df_dec.onnx");
  }

  // Run a session. inputs maps input-name -> (data ptr, shape). All the session's
  // outputs are extracted into owned buffers keyed by output name.
  std::map<std::string, OwnedTensor> run(
      NamedSession& ns,
      const std::map<std::string, std::pair<float*, std::vector<int64_t>>>& inputs) {
    std::vector<Ort::Value> inVals;
    std::vector<const char*> inPtrs;
    inVals.reserve(ns.inNames.size());
    for (const std::string& name : ns.inNames) {
      auto it = inputs.find(name);
      if (it == inputs.end())
        throw std::runtime_error("Dfn3Offline: missing input tensor for \"" + name + "\"");
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
};

Dfn3Offline::Dfn3Offline(const std::string& modelDir) : p_(new Impl(modelDir)) {}
Dfn3Offline::~Dfn3Offline() { delete p_; }

std::vector<float> Dfn3Offline::process(const std::vector<float>& mono48k) {
  const int64_t N = static_cast<int64_t>(mono48k.size());
  if (N == 0) return {};

  constexpr int kHop = 480;
  constexpr int kBins = 481;   // fftSize/2 + 1
  constexpr int kErb = 32;
  constexpr int kDf = 96;
  constexpr int kOrder = 5;
  constexpr int kLookahead = 2;

  // ---- 1. Frame the NOISY spectrogram. T = frames to cover N, plus one flush
  // frame so that after trimming the 480-sample STFT latency the output still
  // reaches the original length. ----
  const int64_t framesForInput = (N + kHop - 1) / kHop;
  const int64_t T = framesForInput + 1;

  std::vector<float> padded(static_cast<size_t>(T) * kHop, 0.0f);
  for (int64_t i = 0; i < N; ++i) padded[static_cast<size_t>(i)] = mono48k[static_cast<size_t>(i)];

  std::vector<float> specRe(static_cast<size_t>(T) * kBins);
  std::vector<float> specIm(static_cast<size_t>(T) * kBins);
  {
    Stft stft(960, kHop);
    for (int64_t t = 0; t < T; ++t) {
      stft.analyze(padded.data() + static_cast<size_t>(t) * kHop,
                   specRe.data() + static_cast<size_t>(t) * kBins,
                   specIm.data() + static_cast<size_t>(t) * kBins);
    }
  }

  // ---- 2. Features -> ONNX tensor layouts. ----
  // feat_erb [1,1,T,32]  index(t,b) = t*32 + b
  // feat_spec[1,2,T,96]  index(p,t,b) = p*(T*96) + t*96 + b
  std::vector<float> featErbBuf(static_cast<size_t>(T) * kErb);
  std::vector<float> featSpecBuf(static_cast<size_t>(2) * T * kDf);
  {
    Dfn3Features feats;
    std::vector<float> fe(kErb), fs(2 * kDf);
    const size_t plane = static_cast<size_t>(T) * kDf;
    for (int64_t t = 0; t < T; ++t) {
      feats.process(specRe.data() + static_cast<size_t>(t) * kBins,
                    specIm.data() + static_cast<size_t>(t) * kBins, fe.data(), fs.data());
      for (int b = 0; b < kErb; ++b)
        featErbBuf[static_cast<size_t>(t) * kErb + b] = fe[b];
      for (int b = 0; b < kDf; ++b) {
        featSpecBuf[static_cast<size_t>(t) * kDf + b] = fs[b];             // plane 0 (real)
        featSpecBuf[plane + static_cast<size_t>(t) * kDf + b] = fs[kDf + b];  // plane 1 (imag)
      }
    }
  }

  // ---- 3. enc ----
  std::vector<int64_t> erbShape{1, 1, T, kErb};
  std::vector<int64_t> specShape{1, 2, T, kDf};
  std::map<std::string, std::pair<float*, std::vector<int64_t>>> encIn;
  for (const std::string& name : p_->enc.inNames) {
    if (name.find("erb") != std::string::npos)
      encIn[name] = {featErbBuf.data(), erbShape};
    else
      encIn[name] = {featSpecBuf.data(), specShape};
  }
  std::map<std::string, OwnedTensor> encOut = p_->run(p_->enc, encIn);

  auto need = [](std::map<std::string, OwnedTensor>& m, const std::string& key) -> OwnedTensor& {
    auto it = m.find(key);
    if (it == m.end()) throw std::runtime_error("Dfn3Offline: enc output \"" + key + "\" not found");
    return it->second;
  };

  // ---- 4. erb_dec(emb, e3, e2, e1, e0) -> m[1,1,T,32] ----
  std::map<std::string, std::pair<float*, std::vector<int64_t>>> erbIn;
  for (const std::string& name : p_->erbDec.inNames) {
    OwnedTensor& src = need(encOut, name);
    erbIn[name] = {src.data.data(), src.shape};
  }
  std::map<std::string, OwnedTensor> erbOut = p_->run(p_->erbDec, erbIn);
  if (erbOut.empty()) throw std::runtime_error("Dfn3Offline: erb_dec produced no output");
  OwnedTensor& maskT = erbOut.begin()->second;  // single output: m[1,1,T,32]
  const float* mask = maskT.data.data();

  // ---- 5. df_dec(emb, c0) -> coefs[1,T,96,10] ----
  std::map<std::string, std::pair<float*, std::vector<int64_t>>> dfIn;
  for (const std::string& name : p_->dfDec.inNames) {
    OwnedTensor& src = need(encOut, name);
    dfIn[name] = {src.data.data(), src.shape};
  }
  std::map<std::string, OwnedTensor> dfOut = p_->run(p_->dfDec, dfIn);
  // coefs is the 4-D output ([1,T,96,10]); the other output is [1,T,1] and ignored.
  const float* coefs = nullptr;
  for (auto& kv : dfOut) {
    if (kv.second.shape.size() == 4) { coefs = kv.second.data.data(); break; }
  }
  if (!coefs) throw std::runtime_error("Dfn3Offline: df_dec coefs output not found");

  // ---- 6. Assemble enhanced spectrum. ----
  // Working copy = NOISY spectrum.
  std::vector<float> outRe = specRe;
  std::vector<float> outIm = specIm;

  // Low bins (<96): deep-filter the noisy spectrum in place.
  apply_deep_filter(outRe.data(), outIm.data(), coefs, static_cast<int>(T), kDf, kOrder, kLookahead);

  // High bins (>=96): ERB-mask the noisy spectrum, then copy bins [96..480] over.
  {
    ErbFilterBank fb(kBins, kErb, 48000.0f, 2);
    std::vector<float> tmpRe(kBins), tmpIm(kBins);
    for (int64_t t = 0; t < T; ++t) {
      const size_t base = static_cast<size_t>(t) * kBins;
      for (int f = 0; f < kBins; ++f) {
        tmpRe[f] = specRe[base + f];
        tmpIm[f] = specIm[base + f];
      }
      apply_erb_mask(fb, mask + static_cast<size_t>(t) * kErb, tmpRe.data(), tmpIm.data());
      for (int f = kDf; f < kBins; ++f) {
        outRe[base + f] = tmpRe[f];
        outIm[base + f] = tmpIm[f];
      }
    }
  }

  // ---- 7. ISTFT + trim. ----
  std::vector<float> synth(static_cast<size_t>(T) * kHop);
  {
    Stft stft(960, kHop);
    std::vector<float> hop(kHop);
    for (int64_t t = 0; t < T; ++t) {
      stft.synthesize(outRe.data() + static_cast<size_t>(t) * kBins,
                      outIm.data() + static_cast<size_t>(t) * kBins, hop.data());
      for (int i = 0; i < kHop; ++i) synth[static_cast<size_t>(t) * kHop + i] = hop[i];
    }
  }

  // Drop the leading 480 latency samples, then clamp to the original length.
  std::vector<float> out;
  const size_t start = kHop;
  if (synth.size() > start) {
    size_t avail = synth.size() - start;
    size_t len = (avail < static_cast<size_t>(N)) ? avail : static_cast<size_t>(N);
    out.assign(synth.begin() + start, synth.begin() + start + len);
  }
  out.resize(static_cast<size_t>(N), 0.0f);
  return out;
}

}  // namespace vc
