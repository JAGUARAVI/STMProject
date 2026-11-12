/**
 * @file    bsp_console.c
 * @brief   USART1 line-oriented console: interrupt-driven RX, blocking TX.
 *
 * Target: STM32G474CEU6. USART1 uses PA9 (TX) and PA10 (RX), AF7, 115200 8N1,
 * no flow control. The peripheral clock is enabled here; HAL_UART_MspInit() is
 * not implemented elsewhere, so this file also owns the GPIO configuration.
 *
 * @par RX strategy
 * Reception is handled directly at register level. console_init() calls
 * HAL_UART_Init() for the baud-rate/frame setup, then enables RXNEIE and EIE and
 * unmasks USART1_IRQn. USART1_IRQHandler() reads USART1->ISR once, moves the
 * byte from USART1->RDR into the ring, and clears ORE/FE/NE/PE through
 * USART1->ICR. HAL_UART_IRQHandler() is deliberately never called, so no
 * HAL_UART_RxCpltCallback() exists and no one-byte HAL receive has to be
 * re-armed: there is no window in which the receiver is disarmed. Do not mix
 * this handler with HAL_UART_Receive_IT() or HAL_UART_IRQHandler().
 *
 * @par Ring invariants (single producer / single consumer)
 * - s_rx_ring is a 256-byte array indexed with a power-of-two mask.
 * - s_rx_head is written only by USART1_IRQHandler() (the producer).
 * - s_rx_tail is written only by the main-loop side (the consumer).
 * - head == tail means empty. One slot is reserved to distinguish full from
 *   empty, so at most 255 bytes are buffered; the producer drops the newest
 *   byte when the ring is full instead of overwriting unread input.
 * - The buffer and both indices are volatile so the compiler cannot move the
 *   data store past the head update (or the head read past the data read).
 *   Cortex-M4 has no data cache, so no explicit barrier is required.
 * - console_flush_rx() only advances tail, so a byte arriving concurrently is
 *   retained rather than lost.
 *
 * @par Line assembler invariants (main-loop context only)
 * - s_line holds at most CONSOLE_LINE_CAPACITY characters; the NUL terminator
 *   lives in s_ready_line, never in s_line.
 * - s_line_overflow latches when an extra character is offered to a full line:
 *   every further character except CR/LF and backspace is discarded until the
 *   terminator. At CR/LF the truncated prefix is still published.
 * - A completed non-empty line is copied to s_ready_line and marked ready. If a
 *   previous line is still pending, it is overwritten: the newest line wins.
 * - Blank lines (bare CR/LF) carry no command and are ignored; they do not
 *   disturb a pending line.
 *
 * @par No echo
 * Received bytes are never echoed back. The host dashboard owns the command
 * display and echoes locally, while the same UART also carries @@EVT telemetry
 * from the application. Echoing here would duplicate every character and would
 * interleave whole lines into the telemetry stream, so the console is a pure
 * line-oriented input plus raw output path.
 *
 * @par Context
 * TX helpers are blocking and must only be called from the main loop. They
 * spin on TXE with a bounded guard, so they cannot hang if USART1 failed to
 * initialise or the peripheral is not clocked. console_printf() uses a static
 * buffer and is therefore not reentrant and not safe from an ISR either.
 */

#include "stm32g4xx_hal.h"
#include "config.h"
#include "bsp_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

/** RX ring size in bytes; must be a power of two for index masking. */
#define CONSOLE_RX_RING_SIZE        256U

/** Index mask for the RX ring. */
#define CONSOLE_RX_RING_MASK        (CONSOLE_RX_RING_SIZE - 1U)

#if ((CONSOLE_RX_RING_SIZE & CONSOLE_RX_RING_MASK) != 0U)
#error "CONSOLE_RX_RING_SIZE must be a power of two"
#endif

/** Maximum characters retained in one command line, excluding the NUL. */
#define CONSOLE_LINE_CAPACITY       128U

/** Size of the static console_printf() staging buffer. */
#define CONSOLE_PRINTF_BUF_SIZE     256U

