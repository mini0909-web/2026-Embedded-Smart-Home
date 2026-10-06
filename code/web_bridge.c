#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define NOTEBOOK_IP "192.168.55.3" 
#define NOTEBOOK_PORT 5002

#define FILE_PATH "/tmp/sensor_data.txt"

// 파싱된 데이터를 담을 구조체
typedef struct {
    float temp;
    float humi;
    int cds;
    int gas;
    char mode[32];
} SensorData;

int parse_sensor_file(SensorData *data) {
    FILE *fp = fopen(FILE_PATH, "r");
    if (!fp) return 0;

    char buf[1024] = {0};
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    data->temp = 24.5; data->humi = 60.0; data->cds = 20; data->gas = 1; 
    strcpy(data->mode, "NORMAL");

    // 공백을 쉼표로 변환
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == ' ') buf[i] = ', ';
    }

    // 1. 문자열 전체를 먼저 검사하여 '명확한 비상 조건' 확인
    // 파일 내용에 확실하게 가스 누출(GAS=0)이 찍혀있을 때만 비상모드로 판정
    if (strstr(buf, "GAS=0") != NULL) {
        data->gas = 0;
        strcpy(data->mode, "EMERGENCY");
    }

    // 2. 토큰별 세부 파싱
    char *token = strtok(buf, ", ");
    while (token != NULL) {
        if (strstr(token, "TEMP=") == token) sscanf(token, "TEMP=%f", &data->temp);
        else if (strstr(token, "HUMI=") == token) sscanf(token, "HUMI=%f", &data->humi);
        else if (strstr(token, "LIGHT=") == token) sscanf(token, "LIGHT=%d", &data->cds);
        else if (strstr(token, "CDS=") == token) sscanf(token, "CDS=%d", &data->cds);
        else if (strstr(token, "GAS=") == token) {
            // 위에서 GAS=0을 체크 못했더라도 여기서 다시 한번 확인
            int g_val = 1;
            sscanf(token, "GAS=%d", &g_val);
            if (g_val == 0) {
                data->gas = 0;
                strcpy(data->mode, "EMERGENCY");
            }
        }
        token = strtok(NULL, ", ");
    }

    if (data->temp >= 30.0) {
        strcpy(data->mode, "EMERGENCY");
    }

    return 1;
}

void send_to_notebook(SensorData *data) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(NOTEBOOK_PORT);
    serv_addr.sin_addr.s_addr = inet_addr(NOTEBOOK_IP);

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        // 원래 있던 실패 메시지를 그대로 유지하되 주소 정보 추가 출력
        
        close(sock);
        return; 
    }

    // 전송할 JSON 문자열 조립
    char json_body[512];
    snprintf(json_body, sizeof(json_body),
             "{\"temperature\":%.1f,\"humidity\":%.1f,\"cds\":%d,\"gas\":%d,\"mode\":\"%s\"}",
             data->temp, data->humi, data->cds, data->gas, data->mode);

    // HTTP Request Header 및 Body 조립
    char http_req[1024];
    snprintf(http_req, sizeof(http_req),
             "POST /api/update_sensor HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n"
             "%s",
             NOTEBOOK_IP, NOTEBOOK_PORT, (int)strlen(json_body), json_body);

    write(sock, http_req, strlen(http_req));
    
    
    close(sock);
}

int main() {
   
    sleep(3); 

    time_t last_mtime = 0;
    struct stat file_stat;

    while (1) {
        if (stat(FILE_PATH, &file_stat) == 0) {
            if (file_stat.st_mtime != last_mtime) {
                last_mtime = file_stat.st_mtime;
                SensorData data;
                if (parse_sensor_file(&data)) {
                    send_to_notebook(&data);
                }
            }
        }
        usleep(200000);
    }
    return 0;
}