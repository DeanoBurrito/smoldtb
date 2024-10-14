#include "smoldtb.h"

/* ---- Section: Defines and Structs ---- */

#define FDT_MAGIC 0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

#define FDT_VERSION 17
#define FDT_CELL_SIZE 4
#define ROOT_NODE_STR "\'/\'"

#define SMOLDTB_FOREACH_CONTINUE 0
#define SMOLDTB_FOREACH_ABORT 1

#ifndef SMOLDTB_NO_LOGGING
    #define LOG_ERROR(msg) do { if (state.ops.on_error != NULL) { state.ops.on_error(msg); }} while(false)
#else
    #define LOG_ERROR(msg)
#endif

/* The 'fdt_*' structs represent data layouts taken directly from the device tree
 * specification. In contrast the 'dtb_*' structs are for the parser.
 */
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

/* The tree is represented in horizontal slices, where all child nodes are represented
 * in a singly-linked list. Only a pointer to the first child is stored in the parent, and
 * the list is build using the node->sibling pointer.
 * For reference the pointer building the tree are:
 * - parent: go up one level
 * - sibling: the next node on this level. To access the previous node, access the parent and then
 *            the child pointer and iterate to just before the target.
 * - child: the first child node.
 */
struct dtb_node_t
{
    dtb_node* parent;
    dtb_node* sibling;
    dtb_node* child;
    dtb_prop* props;
    const char* name;
    bool fromMalloc;
};

/* Similar to nodes, properties are stored a singly linked list. */
struct dtb_prop_t
{
    dtb_node* node;
    const char* name;
    void* data;
    dtb_prop* next;
    uint32_t length;
    bool fromMalloc;
    bool dataFromMalloc;
};

/* Info for initializing the global state during init */
struct dtb_init_info
{
    const uint32_t* cells;
    const char* strings;
    size_t cell_count;
};

/* Global parser state */
struct dtb_state
{
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

/* ---- Section: Utility Functions ---- */

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

static size_t string_len(const char* str)
{
    if (str == NULL)
        return 0;

    size_t count = 0;
    while (str[count] != 0)
        count++;
    return count;
}

static void* memcpy(void* dest, const void* src, size_t count)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = src;

    for (size_t i = 0; i < count; i++)
        d[i] = s[i];

    return dest;
}

static bool strings_eq(const char* a, const char* b, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (a[i] == 0 && b[i] == 0)
            return true;
        if (a[i] != b[i])
            return false;
    }

    return true;
}

static size_t string_find_char(const char* str, char target)
{
    size_t i = 0;
    while (str[i] != target)
    {
        if (str[i] == 0)
            return -1ul;
        i++;
    }
    return i;
}

static size_t dtb_align_up(size_t input, size_t alignment)
{
    return ((input + alignment - 1) / alignment) * alignment;
}

static void do_foreach_sibling(dtb_node* begin, int (*action)(dtb_node* node, void* opaque), void* opaque)
{
    if (begin == NULL)
        return;
    if (action == NULL)
        return;

    for (dtb_node* node = begin; node != NULL; node = node->sibling)
    {
        if (action(node, opaque) == SMOLDTB_FOREACH_ABORT)
            return;
    }
}

static void do_foreach_prop(dtb_node* node, int (*action)(dtb_node* node, dtb_prop* prop, void* opaque), void* opaque)
{
    if (node == NULL)
        return;
    if (node->props == NULL)
        return;
    if (action == NULL)
        return;

    for (dtb_prop* prop = node->props; prop != NULL; prop = prop->next)
    {
        if (action(node, prop, opaque) == SMOLDTB_FOREACH_ABORT)
            return;
    }
}

static uintmax_t extract_cells(const uint32_t* cells, size_t count)
{
    uintmax_t value = 0;
    for (size_t i = 0; i < count; i++)
        value |= (uintmax_t)be32(cells[i]) << ((count - 1 - i) * 32);
    return value;
}

static void* try_malloc(size_t count)
{
    if (state.ops.malloc != NULL)
        return state.ops.malloc(count);

    LOG_ERROR("try_malloc() called but state.ops.malloc is NULL");
    return NULL;
}

static void try_free(void* ptr, size_t count)
{
    if (state.ops.free != NULL)
        state.ops.free(ptr, count);

    LOG_ERROR("try_free() called but state.ops.free is NULL");
}

/* ---- Section: Readonly-Mode Private Functions ---- */

