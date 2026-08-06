#include "Mcp2221Protocol.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

using RTEAutomation::Mcp2221GpioAction;
using RTEAutomation::Mcp2221Protocol::Report;

struct FakeMcp2221 {
    std::vector<Report> requests;
    std::vector<std::chrono::milliseconds> delays;
    unsigned char rejectedCommand = 0;

    bool Exchange(const Report& request, Report& response, std::string&) {
        requests.push_back(request);
        response = {};
        response[0] = request[0];
        response[1] = request[0] == rejectedCommand ? 0x41 : 0;
        if (request[0] == 0x61) {
            response[6] = 0xb5;
            response[7] = 0x1c;
            response[24] = 0x12;
            response[25] = 0x03;
        }
        return true;
    }

    bool Execute(Mcp2221GpioAction action, std::string& error) {
        return RTEAutomation::Mcp2221Protocol::Execute(
            action,
            [this](const Report& request, Report& response, std::string& exchangeError) {
                return Exchange(request, response, exchangeError);
            },
            [this](std::chrono::milliseconds delay) { delays.push_back(delay); },
            error);
    }
};

void ExpectPinWrite(const Report& report, int bootAlter, int bootValue,
                    int resetAlter, int resetValue) {
    EXPECT_EQ(report[0], 0x50);
    EXPECT_EQ(report[2], bootAlter);
    EXPECT_EQ(report[3], bootValue);
    EXPECT_EQ(report[6], resetAlter);
    EXPECT_EQ(report[7], resetValue);
}

TEST(Mcp2221Gpio, EnterBootloaderDrivesBoot0AndPulsesReset) {
    FakeMcp2221 device;
    std::string error;

    ASSERT_TRUE(device.Execute(Mcp2221GpioAction::EnterBootloader, error)) << error;
    ASSERT_EQ(device.requests.size(), 5u);
    EXPECT_EQ(device.requests[0][0], 0x61);

    const Report& configure = device.requests[1];
    EXPECT_EQ(configure[0], 0x60);
    EXPECT_EQ(configure[3], 0x85);
    EXPECT_EQ(configure[4], 0x95);
    EXPECT_EQ(configure[5], 0x87);
    EXPECT_EQ(configure[7], 0x80);
    EXPECT_EQ(configure[8], 0x00);
    EXPECT_EQ(configure[9], 0x10);
    EXPECT_EQ(configure[10], 0x12);
    EXPECT_EQ(configure[11], 0x03);

    ExpectPinWrite(device.requests[2], 1, 1, 1, 1);
    ExpectPinWrite(device.requests[3], 1, 1, 1, 0);
    ExpectPinWrite(device.requests[4], 1, 1, 1, 1);
    EXPECT_EQ(device.delays,
              (std::vector<std::chrono::milliseconds>{
                  std::chrono::milliseconds(10), std::chrono::milliseconds(50),
                  std::chrono::milliseconds(50), std::chrono::milliseconds(250)}));
}

TEST(Mcp2221Gpio, StartApplicationLowersBoot0AndPulsesReset) {
    FakeMcp2221 device;
    std::string error;

    ASSERT_TRUE(device.Execute(Mcp2221GpioAction::StartApplication, error)) << error;
    ASSERT_EQ(device.requests.size(), 4u);
    EXPECT_EQ(device.requests[1][8], 0x00);
    EXPECT_EQ(device.requests[1][9], 0x10);
    ExpectPinWrite(device.requests[2], 1, 0, 1, 0);
    ExpectPinWrite(device.requests[3], 0, 0, 1, 1);
    EXPECT_EQ(device.delays,
              (std::vector<std::chrono::milliseconds>{
                  std::chrono::milliseconds(10), std::chrono::milliseconds(50),
                  std::chrono::milliseconds(100)}));
}

TEST(Mcp2221Gpio, ReleasePinsMakesBootAndResetHighImpedance) {
    FakeMcp2221 device;
    std::string error;

    ASSERT_TRUE(device.Execute(Mcp2221GpioAction::ReleasePins, error)) << error;
    ASSERT_EQ(device.requests.size(), 2u);
    EXPECT_EQ(device.requests[1][0], 0x60);
    EXPECT_EQ(device.requests[1][8], 0x08);
    EXPECT_EQ(device.requests[1][9], 0x08);
    EXPECT_TRUE(device.delays.empty());
}

TEST(Mcp2221Gpio, StopsWhenDeviceRejectsACommand) {
    FakeMcp2221 device;
    device.rejectedCommand = 0x60;
    std::string error;

    EXPECT_FALSE(device.Execute(Mcp2221GpioAction::EnterBootloader, error));
    EXPECT_EQ(device.requests.size(), 2u);
    EXPECT_NE(error.find("0x60"), std::string::npos);
}

}  // namespace
