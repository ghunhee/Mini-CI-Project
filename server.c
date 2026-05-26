#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>

#define PORT 8080
#define BUFFER_SIZE 8192

void *client_handler(void *socket_desc);
void run_sandboxed_test(int client_sock, const char *dir_template, char *source_code);

int main() {
    int server_fd, client_sock, *new_sock;
    struct sockaddr_in server, client;
    socklen_t c = sizeof(struct sockaddr_in);

    // 1. 서버 소켓 생성
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Could not create socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("bind failed");
        return 1;
    }

    listen(server_fd, 5);
    printf("🚀 Mini CI Server started on port %d (Waiting for connections...)\n", PORT);

    // 2. 메인 루프: 멀티 클라이언트 처리를 위한 Pthread 활용
    while ((client_sock = accept(server_fd, (struct sockaddr *)&client, &c))) {
        printf("[+] 새로운 클라이언트 연결됨\n");
        pthread_t sniffer_thread;
        new_sock = malloc(sizeof(int));
        *new_sock = client_sock;
        
        if (pthread_create(&sniffer_thread, NULL, client_handler, (void*)new_sock) < 0) {
            perror("could not create thread");
            return 1;
        }
        pthread_detach(sniffer_thread); // 스레드 자원 자동 반환
    }
    return 0;
}

void *client_handler(void *socket_desc) {
    int sock = *(int*)socket_desc;
    free(socket_desc);
    
    char buffer[BUFFER_SIZE] = {0};
    int read_size = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    
    if (read_size > 0) {
        // 3. 각 테스트 쓰레드가 독립적으로 동작할 수 있도록 임시 디렉토리 생성
        char dir_template[] = "/tmp/minici_XXXXXX";
        if (mkdtemp(dir_template) == NULL) {
            perror("mkdtemp failed");
            close(sock);
            return NULL;
        }
        
        run_sandboxed_test(sock, dir_template, buffer);
        
        // 작업 완료 후 임시 디렉토리 및 파일들 정리
        char rm_cmd[256];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", dir_template);
        system(rm_cmd);
    }
    close(sock);
    return NULL;
}

