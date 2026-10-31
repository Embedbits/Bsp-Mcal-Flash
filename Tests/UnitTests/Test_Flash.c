/**
 * \author Mr.Nobody
 * \file Test_Flash.c
 * \ingroup Flash
 * \brief Unit tests of the internal flash memory controller (FLASH) module.
 *
 * All Flash sources are compiled unchanged. The registers of the flash controller are emulated
 * by RegMem, the flash memory (user flash, OTP, flash size register, high-cycle data flash) by
 * Ut_FlashMem.c at the addresses of the MCU, the RCC module is mocked by CMock.
 *
 * Tested: memory map (areas), default configuration, initialization, read, blank check, erase and
 * write of the user flash in the blocking style and the state of the operation. The tests of the
 * other functions follow the implementation of the functions (until then the functions return an
 * error, see Ut_Flash_NotImplemented_ReturnError).
 *
 * \note Emulated registers are plain RAM, so the tests emulate the behaviour of the controller:
 *       - The control register is locked by the module (Init, end of an operation) and the unlock
 *         key sequence can not clear the LOCK bit, the tests clear it (Ut_Flash_Unlock) before an
 *         operation. The key sequence itself is tested with the LOCK bit that stays set.
 *       - The BSY flag is clear (operation finished at once), the error flags are preset by the tests.
 *       - The erase is verified by the sector selection (SNB, BKSEL) that stays in the control
 *         register, the write by the content of the emulated flash memory.
 *
 * The lines without the high-cycle data flash (STM32H503) skip the tests of that flash.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "RegMem.h"                         /* Register memory emulation      */
#include "Ut_FlashMem.h"                    /* Flash memory emulation         */
#include "Flash_Port.h"                     /* Module under test              */
#include "MockCache_Port.h"                 /* Cache module mock              */
#include "MockRcc_Port.h"                   /* RCC module mock                */
#include "MockNvic_Port.h"                  /* NVIC module mock               */
#include "Stm32.h"                          /* STM32 registers definition     */
#include <string.h>                         /* Test data handling             */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Erase unit of the user flash (8 KB sector) */
#define UT_FLASH_USER_ERASE_SIZE            ( 0x2000u )

/** Erase unit of the high-cycle data flash (6 KB) */
#define UT_FLASH_DATA_ERASE_SIZE            ( 0x1800u )

/** Address of the OTP area */
#define UT_FLASH_OTP_ADDR                   ( 0x08FFF000u )

/** Size of the OTP area */
#define UT_FLASH_OTP_SIZE                   ( 0x800u )

/** Count of the addresses reported by the ECC for the OTP area (one address per word of 4 bytes) */
#define UT_FLASH_ECC_OTP_WORD_CNT           ( UT_FLASH_OTP_SIZE / 4u )

#if defined(STM32H503xx)
/** Address reported by the ECC for the first OTP word: measured on STM32H503 (Bug AB#1191), RM0492 Rev 3 states 0x0600 */
#define UT_FLASH_ECC_OTP_FIRST              ( 0x200u )
#else
/** Address reported by the ECC for the first OTP word (RM0481 Rev 5 Table 88, RM0517 Rev 1 Table 75) */
#define UT_FLASH_ECC_OTP_FIRST              ( 0x600u )
#endif

/** Base address of the high-cycle data flash */
#define UT_FLASH_DATA_ADDR                  ( 0x09000000u )

/** Value of the flash size register that the modelled cache hides (the register is readable with the cache suspended only) */
#define UT_FLASH_CACHE_POISON_KB            ( 1u )

/** Size of the flash of the device (default of the ST header) */
#define UT_FLASH_SIZE                       ( (uint32_t)FLASH_SIZE_DEFAULT )

/** Value of the TZEN field (OPTSR2_CUR) when TrustZone is disabled (RM0481 Rev 5 Section 7.11.23) */
#define UT_FLASH_TZEN_DISABLED              ( 0xC3u )

/** Value of the TZEN field (OPTSR2_CUR) when TrustZone is enabled (RM0481 Rev 5 Section 7.11.23) */
#define UT_FLASH_TZEN_ENABLED               ( 0xB4u )

/* ======================= FORWARD DECLARATIONS ============================= */

#if defined(FLASH_EDATAR_EDATA_EN)
static uint32_t Ut_Flash_Get_DataSectorsMax ( void );
static void     Ut_Flash_Set_DataSectors    ( uint32_t bank1Sectors, uint32_t bank2Sectors );
static void     Ut_Flash_Init_DataFlash     ( uint32_t bank1Sectors, uint32_t bank2Sectors );
static void     Ut_Flash_Load_OptionBytes   ( void );
#endif

static void     Ut_Flash_Init               ( void );
static void     Ut_Flash_Init_Callbacks     ( void );
static void     Ut_Flash_Init_Style         ( flash_OperationStyle_t style, flash_Address_t windowAddress, flash_Size_t windowSize );
static void     Ut_Flash_OperationCallback  ( flash_PeriphId_t periphId, flash_OperationType_t operation, flash_Address_t address, flash_Size_t size, flash_Error_t error );
static nvic_RequestState_t Ut_Flash_NvicSetHandlerStub ( nvic_PeriphIrqList_t irqId, const nvic_IsrCallback_t irqHandler, int callCnt );
static void     Ut_Flash_ErrorCallback      ( flash_PeriphId_t periphId, flash_Error_t error, flash_Address_t address );
static void     Ut_Flash_Init_Config        ( const flash_Config_t * const config );
static void     Ut_Flash_Init_Window        ( flash_Address_t address, flash_Size_t size );
static void     Ut_Flash_Init_NoWindow      ( void );
static void     Ut_Flash_Unlock             ( void );
static void     Ut_Flash_Fill               ( flash_DataByte_t * data, uint32_t dataLen, uint8_t seed );
static cache_RequestState_t Ut_Flash_CacheSuspendStub    ( cache_CacheId_t cacheId, int callCnt );
static cache_RequestState_t Ut_Flash_CacheResumeStub     ( cache_CacheId_t cacheId, int callCnt );
static cache_RequestState_t Ut_Flash_CacheInvalidateStub ( cache_CacheId_t cacheId, int callCnt );

/** Record of the error callback calls */
typedef struct
{
    uint32_t         CallCnt;  /**< Count of calls            */
    flash_PeriphId_t PeriphId; /**< Controller                */
    flash_Error_t    Error;    /**< Error mask                */
    flash_Address_t  Address;  /**< Address of the error      */
}   utFlash_ErrorRecord_t;

/** Record of the last error callback call */
static utFlash_ErrorRecord_t utFlash_ErrorRecord;

/** Record of the end of operation callback calls */
typedef struct
{
    uint32_t              CallCnt;   /**< Count of calls            */
    flash_PeriphId_t      PeriphId;  /**< Controller                */
    flash_OperationType_t Operation; /**< Operation                 */
    flash_Address_t       Address;   /**< Start address             */
    flash_Size_t          Size;      /**< Size in bytes             */
    flash_Error_t         Error;     /**< Error mask                */
}   utFlash_OpRecord_t;

/** Record of the last end of operation callback call */
static utFlash_OpRecord_t utFlash_OpRecord;

/** Record of the calls of the Cache module (the stubs of the mock) */
typedef struct
{
    uint32_t             SuspendCnt;      /**< Count of the suspends of the instruction cache                    */
    uint32_t             ResumeCnt;       /**< Count of the resumes                                              */
    uint32_t             InvalidateCnt;   /**< Count of the invalidations                                        */
    cache_RequestState_t SuspendResult;   /**< Result of the suspend (set by the tests)                          */
    uint16_t             SizeKbSuspended; /**< Flash size register readable while suspended (0: not modelled)   */
    volatile uint8_t *   ResumePoison;    /**< Byte of the memory cleared by a resume (NULL: none)               */
    uint32_t             ResumeEccd;      /**< Value stored to the ECC detection register by a resume (0: none)  */
}   utFlash_CacheRecord_t;

/** Record of the calls of the Cache module */
static utFlash_CacheRecord_t utFlash_CacheRecord;

/** Handler of the flash interrupt registered in the NVIC module */
static nvic_IsrCallback_t utFlash_Isr = FLASH_NULL_PTR;

/** Priority of the flash interrupt used by the tests */
#define UT_FLASH_IRQ_PRIO                   ( 5u )

/* ============================ TEST FIXTURE ================================ */

void setUp( void )
{
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );
    (void)memset( &utFlash_ErrorRecord, 0, sizeof( utFlash_ErrorRecord ) );
    (void)memset( &utFlash_OpRecord, 0, sizeof( utFlash_OpRecord ) );
    (void)memset( &utFlash_CacheRecord, 0, sizeof( utFlash_CacheRecord ) );
    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_OK;
    utFlash_Isr = FLASH_NULL_PTR;
    Nvic_Set_PeriphIrq_Handler_Stub( Ut_Flash_NvicSetHandlerStub );
    Cache_Set_Suspend_Stub( Ut_Flash_CacheSuspendStub );
    Cache_Set_Resume_Stub( Ut_Flash_CacheResumeStub );
    Cache_Set_Invalidate_Stub( Ut_Flash_CacheInvalidateStub );
    TEST_ASSERT_EQUAL_UINT32( 1u, UtFlashMem_Reset() );
}

void tearDown( void )
{
    (void)Flash_Deinit( FLASH_PERIPH_1 );     /* the context of the module is static */
}

/* ============================ MODULE VERSION ============================== */

void Ut_Flash_Get_ModuleVersion_ReturnVersion( void )
{
    flash_ModuleVersion_t version = Flash_Get_ModuleVersion();

    TEST_ASSERT_EQUAL_UINT8( 1u, version.Major );
    TEST_ASSERT_EQUAL_UINT8( 0u, version.Minor );
    TEST_ASSERT_EQUAL_UINT8( 0u, version.Patch );
}

/* ============================ MEMORY MAP ================================== */

void Ut_Flash_Get_AreaCount_NoDataFlash_ThreeAreas( void )
{
    flash_AreaId_t areaCount = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );
    TEST_ASSERT_EQUAL_UINT8( 3u, areaCount );
}

void Ut_Flash_Get_AreaInfo_NoDataFlash_UserBanksAndOtp( void )
{
    flash_AreaInfo_t area;
    uint32_t         bankSize = UT_FLASH_SIZE / 2u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_USER, area.Type );
    TEST_ASSERT_EQUAL_UINT8( 0u, area.Bank );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u, area.Address );
    TEST_ASSERT_EQUAL_UINT32( bankSize, area.Size );
    TEST_ASSERT_EQUAL_UINT32( UT_FLASH_USER_ERASE_SIZE, area.EraseSize );
    TEST_ASSERT_EQUAL_UINT32( 16u, area.ProgramSize );
    TEST_ASSERT_EQUAL_UINT32( 10000u, area.Endurance );
    TEST_ASSERT_EQUAL_HEX32( FLASH_AREA_ATTR_ERASE_REQUIRED | FLASH_AREA_ATTR_ECC, area.Attributes );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 1u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_USER, area.Type );
    TEST_ASSERT_EQUAL_UINT8( 1u, area.Bank );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + bankSize, area.Address );
    TEST_ASSERT_EQUAL_UINT32( bankSize, area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_OTP, area.Type );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_OTP_ADDR, area.Address );
    TEST_ASSERT_EQUAL_UINT32( UT_FLASH_OTP_SIZE, area.Size );
    TEST_ASSERT_EQUAL_UINT32( 0u, area.EraseSize );
    TEST_ASSERT_EQUAL_UINT32( 2u, area.ProgramSize );
    TEST_ASSERT_EQUAL_UINT32( 0u, area.Endurance );
    TEST_ASSERT_EQUAL_HEX32( FLASH_AREA_ATTR_ECC | FLASH_AREA_ATTR_WRITE_ONCE, area.Attributes );
}

