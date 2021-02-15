#define _GNU_SOURCE
#include <sys/types.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include "memlib.h"
#include "mm_thread.h"
#include <sched.h>

////////////////////////////////////////////////////////
///////////////////// MACROS ///////////////////////////
////////////////////////////////////////////////////////

/**
 * @brief We assume that page size is 4096 bytes which was
 * asked and confirmed on piazza
 * 
 */
#define NSIZES 9
#define GLOBAL_HEAP_ID 0
#define BLOCKTYPE_FREE 10
#define BLOCKTYPE_LARGE 11
#define FREE_PAGE_THRESHOLD 2
#define SUPERBLOCK_PAGE_SIZE (2 * 4096)
#define LARGEST_SUPERBLOCK_BLOCK_SIZE 2048

typedef ptrdiff_t vaddr_t;

////////////////////////////////////////////////////////
///////////// Data Structures Definitions //////////////
////////////////////////////////////////////////////////

/**
 * @brief Linked list to hold the free block in pages
 * 
 * next: pointer to a freelist struct
 */
struct freelist
{
	struct freelist *next;
};

/**
 * @brief struct to hold information regarding a page, the pageref struct
 * will be located at the beggining of the corresponding page (false sharing
 * is not possible)
 * 
 * next: pointer to the next page ref in the linked list
 * prev: pointer to the previous page ref in the linked list
 * flist: pointer to the next free block in the page 
 * count: indicates the number of free blocks in the page if the page ref is 
 * for non-large page, otherwise it refers to the number of pages used in the
 * large page
 * heap_ID: indicates the heap id of the heap that the page belongs to
 * Note that we keep track of prev pointers in complete_pages, large_pages 
 * and sizebases. Because in free_pages we only remove the head node.
 */
struct pageref
{
	struct pageref *next;
	struct pageref *prev;
	struct freelist *flist;
	int block_type;
	int count;
	int heap_ID;
};

/**
 * @brief struct that represents a heap
 * n_free_pages: integers refers to the number of free pages in the heap
 * free_pages: pageref pointer to the linked list of free pages in the heap
 * complete_pages: pageref pointer to the linked list of free pages in the
 * heap
 * 
 * large_pages: pointer to the linked list of large pages in the heap
 * sizebases[]: array of size NSIZE where the ith cell is a pointer to a linked
 * list of pages that have at least one free and one used block and these pages
 * have block size 2^(3+i)
 * spinlock_free_pages: spinlock for free_pages list
 * spinlock_complete_pages: spinlock for complete_pages list
 * spinlock_large_pages: spinlock for large_pages list
 * spinlock_sizebases[]: array of size NSIZE where ith cell contains a spinlock
 * for list in sizebases[i]
 * int pad[10]:an auxiliary array to reduce false sharing between processes
 * Note: this struct is padded to 192 bytes to fit in 3 cache lines as without the
 * padding the size of this struct is 152 bytes
 */
struct heap
{
	int n_free_pages;
	struct pageref *free_pages;
	struct pageref *complete_pages;
	struct pageref *large_pages;
	struct pageref *sizebases[NSIZES];
	pthread_spinlock_t spinlock_free_pages;
	pthread_spinlock_t spinlock_complete_pages;
	pthread_spinlock_t spinlock_large_pages;
	pthread_spinlock_t spinlock_sizebases[NSIZES];
	int pad[10];
};

////////////////////////////////////////////////////////
////////////////// Global Variables ////////////////////
////////////////////////////////////////////////////////

static int number_of_processors;				// number of processors in the system
static struct heap *heap_array;					// pointer to the array of heaps
static pthread_spinlock_t spinlock_global_sbrk; // spinlock used for sbrk function
// array of sizes represents the possible sizes of the blocks
static const size_t sizes[NSIZES] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};

////////////////////////////////////////////////////////
////////////////// Helper Functions ////////////////////
////////////////////////////////////////////////////////

/**
 * @brief helper function to figure out the block type of a given size
 * 
 */
static int get_block_type(size_t size)
{
	for (int i = 0; i < NSIZES; i++)
	{
		if (size <= sizes[i])
		{
			return i;
		}
	}
	// Allocator cannot handle allocation of size
	exit(1);
	return 0;
}

////////////////////////////////////////////////////////
/////////////// Page Relocation Functions //////////////
////////////////////////////////////////////////////////

/**
 * @brief function that checks if there are any free pages in the heap
 * pointed by h that can be moved to global heap, if so then it moves 
 * those pages to free pages list in the global heap
 * 
 */
