// dfn3-probe: de-risk the ONNX Runtime dependency for DeepFilterNet3.
//
// For each of the three split DFN3 graphs (enc, erb_dec, df_dec) this:
//   1. Creates an Ort::Session.
//   2. Prints every input/output: name, element type, shape (dynamic dims = -1).
//   3. Builds a zero-filled dummy tensor per input (dynamic dims -> 1),
//      runs the session, and prints each produced output shape.
//   4. Prints a clear PASS/FAIL per model.
//
// Usage: dfn3-probe [models_dir]   (default: models/dfn3)

#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Convert a UTF-8/ASCII std::string path to std::wstring for the Windows
// Ort::Session constructor (which requires wchar_t* paths).
std::wstring widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

const char* TypeName(ONNXTensorElementDataType t) {
  switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return "uint8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8: return "int8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16: return "uint16";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16: return "int16";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return "int32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return "int64";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING: return "string";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL: return "bool";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return "float16";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return "double";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32: return "uint32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64: return "uint64";
    default: return "other";
  }
}

std::string ShapeToString(const std::vector<int64_t>& dims) {
  std::string s = "[";
  for (size_t i = 0; i < dims.size(); ++i) {
    if (i) s += ", ";
    s += std::to_string(dims[i]);
  }
  s += "]";
  return s;
}

bool ProbeModel(Ort::Env& env, const std::string& model_path) {
  std::cout << "\n================================================================\n";
  std::cout << "MODEL: " << model_path << "\n";
  std::cout << "================================================================\n";

  Ort::SessionOptions opts;
  opts.SetIntraOpNumThreads(1);

  Ort::Session session{nullptr};
  try {
    session = Ort::Session(env, widen(model_path).c_str(), opts);
  } catch (const Ort::Exception& e) {
    std::cout << "  [ERROR] failed to create session: " << e.what() << "\n";
    std::cout << "RESULT: FAIL (" << model_path << ")\n";
    return false;
  }

  Ort::AllocatorWithDefaultOptions allocator;
  const size_t num_inputs = session.GetInputCount();
  const size_t num_outputs = session.GetOutputCount();

  // Gather input metadata.
  std::vector<std::string> input_names;
  std::vector<std::vector<int64_t>> input_shapes;
  std::vector<ONNXTensorElementDataType> input_types;

  std::cout << "  INPUTS (" << num_inputs << "):\n";
  for (size_t i = 0; i < num_inputs; ++i) {
    auto name = session.GetInputNameAllocated(i, allocator);
    input_names.emplace_back(name.get());

    Ort::TypeInfo ti = session.GetInputTypeInfo(i);
    auto tensor_info = ti.GetTensorTypeAndShapeInfo();
    ONNXTensorElementDataType et = tensor_info.GetElementType();
    std::vector<int64_t> dims = tensor_info.GetShape();
    input_types.push_back(et);
    input_shapes.push_back(dims);

    std::cout << "    [" << i << "] name=\"" << input_names.back()
              << "\" type=" << TypeName(et)
              << " shape=" << ShapeToString(dims) << "\n";
  }

  std::cout << "  OUTPUTS (" << num_outputs << "):\n";
  std::vector<std::string> output_names;
  for (size_t i = 0; i < num_outputs; ++i) {
    auto name = session.GetOutputNameAllocated(i, allocator);
    output_names.emplace_back(name.get());

    Ort::TypeInfo ti = session.GetOutputTypeInfo(i);
    auto tensor_info = ti.GetTensorTypeAndShapeInfo();
    ONNXTensorElementDataType et = tensor_info.GetElementType();
    std::vector<int64_t> dims = tensor_info.GetShape();

    std::cout << "    [" << i << "] name=\"" << output_names.back()
              << "\" type=" << TypeName(et)
              << " shape=" << ShapeToString(dims) << "\n";
  }

  // Build zero-filled dummy inputs (dynamic dims -> 1).
  Ort::MemoryInfo mem_info =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  std::vector<Ort::Value> input_tensors;
  // Keep backing storage alive until after Run().
  std::vector<std::vector<float>> input_buffers(num_inputs);
  std::vector<std::vector<int64_t>> concrete_shapes(num_inputs);

  bool all_float = true;
  for (size_t i = 0; i < num_inputs; ++i) {
    std::vector<int64_t> dims = input_shapes[i];
    int64_t elems = 1;
    for (auto& d : dims) {
      if (d < 0) d = 1;  // replace dynamic dim
      elems *= d;
    }
    concrete_shapes[i] = dims;

    if (input_types[i] != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      all_float = false;
      std::cout << "  [WARN] input \"" << input_names[i]
                << "\" is non-float (" << TypeName(input_types[i])
                << "); dummy tensor will still be zero-filled float and may"
                   " mismatch.\n";
    }

    input_buffers[i].assign(static_cast<size_t>(elems), 0.0f);
    input_tensors.push_back(Ort::Value::CreateTensor<float>(
        mem_info, input_buffers[i].data(), input_buffers[i].size(),
        concrete_shapes[i].data(), concrete_shapes[i].size()));

    std::cout << "  dummy input \"" << input_names[i]
              << "\" -> " << ShapeToString(concrete_shapes[i])
              << " (" << elems << " elems)\n";
  }
  (void)all_float;

  // Prepare char* name arrays for Run().
  std::vector<const char*> in_ptrs, out_ptrs;
  for (auto& n : input_names) in_ptrs.push_back(n.c_str());
  for (auto& n : output_names) out_ptrs.push_back(n.c_str());

  try {
    auto outputs = session.Run(Ort::RunOptions{nullptr}, in_ptrs.data(),
                               input_tensors.data(), input_tensors.size(),
                               out_ptrs.data(), out_ptrs.size());
    std::cout << "  RAN OK. Produced output shapes:\n";
    for (size_t i = 0; i < outputs.size(); ++i) {
      if (outputs[i].IsTensor()) {
        auto info = outputs[i].GetTensorTypeAndShapeInfo();
        std::cout << "    [" << i << "] \"" << output_names[i]
                  << "\" type=" << TypeName(info.GetElementType())
                  << " shape=" << ShapeToString(info.GetShape()) << "\n";
      } else {
        std::cout << "    [" << i << "] \"" << output_names[i]
                  << "\" (non-tensor output)\n";
      }
    }
  } catch (const Ort::Exception& e) {
    std::cout << "  [ERROR] session.Run failed: " << e.what() << "\n";
    std::cout << "RESULT: FAIL (" << model_path << ")\n";
    return false;
  }

  std::cout << "RESULT: PASS (" << model_path << ")\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = (argc > 1) ? argv[1] : "models/dfn3";
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';

  std::cout << "dfn3-probe: ONNX Runtime " << Ort::GetVersionString() << "\n";
  std::cout << "models dir: " << dir << "\n";

  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "dfn3-probe");

  const char* models[] = {"enc.onnx", "erb_dec.onnx", "df_dec.onnx"};
  int passed = 0;
  const int total = 3;
  for (const char* m : models) {
    if (ProbeModel(env, dir + m)) ++passed;
  }

  std::cout << "\n================================================================\n";
  std::cout << "SUMMARY: " << passed << "/" << total << " models passed\n";
  std::cout << "================================================================\n";
  return (passed == total) ? 0 : 1;
}
