# NoSleep - Windows 防休眠工具

NoSleep 是一个轻量级 Windows 防休眠工具，用于在指定时间内保持系统唤醒，并通过系统托盘显示倒计时。

## 特性

- 纯 Win32 API 实现，无外部运行时依赖
- 中文输入界面
- 常用时长快捷选择：30 分钟、1 小时、90 分钟、2 小时、3 小时、4 小时
- 支持自定义分钟数，范围 1-1440 分钟
- 托盘图标显示剩余时间
- 双击托盘图标查看剩余时间
- 右键托盘可推迟半小时或退出
- 时间到后自动退出，并恢复系统正常睡眠行为
- 新版透明电脑闪电图标

## 使用方法

### 直接运行

```bash
# 打开中文输入窗口，选择快捷时长或输入自定义分钟数
nosleep_pure.exe

# 通过命令行直接指定时长，单位为分钟
nosleep_pure.exe 60
```

### 托盘操作

1. 双击托盘图标：查看剩余时间。
2. 右键托盘图标：选择 `推迟半小时` 或 `退出`。
3. 选择 `推迟半小时` 会在当前剩余时间上增加 30 分钟。

## 编译方法

需要 MinGW-w64 编译器：

```bash
# 编译资源文件
windres -i nosleep.rc -o nosleep.res -O coff

# 编译程序
gcc -static -o nosleep_pure.exe nosleep_win32.c nosleep.res -mwindows -municode -lcomctl32 -lshell32
```

也可以直接运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

## 文件说明

- `nosleep_win32.c` - 主程序源代码
- `nosleep.rc` - Windows 资源文件
- `nosleep.ico` - 应用图标
- `build.ps1` - 构建脚本
- `README.md` - 项目说明文档

## 更新日志

### v1.2

- 新增中文输入窗口。
- 新增 6 个快捷时长按钮：30 分钟、1 小时、90 分钟、2 小时、3 小时、4 小时。
- 新增托盘右键菜单 `推迟半小时`。
- 中文化托盘菜单、剩余时间提示、完成提示和错误提示。
- 替换为新的透明电脑闪电图标。
- 修复关闭输入窗口后进程不退出的问题。
- 更新构建命令，补充 `shell32` 链接参数。

## 系统要求

- Windows 7 或更高版本
- 无需安装额外运行时

## 许可证

MIT License
