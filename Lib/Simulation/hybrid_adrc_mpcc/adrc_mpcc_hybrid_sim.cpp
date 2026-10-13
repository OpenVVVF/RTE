/**
 * Standalone closed-loop test for hybrid ADRC+FCS-MPCC.
 * Writes CSVs + metrics under results/adrc_mpcc_hybrid/ — does not touch
 * existing paper_foc_vs_mpc or mpcc_closed_loop outputs.
 *
 * Build/run from this folder via ./run_hybrid.sh
 */

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "AdrcMpccHybrid.h"
#include "simulation/BenchMotor.h"
#include "simulation/PmsmPlant.h"
#include "simulation/TwoLevelInverter.h"

namespace fs = std::filesystem;
using hybrid_adrc_mpcc::AdrcMpccHybrid;
using hybrid_adrc_mpcc::HybridInputs;
using hybrid_adrc_mpcc::HybridParams;
using namespace simulation;

struct Sample {
    double t = 0.0;
    float id = 0.0f;
    float iq = 0.0f;
    float id_ref = 0.0f;
    float iq_ref = 0.0f;
    float omega_rpm = 0.0f;
    float te = 0.0f;
    float fhat_d = 0.0f;
    float fhat_q = 0.0f;
    float cost = 0.0f;
    int sw = 0;
};

static void writeCsv(const fs::path& path, const std::vector<Sample>& rows) {
    std::ofstream out(path);
    out << "time,id,iq,id_ref,iq_ref,omega_rpm,te,fhat_d,fhat_q,cost,switching_state\n";
    out << std::setprecision(9);
    for (const auto& r : rows) {
        out << r.t << ',' << r.id << ',' << r.iq << ',' << r.id_ref << ',' << r.iq_ref << ','
            << r.omega_rpm << ',' << r.te << ',' << r.fhat_d << ',' << r.fhat_q << ',' << r.cost << ','
            << r.sw << '\n';
    }
}

struct Metrics {
    float iq_rmse = 0.0f;
    float id_rmse = 0.0f;
    float iq_rise_ms = 0.0f;
    float te_ripple_rms = 0.0f;
};

static Metrics computeMetrics(const std::vector<Sample>& s, float iq_target, double metric_start) {
    Metrics m;
    if (s.empty()) return m;
    double sum_iq = 0.0, sum_id = 0.0, sum_te = 0.0;
    int n = 0;
    float te_mean = 0.0f;
    int n_te = 0;
    for (const auto& r : s) {
        if (r.t < metric_start) continue;
        sum_iq += (r.iq - r.iq_ref) * (r.iq - r.iq_ref);
        sum_id += (r.id - r.id_ref) * (r.id - r.id_ref);
        te_mean += r.te;
        ++n_te;
        ++n;
    }
    if (n > 0) {
        m.iq_rmse = static_cast<float>(std::sqrt(sum_iq / n));
        m.id_rmse = static_cast<float>(std::sqrt(sum_id / n));
    }
    if (n_te > 0) te_mean /= static_cast<float>(n_te);
    for (const auto& r : s) {
        if (r.t < metric_start) continue;
        const float e = r.te - te_mean;
        sum_te += e * e;
    }
    if (n_te > 0) m.te_ripple_rms = static_cast<float>(std::sqrt(sum_te / n_te));

    const float thr_lo = 0.1f * std::fabs(iq_target);
    const float thr_hi = 0.9f * std::fabs(iq_target);
    double t10 = -1.0, t90 = -1.0;
    for (const auto& r : s) {
        if (r.t < 0.02) continue;
        if (t10 < 0.0 && std::fabs(r.iq) >= thr_lo) t10 = r.t;
        if (t90 < 0.0 && std::fabs(r.iq) >= thr_hi) {
            t90 = r.t;
            break;
        }
    }
    if (t10 >= 0.0 && t90 >= t10) {
        m.iq_rise_ms = static_cast<float>((t90 - t10) * 1000.0);
    }
    (void)iq_target;
    return m;
}

