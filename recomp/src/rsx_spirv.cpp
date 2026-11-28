#include "rsx_spirv.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>

#include "bytes.hpp"
#include "spirv.hpp"

namespace qilin::rsx {

using namespace qilin::spirv;

namespace {

struct Failure {
    std::string why;
};
[[noreturn]] void refuse(std::string why) { throw Failure{std::move(why)}; }

constexpr unsigned kVertexConstants = 468;
constexpr uint32_t kUniformVariability = 0x1006;

// What the two translations share: the module, its common types, and the arithmetic on vec4s.
struct Common {
    Module m;
    uint32_t f32 = 0, vec2 = 0, vec3 = 0, vec4 = 0, bvec4 = 0, i32 = 0, ivec4 = 0, b = 0;
    uint32_t zero = 0, one = 0, zero4 = 0, one4 = 0, default_output = 0;

    Common() {
        f32 = m.type_float();
        vec2 = m.type_vector(f32, 2);
        vec3 = m.type_vector(f32, 3);
        vec4 = m.type_vec4();
        bvec4 = m.type_bvec4();
        b = m.type_bool();
        i32 = m.type_int();
        ivec4 = m.type_ivec4();
        zero = m.constant_float(0.0f);
        one = m.constant_float(1.0f);
        zero4 = m.constant_vec4(0, 0, 0, 0);
        one4 = m.constant_vec4(1, 1, 1, 1);
        default_output = m.constant_vec4(0, 0, 0, 1);
    }
    uint32_t shuffle(uint32_t v, const std::array<uint8_t, 4>& s) {
        if (s == std::array<uint8_t, 4>{0, 1, 2, 3}) return v;
        return m.op(OpVectorShuffle, vec4, {v, v, s[0], s[1], s[2], s[3]});
    }
    uint32_t splat(uint32_t scalar) { return m.op(OpCompositeConstruct, vec4, {scalar, scalar, scalar, scalar}); }
    uint32_t x(uint32_t v) { return m.op(OpCompositeExtract, f32, {v, 0}); }
    uint32_t xyz(uint32_t v) { return m.op(OpVectorShuffle, vec3, {v, v, 0, 1, 2}); }
    uint32_t dot3(uint32_t a, uint32_t c) { return splat(m.op(OpDot, f32, {xyz(a), xyz(c)})); }
    uint32_t dot4(uint32_t a, uint32_t c) { return splat(m.op(OpDot, f32, {a, c})); }
    uint32_t add(uint32_t a, uint32_t c) { return m.op(OpFAdd, vec4, {a, c}); }
    uint32_t mul(uint32_t a, uint32_t c) { return m.op(OpFMul, vec4, {a, c}); }
    uint32_t glsl(uint32_t instruction, uint32_t a) { return m.ext(vec4, instruction, {a}); }
    uint32_t glsl(uint32_t instruction, uint32_t a, uint32_t c) { return m.ext(vec4, instruction, {a, c}); }
    uint32_t saturate(uint32_t v) { return m.ext(vec4, GlslFClamp, {v, zero4, one4}); }
    // 1.0 where `condition` holds, 0.0 elsewhere.
    uint32_t set_if(uint16_t comparison, uint32_t a, uint32_t c) { return m.op(OpSelect, vec4, {m.op(comparison, bvec4, {a, c}), one4, zero4}); }
    // The condition codes' test for a condition (0 false to 7 true) on `cc`, component by component.
    uint32_t test(unsigned cond, uint32_t cc) {
        static constexpr uint16_t kCompare[] = {0, OpFOrdLessThan, OpFOrdEqual, OpFOrdLessThanEqual, OpFOrdGreaterThan, OpFUnordNotEqual, OpFOrdGreaterThanEqual, 0};
        if (cond == 0 || cond == 7) {
            const uint32_t c = m.constant_bool(cond == 7);
            return m.op(OpCompositeConstruct, bvec4, {c, c, c, c});
        }
        return m.op(kCompare[cond], bvec4, {cc, zero4});
    }
    // `value` into `var` for the components of `mask` (x = 8), and where `where` (a bvec4, or 0 for everywhere) holds.
    void write(uint32_t var, uint32_t value, unsigned mask, uint32_t where) {
        if (mask == 0) return;
        const uint32_t old = m.op(OpLoad, vec4, {var});
        uint32_t merged = value;
        if (mask != 0xf)
            merged = m.op(OpVectorShuffle, vec4, {old, value, (mask & 8) ? 4u : 0u, (mask & 4) ? 5u : 1u, (mask & 2) ? 6u : 2u, (mask & 1) ? 7u : 3u});
        if (where) merged = m.op(OpSelect, vec4, {where, merged, old});
        m.emit(OpStore, {var, merged});
    }
    uint32_t make_local(uint32_t init) {
        const uint32_t v = m.local(vec4);
        m.emit(OpStore, {v, init});
        return v;
    }
};

// ---------------------------------------------------------------- vertex programs

class VertexTranslator : Common {
public:
    VertexTranslator(const CgProgram& p, const Variant& variant) : variant_(variant) {
        for (size_t i = 0; i + 16 <= p.microcode.size(); i += 16) {
            uint32_t w[4];
            for (unsigned k = 0; k < 4; ++k) w[k] = read_be32(p.microcode.data() + i + 4 * k);
            insns_.push_back(decode_vp(std::span<const uint32_t, 4>(w, 4)));
        }
    }

