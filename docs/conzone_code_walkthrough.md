# ConZone+ 代码走读笔记

> 适用范围：`Kbuild` 中 `CONFIG_NVMEVIRT_CONZONE := y`，`BASE_SSD = CONZONE_PROTOTYPE`。
> 对象文件集合：`main.o pci.o admin.o io.o dma.o ssd.o zns_ftl.o zns_read_write.o
> zms_read_write.o zns_mgmt_send.o zns_mgmt_recv.o channel_model.o conv_ftl.o simple_ftl.o
> pqueue/pqueue.o`。

本笔记按照「入门概念 → 整体架构 → 核心数据结构 → 三条关键路径（写 / 读 / pSLC→TLC 迁移）」
的顺序组织，面向需要快速看懂 ConZone+ 仿真器是"怎么把一条 NVMe 命令变成 NAND 事件并计时"
的读者。文中所有文件/行号链接以仓库根目录为基准。

> **怎么用这份文档**
> - 完全没有背景：按顺序从 §0 读起。§0 会把后面章节里"不解释就看不懂"的术语先铺平。
> - 有 SSD / FTL 背景，只想找调用链：跳到 §3/§4/§5 看函数调用图。
> - 想改代码：先看 §2 的数据结构表，再看 §7 的"修改指引"。

---

## 0. 入门前置概念（为初学者准备）

这一节不讲 ConZone+ 本身，而是把"看懂这份代码需要先知道的东西"集中讲一遍。已经熟悉
SSD/FTL/NVMe 的读者可以直接跳到 §1。

### 0.1 NVMe 是怎么把"IO 命令"交给 SSD 的？

NVMe 是 PCIe SSD 的主流协议。它在内存里建两种环形队列：

- **SQ (Submission Queue)**：host 把一条命令（例如 `nvme_cmd_write(LBA, length, PRP1,…)`）
  写进 SQ 的一个槽位，然后把 SQ 的 tail 指针写进 SSD 的 **doorbell 寄存器**（MMIO 写）。
- **CQ (Completion Queue)**：SSD 完成命令后把 CQE（完成项）写入 CQ，并通过中断
  （MSI-X）通知 host。

