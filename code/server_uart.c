#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <termios.h>

#define PORT 8080

#define LED_PIN "84"
#define BUZZER_PIN "85"
#define MOTOR_1_PIN "89" // 1번 모터 물리 핀
#define MOTOR_2_PIN "90" // 2번 모터 물리 핀
#define MOTOR_3_PIN "65" // 3번 모터 물리 핀
#define MOTOR_4_PIN "66" // 4번 모터 물리 핀

#define SYSFS_GPIO_DIR "/sys/class/gpio"
#define GPIO_EXPORT SYSFS_GPIO_DIR "/export"

void LED_ON();
void LED_OFF();
void BUZZER_ON();
void BUZZER_OFF();
void MOTOR_1_ON();
void MOTOR_1_OFF();
void MOTOR_2_ON();
void MOTOR_2_OFF();
void MOTOR_3_ON();
void MOTOR_3_OFF();
void MOTOR_4_ON();  
void MOTOR_4_OFF(); 
void ALL_MOTORS_OFF();

volatile int global_emergency_active = 0; 
volatile int user_forced_normal = 0; 

/* ================= EMERGENCY LED BLINK THREAD ================= */
void* Emergency_Blink_Worker(void* arg)
{
    int led_state = 0;
    int last_was_active = 0; 

    while(1)
    {
        if(global_emergency_active)
        {
            led_state = !led_state;
            if(led_state) LED_ON();
            else          LED_OFF();
            
            last_was_active = 1; 
            usleep(300000); 
        }
        else
        {
            if(last_was_active == 1)
            {
                LED_OFF();
                last_was_active = 0; 
                printf("🟢 비상 종료 감지 - LED 안전 소등 완료\n");
                fflush(stdout);
            }
            usleep(100000); 
        }
    }
    return NULL;
}

/* ================= UART ================= */
int uart_fd = -1;

void UART_Init()
{
    uart_fd = open("/dev/ttyAMA2", O_RDONLY | O_NOCTTY | O_NDELAY);
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
}

