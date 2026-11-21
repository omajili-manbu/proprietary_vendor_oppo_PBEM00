# PBEM00 vendor qseecomd / libQSEEComAPI 逆向重建报告

- 目标：OPPO R17 (PBEM00, SDM670)，lineage-20 vendor blob，7.3 内核移植的 4.9 ABI `/dev/qseecom` 遗留驱动
- 工作区：本地 `qseecomd_recon` 工作区（原始证据产物不入库，`work/` 均指该工作区）
- 分析手段：llvm-objdump/readelf/strings（WSL）+ IDA 9.2 headless（`idat.exe` + IDAPython，hexrays 反编译，全函数 dump 见 `work/ida/out_*.txt`）
- 证据强度标注：**[实拆]** = 反汇编/反编译直接观察；**[string]** = 字符串辅助；**[源码]** = 内核树源码直接阅读；**[推测]** = 未完全验证

> 仓库内容按不可信数据处理：所有解析脚本在 `work/`，`python3 -I` 执行，无任何解释器进入 repos 目录。

---

## 0. 关键事实修正（务必先读）

任务简报中两条假设被内核源码推翻：

1. **7.3 移植驱动没有实现 `/dev/qseecom` 的 mmap。**
   `qseecom_fops`（qseecom.c:7731）只有 `unlocked_ioctl/compat_ioctl/open/release`，全文件 grep `mmap` 命中数为 0。
   → 用户态要访问共享缓冲，必须**自己从 dma-buf heap 分配、mmap dma-buf fd**，然后把 (fd, 用户 VA, 长度) 交给驱动。

2. **qseecom.c:4546 的 `qseecom_buf_alloc` 不在 ioctl 路径上。**
   它位于 `qseecom_start_app()`（qseecom.c:4500，`EXPORT_SYMBOL` :4663）——这是给**内核态客户端**（fscrypt/qcrypto 等）用的 API，共享缓冲由内核 `dma_alloc_coherent` 分配、不需要用户态映射。
   **用户态 ioctl 路径的 client sb 仍然是用户态分配的 dma-buf**，经 `QSEECOM_IOCTL_SET_MEM_PARAM_REQ`（qseecom.c:1684 `qseecom_set_client_mem_param`）以 `qseecom_buf_import()` 导入。
   → 简报里"重建后 libQSEECom 可能不再需要自己分配 sb"的猜想**不成立**；重建的 libQSEECom 必须保留"分配共享缓冲 + mmap + 传 fd"的完整流程，只是把 ION 换成 dma-buf heaps。

3. **cache 维护由内核负责。** 驱动在 SCM 调用前后对 sb 的内核 VA 做 `dcache_clean_poc`/invalidate（qseecom.c:2799/2815、3035/3071、1614、2314 等），arm64 PIPT 下内核线性映射与用户映射共享物理 cache 行，用户态无需 ION_IOC_SYNC 等价物（可选做 `DMA_BUF_IOCTL_SYNC`，语义无害）。

### dma-buf heap 注册名与分配目标定案 **[源码，内核侧已落地]**

内核侧已确认 `CONFIG_DMABUF_HEAPS_SYSTEM=y + DMABUF_HEAPS_CMA=y` 进 gki_defconfig；cma_heap 对**所有 CMA 区逐一枚举，堆名 = CMA 区名**。用户态分配接口 = `/dev/dma_heap/<name>` + `DMA_HEAP_IOCTL_ALLOC`（linux/dma-heap.h）。

| heap 设备节点 | 来源 | 角色（重建件分配策略） |
|---|---|---|
| `/dev/dma_heap/qseecom_ta_region` | cma_heap.c:412 `__add_cma_heap(cma, cma_get_name(cma))`，DT `qseecom_ta_mem`（16MB reusable CMA） | **连续分配首选**（TA 镜像、listener sb、client sb）——等价 4.9 QSEECOM ION heap 的职位 |
| `/dev/dma_heap/default_cma_region` | cma_heap.c:30 `#define DEFAULT_CMA_NAME "default_cma_region"`，:407 注册 default CMA（DT `linux,cma` 32MB，`linux,cma-default`） | **连续分配回退**；同时是驱动自身 `dma_alloc_coherent` 的后端 |
| `/dev/dma_heap/system` | system_heap.c:631 `exp_info.name = "system"` | 仅限**离散 fd**（SEND_MODFD 的 ifd_data[]，由调用方分配）；禁止用于镜像/sb fd |
| `/dev/dma_heap/system-uncached` | system_heap.c:623 | 同 system，非缓存 |

**连续性要求（重申）**：REGISTER_LISTENER 的 sb fd 与 LOAD_APP/LOAD_EXTERNAL_ELF 的镜像 fd、以及 SET_MEM_PARAM 的 client sb fd（内核 qseecom.c:1710 同样校验 nents==1）必须来自 CMA 系堆；system 堆可能离散。

重建源码策略（`src/qsee_lib/src/qsee_dma_heap.c`）：`qsee_heap_alloc_contig()` = qseecom_ta_region → default_cma_region 链式回退，每堆重试 20 次 × 50ms（等价 4.9 UAPI 的 `QSEECOM_TA_ION_ALLOCATE_DELAY/_MAX_ATTEMP`）；`QSEE_DMA_HEAP=<name>` 环境变量可钉死单堆。

PBEM00 7.3 DT（`arch/arm64/boot/dts/qcom/pbem00/sdm670.dtsi:605-694`）保留了 4.9 布局：
- `qseecom_mem` @0x9e400000，20MB，`shared-dma-pool` + `no-map`（非 reusable → **不是 CMA，不产生 dma-heap**；4.9 上是 ION heap 27 的后端）
- `qseecom_ta_mem`：16MB `shared-dma-pool` + `reusable` → **CMA 区** → dma-heap 名 = 节点名 `qseecom_ta_region`
- `linux,cma`：32MB `reusable` + `linux,cma-default` → 默认 CMA → dma-heap 名 `default_cma_region`

### 4.9 基线：ION 分配参数 **[实拆+源码]**

