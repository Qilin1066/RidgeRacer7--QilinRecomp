#include "spirv.hpp"

#include <bit>
#include <cstring>
#include <stdexcept>

namespace qilin::spirv {

Module::Module() {
    glsl_ = id();
}

void Module::put(std::vector<uint32_t>& to, uint16_t opcode, std::span<const uint32_t> operands) {
    to.push_back((static_cast<uint32_t>(operands.size() + 1) << 16) | opcode);
    to.insert(to.end(), operands.begin(), operands.end());
}

std::vector<uint32_t> Module::text(std::string_view s) {
    std::vector<uint32_t> w((s.size() + 4) / 4, 0);  // with its zero, in whole words
    std::memcpy(w.data(), s.data(), s.size());  // (SPIR-V's words are little-endian: the hosts are)
    return w;
}

uint32_t Module::cached(uint16_t opcode, std::initializer_list<uint32_t> operands) {
    std::vector<uint32_t> key{opcode};
    key.insert(key.end(), operands.begin(), operands.end());
    if (const auto it = cache_.find(key); it != cache_.end()) return it->second;
    const uint32_t result = id();
    std::vector<uint32_t> all;
    // A type's result id comes first; a constant's after its type, which `operands` begins with.
    const bool is_constant = opcode == OpConstant || opcode == OpConstantComposite || opcode == OpConstantTrue || opcode == OpConstantFalse;
    if (is_constant) {
        all.push_back(*operands.begin());
        all.push_back(result);
        all.insert(all.end(), operands.begin() + 1, operands.end());
    } else {
        all.push_back(result);
        all.insert(all.end(), operands.begin(), operands.end());
    }
    put(globals_, opcode, all);
    cache_.emplace(std::move(key), result);
    return result;
}

uint32_t Module::type_void() { return cached(OpTypeVoid, {}); }
uint32_t Module::type_bool() { return cached(OpTypeBool, {}); }
uint32_t Module::type_int(bool is_signed) { return cached(OpTypeInt, {32, is_signed ? 1u : 0u}); }
uint32_t Module::type_float() { return cached(OpTypeFloat, {32}); }
uint32_t Module::type_vector(uint32_t component, uint32_t count) { return cached(OpTypeVector, {component, count}); }
uint32_t Module::type_array(uint32_t element, uint32_t length) { return cached(OpTypeArray, {element, constant_uint(length)}); }
uint32_t Module::type_struct(std::initializer_list<uint32_t> members) {
    const uint32_t result = id();
    std::vector<uint32_t> all{result};
    all.insert(all.end(), members.begin(), members.end());
    put(globals_, OpTypeStruct, all);
    return result;
}
uint32_t Module::type_pointer(uint32_t storage, uint32_t type) { return cached(OpTypePointer, {storage, type}); }
uint32_t Module::type_function(uint32_t result, std::initializer_list<uint32_t> parameters) {
    if (parameters.size() != 0) throw std::logic_error("spirv: functions with parameters aren't written");
    return cached(OpTypeFunction, {result});
}
// A float image of `dim`, sampled (Sampled = 1), not arrayed, not multisampled, of an unknown format.
uint32_t Module::type_image(uint32_t dim) { return cached(OpTypeImage, {type_float(), dim, 0, 0, 0, 1, 0}); }
uint32_t Module::type_sampled_image(uint32_t image) { return cached(OpTypeSampledImage, {image}); }

uint32_t Module::constant_float(float value) { return cached(OpConstant, {type_float(), std::bit_cast<uint32_t>(value)}); }
uint32_t Module::constant_int(int32_t value) { return cached(OpConstant, {type_int(), static_cast<uint32_t>(value)}); }
uint32_t Module::constant_uint(uint32_t value) { return cached(OpConstant, {type_int(false), value}); }
uint32_t Module::constant_bool(bool value) { return cached(value ? OpConstantTrue : OpConstantFalse, {type_bool()}); }
uint32_t Module::constant_vec4(float x, float y, float z, float w) {
    return constant_composite(type_vec4(), {constant_float(x), constant_float(y), constant_float(z), constant_float(w)});
}
uint32_t Module::constant_composite(uint32_t type, std::initializer_list<uint32_t> parts) {
    std::vector<uint32_t> operands{type};
    operands.insert(operands.end(), parts.begin(), parts.end());
    // (cached takes an initializer_list; a composite has at most four parts here)
    switch (operands.size()) {
    case 3: return cached(OpConstantComposite, {operands[0], operands[1], operands[2]});
    case 4: return cached(OpConstantComposite, {operands[0], operands[1], operands[2], operands[3]});
    case 5: return cached(OpConstantComposite, {operands[0], operands[1], operands[2], operands[3], operands[4]});
    default: throw std::logic_error("spirv: a composite constant of an unexpected size");
    }
}

uint32_t Module::spec_constant_int(uint32_t spec_id, int32_t default_value) {
    const uint32_t result = id();
    const uint32_t operands[] = {type_int(), result, static_cast<uint32_t>(default_value)};
    put(globals_, OpSpecConstant, operands);
    decorate(result, kDecorationSpecId, {spec_id});
    return result;
}

void Module::capability(uint32_t c) {
    for (size_t i = 0; i < capabilities_.size(); ++i)
        if (capabilities_[i] == c) return;
    capabilities_.push_back(c);
}

uint32_t Module::variable(uint32_t storage, uint32_t type) {
    const uint32_t result = id();
    const uint32_t operands[] = {type_pointer(storage, type), result, storage};
    put(globals_, OpVariable, operands);
    return result;
}

void Module::name(uint32_t target, std::string_view t) {
    std::vector<uint32_t> operands{target};
    const std::vector<uint32_t> s = text(t);
    operands.insert(operands.end(), s.begin(), s.end());
    put(debug_, OpName, operands);
}

void Module::decorate(uint32_t target, uint32_t decoration, std::initializer_list<uint32_t> operands) {
    std::vector<uint32_t> all{target, decoration};
    all.insert(all.end(), operands.begin(), operands.end());
    put(annotations_, OpDecorate, all);
}
void Module::decorate_member(uint32_t type, uint32_t member, uint32_t decoration, std::initializer_list<uint32_t> operands) {
    std::vector<uint32_t> all{type, member, decoration};
    all.insert(all.end(), operands.begin(), operands.end());
    put(annotations_, OpMemberDecorate, all);
}

void Module::entry_point(uint32_t model, uint32_t function, std::string_view entry_name, std::span<const uint32_t> interface) {
    std::vector<uint32_t> all{model, function};
    const std::vector<uint32_t> s = text(entry_name);
    all.insert(all.end(), s.begin(), s.end());
    all.insert(all.end(), interface.begin(), interface.end());
    put(entries_, OpEntryPoint, all);
}
void Module::execution_mode(uint32_t function, uint32_t mode) {
    const uint32_t operands[] = {function, mode};
    put(modes_, OpExecutionMode, operands);
}

uint32_t Module::begin_function(uint32_t result_type, uint32_t function_type) {
    if (in_function_) throw std::logic_error("spirv: a function inside a function");
    in_function_ = true;
    first_block_ = true;
    const uint32_t result = id();
    const uint32_t operands[] = {result_type, result, kFunctionControlNone, function_type};
    put(function_head_, OpFunction, operands);
    return result;
}
uint32_t Module::local(uint32_t type) {
    const uint32_t result = id();
    const uint32_t operands[] = {type_pointer(kStorageFunction, type), result, kStorageFunction};
    put(function_locals_, OpVariable, operands);
    return result;
}
uint32_t Module::op(uint16_t opcode, uint32_t type, std::initializer_list<uint32_t> operands) {
    return op(opcode, type, std::span<const uint32_t>(operands.begin(), operands.size()));
}
uint32_t Module::op(uint16_t opcode, uint32_t type, std::span<const uint32_t> operands) {
    const uint32_t result = id();
    std::vector<uint32_t> all{type, result};
    all.insert(all.end(), operands.begin(), operands.end());
    put(function_body_, opcode, all);
    return result;
}
void Module::emit(uint16_t opcode, std::initializer_list<uint32_t> operands) { emit(opcode, std::span<const uint32_t>(operands.begin(), operands.size())); }
void Module::emit(uint16_t opcode, std::span<const uint32_t> operands) { put(function_body_, opcode, operands); }
uint32_t Module::ext(uint32_t type, uint32_t instruction, std::initializer_list<uint32_t> operands) {
    std::vector<uint32_t> all{glsl_, instruction};
    all.insert(all.end(), operands.begin(), operands.end());
    return op(OpExtInst, type, all);
}
void Module::label(uint32_t label_id) {
    // The function's first block: its locals come straight after its label.
    if (first_block_) {
        const uint32_t operands[] = {label_id};
        put(function_head_, OpLabel, operands);
        first_block_ = false;
        return;
    }
    const uint32_t operands[] = {label_id};
    put(function_body_, OpLabel, operands);
}
void Module::end_function() {
    put(function_body_, OpFunctionEnd, {});
    functions_.insert(functions_.end(), function_head_.begin(), function_head_.end());
    functions_.insert(functions_.end(), function_locals_.begin(), function_locals_.end());
    functions_.insert(functions_.end(), function_body_.begin(), function_body_.end());
    function_head_.clear(), function_locals_.clear(), function_body_.clear();
    in_function_ = false;
}

std::vector<uint32_t> Module::words() const {
    std::vector<uint32_t> w{0x07230203u, 0x00010300u /* SPIR-V 1.3 */, 0 /* generator */, next_, 0};
    const uint32_t capability[] = {kCapabilityShader};
    put(w, OpCapability, capability);
    for (const uint32_t c : capabilities_) {
        const uint32_t more[] = {c};
        put(w, OpCapability, more);
    }
    std::vector<uint32_t> import{glsl_};
    const std::vector<uint32_t> s = text("GLSL.std.450");
    import.insert(import.end(), s.begin(), s.end());
    put(w, OpExtInstImport, import);
    const uint32_t model[] = {kAddressingLogical, kMemoryModelGlsl450};
    put(w, OpMemoryModel, model);
    for (const auto* part : {&entries_, &modes_, &debug_, &annotations_, &globals_, &functions_}) w.insert(w.end(), part->begin(), part->end());
    return w;
}

}  // namespace qilin::spirv
