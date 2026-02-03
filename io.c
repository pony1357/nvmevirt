// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "dma.h"

#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
#include "ssd.h"
#else
struct buffer;
#endif

#undef PERF_DEBUG

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern bool io_using_dma;

static inline unsigned int __get_io_worker(int sqid)
{
#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
	return (sqid - 1) % nvmev_vdev->config.nr_io_workers;
#else
	return nvmev_vdev->io_worker_turn;
#endif
}

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static inline size_t __cmd_io_offset(struct nvme_rw_command *cmd)
{
	return (cmd->slba) << LBA_BITS;
}

static inline size_t __cmd_io_size(struct nvme_rw_command *cmd)
{
	return (cmd->length + 1) << LBA_BITS;
}

static unsigned int __do_perform_io(int sqid, int sq_entry)
{
	// 1. 초기 설정: Submission Queue와 해당 I/O 명령어 가져옴
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;  // 스토리지 내 저장 위치(오프셋)
	size_t length, remaining;  // 총 길이 및 남은 데이터 양
	int prp_offs = 0;  // 현재 몇 번째 PRP를 처리 중인지 카운트. PRP(Physical Region Page): 큰 데이터를 나누어 저장할 때의 주소 목록
	int prp2_offs = 0;  // PRP List 내부의 인덱스
	u64 paddr;  // 현재 처리할 호스트의 물리 주소
	u64 *paddr_list = NULL;  // PRP List가 저장된 페이지의 포인터. 호스트 물리주소 리스트(PRP list)가 담겨 있는 페이지를 CPU가 읽을 수 있도록 매핑한 가상주소(포인터)
	size_t nsid = cmd->nsid - 1; // 0-based   // 네임스페이스 ID
	bool is_paddr_memremap = false;

	// 명령어로부터 '스토리지' 오프셋과 전체 전송 크기를 계산
	offset = __cmd_io_offset(cmd);  // 가상 SSD 스토리지 내부의 절대 위치
	length = __cmd_io_size(cmd);
	remaining = length;

	// 2. 루프: 남은 데이터가 없을 때까지 페이지 단위로 처리
	while (remaining) {
		size_t io_size;
		void *vaddr;  // 호스트 물리 주소를 커널에서 접근하기 위한 가상 주소
		size_t mem_offs = 0;  // 페이지 내 시작 오프셋
		bool is_vaddr_memremap = false;

		prp_offs++;
		// 2-1. PRP 주소 추출 규칙 적용
		if (prp_offs == 1) {
			// 첫 번째 조각은 무조건 PRP1에 주소가 있음
			paddr = cmd->prp1;
		} else if (prp_offs == 2) {
			// 두 번째 조각: 남은 데이터가 1페이지 이하이면 PRP2가 곧 주소, 초과하면 PRP list의 시작 주소임
			paddr = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				// PRP2 주소가 유효한 시스템 메모리(RAM) 범위인지 확인
				if (pfn_valid(paddr >> PAGE_SHIFT)) {
					// 시스템 RAM이면 고속 매핑(kmap_atomic) 사용
					paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) +
						(paddr & PAGE_OFFSET_MASK);
				} else {
					// RAM이 아니면(ex. 예약된 영역 등) memremap으로 접근 권한 획득
					paddr_list = memremap(paddr, PAGE_SIZE, MEMREMAP_WT);
					paddr_list += (paddr & PAGE_OFFSET_MASK);
					is_paddr_memremap = true;  // 나중에 해제할 때 필요
				}
				// PRP List의 첫 번째 항목에서 실제 데이터 주소를 가져옴
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			// 세 번째 이후 조각은 미리 열어둔 PRP List에서 순차적으로 주소 획득
			paddr = paddr_list[prp2_offs++];
		}

		// 2-2. 호스트 물리 주소를 커널 가상 주소로 매핑
		// 데이터가 저장될 호스트 페이지(paddr)를 커널이 읽고 쓸 수 있게 가상 주소(vaddr)로 매핑
		if (pfn_valid(paddr >> PAGE_SHIFT)) {
			vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
		} else {
			vaddr = memremap(paddr, PAGE_SIZE, MEMREMAP_WT);
			is_vaddr_memremap = true;
		}

		// 이번 루프에서 처리할 크기 결정 (기본적으로 1페이지이나 마지막은 남은 양만큼)
		io_size = min_t(size_t, remaining, PAGE_SIZE);

		// 만약 주소가 페이지 중간부터 시작한다면 (Alignment 이슈), 오프셋 처리 및 크기 조정
		if (paddr & PAGE_OFFSET_MASK) {
			// 페이지 내에서 정확히 몇 바이트 지점에서 시작하는지 계산 (오프셋 추출)
			mem_offs = paddr & PAGE_OFFSET_MASK;
			// 이번 루프에서 복사할 크기가 현재 페이지의 경계를 넘어가는지 체크
			// NVMe PRP 규칙상, 첫 번째 PRP를 제외한 나머지 PRP는 반드시 페이지 시작점(0)부터 시작해야 함
			// 하지만 첫 번째 PRP는 페이지 중간에서 시작할 수 있고, 해당 페이지의 끝까지만 전송해야 함
			if (io_size + mem_offs > PAGE_SIZE)
				// 전송 크기를 현재 페이지의 남은 공간으로 제한
				io_size = PAGE_SIZE - mem_offs;
		}

		/* 2-3. 실제 데이터 복사 (CPU memcpy) */
		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			// Write: 호스트 -> 에뮬레이션된 장치 메모리
			memcpy(nvmev_vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			// Read: 에뮬레이션된 장치 메모리 -> 호스트
			memcpy(vaddr + mem_offs, nvmev_vdev->ns[nsid].mapped + offset, io_size);
		}

		// 2-4. 매핑 해제 (메모리 누수 방지)
		if (vaddr != NULL && !is_vaddr_memremap) {
			// kmap_atomic 으로 만든 가상 주소 즉시 무효화
			kunmap_atomic(vaddr);
			vaddr = NULL;
		} else if (vaddr != NULL && is_vaddr_memremap) {
			// memremap으로 매핑한 경우 memunmap 으로 해제
			memunmap(vaddr);
			vaddr = NULL;  // 해제 후 포인터를 초기화하여 잘못된 참조 방지
			is_vaddr_memremap = false;
		}

		remaining -= io_size;  // 실제 복사한 바이트 수(io_size)만큼 남은 작업량에서 차감
		offset += io_size;
	}

	// 3. 사용 완료된 PRP List 페이지 해제
	if (paddr_list) {
		// 데이터 페이지와 마찬가지로 매핑 방식에 따라 적절한 해제 함수 호출
		if (!is_paddr_memremap) 
			kunmap_atomic(paddr_list);
		else if (is_paddr_memremap) 
			memunmap(paddr_list);
	}
	paddr_list = NULL;

	return length;
}

