#===================== Integration tests configuration ========================#
# Integration tests of Flash module on target. Registered in module
# CMakeLists.txt by IntegrationTesting_AddPath() and created by
# IntegrationTesting_Generate() in "IntegrationTest" build
# (see EmBi_Platform/CMake/IntegrationTesting/README.md).
#
# The test set is independent - target interface (entry point, initialization,
# fault handlers, system reset) is implemented by ItTarget_Flash.c and
# all libraries of the firmware are listed here.

# StartUp calls BspMain() - the entry point of the test firmware is provided by
# the test set (BspMain.h, ItTarget_Flash.c)
if(NOT TARGET BspMain_Lib)
    add_library(BspMain_Lib INTERFACE)
    target_include_directories(BspMain_Lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

IntegrationTesting_Add_Test(
    NAME            Flash
    TEST_SOURCE     ItTest_Flash.c
    SOURCES         ItTarget_Flash.c            # Target interface of the test set
    LINK_LIBS       StartUp_Lib                 # Startup code (calls BspMain)
                    Nvic_Lib                    # Fault handlers, system reset (ItTarget)
                    Rcc_Lib                     # Clocks
                    Flash_Lib                   # Module under test
    TIMEOUT         60
)
