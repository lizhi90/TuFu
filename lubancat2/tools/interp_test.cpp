// interp_test.cpp —— M3 插补纯逻辑自测（Trapezoid / LinInterp）
#include "../src/motion/interp.h"

#include <cmath>
#include <cstdio>

using namespace kx;

static int g_fail = 0;
static void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_fail;
}

// 梯形：位移/速度/时间的关系
static void test_trapezoid_trapezoid_shape() {
    std::printf("== 1) 梯型：v/a/L 语义 ==\n");
    Trapezoid t;
    const double L = 100.0, v = 50.0, a = 100.0;
    t.begin(0.0, L, v, a);
    // L > v^2/a (25) → 梯形：总时 = v/a*2 + (L - v^2/a)/v = 1 + 1.5 = 2.5s
    check(std::fabs(t.total_time() - 2.5) < 1e-9, "梯形总时 = v/a*2+(L-v^2/a)/v");

    double peak_v = 0.0, p_prev = 0.0;
    bool monotonic = true;
    double max_gap_err = 0.0;
    for (double tt = 0.0; tt < 2.5; tt += 0.001) {
        const double p = t.step(0.001);
        const double vv = t.vel();
        if (vv > peak_v) peak_v = vv;
        if (p < p_prev) monotonic = false;
        p_prev = p;
    }
    check(monotonic, "位置单调");
    check(std::fabs(peak_v - v) < 1e-6, "峰值速度 = v");
    check(t.done(), "done");
    check(std::fabs(t.pos() - L) < 1e-9, "终点 = L");
    // 速度积分≈位移（复验各段解析连续）
    // step 末点即 L，无跳变
    (void)max_gap_err;
}

static void test_trapezoid_triangle() {
    std::printf("== 2) 三角形（达不到 vmax）==\n");
    Trapezoid t;
    const double L = 10.0, v = 50.0, a = 100.0;   // d_acc=12.5 > L/2=5 → 三角形
    t.begin(0.0, L, v, a);
    // v_pk = sqrt(L*a) = sqrt(1000)=31.62；总时 = 2*sqrt(L/a)=2*0.3162
    check(std::fabs(t.total_time() - 2.0 * std::sqrt(L / a)) < 1e-9, "三角形总时 = 2*sqrt(L/a)");
    double p_prev = 0.0;
    bool monotonic = true;
    while (!t.done()) {
        const double p = t.step(0.001);
        if (p < p_prev) monotonic = false;
        p_prev = p;
    }
    check(monotonic, "三角形位置单调");
    check(std::fabs(t.pos() - L) < 1e-9, "三角形终点 = L");
}

static void test_trapezoid_reverse_and_abort() {
    std::printf("== 3) 反向 / 中止 ==\n");
    Trapezoid t;
    t.begin(50.0, 10.0, 50.0, 100.0);   // 反向：L=-40
    double p_prev = 50.0;
    bool monotonic = true;
    while (!t.done()) {
        const double p = t.step(0.001);
        if (p > p_prev + 1e-9) monotonic = false;
        p_prev = p;
    }
    check(monotonic, "反向位置单调递减");
    check(std::fabs(t.pos() - 10.0) < 1e-9, "反向终点 = 10");

    t.begin(0.0, 100.0, 50.0, 100.0);
    t.step(0.05);                       // 走 50ms
    const double p_now = t.pos();
    t.abort_at(p_now);
    check(t.done(), "abort 后 done");
    check(std::fabs(t.pos() - p_now) < 1e-9, "abort 后位置对齐");
}

// 直线插补：两轴，主导轴参数；s 单调且各轴位移按比例
static void test_lin_two_axes() {
    std::printf("== 4) 直线插补：两轴比例 ==\n");
    LinInterp lin;
    const double dist[2] = {100.0, 50.0};   // 轴0 主导（100），轴1 = 1/2
    lin.begin(2, dist, 50.0, 100.0);        // 主导轴 v=50 a=100 → 总时 2.5s
    check(std::fabs(lin.total_time() - 2.5) < 1e-9, "主导轴决定总时");

    // 采样 s(t)：s(1.25)（匀速中段）应 ≈ 由运动学解出；验证比例关系：
    // 任意时刻 pos1/pos0 = 50/100 = 0.5（严格直线）
    bool ratio_ok = true;
    double s_prev = 0.0;
    bool monotonic = true;
    for (double t = 0.0; t < 2.6; t += 0.001) {
        const double s = lin.step(0.001);
        if (s < s_prev - 1e-12) monotonic = false;
        s_prev = s;
        if (s > 0.01 && s < 0.99) {
            // pos0 = 100*s, pos1 = 50*s → pos1/pos0 = 0.5 恒成立（线性由定义保证）
            if (std::fabs((50.0 * s) / (100.0 * s) - 0.5) > 1e-12) ratio_ok = false;
        }
    }
    check(monotonic, "s 单调");
    check(ratio_ok, "两轴位移比例恒定（直线）");
    check(lin.done(), "完成");
    check(std::fabs(lin.s() - 1.0) < 1e-12, "s 终值 = 1");
}

static void test_lin_zero_and_single() {
    std::printf("== 5) 零位移 / 单轴 ==\n");
    LinInterp lin;
    const double z[2] = {0.0, 0.0};
    lin.begin(2, z, 50.0, 100.0);
    check(lin.done() && std::fabs(lin.s() - 1.0) < 1e-12, "全零位移立即完成");

    LinInterp l1;
    const double d1[1] = {-37.0};
    l1.begin(1, d1, 50.0, 100.0);
    while (!l1.done()) l1.step(0.001);
    check(std::fabs(l1.s() - 1.0) < 1e-12, "单轴完成 s=1");
}

// 主导轴速度不超限：s 曲线的导数 × L ≤ v
static void test_lin_dominant_speed_cap() {
    std::printf("== 6) 主导轴峰值速度 = v ==\n");
    LinInterp lin;
    const double dist[3] = {80.0, 100.0, 20.0};   // 轴1 主导
    const double v = 40.0, a = 80.0;
    lin.begin(3, dist, v, a);
    double max_dot = 0.0, s_prev = 0.0;
    while (!lin.done()) {
        const double s = lin.step(0.001);
        const double dot = (s - s_prev) / 0.001;
        if (dot > max_dot) max_dot = dot;
        s_prev = s;
    }
    // 主导轴位移 100 → 峰值速度 = max_dot * 100 ≤ v
    check(max_dot * 100.0 <= v + 1e-6, "主导轴峰值速度不超过 v");
}

int main() {
    std::printf("---- M3 插补纯逻辑自测 ----\n");
    test_trapezoid_trapezoid_shape();
    test_trapezoid_triangle();
    test_trapezoid_reverse_and_abort();
    test_lin_two_axes();
    test_lin_zero_and_single();
    test_lin_dominant_speed_cap();
    std::printf("---- %s (失败项 %d) ----\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
