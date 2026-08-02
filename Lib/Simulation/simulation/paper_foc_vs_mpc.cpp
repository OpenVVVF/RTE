#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "simulation/BenchMotor.h"
#include "simulation/FocController.h"
#include "simulation/MpccController.h"
#include "simulation/PmsmPlant.h"
#include "simulation/Svpwm.h"
#include "simulation/Transforms.h"

namespace fs = std::filesystem;
using namespace simulation;

/** Deadbeat voltage + SVPWM (continuous). Used for speed-loop fairness vs FOC. */
static void mpccDeadbeatSvpwm(float id, float iq, float id_ref, float iq_ref, float theta_e, float omega_e,
                              float vdc, float ts, const PmsmParameters& m, float& valpha, float& vbeta) {
    float vd = m.rs * id + (m.ld / ts) * (id_ref - id) - omega_e * m.lq * iq;
    float vq = m.rs * iq + (m.lq / ts) * (iq_ref - iq) + omega_e * m.ld * id + omega_e * m.psi_f;
    const float v_max = (vdc / kSqrt3) * 0.95f;
    const float mag = std::sqrt(vd * vd + vq * vq);
    if (mag > v_max && mag > 1e-9f) {
        vd *= v_max / mag;
        vq *= v_max / mag;
    }
    AlphaBeta ab;
    inverseParkDqToAlphaBeta({vd, vq}, theta_e, ab);
    const AlphaBeta pwm = svpwmAlphaBeta(ab.alpha, ab.beta, vdc);
    valpha = pwm.alpha;
    vbeta = pwm.beta;
}

struct Sample {
    double t = 0.0;
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float ia = 0.0f;
    float omega_m = 0.0f;
    float omega_rpm = 0.0f;
    float te = 0.0f;
    float tl = 0.0f;
    float duty = 0.0f;
};

static void writeCsv(const fs::path& path, const std::vector<Sample>& s, const std::string& label) {
    std::ofstream out(path);
    out << "time,controller,id,iq,id_ref,iq_ref,ia,omega_m,omega_rpm,te,tl,duty\n";
    out << std::setprecision(9);
    for (const auto& r : s) {
        out << r.t << ',' << label << ',' << r.id << ',' << r.iq << ',' << r.id_ref << ',' << r.iq_ref << ','
            << r.ia << ',' << r.omega_m << ',' << r.omega_rpm << ',' << r.te << ',' << r.tl << ',' << r.duty
            << '\n';
    }
}

struct Metrics {
    float iq_rise_ms = 0.0f;
    float iq_overshoot_pct = 0.0f;
    float iq_settle_ms = 0.0f;
    float iq_ripple_rms = 0.0f;
    float te_ripple_rms = 0.0f;
    float rpm_final = 0.0f;
    float rpm_rmse = 0.0f;
    float iq_rmse = 0.0f;
};

static float rpmFromOmega(float omega_m) { return omega_m * 60.0f / (2.0f * 3.14159265f); }

static Metrics computeIqStepMetrics(const std::vector<Sample>& s, float iq_target, double settle_start) {
    Metrics m;
    if (s.empty()) {
        return m;
    }
    float peak = 0.0f;
    double t10 = -1.0;
    double t90 = -1.0;
    double t_settle = -1.0;
    const float band = 0.02f * std::abs(iq_target);
    for (const auto& r : s) {
        peak = std::max(peak, r.iq);
        if (t10 < 0.0 && r.iq >= 0.1f * iq_target) {
            t10 = r.t;
        }
        if (t90 < 0.0 && r.iq >= 0.9f * iq_target) {
            t90 = r.t;
        }
    }
    if (t10 >= 0.0 && t90 >= 0.0) {
        m.iq_rise_ms = static_cast<float>((t90 - t10) * 1e3);
    }
    m.iq_overshoot_pct = (peak - iq_target) / iq_target * 100.0f;

    for (const auto& r : s) {
        if (r.t < 0.002) {
            continue;
        }
        if (std::abs(r.iq - iq_target) <= band) {
            bool ok = true;
            for (const auto& q : s) {
                if (q.t < r.t) {
                    continue;
                }
                if (std::abs(q.iq - iq_target) > band) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                t_settle = r.t;
                break;
            }
        }
    }
    if (t_settle >= 0.0) {
        m.iq_settle_ms = static_cast<float>(t_settle * 1e3);
    }

    double sum_e2 = 0.0;
    double sum_te2 = 0.0;
    double sum_iq_e2 = 0.0;
    int n = 0;
    for (const auto& r : s) {
        if (r.t < settle_start) {
            continue;
        }
        const float ei = r.iq - iq_target;
        sum_e2 += static_cast<double>(ei * ei);
        // ripple around local mean approximated by deviation from target after settle
        sum_te2 += static_cast<double>((r.te - r.tl) * (r.te - r.tl));  // rough; refined below
        sum_iq_e2 += static_cast<double>(ei * ei);
        ++n;
    }
    if (n > 0) {
        m.iq_ripple_rms = static_cast<float>(std::sqrt(sum_iq_e2 / n));
        m.iq_rmse = m.iq_ripple_rms;
    }

    // Torque ripple about mean Te in settle window
    double mean_te = 0.0;
    int n2 = 0;
    for (const auto& r : s) {
        if (r.t < settle_start) {
            continue;
        }
        mean_te += r.te;
        ++n2;
    }
    if (n2 > 0) {
        mean_te /= n2;
        double var = 0.0;
        for (const auto& r : s) {
            if (r.t < settle_start) {
                continue;
            }
            const double d = r.te - mean_te;
            var += d * d;
        }
        m.te_ripple_rms = static_cast<float>(std::sqrt(var / n2));
    }
    m.rpm_final = s.back().omega_rpm;
    (void)sum_e2;
    (void)sum_te2;
    return m;
}

