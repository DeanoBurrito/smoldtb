#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct dtb_node_t dtb_node;
typedef struct dtb_prop_t dtb_prop;

typedef struct
{
    size_t a;
    size_t b;
} dtb_pair;

typedef struct 
{
    size_t a;
    size_t b;
    size_t c;
} dtb_triplet;

typedef struct
{
    size_t a;
    size_t b;
    size_t c;
    size_t d;
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

void dtb_init(uintptr_t start, dtb_ops ops);

dtb_node* dtb_find_compatible(dtb_node* node, const char* str);
dtb_node* dtb_find_phandle(unsigned handle);
dtb_node* dtb_find(const char* path);
dtb_node* dtb_find_child(dtb_node* node, const char* name);

dtb_node* dtb_get_sibling(dtb_node* node);
dtb_node* dtb_get_child(dtb_node* node);
dtb_node* dtb_get_parent(dtb_node* node);
dtb_prop* dtb_get_prop(dtb_node* node, const char* name);
void dtb_stat_node(dtb_node* node, dtb_node_stat* stat);

const char* dtb_read_string(dtb_prop* prop, size_t index);
size_t dtb_read_prop_values(dtb_prop* prop, size_t cell_count, size_t* vals);
size_t dtb_read_prop_pairs(dtb_prop* prop, dtb_pair layout, dtb_pair* vals);
size_t dtb_read_prop_triplets(dtb_prop* prop, dtb_triplet layout, dtb_triplet* vals);
size_t dtb_read_prop_quads(dtb_prop* prop, dtb_quad layout, dtb_quad* vals);