static void move_page_global(struct heap *h)
{
	struct heap *global_heap = NULL;
	struct pageref *page = NULL;

	// Don't move to global heap if there is only one processor
	// since all the threads will be sharing the same heap
	if (number_of_processors > 1)
	{
		// Get pointer to global heap
		global_heap = (heap_array + GLOBAL_HEAP_ID);

		// Get the lock for the free_pages list in heap h
		pthread_spin_lock(&(h->spinlock_free_pages));
		if (h->n_free_pages > FREE_PAGE_THRESHOLD)
		{
			// There are more than FREE_PAGE_THRESHOLD free
			// pages remove one of these pages and release
			// the lock of the free_pages list
			page = h->free_pages;
			h->free_pages = h->free_pages->next;
			h->n_free_pages--;
			pthread_spin_unlock(&(h->spinlock_free_pages));

			// Add the removed page to the global list of free pages
			page->prev = NULL;
			page->heap_ID = GLOBAL_HEAP_ID;
			pthread_spin_lock(&(global_heap->spinlock_free_pages));
			page->next = global_heap->free_pages;
			global_heap->free_pages = page;
			global_heap->n_free_pages++;
			pthread_spin_unlock(&(global_heap->spinlock_free_pages));
		}
		else
		{
			// Not enough free pages in the local heap so don't do anything
			pthread_spin_unlock(&(h->spinlock_free_pages));
		}
	}
}

/**
 * @brief moves the page corresponding to page_ref to free list 
 * in heap h and moves pages to global heap if possible
 * 
 * @pre page_ref must not belong to another linked list (otherwise it will
 * belong to multiple lists at the same time) 
 */
static void move_page_free(struct pageref *page_ref, struct heap *h)
{
	page_ref->prev = NULL;
	page_ref->block_type = BLOCKTYPE_FREE;
	pthread_spin_lock(&(h->spinlock_free_pages));
	page_ref->next = h->free_pages;
	h->free_pages = page_ref;
	h->n_free_pages++;
	pthread_spin_unlock(&(h->spinlock_free_pages));
	move_page_global(h);
}

////////////////////////////////////////////////////////
//////////////////// Main functions ////////////////////
////////////////////////////////////////////////////////

/**
 * @brief function to allocate blocks of size at most 
 * LARGEST_SUPERBLOCK_BLOCK_SIZE in the heap with id heap
 * 
 * @return void* pointer to the block allocated
 */
