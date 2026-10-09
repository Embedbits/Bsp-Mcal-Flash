/**
 * \author Mr.Nobody
 * \file Ut_FlashMem.h
 * \ingroup Flash
 * \brief Emulation of the STM32H5 flash memory in the host memory for unit tests.
 *
 * The flash memory (user flash, OTP and flash size register at 0x08FFF000, high-cycle data flash at
 * 0x09000000) is mapped to the same addresses the MCU uses before main() is executed. The code
 * under test and the tests access it by plain pointers. \ref UtFlashMem_Reset sets the whole memory
 * to the erased value (0xFF) - the flash size register then reads 0xFFFF (default size of the device).
 */

#ifndef FLASH_UT_FLASHMEM_H
#define FLASH_UT_FLASHMEM_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "stdint.h"                         /* Standard integer types         */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** First address of the emulated flash memory */
#define UTFLASHMEM_BASE                     ( 0x08000000u )

/** Size of the emulated flash memory (user flash, system flash, high-cycle data flash) */
#define UTFLASHMEM_SIZE                     ( 0x01040000u )

/* ======================== EXPORTED FUNCTIONS ============================== */

/** Sets the whole emulated flash memory to the erased value, returns 1 if the memory is mapped */
uint32_t UtFlashMem_Reset       ( void );

/** Presets the flash size register (size of the user flash in KB, 0 = default of the device) */
void     UtFlashMem_Set_SizeKb  ( uint16_t sizeKb );

#ifdef __cplusplus
}
#endif

#endif /* FLASH_UT_FLASHMEM_H */
