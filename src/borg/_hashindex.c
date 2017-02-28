#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#if defined (__SVR4) && defined (__sun)
#include <sys/isa_defs.h>
#endif

#if (defined(BYTE_ORDER) && (BYTE_ORDER == BIG_ENDIAN)) ||  \
    (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || \
    (defined(_BIG_ENDIAN) && defined(__SVR4)&&defined(__sun))
#define BORG_BIG_ENDIAN 1
#elif (defined(BYTE_ORDER) && (BYTE_ORDER == LITTLE_ENDIAN)) || \
      (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)) || \
      (defined(_LITTLE_ENDIAN) && defined(__SVR4)&&defined(__sun))
#define BORG_BIG_ENDIAN 0
#else
#error Unknown byte order
#endif

#if BORG_BIG_ENDIAN
#define _le32toh(x) __builtin_bswap32(x)
#define _htole32(x) __builtin_bswap32(x)
#else
#define _le32toh(x) (x)
#define _htole32(x) (x)
#endif

#define MAGIC "BORG_IDX"
#define MAGIC_LEN 8

#define DEBUG 0

#define debug_print(fmt, ...)                   \
  do {                                          \
    if (DEBUG) {                                \
      fprintf(stderr, fmt, __VA_ARGS__);        \
      fflush(NULL);                             \
    }                                           \
  } while (0)


typedef struct {
    char magic[MAGIC_LEN];
    int32_t num_entries;
    int32_t num_buckets;
    int8_t  key_size;
    int8_t  value_size;
} __attribute__((__packed__)) HashHeader;

typedef struct {
    void *buckets;
    int num_entries;
    int num_buckets;
    int key_size;
    int value_size;
    off_t bucket_size;
    int lower_limit;
    int upper_limit;
    void *tmp_entry;
} HashIndex;

/* prime (or w/ big prime factors) hash table sizes
 * not sure we need primes for borg's usage (as we have a hash function based
 * on sha256, we can assume an even, seemingly random distribution of values),
 * but OTOH primes don't harm.
 * also, growth of the sizes starts with fast-growing 2x steps, but slows down
 * more and more down to 1.1x. this is to avoid huge jumps in memory allocation,
 * like e.g. 4G -> 8G.
 * these values are generated by hash_sizes.py.
 */
static int hash_sizes[] = {
    1031, 2053, 4099, 8209, 16411, 32771, 65537, 131101, 262147, 445649,
    757607, 1287917, 2189459, 3065243, 4291319, 6007867, 8410991,
    11775359, 16485527, 23079703, 27695653, 33234787, 39881729, 47858071,
    57429683, 68915617, 82698751, 99238507, 119086189, 144378011, 157223263,
    173476439, 190253911, 209915011, 230493629, 253169431, 278728861,
    306647623, 337318939, 370742809, 408229973, 449387209, 493428073,
    543105119, 596976533, 657794869, 722676499, 795815791, 874066969,
    962279771, 1057701643, 1164002657, 1280003147, 1407800297, 1548442699,
    1703765389, 1873768367, 2062383853, /* 32bit int ends about here */
};

#define HASH_MIN_LOAD .25
#define HASH_MAX_LOAD .99  /* use testsuite.benchmark.test_chunk_indexer_* to find
                              an appropriate value; also don't forget to update this
                              value in archive.py */

#define MAX(x, y) ((x) > (y) ? (x): (y))
#define NELEMS(x) (sizeof(x) / sizeof((x)[0]))

#define EMPTY _htole32(0xffffffff)
#define DELETED _htole32(0xfffffffe)

#define BUCKET_ADDR(index, idx) (index->buckets + ((idx) * index->bucket_size))

#define BUCKET_MATCHES_KEY(index, idx, key) (memcmp(key, BUCKET_ADDR(index, idx), index->key_size) == 0)

#define BUCKET_IS_DELETED(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) == DELETED)
#define BUCKET_IS_EMPTY(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) == EMPTY)

#define BUCKET_MARK_DELETED(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, idx) + index->key_size)) = DELETED)
#define BUCKET_MARK_EMPTY(index, idx) (*((uint32_t *)(BUCKET_ADDR(index, (idx)) + index->key_size)) = EMPTY)

