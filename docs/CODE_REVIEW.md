# SlaveTatsMenu 代码审查报告

审查日期：2026-10-18
审查范围：`CMakeLists.txt`、`build.cmd`、`src/`（44 个文件）、`include/`、`Interface/`、`fomod/`、`tools/`
基线：`62ea12a feat(ui): enlarge 3D mannequin preview on left short-click`（工作区含未提交的 i18n 改动）
验证方式：逐文件通读 + 交叉引用检查 + 翻译键全量比对 + 运行离线自检（`stm_selftest`，全部通过）

---

## 〇、修复状态（2026-10-18 第二轮）

按报告建议的顺序 P0-1 → P1-1/2 → P0-3 → P1-4/5 → P0-2 → P1-6/7 → P2 逐条落地，全部改动已通过编译（21 个目标）与离线自检。

第三轮补做 P2-3（NIF 异步解析）：`InitMeshWithSex` 不再同步读盘，改为「立即铺程序化占位 + 后台线程解析 + `RenderFrame` 里上传」，首次打开预览与每次切换性别不再掉帧。

| 编号 | 状态 | 落点 |
| --- | --- | --- |
| P0-1 | ✅ 已修 | `TattooOps.cpp` 新增 `IsExternalSlot()`，`RemoveSlot` / `ClearArea` / `ClearAll` 三条清空路径统一拒绝外部 overlay，并在结果里报告跳过数量 |
| P0-2 | ✅ 已修 | `CallTrace.cpp` 改为逐项包裹/逐项还原（不再整结构体写回），跳过空项，`VirtualProtect` 失败可重试；默认值翻转为 `false`（`Config.h` / `Config.cpp` / INI / 两份 README / 界面「恢复默认」） |
| P0-3 | ✅ 已修 | `UiMain.cpp` 新增 `OnGameThread()`，`DrawMain` / `DrawSettings` 在非游戏线程直接返回；删除 `ReadSlotAppearance()`，渲染线程不再直读 JContainers |
| P1-1 | ✅ 已修 | `Config.cpp` 去掉 `Save()` 后的 `return;`，翻译在任何情况下都会加载 |
| P1-2 | ✅ 已修 | 补齐 11 个缺失键，加上本轮新增的 12 个键（外部槽位 4 个、预设回查 2 个、`CloseParen`、预设确认框 3 个等），删除无用的 `$STM_SimplifiedChinese`；`package-release.ps1` 增加键覆盖率闸门（源码 279 键 = 两份文件 279 键，缺一即打包失败） |
| P1-3 | ✅ 已修 | 不再硬编码 `"2.0.0"`：改为「从已有 actor 的 `.SlaveTats.version` 学习上游真实值」，猜错时上游重建一次后自动纠正 |
| P1-4 | ✅ 已修 | `SlotSnapshot` 增加 `_inFlight` 重入闸（含 5 秒死锁逃生） |
| P1-5 | ✅ 已修 | `RenderFrame` 按 `D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT` 整表备份/还原 RTV，视口改两段式查询并全量还原；两条还原路径（含设备移除早退）同步修改 |
| P1-6 | ✅ 已修 | `SeedEditorFrom` 在条目默认值为空时回落到 `DefaultColor` / `DefaultAlpha` |
| P1-7 | ✅ 已修 | 新增 `src/ops/LedgerVerify.h`（`LedgerCheck` / `CheckSlot` / `VerifyApplied`），`Apply`、`ApplyPreset`、`RestyleApplied` 共用同一套回查；预设批量应用在**一次批量同步之后**逐条回查颜色/透明度，失配项计入结果详情 |
| P1-8 | ✅ 已修 | `RestyleApplied` 改为同时校验颜色与透明度（统一走 `VerifyApplied`） |
| P2-1 | ✅ 已修 | `PresetStore` 新增 `ResolvePresetFile()`：先按原样文件名找，找不到再回落到消毒名；`Load` / `Delete` / 新增的 `Exists` 共用 |
| P2-2 | ✅ 已修 | 同名保存与删除都走确认弹窗（`s_pendingPreset` + 单一 modal），不再一键覆盖/删除 |
| P2-3 | ✅ 已修 | `MannequinRenderer` 增加常驻解析线程：`InitMeshWithSex` 立即铺程序化占位网格并投递请求后返回，`RenderFrame` 每帧开头 `ApplyPendingMeshes()` 在渲染线程建 D3D buffer。带性别代际校验（换性别时旧批次丢弃）、`_meshSex[]` 记录每个槽位的网格属于哪个性别（避免换性别后某部位没有 NIF 而一直留着异性网格）、`Shutdown` 里 join 线程（不 detach，防止 DLL 卸载后线程写已释放内存） |
| P2-4 | ✅ 已修 | `GetAvailableLanguages` 加缓存，`Load()` 时失效 |
| P2-5 | ✅ 已修 | 语言码按 `[A-Za-z0-9_-]{2,16}` 校验，非法值不再拼进路径 |
| P2-6 | ⏳ 不改 | 相对进程工作目录是 MO2 虚拟文件系统下的**有意设计**（`Config::GetIniPath` 已注明），改成绝对路径反而会绕过 VFS |
| P2-7 | ✅ 已修 | `NifMeshExtractor` 增加 64 MB 上限、短读检测（`gcount`），头部结构体改 `memcpy` 避免对齐假设 |
| P2-8 | ⏳ 未做 | UV 偏移启发式需要进游戏实测顶点布局才能定论，不做盲改 |
| P2-9 | ✅ 已修 | 析构函数不再调用 `Shutdown()`，由 `atexit` 单独负责（静态析构阶段释放 D3D 设备是退出崩溃的经典来源） |
| P2-10 | ✅ 已修 | `CMakeLists.txt` 注入 `STM_VERSION_*`，`SKSEPlugin_Query` 与启动日志同源（此前 SKSE 侧报 `1`、日志写 `1.1.0`） |
| P2-11 | ✅ 已修 | 三栏宽度改为先压缩纹身列表（280→180）再兜底，检查器最小宽度下调到 380 |
| P2-12 | ✅ 已修 | 抽 `PumpResults()` 到窗口入口（`RenderWindow` / `RenderMenu` / `RenderSettings`），不再只在「纹身管理」页消费；`ResultBoard` 增加 64 条上限 |
| P2-13 | ⏳ 未做 | 自检里的 INI 解析用例仍测的是副本 |
| P3 `）` | ✅ 已修 | `PresetOps.cpp` 硬编码全角括号改为 `$STM_Ops_CloseParen` |
| P3 CMake | ✅ 已修 | 增加 `COMMONLIBSSE_DIR` 存在性检查（`FATAL_ERROR` + 可操作提示）；`ENABLE_SKYRIM_VR` 去掉 `FORCE` 并注明「VR 实际不支持、可用 `-DENABLE_SKYRIM_VR=OFF` 关掉」，默认保持 ON 以免整份 CommonLibSSE-NG 重编 |
| P3 build.cmd | ✅ 已修 | 增加 Windows SDK 兜底（`reg.exe` 被策略拦截时 vcvars 会静默缺 SDK 路径）、可选单目标参数 |
| P3 其余 | ⏳ 未做 | `/W4 /permissive-`、`include/imgui*.h` 死头文件、`col2`/`col3` 常量整理 |

