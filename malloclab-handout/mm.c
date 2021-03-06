/*
 * mm-naive.c - The fastest, least memory-efficient malloc package.
 *
 * In this naive approach, a block is allocated by simply incrementing
 * the brk pointer.  Blocks are never coalesced or reused.  The size of
 * a block is found at the first aligned word before the block (we need
 * it for realloc).
 *
 * This code is correct and blazingly fast, but very bad usage-wise since
 * it never frees anything.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mm.h"
#include "memlib.h"

/* If you want debugging output, use the following macro.  When you hand
 * in, remove the #define DEBUG line. */
#define DEBUG
#ifdef DEBUG
# define dbg_printf(...) printf(__VA_ARGS__)
#else
# define dbg_printf(...)
#endif


/* do not change the following! */
#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#endif /* def DRIVER */

#define WSIZE 8 /*Word and header/footer size(bytes) in 64 bit machine*/
#define DSIZE 16  
#define CHUNKSIZE (1<<6) /*one extension of heap*/
#define MAX(x, y) ((x) > (y)? (x) : (y))

/* Pack a size and allocated bit into a word */
#define PACK(size, alloc) ((size) | (alloc))

/* Read and write word at address p*/
#define GET(p) (*(unsigned long *)(p))
#define PUT(p, val) (*(unsigned long *)(p) = (val))

/*Read the size and allocated fields from address p*/
#define GET_SIZE(p) (GET(p) & ~0xf)
#define GET_ALLOC(p) (GET(p) & 0x1)

/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Given block ptr bp, compute address of next and previous blocks */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE))) /*check the previous footer*/

/* Given block ptr bp, compute address of pred and succ*/
#define PRED_PT(bp) ((char *)(bp))
#define SUCC_PT(bp) ((char *)(bp) + WSIZE)
#define GET_PRED(bp) (*((unsigned long *)(bp)))
#define GET_SUCC(bp) (*((unsigned long *)(bp) + 1))
#define SET_PRED(bp, val) ((*((unsigned long *)(bp))) = (unsigned long)(val))
#define SET_SUCC(bp, val) ((*((unsigned long *)(bp) + 1)) = (unsigned long)(val))

/* Operations on the segregated list*/
#define GET_HEAD(index) (*(unsigned long *)(segregatedList + (index*WSIZE))) 
#define CLEAR(index) SET_HEAD(index, 0)
#define SET_HEAD(index, val) (*(unsigned long *)(segregatedList + (index*WSIZE)) = (unsigned long)(val))
#define SET_HEAD_BIT(bp) PUT(HDRP(bp),PACK(GET(HDRP(bp)), 0x2))
#define GET_HEAD_BIT(bp) ((GET(HDRP(bp))>>1) & 0x1)
#define CLEAR_HEAD_BIT(bp) PUT(HDRP(bp), GET(HDRP(bp)) & ~0x2)
/*global variable*/
static void *extend_heap(size_t size);
static void *coalesce(void *bp); 
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);
static int find_index(size_t asize);
static void insertNode(void *bp);
static void removeNode(void *bp);
static void *get_list_pred(char *head, char *bp);

/* segregated list with size 14 denoting: 2^0 ~ above2^12
 * note that minimum free block size:4 words = 32bytes, thus the size class for first slots is (2^4+1, 2^5)
 * size of each cell: one word which contains the address of the first node of the free list
 * each size class in the free list denotes the size of the whole block(payload + header + footer)
 */
static char *segregatedList;
static char *heap_lstp;

/*
 * mm_init - Called when a new trace starts.
 */
int mm_init(void)
{
  // alloc an empty segregated list
  if ((segregatedList = (char *)mem_sbrk(12*WSIZE)) == (char *)-1)
    return -1; 
  for (int i = 0;i < 9;i++) {
    CLEAR(i);
  }
  // set header and footer
  PUT(segregatedList + (9*WSIZE), PACK(2*WSIZE, 1));
  PUT(segregatedList + (10*WSIZE), PACK(2*WSIZE, 1));
  PUT(segregatedList + (11*WSIZE), PACK(0, 1));
  heap_lstp = segregatedList + (10*WSIZE);
  return 0;
}

