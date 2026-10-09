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

/** Size of the high-cycle data flash taken by the configuration test (two sectors of 6 KB) */
#define IT_FLASH_DATA_CONFIG_SIZE           ( 2u * 6u * 1024u )

/** Size of the OTP area */
#define IT_FLASH_OTP_SIZE                   ( 2048u )

/** Size of one read of the OTP area */
#define IT_FLASH_OTP_READ_SIZE              ( 16u )

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

            userEnd = area.Address + area.Size;
        }

        if ( FLASH_AREA_OTP == area.Type )
        {
            TEST_ASSERT_EQUAL_HEX32( IT_FLASH_OTP_BASE, area.Address );
            otpCnt++;
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

        if ( ( FLASH_AREA_USER == area.Type ) && ( lastArea.Address <= area.Address ) )
        {
            lastArea = area;
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
 * Configuration of the high-cycle data flash: two sectors of the bank 2 are taken from the user flash (the
 * option bytes are programmed, the new size is valid after the reset between the test cases). Ignored on the
 * lines without the high-cycle data flash and when it is already configured.
 */
void It_Flash_DataFlash_Configure_OptionBytesProgrammed( void )
{
    flash_Config_t      config;
    flash_ConfigState_t configState = FLASH_CONFIG_APPLIED;
    flash_AreaId_t      areaCount   = 0u;
    flash_AreaId_t      areaId;
    flash_AreaInfo_t    area;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    if ( FLASH_REQUEST_OK != Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) )
    {
        TEST_IGNORE_MESSAGE( "The device has no high-cycle data flash" );
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( FLASH_AREA_DATA == area.Type )
        {
            TEST_IGNORE_MESSAGE( "The high-cycle data flash is already configured" );
        }
    }

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, IT_FLASH_DATA_CONFIG_SIZE ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_ConfigState( FLASH_PERIPH_1, &configState ) );
    TEST_ASSERT_EQUAL( FLASH_CONFIG_PENDING, configState );
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

    dataArea.Address = 0u;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount ) );

    for ( areaId = 0u; areaId < areaCount; areaId++ )
    {
        TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area ) );

        if ( ( FLASH_AREA_DATA == area.Type ) && ( FLASH_FLAG_INACTIVE == found ) )
        {
            dataArea = area;
            found    = FLASH_FLAG_ACTIVE;
        }
    }

    if ( FLASH_FLAG_ACTIVE != found )
    {
        TEST_IGNORE_MESSAGE( "The high-cycle data flash is not configured" );
    }

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
 * Read of the whole OTP area does not raise the NMI: it ends with the data or with the reported ECC error (an
 * unprogrammed OTP area is not written by the test - the OTP write is irreversible).
 */
void It_Flash_Otp_Read_NoNmiResultReported( void )
{
    flash_Config_t   config;
    flash_DataByte_t data[ IT_FLASH_OTP_READ_SIZE ];
    flash_EccState_t eccState;
    uint32_t         offset;

    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Init( &config ) );

    for ( offset = 0u; offset < IT_FLASH_OTP_SIZE; offset += IT_FLASH_OTP_READ_SIZE )
    {
        if ( FLASH_REQUEST_OK != Flash_Get_Data( FLASH_PERIPH_1, IT_FLASH_OTP_BASE + offset, data, IT_FLASH_OTP_READ_SIZE ) )
        {
            TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Get_EccState( FLASH_PERIPH_1, &eccState ) );
            TEST_ASSERT_BITS_HIGH( FLASH_ERR_ECC_DETECTED, eccState.Error );
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
        }
    }

    TEST_ASSERT_EQUAL( expected, blankState );
    TEST_ASSERT_EQUAL( FLASH_REQUEST_OK, Flash_Deinit( FLASH_PERIPH_1 ) );
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
    }

    TEST_ASSERT_EQUAL_UINT32( dataCnt, config.WindowCount );
}
