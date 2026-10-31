/**
 * \defgroup Flash Flash
 * \brief Flash module
 */

/**
 * \author Mr.Nobody
 * \file Flash_Types.h
 * \ingroup Flash
 * \brief Internal flash memory controller (FLASH) module global types definition
 *
 * This file contains the types definitions used across the module and are
 * available for other modules through Port file. The types are the same for all
 * families (see the README of the repository).
 *
 */

#ifndef FLASH_FLASH_TYPES_H
#define FLASH_FLASH_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "stdint.h"                         /* Module types definition        */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Null pointer definition */
#define FLASH_NULL_PTR                      ( ( void* ) 0u )

/** Value of an erased flash location (the same in all families) */
#define FLASH_ERASED_VALUE                  ( 0xFFu )

/** Maximal count of the access windows in the configuration */
#define FLASH_WINDOW_MAX                    ( 4u )

/** No area attribute */
#define FLASH_AREA_ATTR_NONE                ( 0x00u )

/** Area attribute: a location has to be erased before it is written again */
#define FLASH_AREA_ATTR_ERASE_REQUIRED      ( 0x01u )

/** Area attribute: the content is protected by ECC */
#define FLASH_AREA_ATTR_ECC                 ( 0x02u )

/** Area attribute: a location can be written only once (no erase) */
#define FLASH_AREA_ATTR_WRITE_ONCE          ( 0x04u )

/** Area attribute: the size of the area can be changed (\ref Flash_Set_AreaSize) */
#define FLASH_AREA_ATTR_CONFIGURABLE        ( 0x08u )

/** Area attribute: an erased location can not be read before it is programmed (ECC), no blank check by reading */
#define FLASH_AREA_ATTR_PROGRAM_BEFORE_READ ( 0x10u )

/** Error mask: no error */
#define FLASH_ERR_NONE                      ( 0x00u )

/** Error mask: write protection, locked control register or a request outside of the access windows */
#define FLASH_ERR_PROTECTION                ( 0x01u )

/** Error mask: programming sequence, alignment or size inconsistency */
#define FLASH_ERR_SEQUENCE                  ( 0x02u )

/** Error mask: destination of a write is not erased */
#define FLASH_ERR_NOT_ERASED                ( 0x04u )

/** Error mask: operation failed in the controller */
#define FLASH_ERR_OPERATION                 ( 0x08u )

/** Error mask: operation did not finish in time */
#define FLASH_ERR_TIMEOUT                   ( 0x10u )

/** Error mask: single ECC error was corrected */
#define FLASH_ERR_ECC_CORRECTED             ( 0x20u )

/** Error mask: uncorrectable ECC error was detected */
#define FLASH_ERR_ECC_DETECTED              ( 0x40u )

/** Count of the flash banks (same value as FLASH_BANK_CNT of Flash.h, duplicated here because this
 *  public header is used without that private header) */
#define FLASH_TZ_BANK_CNT                   ( 2u )

/* ============================== TYPEDEFS ================================== */

/** \brief Type signaling major version of SW module */
typedef uint8_t flash_MajorVersion_t;


/** \brief Type signaling minor version of SW module */
typedef uint8_t flash_MinorVersion_t;


/** \brief Type signaling patch version of SW module */
typedef uint8_t flash_PatchVersion_t;


/** \brief Type signaling actual version of SW module */
typedef struct
{
    flash_MajorVersion_t Major; /**< Major version */
    flash_MinorVersion_t Minor; /**< Minor version */
    flash_PatchVersion_t Patch; /**< Patch version */
}   flash_ModuleVersion_t;


/** Function status enumeration */
typedef enum
{
    FLASH_FUNCTION_INACTIVE = 0u, /**< Function status is inactive */
    FLASH_FUNCTION_ACTIVE         /**< Function status is active   */
}   flash_FunctionState_t;


/** Flag states enumeration */
typedef enum
{
    FLASH_FLAG_INACTIVE = 0u, /**< Inactive flag state */
    FLASH_FLAG_ACTIVE         /**< Active flag state   */
}   flash_FlagState_t;


/** Enumeration used to signal request processing state */
typedef enum
{
    FLASH_REQUEST_ERROR = 0u, /**< Processing request failed  */
    FLASH_REQUEST_OK          /**< Processing request succeed */
}   flash_RequestState_t;


/** Flash controller identification (single instance in all families) */
typedef enum
{
    FLASH_PERIPH_1 = 0u, /**< Flash memory controller */
    FLASH_PERIPH_CNT     /**< Count of the controllers */
}   flash_PeriphId_t;


/** Address in the memory map */
typedef uint32_t flash_Address_t;

/** Size in bytes */
typedef uint32_t flash_Size_t;

/** Byte of data */
typedef uint8_t flash_DataByte_t;

/** Index in the list of the flash areas */
typedef uint8_t flash_AreaId_t;

/** Index of the flash bank (0 based) */
typedef uint8_t flash_BankId_t;

/** Priority of the flash interrupt */
typedef uint8_t flash_IrqPrio_t;

/** Bit mask of the area attributes (FLASH_AREA_ATTR_...) */
typedef uint32_t flash_AreaAttr_t;

/** Bit mask of the errors (FLASH_ERR_...) */
typedef uint32_t flash_Error_t;


