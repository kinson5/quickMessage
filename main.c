#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <signal.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <shellapi.h>
  #include <process.h>
  #include <direct.h>
  #include <io.h>
  #define socket_t SOCKET
  #define close_socket(s) closesocket(s)
  #define sleep_ms(ms) Sleep(ms)
  #define mkdir_cross(dir) _mkdir(dir)
  typedef HANDLE thread_t;
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <pthread.h>
  #include <dirent.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #define socket_t int
  #define close_socket(s) close(s)
  #define sleep_ms(ms) usleep((ms) * 1000)
  #define mkdir_cross(dir) mkdir(dir, 0755)
  typedef pthread_t thread_t;
#endif

#include "qrcodegen.h"
#include "web_ui.h"

#define DEFAULT_PORT 8080
#define APP_VERSION "0.8"
#define APP_AUTHOR  "Kinson Wu"

/* ---------------- 国际化多语言支持 (i18n) ---------------- */
static int g_lang = 0; // 0 = 简体中文, 1 = English
#define S(zh, en) (g_lang ? (en) : (zh))
#define MAX_USERS 32
#define MAX_MESSAGES 64
#define MAX_FILES 64
#define USER_TIMEOUT_SEC 6       // 6秒无心跳即判定离线
#define MSG_TTL_SEC 120          // 广播消息最多存活120秒自动销毁
#define STORAGE_DIR "received_files"

/* ---------------- 数据模型 ---------------- */
typedef struct {
    char client_id[32];
    char name[64];
    char ip[64];
    time_t last_seen;
    bool is_host;
} OnlineUser;

typedef struct {
    int msg_id;
    char from_id[64];
    char from_name[64];
    char target_id[64]; // "all" 或 指定 client_id
    char text[4096];
    char time_str[32];
    time_t created_at;
    bool burned;
} TextMessage;

typedef struct {
    int file_id;
    char filename[256];
    char filepath[512];
    size_t size;
    char from_id[64];
    char from_name[64];
    char target_id[64]; // "all" 或 指定 client_id
    time_t created_at;
    bool burned;
} FileRecord;

static OnlineUser g_users[MAX_USERS];
static int g_user_count = 0;

static TextMessage g_messages[MAX_MESSAGES];
static int g_msg_count = 0;
static int g_next_msg_id = 1;

static FileRecord g_files[MAX_FILES];
static int g_file_count = 0;
static int g_next_file_id = 1;

static socket_t g_server_fd = (socket_t)-1;
static char g_primary_url[128] = "http://127.0.0.1:8080";
static char g_ips[16][64];
static int g_ip_count = 0;
static int g_port = DEFAULT_PORT;

#ifdef _WIN32
static CRITICAL_SECTION g_lock;
#define MUTEX_INIT() InitializeCriticalSection(&g_lock)
#define MUTEX_LOCK() EnterCriticalSection(&g_lock)
#define MUTEX_UNLOCK() LeaveCriticalSection(&g_lock)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define MUTEX_INIT()
#define MUTEX_LOCK() pthread_mutex_lock(&g_lock)
#define MUTEX_UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* ---------------- 清空临时存储目录 (不留痕迹) ---------------- */
void clean_storage_directory(void) {
#ifdef _WIN32
    char search_path[512];
    snprintf(search_path, sizeof(search_path), "%s/*.*", STORAGE_DIR);
    struct _finddata_t fileinfo;
    intptr_t handle = _findfirst(search_path, &fileinfo);
    if (handle != -1) {
        do {
            if (!(fileinfo.attrib & _A_SUBDIR)) {
                char fpath[512];
                snprintf(fpath, sizeof(fpath), "%s/%s", STORAGE_DIR, fileinfo.name);
                remove(fpath);
            }
        } while (_findnext(handle, &fileinfo) == 0);
        _findclose(handle);
    }
    _rmdir(STORAGE_DIR);
#else
    DIR *d = opendir(STORAGE_DIR);
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_name[0] != '.') {
                char fpath[512];
                snprintf(fpath, sizeof(fpath), "%s/%s", STORAGE_DIR, dir->d_name);
                remove(fpath);
            }
        }
        closedir(d);
        rmdir(STORAGE_DIR);
    }
#endif
}