void Ut_Flash_Get_AreaInfo_FlashSizeRegister_UsedForBankSize( void )
{
    flash_AreaInfo_t area;
    uint16_t         sizeKb = (uint16_t)( UT_FLASH_SIZE / 2u / 1024u );

    UtFlashMem_Set_SizeKb( sizeKb );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( (uint32_t)sizeKb * 1024u / 2u, area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 1u, &area ) );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( (uint32_t)sizeKb * 1024u / 2u ), area.Address );
}

void Ut_Flash_Get_AreaInfo_DataFlashBank2_UserBank2ShrunkAndDataArea( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaId_t   areaCount = 0u;
    flash_AreaInfo_t area;
    uint32_t         bankSize = UT_FLASH_SIZE / 2u;
    uint32_t         windowSize = (uint32_t)FLASH_EDATA_SIZE / 2u;

    Ut_Flash_Set_DataSectors( 0u, 3u );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );
    TEST_ASSERT_EQUAL_UINT8( 4u, areaCount );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( bankSize, area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 1u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_USER, area.Type );
    TEST_ASSERT_EQUAL_UINT32( bankSize - ( 3u * UT_FLASH_USER_ERASE_SIZE ), area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_DATA, area.Type );
    TEST_ASSERT_EQUAL_UINT8( 1u, area.Bank );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR + windowSize + ( ( Ut_Flash_Get_DataSectorsMax() - 3u ) * UT_FLASH_DATA_ERASE_SIZE ), area.Address );   /* the last 3 sectors of the window */
    TEST_ASSERT_EQUAL_UINT32( 3u * UT_FLASH_DATA_ERASE_SIZE, area.Size );
    TEST_ASSERT_EQUAL_UINT32( UT_FLASH_DATA_ERASE_SIZE, area.EraseSize );
    TEST_ASSERT_EQUAL_UINT32( 2u, area.ProgramSize );
    TEST_ASSERT_EQUAL_UINT32( 100000u, area.Endurance );
    TEST_ASSERT_EQUAL_HEX32( FLASH_AREA_ATTR_ERASE_REQUIRED | FLASH_AREA_ATTR_ECC | FLASH_AREA_ATTR_CONFIGURABLE | FLASH_AREA_ATTR_PROGRAM_BEFORE_READ, area.Attributes );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 3u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_OTP, area.Type );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_AreaInfo_DataFlashBothBanks_FiveAreasInOrder( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaId_t   areaCount = 0u;
    flash_AreaInfo_t area;
    uint32_t         bankSize = UT_FLASH_SIZE / 2u;
    uint32_t         sectorsMax = Ut_Flash_Get_DataSectorsMax();

    Ut_Flash_Set_DataSectors( sectorsMax, 1u );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );
    TEST_ASSERT_EQUAL_UINT8( 5u, areaCount );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( bankSize - ( sectorsMax * UT_FLASH_USER_ERASE_SIZE ), area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 1u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( bankSize - UT_FLASH_USER_ERASE_SIZE, area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_DATA, area.Type );
    TEST_ASSERT_EQUAL_UINT8( 0u, area.Bank );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR, area.Address );
    TEST_ASSERT_EQUAL_UINT32( sectorsMax * UT_FLASH_DATA_ERASE_SIZE, area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 3u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_DATA, area.Type );
    TEST_ASSERT_EQUAL_UINT8( 1u, area.Bank );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR + ( (uint32_t)FLASH_EDATA_SIZE / 2u ) + ( ( sectorsMax - 1u ) * UT_FLASH_DATA_ERASE_SIZE ), area.Address );
    TEST_ASSERT_EQUAL_UINT32( UT_FLASH_DATA_ERASE_SIZE, area.Size );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 4u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_AREA_OTP, area.Type );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_AreaInfo_DataFlashDisabledWithStartField_NoDataArea( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaId_t areaCount = 0u;

    FLASH_NS->EDATA1R_CUR = 5u;     /* sectors field without the enable bit */
    FLASH_NS->EDATA2R_CUR = 7u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );
    TEST_ASSERT_EQUAL_UINT8( 3u, areaCount );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_AreaCount_InvalidParameters_ReturnError( void )
{
    flash_AreaId_t areaCount = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_AreaCount( FLASH_PERIPH_CNT, &areaCount ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_AreaCount( FLASH_PERIPH_1, FLASH_NULL_PTR ) );
}

void Ut_Flash_Get_AreaInfo_InvalidParameters_ReturnError( void )
{
    flash_AreaInfo_t area;
    flash_AreaId_t   areaCount = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_AreaInfo( FLASH_PERIPH_CNT, 0u, &area ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, FLASH_NULL_PTR ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaCount, &area ) );
}

/* ========================= DEFAULT CONFIGURATION ========================== */

void Ut_Flash_Get_DefaultConfig_NoDataFlash_BlockingWithoutWindows( void )
{
    flash_Config_t config;
    uint32_t       windowId;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    TEST_ASSERT_EQUAL( FLASH_PERIPH_1, config.PeriphId );
    TEST_ASSERT_EQUAL( FLASH_STYLE_BLOCKING, config.OperationStyle );
    TEST_ASSERT_EQUAL_UINT8( 0u, config.IrqPrio );
    TEST_ASSERT_EQUAL_UINT8( 0u, config.WindowCount );
    TEST_ASSERT_NULL( config.OperationCallback );
    TEST_ASSERT_NULL( config.ErrorCallback );

    for ( windowId = 0u; windowId < FLASH_WINDOW_MAX; windowId++ )
    {
        TEST_ASSERT_EQUAL_UINT32( 0u, config.Windows[ windowId ].Address );
        TEST_ASSERT_EQUAL_UINT32( 0u, config.Windows[ windowId ].Size );
    }
}

void Ut_Flash_Get_DefaultConfig_DataFlashBothBanks_WindowPerDataArea( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_Config_t config;

    Ut_Flash_Set_DataSectors( 2u, 4u );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    TEST_ASSERT_EQUAL_UINT8( 2u, config.WindowCount );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR + ( ( Ut_Flash_Get_DataSectorsMax() - 2u ) * UT_FLASH_DATA_ERASE_SIZE ), config.Windows[ 0u ].Address );
    TEST_ASSERT_EQUAL_UINT32( 2u * UT_FLASH_DATA_ERASE_SIZE, config.Windows[ 0u ].Size );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR + ( (uint32_t)FLASH_EDATA_SIZE / 2u ) + ( ( Ut_Flash_Get_DataSectorsMax() - 4u ) * UT_FLASH_DATA_ERASE_SIZE ), config.Windows[ 1u ].Address );
    TEST_ASSERT_EQUAL_UINT32( 4u * UT_FLASH_DATA_ERASE_SIZE, config.Windows[ 1u ].Size );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_DefaultConfig_NullPointer_ReturnError( void )
{
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_DefaultConfig( FLASH_NULL_PTR ) );
}

/* ========================= INITIALIZATION ================================= */

void Ut_Flash_Init_DefaultConfig_EnablesClockLocksAndClearsFlags( void )
{
    flash_Config_t    config;
    flash_FlagState_t state = FLASH_FLAG_INACTIVE;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    FLASH_NS->NSCR = 0u;                                /* unlocked */

    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_FLASH, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL_HEX32( FLASH_CCR_CLR_EOP | FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_OPTCHANGEERR,
                             FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_PeriphState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, state );
}

void Ut_Flash_Init_RccFails_ReturnErrorNotInitialized( void )
{
    flash_Config_t    config;
    flash_FlagState_t state = FLASH_FLAG_ACTIVE;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_FLASH, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_PeriphState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, state );
}

void Ut_Flash_Init_InvalidConfig_ReturnErrorWithoutRcc( void )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( FLASH_NULL_PTR ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    config.PeriphId = FLASH_PERIPH_CNT;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    config.OperationStyle = (flash_OperationStyle_t)( FLASH_STYLE_INTERRUPT + 1u );     /* unknown style */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    config.WindowCount = FLASH_WINDOW_MAX + 1u;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );
}

void Ut_Flash_Init_AlreadyInitialized_ReturnError( void )
{
    flash_Config_t config;

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );
}

void Ut_Flash_Deinit_Initialized_LocksAndDeactivates( void )
{
    flash_FlagState_t state = FLASH_FLAG_ACTIVE;

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );

    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_PeriphState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_1 ) );
}

void Ut_Flash_Deinit_InvalidOrNotInitialized_ReturnError( void )
{
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_1 ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_CNT ) );
}

void Ut_Flash_Get_PeriphState_InvalidParameters_ReturnError( void )
{
    flash_FlagState_t state;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_PeriphState( FLASH_PERIPH_CNT, &state ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_PeriphState( FLASH_PERIPH_1, FLASH_NULL_PTR ) );
}

/* ============================ READ AND BLANK CHECK ======================== */

void Ut_Flash_Get_Data_UserFlash_CopiesBytes( void )
{
    flash_DataByte_t  expected[ 37 ];
    flash_DataByte_t  data[ 37 ];

    Ut_Flash_Fill( expected, sizeof( expected ), 0x21u );
    (void)memcpy( (void *)(uintptr_t)0x08001003u, expected, sizeof( expected ) );
    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08001003u, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( expected, data, sizeof( expected ) );
}

void Ut_Flash_Get_Data_OtpArea_Readable( void )
{
    flash_DataByte_t data[ 4 ] = { 0u };

    Ut_Flash_Init();
    *(volatile uint8_t *)(uintptr_t)0x08FFF000u = 0xA5u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08FFF000u, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL_HEX8( 0xA5u, data[ 0 ] );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, data[ 1 ] );
}

void Ut_Flash_Get_Data_InvalidParameters_ReturnError( void )
{
    flash_DataByte_t data[ 16 ];
    uint32_t         bankSize = UT_FLASH_SIZE / 2u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u, data, sizeof( data ) ) );   /* not initialized */

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_CNT, 0x08000000u, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u, FLASH_NULL_PTR, sizeof( data ) ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u, data, 0u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x07FFFFF0u, data, sizeof( data ) ) );    /* before the flash */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u + bankSize - 8u, data, sizeof( data ) ) ); /* crosses two areas */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u + UT_FLASH_SIZE, data, sizeof( data ) ) );   /* after the flash */
}

void Ut_Flash_Get_BlankState_ErasedAndWritten_ReportsBlank( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08002000u, 0x2000u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    *(volatile uint8_t *)(uintptr_t)0x08003FFFu = 0x00u;        /* last byte of the range */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08002000u, 0x2000u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, blankState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08002000u, 0x1FFFu, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );
}

void Ut_Flash_Get_BlankState_InvalidParameters_ReturnError( void )
{
    flash_FlagState_t blankState;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08000000u, 16u, &blankState ) );  /* not initialized */

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08000000u, 16u, FLASH_NULL_PTR ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08000000u, 0u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, 0x00000000u, 16u, &blankState ) );
}

