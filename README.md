# PAD Helper — 《智龙迷城》LSPosed 模块

让**没有对应角色 / 装扮**的账号，也能在关卡里显示该角色的珠子皮肤、听到该角色的 BGM、使用未获得的队伍徽章。

核心思路：**客户端在本地照常保存你的选择，但在上传存档时把皮肤 / BGM / 徽章替换成默认值**。
服务器全程只看到合法值，因此不会报错、不会封号；真实值只保存在本地，回推时再盖回去。

> 仅用于自己账号 / 自有设备上的逆向学习研究，请勿用于商业用途或破坏游戏平衡。

## 支持版本

| 项 | 值 |
|---|---|
| 游戏 | 日服《パズル＆ドラゴンズ》`jp.gungho.pad` |
| 版本 | **23.5.2**（versionCode `230503`），**其它版本不工作** |
| 系统 | Android 8.0+ / LSPosed（libxposed API 102） |

版本不符时：

- 游戏进程：**不安装任何 hook**，日志打一条 WARN 后直接返回；
- 设置页：显示「版本不匹配」提示，三个开关全部禁用，无法修改配置。

## 功能（v2.7.0）

| 功能 | 状态 | 说明 |
|---|---|---|
| 珠子皮肤解锁 | ✅ | 未拥有的皮肤可在编成里直接选，关卡内正常显示 |
| BGM 解锁 | ✅ | 未拥有的 BGM 可在编成里设置，关卡内正常播放 |
| 队伍徽章解锁 | ✅ | 未获得的徽章不再置灰，可自由选择 |
| 上传默认化 | ✅ | 上传 `save_decks` 时把每队皮肤 / BGM / 徽章替换为默认值 |
| 本地存档 | ✅ | 真实值按账号保存在本地，回推时自动恢复 |
| 多账号 | ✅ | 以 `files/data048.bin` 的哈希区分账号，互不串号 |
| 版本锁 | ✅ | 仅对 23.5.2 生效，升级游戏后自动失效 |

## 工作原理

```
游戏内选择皮肤 / BGM / 徽章
        │  客户端写入 savedata（真实值）
        ▼
[GOT hook] _API_REQ_SAVE_DECKS   ← 上传前
        │  ① 把真实值快照进本地存档
        │  ② 把请求副本里的皮肤 / BGM / 徽章改成默认值
        ▼
   服务器（只看到默认值，不报错）
        │  回推 save_decks 响应
        ▼
[GOT hook] _API_RES_SAVE_DECKS   ← 回推后
        │  服务器写的默认值 → 用本地真实值盖回
        ▼
   游戏内继续显示你的真实皮肤 / BGM / 徽章
```

- **不修改 `libpad.so` 的 `.text`**：所有 hook 都通过改写 `.got.plt` 槽位（`mprotect` 后写入函数指针）实现，绕开完整性检测。
- 本地存档与游戏存档完全独立，卸载模块不会破坏游戏数据；服务器上存的是默认值，重装游戏后需要重新选择一次。

### 关键 hook 点（v23.5.2）

| 符号 | 地址 | GOT 槽 | 作用 |
|---|---|---|---|
| `cAPPHTTP::_API_REQ_SAVE_DECKS` | `0x9988c0` | `0x10f3c98` | 上传前默认化 + 快照 |
| `cAPPHTTP::_API_RES_SAVE_DECKS` | `0x998db0` | `0x10f3ca0` | 回推后盖回本地值 |
| `cAPPHTTP::_padApiExec` | `0x9d0afc` | `0x10f3948` | 请求 trace（调试用） |
| `sSAVEDATA…::hasBadge` | `0xa36830` | `0x10ec728` | 徽章不再置灰 |

## 安装

1. 安装 `build-output/PADHelper-v2.7.0.apk`（或 Releases 里的同名文件）。
2. LSPosed 中启用 **PAD Helper**，作用域勾选 **パズドラ**（`jp.gungho.pad`）。
3. 重启游戏，等 10–20 秒（探针自动安装 hook，日志出现 `got: ON`）。

## 使用

1. 直接在编成里选择想要的皮肤 / BGM / 徽章 —— 不会报错。
2. 正常游玩即可；模块会在上传时自动默认化，回推时自动恢复。
3. 想关闭某项功能，在模块设置页关掉对应开关（或等待游戏重启）。

设置页三个开关分别对应皮肤、BGM、徽章；关闭后该字段按客户端原值上传（会出现「エラー番号:1」并回滚，属正常现象）。

## 日志与调试

模块数据目录：`/data/user/0/jp.gungho.pad/files/padhelper/`（需 root 查看）

