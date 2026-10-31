/**
 * \author Mr.Nobody
 * \file ItTest_Flash.c
 * \ingroup Flash
 * \brief Integration tests of the internal flash memory controller (FLASH) module on target.
 *
 * Flash module runs on the target with real dependent modules (no mocks). Every test case runs after
 * system reset of the target. Target specific code (entry point, initialization, fault handlers) is
 * in ItTarget_Flash.c.
 *
 * The tests of the memory map read the flash size register and the configuration of the high-cycle
 * data flash of the real device without the initialization of the module (open point: the registers
 * have to be readable after the reset). The tests of the operations follow the implementation of the
 * operations. The OTP area is never written (irreversible).
 *
 * The tests of the high-cycle data flash set up the layout they need themselves (the option bytes left by an
 * earlier run do not matter): the configuration is removed, the user sectors to be taken are erased, the data
 * flash is configured and tested, the run ends with the configuration removed (default layout on the board).
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "IntegrationTesting.h"             /* Integration testing on target  */
#include "Flash_Port.h"                     /* Module under test              */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Start of the user flash */
#define IT_FLASH_USER_BASE                  ( 0x08000000u )

/** Address of the OTP area */
#define IT_FLASH_OTP_BASE                   ( 0x08FFF000u )

/** Minimal count of the areas (user flash of both banks and OTP) */
#define IT_FLASH_AREA_CNT_MIN               ( 3u )

/** Size of the test data (four flash words) */
#define IT_FLASH_TEST_SIZE                  ( 64u )

/** Count of the sectors of the bank 2 taken by the configuration test */
#define IT_FLASH_DATA_CONFIG_SECTORS        ( 2u )

/** Size of the high-cycle data flash taken by the configuration test (sectors of 6 KB) */
#define IT_FLASH_DATA_CONFIG_SIZE           ( IT_FLASH_DATA_CONFIG_SECTORS * 6u * 1024u )

/** Size of the OTP area */
#define IT_FLASH_OTP_SIZE                   ( 2048u )

/** Size of one read of the OTP area */
#define IT_FLASH_OTP_READ_SIZE              ( 16u )

/* ======================= FORWARD DECLARATIONS ============================= */

static void     ItFlash_Get_LastUserArea( flash_AreaInfo_t * const lastArea );
static uint32_t ItFlash_Get_DataAreaCnt ( void );

/* ============================ TEST FIXTURE ================================ */

void setUp( void )
{
}

void tearDown( void )
{
}

/* ============================== TESTS ===================================== */

/** The list of the areas of the real device has the user flash of both banks and the OTP area. */
void It_Flash_Get_AreaInfo_RealDevice_UserBanksAndOtp( void )
{
    flash_AreaId_t   areaCount = 0u;
    flash_AreaId_t   areaId;
    flash_AreaInfo_t area;
    flash_Address_t  userEnd = IT_FLASH_USER_BASE;
    uint32_t         otpCnt  = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8( IT_FLASH_AREA_CNT_MIN, areaCount );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );
        TEST_ASSERT_GREATER_THAN_UINT32( 0u, area.Size );
        TEST_ASSERT_GREATER_THAN_UINT32( 0u, area.ProgramSize );

        if ( FLASH_AREA_USER == area.Type )
        {
            TEST_ASSERT_EQUAL_UINT32( 0x2000u, area.EraseSize );
            TEST_ASSERT_EQUAL_UINT32( 16u, area.ProgramSize );

            if ( 0u == area.Bank )
            {
                TEST_ASSERT_EQUAL_HEX32( IT_FLASH_USER_BASE, area.Address );
            }
            else
            {
                /* No action required */
            }

            userEnd = area.Address + area.Size;
        }
        else
        {
            /* No action required */
        }

        if ( FLASH_AREA_OTP == area.Type )
        {
            TEST_ASSERT_EQUAL_HEX32( IT_FLASH_OTP_BASE, area.Address );
            otpCnt++;
        }
        else
        {
            /* No action required */
        }
    }

    TEST_ASSERT_EQUAL_UINT32( 1u, otpCnt );
    TEST_ASSERT_LESS_OR_EQUAL_HEX32( IT_FLASH_OTP_BASE, userEnd );
}

/**
 * Erase, write, read and blank check of the last sector of the user flash (the firmware of the test
 * is at the start of the flash), which is the only access window. The blocking style, the control
 * register is locked afterwards.
 */
