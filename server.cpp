// server.cpp — 前后端分离计算器 · 后端服务
// 课程作业：EE308 Software Engineering Assignment 1
//
// 职责（前端只负责收集输入与展示，计算与存储全部在本服务完成）：
//   1. 接收并校验前端请求
//   2. 解析数学表达式（自研递归下降解析器，禁止 eval/exec）
//   3. 计算并返回结果
//   4. 每次成功计算写入 SQLite 数据库（expression / result / created_at）
//   5. 提供历史记录查询与删除接口
//
// API：
//   POST   /api/calculate    {"expression":"(1+2)*3"} -> {"success":true,...}
//   GET    /api/history      返回最近 50 条历史（新在前）
//   DELETE /api/history?id=N 删除指定 id 的记录
//   DELETE /api/history      清空全部历史
//
// 编译（本项目使用 w64devkit 便携 MinGW + SQLite amalgamation）：
//   gcc  -O2 -c sqlite_src/sqlite-amalgamation-3530400/sqlite3.c -o sqlite3.o
//   g++  -O2 -std=c++17 server.cpp sqlite3.o -Isqlite_src/sqlite-amalgamation-3530400 \
//        -o server.exe -lws2_32 -static
//
// 运行：
//   server.exe [端口]   默认 8080，页面 http://127.0.0.1:8080
//   历史数据库文件与 server.exe 同目录：calculator.db（自动创建）

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <sqlite3.h>

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ===============================================================
// 一、表达式计算：递归下降解析器（自实现，不执行任何外部命令，杜绝注入）
// 支持：+ - * / % ^ 括号、小数、一元正负号
// 优先级：括号 > 幂(右结合) > 一元正负 > 乘除模 > 加减
// ===============================================================

struct CalcError {
    std::string msg;
    explicit CalcError(std::string m) : msg(std::move(m)) {}
};

class ExprParser {
public:
    explicit ExprParser(const std::string& src) : s_(src) {}

    double parse() {
        if (s_.empty()) throw CalcError("表达式为空");
        if (s_.size() > 200) throw CalcError("表达式过长（上限 200 字符）");
        double v = parseExpr();
        skipSpaces();
        if (pos_ < s_.size()) throw CalcError("表达式包含无法识别的字符");
        if (!std::isfinite(v)) throw CalcError("结果超出可表示范围");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skipSpaces() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
    }

    char peek() {
        skipSpaces();
        return pos_ < s_.size() ? s_[pos_] : '\0';
    }

    char take() {
        skipSpaces();
        if (pos_ >= s_.size()) throw CalcError("表达式意外结束");
        return s_[pos_++];
    }

    // expr := term (('+'|'-') term)*
    double parseExpr() {
        double v = parseTerm();
        for (;;) {
            char c = peek();
            if (c == '+') { take(); v += parseTerm(); }
            else if (c == '-') { take(); v -= parseTerm(); }
            else break;
        }
        return v;
    }

    // term := unary (('*'|'/'|'%') unary)*
    double parseTerm() {
        double v = parseUnary();
        for (;;) {
            char c = peek();
            if (c == '*') { take(); v *= parseUnary(); }
            else if (c == '/') {
                take();
                double d = parseUnary();
                if (d == 0.0) throw CalcError("除数不能为 0");
                v /= d;
            }
            else if (c == '%') {
                take();
                double d = parseUnary();
                if (d == 0.0) throw CalcError("除数不能为 0");
                v = std::fmod(v, d);
            }
            else break;
        }
        return v;
    }

    // unary := ('+'|'-') unary | power
    double parseUnary() {
        char c = peek();
        if (c == '+' || c == '-') {
            take();
            double v = parseUnary();
            return (c == '-') ? -v : v;
        }
        return parsePower();
    }

    // power := primary ('^' unary)?   右结合
    double parsePower() {
        double base = parsePrimary();
        if (peek() == '^') {
            take();
            double exp = parseUnary();
            double r = std::pow(base, exp);
            if (std::isnan(r)) throw CalcError("幂运算结果无定义");
            return r;
        }
        return base;
    }

    // primary := NUMBER | '(' expr ')'
    double parsePrimary() {
        char c = peek();
        if (c == '(') {
            take();
            double v = parseExpr();
            if (take() != ')') throw CalcError("缺少右括号 )");
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            return parseNumber();
        }
        if (c == '\0') throw CalcError("表达式意外结束");
        throw CalcError(std::string("无法识别的字符: ") + c);
    }