#define EPRINTF_MSG(msg, ...) fprintf(stderr, "hashindex: " msg "\n", ##__VA_ARGS__)
#define EPRINTF_MSG_PATH(path, msg, ...) fprintf(stderr, "hashindex: %s: " msg "\n", path, ##__VA_ARGS__)
#define EPRINTF(msg, ...) fprintf(stderr, "hashindex: " msg "(%s)\n", ##__VA_ARGS__, strerror(errno))
#define EPRINTF_PATH(path, msg, ...) fprintf(stderr, "hashindex: %s: " msg " (%s)\n", path, ##__VA_ARGS__, strerror(errno))

static HashIndex *hashindex_read(const char *path);
static int hashindex_write(HashIndex *index, const char *path);
static HashIndex *hashindex_init(int capacity, int key_size, int value_size);
static const void *hashindex_get(HashIndex *index, const void *key);
static int hashindex_set(HashIndex *index, const void *key, const void *value);
static int hashindex_delete(HashIndex *index, const void *key);
static void *hashindex_next_key(HashIndex *index, const void *key);

/* Private API */
static void hashindex_free(HashIndex *index);

static int
hashindex_index(HashIndex *index, const void *key)
{
    return _le32toh(*((uint32_t *)key)) % index->num_buckets;
}

inline int
distance(int current_idx, int ideal_idx, int num_buckets)
{
    /* If the current index is smaller than the ideal index we've wrapped
       around the end of the bucket array and need to compensate for that. */
    return current_idx - ideal_idx + ( (current_idx < ideal_idx) ? num_buckets : 0 );
}

static int
hashindex_lookup(HashIndex *index, const void *key, int *skip_hint)
{
    int start = hashindex_index(index, key);
    int idx = start;
    int offset;
    int rv = -1;
    int period = 0;
    for(offset=0; ;offset++) {
        if(BUCKET_IS_EMPTY(index, idx)) {
            rv = -1;
            /* debug_print("\n hashindex_lookup:empty %d\n", offset); */
            break;
        }
        if(BUCKET_MATCHES_KEY(index, idx, key)) {
            return idx;
        }
        if(period++ == 63){
	    period = 0;
	    if (offset > distance(idx, hashindex_index(index, BUCKET_ADDR(index, idx)), index->num_buckets)) {
		rv = -1;
		break;
	    }
	}

        idx ++;
        if (idx >= index->num_buckets) {
            idx = 0;
        }
        if(idx == start) {
            rv = -1;
            break;
        }
    }
    if (skip_hint != NULL) {
        /* compensate for the period, hashindex_set will need to re-examine the last
           16 buckets for a suitable bucket to insert it's value */
        offset = offset - 64;
        if (offset < 0) {
            offset = 0;
        }
        (*skip_hint) = offset;
    }
    return rv;
}

static int
hashindex_resize(HashIndex *index, int capacity)
{
    HashIndex *new;
    void *key = NULL;
    int32_t key_size = index->key_size;
    debug_print("\nresize to %d!\n", capacity);

    if(!(new = hashindex_init(capacity, key_size, index->value_size))) {
        return 0;
    }
    while((key = hashindex_next_key(index, key))) {
        if(!hashindex_set(new, key, key + key_size)) {
            /* This can only happen if there's a bug in the code calculating capacity */
            hashindex_free(new);
            return 0;
        }
    }
    free(index->buckets);
    index->buckets = new->buckets;
    index->num_buckets = new->num_buckets;
    index->lower_limit = new->lower_limit;
    index->upper_limit = new->upper_limit;
    free(new->tmp_entry);
    free(new);
    return 1;
}

int get_lower_limit(int num_buckets){
    int min_buckets = hash_sizes[0];
    if (num_buckets <= min_buckets)
        return 0;
    return (int)(num_buckets * HASH_MIN_LOAD);
}

int get_upper_limit(int num_buckets){
    int max_buckets = hash_sizes[NELEMS(hash_sizes) - 1];
    if (num_buckets >= max_buckets)
        return num_buckets;
    return (int)(num_buckets * HASH_MAX_LOAD);
}

int size_idx(int size){
    /* find the hash_sizes index with entry >= size */
    int elems = NELEMS(hash_sizes);
    int entry, i=0;
    do{
        entry = hash_sizes[i++];
    }while((entry < size) && (i < elems));
    if (i >= elems)
        return elems - 1;
    i--;
    return i;
}

int fit_size(int current){
    int i = size_idx(current);
    return hash_sizes[i];
}

