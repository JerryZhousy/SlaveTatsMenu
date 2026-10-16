# SlaveTats Menu

游戏内纹身编辑器，跑在 SKSE 上，界面用 SKSE Menu Framework 提供的 ImGui。
它直接操作 SlaveTatsNG 的 C++ 接口和 JContainers 账本，不经过 Papyrus，也不占用任何 esp 槽位。

## 依赖

| 组件 | 说明 |
|---|---|
| Skyrim SE/AE | 1.5.39 以上，已在 1.5.97 上运行 |
| [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) | 提供纹身数据的读写接口，必需 |
| JContainers | SlaveTatsNG 的存储层，必需（缺失时插件仍会启动，但功能降级） |
| SKSE Menu Framework | 提供 ImGui 渲染和窗口，必需 |

## 安装

把 `SKSE/Plugins/` 下的内容放到 `Data/SKSE/Plugins/`。首次启动会在
`Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini` 生成默认配置。

默认按 `F10` 呼出悬浮窗，也可以从 SKSE Menu Framework 的 `SlaveTats` 菜单进入。

## 做什么

- 按部位 / 模组 / 关键字检索纹身，列表带虚拟滚动，上千条也不掉帧；纹身列表这一列在左侧
  选中模组后才展开，再点一次同一条即可收起，腾出的宽度还给模组列表
- 给选中的目标（准星拾取的 NPC 或主角）应用、改色、调透明度
- 指定槽位或自动分配空槽
- 查看目标各部位的槽位占用情况，逐槽清除、按部位清空、全身重置
- 强制同步，重载贴图覆层
- **预设**：把当前目标的整套纹身存成命名预设，再一键套用到别的角色
- 诊断探针：查「纹身看起来消失了」这类账本与画面不一致的问题
- 调用追踪（CallTrace）：记录每一次数据变更是谁发起的

## 几个刻意的设计决定

**纹身清单以 SlaveTatsNG 的缓存为准。** 插件按信任度依次尝试三个来源，而不是
自己去解释散落的 JSON 文件：

1. SlaveTatsNG 的 C++ 接口 `query_available_tattoos`——游戏此刻真正认识的清单；
2. `Data/slavetats_cache.json`——SlaveTatsNG 自己落盘的同一份缓存；
3. 扫描 `Data/Textures/Actors/Character/slavetats`——最后兜底，里面可能混有游戏
   不认识的条目，点上去不会有事发生。

界面上会显示当前实际用的是哪一层。

第 1 层有个时序坑：SlaveTatsNG 只在**读档时**编译它的纹身缓存，所以 `kDataLoaded`
阶段这个接口还返回空。插件启动时先建一次清单（通常落到第 2 层），随后监听
`kPostLoadGame` / `kNewGame` 再重建一次，让第 1 层在缓存就绪后接管。

清单条目数不是固定值——每读一次档 SlaveTatsNG 都会重写那份缓存，同一份安装先后
出现过 1565 和 1542 条。

**面板 2 是折叠的。** 左侧模组列表里没有选中任何条目时，中间那列纹身列表整个不画，腾出的
宽度给左侧的模组列表（属性面板保持三列时的宽度不变）；选中一个模组（包括「全部模组」）它
才出现，再点一次同一条即取消选中、列表收起。默认状态少一列，模组名长、条目多的安装里左
列宽一点也更好读。

**按钮宽高按当前字体实测，不写死像素。** 菜单框架的中文字号随字体/主题 mod 在 26 到 32
之间浮动，而 ImGui 的 FramePadding 是固定的 (10, 5)，所以固定 26 像素高的框会把汉字上下
各切掉一截，看起来就像文字没居中、显示不全。现在每个按钮的宽高都用 `CalcTextSize` 加上
内边距算出来，顶部右侧那两个库按钮按实际宽度右对齐，并排放不下的一对按钮第二个自动换到
下一行，而不是压进第一个里。

**能改颜色，是因为改了账本。** 上游的 `simple_add_tattoo` 在「这个纹身已经在身上」
时会直接返回、连颜色都不更新。所以改色走的是另一条路：拿到槽位的 JContainers 句柄，
改 `color` / `invertedAlpha`，置 `.SlaveTats.updated = 1`，再触发同步。这也是官方 MCM 的做法。

**维护 `.SlaveTatsMenu.known_targets`。** 上游的 `SlaveTatsOnLoad.OnPlayerLoadGame`
会遍历这个数组，对里面每个角色重新同步纹身——这是读档后纹身不丢的唯一机制，
而它平时由 MCM 在关闭时写入。本插件绕开了 MCM，所以每次操作目标时都会自己把该角色
登记进去，否则随从身上的纹身读档后就会消失。

