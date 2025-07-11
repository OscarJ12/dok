#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <regex.h>
#include <time.h>

#define MAX_LINE_LENGTH 1024
#define MAX_ITEMS 100
#define MAX_NAME_LENGTH 128
#define MAX_CONTENT_LENGTH 512
#define MAX_PATH_LENGTH 256
#define DOCS_FILE ".project_docs.txt"

// ANSI color codes
#define RESET "\033[0m"
#define BOLD "\033[1m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"

// Navigation states
typedef enum {
    STATE_FILES,
    STATE_FUNCTIONS,
    STATE_FUNCTION_DETAIL,
    STATE_SEARCH,
    STATE_UNDOCUMENTED
} nav_state_t;

// Function information - simplified, removed parameter parsing
typedef struct {
    char name[MAX_NAME_LENGTH];
    char signature[MAX_NAME_LENGTH];
    char filename[MAX_PATH_LENGTH];
    int line_number;
    // Documentation fields
    char description[MAX_CONTENT_LENGTH];
    char parameters[MAX_CONTENT_LENGTH];
    char return_value[MAX_CONTENT_LENGTH];
    char example[MAX_CONTENT_LENGTH];
    char notes[MAX_CONTENT_LENGTH];
    int is_documented;
    // Only keep return type parsing
    char return_type[MAX_NAME_LENGTH];
} function_t;

// File information
typedef struct {
    char filename[MAX_PATH_LENGTH];
    char full_path[MAX_PATH_LENGTH];
    function_t functions[MAX_ITEMS];
    int function_count;
} source_file_t;

// Global state - use static to control memory layout
static struct {
    source_file_t files[MAX_ITEMS];
    int file_count;
    int current_file;
    int current_function;
    int current_selection;
    nav_state_t state;
    char search_term[MAX_NAME_LENGTH];
    int search_results[MAX_ITEMS];
    int search_count;
    function_t *undocumented_functions[MAX_ITEMS];
    int undocumented_count;
} docs;

// Terminal handling - make static
static struct termios orig_termios;

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void clear_screen() {
    printf("\033[2J\033[H");
}

// Forward declarations
void extract_return_type(const char *signature, char *return_type);
void save_as_text(source_file_t *file, const char *filename, struct tm *tm_info);
void save_as_markdown(source_file_t *file, const char *filename, struct tm *tm_info);
void save_as_html(source_file_t *file, const char *filename, struct tm *tm_info);
void save_as_postscript(source_file_t *file, const char *filename, struct tm *tm_info);
void write_function_docs_text(FILE *f, source_file_t *file);
void write_function_docs_markdown(FILE *f, source_file_t *file);
void write_function_docs_html(FILE *f, source_file_t *file);
void write_function_docs_postscript(FILE *f, source_file_t *file);

// Utility functions
void trim_whitespace(char *str) {
    char *end;
    
    while(isspace((unsigned char)*str)) str++;
    
    if(*str == 0) return;
    
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    
    *(end+1) = 0;
}

int is_c_file(const char *filename) {
    int len = strlen(filename);
    return (len > 2 && strcmp(filename + len - 2, ".c") == 0) ||
           (len > 2 && strcmp(filename + len - 2, ".h") == 0);
}

// Extract return type from function signature
void extract_return_type(const char *signature, char *return_type) {
    char temp[MAX_NAME_LENGTH];
    strncpy(temp, signature, MAX_NAME_LENGTH - 1);
    temp[MAX_NAME_LENGTH - 1] = '\0';
    
    // Find the function name (before the opening parenthesis)
    char *paren = strchr(temp, '(');
    if (!paren) {
        strcpy(return_type, "void");
        return;
    }
    
    *paren = '\0';  // Cut off at parenthesis
    
    // Go backwards to find the function name
    char *name_end = paren - 1;
    while (name_end > temp && (isalnum(*name_end) || *name_end == '_')) {
        name_end--;
    }
    
    if (name_end <= temp) {
        strcpy(return_type, "void");
        return;
    }
    
    *name_end = '\0';  // Cut off function name
    trim_whitespace(temp);
    
    if (strlen(temp) == 0) {
        strcpy(return_type, "int");  // Default assumption
    } else {
        strncpy(return_type, temp, MAX_NAME_LENGTH - 1);
        return_type[MAX_NAME_LENGTH - 1] = '\0';
    }
}

// C file parsing functions
int extract_function_name(const char *signature, char *name) {
    // Simple function name extraction - looks for pattern: type name(
    char *paren = strchr(signature, '(');
    if (!paren) return 0;
    
    // Go backwards from ( to find function name
    char *start = paren - 1;
    while (start > signature && (isalnum(*start) || *start == '_')) {
        start--;
    }
    start++; // Move to first character of name
    
    // Skip if no valid name found
    if (start >= paren) return 0;
    
    int len = paren - start;
    if (len >= MAX_NAME_LENGTH) len = MAX_NAME_LENGTH - 1;
    
    strncpy(name, start, len);
    name[len] = '\0';
    
    return strlen(name) > 0;
}

int is_function_line(const char *line, const char *filename) {
    // Skip obvious non-function lines
    if (strstr(line, "//") == line) return 0;  // Comment
    if (strstr(line, "/*") == line) return 0;  // Comment
    if (strstr(line, "#") == line) return 0;   // Preprocessor
    if (strstr(line, "typedef") == line) return 0;
    if (strstr(line, "struct") == line) return 0;
    if (strstr(line, "enum") == line) return 0;
    if (strstr(line, "union") == line) return 0;
    
    // Must contain parentheses
    if (!strchr(line, '(') || !strchr(line, ')')) return 0;
    
    // Skip function calls (likely indented)
    if (line[0] == ' ' || line[0] == '\t') return 0;
    
    char *trimmed = strdup(line);
    if (!trimmed) return 0;
    
    trim_whitespace(trimmed);
    
    // For .h files, accept both declarations (ending with ;) and definitions
    int len = strlen(filename);
    int is_header = (len > 2 && strcmp(filename + len - 2, ".h") == 0);
    
    int is_valid = 0;
    if (is_header) {
        // In headers, accept both declarations and definitions
        is_valid = (strlen(trimmed) > 0);
    } else {
        // In .c files, only accept definitions (not ending with semicolon)
        is_valid = (strlen(trimmed) > 0 && trimmed[strlen(trimmed)-1] != ';');
    }
    
    free(trimmed);
    return is_valid;
}