    double parseNumber() {
        skipSpaces();
        size_t start = pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        }
        if (pos_ == start) throw CalcError("数字格式错误");
        std::string tok = s_.substr(start, pos_ - start);
        return std::strtod(tok.c_str(), nullptr);
    }
};

// ===============================================================
// 二、极简 JSON 提取：从 {"expression":"..."} 中取出 expression 字段
// ===============================================================
static bool extractField(const std::string& body, const char* key, std::string& out) {
    std::string k = "\"" + std::string(key) + "\"";
    size_t pos = body.find(k);
    if (pos == std::string::npos) return false;
    pos = body.find(':', pos + k.size());
    if (pos == std::string::npos) return false;
    pos = body.find('"', pos + 1);
    if (pos == std::string::npos) return false;

    std::string val;
    size_t i = pos + 1;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            char c = body[i + 1];
            switch (c) {
                case 'n':  val += '\n'; i += 2; break;
                case 't':  val += '\t'; i += 2; break;
                case 'r':  val += '\r'; i += 2; break;
                case '\\': val += '\\'; i += 2; break;
                case '"':  val += '"';  i += 2; break;
                case '/':  val += '/';  i += 2; break;
                case 'u':  i += 6; break;  // 跳过 \uXXXX
                default:   val += c;  i += 2; break;
            }
        } else {
            val += body[i];
            ++i;
        }
    }
    if (i >= body.size()) return false;
    out = val;
    return true;
}

// ===============================================================
// 三、结果格式化：消除浮点噪声（0.1+0.2 -> 0.3）
// ===============================================================
static std::string formatNumber(double v) {
    v = std::round(v * 1e12) / 1e12;
    std::ostringstream oss;
    oss.precision(15);
    oss << v;
    std::string s = oss.str();
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t end = s.find_last_not_of('0');
        if (end > dot) s.erase(end + 1);
        else if (end == dot) s.erase(dot);  // x.000 -> x
    }
    return s;
}

// ===============================================================
// 四、SQLite 历史记录模块
// 表结构：calculation_history(id, expression, result, created_at)
// ===============================================================
static sqlite3* g_db = nullptr;
static std::mutex g_dbMutex;

static bool dbInit(const std::string& dbPath) {
    if (sqlite3_open(dbPath.c_str(), &g_db) != SQLITE_OK) return false;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS calculation_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "expression TEXT NOT NULL,"
        "result TEXT NOT NULL,"
        "created_at TEXT NOT NULL);";
    char* err = nullptr;
    int rc = sqlite3_exec(g_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) { sqlite3_free(err); return false; }
    return true;
}

static bool dbInsert(const std::string& expr, const std::string& result,
                     const std::string& time, long long& id) {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO calculation_history(expression,result,created_at) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, expr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, time.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) id = sqlite3_last_insert_rowid(g_db);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

struct HistoryItem {
    long long id;
    std::string expr;
    std::string result;
    std::string time;
};

static std::vector<HistoryItem> dbQueryHistory(int limit) {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    std::vector<HistoryItem> items;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id,expression,result,created_at FROM calculation_history "
                      "ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return items;
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HistoryItem it;
        it.id = sqlite3_column_int64(stmt, 0);
        const unsigned char* e = sqlite3_column_text(stmt, 1);
        const unsigned char* r = sqlite3_column_text(stmt, 2);
        const unsigned char* t = sqlite3_column_text(stmt, 3);
        it.expr = e ? reinterpret_cast<const char*>(e) : "";
        it.result = r ? reinterpret_cast<const char*>(r) : "";
        it.time = t ? reinterpret_cast<const char*>(t) : "";
        items.push_back(std::move(it));
    }
    sqlite3_finalize(stmt);
    return items;
}

static bool dbDeleteById(long long id) {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM calculation_history WHERE id=?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static bool dbClear() {
    std::lock_guard<std::mutex> lock(g_dbMutex);
    char* err = nullptr;
    int rc = sqlite3_exec(g_db, "DELETE FROM calculation_history", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

// ===============================================================
// 五、HTTP 服务器
// ===============================================================
static std::string exeDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t slash = p.find_last_of('\\');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

// SQLite 需要 UTF-8 路径（中文目录下 ANSI 路径会导致打开失败）
static std::string exeDirUtf8() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L'\\');
    std::wstring dir = slash == std::wstring::npos ? L"." : p.substr(0, slash);
    int len = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), static_cast<int>(dir.size()),
                        &s[0], len, nullptr, nullptr);
    return s;
}

