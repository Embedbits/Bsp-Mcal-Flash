# Flash Peripheral Driver

This module provides an abstraction layer for the **internal flash memory controller** of STM32 MCUs - erase, program
and read of the flash by the application at run time, primarily for **runtime data** (settings, calibration values,
counters, event logs) that must survive a power cycle. It is a low-level MCAL module: the memory is described as a list
of *areas* and the operations are independent of the area they work on, so a middleware above the module (key-value
store, EEPROM emulation, log) is written once and works on every family.

The public interface (`Flash_Port.h`, `Flash_Types.h`) is **identical for all supported families**. The implementation
for a family lives in its own branch (`Dev/<Family>` for development, `Releases/<Family>` for the released versions), so
a middleware or an application written against the interface does not change when the MCU family changes.

This README is the landing page and the **specification of the interface** (master). The family branches contain their
own README with everything that is specific to the family: supported lines, the areas of every line, configuration of
the data area, ECC behaviour, integration with the Linker and the status of the implementation.

---

## Intent and purpose

- Many applications must keep a small amount of data in non-volatile memory and change it at run time. The internal
  flash is the cheapest place for it, but the flash has an erase unit much bigger than the data, a limited number of
  erase cycles and a program unit that differs from family to family.
- Some families have a **dedicated high-cycle data flash** - a part of the flash with a much higher endurance (STM32H5:
  100 000 erase cycles instead of 10 000) and a small program unit, made exactly for runtime data. **Not every MCU has
  it**; the other families keep the data in ordinary user flash sectors reserved by the linker (region `USER_DATA`).
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
| `FLASH_AREA_DATA` | Dedicated high-cycle data flash. Higher endurance, small program unit. Exists only on families that have it and, if it has to be enabled (STM32H5: option bytes), only when it is enabled. | Frequently changed runtime data |
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
| `Attributes` | Bit mask: `FLASH_AREA_ATTR_ERASE_REQUIRED`, `FLASH_AREA_ATTR_ECC`, `FLASH_AREA_ATTR_WRITE_ONCE`, `FLASH_AREA_ATTR_CONFIGURABLE` (size can be changed by `Flash_Set_AreaSize`), `FLASH_AREA_ATTR_PROGRAM_BEFORE_READ` (an erased location can not be read before it is programmed - STM32H5 high-cycle data flash) |

Rules valid in every family:

1. **Erase** works with whole erase units: `address` and `size` are multiples of `EraseSize` of the area.
2. **Write** works with whole program units: `address` and `size` are multiples of `ProgramSize` of the area. The
   caller pads the record. The erased value is `FLASH_ERASED_VALUE` (0xFF) in every family.
3. A location can be written **once after an erase**. The module checks that the destination is blank and refuses the
   write otherwise (`FLASH_ERR_NOT_ERASED`), no register is touched. An area with the attribute
   `FLASH_AREA_ATTR_PROGRAM_BEFORE_READ` can not be read while it is erased (the ECC reports an error), so the blank
   check by reading is not possible there: `Flash_Get_BlankState` returns an error, the destination of a write is not
   checked and the storage layer writes a location before it reads it.
4. The storage layer takes the **first DATA area** of the list if there is one, otherwise a user window (see below).

---

## Access windows

`Flash_Config_t` contains a list of access windows (address, size). `Flash_Set_EraseStart` and `Flash_Set_WriteStart`
are accepted only inside a window; reading and the blank check work everywhere inside an area.

- The **default configuration** (`Flash_Get_DefaultConfig`) contains one window for every DATA area that exists, so on
  a family with the high-cycle data flash enabled the data area works out of the box.
- The application adds windows for data in the user flash. In the EmBi platform the Linker module reserves them: the
  region `USER_DATA` at the end of the flash (size from the CMake variable `USER_DATA_SIZE`, a multiple of the erase
  unit) exists in the linker script of every family and its bounds are exported as the symbols `_user_data_start`,
  `_user_data_length` and `_user_data_end` (the symbols `_user_data_flash_start` / `_end` are different: they bound only
  the objects placed in the section `.user_data_flash`). The export is part of the Linker task of every family story.
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

Every family has its own story with the tasks of the implementation, the Linker and the tests, and its own branches
`Dev/<Family>` and `Releases/<Family>` cut from this master.

| Family | Branch | Story | Status |
|---|---|---|---|
| STM32H5 (all lines, with the high-cycle data flash) | `Dev/STM32H5` | AB#1031 | in progress |
| STM32F4 | `Dev/STM32F4` | AB#1044 | planned |
| STM32F7 | `Dev/STM32F7` | AB#1052 | planned |
| STM32G4 | `Dev/STM32G4` | AB#1063 | planned |
| STM32H7 (including H7R / H7S) | `Dev/STM32H7` | AB#1073 | planned |
| STM32L4 / L4+ | `Dev/STM32L4` | AB#1081 | planned |
| STM32U5 | `Dev/STM32U5` | AB#1089 | planned |

---

## Same interface in every family (design basis)

The interface was designed against the flash of all families of the platform, so every family fills the same area list
with its own values.

