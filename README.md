# 计算器后端（Backend）

## 项目介绍

前后端分离计算器系统的**后端**部分。负责接收前端请求、校验输入、解析与计算表达式、处理异常、管理数据库中的计算历史，并返回标准化 JSON 响应。

- 表达式解析使用**自研递归下降解析器**，不使用 `eval` / `exec` 等任意代码执行手段
- 历史记录持久化到 **SQLite** 数据库（与程序同目录的 `calculator.db`，自动创建）
- 基于原生 `winsock2` 实现 HTTP 服务器，无第三方框架依赖

## 技术栈

- C++17
- Windows Socket（winsock2）原生 HTTP 服务
- SQLite3（amalgamation 单文件源码，C API，直接编译进程序）
- 多线程：每连接一个线程（`std::thread`）

## 运行环境

- Windows（本项目基于 Win32 API 开发）
- 需要 MinGW-w64 工具链（本项目使用 w64devkit 便携版）编译

## 安装与编译

前置：安装 MinGW-w64（如 w64devkit），并将 `gcc` / `g++` 加入 PATH。

本项目使用 SQLite 官方 amalgamation 源码（`sqlite3.c` / `sqlite3.h`），
需先从 <https://sqlite.org/download.html> 下载并解压到项目 `sqlite_src/sqlite-amalgamation-XXXX/` 目录。

两步编译（在项目根目录执行）：

```bat
gcc -O2 -c sqlite_src/sqlite-amalgamation-3530400/sqlite3.c -o sqlite3.o
g++ -O2 -std=c++17 server.cpp sqlite3.o -Isqlite_src/sqlite-amalgamation-3530400 -o server.exe -lws2_32 -static
```

## 启动方法

```bat
server.exe [端口]
```

- 默认端口 `8080`；页面地址 `http://127.0.0.1:8080`
- 停止：`Ctrl+C`（或结束进程）

## 配置说明

- 端口：通过命令行参数传入，默认 8080
- 历史数据库文件：`calculator.db`，位于 `server.exe` 同目录，首次启动自动创建
- 前端页面：`index.html` 需放在 `server.exe` 同目录（后端直接托管页面）

## 数据库初始化

首次启动时自动完成（无需手动操作）：

```sql
CREATE TABLE IF NOT EXISTS calculation_history (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  expression TEXT NOT NULL,
  result TEXT NOT NULL,
  created_at TEXT NOT NULL
);
```

## API 接口

| 方法 | 路径 | 说明 | 状态码 |
|---|---|---|---|
| POST | `/api/calculate` | 计算表达式，成功则入库 | 200 / 400 / 500 |
| GET | `/api/history` | 查询最近 50 条历史（新在前） | 200 |
| DELETE | `/api/history?id=N` | 删除指定 id 的记录 | 200 / 500 |
| DELETE | `/api/history` | 清空全部历史 | 200 / 500 |
| GET | `/` | 返回前端页面 index.html | 200 / 404 |

请求示例：

```json
{ "expression": "(1+2)*3" }
```

成功响应：

```json
{ "success": true, "expression": "(1+2)*3", "result": "9", "id": 5, "time": "2026-10-06 17:00:04" }
```

错误响应（400 Bad Request）：

```json
{ "success": false, "message": "除数不能为 0" }
```

## 前后端连接方式

前端通过 HTTP 调用上述接口；后端已配置 CORS 响应头
（`Access-Control-Allow-Origin: *`），支持跨域访问与 `OPTIONS` 预检请求。

## 目录结构

```
calculator_backend/
├── server.cpp      # 后端主程序（HTTP 服务 + 表达式解析 + SQLite）
├── sqlite_src/     # SQLite amalgamation 源码（第三方，编译依赖）
├── README.md
└── codestyle.md
```
