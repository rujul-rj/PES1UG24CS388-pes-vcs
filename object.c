// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header: "blob <size>\0" etc.
    const char *type_str = (type == OBJ_BLOB) ? "blob" :
                           (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1; // +1 for '\0'

    // 2. Build full object = header + data
    size_t full_len = (size_t)header_len + len;
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, header_len);
    memcpy(full + header_len, data, len);

    // 3. Compute hash of full object
    compute_hash(full, full_len, id_out);

    // 4. Check for deduplication
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // 5. Create shard directory
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // ignore error if already exists

    // 6. Write to temp file
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full); return -1; }

    ssize_t written = write(fd, full, full_len);
    free(full);
    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // 7. fsync temp file
    fsync(fd);
    close(fd);

    // 8. Atomic rename
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // 9. fsync shard directory to persist rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Read entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    // 3. Verify integrity: recompute hash and compare to filename
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1; // Corrupted object
    }

    // 4. Parse header: find the '\0' separating header from data
    uint8_t *null_pos = memchr(buf, '\0', (size_t)file_size);
    if (!null_pos) { free(buf); return -1; }

    // 5. Parse type from header
    if (strncmp((char *)buf, "blob ", 5) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp((char *)buf, "tree ", 5) == 0)   *type_out = OBJ_TREE;
    else if (strncmp((char *)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // 6. Extract data portion (after '\0')
    uint8_t *data_start = null_pos + 1;
    size_t data_len = (size_t)file_size - (size_t)(data_start - buf);

    *data_out = malloc(data_len);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, data_start, data_len);
    *len_out = data_len;

    free(buf);
    return 0;
}
