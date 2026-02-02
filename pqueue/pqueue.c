// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>
 * All rights reserved.
 */

#include <linux/prandom.h>
#include "../nvmev.h"
#include "pqueue.h"
#include "../conv_ftl.h"

#define left(i) ((i) << 1)
#define right(i) (((i) << 1) + 1)
#define parent(i) ((i) >> 1)

pqueue_t *pqueue_init(size_t n, pqueue_cmp_pri_f cmppri, pqueue_get_pri_f getpri,
		      pqueue_set_pri_f setpri, pqueue_get_pos_f getpos, pqueue_set_pos_f setpos)
{
	pqueue_t *q;

	pr_info_once(NVMEV_DRV_NAME ": pqueue: "
		     "Copyright (c) 2014, Volkan Yazıcı <volkan.yazici@gmail.com>. "
		     "All rights reserved.\n");

	if (!(q = kmalloc(sizeof(pqueue_t), GFP_KERNEL)))
		return NULL;

	/* Need to allocate n+1 elements since element 0 isn't used. */
	NVMEV_DEBUG("{alloc} n=%ld, size=%ld\n", n, (n + 1) * sizeof(void *));
	if (!(q->d = kmalloc((n + 1) * sizeof(void *), GFP_KERNEL))) {
		kfree(q);
		return NULL;
	}

	q->size = 1;
	q->avail = q->step = (n + 1); /* see comment above about n+1 */
	q->cmppri = cmppri;
	q->setpri = setpri;
	q->getpri = getpri;
	q->getpos = getpos;
	q->setpos = setpos;

	return q;
}

void pqueue_free(pqueue_t *q)
{
	kfree(q->d);
	kfree(q);
}

size_t pqueue_size(pqueue_t *q)
{
	/* queue element 0 exists but doesn't count since it isn't used. */
	return (q->size - 1);
}

static void bubble_up(pqueue_t *q, size_t i)
{
	size_t parent_node;
	void *moving_node = q->d[i];
	pqueue_pri_t moving_pri = q->getpri(moving_node);

	for (parent_node = parent(i);
	     ((i > 1) && q->cmppri(q->getpri(q->d[parent_node]), moving_pri));
	     i = parent_node, parent_node = parent(i)) {
		q->d[i] = q->d[parent_node];
		q->setpos(q->d[i], i);
	}

	q->d[i] = moving_node;
	q->setpos(moving_node, i);
}

static size_t maxchild(pqueue_t *q, size_t i)
{
	size_t child_node = left(i);

	if (child_node >= q->size)
		return 0;

	if ((child_node + 1) < q->size &&
	    q->cmppri(q->getpri(q->d[child_node]), q->getpri(q->d[child_node + 1])))
		child_node++; /* use right child instead of left */

	return child_node;
}

static void percolate_down(pqueue_t *q, size_t i)
{
	size_t child_node;
	void *moving_node = q->d[i];
	pqueue_pri_t moving_pri = q->getpri(moving_node);

	while ((child_node = maxchild(q, i)) &&
	       q->cmppri(moving_pri, q->getpri(q->d[child_node]))) {
		q->d[i] = q->d[child_node];
		q->setpos(q->d[i], i);
		i = child_node;
	}

	q->d[i] = moving_node;
	q->setpos(moving_node, i);
}

int pqueue_insert(pqueue_t *q, void *d)
{
	//void *tmp;
	size_t i;
	//size_t newsize;

	if (!q)
		return 1;

	/* allocate more memory if necessary */
	if (q->size >= q->avail) {
		NVMEV_ERROR("Need more space in pqueue\n");
		// newsize = q->size + q->step;
		// if (!(tmp = realloc(q->d, sizeof(void *) * newsize)))
		//     return 1;
		// q->d = tmp;
		// q->avail = newsize;
	}

	/* insert item */
	i = q->size++;
	q->d[i] = d;
	bubble_up(q, i);

	return 0;
}

void pqueue_change_priority(pqueue_t *q, pqueue_pri_t new_pri, void *d)
{
	size_t posn;
	pqueue_pri_t old_pri = q->getpri(d);

	q->setpri(d, new_pri);
	posn = q->getpos(d);
	if (q->cmppri(old_pri, new_pri))
		bubble_up(q, posn);
	else
		percolate_down(q, posn);
}

