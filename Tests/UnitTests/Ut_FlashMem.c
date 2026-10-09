/**
 * \author Mr.Nobody
 * \file Ut_FlashMem.c
 * \ingroup Flash
 * \brief Emulation of the STM32H5 flash memory in the host memory for unit tests.
 *
 * Maps UTFLASHMEM_SIZE bytes of the host memory at UTFLASHMEM_BASE before main() is executed
 * (like the register memory emulation of the unit testing framework does for the peripherals).
 */

/* ============================= INCLUDES =================================== */
#include "Ut_FlashMem.h"                    /* Self include                   */
#include <string.h>                         /* memset                         */
#include <stdint.h>                         /* uintptr_t                      */

#if defined(_WIN32)
#include <windows.h>                        /* VirtualAlloc                   */
#else
#include <sys/mman.h>                       /* mmap                           */
#endif
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Address of the flash size register (16 bits, size in KB) */
#define UTFLASHMEM_SIZE_REG_ADDR            ( 0x08FFF80Cu )

/** Value of the erased memory */
#define UTFLASHMEM_ERASED                   ( 0xFFu )

/* ========================== LOCAL VARIABLES =============================== */

/** Non-zero if the memory is mapped at the required address */
static uint32_t utFlashMem_Mapped = 0u;

/* ======================= FORWARD DECLARATIONS ============================= */

static void utFlashMem_AutoInit ( void ) __attribute__((constructor));

/* ======================== EXPORTED FUNCTIONS ============================== */

uint32_t UtFlashMem_Reset( void )
{
    if ( 0u != utFlashMem_Mapped )
    {
        (void)memset( (void *)(uintptr_t)UTFLASHMEM_BASE, (int)UTFLASHMEM_ERASED, UTFLASHMEM_SIZE );
    }

    return (utFlashMem_Mapped);
}


void UtFlashMem_Set_SizeKb( uint16_t sizeKb )
{
    if ( 0u != utFlashMem_Mapped )
    {
        *(volatile uint16_t *)(uintptr_t)UTFLASHMEM_SIZE_REG_ADDR = sizeKb;
    }
}

/* ========================== LOCAL FUNCTIONS =============================== */

static void utFlashMem_AutoInit( void )
{
    void * reqAddr = (void *)(uintptr_t)UTFLASHMEM_BASE;
    void * mapAddr;

#if defined(_WIN32)
    mapAddr = VirtualAlloc( reqAddr, UTFLASHMEM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
#else
    mapAddr = mmap( reqAddr, UTFLASHMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0 );

    if ( MAP_FAILED == mapAddr )
    {
        mapAddr = NULL;
    }
#endif

    if ( mapAddr == reqAddr )
    {
        utFlashMem_Mapped = 1u;
        (void)memset( reqAddr, (int)UTFLASHMEM_ERASED, UTFLASHMEM_SIZE );
    }
}
