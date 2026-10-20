// The engine's identities for a shader's resources: see names.h.
#include "gcn/names.h"

#include "gcn/container.h"

#include <cstring>

namespace gcn {

namespace {

struct Reader {
    const std::vector<std::uint8_t>& d;
    bool ok = true;
    std::uint32_t u32(std::size_t at) {
        if (at + 4 > d.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        return v;
    }
    std::uint16_t u16(std::size_t at) {
        if (at + 2 > d.size()) {
            ok = false;
            return 0;
        }
        std::uint16_t v;
        std::memcpy(&v, d.data() + at, 2);
        return v;
    }
};

}  // namespace

bool parse_reflection(const std::vector<std::uint8_t>& c, ShaderReflection& out, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    out = ShaderReflection{};
    ShaderCode code;
    if (!shader_code(c, code)) return fail("no shader code");
    if (code.type != 1 && code.type != 2) return fail("not a vertex or pixel shader container");
    std::size_t shdr = 0;
    while (shdr + 0x60 <= c.size() && shdr < 0x100 && std::memcmp(c.data() + shdr, "Shdr", 4) != 0) ++shdr;
    if (shdr + 0x60 > c.size() || shdr >= 0x100) return fail("no Shdr header");
    Reader r{c};
    // The Gnm header at Shdr + 0x10: the usage-slot count in the first word's
    // top byte, the embedded constant buffer's size in 16-byte units next; the
    // slots after the stage registers (a vertex shader's 0x28 bytes on, a
    // pixel shader's 0x3c), four bytes each.
    const std::size_t base = shdr + 16;
    const std::size_t nusage = c[base + 3];
    const std::size_t usage_at = base + (code.type == 1 ? 0x28 : 0x3c);
    if (usage_at + nusage * 4 > code.offset) return fail("usage slots past the code");
    for (std::size_t k = 0; k < nusage; ++k) {
        const std::uint8_t* u = c.data() + usage_at + 4 * k;
        out.usage.push_back({u[0], u[1], u[2], u[3]});
    }
    // The reflection: after the 0x1c-byte ShaderBinaryInfo and the embedded
    // constant buffer, a header of seven words - resources, 0, members,
    // samplers, inputs | outputs << 8, 0, the string table's bytes - then the
    // records and the strings, which end the container. Names are byte
    // offsets from the word that holds them.
    if (code.footer + 0x1c > c.size() || std::memcmp(c.data() + code.footer, "OrbShdr", 7) != 0) return fail("no OrbShdr footer");
    out.embedded_cb_bytes = static_cast<std::uint32_t>(r.u16(base + 4)) * 16;
    const std::size_t h = code.footer + 0x1c + out.embedded_cb_bytes;
    const std::uint32_t nres = r.u32(h), nel = r.u32(h + 8), nsmp = r.u32(h + 12), io = r.u32(h + 16), strsz = r.u32(h + 24);
    if (!r.ok || r.u32(h + 4) || r.u32(h + 20) || (io >> 16) || nres > 256 || nel > 4096 || nsmp > 256) return fail("no reflection header");
    const std::size_t nin = io & 0xff, nout = (io >> 8) & 0xff;
    const std::size_t res_at = h + 28, el_at = res_at + nres * 24, smp_at = el_at + nel * 36, in_at = smp_at + nsmp * 16;
    const std::size_t out_at = in_at + nin * 16, str_at = out_at + nout * 16;
    if (str_at + strsz != c.size()) return fail("the reflection's sections do not fill the container");
    bool names_ok = true;
    const auto name = [&](std::size_t field) -> std::string {
        const std::uint32_t rel = r.u32(field);
        if (!rel) return {};
        const std::size_t p = field + rel;
        if (p < str_at || p >= c.size()) {
            names_ok = false;
            return {};
        }
        const void* end = std::memchr(c.data() + p, 0, c.size() - p);
        if (!end) {
            names_ok = false;
            return {};
        }
        return std::string(reinterpret_cast<const char*>(c.data() + p), static_cast<const std::uint8_t*>(end) - (c.data() + p));
    };
    for (std::size_t k = 0; k < nres; ++k) {
        const std::size_t o = res_at + k * 24;
        ReflectedResource x;
        x.slot = r.u32(o);
        x.size = r.u32(o + 4);
        x.type = r.u32(o + 8);
        x.elements = r.u32(o + 12);
        x.first_element = r.u32(o + 16);
        x.name = name(o + 20);
        if (x.first_element + x.elements > nel) return fail("a resource's members past the member records");
        out.resources.push_back(std::move(x));
    }
    for (std::size_t k = 0; k < nel; ++k) {
        const std::size_t o = el_at + k * 36;
        ReflectedElement x;
        x.type = r.u32(o);
        x.offset = r.u32(o + 4);
        x.size = r.u32(o + 8);
        x.array = static_cast<std::int32_t>(r.u32(o + 16));
        x.name = name(o + 28);
        x.type_name = name(o + 32);
        out.elements.push_back(std::move(x));
    }
    for (std::size_t k = 0; k < nsmp; ++k) {
        const std::size_t o = smp_at + k * 16;
        out.samplers.push_back({r.u32(o + 8), name(o + 12)});
    }
    for (std::size_t k = 0; k < nin + nout; ++k) {
        const std::size_t o = k < nin ? in_at + k * 16 : out_at + (k - nin) * 16;
        ReflectedAttribute x;
        x.type = r.u32(o);
        x.index = r.u32(o + 4);
        x.name = name(o + 8);
        x.semantic = name(o + 12);
        (k < nin ? out.inputs : out.outputs).push_back(std::move(x));
    }
    if (!r.ok || !names_ok) return fail("a name outside the string table");
    return true;
}

namespace {

// The usage slot a resource path reaches, of one of `types`: a register of
// user data, a dword of the extended user-data table one load away, or an
// entry of a table of descriptors one load away (usage types 0x13 T#s, 0x15
// S#s, 0x16 V#s, by API slot from the table's own: eight dwords a T#, four an
// S# or a V#), whose slot is the table's plus the entry.
bool slot_for(const ShaderReflection& r, const ResourcePath& p, std::initializer_list<std::uint8_t> types, UsageSlot& out) {
    if (p.user_sgpr < 0 || !p.loads.empty()) return false;
    int reg = p.user_sgpr;
    if (!p.immediate) {
        if (p.final_vsharp) return false;
        for (const UsageSlot& u : r.usage) {
            if (u.start_reg != p.user_sgpr) continue;
            const std::uint8_t kind = u.type == kUsageTextureTable ? kUsageTexture : u.type == kUsageSamplerTable ? kUsageSampler
                                      : u.type == kUsageConstantBufferTable                                  ? kUsageConstantBuffer
                                                                                                             : 0xff;
            if (kind == 0xff) continue;
            const std::uint32_t entry = u.type == kUsageTextureTable ? 8 : 4;
            if (p.final_offset_dw % entry) return false;
            for (const std::uint8_t t : types) {
                if (t != kind) continue;
                out = {kind, static_cast<std::uint8_t>(u.api_slot + p.final_offset_dw / entry), u.start_reg, u.flags};
                return true;
            }
            return false;
        }
        bool table = false;
        for (const UsageSlot& u : r.usage) table = table || (u.type == kUsageExtendedUserData && u.start_reg == p.user_sgpr);
        if (!table) return false;
        reg = 16 + p.final_offset_dw;
    }
    for (const UsageSlot& u : r.usage) {
        if (u.start_reg != reg) continue;
        for (const std::uint8_t t : types) {
            if (u.type == t) {
                out = u;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

ShaderNames name_bindings(const ShaderReflection& r, const TranslateResult& paths) {
    ShaderNames n;
    n.inputs = r.inputs;
    n.outputs = r.outputs;
    const auto count = [&](const BindingName& b) {
        ++n.bindings;
        if (!b.name.empty()) ++n.named;
    };
    for (const ImageBinding& b : paths.images) {
        BindingName x;
        if (UsageSlot u; slot_for(r, b.path, {kUsageTexture}, u)) {
            x.api_slot = u.api_slot;
            x.usage_type = u.type;
            for (const ReflectedResource& res : r.resources) {
                if (res.slot == u.api_slot && (res.type & 0xff) != kResourceConstantBuffer) {
                    x.name = res.name;
                    x.resource_type = res.type;
                    break;
                }
            }
        }
        count(x);
        n.images.push_back(std::move(x));
    }
    for (const SamplerBinding& b : paths.samplers) {
        BindingName x;
        if (UsageSlot u; slot_for(r, b.path, {kUsageSampler}, u)) {
            x.api_slot = u.api_slot;
            x.usage_type = u.type;
            for (const ReflectedSampler& s : r.samplers) {
                if (s.slot == u.api_slot) {
                    x.name = s.name;
                    break;
                }
            }
        }
        count(x);
        n.samplers.push_back(std::move(x));
    }
    for (const BufferBinding& b : paths.buffers) {
        BindingName x;
        if (b.pointer) {
            // A table itself (or another pointer in user data): no resource of
            // the engine's, the usage slot names what it is.
            for (const UsageSlot& u : r.usage) {
                if (!b.path.immediate || !b.path.loads.empty() || u.start_reg != b.path.user_sgpr) continue;
                x.usage_type = u.type;
                x.name = u.type == kUsageExtendedUserData ? "(extended user data)" : u.type == kUsageTextureTable ? "(texture table)"
                         : u.type == kUsageSamplerTable                                                        ? "(sampler table)"
                         : u.type == kUsageConstantBufferTable                                                 ? "(constant buffer table)"
                                                                                                              : "";
            }
        } else if (UsageSlot u; slot_for(r, b.path, {kUsageConstantBuffer}, u)) {
            x.api_slot = u.api_slot;
            x.usage_type = u.type;
            for (const ReflectedResource& res : r.resources) {
                if (res.slot == u.api_slot && (res.type & 0xff) == kResourceConstantBuffer) {
                    x.name = res.name;
                    x.resource_type = res.type;
                    x.size = res.size;
                    for (std::uint32_t k = 0; k < res.elements && res.first_element + k < r.elements.size(); ++k) {
                        x.elements.push_back(r.elements[res.first_element + k]);
                    }
                    break;
                }
            }
            // Slot 15 without a record: the shader's embedded constant buffer
            // (every container that uses slot 15 has one, and no other does).
            if (x.name.empty() && u.api_slot == 15 && r.embedded_cb_bytes) {
                x.name = "(embedded constant buffer)";
                x.size = r.embedded_cb_bytes;
            }
        }
        count(x);
        n.buffers.push_back(std::move(x));
    }
    return n;
}

namespace {

std::string spirv_string(const std::uint32_t* w, std::size_t words) {
    std::string s;
    for (std::size_t k = 0; k < words * 4; ++k) {
        const char ch = static_cast<char>((w[k / 4] >> (8 * (k % 4))) & 0xff);
        if (!ch) break;
        s += ch;
    }
    return s;
}

// "img12" -> 12 for prefix "img"; -1 for anything else.
int indexed(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    if (s.size() <= n || s.compare(0, n, prefix) != 0) return -1;
    int v = 0;
    for (std::size_t k = n; k < s.size(); ++k) {
        if (s[k] < '0' || s[k] > '9' || v > 100000) return -1;
        v = v * 10 + (s[k] - '0');
    }
    return v;
}

}  // namespace

std::vector<std::uint32_t> apply_names(const std::vector<std::uint32_t>& spirv, const ShaderNames& names) {
    if (spirv.size() < 5) return spirv;
    std::vector<std::uint32_t> out(spirv.begin(), spirv.begin() + 5);
    const auto pick = [&](const std::string& s) -> std::string {
        int k;
        if ((k = indexed(s, "img")) >= 0 && static_cast<std::size_t>(k) < names.images.size()) return names.images[k].name;
        if ((k = indexed(s, "smp")) >= 0 && static_cast<std::size_t>(k) < names.samplers.size()) return names.samplers[k].name;
        if ((k = indexed(s, "cb")) >= 0 && static_cast<std::size_t>(k) < names.buffers.size()) return names.buffers[k].name;
        if ((k = indexed(s, "attr")) >= 0 && static_cast<std::size_t>(k) < names.inputs.size()) return names.inputs[k].name;
        if ((k = indexed(s, "vertex_in")) >= 0 && static_cast<std::size_t>(k) < names.inputs.size()) return names.inputs[k].name;
        return {};
    };
    for (std::size_t i = 5; i < spirv.size();) {
        const std::uint32_t n = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (!n || i + n > spirv.size()) return spirv;  // malformed: as given
        std::string to;
        if (op == 5 /* OpName */ && n >= 3) to = pick(spirv_string(&spirv[i + 2], n - 2));
        if (to.empty()) {
            out.insert(out.end(), spirv.begin() + static_cast<std::ptrdiff_t>(i), spirv.begin() + static_cast<std::ptrdiff_t>(i + n));
        } else {
            const std::size_t words = to.size() / 4 + 1;
            out.push_back(static_cast<std::uint32_t>(2 + words) << 16 | 5u);
            out.push_back(spirv[i + 1]);
            for (std::size_t k = 0; k < words; ++k) {
                std::uint32_t w = 0;
                for (std::size_t b = 0; b < 4 && 4 * k + b < to.size(); ++b) w |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(to[4 * k + b])) << (8 * b);
                out.push_back(w);
            }
        }
        i += n;
    }
    return out;
}

}  // namespace gcn
