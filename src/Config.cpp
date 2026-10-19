#include "../include/Config.hpp"
#include "../include/DeviceNodes.hpp"

#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace {

using std::chrono::milliseconds;

// Узел YAML вместе с путём до него — для понятных сообщений: ports[1].devices[0].address: ...
struct Node {
    YAML::Node node;
    std::string path;

    Node child(const std::string& key) const { return {node[key], path.empty() ? key : path + "." + key}; }
    Node item(size_t i) const { return {node[i], path + "[" + std::to_string(i) + "]"}; }
    bool has(const std::string& key) const { return node.IsMap() && node[key].IsDefined() && !node[key].IsNull(); }

    [[noreturn]] void fail(const std::string& message) const { throw ConfigError(path + ": " + message); }

    void expect_map(const std::set<std::string>& allowed) const {
        if (!node.IsMap()) fail("a mapping (key: value) is required");
        for (const auto& kv : node) {
            const auto key = kv.first.as<std::string>();
            if (!allowed.count(key)) {
                std::string list;
                for (const auto& a : allowed) list += (list.empty() ? "" : ", ") + a;
                child(key).fail("unknown key; allowed here: " + list);
            }
        }
    }

    template <typename T>
    T as() const {
        try {
            return node.as<T>();
        } catch (const YAML::Exception&) {
            fail("wrong value type");
        }
    }
};

template <typename T>
T get(const Node& parent, const std::string& key, T fallback) {
    return parent.has(key) ? parent.child(key).as<T>() : fallback;
}

std::string required_string(const Node& parent, const std::string& key) {
    if (!parent.has(key)) parent.child(key).fail("required");
    auto value = parent.child(key).as<std::string>();
    if (value.empty()) parent.child(key).fail("must not be empty");
    return value;
}

long long ranged(const Node& parent, const std::string& key, long long fallback, long long lo, long long hi) {
    if (!parent.has(key)) return fallback;
    auto value = parent.child(key).as<long long>();
    if (value < lo || value > hi) {
        parent.child(key).fail("must be " + std::to_string(lo) + ".." + std::to_string(hi));
    }
    return value;
}

milliseconds ms(const Node& parent, const std::string& key, milliseconds fallback, long long lo, long long hi) {
    return milliseconds(ranged(parent, key, fallback.count(), lo, hi));
}

const std::set<std::string> kSerialKeys = {"baud_rate", "parity", "stop_bits", "response_timeout_ms",
                                           "reopen_delay_ms", "max_queue"};
const std::set<std::string> kPollKeys = {"status_poll_ms", "offline_poll_ms", "offline_after", "status_refresh_ms"};
// только у порта и в serial_defaults, не у платы: это свойства порта целиком
const std::set<std::string> kPortKeys = {"status_flush_ms", "poll_schedule"};

struct PortTiming {
    PollSchedule schedule{PollSchedule::Fixed};
    milliseconds flush{0};
};

void read_port_timing(const Node& n, PortTiming& t) {
    if (n.has("poll_schedule")) {
        const auto v = n.child("poll_schedule").as<std::string>();
        if (v == "fixed") t.schedule = PollSchedule::Fixed;
        else if (v == "after_reply") t.schedule = PollSchedule::AfterReply;
        else n.child("poll_schedule").fail("fixed or after_reply");
    }
    t.flush = ms(n, "status_flush_ms", t.flush, 0, 10000);
}

void read_serial(const Node& n, PortSettings& s) {
    s.baud_rate = static_cast<unsigned>(ranged(n, "baud_rate", s.baud_rate, 300, 4000000));
    if (n.has("parity")) {
        const auto p = n.child("parity").as<std::string>();
        using P = asio::serial_port_base::parity;
        if (p == "none") s.parity = P::none;
        else if (p == "even") s.parity = P::even;
        else if (p == "odd") s.parity = P::odd;
        else n.child("parity").fail("none, even or odd");
    }
    if (n.has("stop_bits")) {
        using S = asio::serial_port_base::stop_bits;
        const auto bits = n.child("stop_bits").as<int>();
        if (bits == 1) s.stop_bits = S::one;
        else if (bits == 2) s.stop_bits = S::two;
        else n.child("stop_bits").fail("1 or 2");
    }
    s.response_timeout = ms(n, "response_timeout_ms", s.response_timeout, 5, 5000);
    s.reopen_delay = ms(n, "reopen_delay_ms", s.reopen_delay, 100, 600000);
    s.max_queue = static_cast<size_t>(ranged(n, "max_queue", static_cast<long long>(s.max_queue), 1, 10000));
}