**每个操作都回查。** 调完接口不算成功，插件会再读一次账本，确认槽位/颜色真的变了，
然后把真实结果写到界面底部的状态栏。三色状态：绿=成功、黄=延后、红=失败。

黄色那一档是必要的：上游的 `synchronize_tattoos` 在目标 3D 未加载时会**静默放弃**，
而且故意不清 `.SlaveTats.updated`。这时候账本已经写对了但画面不会变，报「成功」就是撒谎。

**线程模型是明确的。** 实测确认菜单框架的渲染回调挂在游戏主线程上（日志会打出渲染
线程和加载线程，两个号应当一致），而 SKSE 的任务队列不保证在同一线程执行，所以跨线程
共享的结果信箱和槽位快照都带锁。界面只读快照，按 `SlotRefreshMs` 刷新。

**3D 预览的部位跟着上方「部位」一行走。** 预览里不再有第二套部位开关：上面选
「全部 / 身体 / 面部 / 双手 / 双脚」，下面画的就是那一个（「全部」回到全身）；
在「全部」下点列表里的纹身，则切到那条纹身自己的部位。四个部位各带一套相机状态，
切回来还是原来的角度。

**肉色人身的透明度恒定，靠深度预通道保证。** 上半身那些重叠网格（3BA/CBBE 的乳房
压在躯干上）会在同一像素混合两次，`1-(1-0.90)^2 = 0.99`，看起来就是「胸口比四肢
更实」。现在先跑一遍只写深度、不写颜色的预通道，正式绘制时深度只测不写，每个像素
只落最近的那一层。

**着色动态范围是刻意压平的。** 肉色模式的漫反射收在 `BodyColor × (0.80~1.07)`，高光
0.08、边缘光 0.15。换回原来的 `0.20~1.31` 再叠 0.30 高光与 0.40 边缘光，正对主光的
胸口会被推到近白、背光的大腿掉到 0.20 倍的深棕，同一层壳读起来却像透明度在变。想
更立体就调这三个数，别只加高光。

**顶点法线以 NIF 自带的为准。** 各身型的三角形绕序并不统一（实测某身型胸口区域
79% 的三角形绕序与自带法线相反），只按绕序累加会让半个身子被当成背面照亮，表现成
「一块发白一块发黑」。累加结果现在按自带法线定朝向，没有可用三角形时才退回自带法线。

## 预设

把当前目标的整套纹身存下来，再套用到别人身上——给一排随从配同一套纹身时不用一条条点。

- 在「预设」折叠面板里输入名字，点「把当前目标存为预设」。记录每个纹身的部位、槽位、
  模组、名称、颜色和透明度
- 换目标后再点预设旁的「应用」。默认会**先清空目标**（可以取消勾选），然后尽量把每个
  纹身放回**原来的槽位**；槽位被占用时会落到别处，结果里会如实说明「部分未落到原槽位」
- 应用同样逐条回查账本，失败会说明原因，并且会把目标登记进 `.SlaveTatsMenu.known_targets`
- 预设是普通 JSON，一个预设一个文件：`Data/SKSE/Plugins/SlaveTatsMenu/Presets/<名字>.json`，
  可以手改也可以拷给别人。文件名会做净化（路径分隔符、前导点都会被替换掉），中文名可用

## 配置

`Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`

```ini
[General]
ToggleKey = 0x44        ; 呼出快捷键，DirectInput 扫描码
DefaultColor = 0x000000 ; 新纹身的默认颜色
DefaultAlpha = 1.0      ; 新纹身的默认透明度
Language = zh           ; zh / en，可在游戏内设置一键切换或手改此项

[UI]
WindowWidth = 1280.0
WindowHeight = 720.0
SlotRefreshMs = 250     ; 槽位快照刷新间隔

[Debug]
Probe = 0               ; 诊断探针，默认关闭
ProbeIntervalMs = 1000
ProbeArea = Body
ProbeSlots = 12
CallTrace = 1           ; 调用追踪，默认开启
```

## 诊断探针

打开 `Probe` 后（或在界面的「调试探针」折叠项里手动启动），插件会周期性对比两侧状态：

- 账本侧：SlaveTats 的 JFormDB 记录，哪些槽位被占用
- 画面侧：3D 场景树里 `Body [OvlN]` 这类 overlay 节点的实际内容（RTTI 类型、alpha、diffuse 贴图路径）

只在发生变化时写日志，并附一行判定，命名最可能的原因：

