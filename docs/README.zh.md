# SlaveTats Menu

游戏内纹身编辑器，跑在 SKSE 上，界面用 SKSE Menu Framework 提供的 ImGui。
它直接操作 SlaveTatsNG 的 C++ 接口和 JContainers 账本，不经过 Papyrus，也不占用任何 esp 槽位。

## 依赖

| 组件 | 说明 |
|---|---|
| Skyrim SE/AE | 1.5.39 以上，已在 1.5.97 上运行 |
| [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) | 提供纹身数据的读写接口，必需 |
| SlaveTats（murfk 原版） | 可选：只有它的 MCM 需要它。所有纹身操作都走 SlaveTatsNG，读档重贴由本插件的 `ResyncAfterLoad` 原生完成；没有这个 ESP 就没有 MCM（原版的和 SlaveTatsNG 的都没有），这是唯一的代价。纹身包不受影响 |
| JContainers | SlaveTatsNG 的存储层，必需（缺失时插件仍会启动，但功能降级） |
| SKSE Menu Framework | 提供 ImGui 渲染和窗口，必需 |

## 安装

把 `SKSE/Plugins/` 下的内容放到 `Data/SKSE/Plugins/`。首次启动会在
`Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini` 生成默认配置。

默认按 `F10` 呼出悬浮窗，也可以从 SKSE Menu Framework 的 `SlaveTats` 菜单进入。

## 做什么

- 按部位 / 模组 / 关键字检索纹身，列表带虚拟滚动，上千条也不掉帧；左侧一列是两级浏览：
  先列模组，选中模组（或「全部模组」）后原地换成该模组的纹身列表，首行有 `< 返回` 回到
  模组列表
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

这条消息和上游绑定本存档缓存的先后**没有保证**，谁先谁后取决于加载顺序。所以读档时还会
记一个标记：这次读档后**第一次打开窗口**时再补建一次——到那时候玩家肯定已经在游戏里了。
如果清单本来就已经来自第 1 层就直接跳过，不会为了刷新而白白触发一次全量包扫描。

清单条目数不是固定值——每读一次档 SlaveTatsNG 都会重写那份缓存，同一份安装先后
出现过 1565 和 1542 条。

**左列是钻取式的。** 左侧只有一列，宽度固定等于原来「模组列表 + 纹身列表」两列的合计
（536 px，窗口放窄时先压这一列，最低 436 px，属性面板保持 380 px 的下限），所以进出一层
时右侧属性面板完全不动。选中一个模组（包括「全部模组」）后，这一列原地换成该模组的纹身
列表，首行左侧是 `< 返回` 按钮，右边是模组名和条目数；纹身视图里看不到已选中的模组条目，
所以再次点击同一条收起的那套逻辑已经没有了，返回只走返回键。返回时**不会**清掉已选中的
纹身和调好的颜色 / 透明度 / 槽位，去别的模组翻一翻再回来，之前的调整还在。再点一次已选中的
那条纹身则取消选中，右侧属性面板回到空态，不用退出列表。

**按钮宽高按当前字体实测，不写死像素。** 菜单框架的中文字号随字体/主题 mod 在 26 到 32
之间浮动，而 ImGui 的 FramePadding 是固定的 (10, 5)，所以固定 26 像素高的框会把汉字上下
各切掉一截，看起来就像文字没居中、显示不全。现在每个按钮的宽高都用 `CalcTextSize` 加上
内边距算出来，顶部右侧那两个库按钮按实际宽度右对齐，并排放不下的一对按钮第二个自动换到
下一行，而不是压进第一个里。

**能改颜色，是因为改了账本。** 上游的 `simple_add_tattoo` 在「这个纹身已经在身上」
时会直接返回、连颜色都不更新。所以改色走的是另一条路：拿到槽位的 JContainers 句柄，
改 `color` / `invertedAlpha`，置 `.SlaveTats.updated = 1`，再触发同步。这也是官方 MCM 的做法。

**维护 `.SlaveTatsMenu.known_targets`。** 上游的 `SlaveTatsOnLoad.OnPlayerLoadGame`
会遍历这个数组，对里面每个角色重新同步纹身——但前提是原版 SlaveTats 的 ESP 装着，
因为那个别名挂在它的 `SlaveTatsExtra` quest 上。这个数组平时由 MCM 在关闭时写入，
本插件绕开了 MCM，所以每次操作目标时都会自己把该角色登记进去。没有原版时这条链路
根本不存在，改由下面 `ResyncAfterLoad` 的原生补丁兜住。