static void *small_malloc(size_t size, int heap)
{
	/* 
	 * Check the sizebases of the current heap to see if there is an 
	 * available block there if not then look into the free_pages list
	 * of the current heap. If there are no free pages there then look 
	 * in the free_pages list of the global heap if that's empty as 
	 * well then allocate a new page.
	 */

	vaddr_t prpage;					   // page address right after pr
	vaddr_t free_list_addr;			   // free list entry address
	struct pageref *page_ref = NULL;   // pageref for page we're allocating from
	struct freelist *free_list = NULL; // free list entry
	struct heap *h = NULL;			   // pointer to the heap we're allocating from
	struct heap *global_heap = NULL;   // pointer to the global heap
	int block_type;					   // index for sizes[]
	void *result;					   // pointer to the allocated block

	h = (heap_array + heap);
	global_heap = (heap_array + GLOBAL_HEAP_ID);
	block_type = get_block_type(size);
	size = sizes[block_type];

	// check to see if there are any available blocks of
	// the correct size in the sizebases array
	pthread_spin_lock(&((h->spinlock_sizebases)[block_type]));
	page_ref = (h->sizebases)[block_type];
	if (page_ref != NULL)
	{
		// found a free block that can be used
		// in sizebases array
		result = page_ref->flist;
		page_ref->flist = page_ref->flist->next;
		page_ref->count--;
		if (page_ref->count == 0)
		{
			// Page has no more free blocks
			// remove the page from its current list
			if (page_ref->next != NULL)
			{
				page_ref->next->prev = NULL;
			}
			(h->sizebases)[block_type] = page_ref->next;
			page_ref->prev = NULL;
			page_ref->next = NULL;

			// Move page to complete_pages
			pthread_spin_lock(&(h->spinlock_complete_pages));
			if (h->complete_pages != NULL)
			{
				h->complete_pages->prev = page_ref;
			}
			page_ref->next = h->complete_pages;
			h->complete_pages = page_ref;
			pthread_spin_unlock(&(h->spinlock_complete_pages));
			pthread_spin_unlock(&((h->spinlock_sizebases)[block_type]));
		}
		else
		{
			pthread_spin_unlock(&((h->spinlock_sizebases)[block_type]));
		}
		return result;
	}
	// did not find any free block in the coresponding list in the sizebases
	pthread_spin_unlock(&((h->spinlock_sizebases)[block_type]));

	// could not find a block so far so check free pages
	pthread_spin_lock(&(h->spinlock_free_pages));
	if (h->free_pages)
	{
		page_ref = h->free_pages;
		h->free_pages = h->free_pages->next;
		h->n_free_pages--;
	}
	pthread_spin_unlock(&(h->spinlock_free_pages));

	if (page_ref == NULL)
	{
		// could not find a block so far so check global heap's free page list
		pthread_spin_lock(&(global_heap->spinlock_free_pages));
		if (global_heap->free_pages != NULL)
		{
			page_ref = global_heap->free_pages;
			global_heap->free_pages = global_heap->free_pages->next;
			global_heap->n_free_pages--;
		}
		pthread_spin_unlock(&(global_heap->spinlock_free_pages));
	}

	if (page_ref == NULL)
	{
		// no page of the right size available get a new one
		// the first couple of bytes will be used to store the
		// page ref of this new page
		pthread_spin_lock(&spinlock_global_sbrk);
		page_ref = (struct pageref *)mem_sbrk(SUPERBLOCK_PAGE_SIZE);
		pthread_spin_unlock(&spinlock_global_sbrk);
		if (page_ref == NULL)
		{
			// out of memory
			return NULL;
		}
	}
	// get the address of the page
	prpage = (vaddr_t)(page_ref + 1);

	// set page info
	page_ref->block_type = block_type;
	page_ref->count = (SUPERBLOCK_PAGE_SIZE - sizeof(struct pageref)) / sizes[block_type];
	page_ref->heap_ID = heap;
	page_ref->prev = NULL;

	// Build freelist of blocks in the page
	free_list_addr = prpage;
	free_list = (struct freelist *)free_list_addr;
	free_list->next = NULL;
	for (int i = 1; i < page_ref->count; i++)
	{
		free_list = (struct freelist *)(free_list_addr + i * sizes[block_type]);
		free_list->next = (struct freelist *)(free_list_addr + (i - 1) * sizes[block_type]);
	}
	page_ref->flist = free_list;

	// Remove a block from the page
	result = page_ref->flist;
	page_ref->flist = page_ref->flist->next;
	page_ref->count--;

	// add the page ref to the corresponding list in the sizebases array
	pthread_spin_lock(&((h->spinlock_sizebases)[block_type]));
	if ((h->sizebases)[block_type] != NULL)
	{
		(h->sizebases)[block_type]->prev = page_ref;
	}
	page_ref->next = (h->sizebases)[block_type];
	(h->sizebases)[block_type] = page_ref;
	pthread_spin_unlock(&((h->spinlock_sizebases)[block_type]));

	return result;
}

/**
 * @brief function to allocate blocks of size larger than
 * LARGEST_SUPERBLOCK_BLOCK_SIZE in the heap with id heap
 * 
 * @return void* pointer to the block allocated
 */
static void *large_malloc(int size, int heap)
{
	/* 
	 * Since allocations larger than LARGEST_SUPERBLOCK_BLOCK_SIZE will
	 * be rare we allocate contigous pages required for this size. We 
	 * simply round up to the nearest superpage-sized multiple after 
	 * adding some overhead space to hold the page ref.
	 * 
	 */
	vaddr_t result;					 // address of the new block of the given size
	struct pageref *page_ref = NULL; // pageref for page we're allocating from
	struct heap *h = NULL;			 // pointer to the heap that will be used for the allocation
	int npages;						 // number of pages needed

	h = (heap_array + heap);
	npages = (sizeof(struct pageref) + size + SUPERBLOCK_PAGE_SIZE - 1) / SUPERBLOCK_PAGE_SIZE;
	pthread_spin_lock(&spinlock_global_sbrk);
	page_ref = (struct pageref *)mem_sbrk(npages * SUPERBLOCK_PAGE_SIZE);
	pthread_spin_unlock(&spinlock_global_sbrk);

	if (page_ref == NULL)
	{
		// out of memory
		return NULL;
	}
	// get the address of the block
	result = (vaddr_t)(page_ref + 1);
	// set page info
	page_ref->block_type = BLOCKTYPE_LARGE;
	page_ref->count = npages; // count=npages in large blocks
	page_ref->prev = NULL;
	page_ref->heap_ID = heap;

	// Add the page to the large_pages list in the current heap
	pthread_spin_lock(&(h->spinlock_large_pages));
	if (h->large_pages != NULL)
	{
		h->large_pages->prev = page_ref;
	}
	page_ref->next = h->large_pages;
	h->large_pages = page_ref;
	pthread_spin_unlock(&(h->spinlock_large_pages));

	return ((void *)result);
}

