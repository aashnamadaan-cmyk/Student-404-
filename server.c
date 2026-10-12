#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define PORT 8080
#define REQUEST_SIZE 65536
#define BODY_SIZE 32768
#define FIELD_SIZE 600
#define MAX_REPORTS 1000

typedef struct {
    char studentName[FIELD_SIZE];
    char itemName[FIELD_SIZE];
    char itemType[16];
    char location[FIELD_SIZE];
    char description[1200];
} Report;

static Report reports[MAX_REPORTS];
static int reportCount = 0;

static void cleanText(char *s) {
    size_t i, j = 0;
    for (i = 0; s[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') s[i] = ' ';
        else if (ch < 32) continue;
        s[j++] = s[i];
    }
    s[j] = '\0';
}

static void jsonEscape(const char *src, char *dst, size_t dstSize) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstSize; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c >= 32) dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

static void sendAll(SOCKET s, const char *data, int length) {
    int sent = 0;
    while (sent < length) {
        int n = send(s, data + sent, length - sent, 0);
        if (n <= 0) return;
        sent += n;
    }
}

static void sendResponse(SOCKET client, const char *status, const char *type, const char *body) {
    char header[1024];
    int bodyLength = (int)strlen(body);
    int headerLength = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\n"
        "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        status, type, bodyLength);
    sendAll(client, header, headerLength);
    sendAll(client, body, bodyLength);
}

static void decodeJsonString(const char *src, char *dst, size_t dstSize) {
    size_t j = 0;
    while (*src && j + 1 < dstSize) {
        if (*src == '\\' && src[1]) {
            src++;
            if (*src == 'n' || *src == 'r') dst[j++] = ' ';
            else dst[j++] = *src++;
            if (src[-1] != 'n' && src[-1] != 'r') continue;
        } else {
            dst[j++] = *src++;
        }
    }
    dst[j] = '\0';
}

static int getJsonString(const char *json, const char *key, char *out, size_t outSize) {
    char pattern[100];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++;
    size_t j = 0;
    while (*p && j + 1 < outSize) {
        if (*p == '"' ) break;
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n' || *p == 'r') out[j++] = ' ';
            else out[j++] = *p;
            p++;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = '\0';
    return 1;
}

static int loadReports(void) {
    FILE *f = fopen("reports.txt", "r");
    char line[3000];
    Report r;
    int haveAny = 0;
    if (!f) return 1;
    memset(&r, 0, sizeof(r));
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "Student: ", 9) == 0) {
            memset(&r, 0, sizeof(r));
            snprintf(r.studentName, sizeof(r.studentName), "%s", line + 9);
            haveAny = 1;
        } else if (strncmp(line, "Item: ", 6) == 0) {
            snprintf(r.itemName, sizeof(r.itemName), "%s", line + 6);
        } else if (strncmp(line, "Type: ", 6) == 0) {
            snprintf(r.itemType, sizeof(r.itemType), "%s", line + 6);
        } else if (strncmp(line, "Location: ", 10) == 0) {
            snprintf(r.location, sizeof(r.location), "%s", line + 10);
        } else if (strncmp(line, "Description: ", 13) == 0) {
            snprintf(r.description, sizeof(r.description), "%s", line + 13);
        } else if (strcmp(line, "------------------------------") == 0 && haveAny) {
            if (reportCount < MAX_REPORTS && r.studentName[0] && r.itemName[0]) {
                reports[reportCount++] = r;
            }
            memset(&r, 0, sizeof(r));
            haveAny = 0;
        }
    }
    fclose(f);
    return 1;
}

static int saveReport(const Report *r) {
    FILE *f = fopen("reports.txt", "a");
    if (!f) return 0;
    fprintf(f, "Student: %s\nItem: %s\nType: %s\nLocation: %s\nDescription: %s\n------------------------------\n",
        r->studentName, r->itemName, r->itemType, r->location, r->description);
    fclose(f);
    return 1;
}

static void sendReportsJson(SOCKET client) {
    char *json = (char *)malloc(REQUEST_SIZE * 12);
    if (!json) {
        sendResponse(client, "500 Internal Server Error", "application/json", "{\"success\":false,\"message\":\"Out of memory\"}");
        return;
    }
    size_t used = 0;
    used += (size_t)snprintf(json + used, REQUEST_SIZE * 12 - used, "[");
    for (int i = 0; i < reportCount; i++) {
        char student[FIELD_SIZE * 2], item[FIELD_SIZE * 2], type[64], location[FIELD_SIZE * 2], desc[2400];
        jsonEscape(reports[i].studentName, student, sizeof(student));
        jsonEscape(reports[i].itemName, item, sizeof(item));
        jsonEscape(reports[i].itemType, type, sizeof(type));
        jsonEscape(reports[i].location, location, sizeof(location));
        jsonEscape(reports[i].description, desc, sizeof(desc));
        used += (size_t)snprintf(json + used, REQUEST_SIZE * 12 - used,
            "%s{\"studentName\":\"%s\",\"itemName\":\"%s\",\"itemType\":\"%s\",\"location\":\"%s\",\"description\":\"%s\"}",
            i ? "," : "", student, item, type, location, desc);
        if (used >= REQUEST_SIZE * 12 - 3000) break;
    }
    snprintf(json + used, REQUEST_SIZE * 12 - used, "]");
    sendResponse(client, "200 OK", "application/json", json);
    free(json);
}

