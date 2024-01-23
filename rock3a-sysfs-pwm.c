#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define PWM_PATH "/sys/class/pwm/pwmchip9/pwm0/"

#define BUFFER_SIZE 16384

#define T0H 300
#define T0L 900 
#define T1H 600
#define T1L 600
#define RES 80000

char command_buffer[BUFFER_SIZE];
int buffer_index = 0;

/* 
    SK6812 PWM USERSPACE DRIVER
    Shitty sys-fs based userspace driver for the SK6812 LEDSTRIPs. I'm not proud of this. Datasheet:
    https://cdn-shop.adafruit.com/product-files/1138/SK6812+LED+datasheet+.pdf

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

int enable_pwm_chip() 
{
    const char *export_path = "/sys/class/pwm/pwmchip9/export";
    const char *enable_value = "0";
    int fd, ret;

    // Open export file
    fd = open(export_path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open export file");
        return -1;
    }

    // Write to export file to enable PWM chip
    ret = write(fd, enable_value, sizeof(enable_value) - 1);
    if (ret < 0) {
        perror("Failed to write to export file");
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

// send pulse (provide high and low time)
int send_pulse(int high_time_ns, int low_time_ns) 
{
    int period_ns = high_time_ns + low_time_ns;
    int duty_cycle_ns = high_time_ns;

    buffer_command("period", period_ns);
    buffer_command("duty_cycle", duty_cycle_ns);
    buffer_command("enable", 0);
    buffer_command("enable", 1);

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
        if (buffer_index >= BUFFER_SIZE - 500) 
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
    if (buffer_index + len < BUFFER_SIZE) 
    {
        strcpy(&command_buffer[buffer_index], tmp);
        buffer_index += len;
    } else 
    {
        fprintf(stderr, "buffer overflow\n");
        flush_buffer();
    }
}

void flush_buffer() 
{
    FILE *fp = popen("sh", "w");
    if (fp != NULL) 
    {
        fwrite(command_buffer, 1, buffer_index, fp);
        pclose(fp);
        buffer_index = 0; // reset buffer index
    } else 
    {
        perror("no shell");
    }
}

int main() {
    if (enable_pwm_chip() == 0) 
    {
        printf("PWM chip enabled successfully.\n");
    } else 
    {
        printf("Failed to enable PWM chip.\n");
    }

    while (1) 
    {        
        // WRITE WHITE TO ALL LEDs
        for (int n = 0; n < 60; n++)
        {
            for (int i = 0; i < 72; i++) 
            {
                send_pulse(T1H,T1L);
            }
            send_pulse(0,RES);
        }

        printf("Current buffer length=%d",buffer_index);
        flush_buffer();

        // Data refresh cycles are organised so that groups of 24 bit signals are sent in groups of 3
        // This is because each color is represented with 8 bits so R x G x B is 24 bits
        // The pulse is 1 bit, so we need to send 72 bits before sending a reset signal
        // Considering that each send_pulse(x,y) requires 189 bits, we actually need a buffer of size minimum 13608
        // This should take abt 86.4 us total 

        // The reset signal between each data refresh cycle is MINIMUM 50 us, this post reports
        // that it can be significantly longer, so we need to ensure that we can at least send 72 bits
    }

    return 0;
}