---

## 一、项目结构评估

分层是清晰的，且边界基本守住了：

```
src/main.cpp       插件生命周期 / SKSE 消息 / 子系统装配
src/core/          Config、Translation、Tasks(ResultBoard)、Log、Text
src/bridge/        SlaveTatsNG 接口表、JContainers 反射、known_targets 契约
src/data/          目录库、槽位快照、目标状态、预设 IO
src/ops/           应用/改色/移除/清空/同步/预设
src/ui/            ImGui 窗口 + 热键
src/render/        离屏 3D 人偶（D3D11 + NIF 提取）
src/diag/          探针、调用追踪、段比对
```

值得肯定的设计（不要在这些地方"优化"掉）：

- **以账本为准的回查**：`Apply` / `Remove` / `ClearSlot` 都重新读账本再判定结果，`OpResult::Deferred`
  区分"账本已写、画面未同步"，这个三态是本项目最有价值的不变量。
- **池对象生命周期**：`PoolGuard` 覆盖了所有出口路径，`CleanPool` 与 `AddToPool` 成对出现。
- **纯数据层与引擎解耦**：`CacheParser` / `PresetIO` / `SegmentDiff` 不依赖 SKSE，可离线自检。
- **预设文件名消毒**：`SanitizePresetName` 处理了路径分隔符、`..`、Windows 设备名、UTF-8 截断，自检也覆盖了。

结构性问题：

1. `src/ui/UiMain.cpp` 已到 2233 行 / 102 KB，同时承担布局、预览、热键、设置、输入映射
   （`DirectInputToImGuiKey` 等两张 100+ 分支的表）。建议按 `ui/panels/`、`ui/keys/` 拆分，
   否则后续任何 UI 改动都在一个巨型 TU 里编译（当前 PCH 缓解了编译时间，但没缓解认知负担）。
2. `FindAppliedSlot` / `FindSlotOfHandle` 在 `TattooOps.cpp:70-102` 和 `PresetOps.cpp:78-107`
   各有一份实现，签名还不一致。应下沉到 `data/` 或 `bridge/`，否则两处修复会漂移。
3. `include/imgui*.h`、`imstb_*.h` 约 900 KB 声明为"仅作参考"，但项目实际通过
   `SKSEMenuFramework.h` 使用框架自带的 ImGui C-API。这些头没有任何 TU 包含，属于纯负担。

---

## 二、依赖与构建配置