| 文件 | 内容 |
|---|---|
| `probe.log` | 运行日志（`got: ON`、`def: ON`、`sync0:`、`fast: badge restored once` 等） |
| `skin.cfg` | 配置：`fast` / `hold` / `gotauto` / `pin` |
| `skins_<hash>.bin` | 按账号保存的本地存档（32 队 × 0x60 字节） |
| `cmd` | 调试命令入口（写入一行命令，探针读取后执行；同一字符串只执行一次） |

常用调试命令（写入 `cmd` 文件）：

```
gottrace      打开请求日志      gotoff    卸载 hook
defon         上传默认化开      defoff    上传默认化关
badgeon       徽章解锁开        badgeoff  徽章解锁关
gotauto 0     关闭自动装 hook   gotauto 1 打开
r 52f3bc 64   读存档            w16 52f41a 0x4e00  写存档
```

## 逆向要点速查

| 项 | 结论 |
|---|---|
| 存档基址 | `*(void**)(libpad_base + 0x10de320)` |
| 队伍记录 | `savedata + 0x52f3bc`，每队 `0x60` 字节，共 32 队 |
| 皮肤 | 记录 `+0x1c`（u16） |
| BGM1 / BGM2 | 记录 `+0x18` / `+0x1a`（u16） |
| 队伍徽章 | 记录 `+0x5f`（u8，存 `真实值 ^ ((i*15+0x61)&0xff)`） |
| 上传数组 | `q->0x20` 指向的堆副本，字段整体比存档 `+0x0c`（即皮肤在 `+0x28`） |
| 上传格式 | `decksb={"fmt":1,"decks":[[c1..c5, 徽章, c6, bgm1+bgm2*100000, 皮肤], …]}` |
| 完整性检测 | 改 `.text` 数秒内静默 `_exit()`；只能改数据 / GOT |

更详细的逆向记录见 [`docs/阶段性总结.md`](docs/阶段性总结.md)、[`docs/存档上传路径与本地化.md`](docs/存档上传路径与本地化.md)、[`docs/code-wiki/README.md`](docs/code-wiki/README.md) 和 [`docs/reverse/libpad-加壳分析报告.md`](docs/reverse/libpad-加壳分析报告.md)。

## 构建

需要 JDK 17+、Android SDK（`compileSdk 36`）、NDK（编译探针）。

```bash
# 1. 编译原生探针
NDK=$ANDROID_HOME/ndk/26.3.11579264/toolchains/llvm/prebuilt/darwin-x86_64/bin
$NDK/aarch64-linux-android26-clang -shared -fPIC -O2 -llog \
    -o app/src/main/assets/libpadhelper-probe.so \
    app/src/main/cpp/native_probe.c

# 2. 构建 APK（产物会复制到 build-output/）
cd program
./gradlew copyApkToProjectRoot
# → build-output/PADHelper-v2.7.0.apk
```

> 探针 `.so` 已随仓库提交（`app/src/main/assets/libpadhelper-probe.so`），只改 Java/Kotlin 时无需重编 NDK。
> `.so` 必须与 `native_probe.c` 同步更新，否则会出现「代码改了、行为没变」。

## 目录结构

```
program/
├── app/                         模块 APK（Kotlin + Compose + 原生探针）
│   ├── build.gradle             namespace / applicationId = com.niutie.padhelper
│   └── src/main/
│       ├── cpp/native_probe.c   原生探针：GOT hook、存档读写、本地存档（核心逻辑）
│       ├── java/com/niutie/padhelper/
│       │   ├── PadHelperModule.java   模块入口：版本判定 → 装 hook → 加载探针
│       │   ├── Target.java            版本锁（仅 23.5.2）
│       │   ├── ApkManifest.java       直接解析 APK 内的 versionName
│       │   ├── Config*.java/kt        remote preferences 配置同步
│       │   └── ui/                    Compose 设置页、图标、主题
│       ├── assets/libpadhelper-probe.so
│       └── resources/META-INF/xposed/  module.prop / java_init.list / scope.list
├── docs/                         技术文档
├── build-output/                 构建产物（不提交）
└── module/                       早期 Xposed 骨架（历史遗留，不参与当前构建）
```

## 已知限制

- **只支持 23.5.2**：游戏升级后需要重新逆向并更新偏移；模块会拒绝在其它版本上工作。
- **徽章纯本地**：服务器不保存徽章，回推时总是给默认值，因此徽章只存在本地存档里；首次使用（或清数据后）需要重新选一次。
- 启动后约 60 秒是「静默窗口」，期间修改徽章可能被登录回推覆盖，稍等再改即可。
- 服务器端保存的是默认皮肤 / BGM / 徽章；在另一台设备登录会看到默认值（本地存档不跨设备）。
- 模块不做账号鉴权之外的任何联网行为。

## 免责声明

本项目仅供逆向工程学习与自有账号研究使用。使用本模块可能违反游戏服务条款，由此产生的一切后果由使用者自行承担。
