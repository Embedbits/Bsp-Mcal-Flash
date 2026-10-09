/**
 * \author Mr.Nobody
 * \file Flash.c
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) MCAL module common functionality
 *
 * Public interface of the module for STM32H5 (the same interface as of the other families, see
 * the README). The memory map (areas) is in Flash_Map.c, the register level functionality in
 * Flash_Hw.c.
 *
 * Implemented: initialization, read (the NMI of an uncorrectable ECC error is masked), blank check, erase and
 * write of the user flash and of the high-cycle data flash, write of the OTP area, the blocking, polling and
 * interrupt style, ECC state, configuration of the high-cycle data flash and the state of the operation. The
 * control register of the flash is unlocked only for the time of an operation.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Cache_Port.h"                     /* Instruction cache handling     */
#include "Flash.h"                          /* Self include                   */
#include "Flash_Hw.h"                       /* Register level functionality   */
#include "Flash_Port.h"                     /* Own port file include          */
#include "Flash_Types.h"                    /* Module types definitions       */
#include "Nvic_Port.h"                      /* NVIC handler functionality     */
#include "Rcc_Port.h"                       /* RCC handler functionality      */
#include "Stm32.h"                          /* STM32 peripheral registers     */
/* ============================= TYPEDEFS =================================== */

/** Operation (erase or write) that is running */
typedef struct
{
    flash_OperationType_t    Type;    /**< Type of the operation                           */
    flash_AreaInfo_t         Area;    /**< Area of the operation                           */
    flash_Address_t          Address; /**< Start address                                   */
    flash_Size_t             Size;    /**< Size in bytes                                   */
    const flash_DataByte_t * Data;    /**< Data of a write (valid until the end)           */
    flash_Size_t             UnitCnt; /**< Count of the erase / program units              */
    flash_Size_t             UnitId;  /**< Unit in progress                                */
}   flash_Operation_t;


/** Context of the module */
typedef struct
{
    flash_FlagState_t      Initialized;    /**< Module is initialized                    */
    flash_Config_t         Config;         /**< Configuration given to \ref Flash_Init   */
    flash_OperationState_t OperationState; /**< State of the last operation              */
    flash_Error_t          OperationError; /**< Error mask of the last operation         */
    flash_EccState_t       Ecc;            /**< ECC errors latched from the controller   */
    flash_Operation_t      Operation;      /**< Operation in progress                    */
}   flash_Context_t;

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Value of major version of SW module */
#define FLASH_MAJOR_VERSION         ( 1u )

/** Value of minor version of SW module */
#define FLASH_MINOR_VERSION         ( 0u )

/** Value of patch version of SW module */
#define FLASH_PATCH_VERSION         ( 0u )

/** Default priority of the flash interrupt */
#define FLASH_DEFAULT_IRQ_PRIORITY  ( 0u )

/** Number of bits of a byte */
#define FLASH_BITS_PER_BYTE         ( 8u )

/** Number of bytes of a 32-bit word */
#define FLASH_BYTES_PER_WORD        ( 4u )

/* ========================== LOCAL VARIABLES =============================== */

/** Context of the module */
static flash_Context_t flash_Context =
{
    .Initialized    = FLASH_FLAG_INACTIVE,
    .OperationState = FLASH_STATE_IDLE,
    .OperationError = FLASH_ERR_NONE,
    .Ecc            = { FLASH_ERR_NONE, 0u }
};

/* ======================= FORWARD DECLARATIONS ============================= */

static flash_RequestState_t Flash_Check_PeriphId    ( flash_PeriphId_t periphId );
static flash_RequestState_t Flash_Check_Initialized ( flash_PeriphId_t periphId );
static flash_RequestState_t Flash_Check_Request     ( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size, flash_OperationType_t operation, flash_AreaInfo_t * const areaInfo );
static flash_RequestState_t Flash_Check_Config      ( const flash_Config_t * const flashConfig );
static flash_RequestState_t Flash_Check_Windows     ( const flash_Config_t * const flashConfig );
static flash_RequestState_t Flash_Check_Access      ( flash_Address_t address, flash_Size_t size );
static flash_RequestState_t Flash_Is_Blank          ( flash_AreaType_t areaType, flash_Address_t address, flash_Size_t size, flash_FlagState_t * const blankState );
static flash_RequestState_t Flash_Suspend_Cache     ( flash_AreaType_t areaType, flash_FlagState_t * const suspended );
static void                 Flash_Resume_Cache      ( flash_FlagState_t suspended );
static flash_DataByte_t     Flash_Read_Byte         ( flash_AreaType_t areaType, flash_Address_t address );
static flash_RequestState_t Flash_Get_DataDistribution ( flash_Size_t size, flash_Size_t * const sectorCnt );
static flash_Error_t        Flash_Run_DataConfig    ( const flash_Size_t * const sectorCnt );
static flash_Error_t        Flash_Program_DataConfig( const flash_Size_t * const sectorCnt );
static flash_ConfigState_t  Flash_Get_ConfigPending ( void );
static flash_Error_t        Flash_Write_Unit        ( const flash_AreaInfo_t * const areaInfo, flash_Address_t address, const flash_DataByte_t * const data );
static flash_Error_t        Flash_Write_QuadWord    ( flash_Address_t address, const flash_DataByte_t * const data );
static flash_Error_t        Flash_Write_HalfWord    ( flash_Address_t address, const flash_DataByte_t * const data );
static void                 Flash_Set_Result        ( flash_Error_t error );
static flash_Error_t        Flash_Latch_Ecc         ( void );
static flash_Error_t        Flash_Run_Operation     ( flash_OperationType_t operation, const flash_AreaInfo_t * const areaInfo, flash_Address_t address, const flash_DataByte_t * const data, flash_Size_t size );
static flash_Error_t        Flash_Run_Blocking      ( void );
static flash_Error_t        Flash_Start_Unit        ( void );
static void                 Flash_Service           ( void );
static void                 Flash_Service_Ecc       ( void );
static void                 Flash_Finish            ( flash_Error_t error );
static void                 Flash_Irq_Handler       ( void );
static flash_RequestState_t Flash_Init_Interrupt    ( flash_IrqPrio_t irqPrio );
static flash_RequestState_t Flash_Deinit_Interrupt  ( void );

/* ======================== EXPORTED FUNCTIONS ============================== */

/**
 * \brief Returns the version of the module.
 *
 * \return Module SW version
 */
flash_ModuleVersion_t Flash_Get_ModuleVersion( void )
{
    flash_ModuleVersion_t retVersion;

    retVersion.Major = FLASH_MAJOR_VERSION;
    retVersion.Minor = FLASH_MINOR_VERSION;
    retVersion.Patch = FLASH_PATCH_VERSION;

    return (retVersion);
}


/**
 * \brief Initialization of the module.
 *
 * Enables the flash interface clock (RCC module), locks the control register and clears the flags.
 * In the interrupt style the flash interrupt is registered, prioritized and enabled through the NVIC
 * module and the interrupt of the corrected ECC error is enabled; in the polling style the module is
 * served by \ref Flash_Task.
 *
 * Every access window of the configuration has to lie inside one area and to be aligned to the
 * erase unit of that area (to the program unit if the area can not be erased). Erase and write are
 * accepted only inside one access window.
 *
 * \param flashConfig [in]: Pointer to the configuration structure
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR (invalid configuration or access
 *         window, the module is already initialized, the clock can not be enabled).
 */