- `3D not loaded`：3D 未加载时同步会静默放弃，而且不清 `.updated`
- `.updated` 卡住：同步被跳过，账本和画面无法自愈
- `.SlaveTats.version` 为空：下次同步会清空账本、从 3D 节点反推重建，丢失颜色/发光信息
- 外部槽位：该槽被别的 mod 的 overlay 占用，SlaveTats 会跳过它

判定只在**结论变化**时打印，所以日志里每一行都有信息量。

一个实现细节：`<Area> [OvlN]` 挂的那个对象**不保证是 `NiNode`**，实测是 `BSTriShape`
本身。早期版本假设 NiNode，结果 24 个节点全被判成 `not-a-node`，画面侧等于瞎的。

## 调用追踪（CallTrace）

默认开启（`[Debug] CallTrace=1`）。它把 SlaveTatsNG 接口表里 7 个「会改数据」的函数
换成转发垫片，记下**调用方模块 + 偏移**再转给原函数，于是日志里能看到：

```
[CALLTRACE] SlaveTatsMenu.dll+0x5FC56 -> synchronize_tattoos(actor=0x..., silent=false) = false
```

用来回答「这个纹身是谁动的手」。Papyrus 不走这张表，脚本侧的改动要看探针。

注意它改的是 SlaveTatsNG 广播给**所有**插件的同一张表；如果将来有别的插件也 hook
同一张表，两边会互相覆盖。

日志位置：`Documents/My Games/Skyrim Special Edition/SKSE/SlaveTatsMenu.log`

## 构建

需要 Visual Studio 2022、CMake 3.22+、vcpkg（`x64-windows-static-md`）。

```cmd
build.cmd
```

`CMakeLists.txt` 里 CommonLibSSE-NG 的路径默认指向本机 checkout，可以用
`-DCOMMONLIBSSE_DIR=<path>` 覆盖。产物输出到 `SKSE/Plugins/SlaveTatsMenu.dll`。

同一份工程还会构建一个**离线自检程序**（`build/selftest/stm_selftest.exe`）：它不链接
CommonLibSSE、不需要游戏，直接对真实的 `slavetats_cache.json` 和合成的纹身包文件
验证数据层——部位规范化、缓存解析、过滤、排序、探针差异比较、预设往返与名称净化。

```cmd
build\selftest\stm_selftest.exe "E:\MO_Skyrim\overwrite\slavetats_cache.json"
```

## 目录结构

```
src/
  main.cpp              插件生命周期、SKSE 消息、子系统装配
  core/                 日志、配置、主线程任务与结果回传、UTF-8 转换
  bridge/               SlaveTatsNG / JContainers 的强类型封装，以及 known_targets 契约
  data/                 纹身清单、槽位快照、目标角色状态、预设读写
  ops/                  应用 / 改色 / 移除 / 清空 / 同步 / 预设，全部返回真实结果
  ui/                   ImGui 界面与热键
  diag/                 诊断探针、调用追踪、可测的差异比较
tools/selftest/         离线自检程序
include/
  SlaveTatsNG_Interface.h   上游官方接口头（原样使用，不要改）
  JContainers/              JContainers 接口头
  SKSEMenuFramework.h       SKSE Menu Framework 桥接头
```

## 已知边界

- 纹身清单以前两层为准，因此不会出现游戏本身不认识的条目（例如某些包把 JSON
  放错了目录）。代价是条目数可能少于纯扫 JSON 的结果。
- `slavetats_cache.json` 里 area 的 map key 大小写并不统一（`BODY` / `face` /
  `Feet` / `Hands`），所以插件只认每条记录自己的 `area` 字段，不看 map key。
- 缓存里的 `color` / `invertedAlpha` 是可选字段，缺失时分别按 0 和 alpha=1 处理。
- 清单条目数随读档变化，所以自检只断言结构与内部一致性，不比对固定数字。
- JContainers 的反射函数是按 `RE::BSFixedString` 参数编译的（它有非平凡拷贝构造，
  MSVC x64 按隐藏引用传参）。用 `const char*` 声明会传错 ABI，实测直接把
  `"SlaveTatsMenu-DB"` 的前 8 个字节当成指针追，导致访问违例崩溃。
- JContainers 不可用时整体降级：清单退回 JSON 扫描、改色与预设读写不可用，
  但插件仍会启动，不会 abort。
- 3D 预览的墨色是「预览友好」而不是逐像素还原：贴图自带彩色而墨色仍是默认黑
  （`DefaultColor = 0x000000`）时，预览直接显示贴图原色，好让图案看得清；游戏里
  同一张图在默认黑墨下其实是全黑的。单色线稿照旧乘墨色，与游戏一致。