| 项 | 状态 | 说明 |
|---|---|---|
| `COMMONLIBSSE_DIR` 默认值 | ⚠ | `CMakeLists.txt:15-18` 默认指向 `D:/Projects/OpenAnimationReplacer/extern/CommonLibSSE`——另一个项目的目录 |
| `add_subdirectory` 无存在性检查 | ⚠ | 依赖缺失时给出的是 CMake 内部报错，不是"请设置 COMMONLIBSSE_DIR" |
| `build.cmd` | ⚠ | `build.cmd:3,12-14` 硬编码 VS 路径、vcpkg 路径，以及**别的项目**的 `VCPKG_INSTALLED_DIR` |
| `ENABLE_SKYRIM_VR=ON` | ⚠ | `CMakeLists.txt:10` 与 README"VR is not supported"矛盾；实际靠 `SKSEPlugin_Query` 的 `1.5.39` 下限拒绝 VR |
| `BUILD_TESTS=OFF` + 自建 `stm_selftest` | ⚠ | 关掉了依赖的测试，却无条件构建并输出自己的测试可执行文件，没有 `option()` 开关 |
| 编译告警 | ⚠ | 未设 `/W4`、`/permissive-`、`/WX`；`[[nodiscard]]` 级别的返回值被忽略多处 |
| 输出目录 | ℹ | `RUNTIME_OUTPUT_DIRECTORY` 指向源码树 `SKSE/Plugins`，构建污染仓库（已被 `.gitignore` 兜住） |
| 打包脚本 | ✅ | `tools/package-release.ps1` 会校验 FOMOD 引用的每个路径、剔除 `.bak-`、版本从 CMakeLists 注入，质量高于平均水平 |

建议：

```cmake
# CMakeLists.txt:15-18
if(NOT DEFINED COMMONLIBSSE_DIR)
    message(FATAL_ERROR "请通过 -DCOMMONLIBSSE_DIR=<CommonLibSSE-NG checkout> 指定依赖路径")
endif()
if(NOT EXISTS "${COMMONLIBSSE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "COMMONLIBSSE_DIR 不是有效的 CommonLibSSE-NG 检出: ${COMMONLIBSSE_DIR}")
endif()
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)   # 与 README 保持一致
option(STM_BUILD_SELFTEST "Build the offline data-layer self-test" ON)
if(STM_BUILD_SELFTEST) ... endif()
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
```

---

## 三、P0 —— 严重（数据丢失 / 稳定性 / 未定义行为）

### P0-1 清空操作会删掉其他模组的 overlay，与 `Apply` 的保护策略自相矛盾

- 位置：`src/ops/TattooOps.cpp:320-353`（`RemoveSlot`）、`355-389`（`ClearArea`）、`391-428`（`ClearAll`）；
  UI 入口 `src/ui/UiMain.cpp:568-572`
- 现象：`Apply` 明确拒绝覆盖外部 overlay（`TattooOps.cpp:193-199`，注释写着 "refusing to overwrite to
  protect mod data"），但同一份代码里的清空路径只判断"槽位有人"，然后 `RemoveFromSlot(..., true)`
  忽略锁直接删除。槽位列表里外部槽位显示为橙色，但 `x` 按钮对它们同样可见（只有 `Edit` 被
  `if (!slot.external)` 挡住）。
- 风险：用户在"清空该部位 / 清空全部"时，会连带删除 RaceMenu、其他纹身模组写在同一个
  overlay 槽位里的数据，且不可恢复。
- 建议：把"外部槽位"当成一等公民，抽一个共用判定：

```cpp
// ops/TattooOps.cpp
bool IsExternalSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot)
{
    const auto handle = ng::GetAppliedInSlot(a_actor, a_area, a_slot);
    if (handle <= 0) return false;
    return jc::MapGetStr(jc::ObjId{ handle }, "name").empty();
}

// ClearArea / ClearAll / RemoveSlot 的循环内
if (ng::GetAppliedInSlot(actor, area, slot) > 0) {
    if (IsExternalSlot(actor, area, slot)) { ++skipped; continue; }   // 或按配置决定
    ...
}
```

并在 `DrawSlotList` 里对 `slot.external` 隐藏 `x`，或改成需要二次确认。`RemoveSlot` 也应加同样判定，
或给 `ops::RemoveSlot` 增加 `a_allowExternal` 参数由 UI 显式传入。

### P0-2 调用追踪默认开启，运行时改写另一个插件的只读接口表

- 位置：`src/diag/CallTrace.cpp:139-199`（`PatchTable` / `RestoreTable`），安装点
  `src/main.cpp:34,58-62`、`src/bridge/SlaveTatsNg.cpp:124-126`，默认值 `src/core/Config.h:53`