flash_RequestState_t Flash_Init( const flash_Config_t * const flashConfig )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    rcc_RequestState_t   rccState;
    flash_Size_t         bankSize;

    retState = Flash_Check_Config( flashConfig );

    if ( FLASH_REQUEST_OK == retState )
    {
        retState = Flash_Check_Windows( flashConfig );
    }

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_FLAG_INACTIVE != flash_Context.Initialized ) )
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        bankSize = Flash_Get_BankSize();                /* kept for the NMI, where the register can not be read */

        if ( 0u == bankSize )
        {
            retState = FLASH_REQUEST_ERROR;             /* the cache can not be suspended */
        }
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        rccState = Rcc_Set_PeriphActive( RCC_PERIPH_FLASH );

        if ( RCC_REQUEST_OK != rccState )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        Flash_Hw_Lock();
        Flash_Hw_Clear_Flags();

        flash_Context.Config         = *flashConfig;
        flash_Context.OperationState = FLASH_STATE_IDLE;
        flash_Context.OperationError = FLASH_ERR_NONE;
        flash_Context.Ecc.Error      = FLASH_ERR_NONE;
        flash_Context.Ecc.Address    = 0u;
        flash_Context.Initialized    = FLASH_FLAG_ACTIVE;

        if ( FLASH_STYLE_INTERRUPT == flashConfig->OperationStyle )
        {
            retState = Flash_Init_Interrupt( flashConfig->IrqPrio );

            if ( FLASH_REQUEST_OK != retState )
            {
                flash_Context.Initialized = FLASH_FLAG_INACTIVE;
            }
        }
    }

    return (retState);
}


/**
 * \brief Deinitialization of the module.
 *
 * Locks the control register. The flash interface clock stays enabled (it is owned by the RCC
 * module, the code is executed from the flash).
 *
 * \param periphId: Controller
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR (not initialized, operation running).
 */
flash_RequestState_t Flash_Deinit( flash_PeriphId_t periphId )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_STATE_BUSY == flash_Context.OperationState ) )
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        if ( FLASH_STYLE_INTERRUPT == flash_Context.Config.OperationStyle )
        {
            retState = Flash_Deinit_Interrupt();
        }

        Flash_Hw_Lock();

        flash_Context.Initialized    = FLASH_FLAG_INACTIVE;
        flash_Context.OperationState = FLASH_STATE_IDLE;
        flash_Context.OperationError = FLASH_ERR_NONE;
    }

    if ( FLASH_FLAG_INACTIVE == flash_Context.Initialized )
    {
        Flash_Reset_BankSize();                         /* the geometry of the device is read again by the next use */
    }

    return (retState);
}


/**
 * \brief Serves the operation started in polling style and reports the ECC errors by the error callback.
 *
 * Does nothing in the other styles. Every call moves a running operation by one erase / program unit
 * when the controller has finished the previous one; the end of the operation is reported by the
 * operation callback.
 *
 * \note Not implemented yet.
 */
void Flash_Task( void )
{
    if (    ( FLASH_FLAG_ACTIVE == flash_Context.Initialized )
         && ( FLASH_STYLE_POLLING == flash_Context.Config.OperationStyle ) )
    {
        Flash_Service();
        Flash_Service_Ecc();
    }
}


/**
 * \brief Initialization of the configuration structure to default values.
 *
 * Default configuration: blocking operations, interrupt priority 0, no callbacks, one access
 * window for every area of the high-cycle data flash that exists (none if the device has none
 * or it is not enabled in the option bytes).
 *
 * \param flashConfig [out]: Pointer to the configuration structure
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 */
flash_RequestState_t Flash_Get_DefaultConfig( flash_Config_t * const flashConfig )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaList[ FLASH_AREA_CNT_MAX ];
    flash_AreaId_t       areaCnt;
    flash_AreaId_t       areaId;
    uint8_t              windowId;

    if ( FLASH_NULL_PTR != flashConfig )
    {
        flashConfig->PeriphId          = FLASH_PERIPH_1;
        flashConfig->OperationStyle    = FLASH_STYLE_BLOCKING;
        flashConfig->IrqPrio           = FLASH_DEFAULT_IRQ_PRIORITY;
        flashConfig->WindowCount       = 0u;
        flashConfig->OperationCallback = FLASH_NULL_PTR;
        flashConfig->ErrorCallback     = FLASH_NULL_PTR;

        for ( windowId = 0u; windowId < FLASH_WINDOW_MAX; windowId++ )
        {
            flashConfig->Windows[ windowId ].Address = 0u;
            flashConfig->Windows[ windowId ].Size    = 0u;
        }

        areaCnt = Flash_Build_AreaList( areaList );

        for ( areaId = 0u; areaId < areaCnt; areaId++ )
        {
            if ( ( FLASH_AREA_DATA == areaList[ areaId ].Type ) && ( FLASH_WINDOW_MAX > flashConfig->WindowCount ) )
            {
                windowId = flashConfig->WindowCount;

                flashConfig->Windows[ windowId ].Address = areaList[ areaId ].Address;
                flashConfig->Windows[ windowId ].Size    = areaList[ areaId ].Size;
                flashConfig->WindowCount++;
            }
        }

        retState = FLASH_REQUEST_OK;
    }

    return (retState);
}


