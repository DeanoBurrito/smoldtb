#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMOLDTB_INIT_EMPTY_TREE 0

typedef struct dtb_node_t dtb_node;
typedef struct dtb_prop_t dtb_prop;

typedef struct
{
    uintmax_t a;
    uintmax_t b;
} dtb_pair;

typedef struct 
{
    uintmax_t a;
    uintmax_t b;
    uintmax_t c;
} dtb_triplet;

typedef struct
{
    uintmax_t a;
    uintmax_t b;
    uintmax_t c;
    uintmax_t d;
} dtb_quad;

typedef struct
{
    void* (*malloc)(size_t length);
    void (*free)(void* ptr, size_t length);
    void (*on_error)(const char* why);
} dtb_ops;

typedef struct
{
    const char* name;
    size_t child_count;
    size_t prop_count;
    size_t sibling_count;
} dtb_node_stat;

typedef struct
{
    const char* name;
    const void* data;
    size_t data_len;
} dtb_prop_stat;

size_t dtb_query_total_size(uintptr_t fdt_start);

bool dtb_init(uintptr_t start, dtb_ops ops);

dtb_node* dtb_find_compatible(dtb_node* node, const char* str);
dtb_node* dtb_find_phandle(unsigned handle);
dtb_node* dtb_find(const char* path);
dtb_node* dtb_find_child(dtb_node* node, const char* name);
dtb_prop* dtb_find_prop(dtb_node* node, const char* name);

dtb_node* dtb_get_sibling(dtb_node* node);
dtb_node* dtb_get_child(dtb_node* node);
dtb_node* dtb_get_parent(dtb_node* node);
dtb_prop* dtb_get_prop(dtb_node* node, size_t index);
size_t dtb_get_addr_cells_of(dtb_node* node);
size_t dtb_get_size_cells_of(dtb_node* node);
size_t dtb_get_addr_cells_for(dtb_node* node);
size_t dtb_get_size_cells_for(dtb_node* node);
bool dtb_is_compatible(dtb_node* node, const char* str);
bool dtb_stat_node(dtb_node* node, dtb_node_stat* stat);
bool dtb_stat_prop(dtb_prop* prop, dtb_prop_stat* stat);

const char* dtb_read_prop_string(dtb_prop* prop, size_t index);
size_t dtb_read_prop_1(dtb_prop* prop, size_t cell_count, uintmax_t* vals);
size_t dtb_read_prop_2(dtb_prop* prop, dtb_pair layout, dtb_pair* vals);
size_t dtb_read_prop_3(dtb_prop* prop, dtb_triplet layout, dtb_triplet* vals);
size_t dtb_read_prop_4(dtb_prop* prop, dtb_quad layout, dtb_quad* vals);

#ifdef SMOLDTB_ENABLE_WRITE_API
#define SMOLDTB_FINALISE_FAILURE ((size_t)-1)

size_t dtb_finalise_to_buffer(void* buffer, size_t buffer_size, uint32_t boot_cpu_id);

dtb_node* dtb_find_or_create_node(const char* path);
dtb_prop* dtb_find_or_create_prop(dtb_node* node, const char* name);
dtb_node* dtb_create_sibling(dtb_node* node, const char* name);
dtb_node* dtb_create_child(dtb_node* node, const char* name);
dtb_prop* dtb_create_prop(dtb_node* node, const char* name);

bool dtb_destroy_node(dtb_node* node);
bool dtb_destroy_prop(dtb_prop* prop);

bool dtb_write_prop_string(dtb_prop* prop, const char* str, size_t str_len);
bool dtb_write_prop_1(dtb_prop* prop, size_t count, size_t cell_count, const uintmax_t* vals);
bool dtb_write_prop_2(dtb_prop* prop, size_t count, dtb_pair layout, const dtb_pair* vals);
bool dtb_write_prop_3(dtb_prop* prop, size_t count, dtb_triplet layout, const dtb_triplet* vals);
bool dtb_write_prop_4(dtb_prop* prop, size_t count, dtb_quad layout, const dtb_quad* vals);
#endif

#ifdef __cplusplus
}
#endif
