#pragma once

#include "transcribe/inference.hpp"

namespace transcribe::detail {

class OnnxSession {
public:
    virtual ~OnnxSession() = default;
    virtual std::vector<Note> infer(const AudioBuffer&, const OnnxOptions&) = 0;
    virtual void describe(ModelMetadata&) const = 0;
};

std::unique_ptr<OnnxSession> open_onnx_session(
    const ModelDefinition&, const ModelLoadOptions&);
std::vector<ProviderAvailability> onnx_providers();
std::string onnx_runtime_version();

}