static std::string nowString() {
    time_t t = time(nullptr);
    struct tm tmBuf;
    localtime_s(&tmBuf, &t);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmBuf);
    return buf;
}

static std::string readUntil(SOCKET c, const std::string& delim, size_t limit) {
    std::string buf;
    char ch;
    while (buf.size() < limit) {
        int r = recv(c, &ch, 1, 0);
        if (r <= 0) break;
        buf += ch;
        if (buf.size() >= delim.size() &&
            buf.compare(buf.size() - delim.size(), delim.size(), delim) == 0)
            break;
    }
    return buf;
}

// 精确读取 n 字节（用于读取请求体）
static std::string readN(SOCKET c, size_t n) {
    std::string buf;
    buf.resize(n);
    size_t got = 0;
    while (got < n) {
        int r = recv(c, &buf[got], static_cast<int>(n - got), 0);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    buf.resize(got);
    return buf;
}

static void sendAll(SOCKET c, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int r = send(c, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (r <= 0) break;
        sent += static_cast<size_t>(r);
    }
}

static void sendResponse(SOCKET c, int status, const std::string& statusText,
                         const std::string& contentType, const std::string& body) {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << " " << statusText << "\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "Cache-Control: no-store\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\n"
         << "Access-Control-Allow-Headers: Content-Type\r\n"
         << "\r\n";
    sendAll(c, head.str() + body);
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;
        }
    }
    return o;
}

// 历史列表 -> JSON
static std::string historyJson(const std::vector<HistoryItem>& items) {
    std::ostringstream oss;
    oss << "{\"success\":true,\"history\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) oss << ",";
        oss << "{\"id\":" << items[i].id
            << ",\"expression\":\"" << jsonEscape(items[i].expr)
            << "\",\"result\":\"" << jsonEscape(items[i].result)
            << "\",\"time\":\"" << jsonEscape(items[i].time) << "\"}";
    }
    oss << "]}";
    return oss.str();
}