static Metrics computeSpeedMetrics(const std::vector<Sample>& s, float rpm_ref, double settle_start) {
    Metrics m = computeIqStepMetrics(s, s.empty() ? 0.0f : s.back().iq_ref, settle_start);
    double sum = 0.0;
    int n = 0;
    for (const auto& r : s) {
        if (r.t < settle_start) {
            continue;
        }
        const double e = r.omega_rpm - rpm_ref;
        sum += e * e;
        ++n;
    }
    if (n > 0) {
        m.rpm_rmse = static_cast<float>(std::sqrt(sum / n));
    }
    m.rpm_final = s.empty() ? 0.0f : s.back().omega_rpm;
    return m;
}

enum class CtrlKind { FOC, MPC };

static std::vector<Sample> runIqStep(CtrlKind kind, float duration, float ts, float vdc, float iq_ref) {
    // Locked-rotor / constant-speed current test (Li et al. style): isolate
    // the current loop. Mechanical states are frozen after each electrical step.
    auto motor = makeGen6BenchMotor();
    motor.inertia = 1.0e6f;  // effectively locked for the short current test
    motor.viscous_friction = 0.0f;
    PmsmPlant plant(motor);
    plant.reset();

    FocController foc(makeGen6FocParams(motor, ts));
    MpccController mpc(makeGen6MpccParams(motor, ts, MPCCMode::OptimalDutyCycle));
    foc.reset();
    mpc.reset();

    std::vector<Sample> out;
    const int steps = static_cast<int>(duration / ts);
    for (int k = 0; k < steps; ++k) {
        const double t = k * ts;
        const auto& st = plant.state();
        float valpha = 0.0f;
        float vbeta = 0.0f;
        float duty = 1.0f;

        if (kind == CtrlKind::FOC) {
            FocInputs in;
            in.id = st.id;
            in.iq = st.iq;
            in.id_ref = 0.0f;
            in.iq_ref = iq_ref;
            in.theta_e = st.theta_e;
            in.omega_e = st.omega_e;
            in.vdc = vdc;
            in.enable = true;
            const auto o = foc.update(in);
            valpha = o.valpha;
            vbeta = o.vbeta;
        } else {
            MpccInputs in;
            in.id = st.id;
            in.iq = st.iq;
            in.id_ref = 0.0f;
            in.iq_ref = iq_ref;
            in.theta_e = st.theta_e;
            in.omega_e = st.omega_e;
            in.vdc = vdc;
            in.enable = true;
            const auto o = mpc.update(in);
            valpha = o.valpha;
            vbeta = o.vbeta;
            duty = o.duty_active;
        }

        plant.stepAlphaBeta(valpha, vbeta, /*load=*/0.0f, ts);
        // Freeze mechanical dynamics for a pure current-loop comparison.
        plant.state().omega_m = 0.0f;
        plant.state().omega_e = 0.0f;
        plant.state().theta_m = 0.0f;
        // Keep a slowly advancing electrical angle so phase currents look realistic.
        plant.state().theta_e = wrapAngle0TwoPi(st.theta_e + 2.0f * 3.14159265f * 20.0f * ts);

        Sample s;
        s.t = t;
        s.id = plant.state().id;
        s.iq = plant.state().iq;
        s.id_ref = 0.0f;
        s.iq_ref = iq_ref;
        s.ia = plant.state().ia;
        s.omega_m = 0.0f;
        s.omega_rpm = 0.0f;
        s.te = plant.state().torque_em;
        s.tl = 0.0f;
        s.duty = duty;
        out.push_back(s);
    }
    return out;
}

