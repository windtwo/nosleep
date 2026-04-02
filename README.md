# NoSleep - Windows 防休眠工具

一个轻量级的 Windows 防休眠工具，支持自定义时长，托盘图标显示倒计时。

## 特性

- 资源占用低（~300KB）
- 纯 Win32 API 实现，无外部依赖
- 自定义图标
- 托盘图标显示倒计时
- 双击托盘查看剩余时间
- 时间到自动退出

## 使用方法

### 直接运行
```bash
# 运行并输入时长
nosleep_pure.exe

# 或者通过命令行指定时长（分钟）
nosleep_pure.exe 60
```

### 功能
1. 运行后输入需要保持唤醒的分钟数（1-1440）
2. 程序会在托盘区域显示图标和倒计时
3. 双击托盘图标可查看剩余时间
4. 右键托盘图标可选择退出
5. 时间到后程序自动退出，系统恢复正常

## 编译方法

需要 MinGW-w64 编译器：

```bash
# 编译资源文件
windres -i nosleep.rc -o nosleep.res -O coff

# 编译程序
gcc -static -o nosleep_pure.exe nosleep_win32.c nosleep.res -mwindows -municode -lcomctl32
```

## 文件说明

- `nosleep_pure.exe` - 主程序
- `nosleep_win32.c` - 源代码
- `nosleep.rc` - 资源文件
- `nosleep.ico` - 应用图标
- `README.md` - 说明文档

## 系统要求

- Windows 7 或更高版本
- 无需任何运行时库

## 许可证

MIT License