static dtb_node* alloc_node()
{
    if (state.node_alloc_head + 1 < state.node_alloc_max)
        return &state.node_buff[state.node_alloc_head++];

    LOG_ERROR("Not enough space for source dtb node.");
    return NULL;
}

static dtb_prop* alloc_prop()
{
    if (state.prop_alloc_head + 1 < state.prop_alloc_max)
        return &state.prop_buff[state.prop_alloc_head++];

    LOG_ERROR("Not enough space for source dtb property.");
    return NULL;
}

static void free_buffers()
{
#ifndef SMOLDTB_STATIC_BUFFER_SIZE
    size_t buff_size = state.node_alloc_max * sizeof(dtb_node);
    buff_size += state.prop_alloc_max * sizeof(dtb_prop);
    buff_size += state.node_alloc_max * sizeof(void*);

    try_free(state.node_buff, buff_size);
    state.node_buff = NULL;
    state.prop_buff = NULL;
    state.handle_lookup = NULL;
#endif

    state.node_alloc_head = state.node_alloc_max = 0;
    state.prop_alloc_head = state.prop_alloc_max = 0;
}

static bool alloc_buffers(struct dtb_init_info* init_info)
{
    state.node_alloc_max = 0;
    state.prop_alloc_max = 0;
    for (size_t i = 0; i < init_info->cell_count; i++)
    {
        if (be32(init_info->cells[i]) == FDT_BEGIN_NODE)
            state.node_alloc_max++;
        else if (be32(init_info->cells[i]) == FDT_PROP)
            state.prop_alloc_max++;
    }

    size_t total_size = state.node_alloc_max * sizeof(dtb_node);
    total_size += state.prop_alloc_max * sizeof(dtb_prop);
    total_size += state.node_alloc_max * sizeof(void*); //we assume the worst case and that each node has a phandle prop

#ifdef SMOLDTB_STATIC_BUFFER_SIZE
    if (total_size >= SMOLDTB_STATIC_BUFFER_SIZE)
    {
        LOG_ERROR("Too much data for statically allocated buffer.");
        return false;
    }
    uint8_t* buffer = big_buff;
#else
    uint8_t* buffer = try_malloc(total_size);
    if (buffer == NULL)
    {
        LOG_ERROR("Failed to allocate big buffer.");
        return false;
    }
#endif

    for (size_t i = 0; i < total_size; i++)
        buffer[i] = 0;

    state.node_buff = (dtb_node*)buffer;
    state.node_alloc_head = 0;
    state.prop_buff = (dtb_prop*)&state.node_buff[state.node_alloc_max];
    state.prop_alloc_head = 0;
    state.handle_lookup = (dtb_node**)&state.prop_buff[state.prop_alloc_max];

    return true;
}

