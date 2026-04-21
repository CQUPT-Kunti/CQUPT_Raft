# 跨平台 tests 目录说明

这版测试代码的目标是同时兼容 Linux、macOS 和 Windows。

主要调整：

1. `tests/CMakeLists.txt` 统一使用 `FetchContent` 拉取 GoogleTest，避免 Windows 下
   `find_package(GTest)` 命中预编译库后产生 Debug/Release 运行时不一致。
2. 在 MSVC 下显式设置 `CMAKE_MSVC_RUNTIME_LIBRARY`，让测试目标和 GoogleTest 使用同一套运行时。
3. `persistence_test.cpp` 改成 GTest 测试文件，不再使用自定义 `main()`。
4. `persistence_test.cpp` 使用 `std::filesystem::temp_directory_path()` 和 `std::filesystem::path`
   生成临时测试目录，避免平台相关路径分隔符问题。

## 需要的根目录 CMake 配合

建议在根 `CMakeLists.txt` 的 `project(...)` 后加上：

```cmake
if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
```

并确保根目录启用了：

```cmake
enable_testing()
add_subdirectory(tests)
```
