#include <M68k.h>
#include <support.h>
#include <config.h>
#ifdef PISTORM
#ifndef PISTORM32
#define PS_PROTOCOL_IMPL
#include "pistorm/ps_protocol.h"
#endif
#endif

extern struct List ICache[EMU68_HASHSIZE];
void M68K_LoadContext(struct M68KState *ctx);
void M68K_SaveContext(struct M68KState *ctx);

uint8_t ariv_enabled = 0;
uint8_t hrtmon_enabled = 0;
uint8_t debounce_nmi = 0;

static inline void CallARMCode()
{
    register void *ARM asm("x12");
    asm volatile("":"=r"(ARM));
    void (*ptr)() = (void*)ARM;
    ptr();
}

static inline struct M68KTranslationUnit *FindUnit()
{
    register uint16_t *PC asm("x18");
    
    /* Perform search */
    uint32_t hash = (uint32_t)(uintptr_t)PC;
    struct List *bucket = &ICache[(hash >> EMU68_HASHSHIFT) & EMU68_HASHMASK];
    struct M68KTranslationUnit *node;
    
    /* Go through the list of translated units */
    ForeachNode(bucket, node)
    {
        /* Force reload of PC*/
        asm volatile("":"=r"(PC));

        /* Check if unit is found */
        if (node->mt_M68kAddress == PC)
        {
#if 0
            /* Move node to front of the list */
            REMOVE(&node->mt_HashNode);
            ADDHEAD(bucket, &node->mt_HashNode);
#elif 0
            struct Node *prev = node->mt_HashNode.ln_Pred;
            struct Node *succ = node->mt_HashNode.ln_Succ;
            struct Node *prevprev = prev->ln_Pred;
            
            /* If node is not head, then move it one level up */
            if (prevprev != NULL)
            {
                node->mt_HashNode.ln_Pred = prevprev;
                node->mt_HashNode.ln_Succ = prev;

                prevprev->ln_Succ = &node->mt_HashNode;

                prev->ln_Succ = succ;
                prev->ln_Pred = &node->mt_HashNode;

                succ->ln_Pred = prev;
            }
#endif                   
            return node;    
        }
    }

    return NULL;
}

#ifdef PISTORM
#ifndef PISTORM32

extern volatile unsigned char bus_lock;

static inline int GetIPLLevel()
{
    volatile uint32_t *gpio = (void *)0xf2200000;

    *(gpio + 7) = LE32(REG_STATUS << PIN_A0);
    *(gpio + 7) = LE32(1 << PIN_RD);
    *(gpio + 7) = LE32(1 << PIN_RD);
    *(gpio + 7) = LE32(1 << PIN_RD);
    *(gpio + 7) = LE32(1 << PIN_RD);

    unsigned int value = LE32(*(gpio + 13));

    *(gpio + 10) = LE32(0xffffec);

    return (value >> 21) & 7;
}
#endif
#else
static inline int GetIPLLevel() { return 0; }
#endif

static inline uint16_t *getLastPC()
{
    uint16_t *lastPC;
    asm volatile("mrs %0, TPIDR_EL1":"=r"(lastPC));
    return lastPC;
}

static inline struct M68KState *getCTX()
{
    struct M68KState *ctx;
    asm volatile("mrs %0, TPIDRRO_EL0":"=r"(ctx));
    return ctx;
}

static inline uint32_t getSR()
{
    uint32_t sr;
    asm volatile("mrs %0, TPIDR_EL0":"=r"(sr));
    return sr;
}

static inline void setLastPC(uint16_t *pc)
{
    asm volatile("msr TPIDR_EL1, %0"::"r"(pc));
}

static inline void setSR(uint32_t sr)
{
    asm volatile("msr TPIDR_EL0, %0"::"r"(sr));
}

