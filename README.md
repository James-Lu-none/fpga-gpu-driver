# PCIe FPGA GPU Liux Driver

## 運作模式

- **[Data Path]** User space 透過 `mmap()` 向驅動程式請求記憶體映射，驅動分配一塊 DMA Buffer 並直接映射給 User space
- **[Data Path]** User space 將要運算的資料直接寫入該映射位址
- **[Control Path]** 驅動程式在記憶體中維護 Ring Buffer，並將其實體位址告知 GPU 硬體
- **[Control Path]** User space 透過 `ioctl` 將運算指令寫入 Ring Buffer
- **[Control Path]** 驅動程式更新 Ring Buffer 的 Tail Pointer，並寫入 GPU 的 Doorbell 暫存器通知硬體
- **[Hardware]** GPU 的 Command Processor 讀取 Ring Buffer 裡的指令
- **[Hardware]** GPU 根據指令，透過 PCIe DMA 直接讀取 Data Buffer 進行運算，並將結果直接寫回 Data Buffer
- **[Hardware]** GPU 運算完畢，發出硬體中斷 (IRQ / MSI-X)
- **[Synchronization]** Kernel Driver 攔截中斷，喚醒在 Wait Queue 中等待的 User space 執行緒
- **[Result]** User space 被喚醒後從 mmap 的位址讀取運算結果

## Unified Virtual Memory with Zero-Copy Direct DMA via XDMA Scatter-Gather Descriptors

Originally, a fixed DMA buffer was allocated at probe time and mapped to user space via `mmap`. 
Now, the driver dynamically binds and pins user-space virtual memory on demand (`ioctl`) without pre-allocating contiguous physical RAM during probe:

- On probe: Allocates a 64KB coherent DMA buffer (`dev->desc_ring`) to hold up to 8192 XDMA Descriptors, and a 64KB array (`pinned_pages`) to hold `struct page *` pointers (supporting up to 32MB payload limit).
- On ioctl: Dynamically pins user-space pages based on `payload_size` using `get_user_pages_fast()`. Then establishes DMA mappings using `dma_map_sg()` (transparently supporting both IOMMU and Non-IOMMU hosts).
- XDMA Descriptor Chain: Converts the mapped scatterlist (`dev->sgl`) into an XDMA Hardware Descriptor Chain in `dev->desc_ring`, linking non-contiguous physical pages via `next_desc` pointers. The driver submits the first Descriptor's DMA address to the FPGA XDMA IP via MMIO, allowing XDMA to automatically traverse and stream all payload pages.

## 核心架構與功能 (Core Features & Architecture)

- **Basic Scaffolding & Char Device**: 完成 LKM 註冊、`open`/`release` 機制與基礎 Context 隔離。
- **IOCTL Command Submission & Doorbell**: 實作基礎指令協定，透過 `ioctl` (`copy_from_user`) 傳遞任務，完成單一 Ring Buffer 邏輯與 Doorbell 觸發。
- **Multi-Context Isolation & Scheduling**: 實作私有 Ring Buffer 與 Lock-free SPSC，並導入 Virtual Hardware Scheduler 處理多 Context 排程。
- **Zero-Copy MMAP & Virtual Bus**: 將記憶體存取升級為 `mmap`。實作純軟體 Memory Mapping，並嘗試註冊虛擬 Platform Device 以支援正規 DMA API 。
- **Hardware Simulation & Benchmarking**: 整合 Workqueue 模擬非同步運算，利用 Wait Queue 模擬中斷喚醒，並撰寫 User Space Benchmark 收集效能數據。

## build, install and check

```bash
make clean
make

# global queue + spin_lock vs private queue per context
sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=0
sudo ./tests/test_ioctl

sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1
sudo ./tests/test_ioctl

# test working queue & interrupt
sudo rmmod vgpu_core
sudo insmod driver/vgpu_core.ko queue_mode=1
sudo ./tests/test_interrupt
```

## 

