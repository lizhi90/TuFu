// drive_profile.cpp —— 档案注册表 + 身份匹配实现（纯逻辑，无 ecrt 依赖）
#include "motion/drive_profile.h"

#include <cstdio>
#include <cstring>

namespace kx {
namespace {

constexpr size_t kMaxProfiles = 32;

// 注册表内部存储：只存指针（档案本身是 static const，生命周期贯穿进程）
struct Store {
    const DriveProfile* items[kMaxProfiles];
    size_t count = 0;
};

Store& store() {
    static Store s;
    return s;
}

bool ci_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

// SII 名称候选里是否包含 name（子串匹配，容忍 "SV630_1Axis_03716" 这类后缀）
bool sii_name_lists(const DriveProfile& p, const char* name) {
    if (!name || !*name) return false;
    for (int i = 0; i < 4 && p.sii_names[i]; ++i) {
        const char* cand = p.sii_names[i];
        if (std::strstr(name, cand) != nullptr || ci_equal(name, cand)) return true;
    }
    return false;
}

// 档案是否具备「具体身份」（非兜底）
bool has_identity(const DriveProfile& p) {
    return p.vendor_id != 0 || p.product_code != 0;
}

} // namespace

// ---------------------------------------------------------------------------

ProfileRegistry& ProfileRegistry::instance() {
    static ProfileRegistry r;
    return r;
}

void ProfileRegistry::add(const DriveProfile* p) {
    if (!p || !p->name) return;
    Store& s = store();
    if (s.count >= kMaxProfiles) {
        std::fprintf(stderr, "[profile] 注册表已满（%zu），忽略 %s\n", kMaxProfiles, p->name);
        return;
    }
    // 去重：同名则以先注册者为准（防止重复链接）
    if (find_by_name(p->name)) {
        std::fprintf(stderr, "[profile] 档案名重复，忽略后注册的 %s\n", p->name);
        return;
    }
    s.items[s.count++] = p;
}

size_t ProfileRegistry::size() const { return store().count; }

const DriveProfile* ProfileRegistry::at(size_t i) const {
    const Store& s = store();
    return i < s.count ? s.items[i] : nullptr;
}

const DriveProfile* ProfileRegistry::find_by_name(const char* name) const {
    if (!name) return nullptr;
    const Store& s = store();
    for (size_t i = 0; i < s.count; ++i)
        if (ci_equal(s.items[i]->name, name)) return s.items[i];
    return nullptr;
}

const DriveProfile* ProfileRegistry::fallback() const {
    return find_by_name("generic");
}

// ---------------------------------------------------------------------------

MatchResult profile_match(uint32_t vendor_id, uint32_t product_code, uint32_t revision,
                          const char* sii_name) {
    MatchResult r;
    const Store& s = store();

    const DriveProfile* id_hit = nullptr;      // vid/pid 命中
    const DriveProfile* partial = nullptr;     // 只命中 vid 或只命中 pid

    for (size_t i = 0; i < s.count; ++i) {
        const DriveProfile* p = s.items[i];
        if (!has_identity(*p)) continue;

        const bool v_ok = (p->vendor_id == 0) || (p->vendor_id == vendor_id);
        const bool p_ok = (p->product_code == 0) || (p->product_code == product_code);
        if (!(v_ok && p_ok)) {
            // 记录弱命中：vendor 对但 product 不对（同厂新型号）——只用于提示
            if (p->vendor_id == vendor_id && p->product_code != 0 && !partial) partial = p;
            continue;
        }
        // vid/pid 都中，取第一个（注册顺序即优先级）
        if (!id_hit) id_hit = p;
    }

    if (id_hit) {
        r.profile = id_hit;
        const bool rev_ok = (id_hit->revision == 0) || (id_hit->revision == revision);
        r.rev_mismatch = !rev_ok;
        r.name_mismatch = (sii_name && *sii_name && id_hit->sii_names[0]) ? !sii_name_lists(*id_hit, sii_name) : false;

        if (rev_ok && !r.name_mismatch) {
            r.kind = ProfileMatch::EXACT;
            std::snprintf(r.detail, sizeof(r.detail), "精确命中档案 '%s'（vid/pid/rev 全中%s）",
                          id_hit->name, (sii_name && *sii_name) ? "，SII 名称一致" : "");
        } else {
            // 命中但身份不完全一致：疑似克隆/新固件 -> 降级为「需人工确认」
            r.kind = ProfileMatch::ID_ONLY;
            std::snprintf(r.detail, sizeof(r.detail),
                          "vid/pid 命中 '%s'，但 %s%s%s；疑似克隆或新固件，已降级（不自动采用）",
                          id_hit->name,
                          r.rev_mismatch ? "Revision 不符" : "",
                          (r.rev_mismatch && r.name_mismatch) ? " 且 " : "",
                          r.name_mismatch ? "SII 名称不符" : "");
        }
        return r;
    }

    if (partial) {
        std::snprintf(r.detail, sizeof(r.detail),
                      "未找到精确档案；厂商 '%s' 有档案但 Product 0x%08x 不符（总线读到 0x%08x）",
                      partial->vendor_name ? partial->vendor_name : "?",
                      partial->product_code, product_code);
    } else {
        std::snprintf(r.detail, sizeof(r.detail),
                      "未找到匹配档案（vid=0x%08x pid=0x%08x rev=0x%08x）",
                      vendor_id, product_code, revision);
    }

    const DriveProfile* fb = ProfileRegistry::instance().fallback();
    if (fb) {
        r.profile = fb;
        r.kind = ProfileMatch::GENERIC;
        size_t len = std::strlen(r.detail);
        std::snprintf(r.detail + len, sizeof(r.detail) - len,
                      "；回退到通用档案 '%s'（仅标准 CiA402 最小映射，需人工确认）", fb->name);
    } else {
        r.kind = ProfileMatch::NONE;
        r.profile = nullptr;
    }
    return r;
}

// ---------------------------------------------------------------------------

const DriveProfile* profile_resolve_name(const char* name, bool* is_auto, bool* unknown) {
    if (is_auto)  *is_auto  = false;
    if (unknown)  *unknown  = false;
    if (!name || !*name) return nullptr;
    if (ci_equal(name, "auto")) {
        if (is_auto) *is_auto = true;
        return nullptr;   // 由调用方在读到总线身份后再 match
    }
    const DriveProfile* p = ProfileRegistry::instance().find_by_name(name);
    if (!p && unknown) *unknown = true;
    return p;
}

int profile_describe(const DriveProfile& p, char* buf, size_t n) {
    if (!buf || n == 0) return 0;
    const char* mods[4];
    int nm = 0;
    if (p.supports_pp) mods[nm++] = "PP";
    if (p.supports_pv) mods[nm++] = "PV";
    if (p.supports_hm) mods[nm++] = "HM";
    char modebuf[16];
    modebuf[0] = '\0';
    for (int i = 0; i < nm; ++i) {
        size_t off = std::strlen(modebuf);
        std::snprintf(modebuf + off, sizeof(modebuf) - off, "%s%s", i ? "/" : "", mods[i]);
    }
    if (!modebuf[0]) std::snprintf(modebuf, sizeof(modebuf), "-");

    int written = std::snprintf(
        buf, n,
        "%-14s vid=0x%08x pid=0x%08x rev=0x%08x gear=%d/%u 默认模式=%d 模式=%-6s halt=0x%04x rc=0x%04x",
        p.name, p.vendor_id, p.product_code, p.revision,
        p.default_gear, p.n_pdos, (int)p.default_mode, modebuf,
        p.halt.halt_option_index, p.halt.error_code_index);
    return written < 0 ? 0 : written;
}

void profile_dump_all(void (*sink)(const char* line, void* user), void* user) {
    char line[320];
    const Store& s = store();
    for (size_t i = 0; i < s.count; ++i) {
        const DriveProfile* p = s.items[i];
        profile_describe(*p, line, sizeof(line));
        if (sink) sink(line, user);
        for (unsigned g = 0; g < p->n_pdos; ++g) {
            char gline[320];
            std::snprintf(gline, sizeof(gline), "  - 档位%u: %-22s Rx %u 条 / Tx %u 条",
                          g + 1, p->pdos[g].name, p->pdos[g].n_rx, p->pdos[g].n_tx);
            if (sink) sink(gline, user);
        }
        if (p->notes) {
            char nline[320];
            std::snprintf(nline, sizeof(nline), "  - 注意: %s", p->notes);
            if (sink) sink(nline, user);
        }
    }
}

} // namespace kx
