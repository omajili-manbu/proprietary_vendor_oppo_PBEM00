# qsee_recon — vendor qseecomd / libQSEEComAPI 逆向重建件（PBEM00, SDM670）

OPPO R17 (PBEM00) lineage-20 vendor blob 的 `qseecomd` / `libQSEEComAPI.so`
逆向重建，为 7.3 内核的 4.9-ABI `/dev/qseecom` 遗留驱动配套的用户态件：

- `/dev/ion` 分配路径整体替换为上游 dma-buf heaps
  （`/dev/dma_heap/qseecom_ta_region` → `default_cma_region` 链式回退，
  `QSEE_DMA_HEAP=<name>` 可钉死单堆）；
- ioctl 序列、结构布局（编译期 `_Static_assert` 全量锁定）、30 个导出符号
  与 vendor 逐位对齐（SONAME 保持 `libQSEEComAPI.so`）；
- `qseecomd` 服务表（10 项 listener）与两阶段启动逐常量复刻；libspl/libops
  从未出厂（lineage blob 与 ColorOS 7.1 vendor 双源缺席），缺库→日志→跳过
  即 vendor 原味。

## 布局

```
REPORT.md                  逆向报告：ioctl/TA/listener/sb 所有权全案
                           + 9 条 OPEN 问题收口记录（2026-11-21 全闭）
src/qsee_lib/              重建 libQSEEComAPI.so（aarch64 交叉，见 Makefile）
src/qseecomd/              重建 qseecomd（dlopen 真服务库；QSEECOMD_BUILTIN_SERVICES=1
                           时改用内置 listener 桩，仅供实验室）
src/selftest/              qsee_selftest 探针梯子 S0-S5（heap/版本/sb 边界/
                           listener/app/modfd 瞬态补丁），S0-S2 不依赖重建库
work_scripts/              分析脚本（对不可信数据纯结构解析）：服务表重建
                           reloc_c7.py（argv 传二进制路径）、handle+0x20
                           消费者扫描 scan_handle_0x20.py（stdin 喂 objdump）
```

## 构建（WSL，aarch64 交叉）

```
cd src/qsee_lib   && make && make check
cd src/qseecomd   && make && make check
cd src/selftest   && make && make check
```

原始证据（ColorOS 7.1 vendor 分区抽取物、findings 扫描产物、IDA dump）留在
本地工作区，不入库；报告内引用的 `work/...` 路径均指该工作区。

## 集成要点（详见 REPORT.md Q6）

1. `vendor/bin/qseecomd` + `vendor/lib64/libQSEEComAPI.so` 按原路径替换；
2. `/dev/qseecom 0660 system drmrpc`（ueventd 原行不变）；
3. `/dev/dma_heap/{qseecom_ta_region,default_cma_region,system}` 需要
   `0660 system drmrpc` 类权限（gki_defconfig 已含 DMABUF_HEAPS_SYSTEM/CMA）；
4. sepolicy 需补 qseecom_device 与 dma_heap 节点的 allow 规则。