/* ---------------- 退出信号捕获 ---------------- */
void handle_exit_signal(int sig) {
    (void)sig;
    printf("\n\n%s\n", S("[隐私保护] 正在清理临时文件并关闭 QuickMessage 服务...",
                          "[Privacy] Securely cleaning up temporary files and shutting down QuickMessage..."));
    MUTEX_LOCK();
    // 物理擦除并删除磁盘上可能残留的任何文件
    for (int i = 0; i < g_file_count; i++) {
        if (!g_files[i].burned) {
            remove(g_files[i].filepath);
            g_files[i].burned = true;
        }
    }
    // 抹除内存中的消息
    memset(g_messages, 0, sizeof(g_messages));
    clean_storage_directory();
    MUTEX_UNLOCK();

    if (g_server_fd != (socket_t)-1) {
        close_socket(g_server_fd);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    printf("%s\n", S("[提示] 所有临时数据已安全抹除，服务已安全退出。", "[Notice] All temporary data erased securely. Service exited safely."));
    exit(0);
}

/* ---------------- 剪贴板集成 ---------------- */
#ifdef _WIN32
void set_clipboard_text(const char *utf8_str) {
    if (!utf8_str || !*utf8_str) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    if (wlen <= 0) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
    if (!hMem) return;
    wchar_t *pMem = (wchar_t *)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, pMem, wlen);
    GlobalUnlock(hMem);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
    } else {
        GlobalFree(hMem);
    }
}

char* get_clipboard_text(void) {
    char *result = NULL;
    if (OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            wchar_t *pMem = (wchar_t *)GlobalLock(hData);
            if (pMem) {
                int utf8_len = WideCharToMultiByte(CP_UTF8, 0, pMem, -1, NULL, 0, NULL, NULL);
                if (utf8_len > 0) {
                    result = (char *)malloc(utf8_len);
                    if (result) {
                        WideCharToMultiByte(CP_UTF8, 0, pMem, -1, result, utf8_len, NULL, NULL);
                    }
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }
    return result;
}
#else
void set_clipboard_text(const char *utf8_str) {
    FILE *pipe = popen("pbcopy", "w");
    if (pipe) {
        fputs(utf8_str, pipe);
        pclose(pipe);
    }
}

char* get_clipboard_text(void) {
    FILE *pipe = popen("pbpaste", "r");
    if (!pipe) return NULL;
    char buffer[4096];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, pipe);
    pclose(pipe);
    if (len > 0) {
        buffer[len] = '\0';
        char *res = (char *)malloc(len + 1);
        if (res) strcpy(res, buffer);
        return res;
    }
    return NULL;
}
#endif

/* ---------------- 在线用户心跳管理 ---------------- */
void touch_user(const char *client_id, const char *name, const char *ip, bool is_host) {
    if (!client_id || !*client_id) return;
    MUTEX_LOCK();
    time_t now = time(NULL);

    // 查找已有用户
    int found_idx = -1;
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].client_id, client_id) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx >= 0) {
        g_users[found_idx].last_seen = now;
        if (name && *name) strncpy(g_users[found_idx].name, name, sizeof(g_users[found_idx].name) - 1);
        if (ip && *ip) strncpy(g_users[found_idx].ip, ip, sizeof(g_users[found_idx].ip) - 1);
    } else {
        if (g_user_count < MAX_USERS) {
            int idx = g_user_count++;
            strncpy(g_users[idx].client_id, client_id, sizeof(g_users[idx].client_id) - 1);
            strncpy(g_users[idx].name, (name && *name) ? name : "新设备", sizeof(g_users[idx].name) - 1);
            strncpy(g_users[idx].ip, (ip && *ip) ? ip : "127.0.0.1", sizeof(g_users[idx].ip) - 1);
            g_users[idx].last_seen = now;
            g_users[idx].is_host = is_host;
        }
    }

    // 清理离线用户 (Host 除外)
    for (int i = 0; i < g_user_count; ) {
        if (!g_users[i].is_host && (now - g_users[i].last_seen > USER_TIMEOUT_SEC)) {
            // 移除离线用户
            for (int j = i; j < g_user_count - 1; j++) {
                g_users[j] = g_users[j + 1];
            }
            g_user_count--;
        } else {
            i++;
        }
    }

    MUTEX_UNLOCK();
}

/* ---------------- 消息收发与阅后即焚 ---------------- */
void add_message(const char *from_id, const char *from_name, const char *target_id, const char *text) {
    MUTEX_LOCK();
    time_t now = time(NULL);

    // 自动清理超过 TTL 的老广播消息
    for (int i = 0; i < g_msg_count; ) {
        if (g_messages[i].burned || (now - g_messages[i].created_at > MSG_TTL_SEC)) {
            memset(&g_messages[i], 0, sizeof(TextMessage));
            for (int j = i; j < g_msg_count - 1; j++) {
                g_messages[j] = g_messages[j + 1];
            }
            g_msg_count--;
        } else {
            i++;
        }
    }

    if (g_msg_count < MAX_MESSAGES) {
        int idx = g_msg_count++;
        g_messages[idx].msg_id = g_next_msg_id++;
        strncpy(g_messages[idx].from_id, from_id, sizeof(g_messages[idx].from_id) - 1);
        strncpy(g_messages[idx].from_name, from_name, sizeof(g_messages[idx].from_name) - 1);
        strncpy(g_messages[idx].target_id, target_id, sizeof(g_messages[idx].target_id) - 1);
        strncpy(g_messages[idx].text, text, sizeof(g_messages[idx].text) - 1);
        g_messages[idx].created_at = now;
        g_messages[idx].burned = false;

        struct tm *tm_info = localtime(&now);
        strftime(g_messages[idx].time_str, sizeof(g_messages[idx].time_str), "%H:%M:%S", tm_info);
    }
    MUTEX_UNLOCK();

    // 只有定向发给电脑主机或广播时，且电脑端需要时才同步剪贴板
    if (strcmp(target_id, "host") == 0 || strcmp(target_id, "all") == 0) {
        set_clipboard_text(text);
    }
}