static std::vector<Sample> runSpeedTo2000(CtrlKind kind, float duration, float ts, float vdc, float rpm_ref,
                                          float load_nm, float load_at_s) {
    const auto motor = makeGen6BenchMotor();
    PmsmPlant plant(motor);
    plant.reset();

    FocController foc(makeGen6FocParams(motor, ts));
    foc.reset();

    // Same outer speed PI for both — compare current loops only.
    // Mild gains so the outer loop does not fight a stiff current loop.
    const float speed_kp = 0.04f;
    const float speed_ki = 0.8f;
    const float iq_max = 15.0f;
    float speed_i = 0.0f;

    std::vector<Sample> out;
    const int steps = static_cast<int>(duration / ts);
    const float omega_ref = rpm_ref * 2.0f * 3.14159265f / 60.0f;

    for (int k = 0; k < steps; ++k) {
        const double t = k * ts;
        const auto& st = plant.state();
        const float tl = (t >= load_at_s) ? load_nm : 0.0f;

        const float e_w = omega_ref - st.omega_m;
        speed_i += e_w * ts;
        float iq_ref = speed_kp * e_w + speed_ki * speed_i;
        iq_ref = std::max(-iq_max, std::min(iq_max, iq_ref));

        float valpha = 0.0f;
        float vbeta = 0.0f;
        float duty = 1.0f;

        if (kind == CtrlKind::FOC) {
            FocInputs in;
            in.id = st.id;
            in.iq = st.iq;
            in.id_ref = 0.0f;
            in.iq_ref = iq_ref;
            in.theta_e = st.theta_e;
            in.omega_e = st.omega_e;
            in.vdc = vdc;
            in.enable = true;
            const auto o = foc.update(in);
            valpha = o.valpha;
            vbeta = o.vbeta;
        } else {
            // Predictive deadbeat current law + SVPWM (same actuator as FOC).
            mpccDeadbeatSvpwm(st.id, st.iq, 0.0f, iq_ref, st.theta_e, st.omega_e, vdc, ts, motor, valpha,
                              vbeta);
        }

        plant.stepAlphaBeta(valpha, vbeta, tl, ts);

        Sample s;
        s.t = t;
        s.id = plant.state().id;
        s.iq = plant.state().iq;
        s.id_ref = 0.0f;
        s.iq_ref = iq_ref;
        s.ia = plant.state().ia;
        s.omega_m = plant.state().omega_m;
        s.omega_rpm = rpmFromOmega(plant.state().omega_m);
        s.te = plant.state().torque_em;
        s.tl = tl;
        s.duty = duty;
        out.push_back(s);
    }
    return out;
}

static void printMetrics(const char* name, const Metrics& m) {
    std::cout << "  [" << name << "]\n"
              << "    iq rise (10-90%): " << m.iq_rise_ms << " ms\n"
              << "    iq overshoot:     " << m.iq_overshoot_pct << " %\n"
              << "    iq settle(~2%):   " << m.iq_settle_ms << " ms\n"
              << "    iq RMSE (ss):     " << m.iq_ripple_rms << " A\n"
              << "    Te ripple RMS:    " << m.te_ripple_rms << " Nm\n"
              << "    final RPM:        " << m.rpm_final << "\n"
              << "    RPM RMSE (ss):    " << m.rpm_rmse << "\n";
}

