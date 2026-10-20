// Which user-data tables and blocks a no-fallback shader variant still reads
// once its dead code is gone. The translated module goes through
// the SPIRV-Tools optimizer's performance passes (what spirv-opt -O runs). A
// table at user-data slot n is read when the storage buffer bound over it
// (BufferBinding::pointer, path user_sgpr[n]) is still referenced, or when the
// params block's user_sgpr[n] reaches an address - the pointer dereferenced for
// a page-table walk through the table. The renderer leaves the other tables out
// of the host ring.
//
// Reading user_sgpr[n] is not by itself a reason to build the table. A shader
// whose register allocator reused s[n] as scratch keeps the original user-data
// value alive in a phi the translator cannot prove dead, and then uses it as
// data: the pilot 8747a367+a22c7f71 bitcasts user_sgpr[12] and [13] to float
// and multiplies them, and nothing else touches them, so its 272-dword texture
// table was being rebuilt every draw for a value never dereferenced. The value
// itself already differs from the guest's (we hand the shader the host ring's
// address when we do place a table), so a use that is not an address cannot be
// depending on it.
//
// The taint below over-approximates on purpose - an unmodelled instruction
// taints its result, and a value reaching any address operand counts - so the
// error is always towards keeping a table, never dropping a live one.
#include "host/gpu_internal.h"

#ifdef BBHOST_HAVE_SPIRV_OPT
#include <spirv-tools/optimizer.hpp>
#endif

#include <cstdint>
#include <map>
#include <vector>