void parse_c_file(const char *filepath, source_file_t *file) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;
    
    char line[MAX_LINE_LENGTH];
    int line_num = 0;
    file->function_count = 0;
    
    while (fgets(line, sizeof(line), f) && file->function_count < MAX_ITEMS) {
        line_num++;
        trim_whitespace(line);
        
        if (is_function_line(line, filepath)) {
            function_t *func = &file->functions[file->function_count];
            
            // Initialize the entire function structure to zero
            memset(func, 0, sizeof(function_t));
            
            // Store signature
            strncpy(func->signature, line, MAX_NAME_LENGTH - 1);
            func->signature[MAX_NAME_LENGTH - 1] = '\0';
            
            // Extract function name
            if (extract_function_name(line, func->name)) {
                strncpy(func->filename, file->filename, MAX_PATH_LENGTH - 1);
                func->filename[MAX_PATH_LENGTH - 1] = '\0';
                func->line_number = line_num;
                func->is_documented = 0;
                
                // Only extract return type (parameter parsing removed)
                extract_return_type(line, func->return_type);
                
                // Initialize documentation fields
                strcpy(func->description, "");
                strcpy(func->parameters, "");
                strcpy(func->return_value, "");
                strcpy(func->example, "");
                strcpy(func->notes, "");
                
                file->function_count++;
            }
        }
    }
    
    fclose(f);
}

void scan_project_files() {
    DIR *dir = opendir(".");
    if (!dir) return;
    
    struct dirent *entry;
    docs.file_count = 0;
    
    while ((entry = readdir(dir)) != NULL && docs.file_count < MAX_ITEMS) {
        if (is_c_file(entry->d_name)) {
            source_file_t *file = &docs.files[docs.file_count];
            strncpy(file->filename, entry->d_name, MAX_PATH_LENGTH - 1);
            file->filename[MAX_PATH_LENGTH - 1] = '\0';
            strncpy(file->full_path, entry->d_name, MAX_PATH_LENGTH - 1);
            file->full_path[MAX_PATH_LENGTH - 1] = '\0';
            
            parse_c_file(entry->d_name, file);
            
            if (file->function_count > 0) {
                docs.file_count++;
            }
        }
    }
    
    closedir(dir);
}

// Documentation persistence
void save_documentation() {
    FILE *f = fopen(DOCS_FILE, "w");
    if (!f) return;
    
    fprintf(f, "# Project Documentation\n");
    fprintf(f, "# Auto-generated - do not edit the function signatures\n\n");
    
    for (int i = 0; i < docs.file_count; i++) {
        source_file_t *file = &docs.files[i];
        for (int j = 0; j < file->function_count; j++) {
            function_t *func = &file->functions[j];
            if (func->is_documented) {
                fprintf(f, "FUNCTION: %s\n", func->name);
                fprintf(f, "FILE: %s\n", func->filename);
                fprintf(f, "LINE: %d\n", func->line_number);
                fprintf(f, "SIGNATURE: %s\n", func->signature);
                fprintf(f, "DESCRIPTION: %s\n", func->description);
                fprintf(f, "PARAMETERS: %s\n", func->parameters);
                fprintf(f, "RETURN: %s\n", func->return_value);
                fprintf(f, "EXAMPLE: %s\n", func->example);
                fprintf(f, "NOTES: %s\n", func->notes);
                fprintf(f, "---\n");
            }
        }
    }
    
    fclose(f);
}

void load_documentation() {
    FILE *f = fopen(DOCS_FILE, "r");
    if (!f) return;
    
    char line[MAX_LINE_LENGTH];
    char current_func_name[MAX_NAME_LENGTH] = "";
    char current_filename[MAX_PATH_LENGTH] = "";
    
    while (fgets(line, sizeof(line), f)) {
        trim_whitespace(line);
        
        if (strncmp(line, "FUNCTION: ", 10) == 0) {
            strncpy(current_func_name, line + 10, MAX_NAME_LENGTH - 1);
            current_func_name[MAX_NAME_LENGTH - 1] = '\0';
        } else if (strncmp(line, "FILE: ", 6) == 0) {
            strncpy(current_filename, line + 6, MAX_PATH_LENGTH - 1);
            current_filename[MAX_PATH_LENGTH - 1] = '\0';
        } else if (strlen(current_func_name) > 0 && strlen(current_filename) > 0) {
            // Find the function in our parsed data
            function_t *func = NULL;
            for (int i = 0; i < docs.file_count; i++) {
                if (strcmp(docs.files[i].filename, current_filename) == 0) {
                    for (int j = 0; j < docs.files[i].function_count; j++) {
                        if (strcmp(docs.files[i].functions[j].name, current_func_name) == 0) {
                            func = &docs.files[i].functions[j];
                            break;
                        }
                    }
                    break;
                }
            }
            
            if (func) {
                if (strncmp(line, "DESCRIPTION: ", 13) == 0) {
                    strncpy(func->description, line + 13, MAX_CONTENT_LENGTH - 1);
                    func->description[MAX_CONTENT_LENGTH - 1] = '\0';
                    func->is_documented = 1;
                } else if (strncmp(line, "PARAMETERS: ", 12) == 0) {
                    strncpy(func->parameters, line + 12, MAX_CONTENT_LENGTH - 1);
                    func->parameters[MAX_CONTENT_LENGTH - 1] = '\0';
                } else if (strncmp(line, "RETURN: ", 8) == 0) {
                    strncpy(func->return_value, line + 8, MAX_CONTENT_LENGTH - 1);
                    func->return_value[MAX_CONTENT_LENGTH - 1] = '\0';
                } else if (strncmp(line, "EXAMPLE: ", 9) == 0) {
                    strncpy(func->example, line + 9, MAX_CONTENT_LENGTH - 1);
                    func->example[MAX_CONTENT_LENGTH - 1] = '\0';
                } else if (strncmp(line, "NOTES: ", 7) == 0) {
                    strncpy(func->notes, line + 7, MAX_CONTENT_LENGTH - 1);
                    func->notes[MAX_CONTENT_LENGTH - 1] = '\0';
                } else if (strcmp(line, "---") == 0) {
                    strcpy(current_func_name, "");
                    strcpy(current_filename, "");
                }
            }
        }
    }
    
    fclose(f);
}

