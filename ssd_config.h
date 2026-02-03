// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_SSD_CONFIG_H
#define _NVMEVIRT_SSD_CONFIG_H

/* SSD Model */
#define INTEL_OPTANE 0
#define SAMSUNG_970PRO 1
#define ZNS_PROTOTYPE 2
#define KV_PROTOTYPE 3
#define WD_ZN540 4

/* SSD Type */
#define SSD_TYPE_NVM 0
#define SSD_TYPE_CONV 1
#define SSD_TYPE_ZNS 2
#define SSD_TYPE_KV 3

/* Cell Mode */
#define CELL_MODE_UNKNOWN 0
#define CELL_MODE_SLC 1
#define CELL_MODE_MLC 2
#define CELL_MODE_TLC 3
#define CELL_MODE_QLC 4

/* Must select one of INTEL_OPTANE, SAMSUNG_970PRO, or ZNS_PROTOTYPE
 * in Makefile */

#if (BASE_SSD == INTEL_OPTANE)
#define NR_NAMESPACES 1

#define NS_SSD_TYPE_0 SSD_TYPE_NVM
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (5)
#define CELL_MODE (CELL_MODE_UNKNOWN)

#define LBA_BITS (9)
#define LBA_SIZE (1 << LBA_BITS)

#elif (BASE_SSD == KV_PROTOTYPE)
#define NR_NAMESPACES 1

#define NS_SSD_TYPE_0 SSD_TYPE_KV
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (5)
#define CELL_MODE (CELL_MODE_MLC)

enum {
	ALLOCATOR_TYPE_BITMAP,
	ALLOCATOR_TYPE_APPEND_ONLY,
};

#define KV_MAPPING_TABLE_SIZE GB(1)
#define ALLOCATOR_TYPE ALLOCATOR_TYPE_APPEND_ONLY

#define LBA_BITS (9)
#define LBA_SIZE (1 << LBA_BITS)

#elif (BASE_SSD == SAMSUNG_970PRO)
#define NR_NAMESPACES 1  // 가상 SSD가 가질 네임스페이스(디스크) 개수 (host(OS)에 보여줄 디스크 개수)

#define NS_SSD_TYPE_0 SSD_TYPE_CONV // 0번 네임스페이스 타입: 일반적인 SSD(Conventional)
#define NS_CAPACITY_0 (0)  // 0번 네임스페이스 용량 (0이면 기본값 사용)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0 // 1번 네임스페이스 타입 (사용 안 함)
#define NS_CAPACITY_1 (0)  // 1번 네임스페이스 용량 (사용 안 함)
#define MDTS (6)  // Maximum Data Transfer Size (NVMe 규격: 호스트와 컨트롤러 사이 전송 가능한 데이터 최대 크기): 2^MDTS * MPSMIN(4KB)
#define CELL_MODE (CELL_MODE_TLC) // 셀 저장 방식: TLC