- 现象：`VirtualProtect` 把 SlaveTatsNG 的 `Addresses` 结构改成可写，替换 7 个函数指针，再改回原保护。
- 风险：
  1. 这是整个项目里唯一"写别的模块内存"的操作，默认开启（`CallTrace = 1`），用户无法预期。
  2. `RestoreTable` 在 `VirtualProtect` 失败时只打日志就 `return`（`CallTrace.cpp:186-189`），
     表保持被 patch 状态而 `g_shim.installed` 仍为 `true`，之后 `UninstallCallTrace` 永久失效。
  3. 还原时 `*g_shim.table = g_shim.original` 会整表覆盖，抹掉其他插件在我们之后做的 patch。
  4. `install` 与 `restore` 之间的窗口里，若有别的线程正通过该表调用，读到的是半更新的表
     （x64 对齐 8 字节写是原子的，所以不崩，但语义上是竞态）。
- 建议：
  - 默认改为 `false`，把它定位成排障开关（README 里也已经把它写成"诊断"能力）。
  - 逐条写入、逐条还原，只动我们自己替换过的那几个槽位，且写入前校验当前值仍是原值。
  - 还原失败时保持 `installed` 状态并允许重试，而不是静默放弃。
  - 安装/还原都在游戏主线程、且在没有其他调用进行中时完成（可借用 `kDataLoaded` 后的空闲帧）。

### P0-3 线程模型自相矛盾：渲染回调与 SKSE 任务队列被同时当作"安全线程"

- 位置：`src/core/Tasks.h:63-68` 注释（"the task queue is NOT guaranteed to be the same thread …
  in practice they differ"）vs `src/ui/UiMain.cpp:1228-1236`（"verify that the framework calls the
  render callback on the game's main thread"）；`docs/README.zh.md:77-81` 同时承认两点
- 现象：
  - `SlotSnapshot::Build`（任务队列线程）调用 `ng::GetAppliedInSlot` + `jc::MapGetStr`（`SlotSnapshot.cpp:49-101`）
  - `DrawSlotList` → `ops::ReadSlotAppearance`（渲染线程）也调用同样的 JContainers 函数（`UiMain.cpp:580`）
  - `DrawInspector` → `SlotSnapshot::Request` → `TargetState::Handle()` 每帧读引擎对象
  - 运行时检查只 `SKSE::log::warn` 一句，然后**继续**直接访问引擎
- 风险：如果两个前提不成立（框架换实现、或 SKSE 换任务调度），就是对 JContainers 的无锁并发访问，
  表现为随机崩溃或账本读到半成品——正是本项目最想避免的那类 bug。
- 建议：把前提变成硬约束而不是注释。

```cpp
// ui/UiMain.cpp 渲染回调开头
const bool onMainThread = (ThisThreadId() == LoadThreadId());
if (!onMainThread) {
    static bool s_logged = false;
    if (!s_logged) { s_logged = true; SKSE::log::error("UI: render callback is not on the game thread, disabling direct engine access"); }
    return;                       // 宁可不画，也不要跨线程访问引擎
}
```

同时把 `ReadSlotAppearance` 这类"渲染线程直接读账本"的调用改成只读快照（`SlotSnapshot` 已经提供了
`color/alpha`，`UiMain.cpp:578-580` 完全可以不重新读）。

---

## 四、P1 —— 高（功能错误 / 可见缺陷）

### P1-1 INI 缺失时翻译根本不会加载，界面显示原始键名

- 位置：`src/core/Config.cpp:29-34`
- 现象：

```cpp
const SI_Error rc = ini.LoadFile(pathStr.c_str());
if (rc < 0) {
    SKSE::log::warn(...);
    Save();          // 写出默认 INI
    return;          // ← 直接返回，下面的 Translation::Get().Load(language) 永不执行
}
```

- 触发场景：手动安装只拷了 DLL、INI 被误删、MO2 覆盖目录权限异常。
- 后果：`_activeDict` 与 `_fallbackDict` 都为空，`Tr()` 返回 `a_key` 本身，整个界面变成
  `$STM_Title` / `$STM_Area` / `$STM_ApplyUpdate`。
- 建议：`return` 之前补一次 `Translation::Get().Load(language);`，或把"读 INI"和"应用配置"拆成
  两步，保证 `language` 永远有默认值并一定走一遍 `Load`。

### P1-2 翻译文件缺 7 个键，部位单选行与快捷键弹窗显示原始键名

- 位置：`Interface/Translations/SlaveTatsMenu_ENGLISH.txt` / `_CHINESE.txt`
- 全量比对结果（源码 269 个键，两份文件各 263 个）：

| 缺失键 | 出现位置 | 用户可见后果 |
|---|---|---|
| `$STM_Area_All` | `UiMain.cpp:403` | 部位行第 1 个按钮显示 `$STM_Area_All` |
| `$STM_Area_Face` | `UiMain.cpp:405` | 同上 |
| `$STM_Area_Hands` | `UiMain.cpp:406` | 同上 |
| `$STM_Area_Feet` | `UiMain.cpp:407` | 同上 |
| `$STM_Key_F10Default` | `UiMain.cpp:1623` | 快捷键弹窗第一项 |
| `$STM_Key_Num0` | `UiMain.cpp:1642` | 快捷键弹窗 |
| `$STM_Key_Tilde` | `UiMain.cpp:1641` | 快捷键弹窗 |