/* ================================== ERASE ================================= */

void Ut_Flash_Set_EraseStart_SectorOfBank1_SelectsSectorAndLocks( void )
{
    flash_OperationState_t state = FLASH_STATE_BUSY;
    flash_Error_t          error = FLASH_ERR_ECC_DETECTED;

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u + ( 3u * UT_FLASH_USER_ERASE_SIZE ), UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_UINT32( 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_BITS_LOW( FLASH_CR_BKSEL, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_LOW( FLASH_CR_SER, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, error );
}

void Ut_Flash_Set_EraseStart_SectorOfBank2_SelectsBankAndSector( void )
{
    uint32_t bankSize = UT_FLASH_SIZE / 2u;

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u + bankSize + ( 5u * UT_FLASH_USER_ERASE_SIZE ), UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_UINT32( 5u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_BKSEL, FLASH_NS->NSCR );
}

void Ut_Flash_Set_EraseStart_MoreSectors_LastSectorSelected( void )
{
    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u + UT_FLASH_USER_ERASE_SIZE, 3u * UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_UINT32( 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );    /* sectors 1, 2, 3 */
}

void Ut_Flash_Set_EraseStart_LockedControlRegister_KeysWrittenProtectionError( void )
{
    flash_OperationState_t state = FLASH_STATE_IDLE;
    flash_Error_t          error = FLASH_ERR_NONE;

    Ut_Flash_Init();                                    /* locked, the LOCK bit of the emulated register stays set */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( 0xCDEF89ABu, FLASH_NS->NSKEYR );       /* the second key was written last */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_FAILED, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_EraseStart_InvalidRequest_ReturnErrorRegistersUntouched( void )
{
    flash_OperationState_t state = FLASH_STATE_FAILED;
    uint32_t               bankSize = UT_FLASH_SIZE / 2u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );  /* not initialized */

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSCR = 0u;
    FLASH_NS->NSCCR = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_CNT, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, 0u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000010u, UT_FLASH_USER_ERASE_SIZE ) );     /* address not aligned */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE + 16u ) ); /* size not aligned */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u + bankSize - UT_FLASH_USER_ERASE_SIZE, 2u * UT_FLASH_USER_ERASE_SIZE ) ); /* two areas */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08FFF000u, UT_FLASH_OTP_SIZE ) );             /* OTP can not be erased */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x20000000u, UT_FLASH_USER_ERASE_SIZE ) );     /* not a flash */

    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
}

void Ut_Flash_Set_EraseStart_ErrorFlag_FailedWithDecodedError( void )
{
    flash_OperationState_t state = FLASH_STATE_IDLE;
    flash_Error_t          error = FLASH_ERR_NONE;

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_WRPERR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( FLASH_CCR_CLR_WRPERR, FLASH_NS->NSCCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_FAILED, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_EraseStart_BusyForever_TimeoutAndLocked( void )
{
    flash_Error_t error = FLASH_ERR_NONE;

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_BSY;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_TIMEOUT, error );
}

/* ================================== WRITE ================================= */

void Ut_Flash_Set_WriteStart_QuadWords_ProgramsMemoryAndLocks( void )
{
    flash_DataByte_t       data[ 48 ];
    flash_DataByte_t       readBack[ 48 ];
    flash_OperationState_t state = FLASH_STATE_BUSY;

    Ut_Flash_Fill( data, sizeof( data ), 0x10u );
    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08004010u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08004010u, readBack, sizeof( readBack ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, sizeof( data ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_PG, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
}

void Ut_Flash_Set_WriteStart_UnalignedSource_WordsAssembledLittleEndian( void )
{
    flash_DataByte_t buffer[ 17 ];
    flash_DataByte_t readBack[ 16 ];

    Ut_Flash_Fill( buffer, sizeof( buffer ), 0x40u );
    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, &buffer[ 1 ], 16u ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u, readBack, sizeof( readBack ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( &buffer[ 1 ], readBack, sizeof( readBack ) );
}

void Ut_Flash_Set_WriteStart_DestinationNotErased_RefusedMemoryAndRegistersUntouched( void )
{
    flash_DataByte_t       data[ 16 ];
    flash_OperationState_t state = FLASH_STATE_IDLE;
    flash_Error_t          error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x70u );
    *(volatile uint8_t *)(uintptr_t)0x08000005u = 0x00u;
    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSCR = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX8( 0x00u, *(volatile uint8_t *)(uintptr_t)0x08000005u );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08000000u );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_FAILED, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NOT_ERASED, error );
}

void Ut_Flash_Set_WriteStart_InvalidRequest_ReturnError( void )
{
    flash_DataByte_t data[ 32 ] = { 0u };

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, data, 16u ) );      /* not initialized */

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_CNT, 0x08000000u, data, 16u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, FLASH_NULL_PTR, 16u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, data, 0u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000008u, data, 16u ) );      /* address not aligned */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, data, 24u ) );      /* size not aligned */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08FFF000u, data, 16u ) );      /* OTP: not in a window */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u + UT_FLASH_SIZE, data, 16u ) );
}

void Ut_Flash_Set_WriteStart_ErrorFlag_FailedMemoryUntouched( void )
{
    flash_DataByte_t data[ 16 ];
    flash_Error_t    error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x33u );
    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_PGSERR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX32( FLASH_CCR_CLR_PGSERR, FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08000000u );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_SEQUENCE, error );
}

void Ut_Flash_Set_WriteStart_LockedControlRegister_ProtectionError( void )
{
    flash_DataByte_t data[ 16 ] = { 0u };
    flash_Error_t    error = FLASH_ERR_NONE;

    Ut_Flash_Init();                                    /* locked, the LOCK bit of the emulated register stays set */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08000000u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX32( 0xCDEF89ABu, FLASH_NS->NSKEYR );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08000000u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_EraseStart_EndOfOperationFlag_ClearedWithoutError( void )
{
    flash_Error_t error = FLASH_ERR_PROTECTION;

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_EOP;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( FLASH_CCR_CLR_EOP, FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, error );
}

void Ut_Flash_Set_EraseStart_StrobeAndInconsistencyFlags_SequenceError( void )
{
    flash_Error_t error = FLASH_ERR_NONE;

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_STRBERR | FLASH_SR_INCERR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR, FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_SEQUENCE, error );
}

/* ============================ ACCESS WINDOWS ============================== */

void Ut_Flash_Init_InvalidWindow_ReturnErrorWithoutRcc( void )
{
    flash_Config_t config;
    uint32_t       bankSize = UT_FLASH_SIZE / 2u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    config.WindowCount = 1u;

    config.Windows[ 0 ].Address = 0x20000000u;                                  /* not a flash */
    config.Windows[ 0 ].Size    = UT_FLASH_USER_ERASE_SIZE;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    config.Windows[ 0 ].Address = 0x08000000u + bankSize - UT_FLASH_USER_ERASE_SIZE;   /* two areas */
    config.Windows[ 0 ].Size    = 2u * UT_FLASH_USER_ERASE_SIZE;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    config.Windows[ 0 ].Address = 0x08000000u;                                  /* empty window */
    config.Windows[ 0 ].Size    = 0u;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    config.Windows[ 0 ].Address = 0x08000010u;                                  /* address not aligned to the sector */
    config.Windows[ 0 ].Size    = UT_FLASH_USER_ERASE_SIZE;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    config.Windows[ 0 ].Address = 0x08000000u;                                  /* size not aligned to the sector */
    config.Windows[ 0 ].Size    = UT_FLASH_USER_ERASE_SIZE + 16u;
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );
}

void Ut_Flash_Init_ValidWindows_AcceptedUserOtpAndSecondWindowChecked( void )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.WindowCount          = 3u;
    config.Windows[ 0 ].Address = 0x08000000u + ( 2u * UT_FLASH_USER_ERASE_SIZE );    /* sectors of the user flash */
    config.Windows[ 0 ].Size    = 2u * UT_FLASH_USER_ERASE_SIZE;
    config.Windows[ 1 ].Address = UT_FLASH_OTP_ADDR;                                  /* OTP: aligned to the program unit */
    config.Windows[ 1 ].Size    = UT_FLASH_OTP_SIZE;
    config.Windows[ 2 ].Address = 0x08000000u + UT_FLASH_SIZE;                        /* the second window is validated as well */
    config.Windows[ 2 ].Size    = UT_FLASH_USER_ERASE_SIZE;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );

    config.WindowCount = 2u;

    Ut_Flash_Init_Config( &config );
}

void Ut_Flash_Init_DefaultConfigWithDataFlash_Accepted( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_Config_t config;

    Ut_Flash_Set_DataSectors( 2u, 3u );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL_UINT8( 2u, config.WindowCount );

    Ut_Flash_Init_Config( &config );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_EraseStart_OutsideWindow_ProtectionRegistersUntouched( void )
{
    flash_OperationState_t state = FLASH_STATE_IDLE;
    flash_Error_t          error = FLASH_ERR_NONE;

    Ut_Flash_Init_Window( 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE );             /* sectors 2 and 3 */
    Ut_Flash_Unlock();
    FLASH_NS->NSCR  = 0u;
    FLASH_NS->NSCCR = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08002000u, UT_FLASH_USER_ERASE_SIZE ) );      /* sector 1 */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08008000u, UT_FLASH_USER_ERASE_SIZE ) );      /* sector 4 */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08002000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );  /* sectors 1, 2 */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08006000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );  /* sectors 3, 4 */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, 5u * UT_FLASH_USER_ERASE_SIZE ) );  /* around the window */

    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_FAILED, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_EraseStart_InsideWindow_Accepted( void )
{
    flash_OperationState_t state = FLASH_STATE_FAILED;

    Ut_Flash_Init_Window( 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE );             /* sectors 2 and 3 */
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( 2u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );

    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );    /* the whole window */
    TEST_ASSERT_EQUAL_UINT32( 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
}

void Ut_Flash_Set_EraseStart_NoWindow_EverythingRefused( void )
{
    Ut_Flash_Init_NoWindow();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_SER, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_LOW( FLASH_CR_LOCK, FLASH_NS->NSCR );          /* never locked again - never unlocked */
}

void Ut_Flash_Set_EraseStart_AdjacentWindows_RequestOverTwoWindowsRefused( void )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.WindowCount          = 2u;
    config.Windows[ 0 ].Address = 0x08000000u;
    config.Windows[ 0 ].Size    = UT_FLASH_USER_ERASE_SIZE;
    config.Windows[ 1 ].Address = 0x08000000u + UT_FLASH_USER_ERASE_SIZE;
    config.Windows[ 1 ].Size    = UT_FLASH_USER_ERASE_SIZE;

    Ut_Flash_Init_Config( &config );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u + UT_FLASH_USER_ERASE_SIZE, UT_FLASH_USER_ERASE_SIZE ) );   /* the second window */
}

