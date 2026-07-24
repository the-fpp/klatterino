cmake_minimum_required(VERSION 3.22)

project(rumble-credential-store-probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Qt6Keychain REQUIRED)

add_executable(rumble-credential-store-probe
    main.cpp
    ../../src/providers/rumble/RumbleCredentialStore.cpp
    ../../src/providers/rumble/RumbleCredentialStore.hpp
    ../../src/providers/rumble/RumbleCredentialStorePrivate.hpp
)

target_include_directories(rumble-credential-store-probe
    PRIVATE ../../src
)

target_link_libraries(rumble-credential-store-probe
    PRIVATE
        Qt6::Core
        Qt6Keychain::Qt6Keychain
)

install(TARGETS rumble-credential-store-probe
    RUNTIME DESTINATION bin
)
