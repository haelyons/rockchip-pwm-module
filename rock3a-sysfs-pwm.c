#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>

#define PWM_PATH "/sys/class/pwm/pwmchip9/pwm0/"

#define NUM_LEDS 57
#define PB_SIZE 1368 // pixel buf
#define SB_SIZE 16384 // sysfs buf

// SK6812 timing in nanoseconds
#define T0H 300
#define T0L 900
#define T1H 600
#define T1L 600
#define RES 30000 // actual is 50us but this can't be met using sysfs

char cmd_buf[SB_SIZE];
int pb[PB_SIZE];
int buffer_index = 0;

// 1 byte per color for each pixel, 57 pixels on NEO machine chassis ledstrip

typedef struct {
    unsigned char * r;
    unsigned char * g;
    unsigned char * b;
} pixel;

void hex_to_bin(pixel *p, int color_bin[24])
{
    /* for hex input
        p->r = (unsigned char) hex;
        p->g = (unsigned char) (hex >> 8);
        p->b = (unsigned char) (hex >> 16);
    */

    // Recombine them in GRB order ┌( ಠ_ಠ)┘
    unsigned int bin = (*p->g << 16) | (*p->r << 8) | *p->b;

    for (int i = 0; i < 24; ++i) 
    {
        color_bin[23 - i] = (bin >> i) & 1;
    }
}

void send_frame(int* pb, int size) {
    for (int i = 0; i < size; ++i) {
        if (i > 0 && i % 72 == 0) {
            send_pulse(0, RES); // send a RES pulse every refresh window (72 bits)
        }

        if (pb[i] == 0) 
        {
            fprintf(stdout, "%d\n", pb[i]);
            send_pulse(T0H, T0L);
        } 
        else if (pb[i] == 1) 
        {
            fprintf(stdout, "%d\n", pb[i]);
            send_pulse(T1H, T1L);
        }
    }

    // Ensure to send a final reset pulse at the end
    send_pulse(0, RES);
}

int pb_fill(int color_bin[24])
{
    for (int i = 0; i < 57; ++i) 
    {
        for (int j = 0; j < 24; ++j) 
        {
            pb[i * 24 + j] = color_bin[j];
        }
    }
}

/* 
    SK6812 PWM USERSPACE DRIVER
    Sys-fs interface userspace driver for SK6812 LED. I'm not proud of this one, next
    time make sure that DMA for the PWM peripheral is on the accessible bloody PWM.  
    
    Datasheet: https://cdn-shop.adafruit.com/product-files/1138/SK6812+LED+datasheet+.pdf

    ____-_-_--__----____-_-_--__----____-_-_--__----____-_-_--__----____

    +--------+                   +
    |        |                   |
    |        |                   | = 0
    |        |___________________|
    0.3us         0.9us

    +--------------+             +
    |              |             |
    |              |             | = 1
    |              |_____________|
        0.6us         0.6us

    +                            +
    |                            |
    |                            | = RESET
    |____________________________|
                80us
        
    Error bars on all signals are +/- 0.15us
*/ 

// on exit
void signal_handler(int)
{
    // TODO: Write empty / clear the strip pixel values
	disable_pwm_chip();
    fprintf(stdout, "Clean exit");
    exit(0);
}

