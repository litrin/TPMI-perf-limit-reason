# plr-tpmi

[English](#english) | [中文](#中文)

## English

A Linux Intel TPMI utility that reads and decodes **Performance Limit Reasons
(PLR)**. It supports platforms with TPMI PLR capability, including Granite
Rapids (GNR), Granite Rapids-D, Sierra Forest / Clearwater Forest, and Diamond
Rapids.

### Features

- **No third-party dependencies**: uses only libc and sysfs; `make` produces a
  single statically linked executable by default.
- **No kernel PLR driver required**: accesses MMIO directly through PCI
  DVSEC/VSEC, PFS, and TPMI ID `0x0C`; `intel_plr_tpmi` and debugfs are not
  required.
- Reports both **die/domain-level** and **per-core (punit module)-level**
  limit reasons, decoded into kernel-compatible symbolic names (coarse bits
  0-31 and fine bits 32-63).

### How It Works

```
/sys/bus/pci/devices/<BDF>/config
        └─ Intel DVSEC / VSEC, id = 66 (TPMI), rev 1
              ├─ tBIR  → BAR number
              └─ table offset → PFS table physical address
/sys/bus/pci/devices/<BDF>/resource<tBIR>  (mmap; falls back to /dev/mem)
        └─ PFS entry (each 64-bit: tpmi_id / num_entries / entry_size / cap_offset)
              ├─ id 0x81 (INFO)  → package id / segment / partition
              └─ id 0x0C (PLR)   → one register bank per power domain
                     0x00 HEADER
                     0x08 MAILBOX_INTERFACE (module_id[19:12], RUN_BUSY[63], WRITE[0])
                     0x10 MAILBOX_DATA
                     0x18 DIE_LEVEL
```

Per-core results use `MSR 0x54 (PM_LOGICAL_ID)` to obtain
`PM_DOMAIN_ID[15:11] / MODULE_ID[10:3] / LP_ID[2:0]`. The Linux CPU number is
then mapped to a punit module ID before the PLR mailbox is read.

### Build

```sh
make              # static link (recommended); produces ./plr-tpmi
make dynamic      # dynamic link
make install      # install to /usr/local/bin
```

Cross-compilation example:

```sh
make CC=x86_64-linux-musl-gcc
```

> Both musl and `glibc -static` work. musl produces a smaller, more portable
> static binary.

### Usage

```sh
sudo modprobe msr          # optional; required to read per-core reasons
sudo ./plr-tpmi            # print all package/domain limit reasons
sudo ./plr-tpmi -l         # list TPMI features on all devices (PFS dump)
sudo ./plr-tpmi -a         # print only cores reporting a limit reason
sudo ./plr-tpmi -c         # clear PLR state after reading it
sudo ./plr-tpmi -w 1 -n 10 # sample every second, ten times
```

#### Options

| Option | Description |
| --- | --- |
| `-l, --list` | List all features on each TPMI device (ID, instance count, entry size, and base address). |
| `-c, --clear` | Clear die-level and per-core status after reading. |
| `-a, --active-only` | Print only nonzero per-core status. |
| `-w, --watch SEC` | Sampling interval in seconds; decimal values are supported. |
| `-n, --count N` | Stop after N samples; used with `--watch`. |
| `-v, --verbose` | Print probe details to stderr. |

#### Example Output

```
TPMI device 0000:e2:00.1  package 0  segment 0  partition 0  (PFS 0x383ffff00000)
  domain 0  (base 0x383ffff2c000)
    die-level                  0x00000004 : POWER
    module 0   cpu 0,128
      0x0000040000000004 : POWER POWER_PKG_PL1_MSR_TPMI
    module 1   cpu 1,129
      0x0000000000000000 : none
```

- The low 32 bits are **coarse reasons**: `FREQUENCY / CURRENT / POWER /
  THERMAL / PLATFORM / MCP / RAS / MISC / QOS / DFC`.
- The high 32 bits are **fine reasons**, such as `POWER_PKG_PL1_MSR_TPMI`,
  `THERMAL_PER_CORE`, `PLATFORM_PROCHOT`, `DFC_UFS`, and
  `FREQUENCY_CDYN0..5`.
- Only coarse bits are valid in the die-level register.

### Validated Environment

The tool was validated on an `Intel(R) Xeon(R) 6986P-C` (Granite Rapids-AP,
240 logical CPUs / 120 cores / 3 power domains) running Debian 12 with kernel
6.6:

- Detected TPMI device `0000:00:03.1`, BAR1 at `0xd0400000`, and 15 PFS
  features including `0x0c plr` (five instances).
- Read die-level and per-core status for all three active domains and 120 cores.
- Correctly decoded the following under AES/AVX load: `FREQUENCY POWER
  FREQUENCY_CDYN1 FREQUENCY_CDYN2 POWER_PKG_PL1_MSR_TPMI
  POWER_PKG_PL2_MSR_TPMI`.
- Verified `--clear`, `--watch`, and `--active-only`.

The `test/` directory contains three scripts. Copy them to the target machine
and run them with `sh xxx.sh`; use `tr -d '\r'` first if line endings need to
be converted.

| Script | Purpose |
| --- | --- |
| `test/smoke_test.sh` | Collect platform information, list features, clear status, and compare before and after a load test. |
| `test/summarize.sh` | Summarize die-level and per-core reason histograms for each domain. |
| `test/rw_test.sh` | Verify the `--clear` write path, `--watch` sampling, and fine reasons under AVX load. |

### License

This project is licensed under the GNU General Public License v3.0 only
(GPL-3.0-only). See [LICENSE](LICENSE) for the full terms.

### Requirements

- Root privileges are required to map a PCI BAR.
- Kernel lockdown must be disabled. If mmap of `resource<N>` is denied, the
  tool falls back to `/dev/mem`.
- Per-core reasons require the `msr` kernel module (`/dev/cpu/*/msr`).
  Die-level results remain available without it.
- When the kernel `intel_plr_tpmi` driver is also loaded, both readers access
  the same registers. Using `--clear` also clears the values visible through
  the kernel debugfs interface.

### References

- `drivers/platform/x86/intel/plr_tpmi.c`
- `drivers/platform/x86/intel/vsec_tpmi.c`
- `drivers/platform/x86/intel/vsec.c`
- `drivers/platform/x86/intel/tpmi_power_domains.c`

## 中文

Linux 下的 Intel TPMI 工具，用于读取并解码 **Perf Limit Reasons (PLR)**，适用于
Granite Rapids (GNR)、Granite Rapids-D、Sierra Forest / Clearwater Forest、
Diamond Rapids 等支持 TPMI PLR 特性的平台。

## 特点

- **零第三方依赖**：只用 libc + sysfs，`make` 默认产出静态链接的单文件可执行程序。
- **不依赖内核 PLR 驱动**：不需要 `intel_plr_tpmi` / debugfs，直接经
  PCI DVSEC/VSEC → PFS → TPMI ID `0x0C` 访问 MMIO。
- 同时输出 **die/domain 级** 与 **per-core (punit module) 级** 的限频原因，并解码为
  与内核一致的符号名（coarse bit 0-31，fine bit 32-63）。

## 工作原理

```
/sys/bus/pci/devices/<BDF>/config
        └─ Intel DVSEC / VSEC, id = 66 (TPMI), rev 1
              ├─ tBIR  → BAR 编号
              └─ table offset → PFS 表物理地址
/sys/bus/pci/devices/<BDF>/resource<tBIR>  (mmap，失败时回退 /dev/mem)
        └─ PFS 表项 (每项 64bit: tpmi_id / num_entries / entry_size / cap_offset)
              ├─ id 0x81 (INFO)  → package id / segment / partition
              └─ id 0x0C (PLR)   → 每个 power domain 一组寄存器
                     0x00 HEADER
                     0x08 MAILBOX_INTERFACE (module_id[19:12], RUN_BUSY[63], WRITE[0])
                     0x10 MAILBOX_DATA
                     0x18 DIE_LEVEL
```

per-core 结果通过 `MSR 0x54 (PM_LOGICAL_ID)` 得到
`PM_DOMAIN_ID[15:11] / MODULE_ID[10:3] / LP_ID[2:0]`，再把 Linux CPU 号映射到
punit module id 后走 PLR mailbox 读取。

## 编译

```sh
make              # 静态链接（推荐，产出独立可执行文件 ./plr-tpmi）
make dynamic      # 动态链接
make install      # 安装到 /usr/local/bin
```

交叉编译示例：

```sh
make CC=x86_64-linux-musl-gcc
```

> 用 musl 或 `glibc -static` 均可；musl 的静态二进制体积更小、可移植性更好。

## 使用

```sh
sudo modprobe msr          # 可选，但读取 per-core 原因时必需
sudo ./plr-tpmi            # 打印所有 package/domain 的限频原因
sudo ./plr-tpmi -l         # 列出设备上所有 TPMI feature（PFS dump）
sudo ./plr-tpmi -a         # 只显示确实有限频原因的 core
sudo ./plr-tpmi -c         # 读取后清除 PLR 状态
sudo ./plr-tpmi -w 1 -n 10 # 每 1 秒采样一次，共 10 次
```

### 选项

| 选项 | 说明 |
| --- | --- |
| `-l, --list` | 列出每个 TPMI 设备的全部 feature（id / 实例数 / 条目大小 / 基地址） |
| `-c, --clear` | 读取后清零 die-level 与 per-core 状态 |
| `-a, --active-only` | 只打印非零的 per-core 状态 |
| `-w, --watch SEC` | 循环采样间隔（秒，支持小数） |
| `-n, --count N` | 配合 `--watch` 使用，采样 N 次后退出 |
| `-v, --verbose` | 在 stderr 输出探测细节 |

### 输出示例

```
TPMI device 0000:e2:00.1  package 0  segment 0  partition 0  (PFS 0x383ffff00000)
  domain 0  (base 0x383ffff2c000)
    die-level                  0x00000004 : POWER
    module 0   cpu 0,128
      0x0000040000000004 : POWER POWER_PKG_PL1_MSR_TPMI
    module 1   cpu 1,129
      0x0000000000000000 : none
```

- 低 32 bit 为 **coarse reason**：`FREQUENCY / CURRENT / POWER / THERMAL /
  PLATFORM / MCP / RAS / MISC / QOS / DFC`
- 高 32 bit 为 **fine reason**：如 `POWER_PKG_PL1_MSR_TPMI`、`THERMAL_PER_CORE`、
  `PLATFORM_PROCHOT`、`DFC_UFS`、`FREQUENCY_CDYN0..5` 等
- die-level 寄存器只有 coarse 位有效

## 已验证环境

在 `Intel(R) Xeon(R) 6986P-C`（Granite Rapids-AP，240 逻辑核 / 120 core / 3 个 power domain）
+ Debian 12、kernel 6.6 上实测通过：

- 发现 TPMI 设备 `0000:00:03.1`，BAR1 `0xd0400000`，PFS 列出 15 个 feature，含 `0x0c plr`（5 实例）
- 3 个有效 domain 的 die-level 与 120 个 core 的 per-core 状态全部读出
- AES/AVX 满载下正确解码出：
  `FREQUENCY POWER FREQUENCY_CDYN1 FREQUENCY_CDYN2 POWER_PKG_PL1_MSR_TPMI POWER_PKG_PL2_MSR_TPMI`
- `--clear` / `--watch` / `--active-only` 均正常

`test/` 下附带三个脚本（拷到目标机 `sh xxx.sh` 运行，注意先 `tr -d '\r'`）：

| 脚本 | 用途 |
| --- | --- |
| `test/smoke_test.sh` | 平台信息 + feature 列表 + 清除 + 满载前后对比 |
| `test/summarize.sh` | 汇总各 domain die-level 与 per-core 原因直方图 |
| `test/rw_test.sh` | 验证 `--clear` 写路径、`--watch` 采样、AVX 负载下的 fine reason |

## 许可证

本项目采用 GNU General Public License v3.0 only（GPL-3.0-only）授权。完整条款见
[LICENSE](LICENSE)。

## 运行要求

- root 权限（映射 PCI BAR）。
- 内核未开启 lockdown；若 `resource<N>` 的 mmap 被拒绝，会自动回退到 `/dev/mem`。
- per-core 原因需要 `msr` 内核模块（`/dev/cpu/*/msr`）。缺失时仍可输出 die-level。
- 若同时加载了内核 `intel_plr_tpmi` 驱动，两者只是并行读取同一组寄存器；
  但 `--clear` 会影响内核 debugfs 里看到的值。

## 参考

- `drivers/platform/x86/intel/plr_tpmi.c`
- `drivers/platform/x86/intel/vsec_tpmi.c`
- `drivers/platform/x86/intel/vsec.c`
- `drivers/platform/x86/intel/tpmi_power_domains.c`
