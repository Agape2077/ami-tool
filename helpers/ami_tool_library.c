#include "ami_tool_i.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <nfc/nfc_device.h>

#define LIBRARY_DIR AMI_TOOL_SAVED_FOLDER "/.ami_tool"
#define LIBRARY_INDEX_PATH LIBRARY_DIR "/idx"
#define LIBRARY_IDS_PATH LIBRARY_DIR "/ids"
#define LIBRARY_META_PATH LIBRARY_DIR "/meta"
#define LIBRARY_FAV_PATH LIBRARY_DIR "/fav"
#define LIBRARY_FAV_MAX (64U)

#define LIBRARY_SCAN_DEPTH (3U)
#define LIBRARY_READ_BUFFER (256U)
#define LIBRARY_BATCH (24U)

static AmiToolLibraryItem page_items[AMI_TOOL_LIBRARY_PAGE];
static size_t page_count = 0;
static size_t total_count = 0;

/* --- background indexing state --- */
static FuriThread* index_thread = NULL;
static volatile bool index_cancel = false;
static volatile bool index_active = false;
static volatile size_t index_done = 0;
static volatile size_t index_total = 0;
static volatile bool index_finished = false;

static void library_write_meta(AmiToolApp* app);

static bool library_is_nfc_name(const char* name) {
    if(!name) return false;
    size_t len = strlen(name);
    if(len <= 4) return false;
    const char* ext = name + len - 4;
    return ext[0] == '.' &&
           tolower((unsigned char)ext[1]) == 'n' &&
           tolower((unsigned char)ext[2]) == 'f' &&
           tolower((unsigned char)ext[3]) == 'c';
}

static void library_copy_str(char* dest, size_t dest_size, const char* src, size_t len) {
    if(!dest || dest_size == 0) return;
    if(len >= dest_size) len = dest_size - 1;
    if(len > 0 && src) memcpy(dest, src, len);
    dest[len] = '\0';
}

static void library_build_abs(char* out, size_t out_size, const char* rel) {
    size_t prefix = strlen(AMI_TOOL_SAVED_FOLDER);
    size_t rel_len = strlen(rel);
    if(prefix + 1 + rel_len + 1 > out_size) {
        out[0] = '\0';
        return;
    }
    memcpy(out, AMI_TOOL_SAVED_FOLDER, prefix);
    out[prefix] = '/';
    memcpy(out + prefix + 1, rel, rel_len + 1);
}

size_t ami_tool_library_page_count(void) {
    return page_count;
}

size_t ami_tool_library_total(void) {
    return total_count;
}

const AmiToolLibraryItem* ami_tool_library_page_item(size_t index) {
    if(index >= page_count) return NULL;
    return &page_items[index];
}

/* --- metadata lookup: one amiibo.dat pass per batch --- */

static bool library_line_matches_id(const char* line, const char* id) {
    size_t i = 0;
    while(id[i]) {
        char a = line[i];
        char b = id[i];
        if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if(a != b) return false;
        i++;
    }
    return line[i] == ':';
}

static void library_take_field(char* dest, size_t dest_size, const char* src, size_t len) {
    if(len >= dest_size) len = dest_size - 1;
    if(len > 0) memcpy(dest, src, len);
    dest[len] = '\0';
}

