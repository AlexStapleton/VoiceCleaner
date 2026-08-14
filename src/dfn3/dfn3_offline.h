#pragma once
#include <string>
#include <vector>
namespace vc {
// Offline whole-utterance DeepFilterNet3. Loads the 3 split ONNX models from modelDir. Not real-time
// (the export has no recurrent-state I/O, so the whole sequence is run at once).
class Dfn3Offline {
public:
  explicit Dfn3Offline(const std::string& modelDir);   // throws std::runtime_error on load failure
  ~Dfn3Offline();
  std::vector<float> process(const std::vector<float>& mono48k);  // returns enhanced mono
private:
  struct Impl; Impl* p_;
};
}
