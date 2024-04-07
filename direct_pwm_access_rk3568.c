#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

// 
/* -------- Key Resources --------
- RK3568 TRM Part I V1.3
- RK3568 TRM Part II V1.1 (Can't find 1.3)
- Rockchip Linux source (pwm-rockchip.c)
- Firefly ROC-RK3568-PC PWM examples (pwm-firefly.c)
- Raspberry Pi 4 DMA Examples (specifically for SK6812 or ws281x LEDSTRIPS -> see rpi_ws281x library for full, quality RPi implementation)

TODO 
- Add the RPi DMA one
- Additional useful sources
*/

// -------- Debug --------
#define DEBUG   1       // Print non-zero print debug

// -------- Register Definitions --------
#define DMAC0_NS                0xFE530000 // DMA Base address (placeholder, use your actual)
#define PWM2_BASE               0xFE6F0000 
#define PWM2_PERIOD_OFFSET      0x0024 
#define PWM2_DUTY_OFFSET        0x0028
#define PWM2_CTRL_OFFSET        0x002C 

// -------- Register Configuration --------
#define PWM_CTRL_TIMER_EN	    (1 << 0)
#define PWM_CTRL_OUTPUT_EN	    (1 << 3)

#define PWM_ENABLE			    (1 << 0)
#define PWM_CONTINUOUS		    (1 << 1)
#define PWM_DUTY_POSITIVE	    (1 << 3)
#define PWM_DUTY_NEGATIVE	    (0 << 3)
#define PWM_INACTIVE_NEGATIVE	(0 << 4)
#define PWM_INACTIVE_POSITIVE	(1 << 4)
#define PWM_POLARITY_MASK	    (PWM_DUTY_POSITIVE | PWM_INACTIVE_POSITIVE)
#define PWM_OUTPUT_LEFT		    (0 << 5)
#define PWM_OUTPUT_CENTER	    (1 << 5)
#define PWM_LOCK_EN		        (1 << 6)
#define PWM_LP_DISABLE		    (0 << 8)

// TODO delete, running in continuous mode
#define PWM_ONESHOT_COUNT_SHIFT 24
#define PWM_ONESHOT_COUNT_MASK	(0xff << PWM_ONESHOT_COUNT_SHIFT)
#define PWM_ONESHOT_COUNT_MAX	256

#define PWM_REG_INTSTS(n)	    ((3 - (n)) * 0x10 + 0x10)
#define PWM_REG_INT_EN(n)	    ((3 - (n)) * 0x10 + 0x14)

#define PWM_CH_INT(n)		    BIT(n)

// -------- SK6812 Specification --------
#define LEDS                    57           // Number of LEDs
#define T0H                     300          // Duty cycle high / low for 0
#define T0L                     900 
#define T1H                     600          // Duty cycle high / low for 1
#define T1L                     600
#define FPWM                    (T0H + T0L)  // PWM frequency (period)
#define RST                     50000        // min. reset value

// -------- Memory Macros --------
#define PAGE_SIZE         0x1000  // Size of memory page
#define PAGE_ROUNDUP(n)   ((n)%PAGE_SIZE==0 ? (n) : ((n)+PAGE_SIZE)&~(PAGE_SIZE-1)) // Round up to nearest page

// ----- MGMT -----
void FAIL(const char *msg) 
{
    printf("%s\n", msg);
    exit(1);
}

// ----- VIRTUAL MEMORY -----
// Get virtual memory segment for peripheral regs or physical mem
void *map_segment(void *addr, int size)
{
  int fd;
  void *mem;

  size = PAGE_ROUNDUP(size);
  // Removed O_CLOEXEC flag
  if ((fd = open ("/dev/mem", O_RDWR|O_SYNC)) < 0)
    FAIL("Error: can't open /dev/mem, run using sudo\n");

  /* 
  mmap takes a physical address (the peripheral in our case) and opens a window
  in virtual memory that this program can access; any R/W to the window is automatically
  redircted to the peripheral  
  */

  mem = mmap(0, size, PROT_WRITE|PROT_READ, MAP_SHARED, fd, (uint32_t)addr);
  close(fd);

#if DEBUG
  printf("Map %p -> %p\n", (void *)addr, mem);
#endif

  if (mem == MAP_FAILED)
    FAIL("Error: can't map memory\n");
  return(mem);
}

// Free mapped memory
void unmap_segment(void *mem, int size)
{
  if (mem)
    munmap(mem, PAGE_ROUNDUP(size));
}

uint32_t set_pwm_duty_cycle_ns(void *pwm_regs, uint32_t duration_ns) 
{
    uint32_t *duty_reg = (uint32_t *)pwm_regs + (PWM2_DUTY_OFFSET / sizeof(uint32_t));
    // TODO: what are PWM peripheral register units? ns?
    *duty_reg = duration_ns; 
    return *duty_reg;
}

