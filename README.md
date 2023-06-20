# A Tiny Flattened Device Tree Parser

Available [Codeberg](https://codeberg.org/r4/smoldtb) and [Github](https://github.com/deanoburrito/smoldtb).

This project is a standalone C port of the device parser from my kernel [Northport](https://github.com/deanoburrito/northport). The original version has few limitations which are addressed here. 

This version does make use of a single larger buffer for storing node data. This can either be allocated by a user provided function, or from a statically allocated buffer inside the program executable. The second method is suitable for environments where dynamic memory allocation might not be available, but it does limit the maximum number of nodes the parser can process.

## Usage
Copy `smoldtb.c` and `smoldtb.h` into your project and you're good to go. No build flags are required.

The function `dtb_init()` must be called to initialize the parser before using it. The parser is reinitialized each time this function is called. It's recommended to only call this once, but if you want to parse another tree at (and destroy the current one) that's supported.

The arguments for `dtb_init(uintptr_t start, dtb_ops ops)` are as follows:

- `uintptr_t start`: The address where the beginning of the flattened device tree can be found. This should be where the FDT header begins and contain the magic number.
- `dtb_ops`: a struct containing a number of function pointers to the library may need to call at runtime. Best practice is to populate all of these.

The `dtb_ops` struct has the following fields:
- `void* (*malloc)(size_t length)`: This function is called to allocate the buffer used internally by the parser. This is called once per call to dtb_init(). It should return a pointer to a region of memory free for use by the library that is at least `length` bytes in length.
- `void* (*free)(void* ptr, size_t length)`: Frees a buffer previously allocated by the above function. Only called when reinitializing the parser.
- `void (*on_error)(const char* why)`: If the library encounters a fatal error and cannot continue it will call this function with a string describing what happened and why.

## API

The API is split into three main groups of functions: `dtb_find_*` functions are used for quickly locating a node by some characteristic, `dtb_get_*` functions are for manually traversing the tree based on a node, and `dtb_read_*` functions allow data to be extracted from nodes and pproperties within the tree.

This API uses integers that are in the native endianness of whereever the code is running. Swapping between big and little endianness is handled internally by the parser.

### dtb_find_*
`dtb_node* dtb_find_compatible(dtb_node* node, const char* str)`: Linearly searches the tree for any nodes with a 'compatible' property that matches this string. Since this property can contain multiple strings, all of them are checked for a given input. The first argument is where to start the search and can be `NULL` to begin at the root of the tree. If a compatible node has been found previously, that node can be used as the starting location for the search and this function will return the *next node* that matches. In the event no nodes have this compatible string, `NULL` is returned.

`dtb_node* dtb_find_phandle(unsigned handle)`: Looks up which node is associated with a given phandle and returns it. If the phandle is unused, `NULL` is returned.

`dtb_node* dtb_find(const char* path)`: Attempts to find a node based on the path provided. The path is a series of unit names (the trailing address part can be exempt) separated by a forward slash `/`, similar to a unix filepath. Returns `NULL` if the node couldn't be located. Properties cannot be looked up this way, you must look up the node and then use `dtb_get_prop()`.

`dtb_node* dtb_find_child(dtb_node* node, const char* name)`: Attempts to find a child of a node with a matching unit name (unit address is exempt from the string comparison). Returns `NULL` if no matching child is present.

### dtb_get_*
`dtb_node* dtb_get_sibling(dtb_node* node)`: Returns this node's sibling (the next child of this node's parent). Note that a node will always have the same sibling. To traverse the tree horizontally this function should be called on the node returned by an earlier `dtb_get_sibling()` call. If a node has no sibling, `NULL` is returned.

`dtb_node* dtb_get_child(dtb_node* node)`: Returns the first child of this node. Subsequent calls to this function will always return the same node, `dtb_get_sibling()` should be called on the child node to get further child nodes. Returns `NULL` if node has no children.
`dtb_node* dtb_get_parent(dtb_node* node)`: Returns this nodes parent node, or `NULL` if node is at the root level.

`dtb_prop* dtb_get_prop(dtb_node* node, const char* name)`: Returns a property of this node with the matching name, or `NULL` if a property isn't found.

`void dtb_stat_node(dtb_node* node, dtb_node_stat* stat)`: Requires `stat` to be a pointer to a pre-allocated struct, and will provide info about `node` in `stat` such as the node's name, number of children and number of properties.

### dtb_read_*

`const char* dtb_read_string(dtb_prop* prop, size_t index)`: String-based properties can contain multiple null-terminated strings, `index` selects which string you want to read. If the index is out of bounds `NULL` is returned, otherwise a pointer to the ASCII-encoded text (as per the Device Tree v0.4 spec) is returned.

`size_t dtb_read_prop_values(dtb_prop* prop, size_t cell_count size_t* vals)`: The `cell_count` argument determines how many cells comprise a single value. This value is specific to the property you're trying to read and you should consult the spec about what to set this to. This function returns the number of values this property would contain for the given `cell_count`. If `vals` is non-null, this function will treat it as an array to write the values into. To use this function it's recommended to call it once with `vals = NULL` to determine how many values are present, then allocate space for the values, and then call the function again with `vals = your_buffer`.

`size_t dtb_read_prop_pairs(dtb_prop* prop, dtb_pair layout, dtb_pair* vals)`: This function is similar to `dtb_read_values()` except it reads pairs of values. The `layout` argument replaces the `cell_count` argument of the previous function: layout.a is the number of cells for the first element of the value and layout.b is the cell count for second element. As above, if `vals` is non-null it is treated as an array to read the pairs of values into.

`size_t dtb_read_prop_triplets(dtb_prop* prop, dtb_triplet layout, dtb_triplets* vals)`: This function is similar to `dtb_read_pairs()` except that it operates on three-element values.

`size_t dtb_read_prop_quads(dtb_prop* prop, dtb_quad layout, dtb_quad* vals)`: Again this function is similar to the above ones, except it operates on 4-element values.

## Changelog

### v0.1.0
- Initial release.
