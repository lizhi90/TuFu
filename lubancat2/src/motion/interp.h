// interp.h —— M3：CSP 梯型规划 + 多轴直线插补（纯逻辑，不依赖 ecrt，可单测）
//
// Trapezoid：归一化梯型位置规划 p(t): p0 → p1（inc 单位），每 1ms step(dt)。
//             CSP 单轴定位用（Axis 内部持有）。
// LinInterp ：N 轴直线插补的共享进度 s(t): 0 → 1。
//             位置_i = start_i + dist_i * s(t) —— 所有轴共用同一 s，路径严格为直线；
//             speed/accel 作用于主导轴（|dist| 最大的轴），其余轴速度按比例缩放。
#pragma once

#include <cmath>

namespace kx {

// ---- 梯型/S 曲线位置规划（0→L，可含减速停段；done 后 pos() 恒为 L）----
//   smooth_s：S 曲线时间（秒，0=纯梯形；ZBasic `SRAMP=0~250ms`）——加减速各端平滑 smooth_s/2，
//   斜坡带 jerk 限幅（|da/dt| = a_pk / (smooth_s/2)），总时间比梯形延长（≈ smooth_s 的量级）。
class Trapezoid {
public:
    // p0/p1：起止位置（任意单位，inc 或归一化值均可）；vmax/amax 同单位
    // smooth_s：S 曲线时间（秒，0~0.25；0=纯梯形，路径与旧行为完全一致）
    void begin(double p0, double p1, double vmax, double amax, double smooth_s = 0.0) {
        p0_ = p0;
        L_  = p1 - p0;
        t_  = 0.0;
        ts_ = (smooth_s < 0.0) ? 0.0 : (smooth_s > 0.25 ? 0.25 : smooth_s);
        s_mode_ = ts_ > 0.0;
        if (s_mode_) { begin_s(vmax, amax); return; }
        if (!(vmax > 0.0) || !(amax > 0.0) || L_ == 0.0) {
            // 零位移/非法参数：立即完成
            L_      = L_;
            v_pk_   = 0.0;
            t1_     = 0.0;
            t_total_ = 0.0;
            done_   = true;
            return;
        }
        const double aL = std::fabs(L_);
        if (!(vmax > 0.0)) vmax = 1e-9;
        if (!(amax > 0.0)) amax = 1e-9;
        const double t_acc = vmax / amax;
        const double d_acc = 0.5 * vmax * t_acc;          // 单侧加速位移
        if (aL <= 2.0 * d_acc) {
            // 三角形：达不到 vmax
            v_pk_    = std::sqrt(aL * amax);
            t1_      = v_pk_ / amax;
            t_total_ = 2.0 * t1_;
        } else {
            v_pk_    = vmax;
            t1_      = t_acc;
            t_total_ = 2.0 * t1_ + (aL - 2.0 * d_acc) / vmax;
        }
        done_ = false;
    }

    // 推进 dt（秒），返回本拍位置
    double step(double dt) {
        if (done_) return p0_ + L_;
        if (s_mode_) return step_s(dt);
        t_ += dt;
        if (t_ >= t_total_) {
            t_     = t_total_;
            done_  = true;
            return p0_ + L_;
        }
        return p0_ + raw_pos(t_);
    }