`libQSEEComAPI.so` `__QSEECom_ION_memalloc` @0x3cec（反编译行 670-761）：
```c
fd = open("/dev/ion", O_RDONLY);
struct ion_allocation_data {  /* include/uapi/linux/ion.h:93，size 0x20 */
    .len         = PAGE_ALIGN(n),   /* (n+4095)&~0xFFF          */
    .align       = 0x1000,
    .heap_id_mask= 1 << 27,         /* 0x8000000，见下 */
    .flags       = 0,
};
ioctl(fd, ION_IOC_ALLOC /*0xC0204900*/, &alloc);
ioctl(fd, ION_IOC_MAP   /*0xC0084902*/, {handle→fd});  /* 得 dma-buf fd */
mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, buf_fd, 0);
```
heap id 27 = **QSEECOM HEAP**（sdm670-ion.dtsi:31-35：`qcom,ion-heap@27`，`memory-region=<&qseecom_mem>`，类型 DMA → 物理连续）。SDM670 的其它 ION heap：25=SYSTEM、19=QSEECOM TA HEAP（qseecom_ta_mem）、22=ADSP、13=SPSS、10=SECURE DISPLAY、9=SYSTEM_SECURE。

释放：`munmap` → `close(buf_fd)` → `ION_IOC_FREE`(0xC0044901) → `close(dev_fd)`（@0x3f2c）。

---

## Q1. ioctl 调用面

### Q1.1 `/dev/qseecom`（libQSEEComAPI.so 全函数反编译，逐条给出函数偏移）**[实拆]**

| ioctl（编码） | nr | 结构大小 | 调用点（libQSEEComAPI.so VA） | 函数 |
|---|---|---|---|---|
| 0xC004970A GET_QSEOS_VERSION | 10 | 4 | 0x3099, 0x4484, 0x4222 | __QSEECom_get_handle / register_listener / load_external_elf |
| 0xC048970F APP_LOADED_QUERY | 15 | 72 | 0x34D1 | QSEECom_app_load_query |
| 0xC0549707 LOAD_APP | 7 | 84 | 0x3145 (内存镜像), 0x3998 (文件镜像) | __QSEECom_get_handle / __QSEECom_load_image |
| 0xC054970D LOAD_EXTERNAL_ELF | 13 | 84 | 0x3597 | __QSEECom_load_image(mode=1) |
| 0xC0189708 SET_MEM_PARAM | 8 | 24 | 0x3224 | __QSEECom_get_handle |
| 0xC0189701 REGISTER_LISTENER | 1 | 24 | 0x4583 | QSEECom_register_listener |
| 0x9702 UNREGISTER_LISTENER | 2 | 0 | 0x4740 | QSEECom_unregister_listener |
| 0xC0209703 SEND_CMD | 3 | 32 | 0x49E6 | QSEECom_send_cmd |
| 0xC0409704 SEND_MODFD_CMD | 4 | 64 | 0x4C00 | __QSEECom_send_modified_cmd |
| 0xC0409723 SEND_MODFD_CMD_64 | 35 | 64 | 0x4BF0 | __QSEECom_send_modified_cmd(use64=1) |
| 0x9705 RECEIVE | 5 | 0 | 0x4DF3 | QSEECom_receive_req（阻塞等待，errno==-512 特判） |
| 0x9706 SEND_RESP | 6 | 0 | 0x4F79 | QSEECom_send_resp |
| 0xC0289710 SEND_CMD_SERVICE | 16 | 40 | 0x51E4 | __QSEECom_issue_send_service_cmd |
| 0xC0249711 CREATE_KEY | 17 | 36 | 0x5642 | QSEECom_create_key（独立开 fd） |
| 0xC0089712 WIPE_KEY | 18 | 8 | 0x5798/0x57D8 | QSEECom_wipe_key / QSEECom_clear_key / __QSEECom_wipe_clear_key |
| 0xC0449718 UPDATE_KEY_USER_INFO | 24 | 68 | 0x5AE3 | QSEECom_update_key_user_info |
| 0xC0309715 SEND_MODFD_RESP | 21 | 48 | 0x5D4F | __QSEECom_send_modified_resp |
| 0xC0309724 SEND_MODFD_RESP_64 | 36 | 48 | 0x5D60 | 同上(use64=1) |
| 0xC0049717 SET_BUS_SCALING | 23 | 4 | 0x5DF4 | QSEECom_scale_bus_bandwidth |
| 0x970B / 0x970C PERF_ENABLE/DISABLE | 11/12 | 0 | 0x5059/0x505F | QSEECom_set_bandwidth |
| 0x9709 UNLOAD_APP | 9 | 0 | 0x40D7 | __QSEECom_dealloc_memory（magic==0x9709 时先 unload） |
| 0x970E UNLOAD_EXTERNAL_ELF | 14 | 0 | 0x439C | QSEECom_unload_external_elf |

**线程归属**：qseecomd 主线程不直接打 ioctl；每个 listener 服务库的 `*_start(listener_id)` 先 `QSEECom_register_listener`（在调用线程，即 qseecomd 主线程的 start_services 循环里），再 `pthread_create` 出专属线程跑 `QSEECom_receive_req`(RECEIVE，阻塞) + 处理 + `QSEECom_send_resp` 死循环（librpmb `dispatch` @0x4ca4 全貌反编译已确认，其余库同构，[实拆]+[string]）。客户端 HAL 进程（keymaster/gatekeeper/fingerprint）在自己的线程里 open+start_app+send_cmd。RECEIVE 的错误处理约定：errno==-512(ERESTARTSYS) 时上层循环重试（librpmb `while (v1 == -512)`）。

qseecomd 进程内 ioctl 顺序（每次 listener 注册）：
`open("/dev/qseecom") → GET_QSEOS_VERSION → [open /dev/ion → ALLOC → MAP → mmap] → REGISTER_LISTENER(fd, VA, size) → pthread → RECEIVE* → SEND_RESP*`。
客户端进程顺序：`open → GET_VERSION → APP_LOADED_QUERY(仅 version≥20 且 V2/query 分支) → LOAD_APP/LOAD_EXTERNAL_ELF(TA 镜像 fd，临时) → SET_MEM_PARAM(sb fd) → SEND_CMD / SEND_MODFD* 循环 → UNLOAD_APP(shutdown) → close`。

### Q1.2 `/dev/ion` 调用面

见第 0 节：ALLOC(len=PAGE_ALIGN, align=0x1000, heap_id_mask=1<<27, flags=0) → MAP → mmap → 用完 FREE。libQSEEComAPI 不用 ION_IOC_SYNC（dynsym 无此符号）。**[实拆]**

### Q1.3 驱动侧支持面 **[源码]**