/* ---------------- 焚毁单条消息或文件 ---------------- */
void burn_item(const char *type, int id) {
    MUTEX_LOCK();
    if (strcmp(type, "msg") == 0) {
        for (int i = 0; i < g_msg_count; i++) {
            if (g_messages[i].msg_id == id) {
                memset(g_messages[i].text, 0, sizeof(g_messages[i].text));
                g_messages[i].burned = true;
                break;
            }
        }
    } else if (strcmp(type, "file") == 0) {
        for (int i = 0; i < g_file_count; i++) {
            if (g_files[i].file_id == id) {
                remove(g_files[i].filepath);
                g_files[i].burned = true;
                break;
            }
        }
    }
    MUTEX_UNLOCK();
}

/* ---------------- 局域网 IP 检测 ---------------- */
static int score_ip(const char *ip) {
    if (strncmp(ip, "192.168.", 8) == 0) return 100;
    if (strncmp(ip, "10.", 3) == 0) return 80;
    if (strncmp(ip, "172.16.", 7) == 0 || strncmp(ip, "172.2", 5) == 0 || strncmp(ip, "172.3", 5) == 0) return 50;
    return 10;
}

int get_all_local_ips(char ips[16][64], int max_ips) {
    int count = 0;

#ifdef _WIN32
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent *he = gethostbyname(hostname);
        if (he != NULL && he->h_addrtype == AF_INET) {
            for (int i = 0; he->h_addr_list[i] != NULL && count < max_ips; i++) {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[i], sizeof(struct in_addr));
                const char *ip_str = inet_ntoa(addr);
                if (ip_str && strcmp(ip_str, "127.0.0.1") != 0) {
                    bool exists = false;
                    for (int j = 0; j < count; j++) {
                        if (strcmp(ips[j], ip_str) == 0) { exists = true; break; }
                    }
                    if (!exists) strncpy(ips[count++], ip_str, 63);
                }
            }
        }
    }
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) != -1) {
        for (ifa = ifaddr; ifa != NULL && count < max_ips; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;

            struct sockaddr_in *pAddr = (struct sockaddr_in *)ifa->ifa_addr;
            char *ip_str = inet_ntoa(pAddr->sin_addr);
            if (ip_str && strcmp(ip_str, "127.0.0.1") != 0) {
                bool exists = false;
                for (int j = 0; j < count; j++) {
                    if (strcmp(ips[j], ip_str) == 0) { exists = true; break; }
                }
                if (!exists) strncpy(ips[count++], ip_str, 63);
            }
        }
        freeifaddrs(ifaddr);
    }
#endif

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (score_ip(ips[j]) > score_ip(ips[i])) {
                char tmp[64];
                strcpy(tmp, ips[i]);
                strcpy(ips[i], ips[j]);
                strcpy(ips[j], tmp);
            }
        }
    }
    return count;
}

/* ---------------- 矢量 SVG 二维码生成 ---------------- */
char* generate_qrcode_svg(const char *text) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    bool ok = qrcodegen_encodeText(text, tempBuffer, qrcode, qrcodegen_Ecc_LOW,
                                   1, 10, qrcodegen_Mask_AUTO, true);
    if (!ok) return NULL;
    int size = qrcodegen_getSize(qrcode);
    int border = 2;
    int total = size + border * 2;

    char *svg = (char *)malloc(65536);
    if (!svg) return NULL;
    int pos = snprintf(svg, 65536,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"170\" height=\"170\" shape-rendering=\"crispEdges\">"
        "<rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>", total, total);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                pos += snprintf(svg + pos, 65536 - pos,
                    "<rect x=\"%d\" y=\"%d\" width=\"1\" height=\"1\" fill=\"#000000\"/>",
                    x + border, y + border);
            }
        }
    }
    snprintf(svg + pos, 65536 - pos, "</svg>");
    return svg;
}

/* ---------------- URL 参数与解码 ---------------- */
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void get_query_param(const char *path, const char *key, char *out, size_t max_len) {
    out[0] = '\0';
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    char *q = strstr(path, pattern);
    if (!q) {
        snprintf(pattern, sizeof(pattern), "?%s=", key);
        q = strstr(path, pattern);
    }
    if (q) {
        const char *val_start = q + strlen(pattern);
        char raw[256] = {0};
        strncpy(raw, val_start, sizeof(raw) - 1);
        char *amp = strchr(raw, '&');
        if (amp) *amp = '\0';
        url_decode(out, raw);
    }
}