/*
 * malloc - Allocate a block by incrementing the brk pointer.
 *      Always allocate a block whose size is a multiple of the alignment.
 * note that the allocated unit is word
 * need consider splitting policy
 */
void *malloc(size_t size)
{
  size_t asize; /* add the header and footer to size and align to DSIZE*/
  size_t extendsize;
  char *bp;
  if (size <= DSIZE) {//16
    asize = 2*DSIZE;
  } else {
    //ceil((size + WSIZE * 2)/DSIZE) * DSIZE
    // payload + padding should be mutiple of DSIZE
    asize = ((size + DSIZE + DSIZE - 1)/DSIZE) * DSIZE;
  }
  if ((bp = find_fit(asize)) != NULL) {
    place(bp, asize);
    return bp;
  }
  extendsize = MAX(asize, CHUNKSIZE);
  if ((bp = extend_heap(extendsize/WSIZE)) == NULL) {
    return NULL;
  }
  place(bp, asize);
  return bp;
}

/*
 * search the free list from head to insertNode the free block and then coalesce
 */
void free(void *ptr){
  if (ptr == NULL) {
    return;
  }
  size_t size = GET_SIZE(HDRP(ptr));
  PUT(HDRP(ptr), PACK(size, 0));
  PUT(FTRP(ptr), PACK(size, 0));
  insertNode(ptr);  
  coalesce(ptr);
}

/*
 * realloc - Change the size of the block by mallocing a new block,
 *      copying its data, and freeing the old block. 
 * optimization: if the size is smaller, we use the current one and possibly split it
 */
void *realloc(void *oldptr, size_t size)
{
  unsigned long oldsize; 
  unsigned long asize;//alignment
  void *newptr;

  /* If size == 0 then this is just free, and we return NULL. */
  if(size == 0) {
    free(oldptr);
    return 0;
  }

  /* If oldptr is NULL, then this is just malloc. */
  if(oldptr == NULL) {
    return malloc(size);
  }
  oldsize = GET_SIZE(HDRP(oldptr));
  if (size <= DSIZE) {//16
    asize = 2*DSIZE;
  } else {
    asize = ((size + DSIZE + DSIZE - 1)/DSIZE) * DSIZE;
  }
  if (asize == oldsize) {
    return oldptr;
  }
  if (asize < oldsize) {//check if split or not
    if ((oldsize - asize) >= (DSIZE*2)) {
      PUT(HDRP(oldptr), PACK(asize, 1));
      PUT(FTRP(oldptr), PACK(asize, 1));
      PUT(HDRP(NEXT_BLKP(oldptr)), PACK(oldsize - asize, 0));
      PUT(FTRP(NEXT_BLKP(oldptr)), PACK(oldsize - asize, 0));
      insertNode(NEXT_BLKP(oldptr));
    } 
    return oldptr;
  }
  newptr = malloc(size);
  /* If realloc() fails the original block is left untouched  */
  if(!newptr) {
    return 0;
  }
  /* Copy the old data. */
  // bug: old size is the blocksize, we should only copy the payload
  memcpy(newptr, oldptr, oldsize - WSIZE*2);
  free(oldptr);// free will set succ and pred so must after the memcpy
  return newptr;
}

/*
 * calloc - Allocate the block and set it to zero.
 */
void *calloc (size_t nmemb, size_t size)
{
  size_t bytes = nmemb * size;
  void *newptr;

  newptr = malloc(bytes);
  memset(newptr, 0, bytes);

  return newptr;
}

/*
 * mm_checkheap - There are no bugs in my code, so I don't need to check,
 *      so nah!
 */