（另有 `$STM_SimplifiedChinese` 已定义但源码未使用，可删。）
- 建议：补键；并在 CI/自检里加一条"源码 `$STM_*` 集合 ⊆ 翻译文件键集合"的断言，
  这类回归一旦发生就自动失败。

### P1-3 `Synchronize` 硬编码写入 `.SlaveTats.version = "2.0.0"`

- 位置：`src/bridge/SlaveTatsNg.cpp:192-197`
- 现象：为避免上游 `upgrade_tattoos()` 在版本号为空时重建账本（丢掉颜色/光泽），代码直接把
  SlaveTatsNG 的私有键写成固定字符串 `"2.0.0"`。
- 风险：上游一旦把内部版本推进（例如 2.1.0），这里写进去的旧版本号会让 `upgrade_tattoos()`
  **开始执行**，把颜色、glow、gloss 全部丢掉——正好是这段代码想避免的后果。
- 建议：不要在第三方插件的私有键里写死版本。可行方向：
  1. 把版本号做成 INI 项（`[Compat] SlaveTatsVersion = 2.0.0`），由用户/日志提示维护；
  2. 只在一个"上游确认为空"的会话里写一次，并在写入前把它当前读到的值记进日志（现在有日志，
     但没有可配置的退路）；
  3. 更稳的做法：改为在同步前后对比账本的 color 字段，检测到被重建就提示用户，而不是预防性篡改。

### P1-4 `SlotSnapshot::Request` 在快照未就绪时每帧重复投递构建任务

- 位置：`src/data/SlotSnapshot.cpp:26-47`
- 现象：

```cpp
const bool stale = !_valid || (now - _lastRequest) > interval;
if (!changed && !stale) return;
...
PostToGameThread([...] { SlotSnapshot::Get().Build(...); });
```

`_valid` 只有 `Build()` 完成才置 `true`。因此从"发起请求"到"构建完成"之间的每一帧，`!_valid`
都为真，都会再投递一个 `Build`。而 `Invalidate()` 在**每个操作之后**都会被调用
（`TattooOps.cpp:164,231,268,302,338,377,415,442`），所以每次应用/删除都会触发一轮重复投递。
- 风险：任务队列一旦积压（探针注释里自己写了 "the SKSE task queue can back up during a load"），
  这里会形成正反馈：越卡越投，越投越卡，每次 `Build` 还带 12 次 `GetAppliedInSlot` + 最多 48 次
  `MapGetStr`。
- 建议：加 in-flight 标记，只允许一个在途构建。

```cpp
bool _inFlight = false;   // 受 _mutex 保护
// Request(): if (_inFlight) return; ... _inFlight = true;
// Build() 末尾: { std::scoped_lock l(_mutex); _inFlight = false; ... }
```

### P1-5 `RenderFrame` 只备份/恢复 1 个渲染目标和 1 个视口

- 位置：`src/render/MannequinRenderer.cpp:1072-1078`、`1259-1265`
- 现象：`OMGetRenderTargets(1, &prevRTV, &prevDSV)` 只取第一个 RTV；`RSGetViewports(&numViewports=1, ...)`
  只取第一个 viewport。还原时 `OMSetRenderTargets(1, &prevRTV, prevDSV)` 也只绑回 1 个。
- 风险：游戏在 MRT 阶段（或 >1 viewport）调用我们的渲染回调时，回调结束后只剩 1 个 RTV 绑定，
  后续游戏绘制会写到错误的目标上，表现为画面花屏/黑屏，且极难归因。
- 建议：先查数量再按数量备份。

```cpp
UINT nRTV = D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL; // 或 8
ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
ID3D11DepthStencilView* dsv = nullptr;
_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
// ... 还原: OMSetRenderTargets(count, rtvs, dsv); 并释放非空指针
```

viewport 同理，`RSGetViewports(&count, nullptr)` 先取数量。

### P1-6 `DefaultColor` / `DefaultAlpha` 是死配置

- 位置：`src/core/Config.h:29-30`、`Config.cpp:36-38`、`UiMain.cpp:2016-2030`（设置页）、
  `README.md:51-52`（"starting colour for a new tattoo"）
- 现象：两个配置项被读取、被写回、在设置页可调、在文档里承诺"新纹身的起始颜色/不透明度"，
  但**没有任何地方读取它们去初始化编辑器**。`SeedEditorFrom`（`UiMain.cpp:216-238`）用的是
  `a_entry.defaultColor` / `a_entry.defaultAlpha`（纹身自带默认值）。
