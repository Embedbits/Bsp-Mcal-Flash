# Flash Peripheral Driver - STM32H5

Flash MCAL module for the **STM32H5** family: erase, program and read of the internal flash by the application at run
time, primarily for **runtime data**. STM32H5 has a dedicated **high-cycle data flash** (EDATA, 100 000 erase cycles,
16-bit programming with its own ECC) that is made exactly for this purpose, so the module offers it as the area
`FLASH_AREA_DATA`; on the lines and in the configurations where it is not enabled the data live in the user flash
reserved by the linker (region `USER_DATA`).

The public interface (`Flash_Port.h`, `Flash_Types.h`) is the same as in all other families. The concept, the
interface specification, the usage and the comparison of the families are in the README of the master branch of the
repository (landing page); this README describes what is specific to STM32H5.

> **Status: functionality complete, tested by unit tests only** (user story AB#1031, branch `Dev/STM32H5`). All
> functions of the interface are implemented (blocking, polling and interrupt style) and covered by unit tests on all
> STM32H5 lines; the integration tests on hardware are the remaining step, see
> [Implementation status](#implementation-status).

---

## Supported lines

All STM32H5 lines have the same flash controller (two banks of 8 KB sectors). The device differences are handled by the
preprocessor conditions of the ST CMSIS device headers and by the flash size register.

| Line | User flash | Sectors per bank | High-cycle data flash (maximum) |
|---|---|---|---|
| STM32H503 | 128 KB | 8 | none |
| STM32H523, STM32H533 | 512 KB | 32 | 8 sectors per bank, 96 KB |
| STM32H543, STM32H553 | 1 MB | 64 | 8 sectors per bank, 96 KB |
| STM32H562, STM32H563, STM32H573 | 2 MB | 128 | 8 sectors per bank, 96 KB |
| STM32H5E4, STM32H5E5, STM32H5F4, STM32H5F5 | 4 MB | 256 | 16 sectors per bank, 192 KB |

The maximum of the high-cycle data sectors of a bank is the width of the `EDATA_STRT` field of the option bytes
(3 bits, resp. 4 bits on the H5E / H5F lines). The ST HAL uses 8 sectors for H5E4 / H5E5 / H5F4 as well - that is
inconsistent with the CMSIS header of the same package and is not followed (AB#1060, AB#1034).

---

## Areas of the device

The list is built from the flash size register, the configuration of the high-cycle data flash (`EDATA1R_CUR`,
`EDATA2R_CUR`) and the CMSIS constants; it reflects the actual configuration of the device.

| Order | Area | Address | Size | Erase unit | Program unit | Endurance |
|---|---|---|---|---|---|---|
| 0 | `USER` bank 1 | `0x08000000` | bank size - sectors taken by the data flash | 8 KB | 16 B (128 bit) | 10 000 |
| 1 | `USER` bank 2 | `0x08000000` + bank size | bank size - sectors taken by the data flash | 8 KB | 16 B | 10 000 |
| 2 | `DATA` bank 1 (if configured) | `0x09000000` + (max - N) x 6 KB | N sectors x 6 KB | 6 KB | 2 B (16 bit) | 100 000 |
| 3 | `DATA` bank 2 (if configured) | `0x09000000` + window of a bank + (max - N) x 6 KB | N sectors x 6 KB | 6 KB | 2 B | 100 000 |
| last | `OTP` | `0x08FFF000` | 2 KB | - (write once) | 2 B | - |

- The **high-cycle data flash** takes the *last* 1 .. N sectors of a bank (N = 8, 16 on the H5E / H5F lines) from the user
  flash, the number is set by the option bytes `EDATAxR` (enable bit + `STRT` = sectors - 1). One 8 KB sector provides
  6 KB of data: the window of a bank is `FLASH_EDATA_SIZE / 2` and the erase unit is the window divided by the maximum
  sectors per bank (max). The window of the bank 2 follows the window of the bank 1. The N configured sectors are the
  **last** N sectors of the window of the bank (ST example `FLASH_EDATA_EraseProgram`: 1 sector = `0x0900A800`, 8 sectors
  = `0x09000000` on H563); the rest of the window is not valid (a read is a bus error). A sector of the area is erased
  by the number of the flash sector at the end of the bank (sectors per bank - N + k) and the bank of the window.
- The DATA area is **configurable** (`FLASH_AREA_ATTR_CONFIGURABLE`): `Flash_Set_AreaSize` + `Flash_Set_ConfigApply`
  program the option bytes and restart the device, see below. It is disabled in the delivery state of the device.
- The device always has two banks (the STM32H503 has 2 banks of 64 KB, 8 sectors each).
- The module works with TrustZone disabled (non-secure alias of the registers `FLASH_NS`).

---

## Enabling the high-cycle data flash

```c
/* no DATA area in the area list yet: take four sectors (4 x 6 KB of data) */
(void)Flash_Set_AreaSize( FLASH_PERIPH_1, FLASH_AREA_DATA, 4u * 6u * 1024u );
(void)Flash_Set_ConfigApply( FLASH_PERIPH_1 );      /* option byte launch = system reset */
```

After the reset the area is in the list and, because the default configuration contains a window for every DATA area,
it is writable without any other step. The sectors are taken from the end of the bank, so

- they must be **blank** - the module refuses the request otherwise,
- the region `FLASH` of the application must not use them: set the CMake variable `HIGH_CYCLE_SECTORS` of the Linker
  module to the same number, see below. The module prefers the **bank 2** (the end of the flash) - the code of the
  application is in the bank 1 and the linker script supports the reserved sectors at the end of the flash only.

---

## Behaviour of the high-cycle data flash

- **Program before read.** An erased location that was not programmed can not be read: the ECC error raises NMI
  (reference manual, ST community: Init - Erase - Program - Read). Therefore the area has the attribute
  `FLASH_AREA_ATTR_PROGRAM_BEFORE_READ`: `Flash_Get_BlankState` of the area returns an error, the destination of a
  write is not checked (the controller reports an error) and the storage layer has to program (write) a word before it
  reads it - for example a record store reads only the records it has written.
- **Write.** Half words (16 bit) are programmed one by one, `ProgramSize` is 2 B.
- **Not cacheable.** All AHB memory is cacheable, the window of the high-cycle data flash has to be non-cacheable
  (ST example: an MPU region over the window with the attributes 0 = device memory). There is no MPU MCAL module, the
  application configures the MPU.
- **Reset of the configuration.** `Flash_Set_AreaSize` programs the option bytes `EDATA1R` / `EDATA2R`, the new size is
  valid after the reset: `Flash_Get_ConfigState` is `FLASH_CONFIG_PENDING` until then, `Flash_Set_ConfigApply` resets the
  device through the NVIC module. The size is distributed over the banks, the bank 2 first (the end of the flash).

---

## Linker integration

The Linker module (`Bsp/Linker`, branch `Dev/STM32H5` / the release that contains AB#1057) provides:

| Item | Meaning |
|---|---|
| Region `USER_DATA`, CMake variable `USER_DATA_SIZE` | Part of the user flash at the end of `FLASH` reserved for runtime data, a multiple of the 8 KB sector |
| Region `HIGH_CYCLE` | The high-cycle data window of both banks (96 KB, 192 KB on H5E / H5F, 0 on H503) |
| CMake variable `HIGH_CYCLE_SECTORS` | Number of sectors at the end of the flash taken by the high-cycle data flash (`FLASH` and `USER_DATA` end before them) |
| Symbols `_user_data_start / _length / _end`, `_high_cycle_start / _length / _end`, `_high_cycle_sectors` | Bounds of the regions and the reserved sector count |

The access window of the application is then `_user_data_start` .. `_user_data_end` (see the usage in the README of the
master branch); the DATA area needs no window to be declared.

---

## ECC

The flash has a 9-bit ECC of every 128-bit word (the high-cycle data flash: of every 16-bit half word). A corrected
single error is flagged (`ECCC`), an uncorrectable double error (`ECCD`) raises **NMI**.

- **Read.** `Flash_Get_Data` (and the blank check) masks the NMI of the double error (`SBS_ECCNMIR`) for the time of the
  read and reports the error as `FLASH_REQUEST_ERROR` (the buffer is not valid) instead of the NMI; a corrected error
  does not fail the read. A word whose programming was interrupted by a power loss is expected to report `ECCD` when
  it is read - the storage layer above the module treats such a record as invalid. A range with an uncorrectable error
  is never reported as blank (the write is refused), which protects the OTP area.
- **State.** The errors are latched with the decoded address (`Flash_Get_EccState`, the address decode follows the ST
  HAL: user flash, OTP and data flash; 0 for the system flash and the option byte keys) until
  `Flash_Set_EccStateClear`.
- **NMI.** `Flash_Handle_Nmi()` is the hook for the `NMI_Handler` of the application: it clears the flag (the NMI would
  come again), latches the error and calls the error callback.
- **Interrupt style.** The interrupt of the corrected error (`ECCIE`) is enabled and reported by the error callback
  from the flash interrupt; in the polling style `Flash_Task()` does it.

## OTP

The OTP area is written by 16 bits and only once: it is never erased, a destination that is not blank is refused
(`FLASH_ERR_NOT_ERASED`) and the area has to be given as an **access window** of the configuration (it is not in the
default configuration, so the OTP can not be written by accident). The lock of the OTP blocks (`OTPBLR`) is not
touched. The write is irreversible - it is tested by the unit tests only.

---

## Implementation status

| Function | Status | Task |
|---|---|---|
| `Flash_Get_ModuleVersion`, `Flash_Get_DefaultConfig` | done | AB#1034 |
| `Flash_Get_AreaCount`, `Flash_Get_AreaInfo` | done | AB#1034 |
| `Flash_Init`, `Flash_Deinit`, `Flash_Get_PeriphState`, `Flash_Get_Data`, `Flash_Get_BlankState`, `Flash_Set_EraseStart`, `Flash_Set_WriteStart` (user flash) | done | AB#1035 |
| Access windows (`Flash_Init` check, erase / write check) | done | AB#1039 |
| `Flash_Set_AreaSize`, `Flash_Get_ConfigState`, `Flash_Set_ConfigApply`, DATA area erase / write | done | AB#1036 |
| OTP write | done | AB#1037 |
| `Flash_Get_OperationState`, `Flash_Get_OperationError` | done | AB#1035 |
| `Flash_Task`, `Flash_Get_EccState`, `Flash_Set_EccStateClear`, `Flash_Handle_Nmi`, polling / interrupt style, callbacks, ECC protected read | done | AB#1038 |
| Unit tests | 98 tests (all STM32H5 lines): memory map, default configuration, initialization, access windows, read, blank check, erase, write, data flash, configuration of the data flash, ECC, polling and interrupt style, OTP | AB#1040 |
| Integration tests | written for the memory map and the user flash, not run on hardware; data flash, ECC and OTP read tests to be added with the boards | AB#1041 |

---

## Open points (resolved during the implementation, written to the tickets)

1. Behaviour of the high-cycle data flash on hardware (the layout follows the ST example): erase by the sector numbers
   at the end of the bank, programming of half words, the load of the option bytes (programming of the option bytes
   and the reset), the NMI on a read of an erased location.
2. Readability of the flash size register and of the option byte registers before `Flash_Init` (the area list is built
   without the initialization of the module).
3. Ownership of `NMI_Handler` (Startup / Core) and the call of `Flash_Handle_Nmi`.
4. Whether the instruction cache can return stale content for a flash area that was just erased or written; if so, the
   module has to invalidate it (there is no MCAL module of the cache yet).
5. The ST LL has no functions for the flash erase and program - the module uses the registers of the CMSIS header
   (private functions of the module); to be moved to the RAL port when the Ral gets a flash LL.
6. The SBS register `SBS_ECCNMIR` (mask of the ECC NMI) is accessed directly (no MCAL module of the SBS); whether the
   mask works as described for the read of an erased location of the data flash is verified on hardware.
7. The non-cacheable MPU region over the window of the data flash (no MCAL module of the MPU).

---

## Repository layout (branch `Dev/STM32H5`)

| Path | Content |
|---|---|
| `Flash_Port.h`, `Flash_Types.h` | Public interface (identical in all families) |
| `Flash.c`, `Flash_Map.c`, `Flash.h` | Implementation (module functions, memory map) |
| `CMakeLists.txt` | Library `Flash_Lib` of the EmBi platform |
| `Tests/UnitTests` | Unit tests (Unity + RegMem); the flash memory is emulated at the MCU addresses by `Ut_FlashMem.c` |
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
