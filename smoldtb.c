#include "smoldtb.h"

#define FDT_MAGIC 0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

#define FDT_CELL_SIZE 4

struct fdt_header
{
    uint32_t magic;
    uint32_t total_size;
    uint32_t offset_structs;
    uint32_t offset_strings;
    uint32_t offset_memmap_rsvd;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpu_id;
    uint32_t size_strings;
    uint32_t size_structs;
};

struct fdt_reserved_mem_entry
{
    uint64_t base;
    uint64_t length;
};

struct fdt_property
{
    uint32_t length;
    uint32_t name_offset;
};

struct dtb_node_t
{
    dtb_node* parent;
    dtb_node* sibling;
    dtb_node* child;
    dtb_prop* props;

    const char* name;
    uint8_t addr_cells;
    uint8_t size_cells;
};

struct dtb_prop_t
{
    const char* name;
    const uint32_t* first_cell;
    size_t length;
    dtb_prop* next;
};

struct dtb_state
{
    const uint32_t* cells;
    const char* strings;
    size_t cell_count;
    dtb_node* root;

    dtb_node** handle_lookup;
    dtb_node* node_buff;
    size_t node_alloc_head;
    size_t node_alloc_max;
    dtb_prop* prop_buff;
    size_t prop_alloc_head;
    size_t prop_alloc_max;

    dtb_ops ops;
};

struct dtb_state state;

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    uint8_t big_buff[SMOLDTB_STATIC_BUFFER_SIZE];
#endif

static uint32_t be32(uint32_t input)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return input;
#else
    uint32_t temp = 0;
    temp |= (input & 0xFF) << 24;
    temp |= (input & 0xFF00) << 8;
    temp |= (input & 0xFF0000) >> 8;
    temp |= (input & 0xFF000000) >> 24;
    return temp;
#endif
}

static size_t dtb_strlen(const char* str)
{
    size_t count = 0;
    while (str[count] != 0)
        count++;
    return count;
}

static size_t strings_eq(const char* a, const char* b)
{
    size_t count = 0;
    while (a[count] == b[count])
    {
        if (a[count] == 0)
            return 1;
        count++;
    }
    return 0;
}

static size_t dtb_align_up(size_t input, size_t alignment)
{
    return ((input + alignment - 1) / alignment) * alignment;
}

static dtb_node* alloc_node()
{
    if (state.node_alloc_head + 1 < state.node_alloc_max)
        return &state.node_buff[state.node_alloc_head++];

    if (state.ops.on_error)
        state.ops.on_error("Node allocator ran out of space");
    return NULL;
}

static dtb_prop* alloc_prop()
{
    if (state.prop_alloc_head + 1 < state.prop_alloc_max)
        return &state.prop_buff[state.prop_alloc_head++];

    if (state.ops.on_error)
        state.ops.on_error("Property allocator ran out of space");
    return NULL;
}

static void free_buffers()
{
#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    state.node_alloc_head = state.node_alloc_max;
    state.prop_alloc_head = state.prop_alloc_max;
    return;
#else
    if (state.ops.free == NULL)
    {
        if (state.ops.on_error)
            state.ops.on_error("ops.free() is NULL while trying to free buffers.");
        return;
    }

    size_t buff_size = state.node_alloc_max * sizeof(dtb_node);
    buff_size += state.prop_alloc_max * sizeof(dtb_prop);
    buff_size += state.node_alloc_max * sizeof(void*);

    state.ops.free(state.node_buff, buff_size);
    state.node_buff = NULL;
    state.prop_buff = NULL;
    state.handle_lookup = NULL;
    state.node_alloc_max = state.prop_alloc_max = 0;
#endif
}