int grow_size(int current){
    int i = size_idx(current) + 1;
    int elems = NELEMS(hash_sizes);
    if (i >= elems)
        return hash_sizes[elems - 1];
    return hash_sizes[i];
}

int shrink_size(int current){
    int i = size_idx(current) - 1;
    if (i < 0)
        return hash_sizes[0];
    return hash_sizes[i];
}

/* Public API */
static HashIndex *
hashindex_read(const char *path)
{
    FILE *fd;
    off_t length, buckets_length, bytes_read;
    HashHeader header;
    HashIndex *index = NULL;

    if((fd = fopen(path, "rb")) == NULL) {
        EPRINTF_PATH(path, "fopen for reading failed");
        return NULL;
    }
    bytes_read = fread(&header, 1, sizeof(HashHeader), fd);
    if(bytes_read != sizeof(HashHeader)) {
        if(ferror(fd)) {
            EPRINTF_PATH(path, "fread header failed (expected %ju, got %ju)",
                         (uintmax_t) sizeof(HashHeader), (uintmax_t) bytes_read);
        }
        else {
            EPRINTF_MSG_PATH(path, "fread header failed (expected %ju, got %ju)",
                             (uintmax_t) sizeof(HashHeader), (uintmax_t) bytes_read);
        }
        goto fail;
    }
    if(fseek(fd, 0, SEEK_END) < 0) {
        EPRINTF_PATH(path, "fseek failed");
        goto fail;
    }
    if((length = ftell(fd)) < 0) {
        EPRINTF_PATH(path, "ftell failed");
        goto fail;
    }
    if(fseek(fd, sizeof(HashHeader), SEEK_SET) < 0) {
        EPRINTF_PATH(path, "fseek failed");
        goto fail;
    }
    if(memcmp(header.magic, MAGIC, MAGIC_LEN)) {
        EPRINTF_MSG_PATH(path, "Unknown MAGIC in header");
        goto fail;
    }
    buckets_length = (off_t)_le32toh(header.num_buckets) * (header.key_size + header.value_size);
    if((size_t) length != sizeof(HashHeader) + buckets_length) {
        EPRINTF_MSG_PATH(path, "Incorrect file length (expected %ju, got %ju)",
                         (uintmax_t) sizeof(HashHeader) + buckets_length, (uintmax_t) length);
        goto fail;
    }
    if(!(index = malloc(sizeof(HashIndex)))) {
        EPRINTF_PATH(path, "malloc header failed");
        goto fail;
    }
    if(!(index->buckets = malloc(buckets_length))) {
        EPRINTF_PATH(path, "malloc buckets failed");
        free(index);
        index = NULL;
        goto fail;
    }
    if(!(index->tmp_entry = calloc(1, header.key_size + header.value_size))) {
        EPRINTF_PATH(path, "malloc temp entry failed");
        free(index->buckets);
        free(index);
        index = NULL;
        goto fail;
    }
    bytes_read = fread(index->buckets, 1, buckets_length, fd);
    if(bytes_read != buckets_length) {
        if(ferror(fd)) {
            EPRINTF_PATH(path, "fread buckets failed (expected %ju, got %ju)",
                         (uintmax_t) buckets_length, (uintmax_t) bytes_read);
        }
        else {
            EPRINTF_MSG_PATH(path, "fread buckets failed (expected %ju, got %ju)",
                             (uintmax_t) buckets_length, (uintmax_t) bytes_read);
        }
        free(index->tmp_entry);
        free(index->buckets);
        free(index);
        index = NULL;
        goto fail;
    }
    index->num_entries = _le32toh(header.num_entries);
    index->num_buckets = _le32toh(header.num_buckets);
    index->key_size = header.key_size;
    index->value_size = header.value_size;
    index->bucket_size = index->key_size + index->value_size;
    index->lower_limit = get_lower_limit(index->num_buckets);
    index->upper_limit = get_upper_limit(index->num_buckets);
fail:
    if(fclose(fd) < 0) {
        EPRINTF_PATH(path, "fclose failed");
    }
    return index;
}