// Search and filter functions
void perform_search(const char *term) {
    docs.search_count = 0;
    
    for (int i = 0; i < docs.file_count; i++) {
        for (int j = 0; j < docs.files[i].function_count; j++) {
            function_t *func = &docs.files[i].functions[j];
            if (strstr(func->name, term) || strstr(func->description, term) || 
                strstr(func->signature, term)) {
                docs.search_results[docs.search_count++] = i * 1000 + j;
                if (docs.search_count >= MAX_ITEMS) return;
            }
        }
    }
}

void find_undocumented_functions() {
    docs.undocumented_count = 0;
    
    for (int i = 0; i < docs.file_count; i++) {
        for (int j = 0; j < docs.files[i].function_count; j++) {
            function_t *func = &docs.files[i].functions[j];
            if (!func->is_documented) {
                docs.undocumented_functions[docs.undocumented_count++] = func;
                if (docs.undocumented_count >= MAX_ITEMS) return;
            }
        }
    }
}

// Display functions
void display_header() {
    printf(BOLD CYAN "════════════════════════════════════════════════════════════════════════\n");
    printf("                    DYNAMIC C PROJECT DOCUMENTATION                    \n");
    printf("════════════════════════════════════════════════════════════════════════\n" RESET);
}

void display_stats() {
    int total_functions = 0, documented_functions = 0;
    
    for (int i = 0; i < docs.file_count; i++) {
        for (int j = 0; j < docs.files[i].function_count; j++) {
            total_functions++;
            if (docs.files[i].functions[j].is_documented) {
                documented_functions++;
            }
        }
    }
    
    printf(BLUE "📊 Project Stats: " RESET "%d files, %d functions, %d documented (%.1f%%)\n\n",
           docs.file_count, total_functions, documented_functions,
           total_functions > 0 ? (float)documented_functions / total_functions * 100 : 0);
}

void display_files() {
    clear_screen();
    display_header();
    display_stats();
    
    printf(BOLD GREEN "SOURCE FILES\n" RESET);
    printf("Use ↑/↓ to navigate, ENTER to view functions, 'p' to print file docs, 'P' to save printable docs, 'r' to rescan, 's' to search, 'u' for undocumented, 'q' to quit\n\n");
    
    for (int i = 0; i < docs.file_count; i++) {
        int documented = 0;
        for (int j = 0; j < docs.files[i].function_count; j++) {
            if (docs.files[i].functions[j].is_documented) documented++;
        }
        
        if (i == docs.current_selection) {
            printf(BOLD YELLOW "► %s" RESET " (%d functions, %d documented)\n", 
                   docs.files[i].filename, docs.files[i].function_count, documented);
        } else {
            printf("  %s (%d functions, %d documented)\n", 
                   docs.files[i].filename, docs.files[i].function_count, documented);
        }
    }
    
    if (docs.file_count == 0) {
        printf(YELLOW "No C files found in current directory.\n" RESET);
    }
}

void display_functions() {
    clear_screen();
    display_header();
    
    source_file_t *file = &docs.files[docs.current_file];
    printf(BOLD GREEN "\nFUNCTIONS in %s\n" RESET, file->filename);
    printf("Use ↑/↓ to navigate, ENTER to view/edit docs, 'b' to go back\n\n");
    
    for (int i = 0; i < file->function_count; i++) {
        function_t *func = &file->functions[i];
        char status_icon = func->is_documented ? '*' : ' ';
        char *status_color = func->is_documented ? GREEN : YELLOW;
        
        if (i == docs.current_selection) {
            printf(BOLD YELLOW "► " RESET "%s%c" RESET " %s " BLUE "(line %d)" RESET "\n", 
                   status_color, status_icon, func->name, func->line_number);
        } else {
            printf("  %s%c" RESET " %s " BLUE "(line %d)" RESET "\n", 
                   status_color, status_icon, func->name, func->line_number);
        }
    }
}

void display_function_detail() {
    clear_screen();
    display_header();
    
    function_t *func = &docs.files[docs.current_file].functions[docs.current_function];
    
    printf(BOLD GREEN "\nFUNCTION: %s\n" RESET, func->name);
    printf("Press 'e' to edit documentation, 'v' to view source, 'b' to go back\n\n");
    
    printf(BOLD CYAN "File: " RESET "%s:%d\n", func->filename, func->line_number);
    printf(BOLD CYAN "Signature: " RESET "%s\n", func->signature);
    printf(BOLD CYAN "Return Type: " RESET "%s\n\n", func->return_type);
    
    if (func->is_documented) {
        if (strlen(func->description) > 0) {
            printf(BOLD CYAN "Description:\n" RESET "%s\n\n", func->description);
        }
        if (strlen(func->parameters) > 0) {
            printf(BOLD CYAN "Parameters:\n" RESET "%s\n\n", func->parameters);
        }
        if (strlen(func->return_value) > 0) {
            printf(BOLD CYAN "Return Value:\n" RESET "%s\n\n", func->return_value);
        }
        if (strlen(func->example) > 0) {
            printf(BOLD CYAN "Example:\n" RESET "%s\n\n", func->example);
        }
        if (strlen(func->notes) > 0) {
            printf(BOLD CYAN "Notes:\n" RESET "%s\n\n", func->notes);
        }
    } else {
        printf(YELLOW "This function is not yet documented. Press 'e' to add documentation.\n" RESET);
    }
}

void display_auto_parsed_info() {
    clear_screen();
    display_header();
    
    function_t *func = &docs.files[docs.current_file].functions[docs.current_function];
    
    printf(BOLD GREEN "\nFUNCTION INFO: %s\n" RESET, func->name);
    printf("Press any key to go back\n\n");
    
    printf(BOLD CYAN "Return Type: " RESET "%s\n", func->return_type);
    printf(BOLD CYAN "Signature: " RESET "%s\n", func->signature);
    printf(BOLD CYAN "File: " RESET "%s:%d\n", func->filename, func->line_number);
    
    getchar();
}