int enable_pwm_chip() 
{
    const char *export_path = "/sys/class/pwm/pwmchip9/export";
    const char *enable_value = "0";
    int fd, ret;

    // Open export file
    fd = open(export_path, O_WRONLY);
    if (fd < 0) 
    {
        perror("Failed to open export file");
        return -1;
    }

    // Write to export file to enable PWM chip
    ret = write(fd, enable_value, sizeof(enable_value) - 1);
    if (ret < 0) 
    {
        perror("Failed to write to export file");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int disable_pwm_chip()
{
    unsigned char red = 0x00;
    unsigned char green = 0x00;
    unsigned char blue = 0x00;
    int color_exit[24];

    pixel p = {&red, &green, &blue};
    hex_to_bin(&p, color_exit);
    pb_fill(color_exit);

    const char* unexport_path = "/sys/class/pwm/pwmchip9/unexport";
    const char* disable_value = "0";
    int fd, ret;

    // Open the unexport file
    fd = open(unexport_path, O_WRONLY);
    if (fd < 0)
    {
        perror("Failed to open the unexport file");
        return -1;
    }

    // Write to the unexport file to disable PWM chip
    // You can verify this by checking for the prescence of the pwm0 diretory under pwmchip9/
    ret = write(fd, disable_value, sizeof(disable_value) - 1);
    if (ret < 0) 
    {
        perror("Failed to write to unexport file");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

// write value to attribute file
int write_pwm_attribute(const char *filename, int value) 
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s", PWM_PATH, filename);
    
    int fd = open(path, O_WRONLY);
    if (fd == -1) 
    {
        perror("Open PWM attribute");
        return -1;
    }

    char buffer[64];
    int bytes_written = snprintf(buffer, sizeof(buffer), "%d", value);
    if (write(fd, buffer, bytes_written) == -1) 
    {
        perror("Write PWM attribute");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

// send pulse (provide duty cycle)
int send_pulse(int high_time_ns, int low_time_ns) 
{
    int period_ns = high_time_ns + low_time_ns;
    int duty_cycle_ns = high_time_ns;

    //buffer_command("period", period_ns);
    buffer_command("duty_cycle", duty_cycle_ns);
    //buffer_command("enable", 0);
    //buffer_command("enable", 1);

    /*
    // set period
    if (write_pwm_attribute("period", period_ns) == -1) 
    {
        fprintf(stderr, "Failed to set PWM period\n");
        return -1;
    }

    // set duty cycle
    if (write_pwm_attribute("duty_cycle", duty_cycle_ns) == -1) 
    {
        fprintf(stderr, "Failed to set PWM duty cycle\n");
        return -1;
    }

    // enable
    if (write_pwm_attribute("enable", 1) == -1) 
    {
        fprintf(stderr, "Failed to enable PWM\n");
        return -1;
    }

    nanosleep(period_ns);
    // usleep(period_ns / 1000);  // ns -> us for usleep()
    
    // disable
    if (write_pwm_attribute("enable", 0) == -1) 
    {
        fprintf(stderr, "Failed to disable PWM\n");
        return -1;
    }
    */
}

void set_strip_white(int led_count) 
{
    for (int i = 0; i < led_count; ++i) 
    {
        if (buffer_index >= SB_SIZE - 500) 
        {
            flush_buffer();
        }
        
        // Send 'white' (24 bits, all high)
        for (int bit = 0; bit < 24; ++bit) 
        {
            send_pulse(600, 600);  // Sending '1' for white 
        }
    }
    //send_pulse(0, 50000); // reset pulse (0% duty 50us low time)
    //sleep(5);
}

void buffer_command(const char *filename, int value) 
{
    char tmp[128];
    int len = snprintf(tmp, sizeof(tmp), "echo %d > %s%s\n", value, PWM_PATH, filename);
    if (buffer_index + len < SB_SIZE) 
    {
        strcpy(&cmd_buf[buffer_index], tmp);
        buffer_index += len;
    } else 
    {
        fprintf(stdout, "[REMORA PWM] flushing command buffer for tmp at index %d \n", buffer_index);
        flush_buffer();
    }
}

void flush_buffer() 
{
    FILE *fp = popen("sh", "w");
    if (fp != NULL) 
    {
        fwrite(cmd_buf, 1, buffer_index, fp);
        pclose(fp);
        buffer_index = 0; // reset buffer index
    } 
    else 
    {
        perror("no shell");
    }
}

int main() {
    signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

    /* ENABLE PWM */
    if (enable_pwm_chip() == 0) 
    {
        printf("PWM chip enabled successfully!\n");
    } 
    else 
    {
        printf("Failed to enable PWM peripheral, or already enabled...\n");
    }

    /* SET COLOUR */
    unsigned char red = 0x46;
    unsigned char green = 0x66;
    unsigned char blue = 0xFF;
    int color_bin[24];

    /* FILL PIXEL BUFFER */
    pixel p = {&red, &green, &blue};
    hex_to_bin(&p, color_bin);
    pb_fill(color_bin);

    fprintf(stdout, "binary GRB array\n");
    for (int i = 0; i < 24; ++i) 
    {
        fprintf(stdout, "%d", color_bin[i]);
        if (i % 8 == 7) fprintf(stdout," \n"); // add space between  bytes
    }

    /* TEST CONTENTS OF PIXEL BUFFER */
    fprintf(stdout, "pixel buff 0..48\n");
    for (int i = 0; i < 48; ++i) 
    {
        fprintf(stdout, "%d ", pb[i]);
    }
    fprintf(stdout, "\n");

    buffer_command("period", 1200);
    buffer_command("enable", 1);
    
    flush_buffer();

    const char *duty_cycle_path = "/sys/class/pwm/pwmchip9/pwm0/duty_cycle";
    const char *enable_path = "/sys/class/pwm/pwmchip9/pwm0/enable";
    const char *period_path = "/sys/class/pwm/pwmchip9/pwm0/period";

    // use unbufferred system calls open(), write() instead of buffered fopen(), fwrite()
    // O_WRONLY opens for writing only
    FILE *enable_file = open(enable_path, O_WRONLY);
    if (enable_file < 0)
    {
        fprintf(stdout, "Failed to open enable file");
    }

    FILE *period_file = open(period_path, O_WRONLY);
    if (period_file < 0)
    {
        fprintf(stdout, "Failed to open period file");
    }
    
    FILE *duty_file = open(duty_cycle_path, O_WRONLY);
    if (duty_file < 0) 
    {
        perror("Failed to open duty_cycle file");
        return -1;
    }
    //setbuf( duty_file, NULL );
    fprintf(stdout, "\nSending frame...\n");

    char *pbuf = "1200";
    char *ebuf = "1";

    if (write(enable_file, ebuf, strlen(ebuf)) < 0)
    {
        fprintf(stdout, "Failed to write to enable file\n");
    }

    if (write(period_file, pbuf, strlen(pbuf)) < 0)
    {
        fprintf(stdout, "Failed to write to period file\n");
    }
    int i = 0;
    while (i < 10)
    {
        char *dbuf = "300";
        if (write(duty_file, dbuf, strlen(pbuf)) < 0)
        {
            printf("Failed to write to period file\n");
        }
        printf("Unbuffered syscall: 300\n");
        dbuf = "0";
        if (write(duty_file, dbuf, strlen(pbuf)) < 0)
        {
            printf("Failed to write to period file\n");
        }
        printf("Unbuffered syscall: 0\n");
        dbuf = "600";
        if (write(duty_file, dbuf, strlen(pbuf)) < 0)
        {
            printf("Failed to write to period file\n");
        }
        printf("Unbuffered syscall: 600\n");
        i++;
    }

    close(duty_file);
    return 0;
}

        /* COMMENTS TO BE SORTED
        buffer_command("duty_cycle", 300);
        buffer_command("duty_cycle", 600);
        buffer_command("duty_cycle", 300);
        buffer_command("duty_cycle", 600);

        char tmp[128];
        int len = snprintf(tmp, sizeof(tmp), "echo %d > %s%s\n", value, PWM_PATH, filename);
        
        if (buffer_index + len < SB_SIZE) 
        {
            strcpy(&cmd_buf[buffer_index], tmp);
            buffer_index += len;
        } 
        else 
        {
            fprintf(stdout, "[REMORA PWM] flushing command buffer for tmp at index %d \n", buffer_index);
            flush_buffer();
        }
        
        //send_frame(pb, PB_SIZE);
       

        
        // WRITE WHITE TO ALL LEDs
        for (int n = 0; n < 60; n++)
        {
            for (int i = 0; i < 72; i++) 
            {
                //send_pulse(1000,200);
                send_pulse(T1H,T1L);
            }
            send_pulse(0,50000);
            //send_pulse(0,RES);
        }

        printf("Current buffer length=%d\n", buffer_index);
        flush_buffer();
 
        

        // Data refresh cycles are organised so that groups of 24 bit signals are sent in groups of 3
        // This is because each color is represented with 8 bits so R x G x B is 24 bits
        // The pulse is 1 bit, so we need to send 72 bits before sending a reset signal

        // Considering that each send_pulse(x,y) requires 189 bits, we actually need a buffer of size 13608

        // This should take abt 86.4 us total 
        // The reset signal between each data refresh cycle is MINIMUM 50 us, this post reports
        // that it can be significantly longer, so we need to ensure that we can at least send 72 bits

  
        // 1 bit pulse: high for ~600ns, low for ~600ns
        send_pulse(600, 600);
        // 0 bit pulse: high for ~300ns, low for ~900ns
        send_pulse(300, 900);
        send_pulse(0, 500);



        UPDATE 15/12/23 -- 180 bits sent over 217.772uS (each bit approx. 1.208uS)
        This means we meet the minimum sample window (72 ledstrip address bits) but we may not meet it
        in terms of accuracy. 

        It looks like there's variability in the actual duty cycles of the bits, from 584ns to
        624ns.
        */