void MainLoop()
{
    register uint16_t *PC asm("x18");
    register void *ARM asm("x12");
    uint16_t *LastPC;
    struct M68KState *ctx = getCTX();
    
    M68K_LoadContext(ctx);

    asm volatile("mov v28.d[0], xzr");

    /* The JIT loop is running forever */
    while(1)
    {   
        /* Load m68k context and last used PC counter into temporary register */ 
        LastPC = getLastPC();
        ctx = getCTX();

        /* If (unlikely) there was interrupt pending, check if it needs to be processed */
        if (unlikely(ctx->INT32 != 0))
        {
            uint32_t SR, SRcopy;
            int level = 0;
            uint32_t vector;
            uint32_t vbr;

            /* Find out requested IPL level based on ARM state and real IPL line */
            if (ctx->INT.ARM_err)
            {
                level = 7;
                ctx->INT.ARM_err = 0;
            }
            else
            {
                if (ctx->INT.ARM)
                {
                    level = 6;
                    ctx->INT.ARM = 0;
                }
#ifdef PISTORM32
                /* On PiStorm32 IPL level is obtained by second CPU core from the GPIO directly */
                if (ctx->INT.IPL > level)
                {
                    level = ctx->INT.IPL;
                }    
#else
                /* On classic pistorm we need to obtain IPL from PiStorm status register */
                if (ctx->INT.IPL)
                {
                    int ipl_level;

#if PISTORM_WRITE_BUFFER
                    while(__atomic_test_and_set(&bus_lock, __ATOMIC_ACQUIRE)) { asm volatile("yield"); }
#endif

                    ipl_level = GetIPLLevel();

#if PISTORM_WRITE_BUFFER
                    __atomic_clear(&bus_lock, __ATOMIC_RELEASE);
#endif
                    /* Obtained IPL level higher than until now detected? */
                    if (ipl_level > level)
                    {
                        level = ipl_level;
                    }
                }           
#endif
            }

            /* Get SR and test the IPL mask value */
            SR = getSR();

            int IPL_mask = (SR & SR_IPL) >> SRB_IPL;

            /* Any unmasked interrupts? Proceess them */
            if ((level == 7 && !debounce_nmi) || level > IPL_mask)
            {
                register uint64_t sp asm("r29");

                if (likely((SR & SR_S) == 0))
                {
                    /* If we are not yet in supervisor mode, the USP needs to be updated */
                    asm volatile("mov v31.S[1], %w0"::"r"(sp));

                    /* Load eiter ISP or MSP */
                    if (unlikely((SR & SR_M) != 0))
                    {
                        asm volatile("mov %w0, v31.S[3]":"=r"(sp));
                    }
                    else
                    {
                        asm volatile("mov %w0, v31.S[2]":"=r"(sp));
                    }
                }
                
                SRcopy = SR;
                /* Swap C and V flags in the copy */
                if ((SRcopy & 3) != 0 && (SRcopy & 3) != 3)
                SRcopy ^= 3;
                vector = 0x60 + (level << 2);

                /* Set supervisor mode */
                SR |= SR_S;

                /* Clear Trace mode */
                SR &= ~(SR_T0 | SR_T1);

                /* Insert current level into SR */
                SR &= ~SR_IPL;
                SR |= ((level & 7) << SRB_IPL);

                /* Push exception frame */
                asm volatile("strh %w1, [%0, #-8]!":"=r"(sp):"r"(SRcopy),"0"(sp));
                asm volatile("str %w1, [%0, #2]"::"r"(sp),"r"(PC));
                asm volatile("strh %w1, [%0, #6]"::"r"(sp),"r"(vector));

                /* Set SR */
                setSR(SR);

                /* Get VBR */
                vbr = ctx->VBR;

                if (hrtmon_enabled && level==7)
                {
                    asm volatile("movz    %w0, #0x000c":"=r"(PC)); 
                    asm volatile("movk    %w0, #0x00a1, lsl #16 ":"=r"(PC)); 
                }
                else if (ariv_enabled && level==7)
                {
                    asm volatile("ldr %w0, [%1, %2]":"=r"(PC):"r"(0xa10000),"r"(vector)); 
                }
                else
                {
                    /* Load PC */
                    asm volatile("ldr %w0, [%1, %2]":"=r"(PC):"r"(vbr),"r"(vector)); 
                }
            }

            /* All interrupts masked or new PC loaded and stack swapped, continue with code execution */
        }

        /* Check if JIT cache is enabled */
        uint32_t cacr;
        asm volatile("mov %w0, v31.s[0]":"=r"(cacr));

        if (likely(cacr & CACR_IE))
        {   
            /* Force reload of PC*/
            asm volatile("":"=r"(PC));

            /* The last PC is the same as currently set PC? */
            if (LastPC == PC)
            {
                asm volatile("":"=r"(ARM));
                /* Jump to the code now */
                CallARMCode();
                continue;
            }
            else
            {
                /* Find unit in the hashtable based on the PC value */
                struct M68KTranslationUnit *node = FindUnit();

                /* Unit exists ? */
                if (node != NULL)
                {
                    /* Store m68k PC of corresponding ARM code in TPIDR_EL1 */
                    asm volatile("msr TPIDR_EL1, %0"::"r"(PC));

                    /* This is the case, load entry point into x12 */
                    ARM = node->mt_ARMEntryPoint;
                    asm volatile("":"=r"(ARM):"0"(ARM));
                    
                    CallARMCode();

                    /* Go back to beginning of the loop */
                    continue;
                }

                /* If we are that far there was no JIT unit found */
                asm volatile("":"=r"(PC));
                uint16_t *copyPC = PC;
                M68K_SaveContext(ctx);
                /* Get the code. This never fails */
                node = M68K_GetTranslationUnit(copyPC);
                /* Load CPU context */
                M68K_LoadContext(getCTX());
                asm volatile("msr TPIDR_EL1, %0"::"r"(PC));
                /* Prepare ARM pointer in x12 and call it */
                ARM = node->mt_ARMEntryPoint;
                asm volatile("":"=r"(ARM):"0"(ARM));
                CallARMCode();
            }
        }
        else
        {
            struct M68KTranslationUnit *node = NULL;

            /* Uncached mode - reset LastPC */
            setLastPC((void*)~(0));

            /* Save context since C code will be called */
            M68K_SaveContext(ctx);

            /* Find the unit */
            node = FindUnit();
            /* If node is found verify it */
            if (likely(node != NULL))
            {
                node = M68K_VerifyUnit(node);
            }
            /* If node was not found or invalidated, translate code */
            if (unlikely(node == NULL))
            {
                /* Get the code */
                node = M68K_GetTranslationUnit((uint16_t *)(uintptr_t)getCTX()->PC);
            }

            M68K_LoadContext(getCTX());
            ARM = node->mt_ARMEntryPoint;
            asm volatile("":"=r"(ARM):"0"(ARM));
            CallARMCode();
        }
    }
}