/**
 * \brief Reads the state of the module.
 *
 * \param periphId: Controller
 * \param periphState [out]: \ref FLASH_FLAG_ACTIVE if the module is initialized
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 */
flash_RequestState_t Flash_Get_PeriphState( flash_PeriphId_t periphId, flash_FlagState_t * const periphState )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_PeriphId( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != periphState ) )
    {
        *periphState = flash_Context.Initialized;
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Reads the count of the flash areas of the device.
 *
 * The areas are described by \ref Flash_Get_AreaInfo. The list reflects the actual configuration
 * of the device (the high-cycle data flash is in the list only if it is enabled).
 *
 * \param periphId: Controller
 * \param areaCount [out]: Count of the areas
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 */
flash_RequestState_t Flash_Get_AreaCount( flash_PeriphId_t periphId, flash_AreaId_t * const areaCount )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaList[ FLASH_AREA_CNT_MAX ];

    retState = Flash_Check_PeriphId( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != areaCount ) )
    {
        *areaCount = Flash_Build_AreaList( areaList );
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Reads the description of a flash area.
 *
 * \param periphId: Controller
 * \param areaId: Index in the list of the areas (0 .. count - 1)
 * \param areaInfo [out]: Description of the area
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 */
flash_RequestState_t Flash_Get_AreaInfo( flash_PeriphId_t periphId, flash_AreaId_t areaId, flash_AreaInfo_t * const areaInfo )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaList[ FLASH_AREA_CNT_MAX ];
    flash_AreaId_t       areaCnt;

    retState = Flash_Check_PeriphId( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != areaInfo ) )
    {
        areaCnt = Flash_Build_AreaList( areaList );

        if ( areaId < areaCnt )
        {
            *areaInfo = areaList[ areaId ];
        }
        else
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Reads data of the flash with the check of the address and of the ECC.
 *
 * \param periphId: Controller
 * \param address: Address in an area
 * \param data [out]: Buffer for the data
 * \param size: Count of the bytes
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note The range has to lie in one area. The NMI of an uncorrectable ECC error is masked (SBS) for the
 *       time of the read: the error is reported by \ref FLASH_REQUEST_ERROR and latched in the ECC state
 *       (\ref Flash_Get_EccState, \ref FLASH_ERR_ECC_DETECTED), the content of the buffer is not valid then.
 *       A corrected error does not fail the read (it is latched as \ref FLASH_ERR_ECC_CORRECTED).
 */
flash_RequestState_t Flash_Get_Data( flash_PeriphId_t periphId, flash_Address_t address, flash_DataByte_t * const data, flash_Size_t size )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaInfo;
    flash_Size_t         byteId;
    flash_Error_t        eccError;
    flash_FlagState_t    suspended = FLASH_FLAG_INACTIVE;
    uint32_t             nmiMask;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != data ) )
    {
        retState = Flash_Find_Area( address, size, &areaInfo );
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        retState = Flash_Suspend_Cache( areaInfo.Type, &suspended );
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        (void)Flash_Latch_Ecc();                        /* errors of the past are kept, the flags are free for the read */

        nmiMask = Flash_Hw_Mask_EccNmi();

        for ( byteId = 0u; byteId < size; byteId++ )
        {
            data[ byteId ] = Flash_Read_Byte( areaInfo.Type, address + byteId );
        }

        Flash_Resume_Cache( suspended );

        eccError = Flash_Latch_Ecc();

        Flash_Hw_Restore_EccNmi( nmiMask );

        if ( 0u != ( eccError & FLASH_ERR_ECC_DETECTED ) )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Checks that a range of the flash is blank (erased).
 *
 * \param periphId: Controller
 * \param address: Start address of the range
 * \param size: Size of the range in bytes
 * \param blankState [out]: \ref FLASH_FLAG_ACTIVE if the whole range is erased
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note The range has to lie in one area.
 */
flash_RequestState_t Flash_Get_BlankState( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size, flash_FlagState_t * const blankState )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaInfo;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != blankState ) )
    {
        retState = Flash_Find_Area( address, size, &areaInfo );
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( ( FLASH_REQUEST_OK == retState ) && ( 0u != ( areaInfo.Attributes & FLASH_AREA_ATTR_PROGRAM_BEFORE_READ ) ) )
    {
        retState = FLASH_REQUEST_ERROR;                 /* an erased location of the area can not be read */
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        retState = Flash_Is_Blank( areaInfo.Type, address, size, blankState );
    }

    return (retState);
}


/**
 * \brief Starts an erase of whole erase units.
 *
 * \param periphId: Controller
 * \param address: Start address (aligned to the erase unit of the area, inside an access window)
 * \param size: Size in bytes (multiple of the erase unit)
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note User flash and data flash (OTP can not be erased). In the blocking style the function returns when
 *       the operation is finished, in the polling and the interrupt style it returns when the operation is
 *       started and the end is reported by the operation callback (\ref Flash_Task / the interrupt). The
 *       range has to lie
 *       inside one access window, otherwise the request fails with \ref FLASH_ERR_PROTECTION and no
 *       register is touched. The result of the operation is readable by \ref Flash_Get_OperationState and
 *       \ref Flash_Get_OperationError.
 */
flash_RequestState_t Flash_Set_EraseStart( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaInfo;
    flash_Error_t        error;

    retState = Flash_Check_Request( periphId, address, size, FLASH_OPERATION_ERASE, &areaInfo );

    if ( FLASH_REQUEST_OK == retState )
    {
        retState = Flash_Check_Access( address, size );
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        error = Flash_Run_Operation( FLASH_OPERATION_ERASE, &areaInfo, address, FLASH_NULL_PTR, size );

        if ( FLASH_ERR_NONE != error )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Starts a write of whole program units.
 *
 * \param periphId: Controller
 * \param address: Start address (aligned to the program unit of the area, inside an access window)
 * \param data [in]: Data to write
 * \param size: Size in bytes (multiple of the program unit)
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note User flash, data flash and OTP (16-bit programming, write once: OTP is never erased, it has to be
 *       in an access window of the configuration, the lock of the OTP blocks is not touched). In the blocking
 *       style the function returns when the operation is finished, in the polling and the interrupt style it
 *       returns when the operation is started and the end is reported by the operation callback (\ref Flash_Task
 *       / the interrupt). The data of a write have to stay valid until the end. The range has to lie inside one
 *       access window, otherwise the request fails with \ref FLASH_ERR_PROTECTION. A destination in the user
 *       flash or in the OTP that is not erased is refused (\ref FLASH_ERR_NOT_ERASED) before any register is
 *       touched.
 */
flash_RequestState_t Flash_Set_WriteStart( flash_PeriphId_t periphId, flash_Address_t address, const flash_DataByte_t * const data, flash_Size_t size )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_AreaInfo_t     areaInfo;
    flash_FlagState_t    blankState;
    flash_Error_t        error;

    retState = Flash_Check_Request( periphId, address, size, FLASH_OPERATION_WRITE, &areaInfo );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR == data ) )
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        retState = Flash_Check_Access( address, size );
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        if ( 0u != ( areaInfo.Attributes & FLASH_AREA_ATTR_PROGRAM_BEFORE_READ ) )
        {
            blankState = FLASH_FLAG_ACTIVE;             /* an erased location of the area can not be read: not checked */
        }
        else
        {
            blankState = FLASH_FLAG_INACTIVE;           /* not blank if the range can not be read */
            (void)Flash_Is_Blank( areaInfo.Type, address, size, &blankState );
        }

        if ( FLASH_FLAG_ACTIVE == blankState )
        {
            error = Flash_Run_Operation( FLASH_OPERATION_WRITE, &areaInfo, address, data, size );
        }
        else
        {
            error = FLASH_ERR_NOT_ERASED;
            Flash_Set_Result( error );
        }

        if ( FLASH_ERR_NONE != error )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Reads the state of the operation.
 *
 * \param periphId: Controller
 * \param operationState [out]: State of the operation
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note Blocking style: the state of the last finished operation (the polling and the interrupt
 *       style follow AB#1038).
 */
flash_RequestState_t Flash_Get_OperationState( flash_PeriphId_t periphId, flash_OperationState_t * const operationState )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != operationState ) )
    {
        *operationState = flash_Context.OperationState;
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Reads the error mask of the last operation.
 *
 * \param periphId: Controller
 * \param operationError [out]: Error mask
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note The mask of the last finished operation, \ref FLASH_ERR_NONE after a successful one.
 */
flash_RequestState_t Flash_Get_OperationError( flash_PeriphId_t periphId, flash_Error_t * const operationError )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != operationError ) )
    {
        *operationError = flash_Context.OperationError;
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Reads the state of the ECC (last error and its address).
 *
 * \param periphId: Controller
 * \param eccState [out]: ECC state
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note The errors flagged by the controller are latched (and the flags cleared) by this function, by
 *       \ref Flash_Get_Data, by \ref Flash_Handle_Nmi and by the flash interrupt; the state is kept
 *       until \ref Flash_Set_EccStateClear. The address is the address of the last detected error
 *       (of the last corrected one if there is no detected error), 0 outside of the areas of the module.
 */
flash_RequestState_t Flash_Get_EccState( flash_PeriphId_t periphId, flash_EccState_t * const eccState )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_NULL_PTR != eccState ) )
    {
        (void)Flash_Latch_Ecc();

        *eccState = flash_Context.Ecc;
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Clears the state of the ECC.
 *
 * \param periphId: Controller
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note Clears the latched state and the ECC flags of the controller.
 */
flash_RequestState_t Flash_Set_EccStateClear( flash_PeriphId_t periphId )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_Initialized( periphId );

    if ( FLASH_REQUEST_OK == retState )
    {
        Flash_Hw_Clear_Ecc( FLASH_ERR_ECC_CORRECTED | FLASH_ERR_ECC_DETECTED );

        flash_Context.Ecc.Error   = FLASH_ERR_NONE;
        flash_Context.Ecc.Address = 0u;
    }

    return (retState);
}


