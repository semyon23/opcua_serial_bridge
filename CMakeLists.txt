cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0167 NEW)

project(OpcSerialBridge)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(open62541pp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_executable(${PROJECT_NAME}
        src/app.cpp
        src/SerialTransport.cpp
        src/OpcTransport.cpp
        src/OpcSerialBridge.cpp
        src/utils/DataParser.cpp
        src/DeviceContext.cpp
        include/DeviceContext.hpp
        include/serial_config.hpp
)

target_link_libraries(${PROJECT_NAME}
        PRIVATE
        yaml-cpp::yaml-cpp
        Boost::system
        Threads::Threads
        open62541pp::open62541pp
        spdlog::spdlog
)