/**
 * @brief function to free blocks of size larger than
 * LARGEST_SUPERBLOCK_BLOCK_SIZE.
 * 
 * @param ptr pointer to the block to be freed
 * @param heap_pt pointer to the heap of the page that the block
 * belongs to
 * @param page_ref pointer to the page ref of the page that the block 
 * belongs to
 */
static int large_free(void *ptr, struct heap *heap_pt, struct pageref *page_ref)
{
	vaddr_t prpage; // address of the page_ref 

	// Remove the page from list of large pages in the corresponding heap
	pthread_spin_lock(&(heap_pt->spinlock_large_pages));
	if (page_ref->next != NULL)
	{
		page_ref->next->prev = page_ref->prev;
	}
	if (page_ref->prev == NULL)
	{
		heap_pt->large_pages = page_ref->next;
	}
	else
	{
		page_ref->prev->next = page_ref->next;
	}
	pthread_spin_unlock(&(heap_pt->spinlock_large_pages));

	page_ref->block_type = BLOCKTYPE_FREE;
	// divide the large block into SUPERBLOCK_PAGE_SIZE pages
	struct pageref *new_header = page_ref;
	struct pageref *new_tail = page_ref;
	prpage = (vaddr_t)(page_ref);
	for (int i = 1; i < page_ref->count; i++)
	{
		struct pageref *newpr = (struct pageref *)prpage;
		newpr->block_type = BLOCKTYPE_FREE;
		newpr->prev = NULL;
		newpr->heap_ID = page_ref->heap_ID;
		new_tail->next = newpr;
		new_tail = newpr;
		prpage += SUPERBLOCK_PAGE_SIZE;
	}
	new_header->prev = NULL;

	// add the chopped up pages to the free list to be used later
	pthread_spin_lock(&(heap_pt->spinlock_free_pages));
	if (heap_pt->free_pages != NULL)
	{
		heap_pt->free_pages->prev = new_tail;
	}
	new_tail->next = heap_pt->free_pages;
	heap_pt->free_pages = new_header;
	heap_pt->n_free_pages += page_ref->count;
	pthread_spin_unlock(&(heap_pt->spinlock_free_pages));
	// move to global free pages list if possible
	move_page_global(heap_pt);
	return 0;
}

/**
 * @brief function to free blocks of size at most 
 * LARGEST_SUPERBLOCK_BLOCK_SIZE. If the ptr points to a
 * block with larger size than LARGEST_SUPERBLOCK_BLOCK_SIZE
 * it call large_free function to handle it.
 * 
 * @param ptr pointer to the block to be freed
 */
static int small_free(void *ptr)
{
	vaddr_t ptraddr;				 // same as ptr
	struct pageref *page_ref = NULL; // pageref for page of the block we're freeing
	struct heap *heap_pt = NULL;	 // pointer to heap of the page of the block we're freeing
	int block_type;					 // index into sizes[]

	ptraddr = (vaddr_t)ptr;
	// gigure out the page ref for the page of the block 
	page_ref = (struct pageref *)(ptraddr - (ptraddr % SUPERBLOCK_PAGE_SIZE));
	block_type = page_ref->block_type;

	if (block_type == BLOCKTYPE_FREE)
	{
		// trying to free a block that has already been freed.
		return 0;
	}

	heap_pt = (heap_array + (page_ref->heap_ID));
	if (block_type == BLOCKTYPE_LARGE)
	{
		return large_free(ptr, heap_pt, page_ref);
	}

	// take both locks since the page can be in either block
	// we might also have to move from sizebases to complete_pages
	pthread_spin_lock((heap_pt->spinlock_sizebases) + block_type);
	pthread_spin_lock(&(heap_pt->spinlock_complete_pages));

	// add the block to the free list of that page
	((struct freelist *)ptr)->next = page_ref->flist;
	page_ref->flist = (struct freelist *)ptr;
	page_ref->count++;

	if (page_ref->count == (SUPERBLOCK_PAGE_SIZE - sizeof(struct pageref)) / sizes[block_type])
	{
		// all the blocks in the page are empty
		// unlock complete_pages lock since we know that the page can't belong
		// to complete pages as there must have been other empty blocks besides
		// the block that we just freed
		pthread_spin_unlock(&(heap_pt->spinlock_complete_pages));

		// remove the page from the list it belongs to
		if (page_ref->next != NULL)
		{
			page_ref->next->prev = page_ref->prev;
		}
		if (page_ref->prev != NULL)
		{
			page_ref->prev->next = page_ref->next;
		}
		else
		{
			(heap_pt->sizebases)[block_type] = page_ref->next;
		}
		page_ref->block_type = BLOCKTYPE_FREE;
		pthread_spin_unlock((heap_pt->spinlock_sizebases) + block_type);
		// move the page to free pages list
		move_page_free(page_ref, heap_pt);
	}
	else if (page_ref->count == 1)
	{
		// there is only one free block in the page (which is the block
		// we just freed). The page is no longer a complete page so we
		// remove it from complete pages list
		if (page_ref->next != NULL)
		{
			page_ref->next->prev = page_ref->prev;
		}
		if (page_ref->prev != NULL)
		{
			page_ref->prev->next = page_ref->next;
		}
		else
		{
			heap_pt->complete_pages = page_ref->next;
		}
		pthread_spin_unlock(&(heap_pt->spinlock_complete_pages));

		// add the page to the corresponding list in the sizebases array
		// of the corresponding heap
		page_ref->prev = NULL;
		if ((heap_pt->sizebases)[block_type] != NULL)
		{
			(heap_pt->sizebases)[block_type]->prev = page_ref;
		}
		page_ref->next = (heap_pt->sizebases)[block_type];
		(heap_pt->sizebases)[block_type] = page_ref;
		pthread_spin_unlock((heap_pt->spinlock_sizebases) + block_type);
	}
	else
	{
		pthread_spin_unlock(&(heap_pt->spinlock_complete_pages));
		pthread_spin_unlock((heap_pt->spinlock_sizebases) + block_type);
	}
	return 0;
}

