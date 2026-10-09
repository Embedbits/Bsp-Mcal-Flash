#======================== Unit tests configuration ============================#
# Unit tests of Flash module. Registered in module CMakeLists.txt by
# UnitTesting_AddPath() and created by UnitTesting_Generate() in "UnitTest"
# build (see EmBi_Platform/CMake/UnitTesting/README.md).
#
# The test set is independent - it lists everything it needs: code under test,
# host emulation of the target (registers by RegMem, the flash memory by
# Ut_FlashMem.c) and libraries built for host.

get_filename_component(FLASH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(MCAL_DIR "${FLASH_DIR}/.." ABSOLUTE)

UnitTesting_Add_Test(
    NAME            Flash
    TEST_SOURCE     Test_Flash.c
    UUT_SOURCES     ${FLASH_DIR}/Flash.c
                    ${FLASH_DIR}/Flash_Map.c
                    ${FLASH_DIR}/Flash_Hw.c
                    ${CMAKE_CURRENT_SOURCE_DIR}/Ut_FlashMem.c   # Flash memory in host memory
    MOCK_HEADERS    ${MCAL_DIR}/Cache/Cache_Port.h
                    ${MCAL_DIR}/Rcc/Rcc_Port.h
                    ${MCAL_DIR}/Nvic/Nvic_Port.h
    INCLUDE_DIRS    ${FLASH_DIR}
                    ${MCAL_DIR}/Cache
                    ${MCAL_DIR}/Rcc
                    ${MCAL_DIR}/Nvic
                    ${CMAKE_CURRENT_SOURCE_DIR}
    LINK_LIBS       ut_RegMem                           # Registers in host memory
                    ut_CmsisHost                        # CMSIS core intrinsics on host
    FORCE_INCLUDES  ${UNIT_TESTING_CMSIS_HOST_HEADER}   # Replaces cmsis_gcc.h
    HOST_LIBS       Ral_Lib                             # ST LL compiled for host
    COVERAGE_EXCLUDE ".*/Ral/.*"
                     ".*/Ut_FlashMem.c"
)
