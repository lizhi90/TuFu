// modbus_config.cpp —— 组态解析/校验/序列化（planA/20 §4）
#include "modbus/modbus_config.h"

#include <cmath>
#include <cstring>

#include "script/json_lite.h"      // 复用零依赖 JSON（调试通道同款）
#include "script/nvram_store.h"    // persist 范围校验（addr < NvramStore::kRegs）

namespace kx {

namespace {

std::string str_trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool addr_parse(const std::string& s, int* out) {
    const std::string t = str_trim(s);
    if (t.size() < 3 || t[0] != '4' || t[1] != 'x') return false;
    int v = 0;
    for (size_t i = 2; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        v = v * 10 + (t[i] - '0');
        if (v > 65535) return false;
    }
    if (v < 0) return false;
    *out = v;
    return true;
}

std::string addr_str(int a) { return "4x" + std::to_string(a); }

} // namespace

int MbConfig::span(const MbEntry& e) const {
    switch (e.type) {
        case MbType::U32:
        case MbType::F32:
        case MbType::F32HI: return 2;
        default:            return 1;
    }
}

const MbEntry* MbConfig::find_name(const std::string& n) const {
    for (const auto& e : entries) {
        if (e.name == n) return &e;
    }
    return nullptr;
}

const MbEntry* MbConfig::find_addr(int a) const {
    for (const auto& e : entries) {
        if (a >= e.addr && a < e.addr + span(e)) return &e;
    }
    return nullptr;
}

void mb_encode_value(MbType t, double v, uint16_t* w0, uint16_t* w1) {
    *w0 = 0;
    *w1 = 0;
    switch (t) {
        case MbType::U16: {
            long long iv = (long long)llround(v);
            if (iv < 0) iv = 0;
            if (iv > 0xFFFF) iv = 0xFFFF;
            *w0 = (uint16_t)iv;
            break;
        }
        case MbType::I16: {
            long long iv = (long long)llround(v);
            if (iv < -32768) iv = -32768;
            if (iv > 32767) iv = 32767;
            *w0 = (uint16_t)((uint16_t)(int16_t)iv);
            break;
        }
        case MbType::U32: {
            unsigned long long iv = (v <= 0.0) ? 0ULL : (unsigned long long)llround(v);
            if (iv > 0xFFFFFFFFULL) iv = 0xFFFFFFFFULL;
            *w0 = (uint16_t)(iv & 0xFFFF);              // 低字在前
            *w1 = (uint16_t)((iv >> 16) & 0xFFFF);
            break;
        }
        case MbType::F32:
        case MbType::F32HI: {
            const float f = (float)v;
            uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            if (t == MbType::F32) {
                *w0 = (uint16_t)(bits & 0xFFFF);        // 低字在前（屏约定）
                *w1 = (uint16_t)(bits >> 16);
            } else {
                *w0 = (uint16_t)(bits >> 16);           // 高字在前（泵约定）
                *w1 = (uint16_t)(bits & 0xFFFF);
            }
            break;
        }
    }
}

double mb_decode_value(MbType t, uint16_t w0, uint16_t w1) {
    switch (t) {
        case MbType::U16: return (double)w0;
        case MbType::I16: return (double)(int16_t)w0;
        case MbType::U32:
            return (double)(((uint32_t)w1 << 16) | (uint32_t)w0);
        case MbType::F32: {
            const uint32_t bits = ((uint32_t)w1 << 16) | (uint32_t)w0;
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            return (double)f;
        }
        case MbType::F32HI: {
            const uint32_t bits = ((uint32_t)w0 << 16) | (uint32_t)w1;
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            return (double)f;
        }
    }
    return 0.0;
}

void MbConfig::encode(const MbEntry& e, double v, uint16_t* w0, uint16_t* w1) const {
    mb_encode_value(e.type, v, w0, w1);
}

double MbConfig::decode(const MbEntry& e, uint16_t w0, uint16_t w1) const {
    return mb_decode_value(e.type, w0, w1);
}

std::string mb_type_name(MbType t) {
    switch (t) {
        case MbType::U16:   return "u16";
        case MbType::I16:   return "i16";
        case MbType::U32:   return "u32";
        case MbType::F32:   return "f32";
        case MbType::F32HI: return "f32hi";
    }
    return "u16";
}

bool mb_type_from_name(const std::string& s, MbType* out) {
    if (s == "u16")   { *out = MbType::U16;   return true; }
    if (s == "i16")   { *out = MbType::I16;   return true; }
    if (s == "u32")   { *out = MbType::U32;   return true; }
    if (s == "f32")   { *out = MbType::F32;   return true; }
    if (s == "f32hi") { *out = MbType::F32HI; return true; }
    return false;
}

std::string mb_access_name(MbAccess a) {
    switch (a) {
        case MbAccess::R:  return "r";
        case MbAccess::W:  return "w";
        case MbAccess::RW: return "rw";
    }
    return "rw";
}

bool mb_config_parse(const std::string& text, MbConfig* out, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    Json root;
    if (!json_parse(text, &root)) {
        return fail("JSON 解析失败（组态文件格式不正确）");
    }
    MbConfig cfg;
    const Json* st = root.find("station");
    if (st && st->is_num()) {
        const int s = (int)st->as_int(1);
        if (s < 1 || s > 247) return fail("station 需在 1..247");
        cfg.station = s;
    }
    const Json* arr = root.find("registers");
    if (!arr || !arr->is_arr()) {
        return fail("缺少 registers 数组");
    }
    if ((int)arr->arr.size() > kMbMaxEntries) {
        return fail("registers 条目超过上限 " + std::to_string(kMbMaxEntries));
    }
    for (const Json& e : arr->arr) {
        if (!e.is_obj()) return fail("registers 元素必须是对象");
        MbEntry ent;
        const Json* pn = e.find("name");
        if (!pn || !pn->is_str() || str_trim(pn->as_str()).empty()) {
            return fail("条目缺少 name");
        }
        ent.name = pn->as_str();
        if (ent.name.size() > 32) return fail("name 过长（≤32 字节）：" + ent.name);
        const Json* pa = e.find("addr");
        if (!pa || !pa->is_str() || !addr_parse(pa->as_str(), &ent.addr)) {
            return fail("条目 " + ent.name + " 的 addr 需形如 4x300");
        }
        const Json* pt = e.find("type");
        if (pt && pt->is_str()) {
            if (!mb_type_from_name(pt->as_str(), &ent.type)) {
                return fail("条目 " + ent.name + " 的 type 仅支持 u16/i16/u32/f32/f32hi");
            }
        }
        const Json* pac = e.find("access");
        if (pac && pac->is_str()) {
            const std::string a = pac->as_str();
            if (a == "r")       ent.access = MbAccess::R;
            else if (a == "w")  ent.access = MbAccess::W;
            else if (a == "rw") ent.access = MbAccess::RW;
            else return fail("条目 " + ent.name + " 的 access 仅支持 r/w/rw");
        }
        const Json* pp = e.find("persist");
        if (pp && pp->is_bool()) ent.persist = pp->as_bool();
        const Json* pd = e.find("default");
        if (pd && pd->is_num()) {
            ent.has_default = true;
            ent.defval = pd->as_num();
        }
        const Json* pc = e.find("desc");
        if (pc && pc->is_str()) ent.desc = pc->as_str();

        // persist 受 NvramStore 容量限制
        if (ent.persist && ent.addr + (ent.type == MbType::U32 || ent.type == MbType::F32 ||
                                       ent.type == MbType::F32HI ? 1 : 0) >= NvramStore::kRegs) {
            return fail("条目 " + ent.name + " 勾选掉电保持，但地址超出持久化范围（< " +
                        addr_str(NvramStore::kRegs) + "）");
        }
        cfg.entries.push_back(std::move(ent));
    }

    // 名称唯一 + 地址（含 32 位跨占）不重叠
    for (size_t i = 0; i < cfg.entries.size(); ++i) {
        for (size_t j = i + 1; j < cfg.entries.size(); ++j) {
            if (cfg.entries[i].name == cfg.entries[j].name) {
                return fail("变量名重复：" + cfg.entries[i].name);
            }
        }
    }
    std::vector<int> used;                                   // 已占用字（O(n^2) 足够，n≤1024）
    for (const auto& e : cfg.entries) {
        const int sp = cfg.span(e);
        if (e.addr + sp - 1 > 65535) {
            return fail("条目 " + e.name + " 超出 4x65535");
        }
        for (int w = e.addr; w < e.addr + sp; ++w) {
            for (int u : used) {
                if (u == w) {
                    return fail("地址重叠：" + addr_str(w) + "（条目 " + e.name + "）");
                }
            }
            used.push_back(w);
        }
        if (e.has_default) {
            switch (e.type) {
                case MbType::U16:
                    if (e.defval < 0 || e.defval > 65535) return fail("条目 " + e.name + " 默认值超出 u16 范围");
                    break;
                case MbType::I16:
                    if (e.defval < -32768 || e.defval > 32767) return fail("条目 " + e.name + " 默认值超出 i16 范围");
                    break;
                case MbType::U32:
                    if (e.defval < 0 || e.defval > 4294967295.0) return fail("条目 " + e.name + " 默认值超出 u32 范围");
                    break;
                case MbType::F32:
                case MbType::F32HI:
                    if (!std::isfinite(e.defval)) return fail("条目 " + e.name + " 默认值不是有限数");
                    break;
            }
        }
    }
    *out = std::move(cfg);
    return true;
}

std::string mb_config_to_json(const MbConfig& cfg) {
    Json root = Json::make_obj();
    root.set("version", Json::make_num(1));
    root.set("station", Json::make_num(cfg.station));
    Json arr = Json::make_arr();
    for (const auto& e : cfg.entries) {
        Json o = Json::make_obj();
        o.set("name", Json::make_str(e.name));
        o.set("addr", Json::make_str(addr_str(e.addr)));
        o.set("type", Json::make_str(mb_type_name(e.type)));
        o.set("access", Json::make_str(mb_access_name(e.access)));
        o.set("persist", Json::make_bool(e.persist));
        if (e.has_default) o.set("default", Json::make_num(e.defval));
        if (!e.desc.empty()) o.set("desc", Json::make_str(e.desc));
        arr.push(std::move(o));
    }
    root.set("registers", std::move(arr));
    return json_dump(root);
}

} // namespace kx