/** Locked-rotor current-loop step (shaft held). Appropriate with no load motor. */
static std::vector<Sample> runLockedRotorIqStep(const PmsmParameters& plant_motor,
                                                const PmsmParameters& ctrl_motor, float ts, float vdc,
                                                float iq_cmd, double t_end) {
    PmsmPlant plant(plant_motor);
    plant.reset({});

    HybridParams hp;
    hp.motor = ctrl_motor;
    hp.ts = ts;
    hp.i_base = 20.0f;
    hp.i_max = 40.0f;
    hp.omega_o = 2000.0f;
    AdrcMpccHybrid ctrl(hp);
    ctrl.reset();

    std::vector<Sample> rows;
    rows.reserve(static_cast<size_t>(t_end / ts) + 8);

    for (double t = 0.0; t < t_end; t += ts) {
        // Hold shaft: zero speed, advance electrical angle slowly for Park continuity.
        plant.state().omega_m = 0.0f;
        plant.state().omega_e = 0.0f;
        plant.state().theta_e = static_cast<float>(2.0 * 3.14159265 * 5.0 * t);  // 5 Hz elec.

        const float iq_ref = (t >= 0.02) ? iq_cmd : 0.0f;
        const float id_ref = 0.0f;

        HybridInputs in;
        in.id = plant.state().id;
        in.iq = plant.state().iq;
        in.id_ref = id_ref;
        in.iq_ref = iq_ref;
        in.theta_e = plant.state().theta_e;
        in.omega_e = 0.0f;
        in.vdc = vdc;
        in.enable = true;

        const auto out = ctrl.update(in);
        Dq vdq;
        parkAlphaBetaToDq({out.valpha, out.vbeta}, plant.state().theta_e, vdq);
        plant.step(vdq.d, vdq.q, /*Tl=*/0.0f, ts);
        // Re-assert locked rotor after plant mechanical integration.
        plant.state().omega_m = 0.0f;
        plant.state().omega_e = 0.0f;

        Sample r;
        r.t = t;
        r.id = plant.state().id;
        r.iq = plant.state().iq;
        r.id_ref = id_ref;
        r.iq_ref = iq_ref;
        r.omega_rpm = 0.0f;
        r.te = plant.state().torque_em;
        r.fhat_d = out.fhat_d;
        r.fhat_q = out.fhat_q;
        r.cost = out.min_cost;
        r.sw = static_cast<int>(out.switching_state);
        rows.push_back(r);
    }
    return rows;
}

int main(int argc, char** argv) {
    fs::path out_dir = "results/adrc_mpcc_hybrid";
    if (argc > 1) {
        out_dir = argv[1];
    }
    fs::create_directories(out_dir);

    const PmsmParameters motor = makeGen6BenchMotor();
    const float ts = 200e-6f;
    const float vdc = 48.0f;
    const float iq_cmd = 3.0f;
    const double t_end = 0.15;

    std::cout << "Hybrid ADRC+MPCC simulation (Gen6-like, locked-rotor current loop)\n";
    std::cout << "  out: " << out_dir << "\n";

    const auto nom = runLockedRotorIqStep(motor, motor, ts, vdc, iq_cmd, t_end);
    writeCsv(out_dir / "case1_iq_step_hybrid.csv", nom);
    const Metrics m1 = computeMetrics(nom, iq_cmd, 0.05);

    PmsmParameters mism = motor;
    mism.ld *= 0.8f;
    mism.lq *= 0.8f;
    const auto mis = runLockedRotorIqStep(motor, mism, ts, vdc, iq_cmd, t_end);
    writeCsv(out_dir / "case2_iq_step_L_mismatch_m20_hybrid.csv", mis);
    const Metrics m2 = computeMetrics(mis, iq_cmd, 0.05);

    {
        std::ofstream m(out_dir / "metrics_summary.csv");
        m << "case,id_rmse_A,iq_rmse_A,iq_rise_ms,te_ripple_rms_Nm\n";
        m << "iq_step_locked_rotor," << m1.id_rmse << ',' << m1.iq_rmse << ',' << m1.iq_rise_ms << ','
          << m1.te_ripple_rms << '\n';
        m << "iq_step_L_mismatch_m20," << m2.id_rmse << ',' << m2.iq_rmse << ',' << m2.iq_rise_ms << ','
          << m2.te_ripple_rms << '\n';
    }

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  case1 iq_rmse=" << m1.iq_rmse << " A  rise=" << m1.iq_rise_ms << " ms\n";
    std::cout << "  case2 (L-20%) iq_rmse=" << m2.iq_rmse << " A  rise=" << m2.iq_rise_ms << " ms\n";
    std::cout << "Done.\n";
    return 0;
}