void display_search_results() {
    clear_screen();
    display_header();
    
    printf(BOLD GREEN "\nSEARCH RESULTS for \"%s\"\n" RESET, docs.search_term);
    printf("Use ↑/↓ to navigate, ENTER to view, 'b' to go back\n\n");
    
    for (int i = 0; i < docs.search_count; i++) {
        int file_idx = docs.search_results[i] / 1000;
        int func_idx = docs.search_results[i] % 1000;
        function_t *func = &docs.files[file_idx].functions[func_idx];
        
        char status_icon = func->is_documented ? '*' : ' ';
        char *status_color = func->is_documented ? GREEN : YELLOW;
        
        if (i == docs.current_selection) {
            printf(BOLD YELLOW "► " RESET "%s%c %s::%s" RESET " " BLUE "(line %d)" RESET "\n",
                   status_color, status_icon, func->filename, func->name, func->line_number);
        } else {
            printf("  %s%c %s::%s" RESET " " BLUE "(line %d)" RESET "\n",
                   status_color, status_icon, func->filename, func->name, func->line_number);
        }
    }
    
    if (docs.search_count == 0) {
        printf(YELLOW "No results found.\n" RESET);
    }
}

void display_undocumented() {
    clear_screen();
    display_header();
    
    printf(BOLD GREEN "\nUNDOCUMENTED FUNCTIONS\n" RESET);
    printf("Use ↑/↓ to navigate, ENTER to document, 'b' to go back\n\n");
    
    for (int i = 0; i < docs.undocumented_count; i++) {
        function_t *func = docs.undocumented_functions[i];
        
        if (i == docs.current_selection) {
            printf(BOLD YELLOW "► %s::%s" RESET " " BLUE "(line %d)" RESET "\n",
                   func->filename, func->name, func->line_number);
        } else {
            printf("  %s::%s " BLUE "(line %d)" RESET "\n",
                   func->filename, func->name, func->line_number);
        }
    }
    
    if (docs.undocumented_count == 0) {
        printf(GREEN "All functions are documented!\n" RESET);
    }
}

// Function source code extraction
void print_function_source(function_t *func) {
    FILE *f = fopen(func->filename, "r");
    if (!f) {
        printf(RED "Could not open %s to display function source.\n" RESET, func->filename);
        return;
    }
    
    char line[MAX_LINE_LENGTH];
    int current_line = 0;
    int brace_count = 0;
    int found_function = 0;
    int in_function = 0;
    
    // Check if this is a header file
    int len = strlen(func->filename);
    int is_header = (len > 2 && strcmp(func->filename + len - 2, ".h") == 0);
    
    printf(BOLD CYAN "\nFunction Source Code:\n" RESET);
    printf(CYAN "----------------------------------------\n" RESET);
    
    while (fgets(line, sizeof(line), f)) {
        current_line++;
        
        if (current_line == func->line_number) {
            found_function = 1;
            in_function = 1;
            printf(YELLOW "%3d: " RESET "%s", current_line, line);
            
            // For header files, if line ends with semicolon, it's just a declaration
            char *trimmed = strdup(line);
            if (trimmed) {
                trim_whitespace(trimmed);
                if (is_header && strlen(trimmed) > 0 && trimmed[strlen(trimmed)-1] == ';') {
                    free(trimmed);
                    break; // Just show the declaration line
                }
                free(trimmed);
            }
            
            // Count braces in the function signature line
            for (char *p = line; *p; p++) {
                if (*p == '{') brace_count++;
                if (*p == '}') brace_count--;
            }
        } else if (in_function) {
            printf(YELLOW "%3d: " RESET "%s", current_line, line);
            
            // Count braces to find end of function
            for (char *p = line; *p; p++) {
                if (*p == '{') brace_count++;
                if (*p == '}') brace_count--;
            }
            
            // End of function when braces are balanced
            if (brace_count <= 0) {
                in_function = 0;
                break;
            }
        }
    }
    
    printf(CYAN "----------------------------------------\n" RESET);
    
    if (!found_function) {
        printf(RED "Could not find function at line %d\n" RESET, func->line_number);
    }
    
    fclose(f);
}

// Save file documentation in printable format
void save_printable_documentation(source_file_t *file) {
    char filename[MAX_PATH_LENGTH];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    clear_screen();
    display_header();
    
    printf(BOLD GREEN "SAVE PRINTABLE DOCUMENTATION FOR: %s\n" RESET, file->filename);
    printf("Choose output format:\n\n");
    
    printf(BOLD CYAN "Output Format Options:\n" RESET);
    printf("1. Plain Text (.txt) - Simple, printable format\n");
    printf("2. Markdown (.md) - GitHub/web compatible\n");
    printf("3. HTML (.html) - Web browser printable\n");
    printf("4. PostScript (.ps) - Professional printing format\n");
    printf("5. Cancel\n\n");
    
    printf("Choose format (1-5): ");
    char choice = getchar();
    getchar(); // consume newline
    
    char base_name[MAX_PATH_LENGTH];
    char *dot = strrchr(file->filename, '.');
    if (dot) {
        int len = dot - file->filename;
        if (len >= MAX_PATH_LENGTH) len = MAX_PATH_LENGTH - 1;
        strncpy(base_name, file->filename, len);
        base_name[len] = '\0';
    } else {
        strncpy(base_name, file->filename, MAX_PATH_LENGTH - 1);
        base_name[MAX_PATH_LENGTH - 1] = '\0';
    }
    
    switch (choice) {
        case '1':
            snprintf(filename, sizeof(filename) - 10, "%s_docs.txt", base_name);
            save_as_text(file, filename, tm_info);
            break;
        case '2':
            snprintf(filename, sizeof(filename) - 10, "%s_docs.md", base_name);
            save_as_markdown(file, filename, tm_info);
            break;
        case '3':
            snprintf(filename, sizeof(filename) - 12, "%s_docs.html", base_name);
            save_as_html(file, filename, tm_info);
            break;
        case '4':
            snprintf(filename, sizeof(filename) - 10, "%s_docs.ps", base_name);
            save_as_postscript(file, filename, tm_info);
            break;
        case '5':
        default:
            printf("Cancelled.\n");
            printf("Press any key to continue...");
            getchar();
            return;
    }
}