static void library_resolve_batch(
    Storage* storage,
    const char* const* ids,
    char* names,
    size_t name_stride,
    char* series,
    size_t series_stride,
    size_t count) {
    if(count == 0) return;

    File* file = storage_file_alloc(storage);
    if(!file) return;

    if(storage_file_open(file, APP_ASSETS_PATH("amiibo.dat"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t buffer[LIBRARY_READ_BUFFER];
        char line[256];
        size_t line_len = 0;
        bool in_data = false;
        uint32_t pending = count >= 32 ? 0xFFFFFFFFU : (((uint32_t)1 << count) - 1);

        while(pending) {
            size_t read = storage_file_read(file, buffer, sizeof(buffer));
            if(read == 0) break;

            for(size_t i = 0; i < read && pending; i++) {
                char ch = (char)buffer[i];
                if(ch == '\r') continue;

                if(ch != '\n') {
                    if(line_len + 1 < sizeof(line)) line[line_len++] = ch;
                    continue;
                }

                line[line_len] = '\0';
                if(in_data && line_len > 0) {
                    for(size_t k = 0; k < count; k++) {
                        if(!(pending & ((uint32_t)1 << k))) continue;
                        if(!library_line_matches_id(line, ids[k])) continue;

                        const char* field = line;
                        const char* colon = strchr(line, ':');
                        if(colon) {
                            field = colon + 1;
                            while(*field == ' ') field++;
                        }
                        for(size_t f = 0; f < 3; f++) {
                            const char* end = field;
                            while(*end && *end != '|') end++;
                            size_t len = (size_t)(end - field);
                            if(f == 0) {
                                library_take_field(
                                    names + k * name_stride, name_stride, field, len);
                            } else if(f == 2) {
                                library_take_field(
                                    series + k * series_stride, series_stride, field, len);
                            }
                            if(*end != '|') break;
                            field = end + 1;
                        }
                        pending &= ~((uint32_t)1 << k);
                    }
                } else if(line_len == 0) {
                    in_data = true;
                }
                line_len = 0;
            }
        }

        storage_file_close(file);
    }

    storage_file_free(file);
}

size_t ami_tool_library_resolve_page_names(AmiToolApp* app) {
    if(!app || !app->storage) return 0;

    const char* ids[AMI_TOOL_LIBRARY_PAGE];
    size_t slots[AMI_TOOL_LIBRARY_PAGE];
    size_t count = 0;

    for(size_t i = 0; i < page_count; i++) {
        if(page_items[i].id[0] == '\0' || page_items[i].name[0] != '\0') continue;
        ids[count] = page_items[i].id;
        slots[count] = i;
        count++;
    }
    if(count == 0) return 0;

    char names[AMI_TOOL_LIBRARY_PAGE][AMI_TOOL_LIBRARY_NAME_MAX];
    char series[AMI_TOOL_LIBRARY_PAGE][AMI_TOOL_LIBRARY_SERIES_MAX];
    memset(names, 0, sizeof(names));
    memset(series, 0, sizeof(series));

    library_resolve_batch(
        app->storage, ids, (char*)names, sizeof(names[0]), (char*)series, sizeof(series[0]), count);

    size_t resolved = 0;
    for(size_t k = 0; k < count; k++) {
        AmiToolLibraryItem* item = &page_items[slots[k]];
        memcpy(item->name, names[k], sizeof(item->name));
        memcpy(item->series, series[k], sizeof(item->series));
        item->recognized = item->name[0] != '\0';
        if(item->recognized) resolved++;
    }
    return resolved;
}

/* --- index build / load --- */

static void library_walk(AmiToolApp* app, const char* rel_dir, unsigned depth, File* index_file) {
    char abs_path[AMI_TOOL_LIBRARY_PATH_MAX];
    char rel_child[AMI_TOOL_LIBRARY_PATH_MAX];

    if(rel_dir[0] == '\0') {
        size_t prefix = strlen(AMI_TOOL_SAVED_FOLDER);
        if(prefix + 1 > sizeof(abs_path)) return;
        memcpy(abs_path, AMI_TOOL_SAVED_FOLDER, prefix + 1);
    } else {
        library_build_abs(abs_path, sizeof(abs_path), rel_dir);
        if(abs_path[0] == '\0') return;
    }

    File* dir = storage_file_alloc(app->storage);
    if(!dir) return;

    if(storage_dir_open(dir, abs_path)) {
        FileInfo info;
        char name[128];

        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(name[0] == '.') continue;

            size_t rlen = strlen(rel_dir);
            size_t nlen = strlen(name);
            if(rlen + (rlen ? 1 : 0) + nlen + 1 > sizeof(rel_child)) continue;

            if(rlen == 0) {
                memcpy(rel_child, name, nlen + 1);
            } else {
                memcpy(rel_child, rel_dir, rlen);
                rel_child[rlen] = '/';
                memcpy(rel_child + rlen + 1, name, nlen + 1);
            }

            if(file_info_is_dir(&info)) {
                if(depth < LIBRARY_SCAN_DEPTH) {
                    library_walk(app, rel_child, depth + 1, index_file);
                }
            } else if(library_is_nfc_name(name) && index_file) {
                if(rlen + (rlen ? 1 : 0) + nlen >= sizeof(page_items[0].rel)) continue;
                storage_file_write(index_file, rel_child, strlen(rel_child));
                storage_file_write(index_file, "\n", 1);
                total_count++;
            }
        }
    }

    storage_dir_close(dir);
    storage_file_free(dir);
}

static bool library_build_index(AmiToolApp* app) {
    storage_simply_mkdir(app->storage, AMI_TOOL_SAVED_FOLDER);
    storage_simply_mkdir(app->storage, LIBRARY_DIR);

    File* index_file = storage_file_alloc(app->storage);
    if(!index_file) return false;

    bool ok = false;
    total_count = 0;
    if(storage_file_open(index_file, LIBRARY_INDEX_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        library_walk(app, "", 0, index_file);
        storage_file_close(index_file);
        ok = true;
    }
    storage_file_free(index_file);

    library_write_meta(app);
    return ok;
}

static void library_read_meta(AmiToolApp* app, size_t* count, uint32_t* timestamp) {
    *count = 0;
    *timestamp = 0;

    File* meta = storage_file_alloc(app->storage);
    if(!meta) return;

    if(storage_file_open(meta, LIBRARY_META_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[48];
        size_t read = storage_file_read(meta, line, sizeof(line) - 1);
        line[read] = '\0';
        char* newline = strchr(line, '\n');
        if(newline) {
            *newline = '\0';
            *timestamp = (uint32_t)strtoul(newline + 1, NULL, 10);
        }
        *count = (size_t)strtoul(line, NULL, 10);
        storage_file_close(meta);
    }

    storage_file_free(meta);
}

static void library_write_meta(AmiToolApp* app) {
    uint32_t timestamp = 0;
    storage_common_timestamp(app->storage, AMI_TOOL_SAVED_FOLDER, &timestamp);

    File* meta = storage_file_alloc(app->storage);
    if(meta) {
        if(storage_file_open(meta, LIBRARY_META_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            char line[48];
            int len = snprintf(
                line, sizeof(line), "%u\n%lu\n", (unsigned)total_count, (unsigned long)timestamp);
            if(len > 0) storage_file_write(meta, line, (size_t)len);
            storage_file_close(meta);
        }
        storage_file_free(meta);
    }
}

bool ami_tool_library_ensure_index(AmiToolApp* app) {
    if(!app || !app->storage) return false;

    size_t stored = 0;
    uint32_t stored_timestamp = 0;
    library_read_meta(app, &stored, &stored_timestamp);

    if(stored > 0) {
        File* probe = storage_file_alloc(app->storage);
        bool exists = false;
        if(probe) {
            exists = storage_file_open(probe, LIBRARY_INDEX_PATH, FSAM_READ, FSOM_OPEN_EXISTING);
            if(exists) storage_file_close(probe);
            storage_file_free(probe);
        }

        if(exists) {
            uint32_t current = 0;
            FS_Error status =
                storage_common_timestamp(app->storage, AMI_TOOL_SAVED_FOLDER, &current);
            /* Rebuild automatically when the library folder itself was replaced. */
            if(status != FSE_OK || current == stored_timestamp) {
                total_count = stored;
                return true;
            }
        }
    }

    if(!library_build_index(app)) return false;
    return total_count > 0;
}

bool ami_tool_library_rebuild(AmiToolApp* app) {
    if(!app || !app->storage) return false;
    if(!library_build_index(app)) return false;
    return true;
}

/* --- id cache: "<index>\t<id>\t<name>\t<series>\t<rel>" --- */

typedef struct {
    unsigned index;
    char id[AMI_TOOL_LIBRARY_ID_MAX];
    char name[AMI_TOOL_LIBRARY_NAME_MAX];
    char series[AMI_TOOL_LIBRARY_SERIES_MAX];
    char rel[96];
} LibraryRecord;

static bool library_parse_record(char* line, LibraryRecord* rec) {
    char* t1 = strchr(line, '\t');
    if(!t1) return false;
    char* t2 = strchr(t1 + 1, '\t');
    if(!t2) return false;
    char* t3 = strchr(t2 + 1, '\t');
    if(!t3) return false;
    char* t4 = strchr(t3 + 1, '\t');
    if(!t4) return false;

    *t1 = '\0';
    *t2 = '\0';
    *t3 = '\0';
    *t4 = '\0';

    memset(rec, 0, sizeof(*rec));
    rec->index = (unsigned)strtoul(line, NULL, 10);
    library_copy_str(rec->id, sizeof(rec->id), t1 + 1, strlen(t1 + 1));
    library_copy_str(rec->name, sizeof(rec->name), t2 + 1, strlen(t2 + 1));
    library_copy_str(rec->series, sizeof(rec->series), t3 + 1, strlen(t3 + 1));
    library_copy_str(rec->rel, sizeof(rec->rel), t4 + 1, strlen(t4 + 1));
    return true;
}

typedef void (*LibraryRecordFn)(const LibraryRecord* rec, void* context);

static void library_stream_ids(AmiToolApp* app, LibraryRecordFn fn, void* context) {
    File* ids = storage_file_alloc(app->storage);
    if(!ids) return;

    if(storage_file_open(ids, LIBRARY_IDS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t buffer[LIBRARY_READ_BUFFER];
        char line[AMI_TOOL_LIBRARY_PATH_MAX + 128];
        size_t line_len = 0;

        while(true) {
            size_t read = storage_file_read(ids, buffer, sizeof(buffer));
            if(read == 0) break;

            for(size_t i = 0; i < read; i++) {
                char ch = (char)buffer[i];
                if(ch == '\r') continue;
                if(ch != '\n') {
                    if(line_len + 1 < sizeof(line)) line[line_len++] = ch;
                    continue;
                }
                line[line_len] = '\0';
                line_len = 0;

                LibraryRecord rec;
                if(library_parse_record(line, &rec)) {
                    fn(&rec, context);
                }
            }
        }

        storage_file_close(ids);
    }

    storage_file_free(ids);
}

/* --- series browsing --- */

typedef struct {
    AmiToolSeriesEntry* out;
    size_t max;
    size_t count;
} SeriesListContext;

static bool library_name_equal(const char* a, const char* b) {
    while(*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if(ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if(cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if(ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

/* Copy a series name without trailing spaces, so "Pokémon " and "Pokémon" merge. */
static void library_copy_trimmed(char* dst, size_t cap, const char* src) {
    size_t len = strlen(src);
    while(len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\r' || src[len - 1] == '\t')) {
        len--;
    }
    if(len >= cap) len = cap - 1;
    if(len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static void library_series_collect(const LibraryRecord* rec, void* context) {
    SeriesListContext* ctx = context;
    if(rec->series[0] == '\0') return;

    for(size_t i = 0; i < ctx->count; i++) {
        if(library_name_equal(ctx->out[i].name, rec->series)) {
            ctx->out[i].count++;
            return;
        }
    }
    if(ctx->count >= ctx->max) return;

    /* Use a local value: shifting the array must not rewrite the entry we are inserting. */
    AmiToolSeriesEntry entry;
    memset(&entry, 0, sizeof(entry));
    library_copy_trimmed(entry.name, sizeof(entry.name), rec->series);
    entry.count = 1;

    size_t pos = ctx->count;
    while(pos > 0 && strcmp(ctx->out[pos - 1].name, entry.name) > 0) {
        ctx->out[pos] = ctx->out[pos - 1];
        pos--;
    }
    ctx->out[pos] = entry;
    ctx->count++;
}

size_t ami_tool_library_series_list(AmiToolApp* app, AmiToolSeriesEntry* out, size_t max) {
    if(!app || !out || max == 0) return 0;
    SeriesListContext ctx = {.out = out, .max = max, .count = 0};
    library_stream_ids(app, library_series_collect, &ctx);
    return ctx.count;
}

typedef struct {
    const char* series;
    size_t page;
    size_t index_in_series;
    size_t filled;
    size_t total;
} SeriesPageContext;

static void library_series_page_collect(const LibraryRecord* rec, void* context) {
    SeriesPageContext* ctx = context;
    if(!library_name_equal(rec->series, ctx->series)) return;

    size_t position = ctx->total;
    ctx->total++;

    if(position < ctx->page * AMI_TOOL_LIBRARY_PAGE) return;
    if(ctx->filled >= AMI_TOOL_LIBRARY_PAGE) return;

    AmiToolLibraryItem* item = &page_items[ctx->filled];
    memset(item, 0, sizeof(*item));
    library_copy_str(item->rel, sizeof(item->rel), rec->rel, strlen(rec->rel));
    library_copy_str(item->id, sizeof(item->id), rec->id, strlen(rec->id));
    library_copy_str(item->name, sizeof(item->name), rec->name, strlen(rec->name));
    library_copy_str(item->series, sizeof(item->series), rec->series, strlen(rec->series));
    item->file_index = rec->index;
    item->recognized = item->name[0] != '\0';
    item->persisted = true;
    ctx->filled++;
}

typedef struct {
    const char* series;
    size_t total;
} SeriesTotalContext;

static void library_series_total_collect(const LibraryRecord* rec, void* context) {
    SeriesTotalContext* ctx = context;
    if(library_name_equal(rec->series, ctx->series)) ctx->total++;
}

size_t ami_tool_library_series_page(AmiToolApp* app, const char* series, size_t page) {
    if(!app || !series) return 0;

    page_count = 0;
    for(size_t i = 0; i < AMI_TOOL_LIBRARY_PAGE; i++) {
        memset(&page_items[i], 0, sizeof(page_items[i]));
    }

    SeriesPageContext ctx = {
        .series = series,
        .page = page,
        .index_in_series = 0,
        .filled = 0,
        .total = 0,
    };
    library_stream_ids(app, library_series_page_collect, &ctx);
    page_count = ctx.filled;
    return ctx.total;
}

size_t ami_tool_library_series_total(AmiToolApp* app, const char* series) {
    if(!app || !series) return 0;
    SeriesTotalContext ctx = {.series = series, .total = 0};
    library_stream_ids(app, library_series_total_collect, &ctx);
    return ctx.total;
}

static void library_apply_ids_from_log(AmiToolApp* app, size_t page) {
    File* ids = storage_file_alloc(app->storage);
    if(!ids) return;

    if(storage_file_open(ids, LIBRARY_IDS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t buffer[LIBRARY_READ_BUFFER];
        char line[AMI_TOOL_LIBRARY_PATH_MAX + 128];
        size_t line_len = 0;
        size_t unresolved = page_count;
        size_t base = page * AMI_TOOL_LIBRARY_PAGE;

        while(unresolved > 0) {
            size_t read = storage_file_read(ids, buffer, sizeof(buffer));
            if(read == 0) break;

            for(size_t i = 0; i < read && unresolved > 0; i++) {
                char ch = (char)buffer[i];
                if(ch == '\r') continue;
                if(ch != '\n') {
                    if(line_len + 1 < sizeof(line)) line[line_len++] = ch;
                    continue;
                }
                line[line_len] = '\0';
                line_len = 0;

                unsigned index = (unsigned)strtoul(line, NULL, 10);
                if(index < base || index >= base + AMI_TOOL_LIBRARY_PAGE) continue;
                size_t slot = index - base;
                if(slot >= page_count || page_items[slot].id[0] != '\0') continue;

                char* t1 = strchr(line, '\t');
                if(!t1) continue;
                char* t2 = strchr(t1 + 1, '\t');
                if(!t2) continue;
                char* t3 = strchr(t2 + 1, '\t');
                if(!t3) continue;
                char* t4 = strchr(t3 + 1, '\t');
                if(!t4) continue;

                *t1 = '\0';
                *t2 = '\0';
                *t3 = '\0';
                *t4 = '\0';

                /* The cache is keyed by position in the index: after the SD card
                   contents change, positions shift, so the path must match too. */
                if(strcmp(page_items[slot].rel, t4 + 1) != 0) continue;

                library_copy_str(page_items[slot].id, sizeof(page_items[slot].id), t1 + 1, strlen(t1 + 1));
                library_copy_str(
                    page_items[slot].name, sizeof(page_items[slot].name), t2 + 1, strlen(t2 + 1));
                library_copy_str(
                    page_items[slot].series,
                    sizeof(page_items[slot].series),
                    t3 + 1,
                    strlen(t3 + 1));
                page_items[slot].recognized = page_items[slot].name[0] != '\0';
                page_items[slot].persisted = true;
                unresolved--;
            }
        }

        storage_file_close(ids);
    }

    storage_file_free(ids);
}

static void library_append_line(File* ids, size_t index, const AmiToolLibraryItem* item) {
    char line[AMI_TOOL_LIBRARY_PATH_MAX + 128];
    int len = snprintf(
        line,
        sizeof(line),
        "%u\t%.16s\t%.39s\t%.23s\t%.95s\n",
        (unsigned)index,
        item->id,
        item->name,
        item->series,
        item->rel);
    if(len > 0) {
        storage_file_write(ids, line, (size_t)len);
    }
}

void ami_tool_library_append_ids(AmiToolApp* app, size_t page) {
    UNUSED(page);
    if(!app || !app->storage) return;

    storage_simply_mkdir(app->storage, LIBRARY_DIR);
    File* ids = storage_file_alloc(app->storage);
    if(!ids) return;

    if(storage_file_open(ids, LIBRARY_IDS_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        for(size_t i = 0; i < page_count; i++) {
            AmiToolLibraryItem* item = &page_items[i];
            if(item->id[0] == '\0' || item->name[0] == '\0' || item->persisted) continue;
            library_append_line(ids, item->file_index, item);
            item->persisted = true;
        }
        storage_file_close(ids);
    }

    storage_file_free(ids);
}

bool ami_tool_library_load_page(AmiToolApp* app, size_t page) {
    if(!app || !app->storage) return false;

    page_count = 0;
    for(size_t i = 0; i < AMI_TOOL_LIBRARY_PAGE; i++) {
        memset(&page_items[i], 0, sizeof(page_items[i]));
    }

    File* index_file = storage_file_alloc(app->storage);
    if(!index_file) return false;

    bool ok = false;
    if(storage_file_open(index_file, LIBRARY_INDEX_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint8_t buffer[LIBRARY_READ_BUFFER];
        size_t lines_to_skip = page * AMI_TOOL_LIBRARY_PAGE;
        size_t line_len = 0;
        char line[AMI_TOOL_LIBRARY_PATH_MAX];

        while(page_count < AMI_TOOL_LIBRARY_PAGE) {
            size_t read = storage_file_read(index_file, buffer, sizeof(buffer));
            if(read == 0) break;

            for(size_t i = 0; i < read && page_count < AMI_TOOL_LIBRARY_PAGE; i++) {
                char ch = (char)buffer[i];
                if(ch == '\r') continue;
                if(ch != '\n') {
                    if(line_len + 1 < sizeof(line)) line[line_len++] = ch;
                    continue;
                }

                line[line_len] = '\0';
                line_len = 0;

                if(lines_to_skip > 0) {
                    lines_to_skip--;
                    continue;
                }
                if(line[0] == '\0') continue;

                AmiToolLibraryItem* item = &page_items[page_count];
                library_copy_str(item->rel, sizeof(item->rel), line, strlen(line));
                item->file_index = (unsigned)(page * AMI_TOOL_LIBRARY_PAGE + page_count);
                page_count++;
            }
        }

        storage_file_close(index_file);
        ok = true;
    }

    storage_file_free(index_file);

    if(page_count > 0) {
        library_apply_ids_from_log(app, page);
    }
    return ok;
}

/* --- per-item dump loading --- */

static bool library_load_dump(Storage* storage, const char* rel, char* id_out, size_t id_size) {
    UNUSED(storage);
    char path[AMI_TOOL_LIBRARY_PATH_MAX];
    library_build_abs(path, sizeof(path), rel);
    if(path[0] == '\0') return false;

    NfcDevice* device = nfc_device_alloc();
    if(!device) return false;

    bool have_id = false;
    if(nfc_device_load(device, path)) {
        if(nfc_device_get_protocol(device) == NfcProtocolMfUltralight) {
            const MfUltralightData* data =
                (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
            if(data && ami_tool_extract_amiibo_id(data, id_out, id_size)) {
                have_id = true;
            }
        }
    }

    nfc_device_free(device);
    return have_id;
}

static bool library_store_tags(AmiToolApp* app, size_t local_index) {
    AmiToolLibraryItem* item = &page_items[local_index];
    char path[AMI_TOOL_LIBRARY_PATH_MAX];
    library_build_abs(path, sizeof(path), item->rel);
    if(path[0] == '\0') return false;

    NfcDevice* device = nfc_device_alloc();
    if(!device) return false;

    bool ok = false;
    if(nfc_device_load(device, path)) {
        if(nfc_device_get_protocol(device) == NfcProtocolMfUltralight) {
            const MfUltralightData* data =
                (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
            if(data) {
                mf_ultralight_copy(app->tag_data, data);
                app->tag_data_valid = true;
                ok = true;
                library_copy_str(
                    app->last_dump_path, sizeof(app->last_dump_path), path, strlen(path));

                /* Emulation needs the UID, the NTAG215 password and a PACK value, exactly
                   like the Generate path sets up after building a dump. */
                if(app->tag_data->pages_total >= 2) {
                    uint8_t uid[7];
                    memcpy(uid, app->tag_data->page[0].data, 3);
                    memcpy(uid + 3, app->tag_data->page[1].data, 4);
                    ami_tool_store_uid(app, uid, sizeof(uid));
                    if(ami_tool_compute_password_from_uid(uid, sizeof(uid), &app->tag_password)) {
                        app->tag_password_valid = true;
                    } else {
                        app->tag_password_valid = false;
                        memset(&app->tag_password, 0, sizeof(app->tag_password));
                    }
                    static const uint8_t default_pack[4] = {0x80, 0x80, 0x00, 0x00};
                    memcpy(app->tag_pack, default_pack, sizeof(default_pack));
                    app->tag_pack_valid = true;
                }

                char id[AMI_TOOL_LIBRARY_ID_MAX];
                id[0] = '\0';
                if(ami_tool_extract_amiibo_id(data, id, sizeof(id))) {
                    library_copy_str(item->id, sizeof(item->id), id, strlen(id));
                }
            }
        }
    }

    nfc_device_free(device);
    return ok;
}

bool ami_tool_library_open(AmiToolApp* app, size_t local_index) {
    if(local_index >= page_count) return false;

    bool ok = library_store_tags(app, local_index);
    ami_tool_library_resolve_page_names(app);
    return ok;
}

bool ami_tool_library_index_page(AmiToolApp* app, size_t page) {
    if(!app || page_count == 0) return false;

    for(size_t i = 0; i < page_count; i++) {
        if(page_items[i].id[0] == '\0') {
            library_store_tags(app, i);
        }
    }

    ami_tool_library_resolve_page_names(app);
    ami_tool_library_append_ids(app, page);
    return true;
}

size_t ami_tool_library_index_page_unknown(void) {
    size_t unknown = 0;
    for(size_t i = 0; i < page_count; i++) {
        if(page_items[i].id[0] == '\0') unknown++;
    }
    return unknown;
}

/* --- full-library background indexer --- */

typedef struct {
    unsigned index;
    AmiToolLibraryItem item;
} LibraryPending;

static LibraryPending pending_batch[LIBRARY_BATCH];
static size_t pending_count = 0;

static void library_flush_pending(Storage* storage, File* ids) {
    if(pending_count == 0) return;

    for(size_t i = 0; i < pending_count; i++) {
        if(pending_batch[i].item.id[0] == '\0') {
            library_load_dump(
                storage,
                pending_batch[i].item.rel,
                pending_batch[i].item.id,
                sizeof(pending_batch[i].item.id));
        }
    }

    const char* id_ptrs[LIBRARY_BATCH];
    size_t slots[LIBRARY_BATCH];
    size_t with_id = 0;
    for(size_t i = 0; i < pending_count; i++) {
        if(pending_batch[i].item.id[0] == '\0') continue;
        id_ptrs[with_id] = pending_batch[i].item.id;
        slots[with_id] = i;
        with_id++;
    }

    if(with_id > 0) {
        char names[LIBRARY_BATCH][AMI_TOOL_LIBRARY_NAME_MAX];
        char series[LIBRARY_BATCH][AMI_TOOL_LIBRARY_SERIES_MAX];
        memset(names, 0, sizeof(names));
        memset(series, 0, sizeof(series));

        library_resolve_batch(
            storage,
            id_ptrs,
            (char*)names,
            sizeof(names[0]),
            (char*)series,
            sizeof(series[0]),
            with_id);

        for(size_t k = 0; k < with_id; k++) {
            AmiToolLibraryItem* item = &pending_batch[slots[k]].item;
            memcpy(item->name, names[k], sizeof(item->name));
            memcpy(item->series, series[k], sizeof(item->series));
            item->recognized = item->name[0] != '\0';
        }
    }

    if(ids) {
        for(size_t i = 0; i < pending_count; i++) {
            AmiToolLibraryItem* item = &pending_batch[i].item;
            if(item->id[0] == '\0' || item->name[0] == '\0') continue;
            library_append_line(ids, pending_batch[i].index, item);
        }
    }

    pending_count = 0;
}

static int32_t library_index_worker(void* context) {
    AmiToolApp* app = context;
    UNUSED(app);
    Storage* storage = furi_record_open(RECORD_STORAGE);

    size_t total = index_total;
    uint8_t* seen = NULL;
    if(total > 0) {
        size_t bytes = (total + 7) / 8;
        seen = malloc(bytes);
        if(seen) memset(seen, 0, bytes);
    }

    if(seen) {
        File* reader = storage_file_alloc(storage);
        if(reader) {
            if(storage_file_open(reader, LIBRARY_IDS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
                char line[64];
                size_t len = 0;
                uint8_t buf[LIBRARY_READ_BUFFER];
                while(true) {
                    size_t read = storage_file_read(reader, buf, sizeof(buf));
                    if(read == 0) break;
                    for(size_t i = 0; i < read; i++) {
                        char ch = (char)buf[i];
                        if(ch == '\r') continue;
                        if(ch != '\n') {
                            if(len + 1 < sizeof(line)) line[len++] = ch;
                            continue;
                        }
                        line[len] = '\0';
                        len = 0;
                        unsigned n = (unsigned)strtoul(line, NULL, 10);
                        if(n < total) seen[n >> 3] |= (uint8_t)(1u << (n & 7));
                    }
                }
                storage_file_close(reader);
            }
            storage_file_free(reader);
        }
    }

    File* ids = storage_file_alloc(storage);
    bool ids_ready = ids && storage_file_open(ids, LIBRARY_IDS_PATH, FSAM_WRITE, FSOM_OPEN_APPEND);

    pending_count = 0;
    File* idx = storage_file_alloc(storage);
    if(idx && storage_file_open(idx, LIBRARY_INDEX_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[AMI_TOOL_LIBRARY_PATH_MAX];
        size_t line_len = 0;
        uint8_t buf[LIBRARY_READ_BUFFER];
        unsigned index_no = 0;

        while(!index_cancel) {
            size_t read = storage_file_read(idx, buf, sizeof(buf));
            if(read == 0) break;

            for(size_t i = 0; i < read && !index_cancel; i++) {
                char ch = (char)buf[i];
                if(ch == '\r') continue;
                if(ch != '\n') {
                    if(line_len + 1 < sizeof(line)) line[line_len++] = ch;
                    continue;
                }

                line[line_len] = '\0';
                line_len = 0;

                bool already = false;
                if(seen && index_no < total) {
                    already = (seen[index_no >> 3] & (uint8_t)(1u << (index_no & 7))) != 0;
                }

                if(line[0] != '\0' && !already) {
                    LibraryPending* pending = &pending_batch[pending_count];
                    memset(pending, 0, sizeof(*pending));
                    pending->index = index_no;
                    library_copy_str(
                        pending->item.rel, sizeof(pending->item.rel), line, strlen(line));
                    pending_count++;

                    if(pending_count >= LIBRARY_BATCH) {
                        library_flush_pending(storage, ids_ready ? ids : NULL);
                    }
                }

                index_no++;
                index_done = index_no;
            }
        }

        library_flush_pending(storage, ids_ready ? ids : NULL);
        index_done = index_no;
        storage_file_close(idx);
    }
    storage_file_free(idx);

    if(ids) {
        if(ids_ready) storage_file_close(ids);
        storage_file_free(ids);
    }

    if(seen) free(seen);

    furi_record_close(RECORD_STORAGE);
    index_active = false;
    index_finished = true;
    return 0;
}

bool ami_tool_library_start_index_all(AmiToolApp* app) {
    if(index_active) return false;
    if(!ami_tool_library_ensure_index(app)) return false;

    index_total = total_count;
    index_done = 0;
    index_cancel = false;
    index_finished = false;
    index_active = true;

    index_thread = furi_thread_alloc();
    furi_thread_set_name(index_thread, "AmiLibIndex");
    furi_thread_set_stack_size(index_thread, 8 * 1024);
    furi_thread_set_callback(index_thread, library_index_worker);
    furi_thread_set_context(index_thread, app);
    furi_thread_start(index_thread);
    return true;
}

void ami_tool_library_stop_index_all(void) {
    index_cancel = true;
    if(index_thread) {
        furi_thread_join(index_thread);
        furi_thread_free(index_thread);
        index_thread = NULL;
    }
    index_active = false;
}

bool ami_tool_library_indexing(void) {
    return index_active;
}

size_t ami_tool_library_index_progress(size_t* total) {
    if(total) *total = index_total;
    return index_done;
}

bool ami_tool_library_index_finished(void) {
    return index_finished;
}

void ami_tool_library_clear_finished(void) {
    index_finished = false;
}

/* --- favorites: a plain list of Amiibo IDs --- */

static size_t library_read_favorites(
    AmiToolApp* app,
    char (*ids)[AMI_TOOL_LIBRARY_ID_MAX],
    size_t max) {
    size_t count = 0;
    File* file = storage_file_alloc(app->storage);
    if(!file) return 0;

    if(storage_file_open(file, LIBRARY_FAV_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char line[AMI_TOOL_LIBRARY_ID_MAX + 2];
        size_t len = 0;
        uint8_t buf[64];

        while(true) {
            size_t read = storage_file_read(file, buf, sizeof(buf));
            if(read == 0) break;

            for(size_t i = 0; i < read; i++) {
                char ch = (char)buf[i];
                if(ch == '\r') continue;
                if(ch != '\n') {
                    if(len + 1 < sizeof(line)) line[len++] = ch;
                    continue;
                }
                line[len] = '\0';
                len = 0;
                if(line[0] != '\0' && count < max) {
                    library_copy_str(ids[count], AMI_TOOL_LIBRARY_ID_MAX, line, strlen(line));
                    count++;
                }
            }
        }

        storage_file_close(file);
    }

    storage_file_free(file);
    return count;
}

static void library_write_favorites(
    AmiToolApp* app,
    char (*ids)[AMI_TOOL_LIBRARY_ID_MAX],
    size_t count) {
    storage_simply_mkdir(app->storage, LIBRARY_DIR);

    File* file = storage_file_alloc(app->storage);
    if(!file) return;

    if(storage_file_open(file, LIBRARY_FAV_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        for(size_t i = 0; i < count; i++) {
            storage_file_write(file, ids[i], strlen(ids[i]));
            storage_file_write(file, "\n", 1);
        }
        storage_file_close(file);
    }

    storage_file_free(file);
}

bool ami_tool_library_is_favorite(AmiToolApp* app, const char* id) {
    if(!app || !id || id[0] == '\0') return false;

    char ids[LIBRARY_FAV_MAX][AMI_TOOL_LIBRARY_ID_MAX];
    size_t count = library_read_favorites(app, ids, LIBRARY_FAV_MAX);
    for(size_t i = 0; i < count; i++) {
        if(strcmp(ids[i], id) == 0) return true;
    }
    return false;
}

void ami_tool_library_toggle_favorite_current(AmiToolApp* app) {
    if(!app) return;

    if(!app->info_last_has_id || app->info_last_id[0] == '\0') {
        ami_tool_info_show_action_message(app, "No Amiibo selected.");
        return;
    }

    char ids[LIBRARY_FAV_MAX][AMI_TOOL_LIBRARY_ID_MAX];
    size_t count = library_read_favorites(app, ids, LIBRARY_FAV_MAX);

    size_t position = count;
    for(size_t i = 0; i < count; i++) {
        if(strcmp(ids[i], app->info_last_id) == 0) {
            position = i;
            break;
        }
    }

    if(position < count) {
        for(size_t i = position; i + 1 < count; i++) {
            memcpy(ids[i], ids[i + 1], AMI_TOOL_LIBRARY_ID_MAX);
        }
        count--;
        library_write_favorites(app, ids, count);
        ami_tool_info_show_action_message(app, "Removed from favorites.");
        return;
    }

    if(count >= LIBRARY_FAV_MAX) {
        ami_tool_info_show_action_message(app, "Favorites list is full.");
        return;
    }

    library_copy_str(
        ids[count], AMI_TOOL_LIBRARY_ID_MAX, app->info_last_id, strlen(app->info_last_id));
    count++;
    library_write_favorites(app, ids, count);
    ami_tool_info_show_action_message(app, "Added to favorites.");
}

typedef struct {
    char(*favs)[AMI_TOOL_LIBRARY_ID_MAX];
    size_t fav_count;
    size_t page;
    size_t filled;
    size_t total;
} FavoritesContext;

static void library_favorites_collect(const LibraryRecord* rec, void* context) {
    FavoritesContext* ctx = context;

    bool favorite = false;
    for(size_t i = 0; i < ctx->fav_count; i++) {
        if(strcmp(ctx->favs[i], rec->id) == 0) {
            favorite = true;
            break;
        }
    }
    if(!favorite) return;

    size_t position = ctx->total;
    ctx->total++;

    if(position < ctx->page * AMI_TOOL_LIBRARY_PAGE) return;
    if(ctx->filled >= AMI_TOOL_LIBRARY_PAGE) return;

    AmiToolLibraryItem* item = &page_items[ctx->filled];
    memset(item, 0, sizeof(*item));
    library_copy_str(item->rel, sizeof(item->rel), rec->rel, strlen(rec->rel));
    library_copy_str(item->id, sizeof(item->id), rec->id, strlen(rec->id));
    library_copy_str(item->name, sizeof(item->name), rec->name, strlen(rec->name));
    library_copy_str(item->series, sizeof(item->series), rec->series, strlen(rec->series));
    item->file_index = rec->index;
    item->recognized = item->name[0] != '\0';
    item->persisted = true;
    ctx->filled++;
}

size_t ami_tool_library_favorites_page(AmiToolApp* app, size_t page) {
    if(!app) return 0;

    page_count = 0;
    for(size_t i = 0; i < AMI_TOOL_LIBRARY_PAGE; i++) {
        memset(&page_items[i], 0, sizeof(page_items[i]));
    }

    char favs[LIBRARY_FAV_MAX][AMI_TOOL_LIBRARY_ID_MAX];
    size_t fav_count = library_read_favorites(app, favs, LIBRARY_FAV_MAX);

    FavoritesContext ctx = {
        .favs = favs,
        .fav_count = fav_count,
        .page = page,
        .filled = 0,
        .total = 0,
    };
    library_stream_ids(app, library_favorites_collect, &ctx);
    page_count = ctx.filled;
    return ctx.total;
}

/* --- text search over the cached names and series --- */

static bool library_text_contains(const char* haystack, const char* needle) {
    size_t nlen = strlen(needle);
    if(nlen == 0) return true;

    size_t hlen = strlen(haystack);
    if(nlen > hlen) return false;

    for(size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for(size_t k = 0; k < nlen; k++) {
            char a = haystack[i + k];
            char b = needle[k];
            if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if(a != b) {
                match = false;
                break;
            }
        }
        if(match) return true;
    }
    return false;
}

typedef struct {
    const char* query;
    size_t page;
    size_t filled;
    size_t total;
} SearchContext;

static void library_search_collect(const LibraryRecord* rec, void* context) {
    SearchContext* ctx = context;

    if(!library_text_contains(rec->name, ctx->query) &&
       !library_text_contains(rec->series, ctx->query) &&
       !library_text_contains(rec->rel, ctx->query)) {
        return;
    }

    size_t position = ctx->total;
    ctx->total++;

    if(position < ctx->page * AMI_TOOL_LIBRARY_PAGE) return;
    if(ctx->filled >= AMI_TOOL_LIBRARY_PAGE) return;

    AmiToolLibraryItem* item = &page_items[ctx->filled];
    memset(item, 0, sizeof(*item));
    library_copy_str(item->rel, sizeof(item->rel), rec->rel, strlen(rec->rel));
    library_copy_str(item->id, sizeof(item->id), rec->id, strlen(rec->id));
    library_copy_str(item->name, sizeof(item->name), rec->name, strlen(rec->name));
    library_copy_str(item->series, sizeof(item->series), rec->series, strlen(rec->series));
    item->file_index = rec->index;
    item->recognized = item->name[0] != '\0';
    item->persisted = true;
    ctx->filled++;
}

typedef struct {
    size_t page;
    size_t filled;
    size_t total;
} UnrecognizedContext;

static void library_unrecognized_collect(const LibraryRecord* rec, void* context) {
    UnrecognizedContext* ctx = context;
    if(rec->name[0] != '\0') return;

    size_t position = ctx->total;
    ctx->total++;

    if(position < ctx->page * AMI_TOOL_LIBRARY_PAGE) return;
    if(ctx->filled >= AMI_TOOL_LIBRARY_PAGE) return;

    AmiToolLibraryItem* item = &page_items[ctx->filled];
    memset(item, 0, sizeof(*item));
    library_copy_str(item->rel, sizeof(item->rel), rec->rel, strlen(rec->rel));
    library_copy_str(item->id, sizeof(item->id), rec->id, strlen(rec->id));
    item->file_index = rec->index;
    item->persisted = true;
    ctx->filled++;
}

size_t ami_tool_library_unrecognized_page(AmiToolApp* app, size_t page) {
    if(!app) return 0;

    page_count = 0;
    for(size_t i = 0; i < AMI_TOOL_LIBRARY_PAGE; i++) {
        memset(&page_items[i], 0, sizeof(page_items[i]));
    }

    UnrecognizedContext ctx = {.page = page, .filled = 0, .total = 0};
    library_stream_ids(app, library_unrecognized_collect, &ctx);
    page_count = ctx.filled;
    return ctx.total;
}

size_t ami_tool_library_search_page(AmiToolApp* app, const char* query, size_t page) {
    if(!app) return 0;

    page_count = 0;
    for(size_t i = 0; i < AMI_TOOL_LIBRARY_PAGE; i++) {
        memset(&page_items[i], 0, sizeof(page_items[i]));
    }

    SearchContext ctx = {
        .query = query ? query : "",
        .page = page,
        .filled = 0,
        .total = 0,
    };
    library_stream_ids(app, library_search_collect, &ctx);
    page_count = ctx.filled;
    return ctx.total;
}

size_t ami_tool_library_favorites_total(AmiToolApp* app) {
    if(!app) return 0;
    char favs[LIBRARY_FAV_MAX][AMI_TOOL_LIBRARY_ID_MAX];
    size_t fav_count = library_read_favorites(app, favs, LIBRARY_FAV_MAX);
    if(fav_count == 0) return 0;

    FavoritesContext ctx = {
        .favs = favs,
        .fav_count = fav_count,
        .page = 0,
        .filled = 0,
        .total = 0,
    };
    library_stream_ids(app, library_favorites_collect, &ctx);
    return ctx.total;
}