```bash
user@bastion:~/workspace$ sudo lspci -vvv -nn -s 00:01.0
00:01.0 PCI bridge [0604]: Intel Corporation Device [8086:4c01] (rev 01) (prog-if 00 [Normal decode])
        Subsystem: ASUSTeK Computer Inc. Device [1043:8694]
        Control: I/O+ Mem+ BusMaster+ SpecCycle- MemWINV- VGASnoop- ParErr- Stepping- SERR- FastB2B- DisINTx+
        Status: Cap+ 66MHz- UDF- FastB2B- ParErr- DEVSEL=fast >TAbort- <TAbort- <MAbort- >SERR- <PERR- INTx-
        Latency: 0, Cache Line Size: 64 bytes
        Interrupt: pin ? routed to IRQ 121
        IOMMU group: 1
        Bus: primary=00, secondary=01, subordinate=01, sec-latency=0
        I/O behind bridge: f000-0fff [disabled] [16-bit]
        Memory behind bridge: a0a00000-a0bfffff [size=2M] [32-bit]
        Prefetchable memory behind bridge: 00000000fff00000-00000000000fffff [disabled] [64-bit]
        Secondary status: 66MHz- FastB2B- ParErr- DEVSEL=fast >TAbort- <TAbort- <MAbort- <SERR- <PERR-
        BridgeCtl: Parity- SERR+ NoISA- VGA- VGA16+ MAbort- >Reset- FastB2B-
                PriDiscTmr- SecDiscTmr- DiscTmrStat- DiscTmrSERREn-
        Capabilities: [40] Express (v2) Root Port (Slot+), MSI 00
                DevCap: MaxPayload 256 bytes, PhantFunc 0
                        ExtTag- RBE+
                DevCtl: CorrErr+ NonFatalErr+ FatalErr+ UnsupReq+
                        RlxdOrd- ExtTag- PhantFunc- AuxPwr- NoSnoop-
                        MaxPayload 256 bytes, MaxReadReq 128 bytes
                DevSta: CorrErr- NonFatalErr- FatalErr- UnsupReq- AuxPwr+ TransPend-
                LnkCap: Port #2, Speed 16GT/s, Width x16, ASPM not supported
                        ClockPM- Surprise- LLActRep+ BwNot+ ASPMOptComp+
                LnkCtl: ASPM Disabled; RCB 64 bytes, Disabled- CommClk+
                        ExtSynch- ClockPM- AutWidDis- BWInt- AutBWInt-
                LnkSta: Speed 5GT/s, Width x1
                        TrErr- Train- SlotClk+ DLActive+ BWMgmt+ ABWMgmt-
                SltCap: AttnBtn- PwrCtrl- MRL- AttnInd- PwrInd- HotPlug- Surprise-
                        Slot #1, PowerLimit 75W; Interlock- NoCompl+
                SltCtl: Enable: AttnBtn- PwrFlt- MRL- PresDet- CmdCplt- HPIrq- LinkChg-
                        Control: AttnInd Unknown, PwrInd Unknown, Power- Interlock-
                SltSta: Status: AttnBtn- PowerFlt- MRL- CmdCplt- PresDet+ Interlock-
                        Changed: MRL- PresDet- LinkState-
                RootCap: CRSVisible-
                RootCtl: ErrCorrectable- ErrNon-Fatal- ErrFatal- PMEIntEna- CRSVisible-
                RootSta: PME ReqID 0000, PMEStatus- PMEPending-
                DevCap2: Completion Timeout: Range ABC, TimeoutDis+ NROPrPrP- LTR+
                         10BitTagComp- 10BitTagReq- OBFF Via WAKE#, ExtFmt- EETLPPrefix-
                         EmergencyPowerReduction Not Supported, EmergencyPowerReductionInit-
                         FRS- LN System CLS Not Supported, TPHComp- ExtTPHComp- ARIFwd+
                         AtomicOpsCap: Routing- 32bit- 64bit- 128bitCAS-
                DevCtl2: Completion Timeout: 50us to 50ms, TimeoutDis- LTR+ 10BitTagReq- OBFF Disabled, ARIFwd-
                         AtomicOpsCtl: ReqEn+ EgressBlck+
                LnkCap2: Supported Link Speeds: 2.5-16GT/s, Crosslink- Retimer+ 2Retimers+ DRS-
                LnkCtl2: Target Link Speed: 5GT/s, EnterCompliance- SpeedDis-
                         Transmit Margin: Normal Operating Range, EnterModifiedCompliance- ComplianceSOS-
                         Compliance Preset/De-emphasis: -6dB de-emphasis, 0dB preshoot
                LnkSta2: Current De-emphasis Level: -6dB, EqualizationComplete- EqualizationPhase1-
                         EqualizationPhase2- EqualizationPhase3- LinkEqualizationRequest-
                         Retimer- 2Retimers- CrosslinkRes: unsupported
        Capabilities: [80] MSI: Enable+ Count=1/1 Maskable- 64bit-
                Address: fee001f8  Data: 0000
        Capabilities: [90] Subsystem: ASUSTeK Computer Inc. Device [1043:8694]
        Capabilities: [a0] Power Management version 3
                Flags: PMEClk- DSI- D1- D2- AuxCurrent=0mA PME(D0+,D1-,D2-,D3hot+,D3cold+)
                Status: D0 NoSoftRst- PME-Enable- DSel=0 DScale=0 PME-
        Capabilities: [100 v1] Advanced Error Reporting
                UESta:  DLP- SDES- TLP- FCP- CmpltTO- CmpltAbrt- UnxCmplt- RxOF- MalfTLP- ECRC- UnsupReq- ACSViol-
                UEMsk:  DLP- SDES- TLP- FCP- CmpltTO- CmpltAbrt- UnxCmplt+ RxOF- MalfTLP- ECRC- UnsupReq- ACSViol-
                UESvrt: DLP+ SDES- TLP- FCP- CmpltTO- CmpltAbrt- UnxCmplt- RxOF+ MalfTLP+ ECRC- UnsupReq- ACSViol-
                CESta:  RxErr- BadTLP- BadDLLP- Rollover- Timeout- AdvNonFatalErr-
                CEMsk:  RxErr- BadTLP- BadDLLP- Rollover- Timeout- AdvNonFatalErr+
                AERCap: First Error Pointer: 00, ECRCGenCap- ECRCGenEn- ECRCChkCap- ECRCChkEn-
                        MultHdrRecCap- MultHdrRecEn- TLPPfxPres- HdrLogCap-
                HeaderLog: 00000000 00000000 00000000 00000000
                RootCmd: CERptEn+ NFERptEn+ FERptEn+
                RootSta: CERcvd- MultCERcvd- UERcvd- MultUERcvd-
                         FirstFatal- NonFatalMsg- FatalMsg- IntMsg 0
                ErrorSrc: ERR_COR: 0000 ERR_FATAL/NONFATAL: 0000
        Capabilities: [220 v1] Access Control Services
                ACSCap: SrcValid+ TransBlk+ ReqRedir+ CmpltRedir+ UpstreamFwd+ EgressCtrl- DirectTrans-
                ACSCtl: SrcValid+ TransBlk- ReqRedir+ CmpltRedir+ UpstreamFwd+ EgressCtrl- DirectTrans-
        Capabilities: [150 v1] Precision Time Measurement
                PTMCap: Requester:- Responder:+ Root:+
                PTMClockGranularity: 4ns
                PTMControl: Enabled:+ RootSelected:+
                PTMEffectiveGranularity: Unknown
        Capabilities: [280 v1] Virtual Channel
                Caps:   LPEVC=0 RefClk=100ns PATEntryBits=1
                Arb:    Fixed- WRR32- WRR64- WRR128-
                Ctrl:   ArbSelect=Fixed
                Status: InProgress-
                VC0:    Caps:   PATOffset=00 MaxTimeSlots=1 RejSnoopTrans-
                        Arb:    Fixed- WRR32- WRR64- WRR128- TWRR128- WRR256-
                        Ctrl:   Enable+ ID=0 ArbSelect=Fixed TC/VC=ff
                        Status: NegoPending- InProgress-
        Capabilities: [a00 v1] Downstream Port Containment
                DpcCap: INT Msg #0, RPExt+ PoisonedTLP+ SwTrigger+ RP PIO Log 4, DL_ActiveErr+
                DpcCtl: Trigger:1 Cmpl- INT+ ErrCor- PoisonedTLP- SwTrigger- DL_ActiveErr-
                DpcSta: Trigger- Reason:00 INT- RPBusy- TriggerExt:00 RP PIO ErrPtr:1f
                Source: 0000
        Capabilities: [a30 v1] Secondary PCI Express
                LnkCtl3: LnkEquIntrruptEn- PerformEqu-
                LaneErrStat: 0
        Capabilities: [a90 v1] Data Link Feature <?>
        Capabilities: [a9c v1] Physical Layer 16.0 GT/s <?>
        Capabilities: [edc v1] Lane Margining at the Receiver <?>
        Kernel driver in use: pcieport
```