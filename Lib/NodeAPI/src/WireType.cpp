#include "NodeAPI/WireType.h"

#include <stdexcept>

namespace NodeAPI {

std::string_view GetUnitLabel(Quantity quantity) {
    switch (quantity) {
        case Quantity::Voltage: return "V";
        case Quantity::Current: return "A";
        case Quantity::AngularVelocity: return "rad/s";
        case Quantity::Torque: return "N·m";
        case Quantity::Temperature: return "degC";
        case Quantity::Dimensionless: return "";
        case Quantity::Boolean: return "";
        case Quantity::String: return "";
    }
    return "";
}

std::string ToString(Quantity quantity) {
    switch (quantity) {
        case Quantity::Voltage: return "voltage";
        case Quantity::Current: return "current";
        case Quantity::AngularVelocity: return "angular_velocity";
        case Quantity::Torque: return "torque";
        case Quantity::Temperature: return "temperature";
        case Quantity::Dimensionless: return "dimensionless";
        case Quantity::Boolean: return "boolean";
        case Quantity::String: return "string";
    }
    return "";
}

std::string ToString(Frame frame) {
    switch (frame) {
        case Frame::Scalar: return "scalar";
        case Frame::Abc: return "abc";
        case Frame::AlphaBeta: return "alpha_beta";
        case Frame::Dq: return "dq";
    }
    return "";
}

std::string ToString(DType dtype) {
    switch (dtype) {
        case DType::F32: return "f32";
    }
    return "";
}

Quantity QuantityFromString(std::string_view value) {
    if (value == "voltage") return Quantity::Voltage;
    if (value == "current") return Quantity::Current;
    if (value == "angular_velocity") return Quantity::AngularVelocity;
    if (value == "torque") return Quantity::Torque;
    if (value == "temperature") return Quantity::Temperature;
    if (value == "dimensionless") return Quantity::Dimensionless;
    if (value == "boolean") return Quantity::Boolean;
    if (value == "string") return Quantity::String;
    throw std::invalid_argument("unknown quantity");
}

Frame FrameFromString(std::string_view value) {
    if (value == "scalar") return Frame::Scalar;
    if (value == "abc") return Frame::Abc;
    if (value == "alpha_beta") return Frame::AlphaBeta;
    if (value == "dq") return Frame::Dq;
    throw std::invalid_argument("unknown frame");
}

DType DTypeFromString(std::string_view value) {
    if (value == "f32") return DType::F32;
    throw std::invalid_argument("unknown dtype");
}

}  // namespace NodeAPI