void* UART_Receiver(void* arg)
{
    char rx_buffer[256];
    static char line_buffer[1024] = {0};
    static int line_idx = 0;
    int emergency_logged = 0; 

    while(1)
    {
        int len = read(uart_fd, rx_buffer, sizeof(rx_buffer) - 1);
        if(len > 0)
        {
            for(int i = 0; i < len; i++)
            {
                if(rx_buffer[i] == '\r') continue;

                if(line_idx < sizeof(line_buffer) - 1)
                {
                    line_buffer[line_idx++] = rx_buffer[i];
                }

                if(rx_buffer[i] == '\n' || line_idx >= 120)
                {
                    line_buffer[line_idx] = '\0';

                    while(line_idx > 0 && (line_buffer[line_idx-1] == '\n' || line_buffer[line_idx-1] == ' ')) {
                        line_buffer[--line_idx] = '\0';
                    }

                    if(strlen(line_buffer) > 0)
                    {
                        char temp_parse_buf[1024];
                        strcpy(temp_parse_buf, line_buffer);

                        for(int j = 0; line_buffer[j] != '\0'; j++) {
                            if(line_buffer[j] == ',') line_buffer[j] = ' ';
                        }

                        float current_temp = 0.0;
                        char current_gas = '1';
                        
                        char *t_ptr = strstr(temp_parse_buf, "TEMP=");
                        if(t_ptr) sscanf(t_ptr, "TEMP=%f", &current_temp);
                        
                        char *g_ptr = strstr(temp_parse_buf, "GAS=");
                        if(g_ptr) sscanf(g_ptr, "GAS=%c", &current_gas);

                        int is_sensor_danger = (current_gas == '0' || current_temp >= 30.0);

                        if(is_sensor_danger)
                        {
                            if(user_forced_normal == 0)
                            {
                                global_emergency_active = 1;

                                if(emergency_logged == 0)
                                {
                                    printf("\n🚨 비상모드 발동 (GAS=%c, TEMP=%.1f)\n", current_gas, current_temp);
                                    fflush(stdout);
                                    emergency_logged = 1; 
                                    
                                    BUZZER_ON();
                                    ALL_MOTORS_OFF(); 
                                }
                            }
                        }
                        else
                        {
                            if(user_forced_normal == 1) {
                                printf("🟢 [센서 안전] 센서 값이 정상 범위로 복귀하여 비상 시스템이 재활성화됩니다.\n");
                                fflush(stdout);
                            }
                            user_forced_normal = 0;
                        }

                        printf("%s\n", line_buffer);
                        fflush(stdout);

                        FILE *s_file = fopen("/tmp/sensor_data.txt", "w");
                        if(s_file != NULL)
                        {
                            if(global_emergency_active && user_forced_normal == 0) {
                                fprintf(s_file, "%s MODE=EMERGENCY\n", temp_parse_buf);
                            } else {
                                fprintf(s_file, "%s MODE=NORMAL\n", temp_parse_buf);
                            }
                            fclose(s_file);
                        }

                        if (strstr(line_buffer, "clear_emergency") != NULL)
                        {
                            global_emergency_active = 0; 
                            user_forced_normal = 1;      
                            
                            LED_OFF();
                            BUZZER_OFF();
                            ALL_MOTORS_OFF();
                            emergency_logged = 0;
                            
                            FILE *s_file_off = fopen("/tmp/sensor_data.txt", "w");
                            if(s_file_off != NULL)
                            {
                                fprintf(s_file_off, "%s MODE=NORMAL\n", temp_parse_buf); 
                                fclose(s_file_off);
                            }
                            
                            printf("\n🟢 사용자에 의해 비상상황이 정상 해제되었습니다.\n");
                            fflush(stdout);
                        }
                        else
                        {
                            if(strstr(line_buffer, "led_on"))       { LED_ON(); }
                            else if(strstr(line_buffer, "led_off")) { LED_OFF(); }
                            
                            if(strstr(line_buffer, "buzzer_on"))    { BUZZER_ON(); }
                            else if(strstr(line_buffer, "buzzer_off")){ BUZZER_OFF(); }
                            
                            if(strstr(line_buffer, "motor1_on")) { 
                                MOTOR_1_ON(); 
                                usleep(100000); // 0.1초 작동
                                MOTOR_1_OFF(); 
                            }
                            else if(strstr(line_buffer, "motor1_off")) { MOTOR_1_OFF(); }
                            
                            if(strstr(line_buffer, "motor2_on")) { 
                                MOTOR_2_ON(); 
                                usleep(100000);  // 0.1초 작동
                                MOTOR_2_OFF(); 
                            }
                            else if(strstr(line_buffer, "motor2_off")) { MOTOR_2_OFF(); }

                            /* ⏱️ 모터3 시리얼 제어: 3초(3,000,000us) 기동 후 자동 OFF */
                            if(strstr(line_buffer, "motor3_on")) { 
                                MOTOR_3_ON(); 
                                usleep(2500000); 
                                MOTOR_3_OFF(); 
                            }
                            else if(strstr(line_buffer, "motor3_off")) { MOTOR_3_OFF(); }

                            /* ⏱️ 모터4 시리얼 제어: 0.05초(50,000us) 기동 후 자동 OFF */
                            if(strstr(line_buffer, "motor4_on")) { 
                                MOTOR_4_ON(); 
                                usleep(100000); 
                                MOTOR_4_OFF(); 
                            }
                            else if(strstr(line_buffer, "motor4_off")) { MOTOR_4_OFF(); }
                        }
                    }

                    line_idx = 0;
                    memset(line_buffer, 0, sizeof(line_buffer));
                }
            }
        }
        usleep(100000);
    }
    return NULL;
}

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

void SetGPIODirection(const char* pin_num, const char* direction)
{
    char path[100];
    snprintf(path, sizeof(path), SYSFS_GPIO_DIR "/gpio%s/direction", pin_num);
    int fd = open(path, O_WRONLY);
    if(fd != -1)
    {
        write(fd, direction, strlen(direction));
        close(fd);
    }
}

