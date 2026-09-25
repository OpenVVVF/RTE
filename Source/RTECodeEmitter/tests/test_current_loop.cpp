#include "../../../Images/Gen7FW/MainProcessor/Inc/Inverter/Control/CurrentLoopMath.h"
#include <gtest/gtest.h>
#include <limits>
#include "../../../Images/Gen7FW/MainProcessor/Inc/Inverter/Control/ControlSpeedEstimator.h"
using namespace Inverter::CurrentLoopMath;
TEST(Gen7ControlSpeed, TracksAccelerationAcrossAngleAndTimestampWrap) {
    for (float direction : {-1.0f, 1.0f}) {
        Inverter::ControlSpeedEstimator estimator;
        constexpr uint32_t clock = 550000000U, ticks = 27500U;
        uint32_t timestamp = 0xffff0000U;
        double angle = 0;
        float measured = 0;
        for (int i=0; i<100000; ++i) {
            const float rpm = direction * 400.0f * float(i) * .00005f;
            angle += double(rpm) * 6.0 * .00005;
            double wrapped = std::fmod(angle,360.0);
            if (wrapped < 0) wrapped += 360;
            measured = estimator.update(float(wrapped), timestamp, clock);
            timestamp += ticks;
            if (i>10000) { EXPECT_NEAR(measured,rpm,6.0f); }
        }
        EXPECT_NEAR(measured,direction*2000.0f,6.0f);
    }
}
TEST(Gen7ControlSpeed, RejectsStationarySampleNoiseWithoutMainLoopUpdates) {
    Inverter::ControlSpeedEstimator estimator;
    float largest = 0;
    for(uint32_t i=0;i<20000;++i) {
        const float angle = 120.0f + .05f*std::sin(float(i)*1.2345f);
        const float rpm = estimator.update(angle,i*27500U,550000000U);
        if(i>1000) largest=std::max(largest,std::abs(rpm));
    }
    EXPECT_LT(largest,2.0f);
}
TEST(Gen7ControlSpeed, ResetsAfterSampleGapAndInvalidData) {
    Inverter::ControlSpeedEstimator estimator;
    for(uint32_t i=0;i<2000;++i) estimator.update(std::fmod(float(i)*.3f,360.0f), i*27500U,550000000U);
    EXPECT_NEAR(estimator.rpm(),1000.0f,.1f);
    EXPECT_FLOAT_EQ(estimator.update(120,66000000U,550000000U),0);
    EXPECT_FLOAT_EQ(estimator.update(std::numeric_limits<float>::quiet_NaN(),66027500U,550000000U),0);
    EXPECT_FLOAT_EQ(estimator.update(120,66055000U,0),0);
}
TEST(Gen7ControlSpeed, UsesMeasuredTimeAtDifferentAndIrregularSampleRates) {
    for(uint32_t base : {27500U,55000U,110000U}) {
        Inverter::ControlSpeedEstimator estimator;
        uint32_t cycles=0;
        double angle=0;
        for(uint32_t i=0;i<10000;++i) {
            const uint32_t ticks=base+(i%5)*100U;
            cycles+=ticks;
            angle+=1234.0*6.0*double(ticks)/550000000.0;
            estimator.update(float(std::fmod(angle,360.0)),cycles,550000000U);
        }
        EXPECT_NEAR(estimator.rpm(),1234.0f,.1f);
    }
}
TEST(Gen7CurrentLoop, OppositeMidpointPairRejectsGapsAndCancelsAlternatingError) {
    auto v=midpointPair({12,37},{-12,33},true,true,.0002f,.0004f);
    EXPECT_FLOAT_EQ(v.d,0); EXPECT_FLOAT_EQ(v.q,35);
    for (auto args : {0,1,2}) {
        v=midpointPair({12,37},{-12,33},args!=0,args!=1,args==2?.001f:.0002f,.0004f);
        EXPECT_FLOAT_EQ(v.d,12); EXPECT_FLOAT_EQ(v.q,37);
    }
}
TEST(Gen7CurrentLoop, PhysicalVoltageReconstructsAcrossAnglesAndBuses) {
    for(float bus : {50.0f,75.0f,100.0f}) for(int i=0;i<360;i+=3) {
        const float theta=float(i)*0.01745329252f;
        const Vector ab{0.5f*bus*std::cos(theta),0.5f*bus*std::sin(theta)};
        const auto d=modulate(ab,bus);
        const float mean=(d.a+d.b+d.c)/3.0f;
        EXPECT_NEAR((d.a-mean)*bus/100.0f,ab.d,0.0001f);
        EXPECT_NEAR((d.b-d.c)*bus/100.0f*0.57735026919f,ab.q,0.0001f);
        EXPECT_GE(d.a,0);EXPECT_LE(d.a,100);
    }
}
TEST(Gen7CurrentLoop, SymmetricCyclePreservesCenterAndCancelsAlternatingRipple) {
    const auto v=midpointCycle({14,39},{-8,34},{10,37},true);
    EXPECT_FLOAT_EQ(v.d,2); EXPECT_FLOAT_EQ(v.q,36);
    EXPECT_FLOAT_EQ(midpointCycle({14,39},{-8,34},{10,37},false).d,14);
}
TEST(Gen7CurrentLoop, CompatibilityPreservesExistingSmallSignalGain) {
    Controller c;
    const auto r=c.step({3,-4},{2,5},{.04f,.04f},{5,5},.0002f,25,true);
    EXPECT_NEAR(r.limited.d,.5f*(.04f*3+5*3*.0002f+2),1e-6f);
    EXPECT_NEAR(r.limited.q,.5f*(.04f*-4+5*-4*.0002f+5),1e-6f);
}
TEST(Gen7CurrentLoop, CombinedLimitAppliesBelowEachScalarLimit) {
    Controller c;
    const auto r=c.step({0,0},{40,40},{.04f,.04f},{5,5},.0002f,25,true);
    EXPECT_LT(r.scale,1);
    EXPECT_NEAR(std::hypot(r.limited.d,r.limited.q),25,1e-5f);
    EXPECT_LT(r.integral.d,0);EXPECT_LT(r.integral.q,0);
}
TEST(Gen7CurrentLoop, ProlongedSaturationBoundedAndRecovers) {
    Controller c; Result r;
    for(int i=0;i<50000;++i)r=c.step({100,100},{10,10},{.04f,.04f},{5,5},.0002f,25,true);
    EXPECT_LT(std::abs(r.integral.d),40);EXPECT_LT(std::abs(r.integral.q),40);
    for(int i=0;i<5000;++i)r=c.step({-5,-5},{0,0},{.04f,.04f},{5,5},.0002f,25,true);
    EXPECT_GT(r.scale,.999f);
    EXPECT_LT(std::hypot(r.limited.d,r.limited.q),20);
}
TEST(Gen7CurrentLoop, DirectionSymmetry) {
    Controller p,n;
    for(int i=0;i<2000;++i){
        auto a=p.step({12,35},{-8,40},{.04f,.04f},{5,5},.0002f,25,true);
        auto b=n.step({-12,-35},{8,-40},{.04f,.04f},{5,5},.0002f,25,true);
        EXPECT_FLOAT_EQ(a.limited.d,-b.limited.d);EXPECT_FLOAT_EQ(a.limited.q,-b.limited.q);
    }
}
TEST(Gen7CurrentLoop, DisableAndInvalidInputsReset) {
    Controller c;
    c.step({100,100},{0,0},{.04f,.04f},{5,5},.0002f,25,true);
    auto r=c.step({0,0},{0,0},{.04f,.04f},{5,5},.0002f,25,false);
    EXPECT_FLOAT_EQ(c.integral.d,0);EXPECT_FLOAT_EQ(r.limited.q,0);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    auto d=modulate({1,2},nan);EXPECT_FLOAT_EQ(d.a,50);
    r=c.step({nan,0},{0,0},{.04f,.04f},{5,5},.0002f,25,true);
    EXPECT_FLOAT_EQ(r.limited.d,0);
}
TEST(Gen7CurrentLoop, WindowMarginAtLinearCeiling) {
    const float fraction=.94f*.57735026919f;
    for(int i=0;i<360;++i){float t=float(i)*.01745329252f;
        auto d=modulate({75*fraction*std::cos(t),75*fraction*std::sin(t)},75);
        EXPECT_GE(std::min({d.a,d.b,d.c}),2.9999f);
        EXPECT_LE(std::max({d.a,d.b,d.c}),97.0001f);
    }
}

#include <filesystem>
#include <fstream>
#include <sstream>
TEST(Gen7CurrentLoop, TelemetryInitializedBeforeFramLoaders) {
    auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    std::ifstream file(root/"Images/Gen7FW/MainProcessor/Src/Inverter/InverterMain.cpp");
    ASSERT_TRUE(file.good());std::ostringstream contents;contents<<file.rdbuf();
    const auto source=contents.str();
    const auto init=source.find("Telemetry::init();");
    const auto fram=source.find("if (CY15B102Q_Init");
    ASSERT_NE(init,std::string::npos);ASSERT_NE(fram,std::string::npos);
    EXPECT_LT(init,fram);
}
