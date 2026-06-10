# T090 CMake Configure

## 1. 执行命令

```bash
flock -n /tmp/cqupt_raft_build.lock -c 'cmake --preset debug-ninja-low-parallel'
```

## 2. 执行结果和退出码

- 执行结果：通过
- 退出码：0

## 3. 使用的 preset

- `debug-ninja-low-parallel`

## 4. 是否产生 configure warning；如有，简述关键 warning

- 有 1 条 configure warning。
- 关键 warning：
  - `FetchContent.cmake` 的 `DOWNLOAD_EXTRACT_TIMESTAMP` 未显式设置，`CMP0135` 仍使用 `OLD` 行为。
  - 调用栈定位到 `tests/CMakeLists.txt:8 (FetchContent_Declare)`。
- 该 warning 不阻塞 configure，本任务未扩大为修复任务。

## 5. 是否做了任何最小修复；如无，明确说明无代码/CMake 修改

- 未做任何最小修复。
- 本任务未修改代码、未修改 CMake、未修改 proto。

## 6. 是否修改 risk-register.md；如未修改，明确说明未修改

- 未修改 `risk-register.md`。

## 7. T090 是否通过

- 通过。
