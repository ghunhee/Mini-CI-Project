import os
import re
import sys

def apply():
    with open("protocol.h", "r", encoding="utf-8") as f:
        proto = f.read()

    status_payload = """
typedef struct {
    uint16_t submitted;
    uint16_t total;
    char     week[MAX_WEEK_NAME];
    uint32_t csv_records;

    struct {
        char    student_id[MAX_STUDENT_ID];
        uint8_t result;
    } entries[512];
    int entry_count;
} StatusPayload;

static int parse_status_payload(const char *data, StatusPayload *out) {
    memset(out, 0, sizeof(*out));
    const char *p = data;
    char key[32], val[64];
    while (*p && *p != '\\n') {
        if (sscanf(p, "%31[^=]=%63[^|]|", key, val) == 2) {
            if (strcmp(key, "submitted") == 0) out->submitted = (uint16_t)atoi(val);
            else if (strcmp(key, "total") == 0) out->total = (uint16_t)atoi(val);
            else if (strcmp(key, "week") == 0) strncpy(out->week, val, MAX_WEEK_NAME - 1);
            else if (strcmp(key, "records") == 0) out->csv_records = (uint32_t)atoi(val);
        }
        p = strchr(p, '|');
        if (!p) break;
        p++;
    }
    p = strchr(data, '\\n');
    if (!p) return 0;
    p++;
    static const char *grade_names[] = {"AC","WA","TLE","RE","CE",NULL};
    int idx = 0;
    while (*p && idx < 512) {
        char sid[MAX_STUDENT_ID] = {0};
        char gname[8] = {0};
        if (sscanf(p, "%15[^:]:%7[^,\\n]", sid, gname) == 2) {
            strncpy(out->entries[idx].student_id, sid, MAX_STUDENT_ID - 1);
            out->entries[idx].result = 1;
            for (int g = 0; grade_names[g]; g++) {
                if (strcmp(gname, grade_names[g]) == 0) {
                    out->entries[idx].result = (uint8_t)g;
                    break;
                }
            }
            idx++;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    out->entry_count = idx;
    return 0;
}
#endif // PROTOCOL_H
"""
    proto = proto.replace("#endif // PROTOCOL_H", status_payload)
    with open("include/protocol.h", "w", encoding="utf-8") as f:
        f.write(proto)
    print("protocol.h updated")

    # Update server.c
    with open("server/server.c", "r", encoding="utf-8") as f:
        server = f.read()

    server = server.replace('#include "../include/protocol.h"', 
'''#include "../include/protocol.h"
#include "../ipc_protocol.h"
#include "../server_ctx.h"''')

    server = server.replace('FileHeader fheader;', 'SubmitHeader fheader;')
    server = server.replace('fheader.file_size = ntohl(fheader.file_size);', '')
    server = server.replace('if (recv(client_sock, &fheader, sizeof(fheader), MSG_WAITALL) <= 0) {',
'''if (recv(client_sock, &fheader, sizeof(fheader), MSG_WAITALL) <= 0) {
        close(client_sock); return;
    }
    fheader.file_size = ntohl(fheader.file_size);''')

    # Add build_and_send_result
    func_bns = '''static void build_and_send_result(int cli_fd, SubmitHeader hdr, GradeResult grade, uint32_t exec_ms, uint32_t mem_kb, const char *err_msg) {
    ResultHeader resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic        = htonl(MAGIC_NUMBER);
    resp.version      = PROTO_VERSION;
    resp.pkt_type     = RES_RESULT;
    resp.header_size  = htons((uint16_t)sizeof(ResultHeader));
    resp.result       = (uint8_t)grade;
    resp.exec_time_ms = htonl(exec_ms);
    resp.memory_kb    = htonl(mem_kb);
    strncpy(resp.student_id, hdr.student_id, MAX_STUDENT_ID);
    strncpy(resp.filename,   hdr.filename,   MAX_FILENAME);
    if (err_msg) strncpy(resp.message, err_msg, sizeof(resp.message) - 1);
    
    pthread_rwlock_rdlock(&g_ctx.hw_lock);
    strncpy(resp.week_name, g_ctx.current_hw, MAX_WEEK_NAME);
    pthread_rwlock_unlock(&g_ctx.hw_lock);

    pthread_mutex_lock(&g_ctx.tracker_lock);
    resp.submitted_count = htons(g_ctx.submitted_count);
    resp.total_students  = htons(g_ctx.total_students);
    pthread_mutex_unlock(&g_ctx.tracker_lock);

    if (send(cli_fd, &resp, sizeof(resp), MSG_NOSIGNAL) < 0) perror("[worker] send");
    ctx_record_submission(&g_ctx, hdr.student_id, grade);
    ctx_write_csv(&g_ctx, hdr.student_id, grade, exec_ms, mem_kb);

    static const char *gnames[] = {"AC","WA","TLE","RE","CE"};
    const char *gs = (grade < 5) ? gnames[grade] : "UNK";
    fprintf(stderr, "[grade] [%-8s] %-32s %-4s  %4ums  %4uKB\\n", hdr.student_id, hdr.filename, gs, exec_ms, mem_kb);
}

void handle_client(int client_sock, int thread_id) {'''

    server = server.replace('void handle_client(int client_sock, int thread_id) {', func_bns)

    # Replace handle_client logic
    # Find start of "GradeResult final_res =" to "close(pipefd[0]);"
    
    rep_logic = '''
    GradeResult final_res = GRADE_AC;
    uint32_t final_exec = 0;
    uint32_t final_mem = 0;
    char error_msg[256] = {0};

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        final_res = GRADE_CE;
        ssize_t r = read(pipefd[0], error_msg, sizeof(error_msg) - 1);
        if (r >= 0) error_msg[r] = '\\0';
    } else {
        char in_path[512];
        snprintf(in_path, sizeof(in_path), "server/test_cases/case1_in.txt");
        if (access(in_path, F_OK) != 0) snprintf(in_path, sizeof(in_path), "server/input.txt");
        int in_fd = open(in_path, O_RDONLY);
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/output.txt", work_dir);
        int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        
        pid_t exec_pid = fork();
        if (exec_pid == 0) {
            close(pipefd[0]);
            dup2(in_fd, STDIN_FILENO);
            dup2(out_fd, STDOUT_FILENO);
            dup2(out_fd, STDERR_FILENO);
            close(in_fd); close(out_fd);
            
            struct rlimit rl_cpu = { timeout_seconds, timeout_seconds + 1 };
            setrlimit(RLIMIT_CPU, &rl_cpu);
            
            prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
            scmp_filter_ctx sctx = seccomp_init(SCMP_ACT_KILL);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(execve), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(exit_group), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(munmap), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(openat), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(close), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(pread64), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(arch_prctl), 0);
            seccomp_rule_add(sctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);
            seccomp_load(sctx);
            
            execl("./sandbox_exec", "sandbox_exec", NULL);
            exit(1);
        }
        
        close(in_fd); close(out_fd);
        
        struct monitor_bpf *skel = monitor_bpf__open_and_load();
        if (skel) {
            skel->bss->target_pid = exec_pid;
            monitor_bpf__attach(skel);
        }
        
        int status;
        struct rusage usage;
        wait4(exec_pid, &status, 0, &usage);
        
        if (skel) monitor_bpf__destroy(skel);
        
        long user_ms  = usage.ru_utime.tv_sec * 1000 + usage.ru_utime.tv_usec / 1000;
        long max_rss  = usage.ru_maxrss;
        final_exec = user_ms;
        final_mem  = max_rss;
        
        if (WIFSIGNALED(status) && (WTERMSIG(status) == SIGALRM || WTERMSIG(status) == SIGXCPU)) {
            final_res = GRADE_TLE;
        } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS) {
            final_res = GRADE_RE;
        } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
            final_res = GRADE_RE;
        } else if (WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0)) {
            final_res = GRADE_RE;
        } else {
            char expected_path[512];
            pthread_rwlock_rdlock(&g_ctx.hw_lock);
            strncpy(expected_path, g_ctx.ans_path, sizeof(expected_path));
            pthread_rwlock_unlock(&g_ctx.hw_lock);
            
            int diff_res = compare_output(out_path, expected_path);
            if (diff_res == 1) final_res = GRADE_AC;
            else if (diff_res == 0) final_res = GRADE_WA;
            else final_res = GRADE_AC;
        }
    }
    
    close(pipefd[0]);
    build_and_send_result(client_sock, fheader, final_res, final_exec, final_mem, error_msg);
    close(client_sock);
'''

    # Need to regex replace
    server = re.sub(r'if \(\!WIFEXITED\(status\).*?close\(client_sock\);', rep_logic, server, flags=re.DOTALL)
    
    # Update handle_ctl_client
    ipc = '''
        case CMD_SET_HW: {
            char week[MAX_WEEK_NAME] = {0};
            strncpy(week, req.payload, MAX_WEEK_NAME - 1);
            week[strcspn(week, " \\r\\n")] = '\\0';
            ctx_set_hw(&g_ctx, week);
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "OK|week=%s", g_ctx.current_hw);
            break;
        }
        case CMD_SET_STUDENTS: {
            int n = atoi(req.payload);
            pthread_mutex_lock(&g_ctx.tracker_lock);
            g_ctx.total_students = (uint16_t)n;
            pthread_mutex_unlock(&g_ctx.tracker_lock);
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "OK|total=%d", n);
            break;
        }
        case CMD_GET_TRACKER: {
            resp.status = IPC_OK;
            uint16_t snap_submitted, snap_total;
            uint32_t snap_records;
            char snap_week[MAX_WEEK_NAME];
            pthread_mutex_lock(&g_ctx.tracker_lock);
            snap_submitted = g_ctx.submitted_count;
            snap_total     = g_ctx.total_students;
            pthread_mutex_unlock(&g_ctx.tracker_lock);
            pthread_rwlock_rdlock(&g_ctx.hw_lock);
            strncpy(snap_week, g_ctx.current_hw, MAX_WEEK_NAME - 1);
            snap_week[MAX_WEEK_NAME - 1] = '\\0';
            pthread_rwlock_unlock(&g_ctx.hw_lock);
            pthread_mutex_lock(&g_ctx.csv_lock);
            snap_records = g_ctx.csv.total_records;
            pthread_mutex_unlock(&g_ctx.csv_lock);
            
            int pos = snprintf(resp.data, IPC_MAX_PAYLOAD, "submitted=%u|total=%u|week=%s|records=%u|\\n",
                               snap_submitted, snap_total, snap_week, snap_records);
            static const char *gnames[] = {"AC","WA","TLE","RE","CE"};
            pthread_mutex_lock(&g_ctx.tracker_lock);
            for (int i = 0; i < MAX_STUDENTS && pos < IPC_MAX_PAYLOAD - 24; i++) {
                if (g_ctx.students[i].student_id[0] == '\\0') break;
                uint8_t r = g_ctx.students[i].best_result;
                const char *gs = (r < 5) ? gnames[r] : "UNK";
                pos += snprintf(resp.data + pos, IPC_MAX_PAYLOAD - pos, "%s:%s,", g_ctx.students[i].student_id, gs);
            }
            pthread_mutex_unlock(&g_ctx.tracker_lock);
            break;
        }
        case CMD_GET_CSV_STAT: {
            pthread_mutex_lock(&g_ctx.csv_lock);
            uint32_t rec = g_ctx.csv.total_records;
            time_t   ts  = g_ctx.csv.last_write_ts;
            pthread_mutex_unlock(&g_ctx.csv_lock);
            char ts_str[32] = "never";
            if (ts > 0) {
                struct tm *tm = localtime(&ts);
                strftime(ts_str, sizeof(ts_str), "%Y-%m-%dT%H:%M:%S", tm);
            }
            resp.status = IPC_OK;
            snprintf(resp.data, IPC_MAX_PAYLOAD, "records=%u|last_write=%s", rec, ts_str);
            break;
        }
        case CMD_STATUS: {
'''
    server = server.replace('case CMD_STATUS: {', ipc)

    # Initialize ctx in main
    server = server.replace('int main() {', 'int main() {\n    ctx_init(&g_ctx);')

    with open("server/server.c", "w", encoding="utf-8") as f:
        f.write(server)
    print("server.c updated")

apply()
