# QSEE 用户态重建件翻转清单（挂 7.3 内核验收）

本目录 `qsee/` 源码（vendor blob 逆向重建 + dma-buf heaps 适配）为 PBEM00
qseecom 用户态栈的源码替代件，覆盖原 blob 三件：

- `bin/qseecomd`（10 项 TZ listener 守护）
- `lib64/libQSEEComAPI.so`（30 导出函数，SONAME 不变）
- （新增）`bin/qsee_selftest`（驱动/堆/TZ 链路探针梯子）

## 为什么只能跑 7.3

重建件把 4.9 `/dev/ion`（QSEECOM heap 27）分配路径整体替换为上游
dma-buf heaps（`/dev/dma_heap/qseecom_ta_region` → `default_cma_region`
链式回退）。4.9 内核无 dma-heap 节点，**在 4.9 上重建件必然分配失败**
（qseecomd 起、listener 全灭、keymaster/gatekeeper/指纹全断）。
配套内核侧=7.3 树 `0fd76cdcff48`（DMABUF_HEAPS_SYSTEM/CMA）+
qseecom 遗留驱动移植系列（fad1528bcf8a 起四笔）。blob 退役 + 源码构建
翻转已按项目决策执行（本仓库已全量围绕 7.3 开发）。

## 翻转动作（本次已执行）

1. vendor 仓：`opensource/qsee/` 入库（源码 + Android.bp + REPORT.md 逆向
   报告）；`proprietary/vendor/bin/qseecomd`、
   `proprietary/vendor/lib64/libQSEEComAPI.so` blob 退役；
   PBEM00-vendor.mk 删两行拷贝规则（`bin/qseecomd` / `lib64/libQSEEComAPI.so`）。
   `vendor/etc/init/qseecomd.rc` 保留（服务名/binary 路径不变，由源码模块接管）。
2. device 仓：device.mk `PRODUCT_PACKAGES += libQSEEComAPI qseecomd qsee_selftest`
   （置于 QRTR 栈块之后）；proprietary-files.txt 删 `vendor/bin/qseecomd` 与
   `vendor/lib64/libQSEEComAPI.so` 两行（`vendor/etc/init/qseecomd.rc` 保留）；
   ueventd.qcom.rc 增 `/dev/dma_heap/*` 权限四行（0660 system drmrpc，
   qseecomd 降权后为 uid/gid 1000，与 `/dev/qseecom` 行同款组）。
3. sepolicy：本树 sepolicy/vendor 仅有 file_contexts（QRTR 翻转同样未落
   per-domain .te）。若 7.3 实机出现 dma_heap denials（判定：
   `dmesg | grep -E "avc.*dma_heap"`），再补
   `allow <qseecomd 实际域> dma_heap_device:chr_file rw_file_perms;`
   类规则——域以实机 `ps -Z | grep qseecomd` 为准，勿盲写。

## 验收判据（7.3 内核实机）

1. `/dev/dma_heap/` 四节点在位：`qseecom_ta_region`（16MB reusable CMA）、
   `default_cma_region`（32MB linux,cma）、`system`、`system-uncached`
   （`qsee_selftest` S0 直接探测）。
2. `qsee_selftest` 梯子逐段过：
   - S0 堆探测 + 连续分配 sanity
   - S1 GET_QSEOS_VERSION == 0x14 (20)
   - S2 sb 部分使用双向边界（4096/8192 过，12288/8192 -EINVAL 拒）
   - S3 listener 0x7fff 注册/注销（TZ up 前提）
   - S4 `--app keymaster64 --base /vendor/firmware_mnt/image` start_app/
     send_cmd/shutdown
   - S5 SEND_MODFD 瞬态补丁（哨兵 pre-fill → 调用后清零断言）
3. keymaster/gatekeeper/fingerprint HAL 冒烟 + `strace -e ioctl` 对比
   重建件与 blob 的 ioctl 序列（除 ION 三连换 dma-heap 外逐条一致）。
4. qseecomd 日志链核对：10 项 listener 里 8 项 Started；libspl/libops
   两项 `Init dlopen(...) is failed....` 为预期（两库从未出厂——lineage
   blob 与 ColorOS 7.1 vendor 双源缺席，REPORT.md OPEN-1）。

## 设备补丁依据（已验证事实）

- 服务表/listener id/sb 尺寸/handle 布局/ioctl 编码：blob 全函数反编译
  + ColorOS 7.1 vendor 交叉验证（REPORT.md Q1-Q5、"ColorOS 交叉验证"节）
- 内核侧语义（modfd sg 瞬态补丁、sb 长度下限、SEND_CMD_SERVICE 无 app
  校验面）：7.3 树移植驱动源码逐行核验（REPORT.md OPEN-5/9）
- 分配策略：REPORT.md 第 0 节 heap 表（qseecom_ta_region 首选回退链，
  QSEE_DMA_HEAP 环境变量钉死单堆，20×50ms 重试=4.9 UAPI 参数等价）