**每个操作都回查。** 调完接口不算成功，插件会再读一次账本，确认槽位/颜色真的变了，
然后把真实结果写到界面底部的状态栏。三色状态：绿=成功、黄=延后、红=失败。

黄色那一档是必要的：上游的 `synchronize_tattoos` 在目标 3D 未加载时会**静默放弃**，
而且故意不清 `.SlaveTats.updated`。这时候账本已经写对了但画面不会变，报「成功」就是撒谎。

**`.SlaveTats.updated` 是个没人回收的待办。** 上游每次改账本都会把它置 1，只有真正跑到底的
同步才会清 0；但 SlaveTatsNG 里没有任何东西再去读它——没有维护循环。于是任何一次被跳过的
同步（目标 3D 还没加载、NiOverride 的锁被别人占着、加覆层节点失败）都会把这个角色永久卡住：
账本说有这条纹身，身上就是不显示，只有手动点一次「强制同步」才出来。这就是「菜单里能看到、
身上不显示、同步一下才有」的完整解释，而且它跟本插件无关——任何在角色 3D 还没就绪时给他上
纹身的 mod（比如在动画事件里打手印的那类）都会踩到同一个坑，所以在菜单里当场应用反而复现不出来。

现在 `[General] AutoFinishSync = 1`（默认开）会在窗口打开期间盯着当前目标和主角的这个标志：
它连续存在超过 1.5 秒（正常的同步在同一帧就清掉了）就说明那次同步根本没跑，插件替它补一次，
每 2 秒最多一次、每个角色最多 5 次，补上了会在状态栏说明。另外 `simple_add_tattoo` /
`simple_remove_tattoo` 的返回值语义以前读反了——上游这两个高层函数是**成功返回 true**，
和底层 `fail_t` 正好相反；改正之后，账本已经写进去而返回 false 就只剩一种解释：它内部那次
同步被丢了，于是状态栏会如实报「延后」而不是「成功」。

**读档是唯一一个没法靠「操作」兜住的时刻，因为根本没有操作。** 上游的 `SlaveTatsOnLoad`
会遍历 known targets 挨个调 `synchronize_tattoos()`，而 native 在 `.SlaveTats.updated` 为 0
时直接取消——读档后账本本来就没变，于是掉了覆层的身体不会被补回来。`[General]
ResyncAfterLoad = 1`（默认开）在读档后 10 秒和 30 秒各采一次：主角加上所有 3D 已加载的角色，
逐槽位比对账本和身上真实存在的覆层，只在「账本说有、这一格是空的」时才强制同步一次。

不按数量比、也不无差别重同步，都是刻意的：RaceMenu 的覆层写在同一批槽位里，数量比会把
「三条别人的 + 丢了一条自己的」算成没丢；而无差别重同步会先清空整段槽位再按账本重贴，
把别的覆层模组写进去的东西一起抹掉。覆层完好的角色一个都不碰。

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

## 槽位是和 RaceMenu 共用的

SlaveTats 和 RaceMenu 自带的纹身页写的是同一批 NiOverride 节点：每个部位固定若干槽位
（`skee64.ini` 的 `iNumOverlays`，默认 12/6/6/6），节点名就是 `Body [Ovl0]` 这种。两边
的纹身都实打实占掉一个槽位，用完为止。所以槽位面板两边一起算：别的覆层模组占着的槽位
用橙色标成 `[外部占用]`，既不会被写入也不会被清除，面板顶部还会显示还剩几个空槽。

某个部位的空槽归零之后，RaceMenu 的纹身页同样没有地方预览和落笔——「装了这个之后
RaceMenu 纹身菜单就不灵了」通常就是这个。清掉一个槽位，或者把 `iNumOverlays` 调大，
两边就都恢复。

## OBody 预览

装了 OBody 之后，盘上的 `femalebody_1.nif` 是**零号身形**：形状是 OBody 在运行时用 RaceMenu
morph 叠上去的。所以主插件的预览一直显示的是「没有身形的身体」。同包安装的
`SlaveTatsMenuOBody.dll` 解决这一点：它虽然是独立 DLL，但随插件一起安装，没有 OBody 时
只是安静地什么都不做，主插件的行为完全不变。