static HashIndex *
hashindex_init(int capacity, int key_size, int value_size)
{
    HashIndex *index;
    int i;
    capacity = fit_size(capacity);

    if(!(index = malloc(sizeof(HashIndex)))) {
        EPRINTF("malloc header failed");
        return NULL;
    }
    if(!(index->buckets = calloc(capacity, key_size + value_size))) {
        EPRINTF("malloc buckets failed");
        free(index);
        return NULL;
    }
    if(!(index->tmp_entry = calloc(1, key_size + value_size))) {
        EPRINTF("malloc temp entry failed");
        free(index->buckets);
        free(index);
        return NULL;
    }

    index->num_entries = 0;
    index->key_size = key_size;
    index->value_size = value_size;
    index->num_buckets = capacity;
    index->bucket_size = index->key_size + index->value_size;
    index->lower_limit = get_lower_limit(index->num_buckets);
    index->upper_limit = get_upper_limit(index->num_buckets);
    debug_print("\ninit %d < %d\n", index->lower_limit, index->upper_limit);

    for(i = 0; i < capacity; i++) {
        BUCKET_MARK_EMPTY(index, i);
    }
    return index;
}

static void
hashindex_free(HashIndex *index)
{
    free(index->buckets);
    free(index->tmp_entry);
    free(index);
}

static int
hashindex_write(HashIndex *index, const char *path)
{
    off_t buckets_length = (off_t)index->num_buckets * index->bucket_size;
    FILE *fd;
    HashHeader header = {
        .magic = MAGIC,
        .num_entries = _htole32(index->num_entries),
        .num_buckets = _htole32(index->num_buckets),
        .key_size = index->key_size,
        .value_size = index->value_size
    };
    int ret = 1;

    if((fd = fopen(path, "wb")) == NULL) {
        EPRINTF_PATH(path, "fopen for writing failed");
        return 0;
    }
    if(fwrite(&header, 1, sizeof(header), fd) != sizeof(header)) {
        EPRINTF_PATH(path, "fwrite header failed");
        ret = 0;
    }
    if(fwrite(index->buckets, 1, buckets_length, fd) != (size_t) buckets_length) {
        EPRINTF_PATH(path, "fwrite buckets failed");
        ret = 0;
    }
    if(fclose(fd) < 0) {
        EPRINTF_PATH(path, "fclose failed");
    }
    return ret;
}


static const void *
hashindex_get(HashIndex *index, const void *key)
{
    int idx = hashindex_lookup(index, key, NULL);
    if(idx < 0) {
        hashindex_lookup(index, key, NULL);
        return NULL;
    }
    return BUCKET_ADDR(index, idx) + index->key_size;
}


static inline int
rshift_chunk_size(HashIndex *index, int bucket_index) {
    int start = bucket_index;
    while(bucket_index < index->num_buckets) {
        if (BUCKET_IS_EMPTY(index, bucket_index)) {
            return (bucket_index - start) * index->bucket_size;
        }
        bucket_index++;
    }
    return -1;
}

static inline int
lshift_chunk_size(HashIndex *index, int bucket_index) {
    int start = bucket_index;
    while(bucket_index < index->num_buckets) {
        if (BUCKET_IS_EMPTY(index, bucket_index) ||
            (distance(bucket_index,
                      hashindex_index(index, BUCKET_ADDR(index, bucket_index)),
                      index->num_buckets) == 0)) {
            return (bucket_index - start) * index->bucket_size;
        }
        bucket_index++;
    }
    return -1;
}