// Save as plain text format
void save_as_text(source_file_t *file, const char *filename, struct tm *tm_info) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        printf(RED "Error: Could not create file %s\n" RESET, filename);
        printf("Press any key to continue...");
        getchar();
        return;
    }
    
    // Write header
    fprintf(f, "════════════════════════════════════════════════════════════════════════════════\n");
    fprintf(f, "                         C PROJECT DOCUMENTATION                               \n");
    fprintf(f, "════════════════════════════════════════════════════════════════════════════════\n\n");
    
    fprintf(f, "File: %s\n", file->filename);
    fprintf(f, "Generated: %s", asctime(tm_info));
    fprintf(f, "Generated by: DOK - Dynamic C Documentation System\n\n");
    
    write_function_docs_text(f, file);
    
    fclose(f);
    printf(GREEN "Documentation saved to %s\n" RESET, filename);
    printf("Press any key to continue...");
    getchar();
}

// Save as Markdown format
void save_as_markdown(source_file_t *file, const char *filename, struct tm *tm_info) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        printf(RED "Error: Could not create file %s\n" RESET, filename);
        printf("Press any key to continue...");
        getchar();
        return;
    }
    
    // Write header
    fprintf(f, "# C Project Documentation\n\n");
    fprintf(f, "**File:** `%s`  \n", file->filename);
    fprintf(f, "**Generated:** %s", asctime(tm_info));
    fprintf(f, "**Generated by:** DOK - Dynamic C Documentation System\n\n");
    
    write_function_docs_markdown(f, file);
    
    fclose(f);
    printf(GREEN "Markdown documentation saved to %s\n" RESET, filename);
    printf("Press any key to continue...");
    getchar();
}

// Save as HTML format
void save_as_html(source_file_t *file, const char *filename, struct tm *tm_info) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        printf(RED "Error: Could not create file %s\n" RESET, filename);
        printf("Press any key to continue...");
        getchar();
        return;
    }
    
    // Write HTML header
    fprintf(f, "<!DOCTYPE html>\n<html>\n<head>\n");
    fprintf(f, "<title>Documentation - %s</title>\n", file->filename);
    fprintf(f, "<style>\n");
    fprintf(f, "body { font-family: 'Courier New', monospace; margin: 40px; line-height: 1.4; }\n");
    fprintf(f, "h1 { color: #2c3e50; border-bottom: 2px solid #3498db; }\n");
    fprintf(f, "h2 { color: #34495e; margin-top: 30px; }\n");
    fprintf(f, ".function { border: 1px solid #bdc3c7; margin: 20px 0; padding: 15px; }\n");
    fprintf(f, ".signature { background: #ecf0f1; padding: 10px; font-weight: bold; }\n");
    fprintf(f, ".field { margin: 10px 0; }\n");
    fprintf(f, ".field-name { font-weight: bold; color: #2980b9; }\n");
    fprintf(f, "@media print { body { margin: 20px; } }\n");
    fprintf(f, "</style>\n</head>\n<body>\n");
    
    fprintf(f, "<h1>C Project Documentation</h1>\n");
    fprintf(f, "<p><strong>File:</strong> <code>%s</code></p>\n", file->filename);
    fprintf(f, "<p><strong>Generated:</strong> %s</p>\n", asctime(tm_info));
    fprintf(f, "<p><strong>Generated by:</strong> DOK - Dynamic C Documentation System</p>\n");
    
    write_function_docs_html(f, file);
    
    fprintf(f, "</body>\n</html>\n");
    fclose(f);
    printf(GREEN "HTML documentation saved to %s\n" RESET, filename);
    printf("Press any key to continue...");
    getchar();
}

// Save as PostScript format
void save_as_postscript(source_file_t *file, const char *filename, struct tm *tm_info) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        printf(RED "Error: Could not create file %s\n" RESET, filename);
        printf("Press any key to continue...");
        getchar();
        return;
    }
    
    // Write PostScript header
    fprintf(f, "%%!PS-Adobe-3.0\n");
    fprintf(f, "%%%%Title: Documentation - %s\n", file->filename);
    fprintf(f, "%%%%Creator: DOK - Dynamic C Documentation System\n");
    fprintf(f, "%%%%Pages: (atend)\n");
    fprintf(f, "%%%%EndComments\n\n");
    
    fprintf(f, "/Courier findfont 10 scalefont setfont\n");
    fprintf(f, "/newline { currentpoint 12 sub exch pop 72 exch moveto } def\n");
    fprintf(f, "/title { /Courier-Bold findfont 14 scalefont setfont } def\n");
    fprintf(f, "/normal { /Courier findfont 10 scalefont setfont } def\n\n");
    
    fprintf(f, "%%%%Page: 1 1\n");
    fprintf(f, "72 720 moveto\n");
    fprintf(f, "title\n");
    fprintf(f, "(C PROJECT DOCUMENTATION) show newline newline\n");
    fprintf(f, "normal\n");
    fprintf(f, "(File: %s) show newline\n", file->filename);
    fprintf(f, "(Generated: %s) show newline\n", asctime(tm_info));
    fprintf(f, "(Generated by: DOK) show newline newline\n");
    
    write_function_docs_postscript(f, file);
    
    fprintf(f, "showpage\n");
    fprintf(f, "%%%%Trailer\n");
    fprintf(f, "%%%%Pages: 1\n");
    fprintf(f, "%%%%EOF\n");
    
    fclose(f);
    printf(GREEN "PostScript documentation saved to %s\n" RESET, filename);
    printf("You can print this with: lpr %s\n", filename);
    printf("Press any key to continue...");
    getchar();
}

