#pragma once
#include <string>
#include <vector>
namespace vc {
// Offline whole-utterance DeepFilterNet3. Loads the 3 split ONNX models from modelDir. Not real-time
// (the export has no recurrent-state I/O, so the whole sequence is run at once).
class Dfn3Offline {
public:
  // dfLeftPad = deep-filter left/past pad = order - df_lookahead - 1 (standard model: 2; _ll: 4).
  explicit Dfn3Offline(const std::string& modelDir, int dfLeftPad = 2);  // throws on load failure
  ~Dfn3Offline();
  std::vector<float> process(const std::vector<float>& mono48k);  // returns enhanced mono
private:
  struct Impl; Impl* p_;
};
}
