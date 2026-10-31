/**
 * \author Mr.Nobody
 * \file Flash_Hw.c
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) MCAL module register level functionality
 *
 * Unlock / lock of the control register, status and error flags, sector erase and quad word
 * (128-bit) program of the user flash. The module works with TrustZone disabled, so the non-secure
 * registers (NSKEYR, NSCR, NSSR, NSCCR) are used.
 *
 * \note Register write verification (read-back): the sector selection (SNB, BKSEL) and the PG / SER
 *       bits of the control register are verified. Exceptions: the START bit (starts the operation
 *       and is cleared by the controller), the key register (write only, verified by the LOCK bit)
 *       and the clear control register (write 1 to clear, flags are cleared by the controller).
 *
 */
/* ============================== INCLUDES ================================== */
#include "Flash.h"                          /* Count of the banks             */
#include "Flash_Hw.h"                       /* Self include                   */
#include "Flash_Types.h"                    /* Module types definitions       */
#include "Stm32.h"                          /* STM32 peripheral registers     */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** First key of the control register unlock sequence */
#define FLASH_HW_KEY1                       ( 0x45670123u )

/** Second key of the control register unlock sequence */
#define FLASH_HW_KEY2                       ( 0xCDEF89ABu )

/** Status flags that signal a running operation */
#define FLASH_HW_BUSY_MASK                  ( FLASH_SR_BSY | FLASH_SR_WBNE | FLASH_SR_DBNE )

/** First key of the option bytes unlock sequence */
#define FLASH_HW_OPT_KEY1                   ( 0x08192A3Bu )

/** Second key of the option bytes unlock sequence */
#define FLASH_HW_OPT_KEY2                   ( 0x4C5D6E7Fu )

#if defined(STM32H503xx)
/**
 * Address reported by the ECC for the first word of the OTP area: 0x0200 measured on STM32H503 (Bug AB#1191, the
 * words 0x08FFF000 / 0x08FFF004 / 0x08FFF010 / 0x08FFF7FC report 0x200 / 0x201 / 0x204 / 0x3FF); RM0492 Rev 3
 * Table 42 states 0x0600
 */
#define FLASH_HW_ECC_OTP_OFFSET             ( 0x200u )
#else
/** Address reported by the ECC for the first word of the OTP area (RM0481 Rev 5 Table 88, RM0517 Rev 1 Table 75, ST HAL) */
#define FLASH_HW_ECC_OTP_OFFSET             ( 0x600u )
#endif /* STM32H503xx */

/** Multiplier of the address reported by the ECC for the OTP and the high-cycle data area (as in the ST HAL) */
#define FLASH_HW_ECC_WORD_FACTOR            ( 4u )

/** Count of the addresses reported by the ECC for the OTP area (one address per word of 4 bytes) */
#define FLASH_HW_ECC_OTP_WORD_CNT           ( (uint32_t)FLASH_OTP_SIZE / FLASH_HW_ECC_WORD_FACTOR )

/** Interrupt enable bits of the control register (end of the operation and the errors) */
#define FLASH_HW_INTERRUPT_MASK             ( FLASH_CR_EOPIE | FLASH_CR_WRPERRIE | FLASH_CR_PGSERRIE | FLASH_CR_STRBERRIE | FLASH_CR_INCERRIE )

/** Sector erase selection fields of the control register */
#define FLASH_HW_SECTOR_MASK                ( FLASH_CR_SNB | FLASH_CR_BKSEL )

/** Value of the TZEN field (OPTSR2_CUR) when TrustZone is enabled (RM0481 Rev 5 Section 7.11.23) */
#define FLASH_HW_TZEN_ACTIVE_VALUE          ( 0xB4u )

/* ======================= FORWARD DECLARATIONS ============================= */

static flash_Error_t    Flash_Hw_Decode_Errors      ( uint32_t status );
static flash_Address_t  Flash_Hw_Decode_EccAddress  ( uint32_t regValue, flash_Size_t bankSize );
static uint32_t         Flash_Hw_Get_ClearMask      ( uint32_t status );

/* ======================== EXPORTED FUNCTIONS ============================== */

/**
 * \brief Unlocks the control register of the flash (the key sequence is written only if it is locked).
 *
 * \return \ref FLASH_REQUEST_OK if the control register is unlocked, otherwise \ref FLASH_REQUEST_ERROR.
 */