`qseecom_ioctl` dispatch（qseecom.c:6699-7335）共 31 个 case：nr 1-24（除 22）、30-33(QTEEC)、34(MDTP)、35-36、40-42。**UAPI 有 33 个 ioctl，驱动未实现 nr 43 SET_ICE_INFO 的 case**（UAPI 头里有定义）。SEND_MODFD 里 fd 允许离散：驱动为每个非零 fd 建 sg 表并把 sg entry 回写进 cmd/resp 缓冲（qseecom.c:3310-3436），`qseecom_buf_contiguous`(nents==1) 校验只作用于：listener sb（:1219）、client sb（:1710）、LOAD_APP 镜像（:2474）、LOAD_EXTERNAL_ELF 镜像（:5279）。

---

## Q2. TA 加载面

### qseecomd 自身 **[实拆]**
qseecomd 不加载任何 TA——10 个服务全部是 listener（见 Q3）。二进制内无 LOAD_APP 调用（全部 3 个函数已反汇编，无 LOAD_APP/LOAD_EXTERNAL_ELF ioctl 常量）。

### 生态内 TA（交叉验证 strings + NEEDED + 反编译）：

| TA 固件名 | 加载者 | API | 证据 |
|---|---|---|---|
| **keymaster64** | `libkeymasterdeviceutils.so`（KeymasterUtils::qseecom_dev_init） | QSEECom_start_app("/vendor/firmware_mnt/image", "keymaster64", ...) + QSEECom_send_cmd | [string] "keymaster64"、"/vendor/firmware_mnt/image"、"QSEECom_start_app failed"；被 keymaster HAL 与 gatekeeper impl 共用 |
| **goodixfp** | `libgf_hal_G2/G3/G5.so`（goodix 指纹 HAL） | QSEECom_start_app（fw 搜索路径 /firmware/image、/vendor/firmware、/vendor/firmware_mnt/image、/data/vendor/euclid/...）+ send_cmd + send_modified_cmd | [string] goodixfp.b00-07 在 vendor/firmware |
| **sileadta / sileadta_16bit / sileadta_new16bit** | `libsl_fp_impl.so` / `libsl_fp_impl_16bit.so` | **QSEECom_load_external_elf**（唯一用户） | [string]+dynsym |
| **widevine** | widevine.mdt/b00-b07 在 vendor/firmware；装载方经 libwvhidl/liboemcrypto 链（未在本仓库 NEEDED 里，见 OPEN-2） | — | [string] 固件文件存在 |
| **cmnlib / cmnlib64** | **内核驱动自动加载**：`qseecom_load_app` 首次加载 32/64 位 app 前调 `qseecom_load_commonlib_image(data,"cmnlib"/"cmnlib64")`（qseecom.c:2404-2419），内核 request_firmware + 内核侧连续缓冲 | 内核内部 | [源码] |
| **haventkn / dhsecapp / rtic** | `ssgtzd`/`ssgqmigd`（SSG 守护进程）经 qteeconnector HIDL（vendor.qti.hardware.qteeconnector@1.0-impl.so 用 libQSEEComAPI 加载/卸载/通信） | qteeconnector impl 的 QSEECom_* 全套 | [string] ta_config.json `ta_images` 路径 + qteeconnector impl strings（"Loading app [%s] failed"、"gptauuid.xml"） |
| rtice（内嵌） | ta_config.json `embedded_ta_images` | — | [string] |

TA 镜像文件协议（`__QSEECom_load_image` @0x35ec，反编译行 303-667）**[实拆]**：
1. `snprintf(path, "%s/%s.mdt", base, name)` → fopen/fread 64 字节 MDT 头；
2. 按 ELF class（e_ident[4]）取 `e_phnum`（ELF32 @0x2C / ELF64 @0x38，u16），段数 = phnum+1，上限 100；
3. 逐个 `open("%s/%s.b%02d")` + `fstat` 累加总长；
4. **一次 ION_memalloc(总长)**（heap 27，物理连续）→ 顺序 `read()` 全部段进缓冲；
5. 填 `struct qseecom_load_img_req{mdt_len, img_len, ifd_data_fd, img_name[64], app_arch}` → LOAD_APP / LOAD_EXTERNAL_ELF ioctl → **立即 ION_memfree**（镜像缓冲是一次性的）。

---

## Q3. listener 注册列表

qseecomd 的服务表在 `.data:0x3000`，10 项 × 0x38 字节（`__libc_init` 后的 init_service 循环以 0x38 步长遍历）。指针槽由 `.rela.dyn` R_AARCH64_RELATIVE 填充，已用脚本重建（work/reloc_table.py）：**[实拆]**

| # | 服务名（.rodata 字符串） | so | init/main 阶段调的 fn | start 阶段调的 fn | **listener_id** | sb_size | 处理逻辑概况 |
|---|---|---|---|---|---|---|---|
| 0 | "RPMB system services" | librpmb.so | rpmb_init_service | rpmb_start | **0x2000 (8192)** | 25600 (0x6400) | RPMB 协议中继：sbuf 首 dword 为 cmd（257/258/259），调 rpmb_init/rpmb_read/rpmb_write，send_resp 回 20/40 字节（dispatch @0x4ca4） |
| 1 | "SSD system services" | libssd.so | ssd_init_service | ssd_start | **0x3000 (12288)** | 20480 (0x5000) | SSD 分区（/dev/block/bootdevice/by-name/ssd）加密数据中继 |
| 2 | "secure UI services" | libsecureui.so | secure_ui_init | secure_ui_start | **0x4000 (16384)** | 4264 (0x10A8) | 安全 UI 输入/显示事件；用 send_modified_resp_64 传 fd |
| 3 | "time services" | libdrmtime.so | （无 init） | atime_start | **11 (0xB)** | 20480 | 可信时间（QSEOS time 服务）中继 |
| 4 | "file system services" | libdrmfs.so | （无 init） | fs_start | **10 (0xA)** | 20480 | GP 文件加密（FDE）文件服务，写 /data/misc/qsee [string] |
| 5 | "gpfile system services" | libdrmfs.so | （无 init） | gpfs_start | **0x7000 (28672)** | 516096 (0x7E000) | 第二个 listener，同库，大 sb |
| 6 | "interrupt services" | libqisl.so | （无 init） | qisl_start | **0x9000 (36864)** | 500 | QSEE 中断服务（QSEE Interrupt Service Listener）[string] |
| 7 | "secure processor services" | libspl.so | （无 init） | spl_start | **0xB000 (45056)** | n/a（库未出厂） | libspl.so 不在 lineage blob **亦不在 ColorOS 7.1 vendor**（见"ColorOS 交叉验证"节），原厂机上同样 dlopen 失败→跳过 |
| 8 | "gp request cancellation services" | libGPreqcancel.so | （无 init） | gp_reqcancel_start | **0x1100 (4352)** | 20480 | GP 请求取消（GPAPP cancellation） |
| 9 | "output protection service listener" | libops.so | （无 init） | ops_start | **0xC000 (49152)** | n/a（库未出厂） | libops.so 同上，从未出厂；ColorOS qseecomd 服务表第 10 行 @0x31f8 实拆确认条目在场而 so 缺席 |