/* This runs on every new property found, and handles some special cases for us. */
static void check_for_special_prop(dtb_node* node, dtb_prop* prop)
{
    const char name0 = prop->name[0];
    if (name0 != 'p' || name0 != 'l')
        return; //short circuit to save processing

    const size_t name_len = string_len(prop->name);

    const char str_phandle[] = "phandle";
    const size_t len_phandle = sizeof(str_phandle) - 1;
    if (name_len == len_phandle && strings_eq(prop->name, str_phandle, name_len))
    {
        size_t handle;
        dtb_read_prop_1(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }

    const char str_lhandle[] = "linux,phandle";
    const size_t len_lhandle = sizeof(str_lhandle) - 1;
    if (name_len == len_lhandle && strings_eq(prop->name, str_lhandle, name_len))
    {
        size_t handle;
        dtb_read_prop_1(prop, 1, &handle);
        state.handle_lookup[handle] = node;
        return;
    }
}

static dtb_prop* parse_prop(struct dtb_init_info* init_info, size_t* offset)
{
    if (be32(init_info->cells[*offset]) != FDT_PROP)
        return NULL;

    (*offset)++;
    dtb_prop* prop = alloc_prop();
    if (prop == NULL)
    {
        LOG_ERROR("Property allocation failed");
        return NULL;
    }

    const struct fdt_property* fdtprop = (struct fdt_property*)(init_info->cells + *offset);
    prop->name = (const char*)(init_info->strings + be32(fdtprop->name_offset));
    prop->data = (void*)(init_info->cells + *offset + 2);
    prop->length = be32(fdtprop->length);
    prop->fromMalloc = false;
    prop->dataFromMalloc = false;
    (*offset) += (dtb_align_up(be32(fdtprop->length), 4) / 4) + 2;
    
    return prop;
}

static dtb_node* parse_node(struct dtb_init_info* init_info, size_t* offset)
{
    if (be32(init_info->cells[*offset]) != FDT_BEGIN_NODE)
        return NULL;

    dtb_node* node = alloc_node(); 
    if (node == NULL)
    {
        LOG_ERROR("Node allocation failed");
        return NULL;
    }
    node->name = (const char*)(init_info->cells + (*offset) + 1);
    node->fromMalloc = false;

    const size_t name_len = string_len(node->name);
    if (name_len == 0)
        node->name = NULL;
    *offset += (dtb_align_up(name_len + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE) + 1;

    while (*offset < init_info->cell_count)
    {
        const uint32_t test = be32(init_info->cells[*offset]);
        if (test == FDT_END_NODE)
        {
            (*offset)++;
            return node;
        }
        else if (test == FDT_BEGIN_NODE)
        {
            dtb_node* child = parse_node(init_info, offset);
            if (child == NULL)
                continue;

            child->sibling = node->child;
            node->child = child;
            child->parent = node;
        }
        else if (test == FDT_PROP)
        {
            dtb_prop* prop = parse_prop(init_info, offset);
            if (prop == NULL)
                continue;

            prop->next = node->props;
            prop->node = node;
            node->props = prop;
            check_for_special_prop(node, prop);
    }
        else
            (*offset)++;
    }

    LOG_ERROR("Node is missing terminating tag.");
    return NULL;
}

/* ---- Section: Readonly-Mode Public API ---- */

size_t dtb_query_total_size(uintptr_t fdt_start)
{
    if (fdt_start == 0)
        return 0;

    struct fdt_header* header = (struct fdt_header*)fdt_start;
    if (be32(header->magic) != FDT_MAGIC)
        return 0;

    return be32(header->total_size);
}

bool dtb_init(uintptr_t start, dtb_ops ops)
{
    state.ops = ops;

#if !defined(SMOLDTB_STATIC_BUFFER_SIZE)
    if (state.ops.malloc == NULL)
    {
        LOG_ERROR("smoldtb has been compiled without an internal static buffer, but not passed a malloc() function.");
        return false;
    }
#endif

    struct dtb_init_info init_info;
    if (start == SMOLDTB_INIT_EMPTY_TREE)
    {
        state.root = NULL;
        return true;
    }

    struct fdt_header* header = (struct fdt_header*)start;
    if (be32(header->magic) != FDT_MAGIC)
    {
        LOG_ERROR("FDT has incorrect magic number.");
        return false;
    }

    init_info.cells = (const uint32_t*)(start + be32(header->offset_structs));
    init_info.cell_count = be32(header->size_structs) / sizeof(uint32_t);
    init_info.strings = (const char*)(start + be32(header->offset_strings));

    if (state.node_buff != NULL)
        free_buffers();
    if (!alloc_buffers(&init_info))
    {
        LOG_ERROR("failed to allocate readonly buffer");
        return false;
    }

    for (size_t i = 0; i < init_info.cell_count; i++)
    {
        if (be32(init_info.cells[i]) != FDT_BEGIN_NODE)
            continue;

        dtb_node* sub_root = parse_node(&init_info, &i);
        if (sub_root == NULL)
            continue;
        sub_root->sibling = state.root;
        state.root = sub_root;
    }

    return true;
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
        if (dtb_is_compatible(node, str))
            return node;
    }

    return NULL;
} 

dtb_node* dtb_find_phandle(unsigned handle)
{
    if (handle < state.node_alloc_max)
        return state.handle_lookup[handle];

    return NULL;
}

static dtb_node* find_child_internal(dtb_node* start, const char* name, size_t name_bounds)
{
    dtb_node* scan = start->child;
    while (scan != NULL)
    {
        size_t child_name_len = string_find_char(scan->name, '@');
        if (child_name_len == -1ul)
            child_name_len = string_len(scan->name);

        if (child_name_len == name_bounds && strings_eq(scan->name, name, name_bounds))
            return scan;

        scan = scan->sibling;
    }

    return NULL;
}

dtb_node* dtb_find(const char* name)
{
    size_t seg_len;
    dtb_node* scan = state.root;
    while (scan != NULL)
    {
        while (name[0] == '/')
            name++;

        seg_len = string_find_char(name, '/');
        if (seg_len == -1ul)
            seg_len = string_len(name);
        if (seg_len == 0)
            return scan;

        scan = find_child_internal(scan, name, seg_len);
        name += seg_len;
    }

    return NULL;
} 

dtb_node* dtb_find_child(dtb_node* start, const char* name)
{
    if (start == NULL)
        return NULL;

    return find_child_internal(start, name, string_len(name));
}

dtb_prop* dtb_find_prop(dtb_node* node, const char* name)
{
    if (node == NULL)
        return NULL;

    const size_t name_len = string_len(name);
    dtb_prop* prop = node->props;
    while (prop)
    {
        const size_t prop_name_len = string_len(prop->name);
        if (prop_name_len == name_len && strings_eq(prop->name, name, prop_name_len))
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

static size_t get_cells_helper(dtb_node* node, const char* prop_name, size_t orDefault)
{
    if (node == NULL)
        return orDefault;

    dtb_prop* prop = dtb_find_prop(node, prop_name);
    if (prop == NULL)
        return orDefault;

    uintmax_t ret_value;
    if (dtb_read_prop_1(prop, 1, &ret_value) == 1)
        return ret_value;
    return orDefault;
}

size_t dtb_get_addr_cells_of(dtb_node* node)
{
    return get_cells_helper(node, "#address-cells", 2);
}

size_t dtb_get_size_cells_of(dtb_node* node)
{
    return get_cells_helper(node, "#size-cells", 1);
}

size_t dtb_get_addr_cells_for(dtb_node* node)
{
    if (node == NULL)
        return 2;
    return get_cells_helper(node->parent, "#address-cells", 2);
}

size_t dtb_get_size_cells_for(dtb_node* node)
{
    if (node == NULL)
        return 1;
    return get_cells_helper(node->parent, "#size-cells", 1);
}

bool dtb_is_compatible(dtb_node* node, const char* str)
{
    if (node == NULL || str == NULL)
        return false;

    dtb_prop* compat_prop = dtb_find_prop(node, "compatible");
    if (compat_prop == NULL)
        return false;

    const size_t str_len = string_len(str);
    for (size_t i = 0; ; i++)
    {
        const char* check_str = dtb_read_prop_string(compat_prop, i);
        if (check_str == NULL)
            return false;
        if (strings_eq(check_str, str, str_len))
            return true;
    }
}

bool dtb_stat_node(dtb_node* node, dtb_node_stat* stat)
{
    if (node == NULL || stat == NULL)
        return false;

    stat->name = node->name;
    if (node == state.root)
        stat->name = ROOT_NODE_STR;

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

    return true;
}

bool dtb_stat_prop(dtb_prop* prop, dtb_prop_stat* stat)
{
    if (prop == NULL || stat == NULL)
        return false;

    stat->name = prop->name;
    stat->data = prop->data;
    stat->data_len = prop->length;
    return true;
}

const char* dtb_read_prop_string(dtb_prop* prop, size_t index)
{
    if (prop == NULL)
        return NULL;
    
    const uint8_t* name = (const uint8_t*)prop->data;
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

size_t dtb_read_prop_1(dtb_prop* prop, size_t cell_count, uintmax_t* vals)
{
    if (prop == NULL || cell_count == 0)
        return 0;
    
    const uint32_t* prop_cells = prop->data;
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop_cells - 2);
    const size_t count = be32(fdtprop->length) / (cell_count * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop_cells + i * cell_count;
        vals[i] = extract_cells(base, cell_count);
    }

    return count;
}

size_t dtb_read_prop_2(dtb_prop* prop, dtb_pair layout, dtb_pair* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0)
        return 0;
    
    const uint32_t* prop_cells = prop->data;
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop_cells - 2);
    const size_t count = be32(fdtprop->length) / ((layout.a + layout.b) * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop_cells + i * (layout.a + layout.b);
        vals[i].a = extract_cells(base, layout.a);
        vals[i].b = extract_cells(base + layout.a, layout.b);
    }
    return count;
}

size_t dtb_read_prop_3(dtb_prop* prop, dtb_triplet layout, dtb_triplet* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0 || layout.c == 0)
        return 0;

    const uint32_t* prop_cells = prop->data;
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop_cells - 2);
    const size_t stride = layout.a + layout.b + layout.c;
    const size_t count = be32(fdtprop->length) / (stride * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop_cells + i * stride;
        vals[i].a = extract_cells(base, layout.a);
        vals[i].b = extract_cells(base + layout.a, layout.b);
        vals[i].c = extract_cells(base + layout.a + layout.b, layout.c);
    }
    return count;
}