flash_RequestState_t Flash_Hw_Unlock( void )
{
    flash_RequestState_t retState = FLASH_REQUEST_OK;

    if ( 0u != ( FLASH_NS->NSCR & FLASH_CR_LOCK ) )
    {
        FLASH_NS->NSKEYR = FLASH_HW_KEY1;
        FLASH_NS->NSKEYR = FLASH_HW_KEY2;

        if ( 0u != ( FLASH_NS->NSCR & FLASH_CR_LOCK ) )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Locks the control register of the flash.
 */
void Flash_Hw_Lock( void )
{
    FLASH_NS->NSCR |= FLASH_CR_LOCK;
}


/**
 * \brief Clears the error and the end of operation flags.
 */
void Flash_Hw_Clear_Flags( void )
{
    FLASH_NS->NSCCR = FLASH_CCR_CLR_EOP | FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_OPTCHANGEERR;
}


/**
 * \brief Waits until the running operation is finished and reads its result.
 *
 * The error flags and the end of operation flag are cleared.
 *
 * \return Error mask of the operation: \ref FLASH_ERR_NONE, \ref FLASH_ERR_TIMEOUT if the operation
 *         did not finish in \ref FLASH_HW_BUSY_TIMEOUT_CNT iterations, otherwise the decoded error flags
 */
flash_Error_t Flash_Hw_Wait_Ready( void )
{
    flash_Error_t      retError   = FLASH_ERR_NONE;
    flash_TimeoutCnt_t timeoutCnt = FLASH_HW_BUSY_TIMEOUT_CNT;
    uint32_t           status;

    status = FLASH_NS->NSSR;

    while ( ( 0u != ( status & FLASH_HW_BUSY_MASK ) ) && ( 0u < timeoutCnt ) )
    {
        timeoutCnt--;
        status = FLASH_NS->NSSR;
    }

    if ( 0u != ( status & FLASH_HW_BUSY_MASK ) )
    {
        retError = FLASH_ERR_TIMEOUT;
    }
    else
    {
        retError = Flash_Hw_Decode_Errors( status );
        FLASH_NS->NSCCR = Flash_Hw_Get_ClearMask( status );
    }

    return (retError);
}


/**
 * \brief Reads whether an operation is running (BSY, write buffer and data buffer flags).
 *
 * \return \ref FLASH_FLAG_ACTIVE if the controller is busy
 */
flash_FlagState_t Flash_Hw_Get_BusyState( void )
{
    flash_FlagState_t busyState = FLASH_FLAG_INACTIVE;

    if ( 0u != ( FLASH_NS->NSSR & FLASH_HW_BUSY_MASK ) )
    {
        busyState = FLASH_FLAG_ACTIVE;
    }

    return (busyState);
}


/**
 * \brief Reads the result of a finished operation: decodes the error flags and clears them with the end of operation flag.
 *
 * The controller shall not be busy (\ref Flash_Hw_Get_BusyState).
 *
 * \return Error mask of the operation, \ref FLASH_ERR_NONE if there is no error
 */
flash_Error_t Flash_Hw_Get_Result( void )
{
    flash_Error_t retError;
    uint32_t      status = FLASH_NS->NSSR;

    retError        = Flash_Hw_Decode_Errors( status );
    FLASH_NS->NSCCR = Flash_Hw_Get_ClearMask( status );

    return (retError);
}


/**
 * \brief Enables or disables the flash interrupts of the operations (end of the operation and errors).
 *
 * The control register has to be unlocked.
 *
 * \param interruptState: Enable or disable
 */
void Flash_Hw_Set_Interrupts( flash_FunctionState_t interruptState )
{
    if ( FLASH_FUNCTION_ACTIVE == interruptState )
    {
        FLASH_NS->NSCR |= FLASH_HW_INTERRUPT_MASK;
    }
    else
    {
        FLASH_NS->NSCR &= ~FLASH_HW_INTERRUPT_MASK;
    }
}


/**
 * \brief Enables or disables the interrupt of the corrected ECC error.
 *
 * The flag of the corrected error is not changed (write 1 to clear).
 *
 * \param interruptState: Enable or disable
 */
void Flash_Hw_Set_EccInterrupt( flash_FunctionState_t interruptState )
{
    uint32_t regValue = FLASH_NS->ECCCORR & ~FLASH_ECCR_ECCC;

    if ( FLASH_FUNCTION_ACTIVE == interruptState )
    {
        regValue |= FLASH_ECCR_ECCIE;
    }
    else
    {
        regValue &= ~FLASH_ECCR_ECCIE;
    }

    FLASH_NS->ECCCORR = regValue;
}


/**
 * \brief Starts the erase of a sector of the user flash.
 *
 * The control register has to be unlocked and no operation can run. The end of the erase is
 * signaled by \ref Flash_Hw_Wait_Ready, the sector erase request is cleared by
 * \ref Flash_Hw_Stop_SectorErase.
 *
 * \param bankId: Flash bank (0 based)
 * \param sectorId: Sector in the bank
 *
 * \return \ref FLASH_REQUEST_OK if the sector selection was verified by read-back, otherwise
 *         \ref FLASH_REQUEST_ERROR (the erase was not started)
 */
flash_RequestState_t Flash_Hw_Start_SectorErase( flash_BankId_t bankId, uint32_t sectorId )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;
    uint32_t             regValue;
    uint32_t             selection;

    selection = ( sectorId << FLASH_CR_SNB_Pos ) & FLASH_CR_SNB;

    if ( 0u != bankId )
    {
        selection |= FLASH_CR_BKSEL;
    }

    regValue  = FLASH_NS->NSCR;
    regValue &= ~FLASH_HW_SECTOR_MASK;
    regValue |= FLASH_CR_SER | selection;
    FLASH_NS->NSCR = regValue;

    regValue = FLASH_NS->NSCR;

    if ( ( 0u != ( regValue & FLASH_CR_SER ) ) && ( selection == ( regValue & FLASH_HW_SECTOR_MASK ) ) )
    {
        FLASH_NS->NSCR |= FLASH_CR_START;
        retState = FLASH_REQUEST_OK;
    }

    return (retState);
}


/**
 * \brief Clears the sector erase request.
 *
 * The selection of the sector (SNB, BKSEL) stays in the control register.
 */
void Flash_Hw_Stop_SectorErase( void )
{
    FLASH_NS->NSCR &= ~FLASH_CR_SER;
}


/**
 * \brief Enables the programming of the user flash.
 *
 * \return \ref FLASH_REQUEST_OK if the programming bit was verified by read-back, otherwise
 *         \ref FLASH_REQUEST_ERROR
 */
flash_RequestState_t Flash_Hw_Start_Program( void )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

    FLASH_NS->NSCR |= FLASH_CR_PG;

    if ( 0u != ( FLASH_NS->NSCR & FLASH_CR_PG ) )
    {
        retState = FLASH_REQUEST_OK;
    }

    return (retState);
}