    Translation run() {
        Translation out;
        // What the program uses: refused here, by name, if the translation doesn't take it.
        bool branches = false;
        for (size_t i = 0; i < insns_.size(); ++i) {
            const VpInsn& in = insns_[i];
            static const std::set<unsigned> kVector{kVecNop, kVecMov, kVecMul, kVecAdd, kVecMad, kVecDp3, kVecDph, kVecDp4, kVecMin, kVecMax,
                                                    kVecSlt, kVecSge, kVecSgt, kVecSle, kVecSeq, kVecSne, kVecArl, kVecFrc, kVecFlr};
            static const std::set<unsigned> kScalar{kScaNop, kScaMov, kScaRcp, kScaRsq, kScaEx2, kScaLg2, kScaBra};
            if (!kVector.contains(in.vec_op)) refuse(std::format("instruction {}: the vector operation {}", i, vp_vector_name(in.vec_op) ? vp_vector_name(in.vec_op) : "?"));
            if (!kScalar.contains(in.sca_op)) refuse(std::format("instruction {}: the scalar operation {}", i, vp_scalar_name(in.sca_op) ? vp_scalar_name(in.sca_op) : "?"));
            if (in.index_input) refuse(std::format("instruction {} indexes its input", i));
            if (in.sca_op == kScaBra) {
                branches = true;
                if (in.cond_test && (in.cond_swizzle[1] != in.cond_swizzle[0] || in.cond_swizzle[2] != in.cond_swizzle[0] || in.cond_swizzle[3] != in.cond_swizzle[0]))
                    refuse(std::format("instruction {}: a branch on the condition codes of more than one component", i));
                if (in.branch_target >= insns_.size()) refuse(std::format("instruction {}: a branch to {}, past the program's end", i, in.branch_target));
            }
            for (unsigned k = 0; k < 3; ++k) {
                if (in.src[k].type == VpRegister::input) inputs_ |= 1u << in.input;
                // The constants it reads (for the renderer to give it only those): any, if it indexes them.
                if (in.src[k].type == VpRegister::constant) {
                    if (in.index_const) constants_indexed_ = true;
                    constants_low_ = std::min(constants_low_, in.constant), constants_high_ = std::max(constants_high_, in.constant);
                }
            }
            if (in.vec_op && in.vec_op != kVecArl && in.vec_result && in.vec_mask) outputs_ |= 1u << in.dest;
            if (in.sca_op && in.sca_op != kScaBra && in.sca_result && in.sca_mask) outputs_ |= 1u << in.dest;
            // The clip planes' distances it writes: FOGC's (5) y, z, w for planes 0 to 2, PSZ's (6) for 3 to 5 (x = 8 in a mask).
            if (in.dest == 5 || in.dest == 6) {
                unsigned mask = 0;
                if (in.vec_op && in.vec_op != kVecArl && in.vec_result) mask |= in.vec_mask;
                if (in.sca_op && in.sca_op != kScaBra && in.sca_result) mask |= in.sca_mask;
                for (unsigned c = 1; c < 4; ++c)
                    if (mask & (8u >> c)) clip_planes_ |= 1u << ((in.dest - 5) * 3 + c - 1);
            }
        }
        // The interface.
        for (unsigned a = 0; a < 16; ++a)
            if (inputs_ & (1u << a)) {
                attributes_[a] = m.variable(kStorageInput, vec4);
                m.decorate(attributes_[a], kDecorationLocation, {a});
                interface_.push_back(attributes_[a]);
            }
        {
            const uint32_t array = m.type_array(vec4, kVertexConstants);
            m.decorate(array, kDecorationArrayStride, {16});
            const uint32_t block = m.type_struct({array});
            m.decorate(block, kDecorationBlock);
            m.decorate_member(block, 0, kDecorationOffset, {0});
            constants_ = m.variable(kStorageUniform, block);
            m.decorate(constants_, kDecorationDescriptorSet, {0});
            m.decorate(constants_, kDecorationBinding, {0});
            m.name(constants_, "c");
            const uint32_t push = m.type_struct({vec4, vec4});
            m.decorate(push, kDecorationBlock);
            m.decorate_member(push, 0, kDecorationOffset, {0});
            m.decorate_member(push, 1, kDecorationOffset, {16});
            push_ = m.variable(kStoragePushConstant, push);
        }
        position_ = m.variable(kStorageOutput, vec4);
        m.decorate(position_, kDecorationBuiltIn, {kBuiltInPosition});
        interface_.push_back(position_);
        constexpr unsigned kPointSize = 6;
        if (outputs_ & (1u << kPointSize)) {
            point_size_ = m.variable(kStorageOutput, f32);
            m.decorate(point_size_, kDecorationBuiltIn, {kBuiltInPointSize});
            interface_.push_back(point_size_);
        }
        for (unsigned o = 1; o < 17; ++o)
            if (o != kPointSize && (outputs_ & (1u << o))) {
                output_vars_[o] = m.variable(kStorageOutput, vec4);
                m.decorate(output_vars_[o], kDecorationLocation, {o});
                interface_.push_back(output_vars_[o]);
            }
        if (variant_.clip) {
            m.capability(kCapabilityClipDistance);
            clip_distance_ = m.variable(kStorageOutput, m.type_array(f32, 6));
            m.decorate(clip_distance_, kDecorationBuiltIn, {kBuiltInClipDistance});
            interface_.push_back(clip_distance_);
            for (unsigned n = 0; n < 6; ++n) clip_modes_[n] = m.spec_constant_int(n, 0);
        }

        const uint32_t fn = m.begin_function(m.type_void(), m.type_function(m.type_void()));
        m.label(m.id());
        for (unsigned t = 0; t < 32; ++t) temps_[t] = make_local(zero4);
        for (unsigned o = 0; o < 17; ++o) outputs_local_[o] = make_local(default_output);
        cc_[0] = make_local(zero4), cc_[1] = make_local(zero4);
        address_[0] = m.local(ivec4), address_[1] = m.local(ivec4);
        const uint32_t izero = m.constant_int(0);
        const uint32_t izero4 = m.constant_composite(ivec4, {izero, izero, izero, izero});
        m.emit(OpStore, {address_[0], izero4});
        m.emit(OpStore, {address_[1], izero4});

        if (!branches) {
            for (const VpInsn& in : insns_) instruction(in);
        } else {
            // A program with branches runs as a loop round a switch on its instruction's number.
            const uint32_t pc = m.local(i32), done = m.local(b);
            m.emit(OpStore, {pc, izero});
            m.emit(OpStore, {done, m.constant_bool(false)});
            const uint32_t header = m.id(), body = m.id(), select_merge = m.id(), continue_block = m.id(), merge = m.id();
            m.emit(OpBranch, {header});
            m.label(header);
            m.emit(OpLoopMerge, {merge, continue_block, kLoopControlNone});
            m.emit(OpBranch, {body});
            m.label(body);
            const uint32_t pc_now = m.op(OpLoad, i32, {pc});
            std::vector<uint32_t> cases;
            std::vector<uint32_t> switch_operands{pc_now, select_merge};
            for (size_t i = 0; i < insns_.size(); ++i) {
                cases.push_back(m.id());
                switch_operands.push_back(static_cast<uint32_t>(i));
                switch_operands.push_back(cases.back());
            }
            m.emit(OpSelectionMerge, {select_merge, kSelectionControlNone});
            m.emit(OpSwitch, switch_operands);
            for (size_t i = 0; i < insns_.size(); ++i) {
                m.label(cases[i]);
                const VpInsn& in = insns_[i];
                instruction(in);
                const uint32_t next = m.constant_int(static_cast<int32_t>(i + 1));
                uint32_t target = next;
                if (in.sca_op == kScaBra) {
                    const uint32_t jump = m.constant_int(static_cast<int32_t>(in.branch_target));
                    if (in.cond_test) {
                        const uint32_t cc = m.op(OpLoad, vec4, {cc_[in.cond_reg1 ? 1 : 0]});
                        const uint32_t taken = m.op(OpCompositeExtract, b, {test(in.cond, shuffle(cc, in.cond_swizzle)), 0});
                        target = m.op(OpSelect, i32, {taken, jump, next});
                    } else {
                        target = jump;
                    }
                }
                m.emit(OpStore, {pc, target});
                if (in.last && in.sca_op != kScaBra) m.emit(OpStore, {done, m.constant_bool(true)});
                m.emit(OpBranch, {select_merge});
            }
            m.label(select_merge);
            m.emit(OpBranch, {continue_block});
            m.label(continue_block);
            const uint32_t finished = m.op(OpLoad, b, {done});
            m.emit(OpBranchConditional, {finished, merge, header});
            m.label(merge);
        }
        // The outputs: the position, through the viewport's conversion; the rest as they are.
        {
            const uint32_t p = m.op(OpLoad, vec4, {outputs_local_[0]});
            const uint32_t ptr = m.type_pointer(kStoragePushConstant, vec4);
            const uint32_t xy = m.op(OpLoad, vec4, {m.op(OpAccessChain, ptr, {push_, m.constant_int(0)})});
            const uint32_t z = m.op(OpLoad, vec4, {m.op(OpAccessChain, ptr, {push_, m.constant_int(1)})});
            const uint32_t pw = m.op(OpCompositeExtract, f32, {p, 3});
            const auto part = [&](uint32_t value, unsigned vi, uint32_t scale_vec, unsigned si, uint32_t offset_vec, unsigned oi) {
                const uint32_t v = m.op(OpCompositeExtract, f32, {value, vi});
                const uint32_t s = m.op(OpCompositeExtract, f32, {scale_vec, si}), o = m.op(OpCompositeExtract, f32, {offset_vec, oi});
                return m.op(OpFAdd, f32, {m.op(OpFMul, f32, {v, s}), m.op(OpFMul, f32, {pw, o})});
            };
            const uint32_t clip = m.op(OpCompositeConstruct, vec4, {part(p, 0, xy, 0, xy, 2), part(p, 1, xy, 1, xy, 3), part(p, 2, z, 0, z, 1), pw});
            m.emit(OpStore, {position_, clip});
        }
        if (point_size_) m.emit(OpStore, {point_size_, x(m.op(OpLoad, vec4, {outputs_local_[kPointSize]}))});
        // The clip distances: plane n's as its test has it (kept where the distance written is 0 or more): GE the distance, LT
        // its negation (*a distance of exactly 0 is then kept: a stated difference*), off 1.
        if (clip_distance_) {
            const uint32_t ptr = m.type_pointer(kStorageOutput, f32);
            const uint32_t lt = m.constant_int(1), ge = m.constant_int(2);
            for (unsigned n = 0; n < 6; ++n) {
                const uint32_t reg = m.op(OpLoad, vec4, {outputs_local_[n < 3 ? 5 : 6]});
                const uint32_t d = m.op(OpCompositeExtract, f32, {reg, n % 3 + 1});
                const uint32_t is_ge = m.op(OpIEqual, b, {clip_modes_[n], ge}), is_lt = m.op(OpIEqual, b, {clip_modes_[n], lt});
                const uint32_t value = m.op(OpSelect, f32, {is_ge, d, m.op(OpSelect, f32, {is_lt, m.op(OpFNegate, f32, {d}), one})});
                m.emit(OpStore, {m.op(OpAccessChain, ptr, {clip_distance_, m.constant_int(static_cast<int32_t>(n))}), value});
            }
        }
        for (unsigned o = 1; o < 17; ++o)
            if (output_vars_[o]) m.emit(OpStore, {output_vars_[o], m.op(OpLoad, vec4, {outputs_local_[o]})});
        m.emit(OpReturn, {});
        m.end_function();
        m.entry_point(kExecutionVertex, fn, "main", interface_);

        out.spirv = m.words();
        out.inputs = inputs_;
        out.outputs = outputs_;
        out.clip_planes = clip_planes_;
        if (constants_indexed_) out.constants_first = 0, out.constants_end = kVertexConstants;
        else if (constants_low_ <= constants_high_) out.constants_first = constants_low_, out.constants_end = constants_high_ + 1;
        return out;
    }

private:
    uint32_t source(const VpInsn& in, unsigned k) {
        const VpSource& s = in.src[k];
        uint32_t v = 0;
        switch (s.type) {
        case VpRegister::temp: v = m.op(OpLoad, vec4, {temps_[s.temp & 31]}); break;
        case VpRegister::input: v = m.op(OpLoad, vec4, {attributes_[in.input]}); break;
        case VpRegister::constant: {
            uint32_t index = m.constant_int(static_cast<int32_t>(in.constant));
            if (in.index_const) {
                const uint32_t a = m.op(OpLoad, ivec4, {address_[in.addr_reg1 ? 1 : 0]});
                const uint32_t offset = m.op(OpCompositeExtract, i32, {a, in.addr_swizzle});
                index = m.op(OpIAdd, i32, {index, offset});
            }
            const uint32_t ptr = m.type_pointer(kStorageUniform, vec4);
            v = m.op(OpLoad, vec4, {m.op(OpAccessChain, ptr, {constants_, m.constant_int(0), index})});
            break;
        }
        default: refuse("a source of register type 0");
        }
        v = shuffle(v, s.swizzle);
        if (s.abs) v = glsl(GlslFAbs, v);
        if (s.negate) v = m.op(OpFNegate, vec4, {v});
        return v;
    }