void read_poll(const Node& n, PollSettings& p) {
    p.status_poll = ms(n, "status_poll_ms", p.status_poll, 10, 600000);
    p.offline_poll = ms(n, "offline_poll_ms", p.offline_poll, 10, 600000);
    p.offline_after = static_cast<int>(ranged(n, "offline_after", p.offline_after, 1, 1000));
    p.status_refresh = ms(n, "status_refresh_ms", p.status_refresh, 100, 3600000);
}

void read_opc(const Node& n, BridgeConfig& cfg) {
    n.expect_map({"endpoint", "security_mode", "username", "password_env", "timeout_ms", "publishing_interval_ms",
                  "sampling_interval_ms", "lifetime_count", "keep_alive_count", "keep_alive_node",
                  "keep_alive_period_ms", "device_node_pattern", "max_nodes_per_request"});
    auto& o = cfg.opc;
    o.endpoint = required_string(n, "endpoint");
    if (o.endpoint.rfind("opc.tcp://", 0) != 0) n.child("endpoint").fail("must start with opc.tcp://");

    const auto mode = get<std::string>(n, "security_mode", "none");
    if (mode == "none") o.security_mode = opcua::MessageSecurityMode::None;
    else if (mode == "sign") o.security_mode = opcua::MessageSecurityMode::Sign;
    else if (mode == "sign_encrypt") o.security_mode = opcua::MessageSecurityMode::SignAndEncrypt;
    else n.child("security_mode").fail("none, sign or sign_encrypt");

    o.username = get<std::string>(n, "username", "");
    if (n.has("password_env")) {               // пароль — только из окружения, не из файла
        const auto var = n.child("password_env").as<std::string>();
        const char* value = std::getenv(var.c_str());
        if (value == nullptr) n.child("password_env").fail("environment variable " + var + " is not set");
        o.password = value;
    }
    o.request_timeout_ms = static_cast<uint32_t>(ranged(n, "timeout_ms", o.request_timeout_ms, 100, 60000));
    o.publishing_interval_ms = static_cast<double>(ranged(n, "publishing_interval_ms", 50, 1, 60000));
    o.sampling_interval_ms = static_cast<double>(ranged(n, "sampling_interval_ms", -1, -1, 60000));
    o.lifetime_count = static_cast<uint32_t>(ranged(n, "lifetime_count", o.lifetime_count, 3, 100000));
    o.max_keep_alive_count = static_cast<uint32_t>(ranged(n, "keep_alive_count", o.max_keep_alive_count, 1, 10000));
    if (o.lifetime_count < 3 * o.max_keep_alive_count) {
        n.child("lifetime_count").fail("must be at least 3 x keep_alive_count (OPC UA rule)");
    }

    cfg.max_nodes_per_request = static_cast<size_t>(
        ranged(n, "max_nodes_per_request", static_cast<long long>(cfg.max_nodes_per_request), StatusNodeCount, 100000));

    cfg.keep_alive_node = get<std::string>(n, "keep_alive_node", "");
    cfg.keep_alive_period = ms(n, "keep_alive_period_ms", cfg.keep_alive_period, 100, 600000);
    if (!cfg.keep_alive_node.empty()) {
        try {
            (void)opcua::NodeId::parse(cfg.keep_alive_node);
        } catch (const std::exception& e) {
            n.child("keep_alive_node").fail(std::string("not a valid NodeId: ") + e.what());
        }
    }

    auto& pattern = cfg.device_node_pattern;
    pattern = required_string(n, "device_node_pattern");
    if (pattern.find("{port}") == std::string::npos || pattern.find("{device}") == std::string::npos) {
        n.child("device_node_pattern").fail("must contain {port} and {device}");
    }
}

