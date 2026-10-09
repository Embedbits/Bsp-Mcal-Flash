/**
 * \author Mr.Nobody
 * \file Flash.h
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) MCAL module common functionality header file.
 *
 * This file contains the common functionality used internally by the module
 * (STM32H5 flash controller). Nothing from this file is available outside of the
 * module - the public interface is Flash_Port.h.
 *
 */

#ifndef FLASH_FLASH_H
#define FLASH_FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Flash_Types.h"                    /* Module types definitions       */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Count of the flash banks */
#define FLASH_BANK_CNT                      ( 2u )

/** Maximal count of the areas in the area list (user flash and data flash of both banks, OTP) */
#define FLASH_AREA_CNT_MAX                  ( 5u )

/* ============================= TYPEDEFS =================================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

/* Memory map (Flash_Map.c) */
flash_AreaId_t          Flash_Build_AreaList    ( flash_AreaInfo_t * const areaList );
flash_RequestState_t    Flash_Find_Area         ( flash_Address_t address, flash_Size_t size, flash_AreaInfo_t * const areaInfo );
uint32_t                Flash_Get_SectorId      ( const flash_AreaInfo_t * const areaInfo, flash_Address_t address );
flash_Size_t            Flash_Get_DataSectors   ( flash_BankId_t bankId );
flash_Size_t            Flash_Get_DataSectorsMax( void );
flash_Size_t            Flash_Get_DataEraseSize ( void );
flash_Size_t            Flash_Get_BankSize      ( void );
flash_Size_t            Flash_Get_BankSizeStored( void );
void                    Flash_Reset_BankSize    ( void );

#ifdef __cplusplus
}
#endif

#endif /* FLASH_FLASH_H */