namespace gpu {

bool table_slots_read(const gcn::TranslateResult& meta, std::uint32_t& mask, std::uint32_t* any) {
    mask = ~0u;
    if (any) *any = ~0u;
#ifndef BBHOST_HAVE_SPIRV_OPT
    (void)meta;
    return false;
#else
    if (meta.spirv.size() < 5) return false;
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
    optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char*) {});
    optimizer.RegisterPerformancePasses();
    std::vector<std::uint32_t> words;
    if (!optimizer.Run(meta.spirv.data(), meta.spirv.size(), &words) || words.size() < 5) return false;

    enum : std::uint32_t {
        kOpFunctionCall = 57, kOpConstant = 43, kOpLoad = 61, kOpStore = 62, kOpCopyMemory = 63,
        kOpAccessChain = 65, kOpInBoundsAccessChain = 66, kOpDecorate = 71, kDecBinding = 33,
    };
    std::map<std::uint32_t, std::uint32_t> binding_of;  // variable -> binding
    std::map<std::uint32_t, std::uint32_t> constant;    // id -> value
    std::vector<std::size_t> starts;
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || i + count > words.size()) return false;
        if (op == kOpDecorate && count >= 4 && words[i + 2] == kDecBinding) binding_of[words[i + 1]] = words[i + 3];
        if (op == kOpConstant && count == 4) constant[words[i + 2]] = words[i + 3];
        starts.push_back(i);
        i += count;
    }
    std::uint32_t params = 0;
    std::map<std::uint32_t, int> table_var;  // storage-buffer variable -> user-data slot of the table it is bound over
    for (const auto& [var, binding] : binding_of) {
        if (binding == gcn::kBindingParams) params = var;
        const gcn::BufferBinding* found = nullptr;  // buffers bind from the translation's cb_binding_base
        for (const gcn::BufferBinding& mb : meta.buffers) {
            if (mb.binding == binding) found = &mb;
        }
        if (!found) continue;
        const gcn::BufferBinding& b = *found;
        if (b.pointer && b.path.immediate && b.path.user_sgpr >= 0 && b.path.user_sgpr + 1 < 16) table_var[var] = b.path.user_sgpr;
    }

    std::uint32_t read = 0;
    const auto value = [&](std::uint32_t id, std::uint32_t& v) {
        const auto it = constant.find(id);
        if (it == constant.end()) return false;
        v = it->second;
        return true;
    };
    // A storage buffer bound over a table, used at all, is the table being read.
    // Access chains that name one user_sgpr dword: the bits each one stands for,
    // charged only if the value loaded through it reaches an address.
    std::map<std::uint32_t, std::uint32_t> chain_bits;
    const auto table_use = [&](std::uint32_t id, std::size_t i, std::uint32_t count, std::uint32_t op) {
        if (const auto it = table_var.find(id); it != table_var.end()) read |= 3u << it->second;
        if (!params || id != params) return;
        // params . member . row . column: user_sgpr is member 2, 16 dwords in uvec4 rows.
        std::uint32_t member = 0, row = 0, column = 0;
        if ((op != kOpAccessChain && op != kOpInBoundsAccessChain) || count < 5 || !value(words[i + 4], member)) {
            read |= 0xffff;
        } else if (member != 2) {
        } else if (count < 6 || !value(words[i + 5], row) || row > 3) {
            read |= 0xffff;
        } else if (count < 7 || !value(words[i + 6], column) || column > 3) {
            read |= 0xfu << (row * 4);
        } else {
            chain_bits[words[i + 2]] |= 1u << (row * 4 + column);
        }
    };
    for (const std::size_t i : starts) {
        const std::uint32_t count = words[i] >> 16, op = words[i] & 0xffff;
        if ((op == kOpAccessChain || op == kOpInBoundsAccessChain || op == kOpLoad) && count >= 4) {
            table_use(words[i + 3], i, count, op);
        } else if (op == kOpStore && count >= 3) {
            table_use(words[i + 1], i, count, op);
        } else if (op == kOpCopyMemory && count >= 3) {
            table_use(words[i + 2], i, count, op);
        } else if (op == kOpFunctionCall) {
            for (std::uint32_t k = 4; k < count; ++k) table_use(words[i + k], i, count, op);
        }
    }

    // The value loaded through each of those chains, and everything computed
    // from it. Any other use of the chain pointer itself is unmodelled.
    enum : std::uint32_t { kOpFunction = 54, kOpFunctionEnd = 56 };
    std::vector<std::size_t> body;  // only function bodies hold values
    for (bool in_body = false; const std::size_t i : starts) {
        const std::uint32_t op = words[i] & 0xffff;
        if (op == kOpFunction) in_body = true;
        if (in_body) body.push_back(i);
        if (op == kOpFunctionEnd) in_body = false;
    }
    std::map<std::uint32_t, std::uint32_t> taint;
    for (const std::size_t i : body) {
        const std::uint32_t count = words[i] >> 16, op = words[i] & 0xffff;
        for (std::uint32_t k = 1; k < count; ++k) {
            // k == 2 of an access chain is the chain's own result id, not a use
            // of it; without this every chain reported itself as used.
            if (k == 2 && (op == kOpAccessChain || op == kOpInBoundsAccessChain)) continue;
            const auto it = chain_bits.find(words[i + k]);
            if (it == chain_bits.end()) continue;
            if (op == kOpLoad && k == 3 && count >= 4) {
                taint[words[i + 2]] |= it->second;
            } else {
                read |= it->second;
            }
        }
    }
    for (bool again = true; again;) {
        again = false;
        for (const std::size_t i : body) {
            const std::uint32_t count = words[i] >> 16, op = words[i] & 0xffff;
            if (count < 3 || op == kOpStore || op == kOpCopyMemory || op == kOpDecorate) continue;
            std::uint32_t bits = 0;
            for (std::uint32_t k = 1; k < count; ++k) {
                if (k == 2) continue;  // the result id itself
                if (const auto it = taint.find(words[i + k]); it != taint.end()) bits |= it->second;
            }
            if (!bits) continue;
            std::uint32_t& have = taint[words[i + 2]];
            if ((have | bits) != have) {
                have |= bits;
                again = true;
            }
        }
    }

    // A tainted value reaching an address - a pointer operand, or an index that
    // computes one - is the table being dereferenced. Data operands are not:
    // OpStore's object and image coordinates cannot reach the table's contents.
    const auto address_use = [&](std::uint32_t id) {
        if (const auto it = taint.find(id); it != taint.end()) read |= it->second;
    };
    for (const std::size_t i : body) {
        const std::uint32_t count = words[i] >> 16, op = words[i] & 0xffff;
        if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 4) {
            for (std::uint32_t k = 3; k < count; ++k) address_use(words[i + k]);
        } else if (op == kOpLoad && count >= 4) {
            address_use(words[i + 3]);
        } else if (op == kOpStore && count >= 3) {
            address_use(words[i + 1]);
        } else if (op == kOpCopyMemory && count >= 3) {
            address_use(words[i + 1]);
            address_use(words[i + 2]);
        } else if (op == kOpFunctionCall) {
            for (std::uint32_t k = 4; k < count; ++k) address_use(words[i + k]);
        }
    }
    mask = read;
    if (any) {
        // What the taint above leaves out on purpose: a user-data dword read
        // as data. Every dword a chain names counts, loaded or not.
        std::uint32_t touched = read;
        for (const auto& [chain, bits] : chain_bits) touched |= bits;
        *any = touched;
    }
    return true;
#endif
}

}  // namespace gpu