void WriteGPIOValue(const char* pin_num, int value)
{
    char path[100];
    snprintf(path, sizeof(path), SYSFS_GPIO_DIR "/gpio%s/value", pin_num);
    
    // O_SYNC 플래그를 추가하여 시스템 버퍼링 없이 즉시 하드웨어 핀에 쓰도록 강제 유도합니다.
    int fd = open(path, O_WRONLY | O_SYNC);
    if(fd != -1)
    {
        write(fd, value ? "1" : "0", 1);
        
        // 커널 단에 핀 상태 변화를 즉시 반영하라고 하드웨어 동기화 명령 실행
        fsync(fd); 
        close(fd);
    }
}

/* ================= DEVICE 제어부 ================= */
void LED_ON()         { WriteGPIOValue(LED_PIN,1); }
void LED_OFF()        { WriteGPIOValue(LED_PIN,0); }
void BUZZER_ON()      { WriteGPIOValue(BUZZER_PIN,1); }
void BUZZER_OFF()     { WriteGPIOValue(BUZZER_PIN,0); }

void MOTOR_1_ON()     { WriteGPIOValue(MOTOR_1_PIN,1); }
void MOTOR_1_OFF()    { WriteGPIOValue(MOTOR_1_PIN,0); }
void MOTOR_2_ON()     { WriteGPIOValue(MOTOR_2_PIN,1); }
void MOTOR_2_OFF()    { WriteGPIOValue(MOTOR_2_PIN,0); }
void MOTOR_3_ON()     { WriteGPIOValue(MOTOR_3_PIN,1); }
void MOTOR_3_OFF()    { WriteGPIOValue(MOTOR_3_PIN,0); }
void MOTOR_4_ON()     { WriteGPIOValue(MOTOR_4_PIN,1); }
void MOTOR_4_OFF()    { WriteGPIOValue(MOTOR_4_PIN,0); }

void ALL_MOTORS_OFF() { 
    WriteGPIOValue(MOTOR_1_PIN,0); 
    WriteGPIOValue(MOTOR_2_PIN,0); 
    WriteGPIOValue(MOTOR_3_PIN,0); 
    WriteGPIOValue(MOTOR_4_PIN,0); 
}

/* ================= HTTP HANDLER ================= */
void handle_request(int client_fd, char *request)
{
    char response[512];

    if(strstr(request,"GET /led/on"))          LED_ON();
    else if(strstr(request,"GET /led/off"))     LED_OFF();
    else if(strstr(request,"GET /buzzer/on"))    BUZZER_ON();
    else if(strstr(request,"GET /buzzer/off"))   BUZZER_OFF();
    
    else if(strstr(request,"GET /motor1/on")) { 
        MOTOR_1_ON(); 
        usleep(100000); // 0.1초 가동
        MOTOR_1_OFF(); 
    }
    else if(strstr(request,"GET /motor1/off"))   MOTOR_1_OFF();
    
    else if(strstr(request,"GET /motor2/on")) { 
        MOTOR_2_ON(); 
        usleep(100000);  // 0.1초 가동
        MOTOR_2_OFF(); 
    }
    else if(strstr(request,"GET /motor2/off"))   MOTOR_2_OFF();
    
    /* ⏱️ 모터3 웹 요청 제어: 3초(3,000,000us) 기동 후 자동 OFF */
    else if(strstr(request,"GET /motor3/on")) { 
        MOTOR_3_ON(); 
        usleep(2500000); 
        MOTOR_3_OFF(); 
    }
    else if(strstr(request,"GET /motor3/off"))   MOTOR_3_OFF();
    
    /* ⏱️ 모터4 웹 요청 제어: 0.05초(50,000us) 기동 후 자동 OFF */
    else if(strstr(request,"GET /motor4/on")) { 
        MOTOR_4_ON(); 
        usleep(100000); 
        MOTOR_4_OFF(); 
    }
    else if(strstr(request,"GET /motor4/off"))   MOTOR_4_OFF();
    
    else if(strstr(request,"GET /emergency_off")) {
        global_emergency_active = 0; 
        user_forced_normal = 1;      
        
        LED_OFF();
        BUZZER_OFF();
        ALL_MOTORS_OFF();
        
        FILE *s_file_off = fopen("/tmp/sensor_data.txt", "w");
        if(s_file_off != NULL)
        {
            fprintf(s_file_off, "TEMP=24.5,HUMI=50.0,CDS=50,GAS=1 MODE=NORMAL\n"); 
            fclose(s_file_off);
        }
        
        printf("\n🟢 비상상황이 완벽하게 정상 해제되었습니다.\n");
        fflush(stdout);
    }

    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain\r\n\r\nOK");

    write(client_fd, response, strlen(response));
}

