# xbox-eth-gateway: progetto ESP-IDF v5.5.x, target esp32.
# Struttura ripresa da third_party/bluepad32/examples/esp32/CMakeLists.txt:
# le righe qui sotto devono restare in questo ordine.
cmake_minimum_required(VERSION 3.16)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Bluepad32 e BTstack stanno in third_party/ (clone del tag 5.0.0-beta0 con il
# submodule external/btstack). Si può puntare a un'altra copia con la variabile
# d'ambiente BLUEPAD32_ROOT.
if(DEFINED ENV{BLUEPAD32_ROOT})
    set(BLUEPAD32_ROOT $ENV{BLUEPAD32_ROOT})
else()
    get_filename_component(BLUEPAD32_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/bluepad32" ABSOLUTE)
endif()
file(TO_CMAKE_PATH "${BLUEPAD32_ROOT}" BLUEPAD32_ROOT)

if(NOT EXISTS "${BLUEPAD32_ROOT}/src/components/bluepad32/CMakeLists.txt")
    message(FATAL_ERROR
        "Bluepad32 non trovato in ${BLUEPAD32_ROOT}. Vedi firmware/README.md, sezione \"Prerequisiti\".")
endif()
if(NOT EXISTS "${BLUEPAD32_ROOT}/external/btstack/port/esp32/components/btstack/CMakeLists.txt")
    message(FATAL_ERROR
        "BTstack non trovato: manca il submodule external/btstack di Bluepad32. "
        "Vedi firmware/README.md, sezione \"Prerequisiti\".")
endif()

# BTstack si integra come componente ESP-IDF: il suo CMake legge BTSTACK_ROOT
# dall'ambiente e aggiunge port/esp32/components a EXTRA_COMPONENT_DIRS.
set(ENV{BTSTACK_ROOT} "${BLUEPAD32_ROOT}/external/btstack")

# Componenti di Bluepad32 (bluepad32, cmd_nvs, cmd_system). I componenti propri
# del progetto stanno in firmware/components e vengono trovati da soli.
set(EXTRA_COMPONENT_DIRS "${BLUEPAD32_ROOT}/src/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{BTSTACK_ROOT}/port/esp32/components/btstack/cmake/BTstack_project.cmake)

project(xbox_eth_gateway)