/**
 * \brief Writes a quad word (128 bit) to the flash.
 *
 * The programming has to be enabled by \ref Flash_Hw_Start_Program. The four writes are not
 * interrupted (interrupts are disabled for the time of the writes).
 *
 * \param address: Address of the quad word (aligned to 16 bytes)
 * \param words [in]: The four 32-bit words of the quad word
 */
void Flash_Hw_Program_QuadWord( flash_Address_t address, const uint32_t * const words )
{
    volatile uint32_t * destAddr = (volatile uint32_t *)(uintptr_t)address;
    uint32_t            primask  = __get_PRIMASK();
    uint32_t            wordId;

    __disable_irq();

    for ( wordId = 0u; wordId < FLASH_HW_QUADWORD_WORD_CNT; wordId++ )
    {
        destAddr[ wordId ] = words[ wordId ];
    }

    __set_PRIMASK( primask );
}


/**
 * \brief Writes a half word (16 bit) to the high-cycle data flash.
 *
 * The programming has to be enabled by \ref Flash_Hw_Start_Program.
 *
 * \param address: Address of the half word (aligned to 2 bytes)
 * \param value: Value to write
 */
void Flash_Hw_Program_HalfWord( flash_Address_t address, uint16_t value )
{
    volatile uint16_t * destAddr = (volatile uint16_t *)(uintptr_t)address;

    *destAddr = value;
}


/**
 * \brief Disables the programming of the flash.
 */
void Flash_Hw_Stop_Program( void )
{
    FLASH_NS->NSCR &= ~FLASH_CR_PG;
}

/**
 * \brief Reads the ECC flags and the address of the word with the error.
 *
 * The flags are not cleared. If both a corrected and a detected error are flagged, the address is the
 * address of the detected one. The address of a word outside of the areas of the module (system flash,
 * option byte keys, read-only area) is 0.
 *
 * \param bankSize: Size of a bank of the user flash in bytes (the flash size register is not read here: the
 *        function runs in the NMI and the instruction cache can not be suspended there)
 * \param address [out]: Address of the word with the error (0 if there is none)
 *
 * \return Error mask: \ref FLASH_ERR_ECC_CORRECTED and / or \ref FLASH_ERR_ECC_DETECTED
 */
