# builder

这个目录用于存放 `font2c` 的源码和构建脚本。
这份文档只描述构建相关内容。

exe 的使用方式见 `../README_CN.md`。
JSON 的写法见 `../input/README_CN.md`。

## 目录内容

- `src/`：核心源码
- `include/`：公共头文件
- `build_win.cmd`：Windows 构建脚本
- `build_posix.sh`：POSIX 构建脚本
- `font2c.rc`：Windows 资源脚本
- `icon_64x64.ico`：Windows 可执行文件图标

## 构建依赖

通用依赖：

- C 编译器
- `pkg-config`
- `freetype2`

Windows 资源构建还需要：

- `windres`

## Windows 构建

在项目根目录执行：

```txt
builder\build_win.cmd
```

输出：

```txt
font2c.exe
```

说明：

- 如果存在，优先使用 `D:\msys64\mingw64\bin\gcc.exe`
- 否则回退到 `PATH` 里的 `gcc`
- 如果存在，优先使用 `D:\msys64\mingw64\bin\pkg-config.exe`
- 如果存在，优先使用 `D:\msys64\mingw64\bin\windres.exe`
- 如果 `builder/icon_64x64.ico` 存在，会自动嵌入到 exe 图标资源中

可选环境变量覆盖：

- `CC`
- `PKG_CONFIG`
- `WINDRES`

## POSIX 构建

在项目根目录执行：

```sh
sh builder/build_posix.sh
```

输出：

```txt
font2c
```

可选环境变量覆盖：

- `CC`
- `PKG_CONFIG`

## 当前定位

- 当前版本以 Windows 发布为主
- 源码结构已经为后续 macOS 和 Linux 编译预留空间
