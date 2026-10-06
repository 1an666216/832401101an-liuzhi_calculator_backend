# 后端代码规范（codestyle）

> **规范来源**：[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
> 本文件在 Google 规范基础上，结合本项目实际情况提取可执行条目。

## 1. 命名

- 类、结构体使用 `PascalCase`：`ExprParser`、`HistoryItem`
- 函数使用 `camelCase`（Google C++ 为 `CamelCase`，本项目按文件内既有风格统一为小写开头）：`dbInit`、`dbQueryHistory`、`formatNumber`
- 成员变量以下划线结尾：`s_`、`pos_`
- 全局变量以 `g_` 前缀：`g_db`、`g_dbMutex`
- 常量使用 `ALL_CAPS`

## 2. 格式

- 缩进：4 个空格，不使用 Tab
- 花括号：`{` 与语句同行，`}` 独占一行（K&R 风格）
- 每行不超过 80 字符（尽量）
- 指针/引用符号与类型结合：`sqlite3* g_db`

## 3. 头文件与依赖

- 系统头文件在前（`winsock2.h`、`windows.h`），第三方（`sqlite3.h`）居中，标准库（`cmath`、`string`…）在后，每组间空一行
- 头文件按需包含，不引入无用依赖

## 4. 函数与作用域

- 函数体内部变量就近声明
- 单一职责：一个函数只做一件事，超过约 80 行应拆分
- 模块之间通过函数名 `db*` / `send*` / `parse*` 前缀区分职责，不互相渗透

## 5. 错误处理

- 可预知错误（除零、非法字符、JSON 缺字段）抛出 `CalcError` 并返回 400
- 数据库失败返回 500，绝不静默吞掉错误
- 网络连接异常用 `try/catch` 兜底保证 socket 关闭

## 6. 并发与安全

- 共享资源（SQLite 连接）统一由 `g_dbMutex` 互斥保护
- 输入校验：表达式长度上限、字符白名单，禁止任何形式的代码执行路径
- 输出 JSON 一律经过 `jsonEscape` 转义，防止注入

## 7. 注释

- 文件头注明功能、API、编译命令
- 关键算法（递归下降解析）写明产生式规则
- 中文注释，说明"做什么、为什么"

## 8. 其他

- 不使用 `eval` / `system` / `popen` 等执行用户输入
- 不使用未初始化的变量；指针使用前判空
- 提交代码前删除调试输出，保持编译零警告
