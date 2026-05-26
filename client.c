#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define PORT 8080
#define BUFFER_SIZE 8192

// ================= 색상 코드 정의 =================
#define C_RESET   "\x1b[0m"
#define C_RED     "\x1b[1;31m"
#define C_GREEN   "\x1b[1;32m"
#define C_YELLOW  "\x1b[1;33m"
#define C_BLUE    "\x1b[1;34m"
#define C_CYAN    "\x1b[1;36m"
// =================================================

typedef struct {
    char filename[256];
    int err_line;
} ErrorRecord;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("사용법: %s <C소스파일1> [C소스파일2] ...\n", argv[0]);
        return 1;
    }

    ErrorRecord errors[100];
    int error_count = 0;
    int success_count = 0;

    printf("\n%s🚀 총 %d개의 파일에 대해 배치(Batch) 검사를 시작합니다...%s\n", C_CYAN, argc - 1, C_RESET);
    printf("%s==================================================%s\n\n", C_CYAN, C_RESET);

    for (int i = 1; i < argc; i++) {
        const char *filename = argv[i];
        FILE *f = fopen(filename, "r");
        if (!f) {
            printf("%s[-] %s: 파일 열기 실패%s\n\n", C_RED, filename, C_RESET);
            continue;
        }

        char file_buf[BUFFER_SIZE] = {0};
        fread(file_buf, 1, sizeof(file_buf)-1, f);
        fclose(f);

        int sock = 0;
        struct sockaddr_in serv_addr;
        
        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(PORT);

        if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) return -1;
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            printf("%s연결 실패. 서버가 실행 중인지 확인하세요.%s\n\n", C_RED, C_RESET);
            return -1;
        }

        send(sock, file_buf, strlen(file_buf), 0);

        char buffer[BUFFER_SIZE] = {0};
        int read_size = read(sock, buffer, BUFFER_SIZE - 1);
        
        if (read_size > 0) {
            if (strncmp(buffer, "COMPILE_ERROR", 13) == 0) {
                int err_line = -1;
                char *line_ptr = strstr(buffer, "LINE:");
                if (line_ptr) sscanf(line_ptr, "LINE:%d", &err_line);
                
                printf("%s❌ [%s] 컴파일 실패! (에러 라인: %d)%s\n", C_RED, filename, err_line, C_RESET);
                
                char *msg_ptr = strstr(buffer, "[한국어 에러 가시화]");
                if (msg_ptr) {
                    char *hint_start = strstr(msg_ptr, "💡");
                    if (hint_start) {
                        char hint[256] = {0};
                        char *hint_end = strchr(hint_start, '\n');
                        if (hint_end) {
                            strncpy(hint, hint_start, hint_end - hint_start);
                            printf("   -> %s%s%s\n", C_YELLOW, hint, C_RESET);
                        }
                    }
                }

                char *raw_log_start = strstr(buffer, "[컴파일러 원본 로그]");
                if (raw_log_start && msg_ptr) {
                    char *log_begin = raw_log_start + strlen("[컴파일러 원본 로그]");
                    if (*log_begin == '\n') log_begin++;
                    int log_len = msg_ptr - log_begin;
                    if (log_len > 0 && log_len < BUFFER_SIZE) {
                        char raw_log[BUFFER_SIZE] = {0};
                        strncpy(raw_log, log_begin, log_len);
                        printf("   %s[상세 컴파일러 로그]%s\n", C_BLUE, C_RESET);
                        char *line = strtok(raw_log, "\n");
                        while (line != NULL) {
                            printf("      %s\n", line);
                            line = strtok(NULL, "\n");
                        }
                    }
                }

                if (err_line > 0) {
                    strncpy(errors[error_count].filename, filename, 255);
                    errors[error_count].err_line = err_line;
                    error_count++;
                }
            } else if (strncmp(buffer, "RUNTIME_ERROR", 13) == 0) {
                printf("%s⚠️ [%s] 런타임 에러 (무한루프 등)%s\n", C_RED, filename, C_RESET);
                strncpy(errors[error_count].filename, filename, 255);
                errors[error_count].err_line = 0;
                error_count++;
            } else {
                printf("%s✅ [%s] 테스트 통과!%s\n", C_GREEN, filename, C_RESET);
                success_count++;
            }
            
            // 각 파일 검사 결과 사이에 한 줄 띄어쓰기(여백) 추가!
            printf("\n");
        }
        close(sock);
    }

    printf("%s==================================================%s\n", C_CYAN, C_RESET);
    printf("%s📊 검사 결과: 총 %d개 (성공: %d개, 에러: %d개)%s\n", C_CYAN, argc - 1, success_count, error_count, C_RESET);
    
    if (error_count > 0) {
        while (1) {
            printf("\n%s==================================================%s\n", C_BLUE, C_RESET);
            printf("%s🛠️  에러가 발생한 파일 목록 (총 %d개)%s\n", C_YELLOW, error_count, C_RESET);
            for (int i = 0; i < error_count; i++) {
                if (errors[i].err_line > 0) {
                    printf("  %s[%d]%s %s (에러 발생: %s%d번 줄%s)\n", C_CYAN, i + 1, C_RESET, errors[i].filename, C_RED, errors[i].err_line, C_RESET);
                } else {
                    printf("  %s[%d]%s %s (%s런타임 에러%s)\n", C_CYAN, i + 1, C_RESET, errors[i].filename, C_RED, C_RESET);
                }
            }
            printf("%s==================================================%s\n", C_BLUE, C_RESET);
            printf("수정할 옵션을 선택하세요.\n");
            printf("  (%s숫자%s) : 원하는 번호를 띄어쓰기로 여러 개 입력 (예: '1 2 4' -> 3개 파일 동시 팝업)\n", C_CYAN, C_RESET);
            printf("   (%sA%s)   : 1번부터 끝까지 한 번에 모두 동시에 열기\n", C_GREEN, C_RESET);
            printf("   (%sQ%s)   : 그만두고 프로그램 종료하기\n", C_RED, C_RESET);
            printf("%s>> 입력: %s", C_YELLOW, C_RESET);
            
            char input[256];
            if (fgets(input, sizeof(input), stdin) == NULL) break;
            
            if (input[0] == 'Q' || input[0] == 'q') {
                printf("\n%s수정 모드를 종료합니다.%s\n", C_GREEN, C_RESET);
                break;
            } else if (input[0] == 'A' || input[0] == 'a') {
                for (int i = 0; i < error_count; i++) {
                    printf("%s>> 팝업 창 여는 중: %s...%s\n", C_CYAN, errors[i].filename, C_RESET);
                    char cmd[512];
                    if (errors[i].err_line > 0) {
                        snprintf(cmd, sizeof(cmd), "cmd.exe /c start wsl --exec bash -c \"vi -c 'set number' +%d %s\"", errors[i].err_line, errors[i].filename);
                    } else {
                        snprintf(cmd, sizeof(cmd), "cmd.exe /c start wsl --exec bash -c \"vi -c 'set number' %s\"", errors[i].filename);
                    }
                    system(cmd);
                }
                printf("\n%s🎉 전체 파일을 동시에 열었습니다!%s\n", C_GREEN, C_RESET);
            } else {
                // 여러 숫자를 띄어쓰기로 분리하여 동시에 열기 (start 명령어에서 /wait 옵션 제거)
                char *token = strtok(input, " ,\n");
                while (token != NULL) {
                    int choice = atoi(token);
                    if (choice >= 1 && choice <= error_count) {
                        int idx = choice - 1;
                        printf("%s>> 팝업 창 여는 중: %s...%s\n", C_CYAN, errors[idx].filename, C_RESET);
                        char cmd[512];
                        if (errors[idx].err_line > 0) {
                            snprintf(cmd, sizeof(cmd), "cmd.exe /c start wsl --exec bash -c \"vi -c 'set number' +%d %s\"", errors[idx].err_line, errors[idx].filename);
                        } else {
                            snprintf(cmd, sizeof(cmd), "cmd.exe /c start wsl --exec bash -c \"vi -c 'set number' %s\"", errors[idx].filename);
                        }
                        system(cmd);
                    } else {
                        if (strlen(token) > 0) printf("%s⚠️ 무시됨: 잘못된 입력 '%s'%s\n", C_RED, token, C_RESET);
                    }
                    token = strtok(NULL, " ,\n");
                }
            }
        }
    } else {
        printf("%s🎉 모든 파일이 에러 없이 완벽합니다!%s\n", C_GREEN, C_RESET);
    }

    return 0;
}
