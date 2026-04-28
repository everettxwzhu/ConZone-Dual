# ConZone-Dual I/O 处理流程详解

> 适用配置：`CONFIG_NVMEVIRT_CONZONE_DUAL`（即 `BASE_SSD == CONZONE_DUAL_PROTOTYPE`）

---

## 1. 系统整体架构

ConZone-Dual 是一个运行在 Linux 内核中的 NVMe 虚拟设备（`nvmevirt` 内核模块）。它向主机暴露**两个命名空间（Namespace）**，各自对应不同的闪存介质：

```
主机看到的两个 NVMe 命名空间
┌─────────────────┐    ┌─────────────────┐
│  Namespace 0    │    │  Namespace 1    │
│  (SLC 命名空间)  │    │  (TLC 命名空间)  │
│ SSD_TYPE_CONZONE│    │ SSD_TYPE_CONZONE│
│ _SLC            │    │ _TLC            │
└────────┬────────┘    └────────┬────────┘
         │                     │
         └──────────┬──────────┘
                    │  共享同一块物理 NAND
           ┌────────▼────────┐
           │   struct ssd    │   ← 单一 ssd 实例，模拟物理闪存芯片
           │  4 ch × 2 lun   │
           │  × 4 plane      │
           │  SLC 行 + TLC 行 │
           └─────────────────┘
```

**关键宏定义**（[ssd_config.h](../ssd_config.h)）：

```c
#define IS_CONZONE_DUAL (BASE_SSD == CONZONE_DUAL_PROTOTYPE)

// 双命名空间类型
#define NS_SSD_TYPE_0  SSD_TYPE_CONZONE_SLC   // Namespace 0：SLC
#define NS_SSD_TYPE_1  SSD_TYPE_CONZONE_TLC   // Namespace 1：TLC

// 物理配置
#define NAND_CHANNELS      4
#define LUNS_PER_NAND_CH   2
#define PLNS_PER_LUN       4
#define CELL_MODE          CELL_MODE_TLC       // 默认 TLC，SLC 行是 pSLC 模式

// SLC 行数（每个 plane 中划给 SLC 的 block 行数）
#define DUAL_SLC_INIT_BLKS 4
```

---

## 2. 两种介质的区分：LOC_NORMAL 与 LOC_PSLC

