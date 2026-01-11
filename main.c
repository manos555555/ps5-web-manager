/* PS5 Web-Based File Manager + System Monitor
 * By Manos
 * HTTP Server with REST API for file operations and system monitoring
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>

#define HTTP_PORT 8080
#define BUFFER_SIZE (64 * 1024)
#define MAX_PATH 2048

// Server statistics (global)
static unsigned long total_requests = 0;
static unsigned long total_files_transferred = 0;
static unsigned long long total_bytes_transferred = 0;
static int active_connections = 0;

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

void send_notification(const char *msg) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

typedef struct {
    int client_sock;
    struct sockaddr_in client_addr;
} client_info_t;

// Forward declarations
char* get_query_param(const char *query, const char *param_name);

// URL decode helper
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && 
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

// Send HTTP response
void send_http_response(int sock, int code, const char *content_type, const char *body, size_t body_len) {
    char header[1024];
    const char *status;
    
    switch(code) {
        case 200: status = "OK"; break;
        case 404: status = "Not Found"; break;
        case 500: status = "Internal Server Error"; break;
        default: status = "Unknown"; break;
    }
    
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status, content_type, body_len);
    
    send(sock, header, header_len, 0);
    if (body && body_len > 0) {
        send(sock, body, body_len, 0);
    }
}

// Get file list as JSON
void handle_list_files(int sock, const char *path) {
    char decoded_path[MAX_PATH];
    url_decode(decoded_path, path);
    
    DIR *dir = opendir(decoded_path);
    if (!dir) {
        const char *error_msg = "{\"error\":\"Directory not found\"}";
        send_http_response(sock, 404, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    char *json = malloc(1024 * 1024);
    if (!json) {
        closedir(dir);
        const char *error_msg = "{\"error\":\"Memory error\"}";
        send_http_response(sock, 500, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    int pos = sprintf(json, "{\"path\":\"%s\",\"files\":[", decoded_path);
    
    struct dirent *entry;
    int first = 1;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0) continue;
        
        char fullpath[MAX_PATH];
        snprintf(fullpath, MAX_PATH, "%s/%s", decoded_path, entry->d_name);
        
        struct stat st;
        if (stat(fullpath, &st) == 0) {
            if (!first) pos += sprintf(json + pos, ",");
            first = 0;
            
            pos += sprintf(json + pos, 
                "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lld,\"mtime\":%ld}",
                entry->d_name,
                S_ISDIR(st.st_mode) ? "dir" : "file",
                (long long)st.st_size,
                (long)st.st_mtime);
        }
    }
    
    pos += sprintf(json + pos, "]}");
    closedir(dir);
    
    send_http_response(sock, 200, "application/json", json, pos);
    free(json);
}

// Download file
void handle_download_file(int sock, const char *path) {
    char decoded_path[MAX_PATH];
    url_decode(decoded_path, path);
    
    int fd = open(decoded_path, O_RDONLY);
    if (fd < 0) {
        send_http_response(sock, 404, "text/plain", "File not found", 14);
        return;
    }
    
    struct stat st;
    fstat(fd, &st);
    
    char header[1024];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lld\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        (long long)st.st_size,
        strrchr(decoded_path, '/') ? strrchr(decoded_path, '/') + 1 : decoded_path);
    
    send(sock, header, header_len, 0);
    
    unsigned long long bytes_sent = 0;
    char *buffer = malloc(BUFFER_SIZE);
    if (buffer) {
        ssize_t n;
        while ((n = read(fd, buffer, BUFFER_SIZE)) > 0) {
            send(sock, buffer, n, 0);
            bytes_sent += n;
        }
        free(buffer);
    }
    
    close(fd);
    
    total_files_transferred++;
    total_bytes_transferred += bytes_sent;
}

// Delete file/directory
void handle_delete(int sock, const char *path) {
    char decoded_path[MAX_PATH];
    url_decode(decoded_path, path);
    
    struct stat st;
    if (stat(decoded_path, &st) != 0) {
        send_http_response(sock, 404, "application/json", "{\"error\":\"Not found\"}", 21);
        return;
    }
    
    int result;
    if (S_ISDIR(st.st_mode)) {
        result = rmdir(decoded_path);
    } else {
        result = unlink(decoded_path);
    }
    
    const char *success_msg = "{\"success\":true}";
    const char *error_msg = "{\"error\":\"Delete failed\"}";
    if (result == 0) {
        send_http_response(sock, 200, "application/json", success_msg, strlen(success_msg));
    } else {
        send_http_response(sock, 500, "application/json", error_msg, strlen(error_msg));
    }
}

// Get system info as JSON
void handle_system_info(int sock) {
    char json[8192];
    
    // Storage - /data partition
    struct statvfs vfs_data;
    statvfs("/data", &vfs_data);
    unsigned long long data_total = vfs_data.f_blocks * vfs_data.f_frsize;
    unsigned long long data_free = vfs_data.f_bfree * vfs_data.f_frsize;
    unsigned long long data_used = data_total - data_free;
    
    // Storage - /system partition
    struct statvfs vfs_system;
    unsigned long long sys_total = 0, sys_free = 0, sys_used = 0;
    if (statvfs("/system", &vfs_system) == 0) {
        sys_total = vfs_system.f_blocks * vfs_system.f_frsize;
        sys_free = vfs_system.f_bfree * vfs_system.f_frsize;
        sys_used = sys_total - sys_free;
    }
    
    // RAM info (estimated - PS5 has 16GB total, 13.5GB available to apps)
    unsigned long long ram_total = 16ULL * 1024 * 1024 * 1024;  // 16 GB
    unsigned long long ram_available = 13ULL * 1024 * 1024 * 1024;  // ~13 GB available
    // Estimate usage based on uptime (rough approximation)
    unsigned long long ram_used = ram_available * 0.6;  // Assume ~60% usage
    unsigned long long ram_free = ram_total - ram_used;
    
    // Uptime
    static time_t start_time = 0;
    if (start_time == 0) {
        start_time = time(NULL);
    }
    time_t uptime_seconds = time(NULL) - start_time;
    int days = uptime_seconds / 86400;
    int hours = (uptime_seconds % 86400) / 3600;
    int minutes = (uptime_seconds % 3600) / 60;
    int seconds = uptime_seconds % 60;
    
    // Network info - Get IP address
    char hostname[256] = "PS5";
    char ip_address[INET_ADDRSTRLEN] = "192.168.0.160";
    
    gethostname(hostname, sizeof(hostname));
    
    // Try to get actual IP address
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &addr->sin_addr, ip_address, INET_ADDRSTRLEN);
                if (strcmp(ip_address, "127.0.0.1") != 0) {
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
    }
    
    // Build JSON
    int pos = sprintf(json, "{");
    
    // Storage info
    pos += sprintf(json + pos, "\"storage\":{");
    pos += sprintf(json + pos, "\"data\":{\"total\":%llu,\"used\":%llu,\"free\":%llu},", 
                   data_total, data_used, data_free);
    if (sys_total > 0) {
        pos += sprintf(json + pos, "\"system\":{\"total\":%llu,\"used\":%llu,\"free\":%llu}", 
                       sys_total, sys_used, sys_free);
    }
    pos += sprintf(json + pos, "},");
    
    // RAM info
    pos += sprintf(json + pos, "\"ram\":{\"total\":%llu,\"used\":%llu,\"free\":%llu},", 
                   ram_total, ram_used, ram_free);
    
    // Uptime
    pos += sprintf(json + pos, "\"uptime\":{\"seconds\":%ld,\"days\":%d,\"hours\":%d,\"minutes\":%d,\"secs\":%d,\"start_time\":%ld},", 
                   (long)uptime_seconds, days, hours, minutes, seconds, (long)start_time);
    
    // Network info
    pos += sprintf(json + pos, "\"network\":{\"hostname\":\"%s\",\"ip\":\"%s\"},", hostname, ip_address);
    
    // Server statistics
    pos += sprintf(json + pos, "\"server\":{\"total_requests\":%lu,\"files_transferred\":%lu,\"bytes_transferred\":%llu,\"active_connections\":%d}", 
                   total_requests, total_files_transferred, total_bytes_transferred, active_connections);
    
    pos += sprintf(json + pos, "}");
    
    send_http_response(sock, 200, "application/json", json, pos);
}

// Handle rename
void handle_rename(int sock, const char *old_path, const char *new_path) {
    char decoded_old[MAX_PATH], decoded_new[MAX_PATH];
    url_decode(decoded_old, old_path);
    url_decode(decoded_new, new_path);
    
    const char *success_msg = "{\"success\":true}";
    const char *error_msg = "{\"error\":\"Rename failed\"}";
    if (rename(decoded_old, decoded_new) == 0) {
        send_http_response(sock, 200, "application/json", success_msg, strlen(success_msg));
    } else {
        send_http_response(sock, 500, "application/json", error_msg, strlen(error_msg));
    }
}

// Handle copy
void handle_copy(int sock, const char *src_path, const char *dst_path) {
    char decoded_src[MAX_PATH], decoded_dst[MAX_PATH];
    url_decode(decoded_src, src_path);
    url_decode(decoded_dst, dst_path);
    
    struct stat src_stat;
    if (stat(decoded_src, &src_stat) != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "{\"error\":\"Source not found: %s\"}", decoded_src);
        send_http_response(sock, 404, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    int src_fd = open(decoded_src, O_RDONLY);
    if (src_fd < 0) {
        const char *error_msg = "{\"error\":\"Cannot open source\"}";
        send_http_response(sock, 404, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    int dst_fd = open(decoded_dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "{\"error\":\"Cannot create: %s\"}", decoded_dst);
        send_http_response(sock, 500, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    char *buffer = malloc(BUFFER_SIZE);
    if (buffer) {
        ssize_t n;
        while ((n = read(src_fd, buffer, BUFFER_SIZE)) > 0) {
            write(dst_fd, buffer, n);
        }
        free(buffer);
    }
    
    close(src_fd);
    close(dst_fd);
    
    const char *success_msg = "{\"success\":true}";
    send_http_response(sock, 200, "application/json", success_msg, strlen(success_msg));
}

// Serve web interface
void serve_web_interface(int sock) {
    const char *html = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<meta charset='UTF-8'>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"<title>PS5 Web Manager - By Manos</title>\n"
"<style>\n"
"* { margin: 0; padding: 0; box-sizing: border-box; }\n"
"body { font-family: Arial, sans-serif; background: #1a1a1a; color: #fff; }\n"
"header { background: #2563eb; padding: 20px; text-align: center; }\n"
"h1 { font-size: 24px; }\n"
".container { max-width: 1200px; margin: 20px auto; padding: 0 20px; }\n"
".tabs { display: flex; gap: 10px; margin-bottom: 20px; }\n"
".tab { padding: 10px 20px; background: #333; border: none; color: #fff; cursor: pointer; border-radius: 5px; }\n"
".tab.active { background: #2563eb; }\n"
".panel { display: none; background: #2a2a2a; padding: 20px; border-radius: 10px; }\n"
".panel.active { display: block; }\n"
".path-bar { display: flex; gap: 10px; margin-bottom: 20px; align-items: center; }\n"
".path-bar input { flex: 1; padding: 10px; background: #333; border: 1px solid #555; color: #fff; border-radius: 5px; }\n"
".path-bar button { padding: 10px 20px; background: #2563eb; border: none; color: #fff; cursor: pointer; border-radius: 5px; }\n"
".file-list { background: #333; border-radius: 5px; overflow: hidden; }\n"
".file-item { display: flex; justify-content: space-between; padding: 15px; border-bottom: 1px solid #444; cursor: pointer; }\n"
".file-item:hover { background: #3a3a3a; }\n"
".file-info { display: flex; gap: 20px; align-items: center; }\n"
".file-icon { font-size: 24px; }\n"
".file-actions { display: flex; gap: 5px; }\n"
".file-actions button { padding: 5px 10px; border: none; color: #fff; cursor: pointer; border-radius: 3px; font-size: 12px; }\n"
".rename-btn { background: #2563eb; }\n"
".copy-btn { background: #16a34a; }\n"
".move-btn { background: #f59e0b; }\n"
".delete-btn { background: #dc2626; }\n"
".stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; }\n"
".stat-card { background: #333; padding: 20px; border-radius: 10px; }\n"
".stat-card h3 { margin-bottom: 10px; color: #2563eb; }\n"
".stat-value { font-size: 32px; font-weight: bold; }\n"
".loading { text-align: center; padding: 40px; }\n"
".modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); z-index: 1000; }\n"
".modal.active { display: flex; align-items: center; justify-content: center; }\n"
".modal-content { background: #2a2a2a; padding: 30px; border-radius: 10px; max-width: 600px; width: 90%; max-height: 80vh; overflow-y: auto; }\n"
".modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }\n"
".modal-header h2 { margin: 0; }\n"
".modal-close { background: #dc2626; border: none; color: #fff; padding: 5px 15px; cursor: pointer; border-radius: 5px; }\n"
".modal-path { padding: 10px; background: #333; border: 1px solid #555; color: #fff; border-radius: 5px; margin-bottom: 15px; }\n"
".modal-actions { display: flex; gap: 10px; margin-top: 20px; }\n"
".modal-actions button { flex: 1; padding: 10px; border: none; color: #fff; cursor: pointer; border-radius: 5px; }\n"
".btn-select { background: #2563eb; }\n"
".btn-cancel { background: #666; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"<h1>🌐 PS5 Web Manager - By Manos</h1>\n"
"</header>\n"
"<div class='container'>\n"
"<div class='tabs'>\n"
"<button class='tab active' onclick='showTab(0)'>📁 File Manager</button>\n"
"<button class='tab' onclick='showTab(1)'>📊 System Monitor</button>\n"
"</div>\n"
"<div class='panel active' id='panel0'>\n"
"<div class='path-bar'>\n"
"<input type='text' id='currentPath' value='/data' />\n"
"<button onclick='loadFiles()'>Go</button>\n"
"<button onclick='goUp()'>⬆️ Up</button>\n"
"<input type='file' id='fileUpload' style='display:none' onchange='uploadFile()' />\n"
"<button onclick='document.getElementById(\"fileUpload\").click()' style='background:#16a34a;'>📤 Upload File</button>\n"
"</div>\n"
"<div id='uploadProgress' style='display:none;background:#333;padding:10px;border-radius:5px;margin-bottom:10px;'>\n"
"<div style='margin-bottom:5px;'>Uploading: <span id='uploadFilename'></span></div>\n"
"<div style='background:#555;height:20px;border-radius:10px;overflow:hidden;'>\n"
"<div id='uploadBar' style='background:#16a34a;height:100%;width:0%;transition:width 0.3s;'></div>\n"
"</div>\n"
"<div style='margin-top:5px;font-size:12px;'><span id='uploadStatus'>Preparing...</span></div>\n"
"</div>\n"
"<div id='fileList' class='loading'>Loading...</div>\n"
"</div>\n"
"<div class='panel' id='panel1'>\n"
"<div class='stats' id='systemStats'>\n"
"<div class='loading'>Loading system info...</div>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<div class='modal' id='copyModal'>\n"
"<div class='modal-content'>\n"
"<div class='modal-header'>\n"
"<h2 id='modalTitle'>Select Destination Folder</h2>\n"
"<button class='modal-close' onclick='closeModal()'>✕</button>\n"
"</div>\n"
"<div class='modal-path' id='modalPath'>/data</div>\n"
"<button onclick='modalGoUp()' style='width:100%; padding:10px; background:#2563eb; border:none; color:#fff; cursor:pointer; border-radius:5px; margin-bottom:10px;'>⬆️ Up</button>\n"
"<div id='modalFileList' class='file-list'></div>\n"
"<div class='modal-actions'>\n"
"<button class='btn-select' onclick='selectDestination()'>Select This Folder</button>\n"
"<button class='btn-cancel' onclick='closeModal()'>Cancel</button>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<script>\n"
"let currentPath = '/data';\n"
"function showTab(n) {\n"
"  document.querySelectorAll('.tab').forEach((t,i) => t.classList.toggle('active', i===n));\n"
"  document.querySelectorAll('.panel').forEach((p,i) => p.classList.toggle('active', i===n));\n"
"  if(n===0) loadFiles();\n"
"  if(n===1) loadSystemInfo();\n"
"}\n"
"function loadFiles() {\n"
"  currentPath = document.getElementById('currentPath').value;\n"
"  console.log('Loading files from:', currentPath);\n"
"  let url = '/api/list?path=' + encodeURIComponent(currentPath);\n"
"  console.log('Fetching:', url);\n"
"  fetch(url)\n"
"    .then(r => {\n"
"      console.log('Response status:', r.status);\n"
"      if (!r.ok) throw new Error('HTTP ' + r.status);\n"
"      return r.json();\n"
"    })\n"
"    .then(data => {\n"
"      console.log('Data received:', data);\n"
"      if (data.error) {\n"
"        document.getElementById('fileList').innerHTML = '<div class=\"loading\">Error: ' + data.error + '</div>';\n"
"        return;\n"
"      }\n"
"      let html = '<div class=\"file-list\">';\n"
"      if (!data.files || data.files.length === 0) {\n"
"        html += '<div class=\"loading\">Empty directory</div>';\n"
"      } else {\n"
"        data.files.forEach((f, idx) => {\n"
"          let icon = f.type === 'dir' ? '📁' : '📄';\n"
"          let size = f.type === 'dir' ? '' : formatSize(f.size);\n"
"          html += '<div class=\"file-item\">';\n"
"          html += '<div class=\"file-info\" data-name=\"' + f.name + '\" data-type=\"' + f.type + '\" data-idx=\"' + idx + '\">';\n"
"          html += '<span class=\"file-icon\">' + icon + '</span>';\n"
"          html += '<span>' + f.name + '</span>';\n"
"          html += '<span>' + size + '</span>';\n"
"          html += '</div>';\n"
"          html += '<div class=\"file-actions\">';\n"
"          if (f.type === 'file') {\n"
"            html += '<button class=\"download-btn\" data-name=\"' + f.name + '\" style=\"background:#2563eb;\">⬇️ Download</button>';\n"
"          }\n"
"          html += '<button class=\"rename-btn\" data-name=\"' + f.name + '\">Rename</button>';\n"
"          html += '<button class=\"copy-btn\" data-name=\"' + f.name + '\">Copy</button>';\n"
"          html += '<button class=\"move-btn\" data-name=\"' + f.name + '\">Move</button>';\n"
"          html += '<button class=\"delete-btn\" data-name=\"' + f.name + '\">Delete</button>';\n"
"          html += '</div></div>';\n"
"        });\n"
"      }\n"
"      html += '</div>';\n"
"      document.getElementById('fileList').innerHTML = html;\n"
"      document.querySelectorAll('.file-info').forEach(el => {\n"
"        el.addEventListener('click', () => {\n"
"          let name = el.getAttribute('data-name');\n"
"          let type = el.getAttribute('data-type');\n"
"          if (type === 'dir') openDir(name); else downloadFile(name);\n"
"        });\n"
"      });\n"
"      document.querySelectorAll('.download-btn').forEach(el => {\n"
"        el.addEventListener('click', () => downloadFile(el.getAttribute('data-name')));\n"
"      });\n"
"      document.querySelectorAll('.rename-btn').forEach(el => {\n"
"        el.addEventListener('click', () => renameFile(el.getAttribute('data-name')));\n"
"      });\n"
"      document.querySelectorAll('.copy-btn').forEach(el => {\n"
"        el.addEventListener('click', () => copyFile(el.getAttribute('data-name')));\n"
"      });\n"
"      document.querySelectorAll('.move-btn').forEach(el => {\n"
"        el.addEventListener('click', () => moveFile(el.getAttribute('data-name')));\n"
"      });\n"
"      document.querySelectorAll('.delete-btn').forEach(el => {\n"
"        el.addEventListener('click', () => deleteFile(el.getAttribute('data-name')));\n"
"      });\n"
"    })\n"
"    .catch(e => {\n"
"      console.error('Error:', e);\n"
"      document.getElementById('fileList').innerHTML = '<div class=\"loading\">Error: ' + e.message + '<br>Check browser console (F12) for details</div>';\n"
"    });\n"
"}\n"
"function normalizePath(path) {\n"
"  path = path.replace(/\\/+/g, '/');\n"
"  let parts = path.split('/').filter(p => p && p !== '.');\n"
"  let result = [];\n"
"  for (let part of parts) {\n"
"    if (part === '..') {\n"
"      if (result.length > 0) result.pop();\n"
"    } else {\n"
"      result.push(part);\n"
"    }\n"
"  }\n"
"  return '/' + result.join('/');\n"
"}\n"
"function openDir(name) {\n"
"  currentPath = normalizePath(currentPath + '/' + name);\n"
"  document.getElementById('currentPath').value = currentPath;\n"
"  loadFiles();\n"
"}\n"
"function goUp() {\n"
"  let parts = currentPath.split('/').filter(p => p);\n"
"  parts.pop();\n"
"  currentPath = '/' + parts.join('/');\n"
"  document.getElementById('currentPath').value = currentPath;\n"
"  loadFiles();\n"
"}\n"
"function downloadFile(name) {\n"
"  let path = normalizePath(currentPath + '/' + name);\n"
"  window.location.href = '/api/download?path=' + encodeURIComponent(path);\n"
"}\n"
"function renameFile(name) {\n"
"  let newName = prompt('Rename to:', name);\n"
"  if(!newName || newName === name) return;\n"
"  let oldPath = normalizePath(currentPath + '/' + name);\n"
"  let newPath = normalizePath(currentPath + '/' + newName);\n"
"  fetch('/api/rename?old=' + encodeURIComponent(oldPath) + '&new=' + encodeURIComponent(newPath))\n"
"    .then(r => r.json())\n"
"    .then(() => loadFiles())\n"
"    .catch(e => alert('Rename failed'));\n"
"}\n"
"let modalSourceFile = '';\n"
"let modalCurrentPath = '/data';\n"
"let modalOperation = 'copy';\n"
"function copyFile(name) {\n"
"  modalSourceFile = name;\n"
"  modalCurrentPath = currentPath;\n"
"  modalOperation = 'copy';\n"
"  document.getElementById('modalTitle').textContent = 'Copy: Select Destination';\n"
"  document.getElementById('copyModal').classList.add('active');\n"
"  loadModalFiles();\n"
"}\n"
"function moveFile(name) {\n"
"  modalSourceFile = name;\n"
"  modalCurrentPath = currentPath;\n"
"  modalOperation = 'move';\n"
"  document.getElementById('modalTitle').textContent = 'Move: Select Destination';\n"
"  document.getElementById('copyModal').classList.add('active');\n"
"  loadModalFiles();\n"
"}\n"
"function closeModal() {\n"
"  document.getElementById('copyModal').classList.remove('active');\n"
"}\n"
"function loadModalFiles() {\n"
"  document.getElementById('modalPath').textContent = modalCurrentPath;\n"
"  fetch('/api/list?path=' + encodeURIComponent(modalCurrentPath))\n"
"    .then(r => r.json())\n"
"    .then(data => {\n"
"      let html = '';\n"
"      if (data.files) {\n"
"        data.files.filter(f => f.type === 'dir').forEach(f => {\n"
"          html += '<div class=\"file-item\" data-dirname=\"' + f.name + '\" style=\"cursor:pointer;\">';\n"
"          html += '<div class=\"file-info\"><span class=\"file-icon\">📁</span><span>' + f.name + '</span></div>';\n"
"          html += '</div>';\n"
"        });\n"
"      }\n"
"      document.getElementById('modalFileList').innerHTML = html || '<div class=\"loading\">No folders</div>';\n"
"      document.querySelectorAll('#modalFileList .file-item').forEach(el => {\n"
"        el.addEventListener('click', () => modalOpenDir(el.getAttribute('data-dirname')));\n"
"      });\n"
"    });\n"
"}\n"
"function modalOpenDir(name) {\n"
"  modalCurrentPath = normalizePath(modalCurrentPath + '/' + name);\n"
"  loadModalFiles();\n"
"}\n"
"function modalGoUp() {\n"
"  let parts = modalCurrentPath.split('/').filter(p => p);\n"
"  parts.pop();\n"
"  modalCurrentPath = '/' + parts.join('/');\n"
"  loadModalFiles();\n"
"}\n"
"function selectDestination() {\n"
"  let newName = prompt('File name in destination:', modalSourceFile);\n"
"  if(!newName) return;\n"
"  let src = normalizePath(currentPath + '/' + modalSourceFile);\n"
"  let dst = normalizePath(modalCurrentPath + '/' + newName);\n"
"  console.log('Copy operation:');\n"
"  console.log('  Source:', src);\n"
"  console.log('  Destination:', dst);\n"
"  if (modalOperation === 'copy') {\n"
"    let url = '/api/copy?src=' + encodeURIComponent(src) + '&dst=' + encodeURIComponent(dst);\n"
"    console.log('  URL:', url);\n"
"    fetch(url)\n"
"      .then(r => {\n"
"        console.log('  Response status:', r.status);\n"
"        return r.text();\n"
"      })\n"
"      .then(text => {\n"
"        console.log('  Response body:', text);\n"
"        try {\n"
"          let data = JSON.parse(text);\n"
"          if (data.error) {\n"
"            alert('Copy failed: ' + data.error);\n"
"          } else {\n"
"            closeModal();\n"
"            loadFiles();\n"
"          }\n"
"        } catch(e) {\n"
"          alert('Copy failed: Invalid response');\n"
"        }\n"
"      })\n"
"      .catch(e => {\n"
"        console.error('Copy error:', e);\n"
"        alert('Copy failed: ' + e.message);\n"
"      });\n"
"  } else if (modalOperation === 'move') {\n"
"    let url = '/api/copy?src=' + encodeURIComponent(src) + '&dst=' + encodeURIComponent(dst);\n"
"    console.log('  URL:', url);\n"
"    fetch(url)\n"
"      .then(r => r.json())\n"
"      .then(() => fetch('/api/delete?path=' + encodeURIComponent(src)))\n"
"      .then(r => r.json())\n"
"      .then(() => { closeModal(); loadFiles(); })\n"
"      .catch(e => alert('Move failed: ' + e.message));\n"
"  }\n"
"}\n"
"function deleteFile(name) {\n"
"  if(!confirm('Delete ' + name + '?')) return;\n"
"  let path = normalizePath(currentPath + '/' + name);\n"
"  fetch('/api/delete?path=' + encodeURIComponent(path))\n"
"    .then(r => r.json())\n"
"    .then(() => loadFiles())\n"
"    .catch(e => alert('Delete failed'));\n"
"}\n"
"function formatSize(bytes) {\n"
"  if(bytes < 1024) return bytes + ' B';\n"
"  if(bytes < 1024*1024) return (bytes/1024).toFixed(1) + ' KB';\n"
"  if(bytes < 1024*1024*1024) return (bytes/1024/1024).toFixed(1) + ' MB';\n"
"  return (bytes/1024/1024/1024).toFixed(1) + ' GB';\n"
"}\n"
"function uploadFile() {\n"
"  let fileInput = document.getElementById('fileUpload');\n"
"  let file = fileInput.files[0];\n"
"  if (!file) return;\n"
"  let progressDiv = document.getElementById('uploadProgress');\n"
"  let filenameSpan = document.getElementById('uploadFilename');\n"
"  let statusSpan = document.getElementById('uploadStatus');\n"
"  let progressBar = document.getElementById('uploadBar');\n"
"  filenameSpan.textContent = file.name;\n"
"  statusSpan.textContent = 'Uploading...';\n"
"  progressDiv.style.display = 'block';\n"
"  progressBar.style.width = '0%';\n"
"  let formData = new FormData();\n"
"  formData.append('file', file);\n"
"  let currentPath = document.getElementById('currentPath').value;\n"
"  let xhr = new XMLHttpRequest();\n"
"  xhr.upload.addEventListener('progress', function(e) {\n"
"    if (e.lengthComputable) {\n"
"      let percent = (e.loaded / e.total) * 100;\n"
"      progressBar.style.width = percent + '%';\n"
"      statusSpan.textContent = 'Uploading... ' + Math.round(percent) + '%';\n"
"    }\n"
"  });\n"
"  xhr.addEventListener('load', function() {\n"
"    if (xhr.status === 200) {\n"
"      try {\n"
"        let response = JSON.parse(xhr.responseText);\n"
"        if (response.success) {\n"
"          progressBar.style.width = '100%';\n"
"          statusSpan.textContent = 'Upload complete! (' + formatSize(response.size) + ')';\n"
"          setTimeout(function() {\n"
"            progressDiv.style.display = 'none';\n"
"            fileInput.value = '';\n"
"            loadFiles();\n"
"          }, 2000);\n"
"        } else {\n"
"          statusSpan.textContent = 'Upload failed: ' + (response.error || 'Unknown error');\n"
"          console.error('Upload error:', response.error);\n"
"        }\n"
"      } catch(e) {\n"
"        statusSpan.textContent = 'Upload failed: Invalid response - ' + xhr.responseText;\n"
"        console.error('Parse error:', e, 'Response:', xhr.responseText);\n"
"      }\n"
"    } else {\n"
"      try {\n"
"        let response = JSON.parse(xhr.responseText);\n"
"        statusSpan.textContent = 'Upload failed: ' + (response.error || 'Server error');\n"
"        console.error('Server error:', xhr.status, response);\n"
"      } catch(e) {\n"
"        statusSpan.textContent = 'Upload failed: Server error ' + xhr.status;\n"
"        console.error('Server error:', xhr.status, xhr.responseText);\n"
"      }\n"
"    }\n"
"  });\n"
"  xhr.addEventListener('error', function() {\n"
"    statusSpan.textContent = 'Upload failed: Network error';\n"
"  });\n"
"  xhr.open('POST', '/api/upload?path=' + encodeURIComponent(currentPath));\n"
"  xhr.send(formData);\n"
"}\n"
"function loadSystemInfo() {\n"
"  console.log('Loading system info...');\n"
"  fetch('/api/sysinfo')\n"
"    .then(r => {\n"
"      console.log('Sysinfo response status:', r.status);\n"
"      if (!r.ok) throw new Error('HTTP ' + r.status);\n"
"      return r.json();\n"
"    })\n"
"    .then(data => {\n"
"      console.log('System info received:', data);\n"
"      let html = '';\n"
"      html += '<div class=\"stat-card\"><h3>💾 /data Storage</h3><div class=\"stat-value\">' + formatSize(data.storage.data.used) + ' / ' + formatSize(data.storage.data.total) + '</div><div style=\"font-size:14px;margin-top:5px;\">Free: ' + formatSize(data.storage.data.free) + '</div></div>';\n"
"      if (data.storage.system) {\n"
"        html += '<div class=\"stat-card\"><h3>⚙️ /system Storage</h3><div class=\"stat-value\">' + formatSize(data.storage.system.used) + ' / ' + formatSize(data.storage.system.total) + '</div></div>';\n"
"      }\n"
"      html += '<div class=\"stat-card\"><h3>🧠 RAM Usage</h3><div class=\"stat-value\">' + formatSize(data.ram.used) + ' / ' + formatSize(data.ram.total) + '</div><div style=\"font-size:14px;margin-top:5px;\">' + Math.round(data.ram.used/data.ram.total*100) + '% used</div></div>';\n"
"      html += '<div class=\"stat-card\"><h3>⏱️ Uptime</h3><div class=\"stat-value\">' + data.uptime.days + 'd ' + data.uptime.hours + 'h ' + data.uptime.minutes + 'm</div><div style=\"font-size:14px;margin-top:5px;\">' + data.uptime.secs + ' seconds</div></div>';\n"
"      html += '<div class=\"stat-card\"><h3>🌐 Network</h3><div class=\"stat-value\">' + data.network.ip + '</div><div style=\"font-size:14px;margin-top:5px;\">' + data.network.hostname + '</div></div>';\n"
"      html += '<div class=\"stat-card\"><h3>📊 Total Requests</h3><div class=\"stat-value\">' + data.server.total_requests + '</div></div>';\n"
"      html += '<div class=\"stat-card\"><h3>📁 Files Transferred</h3><div class=\"stat-value\">' + data.server.files_transferred + '</div></div>';\n"
"      html += '<div class=\"stat-card\"><h3>📦 Data Transferred</h3><div class=\"stat-value\">' + formatSize(data.server.bytes_transferred) + '</div></div>';\n"
"      html += '<div class=\"stat-card\"><h3>🔗 Active Connections</h3><div class=\"stat-value\">' + data.server.active_connections + '</div></div>';\n"
"      document.getElementById('systemStats').innerHTML = html;\n"
"    })\n"
"    .catch(e => {\n"
"      console.error('Sysinfo error:', e);\n"
"      document.getElementById('systemStats').innerHTML = '<div class=\"loading\">Error: ' + e.message + '</div>';\n"
"    });\n"
"}\n"
"loadFiles();\n"
"setInterval(loadSystemInfo, 1000);\n"
"</script>\n"
"</body>\n"
"</html>";
    
    send_http_response(sock, 200, "text/html", html, strlen(html));
}

// Handle file upload
void handle_upload_file(int sock, const char *request) {
    // Get boundary from Content-Type header
    char *boundary = strstr(request, "boundary=");
    if (!boundary) {
        const char *error_msg = "{\"error\":\"No boundary found in headers\"}";
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    boundary += 9;
    
    // Skip any leading dashes in the boundary value
    while (*boundary == '-') boundary++;
    
    char boundary_str[256];
    int i = 0;
    while (boundary[i] && boundary[i] != '\r' && boundary[i] != '\n' && boundary[i] != ';' && i < 255) {
        boundary_str[i] = boundary[i];
        i++;
    }
    boundary_str[i] = '\0';
    
    // Find body start (after HTTP headers)
    char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        const char *error_msg = "{\"error\":\"No body found - headers incomplete\"}";
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    body += 4;
    
    // Find filename in multipart data
    char *filename_start = strstr(body, "filename=\"");
    if (!filename_start) {
        const char *error_msg = "{\"error\":\"No filename in multipart data\"}";
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    filename_start += 10;
    char filename[MAX_PATH];
    int j = 0;
    while (filename_start[j] && filename_start[j] != '"' && j < MAX_PATH - 1) {
        filename[j] = filename_start[j];
        j++;
    }
    filename[j] = '\0';
    
    // Sanitize filename
    for (int k = 0; filename[k]; k++) {
        if (filename[k] == '/' || filename[k] == '\\') {
            filename[k] = '_';
        }
    }
    
    // Find file data start (after multipart part headers - look for \r\n\r\n after filename)
    char *part_headers_end = strstr(filename_start, "\r\n\r\n");
    if (!part_headers_end) {
        const char *error_msg = "{\"error\":\"No file data start marker\"}";
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    char *data_start = part_headers_end + 4;
    
    // Calculate file size from Content-Length
    // Content-Length includes all multipart data, we need to subtract:
    // - body start to data_start (multipart headers)
    // - boundary at end (--boundary--\r\n is about strlen(boundary)+8)
    char *content_length_str = strstr(request, "Content-Length: ");
    if (!content_length_str) {
        const char *error_msg = "{\"error\":\"No Content-Length\"}";
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    content_length_str += 16;
    size_t content_length = atoll(content_length_str);
    
    // Calculate: body_start = request headers end + 4
    char *body_start = strstr(request, "\r\n\r\n");
    if (!body_start) {
        const char *error_msg = "{\"error\":\"No body start\"}";
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    body_start += 4;
    
    // File size = Content-Length - (data_start - body_start) - boundary_end_size
    size_t multipart_headers_size = data_start - body_start;
    size_t boundary_end_size = strlen(boundary_str) + 8; // \r\n--boundary--\r\n
    size_t file_size = content_length - multipart_headers_size - boundary_end_size;
    
    if (file_size == 0 || file_size > 100 * 1024 * 1024) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "{\"error\":\"Invalid file size: %zu bytes\"}", file_size);
        send_http_response(sock, 400, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    // Get path from query - extract from first line only (not from body)
    char first_line[1024];
    const char *line_end = strstr(request, "\r\n");
    if (line_end) {
        size_t line_len = line_end - request;
        if (line_len > sizeof(first_line) - 1) line_len = sizeof(first_line) - 1;
        memcpy(first_line, request, line_len);
        first_line[line_len] = '\0';
    } else {
        strncpy(first_line, request, sizeof(first_line) - 1);
        first_line[sizeof(first_line) - 1] = '\0';
    }
    
    char *query = strchr(first_line, '?');
    char *path_param = get_query_param(query, "path");
    char filepath[MAX_PATH];
    
    if (path_param && strlen(path_param) > 0) {
        char decoded_path[MAX_PATH];
        url_decode(decoded_path, path_param);
        snprintf(filepath, sizeof(filepath), "%s/%s", decoded_path, filename);
    } else {
        snprintf(filepath, sizeof(filepath), "/data/%s", filename);
    }
    
    // Write file
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "{\"error\":\"Failed to create file: %s (errno=%d)\"}", filepath, errno);
        send_http_response(sock, 500, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    ssize_t written = write(fd, data_start, file_size);
    close(fd);
    
    if (written != (ssize_t)file_size) {
        unlink(filepath);
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "{\"error\":\"Failed to write file: wrote %zd of %zu bytes\"}", written, file_size);
        send_http_response(sock, 500, "application/json", error_msg, strlen(error_msg));
        return;
    }
    
    // Update stats
    total_files_transferred++;
    total_bytes_transferred += file_size;
    
    char response[512];
    snprintf(response, sizeof(response), "{\"success\":true,\"filename\":\"%s\",\"size\":%zu,\"path\":\"%s\"}", 
             filename, file_size, filepath);
    send_http_response(sock, 200, "application/json", response, strlen(response));
}

// Extract query parameter (uses alternating buffers to avoid overwrite)
char* get_query_param(const char *query, const char *param_name) {
    if (!query) return NULL;
    char *param = strstr(query, param_name);
    if (!param) return NULL;
    param += strlen(param_name);
    if (*param != '=') return NULL;
    param++;
    
    static char result1[MAX_PATH];
    static char result2[MAX_PATH];
    static int use_buffer = 0;
    
    char *result = (use_buffer == 0) ? result1 : result2;
    use_buffer = 1 - use_buffer;
    
    int i = 0;
    while (param[i] && param[i] != '&' && param[i] != ' ' && i < MAX_PATH - 1) {
        result[i] = param[i];
        i++;
    }
    result[i] = '\0';
    return result;
}

// Handle HTTP request
void handle_request(int sock, const char *request) {
    char method[16], path[MAX_PATH], version[16];
    sscanf(request, "%s %s %s", method, path, version);
    
    if (strcmp(path, "/") == 0) {
        serve_web_interface(sock);
    } else if (strncmp(path, "/api/list", 9) == 0) {
        char *query = strchr(path, '?');
        char *path_param = get_query_param(query, "path");
        if (path_param) {
            handle_list_files(sock, path_param);
        } else {
            handle_list_files(sock, "/data");
        }
    } else if (strncmp(path, "/api/download", 13) == 0) {
        char *query = strchr(path, '?');
        char *path_param = get_query_param(query, "path");
        if (path_param) {
            handle_download_file(sock, path_param);
        } else {
            send_http_response(sock, 404, "text/plain", "Path required", 13);
        }
    } else if (strncmp(path, "/api/delete", 11) == 0) {
        char *query = strchr(path, '?');
        char *path_param = get_query_param(query, "path");
        if (path_param) {
            handle_delete(sock, path_param);
        } else {
            send_http_response(sock, 404, "text/plain", "Path required", 13);
        }
    } else if (strcmp(path, "/api/sysinfo") == 0) {
        handle_system_info(sock);
    } else if (strncmp(path, "/api/rename", 11) == 0) {
        char *query = strchr(path, '?');
        char *old_param = get_query_param(query, "old");
        char *new_param = get_query_param(query, "new");
        if (old_param && new_param) {
            handle_rename(sock, old_param, new_param);
        } else {
            send_http_response(sock, 404, "text/plain", "Parameters required", 19);
        }
    } else if (strncmp(path, "/api/copy", 9) == 0) {
        char *query = strchr(path, '?');
        char *src_param = get_query_param(query, "src");
        char *dst_param = get_query_param(query, "dst");
        if (src_param && dst_param) {
            handle_copy(sock, src_param, dst_param);
        } else {
            send_http_response(sock, 404, "text/plain", "Parameters required", 19);
        }
    } else if (strncmp(path, "/api/upload", 11) == 0) {
        if (strcmp(method, "POST") == 0) {
            handle_upload_file(sock, request);
        } else {
            send_http_response(sock, 405, "text/plain", "Method not allowed", 18);
        }
    } else {
        send_http_response(sock, 404, "text/plain", "Not found", 9);
    }
}

// Client thread
void* client_thread(void* arg) {
    client_info_t* info = (client_info_t*)arg;
    int sock = info->client_sock;
    
    active_connections++;
    total_requests++;
    
    // Set TCP_NODELAY to ensure responses are sent immediately
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        close(sock);
        free(info);
        active_connections--;
        return NULL;
    }
    
    // Read initial request (headers)
    ssize_t n = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        
        // Check if this is a POST request with body
        if (strncmp(buffer, "POST", 4) == 0) {
            // Get Content-Length
            char *content_length_str = strstr(buffer, "Content-Length: ");
            if (content_length_str) {
                content_length_str += 16;
                size_t content_length = atoll(content_length_str);
                
                // Find where headers end
                char *headers_end = strstr(buffer, "\r\n\r\n");
                
                if (headers_end && content_length > 0 && content_length < 50 * 1024 * 1024) {
                    headers_end += 4;
                    size_t headers_len = headers_end - buffer;
                    
                    // Allocate buffer for full request (headers + body)
                    size_t total_size = headers_len + content_length;
                    char *full_buffer = malloc(total_size + 1);
                    if (full_buffer) {
                        // Copy what we already have
                        memcpy(full_buffer, buffer, n);
                        
                        // Read the rest of the body
                        size_t total_read = n;
                        size_t target = headers_len + content_length;
                        
                        while (total_read < target) {
                            ssize_t nr = recv(sock, full_buffer + total_read, target - total_read, 0);
                            if (nr <= 0) break;
                            total_read += nr;
                        }
                        
                        full_buffer[total_read] = '\0';
                        free(buffer);
                        buffer = full_buffer;
                        n = total_read;
                    }
                }
            }
        }
        
        // Handle request
        handle_request(sock, buffer);
    }
    
    free(buffer);
    
    // Small delay to ensure response is sent before closing
    usleep(50000); // 50ms
    
    close(sock);
    free(info);
    active_connections--;
    
    return NULL;
}

int main() {
    int server_sock;
    struct sockaddr_in server_addr;
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(HTTP_PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 10) < 0) {
        close(server_sock);
        return 1;
    }
    
    // Get actual IP address from network interfaces
    char ip_str[INET_ADDRSTRLEN] = "0.0.0.0";
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);
                if (strcmp(ip_str, "127.0.0.1") != 0) {
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
    }
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Web Manager: http://%s:%d - By Manos", ip_str, HTTP_PORT);
    send_notification(msg);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }
        
        client_info_t* client_info = malloc(sizeof(client_info_t));
        if (!client_info) {
            close(client_sock);
            continue;
        }
        
        client_info->client_sock = client_sock;
        client_info->client_addr = client_addr;
        
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&thread, &attr, client_thread, client_info) != 0) {
            close(client_sock);
            free(client_info);
        }
        
        pthread_attr_destroy(&attr);
    }
    
    close(server_sock);
    return 0;
}