void It_Flash_EraseWriteRead_LastUserSector_DataVerified( void )
{
    flash_Config_t   config;
    flash_AreaId_t   areaCount = 0u;
    flash_AreaId_t   areaId;
    flash_AreaInfo_t area;
    flash_AreaInfo_t lastArea;
    flash_Address_t  sectorAddr;
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;
    flash_DataByte_t data[ IT_FLASH_TEST_SIZE ];
    flash_DataByte_t readBack[ IT_FLASH_TEST_SIZE ];
    uint32_t         byteId;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    lastArea.Type        = FLASH_AREA_USER;
    lastArea.Bank        = 0u;
    lastArea.Address     = 0u;
    lastArea.Size        = 0u;
    lastArea.EraseSize   = 0u;
    lastArea.ProgramSize = 0u;
    lastArea.Endurance   = 0u;
    lastArea.Attributes  = FLASH_AREA_ATTR_NONE;

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( ( FLASH_AREA_USER      == area.Type     ) &&
             ( lastArea.Address <= area.Address ) )
        {
            lastArea = area;
        }
        else
        {
            /* No action required */
        }
    }

    TEST_ASSERT_GREATER_THAN_UINT32( 0u, lastArea.Size );
    sectorAddr = lastArea.Address + lastArea.Size - lastArea.EraseSize;

    for ( byteId = 0u; byteId < IT_FLASH_TEST_SIZE; byteId++ )
    {
        data[ byteId ] = (flash_DataByte_t)( 0x5Au + ( 3u * byteId ) );
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.Windows[ config.WindowCount ].Address = sectorAddr;      /* only the tested sector can be erased and written */
    config.Windows[ config.WindowCount ].Size    = lastArea.EraseSize;
    config.WindowCount++;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_EraseStart( FLASH_PERIPH_1, sectorAddr - lastArea.EraseSize, lastArea.EraseSize ) );   /* outside of the window */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, sectorAddr, lastArea.EraseSize ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, sectorAddr, lastArea.EraseSize, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, sectorAddr, data, IT_FLASH_TEST_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, sectorAddr, readBack, IT_FLASH_TEST_SIZE ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, IT_FLASH_TEST_SIZE );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Set_WriteStart( FLASH_PERIPH_1, sectorAddr, data, IT_FLASH_TEST_SIZE ) );   /* not erased */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, sectorAddr, lastArea.EraseSize ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, sectorAddr, lastArea.EraseSize, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * Preparation of the configuration test: a configuration of the high-cycle data flash left on the board (by an
 * earlier run) is removed, the default layout is valid after the reset between the test cases. The option bytes
 * are programmed only when the data flash is configured. Ignored on the lines without the high-cycle data flash.
 */
void It_Flash_DataFlash_Remove_DefaultLayoutProgrammed( void )
{
    flash_Config_t       config;
    flash_ConfigState_t  configState   = FLASH_CONFIG_PENDING;
    flash_ConfigState_t  expectedState = FLASH_CONFIG_APPLIED;
    flash_RequestState_t cfgRequest    = FLASH_REQUEST_ERROR;
    uint32_t             dataCnt;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    cfgRequest = Flash_Get_ConfigState( FLASH_PERIPH_1, &configState );

    if ( FLASH_REQUEST_OK != cfgRequest )
    {
        TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
    }
    else
    {
        /* No action required */
    }

    dataCnt = ItFlash_Get_DataAreaCnt();

    if ( 0u < dataCnt )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 0u ) );
        expectedState = FLASH_CONFIG_PENDING;
    }
    else
    {
        /* The default layout is valid already, the option bytes are not programmed */
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( expectedState, configState );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * Configuration of the high-cycle data flash: two sectors of the bank 2 are taken from the user flash (the
 * option bytes are programmed, the new size is valid after the reset between the test cases). The default
 * layout is set up by It_Flash_DataFlash_Remove_DefaultLayoutProgrammed, the user sectors to be taken are erased
 * first (an earlier configuration leaves its data in them). Ignored on the lines without the high-cycle data flash.
 */
void It_Flash_DataFlash_Configure_OptionBytesProgrammed( void )
{
    flash_Config_t       config;
    flash_ConfigState_t  configState = FLASH_CONFIG_APPLIED;
    flash_RequestState_t cfgRequest  = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     lastArea;
    flash_Address_t      takenAddr;
    flash_Size_t         takenSize;
    flash_FlagState_t    blankState  = FLASH_FLAG_INACTIVE;
    uint32_t             dataCnt;

    ItFlash_Get_LastUserArea( &lastArea );

    takenSize = IT_FLASH_DATA_CONFIG_SECTORS * lastArea.EraseSize;
    takenAddr = lastArea.Address + lastArea.Size - takenSize;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );

    config.Windows[ config.WindowCount ].Address = takenAddr;      /* only the user sectors to be taken can be erased */
    config.Windows[ config.WindowCount ].Size    = takenSize;
    config.WindowCount++;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    cfgRequest = Flash_Get_ConfigState( FLASH_PERIPH_1, &configState );

    if ( FLASH_REQUEST_OK != cfgRequest )
    {
        TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
    }
    else
    {
        /* No action required */
    }

    dataCnt = ItFlash_Get_DataAreaCnt();

    TEST_ASSERT_EQUAL_UINT32( 0u, dataCnt );                        /* removed by the previous test case */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, takenAddr, takenSize ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, takenAddr, takenSize, &blankState ) );
    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, blankState );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, IT_FLASH_DATA_CONFIG_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_PENDING, configState );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * Erase, program (16 bit) and read of the first data area: only what was programmed is read back ("program before
 * read"). Ignored while the high-cycle data flash is not configured (run the configuration test, the reset between
 * the test cases loads the option bytes). The window of the data flash has to be non-cacheable.
 */
