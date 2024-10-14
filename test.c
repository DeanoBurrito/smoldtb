#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "smoldtb.h"

size_t total_errors = 0;
size_t total_mallocs = 0;
size_t total_frees = 0;
size_t current_memory_usage = 0;

void dtb_on_error(const char* why)
{
    printf("smoldtb error %zu: %s\r\n", total_errors, why);
    total_errors++;
}

void* dtb_malloc(size_t length)
{
    current_memory_usage += length;
    printf("smoldtb malloc %zu: %zu (%zuB in use)\r\n", total_mallocs++, length, current_memory_usage);
    return malloc(length);
}

void dtb_free(void* ptr, size_t length)
{
    (void)length;

    current_memory_usage -= length;
    printf("smoldtb free %zu: %zu (%zuB in use)\r\n", total_frees++, length, current_memory_usage);
    free(ptr);
}

static const char tree_corner = '\\';
static const char tree_cross = '+';
static const char tree_bar = '|';
static const char tree_space = ' ';

static void print_node(dtb_node* node, char* indent_buff, int indent, bool is_last)
{
    if (node == NULL)
        return;

    printf("%.*s", indent, indent_buff);
    if (is_last)
    {
        printf("%c", tree_corner);
        indent_buff[indent++] = tree_space;
        indent_buff[indent++] = ' ';
    }
    else
    {
        printf("%c", tree_cross);
        indent_buff[indent++] = tree_bar;
        indent_buff[indent++] = ' ';
    }

    dtb_node_stat stat;
    if (dtb_stat_node(node, &stat))
    {
        printf("%s: %zu siblings, %zu children, %zu properties.\r\n", stat.name,
            stat.sibling_count, stat.child_count, stat.prop_count);
    }
    else
        printf("<failed to stat node>\r\n");

    for (size_t i = 0; i < stat.prop_count; i++)
    {
        dtb_prop* prop = dtb_get_prop(node, i);
        if (prop == NULL)
            break;

        dtb_prop_stat pstat;
        if (dtb_stat_prop(prop, &pstat))
            printf("%.*s %s: %zu bytes\r\n", indent, indent_buff, pstat.name, pstat.data_len);
        else
            printf("%.*s <failed to stat property>\r\n", indent, indent_buff);
    }

    dtb_node* child = dtb_get_child(node);
    while (child != NULL)
    {
        dtb_node* next = dtb_get_sibling(child);
        print_node(child, indent_buff, indent, next == NULL);
        child = next;
    }
}

static void print_file(const char* filename)
{
    int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd == -1)
    {
        printf("Could not open output file %s\r\n", filename);
        return;
    }

    const size_t out_len = dtb_finalise_to_buffer(NULL, 0, 0);
    if (ftruncate(fd, out_len) != 0)
    {
        printf("ftruncate failed.\r\n");
        return;
    }

    void* buffer = mmap(NULL, out_len, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (buffer == NULL)
    {
        printf("mmap() failed.\r\n");
        return;
    }

    if (dtb_finalise_to_buffer(buffer, out_len, 0) == SMOLDTB_FINALISE_FAILURE)
        printf("smoltdb reports finalise failure\r\n");

    munmap(buffer, out_len);
    close(fd);

    printf("finalized in-memory dtb to file: %s\r\n", filename);
}

static void display_file(const char* filename, const char* output_filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd == -1)
    {
        printf("Could not open file %s\r\n", filename);
        return;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1)
    {
        printf("Could not stat file %s\r\n", filename);
        return;
    }

    void* buffer = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buffer == NULL)
    {
        printf("mmap() failed\r\n");
        return;
    }

    dtb_ops ops;
    ops.malloc = dtb_malloc;
    ops.free = dtb_free;
    ops.on_error = dtb_on_error;
    dtb_init((uintptr_t)buffer, ops);

    char indent_buffer[256]; //256 levels of indentation should be enough for anyone.

    dtb_node* root = dtb_find("/");
    while (root != NULL)
    {
        print_node(root, indent_buffer, 0, true);
        root = dtb_get_sibling(root);
    }

    if (output_filename != NULL)
        print_file(output_filename);

    munmap(buffer, sb.st_size);
    close(fd);
}

void show_usage()
{
    printf("Usage: \n\
    readfdt <filename.dtb> [output_filename] \n\
    \n\
    This program will parse a flattened device tree/device tree blob and \n\
    output a summary of it's contents. \n\
    If [output_filename] is provided, smoldtb will print it's internal representation \n\
    of the device tree to the specified file in the FDT format. \n\
    The intended purpose of this program is for testing smoldtb library code. \n\
    ");
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3)
    {
        show_usage();
        return 0;
    }

    const char* output_filename = NULL;
    if (argc == 3)
        output_filename = argv[2];

    display_file(argv[1], output_filename);
    return 0;
}