BridgeConfig read_config(const YAML::Node& root_node) {
    const Node root{root_node, ""};
    if (!root.node.IsMap()) throw ConfigError("config: a mapping with opc_ua, serial_defaults and ports is required");
    root.expect_map({"opc_ua", "serial_defaults", "ports"});

    BridgeConfig cfg;
    if (!root.has("opc_ua")) throw ConfigError("opc_ua: required");
    read_opc(root.child("opc_ua"), cfg);

    PortSettings serial_defaults;
    PollSettings poll_defaults;
    PortTiming timing_defaults;
    if (root.has("serial_defaults")) {
        const auto d = root.child("serial_defaults");
        std::set<std::string> keys = kSerialKeys;
        keys.insert(kPollKeys.begin(), kPollKeys.end());
        keys.insert(kPortKeys.begin(), kPortKeys.end());
        d.expect_map(keys);
        read_serial(d, serial_defaults);
        read_poll(d, poll_defaults);
        read_port_timing(d, timing_defaults);
    }

    const auto ports = root.child("ports");
    if (!ports.node.IsSequence() || ports.node.size() == 0) ports.fail("a non-empty list is required");
    std::set<uint16_t> port_indices;
    std::set<std::string> port_paths;
    for (size_t i = 0; i < ports.node.size(); ++i) {
        const auto pn = ports.item(i);
        std::set<std::string> keys = kSerialKeys;
        keys.insert(kPollKeys.begin(), kPollKeys.end());
        keys.insert(kPortKeys.begin(), kPortKeys.end());
        keys.insert({"index", "path", "devices"});
        pn.expect_map(keys);

        PortConfig port;
        port.index = static_cast<uint16_t>(ranged(pn, "index", static_cast<long long>(i + 1), 1, 65535));
        if (!port_indices.insert(port.index).second) pn.child("index").fail("duplicate port index");
        port.serial = serial_defaults;
        port.serial.path = required_string(pn, "path");
        if (!port_paths.insert(port.serial.path).second) pn.child("path").fail("the same port is listed twice");
        read_serial(pn, port.serial);
        PortTiming timing = timing_defaults;
        read_port_timing(pn, timing);
        port.poll_schedule = timing.schedule;
        port.status_flush = timing.flush;
        PollSettings port_poll = poll_defaults;
        read_poll(pn, port_poll);
        port.poll_cycle = port_poll.status_poll;

        const auto devices = pn.child("devices");
        if (!devices.node.IsSequence() || devices.node.size() == 0) devices.fail("a non-empty list is required");
        std::set<uint8_t> addresses;
        std::set<uint16_t> device_indices;
        for (size_t j = 0; j < devices.node.size(); ++j) {
            const auto dn = devices.item(j);
            std::set<std::string> dkeys = kPollKeys;
            dkeys.insert({"address", "index"});
            dn.expect_map(dkeys);
            DeviceConfig dev;
            if (!dn.has("address")) dn.child("address").fail("required");
            dev.address = static_cast<uint8_t>(ranged(dn, "address", 1, 1, 254));
            dev.index = static_cast<uint16_t>(ranged(dn, "index", dev.address, 1, 65535));
            if (!addresses.insert(dev.address).second) dn.child("address").fail("duplicate address on this port");
            if (!device_indices.insert(dev.index).second) dn.child("index").fail("duplicate index on this port");
            dev.poll = port_poll;
            read_poll(dn, dev.poll);
            try {
                (void)DeviceNodes::make(cfg.device_node_pattern, port.index, dev.index);   // все NodeId разбираются
            } catch (const std::exception& e) {
                dn.fail(std::string("cannot build PLC node names: ") + e.what());
            }
            port.devices.push_back(dev);
        }
        cfg.ports.push_back(std::move(port));
    }
    return cfg;
}

}  // namespace

BridgeConfig parse_config(const std::string& yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        throw ConfigError(std::string("invalid YAML: ") + e.what());
    }
    return read_config(root);
}

BridgeConfig load_config(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw ConfigError("cannot read " + path.string());
    std::stringstream text;
    text << file.rdbuf();
    try {
        return parse_config(text.str());
    } catch (const ConfigError& e) {
        throw ConfigError(path.string() + ": " + e.what());
    }
}