int pqueue_remove(pqueue_t *q, void *d)
{
	// 1. 삭제할 데이터(d)가 현재 큐의 몇 번째 인덱스(posn)에 있는지 찾기
	size_t posn = q->getpos(d);

// 2. [중요] 큐의 맨 마지막에 있는 데이터를 삭제할 위치(posn)로 끌어오기
// 그리고 큐의 전체 크기(size)를 1 줄이기 (--q->size)
	q->d[posn] = q->d[--q->size];


	// 3. 맨 뒤에서 옮겨온 데이터가 새로운 자리(posn)에서 
  // 위로 올라가야 할지(bubble_up), 아래로 내려가야 할지(percolate_down) 결정
  
  // 만약 삭제된 원래 데이터(d)의 우선순위보다 
  // 새로 옮겨온 데이터(q->d[posn])의 우선순위가 더 높다면 (cmppri 결과에 따라)
	if (q->cmppri(q->getpri(d), q->getpri(q->d[posn])))
	// 부모 노드들과 비교하며 위로 올리기
		bubble_up(q, posn);
	else
	// 자식 노드들과 비교하며 아래로 내리기
		percolate_down(q, posn);

	return 0;
}

void *pqueue_pop(pqueue_t *q)
{
	void *head;

	if (!q || q->size == 1)
		return NULL;

	head = q->d[1];
	q->d[1] = q->d[--q->size];
	percolate_down(q, 1);

	return head;
}

void *pqueue_peek(pqueue_t *q)
{
	void *d;
	
	if (!q || q->size == 1)
		return NULL;
	d = q->d[1];
	return d;
}

void *cost_benefit_select(pqueue_t *q){
	struct line *min_line;
	uint64_t min_res, now;
	size_t i;

	printk("Queue Size: %zu\n", pqueue_size(q));

	if (!q || q->size == 1)
		return NULL;

	min_line = NULL;
	min_res = 0xFFFFFFFFFFFFFFFF;
	now = ktime_get_ns();
	 
	for (i=1; i<q->size; i++){
		struct line *curr = (struct line *)q->d[i];
		uint64_t age, age_lvl, res;
		age = now - curr->age;
		do_div(age, 1000000000);

		// fio 실행시간 고려
		if (age <= 10) age_lvl = 1;
		else if (age <= 20) age_lvl = 2;
		else if (age <= 45) age_lvl = 3;
		else if (age <= 90) age_lvl = 4;
		else if (age <= 180) age_lvl = 5;
		else if (age <= 360) age_lvl = 6;
		else age_lvl = 7;

		res = (uint64_t)(curr->vpc);
		res <<= 10;
		do_div(res, (uint32_t)(curr->ipc) * age_lvl);
		
		
		if (res < min_res){
			min_res = res;
			min_line = curr;
		}	
	}
	if (min_line == NULL) {
    // 이게 찍힌다면 계산식 문제로 아무것도 못 고른 겁니다.
    printk("CB_DEBUG: Loop finished but min_line is NULL! min_res was %llu\n", min_res);
  } else {
    // 이게 찍힌다면 삭제(remove) 로직 문제입니다.
    printk("CB_DEBUG: Selected VPC %u, IPC %u, pos %zu\n", min_line->vpc, min_line->ipc, (size_t)min_line->pos);
  }

	return (void*)min_line;
}

void *random_select(pqueue_t *q){
	void *d;
	unsigned int rand;
	
	if (!q || q->size == 1)
		return NULL;
	
	rand = prandom_u32() % (q->size-1) + 1;

	d = q->d[rand];
	return d;
}

#if 0
void pqueue_dump(pqueue_t *q, FILE *out, pqueue_print_entry_f print)
{
    int i;

    fprintf(stdout,"posn\tleft\tright\tparent\tmaxchild\t...\n");
    for (i = 1; i < q->size ;i++) {
        fprintf(stdout,
                "%d\t%d\t%d\t%d\t%ul\t",
                i,
                left(i), right(i), parent(i),
                (unsigned int)maxchild(q, i));
        print(out, q->d[i]);
    }
}

static void set_pos(void *d, size_t val)
{
    /* do nothing */
}

static void set_pri(void *d, pqueue_pri_t pri)
{
    /* do nothing */
}

void pqueue_print(pqueue_t *q, FILE *out, pqueue_print_entry_f print)
{
    pqueue_t *dup;
	void *e;

    dup = pqueue_init(q->size, q->cmppri, q->getpri, set_pri, q->getpos, set_pos);
    dup->size = q->size;
    dup->avail = q->avail;
    dup->step = q->step;

    memcpy(dup->d, q->d, (q->size * sizeof(void *)));

    while ((e = pqueue_pop(dup)))
		print(out, e);

    pqueue_free(dup);
}
#endif



static int subtree_is_valid(pqueue_t *q, int pos)
{
	if (left(pos) < q->size) {
		/* has a left child */
		if (q->cmppri(q->getpri(q->d[pos]), q->getpri(q->d[left(pos)])))
			return 0;
		if (!subtree_is_valid(q, left(pos)))
			return 0;
	}
	if (right(pos) < q->size) {
		/* has a right child */
		if (q->cmppri(q->getpri(q->d[pos]), q->getpri(q->d[right(pos)])))
			return 0;
		if (!subtree_is_valid(q, right(pos)))
			return 0;
	}
	return 1;
}

int pqueue_is_valid(pqueue_t *q)
{
	return subtree_is_valid(q, 1);
}