#define SSD_PARTITIONS (4)  // SSD 내부 자원을 나눌 파티션(nparts) 수
#define NAND_CHANNELS (4)
#define LUNS_PER_NAND_CH (4)
#define PLNS_PER_LUN (1)
#define FLASH_PAGE_SIZE KB(16)
#define ONESHOT_PAGE_SIZE (FLASH_PAGE_SIZE * 3)
#define BLKS_PER_PLN (1024)
#define BLK_SIZE (0) /*BLKS_PER_PLN should not be 0 */
static_assert((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

#define MAX_CH_XFER_SIZE KB(16) /* to overlap with pcie transfer */ // 컨트롤러와 칩 간 최대 데이터 전송 크기
#define WRITE_UNIT_SIZE (512) // 쓰기 최소 단위 (Sector 크기)

#define NAND_CHANNEL_BANDWIDTH (800ull) //MB/s, 낸드 채널 대역폭
#define PCIE_BANDWIDTH (3360ull) //MB/s, PCIe 버스 대역폭

#define NAND_4KB_READ_LATENCY_LSB (35760 - 6000) //ns, 가장빠른 LSB 4KB 페이지
#define NAND_4KB_READ_LATENCY_MSB (35760 + 6000) //ns, 가장 느린 MSB 4KB 페이지
#define NAND_4KB_READ_LATENCY_CSB (35760) //not used
#define NAND_READ_LATENCY_LSB (36013 - 6000)  // 페이지 전체(16KB) 읽기
#define NAND_READ_LATENCY_MSB (36013 + 6000)
#define NAND_READ_LATENCY_CSB (36013) //not used
#define NAND_PROG_LATENCY (185000)
#define NAND_ERASE_LATENCY (0)

//LG:SLC portion in percentage
#define SLC_PORTION (10)  // 전체 블록 중 10%를 SLC 캐시로 사용
#define SLC_BLKS (BLKS_PER_PLN * SLC_PORTION / 100)  // 실제 SLC로 할당될 블록 수
#define SLC_ONESHOT_PAGE_SIZE KB(16) // SLC 모드에서의 원샷 페이지 크기: 16KB
#define NAND_4KB_READ_LATENCY_SLC (16254) // SLC 영역 4KB 읽기 지연 시간
#define NAND_READ_LATENCY_SLC (16369)  // SLC 영역 전체(16KB) 읽기 지연 시간
#define NAND_PROG_LATENCY_SLC (40547)
#define NAND_ERASE_LATENCY_SLC (0)

//#define NAND_4KB_READ_LATENCY_LSB (1) //ns
//#define NAND_4KB_READ_LATENCY_MSB (1) //ns
//#define NAND_4KB_READ_LATENCY_CSB (1) //not used
//#define NAND_READ_LATENCY_LSB (1)
//#define NAND_READ_LATENCY_MSB (1)
//#define NAND_READ_LATENCY_CSB (1) //not used
//#define NAND_PROG_LATENCY (1)
//#define NAND_ERASE_LATENCY (1)

/* 펌웨어(Controller) 오버헤드 지연 시간 */
#define FW_4KB_READ_LATENCY (21500)  // 펌웨어 레벨의 4KB 읽기 처리시간
#define FW_READ_LATENCY (30490)  // 펌웨어 레벨의 페이지 읽기 처리시간
#define FW_WBUF_LATENCY0 (4000)  // 쓰기 버퍼 처리 기본 지연시간
#define FW_WBUF_LATENCY1 (460)  // 쓰기 버퍼 데이터 양에 따른 가변 지연시간
#define FW_CH_XFER_LATENCY (0)  // 채널 전송 펌웨어 오버헤드 (채널 전송을 CPU에게 명령하는 소프트웨어적인 지연 시간)
#define OP_AREA_PERCENT (0.07)  // Over-Provisioning(예비 공간) 비율: 7%

// 전역 write buffer 크기 계산
#define GLOBAL_WB_SIZE (NAND_CHANNELS * LUNS_PER_NAND_CH * ONESHOT_PAGE_SIZE * 2)  // DRAM에 위치, 호스트가 보낸 데이터 임시 보관 
#define WRITE_EARLY_COMPLETION 1 // 쓰기 완료 보고 시점 설정 (버퍼에 써지면 바로 완료 보고)

#define LBA_BITS (9) // 2^9 = 512
#define LBA_SIZE (1 << LBA_BITS)

#elif (BASE_SSD == ZNS_PROTOTYPE)
#define NR_NAMESPACES 1

#define NS_SSD_TYPE_0 SSD_TYPE_ZNS
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (6)
#define CELL_MODE (CELL_MODE_TLC)

#define SSD_PARTITIONS (1)
#define NAND_CHANNELS (8)
#define LUNS_PER_NAND_CH (16)
#define FLASH_PAGE_SIZE KB(64)
#define PLNS_PER_LUN (1) /* not used*/
#define DIES_PER_ZONE (1)

#if 0
/* Real device configuration. Need to modify kernel to support zone size which is not power of 2*/
#define ONESHOT_PAGE_SIZE (FLASH_PAGE_SIZE * 3)
#define ZONE_SIZE MB(96) /* kernal only support zone size which is power of 2 */
#else /* If kernel is not modified, use this config for just testing ZNS*/
#define ONESHOT_PAGE_SIZE (FLASH_PAGE_SIZE * 2)
#define ZONE_SIZE MB(32)
#endif
static_assert((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

#define MAX_CH_XFER_SIZE (FLASH_PAGE_SIZE) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE (ONESHOT_PAGE_SIZE)

#define NAND_CHANNEL_BANDWIDTH (800ull) //MB/s
#define PCIE_BANDWIDTH (3200ull) //MB/s

#define NAND_4KB_READ_LATENCY_LSB (25485)
#define NAND_4KB_READ_LATENCY_MSB (25485)
#define NAND_4KB_READ_LATENCY_CSB (25485)
#define NAND_READ_LATENCY_LSB (40950)
#define NAND_READ_LATENCY_MSB (40950)
#define NAND_READ_LATENCY_CSB (40950)
#define NAND_PROG_LATENCY (1913640)
#define NAND_ERASE_LATENCY (0)

#define FW_4KB_READ_LATENCY (37540 - 7390 + 2000)
#define FW_READ_LATENCY (37540 - 7390 + 2000)
#define FW_WBUF_LATENCY0 (0)
#define FW_WBUF_LATENCY1 (0)
#define FW_CH_XFER_LATENCY (413)
#define OP_AREA_PERCENT (0)

#define GLOBAL_WB_SIZE (NAND_CHANNELS * LUNS_PER_NAND_CH * ONESHOT_PAGE_SIZE * 2)
#define ZONE_WB_SIZE (0)
#define WRITE_EARLY_COMPLETION 0

/* Don't modify followings. BLK_SIZE is caculated from ZONE_SIZE and DIES_PER_ZONE */
#define BLKS_PER_PLN 0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE (ZONE_SIZE / DIES_PER_ZONE)
static_assert((ZONE_SIZE % DIES_PER_ZONE) == 0);

/* For ZRWA */
#define MAX_ZRWA_ZONES (0)
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE (0)
#define ZRWA_BUFFER_SIZE (0)

#define LBA_BITS (9)
#define LBA_SIZE (1 << LBA_BITS)

#elif (BASE_SSD == WD_ZN540)
#define NR_NAMESPACES 1

#define NS_SSD_TYPE_0 SSD_TYPE_ZNS
#define NS_CAPACITY_0 (0)
#define NS_SSD_TYPE_1 NS_SSD_TYPE_0
#define NS_CAPACITY_1 (0)
#define MDTS (6)
#define CELL_MODE (CELL_MODE_TLC)

#define SSD_PARTITIONS (1)
#define NAND_CHANNELS (8)
#define LUNS_PER_NAND_CH (4)
#define PLNS_PER_LUN (1) /* not used*/
#define DIES_PER_ZONE (NAND_CHANNELS * LUNS_PER_NAND_CH)

#define FLASH_PAGE_SIZE KB(32)
#define ONESHOT_PAGE_SIZE (FLASH_PAGE_SIZE * 3)
/*In an emulator environment, it may be too large to run an application
  which requires a certain number of zones or more.
  So, adjust the zone size to fit your environment */
#define ZONE_SIZE GB(2ULL)

static_assert((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

#define MAX_CH_XFER_SIZE (FLASH_PAGE_SIZE) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE (512)

#define NAND_CHANNEL_BANDWIDTH (450ull) //MB/s
#define PCIE_BANDWIDTH (3050ull) //MB/s

#define NAND_4KB_READ_LATENCY_LSB (50000)
#define NAND_4KB_READ_LATENCY_MSB (50000)
#define NAND_4KB_READ_LATENCY_CSB (50000)
#define NAND_READ_LATENCY_LSB (58000)
#define NAND_READ_LATENCY_MSB (58000)
#define NAND_READ_LATENCY_CSB (58000)
#define NAND_PROG_LATENCY (561000)
#define NAND_ERASE_LATENCY (0)

#define FW_4KB_READ_LATENCY (20000)
#define FW_READ_LATENCY (13000)
#define FW_WBUF_LATENCY0 (5600)
#define FW_WBUF_LATENCY1 (600)
#define FW_CH_XFER_LATENCY (0)
#define OP_AREA_PERCENT (0)

#define ZONE_WB_SIZE (10 * ONESHOT_PAGE_SIZE)
#define GLOBAL_WB_SIZE (0)
#define WRITE_EARLY_COMPLETION 1

/* Don't modify followings. BLK_SIZE is caculated from ZONE_SIZE and DIES_PER_ZONE */
#define BLKS_PER_PLN 0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE (ZONE_SIZE / DIES_PER_ZONE)
static_assert((ZONE_SIZE % DIES_PER_ZONE) == 0);

/* For ZRWA */
#define MAX_ZRWA_ZONES (0)
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE (0)
#define ZRWA_BUFFER_SIZE (0)

#define LBA_BITS (9)
#define LBA_SIZE (1 << LBA_BITS)
#endif
///////////////////////////////////////////////////////////////////////////

static const uint32_t ns_ssd_type[] = { NS_SSD_TYPE_0, NS_SSD_TYPE_1 };
static const uint64_t ns_capacity[] = { NS_CAPACITY_0, NS_CAPACITY_1 };

#define NS_SSD_TYPE(ns) (ns_ssd_type[ns])
#define NS_CAPACITY(ns) (ns_capacity[ns])

/* Still only support NR_NAMESPACES <= 2 */
static_assert(NR_NAMESPACES <= 2);

#define SUPPORTED_SSD_TYPE(type) \
	(NS_SSD_TYPE_0 == SSD_TYPE_##type || NS_SSD_TYPE_1 == SSD_TYPE_##type)

#endif
