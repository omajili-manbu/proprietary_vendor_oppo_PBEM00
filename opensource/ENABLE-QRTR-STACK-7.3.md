# QRTR 三件套启用清单（挂 7.3 内核验收）

本目录源码（qrtr/rmtfs/pd-mapper，andersson 上游 + PBEM00 设备补丁）为 QRTR 传输栈。
当前内核（4.9，CONFIG_QRTR 未开、无 rmtfs_mem）上**不可运行**：blob 三件套继续服役，
PRODUCT_PACKAGES 与 blob 退役一律挂到 7.3 内核（CONFIG_QRTR + rmtfs_mem + qrtr-glink
到 modem 通）实机验收后一次翻转。严禁在 4.9 内核上翻转（rmtfs/pd-mapper 会 crash-loop，
EFS 服务器缺失将复现 modem 43s SSR 崩环）。

## 翻转步骤（7.3 验收通过后）

1. device.mk 增 `PRODUCT_PACKAGES += qrtr-ns qrtr-cfg qrtr-lookup libqrtr rmtfs pd-mapper`
2. PBEM00-vendor.mk 删 6 行拷贝规则：pd-mapper(82)/qrtr-cfg(88)/qrtr-lookup(89)/
   qrtr-ns(90)/rmt_storage(93)/libqrtr.so(894)（行号为翻转前快照，删前复核）
3. proprietary-files.txt 删 6 行：922/924/925/926/928/955，并删 proprietary/ 内对应 blob 文件
4. init/init.qcom.rc：`service vendor.rmt_storage` 二进制路径改
   `/vendor/bin/rmtfs -o /dev/block/by-name -P`（服务名与其余属性不变）
5. 验收判据：
   - `qrtr-lookup` 列出 servreg 服务（service 64 / ver 257）与 RMTFS 服务
   - rmtfs 收到 modem RMTFS_OPEN 并映射 modemst1/2、fsc、fsg、oppostanvbk、oppodycnvbk
   - modem/adsp/cdsp 重启（SSR）观察 ≥10 分钟零异常
   - EFS 写入持久（modemst1/2 mtime 随注册变化）

## 设备补丁依据（已验证事实）

- rmtfs storage.c：modem 请求串 `/oppo/oppostanvbk|/oppo/oppodycnvbk|/oppo/oem_partion`
  （stock blob strings 实存，且无 /oem/nvbk/*），by-name 标签 oppostanvbk=sdf3、
  oppodycnvbk=sdf2（设备实测）
- pd-mapper：jsn 实位 /vendor/firmware_mnt/image/{adspr,adspua,cdspr,modemr,modemuw}.jsn；
  NDK 产物设备实测枚举 9 条 service→PD（adsp 74 / cdsp 76 / modem 180）后死于
  AF_QIPCRTR（预期内核边界）
- blob 时代 OPPO 私有行为（uio/glink 通道、/proc/oppoVersion 门控、modemst 擦除逻辑）
  有意放弃，逐条裁决见三份石头报告