    void instruction(const VpInsn& in) {
        // Both units read their sources before either writes.
        uint32_t vec = 0, sca = 0;
        if (in.vec_op != kVecNop) {
            const uint32_t a = source(in, 0);
            const auto bsrc = [&] { return source(in, 1); };
            switch (in.vec_op) {
            case kVecMov: vec = a; break;
            case kVecMul: vec = mul(a, bsrc()); break;
            case kVecAdd: vec = add(a, source(in, 2)); break;  // (its second source is in slot 2)
            case kVecMad: { const uint32_t c = bsrc(); vec = add(mul(a, c), source(in, 2)); break; }
            case kVecDp3: vec = dot3(a, bsrc()); break;
            case kVecDp4: vec = dot4(a, bsrc()); break;
            case kVecDph: { const uint32_t c = bsrc(); vec = m.op(OpFAdd, vec4, {dot3(a, c), splat(m.op(OpCompositeExtract, f32, {c, 3}))}); break; }
            case kVecMin: vec = glsl(GlslFMin, a, bsrc()); break;
            case kVecMax: vec = glsl(GlslFMax, a, bsrc()); break;
            case kVecSlt: vec = set_if(OpFOrdLessThan, a, bsrc()); break;
            case kVecSge: vec = set_if(OpFOrdGreaterThanEqual, a, bsrc()); break;
            case kVecSgt: vec = set_if(OpFOrdGreaterThan, a, bsrc()); break;
            case kVecSle: vec = set_if(OpFOrdLessThanEqual, a, bsrc()); break;
            case kVecSeq: vec = set_if(OpFOrdEqual, a, bsrc()); break;
            case kVecSne: vec = set_if(OpFUnordNotEqual, a, bsrc()); break;
            case kVecFlr: vec = glsl(GlslFloor, a); break;
            case kVecFrc: vec = m.op(OpFSub, vec4, {a, glsl(GlslFloor, a)}); break;
            case kVecArl: vec = glsl(GlslFloor, a); break;
            default: refuse("an unexpected vector operation");
            }
            if (in.saturate) vec = saturate(vec);
        }
        if (in.sca_op != kScaNop && in.sca_op != kScaBra) {
            const uint32_t s = x(source(in, 2));
            uint32_t r = 0;
            switch (in.sca_op) {
            case kScaMov: r = s; break;
            case kScaRcp: r = m.op(OpFDiv, f32, {one, s}); break;
            case kScaRsq: r = m.ext(f32, GlslInverseSqrt, {m.ext(f32, GlslFAbs, {s})}); break;
            case kScaEx2: r = m.ext(f32, GlslExp2, {s}); break;
            case kScaLg2: r = m.ext(f32, GlslLog2, {m.ext(f32, GlslFAbs, {s})}); break;
            default: refuse("an unexpected scalar operation");
            }
            sca = splat(r);
            if (in.saturate) sca = saturate(sca);
        }
        // The condition codes' test on the writes, and their update. *Stated choice: with both units at work the vector unit's
        // result sets them (in the game's two such instructions the vector half writes nothing else).*
        uint32_t where = 0;
        if (in.cond_test) where = test(in.cond, shuffle(m.op(OpLoad, vec4, {cc_[in.cond_reg1 ? 1 : 0]}), in.cond_swizzle));
        if (vec) {
            if (in.vec_op == kVecArl) {
                // The address register: whole numbers, in the components of the mask.
                const uint32_t var = address_[in.addr_reg1 ? 1 : 0];
                const uint32_t old = m.op(OpLoad, ivec4, {var});
                const uint32_t value = m.op(OpConvertFToS, ivec4, {vec});
                const unsigned mask = in.vec_mask;
                const uint32_t merged = m.op(OpVectorShuffle, ivec4, {old, value, (mask & 8) ? 4u : 0u, (mask & 4) ? 5u : 1u, (mask & 2) ? 6u : 2u, (mask & 1) ? 7u : 3u});
                m.emit(OpStore, {var, merged});
            } else {
                if (in.vec_result) write(outputs_local_[in.dest], vec, in.vec_mask, where);
                if (in.vec_temp != 0x3F) write(temps_[in.vec_temp & 31], vec, in.vec_mask, where);
            }
        }
        if (sca) {
            if (in.sca_result) write(outputs_local_[in.dest], sca, in.sca_mask, where);
            else write(temps_[in.sca_temp], sca, in.sca_mask, where);
        }
        if (in.cond_update) {
            const uint32_t result = vec ? vec : sca;
            const unsigned mask = vec ? in.vec_mask : in.sca_mask;
            if (result) write(cc_[in.cond_reg1 ? 1 : 0], result, mask, 0);
        }
    }

