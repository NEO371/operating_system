#include "dbms.h"
#include "fat16.h"
#include "interrupts.h"

#define DBMS_TABLES 4u
#define DBMS_ROWS 8u
#define DBMS_NAME 16u
#define DBMS_VALUE 32u
#define DBMS_FILE_HEADER 8u
#define DBMS_ROW_DISK_SIZE (4u + DBMS_VALUE)
#define DBMS_TABLE_DISK_SIZE (1u + DBMS_NAME + 1u + DBMS_ROWS * DBMS_ROW_DISK_SIZE)
#define DBMS_FILE_SIZE (DBMS_FILE_HEADER + DBMS_TABLES * DBMS_TABLE_DISK_SIZE)

struct dbms_row {
    uint32_t id;
    char name[DBMS_VALUE];
};

struct dbms_table {
    uint8_t used;
    char name[DBMS_NAME];
    uint32_t row_count;
    struct dbms_row rows[DBMS_ROWS];
};

static struct dbms_table tables[DBMS_TABLES];
static struct dbms_table tables_backup[DBMS_TABLES];
static uint8_t dbms_storage_ready;
static uint8_t dbms_file_buffer[DBMS_FILE_SIZE];
static const char dbms_file_name[11] = {
    'D', 'B', 'M', 'S', ' ', ' ', ' ', ' ', 'D', 'A', 'T'
};

static char upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