- 后果：用户把默认墨水色改成红色，点任何纹身仍然是黑色；配置项看起来"保存成功"。
- 建议：明确语义二选一——
  - 要么让 `SeedEditorFrom` 在纹身没有自定义色时回落到 `Config::defaultColor/defaultAlpha`；
  - 要么删掉这两个配置项与对应的 UI，别留一个骗人的开关。

### P1-7 预设应用不逐条回查账本，与文档承诺不符

- 位置：`src/ops/PresetOps.cpp:216-266`；文档 `docs/README.zh.md:96`（"每个操作都回查"）、
  `README.md`（"every operation is verified by reading the ledger back"）
- 现象：`ApplyPreset` 在 `AddAndGetTattoo` 返回 `kOk` 后直接 `done = true`，随后只统计
  `exactSlot`（用 `FindSlotOfHandle` 找落点），既不校验颜色/透明度是否写入成功，也不校验
  条目是否真的进了账本。相比之下 `TattooOps::Apply` 是逐项回读的。
- 后果：批量应用时"报告成功但部分纹身没上色/没落位"的情况无法被发现，正好违背项目最核心的不变量。
- 建议：抽一个 `VerifyApplied(actor, area, slot, color, alpha)`，`Apply` 与 `ApplyPreset` 共用；
  失败条目收集成 `std::vector<std::string>` 附在 `OpResult::detail` 里。

### P1-8 `RestyleApplied` 只校验颜色，不校验透明度，且失败路径可能已改账本

- 位置：`src/ops/TattooOps.cpp:109-131`
- 现象：写完 `color` 与 `invertedAlpha` 后只比较 `color`；`Synchronize` 之后再读回失败时直接
  `return false`，但账本其实已经被改过了，UI 报 "LedgerMismatch"（`TattooOps.cpp:167`）。
- 建议：同时校验 alpha（带 1e-3 容差），并把"写成功但读回失败"归到 `Deferred` 而不是 `Fail`。

---

## 五、P2 —— 中（健壮性 / 性能 / 可维护性）

### P2-1 预设列表用原始文件名，读写却用消毒后的名字

- 位置：`src/data/PresetStore.cpp:80-85`（`Refresh` 取 `path().stem()`）vs `94-95`（`Load` 先
  `SanitizePresetName`）、`126`（`Delete` 同）
- 现象：手工放进目录的 `我的 预设:v2.json` 会在列表里显示为 `我的 预设:v2`，点击加载时去找
  `我的 预设_v2.json`，永远失败；删除同样失败且只打一条 warn。
- 建议：`Refresh` 里同时保存"磁盘名"和"显示名"，`Load/Delete` 直接用磁盘名（它们本来就是从
  目录枚举来的，不需要再消毒）；只有 `Save`（名字来自用户输入）才消毒。

### P2-2 `PresetStore::Save` 静默覆盖同名预设

- 位置：`src/data/PresetStore.cpp:108-121`
- 建议：存在同名文件时在 UI 层二次确认（`ImGui::OpenPopup`），或在文件名后追加 ` (2)`。

### P2-3 首次打开预览时在渲染线程同步读取最多 4 个 NIF

- 位置：`src/render/MannequinRenderer.cpp:634-724` → `NifMeshExtractor::LoadFromNif`
- 现象：`InitMeshWithSex` 对 Body/Face/Hands/Feet 各走一遍候选路径，命中后**整文件读入内存**
  再解析。3BA/CBBE 的 `femalebody_1.nif` 常见 5–15 MB。
- 影响：第一次点开 3D 预览（以及每次切换性别）会出现明显掉帧。
- 建议：`Initialize` 后异步加载（任务队列 + 完成标志），或在 `kDataLoaded` 预热；至少把
  Body 与其余部位拆成两批。

### P2-4 设置页每帧扫描翻译目录

- 位置：`src/ui/UiMain.cpp:1691` → `Translation::GetAvailableLanguages()`（`Translation.cpp:89-123`）
- 现象：只要"界面与显示"折叠项展开，每帧都会 `directory_iterator` 遍历最多 3 个目录。
  `Data/Interface/Translations` 在整合包里常有上千个文件。
- 建议：结果缓存，仅在语言切换或手动刷新时重扫。

### P2-5 `Language` 配置值直接拼进文件路径

- 位置：`src/core/Translation.cpp:74-87`
- 现象：`"SlaveTatsMenu_" + NormalizeLanguage(a_lang) + ".txt"`，`NormalizeLanguage` 对未知值只做大写化。
  手工把 INI 改成 `Language = ..\..\x` 会让路径里出现 `..` 分量，从而读取父目录下的 `*.txt`。
- 影响：仅限本地配置文件，危害有限（只能读 txt、只进翻译表），但属于典型的输入未校验。
- 建议：语言码只接受 `[A-Za-z0-9_-]{2,16}`，否则回落到 `ENGLISH`；`GetAvailableLanguages` 插入的
  文件名 stem 也要过同一层白名单。

