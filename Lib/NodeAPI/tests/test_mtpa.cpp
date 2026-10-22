// Numeric regression tests for Assets/NodeTemplates/Control.Mtpa.
//
// Regression anchor: the shipped node selected the anti-MTPA root of the MTPA
// current-angle quadratic (Id > 0 for a salient machine with Lq > Ld), costing
// ~8.3% torque at Is=100 A (Ld=70uH, Lq=120uH, Lambda=0.023). These tests
// compile the shipped inline.cpp verbatim against the same rte:: quantity
// types the firmware codegen provides, and pin it against the closed-form
// physical root as well as a dense brute-force torque sweep.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <limits>

#include "InverterCodegen/RteQuantity.h"
#include <NodeAPI/NodeAPI.h>

using namespace NodeAPI;

namespace {

struct MtpaResult {
    float id;
    float iq;
};

// Mirrors the codegen environment of the node: the CurrentRef input and the
// Ld/Lq/Lambda parameters are plain floats (dimensionless quantities stay
// built-in); CurrentMax and the I_D/I_Q outputs are au-backed currents.
MtpaResult RunMtpaNode(float currentRef, float currentMax, float ld, float lq, float lambda) {
    float CurrentRef = currentRef;
    float Ld = ld;
    float Lq = lq;
    float Lambda = lambda;
    rte::Current CurrentMax = rte::Amperes(currentMax);
    rte::Current I_D = rte::Amperes(0.0f);
    rte::Current I_Q = rte::Amperes(0.0f);

#include "../../../Assets/NodeTemplates/Control.Mtpa/inline.cpp"

    return {I_D.in(au::amperes), I_Q.in(au::amperes)};
}

// Electromagnetic torque up to the (positive, irrelevant) 1.5*pp factor.
double MtpaTorque(double id, double iq, double ld, double lq, double lambda) {
    return lambda * iq + (ld - lq) * id * iq;
}

// Dense sweep of the current angle at fixed stator-current magnitude; returns
// the best torque found together with the (id, iq) that achieves it.
double BruteForceMaxTorque(double is, double ld, double lq, double lambda,
                           double* bestId, double* bestIq) {
    double best = -std::numeric_limits<double>::infinity();
    *bestId = 0.0;
    *bestIq = 0.0;
    constexpr double kPi = 3.14159265358979323846;
    constexpr int kSteps = 720000;  // 0.0005-degree resolution
    for (int k = 0; k < kSteps; ++k) {
        const double beta = 2.0 * kPi * k / kSteps;
        const double id = is * std::cos(beta);
        const double iq = is * std::sin(beta);
        const double t = MtpaTorque(id, iq, ld, lq, lambda);
        if (t > best) {
            best = t;
            *bestId = id;
            *bestIq = iq;
        }
    }
    return best;
}

}  // namespace

TEST(NodeTemplateMtpa, SelectsPhysicalRootForSalientMachine) {
    // Dossier case: Ld=70uH, Lq=120uH, Lambda=0.023, Is=100A.
    // Closed form: radical = sqrt(0.023^2 + 8*(50e-6)^2*100^2) = 0.027, so the
    // physical root is cos(beta) = (0.023-0.027)/(4*50e-6*100) = -0.2 exactly.
    const auto out = RunMtpaNode(1.0f, 100.0f, 70e-6f, 120e-6f, 0.023f);
    EXPECT_NEAR(out.id, -20.0f, 1e-3f);
    EXPECT_NEAR(out.iq, 97.9796f, 1e-3f);
    EXPECT_LT(out.id, 0.0f);  // flux-WEAKENING direction, never adding

    // Braking (negative command): Id must still be negative so the reluctance
    // term keeps adding torque; only Iq flips sign.
    const auto braking = RunMtpaNode(-1.0f, 100.0f, 70e-6f, 120e-6f, 0.023f);
    EXPECT_NEAR(braking.id, -20.0f, 1e-3f);
    EXPECT_NEAR(braking.iq, -97.9796f, 1e-3f);
}

