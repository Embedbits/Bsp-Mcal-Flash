# Flash Peripheral Driver

This module provides an abstraction layer for the **internal flash memory controller** of STM32 MCUs - erase, program
and read of the flash by the application at run time, primarily for **runtime data** (settings, calibration values,
counters, event logs) that must survive a power cycle. It is a low-level MCAL module: the memory is described as a list
of *areas* and the operations are independent of the area they work on, so a middleware above the module (key-value
store, EEPROM emulation, log) is written once and works on every family.

The public interface (`Flash_Port.h`, `Flash_Types.h`) is **identical for all supported families**. The implementation
for a family lives in its own branch (`Dev/<Family>` for development, `Releases/<Family>` for the released versions), so
a middleware or an application written against the interface does not change when the MCU family changes.

> **Status: design.** This README is the specification of the interface. The STM32H5 implementation is in preparation
> (branch `Dev/STM32H5`, user story AB#1031); the other families follow with their own stories.

---

## Intent and purpose

- Many applications must keep a small amount of data in non-volatile memory and change it at run time. The internal
  flash is the cheapest place for it, but the flash has an erase unit much bigger than the data, a limited number of
  erase cycles and a program unit that differs from family to family.
- **STM32H5 has a dedicated high-cycle data flash** (EDATA): a part of the flash with 100 000 erase cycles instead of
  10 000, programmed by 16 bits with its own ECC, so a small record does not have to be padded to a 16 byte flash
  word. It is made exactly for runtime data. **Not every MCU has it** - most of the other STM32 families keep the data
  in ordinary user flash sectors reserved by the linker (region `USER_DATA`).
- The module hides the difference. The application asks the module **what the memory looks like** (areas with their
  erase unit, program unit, endurance and attributes) and uses the same four operations - erase, write, read, blank
  check - everywhere. Whether a DATA area exists or the data live in a reserved part of the user flash is a decision of
  the storage layer made from the area list, not a `#if` in the application.
- The module is a **protection** as well: erase and write are accepted only inside *access windows* declared by the
  application, so a wrong address can never destroy the program.

What the module is **not**: it has no wear levelling, no power-fail safe record handling and no file system - that is
the job of a middleware built on top of it. It does not configure the flash latency, prefetch or clocks (RCC module).

---

## Flash areas

The memory is described by a list of areas. An area is a contiguous range of memory with the same properties.

| Area type | Meaning | Typical use |
|---|---|---|
| `FLASH_AREA_USER` | User flash (program memory), standard endurance. The part reserved for data (linker region `USER_DATA`) is made writable by an access window; the code is never touched. | Runtime data on families without a data flash |
| `FLASH_AREA_DATA` | Dedicated high-cycle data flash (STM32H5: EDATA). Higher endurance, small program unit. Exists only on families that have it and, if it has to be enabled (STM32H5: option bytes), only when it is enabled. | Frequently changed runtime data |
| `FLASH_AREA_OTP` | One-time programmable memory, write once, no erase. | Serial numbers, calibration, keys |

Every area is described by `flash_AreaInfo_t`:

| Field | Meaning |
|---|---|
| `Type` | `FLASH_AREA_USER`, `FLASH_AREA_DATA`, `FLASH_AREA_OTP` |
| `Bank` | Flash bank of the area (0 based). Erase and write of a bank can stall the code executed from the same bank. |
| `Address`, `Size` | First address (memory mapped) and usable size in bytes |
| `EraseSize` | Erase unit. Constant inside the area (areas of families with non-uniform sectors are split to uniform parts); 0 = the area can not be erased (OTP) |
| `ProgramSize` | Program unit. Address and size of a write are multiples of it |
| `Endurance` | Guaranteed erase cycles of one erase unit (0 = write once) |
| `Attributes` | Bit mask: `FLASH_AREA_ATTR_ERASE_REQUIRED`, `FLASH_AREA_ATTR_ECC`, `FLASH_AREA_ATTR_WRITE_ONCE`, `FLASH_AREA_ATTR_CONFIGURABLE` (size can be changed by `Flash_Set_AreaSize`) |

Rules valid in every family:

1. **Erase** works with whole erase units: `address` and `size` are multiples of `EraseSize` of the area.
2. **Write** works with whole program units: `address` and `size` are multiples of `ProgramSize` of the area. The
   caller pads the record. The erased value is `FLASH_ERASED_VALUE` (0xFF) in every family.
3. A location can be written **once after an erase**. The module checks that the destination is blank and refuses the
   write otherwise (`FLASH_ERR_NOT_ERASED`), no register is touched.
4. The storage layer takes the **first DATA area** of the list if there is one, otherwise a user window (see below).

---

## Access windows

`Flash_Config_t` contains a list of access windows (address, size). `Flash_Set_EraseStart` and `Flash_Set_WriteStart`
are accepted only inside a window; reading and the blank check work everywhere inside an area.

- The **default configuration** (`Flash_Get_DefaultConfig`) contains one window for every DATA area that exists, so on
  STM32H5 with the high-cycle data enabled the data area works out of the box.
- The application adds windows for data in the user flash. In the EmBi platform the Linker module reserves them: the
  region `USER_DATA` at the end of the flash (size from the CMake variable `USER_DATA_SIZE`, symbols
  `_user_data_flash_start` / `_user_data_flash_end`) exists in the linker script of every family.
- `Flash_Init` fails if a window is not aligned to the erase units of the area it lies in, or if it lies outside every
  area. The module can not know where the code ends - choosing the window is the responsibility of the application
  (the linker script does it).

---

## Operation styles

| Style | Behaviour |
|---|---|
| `FLASH_STYLE_BLOCKING` | `Flash_Set_EraseStart` / `Flash_Set_WriteStart` return when the operation is finished (or failed, or timed out). No callbacks. |
| `FLASH_STYLE_POLLING` | The functions only start the operation and return, the operation is served by `Flash_Task()`. |
| `FLASH_STYLE_INTERRUPT` | The functions only start the operation and return, it is served by the FLASH interrupt. |

In polling and interrupt style the end of the operation is reported by `OperationCallback`, errors by `ErrorCallback`;
both are called **only from the servicing context** (`Flash_Task` or the interrupt), never from an API function. One
operation runs at a time - another request while busy returns `FLASH_REQUEST_ERROR`. The data buffer of a write must
stay valid until the operation is finished. `Flash_Get_OperationState` and `Flash_Get_OperationError` can be polled
instead of using callbacks.

---

## Supported families

| Family | Branch | Status |
|---|---|---|
| STM32H5 (H503, H523, H533, H543, H553, H562, H563, H573, H5E4, H5E5, H5F4, H5F5) | `Dev/STM32H5` | in preparation (AB#1031) |
| Other families | - | interface designed to fit, separate stories per family |

---

## Same interface in other families (design basis)

The interface was designed against the flash of all families of the platform, so the other families only fill the same
area list with their own values.

| Family | User flash erase unit | User flash program unit | Banks | Data flash | OTP |
|---|---|---|---|---|---|
| STM32H5 | 8 KB sector | 16 B (128 bit); 2 B in DATA and OTP | 1 - 2 | **yes**: EDATA, 6 KB erase unit, 100 000 cycles, last sectors of a bank (up to 8, 16 on H5F5 - H5E to be verified) taken from the user flash by option bytes; none on H503 | 2 KB |
| STM32U5 | 8 KB page | 16 B (128 bit) | 2 | no | 512 B |
| STM32H7 | 128 KB sector | 32 B (256 bit flash word) | 2 | no | see family branch |
| STM32L4, STM32G4 | 2 KB page | 8 B (64 bit) | 1 - 2 | no | see family branch |
| STM32F4 | 16 / 64 / 128 KB sectors (non-uniform: one area per uniform group) | 1 - 8 B (by supply voltage) | 1 - 2 | no | 528 B |
| STM32F7 | 32 / 128 / 256 KB sectors (non-uniform) | 1 - 8 B | 1 | no | up to 1 KB |

What it means for the interface:

- **No DATA area is not an error.** The list simply has no `FLASH_AREA_DATA`, the storage layer uses a user window. The
  big erase units of H7 / F4 / F7 are visible in `EraseSize` (the storage layer then needs more space for the same
  number of records), the program unit in `ProgramSize`.
- `Flash_Set_AreaSize`, `Flash_Get_ConfigState`, `Flash_Set_ConfigApply` (configuration of the data area) and
  `Flash_Handle_Nmi` have a meaning only where the hardware has it (STM32H5). In the other families they stay in the
  interface, return `FLASH_REQUEST_ERROR` (the handler returns) and suppress the unused parameters.
- A family with the flash erased and programmed by another mechanism (page buffer, fast program, row program) hides it
  behind `ProgramSize` and the blocking / polling / interrupt styles.

---

## Public interface in short

| Group | Functions |
|---|---|
| Module management | `Flash_Get_ModuleVersion`, `Flash_Init`, `Flash_Deinit`, `Flash_Task`, `Flash_Get_DefaultConfig`, `Flash_Get_PeriphState` |
| Memory description | `Flash_Get_AreaCount`, `Flash_Get_AreaInfo` |
| Data access | `Flash_Get_Data`, `Flash_Get_BlankState`, `Flash_Set_EraseStart`, `Flash_Set_WriteStart` |
| Operation state | `Flash_Get_OperationState`, `Flash_Get_OperationError` |
| ECC | `Flash_Get_EccState`, `Flash_Set_EccStateClear`, `Flash_Handle_Nmi` |
| Data area configuration | `Flash_Set_AreaSize`, `Flash_Get_ConfigState`, `Flash_Set_ConfigApply` |

```c
flash_ModuleVersion_t Flash_Get_ModuleVersion  ( void );

flash_RequestState_t  Flash_Init               ( const flash_Config_t * const flashConfig );
flash_RequestState_t  Flash_Deinit             ( flash_PeriphId_t periphId );
void                  Flash_Task               ( void );
flash_RequestState_t  Flash_Get_DefaultConfig  ( flash_Config_t * const flashConfig );
flash_RequestState_t  Flash_Get_PeriphState    ( flash_PeriphId_t periphId, flash_FlagState_t * const periphState );

flash_RequestState_t  Flash_Get_AreaCount      ( flash_PeriphId_t periphId, flash_AreaId_t * const areaCount );
flash_RequestState_t  Flash_Get_AreaInfo       ( flash_PeriphId_t periphId, flash_AreaId_t areaId,
                                                 flash_AreaInfo_t * const areaInfo );

flash_RequestState_t  Flash_Get_Data           ( flash_PeriphId_t periphId, flash_Address_t address,
                                                 flash_DataByte_t * const data, flash_Size_t size );
flash_RequestState_t  Flash_Get_BlankState     ( flash_PeriphId_t periphId, flash_Address_t address,
                                                 flash_Size_t size, flash_FlagState_t * const blankState );
flash_RequestState_t  Flash_Set_EraseStart     ( flash_PeriphId_t periphId, flash_Address_t address, flash_Size_t size );
flash_RequestState_t  Flash_Set_WriteStart     ( flash_PeriphId_t periphId, flash_Address_t address,
                                                 const flash_DataByte_t * const data, flash_Size_t size );

flash_RequestState_t  Flash_Get_OperationState ( flash_PeriphId_t periphId, flash_OperationState_t * const operationState );
flash_RequestState_t  Flash_Get_OperationError ( flash_PeriphId_t periphId, flash_Error_t * const operationError );

flash_RequestState_t  Flash_Get_EccState       ( flash_PeriphId_t periphId, flash_EccState_t * const eccState );
flash_RequestState_t  Flash_Set_EccStateClear  ( flash_PeriphId_t periphId );
void                  Flash_Handle_Nmi         ( void );

flash_RequestState_t  Flash_Set_AreaSize       ( flash_PeriphId_t periphId, flash_AreaType_t areaType, flash_Size_t size );
flash_RequestState_t  Flash_Get_ConfigState    ( flash_PeriphId_t periphId, flash_ConfigState_t * const configState );
flash_RequestState_t  Flash_Set_ConfigApply    ( flash_PeriphId_t periphId );
```

Every function returns `FLASH_REQUEST_OK` / `FLASH_REQUEST_ERROR` and checks its parameters and the hardware state
before it touches a register. `FLASH_REQUEST_OK` of a start function means "finished" in blocking style and "started"
in polling and interrupt style.

### Public types

| Type | Meaning |
|---|---|
| `flash_RequestState_t`, `flash_FlagState_t`, `flash_ModuleVersion_t` | Common module types |
| `flash_PeriphId_t` | Controller (`FLASH_PERIPH_1`; the interface is the same as of the multi-instance modules) |
| `flash_Address_t`, `flash_Size_t`, `flash_DataByte_t`, `flash_AreaId_t` | Address, size in bytes, data byte, index in the area list |
| `flash_AreaType_t`, `flash_AreaAttr_t`, `flash_AreaInfo_t` | Area description |
| `flash_Window_t` | Access window: `Address`, `Size` |
| `flash_OperationStyle_t` | `FLASH_STYLE_BLOCKING`, `FLASH_STYLE_POLLING`, `FLASH_STYLE_INTERRUPT` |
| `flash_OperationType_t`, `flash_OperationState_t` | `FLASH_OPERATION_NONE / ERASE / WRITE`, `FLASH_STATE_IDLE / BUSY / FAILED` |
| `flash_Error_t` | Error bit mask: `FLASH_ERR_PROTECTION`, `FLASH_ERR_SEQUENCE`, `FLASH_ERR_NOT_ERASED`, `FLASH_ERR_OPERATION`, `FLASH_ERR_TIMEOUT`, `FLASH_ERR_ECC_CORRECTED`, `FLASH_ERR_ECC_DETECTED` |
| `flash_EccState_t` | ECC error mask and the address of the failing word |
| `flash_ConfigState_t` | `FLASH_CONFIG_APPLIED`, `FLASH_CONFIG_PENDING` (data area change waits for the reset) |
| `flash_OperationCallback_t`, `flash_ErrorCallback_t` | End of operation (operation, address, size, error mask), error (error mask, address) |
| `flash_Config_t` | `PeriphId`, `OperationStyle`, `IrqPrio`, `WindowCount`, `Windows[FLASH_WINDOW_MAX]`, `OperationCallback`, `ErrorCallback` |

---

## Usage

### Initialization with the data windows

```c
extern uint8_t _user_data_flash_start;      /* linker region USER_DATA */
extern uint8_t _user_data_flash_end;

flash_Config_t flashConfig;
flash_Size_t   userDataSize;

(void)Flash_Get_DefaultConfig( &flashConfig );  /* blocking, one window per existing DATA area */

userDataSize = (flash_Size_t)( &_user_data_flash_end - &_user_data_flash_start );

if ( 0u < userDataSize )                        /* data in the user flash (all families)       */
{
    flashConfig.Windows[ flashConfig.WindowCount ].Address = (flash_Address_t)&_user_data_flash_start;
    flashConfig.Windows[ flashConfig.WindowCount ].Size    = userDataSize;
    flashConfig.WindowCount++;
}

(void)Flash_Init( &flashConfig );
```

### Choosing the area for the data (storage layer, same code for all families)

```c
flash_AreaId_t   areaCount = 0u;
flash_AreaId_t   areaId    = 0u;
flash_AreaInfo_t area;
flash_AreaInfo_t dataArea;
flash_FlagState_t found    = FLASH_FLAG_INACTIVE;

(void)Flash_Get_AreaCount( FLASH_PERIPH_1, &areaCount );

for ( areaId = 0u; areaId < areaCount; areaId++ )
{
    (void)Flash_Get_AreaInfo( FLASH_PERIPH_1, areaId, &area );

    if ( ( FLASH_FLAG_INACTIVE == found ) && ( FLASH_AREA_DATA == area.Type ) )
    {
        dataArea = area;                        /* high-cycle data flash, if the family has it */
        found    = FLASH_FLAG_ACTIVE;
    }
}
/* if nothing was found, the window in the user flash is used: its area is looked up by the window address */
```

The storage layer reads `ProgramSize`, `EraseSize` and `Endurance` of the chosen area and builds its records on them -
the record size is rounded up to `ProgramSize`, the number of records in an erase unit follows from `EraseSize`.

### Erase, write, read

```c
flash_FlagState_t blank = FLASH_FLAG_INACTIVE;
flash_RequestState_t result;

result = Flash_Set_EraseStart( FLASH_PERIPH_1, dataArea.Address, dataArea.EraseSize );   /* one erase unit   */

if ( FLASH_REQUEST_OK == result )
{
    result = Flash_Set_WriteStart( FLASH_PERIPH_1, dataArea.Address, record, recordSize ); /* recordSize is a multiple of ProgramSize */
}

if ( FLASH_REQUEST_OK == result )
{
    result = Flash_Get_Data( FLASH_PERIPH_1, dataArea.Address, readBack, recordSize );    /* ECC checked read */
}
```

The memory of the DATA and USER areas is memory mapped, so it can be read directly through a pointer as well;
`Flash_Get_Data` adds the address validation and the ECC check.

### Interrupt style

```c
static void Storage_OperationCallback( flash_PeriphId_t periphId, flash_OperationType_t operation,
                                       flash_Address_t address, flash_Size_t size, flash_Error_t error )
{
    /* called from the FLASH interrupt: next step of the storage state machine */
}

flashConfig.OperationStyle    = FLASH_STYLE_INTERRUPT;
flashConfig.OperationCallback = Storage_OperationCallback;
```

### Enabling the high-cycle data flash (STM32H5)

The high-cycle data flash is disabled in the delivery state of the device. It is enabled by the option bytes - the module
does it from the firmware, e.g. at the first start:

```c
/* no DATA area in the area list yet */
(void)Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 4u * 6u * 1024u );  /* four erase units (sectors) */
(void)Flash_Set_ConfigApply( FLASH_PERIPH_1 );                                 /* option byte launch = system reset */
```

The sectors are taken from the end of the user flash, so they must be **blank** - the module refuses the request
otherwise - and the linker region `FLASH` of the application must not use them (see Linker, region `HIGH_CYCLE`).
After the reset the DATA area is in the area list.

---

## Behaviour and limits

- **Erase before write.** Flash can be written once after an erase. A write over a non-blank location is refused.
- **Interrupted write.** A word whose write was interrupted by a power loss may have an invalid ECC and report the
  uncorrectable error when it is read. On STM32H5 the double error raises **NMI**, `Flash_Handle_Nmi()` is the hook for
  the NMI handler and `Flash_Get_EccState` returns the failing address, so the storage layer can treat the record as
  invalid. A corrected single error is reported by `FLASH_ERR_ECC_CORRECTED` and the `ErrorCallback`. Records of the
  storage layer should carry a checksum and the layer should handle the power loss between the erase and the write.
- **Bus stall.** An erase or write of a bank can stall the code fetched from the same bank until the operation is
  finished (sector erase takes milliseconds). Data in the other bank than the interrupt handlers and the hot code
  avoids it - the `Bank` field of the area tells which one it is.
- **Clocks and latency** of the flash are owned by the RCC module; the module only requests the flash interface clock
  from it. The FLASH interrupt is configured through the NVIC module.
- **TrustZone, option bytes, bank swap** are not part of the first version (the module works with TrustZone
  disabled), see the follow-up story AB#1042.
- **OTP** is irreversible: the write is tested by unit tests only.

### Open points (resolved during the implementation, written to the tickets)

1. Geometry of the STM32H5 high-cycle data window on hardware: erase unit (6 KB of an 8 KB sector), address of the bank 2
   window, area with fewer sectors than the maximum, distribution of the sectors over the banks, number of sectors per
   bank on the H5E lines (the ST CMSIS header says 192 KB of high-cycle data, the ST HAL allows 8 sectors per bank as
   on the other lines except H5F5).
2. The Linker region `HIGH_CYCLE` has 48 KB (96 KB on H5E / H5F) - one bank; the area list reports both banks.
3. Ownership of `NMI_Handler` (Startup / Core) and the call of `Flash_Handle_Nmi`.
4. Whether the instruction cache can return stale content for a flash area that was just erased or written; if so, the
   module has to invalidate it (there is no MCAL module of the cache yet).

---

## Repository layout (family branches; master holds this README and the license)

| Path | Content |
|---|---|
| `Flash_Port.h`, `Flash_Types.h` | Public interface |
| `Flash*.c`, `Flash.h` | Implementation (family branches) |
| `CMakeLists.txt` | Library `Flash_Lib` of the EmBi platform |
| `Tests/UnitTests` | Unit tests (Unity + CMock + RegMem) |
| `Tests/IntegrationTests` | Integration tests on the target (probe-rs) |

---

## License

This project is licensed under the **Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**.

You are free to use, modify, and share this work for **non-commercial purposes**, provided appropriate credit is given.

See [LICENSE.md](LICENSE.md) for full terms or visit [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/).

---

## Authors

- **Mr.Nobody** - [embedbits.com](https://embedbits.com)

Contributions are welcome! Please open a pull request.