size_t dtb_read_prop_4(dtb_prop* prop, dtb_quad layout, dtb_quad* vals)
{
    if (prop == NULL || layout.a == 0 || layout.b == 0 || layout.c == 0 || layout.d == 0)
        return 0;

    const uint32_t* prop_cells = prop->data;
    const struct fdt_property* fdtprop = (const struct fdt_property*)(prop_cells - 2);
    const size_t stride = layout.a + layout.b + layout.c + layout.d;
    const size_t count = be32(fdtprop->length) / (stride * FDT_CELL_SIZE);
    if (vals == NULL)
        return count;

    for (size_t i = 0; i < count; i++)
    {
        const uint32_t* base = prop_cells + i * stride;
        vals[i].a = extract_cells(base, layout.a); 
        vals[i].b = extract_cells(base + layout.a, layout.b);
        vals[i].c = extract_cells(base + layout.a + layout.b, layout.c);
        vals[i].d = extract_cells(base + layout.a + layout.b + layout.c, layout.d);
    }
    return count;
}

#ifdef SMOLDTB_ENABLE_WRITE_API
/* ---- Section: Writable-Mode Private Functions ---- */

struct finalise_data
{
    uint32_t* struct_buf;
    char* string_buf;
    size_t struct_ptr;
    size_t string_ptr;
    size_t struct_buf_size;
    size_t string_buf_size;
    bool print_success;
};