flash_Error_t Flash_Hw_Read_Ecc( flash_Size_t bankSize, flash_Address_t * const address )
{
    flash_Error_t retError = FLASH_ERR_NONE;
    uint32_t      corrReg  = FLASH_NS->ECCCORR;
    uint32_t      detReg   = FLASH_NS->ECCDETR;
    uint32_t      regValue = 0u;

    *address = 0u;

    if ( 0u != ( corrReg & FLASH_ECCR_ECCC ) )
    {
        retError |= FLASH_ERR_ECC_CORRECTED;
        regValue  = corrReg;
    }

    if ( 0u != ( detReg & FLASH_ECCR_ECCD ) )
    {
        retError |= FLASH_ERR_ECC_DETECTED;
        regValue  = detReg;
    }

    if ( FLASH_ERR_NONE != retError )
    {
        *address = Flash_Hw_Decode_EccAddress( regValue, bankSize );
    }

    return (retError);
}


/**
 * \brief Clears the ECC flags (write 1 to clear).
 *
 * \param eccMask: \ref FLASH_ERR_ECC_CORRECTED and / or \ref FLASH_ERR_ECC_DETECTED
 */
void Flash_Hw_Clear_Ecc( flash_Error_t eccMask )
{
    if ( 0u != ( eccMask & FLASH_ERR_ECC_CORRECTED ) )
    {
        FLASH_NS->ECCCORR |= FLASH_ECCR_ECCC;
    }

    if ( 0u != ( eccMask & FLASH_ERR_ECC_DETECTED ) )
    {
        FLASH_NS->ECCDETR |= FLASH_ECCR_ECCD;
    }
}


/**
 * \brief Masks the NMI of the uncorrectable ECC error (SBS), the ECCD flag is still set by the controller.
 *
 * \return Previous state of the mask for \ref Flash_Hw_Restore_EccNmi
 */
uint32_t Flash_Hw_Mask_EccNmi( void )
{
    uint32_t previousMask = SBS->ECCNMIR & SBS_ECCNMIR_ECCNMI_MASK_EN;

    SBS->ECCNMIR |= SBS_ECCNMIR_ECCNMI_MASK_EN;

    return (previousMask);
}


/**
 * \brief Restores the mask of the NMI of the uncorrectable ECC error.
 *
 * \param previousMask: Value returned by \ref Flash_Hw_Mask_EccNmi
 */
void Flash_Hw_Restore_EccNmi( uint32_t previousMask )
{
    if ( 0u == previousMask )
    {
        SBS->ECCNMIR &= ~SBS_ECCNMIR_ECCNMI_MASK_EN;
    }
}


/**
 * \brief Unlocks the option bytes (the key sequence is written only if they are locked).
 *
 * \return \ref FLASH_REQUEST_OK if the option bytes are unlocked, otherwise \ref FLASH_REQUEST_ERROR
 */
flash_RequestState_t Flash_Hw_Unlock_Options( void )
{
    flash_RequestState_t retState = FLASH_REQUEST_OK;

    if ( 0u != ( FLASH_NS->OPTCR & FLASH_OPTCR_OPTLOCK ) )
    {
        FLASH_NS->OPTKEYR = FLASH_HW_OPT_KEY1;
        FLASH_NS->OPTKEYR = FLASH_HW_OPT_KEY2;

        if ( 0u != ( FLASH_NS->OPTCR & FLASH_OPTCR_OPTLOCK ) )
        {
            retState = FLASH_REQUEST_ERROR;
        }
    }

    return (retState);
}


/**
 * \brief Locks the option bytes.
 */
void Flash_Hw_Lock_Options( void )
{
    FLASH_NS->OPTCR |= FLASH_OPTCR_OPTLOCK;
}


/**
 * \brief Writes the number of the high-cycle data sectors of a bank to the option byte to be programmed.
 *
 * The option bytes have to be unlocked. The value is programmed by \ref Flash_Hw_Start_OptionProgram
 * and valid after the reset of the device.
 *
 * \param bankId: Flash bank (0 based)
 * \param sectorCnt: Count of the sectors (0 = disabled)
 *
 * \return \ref FLASH_REQUEST_OK if the value was verified by read-back, otherwise \ref FLASH_REQUEST_ERROR
 */