int main() {
    // Vdc with margin for ~2000 rpm with psi≈0.072 Wb (back-EMF ~75 V).
    const float vdc = 180.0f;
    const float ts = 200e-6f;  // matches foc_demo Dt

    const fs::path out_dir = fs::path("results") / "paper_foc_vs_mpc";
    fs::create_directories(out_dir);

    std::cout << "============================================================\n"
              << " FOC vs FCS-MPCC (Optimal Duty) — Gen6 bench parameters\n"
              << " Local study only (RTE git untouched)\n"
              << " Ts=" << ts << " s, Vdc=" << vdc << " V, rpm_ref=2000\n"
              << "============================================================\n";

    // --- Case 1: iq step (Li/Zhang-style dynamic comparison) ---
    std::cout << "\nCase 1: iq* step 0 → 10 A (current-loop dynamics)\n";
    const auto foc_iq = runIqStep(CtrlKind::FOC, 0.05f, ts, vdc, 10.0f);
    const auto mpc_iq = runIqStep(CtrlKind::MPC, 0.05f, ts, vdc, 10.0f);
    writeCsv(out_dir / "case1_iq_step_foc.csv", foc_iq, "FOC");
    writeCsv(out_dir / "case1_iq_step_mpc.csv", mpc_iq, "MPC");
    const auto m_foc_iq = computeIqStepMetrics(foc_iq, 10.0f, 0.02);
    const auto m_mpc_iq = computeIqStepMetrics(mpc_iq, 10.0f, 0.02);
    printMetrics("FOC", m_foc_iq);
    printMetrics("MPC", m_mpc_iq);

    // --- Case 2: speed to 2000 rpm + load step ---
    std::cout << "\nCase 2: speed → 2000 rpm, load 0→1 Nm at t=0.25 s\n"
              << "  (MPC = deadbeat predictive current + SVPWM; FOC = PI + SVPWM)\n";
    const auto foc_sp = runSpeedTo2000(CtrlKind::FOC, 0.5f, ts, vdc, 2000.0f, 1.0f, 0.25f);
    const auto mpc_sp = runSpeedTo2000(CtrlKind::MPC, 0.5f, ts, vdc, 2000.0f, 1.0f, 0.25f);
    writeCsv(out_dir / "case2_speed_2000_foc.csv", foc_sp, "FOC");
    writeCsv(out_dir / "case2_speed_2000_mpc.csv", mpc_sp, "MPC");
    const auto m_foc_sp = computeSpeedMetrics(foc_sp, 2000.0f, 0.35);
    const auto m_mpc_sp = computeSpeedMetrics(mpc_sp, 2000.0f, 0.35);
    printMetrics("FOC", m_foc_sp);
    printMetrics("MPC", m_mpc_sp);

    // Summary CSV
    {
        std::ofstream sum(out_dir / "metrics_summary.csv");
        sum << "case,controller,iq_rise_ms,iq_overshoot_pct,iq_settle_ms,iq_rmse_A,te_ripple_rms_Nm,rpm_final,"
               "rpm_rmse\n";
        sum << "iq_step,FOC," << m_foc_iq.iq_rise_ms << ',' << m_foc_iq.iq_overshoot_pct << ','
            << m_foc_iq.iq_settle_ms << ',' << m_foc_iq.iq_ripple_rms << ',' << m_foc_iq.te_ripple_rms << ','
            << m_foc_iq.rpm_final << ',' << m_foc_iq.rpm_rmse << '\n';
        sum << "iq_step,MPC," << m_mpc_iq.iq_rise_ms << ',' << m_mpc_iq.iq_overshoot_pct << ','
            << m_mpc_iq.iq_settle_ms << ',' << m_mpc_iq.iq_ripple_rms << ',' << m_mpc_iq.te_ripple_rms << ','
            << m_mpc_iq.rpm_final << ',' << m_mpc_iq.rpm_rmse << '\n';
        sum << "speed_2000,FOC," << m_foc_sp.iq_rise_ms << ',' << m_foc_sp.iq_overshoot_pct << ','
            << m_foc_sp.iq_settle_ms << ',' << m_foc_sp.iq_ripple_rms << ',' << m_foc_sp.te_ripple_rms << ','
            << m_foc_sp.rpm_final << ',' << m_foc_sp.rpm_rmse << '\n';
        sum << "speed_2000,MPC," << m_mpc_sp.iq_rise_ms << ',' << m_mpc_sp.iq_overshoot_pct << ','
            << m_mpc_sp.iq_settle_ms << ',' << m_mpc_sp.iq_ripple_rms << ',' << m_mpc_sp.te_ripple_rms << ','
            << m_mpc_sp.rpm_final << ',' << m_mpc_sp.rpm_rmse << '\n';
    }

    std::cout << "\nWrote CSVs under " << out_dir << "\n"
              << "Next: python3 scripts/plot_paper_foc_vs_mpc.py\n";

    // Appropriateness: Case 1 current dynamics (primary claim of FCS-MPCC).
    const bool mpc_faster = m_mpc_iq.iq_rise_ms > 0.0f && m_mpc_iq.iq_rise_ms < m_foc_iq.iq_rise_ms;
    const bool mpc_tracks = m_mpc_iq.iq_ripple_rms < 1.0f;
    const bool foc_tracks = m_foc_iq.iq_ripple_rms < 1.0f;
    const bool both_speed = m_mpc_sp.rpm_final > 1800.0f && m_foc_sp.rpm_final > 1800.0f;
    std::cout << "\nAppropriateness check:\n"
              << "  Case1 FOC tracks iq*?            " << (foc_tracks ? "YES" : "NO") << "\n"
              << "  Case1 MPC tracks iq*?            " << (mpc_tracks ? "YES" : "NO") << "\n"
              << "  Case1 MPC faster rise than FOC?  " << (mpc_faster ? "YES" : "NO") << "\n"
              << "  Case2 both near 2000 rpm?        " << (both_speed ? "YES" : "NO") << "\n";

    return (mpc_faster && mpc_tracks && foc_tracks && both_speed) ? 0 : 1;
}
