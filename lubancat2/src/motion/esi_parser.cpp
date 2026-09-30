// esi_parser.cpp —— 轻量 EtherCATInfo(ESI) XML 解析实现
//
// 不引第三方 XML 库：手写一个「够用的」标签扫描器 + 路径状态机。
// 支持的语法子集（覆盖 ESI 实际写法）：
//   * 注释 <!-- -->、处理指令 <?xml ?>、DOCTYPE 声明（一并跳过）
//   * 元素 <Tag>...</Tag>、自闭合 <Tag/>、属性 name="v" / name='v'
//   * 文本节点（去首尾空白；空文本忽略）
//   * 数字：十进制 / 0x.. / #x..
#include "motion/esi_parser.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kx {
namespace {

// ---------------------------------------------------------------------------
// 极简 XML 标签扫描器
// ---------------------------------------------------------------------------
struct Attr {
    std::string name;
    std::string value;
};

struct Event {
    enum Kind { START, END, TEXT } kind = TEXT;
    std::string        name;      // 标签名（TEXT 时为空）
    std::vector<Attr>  attrs;
    bool               self_close = false;
    std::string        text;
};

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool is_name_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':';
}

// 扫描出所有事件；遇到无法识别的输入就跳过该字符（容错优先）
void tokenize(const char* p, size_t n, std::vector<Event>& out) {
    size_t i = 0;
    while (i < n) {
        if (p[i] != '<') {
            // 文本节点：直到下一个 '<'
            size_t j = i;
            while (j < n && p[j] != '<') ++j;
            Event ev;
            ev.kind = Event::TEXT;
            ev.text = trim(std::string(p + i, j - i));
            if (!ev.text.empty()) out.push_back(ev);
            i = j;
            continue;
        }
        // 注释
        if (i + 3 < n && p[i + 1] == '!' && p[i + 2] == '-' && p[i + 3] == '-') {
            const char* end = std::strstr(p + i + 4, "-->");
            i = end ? (size_t)(end - p) + 3 : n;
            continue;
        }
        // PI / DOCTYPE / CDATA
        if (i + 1 < n && (p[i + 1] == '?' || p[i + 1] == '!')) {
            const char* end = std::strchr(p + i, '>');
            i = end ? (size_t)(end - p) + 1 : n;
            continue;
        }

        // 结束标签
        size_t j = i + 1;
        Event ev;
        if (j < n && p[j] == '/') {
            ev.kind = Event::END;
            ++j;
            size_t s = j;
            while (j < n && is_name_char(p[j])) ++j;
            ev.name = std::string(p + s, j - s);
            const char* end = (j < n) ? std::strchr(p + j, '>') : nullptr;
            i = end ? (size_t)(end - p) + 1 : n;
            out.push_back(ev);
            continue;
        }

        // 开始标签
        ev.kind = Event::START;
        size_t s = j;
        while (j < n && is_name_char(p[j])) ++j;
        ev.name = std::string(p + s, j - s);
        if (ev.name.empty()) { ++i; continue; }

        // 属性
        while (j < n && p[j] != '>' && p[j] != '/') {
            while (j < n && std::isspace((unsigned char)p[j])) ++j;
            if (j < n && (p[j] == '>' || p[j] == '/')) break;
            size_t as = j;
            while (j < n && is_name_char(p[j])) ++j;
            if (j == as) { ++j; continue; }
            Attr a;
            a.name = std::string(p + as, j - as);
            while (j < n && std::isspace((unsigned char)p[j])) ++j;
            if (j < n && p[j] == '=') {
                ++j;
                while (j < n && std::isspace((unsigned char)p[j])) ++j;
                if (j < n && (p[j] == '"' || p[j] == '\'')) {
                    const char q = p[j++];
                    size_t vs = j;
                    while (j < n && p[j] != q) ++j;
                    a.value = std::string(p + vs, j - vs);
                    if (j < n) ++j;
                } else {
                    size_t vs = j;
                    while (j < n && !std::isspace((unsigned char)p[j]) && p[j] != '>' && p[j] != '/') ++j;
                    a.value = std::string(p + vs, j - vs);
                }
            }
            ev.attrs.push_back(a);
        }
        if (j < n && p[j] == '/') { ev.self_close = true; ++j; }
        while (j < n && p[j] != '>') ++j;
        if (j < n) ++j;
        i = j;
        out.push_back(ev);
    }
}