    double pos()  const { return done_ ? p0_ + L_ : p0_ + raw_pos(t_); }
    double vel()  const { return done_ ? 0.0 : raw_vel(t_); }
    bool   done() const { return done_; }
    double total_time() const { return t_total_; }
    // 急停：立即对齐到当前位置并结束（供 STOP/掉线时 DPOS 对齐）
    void   abort_at(double p_now) { L_ = p_now - p0_; t_ = t_total_ = 0.0; done_ = true; }

private:
    // ---- S 曲线（jerk 限幅）----
    void begin_s(double vmax, double amax) {
        const double aL = std::fabs(L_);
        v_pk_ = 0.0; t_total_ = 0.0; t_cruise_ = 0.0; tc_ = 0.0; tj_ = 0.0; d_ramp_ = 0.0;
        if (!(vmax > 0.0) || !(amax > 0.0) || aL == 0.0) { done_ = true; return; }
        tj_ = ts_ / 2.0;                                   // 每端 jerk 相时长（两端合计 = smooth_s）
        const auto ramp_dist = [&](double v, double* a_pk, double* tc) {
            double a2 = amax, t2 = v / amax - tj_;
            if (t2 < 0.0) { a2 = 2.0 * v / ts_; t2 = 0.0; }   // 低速：降峰值加速度，保持 S 时间
            *a_pk = a2; *tc = t2;
            return a2 * (t2 * t2 / 2.0 + 1.5 * t2 * tj_ + tj_ * tj_);
        };
        d_ramp_ = ramp_dist(vmax, &a_pk_, &tc_);
        if (2.0 * d_ramp_ <= aL) {                         // 梯形（含 S 斜坡 + 匀速）
            v_pk_      = a_pk_ * (tj_ + tc_);
            t_cruise_  = (aL - 2.0 * d_ramp_) / v_pk_;
        } else {                                           // 三角形（S）：二分求达不到 vmax 的峰值
            double lo = 1e-9, hi = vmax, a2 = amax, t2 = 0.0;
            for (int i = 0; i < 60; ++i) {
                const double v = 0.5 * (lo + hi);
                const double D = ramp_dist(v, &a2, &t2);
                if (2.0 * D >= aL) hi = v; else lo = v;
            }
            (void)ramp_dist(0.5 * (lo + hi), &a_pk_, &tc_);
            v_pk_     = a_pk_ * (tj_ + tc_);
            d_ramp_   = aL / 2.0;
            t_cruise_ = 0.0;
        }
        t_total_ = 2.0 * (2.0 * tj_ + tc_) + t_cruise_;
        done_ = false;
    }
    // 加速侧位置/速度（τ ∈ [0, 2·tj+tc]；jerk 相 → 恒加速相 → jerk 相）
    double s_pos_ramp(double tau) const {
        const double J = (tj_ > 0.0) ? a_pk_ / tj_ : 0.0;
        if (tau <= 0.0) return 0.0;
        if (tau <= tj_) return J * tau * tau * tau / 6.0;
        const double p1 = J * tj_ * tj_ * tj_ / 6.0;
        const double v1 = 0.5 * a_pk_ * tj_;
        if (tau <= tj_ + tc_) {
            const double td = tau - tj_;
            return p1 + v1 * td + 0.5 * a_pk_ * td * td;
        }
        const double p2 = p1 + v1 * tc_ + 0.5 * a_pk_ * tc_ * tc_;
        const double v2 = a_pk_ * (0.5 * tj_ + tc_);
        const double td = tau - (tj_ + tc_);
        return p2 + v2 * td + 0.5 * a_pk_ * td * td - J * td * td * td / 6.0;
    }
    double s_vel_ramp(double tau) const {
        if (tau <= 0.0) return 0.0;
        if (tau <= tj_) return 0.5 * a_pk_ * tau * tau / tj_;
        if (tau <= tj_ + tc_) return a_pk_ * (0.5 * tj_ + (tau - tj_));
        const double td = tau - (tj_ + tc_);
        return a_pk_ * (0.5 * tj_ + tc_ + td) - 0.5 * a_pk_ * td * td / tj_;
    }
    double raw_pos_s(double t) const {
        const double sgn = (L_ < 0.0) ? -1.0 : 1.0;
        const double aL  = std::fabs(L_);
        const double Tr  = 2.0 * tj_ + tc_;                // 单侧斜坡时长
        if (t <= 0.0) return 0.0;
        if (t >= t_total_) return sgn * aL;
        if (t <= Tr) return sgn * s_pos_ramp(t);                      // 加速（含 S）
        if (t <= Tr + t_cruise_) return sgn * (d_ramp_ + v_pk_ * (t - Tr));  // 匀速
        const double td = t_total_ - t;                    // 减速 = 加速的时间镜像
        return sgn * (aL - s_pos_ramp(td));
    }
    double raw_vel_s(double t) const {
        const double sgn = (L_ < 0.0) ? -1.0 : 1.0;
        const double Tr  = 2.0 * tj_ + tc_;
        if (t <= 0.0) return 0.0;
        if (t >= t_total_) return 0.0;
        if (t <= Tr) return sgn * s_vel_ramp(t);
        if (t <= Tr + t_cruise_) return sgn * v_pk_;
        return sgn * s_vel_ramp(t_total_ - t);
    }
    double step_s(double dt) {
        t_ += dt;
        if (t_ >= t_total_) { t_ = t_total_; done_ = true; return p0_ + L_; }
        return p0_ + raw_pos_s(t_);
    }

