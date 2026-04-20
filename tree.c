// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions: tree_from_index

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ──────────────────────────────────────────────────────────
#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Forward declaration for object_write (in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Recursive helper: build a tree from index entries that share the given prefix.
// 'entries' is the full index entry array, 'count' is total entries.
// 'prefix' is the directory path prefix being processed (e.g., "src/" or "" for root).
// Returns 0 on success, -1 on error.
static int write_tree_level(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;
        size_t prefix_len = strlen(prefix);

        // Must start with prefix
        if (strncmp(path, prefix, prefix_len) != 0) {
            i++;
            continue;
        }

        const char *rel = path + prefix_len; // relative name after prefix

        // Check if this is in a subdirectory
        const char *slash = strchr(rel, '/');
        if (slash != NULL) {
            // It's in a subdirectory — find all entries with same subdir prefix
            size_t dir_name_len = (size_t)(slash - rel);
            char subdir_name[256];
            if (dir_name_len >= sizeof(subdir_name)) { i++; continue; }
            memcpy(subdir_name, rel, dir_name_len);
            subdir_name[dir_name_len] = '\0';

            char subdir_prefix[512];
            snprintf(subdir_prefix, sizeof(subdir_prefix), "%s%s/", prefix, subdir_name);

            // Recursively write the subtree
            ObjectID subtree_id;
            if (write_tree_level(entries, count, subdir_prefix, &subtree_id) != 0) return -1;

            // Add a DIR entry to the current tree
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            strncpy(te->name, subdir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = subtree_id;

            // Skip all entries belonging to this subdir
            while (i < count) {
                if (strncmp(entries[i].path, subdir_prefix, strlen(subdir_prefix)) == 0)
                    i++;
                else
                    break;
            }
        } else {
            // It's a regular file at this level
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = entries[i].hash;
            i++;
        }

        if (tree.count >= MAX_TREE_ENTRIES) break;
    }

    // Serialize and write the tree object
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    memset(&index, 0, sizeof(index));
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) {
        // Empty tree — write an empty tree object
        Tree empty;
        empty.count = 0;
        void *data;
        size_t len;
        if (tree_serialize(&empty, &data, &len) != 0) return -1;
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }
    return write_tree_level(index.entries, index.count, "", id_out);
}
