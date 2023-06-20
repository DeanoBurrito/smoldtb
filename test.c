#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "smoldtb.h"

void dtb_on_error(const char* why)
{
    printf("smol-dtb error: %s\r\n", why);
    exit(1);
}

void* dtb_malloc(size_t length)
{
    return malloc(length);
}

void print_node(dtb_node* node, size_t indent)
{
    const size_t indent_scale = 2;
    if (node == NULL)
        return;

    char indent_buff[indent + 1];
    for (size_t i = 0; i < indent; i++)
        indent_buff[i] = ' ';
    indent_buff[indent] = 0;
    
    dtb_node_stat stat;
    dtb_stat_node(node, &stat);
    printf("%s %s: %lu siblings, %lu children, %lu properties.\r\n", indent_buff, 
        stat.name, stat.sibling_count, stat.child_count, stat.prop_count);

    dtb_node* child = dtb_get_child(node);
    while (child != NULL)
    {
        print_node(child, indent + indent_scale);
        child = dtb_get_sibling(child);
    }
}

void test_file(const char* filename)
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
    ops.on_error = dtb_on_error;
    dtb_init((uintptr_t)buffer, ops);

    dtb_node* root = dtb_find("");
    while (root != NULL)
    {
        print_node(root, 0);
        root = dtb_get_sibling(root);
    }

    munmap(buffer, sb.st_size);
    close(fd);
}

void show_usage()
{
    printf("Usage:\r\n");
    printf("test.elf <filename.dtb>\r\n\r\n");
    printf("The test program will then dump the contents of the FDT,\r\n");
    printf("according to smol-dtb has interpreted it.\r\n");
}

int main(int argc, char** argv)
{
    if (argc == 1)
    {
        show_usage();
        return 0;
    }

    test_file(argv[1]);
    return 0;
}