static int
hashindex_set(HashIndex *index, const void *key, const void *value)
{
    int offset = 0;
    int chunk_size;
    int idx = hashindex_lookup(index, key, &offset);
    if(idx >= 0)
    {
        debug_print("%s", "\nhit\n");
        /* we already have the key in the index we just need to update its value */
        memcpy(BUCKET_ADDR(index, idx) + index->key_size, value, index->value_size);
    }
    else
    {
        /* we don't have the key in the index we need to find an appropriate address */
        debug_print("%s", "\n\nmiss\n");
        if(index->num_entries > index->upper_limit) {
            /* we need to grow the hashindex */
            if(!hashindex_resize(index, grow_size(index->num_buckets))) {
                return 0;
            }
            offset = 0;
        }
        idx = hashindex_index(index, key) + offset;
        if (idx >= index->num_buckets){
            idx = idx - index->num_buckets;
        }
        while(!BUCKET_IS_EMPTY(index, idx) &&
              (offset <= distance(idx,
                                  hashindex_index(index, BUCKET_ADDR(index, idx)),
                                  index->num_buckets))) {
            offset ++;
            idx++;
            if (idx >= index->num_buckets) {
                idx = 0;
            }
        }
        if (!BUCKET_IS_EMPTY(index, idx)) {
            // we have a collision
            chunk_size = rshift_chunk_size(index, idx);
            if (chunk_size > 0) {
                // shift by one bucket
                memmove(BUCKET_ADDR(index, idx+1), BUCKET_ADDR(index, idx), chunk_size);
                // and insert the key
                memcpy(BUCKET_ADDR(index, idx), key, index->key_size);
                memcpy(BUCKET_ADDR(index, idx)+index->key_size, value, index->value_size);
            } else {
                if (chunk_size != -1){
                    debug_print("\n! chunk_size: %d\n\n", chunk_size);
                }
                // we've reached the end of the bucket space, but found no empty bucket
                // make temporary copy of the last entry
                memcpy(index->tmp_entry, BUCKET_ADDR(index, index->num_buckets-1), index->bucket_size);
                if (idx < index->num_buckets - 1) {
                    // shift all remaining buckets by one, unless we're at the very last bucket
                    memmove(BUCKET_ADDR(index, idx+1),
                            BUCKET_ADDR(index, idx),
                            (index->num_buckets - idx -1) * index->bucket_size);
                }
                // insert the value
                memcpy(BUCKET_ADDR(index, idx), key, index->key_size);
                memcpy(BUCKET_ADDR(index, idx) + index->key_size, value, index->value_size);
                idx = 0;
                chunk_size = rshift_chunk_size(index, idx);
                if (chunk_size > 0) {
                    // shift chunk at start by one
                    memmove(BUCKET_ADDR(index, idx+1), BUCKET_ADDR(index, idx), chunk_size);
                } else if (chunk_size == -1) {
                    debug_print("\n! chunk_size: %d\n\n", chunk_size);
                }
                // insert key from the last address at index 0
                memcpy(BUCKET_ADDR(index, idx), index->tmp_entry, index->bucket_size);
            }
        } else {
            memcpy(BUCKET_ADDR(index, idx), key, index->key_size);
            memcpy(BUCKET_ADDR(index, idx)+index->key_size, value, index->value_size);
        }
        index->num_entries += 1;
    }
    return 1;
}

static int
hashindex_delete(HashIndex *index, const void *key)
{
    int idx = hashindex_lookup(index, key, NULL);
    int c_size = -1;
    if (idx < 0) {
        return 1;  // not in index, nothing to do
    }
    if (idx+1 < index->num_buckets) {
        c_size = lshift_chunk_size(index, idx+1);  // includes current idx in chunk
    }
    if(c_size != -1) {
        // the simple case, just shift a chunk
        if (c_size != 0) {
            memmove(BUCKET_ADDR(index, idx), BUCKET_ADDR(index, (idx+1)), c_size);
        }
        // and mark the last position of the chunk empty
        idx += c_size/index->bucket_size;
        BUCKET_MARK_EMPTY(index, idx);
    } else {
        // the complicated case, we shift all the way to the end of the bucket array
        memmove(BUCKET_ADDR(index, idx), BUCKET_ADDR(index, idx+1),
                (index->num_buckets - idx - 1) * index->bucket_size);
        // then check if we need to take the first bucket and move it to the last position
        if (BUCKET_IS_EMPTY(index, 0)) {
            // no need, it's empty anyway
            BUCKET_MARK_EMPTY(index, (index->num_buckets-1));
        }
        else {
            // move first bucket to last address
            memmove(BUCKET_ADDR(index, index->num_buckets-1), BUCKET_ADDR(index, 0),
                    index->bucket_size);
            // then determine if we need to shift an entire chunk after the first bucket
            c_size = lshift_chunk_size(index, 1);
            if(c_size == 0) {
                // nothing to shift, mark first bucket empty and we're done
                BUCKET_MARK_EMPTY(index, 0);
            } else {
                memmove(BUCKET_ADDR(index, 0), BUCKET_ADDR(index, 1), c_size);
                BUCKET_MARK_EMPTY(index, (c_size/index->bucket_size));
            }
        }
    }
    index->num_entries -= 1;
    if(index->num_entries < index->lower_limit) {
        if(!hashindex_resize(index, shrink_size(index->num_buckets))) {
            return 0;
        }
    }
    return 1;
}