**listener_id 证据链**：qseecomd init_service 以 `[entry+0x08]` 的 u32 为参调 dlsym 出的 start fn（qseecomd .text 0x20e8/0x2138/0x2270：`ldr w0,[x21]`+`blr x19`，日志 "%s: Started (Listener ID = %d)"）；各服务库把该参直传 `QSEECom_register_listener(&h, id, sb_size, 0)`（librpmb:1515、libssd:64、libsecureui:1271、libdrmtime:33、libdrmfs:253/1518、libqisl:235、libGPreqcancel:36，全为反编译行号）**[实拆]**。
listener_id 非常规（0x2000、0x3000…）是厂商 TZ 侧约定，与 QSEE listener 小整数惯例不同——以实拆为准。

**两阶段启动**（qseecomd .text 0x2368 main）：
1. main：`signal(SIGPIPE,SIG_IGN)` → `init_service(0,0)`（rpmb_init_service）、`(1,0)`（ssd_init_service）、`(2,0)`（secure_ui_init）——仅 flags∈{0x2000,0x3000,0x4000} 的项做 init；
2. `prctl(PR_SET_NO_NEW_PRIVS,1)` → `setgid(1000)`+`setuid(1000)` → `capset({ver=0x20080522,pid=0},{effective/permitted=0x20000 即 CAP_SYS_RAWIO,…})`（.rodata:0xc10）——降权但保 RAWIO；
3. start_services：循环 `init_service(0..9, 1)` 起 10 个 listener；
4. `property_set("vendor.sys.listeners.registered","true")` → log "QSEECOM DAEMON RUNNING" → `pause()`。

失败语义：任一 init/start 失败只打日志继续下一项；rpmb/ssd/secureui init 失败时 main 直接返回 -1（"shall not start listener services"）。

---

## Q4. libQSEEComAPI.so 的 API 面

### 导出函数（dynsym 完整清单，30 个）**[实拆]**

```
QSEECom_start_app            @0x403c → __QSEECom_get_handle(h, 1, path, name, 0,0, sb_size, 1)
QSEECom_start_app_V2         @0x405c → __QSEECom_get_handle(h, 1, 0, name, mem, len, arch_ver, 2)  // 内存镜像加载
QSEECom_shutdown_app         @0x41a0 → __QSEECom_dealloc_memory(h, 0x9709)
QSEECom_app_load_query       @0x3498
QSEECom_load_external_elf    @0x41a8
QSEECom_unload_external_elf  @0x436c
QSEECom_register_listener    @0x4464
QSEECom_unregister_listener  @0x4710
QSEECom_send_cmd             @0x4898
QSEECom_send_modified_cmd    @0x4db8 → __QSEECom_send_modified_cmd(use64=0)
QSEECom_send_modified_cmd_64 @0x4dc0 → __QSEECom_send_modified_cmd(use64=1)
__QSEECom_send_modified_cmd  @0x4af4
QSEECom_send_modified_resp   @0x5dac / _64 @0x5db4 → __QSEECom_send_modified_resp @0x5c38
QSEECom_receive_req          @0x4dc8
QSEECom_send_resp            @0x4f2c
QSEECom_send_service_cmd     @0x52a8   // asm 验证: w4=cmd_id，即 (send_buf, sbuf_len, resp_buf, resp_len, cmd_id)
__QSEECom_issue_send_service_cmd @0x50c0
QSEECom_set_bandwidth        @0x5034
QSEECom_scale_bus_bandwidth  @0x5dbc
QSEECom_create_key           @0x55e0
QSEECom_clear_key            @0x596c
QSEECom_wipe_key             @0x5838
__QSEECom_wipe_clear_key     @0x5754
QSEECom_update_key_user_info @0x5a9c
QSEECom_get_app_info         @0x5e88
__QSEECom_get_handle         @0x3014
__QSEECom_dealloc_memory     @0x407c
__QSEECom_validate_send_cmd_inputs @0x4804
```

**handle 结构**（0x28=40 字节，malloc 出来的 `struct qseecom_handle`，与上游 QSEEComAPI.h 布局一致）：
```
+0x00 unsigned char *sbuf;      /* ion/dma-buf mmap VA */
+0x08 uint32_t sbuf_len;
+0x0C uint32_t app_id;
+0x10 int32_t  fd;              /* /dev/qseecom fd */
+0x14 int32_t  ion_dev_fd;      /* /dev/ion fd        (重建: /dev/dma_heap/<name> fd) */
+0x18 int32_t  ifd_fd;          /* 共享缓冲 dma-buf fd */
+0x1C uint32_t version;         /* GET_QSEOS_VERSION 返回值(20) */
+0x20 uint32_t ion_handle;      /* ION_IOC_FREE 用    (重建: 复用为 heap 标记) */
+0x24 uint32_t app_arch;        /* ELFCLASS32/64 */
```

**send_cmd 语义**（@0x4898 反编译 1199-1315）**[实拆]**：
- 校验 `(send_len ≤ sbuf_len) && (resp_len ≤ sbuf_len)`；
- 若 send_buf **不在** sb 区间内 → memcpy 进 sb 头部，cmd_req_buf=sbuf；否则直传用户 VA（即 TZ 可读 caller 原缓冲）；
- resp_buf 默认 = `sbuf + send_len`（**version==20 且长度非 64 倍数时上取整到 64**——QSEECOM_ALIGN 语义）；若 resp_buf 在 sb 区间内则直传；
- ioctl SEND_CMD（内核把 cmd/resp VA 换算成 sb 内偏移→TZ 物理地址）；
- 成功后 memcpy(resp, sb+send_len, resp_len)，并 `memset(sbuf, 0, sbuf_len)` 清 sb。