void Ut_Flash_Set_WriteStart_OutsideWindow_ProtectionMemoryUntouched( void )
{
    flash_DataByte_t data[ 32 ];
    flash_Error_t    error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x55u );
    Ut_Flash_Init_Window( 0x08004000u, UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();
    FLASH_NS->NSCR = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08003FF0u, data, 16u ) );              /* before the window */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08006000u, data, 16u ) );              /* after the window */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08005FF0u, data, sizeof( data ) ) );   /* over the end of the window */

    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08003FF0u );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08005FF0u );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08006000u );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_WriteStart_InsideWindow_Accepted( void )
{
    flash_DataByte_t data[ 16 ];
    flash_DataByte_t readBack[ 16 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x66u );
    Ut_Flash_Init_Window( 0x08004000u, UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08005FF0u, data, sizeof( data ) ) );          /* the last flash word of the window */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08005FF0u, readBack, sizeof( readBack ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, sizeof( data ) );
}

/* ========================== HIGH-CYCLE DATA FLASH ========================= */

void Ut_Flash_Set_EraseStart_DataFlash_SectorNumbersAtTheEndOfTheBank( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t bank1Area;
    flash_AreaInfo_t bank2Area;

    Ut_Flash_Init_DataFlash( 2u, 3u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &bank1Area ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 3u, &bank2Area ) );

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, bank1Area.Address + UT_FLASH_DATA_ERASE_SIZE, UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( (uint32_t)FLASH_SECTOR_NB - 2u + 1u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_BITS_LOW( FLASH_CR_BKSEL, FLASH_NS->NSCR );

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, bank2Area.Address, UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( (uint32_t)FLASH_SECTOR_NB - 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_BKSEL, FLASH_NS->NSCR );

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, bank2Area.Address, bank2Area.Size ) );      /* the whole area */
    TEST_ASSERT_EQUAL_UINT32( (uint32_t)FLASH_SECTOR_NB - 1u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_WriteStart_DataFlash_HalfWordsProgrammed( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t       area;
    flash_DataByte_t       data[ 9 ];
    flash_DataByte_t       readBack[ 8 ];
    flash_OperationState_t state = FLASH_STATE_BUSY;

    Ut_Flash_Fill( data, sizeof( data ), 0x81u );
    Ut_Flash_Init_DataFlash( 0u, 2u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, area.Address + 4u, &data[ 1 ], sizeof( readBack ) ) );   /* unaligned source */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, area.Address + 4u, readBack, sizeof( readBack ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( &data[ 1 ], readBack, sizeof( readBack ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_PG, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_WriteStart_DataFlash_DestinationNotChecked( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t area;
    flash_DataByte_t data[ 2 ] = { 0x12u, 0x34u };

    Ut_Flash_Init_DataFlash( 0u, 1u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    *(volatile uint8_t *)(uintptr_t)area.Address = 0x00u;      /* a programmed location: an erased one of the area can not be read */

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, area.Address, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL_HEX8( 0x12u, *(volatile uint8_t *)(uintptr_t)area.Address );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_WriteStart_DataFlash_UnitNotAligned_ReturnError( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t area;
    flash_DataByte_t data[ 4 ] = { 0u };

    Ut_Flash_Init_DataFlash( 0u, 1u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, area.Address + 1u, data, 2u ) );      /* odd address */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, area.Address, data, 3u ) );           /* odd size */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, area.Address + area.Size, data, 2u ) );  /* after the area */
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_BlankState_DataFlash_ReturnError( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t  area;
    flash_FlagState_t blankState = FLASH_FLAG_ACTIVE;

    Ut_Flash_Init_DataFlash( 1u, 0u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, area.Address, area.Size, &blankState ) );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_EraseStart_DataFlashWindows_UserFlashRefused( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    Ut_Flash_Init_DataFlash( 1u, 1u );          /* the default windows are the data areas only */
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_SER, FLASH_NS->NSCR );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

/* ===================== DATA FLASH CONFIGURATION =================== */

void Ut_Flash_Set_AreaSize_DataFlash_OptionBytesOfBank2ProgrammedFirst( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_OperationState_t state = FLASH_STATE_BUSY;

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 5u * UT_FLASH_DATA_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA1R_PRG );
    TEST_ASSERT_EQUAL_HEX32( FLASH_EDATAR_EDATA_EN | 4u, FLASH_NS->EDATA2R_PRG );
    TEST_ASSERT_BITS_HIGH( FLASH_OPTCR_OPTSTART, FLASH_NS->OPTCR );
    TEST_ASSERT_BITS_HIGH( FLASH_OPTCR_OPTLOCK, FLASH_NS->OPTCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA2R_CUR );               /* loaded by the controller, not written by the module */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_MoreThanOneBank_BothBanksProgrammed( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    uint32_t sectorsMax = Ut_Flash_Get_DataSectorsMax();

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, ( sectorsMax + 2u ) * UT_FLASH_DATA_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( FLASH_EDATAR_EDATA_EN | 1u, FLASH_NS->EDATA1R_PRG );
    TEST_ASSERT_EQUAL_HEX32( FLASH_EDATAR_EDATA_EN | ( sectorsMax - 1u ), FLASH_NS->EDATA2R_PRG );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_ZeroSize_Disabled( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    Ut_Flash_Set_DataSectors( 2u, 2u );
    FLASH_NS->EDATA1R_PRG = FLASH_EDATAR_EDATA_EN | 1u;
    FLASH_NS->EDATA2R_PRG = FLASH_EDATAR_EDATA_EN | 1u;

    Ut_Flash_Init_NoWindow();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 0u ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA1R_PRG );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA2R_PRG );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_InvalidRequest_ReturnErrorOptionBytesUntouched( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    uint32_t sectorsMax = Ut_Flash_Get_DataSectorsMax();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );   /* not initialized */

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_CNT, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_USER, UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_OTP, UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE + 2u ) );                 /* not a multiple of the erase unit */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, ( ( 2u * sectorsMax ) + 1u ) * UT_FLASH_DATA_ERASE_SIZE ) );  /* more than both banks */

    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA1R_PRG );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA2R_PRG );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->OPTCR );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_TakenSectorNotBlank_RefusedOptionBytesUntouched( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_Error_t error = FLASH_ERR_NONE;
    uint32_t      lastSectorAddr = 0x08000000u + UT_FLASH_SIZE - UT_FLASH_USER_ERASE_SIZE;

    *(volatile uint8_t *)(uintptr_t)( lastSectorAddr + 100u ) = 0x00u;     /* the last sector of the bank 2 holds data */

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA2R_PRG );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->OPTCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NOT_ERASED, error );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_OnlyNewSectorsChecked( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    uint32_t lastSectorAddr = 0x08000000u + UT_FLASH_SIZE - UT_FLASH_USER_ERASE_SIZE;

    Ut_Flash_Set_DataSectors( 0u, 2u );                                     /* the last two sectors of the bank 2 are the data flash */
    *(volatile uint8_t *)(uintptr_t)( lastSectorAddr + 100u ) = 0x00u;      /* ... so their content does not matter */

    Ut_Flash_Init_NoWindow();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 3u * UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL_HEX32( FLASH_EDATAR_EDATA_EN | 2u, FLASH_NS->EDATA2R_PRG );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_LockedOptionBytes_ProtectionError( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_Error_t error = FLASH_ERR_NONE;

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->OPTCR = FLASH_OPTCR_OPTLOCK;      /* the LOCK bit of the emulated register stays set */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( 0x4C5D6E7Fu, FLASH_NS->OPTKEYR );
    TEST_ASSERT_EQUAL_HEX32( 0u, FLASH_NS->EDATA2R_PRG );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_AreaSize_DataFlash_OptionChangeError_OperationError( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_Error_t error = FLASH_ERR_NONE;

    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_OPTCHANGEERR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_HEX32( FLASH_CCR_CLR_OPTCHANGEERR, FLASH_NS->NSCCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_OPERATION, error );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_ConfigState_DataFlash_PendingUntilTheReset( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_ConfigState_t configState = FLASH_CONFIG_PENDING;

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_APPLIED, configState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 2u * UT_FLASH_DATA_ERASE_SIZE ) );
    Ut_Flash_Load_OptionBytes();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_PENDING, configState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );     /* the reset: the module starts with the loaded configuration */
    Ut_Flash_Init_NoWindow();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_APPLIED, configState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_ConfigState( FLASH_PERIPH_CNT, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_ConfigState( FLASH_PERIPH_1, FLASH_NULL_PTR ) );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_ConfigApply_DataFlash_ResetOnlyWhenPending( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_ConfigApply( FLASH_PERIPH_1 ) );             /* nothing pending: no reset */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );
    Ut_Flash_Load_OptionBytes();

    Nvic_Set_SystemReset_Expect();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_ConfigApply( FLASH_PERIPH_1 ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_ConfigApply( FLASH_PERIPH_CNT ) );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_ConfigState_DataFlash_ValidSizeRequestedAgain_Applied( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_ConfigState_t configState = FLASH_CONFIG_APPLIED;

    Ut_Flash_Set_DataSectors( 0u, 1u );
    Ut_Flash_Init_NoWindow();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 3u * UT_FLASH_DATA_ERASE_SIZE ) );
    Ut_Flash_Load_OptionBytes();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_PENDING, configState );

    Ut_Flash_Unlock();
    FLASH_NS->OPTCR = 0u;                       /* the OPTLOCK bit of the emulated register stays set */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );   /* the size valid until the reset */
    Ut_Flash_Load_OptionBytes();

    TEST_ASSERT_EQUAL_HEX32( FLASH_EDATAR_EDATA_EN | 0u, FLASH_NS->EDATA2R_PRG );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_APPLIED, configState );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_ConfigApply( FLASH_PERIPH_1 ) );             /* nothing pending: no reset */
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Configuration_NoDataFlash_ReturnError( void )
{
#if !defined(FLASH_EDATAR_EDATA_EN)
    flash_ConfigState_t configState;

    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, UT_FLASH_DATA_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 0u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_ConfigApply( FLASH_PERIPH_1 ) );
#else
    TEST_IGNORE_MESSAGE( "The device has the high-cycle data flash" );
#endif
}

/* ========================= POLLING AND INTERRUPT STYLE ===================== */

void Ut_Flash_Set_EraseStart_Polling_ServedByTaskOneUnitPerCall( void )
{
    flash_OperationState_t state = FLASH_STATE_IDLE;

    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, 4u * UT_FLASH_USER_ERASE_SIZE );             /* sectors 2 .. 5 */
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 3u * UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_BUSY, state );
    TEST_ASSERT_EQUAL_UINT32( 2u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );             /* the first unit is started */
    TEST_ASSERT_BITS_LOW( FLASH_CR_LOCK, FLASH_NS->NSCR );                                              /* unlocked while the operation runs */

    Flash_Task();
    TEST_ASSERT_EQUAL_UINT32( 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    Flash_Task();
    TEST_ASSERT_EQUAL_UINT32( 4u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_OpRecord.CallCnt );

    Flash_Task();                                                                                       /* the last unit is finished */

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
    TEST_ASSERT_EQUAL( FLASH_PERIPH_1, utFlash_OpRecord.PeriphId );
    TEST_ASSERT_EQUAL( FLASH_OPERATION_ERASE, utFlash_OpRecord.Operation );
    TEST_ASSERT_EQUAL_HEX32( 0x08004000u, utFlash_OpRecord.Address );
    TEST_ASSERT_EQUAL_UINT32( 3u * UT_FLASH_USER_ERASE_SIZE, utFlash_OpRecord.Size );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, utFlash_OpRecord.Error );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_LOW( FLASH_CR_SER, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );

    Flash_Task();                                                                                       /* nothing to serve */
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
}