static u64 paddr_list[513] = {
	0,
}; // Not using index 0 to make max index == num_prp
static unsigned int __do_perform_io_using_dma(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;
	bool is_memremap = false;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	memset(paddr_list, 0, sizeof(paddr_list));
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr_list[prp_offs] = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr_list[prp_offs] = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				if (pfn_valid(paddr_list[prp_offs] >> PAGE_SHIFT)) {
 					tmp_paddr_list = kmap_atomic_pfn(PRP_PFN(paddr_list[prp_offs])) + 
							(paddr_list[prp_offs] & PAGE_OFFSET_MASK);
 				} else {
 					tmp_paddr_list = memremap(paddr_list[prp_offs], PAGE_SIZE, MEMREMAP_WT);
 					tmp_paddr_list += (paddr_list[prp_offs] & PAGE_OFFSET_MASK);
 					is_memremap = true;
 				}
				paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_list != NULL && !is_memremap) {
 		kunmap_atomic(tmp_paddr_list);
 	} else if (tmp_paddr_list != NULL && is_memremap) {
 		memunmap(tmp_paddr_list);
 		is_memremap = false;
 	}

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[prp_offs] == paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			ioat_dma_submit(paddr, nvmev_vdev->config.storage_start + offset, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			ioat_dma_submit(nvmev_vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}

	return length;
}

