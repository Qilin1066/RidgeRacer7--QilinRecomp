// A small writer of SPIR-V modules: what the RSX programs' translation
// (rsx_spirv) emits, so the build needs no shader compiler.
//
// It knows only what the translation uses. The numbers (opcodes,
// enumerants, GLSL.std.450's instructions) are from Khronos' published
// grammar (SPIRV-Headers' spirv.core.grammar.json and
// extinst.glsl.std.450.grammar.json, at the tag the build's Vulkan headers
// are pinned to). Types and constants are made once each and shared.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::spirv {

// Opcodes.
enum Op : uint16_t {
    OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27,
    OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42,
    OpConstant = 43, OpConstantComposite = 44, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62,
    OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpImageSampleImplicitLod = 87, OpConvertFToS = 110, OpConvertSToF = 111, OpQuantizeToF16 = 116,
    OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpVectorTimesScalar = 142, OpDot = 148,
    OpAny = 154, OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpIEqual = 170, OpFOrdEqual = 180,
    OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188, OpFOrdGreaterThanEqual = 190,
    OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252,
    OpReturn = 253, OpUnreachable = 255,
};
// Enumerants.
enum : uint32_t {
    kCapabilityShader = 1,
    kAddressingLogical = 0, kMemoryModelGlsl450 = 1,
    kExecutionVertex = 0, kExecutionFragment = 4,
    kModeOriginUpperLeft = 7, kModeDepthReplacing = 12,
    kStorageUniformConstant = 0, kStorageInput = 1, kStorageUniform = 2, kStorageOutput = 3, kStorageFunction = 7, kStoragePushConstant = 9,
    kDecorationBlock = 2, kDecorationArrayStride = 6, kDecorationBuiltIn = 11, kDecorationNoPerspective = 13, kDecorationFlat = 14,
    kDecorationCentroid = 16, kDecorationLocation = 30, kDecorationBinding = 33, kDecorationDescriptorSet = 34, kDecorationOffset = 35,
    kBuiltInPosition = 0, kBuiltInPointSize = 1, kBuiltInFragCoord = 15, kBuiltInFrontFacing = 17, kBuiltInFragDepth = 22,
    kDim2D = 1, kDimCube = 3,
    kFunctionControlNone = 0, kSelectionControlNone = 0, kLoopControlNone = 0,
};
// GLSL.std.450's instructions.
enum Glsl : uint32_t {
    GlslRound = 1, GlslFAbs = 4, GlslFloor = 8, GlslFract = 10, GlslSin = 13, GlslCos = 14, GlslPow = 26, GlslExp2 = 29, GlslLog2 = 30,
    GlslSqrt = 31, GlslInverseSqrt = 32, GlslFMin = 37, GlslFMax = 40, GlslFClamp = 43, GlslFma = 50, GlslNormalize = 69,
};

class Module {
public:
    Module();

    uint32_t id() { return next_++; }

    // Types and constants: made once each.
    uint32_t type_void();
    uint32_t type_bool();
    uint32_t type_int(bool is_signed = true);
    uint32_t type_float();
    uint32_t type_vector(uint32_t component, uint32_t count);
    uint32_t type_vec4() { return type_vector(type_float(), 4); }
    uint32_t type_bvec4() { return type_vector(type_bool(), 4); }
    uint32_t type_ivec4() { return type_vector(type_int(), 4); }
    uint32_t type_array(uint32_t element, uint32_t length);  // decorated with its stride when `stride` is given to decorate_stride
    uint32_t type_struct(std::initializer_list<uint32_t> members);  // never shared: each block is its own
    uint32_t type_pointer(uint32_t storage, uint32_t type);
    uint32_t type_function(uint32_t result, std::initializer_list<uint32_t> parameters = {});
    uint32_t type_image(uint32_t dim);
    uint32_t type_sampled_image(uint32_t image);
    uint32_t constant_float(float value);
    uint32_t constant_int(int32_t value);
    uint32_t constant_uint(uint32_t value);
    uint32_t constant_bool(bool value);
    uint32_t constant_vec4(float x, float y, float z, float w);
    uint32_t constant_composite(uint32_t type, std::initializer_list<uint32_t> parts);

    // Globals, names, decorations, the entry point.
    uint32_t variable(uint32_t storage, uint32_t type);  // a global; its type is pointed to in that storage
    void name(uint32_t target, std::string_view text);
    void decorate(uint32_t target, uint32_t decoration, std::initializer_list<uint32_t> operands = {});
    void decorate_member(uint32_t type, uint32_t member, uint32_t decoration, std::initializer_list<uint32_t> operands = {});
    void entry_point(uint32_t model, uint32_t function, std::string_view entry_name, std::span<const uint32_t> interface);
    void execution_mode(uint32_t function, uint32_t mode);
    uint32_t glsl() const { return glsl_; }

    // A function's body. `op` writes an instruction with a result of `type` and returns its id; `emit` one with none.
    uint32_t begin_function(uint32_t result_type, uint32_t function_type);
    uint32_t local(uint32_t type);  // a Function-storage variable, put at the top of the function's first block
    uint32_t op(uint16_t opcode, uint32_t type, std::initializer_list<uint32_t> operands);
    uint32_t op(uint16_t opcode, uint32_t type, std::span<const uint32_t> operands);
    void emit(uint16_t opcode, std::initializer_list<uint32_t> operands);
    void emit(uint16_t opcode, std::span<const uint32_t> operands);
    uint32_t ext(uint32_t type, uint32_t instruction, std::initializer_list<uint32_t> operands);  // GLSL.std.450
    void label(uint32_t id);
    void end_function();

    // The module, as words.
    std::vector<uint32_t> words() const;

private:
    uint32_t cached(uint16_t opcode, std::initializer_list<uint32_t> operands);  // a type or constant: one id for one definition
    static void put(std::vector<uint32_t>& to, uint16_t opcode, std::span<const uint32_t> operands);
    static std::vector<uint32_t> text(std::string_view s);  // a literal string, as words

    uint32_t next_ = 1, glsl_ = 0;
    std::vector<uint32_t> entries_, modes_, debug_, annotations_, globals_, function_head_, function_locals_, function_body_, functions_;
    std::map<std::vector<uint32_t>, uint32_t> cache_;
    bool in_function_ = false, first_block_ = false;
};

}  // namespace qilin::spirv