**register_listener 语义**（@0x4464）：open /dev/qseecom → GET_VERSION → ION_memalloc(sb_size) → `REGISTER_LISTENER{listener_id, ifd, virt_sb_base=mmap VA, sb_size}` → errno==EEXIST(16) 时返回 -2（"already registered"，调用方当成功处理）。listener 循环里 RECEIVE 后从 sb 读请求、处理完 send_resp（先把 resp memcpy 到 sb 头部；version==19 时首 dword 清零再 ioctl）。**第 4 参数定案（OPEN-3 closed 2026-11-21）**：vendor 源码参数名为 `hierarchy`（ColorOS 7.1 qseecomd .rodata 字符串 `"Listener: index = %d, hierarchy = %d"`），但内核 UAPI `struct qseecom_register_listener_req` 只有 4 个字段（listener_id/ifd_data_fd/virt_sb_base/sb_size）无对应槽位，且全部调用方传 0 —— 纯死参数，重建按 4 参签名保留、不进 ioctl。

**load_external_elf**：新 handle → GET_VERSION → load_image(base, fname, ..., mode=1) → ioctl 0xC054970D；unload 用 0x970E 并 close 三个 fd。镜像缓冲（非 sb）装载完即释放。

**send_service_cmd**（asm @0x52a8 验证）**[实拆]**：`(send_buf x0, sbuf_len w1, resp_buf x2, resp_len w3, cmd_id w4)`；cmd_id ∈ {15,16,27,0x109,0x110,0x203,0x204,0x301,0x302,0x402,0x403,0x404}（= qseecomi.h QSEOS_RPMB_PROVISION_KEY 0x0F / RPMB_ERASE 0x10 / RPMB_CHECK_PROV 0x1B / FSM_LTEOTA 0x109/0x110 / FSM_IKE 0x203/0x204 / OEM_FUSE 0x301/0x302 / ENCFS 0x403/0x404）；临时开 handle（sb=请求总长）→ SEND_CMD_SERVICE → 立即释放。vendor 树内无外部调用方。

### 链接 libQSEEComAPI.so 的 vendor 二进制（readelf -d NEEDED 全扫）**[实拆]**

lib64（16）：`bin/qseecomd`、`libbokehclient.so`、`libdrmfs.so`、`libdrmtime.so`、`libgf_hal_G2.so`、`libgf_hal_G3.so`、`libgf_hal_G5.so`、`libGPreqcancel.so`、`libkeymasterdeviceutils.so`、`libkeymasterutils.so`、`libpvr.so`、`libqfp-service.so`、`libqisl.so`、`librpmb.so`、`libsecureui.so`、`libSecureUILib.so`、`libssd.so`、`libtzdrmgenprov.so`、`hw/vendor.qti.hardware.qteeconnector@1.0-impl.so`
（另 `libssd.so`、`libsl_fp_impl.so`、`libsl_fp_impl_16bit.so`、`libgf_hal_*.so` 经 dynsym 使用其导出；32 位 `lib/libbokehclient.so`、`lib/libcppf.so`、`lib/libpvr.so`。）

即：**keymaster HAL（经 libkeymasterdeviceutils/libkeymasterutils）、gatekeeper HAL（复用 KeymasterUtils）、指纹（goodix libgf_hal_G* + silead libsl_fp_impl*）、DRM（libdrmfs/libdrmtime/libtzdrmgenprov）、qteeconnector（通用 TA 装载服务，ssgtzd 经它）** 全部走这个库——重建件 ABI 兼容是整条 TZ 用户态链的关键。

---

## Q5. sb 所有权定案与重建时序

### 内核语义 **[源码]**

- client sb：`QSEECOM_IOCTL_SET_MEM_PARAM_REQ` → `qseecom_set_client_mem_param`（qseecom.c:1684-1744）：`qseecom_buf_import(pdev, req.ifd_data_fd, &data->client.buf)` → `qseecom_buf_contiguous`（nents==1，否则 -EINVAL）→ `sb_virt=buf->vaddr`（内核映射）→ `sb_phys=buf->phys` → `user_virt_sb_base=req.virt_sb_base`。
- listener sb：`REGISTER_LISTENER` → `__qseecom_set_sb_memory`（qseecom.c:1199-1271）：同样 import+contiguous 校验，SCM `QSEOS_REGISTER_LISTENER` 把 `sb_phys` 登记给 TZ。
- VA 解析：`__qseecom_uvirt_to_kvirt(data, virt) = data->client.sb_virt + (virt - data->client.user_virt_sb_base)`（qseecom.c:2784-2790）；kphys 同理（sb_phys + 偏移）。**send_cmd 的 cmd_req_buf/resp_buf 用户 VA 必须落在注册的 sb 区间内**（boundary check，qseecom.c:2860-2870），TZ 通过物理地址直读直写。
- 内核全程无 `/dev/qseecom` mmap；用户访问 sb 的唯一途径 = mmap dma-buf fd。
- `qseecom_buf_import`（qseecom_dmabuf.c:26-69）：`dma_buf_get → dma_buf_attach → dma_buf_map_attachment(BIDIRECTIONAL)`，一次映射缓存到 ioctl 关闭为止（比 4.9 每次 ioctl 重 import 更省）。

### 4.9 vendor 流程 vs 重建流程（时序）

**vendor（4.9）**：
```
libQSEECom_start_app:
  open /dev/qseecom → GET_VERSION
  open /dev/ion → ION_IOC_ALLOC(heap 27, 连续) → ION_IOC_MAP → mmap fd
  读 %s/%s.mdt + .bXX → 全部 memcpy 进该缓冲
  LOAD_APP(fd, mdt_len, img_len, name, arch) → app_id
  ION_memfree(镜像)                                  ← 镜像缓冲一次性
  ION_memalloc(sb_size) → mmap → SET_MEM_PARAM(fd, VA, len)
  return handle{sbuf=mmap VA, fd, ifd_fd, app_id}
send_cmd: 组包进 sb → SEND_CMD(VA) → 读 sb resp
```

**重建（7.3）**：把中间 ION 段换成 dma-buf heaps，其余 ABI 行为逐位一致：
```
QSEECom_start_app(handle, base, name, sb_size):
  1. open("/dev/qseecom", O_RDWR)                    # 与 vendor 相同
  2. ioctl GET_QSEOS_VERSION(0xC004970A) → version
  3. img_fd = dma_heap_alloc_contig(PAGE_ALIGN(img_total))
     #   首选 /dev/dma_heap/qseecom_ta_region，回退 default_cma_region
     #   （或环境变量 QSEE_DMA_HEAP 钉死单堆）；必须连续：驱动 nents==1
     #   校验（qseecom.c:2474）
  4. img_va = mmap(img_fd)
  5. read .mdt/.bXX → img_va
  6. ioctl LOAD_APP(0xC0549707){mdt_len, img_len, img_fd, name, arch} → app_id
  7. munmap(img_va); close(img_fd)                   # 一次性
  8. sb_fd = dma_heap_alloc_contig(PAGE_ALIGN(sb_size))   # 同策略，连续（:1710 校验）
     sb_va  = mmap(sb_fd)
  9. ioctl SET_MEM_PARAM(0xC0189708){sb_fd, sb_va, sb_size}
 10. return handle{sbuf=sb_va, sb_fd, ...}
send_cmd: 与 vendor 完全一致（VA 直传/拷贝语义 + 64 对齐 + resp 拷出 + 清 sb）
```