// Helper functions for writing function documentation in different formats
void write_function_docs_text(FILE *f, source_file_t *file) {
    int total_functions = file->function_count;
    int documented_functions = 0;
    for (int i = 0; i < file->function_count; i++) {
        if (file->functions[i].is_documented) documented_functions++;
    }
    
    fprintf(f, "Project Statistics:\n");
    fprintf(f, "  Total functions: %d\n", total_functions);
    fprintf(f, "  Documented functions: %d\n", documented_functions);
    fprintf(f, "  Documentation coverage: %.1f%%\n\n", 
            total_functions > 0 ? (float)documented_functions / total_functions * 100 : 0);
    
    if (file->function_count == 0) {
        fprintf(f, "No functions found in this file.\n");
        return;
    }
    
    fprintf(f, "════════════════════════════════════════════════════════════════════════════════\n");
    fprintf(f, "                                 FUNCTIONS                                      \n");
    fprintf(f, "════════════════════════════════════════════════════════════════════════════════\n\n");
    
    for (int i = 0; i < file->function_count; i++) {
        function_t *func = &file->functions[i];
        
        fprintf(f, "Function: %s (Line %d)\n", func->name, func->line_number);
        fprintf(f, "────────────────────────────────────────────────────────────────────────────────\n");
        fprintf(f, "Signature: %s\n", func->signature);
        fprintf(f, "Return Type: %s\n\n", func->return_type);
        
        if (func->is_documented) {
            if (strlen(func->description) > 0) {
                fprintf(f, "Description:\n%s\n\n", func->description);
            }
            if (strlen(func->parameters) > 0) {
                fprintf(f, "Parameters:\n%s\n\n", func->parameters);
            }
            if (strlen(func->return_value) > 0) {
                fprintf(f, "Return Value:\n%s\n\n", func->return_value);
            }
            if (strlen(func->example) > 0) {
                fprintf(f, "Example:\n%s\n\n", func->example);
            }
            if (strlen(func->notes) > 0) {
                fprintf(f, "Notes:\n%s\n\n", func->notes);
            }
        } else {
            fprintf(f, "*** NOT YET DOCUMENTED ***\n\n");
        }
        
        fprintf(f, "\n");
    }
}

void write_function_docs_markdown(FILE *f, source_file_t *file) {
    int total_functions = file->function_count;
    int documented_functions = 0;
    for (int i = 0; i < file->function_count; i++) {
        if (file->functions[i].is_documented) documented_functions++;
    }
    
    fprintf(f, "## Project Statistics\n\n");
    fprintf(f, "- **Total functions:** %d\n", total_functions);
    fprintf(f, "- **Documented functions:** %d\n", documented_functions);
    fprintf(f, "- **Documentation coverage:** %.1f%%\n\n", 
            total_functions > 0 ? (float)documented_functions / total_functions * 100 : 0);
    
    if (file->function_count == 0) {
        fprintf(f, "No functions found in this file.\n");
        return;
    }
    
    fprintf(f, "## Functions\n\n");
    
    for (int i = 0; i < file->function_count; i++) {
        function_t *func = &file->functions[i];
        
        fprintf(f, "### %s (Line %d)\n\n", func->name, func->line_number);
        fprintf(f, "**Signature:** `%s`  \n", func->signature);
        fprintf(f, "**Return Type:** `%s`\n\n", func->return_type);
        
        if (func->is_documented) {
            if (strlen(func->description) > 0) {
                fprintf(f, "**Description:**  \n%s\n\n", func->description);
            }
            if (strlen(func->parameters) > 0) {
                fprintf(f, "**Parameters:**  \n%s\n\n", func->parameters);
            }
            if (strlen(func->return_value) > 0) {
                fprintf(f, "**Return Value:**  \n%s\n\n", func->return_value);
            }
            if (strlen(func->example) > 0) {
                fprintf(f, "**Example:**  \n```c\n%s\n```\n\n", func->example);
            }
            if (strlen(func->notes) > 0) {
                fprintf(f, "**Notes:**  \n%s\n\n", func->notes);
            }
        } else {
            fprintf(f, "*Not yet documented*\n\n");
        }
        
        fprintf(f, "---\n\n");
    }
}

void write_function_docs_html(FILE *f, source_file_t *file) {
    int total_functions = file->function_count;
    int documented_functions = 0;
    for (int i = 0; i < file->function_count; i++) {
        if (file->functions[i].is_documented) documented_functions++;
    }
    
    fprintf(f, "<h2>Project Statistics</h2>\n");
    fprintf(f, "<ul>\n");
    fprintf(f, "<li><strong>Total functions:</strong> %d</li>\n", total_functions);
    fprintf(f, "<li><strong>Documented functions:</strong> %d</li>\n", documented_functions);
    fprintf(f, "<li><strong>Documentation coverage:</strong> %.1f%%</li>\n", 
            total_functions > 0 ? (float)documented_functions / total_functions * 100 : 0);
    fprintf(f, "</ul>\n\n");
    
    if (file->function_count == 0) {
        fprintf(f, "<p>No functions found in this file.</p>\n");
        return;
    }
    
    fprintf(f, "<h2>Functions</h2>\n\n");
    
    for (int i = 0; i < file->function_count; i++) {
        function_t *func = &file->functions[i];
        
        fprintf(f, "<div class=\"function\">\n");
        fprintf(f, "<h3>%s <small>(Line %d)</small></h3>\n", func->name, func->line_number);
        fprintf(f, "<div class=\"signature\">%s</div>\n", func->signature);
        fprintf(f, "<p><strong>Return Type:</strong> <code>%s</code></p>\n", func->return_type);
        
        if (func->is_documented) {
            if (strlen(func->description) > 0) {
                fprintf(f, "<div class=\"field\"><span class=\"field-name\">Description:</span><br>%s</div>\n", func->description);
            }
            if (strlen(func->parameters) > 0) {
                fprintf(f, "<div class=\"field\"><span class=\"field-name\">Parameters:</span><br><pre>%s</pre></div>\n", func->parameters);
            }
            if (strlen(func->return_value) > 0) {
                fprintf(f, "<div class=\"field\"><span class=\"field-name\">Return Value:</span><br>%s</div>\n", func->return_value);
            }
            if (strlen(func->example) > 0) {
                fprintf(f, "<div class=\"field\"><span class=\"field-name\">Example:</span><br><pre>%s</pre></div>\n", func->example);
            }
            if (strlen(func->notes) > 0) {
                fprintf(f, "<div class=\"field\"><span class=\"field-name\">Notes:</span><br>%s</div>\n", func->notes);
            }
        } else {
            fprintf(f, "<p><em>Not yet documented</em></p>\n");
        }
        
        fprintf(f, "</div>\n\n");
    }
}

