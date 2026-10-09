/**
 * \author Mr.Nobody
 * \file Flash_Map.c
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) MCAL module memory map (list of the flash areas)
 *
 * The flash memory of STM32H5 consists of two banks of 8 KB sectors. The last sectors of a bank
 * can be taken from the user flash by the option bytes (EDATAxR) as the high-cycle data flash
 * (not on STM32H503). The areas of the device are built from the flash size register, the
 * configuration of the high-cycle data flash and the constants of the ST CMSIS device header:
 *  - USER: one area per bank (the bank without the sectors taken by the high-cycle data flash)
 *  - DATA: one area per bank with the configured number of sectors, the window of the bank 2
 *          follows the window of the bank 1 (as in the ST HAL), the data of a sector fill
 *          FLASH_EDATA_SIZE / (banks * sectors per bank) bytes of the window (6 KB). The
 *          configured sectors are the LAST sectors of the window of the bank (as in the ST
 *          example FLASH_EDATA_EraseProgram), the rest of the window is not valid (bus error)
 *  - OTP: one area
 *
 * \note The ST LL has no flash erase / program functionality, the module uses the registers of
 *       the CMSIS device header (non-secure alias, the module works with TrustZone disabled).
 *
 * \note The flash size register is in the system data area (0x08FFF800). With the instruction cache enabled a
 *       CPU read of it raises a bus fault (Bug AB#1176): it is read with the cache suspended (Cache module),
 *       once - the size is kept until the module is deinitialized.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Cache_Port.h"                     /* Instruction cache suspend      */
#include "Flash.h"                          /* Self include                   */
#include "Flash_Types.h"                    /* Module types definitions       */
#include "Stm32.h"                          /* STM32 peripheral registers     */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Erase unit of the user flash (sector) in bytes */
#define FLASH_USER_ERASE_SIZE               ( (flash_Size_t)FLASH_SECTOR_SIZE )

/** Program unit of the user flash (128-bit flash word) in bytes */
#define FLASH_USER_PROGRAM_SIZE             ( 16u )

/** Guaranteed erase cycles of the user flash */
#define FLASH_USER_ENDURANCE                ( 10000u )

/** Program unit of the OTP area (16 bits) in bytes */
#define FLASH_OTP_PROGRAM_SIZE              ( 2u )

#if defined(FLASH_EDATAR_EDATA_EN)

/** Maximal count of the high-cycle data sectors of a bank (width of the EDATA_STRT field) */
#define FLASH_DATA_BANK_SECTORS_MAX         ( ( ( FLASH_EDATAR_EDATA_STRT ) >> FLASH_EDATAR_EDATA_STRT_Pos ) + 1u )

/** Size of the high-cycle data window of a bank in bytes */
#define FLASH_DATA_BANK_WINDOW_SIZE         ( (flash_Size_t)( FLASH_EDATA_SIZE / FLASH_BANK_CNT ) )

/** Erase unit of the high-cycle data flash in bytes (usable data of a sector) */
#define FLASH_DATA_ERASE_SIZE               ( (flash_Size_t)( FLASH_DATA_BANK_WINDOW_SIZE / FLASH_DATA_BANK_SECTORS_MAX ) )

/** Program unit of the high-cycle data flash (16 bits) in bytes */
#define FLASH_DATA_PROGRAM_SIZE             ( 2u )

/** Guaranteed erase cycles of the high-cycle data flash */
#define FLASH_DATA_ENDURANCE                ( 100000u )

#endif /* FLASH_EDATAR_EDATA_EN */

/* ========================== LOCAL VARIABLES =============================== */

/** Size of a bank of the user flash from the flash size register (0 until it is read) */
static flash_Size_t flash_Map_BankSize = 0u;

/* ======================= FORWARD DECLARATIONS ============================= */

static void             Flash_Fill_User         ( flash_AreaInfo_t * const areaInfo, flash_BankId_t bankId );
static void             Flash_Fill_Data         ( flash_AreaInfo_t * const areaInfo, flash_BankId_t bankId, flash_Size_t sectorCnt );
static void             Flash_Fill_Otp          ( flash_AreaInfo_t * const areaInfo );

/* ======================== EXPORTED FUNCTIONS ============================== */

/**
 * \brief Builds the list of the flash areas of the device.
 *
 * Order of the list: user flash of the bank 1 and 2, high-cycle data flash of the bank 1 and 2
 * (only banks with the configured sectors), OTP.
 *
 * \param areaList [out]: Array of at least \ref FLASH_AREA_CNT_MAX areas
 *
 * \return Count of the areas in the list
 */
