#include <hashmap.h>
#include <slab.h>
#include <spinlock.h>
#include <panic.h>
#include <type.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/// @brief 内置哈希函数：FNV-1a 32 位。
/// @param key 键指针。
/// @param len 键的字节长度。
/// @return 32 位哈希值。
static uint32_t default_hash_func(const void *key, uint32_t len)
{
    const uint8_t *data = (const uint8_t *)key;
    uint32_t hash = 2166136261u;
    uint32_t i;

    for (i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

/// @brief 内置键比较函数：逐字节比较，长度由调用方给定。
/// @param key1 第一个键指针。
/// @param key2 第二个键指针。
/// @param len 比较的字节数。
/// @return 两段内存完全一致返回 true，否则返回 false。
static bool default_key_equal(const void *key1, const void *key2, uint32_t len)
{
    const uint8_t *k1 = (const uint8_t *)key1;
    const uint8_t *k2 = (const uint8_t *)key2;
    uint32_t i;

    if (key1 == key2)
        return true;
    if (!key1 || !key2)
        return false;

    for (i = 0; i < len; i++) {
        if (k1[i] != k2[i])
            return false;
    }
    return true;
}

/// @brief 将 n 向上取整到最近的 2 的幂，最小值不低于 HASH_TABLE_MIN_SIZE。
///        桶数保持 2 的幂可让 `hash % size` 与 `hash & (size-1)` 等价，
///        也便于后续切换到位运算加速。
/// @param n 期望的桶数。
/// @return >= max(n, HASH_TABLE_MIN_SIZE) 的最小 2 的幂。
static uint32_t round_up_pow2(uint32_t n)
{
    uint32_t p = HASH_TABLE_MIN_SIZE;

    while (p < n)
        p <<= 1;
    return p;
}

/// @brief 创建一个哈希表。分配表结构和桶数组，桶数组清零，锁初始化。
/// @param initial_size 期望的初始桶数量；小于最小值会被提升，非 2 的幂会上取整。
/// @param hash_func 哈希函数；NULL 时使用 default_hash_func。
/// @param key_equal 键相等函数；NULL 时使用 default_key_equal。
/// @return 成功返回哈希表指针；slab 分配失败时 panic_wrong 并返回 NULL。
hash_table_t *hash_table_create(uint32_t initial_size,
                                uint32_t (*hash_func)(const void *, uint32_t),
                                bool (*key_equal)(const void *, const void *, uint32_t))
{
    hash_table_t *ht;
    uint32_t i;

    if (initial_size < HASH_TABLE_MIN_SIZE)
        initial_size = HASH_TABLE_MIN_SIZE;
    initial_size = round_up_pow2(initial_size);

    ht = (hash_table_t *)slab_alloc(sizeof(hash_table_t));
    if (!ht) {
        panic_wrong("hash_table_create: slab_alloc(ht) failed\n");
        return NULL;
    }

    ht->buckets = (hash_node_t **)slab_alloc(sizeof(hash_node_t *) * initial_size);
    if (!ht->buckets) {
        slab_free(ht);
        panic_wrong("hash_table_create: slab_alloc(buckets, %u) failed\n",
                    (uint32_t)(sizeof(hash_node_t *) * initial_size));
        return NULL;
    }

    for (i = 0; i < initial_size; i++)
        ht->buckets[i] = NULL;

    ht->size      = initial_size;
    ht->count     = 0;
    ht->hash_func = hash_func ? hash_func : default_hash_func;
    ht->key_equal = key_equal ? key_equal : default_key_equal;

    init_spinlock(&ht->lock);

    return ht;
}

/// @brief 销毁哈希表：释放所有节点，再释放桶数组和表结构。不释放 key 和 value。
/// @param ht 目标哈希表；为 NULL 直接返回。
/// @return 无。
void hash_table_destroy(hash_table_t *ht)
{
    uint32_t i;
    hash_node_t *node, *next;

    if (!ht)
        return;

    acquire(&ht->lock);

    for (i = 0; i < ht->size; i++) {
        node = ht->buckets[i];
        while (node) {
            next = node->next;
            slab_free(node);
            node = next;
        }
    }

    release(&ht->lock);

    slab_free(ht->buckets);
    slab_free(ht);
}

/// @brief 内部：重新分配桶数组并把所有节点重新挂到新桶上。
///        调用者必须已持有 ht->lock，本函数不自己加锁。
///        失败时保持原桶数组不变，表结构仍可用。
/// @param ht 目标哈希表。
/// @param new_size 新的桶数量；会被 round_up_pow2 规范化。
/// @return 成功返回 true；new_size 非法或新桶数组分配失败返回 false。
static bool hash_table_resize(hash_table_t *ht, uint32_t new_size)
{
    hash_node_t **new_buckets;
    hash_node_t *node, *next;
    uint32_t i, new_index;

    new_size = round_up_pow2(new_size);
    if (new_size < HASH_TABLE_MIN_SIZE)
        return false;

    new_buckets = (hash_node_t **)slab_alloc(sizeof(hash_node_t *) * new_size);
    if (!new_buckets)
        return false;

    for (i = 0; i < new_size; i++)
        new_buckets[i] = NULL;

    for (i = 0; i < ht->size; i++) {
        node = ht->buckets[i];
        while (node) {
            next = node->next;
            new_index = node->hash % new_size;
            node->next = new_buckets[new_index];
            new_buckets[new_index] = node;
            node = next;
        }
    }

    slab_free(ht->buckets);
    ht->buckets = new_buckets;
    ht->size    = new_size;
    return true;
}

/// @brief 插入键值对。键已存在时按 overwrite 决定覆盖或拒绝。
///        插入前按负载因子判断是否翻倍扩容；扩容失败仅告警，继续插入。
/// @param ht 目标哈希表；NULL、key 为 NULL、key_len 为 0 时返回 false。
/// @param key 键指针；不复制内容，只保存指针。
/// @param key_len 键的字节长度。
/// @param value 值指针，可为 NULL。
/// @param overwrite 键冲突时是否覆盖已有 value。
/// @return 成功插入或覆盖返回 true；参数非法、键冲突且不覆盖、节点分配失败返回 false。
bool hash_table_insert(hash_table_t *ht, const void *key, uint32_t key_len,
                       void *value, bool overwrite)
{
    uint32_t hash, index;
    hash_node_t *node, *new_node;

    if (!ht || !key || key_len == 0)
        return false;

    hash  = ht->hash_func(key, key_len);
    index = hash % ht->size;

    acquire(&ht->lock);

    node = ht->buckets[index];
    while (node) {
        if (node->hash == hash &&
            node->key_len == key_len &&
            ht->key_equal(node->key, key, key_len)) {
            if (overwrite) {
                node->value = value;
                release(&ht->lock);
                return true;
            }
            release(&ht->lock);
            return false;
        }
        node = node->next;
    }

    if ((ht->count + 1) * 100 / ht->size > HASH_TABLE_LOAD_FACTOR) {
        if (!hash_table_resize(ht, ht->size * 2)) {
            panic_wrong("hash_table_insert: resize %u -> %u failed\n",
                        ht->size, ht->size * 2);
        }
        index = hash % ht->size;
    }

    new_node = (hash_node_t *)slab_alloc(sizeof(hash_node_t));
    if (!new_node) {
        panic_wrong("hash_table_insert: slab_alloc(node) failed\n");
        release(&ht->lock);
        return false;
    }

    new_node->key     = (void *)key;
    new_node->value   = value;
    new_node->key_len = key_len;
    new_node->hash    = hash;
    new_node->next    = ht->buckets[index];
    ht->buckets[index] = new_node;
    ht->count++;

    release(&ht->lock);
    return true;
}

/// @brief 按 key 查找 value。找到时返回保存的指针，不复制。
/// @param ht 目标哈希表；NULL、key 为 NULL、key_len 为 0 时返回 NULL。
/// @param key 键指针。
/// @param key_len 键的字节长度。
/// @return 找到返回保存的 value 指针（可能为 NULL）；未找到或参数非法返回 NULL。
void *hash_table_lookup(hash_table_t *ht, const void *key, uint32_t key_len)
{
    uint32_t hash, index;
    hash_node_t *node;
    void *value = NULL;

    if (!ht || !key || key_len == 0)
        return NULL;

    hash  = ht->hash_func(key, key_len);
    index = hash % ht->size;

    acquire(&ht->lock);

    node = ht->buckets[index];
    while (node) {
        if (node->hash == hash &&
            node->key_len == key_len &&
            ht->key_equal(node->key, key, key_len)) {
            value = node->value;
            break;
        }
        node = node->next;
    }

    release(&ht->lock);
    return value;
}

/// @brief 按 key 删除条目并释放节点。不释放 key 和 value 指向的内存。
///        删除后若负载低于负载因子的一半且 size 大于最小值，尝试减半缩容。
/// @param ht 目标哈希表；NULL、key 为 NULL、key_len 为 0 时返回 false。
/// @param key 键指针。
/// @param key_len 键的字节长度。
/// @return 找到并删除返回 true；键不存在或参数非法返回 false。
bool hash_table_delete(hash_table_t *ht, const void *key, uint32_t key_len)
{
    uint32_t hash, index;
    hash_node_t *node, *prev;
    bool found = false;

    if (!ht || !key || key_len == 0)
        return false;

    hash  = ht->hash_func(key, key_len);
    index = hash % ht->size;

    acquire(&ht->lock);

    prev = NULL;
    node = ht->buckets[index];
    while (node) {
        if (node->hash == hash &&
            node->key_len == key_len &&
            ht->key_equal(node->key, key, key_len)) {
            if (prev)
                prev->next = node->next;
            else
                ht->buckets[index] = node->next;

            slab_free(node);
            ht->count--;
            found = true;
            break;
        }
        prev = node;
        node = node->next;
    }

    if (found && ht->size > HASH_TABLE_MIN_SIZE &&
        ht->count * 100 / ht->size < HASH_TABLE_LOAD_FACTOR / 2) {
        if (!hash_table_resize(ht, ht->size / 2)) {
            panic_wrong("hash_table_delete: shrink %u -> %u failed\n",
                        ht->size, ht->size / 2);
        }
    }

    release(&ht->lock);
    return found;
}

/// @brief 读取当前条目数量，持锁读取保证一致性。
/// @param ht 目标哈希表；为 NULL 返回 0。
/// @return 当前 count。
uint32_t hash_table_count(hash_table_t *ht)
{
    uint32_t count;

    if (!ht)
        return 0;

    acquire(&ht->lock);
    count = ht->count;
    release(&ht->lock);
    return count;
}

/// @brief 在持锁状态下遍历所有条目，对每条调用 callback。
///        回调期间锁被持有，回调内不得调用本表任何接口，否则死锁。
///        遍历顺序不保证。
/// @param ht 目标哈希表；NULL 或 callback 为 NULL 时直接返回。
/// @param callback 回调函数，签名 (key, key_len, value, arg)。
/// @param arg 透传给 callback 的用户参数。
/// @return 无。
void hash_table_foreach(hash_table_t *ht,
                        void (*callback)(const void *, uint32_t,
                                         void *, void *),
                        void *arg)
{
    uint32_t i;
    hash_node_t *node;

    if (!ht || !callback)
        return;

    acquire(&ht->lock);

    for (i = 0; i < ht->size; i++) {
        node = ht->buckets[i];
        while (node) {
            callback(node->key, node->key_len, node->value, arg);
            node = node->next;
        }
    }

    release(&ht->lock);
}


/// @brief 原子地“键不存在才插入”。持写锁期间完成“查重 + 插入”，
///        期间不会被其它线程插入同一个键。
///        等价于 hash_table_insert(ht, key, key_len, value, /*overwrite=*/false)，
///        但语义上明确表示这是一次原子的 check-and-insert。
/// @param ht 目标哈希表；NULL、key 为 NULL、key_len 为 0 时返回 false。
/// @param key 键指针（不复制）。
/// @param key_len 键的字节长度。
/// @param value 值指针，可为 NULL。
/// @return 插入成功 true；键已存在、参数非法、节点分配失败 false。
bool hash_table_insert_if_absent(hash_table_t *ht, const void *key,
                                 uint32_t key_len, void *value)
{
    uint32_t hash, index;
    hash_node_t *node, *new_node;

    if (!ht || !key || key_len == 0)
        return false;

    hash  = ht->hash_func(key, key_len);
    index = hash % ht->size;

    acquire(&ht->lock);

    /* 查重：键已存在则拒绝插入 */
    node = ht->buckets[index];
    while (node) {
        if (node->hash == hash &&
            node->key_len == key_len &&
            ht->key_equal(node->key, key, key_len)) {
            release(&ht->lock);
            return false;
        }
        node = node->next;
    }

    /* 扩容检查 */
    if ((ht->count + 1) * 100 / ht->size > HASH_TABLE_LOAD_FACTOR) {
        if (!hash_table_resize(ht, ht->size * 2)) {
            panic_wrong("hash_table_insert_if_absent: resize %u -> %u failed\n",
                        ht->size, ht->size * 2);
        }
        index = hash % ht->size;
    }

    /* 分配新节点 */
    new_node = (hash_node_t *)slab_alloc(sizeof(hash_node_t));
    if (!new_node) {
        panic_wrong("hash_table_insert_if_absent: slab_alloc(node) failed\n");
        release(&ht->lock);
        return false;
    }

    new_node->key     = (void *)key;
    new_node->value   = value;
    new_node->key_len = key_len;
    new_node->hash    = hash;
    new_node->next    = ht->buckets[index];
    ht->buckets[index] = new_node;
    ht->count++;

    release(&ht->lock);
    return true;
}