---
name: slavetatsmenu-build-deploy
description: SlaveTatsMenu (天际 SKSE 插件) 的编译、打包与部署自动化流程。当用户要求编译、重新构建、打包、发布或将 SlaveTatsMenu 部署到 MO2 时自动触发。负责调用 build.cmd 编译、package-release.ps1 生成 dist/SlaveTatsMenu.zip，并同步分发至 E:/MO_Skyrim。
version: 1.0.0
---

# SlaveTatsMenu 编译、打包与部署流程

用于天际 SKSE 插件 `SlaveTatsMenu` 的完整构建、打包与部署交付。

## 何时使用

- 用户要求“编译 / 构建 SlaveTatsMenu”
- 用户要求“打包 / 打个包 / 发布 SlaveTatsMenu”
- 用户要求“部署到 MO2 / 部署到 E:/MO_Skyrim”
- 修改完代码后需要一条龙交付成品

## 核心路径与环境说明

- **项目源码根目录**：`d:\Projects\SlaveTatsMenu`
- **Shell 环境**：必须使用 PowerShell 7 (`pwsh`，路径为 `"D:\Program Files\PowerShell\7\pwsh.exe"`)
- **构建脚本**：`d:\Projects\SlaveTatsMenu\build.cmd`（底层调用 MSVC `vcvars64.bat` + CMake + Ninja）
- **打包脚本**：`d:\Projects\SlaveTatsMenu\tools\package-release.ps1`（生成 `dist\SlaveTatsMenu.zip`）
- **一键全流程脚本**：`d:\Projects\SlaveTatsMenu\tools\deploy-all.ps1`
- **目标分发位置**：
  - 本地发布包：`dist\SlaveTatsMenu.zip`
  - MO2 激活目录：`E:\MO_Skyrim\mods\SlaveTatsMenu\SKSE\Plugins\SlaveTatsMenu.dll`（热更新）
  - MO2 下载目录：`E:\MO_Skyrim\downloads\SlaveTatsMenu.zip`
  - 可选外部目录：仅在显式指定 `-DownloadPath <路径>` 时复制

---

## 一键执行流水线（推荐）

在项目根目录使用 `pwsh` 执行：

```powershell
& "D:\Program Files\PowerShell\7\pwsh.exe" -File .\tools\deploy-all.ps1
```

支持参数：
- `-SkipBuild`: 跳过编译阶段（仅打包与部署）
- `-SkipPackage`: 跳过打包阶段（仅编译与部署已有产物）
- `-MoPath <路径>`: 自定义 MO2 目录（默认为 `E:\MO_Skyrim`）
- `-DownloadPath <路径>`: 可选的外部下载目录（默认不分发外部目录）

---

## 分步手动执行流程

如果需要对某一步骤单独调试或执行，按以下顺序操作：

### 1. 编译 (Build)
在 `d:\Projects\SlaveTatsMenu` 目录下运行：
```powershell
cmd /c build.cmd
```
- **验证**：编译退出码必须为 0，并且更新生成了 `SKSE\Plugins\SlaveTatsMenu.dll`。

### 2. 打包 (Package)
在 `d:\Projects\SlaveTatsMenu` 目录下运行：
```powershell
& "D:\Program Files\PowerShell\7\pwsh.exe" -File .\tools\package-release.ps1
```
- **输出产物**：`dist\SlaveTatsMenu.zip`
- **内容检查**：包含 `SlaveTatsMenu.dll`、`SlaveTatsMenu.ini`、`LICENSE`、`THIRD_PARTY.md` 及 `LICENSES/*` 许可文件，包名固定为 `SlaveTatsMenu.zip`。

### 3. 部署 (Deploy)
使用 `pwsh` 执行文件分发：
```powershell
# 1. 部署到 MO2 激活 mod 目录（直接生效）
Copy-Item 'SKSE\Plugins\SlaveTatsMenu.dll' 'E:\MO_Skyrim\mods\SlaveTatsMenu\SKSE\Plugins\SlaveTatsMenu.dll' -Force

# 2. 部署到 MO2 下载目录（归档备用）
Copy-Item 'dist\SlaveTatsMenu.zip' 'E:\MO_Skyrim\downloads\SlaveTatsMenu.zip' -Force
```