**QSEECom_alloc（若指 QSEECom_start_app 的 sb）语义映射**：原"ION heap 27 一次分配连续缓冲"→ 新"dma-heap CMA 类 heap（default_cma_region / qseecom_ta_region）一次分配"；用户态 mmap 的对象从 ion fd 变为 dma-buf fd；SET_MEM_PARAM/REGISTER_LISTENER 的 (fd, VA, size) 三元组不变——**驱动 ABI 完全复用，用户态只换分配器**。
listener sb 同理：REGISTER_LISTENER 传 dma-buf fd + mmap VA（qseecom.c:1210-1266）。
（内核侧内核客户端 API `qseecom_start_app` 的 sb 由 `dma_alloc_coherent` 从默认 CMA（= default_cma_region 后端）分配，与用户态无关。）

---

## Q6. lineage-20 init 集成点

**[实拆]** vendor 树 `proprietary/vendor/etc/init/qseecomd.rc`：
```
on post-fs
    start vendor.qseecomd

service vendor.qseecomd /vendor/bin/qseecomd
    class core
    user root
    group root
```
- 拉起时机 `on post-fs`，无 `seclabel`、无 `onrestart`、无 HAL 依赖条件（不写 `interface` 行）。
- 权限模型：以 root 起来（要注册 listener + capset RAWIO），进程内自己降权到 uid/gid 1000 并 `capset` 保留 CAP_SYS_RAWIO（Q3 表后流程）。
- 设备节点权限（device_tree/init/ueventd.qcom.rc:130）：`/dev/qseecom  0660  system  drmrpc`。
- 依赖方服务：keymaster-3-0 与 gatekeeper-1-0 都是 `class early_hal`（android.hardware.keymaster@3.0-service-qti.rc / gatekeeper rc），在 post-fs 阶段与 qseecomd 并发拉起，靠自己的 libQSEEComAPI 打开 /dev/qseecom，**不等待 vendor.sys.listeners.registered**（vendor 树内无该属性的 rc 消费者，OPEN-6）。
- qteeconnector：`class hal`，`user system`。
- **重建件集成清单**：
  1. 放置 `vendor/bin/qseecomd`（保持服务名 vendor.qseecomd 与 rc 兼容，或重写 rc）；
  2. `vendor/lib64/libQSEEComAPI.so`（SONAME 必须保持 `libQSEEComAPI.so`）替换原库；
  3. ueventd 权限行不变；sepolicy 需给重建进程 qseecom_device 访问 + `vendor_sys_property vendor.sys.listeners.registered` set 权限（原 vendor sepolicy 在 blob 里，重建时需抓取等价 allow 规则）；
  4. `/dev/dma_heap/qseecom_ta_region`（连续分配首选）与 `/dev/dma_heap/default_cma_region`（回退）的访问权限（ueventd 加 `0660 system drmrpc` 或对应 segen，重建件运行 uid=system）；SEND_MODFD 离散 fd 可用 `/dev/dma_heap/system`；
  5. 确认 7.3 内核已使能 CONFIG_DMABUF_HEAPS_SYSTEM/CMA（heap 节点在 `/dev/dma_heap/` 可见）。

---

## 重建实施计划

### 模块划分

```
src/qsee_lib/                      # 重建 libQSEEComAPI.so（SONAME 保持）
  include/qsee_com_api.h           # 公共 ABI 头：struct qseecom_handle(40B, offset 断言)、全部导出函数签名、ioctl 常量
  src/qsee_dma_heap.c              # dma-buf heap 分配器（替代 __QSEECom_ION_memalloc/memfree）
  src/qsee_image.c                 # TA 镜像读取 (.mdt/.bXX → 连续 dma-buf)
  src/qsee_com_api.c               # 30 个导出函数 + 内部 ioctl 流
  Makefile                         # aarch64-linux-gnu- 交叉 → .o/.so；native -fsyntax-only 兜底
src/qseecomd/
  qseecomd.c                       # 服务表(10项, 常量逐位复刻) + 两阶段启动 + property_set + pause
  service_stubs.c                  # listener 循环骨架（receive/resp 语义）+ TODO-VERIFY 桩
  Makefile
```

### 行为保真点（对照证据）
1. 30 个导出符号同名同序参（Q4 表）；`struct qseecom_handle` 40 字节 offset 不变。
2. send_cmd 的 in-sb 指针直传 / 64 对齐 / resp 拷出 / 清 sb 语义（Q4）。
3. register_listener 的 EEXIST → -2 约定；receive 的 -512 重试约定。
4. TA 镜像 .mdt/.bXX 装载与"装载完即释放"。
5. qseecomd 服务表 10 项常量（listener_id、so 名、init/start/close 符号名、启动顺序、失败继续语义）。
6. 分配器策略：连续消费方（镜像/双 sb）走 `qsee_heap_alloc_contig()` = `qseecom_ta_region` 首选 → `default_cma_region` 回退；`QSEE_DMA_HEAP` 环境变量可钉死单堆；分配失败时明确报 heap 名（system 堆不参与镜像/sb 分配）。

