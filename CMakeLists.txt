cmake_minimum_required(VERSION 3.20)
project(OpcSerialBridge CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(Boost REQUIRED)
find_package(open62541pp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_library(bridge_core STATIC
        src/protocol.cpp
        src/PortWorker.cpp
        src/OpcTransport.cpp
        src/Config.cpp
        src/DeviceNodes.cpp
        src/DeviceChannel.cpp
        src/Heartbeat.cpp
        src/App.cpp
)
target_include_directories(bridge_core PUBLIC include)
target_compile_options(bridge_core PRIVATE -Wall -Wextra)
target_link_libraries(bridge_core PUBLIC
        open62541pp::open62541pp spdlog::spdlog yaml-cpp::yaml-cpp Boost::headers Threads::Threads)

add_executable(opc_serial_bridge src/main.cpp)
target_compile_options(opc_serial_bridge PRIVATE -Wall -Wextra)
target_link_libraries(opc_serial_bridge PRIVATE bridge_core)


#cmake_minimum_required(VERSION 3.15)
#cmake_policy(SET CMP0167 NEW)
#
#project(OpcSerialBridge)
#
#set(CMAKE_CXX_STANDARD 23)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
##set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
#find_package(Boost REQUIRED COMPONENTS system)
#find_package(Threads REQUIRED)
#find_package(open62541pp CONFIG REQUIRED)
#find_package(spdlog CONFIG REQUIRED)
#find_package(yaml-cpp CONFIG REQUIRED)
#
#add_executable(${PROJECT_NAME}
#        src/protocol.cpp
#        src/PortWorker.cpp
#        src/OpcTransport.cpp
#        src/Config.cpp
#        src/DeviceNodes.cpp
#        src/DeviceChannel.cpp
#        src/Heartbeat.cpp
#        src/App.cpp
#)
#
#target_link_libraries(${PROJECT_NAME}
#        PRIVATE
#        yaml-cpp::yaml-cpp
#        Boost::system
#        Threads::Threads
#        open62541pp::open62541pp
#        spdlog::spdlog
#)
#target_include_directories(bridge_core PUBLIC include)
#target_compile_options(bridge_core PRIVATE -Wall -Wextra)
#
#
#add_executable(opc_serial_bridge src/main.cpp)
#target_compile_options(opc_serial_bridge PRIVATE -Wall -Wextra)
#target_link_libraries(opc_serial_bridge PRIVATE bridge_core)