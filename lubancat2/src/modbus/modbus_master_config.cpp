// modbus_master_config.cpp —— 主站组态解析/校验/序列化（planA/21 §2）
#include "modbus/modbus_master_config.h"

#include <cstring>

#include "script/json_lite.h"

namespace kx {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool addr_parse(const std::string& s, int* out) {
    const std::string t = trim(s);
    if (t.size() < 3 || t[0] != '4' || t[1] != 'x') return false;
    int v = 0;
    for (size_t i = 2; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        v = v * 10 + (t[i] - '0');
        if (v > 65535) return false;
    }
    *out = v;
    return true;
}

int type_span(MbType t) {
    return (t == MbType::U32 || t == MbType::F32 || t == MbType::F32HI) ? 2 : 1;
}

} // namespace

const MbDeviceCfg* MbMasterConfig::find_dev(const std::string& n) const {
    for (const auto& d : devices) {
        if (d.name == n) return &d;
    }
    return nullptr;
}

const MbPointCfg* MbMasterConfig::find_point(const std::string& dev, const std::string& pt) const {
    const MbDeviceCfg* d = find_dev(dev);
    if (!d) return nullptr;
    for (const auto& p : d->points) {
        if (p.name == pt) return &p;
    }
    return nullptr;
}

bool MbMasterConfig::locate(const std::string& dev, const std::string& pt, int* di, int* pi) const {
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].name != dev) continue;
        for (size_t j = 0; j < devices[i].points.size(); ++j) {
            if (devices[i].points[j].name == pt) {
                *di = (int)i;
                *pi = (int)j;
                return true;
            }
        }
    }
    return false;
}

std::string mb_link_name(MbLinkKind k) { return k == MbLinkKind::TCP ? "tcp" : "rtu-tcp"; }

bool mb_link_from_name(const std::string& s, MbLinkKind* out) {
    if (s == "tcp")     { *out = MbLinkKind::TCP;      return true; }
    if (s == "rtu-tcp") { *out = MbLinkKind::RTU_TCP;  return true; }
    return false;
}

std::string mb_fc_name(int fc) { return std::to_string(fc); }
const char* mb_dir_name(bool write) { return write ? "write" : "read"; }

