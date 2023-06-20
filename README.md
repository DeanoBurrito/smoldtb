# A tiny (readonly) Device Tree Blob Parser

Available [Codeberg](https://codeberg.org/r4/smoldtb) and [Github](https://github.com/deanoburrito/smoldtb).

This project is a standalone C port of the device parser from my kernel (https://github.com/deanoburrito/northport). The original version has few limitations which are addressed here. 

This version does allocate a single larger buffer at runtime to hold some data about the tree. There is also an option to use a pre-allocated buffer stored as part of the `.bss` if you want to use this in an environment without dynamic memory allocation.

## Usage
Copy `smoldtb.c` and `smoldtb.h` into your project and you're good to go. No build flags are required.

At runtime a call to `dtb_init(uintptr_t start, dtb_ops ops)` is required before using the rest of the parser. Calling this method multiple times will reinitialize the parser with a new device tree and callback functions. Currently only a single parser instance is supported, so this replace the previously parsed data. The arguments for `dtb_init()` are as follows:

- `uintptr_t start`: The address where the beginning of the flattened device tree can be found. This should be where the FDT header begins and contain the magic number.
- `dtb_ops`: contains a number of function pointers to the library may need to call at runtime. Best practice is to populate all of these.

The `dtb_ops` struct has the following fields:
- `void* (*malloc)(size_t length)`: This function is called to allocate the buffer used internally by the parser. This is called once per call to dtb_init(). It should return a pointer to a region of memory free for use by the library that is at least `length` bytes in length.
- `void (*on_error)(const char* why)`: If the library encounters a fatal error and cannot continue it will call this function with a string describing what happened and why.

## API

The API is split into three main groups of functions: `dtb_find_*` functions are used for quickly locating a node by some characteristic, `dtb_get_*` functions are for manually traversing the tree based on a node, and `dtb_read_*` functions allow data to be extracted from nodes and pproperties within the tree.

## Changelog

### v0.1.0
- Initial release.