/**
 * \brief Hook of the NMI handler: handles the uncorrectable ECC error raised as NMI.
 *
 * Clears the flag of the error (otherwise the NMI would come again), latches the error and its address
 * (\ref Flash_Get_EccState) and calls the error callback of the configuration. It has to be called by the
 * NMI handler of the application; it does nothing if no uncorrectable ECC error is flagged.
 */
void Flash_Handle_Nmi( void )
{
    flash_Error_t   eccError;
    flash_Address_t eccAddress = 0u;

    eccError = Flash_Hw_Read_Ecc( Flash_Get_BankSizeStored(), &eccAddress );    /* no cache handling in the NMI */

    if ( 0u != ( eccError & FLASH_ERR_ECC_DETECTED ) )
    {
        Flash_Hw_Clear_Ecc( FLASH_ERR_ECC_DETECTED );

        if ( FLASH_FLAG_ACTIVE == flash_Context.Initialized )
        {
            flash_Context.Ecc.Error  |= FLASH_ERR_ECC_DETECTED;
            flash_Context.Ecc.Address = eccAddress;

            if ( FLASH_NULL_PTR != flash_Context.Config.ErrorCallback )
            {
                flash_Context.Config.ErrorCallback( flash_Context.Config.PeriphId, FLASH_ERR_ECC_DETECTED, eccAddress );
            }
        }
    }
}


/**
 * \brief Requests a new size of a configurable area (high-cycle data flash).
 *
 * \param periphId: Controller
 * \param areaType: Type of the area
 * \param size: Requested usable size in bytes (multiple of the erase unit of the area)
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * The sectors are distributed over the banks - the bank 2 (the end of the flash) first. The sectors
 * taken from the user flash have to be blank (otherwise \ref FLASH_ERR_NOT_ERASED), the data of the
 * sectors given back to the user flash are lost. The option bytes are programmed, the new size is
 * valid after the reset (\ref Flash_Set_ConfigApply, \ref Flash_Get_ConfigState).
 *
 * \note Only the high-cycle data flash can be configured, only on the lines that have it.
 */
flash_RequestState_t Flash_Set_AreaSize( flash_PeriphId_t periphId, flash_AreaType_t areaType, flash_Size_t size )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_Size_t         sectorCnt[ FLASH_BANK_CNT ];
    flash_Error_t        error;

    retState = Flash_Check_Initialized( periphId );

    if (    ( FLASH_REQUEST_OK == retState )
         && (    ( FLASH_AREA_DATA == areaType )
              && ( FLASH_STATE_BUSY != flash_Context.OperationState ) ) )
    {
        retState = Flash_Get_DataDistribution( size, sectorCnt );
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        flash_Context.OperationState = FLASH_STATE_BUSY;
        flash_Context.OperationError = FLASH_ERR_NONE;

        error = Flash_Run_DataConfig( sectorCnt );

        Flash_Set_Result( error );

        if ( FLASH_ERR_NONE != error )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Reads the state of the configuration of the configurable areas.
 *
 * \param periphId: Controller
 * \param configState [out]: Configuration state
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note The state is \ref FLASH_CONFIG_PENDING if the option bytes to be programmed differ from the
 *       valid ones. Returns an error on the lines without a configurable area.
 */
flash_RequestState_t Flash_Get_ConfigState( flash_PeriphId_t periphId, flash_ConfigState_t * const configState )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_Size_t         maxCnt   = Flash_Get_DataSectorsMax();

    retState = Flash_Check_Initialized( periphId );

    if (    ( FLASH_REQUEST_OK == retState )
         && ( FLASH_NULL_PTR != configState )
         && ( 0u < maxCnt ) )
    {
        *configState = Flash_Get_ConfigPending();
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Applies the new configuration of the configurable areas (reset of the device).
 *
 * \param periphId: Controller
 *
 * \return State of request execution. Returns \ref FLASH_REQUEST_OK if request was
 *         success, otherwise returns \ref FLASH_REQUEST_ERROR.
 *
 * \note Resets the system through the NVIC module (does not return on the target). Returns an error
 *       if there is no pending configuration, an operation runs or the line has no configurable area.
 */
flash_RequestState_t Flash_Set_ConfigApply( flash_PeriphId_t periphId )
{
    flash_RequestState_t retState    = FLASH_REQUEST_ERROR;
    flash_Size_t         maxCnt      = Flash_Get_DataSectorsMax();
    flash_ConfigState_t  configState = Flash_Get_ConfigPending();

    retState = Flash_Check_Initialized( periphId );

    if (    ( FLASH_REQUEST_OK == retState )
         && ( FLASH_STATE_BUSY != flash_Context.OperationState )
         && ( 0u < maxCnt )
         && ( FLASH_CONFIG_PENDING == configState ) )
    {
        Nvic_Set_SystemReset();
    }
    else
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * \brief Distributes a size of the high-cycle data flash over the banks (the bank 2 first).
 *
 * \param size: Requested usable size in bytes
 * \param sectorCnt [out]: Count of the sectors of every bank
 *
 * \return \ref FLASH_REQUEST_OK if the size is a multiple of the erase unit and fits to the banks
 */
static flash_RequestState_t Flash_Get_DataDistribution( flash_Size_t size, flash_Size_t * const sectorCnt )
{
    flash_RequestState_t retState  = FLASH_REQUEST_ERROR;
    flash_Size_t         eraseSize = Flash_Get_DataEraseSize();
    flash_Size_t         maxCnt    = Flash_Get_DataSectorsMax();
    flash_Size_t         totalCnt;

    if ( 0u < eraseSize )
    {
        totalCnt = size / eraseSize;

        if (    ( 0u == ( size % eraseSize ) )
             && ( totalCnt <= ( FLASH_BANK_CNT * maxCnt ) ) )
        {
            sectorCnt[ 1 ] = totalCnt;

            if ( sectorCnt[ 1 ] > maxCnt )
            {
                sectorCnt[ 1 ] = maxCnt;
            }

            sectorCnt[ 0 ] = totalCnt - sectorCnt[ 1 ];
            retState       = FLASH_REQUEST_OK;
        }
    }

    return (retState);
}


/**
 * \brief Programs the number of the high-cycle data sectors of the banks to the option bytes (blocking).
 *
 * The sectors taken from the user flash have to be blank. The flash and the option bytes are
 * unlocked for the time of the programming only.
 *
 * \param sectorCnt [in]: Requested count of the sectors of every bank
 *
 * \return Error mask, \ref FLASH_ERR_NONE if the option bytes were programmed
 */
static flash_Error_t Flash_Run_DataConfig( const flash_Size_t * const sectorCnt )
{
    flash_Error_t        error    = FLASH_ERR_NONE;
    flash_FlagState_t    blankState;
    flash_Size_t         currentCnt;
    flash_Size_t         bankSize = Flash_Get_BankSize();
    flash_Address_t      takenAddr;
    flash_BankId_t       bankId;

    for ( bankId = 0u; ( bankId < FLASH_BANK_CNT ) && ( FLASH_ERR_NONE == error ); bankId++ )
    {
        currentCnt = Flash_Get_DataSectors( bankId );

        if ( sectorCnt[ bankId ] > currentCnt )
        {
            takenAddr  = (flash_Address_t)FLASH_BASE_NS + ( (flash_Size_t)bankId * bankSize ) + ( bankSize - ( sectorCnt[ bankId ] * FLASH_SECTOR_SIZE ) );
            blankState = FLASH_FLAG_INACTIVE;
            (void)Flash_Is_Blank( FLASH_AREA_USER, takenAddr, ( sectorCnt[ bankId ] - currentCnt ) * FLASH_SECTOR_SIZE, &blankState );

            if ( FLASH_FLAG_ACTIVE != blankState )
            {
                error = FLASH_ERR_NOT_ERASED;
            }
        }
    }

    if ( FLASH_ERR_NONE == error )
    {
        error = Flash_Program_DataConfig( sectorCnt );
    }

    return (error);
}


/**
 * \brief Programs the option bytes of the high-cycle data flash (the flash and the option bytes are
 *        unlocked for the time of the programming only).
 *
 * \param sectorCnt [in]: Requested count of the sectors of every bank
 *
 * \return Error mask, \ref FLASH_ERR_NONE if the option bytes were programmed
 */
static flash_Error_t Flash_Program_DataConfig( const flash_Size_t * const sectorCnt )
{
    flash_Error_t        error = FLASH_ERR_NONE;
    flash_RequestState_t hwState;
    flash_BankId_t       bankId;

    Flash_Hw_Clear_Flags();
    error = Flash_Hw_Wait_Ready();

    if ( FLASH_ERR_NONE == error )
    {
        hwState = Flash_Hw_Unlock();

        if ( FLASH_REQUEST_OK == hwState )
        {
            hwState = Flash_Hw_Unlock_Options();
        }

        if ( FLASH_REQUEST_OK != hwState )
        {
            error = FLASH_ERR_PROTECTION;
        }
    }

    if ( FLASH_ERR_NONE == error )
    {
        for ( bankId = 0u; ( bankId < FLASH_BANK_CNT ) && ( FLASH_ERR_NONE == error ); bankId++ )
        {
            hwState = Flash_Hw_Set_DataSectors( bankId, sectorCnt[ bankId ] );

            if ( FLASH_REQUEST_OK != hwState )
            {
                error = FLASH_ERR_SEQUENCE;
            }
        }
    }

    if ( FLASH_ERR_NONE == error )
    {
        Flash_Hw_Start_OptionProgram();
        error = Flash_Hw_Wait_Ready();
    }

    Flash_Hw_Lock_Options();
    Flash_Hw_Lock();

    return (error);
}


/**
 * \brief Compares the option bytes of the high-cycle data flash to be programmed with the valid ones.
 *
 * \return \ref FLASH_CONFIG_PENDING if the configuration of a bank differs
 */
static flash_ConfigState_t Flash_Get_ConfigPending( void )
{
    flash_ConfigState_t configState = FLASH_CONFIG_APPLIED;
    flash_BankId_t      bankId;
    flash_Size_t        validCnt;
    flash_Size_t        programmedCnt;

    for ( bankId = 0u; bankId < FLASH_BANK_CNT; bankId++ )
    {
        validCnt      = Flash_Get_DataSectors( bankId );
        programmedCnt = Flash_Hw_Get_DataSectors( bankId );

        if ( validCnt != programmedCnt )
        {
            configState = FLASH_CONFIG_PENDING;
        }
    }

    return (configState);
}


/**
 * \brief Checks the controller identification.
 *
 * \param periphId: Controller
 *
 * \return \ref FLASH_REQUEST_OK if the controller exists, otherwise \ref FLASH_REQUEST_ERROR.
 */
static flash_RequestState_t Flash_Check_PeriphId( flash_PeriphId_t periphId )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    if ( FLASH_PERIPH_CNT > periphId )
    {
        retState = FLASH_REQUEST_OK;
    }

    return (retState);
}


/**
 * \brief Checks the controller identification and that the module is initialized.
 *
 * \param periphId: Controller
 *
 * \return \ref FLASH_REQUEST_OK if the controller exists and the module is initialized
 */
static flash_RequestState_t Flash_Check_Initialized( flash_PeriphId_t periphId )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    retState = Flash_Check_PeriphId( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_FLAG_ACTIVE != flash_Context.Initialized ) )
    {
        retState = FLASH_REQUEST_ERROR;
    }

    return (retState);
}