static int readRequest(SOCKET client, char *buffer, size_t capacity, int *headerEndOut, int *bodyLengthOut) {
    int received = 0, headerEnd = -1, contentLength = 0;
    buffer[0] = '\0';
    while ((size_t)received < capacity - 1) {
        int n = recv(client, buffer + received, (int)(capacity - (size_t)received - 1), 0);
        if (n <= 0) break;
        received += n;
        buffer[received] = '\0';
        if (headerEnd < 0) {
            char *p = strstr(buffer, "\r\n\r\n");
            if (p) {
                headerEnd = (int)(p - buffer) + 4;
                char *cl = strstr(buffer, "Content-Length:");
                if (cl && cl < p) contentLength = atoi(cl + 15);
            }
        }
        if (headerEnd >= 0 && received >= headerEnd + contentLength) break;
    }
    *headerEndOut = headerEnd;
    *bodyLengthOut = (headerEnd >= 0) ? received - headerEnd : 0;
    return received;
}

static void serveFile(SOCKET client, const char *filename, const char *contentType) {
    FILE *f = fopen(filename, "rb");
    char *buffer;
    long size;
    if (!f) {
        sendResponse(client, "404 Not Found", "text/plain", "File not found. Keep index.html and style.css beside server.exe.");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (size < 0 || size > 2 * 1024 * 1024) {
        fclose(f);
        sendResponse(client, "500 Internal Server Error", "text/plain", "File is too large.");
        return;
    }
    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        sendResponse(client, "500 Internal Server Error", "text/plain", "Out of memory.");
        return;
    }
    fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[size] = '\0';
    sendResponse(client, "200 OK", contentType, buffer);
    free(buffer);
}

static void handleClient(SOCKET client) {
    char *request = (char *)calloc(REQUEST_SIZE, 1);
    char method[12] = "", path[256] = "";
    int headerEnd = -1, bodyLength = 0;
    if (!request) {
        sendResponse(client, "500 Internal Server Error", "text/plain", "Out of memory.");
        return;
    }
    int received = readRequest(client, request, REQUEST_SIZE, &headerEnd, &bodyLength);
    if (received <= 0 || sscanf(request, "%11s %255s", method, path) != 2) {
        sendResponse(client, "400 Bad Request", "text/plain", "Bad request.");
        free(request);
        return;
    }
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        serveFile(client, "index.html", "text/html");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/style.css") == 0) {
        serveFile(client, "style.css", "text/css");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/reports") == 0) {
        sendReportsJson(client);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reports") == 0) {
        Report r;
        char *body;
        memset(&r, 0, sizeof(r));
        if (headerEnd < 0 || bodyLength <= 0) {
            sendResponse(client, "400 Bad Request", "application/json", "{\"success\":false,\"message\":\"Missing request body\"}");
            free(request);
            return;
        }
        body = request + headerEnd;
        getJsonString(body, "studentName", r.studentName, sizeof(r.studentName));
        getJsonString(body, "itemName", r.itemName, sizeof(r.itemName));
        getJsonString(body, "itemType", r.itemType, sizeof(r.itemType));
        getJsonString(body, "location", r.location, sizeof(r.location));
        getJsonString(body, "description", r.description, sizeof(r.description));
        cleanText(r.studentName); cleanText(r.itemName); cleanText(r.itemType); cleanText(r.location); cleanText(r.description);
        if (!r.studentName[0] || !r.itemName[0] || !r.location[0] || !r.description[0] ||
            (strcmp(r.itemType, "Lost") != 0 && strcmp(r.itemType, "Found") != 0)) {
            sendResponse(client, "400 Bad Request", "application/json", "{\"success\":false,\"message\":\"Please complete all fields and select Lost or Found.\"}");
            free(request);
            return;
        }
        if (reportCount >= MAX_REPORTS) {
            sendResponse(client, "507 Insufficient Storage", "application/json", "{\"success\":false,\"message\":\"This demo has reached its report limit.\"}");
            free(request);
            return;
        }
        if (!saveReport(&r)) {
            sendResponse(client, "500 Internal Server Error", "application/json", "{\"success\":false,\"message\":\"Could not write reports.txt. Check folder permissions.\"}");
            free(request);
            return;
        }
        reports[reportCount++] = r;
        printf("Saved: %s | %s | %s\n", r.itemName, r.itemType, r.location);
        sendResponse(client, "201 Created", "application/json", "{\"success\":true,\"message\":\"Report saved\"}");
    } else {
        sendResponse(client, "404 Not Found", "text/plain", "Not found.");
    }
    free(request);
}

int main(void) {
    WSADATA wsa;
    SOCKET serverSocket;
    struct sockaddr_in address;
    int reuse = 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("ERROR: Winsock could not start.\n");
        return 1;
    }
    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        printf("ERROR: Could not create socket. Code: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        printf("ERROR: Port %d is already in use. Close any old server window and try again.\n", PORT);
        closesocket(serverSocket); WSACleanup(); return 1;
    }
    if (listen(serverSocket, 10) == SOCKET_ERROR) {
        printf("ERROR: Server could not listen. Code: %d\n", WSAGetLastError());
        closesocket(serverSocket); WSACleanup(); return 1;
    }
    if (!loadReports()) printf("Warning: Could not load previous reports.\n");

    printf("\n============================================\n");
    printf("          KHOYA PAYA CENTER - C SERVER\n");
    printf("============================================\n");
    printf("Website: http://localhost:%d\n", PORT);
    printf("Loaded %d previous report(s).\n", reportCount);
    printf("Reports are saved in reports.txt.\n");
    printf("Keep this terminal open. Press Ctrl+C to stop.\n\n");

    for (;;) {
        SOCKET client = accept(serverSocket, NULL, NULL);
        if (client == INVALID_SOCKET) {
            printf("Connection error: %d\n", WSAGetLastError());
            continue;
        }
        handleClient(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
    closesocket(serverSocket);
    WSACleanup();
    return 0;
}