struct name_collision_check
{
    const char* name;
    size_t name_len;
    bool collision;
};

static int destroy_props(dtb_node* node, dtb_prop* prop, void* opaque)
{
    (void)node;
    (void)opaque;

    if (prop->dataFromMalloc)
        try_free(prop->data, prop->length);
    if (prop->fromMalloc)
        try_free(prop, sizeof(dtb_prop));

    return SMOLDTB_FOREACH_CONTINUE;
}

static void destroy_dead_node(dtb_node* node)
{
    if (node == NULL || node->parent != NULL)
        return;

    while (node->child != NULL)
    {
        dtb_node* deletee = node->child;
        node->child = node->child->sibling;

        deletee->parent = NULL;
        destroy_dead_node(deletee);
    }

    do_foreach_prop(node, destroy_props, NULL);
    if (node->fromMalloc)
        try_free(node, sizeof(dtb_node));
}

static int init_finalise_data_prop(dtb_node* node, dtb_prop* prop, void* opaque)
{
    (void)node;
    if (node == NULL || prop == NULL)
        return SMOLDTB_FOREACH_CONTINUE;

    struct finalise_data* data = opaque;
    data->struct_buf_size += 3; /* +1 for FDT_PROP token, +2 for prop description struct */
    data->struct_buf_size += dtb_align_up(prop->length, FDT_CELL_SIZE) / FDT_CELL_SIZE;
    data->string_buf_size += string_len(prop->name) + 1; /* +1 for null terminator */

    return SMOLDTB_FOREACH_CONTINUE;
}

