#ifndef _INC_HASHMAP_H
#define _INC_HASHMAP_H

#include <type.h>

#define HASH_TABLE_INIT_SIZE    16
#define HASH_TABLE_LOAD_FACTOR  75
#define HASH_TABLE_MIN_SIZE     8

typedef struct hash_node {
    void    *key;       /* 键指针，由调用者拥有，哈希表不复制也不释放 */
    void    *value;     /* 值指针，由调用者拥有，哈希表不复制也不释放 */
    uint32_t key_len;   /* 键的字节长度 */
    uint32_t hash;      /* 缓存该键的哈希值，避免重复计算 */
    struct hash_node *next; /* 同桶冲突链的下一个节点 */
} hash_node_t;

typedef struct hash_table {
    hash_node_t **buckets; /* 桶数组，每个元素是一条冲突链的头指针 */
    uint32_t size;         /* 桶数组长度，始终为 2 的幂，>= HASH_TABLE_MIN_SIZE */
    uint32_t count;        /* 当前元素个数 */
    spinlock_t lock;       /* 保护所有字段的自旋锁 */
    uint32_t (*hash_func)(const void *, uint32_t);          /* 哈希函数 */
    bool     (*key_equal)(const void *, const void *, uint32_t); /* 键比较函数 */
} hash_table_t;

/// @brief 创建一个哈希表。所有字段初始化为空表，锁初始化为未持有。
/// @param initial_size 期望的初始桶数量；小于 HASH_TABLE_MIN_SIZE 会被提升到最小值，
///                     非 2 的幂会被向上取整到最近的 2 的幂。
/// @param hash_func 哈希函数；传 NULL 时使用内置 FNV-1a。
///                 签名要求：输入键指针和键字节长度，返回 32 位哈希值。
/// @param key_equal 键相等判定函数；传 NULL 时使用逐字节内存比较。
///                  签名要求：输入两个键指针和长度，返回是否相等。
/// @return 成功返回哈希表指针；slab 分配失败或参数非法时 panic_wrong 并返回 NULL。
hash_table_t *hash_table_create(uint32_t initial_size,
                                uint32_t (*hash_func)(const void *, uint32_t),
                                bool (*key_equal)(const void *, const void *, uint32_t));

/// @brief 销毁哈希表，释放所有节点、桶数组和表结构本身。
///        不释放 key 和 value 所指向的内存——它们归调用者所有。
/// @param ht 目标哈希表；为 NULL 时直接返回。
/// @return 无。
void hash_table_destroy(hash_table_t *ht);

/// @brief 插入一个键值对。
///        键已存在时：overwrite 为 true 则替换 value，为 false 则拒绝插入。
///        插入前若 (count+1)/size 超过 HASH_TABLE_LOAD_FACTOR（百分比）会尝试翻倍扩容。
///        注意：只保存 key 指针，不复制内容，调用者需保证 key 生命周期覆盖条目存活期。
/// @param ht 目标哈希表；为 NULL 返回 false。
/// @param key 键指针；为 NULL 或 key_len 为 0 返回 false。
/// @param key_len 键的字节长度。
/// @param value 值指针，可为 NULL。
/// @param overwrite 键冲突时是否覆盖已有值。
/// @return 成功插入或成功覆盖返回 true；键冲突且不允许覆盖、参数非法、分配失败返回 false。
bool hash_table_insert(hash_table_t *ht, const void *key, uint32_t key_len,
                       void *value, bool overwrite);

/// @brief 按 key 查找对应 value。
/// @param ht 目标哈希表；为 NULL 返回 NULL。
/// @param key 键指针；为 NULL 或 key_len 为 0 返回 NULL。
/// @param key_len 键的字节长度。
/// @return 找到时返回保存的 value 指针（可能是 NULL）；未找到或参数非法返回 NULL。
///         注意：无法区分“键不存在”和“键存在但值为 NULL”。
void *hash_table_lookup(hash_table_t *ht, const void *key, uint32_t key_len);

/// @brief 按 key 删除条目，释放对应节点。不释放 key 和 value 指向的内存。
///        删除后若 count/size 低于负载因子的一半且 size 大于最小值，会尝试减半缩容。
/// @param ht 目标哈希表；为 NULL 返回 false。
/// @param key 键指针；为 NULL 或 key_len 为 0 返回 false。
/// @param key_len 键的字节长度。
/// @return 找到并删除返回 true；键不存在或参数非法返回 false。
bool hash_table_delete(hash_table_t *ht, const void *key, uint32_t key_len);

/// @brief 读取当前元素个数。
/// @param ht 目标哈希表；为 NULL 返回 0。
/// @return 哈希表中条目的数量。
uint32_t hash_table_count(hash_table_t *ht);

/// @brief 在持锁状态下遍历所有条目，对每个条目调用一次 callback。
///        回调期间持有 ht->lock，回调内不得再调用本哈希表的任何接口，否则死锁。
///        不保证遍历顺序。
/// @param ht 目标哈希表；为 NULL 直接返回。
/// @param callback 回调函数，签名 (key, key_len, value, arg)；为 NULL 直接返回。
/// @param arg 透传给 callback 的用户参数，可为 NULL。
/// @return 无。
void hash_table_foreach(hash_table_t *ht,
                        void (*callback)(const void *, uint32_t, void *, void *),
                        void *arg);


/// @brief 原子地“键不存在才插入”。键已存在时不修改任何内容，也不覆盖旧值。
///        用于替代“先 lookup 判断 NULL，再 insert”这种非原子的两步操作，避免 TOCTOU。
/// @param ht 目标哈希表；为 NULL 返回 false。
/// @param key 键指针；为 NULL 或 key_len 为 0 返回 false。
/// @param key_len 键的字节长度。
/// @param value 值指针，可为 NULL。
/// @return 插入成功返回 true；键已存在、参数非法或节点分配失败返回 false。
bool hash_table_insert_if_absent(hash_table_t *ht, const void *key,
                                 uint32_t key_len, void *value);

#endif