/**
 * Upper bound on TXE wait iterations per byte. At 170 MHz this is comfortably
 * longer than the 87 us one byte needs at 115200 baud, yet short enough that a
 * dead peripheral cannot stall the main loop for long.
 */
#define CONSOLE_TX_SPIN_LIMIT       200000U

/**
 * NVIC preemption priority for USART1. More urgent than the HAL SysTick at 15,
 * less urgent than the measurement ISRs (which will use smaller numbers). The
 * RX ring provides roughly 22 ms of buffering at 115200 baud, so a short
 * preemption by a measurement ISR is safe.
 */
#define CONSOLE_IRQ_PRIORITY        6U

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

/** HAL handle owned privately by this module (main.c has its own scaffold). */
static UART_HandleTypeDef s_uart;

/** True once HAL_UART_Init() succeeded; gates all register-level TX access. */
static bool s_uart_ready;

/** RX ring: written by the ISR, read by console_poll(). */
static volatile uint8_t  s_rx_ring[CONSOLE_RX_RING_SIZE];
static volatile uint16_t s_rx_head;   /**< Producer index, ISR only.    */
static volatile uint16_t s_rx_tail;   /**< Consumer index, main only.   */

/** Line currently being assembled (main-loop context). */
static char   s_line[CONSOLE_LINE_CAPACITY];
static size_t s_line_len;
static bool   s_line_overflow;

/** Most recently completed line, waiting for console_read_line(). */
static char   s_ready_line[CONSOLE_LINE_CAPACITY + 1U];
static size_t s_ready_len;
static bool   s_line_ready;

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Push one received byte into the RX ring.
 *
 * Called only from USART1_IRQHandler(). If the ring is full the newest byte is
 * dropped: preserving the bytes already queued keeps the earlier part of the
 * command intact, and discarding input is preferable to overwriting data the
 * consumer is about to read.
 *
 * @param  byte Received character.
 */
static void console_rx_push(uint8_t byte)
{
    uint16_t head = s_rx_head;
    uint16_t next = (uint16_t)((head + 1U) & CONSOLE_RX_RING_MASK);

    if (next == s_rx_tail)
    {
        /* Full: one slot is reserved, so this byte cannot be stored. */
        return;
    }

    s_rx_ring[head] = byte;
    s_rx_head = next;
}

/**
 * @brief  Feed one byte to the line assembler.
 *
 * CR and LF terminate a line. Backspace (0x08) and delete (0x7f) remove the
 * last character. Once the line capacity is reached, s_line_overflow latches
 * and all further characters are dropped until the terminator, which keeps the
 * buffer bounded while still publishing the truncated prefix.
 *
 * @param  byte Received character.
 */
static void console_line_feed(uint8_t byte)
{
    if ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n'))
    {
        if (s_line_len > 0U)
        {
            memcpy(s_ready_line, s_line, s_line_len);
            s_ready_line[s_line_len] = '\0';
            s_ready_len = s_line_len;

            /* Newest complete line replaces any unread predecessor. */
            s_line_ready = true;
        }

        s_line_len = 0U;
        s_line_overflow = false;
        return;
    }

    if ((byte == 0x08U) || (byte == 0x7FU))
    {
        if (s_line_len > 0U)
        {
            s_line_len--;
        }
        return;
    }

    if (s_line_overflow)
    {
        return;
    }

    if (s_line_len < CONSOLE_LINE_CAPACITY)
    {
        s_line[s_line_len] = (char)byte;
        s_line_len++;
    }
    else
    {
        /* Excess beyond the buffer is discarded until the next CR/LF. */
        s_line_overflow = true;
    }
}

/**
 * @brief  Transmit one byte, polling TXE with a bounded wait.
 *
 * No-op if the UART never initialised. The spin limit prevents a clocking or
 * configuration failure from trapping the caller in an infinite loop.
 *
 * @param  byte Byte to send.
 */