flash_AreaId_t Flash_Build_AreaList( flash_AreaInfo_t * const areaList )
{
    flash_AreaId_t areaCnt = 0u;
    flash_BankId_t bankId;
    flash_Size_t   sectorCnt;

    for ( bankId = 0u; bankId < FLASH_BANK_CNT; bankId++ )
    {
        Flash_Fill_User( &areaList[ areaCnt ], bankId );
        areaCnt++;
    }

    for ( bankId = 0u; bankId < FLASH_BANK_CNT; bankId++ )
    {
        sectorCnt = Flash_Get_DataSectors( bankId );

        if ( 0u < sectorCnt )
        {
            Flash_Fill_Data( &areaList[ areaCnt ], bankId, sectorCnt );
            areaCnt++;
        }
    }

    Flash_Fill_Otp( &areaList[ areaCnt ] );
    areaCnt++;

    return (areaCnt);
}

/**
 * \brief Returns the size of a bank of the user flash (a half of the flash size register).
 *
 * The flash size register is read with the instruction cache suspended, once: the size is kept until
 * \ref Flash_Reset_BankSize. It must not be called from an interrupt (the cache is suspended); the
 * interrupt handlers use \ref Flash_Get_BankSizeStored.
 *
 * \return Size of a bank in bytes, 0 if the register can not be read (the cache can not be suspended)
 */
flash_Size_t Flash_Get_BankSize( void )
{
    cache_RequestState_t cacheState;

    if ( 0u == flash_Map_BankSize )
    {
        cacheState = Cache_Set_Suspend( CACHE_INSTRUCTION );

        if ( CACHE_REQUEST_OK == cacheState )
        {
            flash_Map_BankSize = (flash_Size_t)FLASH_BANK_SIZE;

            (void)Cache_Set_Resume( CACHE_INSTRUCTION );
        }
    }

    return (flash_Map_BankSize);
}


/**
 * \brief Returns the size of a bank of the user flash that was read before; it does not touch the hardware.
 *
 * \return Size of a bank in bytes, 0 if it was not read yet
 */
flash_Size_t Flash_Get_BankSizeStored( void )
{
    return (flash_Map_BankSize);
}


/**
 * \brief Forgets the size of a bank (it is read again by the next \ref Flash_Get_BankSize).
 */
void Flash_Reset_BankSize( void )
{
    flash_Map_BankSize = 0u;
}


/**
 * \brief Returns the maximal count of the sectors of the high-cycle data flash of a bank.
 *
 * \return Count of the sectors, 0 if the device has no high-cycle data flash
 */
flash_Size_t Flash_Get_DataSectorsMax( void )
{
    flash_Size_t sectorCnt = 0u;

#if defined(FLASH_EDATAR_EDATA_EN)
    sectorCnt = FLASH_DATA_BANK_SECTORS_MAX;
#endif /* FLASH_EDATAR_EDATA_EN */

    return (sectorCnt);
}


/**
 * \brief Returns the erase unit (usable data of a sector) of the high-cycle data flash.
 *
 * \return Erase unit in bytes, 0 if the device has no high-cycle data flash
 */
flash_Size_t Flash_Get_DataEraseSize( void )
{
    flash_Size_t eraseSize = 0u;

#if defined(FLASH_EDATAR_EDATA_EN)
    eraseSize = FLASH_DATA_ERASE_SIZE;
#endif /* FLASH_EDATAR_EDATA_EN */

    return (eraseSize);
}


/**
 * \brief Returns the number of the sector of the bank that holds an address of an area.
 *
 * The user flash starts at the first sector of the bank. The high-cycle data area consists of the
 * last sectors of the bank (the sector of the bank is erased by the sector number, not by the
 * address of the window).
 *
 * \param areaInfo [in]: Area that contains the address
 * \param address: Address in the area
 *
 * \return Number of the sector in the bank of the area
 */
uint32_t Flash_Get_SectorId( const flash_AreaInfo_t * const areaInfo, flash_Address_t address )
{
    uint32_t sectorId = ( address - areaInfo->Address ) / areaInfo->EraseSize;

    if ( FLASH_AREA_DATA == areaInfo->Type )
    {
        sectorId += (uint32_t)FLASH_SECTOR_NB - ( areaInfo->Size / areaInfo->EraseSize );
    }

    return (sectorId);
}


/**
 * \brief Finds the area that contains the whole range of the memory.
 *
 * \param address: Start address of the range
 * \param size: Size of the range in bytes (at least 1)
 * \param areaInfo [out]: Description of the area
 *
 * \return \ref FLASH_REQUEST_OK if the whole range lies in one area, otherwise \ref FLASH_REQUEST_ERROR
 */
flash_RequestState_t Flash_Find_Area( flash_Address_t address, flash_Size_t size, flash_AreaInfo_t * const areaInfo )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaList[ FLASH_AREA_CNT_MAX ];
    flash_AreaId_t       areaCnt;
    flash_AreaId_t       areaId;

    areaCnt = Flash_Build_AreaList( areaList );

    for ( areaId = 0u; areaId < areaCnt; areaId++ )
    {
        if (    ( 0u < size )
             && ( address >= areaList[ areaId ].Address )
             && ( size <= areaList[ areaId ].Size )
             && ( ( address - areaList[ areaId ].Address ) <= ( areaList[ areaId ].Size - size ) ) )
        {
            *areaInfo = areaList[ areaId ];
            retState  = FLASH_REQUEST_OK;
        }
    }

    return (retState);
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * \brief Reads the count of the sectors of a bank configured as the high-cycle data flash.
 *
 * \param bankId: Flash bank (0 based)
 *
 * \return Count of the sectors, 0 if the high-cycle data flash is disabled or the device has none
 */