static void __insert_req_sorted(unsigned int entry, struct nvmev_io_worker *worker,
				unsigned long nsecs_target)
{
	/**
	 * Requests are placed in @work_queue sorted by their target time.
	 * @work_queue is statically allocated and the ordered list is
	 * implemented by chaining the indexes of entries with @prev and @next.
	 * This implementation is nasty but we do this way over dynamically
	 * allocated linked list to minimize the influence of dynamic memory allocation.
	 * Also, this O(n) implementation can be improved to O(logn) scheme with
	 * e.g., red-black tree but....
	 */
	if (worker->io_seq == -1) {
		worker->io_seq = entry;
		worker->io_seq_end = entry;
	} else {
		unsigned int curr = worker->io_seq_end;

		while (curr != -1) {
			if (worker->work_queue[curr].nsecs_target <= worker->latest_nsecs)
				break;

			if (worker->work_queue[curr].nsecs_target <= nsecs_target)
				break;

			curr = worker->work_queue[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			worker->work_queue[worker->io_seq].prev = entry;
			worker->work_queue[entry].next = worker->io_seq;
			worker->io_seq = entry;
		} else if (worker->work_queue[curr].next == -1) { /* Tail */
			worker->work_queue[entry].prev = curr;
			worker->io_seq_end = entry;
			worker->work_queue[curr].next = entry;
		} else { /* In between */
			worker->work_queue[entry].prev = curr;
			worker->work_queue[entry].next = worker->work_queue[curr].next;

			worker->work_queue[worker->work_queue[entry].next].prev = entry;
			worker->work_queue[curr].next = entry;
		}
	}
}

static struct nvmev_io_worker *__allocate_work_queue_entry(int sqid, unsigned int *entry)
{
	unsigned int io_worker_turn = __get_io_worker(sqid);
	struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[io_worker_turn];
	unsigned int e = worker->free_seq;
	struct nvmev_io_work *w = worker->work_queue + e;

	if (w->next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		return NULL;
	}

	if (++io_worker_turn == nvmev_vdev->config.nr_io_workers)
		io_worker_turn = 0;
	nvmev_vdev->io_worker_turn = io_worker_turn;

	worker->free_seq = w->next;
	BUG_ON(worker->free_seq >= NR_MAX_PARALLEL_IO);
	*entry = e;

	return worker;
}

static void __enqueue_io_req(int sqid, int cqid, int sq_entry, unsigned long long nsecs_start,
			     struct nvmev_result *ret)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u[%d], sq %d cq %d, entry %d, %llu + %llu\n", worker->thread_name, entry,
		    sq_entry(sq_entry).rw.opcode, sqid, cqid, sq_entry, nsecs_start,
		    ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	w->sqid = sqid;
	w->cqid = cqid;
	w->sq_entry = sq_entry;
	w->command_id = sq_entry(sq_entry).common.command_id;
	w->nsecs_start = nsecs_start;
	w->nsecs_enqueue = local_clock();
	w->nsecs_target = ret->nsecs_target;
	w->status = ret->status;
	w->is_completed = false;
	w->is_copied = false;
	w->prev = -1;
	w->next = -1;

	w->is_internal = false;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, ret->nsecs_target);
}

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, size_t buffs_to_release)
{
	struct nvmev_io_worker *worker;  // 이 작업을 처리할 전용 워커 스레드
	struct nvmev_io_work *w;  // 구체적인 작업 내용을 담을 구조체
	unsigned int entry;  // 워커의 작업 큐 내에서의 인덱스(번호)

	/* 1. 워커 할당: 해당 Submission Queue(sqid)를 담당하는 I/O 워커와 빈 슬롯을 가져옴 */
	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return;  // 빈 자리가 없으면 작업을 등록하지 못하고 종료

	/* 2. 작업 위치 지정: 할당받은 인덱스를 사용해 실제 작업(work) 객체 주소를 얻음 */
	w = worker->work_queue + entry;

	/* 3. 디버그 로그: 현재 시간 대비 작업이 완료될 목표 시간까지의 남은 지연 시간 출력 */
	NVMEV_DEBUG_VERBOSE("%s/%u, internal sq %d, %llu + %llu\n", worker->thread_name, entry, sqid,
		    local_clock(), nsecs_target - local_clock());

	/////////////////////////////////
	/* 4. 작업 기본 정보 설정 */
	w->sqid = sqid;  // 어떤 큐에서 온 명령인지 기록
	// 시작 시간과 큐에 들어간 시간을 현재 시간으로 기록
	w->nsecs_start = w->nsecs_enqueue = local_clock();
	w->nsecs_target = nsecs_target;  // 낸드 지연 시간이 반영된 "실제 완료될 미래 시간"
	w->is_completed = false;  // 아직 시작 전이므로 완료 플래그는 거짓
	w->is_copied = true;  // 데이터 복사가 이미 완료되었음을 표시
	w->prev = -1;  // 리스트 연결용 (초기값 -1)
	w->next = -1; // 리스트 연결용 (초기값 -1)

	/* 5. 내부 작업 특수 설정 (핵심) */
	w->is_internal = true;  // 호스트 명령이 아닌 FTL 내부의 "관리용 작업"임을 표시
	w->write_buffer = write_buffer;  // 작업 완료 시 해제해야 할 쓰기 버퍼의 주소
	w->buffs_to_release = buffs_to_release;  // 해제할 버퍼의 크기 (데이터 전송 완료 후 빈 공간 확보용)

	/* 6. 메모리 베리어: 멀티코어 환경에서 다른 스레드(워커)가 업데이트된 w의 내용을 즉시 보도록 보장 */
	// conv-write를 실행하는 코어와 io-worker 스레드가 떠 있는 코어가 다를 수 있기 때문
	mb(); /* IO worker shall see the updated w at once */

	/* 7. 작업 삽입: 목표 시간(nsecs_target)에 맞춰 정렬된 상태로 워커의 작업 목록에 집어넣음
	  나중에 워커 스레드는 시간이 빠른 순서대로 이 작업들을 꺼내 처리 */
	__insert_req_sorted(entry, worker, nsecs_target);
}