static void console_tx_byte(uint8_t byte)
{
    uint32_t spin = CONSOLE_TX_SPIN_LIMIT;

    if (!s_uart_ready)
    {
        return;
    }

    while ((USART1->ISR & USART_ISR_TXE) == 0U)
    {
        if (--spin == 0U)
        {
            return;
        }
    }

    USART1->TDR = (uint32_t)byte;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief  Configure USART1 and start interrupt-driven reception.
 *
 * Enables GPIOA and USART1 clocks, maps PA9/PA10 to AF7, calls HAL_UART_Init()
 * for 115200 8N1, then enables the RXNE and error interrupts and USART1_IRQn.
 *
 * @return true on success, false if HAL_UART_Init() failed.
 */
bool console_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* Make a repeated call safe: no ISR may touch the ring while we reset it. */
    s_uart_ready = false;
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART1_IRQn);

    s_rx_head = 0U;
    s_rx_tail = 0U;

    s_line_len = 0U;
    s_line_overflow = false;

    s_ready_len = 0U;
    s_ready_line[0] = '\0';
    s_line_ready = false;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    /* TX: push-pull alternate function, no pull. */
    gpio.Pin = PIN_UART_TX;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(PIN_UART_PORT, &gpio);

    /* RX: alternate function with a pull-up so an unplugged line idles high. */
    gpio.Pin = PIN_UART_RX;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(PIN_UART_PORT, &gpio);

    s_uart.Instance = USART1;
    s_uart.Init.BaudRate = CONSOLE_BAUD;
    s_uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_uart.Init.StopBits = UART_STOPBITS_1;
    s_uart.Init.Parity = UART_PARITY_NONE;
    s_uart.Init.Mode = UART_MODE_TX_RX;
    s_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    s_uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    s_uart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    s_uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&s_uart) != HAL_OK)
    {
        return false;
    }

    /* Start from a clean error state before the interrupt is unmasked. */
    USART1->ICR = USART_ICR_PECF | USART_ICR_FECF |
                  USART_ICR_NECF | USART_ICR_ORECF;

    __HAL_UART_ENABLE_IT(&s_uart, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&s_uart, UART_IT_ERR);

    s_uart_ready = true;

    HAL_NVIC_SetPriority(USART1_IRQn, CONSOLE_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    return true;
}

/**
 * @brief  Drain the RX ring into the line assembler.
 *
 * Call from the main loop. The producer index is snapshotted once per call so a
 * continuous input stream cannot keep the caller inside this function; bytes
 * arriving after the snapshot are picked up on the next call. The completed
 * line (if any) is left for console_read_line(); no dispatcher is invoked, so
 * this module carries no command policy.
 */
void console_poll(void)
{
    uint16_t head = s_rx_head;

    while (s_rx_tail != head)
    {
        uint8_t byte = s_rx_ring[s_rx_tail];

        s_rx_tail = (uint16_t)((s_rx_tail + 1U) & CONSOLE_RX_RING_MASK);
        console_line_feed(byte);
    }
}

/**
 * @brief  True when an entire command line is waiting.
 *
 * @return true if console_read_line() would return a line.
 */
bool console_line_ready(void)
{
    return s_line_ready;
}

/**
 * @brief  Copy the pending line (without CR/LF) into a caller buffer.
 *
 * The pending line is consumed even if the caller's buffer is too small; the
 * copy is truncated to len - 1 characters and NUL-terminated.
 *
 * @param  buf Destination buffer.
 * @param  len Size of @p buf in bytes; must be at least 1.
 * @return true if a line was copied, false if none was pending or arguments
 *         were invalid.
 */
bool console_read_line(char *buf, size_t len)
{
    size_t count;

    if ((buf == NULL) || (len == 0U) || (!s_line_ready))
    {
        return false;
    }

    count = s_ready_len;
    if (count > (len - 1U))
    {
        count = len - 1U;
    }

    if (count > 0U)
    {
        memcpy(buf, s_ready_line, count);
    }
    buf[count] = '\0';

    s_line_ready = false;
    s_ready_len = 0U;

    return true;
}

/**
 * @brief  Blocking raw string write.
 *
 * No newline translation and no echo: the raw bytes are placed on the wire in
 * order, which is what the telemetry path expects. Main-loop context only.
 *
 * @param  s NUL-terminated string; NULL is ignored.
 */
void console_write(const char *s)
{
    if (s == NULL)
    {
        return;
    }

    while (*s != '\0')
    {
        console_putc(*s);
        s++;
    }
}

