# 暖声 WarmVoice

开源的 Windows 麦克风增强软件：AI 降噪、自动音量、智能音色优化、高频修复、防爆音。它直接装在你的麦克风上（Windows APO），游戏、Discord、微信里都不用改设置，也不依赖 Voicemeeter 或虚拟声卡。

> 当前阶段：**第 1 阶段（声音处理核心 + APO）**。处理核心已在 Linux 上编译，并用一个接口相同的 AI 替身通过 59 项测试；真正的 AI 库、APO 插件在 GitHub 的 Windows 机器上自动编译和测试。图形界面是下一阶段。

## 目录

| 路径 | 内容 |
|---|---|
| `core/` | 声音处理核心（C++17，跨平台，没有第三方依赖） |
| `apo/` | Windows APO 插件 `WarmVoiceApo.dll` 和安装工具 `wvinstall.exe` |
| `dfn/libdf-hook.patch` | 给 DeepFilterNet 加的补丁：让我们的频谱处理在它内部运行，共用一次分帧 |
| `tools/wvproc.cpp` | 离线处理 WAV 文件，用来试听和测速度 |
| `tests/` | 单元测试；`tests/mock/` 是接口和延迟都与真 AI 相同的测试替身 |
| `scripts/` | 生成测试噪音和录音、客观评估、端到端测试（`e2e.sh`） |
| `docs/design.md` | 算法复盘、实测数据、还没验证的部分 |

## 编译

**Windows（Visual Studio 2022 + CMake）**

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\wvtests.exe
```

**AI 降噪库**（需要 Rust）

```bash
git clone https://github.com/Rikorose/DeepFilterNet.git
cd DeepFilterNet && git apply ../warmvoice/dfn/libdf-hook.patch
cargo build --release -p deep_filter --features capi,default-model --lib
# 把 target/release/df.dll 改名为 warmvoice_ai.dll，放到 WarmVoiceApo.dll 旁边
```

没有这个 DLL 时不降噪，其他功能照常工作，并会报告原因。

推到 GitHub 后，`.github/workflows/build.yml` 会在 Windows 机器上：编译全部内容 → 编译 AI 库 → 用替身和真 AI 各跑一遍单元测试 → 用真 AI 跑端到端测试，并把指标表写进汇总页，把处理前后的音频打包成可以下载的文件。

**Linux（只编译核心和测试）**

```bash
cmake -S . -B build -G Ninja && ninja -C build && ./build/wvtests
```

## 安装到麦克风（管理员权限）

```bat
wvinstall list               :: 列出麦克风和它们的采样率
wvinstall install 0          :: 装到第 0 个麦克风（默认用 EFX 位置）
wvinstall status 0           :: 对着麦说话，看是否显示 RUNNING
wvinstall uninstall 0        :: 恢复原样
```

安装前会备份原来的注册表设置，卸载时原样恢复。如果某台电脑的驱动不支持 EFX，可以加 `--slot mfx` 或 `--slot sfx` 再试。

## 离线试听

```bash
wvproc in.wav out.wav --ai warmvoice_ai.dll  # 默认：温暖音色，全部功能开启
wvproc in.wav out.wav --tone clear --volume 18
wvproc in.wav out.wav --off texture,eq      # 关掉某几步
wvproc in.wav out.wav --bandlimit 16000     # 模拟"48k 格式但内容只到 8kHz"的无线耳机
```

会打印延迟、检测到的麦克风带宽、每一步占多少算力。

## 许可

MIT。第三方组件见 `THIRD_PARTY.md`。