static int equal_word(const char *a, const char *b) {
    uint32_t i = 0u;
    while (a[i] != '\0' && b[i] != '\0') {
        if (upper(a[i]) != upper(b[i])) return 0;
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

static int take_word(const char **cursor, char *word, uint32_t capacity) {
    uint32_t length = 0u;
    const char *p = skip_spaces(*cursor);
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
        if (length + 1u >= capacity) return 0;
        word[length++] = *p++;
    }
    if (length == 0u) return 0;
    word[length] = '\0';
    *cursor = p;
    return 1;
}

static int take_keyword(const char **cursor, const char *keyword) {
    const char *p = skip_spaces(*cursor);
    uint32_t i = 0u;
    while (keyword[i] != '\0' && p[i] != '\0' && upper(p[i]) == keyword[i]) ++i;
    if (keyword[i] != '\0' ||
        (p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
        (p[i] >= '0' && p[i] <= '9') || p[i] == '_') return 0;
    *cursor = p + i;
    return 1;
}

static int expect_char(const char **cursor, char expected) {
    const char *p = skip_spaces(*cursor);
    if (*p != expected) return 0;
    *cursor = p + 1;
    return 1;
}

static int parse_id(const char **cursor, uint32_t *value) {
    const char *p = skip_spaces(*cursor);
    uint32_t result = 0u;
    uint32_t digits = 0u;
    while (*p >= '0' && *p <= '9') {
        uint32_t digit = (uint32_t)(*p - '0');
        if (result > (0xFFFFFFFFu - digit) / 10u) return 0;
        result = result * 10u + digit;
        ++p;
        ++digits;
    }
    if (digits == 0u) return 0;
    *cursor = p;
    *value = result;
    return 1;
}

static int parse_name(const char **cursor, char *name, uint32_t capacity) {
    uint32_t length = 0u;
    const char *p = skip_spaces(*cursor);
    if (*p != '\'') return 0;
    ++p;
    while (*p != '\0' && *p != '\'') {
        if (length + 1u >= capacity) return 0;
        name[length++] = *p++;
    }
    if (*p != '\'') return 0;
    name[length] = '\0';
    *cursor = p + 1;
    return 1;
}

static void append_char(char *out, uint32_t cap, uint32_t *at, char c) {
    if (*at + 1u < cap) {
        out[*at] = c;
        ++*at;
        out[*at] = '\0';
    }
}

static void append_text(char *out, uint32_t cap, uint32_t *at, const char *text) {
    while (*text != '\0') append_char(out, cap, at, *text++);
}

static void append_number(char *out, uint32_t cap, uint32_t *at, uint32_t value) {
    char digits[10];
    uint32_t count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    while (count != 0u) append_char(out, cap, at, digits[--count]);
}

static struct dbms_table *find_table(const char *name) {
    uint32_t i;
    for (i = 0u; i < DBMS_TABLES; ++i)
        if (tables[i].used != 0u && equal_word(tables[i].name, name)) return &tables[i];
    return 0;
}

static void copy_text(char *to, uint32_t capacity, const char *from) {
    uint32_t i = 0u;
    while (from[i] != '\0' && i + 1u < capacity) {
        to[i] = from[i];
        ++i;
    }
    to[i] = '\0';
}

static void copy_bytes(uint8_t *to, const uint8_t *from, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; ++i) to[i] = from[i];
}

static void zero_bytes(uint8_t *to, uint32_t count) {
    uint32_t i;
    for (i = 0u; i < count; ++i) to[i] = 0u;
}

static int has_terminator(const char *text, uint32_t capacity) {
    uint32_t i;
    for (i = 0u; i < capacity; ++i)
        if (text[i] == '\0') return 1;
    return 0;
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void reset_tables(void) {
    uint32_t i;
    for (i = 0u; i < DBMS_TABLES; ++i) {
        uint32_t j;
        tables[i].used = 0u;
        tables[i].row_count = 0u;
        tables[i].name[0] = '\0';
        for (j = 0u; j < DBMS_ROWS; ++j) {
            tables[i].rows[j].id = 0u;
            tables[i].rows[j].name[0] = '\0';
        }
    }
}

static void backup_tables(void) {
    copy_bytes((uint8_t *)tables_backup, (const uint8_t *)tables,
               (uint32_t)sizeof(tables));
}

static void restore_tables(void) {
    copy_bytes((uint8_t *)tables, (const uint8_t *)tables_backup,
               (uint32_t)sizeof(tables));
}

static void serialize_tables(void) {
    uint32_t offset = 0u;
    uint32_t table_index;
    zero_bytes(dbms_file_buffer, sizeof(dbms_file_buffer));
    dbms_file_buffer[0] = 'M';
    dbms_file_buffer[1] = 'D';
    dbms_file_buffer[2] = 'B';
    dbms_file_buffer[3] = '1';
    dbms_file_buffer[4] = 1u;
    dbms_file_buffer[5] = DBMS_TABLES;
    dbms_file_buffer[6] = (uint8_t)DBMS_FILE_SIZE;
    dbms_file_buffer[7] = (uint8_t)(DBMS_FILE_SIZE >> 8);
    offset = DBMS_FILE_HEADER;
    for (table_index = 0u; table_index < DBMS_TABLES; ++table_index) {
        uint32_t row_index;
        dbms_file_buffer[offset++] = tables[table_index].used;
        copy_bytes(&dbms_file_buffer[offset],
                   (const uint8_t *)tables[table_index].name, DBMS_NAME);
        offset += DBMS_NAME;
        dbms_file_buffer[offset++] = (uint8_t)tables[table_index].row_count;
        for (row_index = 0u; row_index < DBMS_ROWS; ++row_index) {
            write_le32(&dbms_file_buffer[offset], tables[table_index].rows[row_index].id);
            offset += 4u;
            copy_bytes(&dbms_file_buffer[offset],
                       (const uint8_t *)tables[table_index].rows[row_index].name,
                       DBMS_VALUE);
            offset += DBMS_VALUE;
        }
    }
}

static int deserialize_tables(uint32_t file_size) {
    uint32_t offset = DBMS_FILE_HEADER;
    uint32_t table_index;
    struct dbms_table loaded[DBMS_TABLES];
    zero_bytes((uint8_t *)loaded, sizeof(loaded));
    if (file_size != DBMS_FILE_SIZE || dbms_file_buffer[0] != 'M' ||
        dbms_file_buffer[1] != 'D' || dbms_file_buffer[2] != 'B' ||
        dbms_file_buffer[3] != '1' || dbms_file_buffer[4] != 1u ||
        dbms_file_buffer[5] != DBMS_TABLES ||
        dbms_file_buffer[6] != (uint8_t)DBMS_FILE_SIZE ||
        dbms_file_buffer[7] != (uint8_t)(DBMS_FILE_SIZE >> 8)) return 0;
    for (table_index = 0u; table_index < DBMS_TABLES; ++table_index) {
        uint32_t row_index;
        loaded[table_index].used = dbms_file_buffer[offset++];
        copy_bytes((uint8_t *)loaded[table_index].name,
                   &dbms_file_buffer[offset], DBMS_NAME);
        offset += DBMS_NAME;
        loaded[table_index].row_count = dbms_file_buffer[offset++];
        if (loaded[table_index].used > 1u ||
            loaded[table_index].row_count > DBMS_ROWS) return 0;
        if (loaded[table_index].used != 0u) {
            if (!has_terminator(loaded[table_index].name, DBMS_NAME) ||
                loaded[table_index].name[0] == '\0') return 0;
            for (uint32_t previous = 0u; previous < table_index; ++previous)
                if (loaded[previous].used != 0u &&
                    equal_word(loaded[previous].name, loaded[table_index].name)) return 0;
        } else if (loaded[table_index].row_count != 0u) return 0;
        for (row_index = 0u; row_index < DBMS_ROWS; ++row_index) {
            loaded[table_index].rows[row_index].id =
                read_le32(&dbms_file_buffer[offset]);
            offset += 4u;
            copy_bytes((uint8_t *)loaded[table_index].rows[row_index].name,
                       &dbms_file_buffer[offset], DBMS_VALUE);
            offset += DBMS_VALUE;
            if (row_index < loaded[table_index].row_count &&
                !has_terminator(loaded[table_index].rows[row_index].name,
                                DBMS_VALUE)) return 0;
        }
    }
    copy_bytes((uint8_t *)tables, (const uint8_t *)loaded, (uint32_t)sizeof(tables));
    return 1;
}

static int save_tables(void) {
    serialize_tables();
    return dbms_storage_ready != 0u &&
           fat16_write_root_file(dbms_file_name, dbms_file_buffer,
                                 DBMS_FILE_SIZE);
}

static int command_end(const char **cursor) {
    const char *p = skip_spaces(*cursor);
    if (*p == ';') p = skip_spaces(p + 1);
    if (*p != '\0') return 0;
    *cursor = p;
    return 1;
}

void dbms_init(void) {
    uint32_t file_size = 0u;
    reset_tables();
    dbms_storage_ready = fat16_mount() ? 1u : 0u;
    if (dbms_storage_ready == 0u) {
        kernel_log("MiniOS: DBMS disk unavailable; database writes are disabled.\n");
        return;
    }
    if (fat16_read_root_file(dbms_file_name, dbms_file_buffer,
                             sizeof(dbms_file_buffer), &file_size)) {
        if (!deserialize_tables(file_size)) {
            dbms_storage_ready = 0u;
            kernel_log("MiniOS: DBMS file is invalid; database writes are disabled.\n");
        } else kernel_log("MiniOS: persistent DBMS data loaded from FAT16.\n");
    } else kernel_log("MiniOS: FAT16 DBMS disk mounted; a new database will be created on first write.\n");
}

int dbms_storage_is_ready(void) {
    return dbms_storage_ready != 0u;
}

void dbms_execute(const char *query, char *output, uint32_t output_capacity) {
    const char *p;
    char keyword[12];
    char table_name[DBMS_NAME];
    uint32_t out_at = 0u;
    if (output_capacity == 0u) return;
    output[0] = '\0';
    if (query == 0) return;
    p = skip_spaces(query);
    if (*p == '\0') return;
    if (!take_word(&p, keyword, sizeof(keyword))) {
        append_text(output, output_capacity, &out_at, "Syntax error. Type HELP.");
        return;
    }
    if (equal_word(keyword, "HELP")) {
        append_text(output, output_capacity, &out_at,
            "CREATE TABLE people (id INT, name TEXT)\n"
            "INSERT INTO people VALUES (1, 'Ada')\n"
            "SELECT * FROM people\nSELECT COUNT(*) FROM people\n"
            "SHOW TABLES\n"
            "UPDATE people SET name = 'Ada' WHERE id = 1\n"
            "DELETE FROM people WHERE id = 1\nDROP TABLE people\n"
            "FAT16 disk: max 4 tables x 8 rows.");
        return;
    }
    if (equal_word(keyword, "SHOW")) {
        uint32_t i;
        uint32_t shown = 0u;
        if (!take_keyword(&p, "TABLES") || !command_end(&p)) {
            append_text(output, output_capacity, &out_at, "Syntax: SHOW TABLES");
            return;
        }
        append_text(output, output_capacity, &out_at, "Tables\n------");
        for (i = 0u; i < DBMS_TABLES; ++i) {
            if (tables[i].used != 0u) {
                append_char(output, output_capacity, &out_at, '\n');
                append_text(output, output_capacity, &out_at, tables[i].name);
                ++shown;
            }
        }
        if (shown == 0u) append_text(output, output_capacity, &out_at, "\n(no tables)");
        return;
    }
    if (equal_word(keyword, "CREATE")) {
        uint32_t i;
        struct dbms_table *slot = 0;
        if (!take_keyword(&p, "TABLE") || !take_word(&p, table_name, sizeof(table_name)) ||
            !expect_char(&p, '(') || !take_keyword(&p, "ID") || !take_keyword(&p, "INT") ||
            !expect_char(&p, ',') || !take_keyword(&p, "NAME") ||
            !take_keyword(&p, "TEXT") || !expect_char(&p, ')') || !command_end(&p)) {
            append_text(output, output_capacity, &out_at, "Syntax: CREATE TABLE name (id INT, name TEXT)");
            return;
        }
        if (find_table(table_name) != 0) {
            append_text(output, output_capacity, &out_at, "Table already exists.");
            return;
        }
        if (dbms_storage_ready == 0u) {
            append_text(output, output_capacity, &out_at,
                        "DBMS disk unavailable; table was not created.");
            return;
        }
        for (i = 0u; i < DBMS_TABLES; ++i)
            if (tables[i].used == 0u) { slot = &tables[i]; break; }
        if (slot == 0) {
            append_text(output, output_capacity, &out_at, "Database is full (4 tables max).");
            return;
        }
        backup_tables();
        slot->used = 1u;
        slot->row_count = 0u;
        copy_text(slot->name, sizeof(slot->name), table_name);
        if (!save_tables()) {
            restore_tables();
            append_text(output, output_capacity, &out_at,
                        "Disk write failed; table creation rolled back.");
        } else append_text(output, output_capacity, &out_at, "Table created and saved.");
        return;
    }
    if (equal_word(keyword, "INSERT")) {
        struct dbms_table *table;
        uint32_t id;
        char value[DBMS_VALUE];
        if (!take_keyword(&p, "INTO") || !take_word(&p, table_name, sizeof(table_name)) ||
            !take_keyword(&p, "VALUES") || !expect_char(&p, '(') || !parse_id(&p, &id) ||
            !expect_char(&p, ',') || !parse_name(&p, value, sizeof(value)) ||
            !expect_char(&p, ')') || !command_end(&p)) {
            append_text(output, output_capacity, &out_at, "Syntax: INSERT INTO table VALUES (id, 'name')");
            return;
        }
        table = find_table(table_name);
        if (table == 0) append_text(output, output_capacity, &out_at, "Table not found.");
        else if (dbms_storage_ready == 0u)
            append_text(output, output_capacity, &out_at, "DBMS disk unavailable; insert was rejected.");
        else if (table->row_count >= DBMS_ROWS) append_text(output, output_capacity, &out_at, "Table is full (8 rows max).");
        else {
            uint32_t row_index;
            int duplicate = 0;
            for (row_index = 0u; row_index < table->row_count; ++row_index)
                if (table->rows[row_index].id == id) duplicate = 1;
            if (duplicate != 0) {
                append_text(output, output_capacity, &out_at,
                            "Duplicate id; ids must be unique.");
                return;
            }
            backup_tables();
            table->rows[table->row_count].id = id;
            copy_text(table->rows[table->row_count].name,
                      sizeof(table->rows[0].name), value);
            ++table->row_count;
            if (!save_tables()) {
                restore_tables();
                append_text(output, output_capacity, &out_at,
                            "Disk write failed; insert was rolled back.");
            } else append_text(output, output_capacity, &out_at, "1 row inserted and saved.");
        }
        return;
    }
    if (equal_word(keyword, "UPDATE")) {
        struct dbms_table *table;
        uint32_t id;
        uint32_t row_index;
        char value[DBMS_VALUE];
        if (!take_word(&p, table_name, sizeof(table_name)) ||
            !take_keyword(&p, "SET") || !take_keyword(&p, "NAME") ||
            !expect_char(&p, '=') || !parse_name(&p, value, sizeof(value)) ||
            !take_keyword(&p, "WHERE") || !take_keyword(&p, "ID") ||
            !expect_char(&p, '=') || !parse_id(&p, &id) || !command_end(&p)) {
            append_text(output, output_capacity, &out_at,
                        "Syntax: UPDATE table SET name = 'value' WHERE id = n");
            return;
        }
        table = find_table(table_name);
        if (table == 0) { append_text(output, output_capacity, &out_at, "Table not found."); return; }
        if (dbms_storage_ready == 0u) {
            append_text(output, output_capacity, &out_at, "DBMS disk unavailable; update was rejected.");
            return;
        }
        for (row_index = 0u; row_index < table->row_count; ++row_index)
            if (table->rows[row_index].id == id) break;
        if (row_index == table->row_count) {
            append_text(output, output_capacity, &out_at, "0 rows updated.");
            return;
        }
        backup_tables();
        copy_text(table->rows[row_index].name, sizeof(table->rows[row_index].name), value);
        if (!save_tables()) {
            restore_tables();
            append_text(output, output_capacity, &out_at,
                        "Disk write failed; update was rolled back.");
        } else append_text(output, output_capacity, &out_at, "1 row updated and saved.");
        return;
    }
    if (equal_word(keyword, "DELETE")) {
        struct dbms_table *table;
        uint32_t id;
        uint32_t row_index;
        if (!take_keyword(&p, "FROM") || !take_word(&p, table_name, sizeof(table_name)) ||
            !take_keyword(&p, "WHERE") || !take_keyword(&p, "ID") ||
            !expect_char(&p, '=') || !parse_id(&p, &id) || !command_end(&p)) {
            append_text(output, output_capacity, &out_at,
                        "Syntax: DELETE FROM table WHERE id = n");
            return;
        }
        table = find_table(table_name);
        if (table == 0) { append_text(output, output_capacity, &out_at, "Table not found."); return; }
        if (dbms_storage_ready == 0u) {
            append_text(output, output_capacity, &out_at, "DBMS disk unavailable; delete was rejected.");
            return;
        }
        for (row_index = 0u; row_index < table->row_count; ++row_index)
            if (table->rows[row_index].id == id) break;
        if (row_index == table->row_count) {
            append_text(output, output_capacity, &out_at, "0 rows deleted.");
            return;
        }
        backup_tables();
        for (; row_index + 1u < table->row_count; ++row_index)
            table->rows[row_index] = table->rows[row_index + 1u];
        --table->row_count;
        table->rows[table->row_count].id = 0u;
        table->rows[table->row_count].name[0] = '\0';
        if (!save_tables()) {
            restore_tables();
            append_text(output, output_capacity, &out_at,
                        "Disk write failed; delete was rolled back.");
        } else append_text(output, output_capacity, &out_at, "1 row deleted and saved.");
        return;
    }
    if (equal_word(keyword, "SELECT")) {
        struct dbms_table *table;
        uint32_t i;
        int count_only = 0;
        if (expect_char(&p, '*')) {
            if (!take_keyword(&p, "FROM")) goto select_syntax;
        } else if (take_keyword(&p, "COUNT")) {
            if (!expect_char(&p, '(') ||
                !expect_char(&p, '*') || !expect_char(&p, ')') || !take_keyword(&p, "FROM"))
                goto select_syntax;
            count_only = 1;
        } else goto select_syntax;
        if (!take_word(&p, table_name, sizeof(table_name)) || !command_end(&p)) goto select_syntax;
        table = find_table(table_name);
        if (table == 0) { append_text(output, output_capacity, &out_at, "Table not found."); return; }
        if (count_only != 0) {
            append_text(output, output_capacity, &out_at, "COUNT = ");
            append_number(output, output_capacity, &out_at, table->row_count);
            return;
        }
        append_text(output, output_capacity, &out_at, "id | name\n---+----------------");
        for (i = 0u; i < table->row_count; ++i) {
            append_char(output, output_capacity, &out_at, '\n');
            append_number(output, output_capacity, &out_at, table->rows[i].id);
            append_text(output, output_capacity, &out_at, " | ");
            append_text(output, output_capacity, &out_at, table->rows[i].name);
        }
        if (table->row_count == 0u) append_text(output, output_capacity, &out_at, "\n(empty)");
        return;
select_syntax:
        append_text(output, output_capacity, &out_at, "Syntax: SELECT *|COUNT(*) FROM table");
        return;
    }
    if (equal_word(keyword, "DROP")) {
        struct dbms_table *table;
        uint32_t i;
        if (!take_keyword(&p, "TABLE") || !take_word(&p, table_name, sizeof(table_name)) || !command_end(&p)) {
            append_text(output, output_capacity, &out_at, "Syntax: DROP TABLE name");
            return;
        }
        table = find_table(table_name);
        if (table == 0) append_text(output, output_capacity, &out_at, "Table not found.");
        else if (dbms_storage_ready == 0u)
            append_text(output, output_capacity, &out_at, "DBMS disk unavailable; drop was rejected.");
        else {
            backup_tables();
            table->used = 0u;
            table->row_count = 0u;
            table->name[0] = '\0';
            for (i = 0u; i < DBMS_ROWS; ++i) {
                table->rows[i].id = 0u;
                table->rows[i].name[0] = '\0';
            }
            if (!save_tables()) {
                restore_tables();
                append_text(output, output_capacity, &out_at,
                            "Disk write failed; drop was rolled back.");
            } else append_text(output, output_capacity, &out_at, "Table dropped and saved.");
        }
        return;
    }
    append_text(output, output_capacity, &out_at, "Unknown command. Type HELP.");
}

static int contains_text(const char *text, const char *wanted) {
    uint32_t start;
    for (start = 0u; text[start] != '\0'; ++start) {
        uint32_t offset = 0u;
        while (wanted[offset] != '\0' && text[start + offset] == wanted[offset])
            ++offset;
        if (wanted[offset] == '\0') return 1;
    }
    return 0;
}

int dbms_persistence_self_check(void) {
    char result[128];
    dbms_init();
    if (dbms_storage_ready == 0u) return 0;
    dbms_execute("SELECT * FROM mini", result, sizeof(result));
    if (contains_text(result, "Table not found.")) {
        dbms_execute("CREATE TABLE mini (id INT, name TEXT)",
                     result, sizeof(result));
        if (!contains_text(result, "Table created and saved.")) return 0;
        dbms_execute("INSERT INTO mini VALUES (4242, 'durable')",
                     result, sizeof(result));
        if (!contains_text(result, "1 row inserted and saved.")) return 0;
        dbms_execute("UPDATE mini SET name = 'draft' WHERE id = 4242",
                     result, sizeof(result));
        if (!contains_text(result, "1 row updated and saved.")) return 0;
        dbms_execute("UPDATE mini SET name = 'durable' WHERE id = 4242",
                     result, sizeof(result));
        if (!contains_text(result, "1 row updated and saved.")) return 0;
        dbms_execute("INSERT INTO mini VALUES (4243, 'remove')",
                     result, sizeof(result));
        if (!contains_text(result, "1 row inserted and saved.")) return 0;
        dbms_execute("DELETE FROM mini WHERE id = 4243",
                     result, sizeof(result));
        if (!contains_text(result, "1 row deleted and saved.")) return 0;
    } else if (!contains_text(result, "4242 | durable")) return 0;

    /* Reinitialize from the FAT16 file, as a later boot will. */
    dbms_init();
    dbms_execute("SELECT * FROM mini;", result, sizeof(result));
    if (!contains_text(result, "4242 | durable")) return 0;
    dbms_execute("SELECT COUNT(*) FROM mini", result, sizeof(result));
    if (!contains_text(result, "COUNT = 1")) return 0;
    kernel_log("MiniOS: DBMS create, insert, disk reload, and SELECT passed.\n");
    return 1;
}
