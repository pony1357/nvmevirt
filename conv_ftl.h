// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

struct convparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/
};

struct line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	uint64_t age; // kimi added
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;
};

/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct line_mgmt {
	struct line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list; // free: when writing new datas
	pqueue_t *victim_line_pq; // partially valid: 
	struct list_head full_line_list;

	uint32_t tt_lines; // total lines #
	uint32_t free_line_cnt; // free lines # that can use
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

struct conv_ftl {  // 각 partition의 LBA -> PPA
	struct ssd *ssd; // FTL이 관리하는 물리 SSD 장치 객체에 대한 포인터

	struct convparams cp; // FTL 운영에 필요한 파라미터 모음 (Threshold, OP)
	struct ppa *maptbl; /* page(4KB) level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */   // GC할 때 사용, DRAM이 아니라 낸드 페이지의 남는 공간(Out-Of-Band)에 저장
	struct write_pointer wp; // Write pointer: 현재 데이터를 쓰고 있는 지점 (Offset: 4KB)
	struct write_pointer gc_wp; // GC-Write pointer: GC한 데이터들을 따로 모아놓아야 hot/cold 어느정도 따로 저장됨
	struct line_mgmt lm;
	struct write_flow_control wfc; // write credit: line별 남은 페이지수, 쓰기흐름 제어장치 - 호스트의 요청속도를 GC 속도가 못따라가면 SSD가 뻗어버릴 수 있으므로 GC 상태에 따라 호스트의 쓰기 속도 조절하는 용도

	// kimi added
	uint64_t gc_cnt, pg_cnt;
};

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void conv_remove_namespace(struct nvmev_ns *ns);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