/**
 * \brief Checks the configuration given to \ref Flash_Init.
 *
 * \param flashConfig [in]: Configuration
 *
 * \return \ref FLASH_REQUEST_OK if the configuration is valid
 */
static flash_RequestState_t Flash_Check_Config( const flash_Config_t * const flashConfig )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    if ( FLASH_NULL_PTR != flashConfig )
    {
        if (    ( FLASH_PERIPH_CNT > flashConfig->PeriphId )
             && ( FLASH_STYLE_INTERRUPT >= flashConfig->OperationStyle )
             && ( FLASH_WINDOW_MAX >= flashConfig->WindowCount ) )
        {
            retState = FLASH_REQUEST_OK;
        }
    }

    return (retState);
}


/**
 * \brief Checks the access windows of the configuration given to \ref Flash_Init.
 *
 * A window has to lie inside one area and to be aligned (address and size) to the erase unit of the
 * area, to the program unit if the area can not be erased.
 *
 * \param flashConfig [in]: Configuration
 *
 * \return \ref FLASH_REQUEST_OK if all windows are valid
 */
static flash_RequestState_t Flash_Check_Windows( const flash_Config_t * const flashConfig )
{
    flash_RequestState_t retState = FLASH_REQUEST_OK;
    flash_AreaInfo_t     areaInfo;
    flash_Size_t         unitSize;
    uint8_t              windowId;

    for ( windowId = 0u; ( windowId < flashConfig->WindowCount ) && ( FLASH_REQUEST_OK == retState ); windowId++ )
    {
        retState = Flash_Find_Area( flashConfig->Windows[ windowId ].Address, flashConfig->Windows[ windowId ].Size, &areaInfo );

        if ( FLASH_REQUEST_OK == retState )
        {
            unitSize = areaInfo.EraseSize;

            if ( 0u == unitSize )
            {
                unitSize = areaInfo.ProgramSize;
            }

            if (    ( 0u == unitSize )
                 || ( 0u != ( ( flashConfig->Windows[ windowId ].Address - areaInfo.Address ) % unitSize ) )
                 || ( 0u != ( flashConfig->Windows[ windowId ].Size % unitSize ) ) )
            {
                retState = FLASH_REQUEST_ERROR;
            }
        }
    }

    return (retState);
}


/**
 * \brief Checks that a range lies inside one access window.
 *
 * A range outside of the windows is refused: the operation fails with \ref FLASH_ERR_PROTECTION and
 * no register is touched.
 *
 * \param address: Start address of the range
 * \param size: Size of the range in bytes (at least 1)
 *
 * \return \ref FLASH_REQUEST_OK if the whole range lies inside one window
 */
