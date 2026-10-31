/**
 * \author Mr.Nobody
 * \file Flash_Port.h
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) MCAL module public functionality
 *
 * This file contains all available public functionality, any other files shall
 * not used outside of the module. The interface is the same for all families.
 *
 */

#ifndef FLASH_FLASH_PORT_H
#define FLASH_FLASH_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "Flash_Types.h"                    /* Module types definition        */
/* ============================== TYPEDEFS ================================== */

/* ========================== SYMBOLIC CONSTANTS ============================ */

/* ========================== EXPORTED MACROS =============================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

flash_ModuleVersion_t   Flash_Get_ModuleVersion     ( void );

flash_RequestState_t    Flash_Init                  ( const flash_Config_t * const flashConfig );
flash_RequestState_t    Flash_Deinit                ( flash_PeriphId_t periphId );
void                    Flash_Task                  ( void );

flash_RequestState_t    Flash_Get_DefaultConfig     ( flash_Config_t * const flashConfig );
flash_RequestState_t    Flash_Get_PeriphState       ( flash_PeriphId_t periphId, flash_FlagState_t * const periphState );

/*---------------------------- Memory description ----------------------------*/

flash_RequestState_t    Flash_Get_AreaCount         ( flash_PeriphId_t periphId, flash_AreaId_t * const areaCount );
flash_RequestState_t    Flash_Get_AreaInfo          ( flash_PeriphId_t periphId, flash_AreaId_t areaId, flash_AreaInfo_t * const areaInfo );

/*------------------------------- Data access --------------------------------*/

flash_RequestState_t    Flash_Get_Data              ( flash_PeriphId_t periphId, flash_Address_t address, flash_DataByte_t * const data, flash_Size_t size );
flash_RequestState_t    Flash_Get_BlankState        ( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size, flash_FlagState_t * const blankState );
flash_RequestState_t    Flash_Set_EraseStart        ( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size );
flash_RequestState_t    Flash_Set_WriteStart        ( flash_PeriphId_t periphId, flash_Address_t address, const flash_DataByte_t * const data, flash_Size_t size );

/*----------------------------- Operation state ------------------------------*/

flash_RequestState_t    Flash_Get_OperationState    ( flash_PeriphId_t periphId, flash_OperationState_t * const operationState );
flash_RequestState_t    Flash_Get_OperationError    ( flash_PeriphId_t periphId, flash_Error_t * const operationError );

/*----------------------------------- ECC ------------------------------------*/

flash_RequestState_t    Flash_Get_EccState          ( flash_PeriphId_t periphId, flash_EccState_t * const eccState );
flash_RequestState_t    Flash_Set_EccStateClear     ( flash_PeriphId_t periphId );
void                    Flash_Handle_Nmi            ( void );

/*--------------------------------- TrustZone ---------------------------------*/

flash_RequestState_t    Flash_Get_TrustZoneState    ( flash_TrustZoneState_t * const state );

/*------------------------- Data area configuration --------------------------*/

flash_RequestState_t    Flash_Set_AreaSize          ( flash_PeriphId_t periphId, flash_AreaType_t areaType, flash_Size_t size );
flash_RequestState_t    Flash_Get_ConfigState       ( flash_PeriphId_t periphId, flash_ConfigState_t * const configState );
flash_RequestState_t    Flash_Set_ConfigApply       ( flash_PeriphId_t periphId );

#ifdef __cplusplus
}
#endif

#endif /* FLASH_FLASH_PORT_H */