void mm_checkheap(int verbose){
  char *lo = mem_heap_lo();
  char *hi = mem_heap_hi();
	//block level
  if (verbose) {
    printf("%s\n", "Check Heap:");
    printf("Heap: low:%lx high:%lx size:%ld bytes\n", (unsigned long)lo, (unsigned long)hi, (unsigned long)hi - (unsigned long)lo + 1);
  }
  if (GET_SIZE(HDRP(heap_lstp)) != 2*WSIZE || !GET_ALLOC(HDRP(heap_lstp))){
    printf("%s\n", "Invalid Prologue");
  }
  //iterate the block list
  unsigned long count = 0;
  char *curr;
  for (curr = heap_lstp;GET_SIZE(HDRP(curr)) > 0;curr = NEXT_BLKP(curr)){
    if (((unsigned long)curr) % 16 != 0) {
      printf("%s\n", "wrong alignment");
    }
    if ((GET(HDRP(curr)) & ~0xf) != (GET(FTRP(curr)) & ~0xf)) {
      printf("%s\n", "Inconsistency in header and footer size");
    }
    if ((GET(HDRP(curr)) & 0x1) != (GET(FTRP(curr)) & 0x1)) {
      printf("%s\n", "Inconsistency in header and footer alloc bit");
    }
    count++;
  }
  if (GET_SIZE(HDRP(curr)) != 0 || !GET_ALLOC(HDRP(curr))) {
    printf("%s\n", "Invalid Epilogue");
    printf("%s%lx\n", "Epilogue Size:", GET_SIZE(curr));
    printf("%s%lx\n", "Epilogue Alloc bit:", GET_ALLOC(curr));
  }
  count++;
  if (verbose) {
    printf("%s%ld\n", "number of blocks:", count);
    printf("%s%lx\n", "Epilogue:", (unsigned long)(curr - WSIZE));
  }
  // check free list
  for (int i = 0;i < 9;i++) {
    unsigned long head = GET_HEAD(i);
    if (head != 0) {
      char *bp;
      for (bp = (char *)head;(unsigned long)bp > 0; bp = (char *)GET_SUCC(bp))
      {
        if (GET_ALLOC(HDRP(bp)))
        {
            printf("%s\n", "free list has allocated blocks");
        }
      }
    }
  }

}

/*input is the number of words*/
static void *extend_heap(size_t size) 
{
  char *bp;
  size_t ext;
  // convert the size to byte
  // round up the size to meet the double words alignment requirement
  ext = (size % 2)? (WSIZE * (size + 1)): (WSIZE * size);
  if ((long)(bp = mem_sbrk(ext)) ==  -1) {
    return NULL;
  }
  // size_t pred = GET_PRED(bp);
  // set the header and footer for the new block
  PUT(HDRP(bp), PACK(ext, 0));
  PUT(FTRP(bp), PACK(ext, 0));
  PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));
  insertNode(bp);
  return coalesce(bp);
}

// optimization: size order in each lst
static void insertNode(void *bp) {
  int index = find_index(GET_SIZE(HDRP(bp)));
  char *head = (char *)GET_HEAD(index);
  if ((long)head != 0) {//find the correct location
    char *pred = get_list_pred(head, bp);
    // smallest
    if (pred == (char *)0) {
      SET_SUCC(bp, head);
      SET_PRED(head, bp);
      SET_PRED(bp, segregatedList + (index*WSIZE));//head
      SET_HEAD(index, bp);
      SET_HEAD_BIT(bp);
      CLEAR_HEAD_BIT(head);
    } else {
      SET_SUCC(bp, GET_SUCC(pred));
      if (GET_SUCC(pred) != 0) {
        SET_PRED(GET_SUCC(pred), bp);
      }
      SET_SUCC(pred, bp);
      SET_PRED(bp, pred);
    }
  } else {//set head
    SET_SUCC(bp, 0);
    SET_HEAD(index, bp);
    SET_PRED(bp, segregatedList + (index*WSIZE));//head
    SET_HEAD_BIT(bp);
  } 
}

static void *get_list_pred(char *head, char *bp) {
  size_t size = GET_SIZE(HDRP(bp));
  if (GET_SIZE(HDRP(head)) >= size) {
    return (void *)0;
  }
  for (;GET_SUCC(head) != 0;head = (char *)GET_SUCC(head)){
    if (GET_SIZE(HDRP(GET_SUCC(head))) >= size) {
      return head;
    }
  }
  return head;
}