flash_RequestState_t Flash_Hw_Set_DataSectors( flash_BankId_t bankId, flash_Size_t sectorCnt )
{
    flash_RequestState_t retState = FLASH_REQUEST_ERROR;

#if defined(FLASH_EDATAR_EDATA_EN)
    uint32_t regValue = 0u;
    uint32_t readBack;

    if ( 0u < sectorCnt )
    {
        regValue = FLASH_EDATAR_EDATA_EN | ( ( sectorCnt - 1u ) & FLASH_EDATAR_EDATA_STRT );
    }

    if ( 0u == bankId )
    {
        FLASH_NS->EDATA1R_PRG = regValue;
        readBack              = FLASH_NS->EDATA1R_PRG;
    }
    else
    {
        FLASH_NS->EDATA2R_PRG = regValue;
        readBack              = FLASH_NS->EDATA2R_PRG;
    }

    if ( regValue == readBack )
    {
        retState = FLASH_REQUEST_OK;
    }
#else
    (void)bankId;
    (void)sectorCnt;
#endif /* FLASH_EDATAR_EDATA_EN */

    return (retState);
}


/**
 * \brief Starts the programming of the option bytes (the end is signaled by \ref Flash_Hw_Wait_Ready).
 */
void Flash_Hw_Start_OptionProgram( void )
{
    FLASH_NS->OPTCR |= FLASH_OPTCR_OPTSTART;
}


/**
 * \brief Reads the state of TrustZone and the secure watermark of each bank from the option bytes.
 *
 * \param state [out]: State of TrustZone
 *
 * \note Devices without TrustZone (no FLASH_OPTSR2_TZEN) always report \ref FLASH_FUNCTION_INACTIVE
 *       and an empty secure watermark; the option bytes are not accessed (RM0481 Rev 5 Section 7.11.23,
 *       RM0492 has no TZEN option bit).
 */