bool mb_master_config_parse(const std::string& text, MbMasterConfig* out, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    Json root;
    if (!json_parse(text, &root)) return fail("JSON 解析失败（主站组态格式不正确）");
    const Json* arr = root.find("devices");
    if (!arr || !arr->is_arr()) return fail("缺少 devices 数组");
    if ((int)arr->arr.size() > kMbMaxDevices) {
        return fail("设备数超过上限 " + std::to_string(kMbMaxDevices));
    }

    MbMasterConfig cfg;
    for (const Json& dj : arr->arr) {
        if (!dj.is_obj()) return fail("devices 元素必须是对象");
        MbDeviceCfg d;
        const Json* pn = dj.find("name");
        if (!pn || !pn->is_str() || trim(pn->as_str()).empty()) return fail("设备缺少 name");
        d.name = pn->as_str();
        if (d.name.size() > 32) return fail("设备名过长（≤32 字节）：" + d.name);
        const Json* pl = dj.find("link");
        if (!pl || !pl->is_obj()) return fail("设备 " + d.name + " 缺少 link 对象");
        const Json* pk = pl->find("kind");
        if (!pk || !pk->is_str() || !mb_link_from_name(pk->as_str(), &d.kind)) {
            return fail("设备 " + d.name + " 的 link.kind 仅支持 tcp / rtu-tcp");
        }
        const Json* ph = pl->find("host");
        if (!ph || !ph->is_str() || trim(ph->as_str()).empty()) {
            return fail("设备 " + d.name + " 缺少 link.host");
        }
        d.host = ph->as_str();
        const Json* pp = pl->find("port");
        if (!pp || !pp->is_num()) return fail("设备 " + d.name + " 缺少 link.port");
        d.port = (int)pp->as_int(0);
        if (d.port < 1 || d.port > 65535) return fail("设备 " + d.name + " 端口越界");
        const Json* pu = dj.find("unit");
        if (pu && pu->is_num()) d.unit = (int)pu->as_int(1);
        if (d.unit < 1 || d.unit > 247) return fail("设备 " + d.name + " 站号需 1..247");
        const Json* pt = dj.find("timeout_ms");
        if (pt && pt->is_num()) d.timeout_ms = (int)pt->as_int(300);
        if (d.timeout_ms < 50 || d.timeout_ms > 5000) return fail("设备 " + d.name + " timeout_ms 需 50..5000");
        const Json* pr = dj.find("retries");
        if (pr && pr->is_num()) d.retries = (int)pr->as_int(1);
        if (d.retries < 0 || d.retries > 5) return fail("设备 " + d.name + " retries 需 0..5");
        const Json* pm = dj.find("poll_ms");
        if (pm && pm->is_num()) d.poll_ms = (int)pm->as_int(200);
        if (d.poll_ms < 20 || d.poll_ms > 60000) return fail("设备 " + d.name + " poll_ms 需 20..60000");

        const Json* pts = dj.find("points");
        if (!pts || !pts->is_arr() || pts->arr.empty()) return fail("设备 " + d.name + " 缺少 points 数组");
        if ((int)pts->arr.size() > kMbMaxPoints) {
            return fail("设备 " + d.name + " 点位数超过上限 " + std::to_string(kMbMaxPoints));
        }
        for (const Json& pj : pts->arr) {
            if (!pj.is_obj()) return fail("points 元素必须是对象");
            MbPointCfg p;
            const Json* qn = pj.find("name");
            if (!qn || !qn->is_str() || trim(qn->as_str()).empty()) {
                return fail("设备 " + d.name + " 的某个点位缺少 name");
            }
            p.name = qn->as_str();
            if (p.name.size() > 32) return fail("点位名过长（≤32 字节）：" + p.name);
            const Json* qd = pj.find("dir");
            if (!qd || !qd->is_str() || (qd->as_str() != "read" && qd->as_str() != "write")) {
                return fail("点位 " + d.name + "." + p.name + " 的 dir 仅支持 read/write");
            }
            p.write = (qd->as_str() == "write");
            const Json* qf = pj.find("fc");
            if (!qf || !qf->is_num()) return fail("点位 " + p.name + " 缺少 fc");
            p.fc = (int)qf->as_int(0);
            if (p.write) {
                if (p.fc != 5 && p.fc != 6 && p.fc != 15 && p.fc != 16) {
                    return fail("写点位 " + p.name + " 的 fc 仅支持 5/6/15/16");
                }
            } else {
                if (p.fc != 1 && p.fc != 2 && p.fc != 3 && p.fc != 4) {
                    return fail("读点位 " + p.name + " 的 fc 仅支持 1/2/3/4");
                }
            }
            const Json* qa = pj.find("addr");
            if (!qa || !qa->is_num()) return fail("点位 " + p.name + " 缺少 addr");
            p.addr = (int)qa->as_int(0);
            if (p.addr < 0 || p.addr > 65535) return fail("点位 " + p.name + " addr 越界");
            const Json* qc = pj.find("count");
            p.count = (qc && qc->is_num()) ? (int)qc->as_int(1) : 1;
            const Json* qt = pj.find("type");
            if (qt && qt->is_str()) {
                if (!mb_type_from_name(qt->as_str(), &p.type)) {
                    return fail("点位 " + p.name + " 的 type 仅支持 u16/i16/u32/f32/f32hi");
                }
            }
            const bool bits = (p.fc == 1 || p.fc == 2 || p.fc == 5 || p.fc == 15);
            if (bits) {
                if (p.count != 1) return fail("点位 " + p.name + " 线圈类 v1 仅支持单点（count=1）");
                if (p.type != MbType::U16 && p.type != MbType::I16) {
                    return fail("点位 " + p.name + " 线圈类 type 仅支持 u16/i16");
                }
            } else {
                if (p.count != type_span(p.type)) {
                    return fail("点位 " + p.name + " 的 count 必须等于类型的寄存器字数（u16/i16=1，u32/f32/f32hi=2）");
                }
                if (p.fc == 6 && type_span(p.type) == 2) {
                    return fail("点位 " + p.name + " 的 fc6（写单寄存器）不支持 2 字类型，请改用 fc16");
                }
            }
            if (p.addr + p.count - 1 > 65535) return fail("点位 " + p.name + " 地址越界");
            const Json* qm = pj.find("map");
            if (qm && qm->is_obj()) {
                const Json* qk = qm->find("kind");
                if (!qk || !qk->is_str() || (qk->as_str() != "reg" && qk->as_str() != "var")) {
                    return fail("点位 " + p.name + " 的 map.kind 仅支持 reg/var");
                }
                if (qk->as_str() == "reg") {
                    p.map_reg = true;
                    const Json* qma = qm->find("addr");
                    if (!qma || !qma->is_str() || !addr_parse(qma->as_str(), &p.map_addr)) {
                        return fail("点位 " + p.name + " 的 map.addr 需形如 4x600");
                    }
                }
            } else {
                return fail("点位 " + p.name + " 缺少 map 对象（{\"kind\":\"reg|var\"}）");
            }
            const Json* qoc = pj.find("on_change");
            if (qoc && qoc->is_bool()) p.on_change = qoc->as_bool();
            d.points.push_back(std::move(p));
        }
        cfg.devices.push_back(std::move(d));
    }

    // 名称唯一性（设备全局唯一；点位设备内唯一）
    for (size_t i = 0; i < cfg.devices.size(); ++i) {
        for (size_t j = i + 1; j < cfg.devices.size(); ++j) {
            if (cfg.devices[i].name == cfg.devices[j].name) {
                return fail("设备名重复：" + cfg.devices[i].name);
            }
        }
        const auto& pts = cfg.devices[i].points;
        for (size_t a = 0; a < pts.size(); ++a) {
            for (size_t b = a + 1; b < pts.size(); ++b) {
                if (pts[a].name == pts[b].name) {
                    return fail("设备 " + cfg.devices[i].name + " 点位名重复：" + pts[a].name);
                }
            }
        }
    }
    *out = std::move(cfg);
    return true;
}