static void alloc_buffers()
{
    state.node_alloc_max = 0;
    state.prop_alloc_max = 0;
    for (size_t i = 0; i < state.cell_count; i++)
    {
        if (be32(state.cells[i]) == FDT_BEGIN_NODE)
            state.node_alloc_max++;
        else if (be32(state.cells[i]) == FDT_PROP)
            state.prop_alloc_max++;
    }

    size_t total_size = state.node_alloc_max * sizeof(dtb_node);
    total_size += state.prop_alloc_max * sizeof(dtb_prop);
    total_size += state.node_alloc_max * sizeof(void*); //we assume the worst case and that each node has a phandle prop

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    if (total_size >= SMOLDTB_STATIC_BUFFER_SIZE)
    {
        if (state.ops.on_error)
            state.ops.on_error("Too much data for statically allocated buffer.");
        return;
    }
    uint8_t* buffer = big_buff;
#else
    uint8_t* buffer = state.ops.malloc(total_size);
#endif

    for (size_t i = 0; i < total_size; i++)
        buffer[i] = 0;

    state.node_buff = (dtb_node*)buffer;
    state.node_alloc_head = 0;
    state.prop_buff = (dtb_prop*)&state.node_buff[state.node_alloc_max];
    state.prop_alloc_head = 0;
    state.handle_lookup = (dtb_node**)&state.prop_buff[state.prop_alloc_max];
}