// ----- PROGRAM -----
int main() 
{
    fprintf(stdout, "[LIGHT] Using physical address 0x%x\n", PWM2_BASE);
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
        printf("[LIGHT] ERROR: Failed to open /dev/mem\n");
    }

    void *pwm_regs = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PWM2_BASE);
    if (pwm_regs == NULL) 
    {
        printf("[LIGHT] ERROR: Failed to mmap() PWM physical register address to virtual memory\n");
        return -1;
    }
    else 
    { 
        printf("[LIGHT] PWM register mapped at %p\n", pwm_regs); // Print mapped virtual mem addr
    }
    close(fd); // can close file descriptor without invalidating the mapping (see mmap() man page)
    
    fprintf(stdout, "TP0");

    // assume shortest possible period for continuous mode
    uint32_t duty_reg_val = set_pwm_duty_cycle_ns(pwm_regs, T0H);
    
    uint32_t *period_reg = (uint32_t *)pwm_regs + (PWM2_PERIOD_OFFSET / sizeof(uint32_t));
    *period_reg = T0H + T0L;

    fprintf(stdout, "TP1");

    printf("Set PWM2 period (%d) with bus value %d\n", T0H + T0L, *period_reg);
    printf("Set PWM2 duty cycle (%d) with bus value %u\n", T0H, duty_reg_val);

    uint32_t *ctrl_reg = (uint32_t *)pwm_regs + (PWM2_CTRL_OFFSET / sizeof(uint32_t));
    *ctrl_reg |= PWM_ENABLE;
    uint32_t ctrl_reg_value = *ctrl_reg;

    printf("TP2");

    if (ctrl_reg_value & PWM_ENABLE) 
    {
        printf("[LIGHT] PWM2 Enable bus value correct\n");
    } 
    else 
    {
        printf("[LIGHT] PWM2 Enable bus value incorrect\n");
    }
}

// ----- PWM ----- 
void pwm_configure(int period, int duty_cycle, void* pwm_regs) 
{
    printf("Writing period value: %d\n", period);
    printf("Writing duty cycle value: %d\n", duty_cycle);

    uint32_t *pwm2_regs = (uint32_t*)pwm_regs;
    pwm2_regs[PWM2_PERIOD_OFFSET / sizeof(uint32_t)] = period;
    pwm2_regs[PWM2_DUTY_OFFSET / sizeof(uint32_t)] = duty_cycle;

    // Calculate addresses
    uint32_t *period_reg_addr = pwm2_regs + (PWM2_PERIOD_OFFSET / sizeof(uint32_t));
    uint32_t *duty_reg_addr = pwm2_regs + (PWM2_DUTY_OFFSET / sizeof(uint32_t));

    printf("PWM2 period register at %p, value: %u\n", period_reg_addr, *period_reg_addr);
    printf("PWM2 duty cycle register at %p, value: %u\n", duty_reg_addr, *duty_reg_addr);

    /*
    volatile uint32_t *pwm_registers = (volatile uint32_t *)(mapped_base + register_offset);

    // -------- Configuration (Based on Usage Flow) --------

    // 1. Disable PWM channel
    pwm_registers[PWM2_CTRL_OFFSET / 4] &= ~PWM_CTRL_TIMER_EN;  

    // ... (Prescale, Clock selection -  not provided in usage flow)

    // 3. Output mode, Polarity (replace with your choices)
    pwm_registers[PWM2_CTRL_OFFSET / 4] &= ~PWM_POLARITY_MASK; // Clear existing polarity
    pwm_registers[PWM2_CTRL_OFFSET / 4] |= PWM_DUTY_POSITIVE | PWM_INACTIVE_NEGATIVE; 

    // 4. For one-shot mode, configure PWM_PWMx_CTRL.rpt (omitted for continuous)

    // 5. Choose continuous mode
    pwm_registers[PWM2_CTRL_OFFSET / 4] |= PWM_CONTINUOUS;  

    // 6. (Omitted - Assumed interrupts not needed) 
    // 7. (Omitted - Assumed disabled by default)

    // -------- Set Period and Duty Cycle --------
    // You'll need to calculate register values based on your clock and desired timing

    pwm_registers[PWM2_PERIOD_OFFSET / 4] = ; 
    pwm_registers[PWM2_DUTY_OFFSET / 4] = ; 

    // -------- Enable the PWM --------
    pwm_registers[PWM2_CTRL_OFFSET / 4] |= PWM_CTRL_OUTPUT_EN | PWM_CTRL_TIMER_EN ; 
    */
}

void pwm_start(void* pwm_regs) 
{
    uint32_t *pwm2_regs = (uint32_t*)pwm_regs;
    pwm2_regs[PWM2_CTRL_OFFSET / sizeof(uint32_t)] |= PWM_ENABLE;
    printf("Enabled PWM with pwm2_regs value\n");

    for (int i = 0; i < (sizeof (pwm2_regs) / sizeof(pwm2_regs[0])); i++) 
    {
        printf("%lf ", pwm2_regs[i]);
    }

    printf("\n");

}

void pwm_stop(void* pwm_regs) 
{
    uint32_t *pwm2_regs = (uint32_t*)pwm_regs;
    pwm2_regs[PWM2_CTRL_OFFSET / sizeof(uint32_t)] &= ~PWM_ENABLE;
}

int pwm_enabled(void *pwm_regs) 
{
    uint32_t *pwm2_regs = (uint32_t *)pwm_regs;
    uint32_t ctrl_reg_value = pwm2_regs[PWM2_CTRL_OFFSET / sizeof(uint32_t)];

    if ((ctrl_reg_value & PWM_ENABLE) != 0) 
    {
        return 1;
    } 
    else 
    {
        return 0;
    }
}