TEST(NodeTemplateMtpa, SalientOutputSitsOnBruteForceOptimum) {
    const double ld = 70e-6, lq = 120e-6, lambda = 0.023, is = 100.0;
    const auto out = RunMtpaNode(1.0f, 100.0f, static_cast<float>(ld),
                                 static_cast<float>(lq), static_cast<float>(lambda));

    double bestId = 0.0, bestIq = 0.0;
    const double tGrid = BruteForceMaxTorque(is, ld, lq, lambda, &bestId, &bestIq);
    EXPECT_NEAR(static_cast<double>(out.id), bestId, 0.05);
    EXPECT_NEAR(static_cast<double>(out.iq), bestIq, 0.05);

    const double tNode = MtpaTorque(out.id, out.iq, ld, lq, lambda);
    EXPECT_GE(tNode, tGrid * (1.0 - 1e-6));

    // The previously emitted anti-MTPA branch (Id = +20) loses ~8.3% of the
    // available torque; the fix must be clearly separated from it.
    const double tAnti = MtpaTorque(20.0, 97.9796, ld, lq, lambda);
    EXPECT_GT(tNode, tAnti * 1.05);
}

TEST(NodeTemplateMtpa, HandlesLdGreaterThanLq) {
    // Same machine flipped (Ld=120uH, Lq=70uH): root selection must follow the
    // salience sign physically, Id > 0 now adds torque.
    const double ld = 120e-6, lq = 70e-6, lambda = 0.023, is = 100.0;
    const auto out = RunMtpaNode(1.0f, 100.0f, static_cast<float>(ld),
                                 static_cast<float>(lq), static_cast<float>(lambda));
    EXPECT_NEAR(out.id, 20.0f, 1e-3f);
    EXPECT_NEAR(out.iq, 97.9796f, 1e-3f);

    double bestId = 0.0, bestIq = 0.0;
    const double tGrid = BruteForceMaxTorque(is, ld, lq, lambda, &bestId, &bestIq);
    EXPECT_NEAR(static_cast<double>(out.id), bestId, 0.05);
    const double tNode = MtpaTorque(out.id, out.iq, ld, lq, lambda);
    EXPECT_GE(tNode, tGrid * (1.0 - 1e-6));
}

TEST(NodeTemplateMtpa, SpmMachineDegeneratesToZeroId) {
    // SPM (Ld == Lq, as in Assets/Examples/foc_mtpa_demo.json): no saliency
    // means no d-axis current and no division by the zero inductance delta.
    const auto out = RunMtpaNode(1.0f, 100.0f, 1e-4f, 1e-4f, 0.023f);
    EXPECT_EQ(out.id, 0.0f);
    EXPECT_NEAR(out.iq, 100.0f, 1e-3f);

    const auto braking = RunMtpaNode(-1.0f, 100.0f, 1e-4f, 1e-4f, 0.023f);
    EXPECT_EQ(braking.id, 0.0f);
    EXPECT_NEAR(braking.iq, -100.0f, 1e-3f);
}

TEST(NodeTemplateMtpa, MachineParamsDeclareSiblingUnitConvention) {
    Graph graph;
    const std::filesystem::path templates = std::filesystem::path(__FILE__)
                                                .parent_path()
                                                .parent_path() /
                                            ".." / ".." / "Assets" / "NodeTemplates";
    const auto result = LoadNodeTypesFromDirectory(graph, templates);
    ASSERT_TRUE(result.ok);

    const auto type = graph.FindNodeType("Control.Mtpa");
    ASSERT_TRUE(type.has_value());

    // The Quantity enum has no inductance/flux values, so machine params follow
    // the sibling Plant.Machine.PmsmDq convention: quantity stays dimensionless
    // and the description carries the SI unit.
    for (const char* name : {"Ld", "Lq", "Lambda"}) {
        SCOPED_TRACE(name);
        const auto param = type->FindParameterType(name);
        ASSERT_TRUE(param.has_value());
        EXPECT_EQ(param->quantity, Quantity::Dimensionless);
    }
    const auto ldDesc = type->FindParameterDescription("Ld");
    const auto lqDesc = type->FindParameterDescription("Lq");
    const auto lambdaDesc = type->FindParameterDescription("Lambda");
    ASSERT_TRUE(ldDesc.has_value());
    ASSERT_TRUE(lqDesc.has_value());
    ASSERT_TRUE(lambdaDesc.has_value());
    EXPECT_NE(ldDesc->find(", H"), std::string::npos);
    EXPECT_NE(lqDesc->find(", H"), std::string::npos);
    EXPECT_NE(lambdaDesc->find(", Wb"), std::string::npos);
}