static void handleClient(SOCKET client) {
    try {
        std::string request = readUntil(client, "\r\n\r\n", 64 * 1024);
        if (request.empty()) { closesocket(client); return; }

        // 解析请求行: METHOD PATH HTTP/1.1
        std::string method, path;
        {
            size_t eol = request.find("\r\n");
            std::string line = (eol == std::string::npos) ? request : request.substr(0, eol);
            std::istringstream iss(line);
            iss >> method >> path;
        }

        // 解析 query 参数（?id=N）
        long long queryId = -1;
        std::string pathOnly = path;
        {
            size_t q = path.find('?');
            if (q != std::string::npos) {
                pathOnly = path.substr(0, q);
                std::string query = path.substr(q + 1);
                size_t p = query.find("id=");
                if (p != std::string::npos) {
                    queryId = std::strtoll(query.c_str() + p + 3, nullptr, 10);
                }
            }
        }

        // 读取请求体（POST/DELETE 可能携带）
        std::string body;
        if (method == "POST" || method == "DELETE") {
            size_t contentLength = 0;
            {
                size_t p = request.find("Content-Length:");
                if (p == std::string::npos) p = request.find("content-length:");
                if (p != std::string::npos) {
                    size_t eol = request.find("\r\n", p);
                    std::string v = request.substr(p + 15, eol == std::string::npos ? std::string::npos : eol - p - 15);
                    contentLength = static_cast<size_t>(std::strtoull(v.c_str(), nullptr, 10));
                }
            }
            size_t headEnd = request.find("\r\n\r\n");
            if (headEnd != std::string::npos) {
                body = request.substr(headEnd + 4);
                if (body.size() < contentLength)
                    body += readN(client, contentLength - body.size());
            } else {
                body = readN(client, contentLength);
            }
            if (body.size() > contentLength) body.resize(contentLength);
        }

        // 前端页面
        if (method == "GET" && (pathOnly == "/" || pathOnly == "/index.html")) {
            std::string file = exeDir() + "\\index.html";
            FILE* f = fopen(file.c_str(), "rb");
            if (!f) {
                sendResponse(client, 404, "Not Found", "text/plain; charset=utf-8",
                             "index.html 未找到，请将 index.html 放在 server.exe 同目录");
                closesocket(client);
                return;
            }
            std::string page;
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) page.append(buf, n);
            fclose(f);
            sendResponse(client, 200, "OK", "text/html; charset=utf-8", page);
        }
        // 计算接口
        else if (method == "POST" && pathOnly == "/api/calculate") {
            std::string expr;
            if (!extractField(body, "expression", expr) || expr.empty()) {
                sendResponse(client, 400, "Bad Request", "application/json; charset=utf-8",
                             "{\"success\":false,\"message\":\"请求格式错误：缺少 expression 字段\"}");
                closesocket(client);
                return;
            }
            try {
                ExprParser parser(expr);
                double result = parser.parse();
                std::string resultStr = formatNumber(result);
                long long id = 0;
                std::string ts = nowString();
                if (!dbInsert(expr, resultStr, ts, id)) {
                    sendResponse(client, 500, "Internal Server Error", "application/json; charset=utf-8",
                                 "{\"success\":false,\"message\":\"数据库存储失败\"}");
                    closesocket(client);
                    return;
                }
                std::ostringstream oss;
                oss << "{\"success\":true,\"expression\":\"" << jsonEscape(expr)
                    << "\",\"result\":\"" << resultStr
                    << "\",\"id\":" << id
                    << ",\"time\":\"" << ts << "\"}";
                sendResponse(client, 200, "OK", "application/json; charset=utf-8", oss.str());
            } catch (const CalcError& e) {
                sendResponse(client, 400, "Bad Request", "application/json; charset=utf-8",
                             "{\"success\":false,\"message\":\"" + jsonEscape(e.msg) + "\"}");
            }
        }
        // 历史查询
        else if (method == "GET" && pathOnly == "/api/history") {
            std::vector<HistoryItem> items = dbQueryHistory(50);
            sendResponse(client, 200, "OK", "application/json; charset=utf-8", historyJson(items));
        }
        // 删除单条 / 清空
        else if (method == "DELETE" && pathOnly == "/api/history") {
            if (queryId > 0) {
                if (dbDeleteById(queryId))
                    sendResponse(client, 200, "OK", "application/json; charset=utf-8",
                                 "{\"success\":true}");
                else
                    sendResponse(client, 500, "Internal Server Error", "application/json; charset=utf-8",
                                 "{\"success\":false,\"message\":\"删除失败\"}");
            } else {
                if (dbClear())
                    sendResponse(client, 200, "OK", "application/json; charset=utf-8",
                                 "{\"success\":true}");
                else
                    sendResponse(client, 500, "Internal Server Error", "application/json; charset=utf-8",
                                 "{\"success\":false,\"message\":\"清空失败\"}");
            }
        }
        // 预检请求（CORS）
        else if (method == "OPTIONS") {
            sendResponse(client, 204, "No Content", "text/plain", "");
        }
        else {
            sendResponse(client, 404, "Not Found", "application/json; charset=utf-8",
                         "{\"success\":false,\"message\":\"404 Not Found\"}");
        }
    } catch (...) {
        // 任何异常都保证连接被关闭
    }
    closesocket(client);
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 1) {
        int p = std::atoi(argv[1]);
        if (p > 0 && p <= 65535) port = p;
    }

    // 初始化 SQLite 历史数据库
    std::string dbPath = exeDirUtf8() + "\\calculator.db";
    if (!dbInit(dbPath)) {
        std::cerr << "初始化数据库失败: " << dbPath << std::endl;
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup 失败" << std::endl;
        return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "创建套接字失败: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof opt);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        std::cerr << "端口 " << port << " 绑定失败（可能被占用）: " << WSAGetLastError() << std::endl;
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }
    if (listen(listenSock, 16) == SOCKET_ERROR) {
        std::cerr << "监听失败: " << WSAGetLastError() << std::endl;
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    std::cout << "计算器后端已启动" << std::endl;
    std::cout << "页面地址   : http://127.0.0.1:" << port << std::endl;
    std::cout << "历史数据库 : " << dbPath << std::endl;
    std::cout << "按 Ctrl+C 停止服务" << std::endl;

    for (;;) {
        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err == WSAEINTR) break;
            continue;
        }
        std::thread t(handleClient, client);
        t.detach();
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}
