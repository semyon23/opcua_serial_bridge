#include "../include/DeviceNodes.hpp"

#include <cmath>
#include <limits>
#include <optional>

namespace {

std::string replace_all(std::string text, const std::string& what, const std::string& with) {
    for (size_t pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + with.size())) {
        text.replace(pos, what.size(), with);
    }
    return text;
}

opcua::NodeId node(const std::string& base, const char* suffix) {
    return opcua::NodeId::parse(base + suffix);
}

// Число из Variant любого числового типа ПЛК: USINT, INT, DINT, REAL, LREAL, BOOL…
std::optional<double> number(const opcua::Variant& v) {
    if (!v.isScalar()) return std::nullopt;
    if (v.isType<bool>()) return v.to<bool>() ? 1.0 : 0.0;
    if (v.isType<uint8_t>()) return v.to<uint8_t>();
    if (v.isType<int8_t>()) return v.to<int8_t>();
    if (v.isType<uint16_t>()) return v.to<uint16_t>();
    if (v.isType<int16_t>()) return v.to<int16_t>();
    if (v.isType<uint32_t>()) return v.to<uint32_t>();
    if (v.isType<int32_t>()) return v.to<int32_t>();
    if (v.isType<float>()) return v.to<float>();
    if (v.isType<double>()) return v.to<double>();
    return std::nullopt;
}

template <typename T>
std::optional<std::vector<double>> numbers_as(const opcua::Variant& v) {
    if (!v.isType<T>()) return std::nullopt;
    std::vector<double> out;
    for (const auto& x : v.to<std::vector<T>>()) out.push_back(static_cast<double>(x));
    return out;
}

std::optional<std::vector<double>> numbers(const opcua::Variant& v) {
    if (!v.isArray()) return std::nullopt;
    for (auto r : {numbers_as<float>(v), numbers_as<double>(v), numbers_as<int8_t>(v), numbers_as<uint8_t>(v),
                   numbers_as<int16_t>(v), numbers_as<uint16_t>(v), numbers_as<int32_t>(v)}) {
        if (r) return r;
    }
    return std::nullopt;
}

bool fits_float(double x) {
    return std::isfinite(x) && std::fabs(x) <= std::numeric_limits<float>::max();
}

}  // namespace

DeviceNodes DeviceNodes::make(const std::string& pattern, uint16_t port_index, uint16_t device_index) {
    DeviceNodes n;
    n.base = replace_all(replace_all(pattern, "{port}", std::to_string(port_index)), "{device}",
                         std::to_string(device_index));
    n.cmd_ready = node(n.base, ".cmdReady");
    n.info_ready = node(n.base, ".infoReady");
    n.online = node(n.base, ".online");

    n.control = {
        node(n.base, ".control.command"),
        node(n.base, ".control.transaction_id"),
        node(n.base, ".control.powerOn"),
        node(n.base, ".control.axisId"),
        node(n.base, ".control.position"),
        node(n.base, ".control.velocity"),
        node(n.base, ".control.acceleration"),
        node(n.base, ".control.direction"),
    };

    n.status = {
        node(n.base, ".info.transaction_id"),
        node(n.base, ".info.state"),
        node(n.base, ".info.isHome"),
        node(n.base, ".info.isTarget"),
        node(n.base, ".info.isPowered"),
        node(n.base, ".info.inMotion"),
        node(n.base, ".info.errorID"),
        node(n.base, ".info.position"),
    };
    return n;
}