void Ut_Flash_Set_EraseStart_Polling_ControllerBusy_NoProgressUntilReady( void )
{
    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );

    FLASH_NS->NSSR = FLASH_SR_BSY;
    Flash_Task();
    Flash_Task();

    TEST_ASSERT_EQUAL_UINT32( 2u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );             /* still the first unit */
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_OpRecord.CallCnt );

    FLASH_NS->NSSR = 0u;
    Flash_Task();
    TEST_ASSERT_EQUAL_UINT32( 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    Flash_Task();
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
}

void Ut_Flash_Set_EraseStart_Polling_ErrorInSecondUnit_FailedAndReported( void )
{
    flash_OperationState_t state = FLASH_STATE_IDLE;
    flash_Error_t          error = FLASH_ERR_NONE;

    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, 3u * UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 3u * UT_FLASH_USER_ERASE_SIZE ) );
    Flash_Task();                                                                                       /* the second unit runs */

    FLASH_NS->NSSR = FLASH_SR_WRPERR;
    Flash_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, utFlash_OpRecord.Error );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_FAILED, state );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_WriteStart_Polling_UnitsProgrammedByTask( void )
{
    flash_DataByte_t data[ 48 ];
    flash_DataByte_t readBack[ 48 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x2Bu );
    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08004000u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX8( data[ 0 ], *(volatile uint8_t *)(uintptr_t)0x08004000u );                  /* the first flash word */
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08004010u );        /* not yet the second one */
    TEST_ASSERT_BITS_HIGH( FLASH_CR_PG, FLASH_NS->NSCR );

    Flash_Task();
    TEST_ASSERT_EQUAL_HEX8( data[ 16 ], *(volatile uint8_t *)(uintptr_t)0x08004010u );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08004020u );
    Flash_Task();
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_OpRecord.CallCnt );

    Flash_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
    TEST_ASSERT_EQUAL( FLASH_OPERATION_WRITE, utFlash_OpRecord.Operation );
    TEST_ASSERT_EQUAL_HEX32( 0x08004000u, utFlash_OpRecord.Address );
    TEST_ASSERT_EQUAL_UINT32( sizeof( data ), utFlash_OpRecord.Size );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, utFlash_OpRecord.Error );
    TEST_ASSERT_BITS_LOW( FLASH_CR_PG, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08004000u, readBack, sizeof( readBack ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, sizeof( data ) );
}

void Ut_Flash_Set_EraseStart_Polling_OperationRunning_OtherRequestsRefused( void )
{
    flash_DataByte_t data[ 16 ] = { 0u };

    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08006000u, UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08004000u, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_1 ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 0u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_ConfigApply( FLASH_PERIPH_1 ) );

    Flash_Task();
    Flash_Task();                                                                                       /* the operation ends */

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
}

void Ut_Flash_Task_Polling_EccError_ReportedByErrorCallback( void )
{
    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, UT_FLASH_USER_ERASE_SIZE );
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC | 0x10u;

    Flash_Task();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_ErrorRecord.CallCnt );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_CORRECTED, utFlash_ErrorRecord.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x10u * 16u ), utFlash_ErrorRecord.Address );
}

void Ut_Flash_Init_Interrupt_HandlerPriorityAndEccInterruptSet( void )
{
    Ut_Flash_Init_Style( FLASH_STYLE_INTERRUPT, 0x08004000u, UT_FLASH_USER_ERASE_SIZE );

    TEST_ASSERT_NOT_NULL( utFlash_Isr );
    TEST_ASSERT_BITS_HIGH( FLASH_ECCR_ECCIE, FLASH_NS->ECCCORR );

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
    TEST_ASSERT_BITS_LOW( FLASH_ECCR_ECCIE, FLASH_NS->ECCCORR );
}

void Ut_Flash_Init_Interrupt_NvicFails_ReturnErrorNotInitialized( void )
{
    flash_Config_t    config;
    flash_FlagState_t state = FLASH_FLAG_ACTIVE;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    config.OperationStyle = FLASH_STYLE_INTERRUPT;
    config.IrqPrio        = UT_FLASH_IRQ_PRIO;

    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_FLASH, RCC_REQUEST_OK );
    Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, UT_FLASH_IRQ_PRIO, NVIC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_PeriphState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, state );
}

void Ut_Flash_Set_EraseStart_Interrupt_ServedByInterrupt( void )
{
    flash_OperationState_t state = FLASH_STATE_IDLE;

    Ut_Flash_Init_Style( FLASH_STYLE_INTERRUPT, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_BITS_HIGH( FLASH_CR_EOPIE | FLASH_CR_WRPERRIE | FLASH_CR_PGSERRIE | FLASH_CR_STRBERRIE | FLASH_CR_INCERRIE, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL_UINT32( 2u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_BUSY, state );

    Flash_Task();                                                                                       /* not used in the interrupt style */
    TEST_ASSERT_EQUAL_UINT32( 2u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );

    utFlash_Isr();                                                                                      /* end of the first unit */
    TEST_ASSERT_EQUAL_UINT32( 3u, ( FLASH_NS->NSCR & FLASH_CR_SNB ) >> FLASH_CR_SNB_Pos );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_OpRecord.CallCnt );

    utFlash_Isr();                                                                                      /* end of the last unit */

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
    TEST_ASSERT_EQUAL( FLASH_OPERATION_ERASE, utFlash_OpRecord.Operation );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, utFlash_OpRecord.Error );
    TEST_ASSERT_BITS_LOW( FLASH_CR_EOPIE | FLASH_CR_WRPERRIE | FLASH_CR_PGSERRIE | FLASH_CR_STRBERRIE | FLASH_CR_INCERRIE, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

void Ut_Flash_Set_WriteStart_Interrupt_ErrorEndsOperationInInterrupt( void )
{
    flash_DataByte_t data[ 32 ];
    flash_Error_t    error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x44u );
    Ut_Flash_Init_Style( FLASH_STYLE_INTERRUPT, 0x08004000u, UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08004000u, data, sizeof( data ) ) );

    FLASH_NS->NSSR = FLASH_SR_PGSERR;
    utFlash_Isr();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_OpRecord.CallCnt );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_SEQUENCE, utFlash_OpRecord.Error );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08004010u );        /* the second unit was not written */
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_SEQUENCE, error );

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

void Ut_Flash_Irq_Interrupt_CorrectedEcc_ReportedByErrorCallback( void )
{
    Ut_Flash_Init_Style( FLASH_STYLE_INTERRUPT, 0x08004000u, UT_FLASH_USER_ERASE_SIZE );
    FLASH_NS->ECCCORR |= FLASH_ECCR_ECCC | 0x20u;

    utFlash_Isr();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_ErrorRecord.CallCnt );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_CORRECTED, utFlash_ErrorRecord.Error );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_OpRecord.CallCnt );

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, NVIC_REQUEST_OK );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

void Ut_Flash_Deinit_InterruptNvicFails_ReturnErrorButDeinitialized( void )
{
    flash_FlagState_t state = FLASH_FLAG_ACTIVE;

    Ut_Flash_Init_Style( FLASH_STYLE_INTERRUPT, 0x08004000u, UT_FLASH_USER_ERASE_SIZE );

    Nvic_Set_PeriphIrq_Inactive_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, NVIC_REQUEST_ERROR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_1 ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_PeriphState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, state );
}

/* ================================== OTP ==================================== */

void Ut_Flash_Set_WriteStart_Otp_HalfWordsProgrammedWriteOnce( void )
{
    flash_DataByte_t       data[ 6 ];
    flash_DataByte_t       readBack[ 6 ];
    flash_OperationState_t state = FLASH_STATE_BUSY;
    flash_Error_t          error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x61u );
    Ut_Flash_Init_Window( UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 4u, data, sizeof( data ) ) );

    FLASH_NS->ECCDETR = 0u;                                         /* the blank check wrote 1 to clear ECCD: the emulated register keeps it */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 4u, readBack, sizeof( readBack ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, sizeof( data ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_PG, FLASH_NS->NSCR );
    TEST_ASSERT_BITS_HIGH( FLASH_CR_LOCK, FLASH_NS->NSCR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 4u, data, 2u ) );   /* written once only */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NOT_ERASED, error );
    TEST_ASSERT_BITS_LOW( FLASH_CR_LOCK, FLASH_NS->NSCR );          /* refused before the control register was touched */
}

void Ut_Flash_Set_WriteStart_Otp_OutsideWindow_ProtectionMemoryUntouched( void )
{
    flash_DataByte_t data[ 2 ] = { 0x12u, 0x34u };
    flash_Error_t    error = FLASH_ERR_NONE;

    Ut_Flash_Init();                                                /* the windows of the user flash only */
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)UT_FLASH_OTP_ADDR );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_PROTECTION, error );
}

void Ut_Flash_Set_EraseStart_Otp_CanNotBeErased( void )
{
    Ut_Flash_Init_Window( UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, 2u ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_SER, FLASH_NS->NSCR );
}

void Ut_Flash_Set_WriteStart_Otp_UnitNotAligned_ReturnError( void )
{
    flash_DataByte_t data[ 4 ] = { 0u };

    Ut_Flash_Init_Window( UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 1u, data, 2u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, data, 3u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + UT_FLASH_OTP_SIZE, data, 2u ) );
}

void Ut_Flash_Get_BlankState_Otp_ReportsWrittenLocations( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    *(volatile uint8_t *)(uintptr_t)( UT_FLASH_OTP_ADDR + 0x7FFu ) = 0x00u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, blankState );
}

void Ut_Flash_Get_BlankState_UncorrectableEcc_NotBlankAndWriteRefused( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_ACTIVE;
    flash_EccState_t  eccState;
    flash_DataByte_t  data[ 16 ];
    flash_Error_t     error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x01u );
    Ut_Flash_Init_Window( 0x08002000u, UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | 0x200u;                                   /* user flash: the OTP ignores the flag, Bug AB#1192 */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08002000u, 16u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, blankState );
    TEST_ASSERT_BITS_LOW( SBS_ECCNMIR_ECCNMI_MASK_EN, SBS->ECCNMIR );               /* the NMI is unmasked again */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08002000u, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NOT_ERASED, error );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)0x08002000u );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
}

/* ====================== OPERATION STATE ================================== */

void Ut_Flash_Get_OperationState_InvalidParameters_ReturnError( void )
{
    flash_OperationState_t state;
    flash_Error_t          error;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );      /* not initialized */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_OperationState( FLASH_PERIPH_CNT, &state ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_OperationState( FLASH_PERIPH_1, FLASH_NULL_PTR ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_OperationError( FLASH_PERIPH_CNT, &error ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_OperationError( FLASH_PERIPH_1, FLASH_NULL_PTR ) );
}

/* ================================== ECC ==================================== */

void Ut_Flash_Get_EccState_NoError_NoneAndAddressZero( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0u, eccState.Address );
}

void Ut_Flash_Get_EccState_CorrectedErrorOfUserFlash_AddressDecoded( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init();
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC | 0x1234u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_CORRECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x1234u * 16u ), eccState.Address );
}

