
#ifndef OPCSERIALBRIDGE_SERIAL_CONFIG_HPP
#define OPCSERIALBRIDGE_SERIAL_CONFIG_HPP

#include <boost/asio/serial_port_base.hpp>


struct SerialSettings {
    unsigned int baud_rate = 115200;
    unsigned int character_size = 8;
    std::string parity = "none";
    std::string stop_bits = "one";
    std::string flow_control = "none";

    boost::asio::serial_port_base::parity get_asio_parity() const {
        if (parity == "none") return boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none);
        if (parity == "even") return boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::even);
        if (parity == "odd")  return boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::odd);
        throw std::invalid_argument("Unknown parity type: " + parity);
    }

    boost::asio::serial_port_base::stop_bits get_asio_stop_bits() const {
        if (stop_bits == "one")          return boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one);
        if (stop_bits == "onepointfive") return boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::onepointfive);
        if (stop_bits == "two")          return boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::two);
        throw std::invalid_argument("Unknown stop_bits type: " + stop_bits);
    }

    boost::asio::serial_port_base::flow_control get_asio_flow_control() const {
        if (flow_control == "none")     return boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::none);
        if (flow_control == "software") return boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::software);
        if (flow_control == "hardware") return boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::hardware);
        throw std::invalid_argument("Unknown flow_control type: " + flow_control);
    }
};

struct DeviceConfig {
    int address;
};


struct PortConfig{
    std::string port_path;
    unsigned int index;
    SerialSettings serial_settings;
    std::vector<DeviceConfig> devices;
};


#endif //OPCSERIALBRIDGE_SERIAL_CONFIG_HPP