| Family | User flash erase unit | User flash program unit | Banks | Data flash | OTP |
|---|---|---|---|---|---|
| STM32H5 | 8 KB sector | 16 B (128 bit); 2 B in DATA and OTP | 1 - 2 | **yes** (EDATA, 100 000 cycles); none on H503 | 2 KB |
| STM32U5 | 8 KB page | 16 B (128 bit) | 2 | no | 512 B |
| STM32H7 | 128 KB or 8 KB sector (by line) | 16 or 32 B (flash word, by line) | 1 - 2 | no | see family branch |
| STM32L4, STM32G4 | 2 KB page (L4+: 8 KB) | 8 B (64 bit) (L4+: 16 B) | 1 - 2 | no | see family branch |
| STM32F4 | 16 / 64 / 128 KB sectors (non-uniform: one area per uniform group) | 1 - 8 B (by supply voltage) | 1 - 2 | no | 528 B |
| STM32F7 | 32 / 128 / 256 KB sectors (non-uniform) | 1 - 8 B | 1 - 2 | no | up to 1 KB |

What it means for the interface:

- **No DATA area is not an error.** The list simply has no `FLASH_AREA_DATA`, the storage layer uses a user window. The
  big erase units (H7 / F4 / F7) are visible in `EraseSize` (the storage layer then needs more space for the same
  number of records), the program unit in `ProgramSize`.
- `Flash_Set_AreaSize`, `Flash_Get_ConfigState`, `Flash_Set_ConfigApply` (configuration of the data area) and
  `Flash_Handle_Nmi` (ECC double error that raises NMI) have a meaning only where the hardware has it. In the other
  families they stay in the interface, return `FLASH_REQUEST_ERROR` (the handler returns) and suppress the unused
  parameters. The families without ECC return `FLASH_REQUEST_ERROR` from the ECC functions as well.
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

`Flash_Set_AreaSize` / `Flash_Set_ConfigApply` change the size of a **configurable** area (attribute
`FLASH_AREA_ATTR_CONFIGURABLE`, today the high-cycle data flash of STM32H5): the request is stored, it becomes valid
after `Flash_Set_ConfigApply` (reset of the device) and `Flash_Get_ConfigState` reports `FLASH_CONFIG_PENDING` until
then. The module refuses sectors that are not blank. See the README of the family.

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

The examples are the same for every family.

### Initialization with the data windows

```c
extern uint8_t _user_data_start;            /* linker region USER_DATA (bounds exported by the Linker module) */
extern uint8_t _user_data_end;

flash_Config_t flashConfig;
flash_Size_t   userDataSize;

(void)Flash_Get_DefaultConfig( &flashConfig );  /* blocking, one window per existing DATA area */

userDataSize = (flash_Size_t)( &_user_data_end - &_user_data_start );

if ( 0u < userDataSize )                        /* data in the user flash (all families)       */
{
    flashConfig.Windows[ flashConfig.WindowCount ].Address = (flash_Address_t)&_user_data_start;
    flashConfig.Windows[ flashConfig.WindowCount ].Size    = userDataSize;
    flashConfig.WindowCount++;
}

(void)Flash_Init( &flashConfig );
```

### Choosing the area for the data (storage layer)

```c
flash_AreaId_t    areaCount = 0u;
flash_AreaId_t    areaId    = 0u;
flash_AreaInfo_t  area;
flash_AreaInfo_t  dataArea;
flash_FlagState_t found     = FLASH_FLAG_INACTIVE;

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

---

## Behaviour and limits

- **Erase before write.** Flash can be written once after an erase. A write over a non-blank location is refused.
- **Interrupted write.** A word whose write was interrupted by a power loss may have an invalid ECC (families with ECC)
  and report the uncorrectable error when it is read. Where the double error raises **NMI** (e.g. STM32H5),
  `Flash_Handle_Nmi()` is the hook for the NMI handler and `Flash_Get_EccState` returns the failing address, so the
  storage layer can treat the record as invalid. A corrected single error is reported by `FLASH_ERR_ECC_CORRECTED` and
  the `ErrorCallback`. Records of the storage layer should carry a checksum and the layer should handle the power loss
  between the erase and the write.
- **Bus stall.** An erase or write of a bank can stall the code fetched from the same bank until the operation is
  finished (a sector erase takes milliseconds). Data in the other bank than the interrupt handlers and the hot code
  avoids it - the `Bank` field of the area tells which one it is.
- **Caches.** An erase or write must not leave stale content in the flash caches of the family (data cache, ART
  accelerator, instruction cache); how it is guaranteed is documented in the family branch.
- **Clocks and latency** of the flash are owned by the RCC module; the module only requests the flash interface clock
  from it. The FLASH interrupt is configured through the NVIC module.
- **Not part of the first version:** option bytes other than the configuration of the data area (write protection, boot
  address, ...), bank swap, TrustZone secure operations (the module works with TrustZone disabled), external flash
  memories.
- **OTP** is irreversible: the write is tested by unit tests only.

---

## Repository layout and branches

The master holds this README and the license. The family branches (`Dev/<Family>`, `Releases/<Family>`) hold:

| Path | Content |
|---|---|
| `README.md` | README of the family: lines, areas of every line, specifics, status |
| `Flash_Port.h`, `Flash_Types.h` | Public interface (identical in all families) |
| `Flash*.c`, `Flash.h` | Implementation of the family |
| `CMakeLists.txt` | Library `Flash_Lib` of the EmBi platform |
| `Tests/UnitTests` | Unit tests (Unity + CMock + RegMem) |
| `Tests/IntegrationTests` | Integration tests on the target (probe-rs) |

A change of the public interface is made in all families.

---

## License

This project is licensed under the **Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**.

You are free to use, modify, and share this work for **non-commercial purposes**, provided appropriate credit is given.

See [LICENSE.md](LICENSE.md) for full terms or visit [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/).

---

## Authors

- **Mr.Nobody** - [embedbits.com](https://embedbits.com)

Contributions are welcome! Please open a pull request.