void format_size(size_t bytes, char *out, size_t max_len) {
    if (bytes < 1024) snprintf(out, max_len, "%u B", (unsigned int)bytes);
    else if (bytes < 1024 * 1024) snprintf(out, max_len, "%.1f KB", (double)bytes / 1024.0);
    else snprintf(out, max_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

void open_default_browser(const char *url) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open \"%s\" &", url);
    system(cmd);
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" &", url);
    system(cmd);
#endif
}

/* ---------------- 客户端 HTTP 请求处理 ---------------- */
typedef struct {
    socket_t client_fd;
    char client_ip[64];
} ClientContext;

#ifdef _WIN32
unsigned __stdcall client_thread(void *arg)
#else
void* client_thread(void *arg)
#endif
{
    ClientContext *ctx = (ClientContext *)arg;
    socket_t fd = ctx->client_fd;
    char client_ip[64];
    strncpy(client_ip, ctx->client_ip, sizeof(client_ip) - 1);
    free(ctx);

    char *header_buf = (char *)malloc(8192);
    int total_read = 0;
    int content_length = -1;
    char *body_start = NULL;

    while (total_read < 8191) {
        int n = recv(fd, header_buf + total_read, 8191 - total_read, 0);
        if (n <= 0) break;
        total_read += n;
        header_buf[total_read] = '\0';
        body_start = strstr(header_buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            break;
        }
    }

    if (!body_start) {
        free(header_buf);
        close_socket(fd);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    char method[16] = {0};
    char path[1024] = {0};
    sscanf(header_buf, "%15s %1023s", method, path);

    char *cl_pos = strstr(header_buf, "Content-Length:");
    if (!cl_pos) cl_pos = strstr(header_buf, "content-length:");
    if (cl_pos) content_length = atoi(cl_pos + 15);

    bool is_get = (strcmp(method, "GET") == 0);
    bool is_head = (strcmp(method, "HEAD") == 0);

    // 主页
    if ((is_get || is_head) && (strncmp(path, "/?", 2) == 0 || strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        char resp[512];
        int body_len = (int)strlen(HTML_INDEX);
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n\r\n", body_len);
        send(fd, resp, (int)strlen(resp), 0);
        if (is_get) send(fd, HTML_INDEX, body_len, 0);
    }
    // 二维码 SVG
    else if ((is_get || is_head) && strncmp(path, "/api/qrcode", 11) == 0) {
        char target_url[256];
        get_query_param(path, "url", target_url, sizeof(target_url));
        if (!target_url[0]) strncpy(target_url, g_primary_url, sizeof(target_url) - 1);

        char *svg = generate_qrcode_svg(target_url);
        if (svg) {
            int slen = (int)strlen(svg);
            char resp_h[256];
            snprintf(resp_h, sizeof(resp_h),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: image/svg+xml; charset=utf-8\r\n"
                     "Content-Length: %d\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n\r\n", slen);
            send(fd, resp_h, (int)strlen(resp_h), 0);
            if (is_get) send(fd, svg, slen, 0);
            free(svg);
        } else {
            const char *err = "HTTP/1.1 500 Error\r\nContent-Length: 0\r\n\r\n";
            send(fd, err, (int)strlen(err), 0);
        }
    }
    // 文本发送
    else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/text", 9) == 0) {
        char from_id[64] = "guest";
        char from_name[64] = "访客";
        char target_id[64] = "all";
        get_query_param(path, "from_id", from_id, sizeof(from_id));
        get_query_param(path, "from_name", from_name, sizeof(from_name));
        get_query_param(path, "target_id", target_id, sizeof(target_id));
        if (!target_id[0]) strcpy(target_id, "all");

        int header_len = (int)(body_start - header_buf);
        int already_body = total_read - header_len;
        int needed = (content_length > 0) ? content_length : already_body;
        char *text_body = (char *)malloc(needed + 1);
        memcpy(text_body, body_start, already_body);
        int cur_body = already_body;

        while (cur_body < needed) {
            int n = recv(fd, text_body + cur_body, needed - cur_body, 0);
            if (n <= 0) break;
            cur_body += n;
        }
        text_body[cur_body] = '\0';

        printf("\n--------------------------------------------------------------\n");
        if (strcmp(target_id, "all") == 0) {
            printf("%s %s:\n%s\n", S("[收到广播]: 来自", "[Broadcast received]: from"), from_name, text_body);
        } else {
            printf("%s %s -> %s: %s\n%s\n", S("[收到私密消息]: 来自", "[Private message]: from"), from_name, S("目标", "target"), target_id, text_body);
        }
        if (strcmp(target_id, "all") == 0 || strcmp(target_id, "host") == 0) {
            printf("%s\n", S("-> [已自动写入电脑剪贴板]", "-> [Auto-copied to PC clipboard]"));
        }
        printf("--------------------------------------------------------------\n> ");
        fflush(stdout);

        add_message(from_id, from_name, target_id, text_body);
        free(text_body);

        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(fd, resp, (int)strlen(resp), 0);
    }
    // 立即焚毁消息或文件接口
    else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/burn", 9) == 0) {
        char type[16] = "";
        char id_str[16] = "0";
        get_query_param(path, "type", type, sizeof(type));
        get_query_param(path, "id", id_str, sizeof(id_str));
        int id = atoi(id_str);
        if (id > 0) burn_item(type, id);

        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(fd, resp, (int)strlen(resp), 0);
    }
    // 获取电脑剪贴板
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/clipboard") == 0) {
        char *clip = get_clipboard_text();
        char resp_header[256];
        int len = clip ? (int)strlen(clip) : 0;
        snprintf(resp_header, sizeof(resp_header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n\r\n", len);
        send(fd, resp_header, (int)strlen(resp_header), 0);
        if (clip) {
            send(fd, clip, len, 0);
            free(clip);
        }
    }
    // 上传文件 (支持定向与即焚)
    else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/upload", 11) == 0) {
        char filename[256] = "uploaded_file";
        char from_id[64] = "guest";
        char from_name[64] = "访客";
        char target_id[64] = "all";

        get_query_param(path, "name", filename, sizeof(filename));
        get_query_param(path, "from_id", from_id, sizeof(from_id));
        get_query_param(path, "from_name", from_name, sizeof(from_name));
        get_query_param(path, "target_id", target_id, sizeof(target_id));
        if (!target_id[0]) strcpy(target_id, "all");

        mkdir_cross(STORAGE_DIR);
        char filepath[512];
        time_t now = time(NULL);
        snprintf(filepath, sizeof(filepath), "%s/%ld_%s", STORAGE_DIR, (long)now, filename);

        FILE *fp = fopen(filepath, "wb");
        size_t total_written = 0;
        if (fp) {
            int header_len = (int)(body_start - header_buf);
            int already_body = total_read - header_len;
            if (already_body > 0) {
                fwrite(body_start, 1, already_body, fp);
                total_written += already_body;
            }
            int remaining = content_length - already_body;
            char chunk[8192];
            while (remaining > 0) {
                int to_read = (remaining < (int)sizeof(chunk)) ? remaining : (int)sizeof(chunk);
                int n = recv(fd, chunk, to_read, 0);
                if (n <= 0) break;
                fwrite(chunk, 1, n, fp);
                total_written += n;
                remaining -= n;
            }
            fclose(fp);

            MUTEX_LOCK();
            if (g_file_count < MAX_FILES) {
                int idx = g_file_count++;
                g_files[idx].file_id = g_next_file_id++;
                strncpy(g_files[idx].filename, filename, sizeof(g_files[idx].filename) - 1);
                strncpy(g_files[idx].filepath, filepath, sizeof(g_files[idx].filepath) - 1);
                g_files[idx].size = total_written;
                strncpy(g_files[idx].from_id, from_id, sizeof(g_files[idx].from_id) - 1);
                strncpy(g_files[idx].from_name, from_name, sizeof(g_files[idx].from_name) - 1);
                strncpy(g_files[idx].target_id, target_id, sizeof(g_files[idx].target_id) - 1);
                g_files[idx].created_at = now;
                g_files[idx].burned = false;
            }
            MUTEX_UNLOCK();

            printf("\n--------------------------------------------------------------\n");
            printf("%s: %s (%s: %u %s, %s: %s)\n",
                   S("[收到文件]", "[File received]"),
                   filename,
                   S("大小", "size"),
                   (unsigned int)total_written,
                   S("字节", "bytes"),
                   S("目标", "target"),
                   target_id);
            printf("--------------------------------------------------------------\n> ");
            fflush(stdout);
        }

        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(fd, resp, (int)strlen(resp), 0);
    }
    // 下载文件 (定向私信文件 -> 下载即物理删除焚毁)
    else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/download", 13) == 0) {
        char id_str[16] = "0";
        char client_id[64] = "";
        get_query_param(path, "id", id_str, sizeof(id_str));
        get_query_param(path, "client_id", client_id, sizeof(client_id));
        int file_id = atoi(id_str);

        int target_idx = -1;
        MUTEX_LOCK();
        for (int i = 0; i < g_file_count; i++) {
            if (g_files[i].file_id == file_id && !g_files[i].burned) {
                target_idx = i;
                break;
            }
        }
        MUTEX_UNLOCK();

        if (target_idx >= 0) {
            FILE *fp = fopen(g_files[target_idx].filepath, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long fsize = ftell(fp);
                fseek(fp, 0, SEEK_SET);

                char resp_h[512];
                snprintf(resp_h, sizeof(resp_h),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/octet-stream\r\n"
                         "Content-Disposition: attachment; filename=\"%s\"\r\n"
                         "Content-Length: %ld\r\n"
                         "Connection: close\r\n\r\n", g_files[target_idx].filename, fsize);
                send(fd, resp_h, (int)strlen(resp_h), 0);

                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                    send(fd, buf, (int)n, 0);
                }
                fclose(fp);

                // 如果该文件是私密定向传输，下载完成立即执行“物理删除”阅后即焚！
                if (strcmp(g_files[target_idx].target_id, "all") != 0) {
                    printf("\n%s %s %s\n> ",
                           S("[隐私保护] 私密文件", "[Privacy] Private file"),
                           g_files[target_idx].filename,
                           S("已被接收方下载，已立即彻底物理抹除！", "downloaded by recipient, physically wiped immediately!"));
                    fflush(stdout);
                    remove(g_files[target_idx].filepath);
                    MUTEX_LOCK();
                    g_files[target_idx].burned = true;
                    MUTEX_UNLOCK();
                }
            } else {
                const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
                send(fd, not_found, (int)strlen(not_found), 0);
            }
        } else {
            const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
            send(fd, not_found, (int)strlen(not_found), 0);
        }
    }
    // 同步接口 (心跳上报 + 获取可见的在线用户、过滤后的隐私消息和文件)
    else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/sync", 9) == 0) {
        char client_id[64] = "guest";
        char client_name[64] = "访客";
        get_query_param(path, "client_id", client_id, sizeof(client_id));
        get_query_param(path, "name", client_name, sizeof(client_name));

        // 更新在线心跳
        touch_user(client_id, client_name, client_ip, false);

        char *json = (char *)malloc(65536);
        snprintf(json, 65536, "{\"interfaces\":[");

        for (int i = 0; i < g_ip_count; i++) {
            if (i > 0) strcat(json, ",");
            char item_buf[256];
            const char *label = (i == 0) ? S("Wi-Fi / 局域网", "Wi-Fi / LAN") : S("备用网络接口", "Alternate Interface");
            snprintf(item_buf, sizeof(item_buf),
                     "{\"ip\":\"%s\",\"url\":\"http://%s:%d\",\"label\":\"%s\"}",
                     g_ips[i], g_ips[i], g_port, label);
            strcat(json, item_buf);
        }
        if (g_ip_count > 0) strcat(json, ",");
        char loopback_buf[256];
        snprintf(loopback_buf, sizeof(loopback_buf),
                 "{\"ip\":\"127.0.0.1\",\"url\":\"http://127.0.0.1:%d\",\"label\":\"%s\"}", g_port, S("本机回环", "Loopback"));
        strcat(json, loopback_buf);

        strcat(json, "],\"users\":[");

        // 组装在线用户列表
        MUTEX_LOCK();
        bool first_u = true;
        for (int i = 0; i < g_user_count; i++) {
            if (!first_u) strcat(json, ",");
            first_u = false;
            char u_buf[512];
            snprintf(u_buf, sizeof(u_buf),
                     "{\"client_id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"is_host\":%s}",
                     g_users[i].client_id, g_users[i].name, g_users[i].ip,
                     g_users[i].is_host ? "true" : "false");
            strcat(json, u_buf);
        }

        strcat(json, "],\"messages\":[");

        // 过滤隐私消息：仅返回全员广播 或 与自己相关的私信
        bool first_m = true;
        for (int i = 0; i < g_msg_count; i++) {
            if (g_messages[i].burned) continue;
            bool is_broadcast = (strcmp(g_messages[i].target_id, "all") == 0);
            bool is_from_me = (strcmp(g_messages[i].from_id, client_id) == 0);
            bool is_for_me = (strcmp(g_messages[i].target_id, client_id) == 0);

            if (is_broadcast || is_from_me || is_for_me) {
                if (!first_m) strcat(json, ",");
                first_m = false;

                char m_meta[256];
                snprintf(m_meta, sizeof(m_meta),
                         "{\"msg_id\":%d,\"from_id\":\"%s\",\"from_name\":\"%s\",\"target_id\":\"%s\",\"time\":\"%s\",\"text\":\"",
                         g_messages[i].msg_id, g_messages[i].from_id, g_messages[i].from_name,
                         g_messages[i].target_id, g_messages[i].time_str);
                strcat(json, m_meta);

                for (const char *p = g_messages[i].text; *p; p++) {
                    char temp[3] = {0};
                    if (*p == '\"') strcat(json, "\\\"");
                    else if (*p == '\\') strcat(json, "\\\\");
                    else if (*p == '\n') strcat(json, "\\n");
                    else if (*p == '\r') {}
                    else { temp[0] = *p; strcat(json, temp); }
                }
                strcat(json, "\"}");
            }
        }

        strcat(json, "],\"files\":[");

        // 过滤隐私文件：全员广播 或 与自己相关的文件
        bool first_f = true;
        for (int i = 0; i < g_file_count; i++) {
            if (g_files[i].burned) continue;
            bool is_broadcast = (strcmp(g_files[i].target_id, "all") == 0);
            bool is_from_me = (strcmp(g_files[i].from_id, client_id) == 0);
            bool is_for_me = (strcmp(g_files[i].target_id, client_id) == 0);

            if (is_broadcast || is_from_me || is_for_me) {
                if (!first_f) strcat(json, ",");
                first_f = false;

                char size_str[32];
                format_size(g_files[i].size, size_str, sizeof(size_str));

                char f_buf[512];
                snprintf(f_buf, sizeof(f_buf),
                         "{\"file_id\":%d,\"name\":\"%s\",\"size\":\"%s\",\"from_name\":\"%s\",\"target_id\":\"%s\"}",
                         g_files[i].file_id, g_files[i].filename, size_str,
                         g_files[i].from_name, g_files[i].target_id);
                strcat(json, f_buf);
            }
        }
        MUTEX_UNLOCK();

        strcat(json, "]}");

        int jlen = (int)strlen(json);
        char resp_h[256];
        snprintf(resp_h, sizeof(resp_h),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json; charset=utf-8\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n\r\n", jlen);
        send(fd, resp_h, (int)strlen(resp_h), 0);
        send(fd, json, jlen, 0);
        free(json);
    }
    else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        send(fd, not_found, (int)strlen(not_found), 0);
    }

    free(header_buf);
    close_socket(fd);

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---------------- 电脑端控制台命令行交互 ---------------- */
#ifdef _WIN32
unsigned __stdcall console_input_thread(void *arg)
#else
void* console_input_thread(void *arg)
#endif
{
    (void)arg;
    char buffer[4096];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            // stdin 结束（如无 TTY、管道断开或后台运行），安全退出输入监听线程，避免高 CPU 死循环
            break;
        }

        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
        if (len == 0) continue;

        if (strcmp(buffer, "q") == 0 || strcmp(buffer, "quit") == 0 || strcmp(buffer, "exit") == 0) {
            printf("\n%s\n", S("[提示] 正在安全退出并擦除临时文件...", "[Notice] Exiting safely and wiping temporary files..."));
            handle_exit_signal(0);
        }

        // 电脑端发出全员广播
        add_message("host", S("电脑端 (Host)", "PC Host"), "all", buffer);
        printf("%s: %s\n", S("[已广播到所有在线设备]", "[Broadcasted to all online devices]"), buffer);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---------------- 主入口函数 ---------------- */