// this function checks the prev and next block to coalesce if possible
static void *coalesce(void *bp)
{
  //get the free bit for prev and next
  size_t next = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
  size_t prev = GET_ALLOC(HDRP(PREV_BLKP(bp)));
  size_t cs = GET_SIZE(HDRP(bp));
  if (next && prev) {
    return bp;
  } else if (next && !prev) {
    size_t ps = GET_SIZE(HDRP(PREV_BLKP(bp)));
    // remove prev and curr from free list
    removeNode(PREV_BLKP(bp));
    removeNode(bp);
    // merge
    PUT(HDRP(PREV_BLKP(bp)), PACK((cs + ps), 0));
    PUT(FTRP(bp), PACK(cs + ps, 0));
    bp = PREV_BLKP(bp);
    insertNode(bp);
  } else if (!next && prev) {
    // modify link
    removeNode(NEXT_BLKP(bp));
    removeNode(bp);
    size_t ns = GET_SIZE(HDRP(NEXT_BLKP(bp)));
    PUT(FTRP(NEXT_BLKP(bp)), PACK((cs + ns), 0));
    PUT(HDRP(bp), PACK(cs + ns, 0)); 
    insertNode(bp);
  } else {
    removeNode(PREV_BLKP(bp));
    removeNode(NEXT_BLKP(bp));
    removeNode(bp);
    size_t ps = GET_SIZE(HDRP(PREV_BLKP(bp)));
    size_t ns = GET_SIZE(HDRP(NEXT_BLKP(bp)));
    size_t s = ps + ns + cs;
    PUT(HDRP(PREV_BLKP(bp)), PACK(s, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(s, 0));
    bp = PREV_BLKP(bp);
    insertNode(bp);
  }
  return bp;
}

static void removeNode(void *bp){
  if (GET_HEAD_BIT(bp)) {
    PUT(GET_PRED(bp), GET_SUCC(bp));
    CLEAR_HEAD_BIT(bp);
    if (GET_SUCC(bp) != 0) {
      SET_HEAD_BIT(GET_SUCC(bp));
      SET_PRED(GET_SUCC(bp), GET_PRED(bp));
    }
  } else if (GET_SUCC(bp) == 0) {//tail
    SET_SUCC(GET_PRED(bp), 0);
  } else {
    SET_SUCC(GET_PRED(bp), GET_SUCC(bp));
    SET_PRED(GET_SUCC(bp), GET_PRED(bp));
  }
}

// first fit in the free list, asize bytes
// change to best fit 
static void *find_fit(size_t asize) {
  // get the index of the slot
  // smallest power of 2 that is larger than asize
  int index = find_index(asize);
  for (int i = index;i < 9;i++) {
    unsigned long head = GET_HEAD(i);
    if (head != 0) {
      char *bp;
      for (bp = (char *)head;(unsigned long)bp > 0; bp = (char *)GET_SUCC(bp))
      {
        if (GET_SIZE(HDRP(bp)) >= asize)
        {
           return bp;
        }
      }
    }
  }
  return NULL;
}

static int find_index(size_t asize) {
  unsigned long bound = 5;
  for (int index = 0;index < 8;index++){
    if (((unsigned long)1<<(index + bound)) >= asize){
      return index;
    }
  }
  return 8;
}

static void place(void *bp, size_t asize){
  //payload + padding should be mutiple of DSIZE
  size_t csize = GET_SIZE(HDRP(bp));
  removeNode(bp);
  if ((csize - asize) >= (DSIZE * 2)) {
    PUT(HDRP(bp), PACK(asize, 1));
    PUT(FTRP(bp), PACK(asize, 1));
    PUT(HDRP(NEXT_BLKP(bp)), PACK(csize - asize, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(csize - asize, 0));
    insertNode(NEXT_BLKP(bp));
  } else { //no splitting
    PUT(HDRP(bp), PACK(csize, 1));
    PUT(FTRP(bp), PACK(csize, 1));
  }
}