- 预设名来自 OBody NG 的插件 API（`GetPresetAssignedToActor`，要求 OBody 4.3.7+）；
- API 拿不到时（版本旧、预设文件被删）退回读 actor 身上 `OBody` 这个 morph key 的当前值，
  也就是游戏正在显示的那套数值；
- 预设的 BodySlide 滑块用 nif 旁边的 `.tri` 换算成顶点位移，因此 3BA / UBE / HIMBO 都能用，
  前提是 BodySlide 构建时勾了 **Build Morphs**；
- 骨骼缩放类滑块不参与（v1），预览固定用 weight 100 的 `_1` 网格；
- 没有分配预设的角色保持零号身形；生效时网格信息行会多出 `OBody: <预设名>`。

解析在打开菜单时做一次，目标 / 性别 / 预设变化时再做一次；`SliderPresets` 目录在第一次需要预设时
由网格加载线程扫描，变形也在同一个线程上完成，游戏线程与渲染线程都不会等它。`PreviewUseOBody = 0` 可关闭。

**为什么是独立 DLL**：OBody 的接口头是 GPL-3.0，skee64 的 morph 接口也只在装了 RaceMenu 的
机器上存在。主插件不链接、不包含其中任何一个，只在运行时 `GetModuleHandle` 找这个 DLL，
两边通过 `include/StmOBodyApi.h`（纯 C 函数表）通信。因此 `SlaveTatsMenuOBody.dll` 以
GPL-3.0 分发，主插件不受影响。

## 配置

`Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`

```ini
[General]
ToggleKey = 0x44        ; 呼出快捷键，DirectInput 扫描码
DefaultColor = 0x000000 ; 新纹身的默认颜色
DefaultAlpha = 1.0      ; 新纹身的默认透明度
Language = zh           ; zh / en，可在游戏内设置一键切换或手改此项
AutoFinishSync = 1      ; 窗口打开期间，替 SlaveTatsNG 补上它跳过的同步
ResyncAfterLoad = 1     ; 读档后，把身上掉了的覆层补回去

[UI]
WindowWidth = 1280.0
WindowHeight = 720.0
SlotRefreshMs = 250     ; 槽位快照刷新间隔

[Debug]
Probe = 0               ; 诊断探针，默认关闭
ProbeIntervalMs = 1000
ProbeArea = Body
ProbeSlots = 12
CallTrace = 0           ; 调用追踪，默认关闭（它会改写别的插件的只读表）
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

默认**关闭**（`[Debug] CallTrace=0`）。它把 SlaveTatsNG 接口表里 7 个「会改数据」的函数
换成转发垫片，记下**调用方模块 + 偏移**再转给原函数，于是日志里能看到：

```
[CALLTRACE] SlaveTatsMenu.dll+0x5FC56 -> synchronize_tattoos(actor=0x..., silent=false) = false
```

用来回答「这个纹身是谁动的手」。Papyrus 不走这张表，脚本侧的改动要看探针。

之所以默认关：它得用 `VirtualProtect` 把另一个模块的只读数据改成可写再替换函数指针。
一旦有第三个插件也去包同一张表，或者 SlaveTatsNG 自己更新了接口布局，还原就可能
悄悄失败——而这是诊断工具，不是功能，排查完就该关掉。

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
  obody/                主插件侧：动态加载附属 DLL、解析结果回传
tools/selftest/         离线自检程序
tools/obodytest/        附属插件的离线自检（.tri 解析 / 预设 XML / 变形量级）
src/obody/              附属插件本体：OBody API、预设解析、.tri 解析、变形
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
- OBody 预览有三处刻意的近似：骨骼缩放类滑块不参与变形（预设里靠它撑起来的部位会偏小）、
  固定用 weight 100 的 `_1` 网格（不按角色体重在 `_0`/`_1` 之间插值）、tri 形状必须和 NIF
  形状同名且顶点数对得上，否则宁可不变形也不乱拉。
- 3D 预览的墨色是「预览友好」而不是逐像素还原：贴图自带彩色而墨色仍是默认黑
  （`DefaultColor = 0x000000`）时，预览直接显示贴图原色，好让图案看得清；游戏里
  同一张图在默认黑墨下其实是全黑的。单色线稿照旧乘墨色，与游戏一致。