void write_function_docs_postscript(FILE *f, source_file_t *file) {
    int y_pos = 650;
    
    for (int i = 0; i < file->function_count; i++) {
        function_t *func = &file->functions[i];
        
        fprintf(f, "72 %d moveto\n", y_pos);
        fprintf(f, "title\n");
        fprintf(f, "(%s) show newline\n", func->name);
        fprintf(f, "normal\n");
        fprintf(f, "(Signature: %s) show newline\n", func->signature);
        fprintf(f, "(Return Type: %s) show newline\n", func->return_type);
        
        if (func->is_documented && strlen(func->description) > 0) {
            fprintf(f, "(Description: %s) show newline\n", func->description);
        } else {
            fprintf(f, "(Not yet documented) show newline\n");
        }
        
        fprintf(f, "newline\n");
        y_pos -= 100;
        
        if (y_pos < 100) break; // Prevent going off page
    }
}
void print_file_documentation(source_file_t *file) {
    clear_screen();
    display_header();
    
    printf(BOLD GREEN "COMPLETE DOCUMENTATION FOR: %s\n" RESET, file->filename);
    printf("Generated by DOK - Dynamic C Documentation System\n\n");
    
    if (file->function_count == 0) {
        printf(YELLOW "No functions found in this file.\n" RESET);
        printf("\nPress any key to continue...");
        getchar();
        return;
    }
    
    for (int i = 0; i < file->function_count; i++) {
        function_t *func = &file->functions[i];
        
        printf("════════════════════════════════════════════════════════════════════════\n");
        printf(BOLD CYAN "FUNCTION: %s" RESET " (Line %d)\n", func->name, func->line_number);
        printf("════════════════════════════════════════════════════════════════════════\n");
        
        // Show source code
        print_function_source(func);
        
        printf("\n");
        printf(BOLD CYAN "DOCUMENTATION:\n" RESET);
        
        if (func->is_documented) {
            if (strlen(func->description) > 0) {
                printf(BOLD "Description:" RESET " %s\n", func->description);
            }
            if (strlen(func->parameters) > 0) {
                printf(BOLD "Parameters:" RESET " %s\n", func->parameters);
            }
            if (strlen(func->return_value) > 0) {
                printf(BOLD "Return Value:" RESET " %s\n", func->return_value);
            }
            if (strlen(func->example) > 0) {
                printf(BOLD "Example:" RESET " %s\n", func->example);
            }
            if (strlen(func->notes) > 0) {
                printf(BOLD "Notes:" RESET " %s\n", func->notes);
            }
        } else {
            printf(YELLOW "*** NOT YET DOCUMENTED ***\n" RESET);
        }
        
        printf("\n\n");
        
        // Pause every few functions to prevent overwhelming output
        if ((i + 1) % 3 == 0 && (i + 1) < file->function_count) {
            printf(BLUE "--- Press any key to continue (showing function %d of %d) ---" RESET, i + 1, file->function_count);
            getchar();
            printf("\n");
        }
    }
    
    printf("════════════════════════════════════════════════════════════════════════\n");
    printf(BOLD GREEN "END OF DOCUMENTATION FOR %s\n" RESET, file->filename);
    printf("════════════════════════════════════════════════════════════════════════\n");
    printf("\nPress any key to continue...");
    getchar();
}

// Input handling
void get_string_input(const char *prompt, char *buffer, int max_length) {
    disable_raw_mode();
    printf("%s", prompt);
    fflush(stdout);
    
    if (fgets(buffer, max_length, stdin)) {
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
    }
    
    enable_raw_mode();
}

void edit_function_documentation(function_t *func) {
    clear_screen();
    printf(BOLD CYAN "Editing documentation for: %s\n" RESET, func->name);
    printf("File: %s:%d\n", func->filename, func->line_number);
    
    // Show the function source code
    print_function_source(func);
    
    printf(BOLD CYAN "\nDocumentation Editor\n" RESET);
    printf("(Leave empty to keep current value, or type new value)\n");
    printf("Press ENTER after each field to continue...\n\n");
    
    char temp_buffer[MAX_CONTENT_LENGTH];
    
    // Description
    printf(BOLD "Current description:" RESET " %s\n", func->description);
    get_string_input("New description: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strncpy(func->description, temp_buffer, MAX_CONTENT_LENGTH - 1);
        func->description[MAX_CONTENT_LENGTH - 1] = '\0';
    }
    
    // Parameters
    printf(BOLD "\nCurrent parameters:" RESET " %s\n", func->parameters);
    get_string_input("New parameters: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strncpy(func->parameters, temp_buffer, MAX_CONTENT_LENGTH - 1);
        func->parameters[MAX_CONTENT_LENGTH - 1] = '\0';
    }
    
    // Return value
    printf(BOLD "\nCurrent return value:" RESET " %s\n", func->return_value);
    get_string_input("New return value: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strncpy(func->return_value, temp_buffer, MAX_CONTENT_LENGTH - 1);
        func->return_value[MAX_CONTENT_LENGTH - 1] = '\0';
    }
    
    // Example
    printf(BOLD "\nCurrent example:" RESET " %s\n", func->example);
    get_string_input("New example: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strncpy(func->example, temp_buffer, MAX_CONTENT_LENGTH - 1);
        func->example[MAX_CONTENT_LENGTH - 1] = '\0';
    }
    
    // Notes
    printf(BOLD "\nCurrent notes:" RESET " %s\n", func->notes);
    get_string_input("New notes: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strncpy(func->notes, temp_buffer, MAX_CONTENT_LENGTH - 1);
        func->notes[MAX_CONTENT_LENGTH - 1] = '\0';
    }
    
    func->is_documented = 1;
    save_documentation();
    
    printf(GREEN "\nDocumentation saved!\n" RESET);
    printf("Press any key to continue...");
    getchar();
}