void Ut_Flash_Get_EccState_DetectedErrorOfBank2_AddressDecoded( void )
{
    flash_EccState_t eccState;
    uint32_t         bankSize = UT_FLASH_SIZE / 2u;

    Ut_Flash_Init();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_BK_ECC | 0x20u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + bankSize + ( 0x20u * 16u ), eccState.Address );
    TEST_ASSERT_BITS_HIGH( FLASH_ECCR_ECCD, FLASH_NS->ECCDETR );        /* write 1 to clear */
}

void Ut_Flash_Get_EccState_BothErrors_DetectedAddressWins( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init();
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC | 0x10u;
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | 0x30u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_CORRECTED | FLASH_ERR_ECC_DETECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x30u * 16u ), eccState.Address );
}

void Ut_Flash_Get_EccState_ErrorLatchedUntilCleared( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | 0x40u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );

    FLASH_NS->ECCDETR = 0u;                                             /* the controller cleared the flag */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x40u * 16u ), eccState.Address );

    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD;                                /* a pending flag */
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EccStateClear( FLASH_PERIPH_1 ) );
    TEST_ASSERT_BITS_HIGH( FLASH_ECCR_ECCD, FLASH_NS->ECCDETR );        /* write 1 to clear */
    TEST_ASSERT_BITS_HIGH( FLASH_ECCR_ECCC, FLASH_NS->ECCCORR );

    FLASH_NS->ECCDETR = 0u;
    FLASH_NS->ECCCORR = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0u, eccState.Address );
}

void Ut_Flash_Get_EccState_OtpError_AddressDecoded( void )
{
    /* The words measured on STM32H503 (Bug AB#1191): 0x08FFF000 / 0x08FFF004 / 0x08FFF010 / 0x08FFF7FC report 0x200 / 0x201 / 0x204 / 0x3FF */
    static const uint32_t wordIdList[] = { 0u, 1u, 4u, UT_FLASH_ECC_OTP_WORD_CNT - 1u };
    flash_EccState_t eccState;
    uint32_t         listId;

    Ut_Flash_Init();

    for ( listId = 0u; listId < ( sizeof( wordIdList ) / sizeof( wordIdList[ 0 ] ) ); listId++ )
    {
        FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | ( UT_FLASH_ECC_OTP_FIRST + wordIdList[ listId ] );

        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
        TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
        TEST_ASSERT_EQUAL_HEX32( UT_FLASH_OTP_ADDR + ( wordIdList[ listId ] * 4u ), eccState.Address );
    }
}

void Ut_Flash_Get_EccState_OtpFlagOutsideOtpArea_AddressZero( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | ( UT_FLASH_ECC_OTP_FIRST - 1u );     /* below the OTP area */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0u, eccState.Address );

    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | UT_FLASH_ECC_OTP_FIRST;              /* first OTP word */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_OTP_ADDR, eccState.Address );

    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | ( UT_FLASH_ECC_OTP_FIRST + UT_FLASH_ECC_OTP_WORD_CNT );   /* behind the OTP area (read-only area) */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, eccState.Address );
}

void Ut_Flash_Get_EccState_SystemFlashError_AddressZero( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init();
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC | FLASH_ECCR_SYSF_ECC | 0x10u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_CORRECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0u, eccState.Address );
}

void Ut_Flash_Get_EccState_DataFlashError_AddressDecoded( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_EccState_t eccState;
    uint32_t         offset = ( ( (uint32_t)FLASH_SECTOR_NB - Ut_Flash_Get_DataSectorsMax() ) * UT_FLASH_USER_ERASE_SIZE ) / 16u;

    Ut_Flash_Init();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_DATA_ECC | FLASH_ECCR_BK_ECC | ( offset + 0x10u );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR + ( (uint32_t)FLASH_EDATA_SIZE / 2u ) + ( 0x10u * 4u ), eccState.Address );

    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_DATA_ECC | ( offset + 0x20u );     /* bank 1 */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_HEX32( UT_FLASH_DATA_ADDR + ( 0x20u * 4u ), eccState.Address );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_EccState_InvalidParameters_ReturnError( void )
{
    flash_EccState_t eccState;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );       /* not initialized */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EccStateClear( FLASH_PERIPH_1 ) );

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_EccState( FLASH_PERIPH_CNT, &eccState ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_EccState( FLASH_PERIPH_1, FLASH_NULL_PTR ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EccStateClear( FLASH_PERIPH_CNT ) );
}

void Ut_Flash_Handle_Nmi_DetectedError_LatchedClearedAndCallbackCalled( void )
{
    flash_EccState_t eccState;

    Ut_Flash_Init_Callbacks();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | 0x55u;

    Flash_Handle_Nmi();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_ErrorRecord.CallCnt );
    TEST_ASSERT_EQUAL( FLASH_PERIPH_1, utFlash_ErrorRecord.PeriphId );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, utFlash_ErrorRecord.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x55u * 16u ), utFlash_ErrorRecord.Address );
    TEST_ASSERT_BITS_HIGH( FLASH_ECCR_ECCD, FLASH_NS->ECCDETR );         /* write 1 to clear: the NMI does not come again */

    FLASH_NS->ECCDETR = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x55u * 16u ), eccState.Address );
}

void Ut_Flash_Handle_Nmi_NoDetectedErrorOrNotInitialized_NothingHappens( void )
{
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC;                                /* a corrected error is not handled by the NMI */
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD;

    Flash_Handle_Nmi();                                                 /* not initialized: the flag is cleared, no callback */

    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_ErrorRecord.CallCnt );

    FLASH_NS->ECCDETR = 0u;
    Ut_Flash_Init_Callbacks();

    Flash_Handle_Nmi();                                                 /* no flag */

    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_ErrorRecord.CallCnt );
}

void Ut_Flash_Get_Data_UncorrectableError_ReturnErrorLatchedNmiMaskRestored( void )
{
    flash_DataByte_t data[ 16 ];
    flash_EccState_t eccState;

    Ut_Flash_Init();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | 0x80u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u, data, sizeof( data ) ) );

    TEST_ASSERT_BITS_LOW( SBS_ECCNMIR_ECCNMI_MASK_EN, SBS->ECCNMIR );   /* the NMI is unmasked again */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_DETECTED, eccState.Error );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( 0x80u * 16u ), eccState.Address );
}

void Ut_Flash_Get_Data_CorrectedError_ReadSucceedsErrorLatched( void )
{
    flash_DataByte_t data[ 16 ];
    flash_EccState_t eccState;

    Ut_Flash_Init();
    SBS->ECCNMIR = SBS_ECCNMIR_ECCNMI_MASK_EN;                          /* masked by the application before */
    FLASH_NS->ECCCORR = FLASH_ECCR_ECCC | 0x90u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08000000u, data, sizeof( data ) ) );

    TEST_ASSERT_BITS_HIGH( SBS_ECCNMIR_ECCNMI_MASK_EN, SBS->ECCNMIR );  /* the previous state of the mask is kept */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_ECC_CORRECTED, eccState.Error );
}

void Ut_Flash_Task_NothingToServe_NoEffect( void )
{
    flash_OperationState_t state = FLASH_STATE_BUSY;

    Ut_Flash_Init();

    Flash_Task();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_STATE_IDLE, state );
}


/* ============================== TRUSTZONE =================================== */

/**
 * \brief Reading the TrustZone state before Flash_Init is rejected.
 *
 * \par Expected results
 * \ref FLASH_REQUEST_ERROR, the module error value of a getter when it is not initialized.
 */
void Ut_Flash_Get_TrustZoneState_NotInitialized_ReturnError( void )
{
    flash_TrustZoneState_t state;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_TrustZoneState( &state ) );
}

/**
 * \brief A null output pointer is rejected after Flash_Init.
 *
 * \par Expected results
 * \ref FLASH_REQUEST_ERROR, no register is accessed.
 */
void Ut_Flash_Get_TrustZoneState_InvalidParameters_ReturnError( void )
{
    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_TrustZoneState( FLASH_NULL_PTR ) );
}

/**
 * \brief Disabled TrustZone: the lines with TZEN read the option bytes, STM32H503 (no TZEN) reports the
 *        same result without accessing any register.
 *
 * \par Expected results
 * \ref FLASH_FUNCTION_INACTIVE and a secure watermark of zero for both banks.
 */
void Ut_Flash_Get_TrustZoneState_Disabled_InactiveAndWatermarkZero( void )
{
    flash_TrustZoneState_t state;

    Ut_Flash_Init();
#if defined(FLASH_OPTSR2_TZEN)
    FLASH_NS->OPTSR2_CUR  = UT_FLASH_TZEN_DISABLED << FLASH_OPTSR2_TZEN_Pos;
    FLASH_NS->SECWM1R_CUR = 0u;
    FLASH_NS->SECWM2R_CUR = 0u;
#endif /* FLASH_OPTSR2_TZEN */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_TrustZoneState( &state ) );
    TEST_ASSERT_EQUAL( FLASH_FUNCTION_INACTIVE, state.TzState );
    TEST_ASSERT_EQUAL_UINT32( 0u, state.SecureWatermark[ 0 ].StartSector );
    TEST_ASSERT_EQUAL_UINT32( 0u, state.SecureWatermark[ 0 ].EndSector );
    TEST_ASSERT_EQUAL_UINT32( 0u, state.SecureWatermark[ 1 ].StartSector );
    TEST_ASSERT_EQUAL_UINT32( 0u, state.SecureWatermark[ 1 ].EndSector );
}

/**
 * \brief Enabled TrustZone with the secure watermark of both banks: ignored on STM32H503 (no TrustZone).
 *
 * \par Expected results
 * \ref FLASH_FUNCTION_ACTIVE and the start / end sector of the secure watermark of each bank decoded.
 */
void Ut_Flash_Get_TrustZoneState_Active_SecureWatermarkDecoded( void )
{
#if defined(FLASH_OPTSR2_TZEN)
    flash_TrustZoneState_t state;

    Ut_Flash_Init();
    FLASH_NS->OPTSR2_CUR  = UT_FLASH_TZEN_ENABLED << FLASH_OPTSR2_TZEN_Pos;
    FLASH_NS->SECWM1R_CUR = ( 2u << FLASH_SECWMR_SECWM_STRT_Pos ) | ( 5u << FLASH_SECWMR_SECWM_END_Pos );
    FLASH_NS->SECWM2R_CUR = ( 1u << FLASH_SECWMR_SECWM_STRT_Pos ) | ( 3u << FLASH_SECWMR_SECWM_END_Pos );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_TrustZoneState( &state ) );
    TEST_ASSERT_EQUAL( FLASH_FUNCTION_ACTIVE, state.TzState );
    TEST_ASSERT_EQUAL_UINT32( 2u, state.SecureWatermark[ 0 ].StartSector );
    TEST_ASSERT_EQUAL_UINT32( 5u, state.SecureWatermark[ 0 ].EndSector );
    TEST_ASSERT_EQUAL_UINT32( 1u, state.SecureWatermark[ 1 ].StartSector );
    TEST_ASSERT_EQUAL_UINT32( 3u, state.SecureWatermark[ 1 ].EndSector );
#else
    TEST_IGNORE_MESSAGE( "The device has no TrustZone" );
#endif /* FLASH_OPTSR2_TZEN */
}


/* ============================ INSTRUCTION CACHE =========================== */