NVMeVirt 的做法是：用一个内核线程 **`nvmev_dispatcher`**（[main.c:167](../main.c#L167)）
不停地轮询 MMIO 区域里的 doorbell 寄存器；一旦发现 host 更新了 SQ tail，就从 SQ 里读出命
令，模拟 SSD 的处理。所以整个仿真器不需要真的 PCIe 硬件。

本文里"一条 NVMe 命令"你可以理解为：`struct nvme_command`（`nvme.h`）里的一项，主要是
`nvme_cmd_read / nvme_cmd_write / nvme_cmd_zone_append / nvme_cmd_flush /
nvme_cmd_zone_mgmt_send/recv` 这几种。

### 0.2 LBA、LPN、PPA 三套地址

SSD 世界里最容易混的就是地址。这里固定约定：

| 名称 | 单位 | 含义 |
|---|---|---|
| **LBA** (Logical Block Address) | 512 B (`LBA_SIZE = 1<<9`) | host 层面的块号，NVMe 命令里的 `slba/length` 都以 LBA 计。 |
| **LPN** (Logical Page Number) | 4 KiB (`PG_SIZE`) | FTL 内部的映射单位；`LPN = LBA / secs_per_pg`。 |
| **PPA** (Physical Page Address) | 4 KiB | NAND 里的物理页地址，由 `(ch, lun, pl, blk, pg)` 5 元组编码。 |

于是 FTL 最核心的工作就是维护 `maptbl[LPN] → PPA`（正向映射）和 `rmap[PPA] → LPN`
（反向映射，GC 要用）。

### 0.3 NAND 的物理层级

SSD 里的 NAND flash 天然是多层并行的：

```
SSD
 └─ Channel  (并行总线, 本项目 NAND_CHANNELS=4, 每根带宽 1422 MB/s)
     └─ LUN / Die  (可独立调度的颗粒, LUNS_PER_NAND_CH=2)
         └─ Plane  (一个 die 内可并行的子单元, PLNS_PER_LUN=4)
             └─ Block  (擦除的最小单位, BLK_SIZE=33 MiB)
                 └─ Flash Page  (物理读出的最小单位, 16 KiB)
                     └─ 4 KiB Page  (FTL 映射单位, = LPN)
```

三条关键规律（后面代码都反复用）：

1. **读**：最细到 4 KiB；一次读的延迟由 `pg_rd_lat / pg_4kb_rd_lat` 决定，数据通过
   channel 送出。
2. **写**：必须至少写一个 **oneshot page**。TLC 的 oneshot = 3 × flash page = 48 KiB，
   pSLC 的 oneshot = 1 × flash page = 16 KiB。一次 `NAND_PROG` 直接把这个 oneshot 里
   所有 cell 编程到目标电平。
3. **擦**：最小粒度是 Block（33 MiB），延迟 3.5 ms 量级。一个 block 必须"整块擦才能重写"。

"读 ≤ 页，写 = oneshot，擦 = block"这三档错位，就是 FTL 必须存在的根本原因。

### 0.4 SLC / MLC / TLC / QLC & "pSLC"

一个 NAND cell 可以存 1 bit (SLC) / 2 bit (MLC) / 3 bit (TLC) / 4 bit (QLC)。bit 数越多，
容量越大，但：

- 写延迟指数级变高（SLC ≈ 75 μs，TLC ≈ 937 μs，QLC ≈ 6 ms）；
- 读延迟也升高（20/36/40/85 μs）；
- 寿命（可擦写次数）下降。

**pSLC (pseudo-SLC)**：物理上是 TLC / QLC cell，固件让它只存 1 bit，换来 SLC 的速度和寿
命。很多消费级 SSD 的做法就是"先快速写 pSLC，空闲时再把数据搬到 TLC"，这就是 ConZone+
要模拟的核心机制之一。

在 ConZone+ 里：`CELL_MODE=TLC`，但每个 die 划出 `pSLC_INIT_BLKS` 个 block 固定工作在
SLC 模式（`blk->nand_type == CELL_MODE_SLC`）。这就构成了 **"pSLC 区 + TLC 区"混合介质**。

### 0.5 写缓冲 / oneshot 对齐

host 写下来的可能是零散的 4 KiB，但 NAND 只能按 oneshot 写。所以 FTL 必须先把写合并进
**write buffer**（DRAM 缓存），凑够一个 oneshot 再下发 NAND。

相关代码里会反复出现几件事：

- `buffer_allocate(buf, size)`：拿到这个 buffer 的独占权（自旋锁 + busy 标志）。
- `buffer->lpns[]` + `buffer->pgs`：在 buffer 里排队等待刷出去的 LPN。
- `buffer->flush_data == buffer->capacity`：填满就 **buffer_flush** 下发 NAND。
- 刷完之后用 `schedule_internal_operation` 注入一个"在 `nsecs_target` 时刻释放 busy"
  的内部事件，让后续写能再次拿到这个 buffer。

### 0.6 ZNS（Zoned Namespace）

传统 NVMe SSD 把整块空间当一个大数组。**ZNS** 则把 LBA 空间切成很多 **Zone**，每个 zone
有以下约束：

- 必须**顺序写**：host 每次写 zone 都要从 zone 的 write pointer (wp) 开始，不能随便跳；
- 要**整 zone 重置**（`zone reset`，类似 block erase）才能重新从头写；
- 有状态机：`EMPTY → OPENED → FULL → EMPTY`。

好处是 FTL 几乎不需要 GC（因为 host 已经显式告诉它"这块 zone 我不要了"）；坏处是 host
要配合改造（文件系统用 F2FS / btrfs zoned mode 或者块层 zoned 驱动）。

ConZone+ 里的 **data 命名空间** (`SSD_TYPE_CONZONE_ZONED`) 就是 ZNS；**meta 命名空间**
(`SSD_TYPE_CONZONE_META`) 是普通 block 接口，用于放映射表等元数据（所以它里面有真正的
GC 逻辑）。

`zns_write_check()`（[zms_read_write.c:2453](../zms_read_write.c#L2453)）就是在做 ZNS 规范
的状态检查：slba 必须等于 zone wp、不能越界、zone 资源有没有满……

### 0.7 "Superblock / Line"是什么？

为了并行度，FTL 通常把多个 channel × lun × plane 的同位置 block 打包成一条 **super-block
（通常叫 line）**；写这条 line 就等于同时写很多 die，NAND 并行度拉满。在 ConZone+ 里：

- `struct zms_line` 既可能是 super-line（`parent_id == -1`，配 `sub_lines[]`），也可能是
  单个 block（`parent_id != -1`，即某条 super-line 的一个 sub 分量）；
- 每条 line 维护 `vpc/ipc/rpc`：
  - `vpc` (valid page count) 当前还有效的页数；
  - `ipc` (invalid page count) 已被新版本覆盖、等待 GC 的页数；
  - `rpc` (reserved page count) 已经预留 PPA 但还没真正写下去的页数（配合 zoned 的"写前
    先占位"语义）。

一条 line 的生命周期：`free_line_list → (curline of some WP) → full_line_list
或 victim_line_pq → 被 GC/迁移选中 → erase → free_line_list`。pSLC 与 normal 各有一套这
样的列表。

### 0.8 Write Pointer（WP）

**别和 ZNS 的 zone wp 混为一谈**：

- **ZNS zone wp**：host 可见的，记录 host 下一个该往哪个 LBA 写，见
  `zns_ftl->zone_descs[zid].wp`。
- **FTL 内部 WP**：FTL 自己决定的"下一个空闲 PPA 在哪里"，是 `struct zms_write_pointer`，
  它携带 `(ch,lun,pl,blk,pg)` 和当前所在的 line。

FTL 内部 WP 在 ConZone+ 里按 `(io_type, location)` 分成 4 条（[zms_ftl.c:672](../zns_ftl.c#L672)
`zms_get_wp`）：

|  | normal (TLC) | pSLC |
|---|---|---|
| USER_IO | `zms_ftl->wp` | `zms_ftl->pslc_wp` |
| GC_IO/MIGRATE_IO | `zms_ftl->gc_wp` | `zms_ftl->pslc_gc_wp` |

分流的目的：**让"冷的用户数据"和"GC 搬家的数据"不写进同一条 line**，GC 时挑 victim 才更
干净。

### 0.9 GC vs Migration

两者都是 FTL 后台搬数据的动作，但触发口径不同：

- **GC (Garbage Collection)**：一条 line 里 valid + invalid 都有，想回收空间就必须先把
  valid 搬走、再擦除。由 `foreground_gc` 在"写信用耗尽"时触发，目标是把 **valid 页搬去
  同一介质**（pSLC→pSLC 或 normal→normal）。
- **Migration（pSLC→TLC）**：pSLC 专用。pSLC 区容量小，写满之后必须把它搬到 TLC 才能腾空
  间。即使 pSLC 里 `invalid=0`（所有数据都还有效）也可能要搬。触发口径是
  `pslc_free_line_cnt <= migrate_thres_lines_low`。

ConZone+ 里二者复用 `submit_internal_write` → `internal_write` → `nand_read + nand_write`
这套工具函数，只是 `io_type`（`GC_IO` vs `MIGRATE_IO`）和目标 WP 不一样。

### 0.10 L2P 缓存 & 多粒度映射

完整的 maptbl（每 LPN 4 B）在 4 TB SSD 上要 4 GB DRAM，消费级 SSD 装不下。实际做法是把
maptbl 存在 NAND 里，DRAM 只缓存一部分（**L2P cache**）。

ConZone+ 又多做了一步：一条映射不一定以 PAGE 粒度存，如果一整条 zone / chunk 是连续顺
序写下来的（ZNS 语义保证），就可以只存一个 "zone → 起始 PPA" 的条目，大大减小 maptbl。
枚举值见 [ssd_config.h:386](../ssd_config.h#L386)：

```
PAGE_MAP      : 4 KiB 粒度 (传统 page map)
CHUNK_MAP     : 4 MiB 粒度
SUB_ZONE_MAP  : pSLC zone 粒度
ZONE_MAP      : zone 粒度
```

读路径里查 L2P 时会**从粗到细**试：hit 大粒度最便宜，miss 了再降粒度，最差回落到 page
map。这部分代码见 `map_read()`（[zms_read_write.c:3062](../zms_read_write.c#L3062)）和
`handle_read_request` 里的"L2P Search"块。

### 0.11 时间是怎么算出来的？

NVMeVirt 的精髓：**所有 IO 都是"立即"处理的；延迟是"算出来的 nsecs_target"，worker 线程只
是假装在那个时间点才完成**。

一条写请求的 `ret->nsecs_target` 大概是这样层层 `max` 出来的：

```
nsecs_target = max(
   PCIe DMA 完成时间 (chmodel_request, PCIe credit 环),
   firmware overhead (fw_wbuf_lat0 + N·fw_wbuf_lat1),
   NAND channel 传输完成 (chmodel_request, NAND CH credit 环),
   NAND program 完成 (plane.next_pln_avail_time + pg_wr_lat),
   …
)
```

具体地：

- **channel_model**：基于 4 μs 粒度的 credit 滑动窗口，模拟 channel 带宽抢占
  （[channel_model.c:31](../channel_model.c#L31)）。请求多的时候后来者只能拿到后面时刻的
  credit。
- **plane/lun 队列**：每个 plane 有一条 `cmd_queue_head`，新命令挂进去时用
  `stime = max(next_pln_avail_time, cmd_stime)`，模拟资源独占。
- **`schedule_internal_operation`**：用一个"在未来某时刻才执行"的内部事件把 buffer 释放、
  写回映射等动作"推后"处理，就是靠这个函数把"FTL 内部副作用"也编进时间轴的。

读懂了这一点就明白：仓库里所有 `nsecs_latest = max(...)`、`uint64_t complete_time = ...`
的赋值，不是真在等时间，而是在**累积应返回给 host 的"仿真完成时刻"**。

---

## 1. 整体架构

### 1.1 模块层次

```
 Guest Kernel (NVMe Driver)
        │   (MMIO doorbell ring)
        ▼
 ┌────────────────────────────────────────────────────────────┐
 │ 1. PCIe front-end (main.c / pci.c / admin.c)               │
 │    - nvmev_dispatcher kthread 轮询 doorbell                │
 │    - Admin SQ/CQ、IO SQ/CQ 门铃处理                        │
 └────────────────────────────────────────────────────────────┘
        │   nvmev_proc_io_sq(qid,new_db,old_db)
        ▼
 ┌────────────────────────────────────────────────────────────┐
 │ 2. IO path (io.c)                                          │
 │    - __nvmev_proc_io: 取 SQE，调 ns->proc_io_cmd           │
 │    - __enqueue_io_req → 每 worker 的按 nsecs_target 排序   │
 │      链表；nvmev_io_worker 执行 memcpy 和 CQE 填写         │
 └────────────────────────────────────────────────────────────┘
        │   ns->proc_io_cmd(ns, req, ret)   (函数指针)
        ▼
 ┌────────────────────────────────────────────────────────────┐
 │ 3. FTL layer (ConZone+)                                    │
 │    两个命名空间（NR_NAMESPACES = 2）:                      │
 │      ns[0] = SSD_TYPE_CONZONE_META  → zms_block_proc_...   │
 │      ns[1] = SSD_TYPE_CONZONE_ZONED → zms_zoned_proc_...   │
 │                                                            │
 │    zms_ftl: L2P/rmap、line_mgmt（normal & pSLC）、         │
 │             write_buffer[]、pslc/gc write pointer、        │
 │             migrating_line_pq                              │
 └────────────────────────────────────────────────────────────┘
        │   submit_nand_cmd(ssd, &nand_cmd)
        ▼
 ┌────────────────────────────────────────────────────────────┐
 │ 4. NAND timing model (ssd.c)                               │
 │    - plane/lun 队列（cmd_queue_head, next_pln_avail_time） │
 │    - 迁移抢占（MIGRATE_IO 与 USER_IO 的相互插队）          │
 │    - 计算 NAND_READ/WRITE/ERASE 的完成时间                 │
 └────────────────────────────────────────────────────────────┘
        │   chmodel_request(ch->perf_model, …)
        ▼
 ┌────────────────────────────────────────────────────────────┐
 │ 5. Channel / PCIe bandwidth model (channel_model.c)        │
 │    - 基于 4 μs 粒度 credit 环的带宽抢占                    │
 │    - nand ch & pcie 分别持有一个 channel_model             │
 └────────────────────────────────────────────────────────────┘
```

### 1.2 关键设计点（ConZone+ 相较 baseline NVMeVirt）

- **双命名空间 + 共享 NAND 设备**：两个 namespace 通过
  [zms_realize_namespaces()](zns_ftl.c#L974) 共享同一个 `struct ssd`，meta 走 block 接口，
  data 走 zoned 接口。见 [ssd_config.h:260-430](ssd_config.h#L260)。
- **pSLC 混合介质**：每个 die 内预留 `pSLC_INIT_BLKS` 个块配成 SLC 模式
  (`CELL_MODE_SLC`)，其余按 `CELL_MODE` (这里 TLC) 配置。由此 `zms_line_mgmt` 同时维护
  `pslc_*` 与 normal 两套 free / victim / full 队列。
- **按"super-line"管理物理地址**：`zms_line` 可以是跨 die 的 interleave superblock
  （`parent_id==-1`，配 `sub_lines[]`），也可以退化为单块（`parent_id!=-1`）。
  选块由 [get_first_page()](zms_read_write.c#L348)、
  [get_advanced_ppa_fast()](zms_read_write.c#L1013) 计算。
- **混合粒度 L2P 缓存**：`enum { PAGE_MAP, CHUNK_MAP, SUB_ZONE_MAP, ZONE_MAP }`，
  `L2P_HYBRID_MAP=1` 下 zoned ns 从 `ZONE_MAP` 开始查（resident 不被 LRU 踢出），
  miss 后再降粒度，最终才回落到 `PAGE_MAP`。缓存见 [struct l2pcache](ssd.h#L297)。
- **plane 级时间队列**：原版 NVMeVirt 在 lun 级排队，ConZone+ 在 plane 级排队
  （`plane_getstime/plane_update`），并在队列中区分 `MIGRATE_IO`，允许用户 IO 抢占迁移队
  尾，见 [ssd.c:727-808](ssd.c#L727)。

---

## 2. 核心数据结构

> 看代码时建议把这一节当"字典"用。后面任何一个不认识的 `struct foo` 名字都先回这里
> 查一下"它是干嘛的"，再点进字段细节。

### 2.1 PCIe / IO worker

| 结构 | 位置 | 作用 |
|---|---|---|
| `struct nvmev_dev` | [nvmev.h:216](nvmev.h#L216) | 全局单例 `nvmev_vdev`，持有 `ns[]`、`io_workers[]`、`sqes/cqes`。 |
| `struct nvmev_io_worker` | [nvmev.h:201](nvmev.h#L201) | 每 worker 的 `work_queue[NR_MAX_PARALLEL_IO]`，以 `next/prev` 组链表，按 `nsecs_target` 排序。 |
| `struct nvmev_io_work` | [nvmev.h:172](nvmev.h#L172) | 一条 IO 的生命周期字段：`is_copied / is_completed / nsecs_target / is_internal / write_buffer`。 |
| `struct nvmev_ns` | [nvmev.h:277](nvmev.h#L277) | 命名空间；`proc_io_cmd` 函数指针是 front-end 与 FTL 的分界线。 |

### 2.2 FTL 核心（ConZone+）

| 结构 | 位置 | 说明 |
|---|---|---|
| `struct zms_ftl` | [zns_ftl.h:196](zns_ftl.h#L196) | 每 ns 一个实例。含 `maptbl`（ppa）、`l2pcache_idx`（指向共享 cache 的索引）、`rmap`、`lm`（line 管理）、`wp/gc_wp/pslc_wp/pslc_gc_wp`、`migrating_line_pq`、聚合缓冲 `zone_agg_lpns[]`、统计计数等。 |
| `struct znsparams` | [zns_ftl.h:50](zns_ftl.h#L50) | zone/superblock 配置、阈值 `gc_thres_lines_high / migrate_thres_lines_low`。 |
| `struct zms_line_mgmt` | [zns_ftl.h:155](zns_ftl.h#L155) | 同时维护 `free / victim_pq / full` 以及 `pslc_*` 两套列表/堆。 |
| `struct zms_line` | [zns_ftl.h:124](zns_ftl.h#L124) | super-line 或 subline；`vpc/ipc/rpc`（valid/invalid/reserved page count）；`mid` 是"待迁移排序键"。 |
| `struct zms_write_pointer` | [zns_ftl.h:145](zns_ftl.h#L145) | `(curline, loc, ch, lun, pl, blk, pg)`；loc = LOC_NORMAL / LOC_PSLC。 |
| `struct buffer` | [ssd.h:184](ssd.h#L184) | 写缓存：`lpns[] / pgs / capacity / flush_data / zid / busy`。一个 ns 有 `nr_wb` 个 buffer。 |
| `struct ssd / ssdparams` | [ssd.h:208,308](ssd.h#L208) | 共享 NAND 建模，含 `ch[].lun[].pl[].blk[].pg[]` 五层层级。 |
| `struct channel_model` | [channel_model.h:29](channel_model.h#L29) | 每 NAND channel 和 PCIe 各一个，基于 4μs × 96K 条目的 credit 滑动窗口。 |
| `struct l2pcache / l2pcache_ent` | [ssd.h:297](ssd.h#L297) | 哈希 (`num_slots=3`) + slot 内 LRU 链表（`head/tail/next/last`）。容量 ~1 MiB。 |

### 2.3 PPA 布局

`struct ppa` 位域（[ssd.h:81](ssd.h#L81)）：`ch(8) | lun(8) | pl(8) | blk(16) | pg(16)` + ConZone 扩展字段 `map(2) / map_rsv(1)`。`IS_RSV_PPA()` 用于"已预留未写"的槽位。

---

## 3. 写路径（Write Path）

> **快速理解**：host 写一笔数据的过程 ≈ "把数据塞进 DRAM 写缓冲 → 凑够 oneshot →
> 选一段 PPA → 算 NAND/PCIe 时间 → 把'什么时候该完成'告诉 worker"。
> 真实数据 memcpy 是在 worker 线程到期时（`__do_perform_io`）才发生的，FTL 这一层只
> 负责"算时间 + 维护元数据"。

**起点**：`nvmev_proc_dbs` 检测到 IO SQ 门铃更新，调
[nvmev_proc_io_sq()](io.c#L480) → [__nvmev_proc_io()](io.c#L411) →
`ns->proc_io_cmd = zms_zoned_proc_nvme_io_cmd`（zoned ns）或
`zms_block_proc_nvme_io_cmd`（meta ns）。

### 3.1 调用链路（zoned 写为例）

```
dispatcher kthread  [main.c:167 nvmev_dispatcher]
  └─ nvmev_proc_dbs                          [main.c:115]
       └─ nvmev_proc_io_sq(qid,new_db,old_db) [io.c:480]
            └─ __nvmev_proc_io                [io.c:411]
                 ├─ ns->proc_io_cmd(ns,req,ret)
                 │    = zms_zoned_proc_nvme_io_cmd [zns_ftl.c:289]
                 │      └─ zoned_write                          [zms_read_write.c:3246]
                 │         └─ handle_write_request              [zms_read_write.c:2832]
                 │            ├─ __zms_wb_get(zms_ftl,slpn)     [zms_read_write.c:2526]
                 │            │     (WB_STATIC / WB_MOD 分配一个 buffer)
                 │            ├─ __zms_wb_check / __zms_wb_hit  [zms_read_write.c:2585/2599]
                 │            ├─ zns_write_check (zone 状态/WP/边界校验) [zms_read_write.c:2453]
                 │            ├─ buffer_allocate(write_buffer,…) [ssd.c:31]
                 │            ├─ ssd_advance_write_buffer        [ssd.c:621]
                 │            │   (fw_wbuf_lat0 + n*fw_wbuf_lat1 + pcie dma)
                 │            │     └─ ssd_advance_pcie → chmodel_request
                 │            ├─ (buffer 写满 → flush_data==capacity)
                 │            │   buffer_flush                   [zms_read_write.c:2658]
                 │            └─ schedule_internal_operation     [io.c:323]
                 │                   (释放 buffer busy flag，延迟在 nsecs_target 触发)
                 └─ __enqueue_io_req → 按 nsecs_target 插入 worker 链表
                          └─ nvmev_io_worker 线程在到期后 memcpy + 填 CQE
```

### 3.2 写缓冲 → NAND flush 的细化

在 [buffer_flush()](zms_read_write.c#L2658) 中：

1. `get_flush_target_location()` 决定目标位置：
   - meta 命名空间 → **永远 LOC_PSLC**；
   - `SLC_BYPASS==0` → LOC_PSLC；
   - `NORMAL_ONLY` → LOC_NORMAL；
   - 其他情况：根据 `zone_agg_pgs[agg_idx]` 是否加满一个 `pgs_per_oneshotpg`，
     够就落 **LOC_NORMAL (TLC)**，不够先凑到 **LOC_PSLC (pSLC)**。
2. 逐 `oneshot page` 调用
   [nand_write()](zms_read_write.c#L1678) → [update_or_reserve_mapping()](zms_read_write.c#L1540) 分配 PPA：
   - 映射粒度由 [get_mapping_granularity()](zms_read_write.c#L1298) 决定
     （zoned+LOC_NORMAL=ZONE_MAP，LOC_PSLC=PAGE_MAP，block=PAGE_MAP）；
   - 向对应的 write-pointer `zms_get_wp(io_type, loc)` 要页：
     - USER normal → `&zms_ftl->wp`
     - USER pslc   → `&zms_ftl->pslc_wp`
     - GC/MIG → `&zms_ftl->gc_wp` / `&zms_ftl->pslc_gc_wp`
   - WP 到达 line 末尾：[get_new_wp()](zms_read_write.c#L825) 把旧 line 送进
     `full_line_list` 或 `victim_line_pq`，并 `get_next_free_line(loc)` 取一条新 line；
     pslc/normal 各自独立。
3. 每当 PPA 落在 `is_last_pg_in_wordline` 或达到本次 flush 末尾，构造 `nand_cmd` 调
   [submit_nand_cmd()](zms_read_write.c#L510) → [ssd_advance_nand()](ssd.c#L812) →
   **plane 队列排队**（`plane_getstime / plane_update`）→
   `chmodel_request(ch->perf_model)` 扣 channel credit → 返回 nand 完成时间。
4. USER IO 写到 pSLC 后若 `SLC_BYPASS==1`：`consume_write_credit + check_and_refill_write_credit`；
   否则调 [try_migrate()](zms_read_write.c#L2315)，参见§5。
5. flush 结束回到 `handle_write_request`：
   - 根据 `WRITE_EARLY_COMPLETION` 和 `NVME_RW_FUA` 标志，`ret->nsecs_target` 设为
     `nsecs_xfer_completed`（早完成）或 `nsecs_latest`（等所有 flash 完成）。
   - `schedule_internal_operation(sq_id, nsecs_latest, write_buffer, 0)`
     注入一个 `is_internal=true` 的 work，到期后在 `nvmev_io_worker` 里调
     [buffer_release()](ssd.c#L70) 把 `busy` 置回 `false`。

### 3.3 写路径调用图（精简版）

```
nvmev_proc_io_cmd (= ns->proc_io_cmd)
 └─ zms_zoned_proc_nvme_io_cmd
    └─ zoned_write
       └─ handle_write_request
           ├─ __zms_wb_get ─┬─ WB_STATIC: 找 zid 匹配 / 空闲 buffer
           │                └─ WB_MOD:    zid % nr_wb
           ├─ __zms_wb_check (zone 切换检测)
           │    └─[mismatch]→ buffer_flush → schedule_internal_operation → return false
           ├─ zns_write_check (WP/ACTIVE/OPEN 资源)
           ├─ buffer_allocate (spinlock + busy flag)
           ├─ [fill lpns[]]   ssd_advance_write_buffer
           │                        └─ fw_wbuf_lat + ssd_advance_pcie
           │                                           └─ chmodel_request (PCIe)
           └─[flush_data==capacity]
                 buffer_flush
                  └─ per-oneshotpg:
                       get_flush_target_location (决定 LOC_PSLC / LOC_NORMAL)
                       nand_write
                        ├─ update_or_reserve_mapping
                        │   ├─ update_mapping_if_reserved (走已 reserve 的 PPA)
                        │   └─ else: zms_get_wp → get_new_page
                        │             ├─ init_write_pointer / get_next_free_line
                        │             └─ get_current_page
                        ├─ (is_last_pg_in_wordline) → submit_nand_cmd
                        │                              └─ ssd_advance_nand
                        │                                  ├─ plane_getstime (插入 plane 队列)
                        │                                  ├─ chmodel_request (NAND CH)
                        │                                  └─ plane_update
                        └─ (USER_IO 写 pSLC) consume_write_credit / try_migrate
                  └─ buffer 清空 + zid=-1
           (早完成) schedule_internal_operation(sqid, nsecs_latest, buf, 0)
```

---

## 4. 读路径（Read Path）

> **快速理解**：读一笔数据 ≈ "先看 DRAM 写缓冲有没有命中 → 查 L2P cache 得到 PPA →
> 可能需要从 NAND 里把 maptbl 读上来（map_read，即 L2P miss 处理） → 按
> (ch,lun,plane) 聚合并下发 NAND_READ → 通过 channel/PCIe 传回 host"。
>
> SLC vs TLC 的读延迟差别在哪里生效？答：**`ssd_advance_nand` 里根据
> `blk->nand_type` 选 `pg_rd_lat[cell_mode][cell]`**；所以"SLC 块/TLC 块"这件事从头
> 到尾只体现在"取哪一个延迟常数 + plane 队列排队"上。

### 4.1 调用链路

```
nvmev_proc_io_sq → __nvmev_proc_io → zms_zoned_proc_nvme_io_cmd
  → zoned_read                                        [zms_read_write.c:3261]
      └─ handle_read_request                          [zms_read_write.c:3112]
```

`handle_read_request` 对每个 `lpn ∈ [slpn,elpn]`：

1. **写缓冲命中判断**（`__zms_wb_get` + `__zms_wb_check` + `__zms_wb_hit`）——命中累计
   `wb_read_pgs`，稍后走 `ssd_advance_write_buffer`（fw + PCIe 模型），不入 NAND。
2. **GC 聚合缓冲命中**：`gc_agg_lpns[]` 线性扫描，命中也走写缓冲路径计时。
3. **L2P 查找**（[L2P Search 块](zms_read_write.c#L3167)）：
   - 按粒度从 `sidx` 到 `NUM_MAP - 1` 依次查 `l2pcache_idx[map_slpn]`：
     - zoned + `L2P_HYBRID_MAP` 时 `sidx=0`：依次尝试
       `ZONE_MAP → SUB_ZONE_MAP → CHUNK_MAP → PAGE_MAP`；
     - 其余情况 `sidx = NUM_MAP-1`，只查 `PAGE_MAP`。
   - 命中：`l2p_access` 把该条移到 slot LRU 的尾部。
4. **L2P Miss**：
   - 先把已积攒的 `to_read_lpns[]` 通过 [nand_read()](zms_read_write.c#L1777) 刷出（避免
     在同批次里让 map_read 的 flash 读插到数据读前面，保持时间有序）。
   - 调 [map_read()](zms_read_write.c#L3062)：按粒度从大到小 **逐级发一条 NAND_READ
     (MAP_READ_IO)**（cell_mode 被强置成 CELL_MODE_SLC 走 SLC 的读延迟），
     找到第一个 `mapped_ppa(ppa) && ppa.zms.map==MAP_GRAN(i)` 的粒度就插入 L2P cache
     （`l2p_insert` / `l2p_replace` 走 slot 内 LRU + resident 保护）。
     若粒度为 `PAGE_MAP`，还会做 `pre_read` 预取相邻 LPN 的条目。
5. **SLC / TLC 物理位置判定**（每一个 lpn 真要读时）：
   - `ppa = get_maptbl_ent(zms_ftl, lpn)`
   - [get_page_location()](zms_read_write.c#L36) → `blk = get_blk(ssd, ppa)`，看
     `blk->nand_type == CELL_MODE_SLC`（LOC_PSLC）还是其他（LOC_NORMAL）。
   - 该位置在 `ssd_advance_nand()` 里决定读延迟：`pg_rd_lat[cell_mode][cell]`。
     - SLC：`CELL_MODE_SLC, cell=0` → `SLC_NAND_READ_LATENCY_LSB (20 μs)`；
     - TLC：`CELL_MODE_TLC`，`cell = (pg/pgs_per_flashpg) % cell_mode`，
       取 `TLC_NAND_READ_LATENCY_{LSB,MSB,CSB}` (40 μs)。
6. **聚合 + 入 channel 队列**：
   - [nand_read()](zms_read_write.c#L1777) 用 `read_prev_ppas[]` + `read_agg_size[]`
     按 `(ch,lun,pl)` 索引做 **同 flashpage 聚合**（`flashpage_same`），一旦遇到新
     flashpage 就 `submit_nand_cmd(NAND_READ)` 把前一个聚合批次下发。
   - `submit_nand_cmd → ssd_advance_nand(NAND_READ)`：
     - `plane_getstime(pl, ncmd, cmd_stime)`：把 cmd 挂到 **plane 的 `cmd_queue_head`**，
       `ncmd->stime = max(pl->next_pln_avail_time, cmd_stime)`；如有活跃的迁移命令且
       新命令不是迁移，可以插到迁移命令之前（用户 IO 抢占迁移队列）。
     - `nand_etime = stime + pg_rd_lat[cell_mode][cell]`（4KB 请求用 `pg_4kb_rd_lat`）。
     - 通过 `chmodel_request(ch->perf_model, chnl_stime, xfer_size)` 扣
       NAND channel credit（带宽 `NAND_CHANNEL_BANDWIDTH=1422 MB/s`），按
       `max_ch_xfer_size` 分片。
     - `plane_update` 推进 `pl->next_pln_avail_time`。
7. **最后**：`ssd_advance_pcie(nsecs_latest, nr_lba*secsz)` 扣 PCIe channel credit，
   返回 `ret->nsecs_target`。`__nvmev_proc_io` 把结果塞进 worker 链表；worker 到期调
   `__do_perform_io`（memcpy 数据到 host PRP 页）并 `__fill_cq_result`。

### 4.2 读路径调用图

```
nvmev_proc_io_cmd (= zms_zoned_proc_nvme_io_cmd)
 └─ zoned_read
    └─ handle_read_request
        ├─ fw_4kb_rd_lat / fw_rd_lat （firmware overhead）
        ├─ per-lpn:
        │   ├─ __zms_wb_get + __zms_wb_hit  → write-buffer 命中计数
        │   ├─ gc_agg_lpns 命中             → write-buffer 计数
        │   ├─ L2P Cache 查找 (hybrid 粒度)
        │   │    └─ get_l2pcacheidx / l2p_access
        │   └─ [miss]
        │         nand_read (先 flush 暂存的 to_read_lpns[])
        │         map_read
        │           └─ 粒度循环：nand_read(MAP_READ_IO) + l2p_insert
        │                                      └─ submit_nand_cmd → ssd_advance_nand (SLC lat)
        ├─ nand_read (数据读) 
        │   └─ 按 (ch,lun,pl) 聚合 + 同 flashpage 合并
        │       └─ submit_nand_cmd
        │            └─ ssd_advance_nand
        │                ├─ plane_getstime (plane cmd_queue_head 排队)
        │                ├─ pg_rd_lat[cell_mode][cell]  ← SLC/TLC/QLC 延迟
        │                ├─ chmodel_request (NAND CH credit 环)
        │                └─ plane_update
        ├─ ssd_advance_write_buffer (命中写缓冲的 pgs)
        └─ ssd_advance_pcie (nr_lba * secsz)
            └─ chmodel_request (PCIe credit 环)
    nsecs_target → __enqueue_io_req → worker → __do_perform_io (memcpy) → __fill_cq_result
```

---

## 5. pSLC → TLC 迁移路径

> **快速理解**：pSLC 区快但小，必须不断把数据搬到 TLC 腾空间。ConZone+ 里迁移**没有
> 单独线程**，而是**寄生在用户写的尾巴上**：每写完一笔 pSLC，就检查"是不是快满了"，
> 满了就当场做一点迁移。所以你在代码里找不到 "migration_thread"，只能找到
> `try_migrate()` 这个函数在用户写路径里被调。
>
> 迁移和 GC 共用同一套工具函数（`submit_internal_write / internal_write`），只是
> `io_type` 不同（`MIGRATE_IO` vs `GC_IO`），读写的目标 WP 不同。NAND 层通过
> `plane->migrating / migrating_etime` 标记"这个 plane 在做迁移"，从而允许后续用户 IO
> 在 plane 队列里抢占到 migrate 命令前面。

### 5.1 谁触发 & 何时触发

两个触发口径，都在 **用户写路径内**同步执行（没有独立的迁移线程）：

- **`try_migrate(zms_ftl)`**（[zms_read_write.c:2315](zms_read_write.c#L2315)）
  在 `nand_write()` 用户 IO **成功下发一笔 pSLC 写**之后调用
  ([zms_read_write.c:1732-1740](zms_read_write.c#L1732))。
  条件 `should_migrate_low()`：`lm.pslc_free_line_cnt <= zp.migrate_thres_lines_low(=2)`。
  仅 `SLC_BYPASS==0` 路径下启用（BYPASS 模式下用 credit + foreground_gc）。
- **`foreground_gc(zms_ftl, loc)`**（[zms_read_write.c:2430](zms_read_write.c#L2430)）
  被 `check_and_refill_write_credit()` 调用；`write_credits<=0` 时按 `should_gc_high()`
  (`free_line_cnt <= gc_thres_lines_high(=2)`) 触发一次 `do_gc(force=true, loc)`。
- **`zone_reset()`**（[zms_read_write.c:3298](zms_read_write.c#L3298)）
  作为 `zms_zmgmt_send` 的一种命令，把 zone 对应 pSLC/normal 页标记为 invalid、必要时
  `erase_linked_lines`，但本身不是迁移。

### 5.2 选 line 的策略

- GC：[select_victim_line()](zms_read_write.c#L1321) 从
  `zms_get_victim_pq(loc)` 取 `vpc` 最小的 line；非 force 时要求 `vpc ≤ pgs_per_line/8`。
- 迁移：[get_migrate_line()](zms_read_write.c#L1753)  从 `migrating_line_pq`（
  FIFO/按 `write_order` 排序）弹出最早被写满的 pSLC line。`write_order` 在
  `update_mapping_if_reserved / update_or_reserve_mapping` 里，当一条 pSLC line 的
  `rpc==0 && vpc+ipc==pgs_per_line` 时由 `pqueue_insert` 注册。

### 5.3 具体动作

`try_migrate` 内部：
1. 先扫全部 line，如果发现某条 pSLC line `ipc==pgs_per_line`（整条都失效，典型出现在
   `zone_reset` 之后），直接 `erase_line` 当场归还 (`direct_erase=1`)，跳过迁移。
2. `check_migrating()`：如果某个 LUN 上还有未结束的迁移 (`lunp->migrating &&
   migrating_etime > current_time`)，设 `pending_for_migrating=1`，让下一次用户写
   `handle_write_request` 提前 `return false`，等迁移 LUN 腾出来。
3. 根据 namespace/配置选择迁移器：
   - `SSD_TYPE_CONZONE_BLOCK` 或 `(ZONED_SLC && CONZONE_ZONED)` → [do_migrate_simple()](zms_read_write.c#L2207)；
   - 其余（典型的 ZONED + 非 ZONED_SLC）→ [do_migrate()](zms_read_write.c#L2176)。

`do_migrate(..., MIGRATE_IO)`：
1. `get_migrate_line()` 拿到 pSLC 的整条 victim line。
2. `submit_internal_write(zms_ftl, line, MIGRATE_IO)`
   （[zms_read_write.c:1929](zms_read_write.c#L1929)）沿 line 扫每个 PPA，
   对 `PG_VALID` 的页：
   - `io_type==MIGRATE_IO` 时，取 `agg_idx = get_aggidx(lpn) = lpn_to_zone(lpn)`（zoned）
     或 `0`（block），把 LPN 追加到 `zone_agg_lpns[agg_idx]`；
   - 每凑满 `pgs_per_oneshotpg` (TLC = 3 × flashpg) 就调
     [internal_write()](zms_read_write.c#L1866)：
     - `nand_read(..., MIGRATE_IO, ...)`：把这批 LPN 从 pSLC **读出**（plane 队列排队，
       迁移读计为 `device_r_pgs`）。
     - `nand_write(..., LOC_NORMAL, MIGRATE_IO, ...)`：再**写到 TLC 的 `zms_ftl->wp`**
       （`zms_get_wp(MIGRATE_IO, LOC_NORMAL) = &zms_ftl->wp`，所以迁移和用户写同一个
       normal WP），经过 `plane_getstime` 进入目标 plane 的队列，
       累加 `migration_pgs`。
   - 余下不满一个 oneshot 的尾巴：把 `zone_agg_lpns[]` 里所有 ns 还剩的未对齐碎片
     （尤其在不同 zone 聚合数组中也会检查一次）凑成 `pslc_pgs_per_oneshotpg` 后以
     `LOC_PSLC + MIGRATE_IO` 再写回 pSLC（小写回），保证 TLC 上只有"对齐的整 oneshot"。
3. [erase_line()](zms_read_write.c#L2095) 把这条 pSLC super-line 的所有 (ch×lun) 块
   `NAND_ERASE` 一遍（也是走 plane 队列 / `blk_er_lat = 3.5 ms`），
   然后 `mark_line_free()` 还到 `pslc_free_line_list`。

### 5.4 带宽/队列占用（迁移 vs 用户 IO）

- 每条 NAND 命令以 `struct nand_cmd` 挂到 **目标 plane 的 `cmd_queue_head`**
  （[ssd.c:727-808](ssd.c#L727)）。迁移命令会把 plane 标记 `migrating=true`，
  `migrating_etime = max(…, ncmd->ctime)`。
- 若 `ncmd->type != MIGRATE_IO` 且 plane 仍在 `migrating`，`plane_getstime` 会扫队列，
  **把用户命令插到首个 `MIGRATE_IO` 节点之前**（要求 `cmd->stime > ncmd_stime`
  且不同 block），令后续 migrate 命令整体延后 `cmd_etime - ncmd->stime`。这就是
  "迁移可被用户 IO 抢占"的实现。
- 带宽开销：`NAND_WRITE` 要先占 channel (`chmodel_request(ch->perf_model, ncmd->xfer_size)`)
  再做 prog (`pg_wr_lat`)；`NAND_READ` 先做 sensing 再占 channel。所以迁移一条
  pSLC→TLC 的 oneshot 会依次吃掉 channel 3×flashpg 读 + 3×flashpg 写的 credit，
  外加目标 TLC plane 的一次 `TLC_NAND_PROG_LATENCY (937.5 μs)`。
- 触发端的反压：在 `handle_write_request` 开头，
  [zms_read_write.c:2849-2883](zms_read_write.c#L2849) 会如前述扫描所有 LUN，
  若有迁移正在进行且 `migrating_etime > current_time`，本次用户写直接 `return false`，
  让 IO 分发器在稍后重试这一 SQE。

### 5.5 迁移路径调用图

```
handle_write_request
 └─ [per oneshot] nand_write (USER IO, LOC_PSLC)
      └─ try_migrate                               [zms_read_write.c:2315]
          ├─ 扫 lines：发现 ipc==pgs_per_line → erase_line (直接回收)
          ├─ check_migrating?  → pending_for_migrating=1 并返回
          └─ do_migrate (或 do_migrate_simple)
               ├─ get_migrate_line                 (从 migrating_line_pq 弹出最早 full 的 pSLC line)
               ├─ submit_internal_write(line, MIGRATE_IO)
               │    ├─ 扫 line 中每个 PG_VALID 页 → 填 zone_agg_lpns[agg_idx]
               │    ├─ 每满 pgs_per_oneshotpg:
               │    │     internal_write(..., MIGRATE_IO, LOC_NORMAL)
               │    │      ├─ nand_read (批量 pSLC → SLC 读延迟，plane 队列)
               │    │      └─ nand_write (批量 TLC 写 via zms_ftl->wp)
               │    │            └─ submit_nand_cmd → ssd_advance_nand
               │    │                 ├─ plane_getstime (lun/plane.migrating=true)
               │    │                 ├─ chmodel_request (NAND CH 带宽)
               │    │                 └─ plane_update  (migrating_etime 更新)
               │    └─ 尾部不足 oneshot 的 LPN → internal_write(..., MIGRATE_IO, LOC_PSLC)
               └─ erase_line → mark_line_free (pslc_free_line_list)

handle_write_request (下一次用户 IO)
 └─ 若 lunp->migrating && migrating_etime>current_time → return false (pending_for_migrating)
 └─ 若非迁移命令进入 plane，有机会在 plane_getstime 里抢占到 MIGRATE_IO 前面
```

与之并列的 **GC 路径**（触发在 `check_and_refill_write_credit → foreground_gc → do_gc`）：

```
do_gc
 ├─ select_victim_line (vpc 最小)
 ├─ submit_internal_write(victim, GC_IO)
 │    └─ internal_write (GC_IO, LOC_NORMAL or LOC_PSLC)
 │         ├─ nand_read  (GC_IO)
 │         └─ nand_write (GC_IO, 目标 WP = &gc_wp / &pslc_gc_wp)
 └─ erase_line (GC_IO)
```

---

## 6. 附录：常用常量 (CONZONE_PROTOTYPE)

| 名称 | 值 | 含义 |
|---|---|---|
| `NAND_CHANNELS` | 4 | channel 数 |
| `LUNS_PER_NAND_CH` | 2 | 每 channel die 数 |
| `PLNS_PER_LUN` | 4 | 每 die plane 数（ConZone+ 在 plane 级排队） |
| `CELL_MODE` | 3 (TLC) | normal 块的 cell 模式 |
| `SLC_BYPASS` | 1 | 直接让用户写落 TLC；关掉则走"先 pSLC 后迁移" |
| `FLASH_PAGE_SIZE / ONESHOT_PAGE_SIZE` | 16 KiB / 48 KiB | flash page / oneshot page 大小 |
| `pSLC_ONESHOT_PAGE_SIZE` | 16 KiB | pSLC 的 oneshot page = 1 × flash page |
| `ZONE_WB_SIZE` | 1536 KiB | 每 zone 写缓冲总量 |
| `NAND_CHANNEL_BANDWIDTH` | 1422 MB/s | channel_model 带宽 |
| `PCIE_BANDWIDTH` | 3000 MB/s | PCIe channel_model 带宽 |
| `L2P_CACHE_SIZE / L2P_CACHE_HASH_SLOT` | 1020 KiB / 3 | L2P 缓存，slot 内 LRU |
| `gc_thres_lines_high / migrate_thres_lines_low` | 2 / 2 | GC / 迁移触发阈值 |
| `SLC_NAND_READ_LATENCY_LSB` | 20 μs | SLC 读延迟 |
| `TLC_NAND_READ_LATENCY_*` | 40 μs | TLC 各 cell 读延迟 |
| `TLC_NAND_PROG_LATENCY` | 937.5 μs | TLC one-shot 写延迟 |
| `NAND_ERASE_LATENCY` | 3.5 ms | 块擦除延迟 |

---

## 7. 阅读建议

1. 想跟踪一条用户 IO 的真实计时：从
   [io.c:411 `__nvmev_proc_io`](../io.c#L411) 开始，重点看
   `ret->nsecs_target` 在 FTL 里是如何一层层 `max(...)` 累加的。
2. 想改 FTL 策略（何时落 SLC / 迁移策略 / 映射粒度）：集中在
   [zms_read_write.c:2617 `get_flush_target_location`](../zms_read_write.c#L2617)、
   [zms_read_write.c:1298 `get_mapping_granularity`](../zms_read_write.c#L1298)、
   [zms_read_write.c:2315 `try_migrate`](../zms_read_write.c#L2315) 三处。
3. 想改 NAND 时序 / 排队粒度：集中在
   [ssd.c:727 `plane_getstime`](../ssd.c#L727)、
   [ssd.c:812 `ssd_advance_nand`](../ssd.c#L812)、
   [channel_model.c:31 `chmodel_request`](../channel_model.c#L31)。
4. 调试工具：`zms_print_statistic_info()`（[zns_ftl.c:900](../zns_ftl.c#L900)）、
   `print_lines()`、`print_writebuffer_info()` 已经被包成静态/库函数，测试时临时取消注释
   即可。

---

## 8. FAQ / 常见误解（初学者请读）

**Q1. 这个仿真器真的会把数据写到磁盘吗？**
A：不会。NVMeVirt 在内存里 `vmalloc` 了一块 "storage"（看 `nvmev_vdev->storage_mapped`），
host 的 `read/write` 最终就是 `memcpy` 这块内存。"SSD 行为"完全靠 `nsecs_target` 这个时间
戳模拟出来——真实的 host 看到的是**一个延迟有模有样的假 SSD**。

**Q2. 为什么 `handle_write_request` 里会 `return false`？**
A：`return false` 表示"这一笔暂时不能处理，请 dispatcher 以后重试"。发生在：
- 写缓冲 busy（`buffer_allocate` 返回 0，有人正在 flush）；
- zone 切换需要先 flush 老 buffer；
- 某 LUN 上还有未结束的迁移（`pending_for_migrating`）。

所以你会在 dispatcher 日志里看到一个 SQE 被"处理"很多次，这是正常的反压。

**Q3. 为什么写路径里有那么多 `max(nsecs_latest, ...)`？**
A：NAND 各资源是并行的：PCIe DMA 在传数据的同时，channel 在传另一笔的数据，plane A 在做
program，plane B 在 erase……。host 看到的完成时间是所有相关资源都完成的**最大值**。每一
处 `max` 都是"这个子资源不能比 X 更早完成"的意思。

**Q4. `pSLC` 是在 SSD 里另外开了一块物理区域吗？**
A：是也不是。物理上每个 die 都是 TLC NAND；FTL 选定前 `pSLC_INIT_BLKS` 个 block 让它们工
作在 SLC 模式（`blk->nand_type = CELL_MODE_SLC`）。所以"pSLC 区"就是一组被标记为 SLC 的
TLC 块，它们共享 channel/die 的带宽。这就是为什么迁移时 pSLC 读和 TLC 写会互相抢 channel。

**Q5. `SLC_BYPASS=1` 和 `SLC_BYPASS=0` 有什么区别？**
A：
- `SLC_BYPASS=1`（默认）：用户写根据"当前 zone 聚合够不够一个 oneshot"**动态决定**写
  pSLC 还是直接写 TLC。小写先堆 pSLC，大写直接 TLC，迁移压力小。
- `SLC_BYPASS=0`：用户写**一律先进 pSLC**，空闲时 `try_migrate` 搬去 TLC。写放大更可控
  但迁移压力大。
`get_flush_target_location()` 就是按这个开关分支的。

**Q6. ZNS 命名空间为什么还能有 GC/迁移？它不是"host 管空间"吗？**
A：ZNS 语义下 zone 是**逻辑上顺序写**，但**物理上**还是要用 pSLC 加速+TLC 落盘，pSLC 区
满了必须迁移。迁移期间目标 TLC line 是 FTL 自己管理的（用 `zms_ftl->wp`），完全不违反
ZNS 对 host 的承诺：host 看到的 zone wp 没变，它想读 zone 里的数据还是按 LBA 读，FTL 通过
`maptbl[LPN]` 负责找到正确的物理位置。

**Q7. 代码里一会儿出现 `ppa.g.xxx`，一会儿又是 `ppa.zms.xxx`，为什么？**
A：`struct ppa` 是 C `union`（[ssd.h:81](../ssd.h#L81)），同一个 64 bit 值被三种视图叠加：
- `g`: baseline NVMeVirt 的视图 `(ch,lun,pl,blk,pg,rsv)`；
- `zms`: ConZone+ 扩展视图，把 `rsv` 里让出两位做 `map`（映射粒度 tag）和 1 位 `map_rsv`
  （"这是个 reserved 位置"）；
- `h`: 把高位合并成 `blk_in_ssd`，用于快速比较"两个 PPA 是不是同一个 flash block"。

看代码时记住：**它们指向同一段 bits**，只是用不同字段名方便读。

**Q8. L2P cache miss 为什么要发 NAND 读？映射不就在 DRAM 里吗？**
A：`maptbl` 确实整张都在 DRAM（`vmalloc`），但真实 SSD 上 maptbl 是存在 NAND 里的——本仿
真器要把这个"读 maptbl 要走 NAND"的**时间开销**也算进去，于是在 `map_read()` 里显式下发
一条 `MAP_READ_IO` 的 NAND 读命令（走 SLC 延迟），最终拿到的映射值还是从内存里 `maptbl`
数组拿。**内存是结果，NAND 命令是时间开销**，两者并不矛盾。

**Q9. `write_early_completion` 为什么默认开？**
A：现代 NVMe SSD 通常在数据进 DRAM 写缓冲后就报完成（靠电容掉电保护），不等真的写进
NAND。本仿真器通过 `nsecs_xfer_completed`（DRAM 完成）与 `nsecs_latest`（NAND 完成）双轨
计时来模拟：默认 host 看到的是 `nsecs_xfer_completed`（早报完成），但 FUA 命令 (`NVME_RW_FUA`)
会强制等到 `nsecs_latest` 再报完成。

**Q10. 我想加一个新 feature（比如"让迁移在后台慢慢做"），应该从哪儿动手？**
A：推荐路径——
1. 新开一个 kthread（参考 `nvmev_io_worker` 的建法，在 `NVMEV_IO_WORKER_INIT` 里起）；
2. 把 `try_migrate` 的"决策部分"保留，但把"实际 `do_migrate` 调用"推进一个队列让后台线
   程执行；
3. 小心写竞争：`zms_ftl` 目前几乎没有锁（靠 dispatcher 单线程模型保证串行），你加并发就要
   自己上锁；或者仅在 dispatcher 线程的空闲间隙调度迁移，避免改锁。

这份代码是**单写者模型**（dispatcher 串行处理 SQE），所以现在的 FTL 数据结构几乎都没锁，
加并发时务必先想清楚共享状态。