/**
 * @brief  Formatted output into a fixed 256-byte buffer, then transmit.
 *
 * vsnprintf() truncates rather than overflowing. The static buffer makes this
 * function non-reentrant; it is intended for the main loop only, never for an
 * ISR.
 *
 * @param  fmt printf-style format string.
 */
void console_printf(const char *fmt, ...)
{
    static char buffer[CONSOLE_PRINTF_BUF_SIZE];
    va_list args;
    int written;

    if (fmt == NULL)
    {
        return;
    }

    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    /* Negative means an encoding error; the buffer is not valid then. */
    if (written > 0)
    {
        console_write(buffer);
    }
}

/**
 * @brief  True if the RX ring holds at least one unread byte.
 *
 * Long-running commands poll this to notice an abort request without draining
 * the ring themselves.
 *
 * @return true if input is pending.
 */
bool console_rx_pending(void)
{
    return (s_rx_head != s_rx_tail);
}

/**
 * @brief  Discard all pending input.
 *
 * Empties the ring and the line assembler. Only tail is moved, so a byte that
 * arrives concurrently with the flush remains queued for the next poll. Call
 * from the main loop, not from an ISR.
 */
void console_flush_rx(void)
{
    s_rx_tail = s_rx_head;

    s_line_len = 0U;
    s_line_overflow = false;

    s_ready_len = 0U;
    s_ready_line[0] = '\0';
    s_line_ready = false;
}

/**
 * @brief  Raw single-byte write, used by the watcher/telemetry path.
 *
 * Blocking on TXE with a bounded wait. No character translation. Main-loop
 * context only.
 *
 * @param  c Byte to send.
 */
void console_putc(char c)
{
    console_tx_byte((uint8_t)c);
}

/* ------------------------------------------------------------------------- */
/* Interrupt service routine                                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief  USART1 global interrupt: register-level receive handling.
 *
 * Not routed through HAL_UART_IRQHandler(). A single ISR read captures the
 * cause; a byte is pushed only when none of PE/FE/NE is set, because those
 * flags mark the frame in RDR as corrupt. ORE means the newest byte was lost
 * in hardware while the previous one is still valid, so that previous byte is
 * kept. All cause bits are cleared through ICR before returning, otherwise an
 * error interrupt would retrigger forever.
 *
 * @note  Keep this IRQ less urgent than the measurement ISRs. If this handler
 *        is delayed longer than one frame time (about 87 us at 115200 baud),
 *        the hardware reports an overrun; the handler recovers, but the byte
 *        is lost. The 256-byte ring does not help with ISR latency, only with
 *        main-loop latency.
 */
void USART1_IRQHandler(void)
{
    uint32_t isr = USART1->ISR;
    uint32_t errors = isr & (USART_ISR_PE | USART_ISR_FE |
                             USART_ISR_NE | USART_ISR_ORE);

    if ((isr & USART_ISR_RXNE) != 0U)
    {
        uint8_t byte = (uint8_t)(USART1->RDR & 0xFFU);

        if ((errors & (USART_ISR_PE | USART_ISR_FE | USART_ISR_NE)) == 0U)
        {
            console_rx_push(byte);
        }
        /* else: corrupt frame, drop the byte but still clear the flags below. */
    }
    else if ((isr & USART_ISR_ORE) != 0U)
    {
        /* Release a stale RDR value if the overrun left RXNE deasserted. */
        (void)USART1->RDR;
    }
    else
    {
        /* Interrupt without RXNE or ORE: FE/NE alone, handled below. */
    }

    if (errors != 0U)
    {
        uint32_t clear = 0U;

        if ((errors & USART_ISR_PE) != 0U)
        {
            clear |= USART_ICR_PECF;
        }
        if ((errors & USART_ISR_FE) != 0U)
        {
            clear |= USART_ICR_FECF;
        }
        if ((errors & USART_ISR_NE) != 0U)
        {
            clear |= USART_ICR_NECF;
        }
        if ((errors & USART_ISR_ORE) != 0U)
        {
            clear |= USART_ICR_ORECF;
        }

        USART1->ICR = clear;
    }
}