std::variant<PlcCommand, std::string> decode_command(std::span<const opcua::Variant> values) {
    if (values.size() != ControlNodeCount) return "expected " + std::to_string(ControlNodeCount) + " control values";

    auto integer = [&](ControlNode i, const char* name, int lo, int hi) -> std::variant<int, std::string> {
        auto x = number(values[i]);
        if (!x || *x != std::floor(*x) || *x < lo || *x > hi) {
            return std::string("control.") + name + ": an integer " + std::to_string(lo) + ".." + std::to_string(hi) +
                   " is required";
        }
        return static_cast<int>(*x);
    };
    auto real = [&](ControlNode i, const char* name) -> std::variant<float, std::string> {
        auto x = number(values[i]);
        if (!x || !fits_float(*x)) return std::string("control.") + name + ": a finite number is required";
        return static_cast<float>(*x);
    };

    PlcCommand out;
    auto cmd = integer(CtrlCommand, "command", 0, 255);
    if (auto* e = std::get_if<std::string>(&cmd)) return *e;
    out.cmd = static_cast<uint8_t>(std::get<int>(cmd));
    switch (out.cmd) {
        case proto::POWER_ENABLE: case proto::MOVE_RELATIVE: case proto::MOVE_ABSOLUTE: case proto::MOVE_CONTINUE:
        case proto::MOVE_HOME: case proto::HALT: case proto::EMERGENCY_STOP: case proto::SET_POSITION:
        case proto::RESET_ERROR:
            break;
        default:
            return "command " + std::to_string(out.cmd) + " is not allowed from the PLC";
    }

    auto tid = integer(CtrlTransactionId, "transaction_id", 0, 255);
    if (auto* e = std::get_if<std::string>(&tid)) return *e;
    auto power = integer(CtrlPowerOn, "powerOn", 0, 1);
    if (auto* e = std::get_if<std::string>(&power)) return *e;
    auto axis = integer(CtrlAxisId, "axisId", 0, proto::AXIS_GROUP);
    if (auto* e = std::get_if<std::string>(&axis)) return *e;
    out.data.transaction_id = static_cast<uint8_t>(std::get<int>(tid));
    out.data.power_enable = static_cast<uint8_t>(std::get<int>(power));
    out.data.axis_id = static_cast<uint8_t>(std::get<int>(axis));

    auto position = numbers(values[CtrlPosition]);
    if (!position || position->size() < proto::AXIS_COUNT) return "control.position: an array of 3 numbers is required";
    for (size_t i = 0; i < proto::AXIS_COUNT; ++i) {
        if (!fits_float((*position)[i])) return "control.position: finite numbers are required";
        out.data.position[i] = static_cast<float>((*position)[i]);
    }

    auto velocity = real(CtrlVelocity, "velocity");
    if (auto* e = std::get_if<std::string>(&velocity)) return *e;
    auto acceleration = real(CtrlAcceleration, "acceleration");
    if (auto* e = std::get_if<std::string>(&acceleration)) return *e;
    out.data.velocity = std::get<float>(velocity);
    out.data.acceleration = std::get<float>(acceleration);
    out.data.deceleration = out.data.acceleration;        // узла торможения в ПЛК нет

    auto direction = numbers(values[CtrlDirection]);
    if (!direction || direction->size() < proto::AXIS_COUNT) return "control.direction: an array of 3 numbers is required";
    for (size_t i = 0; i < proto::AXIS_COUNT; ++i) {
        const double d = (*direction)[i];
        if (d != -1 && d != 0 && d != 1) return "control.direction: values -1, 0 or 1 are required";
        out.data.direction[i] = static_cast<int8_t>(d);
    }
    return out;
}

std::vector<opcua::Variant> encode_status(const proto::StatusData& s) {
    auto bytes = [](const std::array<uint8_t, proto::AXIS_COUNT>& a) {
        return opcua::Variant(std::vector<uint8_t>(a.begin(), a.end()));
    };
    std::vector<opcua::Variant> values(StatusNodeCount);
    values[StTransactionId] = opcua::Variant(s.transaction_id);
    values[StState] = bytes(s.state);
    values[StIsHome] = bytes(s.is_home);
    values[StIsTarget] = bytes(s.is_target);
    values[StIsPowered] = bytes(s.is_powered);
    values[StInMotion] = bytes(s.in_motion);
    values[StErrorId] = bytes(s.error_id);
    values[StPosition] = opcua::Variant(std::vector<float>(s.position.begin(), s.position.end()));
    return values;
}
