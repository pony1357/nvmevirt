// SPDX-License-Identifier: GPL-2.0-only

#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "conv_ftl.h"

#define GC_MODE COST_BENEFIT
#define GREEDY 0
#define COST_BENEFIT 1
#define RANDOM 2

static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return conv_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG_VERBOSE("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
			ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	return conv_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	conv_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
	conv_ftl->wfc.write_credits--;
}

static void foreground_gc(struct conv_ftl *conv_ftl);

static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(conv_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->blks_per_pl;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static void remove_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->lm.victim_line_pq);
	vfree(conv_ftl->lm.lines);
}

static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

static struct write_pointer *__get_wp(struct conv_ftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}

static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);
	struct line *curline = get_next_free_line(conv_ftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer){
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}

static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	/* 1. 기본 설정 및 포인터 획득 */
	struct ssdparams *spp = &conv_ftl->ssd->sp;  // SSD 하드웨어 설정값
	struct line_mgmt *lm = &conv_ftl->lm;  // Line Mgmt
	// 요청 타입(사용자 IO인지 GC IO인지)에 맞는 쓰기 포인터(wpp)를 가져옴
	struct write_pointer *wpp = __get_wp(conv_ftl, io_type);

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	/* 2. 페이지 번호 증가 */
	check_addr(wpp->pg, spp->pgs_per_blk);  // 현재 페이지 번호가 블록 범위를 넘지 않는지 검사
	wpp->pg++;  // 페이지 번호 1 증가 (4KB)

	/* 3. 워드라인 완료 체크 */
	// pgs_per_oneshotpg: 한 번에 물리적으로 기록되는 페이지 묶음
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)  // 4KB 페이지를 하나 썼는데 아직 WL이 안 끝났다면 그냥 나감(out)
		goto out;

	/* 4. 채널 이동 (스트라이핑) */
	// WL을 다 채웠다면 병렬성을 위해 다음 채널로 이동
	wpp->pg -= spp->pgs_per_oneshotpg;  // 페이지 번호를 해당 워드라인의 시작점으로 되돌림
	check_addr(wpp->ch, spp->nchs);  // 채널 범위에 있는지 확인
	wpp->ch++;  // 다음 채널로 이동
	if (wpp->ch != spp->nchs)  // 아직 모든 채널을 다 돌지 않았다면 다음 채널에서 쓰기 위해 나감
		goto out;

		/* 5. LUN 이동 */
	wpp->ch = 0;  // 모든 채널을 다 돌았으므로 다시 0번 채널로 리셋
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;  // 해당 채널의 다음 LUN으로 이동
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)  // 아직 모든 LUN을 다 돌지 않았다면 나감
		goto out;

	/* 6. 다음 워드라인으로 진입 */
	wpp->lun = 0;  // 모든 LUN까지 다 돌았으므로 다시 0번 LUN으로 리셋
	/* 이제 현재 블록 내에서 다음 수직 위치로 페이지 번호를 점프시킴 */
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(conv_ftl);
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_maptbl(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->maptbl);
}

