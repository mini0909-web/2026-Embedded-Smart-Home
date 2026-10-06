#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <termios.h>

/* ================= 설정 매크로 ================= */

#define GAS_PIN "65"
#define GAS_VAL_PATH "/sys/class/gpio/gpio65/value"
#define GAS_DIR_PATH "/sys/class/gpio/gpio65/direction"

#define I2C_ADDR 0x48
#define I2C_BUS "/dev/i2c-1"

#define DHT_PIN "84"

#define SYSFS_GPIO_DIR "/sys/class/gpio"
#define GPIO_EXPORT SYSFS_GPIO_DIR "/export"
#define GPIO_UNEXPORT SYSFS_GPIO_DIR "/unexport"

/* ================= UART ================= */

int uart_fd = -1;

void UART_Init()
{
    uart_fd = open("/dev/ttyAMA2",
                   O_WRONLY | O_NOCTTY | O_NDELAY);

    if(uart_fd < 0)
    {
        perror("UART OPEN FAIL");
        return;
    }

    struct termios options;

    tcgetattr(uart_fd, &options);

    options.c_cflag = B9600 | CS8 | CLOCAL | CREAD;
    options.c_iflag = IGNPAR;
    options.c_oflag = 0;
    options.c_lflag = 0;

    tcflush(uart_fd, TCIFLUSH);
    tcsetattr(uart_fd, TCSANOW, &options);

    printf("UART TX Ready\n");
}

/* ================= DHT11 전역 ================= */

int fd_dir = -1;
int fd_val = -1;

/* ================= GPIO ================= */

void ExportGPIO(const char* pin_num)
{
    int fd = open(GPIO_EXPORT, O_WRONLY);

    if(fd != -1)
    {
        write(fd, pin_num, strlen(pin_num));
        close(fd);
    }
}

void UnexportGPIO(const char* pin_num)
{
    int fd = open(GPIO_UNEXPORT, O_WRONLY);

    if(fd != -1)
    {
        write(fd, pin_num, strlen(pin_num));
        close(fd);
    }
}

/* ================= DHT11 FAST GPIO ================= */

void InitFastGPIO(const char* pin_num)
{
    char path_dir[128];
    char path_val[128];

    snprintf(path_dir,
             sizeof(path_dir),
             SYSFS_GPIO_DIR "/gpio%s/direction",
             pin_num);

    snprintf(path_val,
             sizeof(path_val),
             SYSFS_GPIO_DIR "/gpio%s/value",
             pin_num);

    fd_dir = open(path_dir, O_RDWR);

    if(fd_dir == -1)
        perror("DHT11 Direction 파일 열기 실패");

    fd_val = open(path_val, O_RDWR);

    if(fd_val == -1)
        perror("DHT11 Value 파일 열기 실패");
}

static inline void FastSetDirOut()
{
    pwrite(fd_dir, "out", 3, 0);
}

static inline void FastSetDirIn()
{
    pwrite(fd_dir, "in", 2, 0);
}

static inline void FastWriteValLow()
{
    pwrite(fd_val, "0", 1, 0);
}

static inline int FastReadVal()
{
    char c;

    pread(fd_val, &c, 1, 0);

    return (c == '1');
}

int wait_for_state(int expected_state)
{
    int timeout = 0;

    while(FastReadVal() != expected_state)
    {
        if(++timeout > 20000)
            return -1;
    }

    return 0;
}

/* ================= RT SCHED ================= */

void SetRealTimePriority()
{
    struct sched_param param;

    param.sched_priority =
        sched_get_priority_max(SCHED_FIFO);

    sched_setscheduler(0,
                       SCHED_FIFO,
                       &param);
}

void SetNormalPriority()
{
    struct sched_param param;

    param.sched_priority = 0;

    sched_setscheduler(0,
                       SCHED_OTHER,
                       &param);
}

/* ================= DHT11 ================= */

int ReadDHT11(float* temp, float* humi)
{
    unsigned char data[5] = {0};

    SetRealTimePriority();

    FastSetDirOut();
    FastWriteValLow();

    usleep(18000);

    FastSetDirIn();

    if(wait_for_state(0)==-1)
    {
        SetNormalPriority();
        return -1;
    }

    if(wait_for_state(1)==-1)
    {
        SetNormalPriority();
        return -1;
    }

    if(wait_for_state(0)==-1)
    {
        SetNormalPriority();
        return -1;
    }

    for(int i=0;i<40;i++)
    {
        if(wait_for_state(1)==-1)
        {
            SetNormalPriority();
            return -1;
        }

        struct timespec start,end;

        clock_gettime(CLOCK_MONOTONIC,&start);

        if(wait_for_state(0)==-1)
        {
            SetNormalPriority();
            return -1;
        }

        clock_gettime(CLOCK_MONOTONIC,&end);

        long diff =
            (end.tv_sec-start.tv_sec)*1000000 +
            (end.tv_nsec-start.tv_nsec)/1000;

        data[i/8] <<= 1;

        if(diff > 40)
            data[i/8] |= 1;
    }

    SetNormalPriority();

    if(((data[0]+data[1]+data[2]+data[3])&0xFF)
        != data[4])
    {
        return -1;
    }

    *humi =
        data[0] +
        (data[1]*0.1);

    *temp =
        data[2] +
        (data[3]*0.1);

    return 0;
}

/* ================= MAIN ================= */

int main()
{
    ExportGPIO(GAS_PIN);

    usleep(100000);

    int gas_fd =
        open(GAS_DIR_PATH,O_WRONLY);

    if(gas_fd != -1)
    {
        write(gas_fd,"in",2);
        close(gas_fd);
    }

    int i2c_file;

    if((i2c_file =
        open(I2C_BUS,O_RDWR)) < 0)
    {
        perror("I2C 버스 열기 실패");
    }
    else if(ioctl(i2c_file,
                  I2C_SLAVE,
                  I2C_ADDR) < 0)
    {
        perror("I2C 슬레이브 설정 실패");
    }

    ExportGPIO(DHT_PIN);

    sleep(1);

    InitFastGPIO(DHT_PIN);

    UART_Init();

    printf("=================================\n");
    printf(" SENSOR + UART TX START\n");
    printf("=================================\n");

    while(1)
    {
        char gas_buf[2] = {0};

        gas_fd =
            open(GAS_VAL_PATH,O_RDONLY);

        if(gas_fd != -1)
        {
            read(gas_fd,
                 gas_buf,
                 1);

            close(gas_fd);
        }

        unsigned char i2c_buffer[2]={0};

        if(read(i2c_file,
                i2c_buffer,
                2) != 2)
        {
            i2c_buffer[1] = 0;
        }

        float temp = 0;
        float humi = 0;

        ReadDHT11(&temp,&humi);

        printf("TEMP=%.1f HUMI=%.1f LIGHT=%d GAS=%c\n",
               temp,
               humi,
               i2c_buffer[1],
               gas_buf[0]);

        char tx_buffer[256];

        snprintf(tx_buffer,
                 sizeof(tx_buffer),
                 "TEMP=%.1f,HUMI=%.1f,LIGHT=%d,GAS=%c\n",
                 temp,
                 humi,
                 i2c_buffer[1],
                 gas_buf[0]);

        if(uart_fd >= 0)
        {
            write(uart_fd,
                  tx_buffer,
                  strlen(tx_buffer));
        }

        sleep(2);
    }

    return 0;
}