static void __reclaim_completed_reqs(void)
{
	unsigned int turn;

	for (turn = 0; turn < nvmev_vdev->config.nr_io_workers; turn++) {
		struct nvmev_io_worker *worker;
		struct nvmev_io_work *w;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		worker = &nvmev_vdev->io_workers[turn];

		first_entry = worker->io_seq;
		curr = first_entry;

		while (curr != -1) {
			w = &worker->work_queue[curr];
			if (w->is_completed == true && w->is_copied == true &&
			    w->nsecs_target <= worker->latest_nsecs) {
				last_entry = curr;
				curr = w->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

		if (last_entry != -1) {
			w = &worker->work_queue[last_entry];
			worker->io_seq = w->next;
			if (w->next != -1) {
				worker->work_queue[w->next].prev = -1;
			}
			w->next = -1;

			w = &worker->work_queue[first_entry];
			w->prev = worker->free_seq_end;

			w = &worker->work_queue[worker->free_seq_end];
			w->next = first_entry;

			worker->free_seq_end = last_entry;
			NVMEV_DEBUG_VERBOSE("%s: %u -- %u, %d\n", __func__,
					first_entry, last_entry, nr_reclaimed);
		}
	}
}

static size_t __nvmev_proc_io(int sqid, int sq_entry, size_t *io_size)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	unsigned long long nsecs_start = __get_wallclock();
	struct nvme_command *cmd = &sq_entry(sq_entry);
#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0; // Some KVSSD programs give 0 as nsid for KV IO
#else
	uint32_t nsid = cmd->common.nsid - 1;
#endif
	struct nvmev_ns *ns = &nvmev_vdev->ns[nsid];

	struct nvmev_request req = {
		.cmd = cmd,
		.sq_id = sqid,
		.nsecs_start = nsecs_start,
	};
	struct nvmev_result ret = {
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
	};

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	if (!ns->proc_io_cmd(ns, &req, &ret))
		return false;
	*io_size = __cmd_io_size(&sq_entry(sq_entry).rw);

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	__enqueue_io_req(sqid, sq->cqid, sq_entry, nsecs_start, &ret);

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	__reclaim_completed_reqs();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n", clock1 / counter, clock2 / counter,
			    clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return true;
}

int nvmev_proc_io_sq(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;

	if (unlikely(!sq))
		return old_db;
	if (unlikely(num_proc < 0))
		num_proc += sq->queue_size;

	for (seq = 0; seq < num_proc; seq++) {
		size_t io_size;
		if (!__nvmev_proc_io(sqid, sq_entry, &io_size))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		sq->stat.nr_dispatched++;
		sq->stat.nr_in_flight++;
		sq->stat.total_io += io_size;
	}
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + seq) % sq->queue_size;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		int sqid = cq_entry(i).sq_id;
		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}

		/* Should check the validity here since SPDK deletes SQ immediately
		 * before processing associated CQes */
		if (!nvmev_vdev->sqes[sqid]) continue;

		nvmev_vdev->sqes[sqid]->stat.nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1)
		cq->cq_tail = cq->queue_size - 1;
}