static void *
hashindex_next_key(HashIndex *index, const void *key)
{
    int idx = 0;
    if(key) {
        idx = 1 + (key - index->buckets) / index->bucket_size;
    }
    if (idx == index->num_buckets) {
        return NULL;
    }
    while(BUCKET_IS_EMPTY(index, idx)) {
        idx ++;
        if (idx == index->num_buckets) {
            return NULL;
        }
    }
    return BUCKET_ADDR(index, idx);
}

static int
hashindex_len(HashIndex *index)
{
    return index->num_entries;
}

static int
hashindex_size(HashIndex *index)
{
    return sizeof(HashHeader) + index->num_buckets * index->bucket_size;
}

static void
benchmark_getitem(HashIndex *index, char *keys, int key_count)
{
    char *key = keys;
    char *last_addr = key + (32 * key_count);
    /* if (DEBUG){ */
    /*     lookups = 0; collisions = 0; swaps = 0; updates = 0; shortcuts = 0; inserts = 0; */
    /* } */
    while (key < last_addr) {
        hashindex_get(index, key);
        key += 32;
    }
  /*   if (DEBUG){ */
  /*       printf("\n\n\nlookups %f; collisions: %lu; swaps %lu; updates %lu; " */
  /*              "shorts %lu; inserts %lu; buckets %d\n\n\n", */
  /*              (double)(lookups) / key_count, collisions, swaps, updates, shortcuts, */
  /*              inserts, index->num_buckets); */
  /* } */
}

static void
benchmark_setitem(HashIndex *index, char *keys, int key_count)
{
    char *key = keys;
    char *last_addr = key + (32 * key_count);
    uint32_t data[3] = {0, 0, 0};
    /* if (DEBUG){ */
    /*     lookups = 0; collisions = 0; swaps = 0; updates = 0; shortcuts = 0; inserts = 0; */
    /* } */
    while (key < last_addr) {
        hashindex_set(index, key, data);
        key += 32;
    }
    /* if (DEBUG) { */
    /*     printf("\n\n\nlookups %f; collisions: %lu; swaps %lu; updates %lu; shorts %lu; " */
    /*            "inserts %lu; buckets %d\n\n\n", */
    /*            (double)(lookups) / key_count, collisions, swaps, updates, shortcuts, */
    /*            inserts, index->num_buckets); */
    /* } */

}


static void
benchmark_delete(HashIndex *index, char *keys, int key_count)
{
    char *key = keys;
    char *last_addr = key + (32 * key_count);
    /* if (DEBUG){ */
    /*     lookups = 0; collisions = 0; swaps = 0; updates = 0; shortcuts = 0; inserts = 0; */
    /* } */
    while (key < last_addr) {
        hashindex_delete(index, key);
        key += 32;
    }
    /* if (DEBUG) { */
    /*     printf("\n\n\nlookups %f; collisions: %lu; swaps %lu; updates %lu; shorts %lu; " */
    /*            "inserts %lu; buckets %d\n\n\n", */
    /*            (double)(lookups) / key_count, collisions, swaps, updates, shortcuts, */
    /*            inserts, index->num_buckets); */
    /* } */

}


static void
benchmark_churn(HashIndex *index, char *keys, int key_count)
{
    char *key = keys;
    char *last_addr = key + (32 * key_count);
    uint32_t data[3] = {0, 0, 0};
    size_t key_size = index->key_size;
    uint8_t deleted_key[key_size];
    unsigned int period = 0;
    /* if (DEBUG){ */
    /*     lookups = 0; collisions = 0; swaps = 0; updates = 0; shortcuts = 0; inserts = 0; */
    /* } */
    while (key < last_addr) {
        switch (period) {
        case 0:
            memcpy(deleted_key, key, key_size);
            hashindex_delete(index, key);
            break;
        case 1 ... 6:
            hashindex_set(index, key, data);
            break;
        case 7 ... 9:
            hashindex_get(index, key);
            break;
        case 10:
            period = 0;
            hashindex_set(index, deleted_key, data);
            continue;
        }
        period ++;
        key += 32;
    }
    /* if (DEBUG) { */
    /*     printf("\n\n\nlookups %f; collisions: %lu; swaps %lu; updates %lu; shorts %lu; " */
    /*            "inserts %lu; buckets %d\n\n\n", */
    /*            (double)(lookups) / key_count, collisions, swaps, updates, shortcuts, */
    /*            inserts, index->num_buckets); */
    /* } */
}