    double raw_pos(double t) const {
        return s_mode_ ? raw_pos_s(t) : raw_pos_trap(t);
    }
    double raw_vel(double t) const {
        return s_mode_ ? raw_vel_s(t) : raw_vel_trap(t);
    }
    double raw_pos_trap(double t) const {
        const double sgn = (L_ < 0.0) ? -1.0 : 1.0;
        const double aL  = std::fabs(L_);
        if (t <= 0.0) return 0.0;
        if (t <= t1_) return sgn * 0.5 * v_pk_ * t * t / std::max(t1_, 1e-12);   // 加速段
        const double t2 = t_total_ - t1_;
        if (t <= t2)  return sgn * (0.5 * v_pk_ * t1_ + v_pk_ * (t - t1_));      // 匀速段
        const double td = t_total_ - t;                                          // 减速段
        return sgn * (aL - 0.5 * v_pk_ * td * td / std::max(t1_, 1e-12));
    }
    double raw_vel_trap(double t) const {
        const double sgn = (L_ < 0.0) ? -1.0 : 1.0;
        if (t <= 0.0) return 0.0;
        if (t <= t1_) return sgn * v_pk_ * t / std::max(t1_, 1e-12);
        const double t2 = t_total_ - t1_;
        if (t <= t2)  return sgn * v_pk_;
        return sgn * v_pk_ * (t_total_ - t) / std::max(t1_, 1e-12);
    }

    double p0_ = 0.0;
    double L_  = 0.0;
    double v_pk_ = 0.0, t1_ = 0.0, t_total_ = 0.0;
    // S 曲线（smooth_s>0）
    bool   s_mode_ = false;
    double ts_ = 0.0;                     // S 曲线时间（秒）
    double tj_ = 0.0, tc_ = 0.0;          // jerk 相 / 恒加速相时长
    double a_pk_ = 0.0;                   // 峰值加速度（低速时 < amax）
    double d_ramp_ = 0.0, t_cruise_ = 0.0;
    double t_ = 0.0;
    bool  done_ = true;
};

// ---- 多轴直线插补：共享进度 s(t): 0 → 1 ----
class LinInterp {
public:
    static constexpr int kMaxLegs = 8;

    // dist[i]：各轴位移（单位一致即可，inc/mm 皆可）；vmax/amax 为主导轴参数
    void begin(int n, const double* dist, double vmax, double amax) {
        n_ = n > 0 ? (n < kMaxLegs ? n : kMaxLegs) : 0;
        L_ = 0.0;
        for (int i = 0; i < n_; ++i) {
            dist_[i] = dist[i];
            const double a = std::fabs(dist[i]);
            if (a > L_) L_ = a;
        }
        if (L_ <= 0.0) {          // 全零位移：立即完成
            L_ = 1.0;             // 防除零；s 直接 1
            norm_.begin(0.0, 1.0, 1.0, 1.0);
            s_ = 1.0;
            return;
        }
        norm_.begin(0.0, L_, vmax, amax);
        s_ = 0.0;
    }

    // 推进 dt（秒），返回进度 s ∈ [0,1]
    double step(double dt) {
        if (norm_.done()) { s_ = 1.0; return s_; }
        s_ = norm_.step(dt) / L_;
        if (s_ > 1.0) s_ = 1.0;
        if (s_ < 0.0) s_ = 0.0;
        return s_;
    }

    double s() const { return s_; }
    bool  done() const { return s_ >= 1.0; }
    int   legs() const { return n_; }
    double dist(int i) const { return (i >= 0 && i < n_) ? dist_[i] : 0.0; }
    // 主导轴完成时刻（秒）：组总时长上界
    double total_time() const { return norm_.total_time(); }

private:
    Trapezoid norm_;               // 0 → L（主导轴位移）
    double    dist_[kMaxLegs] = {0};
    double    L_ = 0.0;
    double    s_ = 0.0;
    int       n_ = 0;
};

} // namespace kx