static void __fill_cq_result(struct nvmev_io_work *w)
{
	int sqid = w->sqid;
	int cqid = w->cqid;
	int sq_entry = w->sq_entry;
	unsigned int command_id = w->command_id;
	unsigned int status = w->status;
	unsigned int result0 = w->result0;
	unsigned int result1 = w->result1;

	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	struct nvme_completion *cqe;
	int cq_head;

	spin_lock(&cq->entry_lock);
	cq_head = cq->cq_head;
	cqe = &cq_entry(cq_head);

	cqe->command_id = command_id;
	cqe->sq_id = sqid;
	cqe->sq_head = sq_entry;
	cqe->status = cq->phase | (status << 1);
	cqe->result0 = result0;
	cqe->result1 = result1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static int nvmev_io_worker(void *data)
{
	struct nvmev_io_worker *worker = (struct nvmev_io_worker *)data;
	struct nvmev_ns *ns;
	static unsigned long last_io_time = 0;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif

	NVMEV_INFO("%s started on cpu %d (node %d)\n", worker->thread_name, smp_processor_id(),
		   cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = worker->io_seq;
		int qidx;

		while (curr != -1) {
			struct nvmev_io_work *w = &worker->work_queue[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			worker->latest_nsecs = curr_nsecs;

			if (w->is_completed == true) {
				curr = w->next;
				continue;
			}

			if (w->is_copied == false) {
#ifdef PERF_DEBUG
				w->nsecs_copy_start = local_clock() + delta;
#endif
				if (w->is_internal) {
					;
				} else if (io_using_dma) {
					__do_perform_io_using_dma(w->sqid, w->sq_entry);
				} else {
#if (BASE_SSD == KV_PROTOTYPE)
					struct nvmev_submission_queue *sq =
						nvmev_vdev->sqes[w->sqid];
					ns = &nvmev_vdev->ns[0];
					if (ns->identify_io_cmd(ns, sq_entry(w->sq_entry))) {
						w->result0 = ns->perform_io_cmd(
							ns, &sq_entry(w->sq_entry), &(w->status));
					} else {
						__do_perform_io(w->sqid, w->sq_entry);
					}
#else 
					__do_perform_io(w->sqid, w->sq_entry);
#endif
				}

#ifdef PERF_DEBUG
				w->nsecs_copy_done = local_clock() + delta;
#endif
				w->is_copied = true;
				last_io_time = jiffies;

				NVMEV_DEBUG_VERBOSE("%s: copied %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);
			}

			if (w->nsecs_target <= curr_nsecs) {
				if (w->is_internal) {
#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
					buffer_release((struct buffer *)w->write_buffer,
						       w->buffs_to_release);
#endif
				} else {
					__fill_cq_result(w);
				}

				NVMEV_DEBUG_VERBOSE("%s: completed %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);

#ifdef PERF_DEBUG
				w->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu\n", w->nsecs_start,
					     w->nsecs_enqueue - w->nsecs_start,
					     w->nsecs_copy_start - w->nsecs_start,
					     w->nsecs_copy_done - w->nsecs_start,
					     w->nsecs_cq_filled - w->nsecs_start,
					     w->nsecs_target - w->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */
				w->is_completed = true;
			}

			curr = w->next;
		}

		for (qidx = 1; qidx <= nvmev_vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];

#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
			if ((worker->id) != __get_io_worker(qidx))
				continue;
#endif
			if (cq == NULL || !cq->irq_enabled)
				continue;

			if (mutex_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {
#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						NVMEV_DEBUG("Intr %d: %llu\n", qidx,
							    intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				mutex_unlock(&cq->irq_lock);
			}
		}
		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_io_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

void NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i, worker_id;

	nvmev_vdev->io_workers =
		kcalloc(nvmev_vdev->config.nr_io_workers, sizeof(struct nvmev_io_worker), GFP_KERNEL);
	nvmev_vdev->io_worker_turn = 0;

	for (worker_id = 0; worker_id < nvmev_vdev->config.nr_io_workers; worker_id++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[worker_id];

		worker->work_queue =
			kzalloc(sizeof(struct nvmev_io_work) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			worker->work_queue[i].next = i + 1;
			worker->work_queue[i].prev = i - 1;
		}
		worker->work_queue[NR_MAX_PARALLEL_IO - 1].next = -1;
		worker->id = worker_id;
		worker->free_seq = 0;
		worker->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		worker->io_seq = -1;
		worker->io_seq_end = -1;

		snprintf(worker->thread_name, sizeof(worker->thread_name), "nvmev_io_worker_%d", worker_id);

		worker->task_struct = kthread_create(nvmev_io_worker, worker, "%s", worker->thread_name);

		kthread_bind(worker->task_struct, nvmev_vdev->config.cpu_nr_io_workers[worker_id]);
		wake_up_process(worker->task_struct);
	}
}

void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		kfree(worker->work_queue);
	}

	kfree(nvmev_vdev->io_workers);
}