void It_Flash_DataFlash_EraseProgramRead_DataVerified( void )
{
    flash_Config_t   config;
    flash_AreaId_t   areaCount = 0u;
    flash_AreaId_t   areaId;
    flash_AreaInfo_t area;
    flash_AreaInfo_t dataArea;
    flash_DataByte_t data[ IT_FLASH_TEST_SIZE ];
    flash_DataByte_t readBack[ IT_FLASH_TEST_SIZE ];
    uint32_t         byteId;
    flash_FlagState_t found = FLASH_FLAG_INACTIVE;
    flash_ConfigState_t  configState = FLASH_CONFIG_PENDING;
    flash_RequestState_t cfgRequest  = FLASH_REQUEST_ERROR;

    dataArea.Address = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    cfgRequest = Flash_Get_ConfigState( FLASH_PERIPH_1, &configState );

    if ( FLASH_REQUEST_OK != cfgRequest )
    {
        TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
    }
    else
    {
        /* No action required */
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( ( FLASH_AREA_DATA     == area.Type ) &&
             ( FLASH_FLAG_INACTIVE == found     ) )
        {
            dataArea = area;
            found    = FLASH_FLAG_ACTIVE;
        }
        else
        {
            /* No action required */
        }
    }

    TEST_ASSERT_EQUAL( FLASH_FLAG_ACTIVE, found );                  /* configured by the previous test case */

    for ( byteId = 0u; byteId < IT_FLASH_TEST_SIZE; byteId++ )
    {
        data[ byteId ] = (flash_DataByte_t)( 0xA5u + ( 5u * byteId ) );
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EraseStart( FLASH_PERIPH_1, dataArea.Address, dataArea.EraseSize ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_WriteStart( FLASH_PERIPH_1, dataArea.Address, data, IT_FLASH_TEST_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_Data( FLASH_PERIPH_1, dataArea.Address, readBack, IT_FLASH_TEST_SIZE ) );
    TEST_ASSERT_EQUAL_UINT8_ARRAY( data, readBack, IT_FLASH_TEST_SIZE );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_BlankState( FLASH_PERIPH_1, dataArea.Address, dataArea.EraseSize, &found ) );   /* an erased location can not be read */

    /* An erased and not programmed location reports the ECC error instead of the NMI (the behaviour described by ST). */
    TEST_ASSERT_EQUAL( FLASH_REQUEST_ERROR, Flash_Get_Data( FLASH_PERIPH_1, dataArea.Address + ( 2u * IT_FLASH_TEST_SIZE ), readBack, 2u ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EccStateClear( FLASH_PERIPH_1 ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * End of the tests of the high-cycle data flash: the configuration is removed, so the run leaves the default
 * layout on the board (valid after the reset). Ignored on the lines without the high-cycle data flash.
 */
void It_Flash_DataFlash_RemoveAfterTests_DefaultLayoutPending( void )
{
    flash_Config_t       config;
    flash_ConfigState_t  configState = FLASH_CONFIG_APPLIED;
    flash_RequestState_t cfgRequest  = FLASH_REQUEST_ERROR;
    uint32_t             dataCnt;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    cfgRequest = Flash_Get_ConfigState( FLASH_PERIPH_1, &configState );

    if ( FLASH_REQUEST_OK != cfgRequest )
    {
        TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
    }
    else
    {
        /* No action required */
    }

    dataCnt = ItFlash_Get_DataAreaCnt();

    TEST_ASSERT_GREATER_THAN_UINT32( 0u, dataCnt );                 /* configured by the configuration test case */

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 0u ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_PENDING, configState );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * Read of the whole OTP area does not raise the NMI: it ends with the data or with the reported ECC error (an
 * unprogrammed OTP area is not written by the test - the OTP write is irreversible).
 */
void It_Flash_Otp_Read_NoNmiResultReported( void )
{
    flash_Config_t       config;
    flash_DataByte_t     data[ IT_FLASH_OTP_READ_SIZE ];
    flash_EccState_t     eccState;
    flash_RequestState_t readRequest = FLASH_REQUEST_ERROR;
    uint32_t             offset;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    for ( offset = 0u; offset < IT_FLASH_OTP_SIZE; offset += IT_FLASH_OTP_READ_SIZE )
    {
        readRequest = Flash_Get_Data( FLASH_PERIPH_1, IT_FLASH_OTP_BASE + offset, data, IT_FLASH_OTP_READ_SIZE );

        if ( FLASH_REQUEST_OK != readRequest )
        {
            TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
            TEST_ASSERT_BITS_HIGH( FLASH_ERR_ECC_DETECTED, eccState.Error );
        }
        else
        {
            /* No action required */
        }
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * The blank check of the OTP agrees with the content read by Flash_Get_Data, and the error flag that a read of a
 * blank OTP word raises is not latched as an ECC error of the module (Bug AB#1192).
 */
void It_Flash_Otp_BlankCheck_ConsistentWithTheContentNoEccLatched( void )
{
    flash_Config_t    config;
    flash_FlagState_t blankState = FLASH_FLAG_INACTIVE;
    flash_FlagState_t expected   = FLASH_FLAG_ACTIVE;
    flash_EccState_t  eccState;
    flash_DataByte_t  data[ IT_FLASH_OTP_READ_SIZE ];
    uint32_t          offset;
    uint32_t          byteId;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_BlankState( FLASH_PERIPH_1, IT_FLASH_OTP_BASE, IT_FLASH_OTP_SIZE, &blankState ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
    TEST_ASSERT_BITS_LOW( FLASH_ERR_ECC_DETECTED, eccState.Error );     /* the blank words flagged the error, the module did not latch it */

    for ( offset = 0u; offset < IT_FLASH_OTP_SIZE; offset += IT_FLASH_OTP_READ_SIZE )
    {
        (void)Flash_Get_Data( FLASH_PERIPH_1, IT_FLASH_OTP_BASE + offset, data, IT_FLASH_OTP_READ_SIZE );    /* a blank word reads as 0xFF with an ECC error */

        for ( byteId = 0u; byteId < IT_FLASH_OTP_READ_SIZE; byteId++ )
        {
            if ( FLASH_ERASED_VALUE != data[ byteId ] )
            {
                expected = FLASH_FLAG_INACTIVE;
            }
            else
            {
                /* No action required */
            }
        }
    }

    TEST_ASSERT_EQUAL( expected, blankState );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/**
 * The uncorrectable ECC error of a read of a blank OTP word reports the address of a word of the read range (Bug
 * AB#1191: STM32H503 reports the OTP area from 0x0200, not from the 0x0600 of RM0492 and the ST HAL). Ignored when
 * no read of the OTP area reports the error (the whole area is programmed).
 */
void It_Flash_Otp_ReadBlankWord_EccAddressInTheReadRange( void )
{
    flash_Config_t       config;
    flash_DataByte_t     data[ IT_FLASH_OTP_READ_SIZE ];
    flash_EccState_t     eccState;
    flash_RequestState_t readState;
    uint32_t             offset;
    uint32_t             errorCnt = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    for ( offset = 0u; offset < IT_FLASH_OTP_SIZE; offset += IT_FLASH_OTP_READ_SIZE )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EccStateClear( FLASH_PERIPH_1 ) );

        readState = Flash_Get_Data( FLASH_PERIPH_1, IT_FLASH_OTP_BASE + offset, data, IT_FLASH_OTP_READ_SIZE );

        if ( FLASH_REQUEST_OK != readState )
        {
            TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
            TEST_ASSERT_BITS_HIGH( FLASH_ERR_ECC_DETECTED, eccState.Error );
            TEST_ASSERT_GREATER_OR_EQUAL_HEX32( IT_FLASH_OTP_BASE + offset, eccState.Address );
            TEST_ASSERT_LESS_THAN_HEX32( IT_FLASH_OTP_BASE + offset + IT_FLASH_OTP_READ_SIZE, eccState.Address );
            errorCnt++;
        }
        else
        {
            /* Programmed words, no ECC error */
        }
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_EccStateClear( FLASH_PERIPH_1 ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );

    if ( 0u == errorCnt )
    {
        TEST_IGNORE_MESSAGE( "No read of the OTP area reports the ECC error (the whole area is programmed)" );
    }
    else
    {
        /* The address of every reported error was checked */
    }
}

/** The default configuration has a window for every data area that is enabled in the option bytes. */
void It_Flash_Get_DefaultConfig_RealDevice_WindowPerDataArea( void )
{
    flash_Config_t   config;
    flash_AreaId_t   areaCount = 0u;
    flash_AreaId_t   areaId;
    flash_AreaInfo_t area;
    uint32_t         dataCnt = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( FLASH_AREA_DATA == area.Type )
        {
            TEST_ASSERT_EQUAL_HEX32( area.Address, config.Windows[ dataCnt ].Address );
            TEST_ASSERT_EQUAL_UINT32( area.Size, config.Windows[ dataCnt ].Size );
            dataCnt++;
        }
        else
        {
            /* No action required */
        }
    }

    TEST_ASSERT_EQUAL_UINT32( dataCnt, config.WindowCount );
}

/**
 * Reads the TrustZone state of the real device (read-only, no option byte is read or written by this
 * test). The secure watermark sectors are checked to be a valid range only when TrustZone is active.
 */
void It_Flash_Get_TrustZoneState_RealDevice_StateReadable( void )
{
    flash_Config_t         config;
    flash_TrustZoneState_t state;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_TrustZoneState( &state ) );
    /* No invariant is assumed of the secure watermark, TzState is one of the two defined values by its type */
    TEST_ASSERT_TRUE( ( FLASH_FUNCTION_ACTIVE == state.TzState ) ||
                       ( FLASH_FUNCTION_INACTIVE == state.TzState ) );

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * Finds the user flash area with the highest address (the end of the bank 2).
 *
 * \param lastArea [out]: Description of the area
 */
static void ItFlash_Get_LastUserArea( flash_AreaInfo_t * const lastArea )
{
    flash_AreaId_t   areaCount = 0u;
    flash_AreaId_t   areaId;
    flash_AreaInfo_t area;

    lastArea->Type        = FLASH_AREA_USER;
    lastArea->Bank        = 0u;
    lastArea->Address     = 0u;
    lastArea->Size        = 0u;
    lastArea->EraseSize   = 0u;
    lastArea->ProgramSize = 0u;
    lastArea->Endurance   = 0u;
    lastArea->Attributes  = FLASH_AREA_ATTR_NONE;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( FLASH_AREA_USER != area.Type )
        {
            /* Not a user flash area */
        }
        else if ( lastArea->Address <= area.Address )
        {
            *lastArea = area;
        }
        else
        {
            /* User flash area of a lower address */
        }
    }

    TEST_ASSERT_GREATER_THAN_UINT32( 0u, lastArea->Size );
}

/**
 * Counts the areas of the high-cycle data flash in the memory map (the configuration of the option bytes).
 *
 * \return Count of the data areas
 */
static uint32_t ItFlash_Get_DataAreaCnt( void )
{
    flash_AreaId_t   areaCount = 0u;
    flash_AreaId_t   areaId;
    flash_AreaInfo_t area;
    uint32_t         dataCnt   = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( FLASH_AREA_DATA == area.Type )
        {
            dataCnt++;
        }
        else
        {
            /* No action required */
        }
    }

    return (dataCnt);
}
