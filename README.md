# A Tiny Flattened Device Tree Parser
Available [Codeberg](https://codeberg.org/r4/smoldtb) and [Github](https://github.com/deanoburrito/smoldtb).

This project is a standalone C port of the device parser from my kernel [Northport](https://github.com/deanoburrito/northport). The original version has few limitations which are addressed here. 

This version does make use of a single larger buffer for storing node data. This can either be allocated by a user provided function, or from a statically allocated buffer inside the program executable. The second method is suitable for environments where dynamic memory allocation might not be available, but it does limit the maximum number of nodes the parser can process.

## Usage
Copy `smoldtb.c` and `smoldtb.h` into your project and you're good to go. No additional compiler flags are required. 

The parser must be initialized before using it by calling `dtb_init()`. This function is the only time memory allocation/deallocation happens. You can call this multiple times, and it will re-initialize itself based on the new data device blob. Re-initializing the parser will destroy the previous parse data, so it effectively operates like a singleton.

The parser assumes that the DTB is always available at it's original address (the one given to `dtb_init()`) at runtime. If the DTB is moved in memory you can re-initialize the parser with the new address.
The arguments for `dtb_init(uintptr_t start, dtb_ops ops)` are as follows:

- `uintptr_t start`: The address where the beginning of the flattened device tree can be found. This should be where the FDT header begins and contain the magic number.
- `dtb_ops`: a struct containing a number of function pointers to the library may need to call at runtime. Best practice is to populate all of these.

The `dtb_ops` struct has the following fields:
- `void* (*malloc)(size_t length)`: This function is called to allocate the buffer used internally by the parser. This is called once per call to `dtb_init()`. It should return a pointer to a region of memory free for use by the library that is at least `length` bytes in length. This function (and `ops.free()`) are both unused if using a statically allocated buffer.
- `void* (*free)(void* ptr, size_t length)`: Frees a buffer previously allocated by the above function. Only called when reinitializing the parser.
- `void (*on_error)(const char* why)`: If the library encounters a fatal error and cannot continue it will call this function with a string describing what happened and why.

### Use Without Malloc/Free
Define `SMOLDTB_STATIC_BUFFER_SIZE=your_buffer_size` when compiling `smoldtb.c` and the parser will only allocate from a single buffer, typically stored in the program's `.bss` section. When compiled with this option `ops.free()` and `ops.malloc()` are never called.

In the event of parsing a DTB that contains too many nodes and/or properties for the static buffer, the parser will exit during `dtb_init()` (with a call to `ops.on_error()` if populated).

### Concurrency
Not an advertised feature, but all (except `dtb_init()`) API functions will only read the internal structures and DTB. To be safe you may want to use a reader-writer lock around the library (only calls to `dtb_init()` will need the writer lock). If you only plan to initialize the parser once, even this is not necessary.

## Changelog
### v0.2.0
- Renamed `dtb_get_prop` to `dtb_find_prop`.
- Added a new `dtb_get_prop` which returns a property based on an index, rather than name.
- Added support for using a static buffer instead of malloc/free.
- Better documentation.

### v0.1.0
- Initial release.