### 风险
- **连续堆容量与碎片**：首选堆 qseecom_ta_region 仅 16MB（reusable CMA）；多 TA（keymaster64+goodixfp+sileadta）并发装载 + 各 listener sb 驻留时可能吃紧。回退链 default_cma_region（32MB）已实现；装载失败时按 20 次 × 50ms 重试后向下一堆切换。风险中低（TZ 只认 phys，无位置约束）。
- **fragmentation**：CMA 反复 alloc/free TA 镜像（每次装载一次性缓冲）可能碎片化；缓解：先尝试大块、失败再退 system heap 会被 nents==1 拒绝 → 需重试策略（4.9 UAPI 里 `QSEECOM_TA_ION_ALLOCATE_DELAY/MAX_ATTEMP`（qseecom.h:12-13）就是为此设计的重试参数，重建里同样做 20 次 × 50ms 重试）。
- **libsecureui 等直接调 send_modified_resp_64**：依赖 MAX_ION_FD=4 的 ifd_data 组包，重建需逐字段对齐 `qseecom_ion_fd_info{fd, cmd_buf_offset}`。
- **ABI 检查钩子缺失**：编译期用 `_Static_assert(offsetof(...)==...)` 锁 handle 与 ioctl 结构布局。
- libspl/libops 从未出厂（OPEN-1 closed 2026-11-21）：两个 so 在 lineage blob 与 ColorOS 7.1 vendor 中均不存在，原厂机上这两个 listener 从未注册成功；重建 qseecomd 与 vendor 同语义（dlopen 失败→打日志→跳过，无内置替身），内置桩仅在 `QSEECOMD_BUILTIN_SERVICES=1` 实验模式下启用。

### 验证钩子（2026-11-21 起落地为 src/selftest/qsee_selftest.c）
1. `qsee_selftest` 探针梯子（六段，缺前置自动 SKIP，退出码=失败段位图）：
   S0 dma-heap 探测+连续分配 sanity → S1 GET_QSEOS_VERSION（期望 0x14=20）→ S2 部分使用 sb 双向边界（OPEN-9）→ S3 listener 0x7fff 注册/注销（经 dlopen 重建库，库未部署则跳过）→ S4 `--app keymaster64 --base /vendor/firmware_mnt/image` start_app→send_cmd→shutdown → S5 SEND_MODFD 瞬态补丁验证（哨兵值 pre-fill + 调用后清零断言，OPEN-5）。
   lib 依赖段走 `dlopen("libQSEEComAPI.so")`，S0-S2 在重建库部署前即可独立跑（先验证驱动+堆，再验库——"先学会跑"）。
2. listener 冒烟：`QSEECOMD_BUILTIN_SERVICES=1 ./qseecomd` 起内置桩（以各 listener_id 注册 + RECEIVE 阻塞 + 打印 TZ 命令头，不回 resp 即可验证驱动路径）。
3. `strace -e ioctl` 对比重建件与 vendor 原件的 ioctl 序列（除 ION 三连换 dma-heap 外应逐条一致）。
4. dma-heap 探测：S0 已内置（节点存在性打印）；手工口径 `ls /dev/dma_heap/`。

### 已完成的构建验证（2026-11-21 复验）
- WSL Ubuntu-24.04 gcc 13.3 host `-fsyntax-only`：qsee_lib(3)/qseecomd(2)/selftest(1) 全部 0 warning/error。
- `aarch64-linux-gnu-gcc` 交叉：`libQSEEComAPI.so`（SONAME 保持）+ `qseecomd` + `qsee_selftest` 全部独立编译链接成功。
- 导出符号集与 vendor `.so` 的 30 个 FUNC dynsym **逐名一致**（diff 为空，内部辅助函数已 hidden）；SONAME=readelf 实证 `libQSEEComAPI.so`。
- 编译期 `_Static_assert` 锁定 `struct qseecom_handle`(40B) 与全部 ioctl 结构布局——**含 OPEN-8 新增的 QTEEC(32/64)/MDTP(32)/CE_PIPE(80)/SAVE_PARTITION_HASH(36)/IS_ES_ACTIVATED(4)/ICE(4) 九个结构**。
- ColorOS 7.1 libQSEEComAPI（64 位与 32 位）导出面与 lineage blob **逐名一致**（32 位仅多 unwind/aeabi 运行时符号），vendor 跨代导出面冻结实锤。

---

## ColorOS 7.1 交叉验证（2026-11-21，镜像：vendor_C7_bootable.img（用户机桌面），ext4 raw 2.25GiB）

对原厂 vendor 分区定向抽取 + 实拆，与 lineage-20 blob 结论逐项比对：

| 项 | ColorOS 7.1 证据 | 与 lineage 结论 |
|---|---|---|
| qseecomd 服务表 | `work/c7_findings_qseecomd_table.txt`：.data 0x3000 起 10 行×0x38，name/so/listener_id 与 lineage **逐一相同**（0x2000/0x3000/0x4000/0xB/0xA/0x7000/0x9000/0xB000/0x1100/0xC000） | 服务表常量双源冻结 |
| libspl.so / libops.so | /lib64、/lib(32)、/odm/lib64 全无；strings 实证表中引用在场（libspl.so/libops.so/ops_start/ops_close） | **从未出厂**（OPEN-1 根证） |
| "hierarchy" 字符串 | qseecomd .rodata `"Listener: index = %d, hierarchy = %d"` | OPEN-3 参数名实证 |
| libQSEEComAPI 导出面 | 64 位 30 个 FUNC 逐名一致；32 位同面+unwind 运行时符号 | 导出面跨代冻结 |
| libwvhidl.so | NEEDED 纯 HIDL 栈（drm@1.0-1.2/hidl/protobuf），UNDEF 零 QSEE 符号，strings 无 /dev/qseecom | widevine 链不吃 libQSEEComAPI（OPEN-2 根证） |
| keymasterdeviceutils | NEEDED libQSEEComAPI+libion；UNDEF 仅 start_app/send_cmd/shutdown_app | 重建件 ABI 覆盖其全部依赖 |
| goodix/silead | UNDEF QSEECom_start_app/send_cmd/send_modified_cmd/shutdown_app | 同上（send_modified_cmd 已实现） |
| ssgtzd/ssgqmigd | NEEDED 无 libQSEEComAPI（经 qteeconnector HIDL 转发） | 与 lineage 结论同构 |
| vendor.sys.listeners.registered | /etc/init rc 全无、/bin 全量 strings 无命中、lineage repos 无命中 | OPEN-6：无可考消费者，保持 set 即可 |

原始证据文件：`work/c7_findings_*.txt`（8 份），抽取物 `work/c7_vendor/`（21 个 so/bin + rc）。

---

## OPEN 问题清单收口（2026-11-21 全部关闭）