    Variant variant_;
    std::vector<VpInsn> insns_;
    uint32_t inputs_ = 0, outputs_ = 0, clip_planes_ = 0;
    unsigned constants_low_ = ~0u, constants_high_ = 0;
    bool constants_indexed_ = false;
    uint32_t clip_distance_ = 0;
    std::array<uint32_t, 6> clip_modes_{};  // the clip form's specialisation constants, one a plane
    std::array<uint32_t, 16> attributes_{};
    std::array<uint32_t, 17> output_vars_{}, outputs_local_{};
    std::array<uint32_t, 32> temps_{};
    std::array<uint32_t, 2> cc_{}, address_{};
    uint32_t constants_ = 0, push_ = 0, position_ = 0, point_size_ = 0;
    std::vector<uint32_t> interface_;
};

// ---------------------------------------------------------------- fragment programs

class FragmentTranslator : Common {
public:
    FragmentTranslator(const CgProgram& p, const Variant& variant) : p_(p), variant_(variant), insns_(decode_fp(p.microcode)) {}

    Translation run() {
        Translation out;
        // The header: the colour from H0 rather than R0; a depth written; a kill.
        const bool from_h0 = (p_.header[4] & 0xff) != 0;
        if (p_.header[5] != 0) refuse(std::format("its header asks for a depth written or a pixel killed ({:#x})", p_.header[5]));
        const uint32_t centroid = p_.header[4] >> 16;
        if (centroid) refuse(std::format("its header asks for centroid sampling of texture coordinates {:#x}", centroid));
        // Its texture units' kinds, from its parameters.
        std::map<unsigned, uint32_t> sampler_type;
        for (const CgParameter& c : p_.parameters)
            if (c.variability == kUniformVariability && c.resource >= 0x800 && c.resource <= 0x80f) sampler_type[c.resource - 0x800] = c.type;

        // What the program uses, checked; and the registers it writes in each view, to refuse a value crossing between them.
        uint32_t no_perspective = 0, perspective = 0;
        for (size_t i = 0; i < insns_.size(); ++i) {
            const FpInsn& in = insns_[i];
            if (in.branch) refuse(std::format("instruction {}: flow control", i));
            static const std::set<unsigned> kOps{kFpNop, kFpMov, kFpMul, kFpAdd, kFpMad, kFpDp3, kFpDp4, kFpMin, kFpMax, kFpFrc, kFpFlr, kFpRcp, kFpRsq,
                                                 kFpEx2, kFpLg2, kFpTex, kFpDiv, kFpDivsq, kFpNrm, kFpFenct, kFpFencb, kFpSlt, kFpSge, kFpSgt, kFpSle, kFpSeq, kFpSne};
            if (!kOps.contains(in.op)) refuse(std::format("instruction {}: the operation {}", i, fp_name(in.op) ? fp_name(in.op) : "?"));
            if (in.precision == 3) refuse(std::format("instruction {}: the precision s0.8", i));
            if (in.scale == 4) refuse(std::format("instruction {}: result scale 4", i));
            if (in.op == kFpNrm && (in.mask & 1)) refuse(std::format("instruction {}: NRM writing w", i));
            const unsigned reads = sources(in.op);
            for (unsigned k = 0; k < reads; ++k)
                if (in.src[k].type == FpRegister::input) {
                    if (in.input == 0 || in.input > 13) refuse(std::format("instruction {}: input register {}", i, in.input));
                    inputs_ |= 1u << in.input;
                    (in.no_perspective ? no_perspective : perspective) |= 1u << in.input;
                } else if (in.src[k].type == FpRegister::unknown) {
                    refuse(std::format("instruction {}: a source of register type 3", i));
                }
            if (in.op == kFpTex) {
                const auto it = sampler_type.find(in.tex_unit);
                if (it == sampler_type.end()) refuse(std::format("instruction {}: texture unit {}, which no parameter names", i, in.tex_unit));
                if (it->second != 0x42a && it->second != 0x42d) refuse(std::format("texture unit {} is of Cg type {:#x}", in.tex_unit, it->second));
                textures_ |= 1u << in.tex_unit;
                if (it->second == 0x42d) cubes_ |= 1u << in.tex_unit;
            }
        }
        if (no_perspective & perspective) refuse(std::format("inputs {:#x} are read with and without perspective", no_perspective & perspective));
        check_views();

        // The interface.
        static constexpr unsigned kLocation[14] = {0, 1, 2, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        for (unsigned n = 1; n < 14; ++n)
            if (inputs_ & (1u << n)) {
                input_vars_[n] = m.variable(kStorageInput, vec4);
                m.decorate(input_vars_[n], kDecorationLocation, {kLocation[n]});
                // (Flat, a colour isn't interpolated at all: with or without perspective is moot.)
                const auto interpolation = [&](uint32_t var) {
                    if (variant_.flat && (n == 1 || n == 2)) m.decorate(var, kDecorationFlat);
                    else if (no_perspective & (1u << n)) m.decorate(var, kDecorationNoPerspective);
                };
                interpolation(input_vars_[n]);
                interface_.push_back(input_vars_[n]);
                // Two-sided: the back colour, BFC0 or BFC1 (the vertex program's outputs 3 and 4), read on back faces.
                if (variant_.two_sided && (n == 1 || n == 2)) {
                    back_vars_[n] = m.variable(kStorageInput, vec4);
                    m.decorate(back_vars_[n], kDecorationLocation, {n + 2});
                    interpolation(back_vars_[n]);
                    interface_.push_back(back_vars_[n]);
                }
            }
        if (variant_.two_sided && (inputs_ & 6)) {
            front_facing_ = m.variable(kStorageInput, b);
            m.decorate(front_facing_, kDecorationBuiltIn, {kBuiltInFrontFacing});
            interface_.push_back(front_facing_);
        }
        size_t slots = 0;
        for (const FpInsn& in : insns_) slots += in.has_constant;
        if (slots) {
            const uint32_t array = m.type_array(vec4, static_cast<uint32_t>(slots));
            m.decorate(array, kDecorationArrayStride, {16});
            const uint32_t block = m.type_struct({array});
            m.decorate(block, kDecorationBlock);
            m.decorate_member(block, 0, kDecorationOffset, {0});
            constants_ = m.variable(kStorageUniform, block);
            m.decorate(constants_, kDecorationDescriptorSet, {0});
            m.decorate(constants_, kDecorationBinding, {1});
        }
        for (unsigned u = 0; u < 16; ++u)
            if (textures_ & (1u << u)) {
                const uint32_t type = m.type_sampled_image(m.type_image((cubes_ & (1u << u)) ? kDimCube : kDim2D));
                samplers_[u] = m.variable(kStorageUniformConstant, type);
                sampler_types_[u] = type;
                m.decorate(samplers_[u], kDecorationDescriptorSet, {1});
                m.decorate(samplers_[u], kDecorationBinding, {u});
            }
        // The alpha test's push constants, after the vertex program's 32 bytes.
        {
            const uint32_t push = m.type_struct({vec4, vec4});
            m.decorate(push, kDecorationBlock);
            m.decorate_member(push, 0, kDecorationOffset, {32});
            m.decorate_member(push, 1, kDecorationOffset, {48});
            push_ = m.variable(kStoragePushConstant, push);
        }
        // Colour outputs: 0 always; 1 to 3 if their registers are written.
        const std::array<std::pair<bool, unsigned>, 4> colour{{{from_h0, from_h0 ? 0u : 0u}, {from_h0, from_h0 ? 4u : 2u}, {from_h0, from_h0 ? 6u : 3u}, {from_h0, from_h0 ? 8u : 4u}}};
        for (unsigned t = 0; t < 4; ++t) {
            const bool written = colour[t].first ? written_h_.contains(colour[t].second) : written_r_.contains(colour[t].second);
            if (t == 0 || written) {
                colour_vars_[t] = m.variable(kStorageOutput, vec4);
                m.decorate(colour_vars_[t], kDecorationLocation, {t});
                interface_.push_back(colour_vars_[t]);
                outputs_ |= 1u << t;
            }
        }

        const uint32_t fn = m.begin_function(m.type_void(), m.type_function(m.type_void()));
        m.label(m.id());
        for (const unsigned r : used_r_) r_[r] = make_local(zero4);
        for (const unsigned h : used_h_) h_[h] = make_local(zero4);
        cc_ = make_local(zero4);
        // Two-sided: COL0 and COL1 as the face drawn has them, chosen once.
        if (front_facing_) {
            const uint32_t front = m.op(OpLoad, b, {front_facing_});
            const uint32_t where = m.op(OpCompositeConstruct, bvec4, {front, front, front, front});
            for (unsigned n = 1; n <= 2; ++n)
                if (back_vars_[n])
                    colours_[n] = make_local(m.op(OpSelect, vec4, {where, m.op(OpLoad, vec4, {input_vars_[n]}), m.op(OpLoad, vec4, {back_vars_[n]})}));
        }
        size_t slot = 0, at = 0;
        for (const FpInsn& in : insns_) {
            at += 16;
            if (in.has_constant) {
                out.constant_slots.push_back(static_cast<uint32_t>(at));
                at += 16;
            }
            instruction(in, in.has_constant ? slot++ : 0);
        }
        alpha_test(colour[0].first ? h_.at(colour[0].second) : r_.at(colour[0].second));
        for (unsigned t = 0; t < 4; ++t)
            if (colour_vars_[t]) {
                const uint32_t var = colour[t].first ? h_.at(colour[t].second) : r_.at(colour[t].second);
                m.emit(OpStore, {colour_vars_[t], m.op(OpLoad, vec4, {var})});
            }
        m.emit(OpReturn, {});
        m.end_function();
        m.entry_point(kExecutionFragment, fn, "main", interface_);
        m.execution_mode(fn, kModeOriginUpperLeft);

        out.spirv = m.words();
        out.inputs = inputs_;
        out.outputs = outputs_;
        out.textures = textures_;
        out.cube_textures = cubes_;
        return out;
    }

private:
    static unsigned sources(unsigned op) {
        switch (op) {
        case kFpNop: case kFpFenct: case kFpFencb: return 0;
        case kFpMov: case kFpFrc: case kFpFlr: case kFpRcp: case kFpRsq: case kFpEx2: case kFpLg2: case kFpTex: case kFpNrm: return 1;
        case kFpMad: return 3;
        default: return 2;
        }
    }
    static bool self_move(const FpInsn& in) {
        const FpSource& s = in.src[0];
        return in.op == kFpMov && s.type == FpRegister::temp && s.half == in.out_half && s.index == in.out && !s.abs && !s.negate &&
               s.swizzle == std::array<uint8_t, 4>{0, 1, 2, 3} && !in.saturate && in.scale == 0 && !in.cond_write && in.cond == 7 && in.precision == 0;
    }
    // The shared register file, as the two views see it: Rn's components x, y, z, w are its four quarters; H2n's x and y share
    // the first quarter and its z and w the second, H2n+1's the third and fourth. For each quarter, which view wrote it last;
    // a read of a quarter the other view wrote last is a value crossing between them, and refused. (A register moved to
    // itself in place is the same bits in either view, and is no crossing.)
    void check_views() {
        std::map<unsigned, std::array<char, 4>> last;  // by R register: 'R', 'H' or 0 for each quarter
        const auto quarters_r = [](unsigned component) { return component; };
        const auto quarters_h = [](unsigned h, unsigned component) { return (h % 2) * 2 + component / 2; };
        for (size_t i = 0; i < insns_.size(); ++i) {
            const FpInsn& in = insns_[i];
            if (self_move(in)) continue;
            // The positions of its sources the result's written components depend on.
            unsigned written = in.mask;  // x = 8
            if (in.no_dest && !in.cond_write) written = 0;
            unsigned positions = 0;  // bit 0 for x
            switch (in.op) {
            case kFpDp3: case kFpNrm: positions = written ? 0x7 : 0; break;
            case kFpDp4: positions = written ? 0xf : 0; break;
            case kFpRcp: case kFpRsq: case kFpEx2: case kFpLg2: positions = written ? 0x1 : 0; break;
            case kFpTex: positions = written ? ((cubes_ & (1u << in.tex_unit)) ? 0x7 : 0x3) : 0; break;
            default:
                for (unsigned c = 0; c < 4; ++c)
                    if (written & (8u >> c)) positions |= 1u << c;
                break;
            }
            for (unsigned k = 0; k < sources(in.op); ++k) {
                const FpSource& s = in.src[k];
                if (s.type != FpRegister::temp) continue;
                const unsigned reg = s.half ? s.index / 2 : s.index;
                (s.half ? used_h_ : used_r_).insert(s.index);
                const unsigned needed = (k == 1 && (in.op == kFpDiv || in.op == kFpDivsq)) ? (positions ? 0x1u : 0u) : positions;
                for (unsigned position = 0; position < 4; ++position) {
                    if (!(needed & (1u << position))) continue;
                    const uint8_t c = s.swizzle[position];
                    const unsigned q = s.half ? quarters_h(s.index, c) : quarters_r(c);
                    const char writer = last[reg][q];
                    if (writer && writer != (s.half ? 'H' : 'R'))
                        refuse(std::format("instruction {} reads {}{}.{}, last written as {}", i, s.half ? "H" : "R", s.index, "xyzw"[c], writer == 'H' ? "H" : "R"));
                }
            }
            if (!in.no_dest) {
                const unsigned reg = in.out_half ? in.out / 2 : in.out;
                (in.out_half ? used_h_ : used_r_).insert(in.out);
                (in.out_half ? written_h_ : written_r_).insert(in.out);
                for (unsigned c = 0; c < 4; ++c)
                    if (in.mask & (8u >> c)) last[reg][in.out_half ? quarters_h(in.out, c) : quarters_r(c)] = in.out_half ? 'H' : 'R';
            }
        }
        used_r_.insert(0), used_h_.insert(0);
    }

    uint32_t source(const FpInsn& in, unsigned k, size_t slot) {
        const FpSource& s = in.src[k];
        uint32_t v = 0;
        switch (s.type) {
        case FpRegister::temp: v = m.op(OpLoad, vec4, {s.half ? h_.at(s.index) : r_.at(s.index)}); break;
        case FpRegister::input: v = m.op(OpLoad, vec4, {in.input <= 2 && colours_[in.input] ? colours_[in.input] : input_vars_[in.input]}); break;
        case FpRegister::constant: {
            const uint32_t ptr = m.type_pointer(kStorageUniform, vec4);
            v = m.op(OpLoad, vec4, {m.op(OpAccessChain, ptr, {constants_, m.constant_int(0), m.constant_int(static_cast<int32_t>(slot))})});
            break;
        }
        default: refuse("a source of register type 3");
        }
        v = shuffle(v, s.swizzle);
        if (s.abs) v = glsl(GlslFAbs, v);
        if (s.negate) v = m.op(OpFNegate, vec4, {v});
        return v;
    }

    void instruction(const FpInsn& in, size_t slot) {
        if (in.op == kFpNop || in.op == kFpFenct || in.op == kFpFencb) return;  // *the fences order the RSX's work, and have no value: a stated reading*
        if (self_move(in)) return;
        const uint32_t a = source(in, 0, slot);
        const auto bsrc = [&] { return source(in, 1, slot); };
        uint32_t r = 0;
        switch (in.op) {
        case kFpMov: r = a; break;
        case kFpMul: r = mul(a, bsrc()); break;
        case kFpAdd: r = add(a, bsrc()); break;
        case kFpMad: { const uint32_t c = bsrc(); r = add(mul(a, c), source(in, 2, slot)); break; }
        case kFpDp3: r = dot3(a, bsrc()); break;
        case kFpDp4: r = dot4(a, bsrc()); break;
        case kFpMin: r = glsl(GlslFMin, a, bsrc()); break;
        case kFpMax: r = glsl(GlslFMax, a, bsrc()); break;
        case kFpSlt: r = set_if(OpFOrdLessThan, a, bsrc()); break;
        case kFpSge: r = set_if(OpFOrdGreaterThanEqual, a, bsrc()); break;
        case kFpSgt: r = set_if(OpFOrdGreaterThan, a, bsrc()); break;
        case kFpSle: r = set_if(OpFOrdLessThanEqual, a, bsrc()); break;
        case kFpSeq: r = set_if(OpFOrdEqual, a, bsrc()); break;
        case kFpSne: r = set_if(OpFUnordNotEqual, a, bsrc()); break;
        case kFpFrc: r = m.op(OpFSub, vec4, {a, glsl(GlslFloor, a)}); break;
        case kFpFlr: r = glsl(GlslFloor, a); break;
        case kFpRcp: r = splat(m.op(OpFDiv, f32, {one, x(a)})); break;
        case kFpRsq: r = splat(m.ext(f32, GlslInverseSqrt, {m.ext(f32, GlslFAbs, {x(a)})})); break;
        case kFpEx2: r = splat(m.ext(f32, GlslExp2, {x(a)})); break;
        case kFpLg2: r = splat(m.ext(f32, GlslLog2, {x(a)})); break;
        // DIV and DIVSQ divide by their second source's first component (or its square root), as a scalar: the game computes
        // a length with DP3 into one component and then divides a vector by it (DP3R R1.x, R2, R2; DIVSQR R3.xyz, R2, R1).
        case kFpDiv: r = m.op(OpFDiv, vec4, {a, splat(x(bsrc()))}); break;
        case kFpDivsq: r = m.op(OpFDiv, vec4, {a, splat(m.ext(f32, GlslSqrt, {x(bsrc())}))}); break;
        case kFpNrm: {
            const uint32_t n = m.ext(vec3, GlslNormalize, {xyz(a)});
            r = m.op(OpVectorShuffle, vec4, {n, zero4, 0, 1, 2, 3});
            break;
        }
        case kFpTex: {
            const uint32_t sampler = m.op(OpLoad, sampler_types_[in.tex_unit], {samplers_[in.tex_unit]});
            const bool cube = cubes_ & (1u << in.tex_unit);
            const uint32_t coord = cube ? xyz(a) : m.op(OpVectorShuffle, vec2, {a, a, 0, 1});
            r = m.op(OpImageSampleImplicitLod, vec4, {sampler, coord});
            break;
        }
        default: refuse("an unexpected operation");
        }
        // The result's scale, its saturation, then its precision. *Fixed point (s1.10) is taken as a clamp to [-2, 2 - 1/1024]
        // and a rounding to 1/1024: a stated reading, to be compared with the console's picture.*
        static constexpr float kScale[] = {1, 2, 4, 8, 1, 0.5f, 0.25f, 0.125f};
        if (in.scale) r = m.op(OpVectorTimesScalar, vec4, {r, m.constant_float(kScale[in.scale])});
        if (in.saturate) r = saturate(r);
        if (in.precision == 2) {
            const float top = 2.0f - 1.0f / 1024.0f;
            r = m.ext(vec4, GlslFClamp, {r, m.constant_vec4(-2, -2, -2, -2), m.constant_vec4(top, top, top, top)});
            const uint32_t k = m.constant_vec4(1024, 1024, 1024, 1024), inv = m.constant_vec4(1.0f / 1024, 1.0f / 1024, 1.0f / 1024, 1.0f / 1024);
            r = mul(glsl(GlslRound, mul(r, k)), inv);
        }
        if (in.precision == 1 || in.out_half) r = m.op(OpQuantizeToF16, vec4, {r});
        uint32_t where = 0;
        if (in.cond != 7 || in.cond_swizzle != std::array<uint8_t, 4>{0, 1, 2, 3})
            where = test(in.cond, shuffle(m.op(OpLoad, vec4, {cc_}), in.cond_swizzle));
        if (!in.no_dest) write(in.out_half ? h_.at(in.out) : r_.at(in.out), r, in.mask, where);
        if (in.cond_write) write(cc_, r, in.mask, 0);
    }

    const CgProgram& p_;
    Variant variant_;
    std::vector<FpInsn> insns_;
    uint32_t inputs_ = 0, outputs_ = 0, textures_ = 0, cubes_ = 0, constants_ = 0, cc_ = 0, push_ = 0;

    // The alpha test, on colour 0's alpha (the RSX's is fixed function; here the renderer's push constants describe it, so
    // one translation serves with the test on and off): {reference, scale, -, -} and {less, equal, greater, -}, each 1 if
    // a value on that side of the reference passes. With a scale, the alpha is first clamped to [0, 1] and rounded to that
    // many steps, as an 8-bit surface would hold it (*a stated choice: the test on the value as stored*). A failing
    // fragment is discarded. With the test off, all three pass.
    void alpha_test(uint32_t colour) {
        const uint32_t ptr = m.type_pointer(kStoragePushConstant, vec4);
        const uint32_t p0 = m.op(OpLoad, vec4, {m.op(OpAccessChain, ptr, {push_, m.constant_int(0)})});
        const uint32_t p1 = m.op(OpLoad, vec4, {m.op(OpAccessChain, ptr, {push_, m.constant_int(1)})});
        const auto part = [&](uint32_t v, unsigned i) { return m.op(OpCompositeExtract, f32, {v, i}); };
        const uint32_t a = part(m.op(OpLoad, vec4, {colour}), 3), reference = part(p0, 0), scale = part(p0, 1);
        const uint32_t stored = m.ext(f32, GlslRound, {m.op(OpFMul, f32, {m.ext(f32, GlslFClamp, {a, zero, one}), scale})});
        const uint32_t value = m.op(OpSelect, f32, {m.op(OpFOrdGreaterThan, b, {scale, zero}), stored, a});
        const uint32_t half = m.constant_float(0.5f);
        const auto fails = [&](Op compare, unsigned i) {
            return m.op(OpLogicalAnd, b, {m.op(compare, b, {value, reference}), m.op(OpFOrdLessThan, b, {part(p1, i), half})});
        };
        const uint32_t killed = m.op(OpLogicalOr, b, {m.op(OpLogicalOr, b, {fails(OpFOrdLessThan, 0), fails(OpFOrdEqual, 1)}), fails(OpFOrdGreaterThan, 2)});
        const uint32_t kill = m.id(), merge = m.id();
        m.emit(OpSelectionMerge, {merge, kSelectionControlNone});
        m.emit(OpBranchConditional, {killed, kill, merge});
        m.label(kill);
        m.emit(OpKill, {});
        m.label(merge);
    }
    std::set<unsigned> used_r_, used_h_, written_r_, written_h_;
    std::map<unsigned, uint32_t> r_, h_;
    std::array<uint32_t, 16> input_vars_{}, samplers_{}, sampler_types_{};
    std::array<uint32_t, 3> back_vars_{}, colours_{};  // the two-sided form's: BFC0 and BFC1, and COL0 and COL1 as the face has them
    uint32_t front_facing_ = 0;
    std::array<uint32_t, 4> colour_vars_{};
    std::vector<uint32_t> interface_;
};

}  // namespace

std::expected<Translation, std::string> translate(const CgProgram& program, const Variant& variant) {
    try {
        if (program.vertex) {
            if (variant.flat || variant.two_sided) refuse("a flat or two-sided form of a vertex program");
            return VertexTranslator(program, variant).run();
        }
        if (variant.clip) refuse("a clip form of a fragment program");
        return FragmentTranslator(program, variant).run();
    } catch (const Failure& f) {
        return std::unexpected(std::format("the {} program at {:#x}: {}", program.vertex ? "vertex" : "fragment", program.address, f.why));
    }
}

uint64_t program_key(const CgProgram& program) {
    std::vector<uint8_t> bytes = program.microcode;
    if (!program.vertex) {
        size_t at = 0;
        for (const FpInsn& in : decode_fp(program.microcode)) {
            at += 16;
            if (in.has_constant) {
                std::fill(bytes.begin() + static_cast<ptrdiff_t>(at), bytes.begin() + static_cast<ptrdiff_t>(at + 16), uint8_t{0});
                at += 16;
            }
        }
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint8_t c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

}  // namespace qilin::rsx