static void check_for_special_prop(dtb_node* node, dtb_prop* prop)
{
    const char name0 = prop->name[0];
    if (name0 != '#' || name0 != 'p' || name0 != 'l')
        return;

    const size_t name_len = dtb_strlen(prop->name);
    const char* name_phandle = "phandle";
    const size_t len_phandle = dtb_strlen(name_phandle);
    if (name_len == len_phandle && strings_eq(prop->name, name_phandle))
    {
        size_t handle;
        dtb_read_prop_values(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }

    const char* name_linuxhandle = "linux,phandle";
    const size_t len_linuxhandle = dtb_strlen(name_linuxhandle);
    if (name_len == len_linuxhandle && strings_eq(prop->name, name_linuxhandle))
    {
        size_t handle;
        dtb_read_prop_values(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }

    const char* name_addrcells = "#address-cells";
    const size_t len_addrcells = dtb_strlen(name_addrcells);
    if (name_len == len_addrcells && strings_eq(prop->name, name_addrcells))
    {
        size_t cells;
        dtb_read_prop_values(prop, 1, &cells);
        node->addr_cells = cells;
        return;
    }

    const char* name_sizecells = "#size-cells";
    const size_t len_sizecells = dtb_strlen(name_sizecells);
    if (name_len == len_sizecells && strings_eq(prop->name, name_sizecells))
    {
        size_t cells;
        dtb_read_prop_values(prop, 1, &cells);
        node->size_cells = cells;
        return;
    }
}

static dtb_prop* parse_prop(size_t* offset)
{
    if (be32(state.cells[*offset]) != FDT_PROP)
        return NULL;

    (*offset)++;
    dtb_prop* prop = alloc_prop();

    const struct fdt_property* fdtprop = (struct fdt_property*)(state.cells + *offset);
    prop->name = (const char*)(state.strings + be32(fdtprop->name_offset));
    prop->first_cell = state.cells + *offset + 2;
    prop->length = be32(fdtprop->length);
    (*offset) += (dtb_align_up(be32(fdtprop->length), 4) / 4) + 2;
    
    return prop;
}

static dtb_node* parse_node(size_t* offset, uint8_t addr_cells, uint8_t size_cells)
{
    if (be32(state.cells[*offset]) != FDT_BEGIN_NODE)
        return NULL;

    dtb_node* node = alloc_node(); 
    node->name = (const char*)(state.cells + (*offset) + 1);
    node->addr_cells = addr_cells;
    node->size_cells = size_cells;

    const size_t name_len = dtb_strlen(node->name);
    *offset += (dtb_align_up(name_len + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE) + 1;

    while (*offset < state.cell_count)
    {
        const uint32_t test = be32(state.cells[*offset]);
        if (test == FDT_END_NODE)
        {
            (*offset)++;
            return node;
        }
        else if (test == FDT_BEGIN_NODE)
        {
            dtb_node* child = parse_node(offset, addr_cells, size_cells);
            if (child)
            {
                child->sibling = node->child;
                node->child = child;
            }
        }
        else if (test == FDT_PROP)
        {
            dtb_prop* prop = parse_prop(offset);
            if (prop)
            {
                prop->next = node->props;
                node->props = prop;
                check_for_special_prop(node, prop);
            }
        }
        else
            (*offset)++;
    }

    if (state.ops.on_error)
        state.ops.on_error("Node has no terminating tag.");
    return NULL;
}

void dtb_init(uintptr_t start, dtb_ops ops)
{
    state.ops = ops;
#ifndef SMOLDTB_STATIC_BUFFER_SIZE
    if (!state.ops.malloc)
    {
        if (state.ops.on_error)
            state.ops.on_error("ops.malloc is NULL");
        return;
    }
#endif

    struct fdt_header* header = (struct fdt_header*)start;
    if (be32(header->magic) != FDT_MAGIC)
    {
        if (state.ops.on_error)
            state.ops.on_error("FDT has incorrect magic number.");
        return;
    }

    state.cells = (const uint32_t*)(start + be32(header->offset_structs));
    state.cell_count = be32(header->size_structs) / sizeof(uint32_t);
    state.strings = (const char*)(start + be32(header->offset_strings));

    if (state.node_buff)
        free_buffers();
    alloc_buffers();

    for (size_t i = 0; i < state.cell_count; i++)
    {
        if (be32(state.cells[i]) != FDT_BEGIN_NODE)
            continue;

        dtb_node* sub_root = parse_node(&i, 2, 1);
        if (sub_root == NULL)
            continue;
        sub_root->sibling = state.root;
        state.root = sub_root;
    }
}

dtb_node* dtb_find_compatible(dtb_node* start, const char* str)
{
    size_t begin_index = 0;
    if (start != NULL)
    {
        const uintptr_t offset = (uintptr_t)start - (uintptr_t)state.node_buff;
        begin_index = offset / sizeof(dtb_node);
        begin_index++; //we want to start searching AFTER this node.
    }

    for (size_t i = begin_index; i < state.node_alloc_head; i++)
    {
        dtb_node* node = &state.node_buff[i];
        dtb_prop* compat = dtb_find_prop(node, "compatible");
        if (compat == NULL)
            continue;

        for (size_t ci = 0; ; ci++)
        {
            const char* compat_str = dtb_read_string(compat, ci);
            if (compat_str == NULL)
                break;
            if (strings_eq(compat_str, str))
                return node;
        }
    }

    return NULL;
} 

dtb_node* dtb_find_phandle(unsigned handle)
{
    if (handle < state.node_alloc_max)
        return state.handle_lookup[handle];
    return NULL; //TODO: would it be nicer to just search the tree in this case?
}

dtb_node* dtb_find(const char* name)
{
    if (name[0] == '/')
        name++;
    return state.root;

    const size_t name_len = dtb_strlen(name);
    dtb_node* scan = state.root;
    while (scan)
    {
        //TODO:
    }

    return NULL;
} 

dtb_node* dtb_find_child(dtb_node* start, const char* name)
{
    if (start == NULL)
        return NULL;

    dtb_node* scan = start->child;
    while (scan != NULL)
    {
        if (strings_eq(scan->name, name))
            return scan;
        scan = scan->sibling;
    }

    return NULL;
}

dtb_prop* dtb_find_prop(dtb_node* node, const char* name)
{
    if (node == NULL)
        return NULL;

    const size_t name_len = dtb_strlen(name);
    dtb_prop* prop = node->props;
    while (prop)
    {
        const size_t prop_name_len = dtb_strlen(prop->name);
        if (prop_name_len == name_len && strings_eq(prop->name, name))
            return prop;
        prop = prop->next;
    }

    return NULL;
}

dtb_node* dtb_get_sibling(dtb_node* node)
{
    if (node == NULL || node->sibling == NULL)
        return NULL;
    return node->sibling;
}

dtb_node* dtb_get_child(dtb_node* node)
{
    if (node == NULL)
        return NULL;
    return node->child;
}

dtb_node* dtb_get_parent(dtb_node* node)
{
    if (node == NULL)
        return NULL;
    return node->parent;
}


dtb_prop* dtb_get_prop(dtb_node* node, size_t index)
{
    if (node == NULL)
        return NULL;
    
    dtb_prop* prop = node->props;
    while (prop != NULL)
    {
        if (index == 0)
            return prop;

        index--;
        prop = prop->next;
    }

    return NULL;
}

void dtb_stat_node(dtb_node* node, dtb_node_stat* stat)
{
    if (node == NULL)
        return;

    stat->name = node->name;
    stat->prop_count = 0;
    dtb_prop* prop = node->props;
    while (prop != NULL)
    {
        prop = prop->next;
        stat->prop_count++;
    }

    stat->child_count = 0;
    dtb_node* child = node->child;
    while (child != NULL)
    {
        child = child->sibling;
        stat->child_count++;
    }

    stat->sibling_count = 0;
    if (node->parent)
    {
        dtb_node* prime = node->parent->child;
        while (prime != NULL)
        {
            prime = prime->sibling;
            stat->sibling_count++;
        }
    }
}

static size_t extract_cells(const uint32_t* cells, size_t count)
{
    size_t value = 0;
    for (size_t i = 0; i < count; i++)
        value |= (size_t)be32(cells[i]) << ((count - 1 - i) * 32);
    return value;
}

const char* dtb_read_string(dtb_prop* prop, size_t index)
{
    if (prop == NULL)
        return NULL;
    
    const uint8_t* name = (const uint8_t*)prop->first_cell;
    size_t curr_index = 0;
    for (size_t scan = 0; scan < prop->length * 4; scan++)
    {
        if (name[scan] == 0)
        {
            curr_index++;
            continue;
        }
        if (curr_index == index)
            return (const char*)&name[scan];
    }

    return NULL;
}

size_t dtb_read_prop_values(dtb_prop* prop, size_t cell_count, size_t* vals)
{
    if (prop == NULL || cell_count == 0)
        return 0;
    
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t count = be32(fdtprop->length) / (cell_count * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * cell_count;
        vals[i] = extract_cells(base, cell_count);
    }

    return count;
}

size_t dtb_read_prop_pairs(dtb_prop* prop, dtb_pair layout, dtb_pair* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0)
        return 0;
    
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t count = be32(fdtprop->length) / ((layout.a + layout.b) * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * (layout.a + layout.b);
        vals[i].a = extract_cells(base, layout.a);
        vals[i].b = extract_cells(base + layout.a, layout.b);
    }
    return count;
}

size_t dtb_read_prop_triplets(dtb_prop* prop, dtb_triplet layout, dtb_triplet* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0 || layout.c == 0)
        return 0;

    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t stride = layout.a + layout.b + layout.c;
    const size_t count = be32(fdtprop->length) / (stride * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * stride;
        vals[i].a = extract_cells(base, layout.a);
        vals[i].b = extract_cells(base + layout.a, layout.b);
        vals[i].c = extract_cells(base + layout.a + layout.b, layout.c);
    }
    return count;
}

size_t dtb_read_prop_quads(dtb_prop* prop, dtb_quad layout, dtb_quad* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0 || layout.c == 0 || layout.d == 0)
        return 0;

    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop->first_cell - 2);
    const size_t stride = layout.a + layout.b + layout.c + layout.d;
    const size_t count = be32(fdtprop->length) / (stride * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop->first_cell + i * stride;
        vals[i].a = extract_cells(base, layout.a); 
        vals[i].b = extract_cells(base + layout.a, layout.b);
        vals[i].c = extract_cells(base + layout.a + layout.b, layout.c);
        vals[i].d = extract_cells(base + layout.a + layout.b + layout.c, layout.d);
    }
    return count;
}

