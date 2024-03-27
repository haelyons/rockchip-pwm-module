/* WIP RESOURCES

    PrimeCell DMA Controller (PL330) Technical Reference:
    https://developer.arm.com/documentation/ddi0424/a/

    Collated ARM SoC Device Assignment Notes
    https://cwshu.github.io/arm_virt_notes/notes/dma/pl330.html
    
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define FPWM    1200    // Pulse period (ns) -- 1.2kHZ
#define RST     50000   // Minimum RESET
#define T0H     300     // high for 0 (ns)
#define T0L     900     // low for 0 (ns)
#define T1H     600     // high for 1 (ns) 
#define T1L     600     // low for 1 (ns)

// If non-zero, print debug information
#define DEBUG   0       // Print non-zero print debug

// Strip characteristics
#define LEDS    57      // Number of LEDs

// -------- Register Definitions (Rockchip RK3568 TRM Part I V1.3) --------
#define DMAC0_NS    0xFE530000 // DMA Base address (placeholder, use your actual)
#define PWM2_BASE   0xFE6F0000 

// Offsets for PWM2 registers
#define PWM2_PERIOD_OFFSET  0x0024 
#define PWM2_DUTY_OFFSET    0x0028
#define PWM2_CTRL_OFFSET    0x002C 
#define PWM2_EN             (1 << 0)

// Configure PWM settings
#define PWM_PERIOD      10000 // Period in some clock units
#define PWM_DUTY_CYCLE  5000  // Initial duty cycle

#define PAGE_SIZE         0x1000  // Size of memory page
#define PAGE_ROUNDUP(n)   ((n)%PAGE_SIZE==0 ? (n) : ((n)+PAGE_SIZE)&~(PAGE_SIZE-1)) // Round up to nearest page

#define T0H 300     
#define T0L 900  
#define T1H 600 
#define T1L 600

// -------- FUNCTION PROTOTYPES --------
// TODO terminate function
void configure_pwm(int period, int duty_cycle, void* pwm_regs);
void start_pwm(void* pwm_regs);
void stop_pwm(void* pwm_regs);

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

uint32_t set_pwm_duty_cycle_ns(void *pwm_regs, uint32_t duration_ns) {
    uint32_t *duty_reg = (uint32_t *)pwm_regs + (PWM2_DUTY_OFFSET / sizeof(uint32_t));
    // TODO: what are PWM peripheral register units? ns?
    *duty_reg = duration_ns; 
    return *duty_reg;
}

// ----- PROGRAM -----
int main() {
    int fd;
    void *pwm_regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    pwm_regs = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PWM2_BASE);
    if (pwm_regs == NULL) 
    {
        printf("Error mapping PWM registers!\n");
        return 1;
    } 
    else 
    { 
      printf("PWM2 register mapped at %p\n", pwm_regs); // Print mapped virtual mem addr
    }
    close(fd);
    
    uint8_t led_data = 0b10110100; // led test data
    int i;

    printf("Writing period value: %d\n", PWM_PERIOD);
    printf("Writing duty cycle value: %d\n", PWM_DUTY_CYCLE);

    // assume shortest possible period for continuous mode
    uint32_t duty_reg_val = set_pwm_duty_cycle_ns(pwm_regs, T0H);
    printf("\n");
    uint32_t *period_reg = (uint32_t *)pwm_regs + (PWM2_PERIOD_OFFSET / sizeof(uint32_t));
    *period_reg = T0H + T0L; 
    
    printf("Set PWM2 period (%d) with bus value %d\n", PWM_PERIOD, *period_reg);
    printf("Set PWM2 duty cycle (%d) with bus value %u\n", PWM_DUTY_CYCLE, duty_reg_val);

    uint32_t *ctrl_reg = (uint32_t *)pwm_regs + (PWM2_CTRL_OFFSET / sizeof(uint32_t));
    *ctrl_reg |= PWM2_EN;
    uint32_t ctrl_reg_value = *ctrl_reg;

    if (ctrl_reg_value & PWM2_EN) 
    {
        printf("Enable PWM2 bit set correctly\n");
    } 
    else 
    {
        printf("Enable PWM2 bit not set\n");
    }

    // send SK6812 sample data
    for (i = 0; i < 8; i++) 
    {
        if (led_data & (1 << (7 - i))) 
        {
            printf("1");
            set_pwm_duty_cycle_ns(pwm_regs, T1H);
            usleep(T1L / 1000); // Rough delay if no high-precision timer available
        } 
        else 
        {
            printf("0");
            set_pwm_duty_cycle_ns(pwm_regs, T0H);
            usleep(T0L / 1000); 
        }
    }
    printf("\n");
    return 0;
}
    /*
    //signal(SIGINT, terminate);

    // Map registers 
    void *pwm2_regs = map_segment((void*)PWM2_BASE, PAGE_SIZE);
    if (pwm2_regs == NULL) 
    {
        printf("Error mapping PWM registers!\n");
        return 1;
    } 
    else 
    { 
      printf("PWM2 register mapped at %p\n", pwm2_regs); // Print mapped addr
    }

    // Configure and start PWM
    pwm_configure(PWM_PERIOD, PWM_DUTY_CYCLE, pwm2_regs);
    pwm_start(pwm2_regs);

    if (pwm_enabled(pwm2_regs)) 
    {
      printf("PWM2 enabled!\n");
    } 
    else 
    {
      printf("PWM2 enabled failed...\n");
    }

    printf("PWM configured. Press Ctrl+C to exit.\n"); 

    while(1) 
    {
        sleep(1); 
    }

    pwm_stop(pwm2_regs);
    unmap_segment(pwm2_regs, PAGE_SIZE);
    exit(0);
    */

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


}

void pwm_start(void* pwm_regs) 
{
    uint32_t *pwm2_regs = (uint32_t*)pwm_regs;
    pwm2_regs[PWM2_CTRL_OFFSET / sizeof(uint32_t)] |= PWM2_EN;
    printf("Enabled PWM with pwm2_regs value\n");

    for (int i=0;i < (sizeof (pwm2_regs) /sizeof (pwm2_regs[0]));i++) {
        printf("%lf ", pwm2_regs[i]);
    }

    printf("\n");

}

void pwm_stop(void* pwm_regs) 
{
    uint32_t *pwm2_regs = (uint32_t*)pwm_regs;
    pwm2_regs[PWM2_CTRL_OFFSET / sizeof(uint32_t)] &= ~PWM2_EN;
}

int pwm_enabled(void *pwm_regs) 
{
    uint32_t *pwm2_regs = (uint32_t *)pwm_regs;
    uint32_t ctrl_reg_value = pwm2_regs[PWM2_CTRL_OFFSET / sizeof(uint32_t)];

    if ((ctrl_reg_value & PWM2_EN) != 0) 
    {
        return 1;
    } 
    else 
    {
        return 0;
    }
}