void handle_input() {
    char c = getchar();
    
    switch (docs.state) {
        case STATE_FILES:
            switch (c) {
                case 'q':
                    exit(0);
                    break;
                case 'r':
                    printf("Rescanning project files...\n");
                    scan_project_files();
                    load_documentation();
                    break;
                case 'p':
                    if (docs.file_count > 0) {
                        print_file_documentation(&docs.files[docs.current_selection]);
                    }
                    break;
                case 'P':
                    if (docs.file_count > 0) {
                        save_printable_documentation(&docs.files[docs.current_selection]);
                    }
                    break;
                case 's':
                    get_string_input("Search term: ", docs.search_term, MAX_NAME_LENGTH);
                    if (strlen(docs.search_term) > 0) {
                        perform_search(docs.search_term);
                        docs.state = STATE_SEARCH;
                        docs.current_selection = 0;
                    }
                    break;
                case 'u':
                    find_undocumented_functions();
                    docs.state = STATE_UNDOCUMENTED;
                    docs.current_selection = 0;
                    break;
                case '\033': // Arrow keys
                    getchar(); // skip [
                    switch (getchar()) {
                        case 'A': // Up
                            if (docs.current_selection > 0) docs.current_selection--;
                            break;
                        case 'B': // Down
                            if (docs.current_selection < docs.file_count - 1) docs.current_selection++;
                            break;
                    }
                    break;
                case '\n':
                case '\r':
                    if (docs.file_count > 0) {
                        docs.current_file = docs.current_selection;
                        docs.state = STATE_FUNCTIONS;
                        docs.current_selection = 0;
                    }
                    break;
            }
            break;
            
        case STATE_FUNCTIONS:
            switch (c) {
                case 'b':
                    docs.state = STATE_FILES;
                    docs.current_selection = docs.current_file;
                    break;
                case '\033': // Arrow keys
                    getchar(); // skip [
                    switch (getchar()) {
                        case 'A': // Up
                            if (docs.current_selection > 0) docs.current_selection--;
                            break;
                        case 'B': // Down
                            if (docs.current_selection < docs.files[docs.current_file].function_count - 1) 
                                docs.current_selection++;
                            break;
                    }
                    break;
                case '\n':
                case '\r':
                    if (docs.files[docs.current_file].function_count > 0) {
                        docs.current_function = docs.current_selection;
                        docs.state = STATE_FUNCTION_DETAIL;
                    }
                    break;
            }
            break;
            
        case STATE_FUNCTION_DETAIL:
            switch (c) {
                case 'b':
                    docs.state = STATE_FUNCTIONS;
                    docs.current_selection = docs.current_function;
                    break;
                case 'e':
                    edit_function_documentation(&docs.files[docs.current_file].functions[docs.current_function]);
                    break;
                case 'v':
                    clear_screen();
                    display_header();
                    function_t *func = &docs.files[docs.current_file].functions[docs.current_function];
                    printf(BOLD GREEN "\nSOURCE CODE: %s\n" RESET, func->name);
                    print_function_source(func);
                    printf("\nPress any key to continue...");
                    getchar();
                    break;
            }
            break;
            
        case STATE_SEARCH:
            switch (c) {
                case 'b':
                    docs.state = STATE_FILES;
                    docs.current_selection = 0;
                    break;
                case '\033': // Arrow keys
                    getchar(); // skip [
                    switch (getchar()) {
                        case 'A': // Up
                            if (docs.current_selection > 0) docs.current_selection--;
                            break;
                        case 'B': // Down
                            if (docs.current_selection < docs.search_count - 1) docs.current_selection++;
                            break;
                    }
                    break;
                case '\n':
                case '\r':
                    if (docs.search_count > 0) {
                        int result = docs.search_results[docs.current_selection];
                        docs.current_file = result / 1000;
                        docs.current_function = result % 1000;
                        docs.state = STATE_FUNCTION_DETAIL;
                    }
                    break;
            }
            break;
            
        case STATE_UNDOCUMENTED:
            switch (c) {
                case 'b':
                    docs.state = STATE_FILES;
                    docs.current_selection = 0;
                    break;
                case '\033': // Arrow keys
                    getchar(); // skip [
                    switch (getchar()) {
                        case 'A': // Up
                            if (docs.current_selection > 0) docs.current_selection--;
                            break;
                        case 'B': // Down
                            if (docs.current_selection < docs.undocumented_count - 1) docs.current_selection++;
                            break;
                    }
                    break;
                case '\n':
                case '\r':
                    if (docs.undocumented_count > 0) {
                        edit_function_documentation(docs.undocumented_functions[docs.current_selection]);
                        find_undocumented_functions(); // Refresh the list
                        if (docs.current_selection >= docs.undocumented_count) {
                            docs.current_selection = docs.undocumented_count - 1;
                        }
                        if (docs.current_selection < 0) docs.current_selection = 0;
                    }
                    break;
            }
            break;
    }
}

int main(int argc, char *argv[]) {
    // Initialize
    docs.file_count = 0;
    docs.current_file = 0;
    docs.current_function = 0;
    docs.current_selection = 0;
    docs.state = STATE_FILES;
    
    // Handle command line arguments
    if (argc > 1) {
        if (chdir(argv[1]) != 0) {
            perror("Failed to change to specified directory");
            printf("Usage: %s [project_directory]\n", argv[0]);
            return 1;
        }
        printf("Changed to directory: %s\n", argv[1]);
    }
    
    printf("Scanning C files in current directory...\n");
    scan_project_files();
    load_documentation();
    
    if (docs.file_count == 0) {
        printf("No C files found in current directory.\n");
        printf("Make sure you're running this from your project directory containing .c and .h files.\n");
        return 1;
    }
    
    enable_raw_mode();
    
    // Main loop
    while (1) {
        switch (docs.state) {
            case STATE_FILES:
                display_files();
                break;
            case STATE_FUNCTIONS:
                display_functions();
                break;
            case STATE_FUNCTION_DETAIL:
                display_function_detail();
                break;
            case STATE_SEARCH:
                display_search_results();
                break;
            case STATE_UNDOCUMENTED:
                display_undocumented();
                break;
        }
        
        handle_input();
    }
    
    return 0;
}