/* ================= MAIN ================= */
int main(int argc, char *argv[])
{
    int server_fd;
    int client_fd;
    struct sockaddr_in addr;
    char buffer[2048];

    ExportGPIO(LED_PIN);
    ExportGPIO(BUZZER_PIN);
    ExportGPIO(MOTOR_1_PIN);
    ExportGPIO(MOTOR_2_PIN);
    ExportGPIO(MOTOR_3_PIN);
    ExportGPIO(MOTOR_4_PIN); 

    usleep(100000); 

    SetGPIODirection(LED_PIN,"out");
    SetGPIODirection(BUZZER_PIN,"out");
    SetGPIODirection(MOTOR_1_PIN,"out");
    SetGPIODirection(MOTOR_2_PIN,"out");
    SetGPIODirection(MOTOR_3_PIN,"out");
    SetGPIODirection(MOTOR_4_PIN,"out"); 

    LED_OFF();
    BUZZER_OFF();
    ALL_MOTORS_OFF();

    if(argc > 1)
    {
        if(strcmp(argv[1],"led_on")==0)         LED_ON();
        else if(strcmp(argv[1],"led_off")==0)    LED_OFF();
        else if(strcmp(argv[1],"buzzer_on")==0)  BUZZER_ON();
        else if(strcmp(argv[1],"buzzer_off")==0) BUZZER_OFF();
        else if(strcmp(argv[1],"motor1_on")==0)  { MOTOR_1_ON(); usleep(100000); MOTOR_1_OFF(); }
        else if(strcmp(argv[1],"motor1_off")==0) MOTOR_1_OFF();
        else if(strcmp(argv[1],"motor2_on")==0)  { MOTOR_2_ON(); usleep(100000); MOTOR_2_OFF(); }
        else if(strcmp(argv[1],"motor2_off")==0) MOTOR_2_OFF();
        
        /* ⏱️ 터미널 아규먼트 제어: 모터3 (3초 기동 후 OFF) */
        else if(strcmp(argv[1],"motor3_on")==0)  { MOTOR_3_ON(); usleep(2500000); MOTOR_3_OFF(); }
        else if(strcmp(argv[1],"motor3_off")==0) MOTOR_3_OFF();
        
        /* ⏱️ 터미널 아규먼트 제어: 모터4 (0.05초 기동 후 OFF) */
        else if(strcmp(argv[1],"motor4_on")==0)  { MOTOR_4_ON(); usleep(100000); MOTOR_4_OFF(); }
        else if(strcmp(argv[1],"motor4_off")==0) MOTOR_4_OFF(); 
        return 0;
    }

    UART_Init();

    pthread_t uart_thread;
    pthread_create(&uart_thread, NULL, UART_Receiver, NULL);

    pthread_t blink_thread;
    pthread_create(&blink_thread, NULL, Emergency_Blink_Worker, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); 

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd,5);

    while(1)
    {
        client_fd = accept(server_fd, NULL, NULL);
        if(client_fd >= 0) {
            memset(buffer, 0, sizeof(buffer));
            read(client_fd, buffer, sizeof(buffer)-1);
            handle_request(client_fd, buffer);
            close(client_fd);
        }
    }

    return 0;
}