static void init_rmap(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->rmap);
}

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;

	/* initialize maptbl */
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(conv_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(conv_ftl, USER_IO);
	prepare_write_pointer(conv_ftl, GC_IO);

	init_write_flow_control(conv_ftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	remove_lines(conv_ftl);
	remove_rmap(conv_ftl);
	remove_maptbl(conv_ftl);
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	conv_init_params(&cpp);

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(conv_ftls[i].ssd->pcie->perf_model);
		kfree(conv_ftls[i].ssd->pcie);
		kfree(conv_ftls[i].ssd->write_buffer);

		conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
		conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void conv_remove_namespace(struct nvmev_ns *ns)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from conv_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		conv_ftls[i].ssd->pcie = NULL;
		conv_ftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		conv_remove_ftl(&conv_ftls[i]);
		ssd_remove(conv_ftls[i].ssd);
		kfree(conv_ftls[i].ssd);
	}

	kfree(conv_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

	return true;
}

static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return (lpn < conv_ftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	return &(conv_ftl->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;  // 이 라인이 방금 전까지 100% 꽉 찬 상태였는지 체크용
	struct line *line;

	/* [STEP 1] update corresponding (physical) page status */
	pg = get_pg(conv_ftl->ssd, ppa);  // 주소(ppa)를 가지고 실제 페이지 객체를 찾음
	NVMEV_ASSERT(pg->status == PG_VALID);  // 반드시 '유효' 상태여야 무효화가 가능함
	pg->status = PG_INVALID;  // 상태를 '무효'로 변경 (이제 쓰레기 데이터임)

	/* [STEP 2] update corresponding (physical) block status */
	blk = get_blk(conv_ftl->ssd, ppa);  // 해당 페이지가 속한 물리 블록을 찾음
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);  // 무효 페이지가 0 이상인지 확인 && 무효 페이지 개수가 블록의 전체 페이지 수 미만인지 확인
	blk->ipc++;			// 블록 내 무효 페이지 수(Invalid Page Count) 1 증가
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);  // 유효 페이지가 있는지 확인
	blk->vpc--;  // 블록 내 유효 페이지 수(Valid Page Count) 1 감소

	/* [STEP 3] update corresponding line status */
	line = get_line(conv_ftl, ppa);  // 해당 페이지가 속한 라인(여러 블록의 묶음)을 찾음
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);

  // 만약 현재 유효 페이지(vpc)가 전체 페이지 수와 같다면, 이 라인은 방금 전까지 100% 유효한 'Full' 상태였음
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;  // 라인 전체의 무효 페이지 수 1 증가
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);


	/* [STEP 4] Adjust the position of the victim line in the pq under over-writes */
	// line->pos는 이 라인이 현재 PQ의 몇 번째 인덱스에 있는지를 나타냄
  // 0이 아니라는 것은 이미 'victim_line_pq'에 들어있다는 뜻
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		/* * pqueue_change_priority: PQ 내부에서 이 라인의 순위를 즉시 재조정합니다.
     * 새로운 우선순위 값으로 'line->vpc - 1'을 전달합니다.
     * Greedy GC에서는 VPC가 낮을수록 우선순위가 높으므로, 이 라인은 큐의 위쪽(청소 1순위 쪽)으로 이동합니다.
     * [참고]: 이 함수 호출 내부에서 line->vpc 값도 실제로 1 감소시킵니다.
     */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		// 만약 PQ에 없다면(Active 라인이거나 Full 리스트에 있다면) 그냥 VPC 숫자만 줄임
		line->vpc--;
	}
		/* [STEP 5] move line: "full" -> "victim" */

		// 방금 전까지 100% 유효했던 라인인데 이제 쓰레기가 1개 생겼다면?
	if (was_full_line) {
/* 이 라인은 이제 '완벽함'을 잃었으므로 "Full 리스트"에서 빼서 "Victim PQ"로 던짐 */
		list_del_init(&line->entry);  // Full 리스트에서 제거
		lm->full_line_cnt--;  // Full 라인 개수 감소

		// 이제 공식적인 청소 후보가 되었으므로 PQ에 처음으로 삽입
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;  // Victim 라인 개수 증가
	}
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	/* advance conv_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
	new_ppa = get_new_page(conv_ftl, GC_IO);
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(conv_ftl->ssd, &gcw);
	}

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *victim_line = NULL;

	// kimi added
	if (GC_MODE == GREEDY)
		victim_line = pqueue_peek(lm->victim_line_pq);
	else if (GC_MODE == COST_BENEFIT)
		victim_line = cost_benefit_select(lm->victim_line_pq);
	else if (GC_MODE == RANDOM)
		victim_line = random_select(lm->victim_line_pq);
	// kimi added

	if (!victim_line) {
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		return NULL;
	}

	if (GC_MODE == GREEDY)
		pqueue_pop(lm->victim_line_pq);
	else if (GC_MODE == COST_BENEFIT || GC_MODE == RANDOM)
		pqueue_remove(lm->victim_line_pq, victim_line);


	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(conv_ftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(conv_ftl, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
/* 하나의 flashpg(16KB) 내부에 들어있는 여러 물리 페이지들(4KB)을 검사해서
   유효한 데이터(PG_VALID)가 있다면 다른 곳으로 이동시키는(GC Write) 함수 */
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp; // SSD의 물리적 특성 파라미터 가져오기
	struct convparams *cpp = &conv_ftl->cp;  // FTL 설정값 가져오기
	struct nand_page *pg_iter = NULL;   // 페이지를 하나씩 살펴볼 반복자
	int cnt = 0, i = 0;  // 유효 페이지 개수(cnt)와 루프 변수(i)
	uint64_t completed_time = 0; // 작업 완료 예상 시간
	struct ppa ppa_copy = *ppa;  // 원본 주소를 복사해서 사용 (주소 조작용)

	// [STEP 1] 현재 flash page 안에 유효한 데이터가 몇 개 있는지 먼저 스캔
	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy); // 현재 물리 주소(ppa_copy)의 페이지 객체 획득

		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);

		/* 만약 페이지가 VALID status라면 카운트 증가 */
		if (pg_iter->status == PG_VALID){
			cnt++;
		}

		ppa_copy.g.pg++; // 다음 페이지 주소로 이동
	}

	ppa_copy = *ppa; // 다시 처음 주소로 복구

	/* 유효한 데이터가 하나도 없으면 복사할 필요가 없으므로 바로 함수 종료 */
	if (cnt <= 0)
		return;

	// [STEP 2] 유효한 데이터 읽기 작업 시뮬레이션 (지연 시간 반영)
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,  // 유효 데이터를 읽어야 이사를 보내므로 READ 명령
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,  // 유효 페이지 개수만큼 데이터 크기 설정
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		// 낸드 장치에 읽기 명령을 보내고 완료 시간을 계산
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
	}

	/* [STEP 3] 유효한 데이터를 실제로 새로운 장소로 기록(Copy-Back) */
	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		/* 다시 확인해서 유효한 페이지인 경우에만 이사(gc_write_page) 실행 */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			/* 이 함수가 새로운 빈 블록을 찾아 데이터를 쓰고 매핑 테이블 업데이트 */
			gc_write_page(conv_ftl, &ppa_copy);
			conv_ftl->pg_cnt++;
		}

		ppa_copy.g.pg++; // 다음 페이지로 이동
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line = get_line(conv_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp; 
	struct ppa ppa; // 물리 주소 구조체
	int flashpg;


	// Select GC line.
	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
		return -1; // Exit if the line doesn't exist.
	}


	conv_ftl->gc_cnt++;

	ppa.g.blk = victim_line->id; // 선택된 Victim 라인의 ID를 물리 블록 번호로 설정

	// 현재 GC 상태(IPC, VPC, 프리 라인 개수 등)를 디버그 메시지로 출력
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, conv_ftl->lm.victim_line_cnt,
		    conv_ftl->lm.full_line_cnt, conv_ftl->lm.free_line_cnt);

	// ipc 만큼 나중에 데이터를 더 쓸 수 있도록 '크레딧'을 보충
	conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	// 라인 내의 모든 플래시 페이지(flashpg) 순회
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg; // 현재 조사할 페이지 번호 설정

		// 병렬 구조(Channel, LUN)를 모두 돌면서 데이터 확인
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;  // Plane 설정 (보통 0)

				lunp = get_lun(conv_ftl->ssd, &ppa); // 해당 물리 위치의 LUN 객체 가져오기

				// [핵심] 유효한 데이터가 있다면 다른 곳으로 복사 후 페이지 비우기
				// 'Valid Page Copy' 발생
				clean_one_flashpg(conv_ftl, &ppa);

				// 해당 블록의 마지막 페이지까지 다 확인했으면 
				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct convparams *cpp = &conv_ftl->cp;

					// 블록을 프리상태로 표시
					mark_block_free(conv_ftl, &ppa);

					// GC 지연 시뮬레이션이 활성화되어 있다면 실제로 NAND 소거(Erase) 명령 보내기
					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = GC_IO,
							.cmd = NAND_ERASE,  // NAND 소거 명령
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(conv_ftl->ssd, &gce);  // 실제/가상 낸드에 소거 수행
					}

					// LUN이 다시 사용 가능해지는 시간 업데이트
					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	// 전체 라인을 프리 라인 풀(Free pool)로 되돌려주어 다시 쓸 수 있게 만듦
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void foreground_gc(struct conv_ftl *conv_ftl)
{
	if (should_gc_high(conv_ftl)) {
		NVMEV_DEBUG_VERBOSE("should_gc_high passed");
		/* perform GC here until !should_gc(conv_ftl) */
		do_gc(conv_ftl, true);
	}
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    start_lpn, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa;
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest);
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}
// 실제복사는 io.c에서, conv-write는 복사행위를 계산하는 용도만
static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	/* 1. 네임스페이스로부터 FTL 인스턴스 배열 가져옴 */
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];


	/* wbuf and spp are shared by all instances */
	/* 2. 전역 자원 참조: 모든 FTL 인스턴스가 공유하는 SSD 파라미터(spp)와
	   컨트롤러 내부의 글로벌 쓰기 버퍼 주소를 가져옴 */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *wbuf = conv_ftl->ssd->write_buffer;


	/* 3. 요청 해석: NVMe 커맨드에서 시작 LBA와 전송할 LBA 개수를 추출 */
	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1); // length는 0-based라 1을 더함


	/* 4. 논리 페이지 번호(LPN) 계산: LBA(512B)를 낸드 페이지 단위인 LPN(4KB)으로 변환 */
	// secs_per_pg: 페이지 하나에 들어가는 섹터(LBA) 개수
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;  // 파티션(FTL 인스턴스) 개수

	uint64_t nsecs_latest;  // 전체 쓰기 작업 중 가장 늦게 끝난 시간 (낸드 완료 시간)
	uint64_t nsecs_xfer_completed;  // 호스트에서 컨트롤러 버퍼로 데이터 전송이 완료된 시간
	uint32_t allocated_buf_size;  // 할당받은 버퍼 크기


	/* 5. 낸드 커맨드 설정: 물리적인 낸드 쓰기 동작을 정의 */
	struct nand_cmd swr = {
		.type = USER_IO,  // 호스트가 요청한 일반 IO
		.cmd = NAND_WRITE,  // 쓰기 동작
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,  // 전송 단위 (oneshot page size)
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);

	/* 6. 범위 검사: 요청된 LPN이 FTL이 관리하는 전체 페이지 범위를 벗어나는지 체크 */
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	/* 7. 쓰기 버퍼 할당: 데이터를 낸드에 쏘기 전, 컨트롤러 내 SRAM/DRAM 버퍼 공간을 확보 */
	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;  // 버퍼 공간 부족 시 실패

	/* 8. 데이터 전송 시간 시뮬레이션: 호스트에서 컨트롤러 버퍼로 데이터가 넘어오는 DMA 시간 계산 */
	nsecs_latest =
		ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;  // 호스트-컨트롤러 간 전송 완료 시점 기록

	/* 낸드 동작은 버퍼 전송이 끝난 시점(stime)부터 시작 가능 */
	swr.stime = nsecs_latest;

	/* 9. 루프 시작: 요청된 모든 LPN에 대해 하나씩 처리 수행 */
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		/* 파티션 분산: LPN을 파티션 수(nr_parts)로 나눈 나머지로 담당 FTL 결정 -> 병렬처리 가능하게 함 */
		conv_ftl = &conv_ftls[lpn % nr_parts];
		/* 해당 FTL 내부에서 사용할 상대적 주소(local LPN) 계산 */
		local_lpn = lpn / nr_parts;

		/* [중요 포인트 A: 덮어쓰기 발생!] */
    /* 기존 맵 확인: 이 LPN이 예전에 쓰인 적이 있는지 매핑 테이블을 뒤져봅니다. */
		ppa = get_maptbl_ent(
			conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			/* 무효화(Invalidate): 이전에 쓰였던 물리 주소(old ppa)를 무효 처리
       * [PQ 영향]: 이 순간 해당 라인의 VPC가 1 감소
       * Greedy 알고리즘에 의해 이 라인은 PQ 내에서 '더 매력적인 청소 후보'가 되어 위로 올라감 
       * Cost-Benefit에서는 여기서 'age'를 계산하여 이 라인이 Hot인지 Cold인지 판단 */
			if (GC_MODE == COST_BENEFIT){
				// kimi added
				struct line *line = get_line(conv_ftl, &ppa);
				line->age = ktime_get_ns();
				// kimi added
			}

			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa); // 역매핑 테이블도 무효화
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		}

		/* new write */
		/* 10. 새 페이지 할당: 데이터를 실제로 저장할 새로운 빈 물리 공간(ppa)을 할당받음 */
		ppa = get_new_page(conv_ftl, USER_IO);

		/* 매핑 업데이트: "이제 이 LPN은 이 PPA에 들어있다"고 장부(maptbl, rmap)를 갱신 */
		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

		/* 유효화(Validate): 새 PPA에 데이터가 들어왔으므로 유효 상태로 마킹합니다. 
     * [PQ 영향]: 이 라인이 속한 새로운 라인의 VPC가 1 증가합니다. */
		mark_page_valid(conv_ftl, &ppa);

		/* need to advance the write pointer here */
		/* 11. 쓰기 포인터 전진: 해당 FTL의 다음 빈 페이지 위치를 한 칸(4KB) 옮김 */
		advance_write_pointer(conv_ftl, USER_IO);

		/* Aggregate write io in flash page */
		/* 12. 낸드 실제 쓰기 트리거: 원샷 페이지(TLC/MLC 등)의 마지막 페이지까지 데이터가 모였는지 확인 */
		if (last_pg_in_wordline(conv_ftl, &ppa)) {
			swr.ppa = &ppa;

			/* 낸드 미디어에 실제로 기록되는 지연 시간(latency)을 시뮬레이션에 반영 */
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
			/* 여러 낸드 동작 중 가장 늦게 끝나는 시간을 전체 완료 시간으로 갱신 */
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			/* 스케줄링: 낸드 쓰기가 완료된 후 버퍼를 비우는 내부 작업 예약 */
			schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		/* 13. 쓰기 크레딧(Credit) 관리: 호스트의 쓰기 속도를 제어(Flow control)하기 위해
					 남은 쓰기 권한을 소모하고, 필요시 GC 상태를 체크하여 보충 */
		consume_write_credit(conv_ftl);
		check_and_refill_write_credit(conv_ftl);
	}

	/* 14. 응답 시간 결정 */
	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		/* FUA(Force Unit Access)이거나 조기 완료 미사용 시:
			 데이터가 낸드 셀에 완전히 기록될 때까지 기다렸다가 응답 */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		/* 데이터가 컨트롤러 버퍼에만 안전하게 들어오면 바로 "쓰기 완료"로 응답 */
		ret->nsecs_target = nsecs_xfer_completed;
	}

	/* 15. 최종 성공 상태 반환 */
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	uint64_t gc_cnts = 0, pg_cnts = 0;
	
	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	
   for (i = 0; i < ns->nr_parts; i++) {
      gc_cnts += conv_ftls[i].gc_cnt;
      pg_cnts += conv_ftls[i].pg_cnt;
   }
   NVMEV_INFO("GC count: %llu\tCopy Page(4KB) Count: %llu\n", gc_cnts, pg_cnts);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!conv_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