/** Type of the flash area */
typedef enum
{
    FLASH_AREA_USER = 0u, /**< User flash (program memory), standard endurance        */
    FLASH_AREA_DATA,      /**< Dedicated high-cycle data flash (if the family has it) */
    FLASH_AREA_OTP,       /**< One-time programmable memory                           */
    FLASH_AREA_TYPE_CNT   /**< Count of the area types                                */
}   flash_AreaType_t;


/** Description of a flash area (contiguous memory with the same properties) */
typedef struct
{
    flash_AreaType_t Type;        /**< Type of the area                                          */
    flash_BankId_t   Bank;        /**< Flash bank of the area (0 based)                          */
    flash_Address_t  Address;     /**< First address (memory mapped)                             */
    flash_Size_t     Size;        /**< Usable size in bytes                                      */
    flash_Size_t     EraseSize;   /**< Erase unit in bytes, 0 = the area can not be erased       */
    flash_Size_t     ProgramSize; /**< Program unit in bytes (address and size of a write)       */
    uint32_t         Endurance;   /**< Guaranteed erase cycles of an erase unit, 0 = write once  */
    flash_AreaAttr_t Attributes;  /**< Bit mask of FLASH_AREA_ATTR_...                           */
}   flash_AreaInfo_t;


/** Access window - the memory range where erase and write are accepted */
typedef struct
{
    flash_Address_t Address; /**< First address of the window  */
    flash_Size_t    Size;    /**< Size of the window in bytes  */
}   flash_Window_t;


/** Style of the erase and write operations */
typedef enum
{
    FLASH_STYLE_BLOCKING = 0u, /**< Function returns when the operation is finished       */
    FLASH_STYLE_POLLING,       /**< Operation is served by \ref Flash_Task                */
    FLASH_STYLE_INTERRUPT      /**< Operation is served by the flash interrupt            */
}   flash_OperationStyle_t;


/** Type of the operation */
typedef enum
{
    FLASH_OPERATION_NONE = 0u, /**< No operation */
    FLASH_OPERATION_ERASE,     /**< Erase        */
    FLASH_OPERATION_WRITE      /**< Write        */
}   flash_OperationType_t;


/** State of the operation */
typedef enum
{
    FLASH_STATE_IDLE = 0u, /**< No operation runs, the last one finished without an error */
    FLASH_STATE_BUSY,      /**< Operation is running                                      */
    FLASH_STATE_FAILED     /**< The last operation failed (see \ref Flash_Get_OperationError) */
}   flash_OperationState_t;


/** State of the configuration of a configurable area */
typedef enum
{
    FLASH_CONFIG_APPLIED = 0u, /**< Configuration is valid                          */
    FLASH_CONFIG_PENDING       /**< New configuration waits for \ref Flash_Set_ConfigApply */
}   flash_ConfigState_t;


/** State of the ECC */
typedef struct
{
    flash_Error_t   Error;   /**< ECC error mask (FLASH_ERR_ECC_CORRECTED, FLASH_ERR_ECC_DETECTED) */
    flash_Address_t Address; /**< Address of the word with the last ECC error                       */
}   flash_EccState_t;


/** Secure watermark area of a flash bank (option bytes, meaningful only if TrustZone is active) */
typedef struct
{
    uint32_t StartSector; /**< First sector of the secure area of the bank */
    uint32_t EndSector;   /**< Last sector of the secure area of the bank  */
}   flash_SecureWatermark_t;


/** State of TrustZone (read-only, from the option bytes) */
typedef struct
{
    flash_FunctionState_t   TzState;                           /**< Active if TrustZone is enabled              */
    flash_SecureWatermark_t SecureWatermark[ FLASH_TZ_BANK_CNT ]; /**< Secure watermark of each bank, valid if active */
}   flash_TrustZoneState_t;


/**
 * \brief End of the operation callback.
 *
 * Called from the servicing context (\ref Flash_Task or the interrupt) when an operation started
 * in polling or interrupt style is finished.
 *
 * \param periphId: Controller
 * \param operation: Finished operation
 * \param address: Start address of the operation
 * \param size: Size of the operation in bytes
 * \param error: Error mask, \ref FLASH_ERR_NONE if the operation succeeded
 */
typedef void ( flash_OperationCallback_t )( flash_PeriphId_t periphId, flash_OperationType_t operation, flash_Address_t address, flash_Size_t size, flash_Error_t error );


/**
 * \brief Error callback.
 *
 * Called from the servicing context when an ECC error is reported outside of an operation.
 *
 * \param periphId: Controller
 * \param error: Error mask
 * \param address: Address of the word with the error
 */
typedef void ( flash_ErrorCallback_t )( flash_PeriphId_t periphId, flash_Error_t error, flash_Address_t address );


/** Configuration of the module */
typedef struct
{
    flash_PeriphId_t           PeriphId;                  /**< Controller                                         */
    flash_OperationStyle_t     OperationStyle;            /**< Style of the erase and write operations            */
    flash_IrqPrio_t            IrqPrio;                   /**< Priority of the flash interrupt                    */
    uint8_t                    WindowCount;               /**< Count of the valid access windows                  */
    flash_Window_t             Windows[ FLASH_WINDOW_MAX ]; /**< Access windows (erase and write only inside)     */
    flash_OperationCallback_t *OperationCallback;         /**< End of operation. FLASH_NULL_PTR if not used.      */
    flash_ErrorCallback_t     *ErrorCallback;             /**< ECC error. FLASH_NULL_PTR if not used.             */
}   flash_Config_t;

#ifdef __cplusplus
}
#endif

#endif /* FLASH_FLASH_TYPES_H */