static int init_finalise_data(dtb_node* node, void* opaque)
{
    if (node == NULL)
        return SMOLDTB_FOREACH_CONTINUE;

    struct finalise_data* data = opaque;
    data->struct_buf_size += 2; /* +1 for BEGIN_NODE token, +1 for END_NODE token */
    data->struct_buf_size += dtb_align_up(string_len(node->name) + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE; /* +1 for null terminator */

    do_foreach_prop(node, init_finalise_data_prop, opaque);
    do_foreach_sibling(node->child, init_finalise_data, opaque);

    return SMOLDTB_FOREACH_CONTINUE;
}

static int print_prop(dtb_node* node, dtb_prop* prop, void* opaque)
{
    (void)node;
    struct finalise_data* data = opaque;

    const uint32_t name_offset = data->string_ptr;
    const size_t name_len = string_len(prop->name);
    if (data->string_ptr + name_len + 1 > data->string_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }

    memcpy(data->string_buf + data->string_ptr, prop->name, name_len);
    data->string_buf[data->string_ptr + name_len] = 0;
    data->string_ptr += name_len + 1; /* +1 for null terminator */

    const size_t data_cells = dtb_align_up(prop->length, FDT_CELL_SIZE) / FDT_CELL_SIZE;
    if (data->struct_ptr + 3 + data_cells > data->struct_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }

    data->struct_buf[data->struct_ptr++] = be32(FDT_PROP);
    data->struct_buf[data->struct_ptr++] = be32((uint32_t)prop->length);
    data->struct_buf[data->struct_ptr++] = be32(name_offset);

    uint32_t* prop_cells = prop->data;
    for (size_t i = 0; i < data_cells; i++)
        data->struct_buf[data->struct_ptr++] = prop_cells[i];

    return SMOLDTB_FOREACH_CONTINUE;
}

static int print_node(dtb_node* node, void* opaque)
{
    struct finalise_data* data = opaque;
    const size_t name_len = string_len(node->name);
    const size_t name_cells = dtb_align_up(name_len + 1, FDT_CELL_SIZE) / FDT_CELL_SIZE;

    if (data->struct_ptr + 1 + name_cells > data->struct_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }

    data->struct_buf[data->struct_ptr++] = be32(FDT_BEGIN_NODE);

    uint8_t* name_buf = (uint8_t*)(data->struct_buf + data->struct_ptr);
    memcpy(name_buf, node->name, name_len);
    name_buf[name_len] = 0;
    data->struct_ptr += name_cells;

    do_foreach_prop(node, print_prop, opaque);
    if (!data->print_success)
        return SMOLDTB_FOREACH_ABORT;
    do_foreach_sibling(node->child, print_node, opaque);
    if (!data->print_success)
        return SMOLDTB_FOREACH_ABORT;

    if (data->struct_ptr + 1 > data->struct_buf_size) /* bounds check */
    {
        data->print_success = false;
        return SMOLDTB_FOREACH_ABORT;
    }
    data->struct_buf[data->struct_ptr++] = be32(FDT_END_NODE);

    return SMOLDTB_FOREACH_CONTINUE;
}

static int check_sibling_name_collisions(dtb_node* node, void* opaque)
{
    struct name_collision_check* check = opaque;

    if (!strings_eq(node->name, check->name, check->name_len))
        return SMOLDTB_FOREACH_CONTINUE;

    check->collision = true;
    return SMOLDTB_FOREACH_ABORT;
}

static int check_prop_name_collisions(dtb_node* node, dtb_prop* prop, void* opaque)
{
    (void)node;
    struct name_collision_check* check = opaque;

    if (!strings_eq(prop->name, check->name, check->name_len))
        return SMOLDTB_FOREACH_CONTINUE;

    check->collision = true;
    return SMOLDTB_FOREACH_ABORT;
}

/* ---- Section: Writable-Mode Public API ---- */

size_t dtb_finalise_to_buffer(void* buffer, size_t buffer_size, uint32_t boot_cpu_id)
{
    struct finalise_data final_data;
    final_data.struct_buf_size = 0;
    final_data.string_buf_size = 1; /* we'll use 1 byte for the empty string */

    do_foreach_sibling(state.root, init_finalise_data, &final_data);
    const size_t reserved_block_size = 2 * sizeof(uint64_t);
    const size_t struct_buf_bytes = final_data.struct_buf_size * FDT_CELL_SIZE;
    const size_t total_bytes = final_data.string_buf_size + struct_buf_bytes + 
        sizeof(struct fdt_header) + reserved_block_size;

    if (buffer == NULL || total_bytes < buffer_size)
        return total_bytes;
    if ((uintptr_t)buffer & 0b11)
        return SMOLDTB_FINALISE_FAILURE; /* check buffer is aligned to a 32-bit boundary */

    struct fdt_header* header = (struct fdt_header*)buffer;
    header->magic = be32(FDT_MAGIC);
    header->total_size = be32(total_bytes);
    header->offset_structs = be32(sizeof(struct fdt_header) + 16); /* size of reserved block */
    header->offset_strings = be32(be32(header->offset_structs) + struct_buf_bytes);
    header->offset_memmap_rsvd = be32(sizeof(struct fdt_header));
    header->version = be32(FDT_VERSION);
    header->last_comp_version = be32(16); /* as per spec, this field must be 16. */
    header->boot_cpu_id = be32(boot_cpu_id);
    header->size_strings = be32(final_data.string_buf_size);
    header->size_structs = be32(struct_buf_bytes);

    /* Apparently the great minds behind the device tree spec were able to think far enough
     * ahead to include size fields for the string and structure blocks, but not
     * the reserved memory block. The end of this block is indicated by an entry filled
     * with zeroes, because a size field would be too easy.
     * So even though we dont use this block, we must include a single entry for it. */
    uint64_t* reserved_block = (uint64_t*)(header + 1);
    reserved_block[0] = 0;
    reserved_block[1] = 0;

    final_data.struct_buf = (uint32_t*)((uintptr_t)buffer + be32(header->offset_structs));
    final_data.string_buf = (char*)((uintptr_t)buffer + be32(header->offset_strings));
    final_data.struct_ptr = 0;
    final_data.string_ptr = 1;
    final_data.string_buf[0] = 0;

    final_data.print_success = true;
    do_foreach_sibling(state.root, print_node, &final_data);
    return final_data.print_success ? total_bytes : SMOLDTB_FINALISE_FAILURE;
}

dtb_node* dtb_find_or_create_node(const char* path)
{
    if (path == NULL)
        return NULL;

    size_t seg_len;
    dtb_node* scan = state.root;
    while (scan != NULL)
    {
        while (path[0] == '/')
            path++;

        seg_len = string_find_char(path, '/');
        if (seg_len == -1ul)
            seg_len = string_len(path);
        if (seg_len == 0)
            return scan;

        dtb_node* next = find_child_internal(scan, path, seg_len);
        if (next == NULL)
            next = dtb_create_child(scan, path);
        scan = next;
    }

    return NULL;
}

dtb_prop* dtb_find_or_create_prop(dtb_node* node, const char* name)
{
    if (node == NULL || name == NULL)
        return NULL;

    dtb_prop* prop = dtb_find_prop(node, name);
    if (prop == NULL)
        prop = dtb_create_prop(node, name);
    return prop;
}

dtb_node* dtb_create_sibling(dtb_node* node, const char* name)
{
    if (node == NULL || name == NULL || node->parent == NULL) /* creating siblings of root node is disallowed */
        return NULL;

    struct name_collision_check check_data;
    check_data.collision = false;
    check_data.name = name;
    check_data.name_len = string_len(name);
    if (string_find_char(name, '/') < check_data.name_len)
        check_data.name_len = string_find_char(name, '/');

    do_foreach_sibling(node->parent->child, check_sibling_name_collisions, &check_data);
    if (check_data.collision)
    {
        LOG_ERROR("Failed to create node with duplicate name.");
        return NULL;
    }

    const size_t name_len = string_len(name);
    char* name_buf = try_malloc(name_len + 1);
    if (name_buf == NULL)
        return NULL;
    memcpy(name_buf, name, name_len);

    dtb_node* sibling = try_malloc(sizeof(dtb_node));
    if (sibling == NULL)
    {
        LOG_ERROR("Failed to allocate node for sibling.");
        return NULL;
    }

    sibling->name = name_buf;
    sibling->parent = node->parent;

    sibling->fromMalloc = true;
    sibling->sibling = node->sibling;
    node->sibling = sibling;
    return sibling;
}

dtb_node* dtb_create_child(dtb_node* node, const char* name)
{
    if (node == NULL || name == NULL)
        return NULL;

    struct name_collision_check check_data;
    check_data.collision = false;
    check_data.name = name;
    check_data.name_len = string_len(name);
    if (string_find_char(name, '/') < check_data.name_len)
        check_data.name_len = string_find_char(name, '/');

    do_foreach_sibling(node->child, check_sibling_name_collisions, &check_data);
    if (check_data.collision)
    {
        LOG_ERROR("Failed to create node with duplicate name.");
        return NULL;
    }

    const size_t name_len = string_len(name);
    char* name_buf = try_malloc(name_len + 1);
    if (name_buf == NULL)
        return NULL;
    memcpy(name_buf, name, name_len);

    dtb_node* child = try_malloc(sizeof(dtb_node));
    if (child == NULL)
    {
        LOG_ERROR("Failed to allocate node for child.");
        return NULL;
    }

    child->parent = node;
    child->name = name_buf;
    child->fromMalloc = true;
    child->sibling = node->child;
    node->child = child;
    return child;
}

dtb_prop* dtb_create_prop(dtb_node* node, const char* name)
{
    if (node == NULL || name == NULL)
        return NULL;

    const size_t name_len = string_len(name);
    struct name_collision_check check_data;
    check_data.collision = false;
    check_data.name = name;
    check_data.name_len = name_len;

    do_foreach_prop(node, check_prop_name_collisions, &check_data);
    if (check_data.collision)
    {
        LOG_ERROR("Failed to create prop with duplicate name.");
        return NULL;
    }

    char* name_buf = try_malloc(name_len + 1);
    if (name_buf == NULL)
        return NULL;
    memcpy(name_buf, name, name_len);

    dtb_prop* prop = try_malloc(sizeof(dtb_prop));
    if (prop == NULL)
    {
        LOG_ERROR("Failed to allocate property");
        return NULL;
    }

    prop->length = 0;
    prop->data = NULL;
    prop->name = name_buf;
    prop->fromMalloc = true;
    prop->dataFromMalloc = false;
    prop->next = node->props;
    prop->node = node;
    node->props = prop;
    return prop;
}

bool dtb_destroy_node(dtb_node* node)
{
    if (node == NULL)
        return false;

    if (node->parent != NULL) /* break linkage in parents list of child nodes */
    {
        dtb_node* scan = node->parent->child;
        if (scan == node)
        {
            scan = NULL;
            node->parent->child = node->sibling;
        }

        while (scan != NULL)
        {
            if (scan->name == NULL)
            {
                LOG_ERROR("Corrupt internal state: node not in parent's child list.");
                return false;
            }
            if (scan->sibling != node)
            {
                scan = scan->sibling;
                continue;
            }

            scan->sibling = node->sibling;
            break;
        }
    }

    node->parent = NULL;
    destroy_dead_node(node);
    return true;
}

bool dtb_destroy_prop(dtb_prop* prop)
{
    if (prop == NULL)
        return false;

    dtb_prop* scan = prop->node->props;
    if (scan == prop)
    {
        scan = NULL;
        prop->node->props = prop->next;
    }

    while (scan != NULL)
    {
        if (scan->next == NULL)
            return false;
        if (scan->next != prop)
        {
            scan = scan->next;
            continue;
        }

        scan->next = prop->next;
        break;
    }

    if (prop->dataFromMalloc)
        try_free(prop->data, prop->length);
    if (prop->fromMalloc)
        try_free(prop, sizeof(dtb_prop));

    return true;
}

static bool ensure_prop_has_buffer_for(dtb_prop* prop, size_t buf_size)
{
    if (prop == NULL)
        return false;

    if (prop->dataFromMalloc && buf_size <= prop->length)
        return true;

    void* new_data = try_malloc(buf_size);
    if (new_data == NULL)
        return false;
    if (prop->dataFromMalloc)
        try_free(prop->data, prop->length);

    prop->data = new_data;
    prop->length = buf_size;
    return true;
}

bool dtb_write_prop_string(dtb_prop* prop, const char* str, size_t str_len)
{
    if (prop == NULL)
        return false;

    if (!ensure_prop_has_buffer_for(prop, str_len))
        return false;

    memcpy(prop->data, str, str_len);
    return true;
}

static bool copy_prop_buffer(dtb_prop* prop, size_t buf_cells, const uint32_t* buf)
{
    if (prop == NULL)
        return false;
    if (buf == NULL && buf_cells != 0)
        return false;

    if (!ensure_prop_has_buffer_for(prop, buf_cells * FDT_CELL_SIZE))
        return false;

    uint32_t* dest_cells = prop->data;
    for (size_t i = 0; i < buf_cells; i++)
        dest_cells[i] = be32(buf[i]);

    return true;
}

bool dtb_write_prop_1(dtb_prop* prop, size_t count, size_t cell_count, const uintmax_t* vals)
{
    const size_t buf_cells = count * cell_count;
    return copy_prop_buffer(prop, buf_cells, (const uint32_t*)vals);
}

bool dtb_write_prop_2(dtb_prop* prop, size_t count, dtb_pair layout, const dtb_pair* vals)
{
    const size_t buf_cells = count * (layout.a + layout.b);
    return copy_prop_buffer(prop, buf_cells, (const uint32_t*)vals);
}

bool dtb_write_prop_3(dtb_prop* prop, size_t count, dtb_triplet layout, const dtb_triplet* vals)
{
    const size_t buf_cells = count * (layout.a + layout.b + layout.c);
    return copy_prop_buffer(prop, buf_cells, (const uint32_t*)vals);
}

bool dtb_write_prop_4(dtb_prop* prop, size_t count, dtb_quad layout, const dtb_quad* vals)
{
    const size_t buf_cells = count * (layout.a + layout.b + layout.c + layout.d);
    return copy_prop_buffer(prop, buf_cells, (const uint32_t*)vals);
}
#endif /* SMOLDTB_ENABLE_WRITE_API */