static flash_RequestState_t Flash_Check_Access( flash_Address_t address, flash_Size_t size )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    uint8_t              windowId;

    for ( windowId = 0u; windowId < flash_Context.Config.WindowCount; windowId++ )
    {
        if (    ( address >= flash_Context.Config.Windows[ windowId ].Address )
             && ( size <= flash_Context.Config.Windows[ windowId ].Size )
             && ( ( address - flash_Context.Config.Windows[ windowId ].Address ) <= ( flash_Context.Config.Windows[ windowId ].Size - size ) ) )
        {
            retState = FLASH_REQUEST_OK;
        }
    }

    if ( FLASH_REQUEST_OK != retState )
    {
        Flash_Set_Result( FLASH_ERR_PROTECTION );
    }

    return (retState);
}


/**
 * \brief Checks a request of an erase or a write: module state, area, type of the area and alignment.
 *
 * \param periphId: Controller
 * \param address: Start address
 * \param size: Size in bytes
 * \param operation: Erase or write
 * \param areaInfo [out]: Area that contains the range
 *
 * \return \ref FLASH_REQUEST_OK if the request can be executed
 */
static flash_RequestState_t Flash_Check_Request( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size, flash_OperationType_t operation, flash_AreaInfo_t * const areaInfo )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    flash_Size_t         unitSize = 0u;

    retState = Flash_Check_Initialized( periphId );

    if ( ( FLASH_REQUEST_OK == retState ) && ( FLASH_STATE_BUSY == flash_Context.OperationState ) )
    {
        retState = FLASH_REQUEST_ERROR;
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        retState = Flash_Find_Area( address, size, areaInfo );
    }

    if ( FLASH_REQUEST_OK == retState )
    {
        if ( FLASH_OPERATION_ERASE == operation )
        {
            unitSize = areaInfo->EraseSize;
        }
        else
        {
            unitSize = areaInfo->ProgramSize;
        }

        if (    ( 0u == unitSize )
             || ( 0u != ( ( address - areaInfo->Address ) % unitSize ) )
             || ( 0u != ( size % unitSize ) ) )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Checks that a range of the memory is erased.
 *
 * The NMI of an uncorrectable ECC error is masked for the time of the check; such an error means that the
 * range is not blank (the error is latched in the ECC state).
 *
 * \param areaType: Type of the area that contains the range
 * \param address: Start address
 * \param size: Size in bytes
 * \param blankState [out]: \ref FLASH_FLAG_ACTIVE if every byte of the range has the erased value
 *
 * \return \ref FLASH_REQUEST_OK if the range was read, \ref FLASH_REQUEST_ERROR if it can not be read (the
 *         instruction cache can not be suspended for the OTP area)
 */
static flash_RequestState_t Flash_Is_Blank( flash_AreaType_t areaType, flash_Address_t address, flash_Size_t size, flash_FlagState_t * const blankState )
{
    flash_RequestState_t              retState;
    flash_FlagState_t                 isBlank   = FLASH_FLAG_ACTIVE;
    flash_FlagState_t                 suspended = FLASH_FLAG_INACTIVE;
    flash_Size_t                      byteId    = 0u;
    flash_DataByte_t                  byteValue;
    flash_Error_t                     eccError;
    uint32_t                          nmiMask;

    retState = Flash_Suspend_Cache( areaType, &suspended );

    if ( FLASH_REQUEST_OK == retState )
    {
        (void)Flash_Latch_Ecc();                        /* errors of the past are kept, the flags are free for the read */

        nmiMask = Flash_Hw_Mask_EccNmi();

        while ( ( byteId < size                ) &&
                ( FLASH_FLAG_ACTIVE == isBlank )    )
        {
            byteValue = Flash_Read_Byte( areaType, address + byteId );

            if ( FLASH_ERASED_VALUE != byteValue )
            {
                isBlank = FLASH_FLAG_INACTIVE;
            }

            byteId++;
        }

        Flash_Resume_Cache( suspended );

        if ( FLASH_AREA_OTP == areaType )
        {
            Flash_Hw_Clear_Ecc( FLASH_ERR_ECC_DETECTED );   /* a blank OTP word flags the double error when it is read (Bug AB#1192): the data decide */
            eccError = FLASH_ERR_NONE;
        }
        else
        {
            eccError = Flash_Latch_Ecc();
        }

        Flash_Hw_Restore_EccNmi( nmiMask );

        if ( 0u != ( eccError & FLASH_ERR_ECC_DETECTED ) )
        {
            isBlank = FLASH_FLAG_INACTIVE;              /* an uncorrectable error: the range is not usable as blank */
        }

        *blankState = isBlank;
    }

    return (retState);
}


/**
 * \brief Suspends the instruction cache for the read of an area that the cache can not serve.
 *
 * With the instruction cache enabled a CPU read of the OTP area and of the high-cycle data flash raises a
 * bus fault (Bug AB#1176). The user flash is served by the cache.
 *
 * \param areaType: Type of the area that is read
 * \param suspended [out]: \ref FLASH_FLAG_ACTIVE if the cache was suspended (\ref Flash_Resume_Cache is needed)
 *
 * \return \ref FLASH_REQUEST_OK if the area can be read, \ref FLASH_REQUEST_ERROR if the cache can not be suspended
 */
static flash_RequestState_t Flash_Suspend_Cache( flash_AreaType_t areaType, flash_FlagState_t * const suspended )
{
    flash_RequestState_t retState = FLASH_REQUEST_OK;
    cache_RequestState_t cacheState;

    *suspended = FLASH_FLAG_INACTIVE;

    if ( FLASH_AREA_USER != areaType )
    {
        cacheState = Cache_Set_Suspend( CACHE_INSTRUCTION );

        if ( CACHE_REQUEST_OK == cacheState )
        {
            *suspended = FLASH_FLAG_ACTIVE;
        }
        else
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Resumes the instruction cache suspended by \ref Flash_Suspend_Cache.
 *
 * \param suspended: \ref FLASH_FLAG_ACTIVE if the cache was suspended
 */
static void Flash_Resume_Cache( flash_FlagState_t suspended )
{
    if ( FLASH_FLAG_ACTIVE == suspended )
    {
        (void)Cache_Set_Resume( CACHE_INSTRUCTION );
    }
}


/**
 * \brief Reads one byte of an area.
 *
 * The OTP area and the high-cycle data flash do not accept an 8-bit access (precise bus fault, Bug AB#1190,
 * measured on STM32H503): the byte is taken from the 16-bit word that holds it (little endian). The user
 * flash is read by bytes.
 *
 * \param areaType: Type of the area that holds the byte
 * \param address: Address of the byte
 *
 * \return Value of the byte
 */
static flash_DataByte_t Flash_Read_Byte( flash_AreaType_t areaType, flash_Address_t address )
{
    flash_DataByte_t value;
    uint16_t         halfWord;

    if ( FLASH_AREA_USER == areaType )
    {
        value = *(const volatile flash_DataByte_t *)(uintptr_t)address;
    }
    else
    {
        halfWord = *(const volatile uint16_t *)(uintptr_t)( address & ~(flash_Address_t)1u );

        if ( 0u != ( address & 1u ) )
        {
            value = (flash_DataByte_t)( halfWord >> FLASH_BITS_PER_BYTE );
        }
        else
        {
            value = (flash_DataByte_t)halfWord;
        }
    }

    return (value);
}


/**
 * \brief Starts an erase or a write and, in the blocking style, runs it to the end.
 *
 * Clears the flags, unlocks the control register (it is locked again when the operation finishes), enables the
 * interrupts in the interrupt style and starts the first erase / program unit.
 *
 * \param operation: Erase or write
 * \param areaInfo [in]: Area that contains the range
 * \param address: Start address (aligned to the unit of the area)
 * \param data [in]: Data of a write
 * \param size: Size in bytes (multiple of the unit of the area)
 *
 * \return Error mask of the start (polling and interrupt style) or of the whole operation (blocking style),
 *         \ref FLASH_ERR_NONE if there is no error
 */
static flash_Error_t Flash_Run_Operation( flash_OperationType_t operation, const flash_AreaInfo_t * const areaInfo, flash_Address_t address, const flash_DataByte_t * const data, flash_Size_t size )
{
    flash_Error_t        error;
    flash_RequestState_t hwState;
    flash_Size_t         unitSize;

    if ( FLASH_OPERATION_ERASE == operation )
    {
        unitSize = areaInfo->EraseSize;
    }
    else
    {
        unitSize = areaInfo->ProgramSize;
    }

    flash_Context.Operation.Type    = operation;
    flash_Context.Operation.Area    = *areaInfo;
    flash_Context.Operation.Address = address;
    flash_Context.Operation.Size    = size;
    flash_Context.Operation.Data    = data;
    flash_Context.Operation.UnitCnt = size / unitSize;
    flash_Context.Operation.UnitId  = 0u;

    flash_Context.OperationState = FLASH_STATE_BUSY;
    flash_Context.OperationError = FLASH_ERR_NONE;

    Flash_Hw_Clear_Flags();
    error = Flash_Hw_Wait_Ready();

    if ( FLASH_ERR_NONE == error )
    {
        hwState = Flash_Hw_Unlock();

        if ( FLASH_REQUEST_OK != hwState )
        {
            error = FLASH_ERR_PROTECTION;
        }
    }

    if ( ( FLASH_ERR_NONE == error ) && ( FLASH_OPERATION_WRITE == operation ) )
    {
        hwState = Flash_Hw_Start_Program();

        if ( FLASH_REQUEST_OK != hwState )
        {
            error = FLASH_ERR_SEQUENCE;
        }
    }

    if ( FLASH_ERR_NONE == error )
    {
        if ( FLASH_STYLE_INTERRUPT == flash_Context.Config.OperationStyle )
        {
            Flash_Hw_Set_Interrupts( FLASH_FUNCTION_ACTIVE );
        }

        error = Flash_Start_Unit();
    }

    if ( FLASH_ERR_NONE != error )
    {
        Flash_Finish( error );
    }
    else if ( FLASH_STYLE_BLOCKING == flash_Context.Config.OperationStyle )
    {
        error = Flash_Run_Blocking();
    }
    else
    {
        /* started: the operation is served by Flash_Task() or by the interrupt */
    }

    return (error);
}


/**
 * \brief Serves the running operation until it is finished (blocking style).
 *
 * The timeout is counted for every erase / program unit separately.
 *
 * \return Error mask of the operation, \ref FLASH_ERR_NONE if it succeeded
 */
static flash_Error_t Flash_Run_Blocking( void )
{
    flash_TimeoutCnt_t timeoutCnt = FLASH_HW_BUSY_TIMEOUT_CNT;
    flash_Size_t       unitId     = flash_Context.Operation.UnitId;

    while ( FLASH_STATE_BUSY == flash_Context.OperationState )
    {
        Flash_Service();

        if ( FLASH_STATE_BUSY == flash_Context.OperationState )
        {
            if ( unitId != flash_Context.Operation.UnitId )
            {
                unitId     = flash_Context.Operation.UnitId;
                timeoutCnt = FLASH_HW_BUSY_TIMEOUT_CNT;
            }
            else if ( 0u < timeoutCnt )
            {
                timeoutCnt--;
            }
            else
            {
                Flash_Finish( FLASH_ERR_TIMEOUT );
            }
        }
    }

    return (flash_Context.OperationError);
}


/**
 * \brief Starts the erase / program unit in progress of the running operation.
 *
 * \return Error mask of the start, \ref FLASH_ERR_NONE if the unit was started
 */
static flash_Error_t Flash_Start_Unit( void )
{
    flash_Error_t        error = FLASH_ERR_NONE;
    flash_RequestState_t hwState;
    flash_Size_t         offset;

    if ( FLASH_OPERATION_ERASE == flash_Context.Operation.Type )
    {
        offset  = flash_Context.Operation.UnitId * flash_Context.Operation.Area.EraseSize;
        hwState = Flash_Hw_Start_SectorErase( flash_Context.Operation.Area.Bank,
                                              Flash_Get_SectorId( &flash_Context.Operation.Area, flash_Context.Operation.Address + offset ) );

        if ( FLASH_REQUEST_OK != hwState )
        {
            error = FLASH_ERR_SEQUENCE;
        }
    }
    else
    {
        offset = flash_Context.Operation.UnitId * flash_Context.Operation.Area.ProgramSize;
        error  = Flash_Write_Unit( &flash_Context.Operation.Area, flash_Context.Operation.Address + offset, &flash_Context.Operation.Data[ offset ] );
    }

    return (error);
}


/**
 * \brief Moves the running operation: reads the result of the finished unit and starts the next one or finishes.
 *
 * Does nothing if no operation runs or the controller is busy. The operation callback is called when the
 * operation is finished in the polling or the interrupt style.
 */
static void Flash_Service( void )
{
    flash_Error_t         error     = FLASH_ERR_NONE;
    flash_FlagState_t     busyState;
    flash_FlagState_t     finished = FLASH_FLAG_INACTIVE;
    flash_OperationType_t operation = FLASH_OPERATION_NONE;

    if ( FLASH_STATE_BUSY == flash_Context.OperationState )
    {
        busyState = Flash_Hw_Get_BusyState();

        if ( FLASH_FLAG_ACTIVE != busyState )
        {
            error     = Flash_Hw_Get_Result();
            finished  = FLASH_FLAG_ACTIVE;
            operation = flash_Context.Operation.Type;

            if ( FLASH_ERR_NONE == error )
            {
                flash_Context.Operation.UnitId++;

                if ( flash_Context.Operation.UnitId < flash_Context.Operation.UnitCnt )
                {
                    error = Flash_Start_Unit();

                    if ( FLASH_ERR_NONE == error )
                    {
                        finished = FLASH_FLAG_INACTIVE;
                    }
                }
            }

            if ( FLASH_FLAG_ACTIVE == finished )
            {
                Flash_Finish( error );

                if (    ( FLASH_STYLE_BLOCKING != flash_Context.Config.OperationStyle )
                     && ( FLASH_NULL_PTR != flash_Context.Config.OperationCallback ) )
                {
                    flash_Context.Config.OperationCallback( flash_Context.Config.PeriphId, operation, flash_Context.Operation.Address, flash_Context.Operation.Size, error );
                }
            }
        }
    }
}


/**
 * \brief Reads the ECC flags and reports the new errors by the error callback (polling and interrupt style).
 */
static void Flash_Service_Ecc( void )
{
    flash_Error_t eccError;

    eccError = Flash_Latch_Ecc();

    if ( ( FLASH_ERR_NONE != eccError ) && ( FLASH_NULL_PTR != flash_Context.Config.ErrorCallback ) )
    {
        flash_Context.Config.ErrorCallback( flash_Context.Config.PeriphId, eccError, flash_Context.Ecc.Address );
    }
}


/**
 * \brief Finishes the running operation: disables the programming / erase request and the interrupts, locks the
 *        control register and stores the result.
 *
 * \param error: Error mask of the operation
 */
static void Flash_Finish( flash_Error_t error )
{
    Flash_Hw_Stop_SectorErase();
    Flash_Hw_Stop_Program();

    if ( FLASH_STYLE_INTERRUPT == flash_Context.Config.OperationStyle )
    {
        Flash_Hw_Set_Interrupts( FLASH_FUNCTION_INACTIVE );
    }

    Flash_Hw_Lock();

    if ( FLASH_AREA_USER == flash_Context.Operation.Area.Type )
    {
        (void)Cache_Set_Invalidate( CACHE_INSTRUCTION );        /* the cache may hold the old content of the flash */
    }

    Flash_Set_Result( error );
}


/**
 * \brief Handler of the flash interrupt: serves the running operation and the ECC errors.
 */
static void Flash_Irq_Handler( void )
{
    Flash_Service();
    Flash_Service_Ecc();
}


/**
 * \brief Registers, prioritizes and enables the flash interrupt and enables the interrupt of the corrected ECC error.
 *
 * \param irqPrio: Priority of the interrupt
 *
 * \return \ref FLASH_REQUEST_OK if the NVIC module accepted the configuration
 */
static flash_RequestState_t Flash_Init_Interrupt( flash_IrqPrio_t irqPrio )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    nvic_RequestState_t  nvicState;

    nvicState = Nvic_Set_PeriphIrq_Handler( NVIC_PERIPH_IRQ_FLASH, Flash_Irq_Handler );

    if ( NVIC_REQUEST_OK == nvicState )
    {
        nvicState = Nvic_Set_PeriphIrq_Prio( NVIC_PERIPH_IRQ_FLASH, irqPrio );
    }

    if ( NVIC_REQUEST_OK == nvicState )
    {
        nvicState = Nvic_Set_PeriphIrq_Active( NVIC_PERIPH_IRQ_FLASH );
    }

    if ( NVIC_REQUEST_OK == nvicState )
    {
        Flash_Hw_Set_EccInterrupt( FLASH_FUNCTION_ACTIVE );
        retState = FLASH_REQUEST_OK;
    }

    return (retState);
}


/**
 * \brief Disables the interrupt of the corrected ECC error and the flash interrupt.
 *
 * \return \ref FLASH_REQUEST_OK if the NVIC module disabled the interrupt
 */
static flash_RequestState_t Flash_Deinit_Interrupt( void )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    nvic_RequestState_t  nvicState;

    Flash_Hw_Set_EccInterrupt( FLASH_FUNCTION_INACTIVE );

    nvicState = Nvic_Set_PeriphIrq_Inactive( NVIC_PERIPH_IRQ_FLASH );

    if ( NVIC_REQUEST_OK == nvicState )
    {
        retState = FLASH_REQUEST_OK;
    }

    return (retState);
}






/**
 * \brief Starts the programming of one program unit of an area (the end is signaled by the status flags).
 *
 * \param areaInfo [in]: Area that contains the unit
 * \param address: Address of the unit
 * \param data [in]: Data of the unit
 *
 * \return Error mask, \ref FLASH_ERR_NONE if the unit was programmed
 */
static flash_Error_t Flash_Write_Unit( const flash_AreaInfo_t * const areaInfo, flash_Address_t address, const flash_DataByte_t * const data )
{
    flash_Error_t error;

    if ( FLASH_AREA_USER != areaInfo->Type )
    {
        error = Flash_Write_HalfWord( address, data );          /* high-cycle data flash and OTP: 16 bit */
    }
    else
    {
        error = Flash_Write_QuadWord( address, data );
    }

    return (error);
}


/**
 * \brief Writes one half word of the data flash (the programming runs in the controller).
 *
 * \param address: Address of the half word (aligned to 2 bytes)
 * \param data [in]: 2 bytes of data (any alignment, little endian)
 *
 * \return Error mask, \ref FLASH_ERR_NONE if the half word was programmed
 */
static flash_Error_t Flash_Write_HalfWord( flash_Address_t address, const flash_DataByte_t * const data )
{
    flash_Error_t error;
    uint16_t      value;

    value = (uint16_t)( (uint16_t)data[ 0 ] | (uint16_t)( (uint16_t)data[ 1 ] << FLASH_BITS_PER_BYTE ) );

    Flash_Hw_Program_HalfWord( address, value );

    error = FLASH_ERR_NONE;                             /* the end of the programming is signaled by the status flags */

    return (error);
}


/**
 * \brief Writes one quad word (the programming runs in the controller).
 *
 * \param address: Address of the quad word (aligned to 16 bytes)
 * \param data [in]: 16 bytes of data (any alignment, little endian words)
 *
 * \return Error mask, \ref FLASH_ERR_NONE if the quad word was programmed
 */
static flash_Error_t Flash_Write_QuadWord( flash_Address_t address, const flash_DataByte_t * const data )
{
    flash_Error_t error;
    uint32_t      words[ FLASH_HW_QUADWORD_WORD_CNT ];
    uint32_t      wordId;
    uint32_t      byteId;

    for ( wordId = 0u; wordId < FLASH_HW_QUADWORD_WORD_CNT; wordId++ )
    {
        words[ wordId ] = 0u;

        for ( byteId = 0u; byteId < FLASH_BYTES_PER_WORD; byteId++ )
        {
            words[ wordId ] |= (uint32_t)data[ ( wordId * FLASH_BYTES_PER_WORD ) + byteId ] << ( FLASH_BITS_PER_BYTE * byteId );
        }
    }

    Flash_Hw_Program_QuadWord( address, words );

    error = FLASH_ERR_NONE;                             /* the end of the programming is signaled by the status flags */

    return (error);
}


/**
 * \brief Reads the ECC flags of the controller, adds them to the latched ECC state and clears the flags.
 *
 * \return ECC errors flagged since the last call (\ref FLASH_ERR_NONE if none)
 */
static flash_Error_t Flash_Latch_Ecc( void )
{
    flash_Error_t   eccError;
    flash_Address_t eccAddress = 0u;

    eccError = Flash_Hw_Read_Ecc( Flash_Get_BankSizeStored(), &eccAddress );

    if ( FLASH_ERR_NONE != eccError )
    {
        Flash_Hw_Clear_Ecc( eccError );

        if (    ( 0u != ( eccError & FLASH_ERR_ECC_DETECTED ) )
             || ( 0u == ( flash_Context.Ecc.Error & FLASH_ERR_ECC_DETECTED ) ) )
        {
            flash_Context.Ecc.Address = eccAddress;
        }

        flash_Context.Ecc.Error |= eccError;
    }

    return (eccError);
}


/**
 * \brief Stores the result of an operation.
 *
 * \param error: Error mask of the finished operation
 */
static void Flash_Set_Result( flash_Error_t error )
{
    flash_Context.OperationError = error;

    if ( FLASH_ERR_NONE == error )
    {
        flash_Context.OperationState = FLASH_STATE_IDLE;
    }
    else
    {
        flash_Context.OperationState = FLASH_STATE_FAILED;
    }
}
