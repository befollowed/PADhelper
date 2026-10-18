# PAD Helper — 《智龙迷城》LSPosed 模块

让**没有对应角色**的账号也能在关卡里显示该角色的皮肤、听到该角色的 BGM。
核心原则：**只改内存，存档永远保持服务器认可的合法值**，因此联机同步不报错、不被回滚。

> 仅用于自己账号 / 自有设备上的研究。

## 功能现状（v1.0.0）

| 功能 | 状态 | 说明 |
|---|---|---|
| 皮肤影子配置（按队伍） | ✅ 可用 | 编成里选好目标皮肤后，进关卡自动生效，15 秒后自动回落 |
| 皮肤 / BGM 列表解锁 | ✅ 自动 | 存档就绪 +3 秒后自动把 `sSKINDATA` 的 378 条准入位与两段拥有位图打满 |
| 列表 New 角标 | ✅ 可用 | 只有「强制解锁」的条目标闪金色 New，原本就拥有的不标 |
| BGM 改道（本地音频替换） | ✅ 保留 | `bgm.dir` 指向的本地文件按原名替换 |
| BGM 按队伍生效 | ⏳ 待做 | 槽位尚未定位（皮肤已定位） |

## 原理：用「时序差」绕过服务端校验

```
编成里选皮肤 → 客户端写入 savedata+0x52f3d8+(N-1)*0x60
      │  探针 50ms 轮询：记下这是「目标值」，立刻改回「合法值」
      ▼
进「选择关卡」→ 客户端上传队伍数据（合法值）→ 服务器 ✅
      │
      ▼
点「挑戦する」→ 体力扣减（savedata+0x52b1d8 变小）
      │  探针发现体力变小 → 把槽位切成目标值，保持 15 秒
      ▼
关卡内显示目标皮肤；服务器全程只见过合法值
```

**为什么用体力当触发器**：游戏 UI 是引擎自绘的 OpenGL 画面，没有 Android 控件可 hook；改 `.text`
会被完整性检测（实测数秒内静默退出）。而「体力变小」只可能发生在玩家确认进关那一刻——自然恢复只会让
数字变大，所以零 hook、零坐标、不会误触。

## 安装与启用

1. 安装 `app/build-output/PADHelper-v1.0.0.apk`（模块图标沿用 PAD 本体图标）。
2. LSPosed 里启用 **PAD Helper**，作用域勾选 **パズドラ**（`jp.gungho.pad`）。
3. 重启游戏，等十几秒（存档一加载完、稳定 3 秒后，探针自动解锁皮肤 / BGM 列表）。

## 使用

1. 进编成，把队伍皮肤选成目标款 —— 选完外观会立刻跳回原来的，这是正常的（目标值已被记住）。
2. 进「选择关卡」，不会报错。
3. 点「挑戦する」—— 体力扣减瞬间自动切换，关卡内即为目标皮肤。

日志：`/data/user/0/jp.gungho.pad/files/padhelper/probe.log`
影子配置：同目录 `skin.cfg`（`mode legal|target`、`new 0|1|2`）

## 构建

```bash
# 1. 编译探针（Android NDK，按你的实际路径改）
NDK=<你的 SDK>/ndk/26.3.11579264/toolchains/llvm/prebuilt/darwin-x86_64/bin
$NDK/aarch64-linux-android26-clang -shared -fPIC -O2 \
    -o app/src/main/assets/libpadhelper-probe.so app/src/main/cpp/native_probe.c -llog

# 2. 打包（aapt2 + d8 + apksigner，不依赖 Gradle）
SDK=<你的 SDK> bash app/build.sh
# 产物：app/build-output/PADHelper-v1.0.0.apk
```

## 关键事实（逆向自 jp.gungho.pad 23.5.2）

| 项 | 结论 |
|---|---|
| 加壳 | `libpad.so` 磁盘上的 `.text` 只有 15,881 非零字节；运行时内存里已是明文（14 MB 可执行代码） |
| 符号 | 未 strip，29,032 个动态符号、12,702 个函数 |
| 皮肤槽位 | `savedata+0x52f3d8`，每队 +0x60，共 10 队；队伍 2 = 0x52f438 |
| 体力 | `savedata+0x52b1d8`（u16 当前）/ `+0x52b1da`（上限） |
| 皮肤表 | `sSKINDATA`：`+0xc773c4` 条目数、`+0xc773c8` 数组指针，每条 0x34 字节 |
| 拥有位图 | `savedata+0x5312f0`（正 id）/ `+0x5311f0`（活动 id），各 0x100 字节 |
| 校验时机 | 进入「选择关卡」画面时上传队伍数据，服务器权威校验；显示要等关卡加载时才读字段 |
| 完整性检测 | 改 `.text` 数秒内静默 `_exit()`，无崩溃日志；**只能改数据** |

## 目录

```
app/src/main/cpp/native_probe.c      探针（注入游戏进程，读写 savedata）
app/src/main/java/com/padhelper/     模块入口、探针加载
app/src/main/assets/                 探针 .so 与默认配置
docs/阶段性总结.md                    详细技术记录
docs/reverse/libpad-加壳分析报告.md   libpad.so 加壳静态逆向报告
tools/                               逆向辅助工具（源码，不含二进制）
release/                             成品 APK 说明
```

> `work/`（dump、符号、截图、原始 APK）体积大且含版权资源，不随源码仓库发布。