### P2-6 相对路径依赖进程工作目录

- 位置：`Config::IniPath()`（`Config.cpp:14-19`）、`PresetStore::Directory()`（`PresetStore.cpp:60-65`）、
  `TattooDatabase::BuildFromCacheFile()`（`TattooDatabase.cpp:157`）、`NifMeshExtractor` 的全部候选路径
- 现象：全部以 `std::filesystem::current_path()` 或裸相对路径为基准。
- 风险：从快捷方式/某些启动器启动时 CWD 可能不是游戏根目录，配置和预设会写到别处（且用户找不到）。
- 建议：统一用模块路径推导根目录（`REL::Module::get().path().parent_path()`，或 `GetModuleFileNameW`
  上溯 3 级，`Translation::GetSearchDirectories` 已经这么做了，可以复用）。

### P2-7 `NifMeshExtractor` 的健壮性缺口

- 位置：`src/render/NifMeshExtractor.cpp:256-270`
- 现象：`tellg()` 得到的文件大小直接 `std::vector<std::uint8_t> buffer(fileSize)` 分配并整读，
  没有上限；`file.read` 的返回值未检查；后续大量 `reinterpret_cast<const std::uint16_t*>(&buffer[pos])`
  都是非对齐读取（x64 能跑，但属于 UB，且 `-O2` 下编译器有权假设对齐）。
- 建议：加上限（如 64 MB）+ 检查 `gcount()`；用 `std::memcpy` 到局部变量替代非对齐解引用
  （`ReadU16/ReadU32/ReadF32` 三个小工具函数即可），顺带让代码可读性变好。

### P2-8 NIF UV 偏移启发式与 SE 顶点布局不符（需实测确认）

- 位置：`NifMeshExtractor.cpp:723-725`、`923-936`
- 现象：`uvOffset = (vertexStride >= 28) ? 16 : 12`。Skyrim SE 的顶点布局是
  `position(12) → UV(4, half2) → normal(4, byte4) → bitangent(4)`，UV 始终在 offset 12；
  stride ≥ 28 时读 offset 16 读到的是 normal 的 4 个字节。
- 影响：如果属实，NIF 网格的 UV 是随机值，贴图位置错乱（当前靠程序化兜底网格掩盖了）。
- 建议：先用一份已知 NIF 打日志对比 offset 12 / 16 解出的 UV 是否落在 [0,1]，再决定；
  更稳妥的是解析 `BSVertexData` 的 `vertexDesc` 位域（该参数已经传进来但被 `/*a_vertexDesc*/` 忽略了）。

### P2-9 退出路径重复 Shutdown

- 位置：`src/main.cpp:137-140`（`std::atexit` → `Shutdown()`）+ `MannequinRenderer.h:114`（析构函数也调 `Shutdown()`）
- 现象：两处都会释放 D3D 对象；`Shutdown` 内部置空所以不会二次释放，但 `atexit` 的执行时机在
  D3D 运行时可能已部分拆除之后，`Release()` 落在已卸载的运行时上属于 UB。
- 建议：只保留一处（推荐析构函数），`atexit` 里只 `diag::Stop()`。

### P2-10 版本号三处不一致

- `CMakeLists.txt:3` → `1.1.0`；`main.cpp:97` → `REL::Version{1,1,0,0}`；`main.cpp:109` → `a_info->version = 1`
- SKSE 日志与 MO2 显示的版本来自 `PluginInfo::version`，所以会显示 "version 1"。
- 建议：`a_info->version = 1` 保持不变（SKSE 约定），但把 `PluginVersion` 与 CMake 版本用同一个
  宏注入，避免三处手工同步。

### P2-11 窄窗口下三栏布局溢出

- 位置：`src/ui/UiMain.cpp:1264-1278`
- 现象：`col3` 下限 400，`col1` 固定 240，`col2` 固定 280 → 最窄需要 936 px；而
  `WindowWidth` 允许降到 800（`UiMain.cpp:1788`，且预设里有 1024）。
- 建议：`col3 = std::max(320.0f, available - col1 - col2 - gaps)`，并在 `available` 不足时
  自动收起面板 2（`sectionChosen = false`）。

### P2-12 结果信箱只在"纹身管理"页被消费

- 位置：`src/ui/UiMain.cpp:1191-1203`（`DrawStatusLine` 只在 `DrawMain` 里调用）
- 现象：停在"设置"页时，操作结果持续堆在 `ResultBoard::_pending` 里；窗口关闭时同理。
- 建议：在 `RenderWindow` 层统一 drain（无论当前 tab），或给 `_pending` 加上限（丢弃最旧）。

### P2-13 自检里的 INI 解析用例测的是复制品，不是产品代码

- 位置：`tools/selftest/main.cpp:228-243`
- 现象：`ParseIntRelaxed` 是测试里现写的一份 lambda，和 `SlaveTatsNg.cpp:53-64` 的
  `parseSlot` 是两份独立实现。真实解析逻辑回归时这条用例仍然会绿。