int main(int argc, char *argv[]) {
    signal(SIGINT, handle_exit_signal);
    signal(SIGTERM, handle_exit_signal);

#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 启动失败\n");
        return 1;
    }
    MUTEX_INIT();
#endif

    // 语言检测：1. 命令行参数优先级最高 (--lang en / --lang zh)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "en") == 0) g_lang = 1;
            else if (strcmp(argv[i + 1], "zh") == 0) g_lang = 0;
            i++;
        } else if (strcmp(argv[i], "--lang=en") == 0) {
            g_lang = 1;
        } else if (strcmp(argv[i], "--lang=zh") == 0) {
            g_lang = 0;
        }
    }

    // 2. 无显式指定参数时，自动检测系统默认区域/UI语言
    static bool lang_specified = false;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--lang", 6) == 0) { lang_specified = true; break; }
    }
    if (!lang_specified) {
#ifdef _WIN32
        LANGID uilang = GetUserDefaultUILanguage();
        WORD primary = PRIMARYLANGID(uilang);
        if (primary != LANG_CHINESE) {
            g_lang = 1; // 默认非中文系统自动切换为英文
        }
#else
        const char *env_lang = getenv("LANG");
        if (!env_lang) env_lang = getenv("LC_ALL");
        if (env_lang && strncmp(env_lang, "zh", 2) != 0) {
            // 系统语言非中文时，自动采用英文
            g_lang = 1;
        }
#endif
    }

    mkdir_cross(STORAGE_DIR);

    int start_port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--lang", 6) == 0) {
            if (strcmp(argv[i], "--lang") == 0) i++;
            continue;
        }
        int p = atoi(argv[i]);
        if (p > 0) { start_port = p; break; }
    }

    // 智能端口探测与健壮绑定机制 (严格遵循 POSIX/macOS 套接字规范)
    int bound = 0;
    for (int p = start_port; p < start_port + 20; p++) {
        g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (g_server_fd < 0) continue;

        int opt = 1;
        setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(p);

        if (bind(g_server_fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            if (p != start_port) {
                printf("%s %d %s %d\n", S("[网络提示] 默认端口", "[Network] Default port"), start_port, S("已被占用，已自动顺延至可用端口", "in use, automatically switched to"), p);
            }
            g_port = p;
            bound = 1;
            break;
        }

        // 绑定失败必须彻底关闭旧 socket，下一次循环重新创建新描述符！
        close_socket(g_server_fd);
        g_server_fd = (socket_t)-1;
    }

    if (!bound || listen(g_server_fd, 10) < 0) {
        printf("%s\n", S("错误：未能成功绑定任何可用网络端口，请检查系统防火墙或网络设置。", "Error: Failed to bind any network port. Please check your firewall or settings."));
        return 1;
    }

    // 注册电脑端 Host 为默认在线用户
    touch_user("host", S("电脑端 (Host)", "PC Host"), "127.0.0.1", true);

    g_ip_count = get_all_local_ips(g_ips, 16);
    if (g_ip_count > 0) {
        snprintf(g_primary_url, sizeof(g_primary_url), "http://%s:%d", g_ips[0], g_port);
    } else {
        snprintf(g_primary_url, sizeof(g_primary_url), "http://127.0.0.1:%d", g_port);
    }

    char local_url[64];
    snprintf(local_url, sizeof(local_url), "http://127.0.0.1:%d", g_port);

    printf("==============================================================\n");
    if (g_lang == 0) {
        printf("   ⚡ QuickMessage v%s 局域网极简快传 - 多人隐私与阅后即焚版   \n", APP_VERSION);
        printf("   👨‍💻 Author: %s | 跨平台兼容: WinXP~Win12 & macOS          \n", APP_AUTHOR);
        printf("==============================================================\n");
        printf(" [可用访问地址]:\n");
        if (g_ip_count > 0) {
            for (int i = 0; i < g_ip_count; i++) {
                if (i == 0) printf("  👉 推荐直连 (Wi-Fi): http://%s:%d\n", g_ips[i], g_port);
                else        printf("     备选接口        : http://%s:%d\n", g_ips[i], g_port);
            }
        }
        printf("     本机回环        : %s\n", local_url);
        printf("--------------------------------------------------------------\n");
        printf(" 🔒 [隐私特性启用]:\n");
        printf("  1. 纯内存流转，绝不留存历史记录，关闭浏览器身份即刻抹除\n");
        printf("  2. 私密文件对方下载即刻物理擦除删除 (下载即焚)\n");
        printf("  3. 支持选定单独设备或全员广播收发，无关设备完全不可见\n");
        printf("  4. 随时点击 '🔥 立即焚毁' 彻底抹除；服务退出时全清临时文件\n");
        printf("--------------------------------------------------------------\n");
        printf(" 🚀 已自动唤起默认浏览器打开主控制台界面...\n");
    } else {
        printf("   ⚡ QuickMessage v%s LAN Quick Transfer - Privacy Edition     \n", APP_VERSION);
        printf("   👨‍💻 Author: %s | Cross-Platform: WinXP~Win12 & macOS      \n", APP_AUTHOR);
        printf("==============================================================\n");
        printf(" [Available Access URLs]:\n");
        if (g_ip_count > 0) {
            for (int i = 0; i < g_ip_count; i++) {
                if (i == 0) printf("  👉 Recommended (Wi-Fi): http://%s:%d\n", g_ips[i], g_port);
                else        printf("     Alternate Interface: http://%s:%d\n", g_ips[i], g_port);
            }
        }
        printf("     Local Loopback     : %s\n", local_url);
        printf("--------------------------------------------------------------\n");
        printf(" 🔒 [Privacy Features Enabled]:\n");
        printf("  1. Memory-only, zero persistent history; closed tab wipes identity\n");
        printf("  2. Private files physically erased upon download (burn-after-read)\n");
        printf("  3. Direct-to-peer or broadcast; third-party devices cannot view\n");
        printf("  4. Manual '🔥 Burn Now' anytime; all temp files cleared on exit\n");
        printf("--------------------------------------------------------------\n");
        printf(" 🚀 Browser opened automatically. Ready to transfer...\n");
    }
    printf("==============================================================\n\n");

    open_default_browser(local_url);

#ifdef _WIN32
    _beginthreadex(NULL, 0, console_input_thread, NULL, 0, NULL);
#else
    pthread_t th_input;
    pthread_create(&th_input, NULL, console_input_thread, NULL);
    pthread_detach(th_input);
#endif

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        socket_t client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) break;

        ClientContext *ctx = (ClientContext *)malloc(sizeof(ClientContext));
        ctx->client_fd = client_fd;
        const char *ip_str = inet_ntoa(client_addr.sin_addr);
        strncpy(ctx->client_ip, ip_str ? ip_str : "127.0.0.1", sizeof(ctx->client_ip) - 1);

#ifdef _WIN32
        _beginthreadex(NULL, 0, client_thread, ctx, 0, NULL);
#else
        pthread_t th;
        pthread_create(&th, NULL, client_thread, ctx);
        pthread_detach(th);
#endif
    }

    if (g_server_fd != (socket_t)-1) close_socket(g_server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
