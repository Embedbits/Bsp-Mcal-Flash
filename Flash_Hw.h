/**
 * \author Mr.Nobody
 * \file Flash_Hw.h
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) MCAL module register level functionality header file.
 *
 * Private functions that work directly with the registers of the flash controller (the ST LL has no
 * flash erase / program functionality). Nothing from this file is available outside of the module.
 *
 */

#ifndef FLASH_FLASH_HW_H
#define FLASH_FLASH_HW_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Flash_Types.h"                    /* Module types definitions       */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Count of the iterations of the busy-wait loop of a flash operation (the longest operation is a sector erase) */
#define FLASH_HW_BUSY_TIMEOUT_CNT           ( 5000000u )

/** Size of the flash word (quad word) programmed at once in bytes */
#define FLASH_HW_QUADWORD_SIZE              ( 16u )

/** Count of the 32-bit words of the quad word */
#define FLASH_HW_QUADWORD_WORD_CNT          ( 4u )

/* ============================= TYPEDEFS =================================== */

/** Type representing iteration count of busy-wait loops */
typedef uint32_t flash_TimeoutCnt_t;

/* ========================= EXPORTED FUNCTIONS ============================= */

flash_RequestState_t    Flash_Hw_Unlock             ( void );
void                    Flash_Hw_Lock               ( void );
void                    Flash_Hw_Clear_Flags        ( void );
flash_Error_t           Flash_Hw_Wait_Ready         ( void );
flash_FlagState_t       Flash_Hw_Get_BusyState      ( void );
flash_Error_t           Flash_Hw_Get_Result         ( void );
void                    Flash_Hw_Set_Interrupts     ( flash_FunctionState_t interruptState );
void                    Flash_Hw_Set_EccInterrupt   ( flash_FunctionState_t interruptState );

flash_RequestState_t    Flash_Hw_Start_SectorErase  ( flash_BankId_t bankId, uint32_t sectorId );
void                    Flash_Hw_Stop_SectorErase   ( void );

flash_RequestState_t    Flash_Hw_Start_Program      ( void );
void                    Flash_Hw_Program_QuadWord   ( flash_Address_t address, const uint32_t * const words );
void                    Flash_Hw_Program_HalfWord   ( flash_Address_t address, uint16_t value );
void                    Flash_Hw_Stop_Program       ( void );

flash_Error_t           Flash_Hw_Read_Ecc           ( flash_Size_t bankSize, flash_Address_t * const address );
void                    Flash_Hw_Clear_Ecc          ( flash_Error_t eccMask );
uint32_t                Flash_Hw_Mask_EccNmi        ( void );
void                    Flash_Hw_Restore_EccNmi     ( uint32_t previousMask );

flash_RequestState_t    Flash_Hw_Unlock_Options     ( void );
void                    Flash_Hw_Lock_Options       ( void );
flash_RequestState_t    Flash_Hw_Set_DataSectors    ( flash_BankId_t bankId, flash_Size_t sectorCnt );
flash_Size_t            Flash_Hw_Get_DataSectors    ( flash_BankId_t bankId );
void                    Flash_Hw_Start_OptionProgram( void );

#ifdef __cplusplus
}
#endif

#endif /* FLASH_FLASH_HW_H */
