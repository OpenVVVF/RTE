// PlantCodegen D1 dual-plant link test: the same graph emitted twice with
// different namespaces/file stems (plant_golden_gen7 = v1 defaults;
// plant_golden_gen7_alt = Options{plant_gen7_bench_pmsm, plant_alt_generated})
// must compile and link into one binary without ODR collisions, and both
// engines must behave identically.
//
// The CMake namespace for the _alt library is the value
// PlantGenerator::SuggestNamespace(graph name) returns, so the helper and
// the build stay in agreement by construction.

#include <cmath>
#include <gtest/gtest.h>

#include "PlantCodegen/PlantGenerator.h"
#include "plant_generated.h"
#include "plant_alt_generated.h"

namespace {

TEST(PlantCodegenDualPlant, TwoPlantsCoexistInOneBinary) {
    // The _alt namespace must equal the helper's suggestion for the graph.
    EXPECT_EQ(PlantCodegen::PlantGenerator::SuggestNamespace("gen7_bench_pmsm"),
              "plant_gen7_bench_pmsm");

    plant::PlantState a{};
    plant_gen7_bench_pmsm::PlantState b{};
    plant::PlantInit(a);
    plant_gen7_bench_pmsm::PlantInit(b);

    plant::PlantMeas ma{};
    plant_gen7_bench_pmsm::PlantMeas mb{};
    plant::PlantCollectMeas(a, ma);
    plant_gen7_bench_pmsm::PlantCollectMeas(b, mb);
    EXPECT_FLOAT_EQ(ma.Enc_Sin, mb.Enc_Sin);
    EXPECT_FLOAT_EQ(ma.Temp_Inv1_C, mb.Temp_Inv1_C);
    EXPECT_FLOAT_EQ(ma.P_Heat_W, mb.P_Heat_W);

    // Drive both engines identically for a few periods: identical outputs.
    plant::PlantSetDynoRpm(a, 1200.0f);
    plant_gen7_bench_pmsm::PlantSetDynoRpm(b, 1200.0f);
    plant::PlantSetDuty(a, 52.0f, 50.5f, 49.0f);
    plant_gen7_bench_pmsm::PlantSetDuty(b, 52.0f, 50.5f, 49.0f);
    for (int i = 0; i < 20; ++i) {
        plant::PlantStep(a, 200e-6);
        plant_gen7_bench_pmsm::PlantStep(b, 200e-6);
    }
    EXPECT_FLOAT_EQ(a.DcBus.Vdc, b.DcBus.Vdc);
    EXPECT_FLOAT_EQ(a.Machine.Id, b.Machine.Id);
    EXPECT_FLOAT_EQ(a.Machine.Iq, b.Machine.Iq);

    // The D8/D10 surfaces exist in both namespaces (independent code paths).
    double polesA = 0, polesB = 0, ignore = 0;
    plant::PlantBootParams(a, &polesA, &ignore, &ignore);
    plant_gen7_bench_pmsm::PlantBootParams(b, &polesB, &ignore, &ignore);
    EXPECT_DOUBLE_EQ(polesA, 5.0);
    EXPECT_DOUBLE_EQ(polesA, polesB);
    EXPECT_TRUE(plant::PlantSetFault(a, "OpenGates", true));
    EXPECT_TRUE(plant_gen7_bench_pmsm::PlantSetFault(b, "OpenGates", true));
    EXPECT_FLOAT_EQ(a.Fault.OpenGates, 1.0f);
    EXPECT_FLOAT_EQ(b.Fault.OpenGates, 1.0f);
    EXPECT_FALSE(plant_gen7_bench_pmsm::PlantSetFault(b, "Bogus", true));
}

}  // namespace