/**
 * @brief function returns a pointer to an allocated region of at least size bytes. 
 * The pointer is aligned to 8 bytes, and the entire allocated region should lies
 * within the memory region from dseg_lo to dseg_hi.
 * 
 * @param size size of the block to be allocated
 * @return void* pointer to the block allocated
 */

void *mm_malloc(size_t size)
{
	int heap_id = (sched_getcpu() % number_of_processors) + 1;
	if (size > LARGEST_SUPERBLOCK_BLOCK_SIZE)
	{
		return large_malloc(size, heap_id);
	}
	return small_malloc(size, heap_id);
}

/**
 * @brief function to free a block pointed by ptr and is only guaranteed to work when 
 * it is passed pointers to allocated blocks that were returned by previous calls to 
 * mm_malloc. It adds the freed block to the pool of unallocated blocks, making the
 * memory available to future mm_malloc calls.
 * 
 * @param ptr pointer to the block that should be freed
 */
void mm_free(void *ptr)
{
	if (ptr == NULL)
	{
		return;
	}
	small_free(ptr);
}

/**
 * @brief Any appilcation that uses the functions mm_malloc or mm_free, must call 
 * this function to perform the necessary initializations before using those functions.
 * 
 * @return int -1 if there was a problem with the initialization, 0 otherwise.
 */
int mm_init(void)
{
	int npages;
	int heap_size = sizeof(struct heap);

	if (mem_init() != 0)
	{
		// Failed to initialize memory
		return -1;
	}

	// align the pages with so that it is possible to get the
	// address of page refs of given address of a block
	int diff = abs(((long)dseg_lo) % SUPERBLOCK_PAGE_SIZE);
	if (diff > 0)
	{
		mem_sbrk(diff);
	}

	pthread_spin_init(&spinlock_global_sbrk, 0);
	number_of_processors = getNumProcessors();
	npages = ((heap_size * (number_of_processors + 1)) + SUPERBLOCK_PAGE_SIZE - 1) / SUPERBLOCK_PAGE_SIZE;
	heap_array = (struct heap *)mem_sbrk(npages * SUPERBLOCK_PAGE_SIZE);

	if (heap_array == NULL)
	{
		// Allocator couldn't get enough memory for initialization
		return -1;
	}

	for (int i = 0; i <= number_of_processors; i++)
	{
		struct heap *h = (heap_array + i);
		h->n_free_pages = 0;
		;
		h->free_pages = NULL;
		h->complete_pages = NULL;
		h->large_pages = NULL;

		pthread_spin_init(&(h->spinlock_free_pages), 0);
		pthread_spin_init(&(h->spinlock_complete_pages), 0);
		for (int j = 0; j < NSIZES; j++)
		{
			pthread_spin_init(&((h->spinlock_sizebases)[j]), 0);
			h->sizebases[j] = NULL;
		}
		pthread_spin_init(&(h->spinlock_large_pages), 0);
	}

	return 0;
}