- 建议：把 `parseSlot` 提成 `data/` 里的纯函数（`ParseIniInt(std::string_view, int fallback)`），
  自检直接调用它。

---

## 六、P3 —— 低（整洁度 / 小瑕疵）

| 位置 | 问题 | 建议 |
|---|---|---|
| `PresetOps.cpp:168` | 详情串里硬编码全角右括号 `"）"`，英文界面会变成 `… from Player）` | 用成对的翻译键，或去掉括号 |
| `Config.cpp:112` | 保存用户设置时日志写 "wrote defaults" | 改成 "wrote …" |
| `Config.cpp:41` | `slotPolicy` 不做范围校验，INI 写 7 会变成非法枚举 | 与 `MannequinColor` 一样 clamp 到 [0,1] |
| `Config.cpp:43-48` | `windowAlpha/fontScale/previewSize` 不 clamp，`WindowAlpha=0` 会让窗口彻底不可见且无法在 UI 里救回 | 加载时 clamp 到文档区间 |
| `UiState.h:45` | `previewSize` 恒为 `{0,0}`，从未被赋值，`LoadTexture` 实际一直用框架默认尺寸 | 删除该字段，或从 `Config::previewSize` 赋值 |
| `SlaveTatsNg.cpp:35-78` | `LoadSlotLimits` 的 `g_slots.loaded` 无锁；靠 `main.cpp:67-69` 预热规避竞态 | 改成 `std::once_flag` |
| `SlaveTatsNg.cpp:221` | `a_template <= 0` 返回 `kUnavailable`，语义上应为 `kFailed` | 修正枚举，避免日志误导 |
| `UiMain.cpp:2110` | `AddSectionItem` 用当前语言字符串注册，切语言后框架菜单项仍是旧语言 | 可接受，但应在文档里注明 |
| `include/SKSEMenuFramework.h:158` | `(key + "/" + menu).c_str()` 把临时 `std::string` 的指针交给 `const char*` API | 若框架内部不拷贝就是悬垂指针；本地改成 `static std::string path = ...` 更安全 |
| `UiMain.cpp:1228-1236` | 线程不一致时只 warn，不改变行为 | 见 P0-3 |
| `docs/README.zh.md:105` | 文档称"顶点法线以 NIF 自带的为准"，但 `ComputeSmoothNormals` 每次都覆盖，且着色器根本不用法线 | 二选一：更新文档，或删掉无用的法线计算 |
| `include/imgui*.h` | 约 900 KB 死头文件 | 移到 `docs/reference/` 或删除 |

---

## 七、安全与内存安全小结

这个项目的"安全"主要是内存安全与跨模组数据完整性，不是网络攻击面。按风险排序：

1. **写别的模块的只读内存**（P0-2）——唯一真正危险的运行时行为，且默认开启。
2. **删除其他模组的数据**（P0-1）——不可恢复的数据丢失，用户很难意识到是我们干的。
3. **无锁并发访问 JContainers**（P0-3）——前提一旦不成立就是随机崩溃。
4. **未校验的配置值拼路径**（P2-5）——本地路径穿越，危害有限但应修。
5. **NIF 解析的非对齐解引用 + 无上限读取**（P2-7）——损坏的模组文件可以把游戏打崩，
   而 NIF 来自任意第三方模组，属于"不可信输入"。
6. 路径穿越防护（`SanitizePresetName`）与 Windows 设备名处理做得不错，这块不用动。

---

## 八、建议的修复顺序

1. **P0-1**（外部 overlay 保护）—— 改动小、收益最大，直接防数据丢失。
2. **P1-1 + P1-2**（翻译加载与缺键）—— 半天内可完成，用户第一眼就能看到差别。
3. **P0-3**（线程前提硬化）—— 把注释变成 `return`，成本极低。
4. **P1-4**（in-flight 标记）、**P1-5**（RTV/viewport 备份）—— 各十几行。
5. **P0-2**（CallTrace 默认关 + 逐条还原）—— 需要一点设计，但可以先把默认值改掉。
6. **P1-6 / P1-7**（死配置、预设回查）—— 涉及语义决策，需要产品层面的确认。
7. P2 系列按"用户可感知 → 内部质量"排序推进；P3 可以顺手清。

---

## 附：本次审查的验证记录

- `build/selftest/stm_selftest.exe`：**ALL CHECKS PASSED**（未提供 `slavetats_cache.json`，
  真实缓存那一组按设计跳过）
- 翻译键比对：源码 269 键 / EN 263 键 / ZH 263 键，缺失 7 键、多余 1 键（见 P1-2）
- 两份翻译文件均无重复键、无空值、无缺制表符的行
- `dist/SlaveTatsMenu.zip` 内容与 `ModuleConfig.xml` 的引用逐条对应，FOMOD 语言步骤可用