代码中用一个枚举区分数据存在哪种介质上（[zns_ftl.h:38-41](../zns_ftl.h#L38)）：

```c
enum {
    LOC_NORMAL = 0,  // TLC 区域（普通闪存块）
    LOC_PSLC   = 1,  // pSLC 区域（伪 SLC 模式的闪存块，每页只写 1 bit）
};
```

**pSLC（pseudo-SLC）**：物理上还是 TLC 块，但固件控制每个存储单元只写入 1 bit，牺牲容量换取更快的读写速度和更低的延迟。

根据命名空间类型，可以知道写入目标介质（[zns_ftl.h:455-458](../zns_ftl.h#L455)）：

```c
static inline int zms_dual_target_loc(int ns_type)
{
    return ns_type == SSD_TYPE_CONZONE_SLC ? LOC_PSLC : LOC_NORMAL;
}
```

---

## 3. I/O 请求的完整处理流程

下面是一个写/读请求从主机下发到完成的完整路径。

### 3.1 第一步：提交队列处理（io.c）

主机将 NVMe 命令放入**提交队列（Submission Queue，SQ）**，内核模块的调度器调用：

```
nvmev_proc_io_sq()          ← 处理一批 SQ 条目
  └─ __nvmev_proc_io()      ← 处理单个命令
        ├─ ns->proc_io_cmd()   ← 调用命名空间的命令分发器
        │     └─ 根据命令类型（读/写/zone管理）路由到具体函数
        ├─ __enqueue_io_req()  ← 将工作项加入 I/O worker 的工作队列（按完成时间排序）
        └─ __reclaim_completed_reqs()  ← 回收已完成的工作项
```

关键数据结构（`struct nvmev_io_work`）：
- `nsecs_start`：请求到达时间
- `nsecs_target`：预计完成时间（由 FTL 计算 NAND 延迟后设定）
- `is_copied`：数据 DMA/memcpy 是否完成
- `is_internal`：是否是内部操作（写缓冲区刷新）

### 3.2 第二步：命令分发（zms_ftl.c）

两个命名空间各有自己的命令处理入口 `proc_io_cmd`，最终都路由到：
- **写请求** → `zoned_write()` → `handle_write_request()`
- **读请求** → `zoned_read()` → `handle_read_request()`

### 3.3 第三步：写请求处理（handle_write_request，zms_read_write.c:2844）

```
handle_write_request()
  │
  ├─ 1. 检查设备是否已满（device_full / pslc_full）
  │
  ├─ 2. 检查是否有进行中的迁移操作（pending_for_migrating）
  │      如果有，等待迁移完成再接受新写入
  │
  ├─ 3. 获取写缓冲区（__zms_wb_get）
  │      每个 zone 对应一个写缓冲区（ZONE_WB_SIZE = 1536 KiB）
  │      如果缓冲区属于其他 zone，先 flush 旧缓冲区
  │
  ├─ 4. 对每个 4KiB 页（LPN）：
  │      └─ 将数据写入写缓冲区，更新 L2P 映射
  │
  ├─ 5. 当缓冲区写满一个 oneshot page（程序单元）时：
  │      └─ buffer_flush() → nand_write() → submit_nand_cmd()
  │            ├─ SLC 命名空间：写入 pSLC 区域（1 flash page = 1 bit/cell）
  │            └─ TLC 命名空间：写入 TLC 区域（3 flash pages = 3 bit/cell）
  │
  └─ 6. 写信用（write credit）管理：
         consume_write_credit() → check_and_refill_write_credit()
         如果信用用完，触发 GC 来补充
```

**写缓冲区的作用**：收集小写，凑满一个 oneshot page（TLC = 3 个 flash page，SLC = 1 个 flash page）再一次性写入 NAND，提升写入效率。

### 3.4 第四步：读请求处理（handle_read_request，zms_read_write.c:3124）

```
handle_read_request()
  │
  ├─ 1. 计算固件延迟（fw_4kb_rd_lat 或 fw_rd_lat）
  │
  ├─ 2. 对每个 LPN：
  │      ├─ a. 检查写缓冲区命中（热数据直接返回）
  │      ├─ b. L2P 缓存查找（4 级粒度，从粗到细）
  │      │     ZONE_MAP → SUB_ZONE_MAP → CHUNK_MAP → PAGE_MAP
  │      ├─ c. 如果 L2P 缓存未命中：从 NAND 读取 L2P 表（map_read）
  │      └─ d. 查 maptbl 得到 PPA（物理页地址）
  │
  └─ 3. 聚合同一 flash page 的多次读：
         nand_read() → submit_nand_cmd(NAND_READ)
         按 (ch, lun, plane) 分组，同一 flash page 的读只提交一次 NAND 命令
```

### 3.5 第五步：I/O Worker 线程完成 I/O（io.c:569）

```
nvmev_io_worker()  ← 绑定在特定 CPU 上的内核线程
  │
  ├─ 按 nsecs_target 顺序扫描工作队列
  ├─ 如果 is_copied == false：执行数据 memcpy（__do_perform_io）
  │      将数据从/到 nvmev_vdev->ns[nsid].mapped（模拟的 SSD 存储区）
  ├─ 如果 nsecs_target <= 当前时钟：
  │      ├─ 普通 I/O：填写完成队列条目（__fill_cq_result）并触发中断
  │      └─ 内部操作（写缓冲区释放）：buffer_release()
  └─ 触发 MSI/MSI-X 中断通知主机
```

---

## 4. 关键数据结构关系图

```
struct nvmev_dev (nvmev_vdev)
  ├─ ns[0]  ← SLC 命名空间
  │    └─ ftls → struct zms_ftl (SLC)
  │               ├─ ssd ──────────────────────────┐
  │               ├─ maptbl[]  (LPN→PPA 映射表)     │
  │               ├─ l2pcache_idx[]                  │
  │               ├─ lm (line_mgmt)                  │
  │               │    ├─ pslc_free_line_list        │
  │               │    ├─ pslc_victim_line_pq        │
  │               │    └─ pslc_full_line_list        │
  │               ├─ pslc_wp   (SLC 写指针)          │
  │               ├─ pslc_gc_wp (SLC GC 写指针)      │
  │               └─ write_buffer[]                  │
  │                                                  │  共享同一个
  ├─ ns[1]  ← TLC 命名空间                           │  struct ssd
  │    └─ ftls → struct zms_ftl (TLC)               │
  │               ├─ ssd ──────────────────────────▶│
  │               ├─ maptbl[]  (独立的 L2P 表)        │
  │               ├─ lm (line_mgmt)                  │
  │               │    ├─ free_line_list             │
  │               │    ├─ victim_line_pq             │
  │               │    └─ full_line_list             │
  │               ├─ wp   (TLC 写指针)               │
  │               ├─ gc_wp (TLC GC 写指针)           │
  │               └─ write_buffer[]                  │
  │                                                  │
  └─ io_workers[]  ← I/O Worker 线程池               │
       └─ work_queue (按 nsecs_target 排序的链表)     │
                                                     ▼
                                          struct ssd
                                            ├─ ch[0..3]  (4 个 channel)
                                            │    └─ lun[0..1]  (每 ch 2 个 lun)
                                            │         └─ blk[0..N]  (每 lun N 个 block)
                                            │              ├─ blk[0..3]   → pSLC 块 (SLC 命名空间)
                                            │              └─ blk[4..N-1] → TLC 块  (TLC 命名空间)
                                            └─ l2pcache  (两个命名空间共享)
```

---

## 5. 两个介质各自独享的部分

以下内容每个命名空间/介质独立拥有，互不干扰：

| 组件 | SLC 命名空间（NS 0）| TLC 命名空间（NS 1）| 说明 |
|------|---------------------|---------------------|------|
| **L2P 映射表** | `zms_ftl[0]->maptbl[]` | `zms_ftl[1]->maptbl[]` | 各自管理自己逻辑地址到物理地址的映射 |
| **Zone 描述符** | `zone_descs[]` (SLC zones) | `zone_descs[]` (TLC zones) | zone 的写指针、状态（EMPTY/OPEN/FULL）各自独立 |
| **写指针** | `pslc_wp`, `pslc_gc_wp` | `wp`, `gc_wp` | 4 个写指针：用户IO和GC各一个，两个介质各两个 |
| **Line 管理链表** | `pslc_free/victim/full_list` | `free/victim/full_list` | 空闲块、垃圾回收候选块、写满块的链表各自独立 |
| **写信用** | `pslc_wfc` | `wfc` | 流控：写一页消耗一个信用，GC 完成后补充 |
| **写缓冲区** | `write_buffer[]`（SLC）| `write_buffer[]`（TLC）| 各自缓冲区，ZONE_WB_SIZE = 1536 KiB |
| **物理块行** | block row 0..3（pSLC 模式）| block row 4..N（TLC 模式）| 物理上哪些块属于谁，由 `pslc_blks` 划分 |
| **写入 oneshot 大小** | 1 flash page（16 KiB）| 3 flash pages（48 KiB）| TLC 需要凑满 3 页才能一次性编程 |

---

## 6. 两个介质共享的部分

以下内容由两个命名空间**共同使用**：

| 组件 | 位置 | 说明 |
|------|------|------|
| **物理 NAND 模拟器** | `struct ssd` | 单一 ssd 实例，模拟 4ch × 2lun × 4plane 的闪存芯片 |
| **Channel/LUN/Plane 资源** | `ssd->ch[]` | 物理通道带宽、并发访问冲突由同一个 `ssd_advance_nand()` 仲裁 |
| **L2P 缓存** | `ssd->l2pcache` | 1020 KiB 的 LRU 缓存，两个命名空间的 L2P 条目都缓存在这里，按 slot 哈希区分 |
| **I/O Worker 线程** | `nvmev_vdev->io_workers[]` | 两个命名空间的 I/O 完成都由同一组 worker 线程处理 |
| **中断机制** | `nvmev_signal_irq()` | 同一套 MSI/MSI-X 中断向量 |
| **NAND 延迟参数** | `ssdparams` | SLC/TLC 各自的读写延迟数值存在同一个参数结构里，但 cell mode 决定使用哪套参数 |

**L2P 缓存共享的细节**（[zms_read_write.c:594-608](../zms_read_write.c#L594)）：

缓存项里存有 `nsid` 字段，evict 时根据 nsid 找到对应的 `zms_ftl` 来更新 `l2pcache_idx[]`，确保两个命名空间的缓存条目不互相覆盖出错。

---

## 7. 写入流程中的 SLC/TLC 路由决策

每次写 NAND 时，`nand_write()` 根据命名空间类型决定目标介质（[zms_read_write.c:1682](../zms_read_write.c#L1682)）：

```
nand_write(zms_ftl, lpn, location, io_type)
  │
  ├─ location == LOC_PSLC？
  │    → 写入 pSLC 块（SLC 命名空间的数据）
  │    → 使用 pslc_wp 写指针
  │    → oneshot page size = 1 flash page (16 KiB)
  │    → 写后：consume_write_credit(LOC_PSLC)
  │
  └─ location == LOC_NORMAL？
       → 写入 TLC 块（TLC 命名空间的数据）
       → 使用 wp 写指针
       → oneshot page size = 3 flash pages (48 KiB)，TLC 三步编程
       → 写后：consume_write_credit(LOC_NORMAL)
```

判断已有数据在哪个介质（[zms_read_write.c:47-62](../zms_read_write.c#L47)）：

```c
static int get_page_location(struct zms_ftl *zms_ftl, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    switch (blk->nand_type) {
    case CELL_MODE_SLC: return LOC_PSLC;   // 该块是 SLC 模式
    default:            return LOC_NORMAL; // 该块是 TLC 模式
    }
}
```

---

## 8. 四级 L2P 映射粒度

ConZone-Dual 使用混合映射粒度（`L2P_HYBRID_MAP = 1`）来节省内存，同时保持查找效率（[ssd_config.h:399-408](../ssd_config.h#L399)）：

```c
enum {
    PAGE_MAP     = 0,  // 4 KiB：精确到页（可被 evict）
    CHUNK_MAP    = 1,  // 4 MiB：一个 chunk（可被 evict）
    SUB_ZONE_MAP = 2,  // pSLC zone 大小（常驻）
    ZONE_MAP     = 3,  // 完整 zone（常驻）
};
```

- **常驻**（`L2P_HYBRID_MAP_RESIDENT = 1`）：ZONE_MAP 和 SUB_ZONE_MAP 粒度的条目一直保留在缓存中
- **可 evict**：CHUNK_MAP 和 PAGE_MAP 条目按 LRU 替换

**读请求时的 L2P 查找顺序**（从粗粒度到细粒度）：

```
查找 ZONE_MAP → 命中？→ 用该条目推算出具体 PPA
  ↓ 未命中
查找 SUB_ZONE_MAP → 命中？→ 用该条目推算出具体 PPA
  ↓ 未命中
查找 CHUNK_MAP → 命中？→ 用该条目推算出具体 PPA
  ↓ 未命中
查找 PAGE_MAP → 命中？→ 直接得到 PPA
  ↓ 未命中（L2P Cache Miss）
从 NAND 读取 L2P 页（map_read），然后继续
```

---

## 9. GC 与数据迁移（SLC → TLC）

ConZone-Dual 使用 `SLC_BYPASS = 1` 模式，即 SLC 命名空间的数据**直接由主机显式管理**，不在固件内部做 SLC→TLC 迁移。

TLC 命名空间的 GC 流程：

```
check_and_refill_write_credit(LOC_NORMAL)
  └─ write_credits 不足？
       └─ foreground_gc(LOC_NORMAL)
            ├─ 1. 从 victim_line_pq 中选 VPC（有效页数）最少的 TLC 行
            ├─ 2. submit_internal_write() 读取该行中所有有效页
            ├─ 3. internal_write() 将有效数据重新写入新的 TLC 块
            └─ 4. erase_line() 擦除旧块，将其放回 free_line_list
```

SLC 命名空间的 GC 流程类似，但目标是 `pslc_free_line_list`，操作的是 pSLC 块。

---

## 10. 完整数据流时序图（写请求示例）

```
主机                   io.c                    zms_read_write.c           ssd.c
 │                       │                            │                     │
 │──Write(NS0, zone1)──▶ │                            │                     │
 │                       │─nvmev_proc_io_sq()──▶      │                     │
 │                       │─__nvmev_proc_io()──▶       │                     │
 │                       │──proc_io_cmd(NS0)──────▶   │                     │
 │                       │                     zoned_write()                │
 │                       │                     handle_write_request()       │
 │                       │                     __zms_wb_get() [获取写缓冲]   │
 │                       │                     buffer_allocate()            │
 │                       │                     [数据写入写缓冲区]            │
 │                       │                     [凑满 oneshot page?]         │
 │                       │                     buffer_flush()               │
 │                       │                     nand_write(LOC_PSLC)         │
 │                       │                     submit_nand_cmd()────────▶   │
 │                       │                            │              ssd_advance_nand()
 │                       │                            │              [计算 pSLC 写延迟]
 │                       │                            │              [返回 nsecs_target]
 │                       │◀──────────────── nsecs_target ──────────────────│
 │                       │─__enqueue_io_req()         │                     │
 │                       │  [按完成时间插入工作队列]    │                     │
 │                       │                            │                     │
 │       (时间流逝...)    │                            │                     │
 │                       │─nvmev_io_worker()          │                     │
 │                       │  [时钟 >= nsecs_target]     │                     │
 │                       │  __do_perform_io()          │                     │
 │                       │  [memcpy 数据到存储区]       │                     │
 │                       │  __fill_cq_result()         │                     │
 │                       │  nvmev_signal_irq()         │                     │
 │◀──MSI-X 中断──────────│                            │                     │
 │  (I/O 完成)            │                            │                     │
```

---

## 11. 关键延迟参数（ssd_config.h）

| 参数 | 数值 | 说明 |
|------|------|------|
| `SLC_NAND_PROG_LATENCY` | 75 µs | pSLC 写延迟（来自 ISSCC 2020） |
| `TLC_NAND_PROG_LATENCY` | 937.5 µs | TLC 写延迟（来自 ISSCC 2024） |
| `SLC_NAND_READ_LATENCY_LSB` | 20 µs | pSLC 读延迟 |
| `TLC_NAND_READ_LATENCY_LSB/MSB/CSB` | 40 µs × 3 | TLC 三位读延迟（LSB/MSB/CSB 相同） |
| `NAND_ERASE_LATENCY` | 3500 µs | 块擦除延迟 |
| `FW_4KB_READ_LATENCY` | 20 µs | 固件处理 4KiB 读的额外延迟 |
| `FW_WBUF_LATENCY0` | 5.6 µs | 写缓冲区基础延迟 |

SLC 写比 TLC 写快约 **12.5 倍**，SLC 读比 TLC 读快约 **2 倍**。

---

## 12. 源文件索引

| 文件 | 主要内容 |
|------|----------|
| [ssd_config.h](../ssd_config.h) | 所有配置宏：命名空间类型、物理参数、延迟数值 |
| [ssd.h](../ssd.h) | 核心数据结构：PPA、NAND 页/块/lun、ssdparams |
| [zns_ftl.h](../zns_ftl.h) | FTL 数据结构：zms_ftl、zms_write_pointer、zms_line_mgmt |
| [io.c](../io.c) | I/O 提交/完成处理、I/O Worker 线程 |
| [zns_ftl.c](../zns_ftl.c) | 命名空间初始化、参数计算、realize |
| [zms_read_write.c](../zms_read_write.c) | 核心读写逻辑、L2P 缓存、GC、迁移 |
| [ssd.c](../ssd.c) | 物理 NAND 模拟、延迟计算 |