void Flash_Hw_Get_TrustZoneState( flash_TrustZoneState_t * const state )
{
#if defined(FLASH_OPTSR2_TZEN)
    uint32_t       regValue;
    flash_BankId_t bankId;

    regValue = ( FLASH_NS->OPTSR2_CUR & FLASH_OPTSR2_TZEN ) >> FLASH_OPTSR2_TZEN_Pos;

    if ( FLASH_HW_TZEN_ACTIVE_VALUE == regValue )
    {
        state->TzState = FLASH_FUNCTION_ACTIVE;
    }
    else
    {
        state->TzState = FLASH_FUNCTION_INACTIVE;
    }

    for ( bankId = 0u; bankId < FLASH_BANK_CNT; bankId++ )
    {
        if ( 0u == bankId )
        {
            regValue = FLASH_NS->SECWM1R_CUR;
        }
        else
        {
            regValue = FLASH_NS->SECWM2R_CUR;
        }

        state->SecureWatermark[ bankId ].StartSector = ( regValue & FLASH_SECWMR_SECWM_STRT )
                                                         >> FLASH_SECWMR_SECWM_STRT_Pos;
        state->SecureWatermark[ bankId ].EndSector   = ( regValue & FLASH_SECWMR_SECWM_END )
                                                         >> FLASH_SECWMR_SECWM_END_Pos;
    }
#else
    flash_BankId_t bankId;

    state->TzState = FLASH_FUNCTION_INACTIVE;

    for ( bankId = 0u; bankId < FLASH_BANK_CNT; bankId++ )
    {
        state->SecureWatermark[ bankId ].StartSector = 0u;
        state->SecureWatermark[ bankId ].EndSector   = 0u;
    }
#endif /* FLASH_OPTSR2_TZEN */
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * \brief Decodes the error flags of the status register to the error mask of the module.
 *
 * \param status: Value of the status register
 *
 * \return Error mask (write protection, sequence)
 */
static flash_Error_t Flash_Hw_Decode_Errors( uint32_t status )
{
    flash_Error_t retError = FLASH_ERR_NONE;

    if ( 0u != ( status & FLASH_SR_WRPERR ) )
    {
        retError |= FLASH_ERR_PROTECTION;
    }

    if ( 0u != ( status & ( FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR ) ) )
    {
        retError |= FLASH_ERR_SEQUENCE;
    }

    if ( 0u != ( status & FLASH_SR_OPTCHANGEERR ) )
    {
        retError |= FLASH_ERR_OPERATION;
    }

    return (retError);
}


/**
 * \brief Decodes the address of the word with an ECC error from the ECC register (as the ST HAL does).
 *
 * The OTP area is decoded with the offset of the device line (STM32H503 reports 0x0200, not the 0x0600 of the
 * reference manual and the ST HAL, Bug AB#1191); an address of the OTP_ECC group outside of the OTP area (the
 * read-only area) is not an area of the module and decodes to 0.
 *
 * \param regValue: Value of the ECC correction or detection register
 * \param bankSize: Size of a bank of the user flash in bytes
 *
 * \return Address of the word, 0 if it is outside of the areas of the module
 */
static flash_Address_t Flash_Hw_Decode_EccAddress( uint32_t regValue, flash_Size_t bankSize )
{
    flash_Address_t address  = 0u;
    uint32_t        addrReg  = regValue & FLASH_ECCR_ADDR_ECC;
    flash_Size_t    bankId   = 0u;

    if ( 0u != ( regValue & FLASH_ECCR_BK_ECC ) )
    {
        bankId = 1u;
    }

    if ( 0u != ( regValue & FLASH_ECCR_OTP_ECC ) )
    {
        if ( ( FLASH_HW_ECC_OTP_OFFSET <= addrReg ) &&
             ( ( FLASH_HW_ECC_OTP_OFFSET + FLASH_HW_ECC_OTP_WORD_CNT ) > addrReg )    )
        {
            address = (flash_Address_t)FLASH_OTP_BASE + ( ( addrReg - FLASH_HW_ECC_OTP_OFFSET ) * FLASH_HW_ECC_WORD_FACTOR );
        }
        else
        {
            address = 0u;                                   /* Read-only area (the OTP_ECC flag is shared), not an area of the module */
        }
    }
    else if ( 0u != ( regValue & FLASH_ECCR_SYSF_ECC ) )
    {
        address = 0u;
    }
#if defined(FLASH_ECCR_OBK_ECC)
    else if ( 0u != ( regValue & FLASH_ECCR_OBK_ECC ) )
    {
        address = 0u;
    }
#endif /* FLASH_ECCR_OBK_ECC */
#if defined(FLASH_ECCR_DATA_ECC)
    else if ( 0u != ( regValue & FLASH_ECCR_DATA_ECC ) )
    {
        address = (flash_Address_t)FLASH_EDATA_BASE_NS
                + ( bankId * ( (flash_Size_t)FLASH_EDATA_SIZE / FLASH_BANK_CNT ) )
                + ( ( addrReg - ( ( ( (uint32_t)FLASH_SECTOR_NB - ( ( ( FLASH_EDATAR_EDATA_STRT ) >> FLASH_EDATAR_EDATA_STRT_Pos ) + 1u ) ) * FLASH_SECTOR_SIZE ) / FLASH_HW_QUADWORD_SIZE ) ) * FLASH_HW_ECC_WORD_FACTOR );
    }
#endif /* FLASH_ECCR_DATA_ECC */
    else
    {
        address = (flash_Address_t)FLASH_BASE_NS + ( bankId * bankSize ) + ( addrReg * FLASH_HW_QUADWORD_SIZE );
    }

    return (address);
}


/**
 * \brief Builds the value of the clear control register that clears the flags set in the status.
 *
 * \param status: Value of the status register
 *
 * \return Value for the clear control register
 */
static uint32_t Flash_Hw_Get_ClearMask( uint32_t status )
{
    uint32_t clearMask = 0u;

    if ( 0u != ( status & FLASH_SR_EOP ) )
    {
        clearMask |= FLASH_CCR_CLR_EOP;
    }

    if ( 0u != ( status & FLASH_SR_WRPERR ) )
    {
        clearMask |= FLASH_CCR_CLR_WRPERR;
    }

    if ( 0u != ( status & FLASH_SR_PGSERR ) )
    {
        clearMask |= FLASH_CCR_CLR_PGSERR;
    }

    if ( 0u != ( status & FLASH_SR_STRBERR ) )
    {
        clearMask |= FLASH_CCR_CLR_STRBERR;
    }

    if ( 0u != ( status & FLASH_SR_INCERR ) )
    {
        clearMask |= FLASH_CCR_CLR_INCERR;
    }

    if ( 0u != ( status & FLASH_SR_OPTCHANGEERR ) )
    {
        clearMask |= FLASH_CCR_CLR_OPTCHANGEERR;
    }

    return (clearMask);
}