void Ut_Flash_Get_AreaInfo_SizeRegisterRead_OnlyWhileInstructionCacheSuspended( void )
{
    flash_AreaInfo_t area;
    uint16_t         sizeKb = (uint16_t)( UT_FLASH_SIZE / 1024u );

    UtFlashMem_Set_SizeKb( UT_FLASH_CACHE_POISON_KB );                  /* the register is readable while the cache is suspended only */
    utFlash_CacheRecord.SizeKbSuspended = sizeKb;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );

    TEST_ASSERT_EQUAL_UINT32( UT_FLASH_SIZE / 2u, area.Size );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 1u, &area ) );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( UT_FLASH_SIZE / 2u ), area.Address );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );     /* the size is read once */
}

void Ut_Flash_Get_AreaInfo_CacheSuspendFails_UserAreaEmptyAndSizeReadAgain( void )
{
    flash_AreaInfo_t area;
    uint32_t         suspendCnt;

    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_ERROR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, area.Size );                          /* the register was not read */
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.ResumeCnt );
    suspendCnt = utFlash_CacheRecord.SuspendCnt;                        /* the suspend is tried for every bank */

    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_OK;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( UT_FLASH_SIZE / 2u, area.Size );
    TEST_ASSERT_EQUAL_UINT32( suspendCnt + 1u, utFlash_CacheRecord.SuspendCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );
}

void Ut_Flash_Init_CacheSuspendFails_ReturnErrorRccUntouched( void )
{
    flash_Config_t    config;
    flash_FlagState_t state = FLASH_FLAG_ACTIVE;

    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_ERROR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Init( &config ) );    /* the RCC mock is not expected to be called */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_PeriphState( FLASH_PERIPH_1, &state ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, state );
}

void Ut_Flash_Deinit_BankSizeForgotten_ReadAgainByTheNextUse( void )
{
    flash_AreaInfo_t area;

    Ut_Flash_Init();
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( 2u, utFlash_CacheRecord.SuspendCnt );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_1 ) );   /* not initialized: the size is forgotten as well */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( 3u, utFlash_CacheRecord.SuspendCnt );
}

void Ut_Flash_Deinit_OperationRunning_BankSizeKept( void )
{
    flash_AreaInfo_t area;

    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, 4u * UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Deinit( FLASH_PERIPH_1 ) );   /* the operation is running: still initialized */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 0u, &area ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );

    Flash_Task();                                                       /* the operation is finished */
    Flash_Task();
}

void Ut_Flash_Get_Data_OtpArea_ReadWhileInstructionCacheSuspended( void )
{
    flash_DataByte_t data[ 4 ] = { 0u };

    Ut_Flash_Init();
    *(volatile uint8_t *)(uintptr_t)UT_FLASH_OTP_ADDR = 0xA5u;
    utFlash_CacheRecord.ResumePoison = (volatile uint8_t *)(uintptr_t)UT_FLASH_OTP_ADDR;     /* a resume destroys the byte */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX8( 0xA5u, data[ 0 ] );                         /* read before the resume */
    TEST_ASSERT_EQUAL_UINT32( 2u, utFlash_CacheRecord.SuspendCnt );     /* size of the flash + the read */
    TEST_ASSERT_EQUAL_UINT32( 2u, utFlash_CacheRecord.ResumeCnt );
}

void Ut_Flash_Get_Data_UserFlash_InstructionCacheNotSuspended( void )
{
    flash_DataByte_t data[ 16 ];

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, 0x08001000u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );     /* the size of the flash only */
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );
}

void Ut_Flash_Get_Data_OtpArea_CacheSuspendFails_ReturnErrorNothingRead( void )
{
    flash_DataByte_t data[ 4 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x5Au );
    Ut_Flash_Init();
    *(volatile uint8_t *)(uintptr_t)UT_FLASH_OTP_ADDR = 0xA5u;
    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_ERROR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_HEX8( 0x5Au, data[ 0 ] );                         /* the memory was not touched */
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );      /* the one of the size of the flash */
}

void Ut_Flash_Get_Data_DataFlash_ReadWhileInstructionCacheSuspended( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t area;
    flash_DataByte_t data[ 4 ];

    Ut_Flash_Init_DataFlash( 0u, 2u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    utFlash_CacheRecord.SuspendCnt = 0u;
    utFlash_CacheRecord.ResumeCnt  = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, area.Address, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_BlankState_Otp_ReadWhileInstructionCacheSuspended( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;

    Ut_Flash_Init();
    utFlash_CacheRecord.SuspendCnt = 0u;
    utFlash_CacheRecord.ResumeCnt  = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );

    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_ERROR;
    blankState = FLASH_FLAG_INACTIVE;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, blankState );               /* not touched */
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );
}

void Ut_Flash_Get_BlankState_UserFlash_InstructionCacheNotSuspended( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;

    Ut_Flash_Init();
    utFlash_CacheRecord.SuspendCnt = 0u;
    utFlash_CacheRecord.ResumeCnt  = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, 0x08002000u, UT_FLASH_USER_ERASE_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.SuspendCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.ResumeCnt );
}

void Ut_Flash_Set_EraseStart_UserFlash_CacheInvalidatedOnceAtTheEnd( void )
{
    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, 3u * UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.InvalidateCnt );  /* three sectors, one invalidation */
}

void Ut_Flash_Set_WriteStart_UserFlash_CacheInvalidatedAtTheEnd( void )
{
    flash_DataByte_t data[ 32 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x11u );
    Ut_Flash_Init();
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, 0x08003000u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.InvalidateCnt );
}

void Ut_Flash_Set_EraseStart_FailedOperation_CacheInvalidated( void )
{
    Ut_Flash_Init();
    Ut_Flash_Unlock();
    FLASH_NS->NSSR = FLASH_SR_WRPERR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08000000u, UT_FLASH_USER_ERASE_SIZE ) );

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.InvalidateCnt );  /* the content may have changed */
}

void Ut_Flash_Set_EraseStart_Polling_CacheInvalidatedByTheTaskThatFinishes( void )
{
    Ut_Flash_Init_Style( FLASH_STYLE_POLLING, 0x08004000u, 4u * UT_FLASH_USER_ERASE_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, 0x08004000u, 2u * UT_FLASH_USER_ERASE_SIZE ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.InvalidateCnt );

    Flash_Task();
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.InvalidateCnt );

    Flash_Task();                                                       /* the last unit is finished */
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.InvalidateCnt );
}

void Ut_Flash_Set_WriteStart_DataFlash_CacheNotInvalidated( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t area;
    flash_DataByte_t data[ 8 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x81u );
    Ut_Flash_Init_DataFlash( 0u, 2u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );

    Ut_Flash_Unlock();
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, area.Address, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.InvalidateCnt );  /* the data flash is not served by the cache */
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Set_WriteStart_Otp_BlankCheckReadWithCacheSuspendedNotInvalidated( void )
{
    flash_DataByte_t data[ 6 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x61u );
    Ut_Flash_Init_Window( UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE );
    Ut_Flash_Unlock();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 4u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL_UINT32( 2u, utFlash_CacheRecord.SuspendCnt );     /* size of the flash + the blank check of the OTP */
    TEST_ASSERT_EQUAL_UINT32( 2u, utFlash_CacheRecord.ResumeCnt );
    TEST_ASSERT_EQUAL_UINT32( 0u, utFlash_CacheRecord.InvalidateCnt );  /* the OTP is not served by the cache */
}

void Ut_Flash_Set_WriteStart_Otp_CacheSuspendFails_RefusedAsNotErasedMemoryUntouched( void )
{
    flash_DataByte_t data[ 6 ];
    flash_Error_t    error = FLASH_ERR_NONE;

    Ut_Flash_Fill( data, sizeof( data ), 0x61u );
    Ut_Flash_Init_Window( UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE );
    Ut_Flash_Unlock();
    utFlash_CacheRecord.SuspendResult = CACHE_REQUEST_ERROR;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 4u, data, sizeof( data ) ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_OperationError( FLASH_PERIPH_1, &error ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NOT_ERASED, error );
    TEST_ASSERT_EQUAL_HEX8( FLASH_ERASED_VALUE, *(volatile uint8_t *)(uintptr_t)( UT_FLASH_OTP_ADDR + 4u ) );
    TEST_ASSERT_BITS_LOW( FLASH_CR_LOCK, FLASH_NS->NSCR );              /* refused before the control register was touched */
}

/* ========================= READ OF THE 16-BIT AREAS ======================= */

/* The OTP area does not accept an 8-bit access (Bug AB#1190): the bytes come from the 16-bit words that hold them.
   The width of an access can not be seen in the emulated memory - the integration tests on the device read the area. */
void Ut_Flash_Get_Data_OtpArea_UnalignedRange_BytesFromHalfWords( void )
{
    flash_DataByte_t pattern[ 8 ];
    flash_DataByte_t data[ 5 ];
    flash_DataByte_t single = 0u;

    Ut_Flash_Fill( pattern, sizeof( pattern ), 0x31u );
    (void)memcpy( (void *)(uintptr_t)( UT_FLASH_OTP_ADDR + 0x100u ), pattern, sizeof( pattern ) );
    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x101u, data, sizeof( data ) ) );     /* odd start, odd length */
    TEST_ASSERT_EQUAL_UINT8_ARRAY( &pattern[ 1 ], data, sizeof( data ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x102u, &single, 1u ) );               /* one byte: low half */
    TEST_ASSERT_EQUAL_HEX8( pattern[ 2 ], single );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x103u, &single, 1u ) );               /* one byte: high half */
    TEST_ASSERT_EQUAL_HEX8( pattern[ 3 ], single );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x100u, data, 4u ) );                  /* even start, even length */
    TEST_ASSERT_EQUAL_UINT8_ARRAY( &pattern[ 0 ], data, 4u );
}

void Ut_Flash_Get_Data_DataFlash_UnalignedRange_BytesFromHalfWords( void )
{
#if defined(FLASH_EDATAR_EDATA_EN)
    flash_AreaInfo_t area;
    flash_DataByte_t pattern[ 8 ];
    flash_DataByte_t data[ 5 ];

    Ut_Flash_Fill( pattern, sizeof( pattern ), 0x47u );
    Ut_Flash_Init_DataFlash( 0u, 2u );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, 2u, &area ) );
    (void)memcpy( (void *)(uintptr_t)area.Address, pattern, sizeof( pattern ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, area.Address + 1u, data, sizeof( data ) ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( &pattern[ 1 ], data, sizeof( data ) );
#else
    TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
#endif
}

void Ut_Flash_Get_BlankState_Otp_UnalignedRange_ByteOfTheHalfWordDecides( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;

    Ut_Flash_Init();

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x201u, 3u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    *(volatile uint8_t *)(uintptr_t)( UT_FLASH_OTP_ADDR + 0x200u ) = 0x00u;                 /* before the range: the same half word */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x201u, 3u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );                                     /* the byte 0x200 is not in the range */

    *(volatile uint8_t *)(uintptr_t)( UT_FLASH_OTP_ADDR + 0x203u ) = 0x00u;                 /* last byte of the range, high half */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 0x201u, 3u, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, blankState );
}

/* ====================== BLANK OTP WORDS FLAG THE ECC ====================== */