std::string mb_master_config_to_json(const MbMasterConfig& cfg) {
    Json root = Json::make_obj();
    root.set("version", Json::make_num(1));
    Json devs = Json::make_arr();
    for (const auto& d : cfg.devices) {
        Json dj = Json::make_obj();
        dj.set("name", Json::make_str(d.name));
        Json lj = Json::make_obj();
        lj.set("kind", Json::make_str(mb_link_name(d.kind)));
        lj.set("host", Json::make_str(d.host));
        lj.set("port", Json::make_num(d.port));
        dj.set("link", std::move(lj));
        dj.set("unit", Json::make_num(d.unit));
        dj.set("timeout_ms", Json::make_num(d.timeout_ms));
        dj.set("retries", Json::make_num(d.retries));
        dj.set("poll_ms", Json::make_num(d.poll_ms));
        Json pts = Json::make_arr();
        for (const auto& p : d.points) {
            Json pj = Json::make_obj();
            pj.set("name", Json::make_str(p.name));
            pj.set("dir", Json::make_str(mb_dir_name(p.write)));
            pj.set("fc", Json::make_num(p.fc));
            pj.set("addr", Json::make_num(p.addr));
            pj.set("count", Json::make_num(p.count));
            pj.set("type", Json::make_str(mb_type_name(p.type)));
            Json mj = Json::make_obj();
            if (p.map_reg) {
                mj.set("kind", Json::make_str("reg"));
                mj.set("addr", Json::make_str("4x" + std::to_string(p.map_addr)));
            } else {
                mj.set("kind", Json::make_str("var"));
            }
            pj.set("map", std::move(mj));
            if (p.write) pj.set("on_change", Json::make_bool(p.on_change));
            pts.push(std::move(pj));
        }
        dj.set("points", std::move(pts));
        devs.push(std::move(dj));
    }
    root.set("devices", std::move(devs));
    return json_dump(root);
}

bool mb_master_cross_check(const MbMasterConfig& m, const MbConfig& slave, std::string* err) {
    auto fail = [&](const std::string& s) {
        if (err) *err = s;
        return false;
    };
    std::vector<int> write_words;                       // 主站写点已占用的 4x 字
    for (const auto& d : m.devices) {
        for (const auto& p : d.points) {
            if (!p.map_reg) continue;
            const int span = p.count;
            if (p.write) {
                for (int w = p.map_addr; w < p.map_addr + span; ++w) {
                    for (int u : write_words) {
                        if (u == w) {
                            return fail("主站写点 4x 映射重叠：" + d.name + "." + p.name + " @ 4x" + std::to_string(w));
                        }
                    }
                    write_words.push_back(w);
                    const MbEntry* se = slave.find_addr(w);
                    if (se && se->access == MbAccess::R) {
                        return fail("主站写点 " + d.name + "." + p.name + " 的映射 4x" + std::to_string(w) +
                                    " 与从站只读条目（" + se->name + "）冲突");
                    }
                }
            }
        }
    }
    return true;
}

} // namespace kx
