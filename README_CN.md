# 本项目仍在调试中，可能仍然存在问题，如果不打算Debug，请暂时不要在生产环境使用

# Project Phosphor Base

[English](README.md) | **中文**

Windows PE 的启动指挥程序：一个可执行文件，用于在 PE 启动过程中运行启动配置脚本，并提供 PE 启动所需的各项操作。

[![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20PE-lightgrey.svg)](#运行环境与构建要求)

## 目录

- [概述](#概述)
- [状态](#状态)
- [运行环境与构建要求](#运行环境与构建要求)
- [安装](#安装)
- [用法](#用法)
- [构建](#构建)
- [仓库结构](#仓库结构)
- [不包含的内容](#不包含的内容)
- [贡献](#贡献)
- [致谢](#致谢)
- [许可](#许可)

## 概述

`phos.exe` 在 Windows PE 启动过程中运行脚本。脚本是结构化脚本（`.phos`，首行 `#!phos`）：
语句与块、函数、变量与错误处理，配套的库调用一次启动所需的各项操作。

启动配置可用的操作：

| 类别 | 操作 |
|---|---|
| 服务 | 启动、停止、查询、创建、删除 |
| 卷 | 列出卷；给没有盘符的卷分配盘符；弹出可移动介质与光驱 |
| 镜像 | 把 WIM 镜像挂载到目录、卸载；设置 Ramdriv 内存盘大小 |
| 回收站 | 按卷设置容量 |
| 文件 | 删除、建目录、读取、写入、追加、改某一行、硬链接、快捷方式、工作目录 |
| 属性 | 文件属性 |
| 注册表 | 读写与删除键值；加载/卸载离线注册表文件 |
| 进程 | 运行程序（可隐藏、可指定桌面、可等待）；调用 DLL 入口；等待；终止 |
| 环境变量 | 机器级、用户级与进程级环境 |
| 显示 | 显示模式、页面文件、写过滤缓存大小、NumLock |
| 启动画面 | 启动图、启动文本、壁纸、字体 |
| 外壳 | 交接给外壳并在其退出后重启；托盘图标与菜单；全局热键；键盘钩子；固定到任务栏；一个文件夹选项 |
| 文本 | 子串、位置、替换、字段拆分、按行处理、路径切片与文件名 |
| 时间、哈希与数字 | 当前时间、睡眠、文件摘要、带种子的随机范围与随机挑选 |

## 状态

- 版本：`0.2.0-beta2`
- 构建目标：x64 与 x86（Win32）。
- 验证方式：启动链与命令测试矩阵在 VMware 的 Windows PE 镜像中执行。该测试环境不在本仓库内。

## 运行环境与构建要求

- **运行**：Windows 10 或更高版本内核的 Windows PE 镜像。程序调用的 Windows API 在 PE 环境中存在；它不面向完整的桌面系统安装。
- **构建**：Visual Studio 2019 或 2022 的 C++ 工作负载（工具集 `v142`）与 Windows 10 SDK。无第三方库；除默认的之外，源码只链接 `ShLwApi` 与 `SetupAPI`。

## 安装

没有安装程序。把 `phos.exe` 复制到镜像的 `\Windows\System32\`。

同目录下名为 `init.phos` 的脚本是 `--init` 运行的文件（见下文）。

## 用法

### 作为启动入口

设置镜像的启动命令：

    HKLM\SYSTEM\Setup\CmdLine = phos.exe --init

`--init` 运行 `phos.exe` 所在目录的 `init.phos`。也可以指定其它文件：`phos.exe --init D:\boot\start.phos`。该命令行不含变量与脚本路径，因此不依赖镜像启动后的盘符；脚本结束后进程继续驻留，以维持外壳运行。

### 作为脚本宿主

    phos.exe run   hello.phos
    phos.exe check hello.phos
    phos.exe version

- `run <文件>`：执行脚本。
- `check <文件> [报告文件]`：只解析脚本并写出结论（默认 `<文件>.check.txt`），不执行。
- 已知动词但参数个数不对时，把用法写进程序旁边的 `phos-usage.txt` 并返回 2；程序不认识的首词会被点名拒绝，理由写在 `init-error.txt`。
- `version`：把版本写进程序旁边的 `phos-version.txt`。

不带参数运行时，`phos.exe` 显示一个对话框，内容是版本号与项目地址。

### 日志

PE 没有控制台。轨迹通道会逐条写出命令及其结果：

    set PHOS_DEBUG_LOG=\\.\COM1

目标可以是文件名或设备路径。该变量未设置时不写轨迹。

## 构建

    msbuild phosphor.vcxproj /p:Configuration=Release /p:Platform=x64
    msbuild phosphor.vcxproj /p:Configuration=Release /p:Platform=Win32

产物：`build\x64\phos.exe`、`build\Win32\phos.exe`。

源码以单个编译单元构建：`src/app/unity.cpp` 依次包含各区域的源文件，这个包含顺序就是程序的声明顺序。新增源文件时在该文件中加入对应的 include。

## 仓库结构

    src/app         入口、命令行、全局状态、资源、崩溃记录、unity 包含文件
    src/core        字符串、路径、编码、内存与磁盘查询
    src/engine      脚本的加载、启动图、轨迹通道
    src/commands    程序自己显示的对话框
    src/platform    文件、进程、注册表、卷、显示、托盘与窗口操作
    src/lang        结构化脚本语言：词法、语法、解释器、库函数

## 不包含的内容

- Windows PE 镜像。镜像另行组装。
- 测试环境。它运行虚拟机并包含镜像文件。
- 用户界面外壳。本仓库中的托盘图标与消息窗口是启动链所需的最小部分；界面外壳是另一个项目。
- 语言文档。写在本仓库的 Wiki 中（正在施工）。

## 贡献

缺陷报告与合并请求在本仓库的 issue 区处理。命令集合与脚本语法处于稳定化阶段而非冻结：允许新增；只有当现有行为本身是缺陷时，才会改变既有命令或脚本的运行结果。

## 致谢

- 致谢 [@Kazusa1085](https://github.com/Kazusa1085)：提供了自动化 PE 测试台。
- 致谢 [PECMD 2.8.8U](https://github.com/grub4dos/pecmd) 及其贡献者：为本项目提供了启发。

## 许可

GPL-3.0（见 [LICENSE](LICENSE)）。Copyright (C) 2026 Mortis0114 & Kazusa1085。