/* A read of a blank OTP word sets ECCD (Bug AB#1192): the resume that ends the read of the area sets the flag. */
void Ut_Flash_Get_BlankState_Otp_BlankWordsFlagEccd_BlankAndNotLatched( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;
    flash_EccState_t  eccState;

    Ut_Flash_Init();
    utFlash_CacheRecord.ResumeEccd = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | 0x200u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    FLASH_NS->ECCDETR = 0u;                                             /* the module cleared the flag (write 1 to clear) */
    utFlash_CacheRecord.ResumeEccd = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_EQUAL_UINT32( FLASH_ERR_NONE, eccState.Error );         /* not an error of the module */
}

void Ut_Flash_Get_BlankState_Otp_ProgrammedWordWithEccd_NotBlank( void )
{
    flash_FlagState_t blankState = FLASH_FLAG_ACTIVE;

    Ut_Flash_Init();
    *(volatile uint8_t *)(uintptr_t)( UT_FLASH_OTP_ADDR + 0x40u ) = 0x12u;
    utFlash_CacheRecord.ResumeEccd = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | 0x210u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_INACTIVE, blankState );               /* the data decide */
}

void Ut_Flash_Set_WriteStart_Otp_BlankWordsFlagEccd_Programmed( void )
{
    flash_DataByte_t data[ 6 ];
    flash_DataByte_t readBack[ 6 ];

    Ut_Flash_Fill( data, sizeof( data ), 0x61u );
    Ut_Flash_Init_Window( UT_FLASH_OTP_ADDR, UT_FLASH_OTP_SIZE );
    Ut_Flash_Unlock();
    utFlash_CacheRecord.ResumeEccd = FLASH_ECCR_ECCD | FLASH_ECCR_OTP_ECC | 0x201u;     /* the blank check of the destination flags the error */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, UT_FLASH_OTP_ADDR + 4u, data, sizeof( data ) ) );

    (void)memcpy( readBack, (const void *)(uintptr_t)( UT_FLASH_OTP_ADDR + 4u ), sizeof( readBack ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, sizeof( data ) );
}

void Ut_Flash_Handle_Nmi_ErrorOfBank2_AddressFromStoredBankSizeNoCacheHandling( void )
{
    Ut_Flash_Init_Callbacks();
    FLASH_NS->ECCDETR = FLASH_ECCR_ECCD | FLASH_ECCR_BK_ECC | 0x55u;

    Flash_Handle_Nmi();

    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_ErrorRecord.CallCnt );
    TEST_ASSERT_EQUAL_HEX32( 0x08000000u + ( UT_FLASH_SIZE / 2u ) + ( 0x55u * 16u ), utFlash_ErrorRecord.Address );
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.SuspendCnt );     /* the size was read by the initialization, not by the NMI */
    TEST_ASSERT_EQUAL_UINT32( 1u, utFlash_CacheRecord.ResumeCnt );
}

/* ========================== LOCAL FUNCTIONS =============================== */

/** Initializes the module with a configuration (flash clock enabled by the mocked RCC module) */
static void Ut_Flash_Init_Config( const flash_Config_t * const config )
{
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_FLASH, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( config ) );
}

/** Initializes the module with the default configuration and one access window (aligned to the sector, inside the user flash) */
static void Ut_Flash_Init_Window( flash_Address_t address, flash_Size_t size )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.WindowCount          = 1u;
    config.Windows[ 0 ].Address = address;
    config.Windows[ 0 ].Size    = size;

    Ut_Flash_Init_Config( &config );
}

/** Handler of the flash interrupt registered in the NVIC module mock */
static nvic_RequestState_t Ut_Flash_NvicSetHandlerStub( nvic_PeriphIrqList_t irqId, const nvic_IsrCallback_t irqHandler, int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( NVIC_PERIPH_IRQ_FLASH, irqId );

    utFlash_Isr = irqHandler;

    return ( NVIC_REQUEST_OK );
}

/** Suspend of the instruction cache (Cache module stub): counts the calls, models the flash size register */
static cache_RequestState_t Ut_Flash_CacheSuspendStub( cache_CacheId_t cacheId, int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( CACHE_INSTRUCTION, cacheId );

    utFlash_CacheRecord.SuspendCnt++;

    if ( ( CACHE_REQUEST_OK == utFlash_CacheRecord.SuspendResult ) &&
         ( 0u              != utFlash_CacheRecord.SizeKbSuspended )    )
    {
        UtFlashMem_Set_SizeKb( utFlash_CacheRecord.SizeKbSuspended );
    }

    return ( utFlash_CacheRecord.SuspendResult );
}

/** Resume of the instruction cache (Cache module stub): counts the calls, removes what is not readable with the cache */
static cache_RequestState_t Ut_Flash_CacheResumeStub( cache_CacheId_t cacheId, int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( CACHE_INSTRUCTION, cacheId );

    utFlash_CacheRecord.ResumeCnt++;

    TEST_ASSERT_LESS_OR_EQUAL_UINT32( utFlash_CacheRecord.SuspendCnt, utFlash_CacheRecord.ResumeCnt );

    if ( 0u != utFlash_CacheRecord.SizeKbSuspended )
    {
        UtFlashMem_Set_SizeKb( UT_FLASH_CACHE_POISON_KB );
    }

    if ( FLASH_NULL_PTR != utFlash_CacheRecord.ResumePoison )
    {
        *utFlash_CacheRecord.ResumePoison = 0u;
    }

    if ( 0u != utFlash_CacheRecord.ResumeEccd )
    {
        FLASH_NS->ECCDETR = utFlash_CacheRecord.ResumeEccd;     /* the read that ended with this resume flagged the error */
    }

    return ( CACHE_REQUEST_OK );
}

/** Invalidation of the instruction cache (Cache module stub): counts the calls */
static cache_RequestState_t Ut_Flash_CacheInvalidateStub( cache_CacheId_t cacheId, int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( CACHE_INSTRUCTION, cacheId );

    utFlash_CacheRecord.InvalidateCnt++;

    return ( CACHE_REQUEST_OK );
}

/** End of operation callback of the tests */
static void Ut_Flash_OperationCallback( flash_PeriphId_t periphId, flash_OperationType_t operation, flash_Address_t address, flash_Size_t size, flash_Error_t error )
{
    utFlash_OpRecord.CallCnt++;
    utFlash_OpRecord.PeriphId  = periphId;
    utFlash_OpRecord.Operation = operation;
    utFlash_OpRecord.Address   = address;
    utFlash_OpRecord.Size      = size;
    utFlash_OpRecord.Error     = error;
}

/** Initializes the module in a style with one access window and both callbacks (the NVIC calls of the interrupt style are expected) */
static void Ut_Flash_Init_Style( flash_OperationStyle_t style, flash_Address_t windowAddress, flash_Size_t windowSize )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.OperationStyle       = style;
    config.IrqPrio              = UT_FLASH_IRQ_PRIO;
    config.WindowCount          = 1u;
    config.Windows[ 0 ].Address = windowAddress;
    config.Windows[ 0 ].Size    = windowSize;
    config.OperationCallback    = Ut_Flash_OperationCallback;
    config.ErrorCallback        = Ut_Flash_ErrorCallback;

    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_FLASH, RCC_REQUEST_OK );        /* the mocks are called in this order */

    if ( FLASH_STYLE_INTERRUPT == style )
    {
        Nvic_Set_PeriphIrq_Prio_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, UT_FLASH_IRQ_PRIO, NVIC_REQUEST_OK );
        Nvic_Set_PeriphIrq_Active_ExpectAndReturn( NVIC_PERIPH_IRQ_FLASH, NVIC_REQUEST_OK );
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );
}

/** Error callback of the tests */
static void Ut_Flash_ErrorCallback( flash_PeriphId_t periphId, flash_Error_t error, flash_Address_t address )
{
    utFlash_ErrorRecord.CallCnt++;
    utFlash_ErrorRecord.PeriphId = periphId;
    utFlash_ErrorRecord.Error    = error;
    utFlash_ErrorRecord.Address  = address;
}

/** Initializes the module with the default configuration and the error callback */
static void Ut_Flash_Init_Callbacks( void )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.WindowCount   = 0u;
    config.ErrorCallback = Ut_Flash_ErrorCallback;

    Ut_Flash_Init_Config( &config );
}

/** Initializes the module with the default configuration without any access window */
static void Ut_Flash_Init_NoWindow( void )
{
    flash_Config_t config;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.WindowCount = 0u;

    Ut_Flash_Init_Config( &config );
}

/** Initializes the module with the default configuration and the access windows of the whole user flash of both banks */
static void Ut_Flash_Init( void )
{
    flash_Config_t config;
    uint32_t       bankSize = UT_FLASH_SIZE / 2u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.WindowCount          = 2u;
    config.Windows[ 0 ].Address = 0x08000000u;
    config.Windows[ 0 ].Size    = bankSize;
    config.Windows[ 1 ].Address = 0x08000000u + bankSize;
    config.Windows[ 1 ].Size    = bankSize;

    Ut_Flash_Init_Config( &config );
}

/** Emulates the unlock of the control register: the key sequence can not clear the LOCK bit of the plain memory */
static void Ut_Flash_Unlock( void )
{
    FLASH_NS->NSCR &= ~FLASH_CR_LOCK;
}

/** Fills a buffer with a pattern */
static void Ut_Flash_Fill( flash_DataByte_t * data, uint32_t dataLen, uint8_t seed )
{
    uint32_t byteId;

    for ( byteId = 0u; byteId < dataLen; byteId++ )
    {
        data[ byteId ] = (flash_DataByte_t)( seed + ( 7u * byteId ) );
    }
}

#if defined(FLASH_EDATAR_EDATA_EN)

/** Maximal count of the high-cycle data sectors of a bank of the device */
static uint32_t Ut_Flash_Get_DataSectorsMax( void )
{
    return ( ( (uint32_t)FLASH_EDATAR_EDATA_STRT >> FLASH_EDATAR_EDATA_STRT_Pos ) + 1u );
}

/** Presets the configuration of the high-cycle data flash (0 sectors = disabled) */
static void Ut_Flash_Set_DataSectors( uint32_t bank1Sectors, uint32_t bank2Sectors )
{
    FLASH_NS->EDATA1R_CUR = 0u;
    FLASH_NS->EDATA2R_CUR = 0u;

    if ( 0u < bank1Sectors )
    {
        FLASH_NS->EDATA1R_CUR = FLASH_EDATAR_EDATA_EN | ( bank1Sectors - 1u );
    }

    if ( 0u < bank2Sectors )
    {
        FLASH_NS->EDATA2R_CUR = FLASH_EDATAR_EDATA_EN | ( bank2Sectors - 1u );
    }
}

/** Presets the configuration of the data flash and initializes the module with the default configuration (windows = data areas) */
static void Ut_Flash_Init_DataFlash( uint32_t bank1Sectors, uint32_t bank2Sectors )
{
    flash_Config_t config;

    Ut_Flash_Set_DataSectors( bank1Sectors, bank2Sectors );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    Ut_Flash_Init_Config( &config );
}

/** Emulates the end of the option byte change: the controller loads the current option registers (RM0481 Rev 5, 7.4.3) */
static void Ut_Flash_Load_OptionBytes( void )
{
    FLASH_NS->EDATA1R_CUR = FLASH_NS->EDATA1R_PRG;
    FLASH_NS->EDATA2R_CUR = FLASH_NS->EDATA2R_PRG;
}

#endif /* FLASH_EDATAR_EDATA_EN */