void run_sandboxed_test(int client_sock, const char *dir_template, char *source_code) {
    char source_path[256];
    snprintf(source_path, sizeof(source_path), "%s/source.c", dir_template);
    
    FILE *fp = fopen(source_path, "w");
    if (fp) {
        fputs(source_code, fp);
        fclose(fp);
    }
    
    int pipe_err[2];
    pipe(pipe_err); // gcc stderr 캡처용 파이프
    
    pid_t pid = fork();
    if (pid == 0) {
        // ======================= 자식 프로세스 (격리 환경) =======================
        close(pipe_err[0]);
        
        // Namespaces를 이용한 Sandboxing (root 권한 필요)
        unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWNET);
        chdir(dir_template);
        
        // (1) 빌드 프로세스 격리 실행
        pid_t compile_pid = fork();
        if (compile_pid == 0) {
            dup2(pipe_err[1], STDERR_FILENO); // 에러 출력을 파이프로 리다이렉션
            execlp("gcc", "gcc", "source.c", "-o", "exec_out", NULL);
            exit(1);
        }
        
        int status;
        waitpid(compile_pid, &status, 0);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            exit(125); // 컴파일 실패 코드
        }
        close(pipe_err[1]); // 컴파일 성공 시 파이프 쓰기 닫음
        
        // (2) 컴파일 성공 코드 실행 프로세스
        pid_t exec_pid = fork();
        if (exec_pid == 0) {
            // 무한 루프 방지를 위한 setitimer 타임아웃 설정 (2초)
            struct itimerval timer;
            timer.it_value.tv_sec = 2;
            timer.it_value.tv_usec = 0;
            timer.it_interval.tv_sec = 0;
            timer.it_interval.tv_usec = 0;
            setitimer(ITIMER_REAL, &timer, NULL);
            
            int out_fd = open("run_output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(out_fd, STDOUT_FILENO); // 표준 출력을 파일로 저장
            close(out_fd);
            
            execl("./exec_out", "./exec_out", NULL);
            exit(1);
        }
        
        int exec_status;
        struct rusage usage;
        wait4(exec_pid, &exec_status, 0, &usage); // 자원 측정 및 상태 대기
        
        FILE *uf = fopen("usage.txt", "w");
        if (uf) {
            long utime = usage.ru_utime.tv_sec * 1000 + usage.ru_utime.tv_usec / 1000;
            long stime = usage.ru_stime.tv_sec * 1000 + usage.ru_stime.tv_usec / 1000;
            int code = -1;
            
            if (WIFSIGNALED(exec_status) && WTERMSIG(exec_status) == SIGALRM) {
                code = 124; // Timeout 에러 코드 매핑
            } else if (WIFEXITED(exec_status)) {
                code = WEXITSTATUS(exec_status);
            } else if (WIFSIGNALED(exec_status)) {
                code = 128 + WTERMSIG(exec_status);
            }
            fprintf(uf, "%ld\n%ld\n%d\n", utime, stime, code);
            fclose(uf);
        }
        exit(0);
        // =======================================================================
    } else {
        // 부모 쓰레드
        close(pipe_err[1]);
        int status;
        waitpid(pid, &status, 0);
        
        char response[BUFFER_SIZE] = {0};
        
        // 컴파일 에러 파싱 로직
        if (WIFEXITED(status) && WEXITSTATUS(status) == 125) {
            char err_buf[4096] = {0};
            read(pipe_err[0], err_buf, sizeof(err_buf)-1);
            
            int err_line = -1;
            char *ptr = err_buf;
            while ((ptr = strstr(ptr, "source.c:"))) {
                if (sscanf(ptr, "source.c:%d:", &err_line) == 1) {
                    break; // 첫 번째로 숫자가 파싱되는 곳이 실제 에러 라인
                }
                ptr += 9; // "source.c:" 길이만큼 이동해서 계속 찾음
            }
            
            char friendly_msg[512] = "💡 힌트: 일반적인 문법 오류입니다. 상세 로그를 확인해 주세요.";
            if (strstr(err_buf, "expected ',' or ';'")) {
                strcpy(friendly_msg, "💡 힌트: 줄 끝에 세미콜론(;)이 빠진 것 같습니다!");
            } else if (strstr(err_buf, "undeclared")) {
                strcpy(friendly_msg, "💡 힌트: 선언되지 않은 변수를 사용했습니다. 오타나 변수 선언을 확인하세요!");
            } else if (strstr(err_buf, "expected ')'") || strstr(err_buf, "expected '}'")) {
                strcpy(friendly_msg, "💡 힌트: 괄호가 제대로 닫히지 않았습니다!");
            } else if (strstr(err_buf, "implicit declaration")) {
                strcpy(friendly_msg, "💡 힌트: 헤더 파일(#include)이 누락되었거나 함수 선언이 없습니다!");
            }
            
            snprintf(response, sizeof(response), 
                     "COMPILE_ERROR\nLINE:%d\n\n[한국어 에러 가시화]\n컴파일 중 에러가 발생했습니다.\n에러 라인: %d번 줄\n%s\n\n상세 로그:\n%s", 
                     err_line, err_line, friendly_msg, err_buf);
            send(client_sock, response, strlen(response), 0);
        } else {
            // 실행 결과 수집 및 응답
            char usage_path[256];
            snprintf(usage_path, sizeof(usage_path), "%s/usage.txt", dir_template);
            FILE *uf = fopen(usage_path, "r");
            long user_ms = 0, sys_ms = 0;
            int exec_code = 0;
            if (uf) {
                fscanf(uf, "%ld\n%ld\n%d", &user_ms, &sys_ms, &exec_code);
                fclose(uf);
            }
            
            if (exec_code == 124) { // Timeout
                snprintf(response, sizeof(response), "RUNTIME_ERROR\n[실행 시간 초과]\n무한 루프 혹은 타임아웃(2초)이 발생하여 강제 종료되었습니다.\n");
                send(client_sock, response, strlen(response), 0);
            } else {
                char out_path[256];
                snprintf(out_path, sizeof(out_path), "%s/run_output.txt", dir_template);
                FILE *f = fopen(out_path, "r");
                char out_buf[4096] = {0};
                if (f) {
                    fread(out_buf, 1, sizeof(out_buf)-1, f);
                    fclose(f);
                }
                snprintf(response, sizeof(response), 
                         "SUCCESS\n[실행 완료]\n결과:\n%s\n[자원 사용량]\n사용자 CPU 시간(User Time): %ld ms\n시스템 CPU 시간(System Time): %ld ms\n종료 코드: %d\n", 
                         out_buf, user_ms, sys_ms, exec_code);
                send(client_sock, response, strlen(response), 0);
            }
        }
        close(pipe_err[0]);
    }
}
