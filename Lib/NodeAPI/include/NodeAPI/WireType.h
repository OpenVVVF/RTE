#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace NodeAPI {

enum class Quantity {
    Voltage,
    Current,
    AngularVelocity,
    Torque,
    Temperature,
    Dimensionless,
    Boolean,
    String,
};

enum class Frame {
    Scalar,
    Abc,
    AlphaBeta,
    Dq,
};

enum class DType {
    F32,
};

struct WireType {
    Quantity quantity = Quantity::Dimensionless;
    Frame frame = Frame::Scalar;
    DType dtype = DType::F32;

    friend bool operator==(const WireType& lhs, const WireType& rhs) = default;
    friend bool operator!=(const WireType& lhs, const WireType& rhs) = default;
};

std::string_view GetUnitLabel(Quantity quantity);
std::string ToString(Quantity quantity);
std::string ToString(Frame frame);
std::string ToString(DType dtype);

Quantity QuantityFromString(std::string_view value);
Frame FrameFromString(std::string_view value);
DType DTypeFromString(std::string_view value);

}  // namespace NodeAPI