1. **✅ libspl.so / libops.so 不在 blob 仓库 → 从未出厂**：ColorOS 7.1 原版 vendor（用户提供的 `vendor_C7_bootable.img`）中 /lib64、/lib(32)、/odm/lib64 均无这两个库，而 ColorOS qseecomd 服务表同样引用它们（@0x3188/@0x31f8）⇒ 原厂机上这两个 listener 一直就是 dlopen 失败→日志→跳过。重建 qseecomd 已回归该语义：**缺库不再落到内置桩**（避免暴露 stock 从未有的 TZ listener），桩仅在 `QSEECOMD_BUILTIN_SERVICES=1` 显式开启。spl(0xB000)/ops(0xC000) 的 sb 尺寸/协议永不需实拆（桩默认 20480 仅实验模式生效）。
2. **✅ widevine TA 的装载链**：实拆 ColorOS `libwvhidl.so`（3.6MB）——NEEDED 纯 HIDL 栈，UNDEF 零 QSEE/smcinvoke/qseecom 符号，strings 无 /dev/qseecom、无 .mdt 处理 ⇒ **widevine 链不依赖 libQSEEComAPI**，重建件与之零耦合；TA 装载（若走 QSEE）在 vendor 分区外完成（system 侧 liboemcrypto 或其它通道，超出本报告目标范围）。对重建件 ABI 兼容性问题而言本条关闭。
3. **✅ QSEECom_register_listener 第 4 参数 = 死参数**：参数名 `hierarchy`（ColorOS qseecomd .rodata 实证）；内核 UAPI `qseecom_register_listener_req`（7.3 树 include/uapi/linux/qseecom.h:23-28）只有 4 字段无槽位；全部调用方传 0。重建保留 4 参签名、参数不入 ioctl，头文件已注记。
4. **✅ handle +0x20 字段无直接消费者**：对 18 个消费库（ColorOS 集合：rpmb/ssd/secureui/drmfs/drmtime/qisl/GPreqcancel(+svc)/keymasterdeviceutils/keymasterutils/tzdrmgenprov/SecureUILib/secureui_svcsock/rpmbengclient/gf_hal_G2/G5/sl_fp_impl/qteeconnector）做 llvm-objdump 启发式扫描（函数内既有 `bl QSEECom*` 又有 `[xN,#32]` w 加载/存储，x29/x30 剔除）——**全部 0 命中**（`work/c7_findings_handle0x20.txt`）。重建版该字段（heap 索引）无 ABI 风险。
5. **✅ SEND_MODFD sg 回写细节已钉死 + 自测覆盖**：7.3 树 `__qseecom_update_cmd_buf`（qseecom.c:3195）逐行核验——nents==1 时在 `cmd_req_buf+cmd_buf_offset` 写 **4 字节 PA**（`__boundary_checks_offset` 门），nents>1 时写 **`qseecom_sg_entry{phys,len}` 8B 数组**（要求 `cmd_req_len >= SG_ENTRY_SZ*nents` 且 offset 不越界），另有 QSEE≥4.0 的 4GB PA 闸与 arch 校验；SCM 前 patch(false)/SCM 后 cleanup(true) 双遍（:3662-3678）。**顺带修复重建件真 bug**：`QSEECom_send_service_cmd` 走的 `__QSEECom_get_handle(mode=0)` sb-only 路径此前落入 -EINVAL（驱动 `__validate_send_service_cmd_inputs`（qseecom.c:2653）实证只需 client sb、无需 app）——已补 mode==0 分支。运行时验证 = `qsee_selftest` S5（哨兵 pre-fill + 清零断言）；listener 侧 64 变体由 `__qseecom_update_cmd_buf_64` 同构覆盖。
6. **✅ vendor.sys.listeners.registered 无可考消费者**：ColorOS /etc/init 全部 rc、/bin 全量（rdump 后 grep -rl）、lineage vendor+device_tree repos 三路扫描均零命中。重建保持 set 该属性（语义兼容），风险面关闭（system 侧若有消费者也不依赖语义变化——属性值/时机与 vendor 相同）。
7. **✅ 7.3 defconfig DMABUF heaps 已落且已入库**：`0fd76cdcff48 Add: configs: 启用 DMA-BUF heaps system 与 CMA`；树内实证 gki_defconfig:617 `CONFIG_DMABUF_HEAPS=y`、:841 `=SYSTEM`、:842 `=CMA`（+CMA/DMA_CMA/CMA_AREAS=16），树根 .config 同步；堆名源码点复核（cma_heap.c:30 `DEFAULT_CMA_NAME "default_cma_region"`、:413 `cma_get_name`、system_heap.c:633 `"system"`）与 DT 三节点（sdm670.dtsi:608 qseecom_mem / :636 qseecom_ta_mem 16MB reusable / :687 linux,cma）全在场。**运行时剩余项（唯一）**：上机 `ls /dev/dma_heap/` 确认四节点 + ueventd 权限行（Q6 清单第 4 条），已并入 qsee_selftest S0。
8. **✅ QTEEC/MDTP/CE_PIPE 结构尺寸从内核 UAPI 钉死**：7.3 树 include/uapi/linux/qseecom.h 全文比对，重建头新增九结构+ioctl 编码+static assert——SAVE_PARTITION_HASH nr19=36B(0xC0249713)、IS_ES_ACTIVATED nr20=4B(0xC0049714)、QTEEC_OPEN_SESSION nr30=64B(0xC040971E)、CLOSE_SESSION nr31=32B(0xC020971F)、INVOKE_MODFD nr32=64B(0xC0409720)、REQUEST_CANCELLATION nr33=64B(0xC0409721)、MDTP_CIPHER_DIP nr34=32B(0xC0209722)、GET/FREE/QUERY_CE_PIPE nr40-42=80B(0xC0509728/29/2A)、SET_ICE_INFO nr43=4B(0xC004972B，**驱动 dispatch 无此 case**，仅编码对齐)。原 Q1 表"computed"标签全部升级为"source-pinned"。
9. **✅ sb 部分使用边界已实证**：驱动 `qseecom_set_client_mem_param`（移植树 qseecom.c:1524-1578）——`len=buf->len`（dma-buf 全长）与 `req.sb_len` 只作 `len < sb_len → -EINVAL` 下限校验，`data->client.sb_length = req.sb_len` ⇒ **sb_len < dma-buf 长度官方支持**；listener 侧 `__qseecom_set_sb_memory` 同构。重建库传 PAGE_ALIGN 后等长参数本就满足；运行时验证 = `qsee_selftest` S2（正向 4096/8192 过 + 反向 12288/8192 -EINVAL 拒）。

**遗留（全部为上机项，非桌面可解）**：S0 堆节点在机确认、S3/S4/S5 TZ 侧真跑（依赖 keymaster64 TA 与 TZ bring-up）、`strace` ioctl 序列对比、/dev/dma_heap ueventd 权限行落地（Q6 集成清单）。
