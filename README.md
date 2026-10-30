# PAD Helper — 《智龙迷城》LSPosed 模块

让**没有对应角色 **的账号，也能在关卡里显示该角色的珠子皮肤、听到该角色的 BGM、使用未获得的队伍徽章。

核心思路：**客户端在本地照常保存你的选择，但在上传存档时把皮肤 / BGM / 徽章替换成默认值**。
服务器全程只看到合法值，因此不会报错、不会封号；真实值只保存在本地，回推时再盖回去。

> 仅用于自己账号 / 自有设备上的逆向学习研究，请勿用于商业用途或破坏游戏平衡。

## 支持版本

| 项 | 值 |
|---|---|
| 游戏 | 日服《パズル＆ドラゴンズ》`jp.gungho.pad` |
| 版本 | **23.6.0**（versionCode `230600`），**其它版本不工作** |
| 模块 | 2.9.6（versionCode `96`） |
| 系统 | Android 8.0+（minSdk 26）/ LSPosed（libxposed API 102） |

版本不符时：

- 游戏进程：**不安装任何 hook**，日志打一条 WARN 后直接返回；
- 设置页：显示「版本不匹配」提示，四个开关全部禁用，无法修改配置。

## 功能（v2.9.6）

| 功能 | 状态 | 说明 |
|---|---|---|
| 珠子皮肤解锁 | ✅ | 未拥有的皮肤可在编成里直接选，关卡内正常显示 |
| BGM 解锁 | ✅ | 未拥有的 BGM 可在编成里设置，关卡内正常播放 |
| 队伍徽章解锁 | ✅ | 未获得的徽章不再置灰，可自由选择 |
| 限时本防退 | ✅ | 只清「计时暂停」标志位，让倒计时继续走；**不干预游戏阶段** |
| └ 重新计时 | 可选 | 子开关，默认关。关：按墙钟自然流逝，中途退出多久就少多少，耗尽判负；开：杀进程重进后倒计时从满值重来 |
| 上传默认化 | ✅ | 上传队伍配置时把每队皮肤 / BGM / 徽章替换为默认值 |
| 本地存档 | ✅ | 真实值按账号保存在本地，回推时自动恢复 |
| 多账号 | ✅ | 以 `files/data048.bin` 的哈希区分账号，互不串号 |
| 版本锁 | ✅ | 仅对 23.6.0 生效，升级游戏后自动失效 |

## 工作原理

本模块不修改游戏数据本身，而是改「客户端发给服务器的内容」：

1. 你在编成里选择的皮肤 / BGM / 徽章，客户端照常保存在本机；
2. 模块在你上传队伍配置前，先把要发送的内容替换成默认值（服务器只看到默认值，因此不会报错）；
3. 服务器回推配置后，模块再用本机保存的真实值覆盖回去，游戏里继续显示你的选择。

真实值只保存在本地，不上传、不联网；服务器端始终是默认值。

实现上分两层：Java 层负责版本判定、LSPosed 入口、加载原生探针与配置同步；
原生探针负责拦截游戏的网络请求与徽章查询，并读写游戏内存。

> 升级模块后 LSPosed 里的作用域**可能被重置**，装完记得回去确认一眼。

## 使用

1. 直接在编成里选择想要的皮肤 / BGM / 徽章 。
2. 正常游玩即可；模块会在上传时自动默认化，回推时自动恢复。
3. 想关闭某项功能，在模块设置页关掉对应开关（或等待游戏重启）。

## 日志与调试

模块数据目录：`/data/user/0/jp.gungho.pad/files/padhelper/`（需 root 查看）

| 文件 | 内容 |
|---|---|
| `probe.log` | 运行日志（`got: ON`、`def: ON`、`sync0:`、`fast: badge restored once` 等） |
| `probe.status` | 心跳（pid / 时间戳 / 四个开关状态），每 10 秒刷新 |
| `skin.cfg` | 配置：`cfgver` / `skin` / `bgm` / `badge` / `timerfix` / `retimer` / `fast` / `hold` / `gotauto` / `pin` |
| `skins_<hash>.bin` | 按账号保存的本地存档 |
| `svd.bin` | 最近一次读到的存档快照（调试用） |
| `cmd` | 调试命令入口（写入一行命令，探针读取后执行；同一字符串只执行一次） |

### 发命令

`cmd` 只从**游戏私有目录**读取（外部存储不生效），需要 root：

```bash
adb shell su -c 'printf "svd\n" > /data/user/0/jp.gungho.pad/files/padhelper/cmd'
```

探针只读第一行 —— **一次一条**，发下一条前等 2～3 秒。

### 常用命令

```
svd           打印存档条目表统计       deckinfo   打印 32 队记录（皮肤/BGM/徽章）
svdpath       打印队伍记录原始字节      svdbits    打印两段解锁位图
cards         卡牌总数与数组           svdnames   导出皮肤名称表
r <off> <len> 读内存（hex）            lr <off> <len> 读代码段
dumppad       把游戏原生库各段 dump 到文件（分析用）
gottrace      打开请求日志             gotoff     卸载 hook
defon/defoff  上传默认化开/关          badgeon/badgeoff  徽章解锁开/关
gotauto 0/1   自动装 hook 开/关        fast/fastoff     快速回盖开/关
unpopup       手动撤掉强制退出弹窗（仅限人在场取证，不会自动触发）
```

> `unpopup` 会直接改 `impl` 的阶段字段并调 phase setter。**只在弹窗已经出现、你想手工救回来时用**；
> 不要在正常流程里反复调用 —— 绕过游戏自己的判定会让状态机对不上。

## 版本升级

游戏升级后本模块会整体失效（偏移全错），需要重新适配。

## 目录结构

```
program/
├── app/                         模块 APK（Kotlin + Compose + 原生探针）
│   ├── build.gradle             namespace / applicationId = com.niutie.padhelper
│   └── src/main/
│       ├── cpp/native_probe.c   原生探针（核心逻辑）
│       ├── java/com/niutie/padhelper/
│       │   ├── PadHelperModule.java   模块入口：版本判定 → 装 hook → 加载探针
│       │   ├── Target.java            版本锁（仅 23.6.0）
│       │   ├── ApkManifest.java       直接解析 APK 内的 versionName
│       │   ├── Config*.java/kt        remote preferences 配置同步
│       │   └── ui/                    Compose 设置页、图标、主题
│       ├── assets/libpadhelper-probe.so
│       └── resources/META-INF/xposed/  module.prop / java_init.list / scope.list
├── tools/libpad_offsets/        版本升级用的偏移翻译工具
│   ├── fde_align.py             函数长度序列对齐 / 偏移翻译
│   └── offsets.txt              偏移登记表
├── build-output/                构建产物（不提交）
└── module/                      早期 Xposed 骨架（历史遗留，settings.gradle 不引用）
```

## 已知限制

- **只支持 23.6.0**：游戏升级后需要重新逆向并更新偏移；模块会拒绝在其它版本上工作。
- 服务器端保存的是默认皮肤 / BGM / 徽章；在另一台设备登录会看到默认值（本地存档不跨设备）。
- 模块不做账号鉴权之外的任何联网行为。

## 免责声明

本项目仅供逆向工程学习与自有账号研究使用。使用本模块可能违反游戏服务条款，由此产生的一切后果由使用者自行承担。