flash_Size_t Flash_Get_DataSectors( flash_BankId_t bankId )
{
    flash_Size_t sectorCnt = 0u;

#if defined(FLASH_EDATAR_EDATA_EN)
    uint32_t regValue;

    if ( 0u == bankId )
    {
        regValue = FLASH_NS->EDATA1R_CUR;
    }
    else
    {
        regValue = FLASH_NS->EDATA2R_CUR;
    }

    if ( 0u != ( regValue & FLASH_EDATAR_EDATA_EN ) )
    {
        sectorCnt = ( ( regValue & FLASH_EDATAR_EDATA_STRT ) >> FLASH_EDATAR_EDATA_STRT_Pos ) + 1u;
    }
#else
    (void)bankId;
#endif /* FLASH_EDATAR_EDATA_EN */

    return (sectorCnt);
}


/**
 * \brief Fills the description of the user flash of a bank.
 *
 * The last sectors of the bank taken by the high-cycle data flash are not part of the area.
 *
 * \param areaInfo [out]: Area description
 * \param bankId: Flash bank (0 based)
 */
static void Flash_Fill_User( flash_AreaInfo_t * const areaInfo, flash_BankId_t bankId )
{
    flash_Size_t bankSize = Flash_Get_BankSize();
    flash_Size_t dataSize = Flash_Get_DataSectors( bankId ) * FLASH_USER_ERASE_SIZE;

    areaInfo->Type        = FLASH_AREA_USER;
    areaInfo->Bank        = bankId;
    areaInfo->Address     = (flash_Address_t)FLASH_BASE_NS + ( (flash_Size_t)bankId * bankSize );
    areaInfo->Size        = 0u;                         /* the size register could not be read */

    if ( bankSize > dataSize )
    {
        areaInfo->Size = bankSize - dataSize;
    }

    areaInfo->EraseSize   = FLASH_USER_ERASE_SIZE;
    areaInfo->ProgramSize = FLASH_USER_PROGRAM_SIZE;
    areaInfo->Endurance   = FLASH_USER_ENDURANCE;
    areaInfo->Attributes  = FLASH_AREA_ATTR_ERASE_REQUIRED | FLASH_AREA_ATTR_ECC;
}


/**
 * \brief Fills the description of the high-cycle data flash of a bank.
 *
 * \param areaInfo [out]: Area description
 * \param bankId: Flash bank (0 based)
 * \param sectorCnt: Count of the configured sectors of the bank
 */
static void Flash_Fill_Data( flash_AreaInfo_t * const areaInfo, flash_BankId_t bankId, flash_Size_t sectorCnt )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    areaInfo->Type        = FLASH_AREA_DATA;
    areaInfo->Bank        = bankId;
    areaInfo->Address     = (flash_Address_t)FLASH_EDATA_BASE_NS + ( (flash_Size_t)bankId * FLASH_DATA_BANK_WINDOW_SIZE )
                          + ( ( FLASH_DATA_BANK_SECTORS_MAX - sectorCnt ) * FLASH_DATA_ERASE_SIZE );
    areaInfo->Size        = sectorCnt * FLASH_DATA_ERASE_SIZE;
    areaInfo->EraseSize   = FLASH_DATA_ERASE_SIZE;
    areaInfo->ProgramSize = FLASH_DATA_PROGRAM_SIZE;
    areaInfo->Endurance   = FLASH_DATA_ENDURANCE;
    areaInfo->Attributes  = FLASH_AREA_ATTR_ERASE_REQUIRED | FLASH_AREA_ATTR_ECC | FLASH_AREA_ATTR_CONFIGURABLE | FLASH_AREA_ATTR_PROGRAM_BEFORE_READ;
#else
    (void)areaInfo;
    (void)bankId;
    (void)sectorCnt;
#endif /* FLASH_EDATAR_EDATA_EN */

    return;
}


/**
 * \brief Fills the description of the OTP area.
 *
 * \param areaInfo [out]: Area description
 */
static void Flash_Fill_Otp( flash_AreaInfo_t * const areaInfo )
{
    areaInfo->Type        = FLASH_AREA_OTP;
    areaInfo->Bank        = 0u;
    areaInfo->Address     = (flash_Address_t)FLASH_OTP_BASE;
    areaInfo->Size        = (flash_Size_t)FLASH_OTP_SIZE;
    areaInfo->EraseSize   = 0u;
    areaInfo->ProgramSize = FLASH_OTP_PROGRAM_SIZE;
    areaInfo->Endurance   = 0u;
    areaInfo->Attributes  = FLASH_AREA_ATTR_ECC | FLASH_AREA_ATTR_WRITE_ONCE;
}