const std::string* find_attr(const Event& ev, const char* name) {
    for (const Attr& a : ev.attrs)
        if (a.name == name) return &a.value;
    return nullptr;
}

bool leaf_is(const std::vector<std::string>& path, const char* a) {
    return path.size() >= 1 && path.back() == a;
}
bool has(const std::vector<std::string>& path, const char* tag) {
    for (const std::string& s : path)
        if (s == tag) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------

bool esi_parse_u32(const std::string& token, uint32_t* out) {
    if (!out) return false;
    std::string t = trim(token);
    if (t.empty()) return false;
    int base = 10;
    size_t start = 0;
    if (t.size() > 2 && t[0] == '#' && (t[1] == 'x' || t[1] == 'X')) { base = 16; start = 2; }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { base = 16; start = 2; }
    if (start >= t.size()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(t.c_str() + start, &end, base);
    if (end == t.c_str() + start || errno != 0) return false;
    while (end && *end && std::isspace((unsigned char)*end)) ++end;
    if (end && *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

const EsiPdo* EsiDevice::find_pdo(uint16_t index) const {
    for (const EsiPdo& p : pdos)
        if (p.index == index) return &p;
    return nullptr;
}

const EsiEntry* EsiDevice::find_entry(uint16_t index, uint8_t subindex) const {
    for (const EsiPdo& p : pdos)
        for (const EsiEntry& e : p.entries)
            if (e.index == index && e.subindex == subindex) return &e;
    return nullptr;
}

unsigned EsiDevice::count_rx_entries() const {
    unsigned n = 0;
    for (const EsiPdo& p : pdos) if (p.is_rx) n += (unsigned)p.entries.size();
    return n;
}
unsigned EsiDevice::count_tx_entries() const {
    unsigned n = 0;
    for (const EsiPdo& p : pdos) if (!p.is_rx) n += (unsigned)p.entries.size();
    return n;
}

// ---------------------------------------------------------------------------

bool esi_parse_string(const char* xml, size_t len, EsiDevice* out) {
    if (!xml || !out) return false;
    *out = EsiDevice{};

    std::vector<Event> events;
    events.reserve(256);
    tokenize(xml, len, events);

    std::vector<std::string> path;
    EsiPdo*   cur_pdo   = nullptr;
    EsiEntry* cur_entry = nullptr;
    EsiSm*    cur_sm    = nullptr;
    bool      device_seen = false;
    bool      in_vendor   = false;

    for (const Event& ev : events) {
        if (ev.kind == Event::START) {
            path.push_back(ev.name);

            if (ev.name == "Vendor") in_vendor = true;
            else if (ev.name == "Device") {
                in_vendor = false;
                if (!device_seen) {                 // 多 Device 时只认第一个
                    device_seen = true;
                    const std::string* pc = find_attr(ev, "ProductCode");
                    const std::string* rv = find_attr(ev, "RevisionNo");
                    if (pc) esi_parse_u32(*pc, &out->product_code);
                    if (rv) esi_parse_u32(*rv, &out->revision);
                } else {
                    // 后续 Device 不解析：把 path 打上标记，靠下面的 device_seen 过滤
                }
            }
            else if (ev.name == "Type" && device_seen) {
                const std::string* pc = find_attr(ev, "ProductCode");
                const std::string* rv = find_attr(ev, "RevisionNo");
                if (pc) esi_parse_u32(*pc, &out->product_code);
                if (rv) esi_parse_u32(*rv, &out->revision);
            }
            else if (ev.name == "Sm" && device_seen) {
                out->sms.push_back(EsiSm{});
                cur_sm = &out->sms.back();
                const std::string* ix = find_attr(ev, "Index");
                if (ix) { uint32_t v = 0; if (esi_parse_u32(*ix, &v)) cur_sm->index = (uint8_t)v; }
                const std::string* nm = find_attr(ev, "Name");
                if (nm) {
                    if (nm->find("Input") != std::string::npos) cur_sm->is_input = true;
                    else if (nm->find("Output") != std::string::npos) cur_sm->is_input = false;
                }
            }
            else if ((ev.name == "RxPdo" || ev.name == "TxPdo") && device_seen) {
                out->pdos.push_back(EsiPdo{});
                cur_pdo = &out->pdos.back();
                cur_pdo->is_rx = (ev.name == "RxPdo");
                const std::string* ix = find_attr(ev, "Index");
                if (ix) { uint32_t v = 0; if (esi_parse_u32(*ix, &v)) cur_pdo->index = (uint16_t)v; }
                const std::string* nm = find_attr(ev, "Name");
                if (nm) cur_pdo->name = *nm;
            }
            else if (ev.name == "Entry" && cur_pdo) {
                cur_pdo->entries.push_back(EsiEntry{});
                cur_entry = &cur_pdo->entries.back();
                const std::string* ix = find_attr(ev, "Index");
                if (ix) { uint32_t v = 0; if (esi_parse_u32(*ix, &v)) cur_entry->index = (uint16_t)v; }
                const std::string* si = find_attr(ev, "SubIndex");
                if (si) { uint32_t v = 0; if (esi_parse_u32(*si, &v)) cur_entry->subindex = (uint8_t)v; }
                const std::string* bl = find_attr(ev, "BitLen");
                if (bl) { uint32_t v = 0; if (esi_parse_u32(*bl, &v)) cur_entry->bits = (uint8_t)v; }
                const std::string* nm = find_attr(ev, "Name");
                if (nm) cur_entry->name = *nm;
            }

            if (ev.self_close) {
                // 自闭合元素：立即做一次「结束」清理
                if (ev.name == "RxPdo" || ev.name == "TxPdo") { cur_pdo = nullptr; cur_entry = nullptr; }
                if (ev.name == "Entry") cur_entry = nullptr;
                if (ev.name == "Sm")    cur_sm = nullptr;
                path.pop_back();
            }
            continue;
        }

        if (ev.kind == Event::END) {
            if (ev.name == "Vendor") in_vendor = false;
            if (ev.name == "RxPdo" || ev.name == "TxPdo") { cur_pdo = nullptr; cur_entry = nullptr; }
            if (ev.name == "Entry") cur_entry = nullptr;
            if (ev.name == "Sm")    cur_sm = nullptr;
            if (!path.empty()) path.pop_back();
            continue;
        }

        // ---- 文本节点：按当前路径归位 ----
        if (device_seen == false && in_vendor) {
            if (leaf_is(path, "Id"))   esi_parse_u32(ev.text, &out->vendor_id);
            else if (leaf_is(path, "Name")) out->vendor_name = ev.text;
            continue;
        }
        if (!device_seen) continue;

        if (leaf_is(path, "Type")) out->device_type = ev.text;
        else if (leaf_is(path, "Name") && has(path, "Device") && !has(path, "RxPdo") &&
                 !has(path, "TxPdo") && !has(path, "Entry") && !has(path, "Sm"))
            out->device_name = ev.text;
        else if (cur_entry && leaf_is(path, "Index"))    { uint32_t v; if (esi_parse_u32(ev.text, &v)) cur_entry->index = (uint16_t)v; }
        else if (cur_entry && leaf_is(path, "SubIndex")) { uint32_t v; if (esi_parse_u32(ev.text, &v)) cur_entry->subindex = (uint8_t)v; }
        else if (cur_entry && leaf_is(path, "BitLen"))   { uint32_t v; if (esi_parse_u32(ev.text, &v)) cur_entry->bits = (uint8_t)v; }
        else if (cur_entry && leaf_is(path, "Name"))     { cur_entry->name = ev.text; }
        else if (cur_pdo && leaf_is(path, "Index"))      { uint32_t v; if (esi_parse_u32(ev.text, &v)) cur_pdo->index = (uint16_t)v; }
        else if (cur_pdo && leaf_is(path, "Name"))       { cur_pdo->name = ev.text; }
        else if (cur_sm && leaf_is(path, "Index") && !has(path, "Pdo")) {
            uint32_t v;
            if (esi_parse_u32(ev.text, &v)) cur_sm->index = (uint8_t)v;
        }
        else if (cur_sm && leaf_is(path, "Index") && has(path, "Pdo")) {
            uint32_t v;
            if (esi_parse_u32(ev.text, &v)) cur_sm->pdo_indexes.push_back((uint16_t)v);
        }
        else if (cur_sm && leaf_is(path, "Name") && has(path, "Sm") && !has(path, "Pdo")) {
            if (ev.text.find("Input") != std::string::npos) cur_sm->is_input = true;
            else if (ev.text.find("Output") != std::string::npos) cur_sm->is_input = false;
        }
    }

    if (!device_seen) {
        out->error = "不是 EtherCATInfo（未找到 <Device>）";
        return false;
    }
    if (out->vendor_id == 0 && out->product_code == 0) {
        out->error = "未解析到 Vendor Id / ProductCode";
        return false;
    }
    out->valid = true;
    return true;
}

bool esi_parse_file(const char* path, EsiDevice* out) {
    if (!path || !out) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *out = EsiDevice{};
        out->error = std::string("打不开文件: ") + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string buf = ss.str();
    return esi_parse_string(buf.data(), buf.size(), out);
}

// ---------------------------------------------------------------------------
// 与档案对比
// ---------------------------------------------------------------------------

namespace {

// 收集 (index<<8|sub) 集合
void collect(const EsiPdo& pdo, std::vector<uint32_t>& dst) {
    for (const EsiEntry& e : pdo.entries)
        dst.push_back(((uint32_t)e.index << 8) | e.subindex);
}

const PdoProfile* gear_of(const DriveProfile& prof, int gear_1based, unsigned* out_no) {
    if (prof.n_pdos == 0) return nullptr;
    int g = gear_1based > 0 ? gear_1based : prof.default_gear;
    if (g < 1 || (unsigned)g > prof.n_pdos) g = 1;
    if (out_no) *out_no = (unsigned)g;
    return &prof.pdos[g - 1];
}

} // namespace

EsiCheckReport esi_check_profile(const EsiDevice& dev, const DriveProfile& prof,
                                 int gear_1based) {
    EsiCheckReport r;
    char buf[256];

    r.vid_ok = (dev.vendor_id == prof.vendor_id);
    r.pid_ok = (dev.product_code == prof.product_code);
    r.rev_ok = (prof.revision == 0) || (dev.revision == prof.revision);

    std::snprintf(buf, sizeof(buf), "Vendor 0x%08x %s 档案 0x%08x",
                  dev.vendor_id, r.vid_ok ? "==" : "!=", prof.vendor_id);
    r.findings.push_back(buf);
    std::snprintf(buf, sizeof(buf), "Product 0x%08x %s 档案 0x%08x",
                  dev.product_code, r.pid_ok ? "==" : "!=", prof.product_code);
    r.findings.push_back(buf);
    std::snprintf(buf, sizeof(buf), "Revision 0x%08x %s 档案 0x%08x",
                  dev.revision, r.rev_ok ? "==" : "!=", prof.revision);
    r.findings.push_back(buf);

    const PdoProfile* gear = gear_of(prof, gear_1based, &r.gear_checked);
    if (!gear) {
        r.findings.push_back("档案没有任何 PDO 档位，无法比对");
        return r;
    }

    std::vector<uint32_t> esi_rx, esi_tx, prx, ptx;
    for (const EsiPdo& p : dev.pdos) collect(p, p.is_rx ? esi_rx : esi_tx);

    for (unsigned i = 0; i < gear->n_slots && i < gear->n_rx + gear->n_tx; ++i) {
        const uint32_t key = ((uint32_t)gear->entries[i].index << 8) | gear->entries[i].subindex;
        (i < gear->n_rx ? prx : ptx).push_back(key);
    }

    std::sort(esi_rx.begin(), esi_rx.end());
    std::sort(esi_tx.begin(), esi_tx.end());
    std::sort(prx.begin(), prx.end());
    std::sort(ptx.begin(), ptx.end());

    r.esi_rx = (unsigned)esi_rx.size();
    r.esi_tx = (unsigned)esi_tx.size();
    r.profile_rx = (unsigned)prx.size();
    r.profile_tx = (unsigned)ptx.size();

    // ESI 里可能含非运动 PDO（如 0x1a01 状态 PDO），故用「档案条目是否都能在 ESI 找到」判定
    bool all_found = true;
    std::vector<uint32_t> missing;
    for (uint32_t k : prx) if (!std::binary_search(esi_rx.begin(), esi_rx.end(), k)) { all_found = false; missing.push_back(k); }
    for (uint32_t k : ptx) if (!std::binary_search(esi_tx.begin(), esi_tx.end(), k)) { all_found = false; missing.push_back(k); }
    r.pdo_matches = all_found;

    std::snprintf(buf, sizeof(buf),
                  "PDO：ESI Rx %u / Tx %u 条；档案档位%u Rx %u / Tx %u 条 -> %s",
                  r.esi_rx, r.esi_tx, r.gear_checked, r.profile_rx, r.profile_tx,
                  r.pdo_matches ? "档案条目均可在 ESI 找到（OK）" : "档案有 ESI 里不存在的条目（危险）");
    r.findings.push_back(buf);

    for (uint32_t k : missing) {
        std::snprintf(buf, sizeof(buf), "  档案条目 0x%04x:%02x 在 ESI 中找不到",
                      (unsigned)(k >> 8), (unsigned)(k & 0xff));
        r.findings.push_back(buf);
    }
    return r;
}

std::string esi_check_to_text(const EsiCheckReport& r) {
    std::ostringstream os;
    os << (r.all_ok() ? "[ESI-Check] 一致\n" : "[ESI-Check] 存在差异，需人工确认\n");
    for (const std::string& s : r.findings) os << "  " << s << "\n";
    return os.str();
}

// ---------------------------------------------------------------------------
// 生成候选档案片段
// ---------------------------------------------------------------------------

std::string esi_to_profile_snippet(const EsiDevice& dev, const char* profile_name) {
    std::ostringstream os;
    const char* nm = (profile_name && *profile_name) ? profile_name : "candidate";

    // 挑出「含运动对象」的 PDO 组合：优先含 0x6040 的 Rx 与含 0x6041 的 Tx
    auto has_obj = [&](const EsiPdo& p, uint16_t idx) {
        for (const EsiEntry& e : p.entries) if (e.index == idx) return true;
        return false;
    };
    const EsiPdo* rx = nullptr;
    const EsiPdo* tx = nullptr;
    for (const EsiPdo& p : dev.pdos) {
        if (p.is_rx && has_obj(p, 0x6040) && !rx) rx = &p;
        if (!p.is_rx && has_obj(p, 0x6041) && !tx) tx = &p;
    }

    os << "// ---- 由 ESI 生成的候选档案（必须人工确认后再启用）----\n";
    os << "// 来源: " << dev.device_name << " / " << dev.device_type
       << "（" << dev.vendor_name << "）\n";
    os << "// 注意：ProductCode 不区分功率/机械规格；同 vid/pid 不同固件 PDO 可能不同。\n";
    os << "const PdoEntry kEntries_" << nm << "[] = {\n";
    if (rx) for (const EsiEntry& e : rx->entries)
        os << "    {0x" << std::hex << e.index << std::dec << ", 0x00, " << (int)e.bits << "},  // Rx "
           << e.name << "\n";
    if (tx) for (const EsiEntry& e : tx->entries)
        os << "    {0x" << std::hex << e.index << std::dec << ", 0x00, " << (int)e.bits << "},  // Tx "
           << e.name << "\n";
    os << "};\n\n";
    os << "const DriveProfile kDriveCandidate = {\n";
    os << "    \"" << nm << "\", \"" << dev.vendor_name << "\",\n";
    os << "    0x" << std::hex << dev.vendor_id << ", 0x" << dev.product_code
       << ", 0x" << dev.revision << std::dec << ", false,\n";
    os << "    {\"" << dev.device_name << "\", nullptr, nullptr, nullptr},\n";
    os << "    1, nullptr, 0, false,\n";
    os << "    {0x6081, 0x6083, 0x6084, true},\n";
    os << "    {0x605D, 1, 0x605A, 2, 0x605C, 0, 0x603F, 16},\n";
    os << "    1, true, false, false,\n";
    os << "    \"由 ESI 生成，需人工补齐 PdoProfile/SlotMap 与停机语义\",\n";
    os << "};\n";
    return os.str();
}

} // namespace kx
